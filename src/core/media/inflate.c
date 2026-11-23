/* DEFLATE decoder (inflate.h). ZetRo's own code, written from RFC 1951
 * (block formats, the canonical Huffman code assignment of section 3.2.2,
 * the length and distance tables) and RFC 1950 (the wrapper and Adler-32). */
#include "inflate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXBITS 15

typedef struct state {
    const uint8_t *src;
    size_t length, pos;
    uint32_t bits;                      /* bits not yet used, the next one in bit 0 */
    unsigned have;                      /* how many bits `bits` holds */
    uint8_t *out;
    size_t n, cap, limit;
    const char *error;                  /* the first failure, NULL while all is well */
} state;

/* A canonical Huffman code: how many codes there are of each length, and the symbols in code order. */
typedef struct huff {
    uint16_t count[MAXBITS + 1];
    uint16_t symbol[288];
} huff;

static int fail(state *s, const char *why) {
    if (!s->error) s->error = why;
    return 0;
}

/* The next `need` bits (at most 16), the first one in bit 0 of the result. */
static unsigned getbits(state *s, unsigned need) {
    while (s->have < need) {
        if (s->pos >= s->length) { fail(s, "The compressed data is cut short"); return 0; }
        s->bits |= (uint32_t)s->src[s->pos++] << s->have;
        s->have += 8;
    }
    const unsigned v = s->bits & ((1u << need) - 1u);
    s->bits >>= need;
    s->have -= need;
    return v;
}

/* Builds the code from the length of each symbol (0: unused). Returns the number of codes left
 * unassigned (0 for a complete code), or -1 when the lengths promise more codes than exist. */
static int huff_build(huff *h, const uint8_t *lengths, unsigned n) {
    uint16_t offset[MAXBITS + 2];
    memset(h->count, 0, sizeof h->count);
    for (unsigned i = 0; i < n; ++i) ++h->count[lengths[i]];
    h->count[0] = 0;
    int left = 1;
    for (unsigned len = 1; len <= MAXBITS; ++len) {
        left = left * 2 - (int)h->count[len];
        if (left < 0) return -1;
    }
    offset[1] = 0;
    for (unsigned len = 1; len <= MAXBITS; ++len) offset[len + 1] = (uint16_t)(offset[len] + h->count[len]);
    for (unsigned i = 0; i < n; ++i)
        if (lengths[i]) h->symbol[offset[lengths[i]]++] = (uint16_t)i;
    return left;
}

/* The next symbol: the code is read most significant bit first, and a code of length L
 * is one of the count[L] consecutive values starting at `first` (RFC 1951 section 3.2.2). */
static int huff_decode(state *s, const huff *h) {
    unsigned code = 0, first = 0, index = 0;
    for (unsigned len = 1; len <= MAXBITS; ++len) {
        code = code << 1 | getbits(s, 1);
        if (s->error) return -1;
        const unsigned count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
    }
    fail(s, "The compressed data has a code that does not exist");
    return -1;
}

static int put(state *s, uint8_t byte) {
    if (s->n >= s->cap) {
        if (s->n >= s->limit) return fail(s, "The decompressed data is too large");
        size_t cap = s->cap ? s->cap * 2 : 4096;
        if (cap > s->limit) cap = s->limit;
        uint8_t *p = realloc(s->out, cap);
        if (!p) return fail(s, "Out of memory");
        s->out = p;
        s->cap = cap;
    }
    s->out[s->n++] = byte;
    return 1;
}

static const uint16_t LEN_BASE[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                      35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t LEN_EXTRA[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t DIST_BASE[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                       257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t DIST_EXTRA[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                       7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* The symbols of one compressed block, up to the end-of-block symbol. */
static int codes(state *s, const huff *lit, const huff *dist) {
    for (;;) {
        const int sym = huff_decode(s, lit);
        if (sym < 0) return 0;
        if (sym < 256) { if (!put(s, (uint8_t)sym)) return 0; continue; }
        if (sym == 256) return 1;
        const unsigned li = (unsigned)sym - 257;
        if (li >= 29) return fail(s, "The compressed data has a bad length code");
        unsigned length = LEN_BASE[li] + getbits(s, LEN_EXTRA[li]);
        if (s->error) return 0;
        const int ds = huff_decode(s, dist);
        if (ds < 0) return 0;
        if (ds >= 30) return fail(s, "The compressed data has a bad distance code");
        const unsigned distance = DIST_BASE[ds] + getbits(s, DIST_EXTRA[ds]);
        if (s->error) return 0;
        if (distance > s->n) return fail(s, "The compressed data points before its start");
        while (length--) {
            if (!put(s, s->out[s->n - distance])) return 0;
        }
    }
}

static int stored(state *s) {
    s->bits = 0;                        /* the rest of the current byte is dropped */
    s->have = 0;
    if (s->length - s->pos < 4) return fail(s, "The compressed data is cut short");
    const unsigned len = s->src[s->pos] | (unsigned)s->src[s->pos + 1] << 8;
    const unsigned nlen = s->src[s->pos + 2] | (unsigned)s->src[s->pos + 3] << 8;
    s->pos += 4;
    if ((len ^ 0xFFFFu) != nlen) return fail(s, "The compressed data has a damaged stored block");
    if (s->length - s->pos < len) return fail(s, "The compressed data is cut short");
    for (unsigned i = 0; i < len; ++i)
        if (!put(s, s->src[s->pos++])) return 0;
    return 1;
}

static int fixed(state *s) {
    static huff lit, dist;
    static int built;
    if (!built) {
        uint8_t lengths[288];
        for (unsigned i = 0; i < 288; ++i) lengths[i] = (uint8_t)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
        huff_build(&lit, lengths, 288);
        for (unsigned i = 0; i < 30; ++i) lengths[i] = 5;
        huff_build(&dist, lengths, 30);
        built = 1;
    }
    return codes(s, &lit, &dist);
}

static int dynamic(state *s) {
    static const uint8_t ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[288 + 32];
    huff lencode, lit, dist;
    const unsigned nlen = getbits(s, 5) + 257;
    const unsigned ndist = getbits(s, 5) + 1;
    const unsigned ncode = getbits(s, 4) + 4;
    if (s->error) return 0;
    if (nlen > 286 || ndist > 30) return fail(s, "The compressed data has bad code counts");
    memset(lengths, 0, 19);
    for (unsigned i = 0; i < ncode; ++i) lengths[ORDER[i]] = (uint8_t)getbits(s, 3);
    if (s->error) return 0;
    if (huff_build(&lencode, lengths, 19) != 0) return fail(s, "The compressed data has a bad code-length code");
    unsigned n = 0;
    while (n < nlen + ndist) {
        const int sym = huff_decode(s, &lencode);
        if (sym < 0) return 0;
        if (sym < 16) { lengths[n++] = (uint8_t)sym; continue; }
        unsigned repeat, value = 0;
        if (sym == 16) {
            if (n == 0) return fail(s, "The compressed data repeats a length that is not there");
            value = lengths[n - 1];
            repeat = 3 + getbits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + getbits(s, 3);
        } else {
            repeat = 11 + getbits(s, 7);
        }
        if (s->error) return 0;
        if (n + repeat > nlen + ndist) return fail(s, "The compressed data has too many lengths");
        while (repeat--) lengths[n++] = (uint8_t)value;
    }
    if (lengths[256] == 0) return fail(s, "The compressed data has no end-of-block code");
    /* A code may be incomplete (a block with a single distance code is), never over-subscribed. */
    if (huff_build(&lit, lengths, nlen) < 0) return fail(s, "The compressed data has a bad literal code");
    if (huff_build(&dist, lengths + nlen, ndist) < 0) return fail(s, "The compressed data has a bad distance code");
    return codes(s, &lit, &dist);
}

static void report(const state *s, char *error, size_t capacity) {
    if (capacity) snprintf(error, capacity, "%s", s->error ? s->error : "The compressed data is damaged");
}

int zr_inflate(const uint8_t *src, size_t length, size_t limit, uint8_t **out, size_t *out_length, size_t *used,
               char *error, size_t capacity) {
    state s;
    memset(&s, 0, sizeof s);
    s.src = src;
    s.length = length;
    s.limit = limit;
    int final = 0;
    while (!final && !s.error) {
        final = (int)getbits(&s, 1);
        const unsigned type = getbits(&s, 2);
        if (s.error) break;
        if (type == 0) stored(&s);
        else if (type == 1) fixed(&s);
        else if (type == 2) dynamic(&s);
        else fail(&s, "The compressed data has a bad block type");
    }
    if (s.error) {
        report(&s, error, capacity);
        free(s.out);
        return 0;
    }
    if (!s.out) {
        s.out = malloc(1);
        if (!s.out) { if (capacity) snprintf(error, capacity, "Out of memory"); return 0; }
    }
    /* The bits left in the buffer belong to whole bytes that were read ahead. */
    *out = s.out;
    *out_length = s.n;
    if (used) *used = s.pos - s.have / 8;
    return 1;
}

static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    while (n) {
        const size_t chunk = n < 5000 ? n : 5000;       /* few enough that the sums cannot overflow */
        for (size_t i = 0; i < chunk; ++i) { a += p[i]; b += a; }
        a %= 65521u;
        b %= 65521u;
        p += chunk;
        n -= chunk;
    }
    return b << 16 | a;
}

int zr_inflate_zlib(const uint8_t *src, size_t length, size_t limit, uint8_t **out, size_t *out_length,
                    char *error, size_t capacity) {
    size_t skip = 0;
    if (length >= 2 && (src[0] & 0x0Fu) == 8 && (src[0] >> 4) <= 7 && ((unsigned)src[0] << 8 | src[1]) % 31u == 0) {
        if (src[1] & 0x20u) {
            if (capacity) snprintf(error, capacity, "The compressed data needs a preset dictionary");
            return 0;
        }
        skip = 2;
    }
    size_t used = 0;
    if (!zr_inflate(src + skip, length - skip, limit, out, out_length, &used, error, capacity)) return 0;
    if (skip) {
        const size_t at = skip + used;
        if (length - at >= 4) {
            const uint32_t want = (uint32_t)src[at] << 24 | (uint32_t)src[at + 1] << 16 | (uint32_t)src[at + 2] << 8 | src[at + 3];
            if (adler32(*out, *out_length) != want) {
                free(*out);
                *out = NULL;
                if (capacity) snprintf(error, capacity, "The compressed data fails its checksum");
                return 0;
            }
        }
    }
    return 1;
}
