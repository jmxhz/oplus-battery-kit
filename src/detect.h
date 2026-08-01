/* AVB 校验状态与 dtbo 形态探测 */
#ifndef OBK_DETECT_H
#define OBK_DETECT_H

#include "common.h"
#include "avb.h"

typedef enum { TRI_UNKNOWN = 0, TRI_YES, TRI_NO } tri;

typedef enum { MODE_GRAFT = 0, MODE_SELFSIGN, MODE_RAW, MODE_BLOCKED } avb_mode;

typedef struct {
    tri       verification;      /* AVB 校验是否仍开启 */
    tri       verity;            /* dm-verity 是否仍开启，仅供展示 */
    avb_form  dtbo_form;
    avb_mode  mode;
    int       mode_locked;       /* 1 表示用户不可覆盖 */
    int       requires_stock;
    int       spoof_detected;
    int       snapshot_dirty;    /* dtbo 已被其他工具改过 */
    char     *block_reason;      /* 非空则拒绝写入 */
    char     *note;              /* 面向用户的说明，可为空 */
    char     *bl_state;          /* 仅信息：cmdline 读到的引导状态 */
} detect_t;

/* img 可为 NULL，此时只做状态探测不判 dtbo 形态 */
void detect_run(const uint8_t *img, size_t len, detect_t *out);
void detect_free(detect_t *d);
void detect_json(const detect_t *d, buf_t *b);
const char *mode_name(avb_mode m);

/* dtbo 中是否留有本模块或其他工具的改动痕迹 */
int  detect_dtbo_dirty(const uint8_t *img, size_t len);

#endif
