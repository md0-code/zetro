/* The TZX format, revision 1.20: the header "ZXTape!", 0x1A and the version,
 * then blocks, each an ID byte and a body. Every block type of 1.20 is
 * understood; a block it does not know (and the deprecated ones) is kept and
 * skipped by the extension rule, a DWORD length after the ID, which is how
 * every block added after 1.10 is laid out. Each block is kept as it is in
 * the file (the arena's `raw`), so that a tape read from a TZX and written
 * back as one loses nothing. ZetRo's own code, written from the TZX 1.20
 * specification. */
#include "tape_impl.h"
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

/* The length of a block's body, the bytes after its ID, as the block's own header says. 0 when
 * the header itself is cut short. */
static int body_length(unsigned id, const uint8_t *p, size_t avail, uint64_t *body) {
    size_t need = 0;
    switch (id) {
    case 0x10: need = 4; break;
    case 0x11: need = 18; break;
    case 0x12: need = 4; break;
    case 0x13: need = 1; break;
    case 0x14: need = 10; break;
    case 0x15: need = 8; break;
    case 0x20: case 0x23: case 0x24: need = 2; break;
    case 0x21: case 0x30: case 0x33: need = 1; break;
    case 0x22: case 0x25: case 0x27: break;
    case 0x26: case 0x28: case 0x31: case 0x32: need = 2; break;
    case 0x2A: need = 4; break;
    case 0x34: need = 8; break;
    case 0x35: need = 14; break;
    case 0x40: need = 4; break;
    case 0x5A: need = 9; break;
    default: need = 4; break;                                   /* 18, 19, 2B and the unknown: a DWORD length */
    }
    if (avail < need) return 0;
    switch (id) {
    case 0x10: *body = 4 + (uint64_t)zr_rd16(p + 2); break;
    case 0x11: *body = 18 + (uint64_t)zr_rd24(p + 15); break;
    case 0x12: *body = 4; break;
    case 0x13: *body = 1 + 2 * (uint64_t)p[0]; break;
    case 0x14: *body = 10 + (uint64_t)zr_rd24(p + 7); break;
    case 0x15: *body = 8 + (uint64_t)zr_rd24(p + 5); break;
    case 0x20: case 0x23: case 0x24: *body = 2; break;
    case 0x21: case 0x30: *body = 1 + (uint64_t)p[0]; break;
    case 0x22: case 0x25: case 0x27: *body = 0; break;
    case 0x26: *body = 2 + 2 * (uint64_t)zr_rd16(p); break;
    case 0x28: case 0x32: *body = 2 + (uint64_t)zr_rd16(p); break;
    case 0x2A: *body = 4; break;
    case 0x31: *body = 2 + (uint64_t)p[1]; break;
    case 0x33: *body = 1 + 3 * (uint64_t)p[0]; break;
    case 0x34: *body = 8; break;
    case 0x35: *body = 14 + (uint64_t)zr_rd32(p + 10); break;
    case 0x40: *body = 4 + (uint64_t)zr_rd24(p + 1); break;
    case 0x5A: *body = 9; break;
    default: *body = 4 + (uint64_t)zr_rd32(p); break;
    }
    return 1;
}

static unsigned used_bits(unsigned v) { return v >= 1 && v <= 8 ? v : 8; }

/* Fills the block (already added, with its raw bytes in the arena at `raw`) from its body `p`. */
static int parse_block(zr_tape *t, zr_tape_block *b, unsigned id, const uint8_t *p, uint64_t body, size_t raw,
                       unsigned index, char *error, size_t capacity) {
    const size_t data = raw + 1;                                /* the body in the arena */
    char why[160];
    switch (id) {
    case 0x10: {
        const size_t len = zr_rd16(p + 2);
        if (!zr_tape_fill_standard(t, b, data + 4, len, zr_rd16(p))) return ZR_TAPE_FAIL("Out of memory");
        break;
    }
    case 0x11: {
        b->kind = ZR_TB_TURBO;
        b->u.d.pilot = zr_rd16(p);
        b->u.d.sync1 = zr_rd16(p + 2);
        b->u.d.sync2 = zr_rd16(p + 4);
        b->u.d.sync_n = 2;
        b->u.d.pilot_count = zr_rd16(p + 10);
        b->used = (uint8_t)used_bits(p[12]);
        b->pause = zr_rd16(p + 13);
        b->u.d.zero_n = b->u.d.one_n = 2;
        b->off = data + 18;
        b->len = zr_rd24(p + 15);
        if (!zr_tape_put_bits(t, zr_rd16(p + 6), zr_rd16(p + 8), &b->u.d.table)) return ZR_TAPE_FAIL("Out of memory");
        break;
    }
    case 0x12:
        b->kind = ZR_TB_TONE;
        b->u.d.pilot = zr_rd16(p);
        b->u.d.pilot_count = zr_rd16(p + 2);
        break;
    case 0x13: {
        const uint32_t first = (uint32_t)t->runs_n;
        for (unsigned k = 0; k < p[0]; ++k)
            if (!zr_tape_run(t, first, zr_rd16(p + 1 + 2 * (size_t)k), 1)) return ZR_TAPE_FAIL("The tape has too many pulses");
        b->kind = ZR_TB_PULSES;
        b->u.r.first = first;
        b->u.r.count = (uint32_t)(t->runs_n - first);
        break;
    }
    case 0x14:
        b->kind = ZR_TB_DATA;
        b->used = (uint8_t)used_bits(p[4]);
        b->pause = zr_rd16(p + 5);
        b->u.d.zero_n = b->u.d.one_n = 2;
        b->off = data + 10;
        b->len = zr_rd24(p + 7);
        if (!zr_tape_put_bits(t, zr_rd16(p), zr_rd16(p + 2), &b->u.d.table)) return ZR_TAPE_FAIL("Out of memory");
        break;
    case 0x15:
        b->kind = ZR_TB_DIRECT;
        b->u.direct.sample = zr_rd16(p);
        if (!b->u.direct.sample) return ZR_TAPE_FAIL("Damaged TZX file: block %u (direct recording) has no sample length", index);
        b->pause = zr_rd16(p + 2);
        b->used = (uint8_t)used_bits(p[4]);
        b->off = data + 8;
        b->len = zr_rd24(p + 5);
        break;
    case 0x18: {
        if (body < 14) return ZR_TAPE_FAIL("Damaged TZX file: block %u (CSW recording) is too short", index);
        const uint32_t rate = zr_rd24(p + 6);
        const unsigned compression = p[9];
        const uint8_t *rle = p + 14;
        size_t length = (size_t)body - 14;
        uint8_t *inflated = NULL;
        if (compression == 2) {
            if (!zr_inflate_zlib(rle, length, ZR_MEDIA_LIMIT, &inflated, &length, why, sizeof why))
                return ZR_TAPE_FAIL("Damaged TZX file: block %u (CSW recording): %s", index, why);
            rle = inflated;
        } else if (compression != 1) {
            return ZR_TAPE_FAIL("Unsupported TZX file: block %u (CSW recording) has compression %u", index, compression);
        }
        uint32_t first = 0, count = 0;
        const int ok = zr_tape_csw_runs(t, rle, length, rate, &first, &count, why, sizeof why);
        free(inflated);
        if (!ok) return ZR_TAPE_FAIL("Damaged TZX file: block %u (CSW recording): %s", index, why);
        b->kind = ZR_TB_AUDIO;
        b->flags = ZR_TF_KEEP;
        b->pause = zr_rd16(p + 4);
        b->u.r.first = first;
        b->u.r.count = count;
        break;
    }
    case 0x19: {
        if (body < 18) return ZR_TAPE_FAIL("Damaged TZX file: block %u (generalized data) is too short", index);
        const uint32_t totp = zr_rd32(p + 6), totd = zr_rd32(p + 12);
        const unsigned npp = p[10], npd = p[16];
        const unsigned asp = p[11] ? p[11] : 256u, asd = p[17] ? p[17] : 256u;
        unsigned nb = 0;
        while ((1u << nb) < asd) ++nb;
        const uint64_t sym_p = totp ? (uint64_t)asp * (1u + 2u * npp) : 0;
        const uint64_t prle = (uint64_t)totp * 3;
        const uint64_t sym_d = totd ? (uint64_t)asd * (1u + 2u * npd) : 0;
        const uint64_t stream = totd ? ((uint64_t)nb * totd + 7) / 8 : 0;
        if (18 + sym_p + prle + sym_d + stream > body)
            return ZR_TAPE_FAIL("Damaged TZX file: block %u (generalized data) is shorter than it says", index);
        for (uint64_t k = 0; k < totp; ++k)
            if (p[18 + sym_p + 3 * k] >= asp) return ZR_TAPE_FAIL("Damaged TZX file: block %u (generalized data) uses a symbol it does not define", index);
        b->kind = ZR_TB_GENERAL;
        b->pause = zr_rd16(p + 4);
        b->u.g.totp = totp;
        b->u.g.totd = totd;
        b->u.g.asp = asp;
        b->u.g.asd = asd;
        b->u.g.npp = (uint8_t)npp;
        b->u.g.npd = (uint8_t)npd;
        b->u.g.nb = (uint8_t)nb;
        b->u.g.sym_p = data + 18;
        b->u.g.prle = b->u.g.sym_p + (size_t)sym_p;
        b->u.g.sym_d = b->u.g.prle + (size_t)prle;
        b->u.g.stream = b->u.g.sym_d + (size_t)sym_d;
        break;
    }
    case 0x20:
        if (zr_rd16(p)) { b->kind = ZR_TB_PAUSE; b->pause = zr_rd16(p); }
        else b->kind = ZR_TB_STOP;
        break;
    case 0x21: b->kind = ZR_TB_GROUP; b->off = data + 1; b->len = p[0]; break;
    case 0x22: b->kind = ZR_TB_GROUP_END; break;
    case 0x23: b->kind = ZR_TB_JUMP; b->u.f.value = (int16_t)(uint16_t)zr_rd16(p); break;
    case 0x24: b->kind = ZR_TB_LOOP; b->u.f.value = (int32_t)zr_rd16(p); break;
    case 0x25: b->kind = ZR_TB_LOOP_END; break;
    case 0x26: b->kind = ZR_TB_CALL; b->u.f.value = (int32_t)zr_rd16(p); break;
    case 0x27: b->kind = ZR_TB_RETURN; break;
    case 0x28: b->kind = ZR_TB_SELECT; break;
    case 0x2A: b->kind = ZR_TB_STOP48; break;
    case 0x2B: b->kind = ZR_TB_LEVEL; b->level = body >= 5 ? p[4] & 1u : 0u; break;
    case 0x30: b->kind = ZR_TB_TEXT; b->off = data + 1; b->len = p[0]; break;
    case 0x31: b->kind = ZR_TB_TEXT; b->off = data + 2; b->len = p[1]; break;
    default:
        b->kind = ZR_TB_INFO;                       /* 32, 33, 34, 35, 40, 5A and the blocks it does not know */
        break;
    }
    return 1;
}

int zr_tape_read_tzx(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity) {
    if (n < 10) return ZR_TAPE_FAIL("Damaged TZX file: the header is cut short");
    if (d[8] != 1) return ZR_TAPE_FAIL("Unsupported TZX version %u.%u", d[8], d[9]);
    size_t pos = 10;
    unsigned index = 0;
    while (pos < n) {
        ++index;
        const unsigned id = d[pos];
        const uint8_t *p = d + pos + 1;
        const size_t avail = n - pos - 1;
        uint64_t body = 0;
        if (!body_length(id, p, avail, &body)) return ZR_TAPE_FAIL("Damaged TZX file: block %u (ID 0x%02X) is cut short", index, id);
        if (body > avail) return ZR_TAPE_FAIL("Damaged TZX file: block %u (ID 0x%02X) runs past the end of the file", index, id);
        size_t raw;
        if (!zr_tape_put(t, d + pos, 1 + (size_t)body, &raw)) return ZR_TAPE_FAIL("The tape is too large");
        zr_tape_block *b = zr_tape_add_block(t, ZR_TB_INFO);
        if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
        b->raw = raw;
        b->raw_len = 1 + (size_t)body;
        if (!parse_block(t, b, id, p, body, raw, index, error, capacity)) return 0;
        pos += 1 + (size_t)body;
    }
    return 1;
}
