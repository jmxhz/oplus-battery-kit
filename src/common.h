/* obk - oplus battery kit
 * 公共类型与工具
 */
#ifndef OBK_COMMON_H
#define OBK_COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 退出码，安装脚本依赖 ---- */
#define EX_OK          0
#define EX_ERR         1
#define EX_USAGE       2
#define EX_CHECK_SOFT  10   /* 校验未通过，可越过 */
#define EX_CHECK_HARD  11   /* 校验未通过，硬阻断 */
#define EX_NEED_STOCK  20   /* 缺 dtbo_stock.img */
#define EX_NO_SNAP     21   /* 快照缺失或版本不符 */
#define EX_UNSUPPORTED 30   /* 机型不支持 */

/* ---- 全局选项 ---- */
extern int   g_json;
extern int   g_verbose;
extern int   g_quiet;
extern const char *g_root;      /* /data 根，测试时可重定向 */
extern const char *g_dtbo_path; /* 指定 dtbo 文件，替代分区 */
extern const char *g_profdir;   /* profile 目录 */

/* ---- 日志 ---- */
void info(const char *fmt, ...);
void warn(const char *fmt, ...);
void err(const char *fmt, ...);
void dbg(const char *fmt, ...);
void die(int code, const char *fmt, ...) __attribute__((noreturn));

/* ---- 内存 ---- */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t sz);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* ---- 可增长缓冲区 ---- */
typedef struct {
    uint8_t *data;
    size_t   len;
    size_t   cap;
} buf_t;

void buf_init(buf_t *b);
void buf_free(buf_t *b);
void buf_reserve(buf_t *b, size_t need);
void buf_append(buf_t *b, const void *p, size_t n);
void buf_u8(buf_t *b, uint8_t v);
void buf_be32(buf_t *b, uint32_t v);
void buf_be64(buf_t *b, uint64_t v);
void buf_zero(buf_t *b, size_t n);
void buf_align(buf_t *b, size_t a);
void buf_printf(buf_t *b, const char *fmt, ...);

/* ---- 字节序 ---- */
static inline uint32_t rd_be32(const void *p) {
    const uint8_t *q = (const uint8_t *)p;
    return ((uint32_t)q[0] << 24) | ((uint32_t)q[1] << 16) |
           ((uint32_t)q[2] << 8)  |  (uint32_t)q[3];
}
static inline uint64_t rd_be64(const void *p) {
    const uint8_t *q = (const uint8_t *)p;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | q[i];
    return v;
}
static inline void wr_be32(void *p, uint32_t v) {
    uint8_t *q = (uint8_t *)p;
    q[0] = v >> 24; q[1] = v >> 16; q[2] = v >> 8; q[3] = v;
}
static inline void wr_be64(void *p, uint64_t v) {
    uint8_t *q = (uint8_t *)p;
    for (int i = 7; i >= 0; i--) { q[i] = v & 0xff; v >>= 8; }
}

/* ---- 文件 ---- */
int    file_read(const char *path, buf_t *out);       /* 0 成功 */
int    file_write(const char *path, const void *p, size_t n);
int    file_exists(const char *path);
long   file_size(const char *path);
int    mkdir_p(const char *path);
char  *path_join(const char *a, const char *b);
char  *read_line_file(const char *path);              /* 读首行，去尾空白 */

/* 分区读写：走块设备时用 O_SYNC 并回读校验 */
int    part_read(const char *path, buf_t *out);
int    part_write(const char *path, const void *p, size_t n);

/* ---- 字符串 ---- */
int    str_eq(const char *a, const char *b);
int    str_startswith(const char *s, const char *pfx);
char  *str_trim(char *s);
int    glob_match(const char *pat, const char *s);    /* 仅支持 * */
char **str_split(const char *s, char sep, int *n_out);
void   str_split_free(char **v, int n);
void   json_escape(buf_t *b, const char *s);

/* ---- 系统 ---- */
char  *getprop(const char *key);                      /* 需自行 free，失败返回 NULL */
char  *cmdline_get(const char *key);                  /* /proc/cmdline + /proc/bootconfig */
int    run_capture(const char *argv[], buf_t *out);   /* 返回退出码，-1 表示无法执行 */

#endif
