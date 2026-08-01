/* SHA-256 与 RSA-PKCS1v15 签名（无外部依赖） */
#ifndef OBK_CRYPTO_H
#define OBK_CRYPTO_H

#include "common.h"

/* ---- SHA-256 ---- */
typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *p, size_t n);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *p, size_t n, uint8_t out[32]);
void sha256_hex(const uint8_t d[32], char out[65]);

/* ---- MD5（仅用于完整性自检，非安全用途） ---- */
void md5(const void *p, size_t n, uint8_t out[16]);
void md5_hex(const uint8_t d[16], char out[33]);

/* ---- 大数 ---- */
/* 必须覆盖所受理的最大密钥位数：8192 位需要 256 个 limb。
   低于此值时 mont_init 的 memcpy 与 mont_mul 的栈数组都会越界。 */
#ifndef BN_MAX_KEY_BITS
#define BN_MAX_KEY_BITS 8192
#endif
#define BN_MAX_LIMBS    (BN_MAX_KEY_BITS / 32 + 8)
typedef struct { uint32_t v[BN_MAX_LIMBS]; int n; } bn_t;

void bn_zero(bn_t *a);
void bn_from_be(bn_t *a, const uint8_t *p, size_t len);
void bn_to_be(const bn_t *a, uint8_t *p, size_t len);
int  bn_cmp(const bn_t *a, const bn_t *b);
int  bn_is_zero(const bn_t *a);

/* ---- RSA 私钥（CRT） ---- */
typedef struct {
    int    nbits;
    bn_t   n, d, p, q, dp, dq, qinv;
    int    have_crt;
} rsa_key;

/* 解析 OBKKEY1 原始格式 */
int  rsa_key_load(const uint8_t *blob, size_t len, rsa_key *k);
/* 原始私钥运算：out = in^d mod n，长度均为 nbits/8 */
int  rsa_raw_sign(const rsa_key *k, const uint8_t *in, uint8_t *out);
/* 生成 AVB 公钥 blob：u32 位数 + u32 n0inv + n + rr */
int  rsa_avb_pubkey(const rsa_key *k, buf_t *out);
/* PKCS#1 v1.5 + SHA-256 DigestInfo 填充，输出 keylen 字节 */
void rsa_pkcs1_sha256_pad(const uint8_t hash[32], size_t keylen, uint8_t *out);

/* ---- base64 ---- */
void b64_encode(const uint8_t *p, size_t n, buf_t *out);
int  b64_decode(const char *s, buf_t *out);

#endif
