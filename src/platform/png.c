/* PNG screenshots (png.h). ZetRo's own code, written from RFC 2083 (the
 * file), RFC 1950 (zlib) and RFC 1951 (deflate: the fixed Huffman code and
 * the length and distance tables). */
#include "png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int zr_png_name(const char *path) {
    const size_t n = strlen(path);
    if (n < 4) return 0;
    const char *e = path + n - 4;
    return e[0] == '.' && (e[1] | 32) == 'p' && (e[2] | 32) == 'n' && (e[3] | 32) == 'g';
}

/* --- checksums --------------------------------------------------------------------- */

static uint32_t crc_table[256];
static int crc_ready;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n) {
    if (!crc_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        crc_ready = 1;
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 255u] ^ (crc >> 8);
    return ~crc;
}

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        const size_t k = n < 5552 ? n : 5552;
        for (size_t i = 0; i < k; ++i) { a += p[i]; b += a; }
        a %= 65521u;
        b %= 65521u;
        p += k;
        n -= k;
    }
    return b << 16 | a;
}

/* --- deflate, one block of the fixed Huffman code ------------------------------------------ */

typedef struct bits {
    uint8_t *out;
    size_t n;
    uint32_t acc;
    int count;
} bits;

static void put_bits(bits *b, uint32_t value, int count) {     /* least significant bit first */
    b->acc |= value << b->count;
    b->count += count;
    while (b->count >= 8) {
        b->out[b->n++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->count -= 8;
    }
}

static void put_code(bits *b, uint32_t code, int length) {      /* Huffman codes go most significant bit first */
    uint32_t r = 0;
    for (int i = 0; i < length; ++i) r |= (code >> i & 1u) << (length - 1 - i);
    put_bits(b, r, length);
}

static void put_literal(bits *b, unsigned v) {
    if (v < 144) put_code(b, 0x30u + v, 8);
    else if (v < 256) put_code(b, 0x190u + (v - 144), 9);
    else if (v < 280) put_code(b, v - 256, 7);
    else put_code(b, 0xC0u + (v - 280), 8);
}

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static void put_match(bits *b, unsigned length, unsigned distance) {
    unsigned l = 28;
    while (LEN_BASE[l] > length) --l;
    put_literal(b, 257u + l);
    put_bits(b, length - LEN_BASE[l], LEN_EXTRA[l]);
    unsigned d = 29;
    while (DIST_BASE[d] > distance) --d;
    put_code(b, d, 5);
    put_bits(b, distance - DIST_BASE[d], DIST_EXTRA[d]);
}

#define HASH_BITS 15
#define WINDOW 32768u

/* The zlib stream of `data`; the caller's buffer holds at least len * 9 / 8 + 64 bytes. */
static size_t deflate(const uint8_t *data, size_t len, uint8_t *out) {
    bits b = {out, 0, 0, 0};
    int32_t *head = malloc(((size_t)1 << HASH_BITS) * sizeof *head);
    if (!head) return 0;
    for (size_t i = 0; i < (size_t)1 << HASH_BITS; ++i) head[i] = -1;
    out[b.n++] = 0x78;
    out[b.n++] = 0x01;
    put_bits(&b, 1, 1);     /* the only block is the last */
    put_bits(&b, 1, 2);     /* fixed Huffman codes */
    size_t i = 0;
    while (i < len) {
        unsigned best = 0, best_distance = 0;
        if (i + 3 <= len) {
            const uint32_t h = ((uint32_t)data[i] * 506832829u ^ (uint32_t)data[i + 1] * 2654435761u ^ (uint32_t)data[i + 2] * 40503u) >> (32 - HASH_BITS);
            const int32_t candidate = head[h];
            head[h] = (int32_t)i;
            if (candidate >= 0 && i - (size_t)candidate <= WINDOW) {
                const size_t max = len - i < 258 ? len - i : 258;
                size_t m = 0;
                while (m < max && data[(size_t)candidate + m] == data[i + m]) ++m;
                if (m >= 3) { best = (unsigned)m; best_distance = (unsigned)(i - (size_t)candidate); }
            }
        }
        if (best) {
            put_match(&b, best, best_distance);
            /* The positions inside the match join the hash too, so runs keep matching. */
            for (size_t k = 1; k < best && i + k + 3 <= len; ++k) {
                const size_t p = i + k;
                const uint32_t h = ((uint32_t)data[p] * 506832829u ^ (uint32_t)data[p + 1] * 2654435761u ^ (uint32_t)data[p + 2] * 40503u) >> (32 - HASH_BITS);
                head[h] = (int32_t)p;
            }
            i += best;
        } else put_literal(&b, data[i++]);
    }
    put_literal(&b, 256);
    if (b.count) put_bits(&b, 0, 8 - b.count);
    const uint32_t a = adler32(data, len);
    out[b.n++] = (uint8_t)(a >> 24);
    out[b.n++] = (uint8_t)(a >> 16);
    out[b.n++] = (uint8_t)(a >> 8);
    out[b.n++] = (uint8_t)a;
    free(head);
    return b.n;
}

/* --- the file --------------------------------------------------------------------------------- */

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static int chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
    uint8_t head[8], tail[4];
    be32(head, (uint32_t)n);
    memcpy(head + 4, type, 4);
    uint32_t crc = crc32_update(0, head + 4, 4);
    crc = crc32_update(crc, data, n);
    be32(tail, crc);
    return fwrite(head, 1, 8, f) == 8 && (!n || fwrite(data, 1, n, f) == n) && fwrite(tail, 1, 4, f) == 4;
}

int zr_png_write(const char *path, const uint32_t *argb, int width, int height, size_t pitch, int flip) {
    if (width <= 0 || height <= 0) return 0;
    const size_t stride = 1u + 3u * (size_t)width, raw_size = stride * (size_t)height;
    uint8_t *raw = malloc(raw_size);
    uint8_t *packed = malloc(raw_size + raw_size / 8 + 64);
    if (!raw || !packed) { free(raw); free(packed); return 0; }
    for (int y = 0; y < height; ++y) {
        const uint32_t *row = (const uint32_t *)((const uint8_t *)argb + (size_t)(flip ? height - 1 - y : y) * pitch);
        uint8_t *o = raw + (size_t)y * stride;
        o[0] = 1;                                           /* the Sub filter: each byte less the one a pixel before */
        uint8_t prev[3] = {0, 0, 0};
        for (int x = 0; x < width; ++x) {
            const uint8_t c[3] = {(uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x]};
            for (int k = 0; k < 3; ++k) { o[1 + 3 * x + k] = (uint8_t)(c[k] - prev[k]); prev[k] = c[k]; }
        }
    }
    const size_t packed_size = deflate(raw, raw_size, packed);
    free(raw);
    FILE *f = packed_size ? fopen(path, "wb") : NULL;
    int ok = f != NULL;
    if (ok) {
        static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)width);
        be32(ihdr + 4, (uint32_t)height);
        ihdr[8] = 8;        /* bits a sample */
        ihdr[9] = 2;        /* RGB */
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        ok = fwrite(signature, 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, sizeof ihdr) && chunk(f, "IDAT", packed, packed_size) &&
             chunk(f, "IEND", NULL, 0);
        if (fclose(f)) ok = 0;
        if (!ok) remove(path);
    }
    free(packed);
    return ok;
}
