#include "cfg.h"
#include "crypto.h"

char *obk_path(const char *rel) { return path_join(g_root, rel); }

char *obk_avb_dir(void) { return obk_path("avb"); }

char *obk_prof_dir(void)
{
    if (g_profdir && *g_profdir) return xstrdup(g_profdir);
    if (file_exists("/data/adb/modules/oplus_batt_kit/profiles"))
        return xstrdup("/data/adb/modules/oplus_batt_kit/profiles");
    return obk_path("profiles");
}

/* 用 fingerprint 的 md5 前 16 位做文件名，避开路径非法字符 */
char *obk_snap_path(const char *fp)
{
    uint8_t d[16]; char hex[33];
    md5(fp ? fp : "", fp ? strlen(fp) : 0, d);
    md5_hex(d, hex);
    hex[16] = 0;
    char rel[128];
    snprintf(rel, sizeof(rel), "stock/%s.snap", hex);
    return obk_path(rel);
}

char *dtbo_partition(void)
{
    if (g_dtbo_path && *g_dtbo_path) return xstrdup(g_dtbo_path);
    char *slot = getprop("ro.boot.slot_suffix");
    if (!slot) slot = cmdline_get("slot_suffix");
    char p[256];
    snprintf(p, sizeof(p), "/dev/block/bootdevice/by-name/dtbo%s", slot ? slot : "");
    free(slot);
    return xstrdup(p);
}

/* ------------------------------------------------------------- 配置 -- */

cfg_t *cfg_load(void)
{
    cfg_t *c = xcalloc(1, sizeof(cfg_t));
    char *p = obk_path("config");
    buf_t b;
    if (file_read(p, &b) == 0) {
        buf_u8(&b, 0);
        char *s = (char *)b.data;
        while (s && *s) {
            char *nl = strchr(s, '\n');
            if (nl) *nl = 0;
            char *L = str_trim(s);
            if (*L && L[0] != '#') {
                char *eq = strchr(L, '=');
                if (eq) { *eq = 0; cfg_set(c, str_trim(L), str_trim(eq + 1)); }
            }
            s = nl ? nl + 1 : NULL;
        }
        buf_free(&b);
    }
    free(p);
    return c;
}

int cfg_save(cfg_t *c)
{
    if (mkdir_p(g_root) != 0) return -1;
    buf_t b; buf_init(&b);
    for (cfg_item *i = c->items; i; i = i->next)
        buf_printf(&b, "%s=%s\n", i->key, i->val);
    char *p = obk_path("config");
    int rc = file_write(p, b.data, b.len);
    free(p);
    buf_free(&b);
    return rc;
}

void cfg_free(cfg_t *c)
{
    if (!c) return;
    cfg_item *i = c->items;
    while (i) { cfg_item *n = i->next; free(i->key); free(i->val); free(i); i = n; }
    free(c);
}

const char *cfg_get(cfg_t *c, const char *key, const char *dflt)
{
    for (cfg_item *i = c->items; i; i = i->next)
        if (str_eq(i->key, key)) return i->val;
    return dflt;
}

int cfg_get_int(cfg_t *c, const char *key, int dflt)
{
    const char *v = cfg_get(c, key, NULL);
    if (!v) return dflt;
    if (str_eq(v, "on") || str_eq(v, "yes") || str_eq(v, "true")) return 1;
    if (str_eq(v, "off") || str_eq(v, "no") || str_eq(v, "false")) return 0;
    return atoi(v);
}

void cfg_set(cfg_t *c, const char *key, const char *val)
{
    for (cfg_item *i = c->items; i; i = i->next) {
        if (str_eq(i->key, key)) {
            free(i->val);
            i->val = xstrdup(val);
            return;
        }
    }
    cfg_item *i = xcalloc(1, sizeof(cfg_item));
    i->key = xstrdup(key);
    i->val = xstrdup(val);
    cfg_item **pp = &c->items;
    while (*pp) pp = &(*pp)->next;
    *pp = i;
}

void cfg_set_int(cfg_t *c, const char *key, int v)
{
    char t[16];
    snprintf(t, sizeof(t), "%d", v);
    cfg_set(c, key, t);
}
