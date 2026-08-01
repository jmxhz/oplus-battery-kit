/* AVB 2.0：footer / vbmeta 解析、免解拼接、自签 */
#ifndef OBK_AVB_H
#define OBK_AVB_H

#include "common.h"
#include "crypto.h"

#define AVB_FOOTER_MAGIC   "AVBf"
#define AVB_VBMETA_MAGIC   "AVB0"
#define AVB_FOOTER_SIZE    64
#define AVB_HEADER_SIZE    256

#define AVB_ALG_NONE            0
#define AVB_ALG_SHA256_RSA2048  1
#define AVB_ALG_SHA256_RSA4096  2
#define AVB_ALG_SHA256_RSA8192  3

#define AVB_DESC_PROPERTY   0
#define AVB_DESC_HASHTREE   1
#define AVB_DESC_HASH       2
#define AVB_DESC_CMDLINE    3
#define AVB_DESC_CHAIN      4

typedef enum { AVB_FORM_NONE = 0, AVB_FORM_FOOTER, AVB_FORM_RAW } avb_form;

typedef struct {
    avb_form form;
    uint64_t orig_image_size;
    uint64_t vbmeta_offset;
    uint64_t vbmeta_size;
    uint64_t image_size;        /* 整个分区/镜像字节数 */
} avb_layout;

typedef struct {
    int      valid;
    uint32_t algorithm;
    uint64_t rollback_index;
    uint32_t flags;
    uint32_t rollback_index_location;
    char     release_string[49];

    /* hash 描述符 */
    int      have_hash;
    char     partition_name[128];
    char     hash_algorithm[33];
    uint64_t hd_image_size;
    uint8_t  salt[128];
    uint32_t salt_len;
    uint32_t digest_len;
    uint32_t hd_flags;

    /* 其余描述符原样保留，重建时照抄 */
    uint8_t *other_desc;
    size_t   other_desc_len;
} avb_params;

/* 探测镜像的 AVB 形态 */
int  avb_probe(const uint8_t *img, size_t len, avb_layout *out);
/* 从镜像中抠出 vbmeta blob（调用方 free） */
int  avb_extract_vbmeta(const uint8_t *img, size_t len,
                        const avb_layout *lay, buf_t *out);
/* 解析 vbmeta 参数 */
int  avb_parse_vbmeta(const uint8_t *vb, size_t len, avb_params *p);
void avb_params_free(avb_params *p);

/* 免解拼接：data + 官方 vbmeta + 零填充 + 重建 footer，输出恰好 partition_size */
int  avb_graft(const uint8_t *data, size_t data_len,
               const uint8_t *stock_vbmeta, size_t vb_len,
               uint64_t stock_orig_size, uint64_t partition_size,
               buf_t *out);

/* 自签：按 params 重建 vbmeta 并写入 footer，输出恰好 partition_size */
int  avb_sign(const uint8_t *data, size_t data_len,
              const avb_params *p, const rsa_key *key,
              uint64_t partition_size, buf_t *out);

/* 参数持久化 */
int  avb_params_save(const avb_params *p, const char *dir);
int  avb_params_load(avb_params *p, const char *dir);

const char *avb_form_name(avb_form f);
const char *avb_alg_name(uint32_t a);

#endif
