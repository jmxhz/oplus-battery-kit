#include "profile.h"
#include "crypto.h"
#include <ctype.h>

/* ============================================================== 规则解析 */

static char *dupset(char **slot, const char *v)
{
    free(*slot);
    *slot = xstrdup(v);
    return *slot;
}

static rule_op *op_new(rule_target *t, op_kind k, int line)
{
    rule_op *o = xcalloc(1, sizeof(rule_op));
    o->kind = k;
    o->line = line;
    rule_op **pp = &t->ops;
    while (*pp) pp = &(*pp)->next;
    *pp = o;
    return o;
}

static rule_target *target_new(rule_sec *s)
{
    rule_target *t = xcalloc(1, sizeof(rule_target));
    rule_target **pp = &s->targets;
    while (*pp) pp = &(*pp)->next;
    *pp = t;
    return t;
}

/* 把 "a/b : c = v" 拆成 node_rel / name / value；无冒号则 node_rel 为空 */
static void split_assign(const char *rest, char **node_rel, char **name, char **value)
{
    *node_rel = *name = *value = NULL;
    const char *eq = strchr(rest, '=');
    if (!eq) return;
    char *lhs = xstrndup(rest, (size_t)(eq - rest));
    char *rhs = xstrdup(eq + 1);
    char *colon = strchr(lhs, ':');
    if (colon) {
        *colon = 0;
        *node_rel = xstrdup(str_trim(lhs));
        *name     = xstrdup(str_trim(colon + 1));
    } else {
        *name = xstrdup(str_trim(lhs));
    }
    *value = xstrdup(str_trim(rhs));
    free(lhs); free(rhs);
    if (*node_rel && !**node_rel) { free(*node_rel); *node_rel = NULL; }
}

static void sec_free(rule_sec *s)
{
    free(s->id); free(s->title); free(s->desc); free(s->warn);
    free(s->def); free(s->devices); free(s->handler);
    free(s->suggest); free(s->requires_); free(s->conflicts);
    rule_target *t = s->targets;
    while (t) {
        rule_target *tn = t->next;
        rule_op *o = t->ops;
        while (o) {
            rule_op *on = o->next;
            free(o->node_rel); free(o->name); free(o->value); free(o->var);
            free(o);
            o = on;
        }
        free(t->label); free(t->prefer); free(t->fixup); free(t->match);
        free(t);
        t = tn;
    }
    free(s);
}

static void rules_remove(ruleset *rs, const char *id)
{
    rule_sec **pp = &rs->secs;
    while (*pp) {
        if (str_eq((*pp)->id, id)) {
            rule_sec *d = *pp;
            *pp = d->next;
            d->next = NULL;
            sec_free(d);
            return;
        }
        pp = &(*pp)->next;
    }
}

static int parse_file(ruleset *rs, const char *path)
{
    buf_t b;
    if (file_read(path, &b) != 0) return -1;
    buf_u8(&b, 0);

    rule_sec    *sec = NULL;
    rule_target *tgt = NULL;
    char        *s   = (char *)b.data;
    int          line = 0;

    while (s && *s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        line++;
        char *L = str_trim(s);
        char *hash = strchr(L, '#');
        if (hash) { *hash = 0; L = str_trim(L); }
        if (!*L) goto next;

        if (L[0] == '[') {
            char *end = strchr(L, ']');
            if (!end) { warn("%s:%d 段头缺少 ]", path, line); goto next; }
            *end = 0;
            char *id = L + 1;
            int force = 0;
            if (*id == '!') { force = 1; id++; }
            rules_remove(rs, id);          /* 机型文件整段覆盖 */
            sec = xcalloc(1, sizeof(rule_sec));
            sec->id     = xstrdup(id);
            sec->force  = force;
            sec->reboot = 1;
            sec->def    = xstrdup(force ? "on" : "off");
            rule_sec **pp = &rs->secs;
            while (*pp) pp = &(*pp)->next;
            *pp = sec;
            tgt = NULL;
            goto next;
        }
        if (!sec) { warn("%s:%d 段外指令被忽略", path, line); goto next; }

        char *sp = L;
        while (*sp && !isspace((unsigned char)*sp)) sp++;
        char kw[32];
        size_t kl = (size_t)(sp - L);
        if (kl >= sizeof(kw)) kl = sizeof(kw) - 1;
        memcpy(kw, L, kl); kw[kl] = 0;
        char *rest = str_trim(sp);

        if      (str_eq(kw, "title"))     dupset(&sec->title, rest);
        else if (str_eq(kw, "desc"))      dupset(&sec->desc, rest);
        else if (str_eq(kw, "warn"))      dupset(&sec->warn, rest);
        else if (str_eq(kw, "default"))   dupset(&sec->def, rest);
        else if (str_eq(kw, "devices"))   dupset(&sec->devices, rest);
        else if (str_eq(kw, "handler"))   dupset(&sec->handler, rest);
        else if (str_eq(kw, "suggest"))   dupset(&sec->suggest, rest);
        else if (str_eq(kw, "requires"))  dupset(&sec->requires_, rest);
        else if (str_eq(kw, "conflicts")) dupset(&sec->conflicts, rest);
        else if (str_eq(kw, "force"))     sec->force = 1;
        else if (str_eq(kw, "reboot"))    sec->reboot = str_eq(rest, "no") ? 0 : 1;
        else if (str_eq(kw, "kind"))      sec->runtime = str_eq(rest, "runtime");
        else if (str_eq(kw, "node")) {
            tgt = target_new(sec);
            tgt->label = xstrdup(rest[0] == '&' ? rest + 1 : rest);
        }
        else if (str_eq(kw, "fixup")) {
            tgt = target_new(sec);
            tgt->fixup = xstrdup(rest);
        }
        else if (str_eq(kw, "prefer")) {
            if (tgt) dupset(&tgt->prefer, rest);
        }
        else if (str_eq(kw, "match")) {
            if (tgt) dupset(&tgt->match, rest);
        }
        else if (str_eq(kw, "read")) {
            if (!tgt) { warn("%s:%d read 出现在 node 之前", path, line); goto next; }
            /* read $uv = prop */
            char *eq = strchr(rest, '=');
            if (!eq) { warn("%s:%d read 缺少 =", path, line); goto next; }
            *eq = 0;
            char *v = str_trim(rest);
            if (*v == '$') v++;
            rule_op *o = op_new(tgt, OP_READ, line);
            o->var  = xstrdup(v);
            o->name = xstrdup(str_trim(eq + 1));
        }
        else if (str_eq(kw, "set") || str_eq(kw, "each") || str_eq(kw, "verify")) {
            if (!tgt) { warn("%s:%d %s 出现在 node 之前", path, line, kw); goto next; }
            op_kind k = str_eq(kw, "set") ? OP_SET :
                        str_eq(kw, "each") ? OP_EACH : OP_VERIFY;
            char *r2 = rest;
            if (k == OP_VERIFY) {
                /* verify 用 ==，先归一成 = */
                char *dbl = strstr(rest, "==");
                if (dbl) { memmove(dbl, dbl + 1, strlen(dbl + 1) + 1); }
            }
            rule_op *o = op_new(tgt, k, line);
            split_assign(r2, &o->node_rel, &o->name, &o->value);
            if (!o->name || !o->value)
                warn("%s:%d %s 语法错误", path, line, kw);
        }
        else if (str_eq(kw, "rm") || str_eq(kw, "rm?")) {
            if (!tgt) { warn("%s:%d rm 出现在 node 之前", path, line); goto next; }
            rule_op *o = op_new(tgt, str_eq(kw, "rm") ? OP_RM : OP_RM_OPT, line);
            o->name = xstrdup(rest);
        }
        else warn("%s:%d 未知指令 %s", path, line, kw);
next:
        s = nl ? nl + 1 : NULL;
    }
    buf_free(&b);
    return 0;
}

ruleset *rules_load(const char *dir, const char *device)
{
    ruleset *rs = xcalloc(1, sizeof(ruleset));
    rs->device = xstrdup(device ? device : "");

    char *c = path_join(dir, "common.rule");
    if (parse_file(rs, c) != 0) { free(c); rules_free(rs); return NULL; }
    free(c);

    if (device && *device) {
        char fn[256];
        snprintf(fn, sizeof(fn), "%s.rule", device);
        char *d = path_join(dir, fn);
        if (file_exists(d)) parse_file(rs, d);
        free(d);
    }
    return rs;
}

void rules_free(ruleset *rs)
{
    if (!rs) return;
    rule_sec *s = rs->secs;
    while (s) { rule_sec *n = s->next; sec_free(s); s = n; }
    free(rs->device);
    free(rs);
}

rule_sec *rules_find(ruleset *rs, const char *id)
{
    for (rule_sec *s = rs->secs; s; s = s->next)
        if (str_eq(s->id, id)) return s;
    return NULL;
}

int rules_applies(const rule_sec *s, const char *device)
{
    if (!s->devices || !*s->devices) return 1;
    int n = 0;
    char **v = str_split(s->devices, ' ', &n);
    int ok = 0;
    for (int i = 0; i < n; i++) {
        char *p = str_trim(v[i]);
        if (*p && glob_match(p, device)) { ok = 1; break; }
    }
    str_split_free(v, n);
    return ok;
}

/* ============================================================== 值求解 */

#define MAX_VARS 32
typedef struct { char *name[MAX_VARS]; uint32_t val[MAX_VARS]; int n; } varmap;

static void vars_set(varmap *m, const char *name, uint32_t v)
{
    for (int i = 0; i < m->n; i++)
        if (str_eq(m->name[i], name)) { m->val[i] = v; return; }
    if (m->n >= MAX_VARS) return;
    m->name[m->n] = xstrdup(name);
    m->val[m->n] = v;
    m->n++;
}
static int vars_get(varmap *m, const char *name, uint32_t *out)
{
    for (int i = 0; i < m->n; i++)
        if (str_eq(m->name[i], name)) { *out = m->val[i]; return 0; }
    return -1;
}
static void vars_free(varmap *m)
{
    for (int i = 0; i < m->n; i++) free(m->name[i]);
    m->n = 0;
}

/* 解析 "<a b c>" / "\"str\"" / "!"  -> 二进制属性值 */
static int eval_value(const char *txt, varmap *vars, buf_t *out)
{
    buf_init(out);
    const char *p = txt;
    while (*p && isspace((unsigned char)*p)) p++;

    if (*p == '!') return 0;                       /* 空属性 */

    if (*p == '"') {
        const char *e = strrchr(p + 1, '"');
        if (!e) return -1;
        buf_append(out, p + 1, (size_t)(e - p - 1));
        buf_u8(out, 0);
        return 0;
    }

    if (*p != '<') return -1;
    p++;
    while (*p && *p != '>') {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p || *p == '>') break;
        uint32_t v = 0;
        if (*p == '$') {
            p++;
            const char *s = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            char *nm = xstrndup(s, (size_t)(p - s));
            if (vars_get(vars, nm, &v) != 0) {
                err("未定义的变量 $%s", nm);
                free(nm);
                buf_free(out);
                return -1;
            }
            free(nm);
        } else {
            char *end = NULL;
            unsigned long long n = strtoull(p, &end, 0);
            if (end == p) { buf_free(out); return -1; }
            v = (uint32_t)n;
            p = end;
        }
        buf_be32(out, v);
    }
    return 0;
}

/* ============================================================== 目标解析 */

typedef struct {
    fdt_t    *fdt;
    fdt_node *node;
    int       entry;
} scope;

/* 在某个 dtb 条目里解析 target 的作用域，成功返回 0 */
static int resolve_target(dtbo_t *d, int i, rule_target *t, scope *sc)
{
    fdt_t *f = dtbo_fdt(d, i);
    if (!f) return -1;
    sc->fdt = f;
    sc->entry = i;

    if (t->fixup) {
        char **paths = NULL;
        int n = fdt_fixup_targets(f, t->fixup, &paths);
        int found = -1;
        for (int k = 0; k < n && found < 0; k++) {
            char sub[512];
            snprintf(sub, sizeof(sub), "%s/__overlay__", paths[k]);
            fdt_node *ov = fdt_find(f, sub);
            if (!ov) continue;
            if (t->match) {
                if (!fdt_child(ov, t->match) && !fdt_child_glob(ov, t->match)) continue;
            }
            sc->node = ov;
            found = k;
        }
        for (int k = 0; k < n; k++) free(paths[k]);
        free(paths);
        return found >= 0 ? 0 : -1;
    }

    const char *sym = fdt_symbol(f, t->label);
    if (!sym) return -1;
    fdt_node *n = fdt_find(f, sym);
    if (!n) return -1;
    if (t->prefer) {
        fdt_node *c = fdt_child_glob(n, t->prefer);
        if (c) n = c;
    }
    sc->node = n;
    return 0;
}

/* ============================================================== 快照 */

snapshot *snap_new(const char *fp, const char *dev, const char *src,
                   uint64_t partsize, int entries)
{
    snapshot *s = xcalloc(1, sizeof(snapshot));
    s->fingerprint = xstrdup(fp ? fp : "");
    s->device      = xstrdup(dev ? dev : "");
    s->source      = xstrdup(src ? src : "live");
    s->partsize    = partsize;
    s->entries     = entries;
    return s;
}

void snap_free(snapshot *s)
{
    if (!s) return;
    snap_item *it = s->items;
    while (it) {
        snap_item *n = it->next;
        free(it->path); free(it->name); free(it->data); free(it);
        it = n;
    }
    free(s->fingerprint); free(s->device); free(s->source);
    free(s);
}

static snap_item *snap_add(snapshot *s, int entry, const char *path,
                           const char *name, const void *data, size_t len,
                           int is_tree)
{
    /* 同一 (entry,path,name) 只记录首次，保证是原厂值 */
    for (snap_item *it = s->items; it; it = it->next) {
        if (it->entry == entry && it->is_tree == is_tree &&
            str_eq(it->path, path) && str_eq(it->name, name))
            return it;
    }
    snap_item *it = xcalloc(1, sizeof(snap_item));
    it->entry = entry;
    it->path  = xstrdup(path);
    it->name  = name ? xstrdup(name) : NULL;
    it->is_tree = is_tree;
    if (data) {
        it->len  = len;
        it->data = xmalloc(len ? len : 1);
        memcpy(it->data, data, len);
    }
    snap_item **pp = &s->items;
    while (*pp) pp = &(*pp)->next;
    *pp = it;
    return it;
}

int snap_save(const snapshot *s, const char *path)
{
    buf_t b; buf_init(&b);
    buf_printf(&b, "# obk-snapshot v1\n");
    buf_printf(&b, "meta fingerprint %s\n", s->fingerprint);
    buf_printf(&b, "meta device %s\n", s->device);
    buf_printf(&b, "meta source %s\n", s->source);
    buf_printf(&b, "meta partsize %llu\n", (unsigned long long)s->partsize);
    buf_printf(&b, "meta entries %d\n", s->entries);
    for (snap_item *it = s->items; it; it = it->next) {
        if (it->is_tree) {
            buf_printf(&b, "tree %d %s ", it->entry, it->path);
        } else {
            buf_printf(&b, "prop %d %s %s ", it->entry, it->path, it->name);
        }
        if (!it->data) buf_printf(&b, "-");
        else b64_encode(it->data, it->len, &b);
        buf_u8(&b, '\n');
    }
    int rc = file_write(path, b.data, b.len);
    buf_free(&b);
    return rc;
}

snapshot *snap_load(const char *path)
{
    buf_t b;
    if (file_read(path, &b) != 0) return NULL;
    buf_u8(&b, 0);
    snapshot *s = snap_new("", "", "live", 0, 0);
    char *p = (char *)b.data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *L = str_trim(p);
        if (*L && L[0] != '#') {
            if (str_startswith(L, "meta ")) {
                char *k = L + 5;
                char *sp = strchr(k, ' ');
                if (sp) {
                    *sp = 0;
                    char *v = sp + 1;
                    if      (str_eq(k, "fingerprint")) dupset(&s->fingerprint, v);
                    else if (str_eq(k, "device"))      dupset(&s->device, v);
                    else if (str_eq(k, "source"))      dupset(&s->source, v);
                    else if (str_eq(k, "partsize"))    s->partsize = strtoull(v, NULL, 10);
                    else if (str_eq(k, "entries"))     s->entries = atoi(v);
                }
            } else if (str_startswith(L, "prop ") || str_startswith(L, "tree ")) {
                int is_tree = L[0] == 't';
                int nf = 0;
                char **v = str_split(L, ' ', &nf);
                int need = is_tree ? 4 : 5;
                if (nf >= need) {
                    const char *b64 = v[need - 1];
                    buf_t data; buf_init(&data);
                    int absent = str_eq(b64, "-");
                    if (!absent) b64_decode(b64, &data);
                    snap_add(s, atoi(v[1]), v[2], is_tree ? NULL : v[3],
                             absent ? NULL : data.data, data.len, is_tree);
                    buf_free(&data);
                }
                str_split_free(v, nf);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    buf_free(&b);
    return s;
}

/* ------------------------------------------------------ 快照采集 -------- */

typedef struct { fdt_node **arr; int *cnt; int cap; } nodelist;

static void collect_nodes(fdt_node *n, void *ud)
{
    nodelist *c = ud;
    if (*c->cnt < c->cap) c->arr[(*c->cnt)++] = n;
}

/* 把 "a/b/leaf" 拆成父路径与叶名，父路径可含通配 */
static void split_leaf(const char *spec, char *parent, size_t psz, const char **leaf,
                       char ***parts_out, int *nparts)
{
    int nn = 0;
    char **parts = str_split(spec, '/', &nn);
    parent[0] = 0;
    for (int k = 0; k + 1 < nn; k++) {
        if (parent[0]) strncat(parent, "/", psz - strlen(parent) - 1);
        strncat(parent, parts[k], psz - strlen(parent) - 1);
    }
    *leaf = nn ? parts[nn - 1] : spec;
    *parts_out = parts;
    *nparts = nn;
}

static void record_prop(snapshot *s, scope *sc, fdt_node *n, const char *prop)
{
    char *path = fdt_node_path(n);
    fdt_prop *p = fdt_getprop(n, prop);
    snap_add(s, sc->entry, path, prop, p ? p->data : NULL, p ? p->len : 0, 0);
    free(path);
}

typedef struct { snapshot *s; scope *sc; const char *glob; } eachrec;

static void each_record_cb(fdt_node *n, void *ud)
{
    eachrec *e = ud;
    for (fdt_prop *p = n->props; p; p = p->next)
        if (glob_match(e->glob, p->name))
            record_prop(e->s, e->sc, n, p->name);
}

snapshot *prof_snapshot(ruleset *rs, dtbo_t *d, const char *fp,
                        const char *dev, const char *src, uint64_t partsize)
{
    snapshot *s = snap_new(fp, dev, src, partsize, d->n);

    for (rule_sec *sec = rs->secs; sec; sec = sec->next) {
        if (sec->runtime) continue;
        if (!rules_applies(sec, dev)) continue;
        for (rule_target *t = sec->targets; t; t = t->next) {
            for (int i = 0; i < d->n; i++) {
                scope sc;
                if (resolve_target(d, i, t, &sc) != 0) continue;
                for (rule_op *o = t->ops; o; o = o->next) {
                    switch (o->kind) {
                    case OP_SET: {
                        fdt_node *n = fdt_walk(sc.node, o->node_rel);
                        if (n) record_prop(s, &sc, n, o->name);
                        break;
                    }
                    case OP_EACH: {
                        eachrec e = { s, &sc, o->name };
                        fdt_walk_glob(sc.node, o->node_rel, each_record_cb, &e);
                        break;
                    }
                    case OP_RM:
                    case OP_RM_OPT: {
                        /* 通配可能命中多个，逐个记录整棵子树 */
                        char rel[512]; const char *leaf; char **parts; int nn;
                        split_leaf(o->name, rel, sizeof(rel), &leaf, &parts, &nn);

                        fdt_node *bases[64]; int nb = 0;
                        nodelist nl = { bases, &nb, 64 };
                        if (!rel[0]) bases[nb++] = sc.node;
                        else fdt_walk_glob(sc.node, rel, collect_nodes, &nl);

                        for (int bi = 0; bi < nb; bi++) {
                            for (fdt_node *ch = bases[bi]->children; ch; ch = ch->next) {
                                if (!glob_match(leaf, ch->name)) continue;
                                char *cp = fdt_node_path(ch);
                                buf_t sub;
                                fdt_subtree_dump(ch, &sub);
                                snap_add(s, sc.entry, cp, NULL, sub.data, sub.len, 1);
                                buf_free(&sub);
                                free(cp);
                            }
                        }
                        str_split_free(parts, nn);
                        break;
                    }
                    default: break;
                    }
                }
            }
        }
    }
    return s;
}

int prof_restore_baseline(snapshot *s, dtbo_t *d)
{
    int touched = 0;
    /* 先还原属性，再还原子树，避免子树被后续属性写入影响 */
    for (int pass = 0; pass < 2; pass++) {
        for (snap_item *it = s->items; it; it = it->next) {
            if ((pass == 0) == (it->is_tree != 0)) continue;
            if (it->entry < 0 || it->entry >= d->n) continue;
            fdt_t *f = dtbo_fdt(d, it->entry);
            if (!f) continue;

            if (it->is_tree) {
                if (fdt_find(f, it->path)) continue;      /* 还在就不动 */
                char *parent = xstrdup(it->path);
                char *slash = strrchr(parent, '/');
                if (!slash) { free(parent); continue; }
                *slash = 0;
                fdt_node *pn = fdt_find(f, parent[0] ? parent : "/");
                free(parent);
                if (!pn) continue;
                if (fdt_subtree_load(pn, it->data, it->len)) {
                    dtbo_touch(d, it->entry);
                    touched++;
                }
            } else {
                fdt_node *n = fdt_find(f, it->path);
                if (!n) continue;
                fdt_prop *cur = fdt_getprop(n, it->name);
                if (!it->data) {
                    if (cur) { fdt_delprop(n, it->name); dtbo_touch(d, it->entry); touched++; }
                } else if (!cur || cur->len != it->len ||
                           memcmp(cur->data, it->data, it->len) != 0) {
                    fdt_setprop(n, it->name, it->data, (uint32_t)it->len);
                    dtbo_touch(d, it->entry);
                    touched++;
                }
            }
        }
    }
    return touched;
}

/* ============================================================== 应用 */

typedef struct {
    dtbo_t   *d;
    scope    *sc;
    rule_op  *op;
    varmap   *vars;
    buf_t    *log;
    int       count;
    int       fail;
} applyctx;

static void each_apply_cb(fdt_node *n, void *ud)
{
    applyctx *c = ud;
    buf_t val;
    if (eval_value(c->op->value, c->vars, &val) != 0) { c->fail++; return; }
    for (fdt_prop *p = n->props; p; p = p->next) {
        if (!glob_match(c->op->name, p->name)) continue;
        if (p->len != val.len || memcmp(p->data, val.data, val.len) != 0) {
            fdt_setprop(n, p->name, val.data, (uint32_t)val.len);
            dtbo_touch(c->d, c->sc->entry);
        }
        c->count++;
    }
    buf_free(&val);
}

int prof_apply_section(rule_sec *sec, dtbo_t *d, buf_t *log)
{
    int applied = 0, hard_fail = 0;

    for (rule_target *t = sec->targets; t; t = t->next) {
        for (int i = 0; i < d->n; i++) {
            scope sc;
            if (resolve_target(d, i, t, &sc) != 0) continue;

            varmap vars; memset(&vars, 0, sizeof(vars));

            for (rule_op *o = t->ops; o; o = o->next) {
                switch (o->kind) {
                case OP_READ: {
                    fdt_node *n = fdt_walk(sc.node, o->node_rel);
                    fdt_prop *p = n ? fdt_getprop(n, o->name) : NULL;
                    if (!p || p->len < 4) {
                        if (log) buf_printf(log, "  条目%d 读不到 %s\n", i, o->name);
                        hard_fail++;
                    } else {
                        vars_set(&vars, o->var, rd_be32(p->data));
                    }
                    break;
                }
                case OP_SET: {
                    fdt_node *n = fdt_walk(sc.node, o->node_rel);
                    if (!n) {
                        if (log) buf_printf(log, "  条目%d 找不到节点 %s\n",
                                            i, o->node_rel ? o->node_rel : ".");
                        hard_fail++;
                        break;
                    }
                    buf_t val;
                    if (eval_value(o->value, &vars, &val) != 0) { hard_fail++; break; }
                    fdt_prop *cur = fdt_getprop(n, o->name);
                    if (!cur || cur->len != val.len ||
                        memcmp(cur->data, val.data, val.len) != 0) {
                        fdt_setprop(n, o->name, val.data, (uint32_t)val.len);
                        dtbo_touch(d, i);
                    }
                    buf_free(&val);
                    applied++;
                    break;
                }
                case OP_EACH: {
                    applyctx c = { d, &sc, o, &vars, log, 0, 0 };
                    fdt_walk_glob(sc.node, o->node_rel, each_apply_cb, &c);
                    if (c.fail) hard_fail += c.fail;
                    if (c.count == 0) {
                        if (log) buf_printf(log, "  条目%d each 未命中 %s:%s\n",
                                            i, o->node_rel ? o->node_rel : ".", o->name);
                    }
                    applied += c.count;
                    break;
                }
                case OP_RM:
                case OP_RM_OPT: {
                    char rel[512]; const char *leaf; char **parts; int nn;
                    split_leaf(o->name, rel, sizeof(rel), &leaf, &parts, &nn);

                    fdt_node *bases[64]; int nb = 0;
                    nodelist cc = { bases, &nb, 64 };
                    if (!rel[0]) bases[nb++] = sc.node;
                    else fdt_walk_glob(sc.node, rel, collect_nodes, &cc);

                    int hit = 0;
                    for (int bi = 0; bi < nb; bi++) {
                        fdt_node *ch = bases[bi]->children;
                        while (ch) {
                            fdt_node *nx = ch->next;
                            if (glob_match(leaf, ch->name)) {
                                fdt_del_node(ch);
                                dtbo_touch(d, i);
                                hit++;
                            }
                            ch = nx;
                        }
                    }
                    if (!hit && o->kind == OP_RM) {
                        if (log) buf_printf(log, "  条目%d 待删节点不存在 %s\n", i, o->name);
                    }
                    applied += hit;
                    str_split_free(parts, nn);
                    break;
                }
                case OP_VERIFY:
                    break;
                }
            }
            vars_free(&vars);
        }
    }

    if (log && applied)
        buf_printf(log, "  %s: 生效 %d 处\n", sec->id, applied);
    if (hard_fail) return -1;
    return applied > 0 ? 0 : 1;
}

/* ---------------------------------------------------------- 读回校验 -- */

static int verify_one(scope *sc, rule_op *o, varmap *vars)
{
    fdt_node *n = fdt_walk(sc->node, o->node_rel);
    if (!n) return 0;
    buf_t want;
    if (eval_value(o->value, vars, &want) != 0) return 0;
    int ok = 0;
    if (strchr(o->name, '*')) {
        ok = 1;
        int any = 0;
        for (fdt_prop *p = n->props; p; p = p->next) {
            if (!glob_match(o->name, p->name)) continue;
            any = 1;
            if (p->len != want.len || memcmp(p->data, want.data, want.len) != 0) ok = 0;
        }
        if (!any) ok = 0;
    } else {
        fdt_prop *p = fdt_getprop(n, o->name);
        ok = p && p->len == want.len && memcmp(p->data, want.data, want.len) == 0;
    }
    buf_free(&want);
    return ok;
}

int prof_verify_section(rule_sec *sec, dtbo_t *d, buf_t *log)
{
    int checked = 0, bad = 0;
    for (rule_target *t = sec->targets; t; t = t->next) {
        for (int i = 0; i < d->n; i++) {
            scope sc;
            if (resolve_target(d, i, t, &sc) != 0) continue;
            varmap vars; memset(&vars, 0, sizeof(vars));
            for (rule_op *o = t->ops; o; o = o->next) {
                if (o->kind == OP_READ) {
                    fdt_node *n = fdt_walk(sc.node, o->node_rel);
                    fdt_prop *p = n ? fdt_getprop(n, o->name) : NULL;
                    if (p && p->len >= 4) vars_set(&vars, o->var, rd_be32(p->data));
                } else if (o->kind == OP_VERIFY) {
                    checked++;
                    if (!verify_one(&sc, o, &vars)) {
                        bad++;
                        if (log) buf_printf(log, "  条目%d 断言不成立: %s (行 %d)\n",
                                            i, o->name, o->line);
                    }
                }
            }
            vars_free(&vars);
        }
    }
    if (!checked) return 0;
    return bad ? -1 : 0;
}

int prof_section_active(rule_sec *sec, dtbo_t *d)
{
    int checked = 0, good = 0;
    for (rule_target *t = sec->targets; t; t = t->next) {
        for (int i = 0; i < d->n; i++) {
            scope sc;
            if (resolve_target(d, i, t, &sc) != 0) continue;
            varmap vars; memset(&vars, 0, sizeof(vars));
            for (rule_op *o = t->ops; o; o = o->next) {
                if (o->kind == OP_READ) {
                    fdt_node *n = fdt_walk(sc.node, o->node_rel);
                    fdt_prop *p = n ? fdt_getprop(n, o->name) : NULL;
                    if (p && p->len >= 4) vars_set(&vars, o->var, rd_be32(p->data));
                } else if (o->kind == OP_VERIFY) {
                    checked++;
                    if (verify_one(&sc, o, &vars)) good++;
                }
            }
            vars_free(&vars);
        }
    }
    if (!checked) {
        /* 无断言的段（例如纯 rm），用「目标节点是否已消失」判断 */
        for (rule_target *t = sec->targets; t; t = t->next) {
            for (int i = 0; i < d->n; i++) {
                scope sc;
                if (resolve_target(d, i, t, &sc) != 0) continue;
                for (rule_op *o = t->ops; o; o = o->next) {
                    if (o->kind != OP_RM && o->kind != OP_RM_OPT) continue;
                    checked++;
                    char rel[512]; const char *leaf; char **parts; int nn;
                    split_leaf(o->name, rel, sizeof(rel), &leaf, &parts, &nn);
                    fdt_node *bases[64]; int nb = 0;
                    nodelist cc = { bases, &nb, 64 };
                    if (!rel[0]) bases[nb++] = sc.node;
                    else fdt_walk_glob(sc.node, rel, collect_nodes, &cc);
                    int present = 0;
                    for (int bi = 0; bi < nb; bi++)
                        for (fdt_node *ch = bases[bi]->children; ch; ch = ch->next)
                            if (glob_match(leaf, ch->name)) present = 1;
                    if (!present) good++;
                    str_split_free(parts, nn);
                }
            }
        }
    }
    return checked > 0 && good == checked;
}
