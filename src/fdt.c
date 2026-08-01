#include "fdt.h"

/* ---------------------------------------------------------------- 解析 -- */

static fdt_node *node_new(const char *name, fdt_node *parent)
{
    fdt_node *n = xcalloc(1, sizeof(fdt_node));
    n->name = xstrdup(name);
    n->parent = parent;
    if (parent) {
        fdt_node **pp = &parent->children;
        while (*pp) pp = &(*pp)->next;
        *pp = n;
    }
    return n;
}

static void node_free(fdt_node *n)
{
    if (!n) return;
    fdt_prop *p = n->props;
    while (p) { fdt_prop *q = p->next; free(p->name); free(p->data); free(p); p = q; }
    fdt_node *c = n->children;
    while (c) { fdt_node *q = c->next; node_free(c); c = q; }
    free(n->name);
    free(n);
}

fdt_t *fdt_parse(const uint8_t *blob, size_t len)
{
    if (len < 40 || rd_be32(blob) != FDT_MAGIC) return NULL;

    uint32_t totalsize   = rd_be32(blob + 4);
    uint32_t off_struct  = rd_be32(blob + 8);
    uint32_t off_strings = rd_be32(blob + 12);
    uint32_t off_rsv     = rd_be32(blob + 16);
    uint32_t version     = rd_be32(blob + 20);
    uint32_t boot_cpuid  = rd_be32(blob + 28);
    uint32_t size_strings= rd_be32(blob + 32);
    uint32_t size_struct = rd_be32(blob + 36);

    if (totalsize > len) return NULL;
    if (version < 16) return NULL;
    if ((size_t)off_struct + size_struct > len)   return NULL;
    if ((size_t)off_strings + size_strings > len) return NULL;

    fdt_t *f = xcalloc(1, sizeof(fdt_t));
    f->boot_cpuid = boot_cpuid;

    /* 保留内存表：一直读到 (0,0) */
    if (off_rsv < len) {
        size_t p = off_rsv;
        while (p + 16 <= len) {
            uint64_t a = rd_be64(blob + p), s = rd_be64(blob + p + 8);
            p += 16;
            if (a == 0 && s == 0) break;
        }
        f->rsvlen = p - off_rsv;
        f->rsvmap = xmalloc(f->rsvlen);
        memcpy(f->rsvmap, blob + off_rsv, f->rsvlen);
    }

    const uint8_t *st  = blob + off_struct;
    const uint8_t *end = st + size_struct;
    const char    *strs= (const char *)(blob + off_strings);

    fdt_node *cur = NULL;
    const uint8_t *p = st;

    while (p + 4 <= end) {
        uint32_t tok = rd_be32(p);
        p += 4;
        if (tok == FDT_NOP) continue;
        if (tok == FDT_END) break;

        if (tok == FDT_BEGIN_NODE) {
            const char *nm = (const char *)p;
            size_t nl = strnlen(nm, (size_t)(end - p));
            if (nl == (size_t)(end - p)) goto bad;
            fdt_node *n = node_new(nm, cur);
            if (!cur) f->root = n;
            cur = n;
            p += nl + 1;
            p += (4 - ((uintptr_t)(p - st) % 4)) % 4;
        } else if (tok == FDT_END_NODE) {
            if (!cur) goto bad;
            cur = cur->parent;
        } else if (tok == FDT_PROP) {
            if (p + 8 > end || !cur) goto bad;
            uint32_t plen = rd_be32(p);
            uint32_t noff = rd_be32(p + 4);
            p += 8;
            if (p + plen > end || noff >= size_strings) goto bad;
            /* 字符串块里的名字必须在块内终止，否则 strlen 会越界读 */
            size_t navail = size_strings - noff;
            size_t nlen   = strnlen(strs + noff, navail);
            if (nlen == navail) goto bad;
            fdt_prop *pr = xcalloc(1, sizeof(fdt_prop));
            pr->name = xstrndup(strs + noff, nlen);
            pr->len  = plen;
            pr->data = xmalloc(plen ? plen : 1);
            memcpy(pr->data, p, plen);
            fdt_prop **pp = &cur->props;
            while (*pp) pp = &(*pp)->next;
            *pp = pr;
            p += plen;
            p += (4 - ((uintptr_t)(p - st) % 4)) % 4;
        } else {
            goto bad;
        }
    }
    if (!f->root) goto bad;
    return f;
bad:
    fdt_free(f);
    return NULL;
}

void fdt_free(fdt_t *f)
{
    if (!f) return;
    node_free(f->root);
    free(f->rsvmap);
    free(f);
}

/* ------------------------------------------------------------ 序列化 -- */

typedef struct { char **v; uint32_t *off; int n, cap; buf_t blob; } strtab;

static uint32_t strtab_add(strtab *t, const char *s)
{
    for (int i = 0; i < t->n; i++)
        if (strcmp(t->v[i], s) == 0) return t->off[i];
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->v   = xrealloc(t->v, sizeof(char *) * t->cap);
        t->off = xrealloc(t->off, sizeof(uint32_t) * t->cap);
    }
    uint32_t off = (uint32_t)t->blob.len;
    buf_append(&t->blob, s, strlen(s) + 1);
    t->v[t->n] = xstrdup(s);
    t->off[t->n] = off;
    t->n++;
    return off;
}

static void emit_node(fdt_node *n, buf_t *sb, strtab *t)
{
    buf_be32(sb, FDT_BEGIN_NODE);
    buf_append(sb, n->name, strlen(n->name) + 1);
    buf_align(sb, 4);
    for (fdt_prop *p = n->props; p; p = p->next) {
        buf_be32(sb, FDT_PROP);
        buf_be32(sb, p->len);
        buf_be32(sb, strtab_add(t, p->name));
        buf_append(sb, p->data, p->len);
        buf_align(sb, 4);
    }
    for (fdt_node *c = n->children; c; c = c->next) emit_node(c, sb, t);
    buf_be32(sb, FDT_END_NODE);
}

int fdt_serialize(fdt_t *f, buf_t *out)
{
    if (!f || !f->root) return -1;
    strtab t; memset(&t, 0, sizeof(t)); buf_init(&t.blob);
    buf_t sb; buf_init(&sb);

    emit_node(f->root, &sb, &t);
    buf_be32(&sb, FDT_END);

    uint32_t off_rsv     = 40;
    size_t   rsvlen      = f->rsvlen ? f->rsvlen : 16;
    uint32_t off_struct  = (uint32_t)(off_rsv + rsvlen);
    off_struct = (off_struct + 3) & ~3u;
    uint32_t off_strings = off_struct + (uint32_t)sb.len;
    uint32_t total       = off_strings + (uint32_t)t.blob.len;

    buf_init(out);
    buf_be32(out, FDT_MAGIC);
    buf_be32(out, total);
    buf_be32(out, off_struct);
    buf_be32(out, off_strings);
    buf_be32(out, off_rsv);
    buf_be32(out, 17);
    buf_be32(out, 16);
    buf_be32(out, f->boot_cpuid);
    buf_be32(out, (uint32_t)t.blob.len);
    buf_be32(out, (uint32_t)sb.len);

    if (f->rsvlen) buf_append(out, f->rsvmap, f->rsvlen);
    else { buf_be64(out, 0); buf_be64(out, 0); }
    while (out->len < off_struct) buf_u8(out, 0);
    buf_append(out, sb.data, sb.len);
    buf_append(out, t.blob.data, t.blob.len);

    buf_free(&sb);
    buf_free(&t.blob);
    for (int i = 0; i < t.n; i++) free(t.v[i]);
    free(t.v); free(t.off);
    return 0;
}

/* -------------------------------------------------------------- 查找 -- */

fdt_node *fdt_child(fdt_node *n, const char *name)
{
    if (!n) return NULL;
    for (fdt_node *c = n->children; c; c = c->next)
        if (strcmp(c->name, name) == 0) return c;
    return NULL;
}

fdt_node *fdt_child_glob(fdt_node *n, const char *pat)
{
    if (!n) return NULL;
    for (fdt_node *c = n->children; c; c = c->next)
        if (glob_match(pat, c->name)) return c;
    return NULL;
}

fdt_node *fdt_walk(fdt_node *base, const char *relpath)
{
    if (!base) return NULL;
    if (!relpath || !*relpath) return base;
    int n = 0;
    char **parts = str_split(relpath, '/', &n);
    fdt_node *cur = base;
    for (int i = 0; i < n && cur; i++) {
        if (!*parts[i]) continue;
        cur = strchr(parts[i], '*') ? fdt_child_glob(cur, parts[i])
                                    : fdt_child(cur, parts[i]);
    }
    str_split_free(parts, n);
    return cur;
}

static void walk_glob_rec(fdt_node *cur, char **parts, int n, int i,
                          fdt_walk_cb cb, void *ud)
{
    if (!cur) return;
    while (i < n && !*parts[i]) i++;
    if (i >= n) { cb(cur, ud); return; }
    if (strchr(parts[i], '*')) {
        for (fdt_node *c = cur->children; c; c = c->next)
            if (glob_match(parts[i], c->name))
                walk_glob_rec(c, parts, n, i + 1, cb, ud);
    } else {
        walk_glob_rec(fdt_child(cur, parts[i]), parts, n, i + 1, cb, ud);
    }
}

void fdt_walk_glob(fdt_node *base, const char *relpath, fdt_walk_cb cb, void *ud)
{
    if (!base) return;
    if (!relpath || !*relpath) { cb(base, ud); return; }
    int n = 0;
    char **parts = str_split(relpath, '/', &n);
    walk_glob_rec(base, parts, n, 0, cb, ud);
    str_split_free(parts, n);
}

fdt_node *fdt_find(fdt_t *f, const char *path)
{
    if (!f || !f->root) return NULL;
    if (!path || !*path || strcmp(path, "/") == 0) return f->root;
    return fdt_walk(f->root, path[0] == '/' ? path + 1 : path);
}

fdt_prop *fdt_getprop(fdt_node *n, const char *name)
{
    if (!n) return NULL;
    for (fdt_prop *p = n->props; p; p = p->next)
        if (strcmp(p->name, name) == 0) return p;
    return NULL;
}

void fdt_setprop(fdt_node *n, const char *name, const void *data, uint32_t len)
{
    fdt_prop *p = fdt_getprop(n, name);
    if (!p) {
        p = xcalloc(1, sizeof(fdt_prop));
        p->name = xstrdup(name);
        fdt_prop **pp = &n->props;
        while (*pp) pp = &(*pp)->next;
        *pp = p;
    } else {
        free(p->data);
    }
    p->len  = len;
    p->data = xmalloc(len ? len : 1);
    if (len) memcpy(p->data, data, len);
}

int fdt_delprop(fdt_node *n, const char *name)
{
    if (!n) return -1;
    fdt_prop **pp = &n->props;
    while (*pp) {
        if (strcmp((*pp)->name, name) == 0) {
            fdt_prop *d = *pp;
            *pp = d->next;
            free(d->name); free(d->data); free(d);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

fdt_node *fdt_add_child(fdt_node *parent, const char *name)
{
    fdt_node *e = fdt_child(parent, name);
    if (e) return e;
    return node_new(name, parent);
}

int fdt_del_node(fdt_node *n)
{
    if (!n || !n->parent) return -1;
    fdt_node **pp = &n->parent->children;
    while (*pp) {
        if (*pp == n) {
            *pp = n->next;
            n->next = NULL;
            node_free(n);
            return 0;
        }
        pp = &(*pp)->next;
    }
    return -1;
}

char *fdt_node_path(fdt_node *n)
{
    if (!n) return NULL;
    if (!n->parent) return xstrdup("/");
    const char *parts[64];
    int np = 0;
    for (fdt_node *c = n; c && c->parent && np < 64; c = c->parent)
        parts[np++] = c->name;
    buf_t b; buf_init(&b);
    for (int i = np - 1; i >= 0; i--) {
        buf_u8(&b, '/');
        buf_append(&b, parts[i], strlen(parts[i]));
    }
    buf_u8(&b, 0);
    return (char *)b.data;
}

const char *fdt_symbol(fdt_t *f, const char *label)
{
    fdt_node *s = fdt_child(f->root, "__symbols__");
    if (!s) return NULL;
    fdt_prop *p = fdt_getprop(s, label);
    if (!p || !p->len) return NULL;
    /* 调用方会当成 C 字符串用，未以 NUL 结尾的属性一律视为无效 */
    if (p->data[p->len - 1] != 0) return NULL;
    return (const char *)p->data;
}

int fdt_fixup_targets(fdt_t *f, const char *prop, char ***out)
{
    *out = NULL;
    fdt_node *fx = fdt_child(f->root, "__fixups__");
    if (!fx) return 0;
    fdt_prop *p = fdt_getprop(fx, prop);
    if (!p || !p->len) return 0;
    int cap = 8, n = 0;
    char **v = xmalloc(sizeof(char *) * cap);
    uint32_t i = 0;
    while (i < p->len) {
        const char *s = (const char *)p->data + i;
        size_t sl = strnlen(s, p->len - i);
        if (!sl) break;
        /* 形如 /fragment@3:target:0，取冒号前 */
        const char *colon = memchr(s, ':', sl);
        size_t keep = colon ? (size_t)(colon - s) : sl;
        if (n == cap) { cap *= 2; v = xrealloc(v, sizeof(char *) * cap); }
        v[n++] = xstrndup(s, keep);
        i += sl + 1;
    }
    *out = v;
    return n;
}

/* ------------------------------------------------------ 子树序列化 -- */
/* 标签: 1=节点开始 2=属性 0=节点结束 */

static void subtree_emit(fdt_node *n, buf_t *out)
{
    size_t nl = strlen(n->name);
    buf_u8(out, 1);
    buf_be32(out, (uint32_t)nl);
    buf_append(out, n->name, nl);
    for (fdt_prop *p = n->props; p; p = p->next) {
        size_t pl = strlen(p->name);
        buf_u8(out, 2);
        buf_be32(out, (uint32_t)pl);
        buf_append(out, p->name, pl);
        buf_be32(out, p->len);
        buf_append(out, p->data, p->len);
    }
    for (fdt_node *c = n->children; c; c = c->next) subtree_emit(c, out);
    buf_u8(out, 0);
}

void fdt_subtree_dump(fdt_node *n, buf_t *out)
{
    buf_init(out);
    if (n) subtree_emit(n, out);
}

static const uint8_t *subtree_read(fdt_node *parent, const uint8_t *p,
                                   const uint8_t *end, fdt_node **first)
{
    if (p >= end || *p != 1) return NULL;
    p++;
    if (p + 4 > end) return NULL;
    uint32_t nl = rd_be32(p); p += 4;
    if (p + nl > end) return NULL;
    char *nm = xstrndup((const char *)p, nl); p += nl;
    fdt_node *n = node_new(nm, parent);
    free(nm);
    if (first && !*first) *first = n;

    while (p < end) {
        uint8_t tag = *p;
        if (tag == 0) { p++; return p; }
        if (tag == 2) {
            p++;
            if (p + 4 > end) return NULL;
            uint32_t pl = rd_be32(p); p += 4;
            if (p + pl > end) return NULL;
            char *pn = xstrndup((const char *)p, pl); p += pl;
            if (p + 4 > end) { free(pn); return NULL; }
            uint32_t dl = rd_be32(p); p += 4;
            if (p + dl > end) { free(pn); return NULL; }
            fdt_setprop(n, pn, p, dl);
            free(pn);
            p += dl;
        } else if (tag == 1) {
            p = subtree_read(n, p, end, NULL);
            if (!p) return NULL;
        } else {
            return NULL;
        }
    }
    return NULL;
}

fdt_node *fdt_subtree_load(fdt_node *parent, const uint8_t *p, size_t len)
{
    fdt_node *first = NULL;
    if (!subtree_read(parent, p, p + len, &first)) {
        if (first) fdt_del_node(first);
        return NULL;
    }
    return first;
}

/* ------------------------------------------------------------ 格式化 -- */

int fdt_prop_is_cells(const fdt_prop *p)
{
    return p && p->len && (p->len % 4) == 0;
}

char *fdt_prop_fmt_cells(const fdt_prop *p)
{
    buf_t b; buf_init(&b);
    if (p) {
        uint32_t n = p->len / 4;
        for (uint32_t i = 0; i < n; i++) {
            if (i) buf_u8(&b, ',');
            buf_printf(&b, "%u", rd_be32(p->data + i * 4));
        }
    }
    buf_u8(&b, 0);
    return (char *)b.data;
}

char *fdt_prop_fmt_str(const fdt_prop *p)
{
    if (!p || !p->len) return xstrdup("");
    return xstrndup((const char *)p->data, p->len - (p->data[p->len - 1] ? 0 : 1));
}
