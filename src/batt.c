#include "batt.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

#define OC_COMMON  "/sys/class/oplus_chg/common"
#define OC_BATT    "/sys/class/oplus_chg/battery"
#define PS_BATT    "/sys/class/power_supply/battery"
#define PS_USB     "/sys/class/power_supply/usb"

/* 新内核 votable 强制接口：判满后真正停充 */
#define WD_FORCE_VAL  "/proc/oplus-votable/WIRED_CHARGING_DISABLE/force_val"
#define WD_FORCE_ACT  "/proc/oplus-votable/WIRED_CHARGING_DISABLE/force_active"

/* ------------------------------------------------------------ 小工具 -- */

static long read_long(const char *path, long dflt)
{
    char *s = read_line_file(path);
    if (!s) return dflt;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    long r = (end == s) ? dflt : v;
    free(s);
    return r;
}

static long read_long2(const char *dir, const char *name, long dflt)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    return read_long(p, dflt);
}

static void read_str2(const char *dir, const char *name, char *out, size_t n)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    char *s = read_line_file(p);
    snprintf(out, n, "%s", s ? s : "");
    free(s);
}

static int write_str(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, val, strlen(val));
    close(fd);
    return n > 0 ? 0 : -1;
}

static int write_long(const char *path, long v)
{
    char t[32];
    snprintf(t, sizeof(t), "%ld", v);
    return write_str(path, t);
}

/* ---------------------------------------------------- 节点候选表 ------ */

node_cand *nodes_load(const char *dir)
{
    char *p = path_join(dir, "nodes.conf");
    buf_t b;
    node_cand *head = NULL, **tail = &head;
    if (file_read(p, &b) == 0) {
        buf_u8(&b, 0);
        char *s = (char *)b.data;
        while (s && *s) {
            char *nl = strchr(s, '\n');
            if (nl) *nl = 0;
            char *L = str_trim(s);
            char *h = strchr(L, '#');
            if (h) { *h = 0; L = str_trim(L); }
            if (*L) {
                /* key path=... on=... off=... */
                int n = 0;
                char **v = str_split(L, ' ', &n);
                node_cand *c = xcalloc(1, sizeof(node_cand));
                int fi = 0;
                for (int i = 0; i < n; i++) {
                    char *t = str_trim(v[i]);
                    if (!*t) continue;
                    if (fi == 0 && !strchr(t, '=')) { c->key = xstrdup(t); fi = 1; continue; }
                    char *eq = strchr(t, '=');
                    if (!eq) continue;
                    *eq = 0;
                    if      (str_eq(t, "path")) c->path = xstrdup(eq + 1);
                    else if (str_eq(t, "on"))   c->on   = xstrdup(eq + 1);
                    else if (str_eq(t, "off"))  c->off  = xstrdup(eq + 1);
                }
                str_split_free(v, n);
                if (c->key && c->path) {
                    if (!c->on)  c->on  = xstrdup("1");
                    if (!c->off) c->off = xstrdup("0");
                    *tail = c; tail = &c->next;
                } else {
                    free(c->key); free(c->path); free(c->on); free(c->off); free(c);
                }
            }
            s = nl ? nl + 1 : NULL;
        }
        buf_free(&b);
    }
    free(p);
    return head;
}

void nodes_free(node_cand *n)
{
    while (n) {
        node_cand *x = n->next;
        free(n->key); free(n->path); free(n->on); free(n->off); free(n);
        n = x;
    }
}

node_cand *nodes_find(node_cand *list, const char *key)
{
    for (node_cand *c = list; c; c = c->next)
        if (str_eq(c->key, key) && file_exists(c->path)) return c;
    return NULL;
}

int batt_switch_set(node_cand *list, const char *key, int on)
{
    node_cand *c = nodes_find(list, key);
    if (!c) return -1;
    chmod(c->path, 0644);
    return write_str(c->path, on ? c->on : c->off);
}

int batt_switch_get(node_cand *list, const char *key)
{
    node_cand *c = nodes_find(list, key);
    if (!c) return -1;
    char *s = read_line_file(c->path);
    if (!s) return -1;
    int r = str_eq(s, c->on) ? 1 : str_eq(s, c->off) ? 0 : -1;
    free(s);
    return r;
}

/* ------------------------------------------------------- 温度伪装 ---- */

#define SHELL_TEMP "/proc/shell-temp"

int batt_faketemp_set(int on, int milli_c)
{
    if (!file_exists(SHELL_TEMP)) return -1;
    if (!on) return chmod(SHELL_TEMP, 0666);
    if (chmod(SHELL_TEMP, 0644) != 0) return -1;
    for (int i = 0; i < 3; i++) {
        char t[64];
        snprintf(t, sizeof(t), "%d %d", i, milli_c);
        write_str(SHELL_TEMP, t);
    }
    return chmod(SHELL_TEMP, 0000);   /* 锁死，防止系统写回 */
}

int batt_faketemp_get(void)
{
    struct stat st;
    if (stat(SHELL_TEMP, &st) != 0) return -1;
    return (st.st_mode & 0777) == 0 ? 1 : 0;
}

/* --------------------------------------------------- 电流投票锁定 ---- */

/* bcc_current 单位校准：Android 16 新内核按 0.1A/格解析（写 73=7300mA），
   旧内核直接按 mA。写测试值读 votable 反推倍率（100/50/1），
   供守护进程与锁电流投票共用。 */
static long batt_bcc_scale(void)
{
    const char *bcc = OC_BATT "/bcc_current";
    const char *vs = "/proc/oplus-votable/VOOC_CURR/status";
    long scale = 1;
    if (file_exists(vs)) {
        chmod(bcc, 0644);
        write_long(bcc, 100);
        chmod(bcc, 0400);
        buf_t b;
        if (file_read(vs, &b) == 0) {
            buf_u8(&b, 0);
            char *st = (char *)b.data;
            char *p = strstr(st, "BCC_VOTER");
            if (p && (p = strstr(p, "v=")))
                scale = atol(p + 2) / 100;
            buf_free(&b);
        }
        if (scale < 1) scale = 1;
        chmod(bcc, 0644);
        write_long(bcc, 0);
        chmod(bcc, 0400);
    }
    return scale;
}

int batt_lockvotes(int on, int bcc_current)
{
    const char *bcc = OC_BATT "/bcc_current";
    const char *cd  = OC_BATT "/cool_down";
    const char *ncd = OC_BATT "/normal_cool_down";
    const char *cn  = PS_BATT "/current_now";
    if (!file_exists(bcc)) return -1;

    if (!on) {
        chmod(bcc, 0644); chmod(cd, 0644); chmod(ncd, 0644); chmod(cn, 0644);
        return 0;
    }
    chmod(bcc, 0644); chmod(cd, 0644); chmod(ncd, 0644);
    write_long(cd, 0);
    write_long(ncd, 0);
    long scale = batt_bcc_scale();
    chmod(bcc, 0644);
    write_long(bcc, bcc_current / scale);
    chmod(bcc, 0400); chmod(cd, 0400); chmod(ncd, 0400);
    chmod(cn, 0444);
    return 0;
}

/* -------------------------------------------------------- 协议认证 -- */
/* 两条通道各自的 ioctl 与载荷来自对原设备节点行为的分析，
 * 载荷保存在 profiles/auth_<which>.bin，未提供则跳过。 */

static int do_auth(const char *dev, unsigned long req, const uint8_t *p, size_t n)
{
    int fd = open(dev, O_RDWR);
    if (fd < 0) return -1;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    if (n > sizeof(buf)) n = sizeof(buf);
    memcpy(buf, p, n);
    int rc = ioctl(fd, req, buf);
    close(fd);
    return rc < 0 ? -1 : 0;
}

int batt_auth(const char *which)
{
    char *dir = obk_prof_dir();
    char fn[128];
    snprintf(fn, sizeof(fn), "auth_%s.bin", which);
    char *p = path_join(dir, fn);
    free(dir);
    buf_t b;
    int rc = file_read(p, &b);
    free(p);
    if (rc != 0) { err("缺少 %s 认证载荷", which); return -1; }

    unsigned long req;
    const char *dev;
    if (str_eq(which, "ufcs")) { dev = "/dev/ufcs_dev"; req = 0x40016601UL; }
    else if (str_eq(which, "sec")) { dev = "/dev/sec_dev"; req = 0x40017301UL; }
    else { buf_free(&b); err("未知通道 %s", which); return -1; }

    rc = do_auth(dev, req, b.data, b.len);
    buf_free(&b);
    return rc;
}

/* ---------------------------------------------------- 深放计数刷新 -- */

int batt_refresh_deep_dischg(void)
{
    const char *cnt = OC_COMMON "/deep_dischg_counts";
    const char *cal = OC_COMMON "/deep_dischg_count_cali";
    if (!file_exists(cnt) || !file_exists(cal)) return -1;
    long origin = read_long(cnt, -1);
    if (origin < 0) return -1;
    /* 写 1 -> 开校准 -> 写回原值 -> 关校准，促使驱动按新策略表重算
       截止电压与 FCC 修正。刻意保留原始计数，不做归零。 */
    write_long(cnt, 1);
    write_long(cal, 1);
    write_long(cnt, origin);
    write_long(cal, 0);
    return 0;
}

/* ------------------------------------------------------------ 状态 -- */

void batt_read_status(batt_status *s)
{
    memset(s, 0, sizeof(*s));
    s->soc        = read_long2(PS_BATT, "capacity", -1);
    s->real_soc   = read_long2(OC_BATT, "chip_soc", -1);
    s->cc         = read_long2(OC_BATT, "battery_cc", -1);
    s->fcc        = read_long2(OC_BATT, "battery_fcc", -1);
    s->rm         = read_long2(OC_BATT, "battery_rm", -1);
    s->soh        = read_long2(OC_BATT, "battery_soh", -1);
    s->vbat_uv    = read_long2(OC_BATT, "vbat_uv", -1);
    s->bcc_current= read_long2(OC_BATT, "bcc_current", -1);
    s->cool_down  = read_long2(OC_BATT, "cool_down", -1);
    s->normal_cool_down = read_long2(OC_BATT, "normal_cool_down", -1);
    s->vbat_now   = read_long2(PS_BATT, "voltage_now", -1);
    s->curr_now   = read_long2(PS_BATT, "current_now", -1);
    s->temp       = read_long2(PS_BATT, "temp", -1000);
    s->ttf        = read_long2(PS_BATT, "time_to_full_avg", -1);
    s->usb_online = read_long2(PS_USB, "online", -1);
    s->cpa_power  = read_long2(OC_COMMON, "cpa_power", -1);
    s->deep_dischg= read_long2(OC_COMMON, "deep_dischg_counts", -1);
    read_str2(OC_BATT, "battery_type", s->batt_type, sizeof(s->batt_type));
    read_str2(OC_BATT, "battery_manu_date", s->manu_date, sizeof(s->manu_date));
    read_str2(PS_BATT, "status", s->status, sizeof(s->status));

    /* voltage_now 有的机型是微伏，归一到毫伏 */
    if (s->vbat_now > 100000) s->vbat_now /= 1000;
    if (s->curr_now > 100000 || s->curr_now < -100000) s->curr_now /= 1000;
}

void batt_status_json(const batt_status *s, buf_t *b)
{
    buf_printf(b, "{\n");
    buf_printf(b, "  \"soc\": %ld,\n", s->soc);
    buf_printf(b, "  \"real_soc\": %ld,\n", s->real_soc);
    buf_printf(b, "  \"cycle_count\": %ld,\n", s->cc);
    buf_printf(b, "  \"fcc\": %ld,\n", s->fcc);
    buf_printf(b, "  \"rm\": %ld,\n", s->rm);
    buf_printf(b, "  \"soh\": %ld,\n", s->soh);
    buf_printf(b, "  \"vbat_uv\": %ld,\n", s->vbat_uv);
    buf_printf(b, "  \"vbat_mv\": %ld,\n", s->vbat_now);
    buf_printf(b, "  \"current_ma\": %ld,\n", s->curr_now);
    buf_printf(b, "  \"temp_dc\": %ld,\n", s->temp);
    buf_printf(b, "  \"bcc_current\": %ld,\n", s->bcc_current);
    buf_printf(b, "  \"cool_down\": %ld,\n", s->cool_down);
    buf_printf(b, "  \"normal_cool_down\": %ld,\n", s->normal_cool_down);
    buf_printf(b, "  \"usb_online\": %ld,\n", s->usb_online);
    buf_printf(b, "  \"cpa_power\": %ld,\n", s->cpa_power);
    buf_printf(b, "  \"time_to_full\": %ld,\n", s->ttf);
    buf_printf(b, "  \"deep_dischg\": %ld,\n", s->deep_dischg);
    buf_printf(b, "  \"battery_type\": ");   json_escape(b, s->batt_type);
    buf_printf(b, ",\n  \"manu_date\": ");   json_escape(b, s->manu_date);
    buf_printf(b, ",\n  \"status\": ");      json_escape(b, s->status);
    buf_printf(b, "\n}\n");
}

/* ==================================================== 恒压涓流守护 ==== */

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

typedef enum { ST_IDLE = 0, ST_RISE, ST_CV, ST_TC, ST_FULL } cvstate;

static const char *state_name(cvstate s)
{
    switch (s) {
    case ST_RISE: return "升流";
    case ST_CV:   return "恒压";
    case ST_TC:   return "涓流";
    case ST_FULL: return "已满";
    default:      return "空闲";
    }
}

/* 解析 "42,43,44" 形式的整数列表 */
static int parse_list(const char *s, long *out, int max)
{
    if (!s || !*s) return 0;
    int n = 0, cnt = 0;
    char **v = str_split(s, ',', &cnt);
    for (int i = 0; i < cnt && n < max; i++) {
        char *t = str_trim(v[i]);
        if (*t) out[n++] = strtol(t, NULL, 10);
    }
    str_split_free(v, cnt);
    return n;
}

static void daemon_log(const char *line)
{
    char *p = obk_path("daemon.log");
    long sz = file_size(p);
    int fd = open(p, O_WRONLY | O_CREAT | (sz > 512 * 1024 ? O_TRUNC : O_APPEND), 0644);
    free(p);
    if (fd < 0) return;
    /* musl 静态库读不到 Android 时区库，直接用系统 date 取本地时间 */
    char hdr[64] = "[----/--/-- --:--:--] ";
    FILE *d = popen("date '+%Y-%m-%d %H:%M:%S'", "r");
    if (d) {
        char ts[32] = {0};
        if (fgets(ts, sizeof(ts), d)) {
            char *nl = strchr(ts, '\n');
            if (nl) *nl = 0;
            if (*ts) snprintf(hdr, sizeof(hdr), "[%s] ", ts);
        }
        pclose(d);
    }
    ssize_t ignored = write(fd, hdr, strlen(hdr));
    ignored = write(fd, line, strlen(line));
    ignored = write(fd, "\n", 1);
    (void)ignored;
    close(fd);
}

/* 守护日志使用设备本地时区（root 进程默认 UTC，读 persist.sys.timezone 修正） */
static void daemon_set_tz(void)
{
    char tz[64] = {0};
    FILE *f = fopen("/data/property/persist.sys.timezone", "r");
    if (f) {
        if (fgets(tz, sizeof(tz), f)) {
            char *nl = strchr(tz, '\n');
            if (nl) *nl = 0;
        }
        fclose(f);
    }
    if (!*tz) {
        f = popen("getprop persist.sys.timezone", "r");
        if (f) {
            if (fgets(tz, sizeof(tz), f)) {
                char *nl = strchr(tz, '\n');
                if (nl) *nl = 0;
            }
            pclose(f);
        }
    }
    if (*tz) {
        char *nl = strchr(tz, '\n');
        if (nl) *nl = 0;
        setenv("TZ", tz, 1);
        tzset();
    }
}

/* pid 文件：单实例保护，并让 daemon stop 能找到进程 */
static char *daemon_pidfile(void) { return obk_path("daemon.pid"); }

int batt_daemon_pid(void)
{
    char *p = daemon_pidfile();
    char *s = read_line_file(p);
    free(p);
    if (!s) return 0;
    int pid = atoi(s);
    free(s);
    if (pid <= 0) return 0;
    if (kill(pid, 0) != 0) return 0;      /* 进程已不存在 */
    /* 防 pid 复用：确认该 pid 确实是 obk 守护，否则视为残留并清掉 */
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "/proc/%d/cmdline", pid);
    char *c = read_line_file(cmd);
    if (!c || !strstr(c, "obk")) {
        free(c);
        char *pf = daemon_pidfile();
        unlink(pf);
        free(pf);
        return 0;
    }
    free(c);
    return pid;
}

int batt_daemon_stop(void)
{
    int pid = batt_daemon_pid();
    if (!pid) return -1;
    if (kill(pid, SIGTERM) != 0) return -1;
    char *p = daemon_pidfile();
    unlink(p);
    free(p);
    return 0;
}

int batt_daemon(cfg_t *c, node_cand *nodes)
{
    (void)nodes;
    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    daemon_set_tz();

    int old = batt_daemon_pid();
    if (old) { err("守护已在运行 (pid %d)", old); return EX_ERR; }
    {
        char *pf = daemon_pidfile();
        char t[32];
        snprintf(t, sizeof(t), "%d", (int)getpid());
        mkdir_p(g_root);
        file_write(pf, t, strlen(t));
        free(pf);
    }

    const char *bcc = OC_BATT "/bcc_current";
    if (!file_exists(bcc)) { err("找不到 %s，守护无法工作", bcc); return EX_ERR; }

    long bcc_scale = batt_bcc_scale();
    /* 复位判满停充的强制通道，避免上次异常残留 */
    write_str(WD_FORCE_VAL, "0");
    write_str(WD_FORCE_ACT, "0");

    long imax_ufcs = cfg_get_int(c, "cv_ufcs_max_ma", 14600);
    long imax_pps  = cfg_get_int(c, "cv_pps_max_ma", 6500);
    long inc_step  = cfg_get_int(c, "cv_inc_step_ma", 100);
    long dec_step  = cfg_get_int(c, "cv_dec_step_ma", 100);
    long loop_ms   = cfg_get_int(c, "cv_loop_ms", 2000);
    long cv_vol    = cfg_get_int(c, "cv_vol_mv", 4565);
    long cv_max    = cfg_get_int(c, "cv_max_ma", 11000);
    long tc_vol    = cfg_get_int(c, "cv_tc_vol_thr_mv", 4500);
    long tc_soc    = cfg_get_int(c, "cv_tc_thr_soc", 98);
    long tc_full   = cfg_get_int(c, "cv_tc_full_ma", 400);
    long tc_vfull  = cfg_get_int(c, "cv_tc_vol_full_mv", 4485);
    long full_thr  = cfg_get_int(c, "cv_batt_full_thr_mv", 4570);
    long rise_quick= cfg_get_int(c, "cv_rise_quickstep_thr_mv", 4250);
    long rise_wait = cfg_get_int(c, "cv_rise_wait_thr_mv", 3800);
    long inc_wait  = cfg_get_int(c, "cv_curr_inc_wait_cycles", 4);

    long trange[8], toff[8];
    int ntr = parse_list(cfg_get(c, "cv_temp_range", "42,43,44,45,46"), trange, 8);
    int nto = parse_list(cfg_get(c, "cv_temp_curr_offset", "800,1200,1800,2500,4500"), toff, 8);
    if (ntr != nto) { ntr = nto = 0; warn("温控档位与减速量数量不符，已停用温控回退"); }

    long imax = imax_ufcs > imax_pps ? imax_ufcs : imax_pps;
    long target = imax;
    cvstate st = ST_IDLE;
    int wait = 0;
    char msg[256];

    snprintf(msg, sizeof(msg),
             "守护启动 imax=%ld cv=%ldmV tc=%ldmA full=%ldmV", imax, cv_vol, tc_full, full_thr);
    daemon_log(msg);

    while (!g_stop) {
        batt_status s;
        batt_read_status(&s);

        if (s.usb_online != 1) {
            if (st != ST_IDLE) { daemon_log("充电器断开，释放控制"); st = ST_IDLE; }
            write_str(WD_FORCE_VAL, "0");
            struct timespec ts = { loop_ms / 1000, (loop_ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        long vbat = s.vbat_now;
        long icur = s.curr_now < 0 ? -s.curr_now : s.curr_now;
        long temp = s.temp;          /* 0.1 摄氏度 */

        /* 温度回退：命中档位则按该档减速量下调上限 */
        long cap = imax;
        for (int i = 0; i < ntr; i++)
            if (temp >= trange[i] * 10) cap = imax - toff[i];
        if (cap < 500) cap = 500;

        cvstate next = st;
        switch (st) {
        case ST_IDLE:
            target = cap;
            next = ST_RISE;
            break;

        case ST_RISE:
            if (s.soc >= tc_soc && vbat >= tc_vol) { next = ST_TC; break; }
            if (vbat >= cv_vol) { next = ST_CV; break; }
            if (vbat < rise_wait) target = cap;
            else if (vbat < rise_quick) {
                if (++wait >= inc_wait) { wait = 0; target += inc_step; }
            } else {
                if (++wait >= inc_wait * 2) { wait = 0; target += inc_step / 2; }
            }
            if (target > cap) target = cap;
            break;

        case ST_CV:
            if (s.soc >= tc_soc && vbat >= tc_vol) { next = ST_TC; break; }
            if (vbat >= full_thr)      target -= dec_step * 2;
            else if (vbat >= cv_vol)   target -= dec_step;
            else if (vbat < cv_vol - 30) {
                if (++wait >= inc_wait) { wait = 0; target += inc_step; }
            }
            if (target > cv_max) target = cv_max;
            if (target > cap)    target = cap;
            if (target < tc_full) target = tc_full;
            if (vbat < rise_quick) next = ST_RISE;
            break;

        case ST_TC:
            if (vbat >= full_thr)    target -= dec_step;
            else if (vbat >= cv_vol) target -= dec_step / 2;
            else if (vbat < tc_vfull) {
                if (++wait >= inc_wait) { wait = 0; target += inc_step / 2; }
            }
            if (target > cv_max) target = cv_max;
            if (target > cap)    target = cap;
            if (target < 100)    target = 100;
            if (icur <= tc_full && vbat >= tc_vfull) next = ST_FULL;
            break;

        case ST_FULL:
            target = 0;
            write_str(WD_FORCE_VAL, "1");
            write_str(WD_FORCE_ACT, "1");
            if (vbat < tc_vfull - 100 || s.soc < tc_soc - 2) {
                next = ST_TC;
                write_str(WD_FORCE_VAL, "0");
            }
            break;
        }

        if (next != st) {
            snprintf(msg, sizeof(msg), "%s -> %s  vbat=%ldmV i=%ldmA soc=%ld%% t=%.1fC",
                     state_name(st), state_name(next), vbat, icur, s.soc, temp / 10.0);
            daemon_log(msg);
            st = next;
            wait = 0;
        }

        if (st != ST_IDLE) {
            chmod(bcc, 0644);
            write_long(bcc, target / bcc_scale);
            chmod(bcc, 0400);
        }

        struct timespec ts = { loop_ms / 1000, (loop_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }

    {
        write_str(WD_FORCE_VAL, "0");
        write_str(WD_FORCE_ACT, "0");
        char *pf = daemon_pidfile();
        unlink(pf);
        free(pf);
    }
    daemon_log("守护退出");
    return EX_OK;
}
