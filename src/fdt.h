/* 扁平设备树解析与序列化 */
#ifndef OBK_FDT_H
#define OBK_FDT_H

#include "common.h"

#define FDT_MAGIC        0xd00dfeedu
#define FDT_BEGIN_NODE   0x00000001u
#define FDT_END_NODE     0x00000002u
#define FDT_PROP         0x00000003u
#define FDT_NOP          0x00000004u
#define FDT_END          0x00000009u

typedef struct fdt_prop {
    char            *name;
    uint8_t         *data;
    uint32_t         len;
    struct fdt_prop *next;
} fdt_prop;

typedef struct fdt_node {
    char            *name;
    fdt_prop        *props;
    struct fdt_node *children;
    struct fdt_node *next;
    struct fdt_node *parent;
} fdt_node;

typedef struct {
    fdt_node *root;
    uint8_t  *rsvmap;      /* 保留内存表原样保留 */
    size_t    rsvlen;
    uint32_t  boot_cpuid;
    int       dirty;       /* 是否被修改过，未修改的条目原样透传 */
} fdt_t;

fdt_t     *fdt_parse(const uint8_t *blob, size_t len);
void       fdt_free(fdt_t *f);
int        fdt_serialize(fdt_t *f, buf_t *out);

fdt_node  *fdt_find(fdt_t *f, const char *path);
fdt_node  *fdt_child(fdt_node *n, const char *name);
fdt_node  *fdt_child_glob(fdt_node *n, const char *pat);
fdt_prop  *fdt_getprop(fdt_node *n, const char *name);
void       fdt_setprop(fdt_node *n, const char *name, const void *data, uint32_t len);
int        fdt_delprop(fdt_node *n, const char *name);
fdt_node  *fdt_add_child(fdt_node *parent, const char *name);
int        fdt_del_node(fdt_node *n);
char      *fdt_node_path(fdt_node *n);

/* 沿相对路径查找子节点，a/b/c */
fdt_node  *fdt_walk(fdt_node *base, const char *relpath);
/* 同上但末段支持 glob，回调每个命中 */
typedef void (*fdt_walk_cb)(fdt_node *n, void *ud);
void       fdt_walk_glob(fdt_node *base, const char *relpath, fdt_walk_cb cb, void *ud);

/* /__symbols__ 中的标签解析，返回内部指针，勿 free */
const char *fdt_symbol(fdt_t *f, const char *label);
/* /__fixups__ 中某属性的字符串列表，返回数量，值写入 out（需调用方 free 元素） */
int         fdt_fixup_targets(fdt_t *f, const char *prop, char ***out);

/* 子树序列化，用于快照 */
void       fdt_subtree_dump(fdt_node *n, buf_t *out);
/* 反序列化并挂到 parent 下，返回新节点 */
fdt_node  *fdt_subtree_load(fdt_node *parent, const uint8_t *p, size_t len);

/* 属性值格式化：u32 数组 -> "1,2,3"；字符串 -> 原文 */
char      *fdt_prop_fmt_cells(const fdt_prop *p);
char      *fdt_prop_fmt_str(const fdt_prop *p);
int        fdt_prop_is_cells(const fdt_prop *p);

#endif
