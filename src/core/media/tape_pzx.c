/* The PZX format, version 1.0: a file is blocks, each a four-letter tag, a
 * 32-bit length and that many bytes, the first block PZXT (the version and
 * the tape's title). PULS is a sequence of pulses with repeat counts, DATA
 * a stream of bits with the pulses of a 0 and a 1 bit and an initial level,
 * PAUS one pulse of a given level and length, BRWS a text for browsing and
 * STOP a stop (or a stop on a 48K machine). Pulse lengths are T-states of
 * 3.5 MHz; a block of a tag this reader does not know is skipped, as the
 * format says. ZetRo's own code, written from the PZX format description. */
#include "tape_impl.h"

#include <string.h>

/* A text of the block's bytes: up to the first zero byte. */
static size_t text_length(const uint8_t *p, size_t n) {
    size_t k = 0;
    while (k < n && p[k]) ++k;
    return k;
}

static int add_text(zr_tape *t, const uint8_t *p, size_t n, char *error, size_t capacity) {
    const size_t len = text_length(p, n);
    if (!len) return 1;
    size_t off;
    if (!zr_tape_put(t, p, len, &off)) return ZR_TAPE_FAIL("The tape is too large");
    zr_tape_block *b = zr_tape_add_block(t, ZR_TB_TEXT);
    if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
    b->off = off;
    b->len = len;
    return 1;
}

/* PULS: [count] duration [extended duration], repeated to the end of the block. */
static int read_puls(zr_tape *t, const uint8_t *p, size_t n, unsigned index, char *error, size_t capacity) {
    const uint32_t first = (uint32_t)t->runs_n;
    size_t at = 0;
    while (at < n) {
        if (n - at < 2) return ZR_TAPE_FAIL("Damaged PZX file: block %u (PULS) is cut short", index);
        uint32_t v = zr_rd16(p + at);
        at += 2;
        uint32_t count = 1;
        if (v > 0x8000u) {
            count = v & 0x7FFFu;
            if (n - at < 2) return ZR_TAPE_FAIL("Damaged PZX file: block %u (PULS) is cut short", index);
            v = zr_rd16(p + at);
            at += 2;
        }
        uint32_t duration = v;
        if (v >= 0x8000u) {
            if (n - at < 2) return ZR_TAPE_FAIL("Damaged PZX file: block %u (PULS) is cut short", index);
            duration = (v & 0x7FFFu) << 16 | zr_rd16(p + at);
            at += 2;
        }
        if (!zr_tape_run(t, first, duration, count)) return ZR_TAPE_FAIL("The tape has too many pulses");
    }
    zr_tape_block *b = zr_tape_add_block(t, ZR_TB_PULSES);
    if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
    b->u.r.first = first;
    b->u.r.count = (uint32_t)(t->runs_n - first);
    return 1;
}

/* DATA: count (bits 0-30) and initial level (bit 31), tail, the number of pulses of a 0 and of a 1,
 * those pulses, then the bytes. */
static int read_data(zr_tape *t, const uint8_t *p, size_t n, unsigned index, char *error, size_t capacity) {
    if (n < 8) return ZR_TAPE_FAIL("Damaged PZX file: block %u (DATA) is cut short", index);
    const uint32_t head = zr_rd32(p);
    const uint64_t bits = head & 0x7FFFFFFFu;
    const unsigned p0 = p[6], p1 = p[7];
    const size_t table = 8, bytes_at = table + 2 * ((size_t)p0 + p1);
    const uint64_t bytes = (bits + 7) / 8;
    if (bytes_at > n || bytes > n - bytes_at) return ZR_TAPE_FAIL("Damaged PZX file: block %u (DATA) is shorter than it says", index);
    size_t table_off, data_off;
    if (!zr_tape_put(t, p + table, bytes_at - table, &table_off) || !zr_tape_put(t, p + bytes_at, (size_t)bytes, &data_off))
        return ZR_TAPE_FAIL("The tape is too large");
    zr_tape_block *b = zr_tape_add_block(t, ZR_TB_DATA);
    if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
    b->flags = ZR_TF_LEVEL;
    b->level = head >> 31 & 1u;
    b->used = bits % 8 ? (uint8_t)(bits % 8) : 8;
    b->off = data_off;
    b->len = (size_t)bytes;
    b->u.d.tail = zr_rd16(p + 4);
    b->u.d.zero_n = (uint16_t)p0;
    b->u.d.one_n = (uint16_t)p1;
    b->u.d.table = table_off;
    return 1;
}

int zr_tape_read_pzx(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity) {
    size_t pos = 0;
    unsigned index = 0;
    while (pos < n) {
        ++index;
        if (n - pos < 8) return ZR_TAPE_FAIL("Damaged PZX file: block %u is cut short", index);
        const uint8_t *tag = d + pos;
        const size_t size = zr_rd32(tag + 4);
        const uint8_t *p = tag + 8;
        pos += 8;
        if (size > n - pos) return ZR_TAPE_FAIL("Damaged PZX file: block %u runs past the end of the file", index);
        pos += size;
        if (index == 1) {
            if (memcmp(tag, "PZXT", 4) || size < 2) return ZR_TAPE_FAIL("Damaged PZX file: it does not start with its header block");
            if (p[0] != 1) return ZR_TAPE_FAIL("Unsupported PZX version %u.%u", p[0], p[1]);
            if (!add_text(t, p + 2, size - 2, error, capacity)) return 0;
        } else if (!memcmp(tag, "PULS", 4)) {
            if (!read_puls(t, p, size, index, error, capacity)) return 0;
        } else if (!memcmp(tag, "DATA", 4)) {
            if (!read_data(t, p, size, index, error, capacity)) return 0;
        } else if (!memcmp(tag, "PAUS", 4)) {
            if (size < 4) return ZR_TAPE_FAIL("Damaged PZX file: block %u (PAUS) is cut short", index);
            const uint32_t v = zr_rd32(p);
            const uint32_t first = (uint32_t)t->runs_n;
            if (!zr_tape_run(t, first, v & 0x7FFFFFFFu, 1)) return ZR_TAPE_FAIL("The tape has too many pulses");
            zr_tape_block *b = zr_tape_add_block(t, ZR_TB_PULSES);
            if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
            b->flags = ZR_TF_LEVEL | ZR_TF_PAUSE;
            b->level = v >> 31 & 1u;
            b->u.r.first = first;
            b->u.r.count = 1;
        } else if (!memcmp(tag, "BRWS", 4)) {
            if (!add_text(t, p, size, error, capacity)) return 0;
        } else if (!memcmp(tag, "STOP", 4)) {
            zr_tape_block *b = zr_tape_add_block(t, size >= 2 && zr_rd16(p) == 1 ? ZR_TB_STOP48 : ZR_TB_STOP);
            if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
        }
        /* any other tag is skipped */
    }
    return 1;
}
