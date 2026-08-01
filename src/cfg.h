/* 配置读写与路径 */
#ifndef OBK_CFG_H
#define OBK_CFG_H

#include "common.h"

typedef struct cfg_item {
    char *key;
    char *val;
    struct cfg_item *next;
} cfg_item;

typedef struct { cfg_item *items; } cfg_t;

cfg_t      *cfg_load(void);
int         cfg_save(cfg_t *c);
void        cfg_free(cfg_t *c);
const char *cfg_get(cfg_t *c, const char *key, const char *dflt);
int         cfg_get_int(cfg_t *c, const char *key, int dflt);
void        cfg_set(cfg_t *c, const char *key, const char *val);
void        cfg_set_int(cfg_t *c, const char *key, int v);

/* 路径工具，全部基于 g_root */
char *obk_path(const char *rel);          /* 需 free */
char *obk_snap_path(const char *fp);      /* stock/<fp 摘要>.snap */
char *obk_avb_dir(void);
char *obk_prof_dir(void);                 /* profile 目录，默认与可执行文件同级 */

/* 分区路径 */
char *dtbo_partition(void);

#endif
