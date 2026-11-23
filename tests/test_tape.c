/* The tape's smoke test: small tapes of every format are built in memory, read, and played by the
 * deck, and the pulses are checked; a recording of a SAVE is turned into blocks, written as TAP,
 * TZX and PZX, read back and played to the same pulses. No SDL, no files. */
#include "media/inflate.h"
#include "media/tape.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* --- building files ------------------------------------------------------------------------ */

typedef struct buf { uint8_t d[8192]; size_t n; } buf;
static void b8(buf *b, unsigned v) { b->d[b->n++] = (uint8_t)v; }
static void b16(buf *b, unsigned v) { b8(b, v); b8(b, v >> 8); }
static void b24(buf *b, unsigned v) { b16(b, v); b8(b, v >> 16); }
static void b32(buf *b, uint32_t v) { b16(b, v & 0xFFFFu); b16(b, v >> 16); }
static void bs(buf *b, const char *s, size_t n) { memcpy(b->d + b->n, s, n); b->n += n; }
static void tzx_start(buf *b) { b->n = 0; bs(b, "ZXTape!\x1A", 8); b8(b, 1); b8(b, 20); }

static zr_tape *open_bytes(const uint8_t *d, size_t n, const char *name, char *error) {
    zr_media *m = zr_media_from(name, d, n);
    zr_tape *t = zr_tape_open(m, error, 160);
    zr_media_close(m);
    return t;
}

/* --- playing ------------------------------------------------------------------------------- */

static uint64_t ticks(uint64_t tstates, uint32_t clock) { return tstates * clock / ZR_TAPE_TSTATE_HZ; }

/* The edges the deck makes from now until it has none (or `cap` of them), in guest time. */
static size_t drain(zr_tape_deck *d, uint64_t *out, size_t cap) {
    size_t n = 0;
    while (n < cap) {
        const uint64_t e = zr_tape_deck_next_edge(d);
        if (e == ZR_TAPE_NEVER) break;
        out[n++] = e;
        zr_tape_deck_level(d, e);
    }
    return n;
}

static size_t play_edges(zr_tape_deck *d, uint64_t start, uint64_t *out, size_t cap) {
    zr_tape_deck_play(d, start);
    const size_t n = drain(d, out, cap);
    if (n < cap) zr_tape_deck_advance(d, (n ? out[n - 1] : start) + 100000000u);
    return n;
}

static uint64_t edges[16384], edges2[16384], edges3[16384];

/* Whether the pulses `dur` (T-states), the first at `first_level`, are what the tape plays on its own:
 * the level at the start, then an edge at the end of every pulse but the last. */
static int plays_pulses(zr_tape *t, uint32_t clock, const uint32_t *dur, size_t count, int first_level) {
    zr_tape_deck d;
    zr_tape_deck_init(&d, clock);
    zr_tape_deck_set_tape(&d, t);
    zr_tape_deck_play(&d, 0);
    int ok = zr_tape_deck_level(&d, 0) == first_level;                  /* a first pulse that is high is an edge at the start */
    const size_t n = drain(&d, edges, 16384);
    uint64_t sum = 0;
    size_t k = 0;
    for (size_t i = 0; ok && i + 1 < count; ++i) {
        sum += dur[i];
        ok = k < n && edges[k++] == ticks(sum, clock);
    }
    ok = ok && k == n;
    zr_tape_deck_release(&d);
    return ok;
}

static int file_plays(const buf *f, const char *name, uint32_t clock, const uint32_t *dur, size_t count, int first_level) {
    char error[160];
    zr_tape *t = open_bytes(f->d, f->n, name, error);
    if (!t) { printf("  %s: %s\n", name, error); return 0; }
    const int ok = plays_pulses(t, clock, dur, count, first_level);
    zr_tape_close(t);
    return ok;
}

/* --- the tests ------------------------------------------------------------------------------- */

static const uint8_t DEFLATE_DYNAMIC[] = {
    0x78, 0xDA, 0x55, 0xCE, 0xBB, 0x0D, 0x80, 0x20, 0x00, 0x40, 0xC1, 0x55, 0x18, 0x81, 0x27, 0x7E, 0x20, 0x4C,
    0x83, 0xC6, 0x4A, 0x13, 0x2D, 0xD8, 0x3F, 0xB4, 0xBC, 0xF2, 0xBA, 0xEB, 0xED, 0xBF, 0xC3, 0xF9, 0x7E, 0xD7,
    0x13, 0x62, 0xAC, 0x7D, 0x12, 0xD2, 0x22, 0x25, 0x69, 0x95, 0x36, 0x69, 0x97, 0x0E, 0x29, 0x4B, 0x65, 0x16,
    0xBA, 0xA0, 0x0B, 0xBA, 0xA0, 0x0B, 0xBA, 0xA0, 0x0B, 0xBA, 0xA0, 0x0B, 0xBA, 0x50, 0xEA, 0x00, 0x8E, 0x26,
    0x5B, 0xA5};
static const uint8_t DEFLATE_FIXED[] = {
    0x78, 0x01, 0x2B, 0x49, 0x2C, 0x48, 0x55, 0x48, 0xCA, 0xC9, 0x4F, 0xCE, 0x56, 0x30, 0x30, 0xB0, 0x2E, 0x41,
    0xE2, 0x19, 0xA2, 0xF0, 0x8C, 0x50, 0x78, 0xC6, 0x28, 0x3C, 0x13, 0x14, 0x9E, 0x29, 0x0A, 0xCF, 0x0C, 0x85,
    0x67, 0x8E, 0xC2, 0xB3, 0x40, 0xE1, 0x59, 0x22, 0xF3, 0x0C, 0x51, 0xDC, 0x62, 0x88, 0xE2, 0x16, 0x43, 0x14,
    0xB7, 0x18, 0xA2, 0xB8, 0xC5, 0x10, 0xC5, 0x2D, 0x86, 0x28, 0x6E, 0x31, 0x44, 0x71, 0x8B, 0x21, 0x8A, 0x5B,
    0x0C, 0x51, 0xDC, 0x62, 0x68, 0x69, 0x0D, 0x00, 0x8E, 0x26, 0x5B, 0xA5};
static const uint8_t CSW_ZRLE[] = {
    0x78, 0xDA, 0x63, 0x65, 0x66, 0xD0, 0x61, 0x64, 0x60, 0x60, 0x62, 0xE7, 0xA4, 0x2E, 0x04, 0x00, 0x77, 0xBF,
    0x02, 0xB8};

static int inflates_to(const uint8_t *z, size_t zn, const char *want, size_t wn) {
    uint8_t *out = NULL;
    size_t n = 0;
    char error[160];
    if (!zr_inflate_zlib(z, zn, 4096, &out, &n, error, sizeof error)) return 0;
    const int ok = n == wn && !memcmp(out, want, wn);
    free(out);
    return ok;
}

static void test_inflate(void) {
    char text[300];
    size_t n = 0;
    for (int i = 0; i < 20; ++i) n += (size_t)snprintf(text + n, sizeof text - n, "tape block %02d;", i);
    CHECK(inflates_to(DEFLATE_DYNAMIC, sizeof DEFLATE_DYNAMIC, text, n));           /* a dynamic Huffman block */
    CHECK(inflates_to(DEFLATE_FIXED, sizeof DEFLATE_FIXED, text, n));               /* a fixed one */
    static const uint8_t STORED[] = {0x78, 0x01, 0x01, 0x05, 0x00, 0xFA, 0xFF, 'h', 'e', 'l', 'l', 'o'};
    CHECK(inflates_to(STORED, sizeof STORED, "hello", 5));
    /* Damage is an error: cut short, a wrong checksum, too large for the limit. */
    uint8_t bad[sizeof DEFLATE_DYNAMIC];
    uint8_t *out;
    size_t len;
    char error[160];
    CHECK(!zr_inflate_zlib(DEFLATE_DYNAMIC, sizeof DEFLATE_DYNAMIC - 20, 4096, &out, &len, error, sizeof error));
    memcpy(bad, DEFLATE_DYNAMIC, sizeof bad);
    bad[sizeof bad - 1] ^= 1;
    CHECK(!zr_inflate_zlib(bad, sizeof bad, 4096, &out, &len, error, sizeof error));
    CHECK(!zr_inflate_zlib(DEFLATE_DYNAMIC, sizeof DEFLATE_DYNAMIC, 100, &out, &len, error, sizeof error));
}

/* TAP: a header and a data block at the ROM's speed; the deck's pulses in guest time on a 10 MHz clock
 * (not a multiple of 3.5 MHz, so that the carried remainder shows). */
static void test_tap(void) {
    static buf f;
    f.n = 0;
    uint8_t header[19] = {0, 0};
    memcpy(header + 2, "TEST      ", 10);
    header[12] = 3;
    uint8_t parity = 0;
    for (int i = 0; i < 18; ++i) parity ^= header[i];
    header[18] = parity;
    b16(&f, 19);
    bs(&f, (const char *)header, 19);
    b16(&f, 5);
    static const uint8_t DATA[5] = {0xFF, 'A', 'B', 'C', 0xFF ^ 'A' ^ 'B' ^ 'C'};
    bs(&f, (const char *)DATA, 5);

    char error[160], name[64];
    zr_tape *t = open_bytes(f.d, f.n, "test.TAP", error);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(zr_tape_format(t) == ZR_TAPE_TAP && zr_tape_count(t) == 2 && zr_tape_block_kind(t, 0) == ZR_TB_STANDARD);
    zr_tape_block_name(t, 0, name, sizeof name);
    CHECK(!strcmp(name, "Program: TEST"));
    zr_tape_block_name(t, 1, name, sizeof name);
    CHECK(!strcmp(name, "Data 3 bytes"));
    const uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned flag = 0, pause = 0;
    CHECK(zr_tape_block_data(t, 1, &bytes, &length, &flag, &pause) && length == 5 && flag == 0xFF && pause == 1000 && bytes[1] == 'A');

    /* The pulses of the header block: 8063 pilot, 2 sync, 19 bytes of bits, then the pause (1 ms at the
     * level the next pulse would have, then low); the next block's first pulse is low, so the edge ending it
     * is the first of that block. */
    static uint32_t pulses[8063 + 2 + 19 * 16];
    size_t np = 0;
    uint64_t sum = 0;
    for (int i = 0; i < 8063; ++i) pulses[np++] = 2168;
    pulses[np++] = 667;
    pulses[np++] = 735;
    for (int i = 0; i < 19; ++i)
        for (int bit = 7; bit >= 0; --bit) { const uint32_t p = header[i] >> bit & 1 ? 1710 : 855; pulses[np++] = p; pulses[np++] = p; }
    for (size_t i = 0; i < np; ++i) sum += pulses[i];
    const uint32_t clock = 10000000u;
    zr_tape_deck d;
    zr_tape_deck_init(&d, clock);
    zr_tape_deck_set_tape(&d, t);
    CHECK(zr_tape_deck_find_data(&d) == 0);
    CHECK(zr_tape_deck_play(&d, 0));
    const size_t n = drain(&d, edges, 8371);
    CHECK(n == 8371);
    CHECK(edges[0] == ticks(2168, clock) && edges[8062] == ticks(8063 * 2168, clock));      /* no drift after the pilot */
    CHECK(edges[8064] == ticks(8063 * 2168 + 667 + 735, clock));
    CHECK(edges[8368] == ticks(sum, clock));                                                /* the last pulse of the block */
    CHECK(edges[8369] == ticks(sum + 3500, clock));                                         /* the 1 ms of the pause ends */
    CHECK(edges[8370] == ticks(sum + 3500000 + 2168, clock));                               /* the next block's first pulse */
    CHECK(zr_tape_block_tstates(t, 0) == sum + 3500000);
    CHECK(zr_tape_deck_block(&d) == 1);
    /* Transport: next block while running, rewind stops. */
    zr_tape_deck_seek(&d, 1, edges[8370] + 100);
    CHECK(zr_tape_deck_playing(&d) && zr_tape_deck_block(&d) == 1);
    zr_tape_deck_previous(&d, edges[8370] + 200);
    CHECK(zr_tape_deck_block(&d) == 0);
    zr_tape_deck_rewind(&d, edges[8370] + 300);
    CHECK(!zr_tape_deck_playing(&d) && zr_tape_deck_block(&d) == 0 && zr_tape_deck_next_edge(&d) == ZR_TAPE_NEVER);
    /* The level at a time: low before the first edge, high from it, low from the next. */
    zr_tape_deck_rewind(&d, 0);
    CHECK(zr_tape_deck_play(&d, 0));
    CHECK(zr_tape_deck_level(&d, edges[0] - 1) == 0 && zr_tape_deck_level(&d, edges[0]) == 1 && zr_tape_deck_level(&d, edges[1]) == 0);
    zr_tape_deck_release(&d);
    zr_tape_close(t);

    /* An empty .tap is an empty tape; a TAP that is cut short is an error. */
    t = open_bytes(NULL, 0, "new.tap", error);
    CHECK(t && zr_tape_count(t) == 0 && zr_tape_format(t) == ZR_TAPE_TAP);
    zr_tape_close(t);
    f.n -= 1;
    CHECK(open_bytes(f.d, f.n, "test.tap", error) == NULL && strstr(error, "TAP"));
}

/* TZX: loops, jumps, calls, a stop, text, an unknown block, and the other pulse blocks. */
static void test_tzx(void) {
    static buf f;
    char error[160], name[64];
    tzx_start(&f);
    b8(&f, 0x77); b32(&f, 3); bs(&f, "xyz", 3);                         /* unknown: skipped by its length */
    b8(&f, 0x12); b16(&f, 1000); b16(&f, 4);                            /* 1 */
    b8(&f, 0x24); b16(&f, 3);                                           /* 2 loop 3 times */
    b8(&f, 0x13); b8(&f, 2); b16(&f, 300); b16(&f, 700);               /* 3 */
    b8(&f, 0x25);                                                       /* 4 */
    b8(&f, 0x23); b16(&f, 2);                                           /* 5 jump over the next */
    b8(&f, 0x12); b16(&f, 9999); b16(&f, 50);                           /* 6 never played */
    b8(&f, 0x30); b8(&f, 2); bs(&f, "Hi", 2);                           /* 7 */
    b8(&f, 0x20); b16(&f, 0);                                           /* 8 stop the tape */
    b8(&f, 0x12); b16(&f, 400); b16(&f, 2);                             /* 9 */
    zr_tape *t = open_bytes(f.d, f.n, "multi.tzx", error);
    CHECK(t != NULL);
    if (!t) return;
    CHECK(zr_tape_count(t) == 10 && zr_tape_block_kind(t, 0) == ZR_TB_INFO && zr_tape_block_kind(t, 5) == ZR_TB_JUMP);
    zr_tape_block_name(t, 0, name, sizeof name);
    CHECK(!strcmp(name, "Block 119"));
    zr_tape_block_name(t, 7, name, sizeof name);
    CHECK(!strcmp(name, "Hi"));
    zr_tape_block_name(t, 1, name, sizeof name);
    CHECK(!strcmp(name, "Tone 4 x 1000"));
    static const uint32_t first[] = {1000, 1000, 1000, 1000, 300, 700, 300, 700, 300, 700};
    CHECK(plays_pulses(t, 14000000u, first, 10, 0));
    zr_tape_deck d;
    zr_tape_deck_init(&d, 14000000u);
    zr_tape_deck_set_tape(&d, t);
    CHECK(play_edges(&d, 0, edges, 100) == 9 && !zr_tape_deck_playing(&d) && zr_tape_deck_block(&d) == 9);
    /* Playing again goes on after the stop block, and the tape ends. */
    CHECK(zr_tape_deck_play(&d, 1000000000u));
    const size_t n = drain(&d, edges, 100);
    zr_tape_deck_advance(&d, 2000000000u);
    CHECK(n == 1 && edges[0] == 1000000000u + ticks(400, 14000000u) && !zr_tape_deck_playing(&d) && zr_tape_deck_block(&d) == 10);
    CHECK(!zr_tape_deck_play(&d, 3000000000u));
    zr_tape_deck_release(&d);

    /* Written back as TZX it is the same file; as PZX (loops unrolled) it plays the same until the stop. */
    uint8_t *bytes;
    size_t size;
    unsigned dropped;
    CHECK(zr_tape_write(t, ZR_TAPE_TZX, &bytes, &size, &dropped, error, sizeof error) && size == f.n && !memcmp(bytes, f.d, size));
    free(bytes);
    CHECK(zr_tape_write(t, ZR_TAPE_PZX, &bytes, &size, &dropped, error, sizeof error));
    zr_tape *p = open_bytes(bytes, size, "multi.pzx", error);
    free(bytes);
    CHECK(p && plays_pulses(p, 14000000u, first, 10, 0));
    zr_tape_close(p);
    zr_tape_close(t);

    /* The other blocks, each a tape of its own: [pulses], the level of the first. */
    static const uint32_t direct[] = {400, 400}, general[] = {500, 500, 500, 400, 200, 400, 200, 200, 200, 200, 200};
    static const uint32_t turbo[] = {1000, 1000, 1000, 300, 400, 200, 200, 100, 100, 200, 200, 100, 100};
    static const uint32_t call[] = {222, 222, 111, 111, 222, 222}, level[] = {500, 500}, csw[] = {500, 300};
    tzx_start(&f);                                                      /* direct recording: samples 1111 0000 */
    b8(&f, 0x15); b16(&f, 100); b16(&f, 0); b8(&f, 8); b24(&f, 1); b8(&f, 0xF0);
    CHECK(file_plays(&f, "direct.tzx", 14000000u, direct, 2, 1));
    tzx_start(&f);                                                      /* generalized data */
    b8(&f, 0x19); b32(&f, 33); b16(&f, 0); b32(&f, 1); b8(&f, 2); b8(&f, 1); b32(&f, 8); b8(&f, 2); b8(&f, 2);
    b8(&f, 0); b16(&f, 500); b16(&f, 0);                                /* pilot symbol 0 */
    b8(&f, 0); b16(&f, 3);                                              /* three of it */
    b8(&f, 0); b16(&f, 200); b16(&f, 0); b8(&f, 0); b16(&f, 400); b16(&f, 0);      /* data symbols 0 and 1 */
    b8(&f, 0xA0);                                                       /* 1 0 1 0 0 0 0 0 */
    CHECK(file_plays(&f, "general.tzx", 14000000u, general, 11, 0));
    tzx_start(&f);                                                      /* turbo speed, four bits used of the last byte */
    b8(&f, 0x11); b16(&f, 1000); b16(&f, 300); b16(&f, 400); b16(&f, 100); b16(&f, 200); b16(&f, 3); b8(&f, 4); b16(&f, 0);
    b24(&f, 1); b8(&f, 0xA0);
    CHECK(file_plays(&f, "turbo.tzx", 14000000u, turbo, 13, 0));
    tzx_start(&f);                                                      /* a call: 222, then back to 111, then 222 in line */
    b8(&f, 0x26); b16(&f, 1); b16(&f, 2);
    b8(&f, 0x12); b16(&f, 111); b16(&f, 2);
    b8(&f, 0x12); b16(&f, 222); b16(&f, 2);
    b8(&f, 0x27);
    CHECK(file_plays(&f, "call.tzx", 14000000u, call, 6, 0));
    tzx_start(&f);                                                      /* set signal level high */
    b8(&f, 0x2B); b32(&f, 1); b8(&f, 1);
    b8(&f, 0x12); b16(&f, 500); b16(&f, 2);
    CHECK(file_plays(&f, "level.tzx", 14000000u, level, 2, 1));
    tzx_start(&f);                                                      /* a CSW recording, 100 T-states a sample */
    b8(&f, 0x18); b32(&f, 12); b16(&f, 0); b24(&f, 35000); b8(&f, 1); b32(&f, 2); b8(&f, 5); b8(&f, 3);
    CHECK(file_plays(&f, "csw.tzx", 14000000u, csw, 2, 0));

    tzx_start(&f);                                                      /* stop if 48K: only when the board says so */
    b8(&f, 0x12); b16(&f, 300); b16(&f, 2); b8(&f, 0x2A); b32(&f, 0); b8(&f, 0x12); b16(&f, 400); b16(&f, 2);
    zr_tape *s48 = open_bytes(f.d, f.n, "s48.tzx", error);
    CHECK(s48 != NULL);
    for (int is_48k = 0; is_48k < 2; ++is_48k) {
        zr_tape_deck_init(&d, 14000000u);
        zr_tape_deck_set_tape(&d, s48);
        zr_tape_deck_set_48k(&d, is_48k);
        const size_t m = play_edges(&d, 0, edges, 100);
        CHECK(m == (is_48k ? 1u : 3u) && zr_tape_deck_block(&d) == (is_48k ? 2u : 3u));
        zr_tape_deck_release(&d);
    }
    zr_tape_close(s48);

    /* Endless loops: with no time in them the deck gives up, with time in them it keeps going. */
    tzx_start(&f);
    b8(&f, 0x22); b8(&f, 0x23); b16(&f, 0xFFFF);
    zr_tape *e = open_bytes(f.d, f.n, "loop.tzx", error);
    CHECK(e != NULL);
    zr_tape_deck_init(&d, 14000000u);
    zr_tape_deck_set_tape(&d, e);
    zr_tape_deck_play(&d, 0);
    zr_tape_deck_advance(&d, 1);
    CHECK(!zr_tape_deck_playing(&d));
    zr_tape_deck_release(&d);
    zr_tape_close(e);
    tzx_start(&f);
    b8(&f, 0x20); b16(&f, 1); b8(&f, 0x23); b16(&f, 0xFFFF);
    e = open_bytes(f.d, f.n, "loop2.tzx", error);
    zr_tape_deck_init(&d, 14000000u);
    zr_tape_deck_set_tape(&d, e);
    zr_tape_deck_play(&d, 0);
    zr_tape_deck_advance(&d, 28000000u);
    CHECK(zr_tape_deck_playing(&d));
    zr_tape_deck_release(&d);
    zr_tape_close(e);

    /* A cut-short block is an error. */
    tzx_start(&f);
    b8(&f, 0x10); b16(&f, 1000); b16(&f, 10); b8(&f, 0xFF);
    CHECK(open_bytes(f.d, f.n, "cut.tzx", error) == NULL && strstr(error, "TZX"));
}

static void test_pzx(void) {
    static buf f;
    char error[160];
    f.n = 0;
    bs(&f, "PZXT", 4); b32(&f, 4); b8(&f, 1); b8(&f, 0); bs(&f, "t\0", 2);
    bs(&f, "PULS", 4); b32(&f, 6); b16(&f, 0x8003); b16(&f, 500); b16(&f, 700);       /* 3 x 500, 700 */
    bs(&f, "DATA", 4); b32(&f, 8 + 6 + 1); b32(&f, 8); b16(&f, 100); b8(&f, 1); b8(&f, 2);
    b16(&f, 200); b16(&f, 300); b16(&f, 400); b8(&f, 0xA5);                          /* a 0 is 200, a 1 is 300 400; tail 100 */
    bs(&f, "BRWS", 4); b32(&f, 1); bs(&f, "x", 1);
    bs(&f, "ZZZZ", 4); b32(&f, 3); bs(&f, "abc", 3);                                 /* unknown: skipped */
    bs(&f, "PAUS", 4); b32(&f, 4); b32(&f, 5000u | 0x80000000u);                     /* 5000 T-states, high */
    bs(&f, "STOP", 4); b32(&f, 2); b16(&f, 0);
    static const uint32_t pulses[] = {500, 500, 500, 700, 300, 400, 200, 300, 400, 200, 200, 300, 400, 200, 300, 400, 100, 5000};
    CHECK(file_plays(&f, "t.pzx", 10000000u, pulses, sizeof pulses / sizeof *pulses, 0));
    zr_tape *t = open_bytes(f.d, f.n, "t.pzx", error);
    CHECK(t && zr_tape_count(t) == 6 && zr_tape_block_kind(t, 3) == ZR_TB_TEXT && zr_tape_block_kind(t, 5) == ZR_TB_STOP);
    zr_tape_close(t);
    f.n -= 1;
    CHECK(open_bytes(f.d, f.n, "t.pzx", error) == NULL && strstr(error, "PZX"));
}

static void test_csw_wav(void) {
    static buf f;
    char error[160];
    static const uint32_t pulses[] = {500, 300, 30000, 200};
    f.n = 0;                                                            /* CSW 1.01, RLE, starting high */
    bs(&f, "Compressed Square Wave\x1A", 23); b8(&f, 1); b8(&f, 1); b16(&f, 35000); b8(&f, 1); b8(&f, 1); b24(&f, 0);
    b8(&f, 5); b8(&f, 3); b8(&f, 0); b32(&f, 300); b8(&f, 2);
    CHECK(file_plays(&f, "a.csw", 14000000u, pulses, 4, 1));
    f.n = 0;                                                            /* CSW 2.00, Z-RLE */
    bs(&f, "Compressed Square Wave\x1A", 23); b8(&f, 2); b8(&f, 0); b32(&f, 35000); b32(&f, 84); b8(&f, 2); b8(&f, 0); b8(&f, 0);
    bs(&f, "test\0\0\0\0\0\0\0\0\0\0\0\0", 16);
    bs(&f, (const char *)CSW_ZRLE, sizeof CSW_ZRLE);
    static uint32_t z[84] = {500, 300, 30000, 200};
    for (int i = 4; i < 84; i += 2) { z[i] = 700; z[i + 1] = 900; }
    CHECK(file_plays(&f, "b.csw", 14000000u, z, 84, 0));
    f.n = 0;                                                            /* WAV, 8 bit mono, 35000 Hz: 5 high, 3 low, 4 high, 2 low */
    static const uint8_t samples[14] = {200, 200, 200, 200, 200, 56, 56, 56, 200, 200, 200, 200, 56, 56};
    static const uint32_t wav[] = {500, 300, 400, 200};
    bs(&f, "RIFF", 4); b32(&f, 36 + 14); bs(&f, "WAVEfmt ", 8); b32(&f, 16); b16(&f, 1); b16(&f, 1); b32(&f, 35000); b32(&f, 35000);
    b16(&f, 1); b16(&f, 8); bs(&f, "data", 4); b32(&f, 14); bs(&f, (const char *)samples, 14);
    CHECK(file_plays(&f, "c.wav", 14000000u, wav, 4, 1));
    f.n = 40;
    CHECK(open_bytes(f.d, f.n, "d.wav", error) == NULL && strstr(error, "WAV"));
    CHECK(open_bytes((const uint8_t *)"not a tape", 10, "x.bin", error) == NULL && strstr(error, "Not a tape"));
}

/* What the machine's SAVE puts on the MIC output: a ROM-speed block, a silence, ten odd pulses. */
static uint64_t feed_time;
static uint8_t feed_level;
static void feed(zr_tape_deck *d, uint32_t tstates) {
    feed_time += (uint64_t)tstates * 4;                                 /* a 14 MHz clock */
    feed_level ^= 1;
    zr_tape_deck_output(d, feed_level, feed_time);
}

static void test_record(void) {
    char error[160];
    zr_tape *t = zr_tape_new();
    zr_tape_deck d;
    zr_tape_deck_init(&d, 14000000u);
    zr_tape_deck_set_tape(&d, t);
    CHECK(zr_tape_deck_record(&d, 1, 1000) && !zr_tape_deck_play(&d, 1000));
    feed_time = 1000;
    feed_level = 0;
    zr_tape_deck_output(&d, 0, feed_time);
    static const uint8_t block[4] = {0xFF, 'A', 'B', 0xFF ^ 'A' ^ 'B'};
    for (int i = 0; i < 3223; ++i) feed(&d, 2168);
    feed(&d, 667);
    feed(&d, 735);
    for (int i = 0; i < 4; ++i)
        for (int bit = 7; bit >= 0; --bit) { const uint32_t p = block[i] >> bit & 1 ? 1710 : 855; feed(&d, p); feed(&d, p); }
    feed(&d, 350000);                                                   /* 100 ms of silence */
    for (int i = 0; i < 10; ++i) feed(&d, i % 2 ? 3000 : 1000);
    CHECK(zr_tape_deck_record(&d, 0, feed_time + 4 * 7000000ull));      /* two seconds after the last edge */
    CHECK(zr_tape_count(t) == 2 && zr_tape_modified(t) && zr_tape_deck_block(&d) == 2);
    const uint8_t *bytes = NULL;
    size_t length = 0;
    unsigned flag = 0, pause = 0;
    CHECK(zr_tape_block_kind(t, 0) == ZR_TB_STANDARD && zr_tape_block_data(t, 0, &bytes, &length, &flag, &pause));
    CHECK(length == 4 && !memcmp(bytes, block, 4) && pause == 100 && zr_tape_block_kind(t, 1) == ZR_TB_PULSES);

    /* As TAP: the standard block only, the other one counted. */
    uint8_t *out;
    size_t size;
    unsigned dropped = 0;
    CHECK(zr_tape_write(t, ZR_TAPE_TAP, &out, &size, &dropped, error, sizeof error));
    CHECK(size == 6 && out[0] == 4 && out[1] == 0 && !memcmp(out + 2, block, 4) && dropped == 1);
    free(out);

    /* As TZX and as PZX, read back and played: the very same edges as the recording plays. */
    zr_tape_deck_rewind(&d, 0);
    const size_t n = play_edges(&d, 0, edges, 16384);
    CHECK(n > 3290 && n < 3320);
    const int formats[2] = {ZR_TAPE_TZX, ZR_TAPE_PZX};
    for (int k = 0; k < 2; ++k) {
        CHECK(zr_tape_write(t, formats[k], &out, &size, NULL, error, sizeof error));
        zr_tape *rt = open_bytes(out, size, k ? "r.pzx" : "r.tzx", error);
        free(out);
        CHECK(rt != NULL);
        if (!rt) { printf("  %s\n", error); continue; }
        zr_tape_deck rd;
        zr_tape_deck_init(&rd, 14000000u);
        zr_tape_deck_set_tape(&rd, rt);
        const size_t m = play_edges(&rd, 0, edges2, 16384);
        CHECK(m == n && !memcmp(edges, edges2, n * sizeof *edges));
        if (!k) CHECK(zr_tape_block_data(rt, 0, &bytes, &length, &flag, &pause) && length == 4 && pause == 100);
        zr_tape_deck_release(&rd);
        zr_tape_close(rt);
    }

    /* A saved state of the deck in the middle of the tape goes on to the same edges. */
    zr_tape_deck a, b;
    zr_tape_deck_init(&a, 14000000u);
    zr_tape_deck_set_tape(&a, t);
    zr_tape_deck_play(&a, 0);
    drain(&a, edges3, 500);
    zr_tape_deck_level(&a, edges3[499] + 7);
    zr_state_writer w;
    zr_sw_init(&w);
    const uint8_t hash[32] = {0};
    zr_sw_header(&w, "t", "t", hash, 1);
    zr_sw_begin(&w, "TAPE", 0);
    zr_tape_deck_save(&a, &w);
    zr_sw_end(&w);
    CHECK(zr_sw_finish(&w));
    const size_t rest = drain(&a, edges, 16384);
    zr_tape_deck_init(&b, 14000000u);
    zr_tape_deck_set_tape(&b, t);
    zr_state_reader r;
    CHECK(zr_sr_open(&r, w.data, w.size, error, sizeof error) && zr_sr_chunk(&r, "TAPE", 0) && zr_tape_deck_load(&b, &r));
    const size_t rest2 = drain(&b, edges2, 16384);
    CHECK(rest > 2000 && rest == rest2 && !memcmp(edges, edges2, rest * sizeof *edges) && zr_tape_deck_playing(&b));
    zr_sw_free(&w);
    zr_tape_deck_release(&a);
    zr_tape_deck_release(&b);
    zr_tape_deck_release(&d);
    zr_tape_close(t);
}

int main(void) {
    test_inflate();
    test_tap();
    test_tzx();
    test_pzx();
    test_csw_wav();
    test_record();
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
