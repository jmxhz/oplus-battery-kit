#include "common.h"
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <ctype.h>

int   g_json = 0;
int   g_verbose = 0;
int   g_quiet = 0;
const char *g_root = "/data/obk";
const char *g_dtbo_path = NULL;
const char *g_profdir = NULL;

/* ------------------------------------------------------------ 日志 ---- */
static void vlog(FILE *f, const char *pfx, const char *fmt, va_list ap)
{
    if (pfx) fputs(pfx, f);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
}
void info(const char *fmt, ...)
{
    if (g_quiet || g_json) return;
    va_list ap; va_start(ap, fmt); vlog(stdout, NULL, fmt, ap); va_end(ap);
}
void warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vlog(stderr, "warn: ", fmt, ap); va_end(ap);
}
void err(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vlog(stderr, "error: ", fmt, ap); va_end(ap);
}
void dbg(const char *fmt, ...)
{
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); vlog(stderr, "debug: ", fmt, ap); va_end(ap);
}
void die(int code, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vlog(stderr, "error: ", fmt, ap); va_end(ap);
    exit(code);
}

/* ------------------------------------------------------------ 内存 ---- */
void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die(EX_ERR, "内存不足 (%zu)", n);
    return p;
}
void *xcalloc(size_t n, size_t sz)
{
    void *p = calloc(n ? n : 1, sz ? sz : 1);
    if (!p) die(EX_ERR, "内存不足 (%zu*%zu)", n, sz);
    return p;
}
void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die(EX_ERR, "内存不足 (%zu)", n);
    return q;
}
char *xstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = xmalloc(n);
    memcpy(p, s, n);
    return p;
}
char *xstrndup(const char *s, size_t n)
{
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------ buf ---- */
void buf_init(buf_t *b) { b->data = NULL; b->len = b->cap = 0; }
void buf_free(buf_t *b) { free(b->data); buf_init(b); }

void buf_reserve(buf_t *b, size_t need)
{
    if (b->len + need <= b->cap) return;
    size_t cap = b->cap ? b->cap : 256;
    while (cap < b->len + need) cap *= 2;
    b->data = xrealloc(b->data, cap);
    b->cap = cap;
}
void buf_append(buf_t *b, const void *p, size_t n)
{
    if (!n) return;
    buf_reserve(b, n);
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
void buf_u8(buf_t *b, uint8_t v)   { buf_append(b, &v, 1); }
void buf_be32(buf_t *b, uint32_t v){ uint8_t t[4]; wr_be32(t, v); buf_append(b, t, 4); }
void buf_be64(buf_t *b, uint64_t v){ uint8_t t[8]; wr_be64(t, v); buf_append(b, t, 8); }
void buf_zero(buf_t *b, size_t n)
{
    buf_reserve(b, n);
    memset(b->data + b->len, 0, n);
    b->len += n;
}
void buf_align(buf_t *b, size_t a)
{
    size_t r = b->len % a;
    if (r) buf_zero(b, a - r);
}
void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof(tmp)) { buf_append(b, tmp, (size_t)n); return; }
    char *big = xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    buf_append(b, big, (size_t)n);
    free(big);
}

/* ------------------------------------------------------------ 文件 ---- */
int file_exists(const char *path) { return access(path, F_OK) == 0; }

long file_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

int file_read(const char *path, buf_t *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    buf_init(out);
    uint8_t tmp[65536];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) buf_append(out, tmp, (size_t)n);
    int rc = (n < 0) ? -1 : 0;
    close(fd);
    if (rc) buf_free(out);
    return rc;
}

int file_write(const char *path, const void *p, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    const uint8_t *q = p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, q + off, n - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    int rc = fsync(fd);
    close(fd);
    return rc;
}

int mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    size_t n = strlen(tmp);
    if (n && tmp[n - 1] == '/') tmp[n - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    int rc = (mkdir(tmp, 0755) != 0 && errno != EEXIST) ? -1 : 0;
    free(tmp);
    return rc;
}

char *path_join(const char *a, const char *b)
{
    size_t la = strlen(a);
    int need = (la && a[la - 1] != '/');
    size_t n = la + need + strlen(b) + 1;
    char *p = xmalloc(n);
    snprintf(p, n, "%s%s%s", a, need ? "/" : "", b);
    return p;
}

char *read_line_file(const char *path)
{
    buf_t b;
    if (file_read(path, &b) != 0) return NULL;
    size_t i = 0;
    while (i < b.len && b.data[i] != '\n' && b.data[i] != '\r') i++;
    char *s = xstrndup((char *)b.data, i);
    buf_free(&b);
    return str_trim(s);
}

/* 块设备用 stat 拿不到大小，需 lseek 到末尾 */
int part_read(const char *path, buf_t *out)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    buf_init(out);
    uint8_t tmp[262144];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp))) > 0) buf_append(out, tmp, (size_t)n);
    int rc = (n < 0) ? -1 : 0;
    close(fd);
    if (rc) buf_free(out);
    return rc;
}

int part_write(const char *path, const void *p, size_t n)
{
    int fd = open(path, O_WRONLY | O_SYNC);
    if (fd < 0) {
        /* 普通文件（测试路径）走 O_CREAT */
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) return -1;
    }
    const uint8_t *q = p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, q + off, n - off);
        if (w <= 0) { close(fd); return -1; }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    return 0;
}

/* ------------------------------------------------------------ 字符串 -- */
int str_eq(const char *a, const char *b)
{
    if (!a || !b) return a == b;
    return strcmp(a, b) == 0;
}
int str_startswith(const char *s, const char *pfx)
{
    size_t n = strlen(pfx);
    return strncmp(s, pfx, n) == 0;
}
char *str_trim(char *s)
{
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

/* 仅支持 '*'，可出现多次 */
int glob_match(const char *pat, const char *s)
{
    const char *star = NULL, *ss = s;
    while (*s) {
        if (*pat == '*') { star = pat++; ss = s; }
        else if (*pat == *s) { pat++; s++; }
        else if (star) { pat = star + 1; s = ++ss; }
        else return 0;
    }
    while (*pat == '*') pat++;
    return *pat == 0;
}

char **str_split(const char *s, char sep, int *n_out)
{
    int cap = 8, n = 0;
    char **v = xmalloc(sizeof(char *) * cap);
    const char *p = s;
    while (1) {
        const char *q = strchr(p, sep);
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (n == cap) { cap *= 2; v = xrealloc(v, sizeof(char *) * cap); }
        v[n++] = xstrndup(p, len);
        if (!q) break;
        p = q + 1;
    }
    *n_out = n;
    return v;
}
void str_split_free(char **v, int n)
{
    for (int i = 0; i < n; i++) free(v[i]);
    free(v);
}

void json_escape(buf_t *b, const char *s)
{
    if (!s) { buf_printf(b, "null"); return; }
    buf_u8(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  buf_append(b, "\\\"", 2); break;
        case '\\': buf_append(b, "\\\\", 2); break;
        case '\n': buf_append(b, "\\n", 2);  break;
        case '\r': buf_append(b, "\\r", 2);  break;
        case '\t': buf_append(b, "\\t", 2);  break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_u8(b, *p);
        }
    }
    buf_u8(b, '"');
}

/* ------------------------------------------------------------ 系统 ---- */
char *getprop(const char *key)
{
    const char *argv[] = { "getprop", key, NULL };
    buf_t o;
    if (run_capture(argv, &o) != 0) { buf_free(&o); return NULL; }
    buf_u8(&o, 0);
    char *s = xstrdup(str_trim((char *)o.data));
    buf_free(&o);
    if (!s || !*s) { free(s); return NULL; }   /* str_trim 可能返回 NULL */
    return s;
}

/* 在 /proc/cmdline 与 /proc/bootconfig 中查 androidboot.<key> */
static char *scan_kv(const char *path, const char *full)
{
    buf_t b;
    if (file_read(path, &b) != 0) return NULL;
    buf_u8(&b, 0);
    char *hay = (char *)b.data;
    size_t klen = strlen(full);
    char *ret = NULL;
    for (char *p = hay; *p; p++) {
        if ((p == hay || isspace((unsigned char)p[-1])) &&
            strncmp(p, full, klen) == 0 && p[klen] == '=') {
            char *v = p + klen + 1;
            /* bootconfig 的值可能带引号 */
            if (*v == '"') {
                char *e = strchr(v + 1, '"');
                if (e) { ret = xstrndup(v + 1, (size_t)(e - v - 1)); break; }
            }
            size_t n = 0;
            while (v[n] && !isspace((unsigned char)v[n])) n++;
            ret = xstrndup(v, n);
            break;
        }
    }
    buf_free(&b);
    return ret;
}

char *cmdline_get(const char *key)
{
    char full[256];
    snprintf(full, sizeof(full), "androidboot.%s", key);
    char *v = scan_kv("/proc/cmdline", full);
    if (!v) v = scan_kv("/proc/bootconfig", full);
    return v;
}

int run_capture(const char *argv[], buf_t *out)
{
    int pipefd[2];
    buf_init(out);
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(pipefd[1]);
    uint8_t tmp[4096];
    ssize_t n;
    while ((n = read(pipefd[0], tmp, sizeof(tmp))) > 0) buf_append(out, tmp, (size_t)n);
    close(pipefd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st)) return -1;
    int code = WEXITSTATUS(st);
    return code == 127 ? -1 : code;
}
