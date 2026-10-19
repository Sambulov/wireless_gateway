#include "esp_rom_md5.h"

#include <string.h>

/* MD5 (RFC 1321) для хостовых тестов: служба прошивки считает им контрольную
 * сумму образа и сверяет с той, что прислал клиент. Заглушка обязана считать
 * по-настоящему — иначе тест «MD5 не совпал» проверял бы совпадение двух
 * заглушек, а не сверку. Реализация без таблиц-констант, кроме раундовых. */

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static uint32_t md5_rotl(uint32_t x, uint8_t n) {
    return (x << n) | (x >> (32 - n));
}

static void md5_block(uint32_t state[4], const uint8_t block[64]) {
    uint32_t m[16], a = state[0], b = state[1], c = state[2], d = state[3];

    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) | ((uint32_t)block[i * 4 + 3] << 24);

    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16)      { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
        else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }

        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + md5_rotl(a + f + md5_k[i] + m[g], md5_s[i]);
        a = tmp;
    }

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void esp_rom_md5_init(md5_context_t *context) {
    memset(context, 0, sizeof(*context));
    context->buf[0] = 0x67452301;
    context->buf[1] = 0xefcdab89;
    context->buf[2] = 0x98badcfe;
    context->buf[3] = 0x10325476;
}

void esp_rom_md5_update(md5_context_t *context, const void *buf, uint32_t len) {
    const uint8_t *p = buf;
    uint32_t used = (context->bits[0] >> 3) & 0x3f;
    uint32_t low_before = context->bits[0];

    context->bits[0] = low_before + (len << 3);
    if (context->bits[0] < low_before) context->bits[1]++;
    context->bits[1] += len >> 29;

    uint32_t i = 0;
    uint32_t free_in_block = 64 - used;
    if (len >= free_in_block) {
        memcpy(context->in + used, p, free_in_block);
        md5_block(context->buf, context->in);
        for (i = free_in_block; i + 63 < len; i += 64)
            md5_block(context->buf, p + i);
        used = 0;
    }
    memcpy(context->in + used, p + i, len - i);
}

void esp_rom_md5_final(uint8_t *digest, md5_context_t *context) {
    uint8_t pad[64];
    uint8_t len_le[8];
    uint32_t used = (context->bits[0] >> 3) & 0x3f;
    uint32_t pad_len = (used < 56) ? (56 - used) : (120 - used);

    for (int i = 0; i < 4; i++) {
        len_le[i]     = (uint8_t)(context->bits[0] >> (8 * i));
        len_le[4 + i] = (uint8_t)(context->bits[1] >> (8 * i));
    }
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;

    esp_rom_md5_update(context, pad, pad_len);
    esp_rom_md5_update(context, len_le, sizeof(len_le));

    for (int i = 0; i < 4; i++) {
        digest[i * 4]     = (uint8_t)(context->buf[i]);
        digest[i * 4 + 1] = (uint8_t)(context->buf[i] >> 8);
        digest[i * 4 + 2] = (uint8_t)(context->buf[i] >> 16);
        digest[i * 4 + 3] = (uint8_t)(context->buf[i] >> 24);
    }
}
