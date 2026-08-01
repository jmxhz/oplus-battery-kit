#include "dtbo.h"

dtbo_t *dtbo_parse(const uint8_t *img, size_t len)
{
    if (len < 32 || rd_be32(img) != DTBO_MAGIC) return NULL;

    uint32_t total   = rd_be32(img + 4);
    uint32_t hsz     = rd_be32(img + 8);
    uint32_t esz     = rd_be32(img + 12);
    uint32_t cnt     = rd_be32(img + 16);
    uint32_t eoff    = rd_be32(img + 20);
    uint32_t psz     = rd_be32(img + 24);
    uint32_t ver     = rd_be32(img + 28);

    if (esz < 32 || cnt == 0 || cnt > 4096) return NULL;
    if ((size_t)eoff + (size_t)cnt * esz > len) return NULL;

    /* 分区镜像尾部可能挂着 AVB vbmeta 与 footer，容器本身到 total_size 为止。
       个别镜像的 total_size 不含末尾条目，故取二者最大值。 */
    size_t clen = total;
    /* 条目表本身也属于容器，必须纳入边界，否则 dtbo_pack 写表时会越界 */
    uint64_t tbl_end = (uint64_t)eoff + (uint64_t)cnt * esz;
    if (tbl_end > clen) clen = (size_t)tbl_end;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = img + eoff + (size_t)i * esz;
        size_t end = (size_t)rd_be32(e + 4) + rd_be32(e);
        if (end > clen) clen = end;
    }
    if (clen > len || clen == 0) clen = len;

    dtbo_t *d = xcalloc(1, sizeof(dtbo_t));
    d->page_size      = psz;
    d->version        = ver;
    d->header_size    = hsz;
    d->entry_size     = esz;
    d->entries_offset = eoff;
    d->orig_total     = total;
    d->container_len  = clen;
    d->orig_len       = clen;
    d->orig           = xmalloc(clen);
    memcpy(d->orig, img, clen);
    d->n              = (int)cnt;
    d->ent            = xcalloc(cnt, sizeof(dtbo_entry));

    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *e = img + eoff + (size_t)i * esz;
        uint32_t dsz = rd_be32(e);
        uint32_t doff = rd_be32(e + 4);
        if ((size_t)doff + dsz > len) { dtbo_free(d); return NULL; }
        dtbo_entry *t = &d->ent[i];
        t->orig_size = dsz;
        t->orig_off  = doff;
        t->id        = rd_be32(e + 8);
        t->rev       = rd_be32(e + 12);
        for (int k = 0; k < 4; k++) t->custom[k] = rd_be32(e + 16 + k * 4);
        t->len  = dsz;
        t->blob = xmalloc(dsz ? dsz : 1);
        memcpy(t->blob, img + doff, dsz);
    }
    return d;
}

void dtbo_free(dtbo_t *d)
{
    if (!d) return;
    for (int i = 0; i < d->n; i++) {
        free(d->ent[i].blob);
        fdt_free(d->ent[i].fdt);
    }
    free(d->ent);
    free(d->orig);
    free(d);
}

fdt_t *dtbo_fdt(dtbo_t *d, int i)
{
    if (i < 0 || i >= d->n) return NULL;
    dtbo_entry *e = &d->ent[i];
    if (!e->fdt) e->fdt = fdt_parse(e->blob, e->len);
    return e->fdt;
}

void dtbo_touch(dtbo_t *d, int i)
{
    if (i >= 0 && i < d->n) d->ent[i].modified = 1;
}

/* 计算每个条目在原布局中的可用空间：到下一个条目起点或镜像末尾 */
static uint32_t slot_capacity(dtbo_t *d, int i)
{
    uint32_t start = d->ent[i].orig_off;
    uint32_t next  = (uint32_t)d->container_len;
    for (int k = 0; k < d->n; k++) {
        uint32_t o = d->ent[k].orig_off;
        if (o > start && o < next) next = o;
    }
    /* 条目表本身也不能被覆盖 */
    uint32_t tbl_end = d->entries_offset + (uint32_t)d->n * d->entry_size;
    if (start >= tbl_end && d->entries_offset > start && d->entries_offset < next)
        next = d->entries_offset;
    return next > start ? next - start : 0;
}

int dtbo_pack(dtbo_t *d, buf_t *out)
{
    /* 先生成每个条目的最终字节 */
    uint8_t **blobs = xcalloc(d->n, sizeof(uint8_t *));
    size_t   *lens  = xcalloc(d->n, sizeof(size_t));
    buf_t    *tmps  = xcalloc(d->n, sizeof(buf_t));
    int rc = -1;

    for (int i = 0; i < d->n; i++) {
        dtbo_entry *e = &d->ent[i];
        if (e->modified && e->fdt) {
            if (fdt_serialize(e->fdt, &tmps[i]) != 0) goto done;
            blobs[i] = tmps[i].data;
            lens[i]  = tmps[i].len;
        } else {
            blobs[i] = e->blob;
            lens[i]  = e->len;
        }
    }

    /* 路径 A：全部能放回原槽位，则复制原镜像并原位写回，保证无改动时逐字节一致 */
    int fits = 1;
    for (int i = 0; i < d->n; i++)
        if (lens[i] > slot_capacity(d, i)) { fits = 0; break; }

    buf_init(out);
    if (fits) {
        buf_append(out, d->orig, d->orig_len);
        for (int i = 0; i < d->n; i++) {
            dtbo_entry *e = &d->ent[i];
            uint32_t cap = slot_capacity(d, i);
            memcpy(out->data + e->orig_off, blobs[i], lens[i]);
            if (lens[i] < cap)
                memset(out->data + e->orig_off + lens[i], 0, cap - lens[i]);
            uint8_t *ep = out->data + d->entries_offset + (size_t)i * d->entry_size;
            wr_be32(ep, (uint32_t)lens[i]);
            wr_be32(ep + 4, e->orig_off);
            wr_be32(ep + 8, e->id);
            wr_be32(ep + 12, e->rev);
            for (int k = 0; k < 4; k++) wr_be32(ep + 16 + k * 4, e->custom[k]);
        }
        rc = 0;
        goto done;
    }

    /* 路径 B：重建布局。header + 条目表 + 各 dtb（4 字节对齐） */
    {
        uint32_t hsz  = d->header_size ? d->header_size : 32;
        uint32_t esz  = d->entry_size;
        uint32_t eoff = d->entries_offset ? d->entries_offset : hsz;
        uint32_t data_start = eoff + (uint32_t)d->n * esz;
        data_start = (data_start + 3) & ~3u;

        uint32_t *offs = xcalloc(d->n, sizeof(uint32_t));
        uint32_t cur = data_start;
        for (int i = 0; i < d->n; i++) {
            offs[i] = cur;
            cur += (uint32_t)((lens[i] + 3) & ~(size_t)3);
        }
        uint32_t total = cur;

        buf_zero(out, total);
        uint8_t *o = out->data;
        wr_be32(o, DTBO_MAGIC);
        wr_be32(o + 4, total);
        wr_be32(o + 8, hsz);
        wr_be32(o + 12, esz);
        wr_be32(o + 16, (uint32_t)d->n);
        wr_be32(o + 20, eoff);
        wr_be32(o + 24, d->page_size);
        wr_be32(o + 28, d->version);
        for (int i = 0; i < d->n; i++) {
            uint8_t *ep = o + eoff + (size_t)i * esz;
            wr_be32(ep, (uint32_t)lens[i]);
            wr_be32(ep + 4, offs[i]);
            wr_be32(ep + 8, d->ent[i].id);
            wr_be32(ep + 12, d->ent[i].rev);
            for (int k = 0; k < 4; k++) wr_be32(ep + 16 + k * 4, d->ent[i].custom[k]);
            memcpy(o + offs[i], blobs[i], lens[i]);
        }
        free(offs);
        rc = 0;
    }

done:
    for (int i = 0; i < d->n; i++) buf_free(&tmps[i]);
    free(tmps); free(blobs); free(lens);
    return rc;
}

int dtbo_selftest(const uint8_t *img, size_t len, char **detail)
{
    buf_t msg; buf_init(&msg);
    if (detail) *detail = NULL;

    dtbo_t *d = dtbo_parse(img, len);
    if (!d) {
        buf_printf(&msg, "无法解析 DTBO 容器"); buf_u8(&msg, 0);
        if (detail) *detail = (char *)msg.data; else buf_free(&msg);
        return -1;
    }

    buf_t rp;
    if (dtbo_pack(d, &rp) != 0) {
        dtbo_free(d);
        buf_printf(&msg, "重新打包失败"); buf_u8(&msg, 0);
        if (detail) *detail = (char *)msg.data; else buf_free(&msg);
        return -1;
    }

    size_t clen = d->container_len;
    int byte_same = (rp.len == clen && clen <= len &&
                     memcmp(rp.data, img, clen) == 0);

    /* 条目级比对：重新解析并逐条对齐元数据与内容 */
    dtbo_t *d2 = dtbo_parse(rp.data, rp.len);
    int entry_same = 1;
    if (!d2 || d2->n != d->n) {
        entry_same = 0;
        buf_printf(&msg, "重打包后条目数不符; ");
    } else {
        for (int i = 0; i < d->n; i++) {
            dtbo_entry *a = &d->ent[i], *b = &d2->ent[i];
            if (a->id != b->id || a->rev != b->rev ||
                memcmp(a->custom, b->custom, sizeof(a->custom)) != 0) {
                entry_same = 0;
                buf_printf(&msg, "条目 %d 元数据丢失; ", i);
            }
            if (a->len != b->len || memcmp(a->blob, b->blob, a->len) != 0) {
                entry_same = 0;
                buf_printf(&msg, "条目 %d 内容不一致; ", i);
            }
        }
    }
    /* 每个条目必须是可解析的 FDT */
    for (int i = 0; i < d->n && entry_same; i++) {
        fdt_t *f = fdt_parse(d->ent[i].blob, d->ent[i].len);
        if (!f) { entry_same = 0; buf_printf(&msg, "条目 %d 不是有效 FDT; ", i); }
        fdt_free(f);
    }

    if (byte_same) buf_printf(&msg, "逐字节一致 (%d 条目, 容器 %zu 字节)", d->n, clen);
    else if (entry_same) buf_printf(&msg, "条目级一致但布局有差异 (%d 条目)", d->n);
    buf_u8(&msg, 0);
    if (detail) *detail = (char *)msg.data; else buf_free(&msg);

    dtbo_free(d2);
    dtbo_free(d);
    buf_free(&rp);
    return byte_same ? 0 : (entry_same ? 1 : -1);
}
