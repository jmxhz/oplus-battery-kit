#include "detect.h"
#include "dtbo.h"

const char *mode_name(avb_mode m)
{
    switch (m) {
    case MODE_GRAFT:    return "graft";
    case MODE_SELFSIGN: return "selfsign";
    case MODE_RAW:      return "raw";
    default:            return "blocked";
    }
}

static const char *tri_name(tri t)
{
    return t == TRI_YES ? "enabled" : t == TRI_NO ? "disabled" : "unknown";
}

/* avbctl 输出形如 "verification is enabled"，三态解析，失败不当作 enabled */
static tri read_avb_flag(const char *sub)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "get-%s", sub);
    const char *argv[] = { "avbctl", cmd, NULL };
    buf_t o;
    int rc = run_capture(argv, &o);
    if (rc != 0 || o.len == 0) { buf_free(&o); return TRI_UNKNOWN; }
    buf_u8(&o, 0);
    tri r = TRI_UNKNOWN;
    if (strstr((char *)o.data, "disabled"))     r = TRI_NO;
    else if (strstr((char *)o.data, "enabled")) r = TRI_YES;
    buf_free(&o);
    return r;
}

/* 遗留痕迹：dtb 根节点 model 里被追加过标记，或 /data 下有其他工具目录 */
int detect_dtbo_dirty(const uint8_t *img, size_t len)
{
    if (!img || !len) return 0;
    dtbo_t *d = dtbo_parse(img, len);
    if (!d) return 0;
    int dirty = 0;
    for (int i = 0; i < d->n && !dirty; i++) {
        fdt_t *f = dtbo_fdt(d, i);
        if (!f) continue;
        fdt_prop *m = fdt_getprop(f->root, "model");
        if (!m || !m->len) continue;
        char *s = xstrndup((const char *)m->data, m->len);
        if (strstr(s, "obk_")) dirty = 1;
        free(s);
    }
    dtbo_free(d);
    return dirty;
}

void detect_run(const uint8_t *img, size_t len, detect_t *out)
{
    memset(out, 0, sizeof(*out));

    out->verification = read_avb_flag("verification");
    out->verity       = read_avb_flag("verity");

    /* 伪装检测：prop 与 cmdline 不一致说明有模块在改 ro.boot.* */
    static const char *keys[] = { "veritymode", "verifiedbootstate",
                                  "flash.locked", "vbmeta.device_state", NULL };
    buf_t note; buf_init(&note);
    for (int i = 0; keys[i]; i++) {
        char pk[64];
        snprintf(pk, sizeof(pk), "ro.boot.%s", keys[i]);
        char *hard = cmdline_get(keys[i]);
        char *soft = getprop(pk);
        if (hard && soft && !str_eq(hard, soft)) {
            out->spoof_detected = 1;
            buf_printf(&note, "%s 被伪装(cmdline=%s prop=%s); ", keys[i], hard, soft);
        }
        free(hard); free(soft);
    }

    /* 引导状态仅作展示与保险丝 */
    char *locked = cmdline_get("flash.locked");
    char *vbs    = cmdline_get("verifiedbootstate");
    buf_t bl; buf_init(&bl);
    buf_printf(&bl, "%s/%s", locked ? locked : "?", vbs ? vbs : "?");
    buf_u8(&bl, 0);
    out->bl_state = (char *)bl.data;

    int bl_locked = 0;
    if (locked && str_eq(locked, "1")) bl_locked = 1;
    if (vbs && (str_eq(vbs, "green") || str_eq(vbs, "yellow"))) bl_locked = 1;
    free(locked); free(vbs);

    /* dtbo 形态 */
    avb_layout lay;
    if (img && len) {
        if (avb_probe(img, len, &lay) == 0) out->dtbo_form = lay.form;
        else out->dtbo_form = AVB_FORM_NONE;
        out->snapshot_dirty = detect_dtbo_dirty(img, len);
    } else {
        out->dtbo_form = AVB_FORM_NONE;
    }

    /* 判定表 */
    if (bl_locked) {
        out->mode = MODE_BLOCKED;
        out->mode_locked = 1;
        out->block_reason = xstrdup(
            "引导程序处于锁定状态，与本模块前提不符。锁定状态下写坏 dtbo "
            "无法用 fastboot 回刷，拒绝写入。");
    } else if (out->verification == TRI_NO) {
        if (out->dtbo_form == AVB_FORM_FOOTER) {
            out->mode = MODE_SELFSIGN;
            out->mode_locked = 0;
            out->requires_stock = 0;
        } else {
            out->mode = MODE_RAW;
            out->mode_locked = 1;
            out->requires_stock = 0;
            buf_printf(&note, "dtbo 无 AVB footer，直接裸写; ");
        }
    } else {
        /* 校验开启或未知，一律保守走免解拼接 */
        if (out->dtbo_form == AVB_FORM_FOOTER) {
            out->mode = MODE_GRAFT;
            out->mode_locked = 1;
            out->requires_stock = 1;
        } else {
            out->mode = MODE_BLOCKED;
            out->mode_locked = 1;
            out->block_reason = xstrdup(
                "AVB 校验仍开启但 dtbo 没有可用的 AVB footer，"
                "既不能裸写也没有官方 vbmeta 可搬。请先关闭 AVB 校验后重试。");
        }
    }

    buf_u8(&note, 0);
    out->note = (char *)note.data;
}

void detect_free(detect_t *d)
{
    free(d->block_reason);
    free(d->note);
    free(d->bl_state);
    memset(d, 0, sizeof(*d));
}

void detect_json(const detect_t *d, buf_t *b)
{
    buf_printf(b, "{\n");
    buf_printf(b, "  \"verification\": \"%s\",\n", tri_name(d->verification));
    buf_printf(b, "  \"verity\": \"%s\",\n", tri_name(d->verity));
    buf_printf(b, "  \"dtbo_form\": \"%s\",\n", avb_form_name(d->dtbo_form));
    buf_printf(b, "  \"mode\": \"%s\",\n", mode_name(d->mode));
    buf_printf(b, "  \"mode_locked\": %s,\n", d->mode_locked ? "true" : "false");
    buf_printf(b, "  \"requires_stock\": %s,\n", d->requires_stock ? "true" : "false");
    buf_printf(b, "  \"spoof_detected\": %s,\n", d->spoof_detected ? "true" : "false");
    buf_printf(b, "  \"snapshot_dirty\": %s,\n", d->snapshot_dirty ? "true" : "false");
    buf_printf(b, "  \"fastboot_rescue\": %s,\n",
               d->mode == MODE_BLOCKED ? "false" : "true");
    buf_printf(b, "  \"bl_state\": ");
    json_escape(b, d->bl_state);
    buf_printf(b, ",\n  \"note\": ");
    json_escape(b, d->note);
    buf_printf(b, ",\n  \"block_reason\": ");
    json_escape(b, d->block_reason);
    buf_printf(b, "\n}\n");
}
