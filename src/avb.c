#include "avb.h"

const char *avb_form_name(avb_form f)
{
    switch (f) {
    case AVB_FORM_FOOTER: return "footer";
    case AVB_FORM_RAW:    return "raw";
    default:              return "none";
    }
}

const char *avb_alg_name(uint32_t a)
{
    switch (a) {
    case AVB_ALG_NONE:           return "NONE";
    case AVB_ALG_SHA256_RSA2048: return "SHA256_RSA2048";
    case AVB_ALG_SHA256_RSA4096: return "SHA256_RSA4096";
    case AVB_ALG_SHA256_RSA8192: return "SHA256_RSA8192";
    default:                     return "UNKNOWN";
    }
}

static size_t alg_sig_len(uint32_t a)
{
    switch (a) {
    case AVB_ALG_SHA256_RSA2048: return 256;
    case AVB_ALG_SHA256_RSA4096: return 512;
    case AVB_ALG_SHA256_RSA8192: return 1024;
    default:                     return 0;
    }
}

static uint64_t round_up(uint64_t v, uint64_t m) { return m ? ((v + m - 1) / m) * m : v; }

/* ---------------------------------------------------------------- 探测 -- */

int avb_probe(const uint8_t *img, size_t len, avb_layout *out)
{
    memset(out, 0, sizeof(*out));
    out->image_size = len;
    if (len < AVB_FOOTER_SIZE) return -1;

    const uint8_t *f = img + len - AVB_FOOTER_SIZE;
    if (memcmp(f, AVB_FOOTER_MAGIC, 4) == 0) {
        out->form            = AVB_FORM_FOOTER;
        out->orig_image_size = rd_be64(f + 12);
        out->vbmeta_offset   = rd_be64(f + 20);
        out->vbmeta_size     = rd_be64(f + 28);
        uint64_t cap = (uint64_t)len - AVB_FOOTER_SIZE;
        if (out->vbmeta_size == 0 ||
            out->vbmeta_offset > cap ||
            out->vbmeta_size > cap - out->vbmeta_offset ||
            out->orig_image_size > out->vbmeta_offset)
            return -1;
        return 0;
    }
    if (memcmp(img, AVB_VBMETA_MAGIC, 4) == 0 && len >= AVB_HEADER_SIZE) {
        uint64_t auth = rd_be64(img + 12);
        uint64_t aux  = rd_be64(img + 20);
        uint64_t room = (uint64_t)len - AVB_HEADER_SIZE;
        if (auth > room || aux > room - auth) return -1;
        uint64_t sz = AVB_HEADER_SIZE + auth + aux;
        out->form          = AVB_FORM_RAW;
        out->vbmeta_offset = 0;
        out->vbmeta_size   = sz;
        return 0;
    }
    out->form = AVB_FORM_NONE;
    return -1;
}

int avb_extract_vbmeta(const uint8_t *img, size_t len,
                       const avb_layout *lay, buf_t *out)
{
    buf_init(out);
    if (lay->form == AVB_FORM_NONE) return -1;
    if (lay->vbmeta_offset + lay->vbmeta_size > len) return -1;
    buf_append(out, img + lay->vbmeta_offset, (size_t)lay->vbmeta_size);
    if (out->len < 4 || memcmp(out->data, AVB_VBMETA_MAGIC, 4) != 0) {
        buf_free(out);
        return -1;
    }
    return 0;
}

/* --------------------------------------------------------- vbmeta 解析 -- */

int avb_parse_vbmeta(const uint8_t *vb, size_t len, avb_params *p)
{
    memset(p, 0, sizeof(*p));
    if (len < AVB_HEADER_SIZE || memcmp(vb, AVB_VBMETA_MAGIC, 4) != 0) return -1;

    uint64_t auth_sz = rd_be64(vb + 12);
    uint64_t aux_sz  = rd_be64(vb + 20);
    /* 长度字段来自镜像，一律用减法比较，避免相加溢出后绕过检查 */
    if (len < AVB_HEADER_SIZE) return -1;
    uint64_t avail = (uint64_t)len - AVB_HEADER_SIZE;
    if (auth_sz > avail || aux_sz > avail - auth_sz) return -1;

    p->algorithm               = rd_be32(vb + 28);
    uint64_t desc_off          = rd_be64(vb + 96);
    uint64_t desc_size         = rd_be64(vb + 104);
    p->rollback_index          = rd_be64(vb + 112);
    p->flags                   = rd_be32(vb + 120);
    p->rollback_index_location = rd_be32(vb + 124);
    memcpy(p->release_string, vb + 128, 48);
    p->release_string[48] = 0;

    const uint8_t *aux = vb + AVB_HEADER_SIZE + auth_sz;
    if (desc_off > aux_sz || desc_size > aux_sz - desc_off) return -1;
    const uint8_t *d = aux + desc_off;
    const uint8_t *dend = d + desc_size;

    buf_t others; buf_init(&others);

    /* 只比较剩余长度，绝不构造可能越过对象末尾的指针 */
    while ((uint64_t)(dend - d) >= 16) {
        uint64_t tag = rd_be64(d);
        uint64_t nbf = rd_be64(d + 8);
        /* 用剩余长度做比较，不做指针加法，nbf 可为任意 64 位值 */
        uint64_t rem = (uint64_t)(dend - d);
        if (nbf > rem - 16) break;
        size_t tot = 16 + (size_t)nbf;

        if (tag == AVB_DESC_HASH && !p->have_hash) {
            if (tot < 132) break;
            uint32_t pn_len = rd_be32(d + 56);
            uint32_t sa_len = rd_be32(d + 60);
            uint32_t dg_len = rd_be32(d + 64);
            if (tot < 132 ||
                (uint64_t)pn_len + sa_len + dg_len > (uint64_t)tot - 132) break;
            if (pn_len >= sizeof(p->partition_name) || sa_len > sizeof(p->salt)) {
                err("hash 描述符字段超长 (name=%u salt=%u)", pn_len, sa_len);
                buf_free(&others);
                return -1;
            }
            p->have_hash     = 1;
            p->hd_image_size = rd_be64(d + 16);
            memcpy(p->hash_algorithm, d + 24, 32);
            p->hash_algorithm[32] = 0;
            p->hd_flags = rd_be32(d + 68);
            memcpy(p->partition_name, d + 132, pn_len);
            p->partition_name[pn_len] = 0;
            memcpy(p->salt, d + 132 + pn_len, sa_len);
            p->salt_len   = sa_len;
            p->digest_len = dg_len;
        } else {
            buf_append(&others, d, tot);
        }
        d += tot;
    }

    p->other_desc     = others.data;
    p->other_desc_len = others.len;
    p->valid          = 1;
    return 0;
}

void avb_params_free(avb_params *p)
{
    free(p->other_desc);
    p->other_desc = NULL;
    p->other_desc_len = 0;
}

/* -------------------------------------------------------------- 免解 -- */

int avb_graft(const uint8_t *data, size_t data_len,
              const uint8_t *stock_vbmeta, size_t vb_len,
              uint64_t stock_orig_size, uint64_t partition_size,
              buf_t *out)
{
    buf_init(out);
    if (partition_size < AVB_FOOTER_SIZE) return -1;
    uint64_t footer_off = partition_size - AVB_FOOTER_SIZE;
    /* 与上面的长度校验保持同一写法：只做减法，不做可能溢出的加法 */
    if ((uint64_t)data_len > footer_off ||
        (uint64_t)vb_len > footer_off - data_len) {
        err("改后数据 %zu + vbmeta %zu 超出可用空间 %llu",
            data_len, vb_len, (unsigned long long)footer_off);
        return -1;
    }

    buf_append(out, data, data_len);
    buf_append(out, stock_vbmeta, vb_len);
    buf_zero(out, (size_t)(footer_off - data_len - vb_len));

    /* footer：magic + 版本 1.0 + 三个 u64 + 28 字节保留 */
    buf_append(out, AVB_FOOTER_MAGIC, 4);
    buf_be32(out, 1);
    buf_be32(out, 0);
    buf_be64(out, stock_orig_size);
    buf_be64(out, (uint64_t)data_len);
    buf_be64(out, (uint64_t)vb_len);
    buf_zero(out, 28);

    if (out->len != partition_size) { buf_free(out); return -1; }
    return 0;
}

/* -------------------------------------------------------------- 自签 -- */

static void put_hash_descriptor(buf_t *b, const avb_params *p,
                                uint64_t image_size, const uint8_t *digest,
                                uint32_t digest_len)
{
    size_t pn = strlen(p->partition_name);
    size_t body = 132 - 16 + pn + p->salt_len + digest_len;
    size_t pad  = (size_t)(round_up(body, 8) - body);

    buf_be64(b, AVB_DESC_HASH);
    buf_be64(b, (uint64_t)(body + pad));
    buf_be64(b, image_size);
    uint8_t ha[32]; memset(ha, 0, 32);
    memcpy(ha, p->hash_algorithm, strnlen(p->hash_algorithm, 32));
    buf_append(b, ha, 32);
    buf_be32(b, (uint32_t)pn);
    buf_be32(b, p->salt_len);
    buf_be32(b, digest_len);
    buf_be32(b, p->hd_flags);
    buf_zero(b, 60);
    buf_append(b, p->partition_name, pn);
    buf_append(b, p->salt, p->salt_len);
    buf_append(b, digest, digest_len);
    buf_zero(b, pad);
}

int avb_sign(const uint8_t *data, size_t data_len,
             const avb_params *p, const rsa_key *key,
             uint64_t partition_size, buf_t *out)
{
    buf_init(out);
    if (!p->valid || !p->have_hash) { err("AVB 参数不完整，无法自签"); return -1; }

    size_t siglen = alg_sig_len(p->algorithm);
    if (!siglen) { err("不支持的签名算法 %u", p->algorithm); return -1; }
    if ((size_t)key->nbits / 8 != siglen) {
        err("密钥位数 %d 与原算法 %s 不符", key->nbits, avb_alg_name(p->algorithm));
        return -1;
    }

    /* 1. 镜像摘要 = SHA256(salt || data) */
    uint8_t digest[32];
    {
        sha256_ctx c; sha256_init(&c);
        if (p->salt_len) sha256_update(&c, p->salt, p->salt_len);
        sha256_update(&c, data, data_len);
        sha256_final(&c, digest);
    }
    uint32_t dlen = p->digest_len ? p->digest_len : 32;
    if (dlen > 32) dlen = 32;

    /* 2. 描述符块：新的 hash 描述符 + 原样保留的其余描述符 */
    buf_t desc; buf_init(&desc);
    put_hash_descriptor(&desc, p, (uint64_t)data_len, digest, dlen);
    if (p->other_desc_len) buf_append(&desc, p->other_desc, p->other_desc_len);

    /* 3. 公钥 */
    buf_t pk;
    if (rsa_avb_pubkey(key, &pk) != 0) { buf_free(&desc); return -1; }

    /* 4. 辅助块 = 描述符 + 公钥，按 64 对齐 */
    buf_t aux; buf_init(&aux);
    buf_append(&aux, desc.data, desc.len);
    uint64_t pk_off = aux.len;
    buf_append(&aux, pk.data, pk.len);
    uint64_t aux_raw = aux.len;
    buf_zero(&aux, (size_t)(round_up(aux_raw, 64) - aux_raw));

    uint64_t auth_raw  = 32 + siglen;
    uint64_t auth_size = round_up(auth_raw, 64);

    /* 5. 头部 */
    buf_t hdr; buf_init(&hdr);
    buf_append(&hdr, AVB_VBMETA_MAGIC, 4);
    buf_be32(&hdr, 1);                    /* required major */
    buf_be32(&hdr, 0);                    /* required minor */
    buf_be64(&hdr, auth_size);
    buf_be64(&hdr, aux.len);
    buf_be32(&hdr, p->algorithm);
    buf_be64(&hdr, 0);                    /* hash_offset */
    buf_be64(&hdr, 32);                   /* hash_size   */
    buf_be64(&hdr, 32);                   /* signature_offset */
    buf_be64(&hdr, siglen);               /* signature_size   */
    buf_be64(&hdr, pk_off);               /* public_key_offset */
    buf_be64(&hdr, pk.len);               /* public_key_size   */
    buf_be64(&hdr, 0);                    /* pk metadata off   */
    buf_be64(&hdr, 0);                    /* pk metadata size  */
    buf_be64(&hdr, 0);                    /* descriptors_offset */
    buf_be64(&hdr, desc.len);             /* descriptors_size   */
    buf_be64(&hdr, p->rollback_index);
    buf_be32(&hdr, p->flags);
    buf_be32(&hdr, p->rollback_index_location);
    uint8_t rel[48]; memset(rel, 0, 48);
    memcpy(rel, p->release_string, strnlen(p->release_string, 47));
    buf_append(&hdr, rel, 48);
    buf_zero(&hdr, AVB_HEADER_SIZE - hdr.len);

    /* 6. 签名：SHA256(头部 || 辅助块) 经 PKCS#1 v1.5 填充后做私钥运算 */
    uint8_t vbhash[32];
    {
        sha256_ctx c; sha256_init(&c);
        sha256_update(&c, hdr.data, hdr.len);
        sha256_update(&c, aux.data, aux.len);
        sha256_final(&c, vbhash);
    }
    uint8_t *padded = xmalloc(siglen);
    uint8_t *sig    = xmalloc(siglen);
    rsa_pkcs1_sha256_pad(vbhash, siglen, padded);
    int rc = rsa_raw_sign(key, padded, sig);
    free(padded);
    if (rc != 0) {
        free(sig); buf_free(&hdr); buf_free(&aux); buf_free(&desc); buf_free(&pk);
        err("RSA 签名失败");
        return -1;
    }

    /* 7. 组装 vbmeta */
    buf_t vb; buf_init(&vb);
    buf_append(&vb, hdr.data, hdr.len);
    buf_append(&vb, vbhash, 32);
    buf_append(&vb, sig, siglen);
    buf_zero(&vb, (size_t)(auth_size - auth_raw));
    buf_append(&vb, aux.data, aux.len);
    free(sig);

    /* 8. 布局：数据 -> 4096 对齐 -> vbmeta -> 零 -> footer */
    uint64_t vb_off = round_up(data_len, 4096);
    uint64_t footer_off = partition_size - AVB_FOOTER_SIZE;
    if (vb_off + vb.len > footer_off) {
        err("签名后超出分区：数据 %zu + vbmeta %zu > %llu",
            data_len, vb.len, (unsigned long long)footer_off);
        buf_free(&vb); buf_free(&hdr); buf_free(&aux); buf_free(&desc); buf_free(&pk);
        return -1;
    }

    buf_append(out, data, data_len);
    buf_zero(out, (size_t)(vb_off - data_len));
    buf_append(out, vb.data, vb.len);
    buf_zero(out, (size_t)(footer_off - vb_off - vb.len));
    buf_append(out, AVB_FOOTER_MAGIC, 4);
    buf_be32(out, 1);
    buf_be32(out, 0);
    buf_be64(out, (uint64_t)data_len);
    buf_be64(out, vb_off);
    buf_be64(out, vb.len);
    buf_zero(out, 28);

    buf_free(&vb); buf_free(&hdr); buf_free(&aux); buf_free(&desc); buf_free(&pk);
    if (out->len != partition_size) { buf_free(out); return -1; }
    return 0;
}

/* ------------------------------------------------------------ 持久化 -- */

static void hex_out(buf_t *b, const uint8_t *p, size_t n)
{
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { buf_u8(b, H[p[i] >> 4]); buf_u8(b, H[p[i] & 15]); }
}

static int hex_in(const char *s, uint8_t *out, size_t max)
{
    size_t n = strlen(s) / 2;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) {
        int hi = -1, lo = -1;
        char a = s[i*2], b = s[i*2+1];
        hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)n;
}

int avb_params_save(const avb_params *p, const char *dir)
{
    if (mkdir_p(dir) != 0) return -1;
    buf_t b; buf_init(&b);
    buf_printf(&b, "algorithm=%u\n", p->algorithm);
    buf_printf(&b, "rollback_index=%llu\n", (unsigned long long)p->rollback_index);
    buf_printf(&b, "flags=%u\n", p->flags);
    buf_printf(&b, "rollback_index_location=%u\n", p->rollback_index_location);
    buf_printf(&b, "release_string=%s\n", p->release_string);
    buf_printf(&b, "partition_name=%s\n", p->partition_name);
    buf_printf(&b, "hash_algorithm=%s\n", p->hash_algorithm);
    buf_printf(&b, "image_size=%llu\n", (unsigned long long)p->hd_image_size);
    buf_printf(&b, "digest_len=%u\n", p->digest_len);
    buf_printf(&b, "hd_flags=%u\n", p->hd_flags);
    buf_printf(&b, "salt=");
    hex_out(&b, p->salt, p->salt_len);
    buf_printf(&b, "\n");

    char *cf = path_join(dir, "avb.conf");
    int rc = file_write(cf, b.data, b.len);
    free(cf);
    buf_free(&b);
    if (rc != 0) return -1;

    char *od = path_join(dir, "other_desc.bin");
    rc = file_write(od, p->other_desc ? p->other_desc : (const uint8_t *)"",
                    p->other_desc_len);
    free(od);
    return rc;
}

int avb_params_load(avb_params *p, const char *dir)
{
    memset(p, 0, sizeof(*p));
    char *cf = path_join(dir, "avb.conf");
    buf_t b;
    int rc = file_read(cf, &b);
    free(cf);
    if (rc != 0) return -1;
    buf_u8(&b, 0);

    char *s = (char *)b.data;
    while (s && *s) {
        char *nl = strchr(s, '\n');
        if (nl) *nl = 0;
        char *eq = strchr(s, '=');
        if (eq) {
            *eq = 0;
            char *k = s, *v = eq + 1;
            if (str_eq(k, "algorithm"))                p->algorithm = (uint32_t)strtoul(v, NULL, 10);
            else if (str_eq(k, "rollback_index"))      p->rollback_index = strtoull(v, NULL, 10);
            else if (str_eq(k, "flags"))               p->flags = (uint32_t)strtoul(v, NULL, 10);
            else if (str_eq(k, "rollback_index_location")) p->rollback_index_location = (uint32_t)strtoul(v, NULL, 10);
            else if (str_eq(k, "release_string"))      snprintf(p->release_string, sizeof(p->release_string), "%s", v);
            else if (str_eq(k, "partition_name"))      snprintf(p->partition_name, sizeof(p->partition_name), "%s", v);
            else if (str_eq(k, "hash_algorithm"))      snprintf(p->hash_algorithm, sizeof(p->hash_algorithm), "%s", v);
            else if (str_eq(k, "image_size"))          p->hd_image_size = strtoull(v, NULL, 10);
            else if (str_eq(k, "digest_len"))          p->digest_len = (uint32_t)strtoul(v, NULL, 10);
            else if (str_eq(k, "hd_flags"))            p->hd_flags = (uint32_t)strtoul(v, NULL, 10);
            else if (str_eq(k, "salt")) {
                int n = hex_in(v, p->salt, sizeof(p->salt));
                p->salt_len = n > 0 ? (uint32_t)n : 0;
            }
        }
        s = nl ? nl + 1 : NULL;
    }
    buf_free(&b);

    char *od = path_join(dir, "other_desc.bin");
    buf_t o;
    if (file_read(od, &o) == 0) { p->other_desc = o.data; p->other_desc_len = o.len; }
    free(od);

    p->have_hash = p->partition_name[0] ? 1 : 0;
    p->valid = 1;
    return 0;
}
