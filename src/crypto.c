#include "crypto.h"

/* ================================================================ SHA-256 */

static const uint32_t K256[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

#define ROR(x,n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(sha256_ctx *c, const uint8_t *p)
{
    uint32_t w[64], a,b,cc,d,e,f,g,h;
    for (int i = 0; i < 16; i++) w[i] = rd_be32(p + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i-15],7) ^ ROR(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = ROR(w[i-2],17) ^ ROR(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6];  h=c->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e,6) ^ ROR(e,11) ^ ROR(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a,2) ^ ROR(a,13) ^ ROR(a,22);
        uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g;  c->h[7]+=h;
}

void sha256_init(sha256_ctx *c)
{
    c->h[0]=0x6a09e667; c->h[1]=0xbb67ae85; c->h[2]=0x3c6ef372; c->h[3]=0xa54ff53a;
    c->h[4]=0x510e527f; c->h[5]=0x9b05688c; c->h[6]=0x1f83d9ab; c->h[7]=0x5be0cd19;
    c->total = 0; c->buflen = 0;
}

void sha256_update(sha256_ctx *c, const void *p, size_t n)
{
    const uint8_t *q = p;
    c->total += n;
    if (c->buflen) {
        size_t need = 64 - c->buflen;
        size_t take = n < need ? n : need;
        memcpy(c->buf + c->buflen, q, take);
        c->buflen += take; q += take; n -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
    while (n >= 64) { sha256_block(c, q); q += 64; n -= 64; }
    if (n) { memcpy(c->buf, q, n); c->buflen = n; }
}

void sha256_final(sha256_ctx *c, uint8_t out[32])
{
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) sha256_update(c, &z, 1);
    uint8_t lb[8]; wr_be64(lb, bits);
    /* 直接落入缓冲，避免 total 影响 */
    memcpy(c->buf + 56, lb, 8);
    sha256_block(c, c->buf);
    c->buflen = 0;
    for (int i = 0; i < 8; i++) wr_be32(out + i * 4, c->h[i]);
}

void sha256(const void *p, size_t n, uint8_t out[32])
{
    sha256_ctx c; sha256_init(&c); sha256_update(&c, p, n); sha256_final(&c, out);
}

void sha256_hex(const uint8_t d[32], char out[65])
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[i*2] = H[d[i] >> 4]; out[i*2+1] = H[d[i] & 15]; }
    out[64] = 0;
}

/* ==================================================================== MD5 */

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

void md5(const void *pv, size_t n, uint8_t out[16])
{
    static const uint32_t T[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const int S[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};

    size_t total = ((n + 8) / 64 + 1) * 64;
    uint8_t *m = xcalloc(total, 1);
    memcpy(m, pv, n);
    m[n] = 0x80;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) m[total - 8 + i] = (uint8_t)(bits >> (8 * i));

    uint32_t h0=0x67452301,h1=0xefcdab89,h2=0x98badcfe,h3=0x10325476;
    for (size_t off = 0; off < total; off += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            const uint8_t *b = m + off + i * 4;
            M[i] = (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
        }
        uint32_t A=h0,B=h1,C=h2,D=h3;
        for (int i = 0; i < 64; i++) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);        g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;                 g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);              g = (7*i) % 16; }
            F = F + A + T[i] + M[g];
            A = D; D = C; C = B; B = B + rol32(F, S[i]);
        }
        h0+=A; h1+=B; h2+=C; h3+=D;
    }
    free(m);
    uint32_t hs[4] = {h0,h1,h2,h3};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i*4+j] = (uint8_t)(hs[i] >> (8*j));
}

void md5_hex(const uint8_t d[16], char out[33])
{
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i*2] = H[d[i] >> 4]; out[i*2+1] = H[d[i] & 15]; }
    out[32] = 0;
}

/* ================================================================= 大数 */

void bn_zero(bn_t *a) { memset(a->v, 0, sizeof(a->v)); a->n = 0; }

static void bn_norm(bn_t *a)
{
    int i = BN_MAX_LIMBS;
    while (i > 0 && a->v[i-1] == 0) i--;
    a->n = i;
}

void bn_from_be(bn_t *a, const uint8_t *p, size_t len)
{
    bn_zero(a);
    for (size_t i = 0; i < len; i++) {
        size_t bi = len - 1 - i;          /* 从最低字节起 */
        size_t limb = i / 4, sh = (i % 4) * 8;
        if (limb >= BN_MAX_LIMBS) break;
        a->v[limb] |= (uint32_t)p[bi] << sh;
    }
    bn_norm(a);
}

void bn_to_be(const bn_t *a, uint8_t *p, size_t len)
{
    memset(p, 0, len);
    for (size_t i = 0; i < len; i++) {
        size_t limb = i / 4, sh = (i % 4) * 8;
        uint8_t byte = (limb < BN_MAX_LIMBS) ? (uint8_t)(a->v[limb] >> sh) : 0;
        p[len - 1 - i] = byte;
    }
}

int bn_is_zero(const bn_t *a) { return a->n == 0; }

int bn_cmp(const bn_t *a, const bn_t *b)
{
    for (int i = BN_MAX_LIMBS - 1; i >= 0; i--) {
        if (a->v[i] != b->v[i]) return a->v[i] > b->v[i] ? 1 : -1;
    }
    return 0;
}

/* a += b（宽度 limbs），返回进位 */
static uint32_t bn_add_n(uint32_t *a, const uint32_t *b, int limbs)
{
    uint64_t c = 0;
    for (int i = 0; i < limbs; i++) {
        c += (uint64_t)a[i] + b[i];
        a[i] = (uint32_t)c;
        c >>= 32;
    }
    return (uint32_t)c;
}

/* a -= b，返回借位 */
static uint32_t bn_sub_n(uint32_t *a, const uint32_t *b, int limbs)
{
    int64_t brw = 0;
    for (int i = 0; i < limbs; i++) {
        int64_t t = (int64_t)a[i] - b[i] - brw;
        if (t < 0) { t += ((int64_t)1 << 32); brw = 1; } else brw = 0;
        a[i] = (uint32_t)t;
    }
    return (uint32_t)brw;
}

static int bn_cmp_n(const uint32_t *a, const uint32_t *b, int limbs)
{
    for (int i = limbs - 1; i >= 0; i--)
        if (a[i] != b[i]) return a[i] > b[i] ? 1 : -1;
    return 0;
}

/* ------------------------------------------------------------ 蒙哥马利 */

typedef struct {
    int      limbs;
    uint32_t n[BN_MAX_LIMBS];
    uint32_t rr[BN_MAX_LIMBS];
    uint32_t n0inv;      /* -n^-1 mod 2^32 */
} mont_ctx;

static uint32_t inv32(uint32_t a)          /* a^-1 mod 2^32，a 必须为奇 */
{
    uint32_t x = 1;
    for (int i = 0; i < 5; i++) x *= 2 - a * x;
    return x;
}

/* r = a*b*R^-1 mod n，CIOS */
static void mont_mul(uint32_t *r, const uint32_t *a, const uint32_t *b,
                     const mont_ctx *c)
{
    int L = c->limbs;
    uint32_t t[BN_MAX_LIMBS + 2];
    memset(t, 0, sizeof(uint32_t) * (L + 2));

    for (int i = 0; i < L; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < L; j++) {
            uint64_t s = (uint64_t)a[j] * b[i] + t[j] + carry;
            t[j] = (uint32_t)s;
            carry = s >> 32;
        }
        uint64_t s = (uint64_t)t[L] + carry;
        t[L] = (uint32_t)s;
        t[L+1] = (uint32_t)(s >> 32);

        uint32_t m = t[0] * c->n0inv;
        carry = 0;
        {
            uint64_t s2 = (uint64_t)m * c->n[0] + t[0];
            carry = s2 >> 32;
        }
        for (int j = 1; j < L; j++) {
            uint64_t s2 = (uint64_t)m * c->n[j] + t[j] + carry;
            t[j-1] = (uint32_t)s2;
            carry = s2 >> 32;
        }
        uint64_t s3 = (uint64_t)t[L] + carry;
        t[L-1] = (uint32_t)s3;
        t[L] = t[L+1] + (uint32_t)(s3 >> 32);
        t[L+1] = 0;
    }
    if (t[L] || bn_cmp_n(t, c->n, L) >= 0) {
        bn_sub_n(t, c->n, L);
    }
    memcpy(r, t, sizeof(uint32_t) * L);
}

static int mont_init(mont_ctx *c, const uint32_t *n, int limbs)
{
    if (limbs <= 0 || limbs > BN_MAX_LIMBS) return -1;
    c->limbs = limbs;
    memset(c->n, 0, sizeof(c->n));
    memcpy(c->n, n, sizeof(uint32_t) * limbs);
    c->n0inv = (uint32_t)(0 - inv32(n[0]));

    /* rr = 2^(2*32*limbs) mod n，用逐位左移-约减求得，避免长除法 */
    uint32_t t[BN_MAX_LIMBS];
    memset(t, 0, sizeof(t));
    t[0] = 1;
    int total_bits = 2 * 32 * limbs;
    for (int i = 0; i < total_bits; i++) {
        uint32_t carry = 0;
        for (int j = 0; j < limbs; j++) {
            uint32_t nv = (t[j] << 1) | carry;
            carry = t[j] >> 31;
            t[j] = nv;
        }
        if (carry || bn_cmp_n(t, c->n, limbs) >= 0) bn_sub_n(t, c->n, limbs);
    }
    memset(c->rr, 0, sizeof(c->rr));
    memcpy(c->rr, t, sizeof(uint32_t) * limbs);
    return 0;
}

/* r = base^exp mod n */
static void mont_pow(uint32_t *r, const uint32_t *base, const uint32_t *exp,
                     int exp_limbs, const mont_ctx *c)
{
    int L = c->limbs;
    uint32_t x[BN_MAX_LIMBS], acc[BN_MAX_LIMBS], one[BN_MAX_LIMBS];
    memset(one, 0, sizeof(one)); one[0] = 1;

    mont_mul(x, base, c->rr, c);            /* x = base * R */
    mont_mul(acc, one, c->rr, c);           /* acc = R      */

    int top = exp_limbs * 32 - 1;
    while (top > 0 && !((exp[top / 32] >> (top % 32)) & 1)) top--;

    for (int i = top; i >= 0; i--) {
        mont_mul(acc, acc, acc, c);
        if ((exp[i / 32] >> (i % 32)) & 1) mont_mul(acc, acc, x, c);
    }
    memset(one, 0, sizeof(one)); one[0] = 1;
    mont_mul(r, acc, one, c);               /* 退出蒙域 */
    (void)L;
}

/* =================================================================== RSA */

static int rd_field(const uint8_t **p, const uint8_t *end, bn_t *out, int *bytes)
{
    if (*p + 4 > end) return -1;
    uint32_t len = rd_be32(*p); *p += 4;
    if (len > 1024 || *p + len > end) return -1;
    bn_from_be(out, *p, len);
    if (bytes) *bytes = (int)len;
    *p += len;
    return 0;
}

int rsa_key_load(const uint8_t *blob, size_t len, rsa_key *k)
{
    memset(k, 0, sizeof(*k));
    if (len < 16 || memcmp(blob, "OBKKEY1", 8) != 0) return -1;
    const uint8_t *p = blob + 8, *end = blob + len;
    if (p + 4 > end) return -1;
    k->nbits = (int)rd_be32(p); p += 4;
    if (k->nbits != 2048 && k->nbits != 4096 && k->nbits != 8192) return -1;
    if ((k->nbits + 31) / 32 > BN_MAX_LIMBS) return -1;   /* 与大数容量对齐 */
    if (rd_field(&p, end, &k->n, NULL) != 0) return -1;
    if (rd_field(&p, end, &k->d, NULL) != 0) return -1;
    if (rd_field(&p, end, &k->p, NULL) == 0 &&
        rd_field(&p, end, &k->q, NULL) == 0 &&
        rd_field(&p, end, &k->dp, NULL) == 0 &&
        rd_field(&p, end, &k->dq, NULL) == 0 &&
        rd_field(&p, end, &k->qinv, NULL) == 0)
        k->have_crt = 1;
    return 0;
}

int rsa_raw_sign(const rsa_key *k, const uint8_t *in, uint8_t *out)
{
    int klen = k->nbits / 8;
    int nlimbs = (k->nbits + 31) / 32;

    bn_t m; bn_from_be(&m, in, (size_t)klen);
    if (bn_cmp(&m, &k->n) >= 0) return -1;

    uint32_t res[BN_MAX_LIMBS];
    memset(res, 0, sizeof(res));

    if (k->have_crt) {
        int hl = nlimbs / 2;
        mont_ctx cp, cq;
        if (mont_init(&cp, k->p.v, hl) != 0 ||
            mont_init(&cq, k->q.v, hl) != 0) return -1;

        uint32_t mp[BN_MAX_LIMBS], mq[BN_MAX_LIMBS];
        uint32_t m1[BN_MAX_LIMBS], m2[BN_MAX_LIMBS];
        /* 必须整体清零：mont_pow 只写前 hl 个 limb，
           后续 bn_add_n 会按 nlimbs 宽度读取 */
        memset(mp, 0, sizeof(mp)); memset(mq, 0, sizeof(mq));
        memset(m1, 0, sizeof(m1)); memset(m2, 0, sizeof(m2));
        /* m mod p / m mod q：逐位构造，规避长除法 */
        for (int which = 0; which < 2; which++) {
            const mont_ctx *c = which ? &cq : &cp;
            uint32_t *dst = which ? mq : mp;
            uint32_t t[BN_MAX_LIMBS]; memset(t, 0, sizeof(t));
            for (int b = k->nbits - 1; b >= 0; b--) {
                uint32_t carry = 0;
                for (int j = 0; j < hl; j++) {
                    uint32_t nv = (t[j] << 1) | carry;
                    carry = t[j] >> 31;
                    t[j] = nv;
                }
                t[0] |= (m.v[b / 32] >> (b % 32)) & 1;
                if (carry || bn_cmp_n(t, c->n, hl) >= 0) bn_sub_n(t, c->n, hl);
            }
            memcpy(dst, t, sizeof(uint32_t) * hl);
        }

        mont_pow(m1, mp, k->dp.v, hl, &cp);
        mont_pow(m2, mq, k->dq.v, hl, &cq);

        /* h = qinv * (m1 - m2 mod p) mod p */
        uint32_t diff[BN_MAX_LIMBS];
        memset(diff, 0, sizeof(diff));
        memcpy(diff, m1, sizeof(uint32_t) * hl);
        if (bn_cmp_n(m1, m2, hl) < 0) bn_add_n(diff, cp.n, hl);
        bn_sub_n(diff, m2, hl);

        uint32_t h[BN_MAX_LIMBS], tmp[BN_MAX_LIMBS];
        memset(h, 0, sizeof(h)); memset(tmp, 0, sizeof(tmp));
        mont_mul(tmp, diff, cp.rr, &cp);            /* 进蒙域 */
        mont_mul(h, tmp, k->qinv.v, &cp);           /* qinv 以普通域给出，乘后即为普通域 */

        /* res = m2 + h*q */
        memset(res, 0, sizeof(res));
        for (int i = 0; i < hl; i++) {
            uint64_t carry = 0;
            for (int j = 0; j < hl; j++) {
                uint64_t s = (uint64_t)h[i] * k->q.v[j] + res[i + j] + carry;
                res[i + j] = (uint32_t)s;
                carry = s >> 32;
            }
            int idx = i + hl;
            while (carry && idx < BN_MAX_LIMBS) {
                uint64_t s = (uint64_t)res[idx] + carry;
                res[idx] = (uint32_t)s;
                carry = s >> 32;
                idx++;
            }
        }
        bn_add_n(res, m2, nlimbs);
    } else {
        mont_ctx c;
        if (mont_init(&c, k->n.v, nlimbs) != 0) return -1;
        mont_pow(res, m.v, k->d.v, nlimbs, &c);
    }

    bn_t r; bn_zero(&r);
    memcpy(r.v, res, sizeof(uint32_t) * nlimbs);
    bn_to_be(&r, out, (size_t)klen);
    return 0;
}

int rsa_avb_pubkey(const rsa_key *k, buf_t *out)
{
    int nlimbs = (k->nbits + 31) / 32;
    int klen   = k->nbits / 8;
    mont_ctx c;
    if (mont_init(&c, k->n.v, nlimbs) != 0) return -1;

    buf_init(out);
    buf_be32(out, (uint32_t)k->nbits);
    buf_be32(out, c.n0inv);
    uint8_t *tmp = xmalloc((size_t)klen);
    bn_to_be(&k->n, tmp, (size_t)klen);
    buf_append(out, tmp, (size_t)klen);
    bn_t rr; bn_zero(&rr);
    memcpy(rr.v, c.rr, sizeof(uint32_t) * nlimbs);
    bn_to_be(&rr, tmp, (size_t)klen);
    buf_append(out, tmp, (size_t)klen);
    free(tmp);
    return 0;
}

void rsa_pkcs1_sha256_pad(const uint8_t hash[32], size_t keylen, uint8_t *out)
{
    static const uint8_t DI[] = {
        0x30,0x31,0x30,0x0d,0x06,0x09,0x60,0x86,0x48,0x01,
        0x65,0x03,0x04,0x02,0x01,0x05,0x00,0x04,0x20 };
    size_t dlen = sizeof(DI) + 32;
    size_t nff  = keylen - dlen - 3;
    out[0] = 0x00;
    out[1] = 0x01;
    memset(out + 2, 0xff, nff);
    out[2 + nff] = 0x00;
    memcpy(out + 3 + nff, DI, sizeof(DI));
    memcpy(out + 3 + nff + sizeof(DI), hash, 32);
}

/* ================================================================ base64 */

static const char B64C[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void b64_encode(const uint8_t *p, size_t n, buf_t *out)
{
    size_t i = 0;
    while (i + 3 <= n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i+1] << 8) | p[i+2];
        buf_u8(out, B64C[(v >> 18) & 63]); buf_u8(out, B64C[(v >> 12) & 63]);
        buf_u8(out, B64C[(v >> 6) & 63]);  buf_u8(out, B64C[v & 63]);
        i += 3;
    }
    if (i < n) {
        uint32_t v = (uint32_t)p[i] << 16;
        int rem = (int)(n - i);
        if (rem == 2) v |= (uint32_t)p[i+1] << 8;
        buf_u8(out, B64C[(v >> 18) & 63]);
        buf_u8(out, B64C[(v >> 12) & 63]);
        buf_u8(out, rem == 2 ? B64C[(v >> 6) & 63] : '=');
        buf_u8(out, '=');
    }
}

int b64_decode(const char *s, buf_t *out)
{
    static int8_t T[256];
    static int init = 0;
    if (!init) {
        memset(T, -1, sizeof(T));
        for (int i = 0; i < 64; i++) T[(unsigned char)B64C[i]] = (int8_t)i;
        init = 1;
    }
    buf_init(out);
    uint32_t acc = 0; int bits = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '=' || *p == '\n' || *p == '\r' || *p == ' ') continue;
        int8_t v = T[*p];
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; buf_u8(out, (uint8_t)(acc >> bits)); }
    }
    return 0;
}
