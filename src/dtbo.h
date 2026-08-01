/* DTBO 容器：拆包/打包，逐字节保留条目元数据 */
#ifndef OBK_DTBO_H
#define OBK_DTBO_H

#include "common.h"
#include "fdt.h"

#define DTBO_MAGIC 0xd7b7ab1eu

typedef struct {
    uint32_t id, rev, custom[4];
    uint32_t orig_off, orig_size;
    uint8_t *blob;          /* 原始字节，未修改时原样透传 */
    size_t   len;
    fdt_t   *fdt;           /* 惰性解析 */
    int      modified;
} dtbo_entry;

typedef struct {
    uint32_t page_size, version, header_size, entry_size, entries_offset;
    uint32_t orig_total;
    uint8_t *orig;          /* 原始容器字节，用于原位写回 */
    size_t   orig_len;      /* == container_len */
    size_t   container_len; /* DTBO 容器实际长度，不含分区尾部的 AVB 填充 */
    dtbo_entry *ent;
    int      n;
} dtbo_t;

dtbo_t *dtbo_parse(const uint8_t *img, size_t len);
void    dtbo_free(dtbo_t *d);
int     dtbo_pack(dtbo_t *d, buf_t *out);

/* 惰性解析某条目为 FDT 树；返回 NULL 表示该条目不是有效 FDT */
fdt_t  *dtbo_fdt(dtbo_t *d, int i);
/* 标记条目已修改，打包时重新序列化 */
void    dtbo_touch(dtbo_t *d, int i);

/* 空转往返：拆开再原样打包，与原镜像比对
 * 返回 0 逐字节一致；1 条目级一致但字节不同；-1 不一致 */
int     dtbo_selftest(const uint8_t *img, size_t len, char **detail);

#endif
