/* 声明式补丁规则：解析、应用、还原、快照 */
#ifndef OBK_PROFILE_H
#define OBK_PROFILE_H

#include "common.h"
#include "dtbo.h"

typedef enum {
    OP_SET = 0,     /* 设置属性，不存在则创建     */
    OP_EACH,        /* 通配批量设置，仅改已存在的 */
    OP_RM,          /* 删除子节点，必须存在       */
    OP_RM_OPT,      /* 删除子节点，不存在则跳过   */
    OP_VERIFY,      /* 读回断言                   */
    OP_READ         /* 读属性首个 cell 到变量     */
} op_kind;

typedef struct rule_op {
    op_kind         kind;
    char           *node_rel;   /* 冒号左侧子节点路径，可为 NULL */
    char           *name;       /* 属性名，或 rm 的节点路径      */
    char           *value;      /* 原始值文本                    */
    char           *var;        /* OP_READ 的变量名              */
    int             line;
    struct rule_op *next;
} rule_op;

typedef struct rule_target {
    char               *label;  /* &oplus_mms_gauge */
    char               *prefer; /* silicon_p_*      */
    char               *fixup;  /* __fixups__ 属性  */
    char               *match;  /* fixup 模式下需含的子节点 */
    rule_op            *ops;
    struct rule_target *next;
} rule_target;

typedef struct rule_sec {
    char        *id;
    char        *title;
    char        *desc;
    char        *warn;
    char        *def;        /* on | off | auto:<probe> */
    char        *devices;    /* 空格分隔的 glob，NULL 表示全部 */
    char        *handler;    /* runtime 段的处理器名 */
    char        *suggest;    /* 空格分隔的段 id */
    char        *requires_;
    char        *conflicts;
    int          force;      /* 强制段，不进菜单 */
    int          runtime;    /* kind runtime */
    int          reboot;
    rule_target *targets;
    struct rule_sec *next;
} rule_sec;

typedef struct {
    rule_sec *secs;
    char     *device;
} ruleset;

/* 解析：先 common.rule 再 <device>.rule，同名段以后者整段覆盖 */
ruleset *rules_load(const char *dir, const char *device);
void     rules_free(ruleset *rs);
rule_sec *rules_find(ruleset *rs, const char *id);
int       rules_applies(const rule_sec *s, const char *device);

/* ---------------- 快照 ---------------- */
typedef struct snap_item {
    int      entry;
    char    *path;
    char    *name;      /* 属性名；tree 条目为 NULL */
    uint8_t *data;      /* 属性原值或子树序列化；NULL 表示原本不存在 */
    size_t   len;
    int      is_tree;
    struct snap_item *next;
} snap_item;

typedef struct {
    char      *fingerprint;
    char      *device;
    char      *source;
    uint64_t   partsize;
    int        entries;
    snap_item *items;
} snapshot;

snapshot *snap_new(const char *fp, const char *dev, const char *src,
                   uint64_t partsize, int entries);
void      snap_free(snapshot *s);
int       snap_save(const snapshot *s, const char *path);
snapshot *snap_load(const char *path);

/* 遍历全部规则，记录会被触碰的属性与子树原值 */
snapshot *prof_snapshot(ruleset *rs, dtbo_t *d, const char *fp,
                        const char *dev, const char *src, uint64_t partsize);

/* 把快照写回 dtbo，恢复原厂基线 */
int prof_restore_baseline(snapshot *s, dtbo_t *d);

/* 应用某个段；返回 0 成功，>0 表示跳过（机型不适用/目标缺失） */
int prof_apply_section(rule_sec *sec, dtbo_t *d, buf_t *log);

/* 读回校验某个段的 verify 断言，全部通过返回 0 */
int prof_verify_section(rule_sec *sec, dtbo_t *d, buf_t *log);

/* 判断某段当前是否已生效（所有 verify 断言成立） */
int prof_section_active(rule_sec *sec, dtbo_t *d);

#endif
