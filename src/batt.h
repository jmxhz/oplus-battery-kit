/* 运行时：协议开关、温度伪装、电流投票锁定、协议认证、状态读取、恒压守护 */
#ifndef OBK_BATT_H
#define OBK_BATT_H

#include "common.h"
#include "cfg.h"

/* 可写节点的候选表，来自 profiles/nodes.conf。
 * 同一个 key 可以有多行候选，按顺序取第一个存在的。 */
typedef struct node_cand {
    char *key;
    char *path;
    char *on;
    char *off;
    struct node_cand *next;
} node_cand;

node_cand *nodes_load(const char *dir);
void       nodes_free(node_cand *n);
/* 找到 key 的第一个存在的候选，返回其指针，无则 NULL */
node_cand *nodes_find(node_cand *list, const char *key);

/* 开关类：写候选节点。返回 0 成功，-1 无可用节点 */
int  batt_switch_set(node_cand *list, const char *key, int on);
int  batt_switch_get(node_cand *list, const char *key);   /* 1 开 0 关 -1 未知 */

/* 温度伪装：写 /proc/shell-temp 后置权限 0000 锁死 */
int  batt_faketemp_set(int on, int milli_c);
int  batt_faketemp_get(void);

/* 锁死充电电流投票节点 */
int  batt_lockvotes(int on, int bcc_current);

/* 协议认证数据下发 */
int  batt_auth(const char *which);

/* 深放计数刷新，使新的 deep_spec 策略表立即重算 */
int  batt_refresh_deep_dischg(void);

/* 状态读取 */
typedef struct {
    long soc, real_soc, cc, fcc, rm, soh;
    long vbat_uv, vbat_now, curr_now, temp;
    long bcc_current, cool_down, normal_cool_down;
    long usb_online, cpa_power, ttf;
    long deep_dischg;
    char batt_type[64];
    char manu_date[32];
    char status[32];
} batt_status;

void batt_read_status(batt_status *s);
void batt_status_json(const batt_status *s, buf_t *b);

/* 恒压涓流守护主循环 */
int  batt_daemon(cfg_t *c, node_cand *nodes);
/* 返回运行中的守护 pid，0 表示未运行 */
int  batt_daemon_pid(void);
/* 向运行中的守护发 SIGTERM */
int  batt_daemon_stop(void);

#endif
