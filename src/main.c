/* obk - oplus battery kit
 * 命令分发
 */
#include "common.h"
#include "fdt.h"
#include "dtbo.h"
#include "avb.h"
#include "crypto.h"
#include "profile.h"
#include "cfg.h"
#include "detect.h"
#include "batt.h"
#include <unistd.h>
#include <sys/stat.h>

#define OBK_VERSION "1.0"

static const char *g_device = NULL;
static const char *g_avb_override = NULL;

/* 把设备上读到的机型代号按 profiles/devices.map 映射到规则前缀 */
static char *map_device(const char *raw)
{
    if (!raw || !*raw) return xstrdup("");
    char *dir = obk_prof_dir();
    char *mp  = path_join(dir, "devices.map");
    free(dir);
    buf_t b;
    char *res = NULL;
    if (file_read(mp, &b) == 0) {
        buf_u8(&b, 0);
        char *s = (char *)b.data;
        while (s && *s && !res) {
            char *nl = strchr(s, '\n');
            if (nl) *nl = 0;
            char *L = str_trim(s);
            char *h = strchr(L, '#');
            if (h) { *h = 0; L = str_trim(L); }
            char *eq = strchr(L, '=');
            if (eq) {
                *eq = 0;
                if (str_eq(str_trim(L), raw)) res = xstrdup(str_trim(eq + 1));
            }
            s = nl ? nl + 1 : NULL;
        }
        buf_free(&b);
    }
    free(mp);
    return res ? res : xstrdup(raw);
}

static const char *device_code(void)
{
    static char *cached = NULL;
    if (cached) return cached;
    if (g_device && *g_device) { cached = map_device(g_device); return cached; }
    char *p = getprop("ro.product.vendor.name");
    if (!p) p = getprop("ro.product.name");
    cached = map_device(p ? p : "");
    free(p);
    return cached;
}

static char *build_fingerprint(void)
{
    char *p = getprop("ro.build.fingerprint");
    return p ? p : xstrdup("unknown");
}

static void usage(void)
{
    printf(
"obk %s - oplus battery kit\n"
"\n"
"用法: obk [全局选项] <组> <命令> [参数]\n"
"\n"
"全局选项:\n"
"  --json               结构化输出\n"
"  --verbose / --quiet\n"
"  --device <代号>      覆盖机型识别\n"
"  --dtbo <文件>        对文件操作而非分区\n"
"  --root <目录>        重定向数据根目录 (默认 /data/obk)\n"
"  --profiles <目录>    规则目录\n"
"  --avb <模式>         覆盖 AVB 处理模式: graft | selfsign | raw\n"
"\n"
"avb    detect | cache --stock <img> | graft --in A --out B | sign --in A --out B\n"
"dtbo   list | unpack --dir D | pack --dir D --out F | selftest\n"
"dt     syms F | get F <节点> [属性] | set F <节点> <属性> <值> | rm F <节点>\n"
"prof   list | show <id> | apply [--dry-run] [--force] | revert\n"
"snap   create [--source live|stock --stock <img>] | info | verify\n"
"cfg    get [key] | set k=v ... | reset\n"
"batt   status | proto <pps|ufcs|svooc> [on|off] | faketemp [on|off]\n"
"       lockvotes [on|off] | auth <ufcs|sec> | refresh\n"
"daemon start | stop | status\n"
"version\n", OBK_VERSION);
}

/* ------------------------------------------------------------ 载入 -- */

static int load_dtbo_image(buf_t *out, char **path_out)
{
    char *p = dtbo_partition();
    if (path_out) *path_out = xstrdup(p);
    int rc = part_read(p, out);
    if (rc != 0) err("读取 %s 失败", p);
    free(p);
    return rc;
}

static ruleset *load_rules(void)
{
    char *dir = obk_prof_dir();
    ruleset *rs = rules_load(dir, device_code());
    if (!rs) err("无法加载规则目录 %s", dir);
    free(dir);
    return rs;
}

/* 段的默认开关状态 */
static int section_default(rule_sec *s)
{
    if (s->force) return 1;
    if (!s->def) return 0;
    if (str_startswith(s->def, "auto:")) {
        const char *probe = s->def + 5;
        if (str_eq(probe, "kernel_gki")) {
            /* 官方内核判定：存在 hmbird 调度节点即视为官方 */
            return file_exists("/proc/sys/kernel/sched_hmbird") ||
                   file_exists("/sys/kernel/hmbird") ? 0 : 1;
        }
        return 0;
    }
    return str_eq(s->def, "on") || str_eq(s->def, "1");
}

static int section_enabled(cfg_t *c, rule_sec *s)
{
    if (s->force) return 1;
    const char *v = cfg_get(c, s->id, NULL);
    if (!v) return section_default(s);
    return cfg_get_int(c, s->id, 0);
}

/* ============================================================== avb -- */

static int cmd_avb(int argc, char **argv)
{
    if (argc < 1) { usage(); return EX_USAGE; }

    if (str_eq(argv[0], "detect")) {
        buf_t img;
        int have = (load_dtbo_image(&img, NULL) == 0);
        detect_t d;
        detect_run(have ? img.data : NULL, have ? img.len : 0, &d);
        if (g_json) {
            buf_t b; buf_init(&b);
            detect_json(&d, &b);
            fwrite(b.data, 1, b.len, stdout);
            buf_free(&b);
        } else {
            printf("AVB 校验    : %s\n", d.verification == TRI_YES ? "开启" :
                                         d.verification == TRI_NO ? "已关闭" : "未知");
            printf("dm-verity   : %s\n", d.verity == TRI_YES ? "开启" :
                                         d.verity == TRI_NO ? "已关闭" : "未知");
            printf("dtbo 形态   : %s\n", avb_form_name(d.dtbo_form));
            printf("引导状态    : %s\n", d.bl_state);
            printf("处理模式    : %s%s\n", mode_name(d.mode),
                   d.mode_locked ? " (不可覆盖)" : "");
            if (d.requires_stock) printf("需要官方 dtbo 镜像\n");
            if (d.snapshot_dirty) printf("注意: dtbo 已被改动过，快照应改用官方镜像\n");
            if (d.note && *d.note) printf("说明: %s\n", d.note);
            if (d.block_reason) printf("拒绝原因: %s\n", d.block_reason);
        }
        int rc = d.block_reason ? EX_CHECK_HARD : EX_OK;
        detect_free(&d);
        if (have) buf_free(&img);
        return rc;
    }

    if (str_eq(argv[0], "cache")) {
        const char *stock = NULL;
        for (int i = 1; i < argc - 1; i++)
            if (str_eq(argv[i], "--stock")) stock = argv[i + 1];
        buf_t src;
        char *from = NULL;
        if (stock) {
            if (file_read(stock, &src) != 0) { err("读取 %s 失败", stock); return EX_ERR; }
            from = xstrdup(stock);
        } else {
            if (load_dtbo_image(&src, &from) != 0) return EX_ERR;
        }
        avb_layout lay;
        if (avb_probe(src.data, src.len, &lay) != 0 || lay.form == AVB_FORM_NONE) {
            err("%s 中没有可用的 AVB 信息", from);
            buf_free(&src); free(from);
            return EX_ERR;
        }
        buf_t vb;
        if (avb_extract_vbmeta(src.data, src.len, &lay, &vb) != 0) {
            err("抽取 vbmeta 失败");
            buf_free(&src); free(from);
            return EX_ERR;
        }
        avb_params p;
        if (avb_parse_vbmeta(vb.data, vb.len, &p) != 0) {
            err("解析 vbmeta 失败");
            buf_free(&src); buf_free(&vb); free(from);
            return EX_ERR;
        }
        char *dir = obk_avb_dir();
        mkdir_p(dir);
        avb_params_save(&p, dir);
        char *bp = path_join(dir, "vbmeta.blob");
        file_write(bp, vb.data, vb.len);
        free(bp);
        buf_t meta; buf_init(&meta);
        buf_printf(&meta, "form=%s\n", avb_form_name(lay.form));
        buf_printf(&meta, "orig_image_size=%llu\n", (unsigned long long)lay.orig_image_size);
        buf_printf(&meta, "partition_size=%llu\n", (unsigned long long)src.len);
        buf_printf(&meta, "vbmeta_size=%llu\n", (unsigned long long)lay.vbmeta_size);
        char *mp = path_join(dir, "layout.conf");
        file_write(mp, meta.data, meta.len);
        free(mp);
        buf_free(&meta);
        info("已固化 AVB 参数到 %s (vbmeta %zu 字节)", dir, vb.len);
        free(dir);
        avb_params_free(&p);
        buf_free(&src); buf_free(&vb); free(from);
        return EX_OK;
    }


    if (str_eq(argv[0], "graft") || str_eq(argv[0], "sign")) {
        const char *in = NULL, *out = NULL;
        long psz = 0;
        for (int i = 1; i < argc - 1; i++) {
            if (str_eq(argv[i], "--in"))  in  = argv[i + 1];
            if (str_eq(argv[i], "--out")) out = argv[i + 1];
            if (str_eq(argv[i], "--partition-size")) psz = atol(argv[i + 1]);
        }
        if (!in || !out) { err("需要 --in 与 --out"); return EX_USAGE; }
        buf_t data;
        if (file_read(in, &data) != 0) { err("读取 %s 失败", in); return EX_ERR; }

        char *dir = obk_avb_dir();
        if (psz <= 0) {
            char *lp = path_join(dir, "layout.conf");
            buf_t lc;
            if (file_read(lp, &lc) == 0) {
                buf_u8(&lc, 0);
                char *q = strstr((char *)lc.data, "partition_size=");
                if (q) psz = atol(q + 15);
                buf_free(&lc);
            }
            free(lp);
        }
        if (psz <= 0) { err("需要 --partition-size 或先执行 avb cache"); free(dir); return EX_USAGE; }

        buf_t final;
        int rc = EX_ERR;
        if (str_eq(argv[0], "graft")) {
            char *bp = path_join(dir, "vbmeta.blob");
            char *lp = path_join(dir, "layout.conf");
            buf_t vb;
            uint64_t orig = 0;
            if (file_read(bp, &vb) != 0) err("缺少 vbmeta 缓存，请先执行 avb cache");
            else {
                buf_t lc;
                if (file_read(lp, &lc) == 0) {
                    buf_u8(&lc, 0);
                    char *q = strstr((char *)lc.data, "orig_image_size=");
                    if (q) orig = strtoull(q + 16, NULL, 10);
                    buf_free(&lc);
                }
                if (avb_graft(data.data, data.len, vb.data, vb.len,
                              orig, (uint64_t)psz, &final) == 0) rc = EX_OK;
                buf_free(&vb);
            }
            free(bp); free(lp);
        } else {
            avb_params p;
            if (avb_params_load(&p, dir) != 0) err("缺少 AVB 参数缓存");
            else {
                int bits = p.algorithm == AVB_ALG_SHA256_RSA2048 ? 2048 :
                           p.algorithm == AVB_ALG_SHA256_RSA8192 ? 8192 : 4096;
                char *pd = obk_prof_dir();
                char nm[64];
                snprintf(nm, sizeof(nm), "avb_key_%d.bin", bits);
                char *kp = path_join(pd, nm);
                if (!file_exists(kp)) { free(kp); kp = path_join(pd, "avb_key.bin"); }
                free(pd);
                buf_t kb; rsa_key key;
                if (file_read(kp, &kb) != 0 || rsa_key_load(kb.data, kb.len, &key) != 0)
                    err("缺少可用密钥 %s", kp);
                else if (avb_sign(data.data, data.len, &p, &key,
                                  (uint64_t)psz, &final) == 0) rc = EX_OK;
                free(kp);
            }
        }
        free(dir);
        if (rc == EX_OK) {
            if (file_write(out, final.data, final.len) != 0) { err("写出失败"); rc = EX_ERR; }
            else info("已写出 %s (%zu 字节)", out, final.len);
            buf_free(&final);
        }
        buf_free(&data);
        return rc;
    }

    err("未知 avb 子命令 %s", argv[0]);
    return EX_USAGE;
}

/* ============================================================= dtbo -- */

static int cmd_dtbo(int argc, char **argv)
{
    if (argc < 1) { usage(); return EX_USAGE; }

    buf_t img;
    if (load_dtbo_image(&img, NULL) != 0) return EX_ERR;

    if (str_eq(argv[0], "selftest")) {
        char *detail = NULL;
        int st = dtbo_selftest(img.data, img.len, &detail);
        if (g_json)
            printf("{\"result\":%d,\"detail\":\"%s\"}\n", st, detail ? detail : "");
        else
            printf("%s: %s\n", st == 0 ? "逐字节一致" :
                               st == 1 ? "条目级一致" : "校验失败",
                   detail ? detail : "");
        free(detail);
        buf_free(&img);
        return st < 0 ? EX_CHECK_HARD : EX_OK;
    }

    if (str_eq(argv[0], "list")) {
        dtbo_t *d = dtbo_parse(img.data, img.len);
        if (!d) { err("解析失败"); buf_free(&img); return EX_ERR; }
        if (g_json) {
            printf("{\"page_size\":%u,\"entries\":[", d->page_size);
            for (int i = 0; i < d->n; i++)
                printf("%s{\"index\":%d,\"size\":%zu,\"id\":%u,\"rev\":%u}",
                       i ? "," : "", i, d->ent[i].len, d->ent[i].id, d->ent[i].rev);
            printf("]}\n");
        } else {
            printf("条目 %d  page_size %u\n", d->n, d->page_size);
            for (int i = 0; i < d->n; i++) {
                fdt_t *f = dtbo_fdt(d, i);
                const char *sym = f ? fdt_symbol(f, "oplus_mms_gauge") : NULL;
                printf("  [%2d] %8zu 字节  id=0x%x rev=0x%x  %s\n",
                       i, d->ent[i].len, d->ent[i].id, d->ent[i].rev,
                       sym ? "含 oplus_mms_gauge" : "");
            }
        }
        dtbo_free(d);
        buf_free(&img);
        return EX_OK;
    }

    if (str_eq(argv[0], "unpack")) {
        const char *dir = NULL;
        for (int i = 1; i < argc - 1; i++)
            if (str_eq(argv[i], "--dir")) dir = argv[i + 1];
        if (!dir) { err("需要 --dir"); buf_free(&img); return EX_USAGE; }
        dtbo_t *d = dtbo_parse(img.data, img.len);
        if (!d) { err("解析失败"); buf_free(&img); return EX_ERR; }
        mkdir_p(dir);
        for (int i = 0; i < d->n; i++) {
            char fn[64];
            snprintf(fn, sizeof(fn), "dtb.%d", i);
            char *p = path_join(dir, fn);
            file_write(p, d->ent[i].blob, d->ent[i].len);
            free(p);
        }
        info("已导出 %d 个条目到 %s", d->n, dir);
        dtbo_free(d);
        buf_free(&img);
        return EX_OK;
    }

    err("未知 dtbo 子命令 %s", argv[0]);
    buf_free(&img);
    return EX_USAGE;
}

/* =============================================================== dt -- */

static int cmd_dt(int argc, char **argv)
{
    if (argc < 2) { usage(); return EX_USAGE; }
    buf_t b;
    if (file_read(argv[1], &b) != 0) { err("读取 %s 失败", argv[1]); return EX_ERR; }
    fdt_t *f = fdt_parse(b.data, b.len);
    if (!f) { err("不是有效的 FDT"); buf_free(&b); return EX_ERR; }
    int rc = EX_OK;

    if (str_eq(argv[0], "syms")) {
        fdt_node *s = fdt_child(f->root, "__symbols__");
        if (!s) { printf("(无 __symbols__)\n"); }
        else for (fdt_prop *p = s->props; p; p = p->next)
            printf("%-32s %s\n", p->name, (const char *)p->data);
    } else if (str_eq(argv[0], "get") && argc >= 3) {
        fdt_node *n = fdt_find(f, argv[2]);
        if (!n) { err("找不到节点 %s", argv[2]); rc = EX_ERR; }
        else if (argc >= 4) {
            fdt_prop *p = fdt_getprop(n, argv[3]);
            if (!p) { err("找不到属性 %s", argv[3]); rc = EX_ERR; }
            else {
                char *t = fdt_prop_is_cells(p) ? fdt_prop_fmt_cells(p) : fdt_prop_fmt_str(p);
                printf("%s\n", t);
                free(t);
            }
        } else {
            for (fdt_node *c = n->children; c; c = c->next) printf("%s/\n", c->name);
            for (fdt_prop *p = n->props; p; p = p->next) printf("%s\n", p->name);
        }
    } else {
        err("未知 dt 子命令");
        rc = EX_USAGE;
    }
    fdt_free(f);
    buf_free(&b);
    return rc;
}

/* ============================================================= prof -- */

static void print_section_line(rule_sec *s, cfg_t *c, dtbo_t *d, int json, int first)
{
    int en  = section_enabled(c, s);
    int act = d ? prof_section_active(s, d) : 0;
    if (json) {
        printf("%s{\"id\":", first ? "" : ",");
        buf_t b; buf_init(&b); json_escape(&b, s->id);
        fwrite(b.data, 1, b.len, stdout); buf_free(&b);
        printf(",\"title\":"); buf_init(&b); json_escape(&b, s->title ? s->title : s->id);
        fwrite(b.data, 1, b.len, stdout); buf_free(&b);
        printf(",\"warn\":"); buf_init(&b); json_escape(&b, s->warn);
        fwrite(b.data, 1, b.len, stdout); buf_free(&b);
        printf(",\"suggest\":"); buf_init(&b); json_escape(&b, s->suggest);
        fwrite(b.data, 1, b.len, stdout); buf_free(&b);
        printf(",\"force\":%s", s->force ? "true" : "false");
        printf(",\"runtime\":%s", s->runtime ? "true" : "false");
        printf(",\"reboot\":%s", s->reboot ? "true" : "false");
        printf(",\"enabled\":%s", en ? "true" : "false");
        printf(",\"active\":%s", act ? "true" : "false");
        printf(",\"default\":"); buf_init(&b); json_escape(&b, s->def);
        fwrite(b.data, 1, b.len, stdout); buf_free(&b);
        printf("}");
    } else {
        printf("  %-14s %-3s %-3s %-8s %s\n",
               s->id,
               s->force ? "锁" : (en ? "开" : "关"),
               act ? "生效" : "",
               s->runtime ? "运行时" : "dtbo",
               s->title ? s->title : "");
    }
}

static int cmd_prof(int argc, char **argv);

/* prof apply 的核心 */
static int do_apply(int dry, int force)
{
    const char *dev = device_code();
    if (!*dev) { err("无法识别机型"); return EX_UNSUPPORTED; }

    ruleset *rs = load_rules();
    if (!rs) return EX_ERR;

    char *partpath = NULL;
    buf_t img;
    if (load_dtbo_image(&img, &partpath) != 0) { rules_free(rs); return EX_ERR; }

    /* 1. 探测 */
    detect_t det;
    detect_run(img.data, img.len, &det);
    if (g_avb_override) {
        avb_mode m = str_eq(g_avb_override, "graft")    ? MODE_GRAFT :
                     str_eq(g_avb_override, "selfsign") ? MODE_SELFSIGN :
                     str_eq(g_avb_override, "raw")      ? MODE_RAW : MODE_BLOCKED;
        if (m == MODE_BLOCKED) {
            err("未知 AVB 模式 %s", g_avb_override);
            detect_free(&det); buf_free(&img); free(partpath); rules_free(rs);
            return EX_USAGE;
        }
        if (det.mode_locked && m != det.mode)
            warn("探测建议使用 %s，已按要求改用 %s", mode_name(det.mode), g_avb_override);
        det.mode = m;
        free(det.block_reason);
        det.block_reason = NULL;
    }
    if (det.block_reason && !force) {
        err("%s", det.block_reason);
        detect_free(&det); buf_free(&img); free(partpath); rules_free(rs);
        return EX_CHECK_HARD;
    }

    /* 2. 空转往返，硬门禁 */
    char *sdetail = NULL;
    int st = dtbo_selftest(img.data, img.len, &sdetail);
    info("空转往返: %s", sdetail ? sdetail : "");
    free(sdetail);
    if (st < 0 && !force) {
        err("DTBO 空转往返校验未通过，拒绝写入");
        detect_free(&det); buf_free(&img); free(partpath); rules_free(rs);
        return EX_CHECK_HARD;
    }

    dtbo_t *d = dtbo_parse(img.data, img.len);
    if (!d) {
        err("DTBO 解析失败");
        detect_free(&det); buf_free(&img); free(partpath); rules_free(rs);
        return EX_ERR;
    }

    /* 3. 快照 */
    char *fp = build_fingerprint();
    char *sp = obk_snap_path(fp);
    snapshot *snap = snap_load(sp);
    if (!snap) {
        err("找不到原厂快照 %s，请先执行 obk snap create", sp);
        free(sp); free(fp); dtbo_free(d);
        detect_free(&det); buf_free(&img); free(partpath); rules_free(rs);
        return EX_NO_SNAP;
    }
    free(sp);

    /* 4. 还原基线 */
    int restored = prof_restore_baseline(snap, d);
    info("基线还原: 回写 %d 处", restored);

    /* 5. 应用启用的段 */
    cfg_t *c = cfg_load();
    buf_t log; buf_init(&log);
    int nsec = 0, nfail = 0;
    for (rule_sec *s = rs->secs; s; s = s->next) {
        if (s->runtime) continue;
        if (!rules_applies(s, dev)) continue;
        if (!section_enabled(c, s)) continue;
        int r = prof_apply_section(s, d, &log);
        if (r < 0) { nfail++; err("段 %s 应用失败", s->id); }
        else if (r == 0) nsec++;
    }
    /* 6. 读回校验 */
    for (rule_sec *s = rs->secs; s; s = s->next) {
        if (s->runtime || !rules_applies(s, dev) || !section_enabled(c, s)) continue;
        if (prof_verify_section(s, d, &log) != 0) {
            nfail++;
            err("段 %s 读回校验不通过", s->id);
        }
    }
    if (log.len) { buf_u8(&log, 0); info("%s", (char *)log.data); }
    buf_free(&log);

    if (nfail && !force) {
        err("有 %d 个段未通过，拒绝写入分区", nfail);
        cfg_free(c); snap_free(snap); dtbo_free(d);
        detect_free(&det); buf_free(&img); free(partpath); free(fp); rules_free(rs);
        return EX_CHECK_HARD;
    }

    /* 7. 打标记，便于识别分区已被本模块改过 */
    for (int i = 0; i < d->n; i++) {
        if (!d->ent[i].modified) continue;
        fdt_t *f = dtbo_fdt(d, i);
        if (!f) continue;
        fdt_prop *m = fdt_getprop(f->root, "model");
        char base[256] = "";
        if (m && m->len) {
            char *s = xstrndup((const char *)m->data, m->len);
            char *mk = strstr(s, " obk_");
            if (mk) *mk = 0;
            snprintf(base, sizeof(base), "%s", s);
            free(s);
        }
        char nm[300];
        snprintf(nm, sizeof(nm), "%s obk_%d", base, i);
        fdt_setprop(f->root, "model", nm, (uint32_t)strlen(nm) + 1);
    }

    /* 8. 重新打包 */
    buf_t packed;
    if (dtbo_pack(d, &packed) != 0) {
        err("重新打包失败");
        cfg_free(c); snap_free(snap); dtbo_free(d);
        detect_free(&det); buf_free(&img); free(partpath); free(fp); rules_free(rs);
        return EX_ERR;
    }
    info("打包结果 %zu 字节 / 分区 %zu 字节", packed.len, img.len);

    /* 9. AVB */
    buf_t final; buf_init(&final);
    char *avbdir = obk_avb_dir();
    int rc = EX_OK;

    if (det.mode == MODE_RAW) {
        if (packed.len > img.len) { err("打包结果超出分区"); rc = EX_ERR; }
        else {
            buf_append(&final, packed.data, packed.len);
            buf_zero(&final, img.len - packed.len);
        }
    } else if (det.mode == MODE_SELFSIGN) {
        avb_params p;
        buf_t keyb;
        rsa_key key;
        if (avb_params_load(&p, avbdir) != 0) {
            err("缺少 AVB 参数缓存，请先执行 obk avb cache");
            rc = EX_ERR;
        } else {
            /* 按原算法位数挑密钥，找不到再退回默认 */
            int bits = p.algorithm == AVB_ALG_SHA256_RSA2048 ? 2048 :
                       p.algorithm == AVB_ALG_SHA256_RSA8192 ? 8192 : 4096;
            char *pd = obk_prof_dir();
            char nm[64];
            snprintf(nm, sizeof(nm), "avb_key_%d.bin", bits);
            char *kp = path_join(pd, nm);
            if (!file_exists(kp)) { free(kp); kp = path_join(pd, "avb_key.bin"); }
            free(pd);
            if (file_read(kp, &keyb) != 0 ||
                rsa_key_load(keyb.data, keyb.len, &key) != 0) {
                err("缺少可用的签名密钥 %s", kp);
                rc = EX_ERR;
            } else if (avb_sign(packed.data, packed.len, &p, &key,
                                img.len, &final) != 0) {
                rc = EX_ERR;
            }
            free(kp);
        }
    } else {
        /* 免解拼接 */
        char *bp = path_join(avbdir, "vbmeta.blob");
        char *lp = path_join(avbdir, "layout.conf");
        buf_t vb;
        uint64_t orig = 0;
        if (file_read(bp, &vb) != 0) {
            err("缺少官方 vbmeta 缓存，请先执行 obk avb cache --stock <官方 dtbo>");
            rc = EX_NEED_STOCK;
        } else {
            buf_t lc;
            if (file_read(lp, &lc) == 0) {
                buf_u8(&lc, 0);
                char *q = strstr((char *)lc.data, "orig_image_size=");
                if (q) orig = strtoull(q + 16, NULL, 10);
                buf_free(&lc);
            }
            if (avb_graft(packed.data, packed.len, vb.data, vb.len,
                          orig, img.len, &final) != 0)
                rc = EX_ERR;
            buf_free(&vb);
        }
        free(bp); free(lp);
    }
    free(avbdir);

    /* 10. 写入并回读比对 */
    if (rc == EX_OK) {
        if (final.len != img.len) {
            err("最终镜像 %zu 与分区 %zu 不符，拒绝写入", final.len, img.len);
            rc = EX_ERR;
        } else if (dry) {
            info("试运行：未写入分区");
        } else {
            info("正在写入 %s", partpath);
            if (part_write(partpath, final.data, final.len) != 0) {
                err("写入失败");
                rc = EX_ERR;
            } else {
                buf_t back;
                if (part_read(partpath, &back) == 0) {
                    uint8_t a[16], b2[16];
                    md5(final.data, final.len, a);
                    md5(back.data, back.len < final.len ? back.len : final.len, b2);
                    if (memcmp(a, b2, 16) != 0) {
                        err("写入后回读不一致，正在用原镜像恢复");
                        part_write(partpath, img.data, img.len);
                        rc = EX_ERR;
                    } else {
                        char hex[33];
                        md5_hex(a, hex);
                        char *mp = obk_path("dtbo.md5");
                        file_write(mp, hex, strlen(hex));
                        free(mp);
                        info("写入完成并已回读校验");
                    }
                    buf_free(&back);
                }
            }
        }
    }

    buf_free(&final); buf_free(&packed);
    cfg_free(c); snap_free(snap); dtbo_free(d);
    detect_free(&det); buf_free(&img); free(partpath); free(fp); rules_free(rs);
    if (rc == EX_OK) info("已应用 %d 个功能段", nsec);
    return rc;
}

static int cmd_prof(int argc, char **argv)
{
    if (argc < 1) { usage(); return EX_USAGE; }

    if (str_eq(argv[0], "apply")) {
        int dry = 0, force = 0;
        for (int i = 1; i < argc; i++) {
            if (str_eq(argv[i], "--dry-run")) dry = 1;
            if (str_eq(argv[i], "--force"))   force = 1;
        }
        return do_apply(dry, force);
    }
    if (str_eq(argv[0], "revert")) {
        int force = 0;
        for (int i = 1; i < argc; i++) if (str_eq(argv[i], "--force")) force = 1;
        cfg_t *c = cfg_load();
        ruleset *rs = load_rules();
        if (rs) {
            for (rule_sec *s = rs->secs; s; s = s->next)
                if (!s->force) cfg_set_int(c, s->id, 0);
            rules_free(rs);
        }
        cfg_save(c);
        cfg_free(c);
        info("已关闭全部可选功能，正在应用");
        return do_apply(0, force);
    }

    ruleset *rs = load_rules();
    if (!rs) return EX_ERR;
    cfg_t *c = cfg_load();
    buf_t img;
    dtbo_t *d = NULL;
    if (load_dtbo_image(&img, NULL) == 0) d = dtbo_parse(img.data, img.len);

    if (str_eq(argv[0], "list")) {
        const char *dev = device_code();
        if (g_json) printf("{\"device\":\"%s\",\"sections\":[", dev);
        else printf("机型 %s\n  %-14s %-3s %-3s %-8s %s\n", dev,
                    "id", "开关", "状态", "类型", "标题");
        int first = 1;
        for (rule_sec *s = rs->secs; s; s = s->next) {
            if (!rules_applies(s, dev)) continue;
            print_section_line(s, c, s->runtime ? NULL : d, g_json, first);
            first = 0;
        }
        if (g_json) printf("]}\n");
    } else if (str_eq(argv[0], "show") && argc >= 2) {
        rule_sec *s = rules_find(rs, argv[1]);
        if (!s) { err("没有段 %s", argv[1]); }
        else {
            printf("id        %s\n", s->id);
            printf("标题      %s\n", s->title ? s->title : "");
            printf("说明      %s\n", s->desc ? s->desc : "");
            printf("默认      %s\n", s->def ? s->def : "off");
            printf("强制      %s\n", s->force ? "是" : "否");
            printf("类型      %s\n", s->runtime ? "运行时" : "dtbo");
            printf("适用机型  %s\n", s->devices ? s->devices : "全部");
            if (s->warn)    printf("警告      %s\n", s->warn);
            if (s->suggest) printf("建议同时  %s\n", s->suggest);
            printf("当前开关  %s\n", section_enabled(c, s) ? "开" : "关");
            if (d && !s->runtime)
                printf("当前状态  %s\n", prof_section_active(s, d) ? "已生效" : "未生效");
        }
    } else {
        err("未知 prof 子命令");
    }

    if (d) dtbo_free(d);
    buf_free(&img);
    cfg_free(c);
    rules_free(rs);
    return EX_OK;
}

/* ============================================================= snap -- */

static int cmd_snap(int argc, char **argv)
{
    if (argc < 1) { usage(); return EX_USAGE; }

    if (str_eq(argv[0], "create")) {
        const char *source = "live", *stock = NULL;
        for (int i = 1; i < argc - 1; i++) {
            if (str_eq(argv[i], "--source")) source = argv[i + 1];
            if (str_eq(argv[i], "--stock"))  stock = argv[i + 1];
        }
        buf_t img;
        if (str_eq(source, "stock")) {
            if (!stock) { err("--source stock 需要 --stock <镜像>"); return EX_USAGE; }
            if (file_read(stock, &img) != 0) { err("读取 %s 失败", stock); return EX_ERR; }
        } else {
            if (load_dtbo_image(&img, NULL) != 0) return EX_ERR;
        }
        dtbo_t *d = dtbo_parse(img.data, img.len);
        if (!d) { err("DTBO 解析失败"); buf_free(&img); return EX_ERR; }

        ruleset *rs = load_rules();
        if (!rs) { dtbo_free(d); buf_free(&img); return EX_ERR; }

        char *fp = build_fingerprint();
        snapshot *s = prof_snapshot(rs, d, fp, device_code(), source, img.len);

        char *dir = obk_path("stock");
        mkdir_p(dir);
        free(dir);
        char *sp = obk_snap_path(fp);
        int rc = snap_save(s, sp);
        int n = 0;
        for (snap_item *it = s->items; it; it = it->next) n++;
        if (rc == 0) info("已记录 %d 项原厂值 -> %s", n, sp);
        else err("写入快照失败 %s", sp);
        free(sp); free(fp);
        snap_free(s); rules_free(rs); dtbo_free(d); buf_free(&img);
        return rc == 0 ? EX_OK : EX_ERR;
    }

    if (str_eq(argv[0], "info")) {
        char *fp = build_fingerprint();
        char *sp = obk_snap_path(fp);
        snapshot *s = snap_load(sp);
        if (!s) { err("没有快照 %s", sp); free(sp); free(fp); return EX_NO_SNAP; }
        int props = 0, trees = 0;
        for (snap_item *it = s->items; it; it = it->next)
            it->is_tree ? trees++ : props++;
        if (g_json)
            printf("{\"device\":\"%s\",\"source\":\"%s\",\"partsize\":%llu,"
                   "\"entries\":%d,\"props\":%d,\"trees\":%d,\"match\":%s}\n",
                   s->device, s->source, (unsigned long long)s->partsize,
                   s->entries, props, trees,
                   str_eq(s->fingerprint, fp) ? "true" : "false");
        else {
            printf("机型      %s\n来源      %s\n分区大小  %llu\n条目数    %d\n"
                   "属性快照  %d\n子树快照  %d\n版本匹配  %s\n",
                   s->device, s->source, (unsigned long long)s->partsize,
                   s->entries, props, trees,
                   str_eq(s->fingerprint, fp) ? "是" : "否（系统已更新）");
        }
        snap_free(s); free(sp); free(fp);
        return EX_OK;
    }

    err("未知 snap 子命令 %s", argv[0]);
    return EX_USAGE;
}

/* ============================================================== cfg -- */

static int cmd_cfg(int argc, char **argv)
{
    cfg_t *c = cfg_load();
    int rc = EX_OK;
    if (argc >= 1 && str_eq(argv[0], "set")) {
        for (int i = 1; i < argc; i++) {
            char *eq = strchr(argv[i], '=');
            if (!eq) continue;
            *eq = 0;
            cfg_set(c, argv[i], eq + 1);
            *eq = '=';
        }
        rc = cfg_save(c) == 0 ? EX_OK : EX_ERR;
    } else if (argc >= 1 && str_eq(argv[0], "reset")) {
        char *p = obk_path("config");
        unlink(p);
        free(p);
    } else {
        if (argc >= 2) {
            const char *v = cfg_get(c, argv[1], "");
            printf("%s\n", v);
        } else if (g_json) {
            printf("{");
            int first = 1;
            for (cfg_item *i = c->items; i; i = i->next) {
                printf("%s\"%s\":", first ? "" : ",", i->key);
                buf_t b; buf_init(&b); json_escape(&b, i->val);
                fwrite(b.data, 1, b.len, stdout); buf_free(&b);
                first = 0;
            }
            printf("}\n");
        } else {
            for (cfg_item *i = c->items; i; i = i->next)
                printf("%s=%s\n", i->key, i->val);
        }
    }
    cfg_free(c);
    return rc;
}

/* ============================================================= batt -- */

static int cmd_batt(int argc, char **argv)
{
    if (argc < 1) { usage(); return EX_USAGE; }
    char *pdir = obk_prof_dir();
    node_cand *nodes = nodes_load(pdir);
    free(pdir);
    int rc = EX_OK;

    if (str_eq(argv[0], "status")) {
        batt_status s;
        batt_read_status(&s);
        if (g_json) {
            buf_t b; buf_init(&b);
            batt_status_json(&s, &b);
            fwrite(b.data, 1, b.len, stdout);
            buf_free(&b);
        } else {
            printf("电量      %ld%% (真实 %ld%%)\n", s.soc, s.real_soc);
            printf("电压      %ld mV\n", s.vbat_now);
            printf("电流      %ld mA\n", s.curr_now);
            printf("温度      %.1f C\n", s.temp / 10.0);
            printf("关机电压  %ld mV\n", s.vbat_uv);
            printf("FCC       %ld mAh\n", s.fcc);
            printf("剩余容量  %ld mAh\n", s.rm);
            printf("循环次数  %ld\n", s.cc);
            printf("健康度    %ld\n", s.soh);
            printf("深放计数  %ld\n", s.deep_dischg);
            printf("电池型号  %s\n", s.batt_type);
        }
    } else if (str_eq(argv[0], "proto") && argc >= 2) {
        char key[32];
        snprintf(key, sizeof(key), "proto_%s", argv[1]);
        if (argc >= 3) {
            int on = str_eq(argv[2], "on") || str_eq(argv[2], "1");
            if (batt_switch_set(nodes, key, on) != 0) {
                err("找不到 %s 的可写节点，请在 profiles/nodes.conf 中补充", key);
                rc = EX_ERR;
            } else info("%s 已%s，重插充电器生效", argv[1], on ? "开启" : "关闭");
        } else {
            int v = batt_switch_get(nodes, key);
            printf("%s\n", v == 1 ? "on" : v == 0 ? "off" : "unknown");
        }
    } else if (str_eq(argv[0], "faketemp")) {
        if (argc >= 2) {
            cfg_t *c = cfg_load();
            int milli = cfg_get_int(c, "fake_temp_milli_c", 36000);
            cfg_free(c);
            int on = str_eq(argv[1], "on") || str_eq(argv[1], "1");
            if (batt_faketemp_set(on, milli) != 0) { err("温度伪装设置失败"); rc = EX_ERR; }
            else info("温度伪装已%s", on ? "开启" : "关闭");
        } else {
            int v = batt_faketemp_get();
            printf("%s\n", v == 1 ? "on" : v == 0 ? "off" : "unknown");
        }
    } else if (str_eq(argv[0], "lockvotes")) {
        cfg_t *c = cfg_load();
        int cur = cfg_get_int(c, "lock_votes_ma", 13700);
        cfg_free(c);
        int on = (argc >= 2) ? (str_eq(argv[1], "on") || str_eq(argv[1], "1")) : 1;
        if (batt_lockvotes(on, cur) != 0) { err("电流投票节点不可用"); rc = EX_ERR; }
        else info("电流投票已%s", on ? "锁定" : "解锁");
    } else if (str_eq(argv[0], "auth") && argc >= 2) {
        if (batt_auth(argv[1]) != 0) { err("%s 认证下发失败", argv[1]); rc = EX_ERR; }
        else info("%s 认证数据已写入", argv[1]);
    } else if (str_eq(argv[0], "refresh")) {
        if (batt_refresh_deep_dischg() != 0) { err("深放计数刷新失败"); rc = EX_ERR; }
        else info("深放计数已刷新");
    } else {
        err("未知 batt 子命令");
        rc = EX_USAGE;
    }
    nodes_free(nodes);
    return rc;
}

/* =========================================================== daemon -- */

static int cmd_daemon(int argc, char **argv)
{
    if (argc >= 1 && str_eq(argv[0], "start")) {
        cfg_t *c = cfg_load();
        char *pdir = obk_prof_dir();
        node_cand *nodes = nodes_load(pdir);
        free(pdir);
        int rc = batt_daemon(c, nodes);
        nodes_free(nodes);
        cfg_free(c);
        return rc;
    }
    if (argc >= 1 && str_eq(argv[0], "stop")) {
        if (batt_daemon_stop() != 0) { err("守护未在运行"); return EX_ERR; }
        info("已发送停止信号");
        return EX_OK;
    }
    if (argc >= 1 && str_eq(argv[0], "status")) {
        int pid = batt_daemon_pid();
        char *p = obk_path("daemon.log");
        if (g_json)
            printf("{\"running\":%s,\"pid\":%d,\"log\":\"%s\",\"log_size\":%ld}\n",
                   pid ? "true" : "false", pid, p, file_size(p));
        else
            printf("守护 %s%s\n日志 %s (%ld 字节)\n",
                   pid ? "运行中 pid " : "未运行", pid ? "" : "", p, file_size(p));
        free(p);
        return EX_OK;
    }
    usage();
    return EX_USAGE;
}

/* ============================================================= main -- */

int main(int argc, char **argv)
{
    int i = 1;
    while (i < argc && str_startswith(argv[i], "--")) {
        if      (str_eq(argv[i], "--json"))    g_json = 1;
        else if (str_eq(argv[i], "--verbose")) g_verbose = 1;
        else if (str_eq(argv[i], "--quiet"))   g_quiet = 1;
        else if (str_eq(argv[i], "--device")   && i + 1 < argc) g_device = argv[++i];
        else if (str_eq(argv[i], "--dtbo")     && i + 1 < argc) g_dtbo_path = argv[++i];
        else if (str_eq(argv[i], "--root")     && i + 1 < argc) g_root = argv[++i];
        else if (str_eq(argv[i], "--profiles") && i + 1 < argc) g_profdir = argv[++i];
        else if (str_eq(argv[i], "--avb")      && i + 1 < argc) g_avb_override = argv[++i];
        else { err("未知选项 %s", argv[i]); return EX_USAGE; }
        i++;
    }
    if (i >= argc) { usage(); return EX_USAGE; }

    const char *grp = argv[i++];
    int rest = argc - i;
    char **rv = argv + i;

    if (str_eq(grp, "avb"))         return cmd_avb(rest, rv);
    if (str_eq(grp, "dtbo"))        return cmd_dtbo(rest, rv);
    if (str_eq(grp, "dt"))          return cmd_dt(rest, rv);
    if (str_eq(grp, "prof"))        return cmd_prof(rest, rv);
    if (str_eq(grp, "snap"))        return cmd_snap(rest, rv);
    if (str_eq(grp, "cfg"))         return cmd_cfg(rest, rv);
    if (str_eq(grp, "batt"))        return cmd_batt(rest, rv);
    if (str_eq(grp, "daemon"))      return cmd_daemon(rest, rv);
    if (str_eq(grp, "version"))     { printf("obk %s\n", OBK_VERSION); return EX_OK; }
    if (str_eq(grp, "help") || str_eq(grp, "--help")) { usage(); return EX_OK; }

    err("未知命令组 %s", grp);
    usage();
    return EX_USAGE;
}
