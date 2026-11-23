/* The CSW format (Compressed Square Wave), versions 1.01 and 2.00: a header
 * and the lengths of the pulses of a square wave in samples at the stated
 * rate, one byte each (a zero byte is followed by the length as 32 bits),
 * for version 2 optionally compressed with zlib (Z-RLE). The pulses become
 * runs in T-states of 3.5 MHz, the remainder of every conversion carried to
 * the next pulse so that the whole is exact. ZetRo's own code, written from
 * the CSW specification. */
#include "tape_impl.h"
#include "inflate.h"

#include <stdlib.h>
#include <string.h>

int zr_tape_csw_runs(zr_tape *t, const uint8_t *rle, size_t n, uint32_t rate, uint32_t *first, uint32_t *count,
                     char *error, size_t capacity) {
    if (!rate) return ZR_TAPE_FAIL("The sample rate is zero");
    const uint32_t start = (uint32_t)t->runs_n;
    uint64_t carry = 0;
    size_t i = 0;
    while (i < n) {
        uint64_t samples = rle[i++];
        if (samples == 0) {
            if (n - i < 4) { t->runs_n = start; return ZR_TAPE_FAIL("The pulse data is cut short"); }
            samples = zr_rd32(rle + i);
            i += 4;
        }
        const uint64_t v = carry + samples * ZR_TAPE_TSTATE_HZ;
        carry = v % rate;
        const uint64_t tstates = v / rate;
        if (!zr_tape_run(t, start, tstates > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)tstates, 1)) {
            t->runs_n = start;
            return ZR_TAPE_FAIL("The tape has too many pulses");
        }
    }
    *first = start;
    *count = (uint32_t)(t->runs_n - start);
    return 1;
}

int zr_tape_read_csw(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity) {
    /* The signature and its end mark (23 bytes) were checked; the revision follows. */
    if (n < 0x20) return ZR_TAPE_FAIL("Damaged CSW file: the header is cut short");
    const unsigned major = d[0x17];
    uint32_t rate, compression, flags;
    size_t data;
    if (major == 1) {
        rate = zr_rd16(d + 0x19);
        compression = d[0x1B];
        flags = d[0x1C];
        data = 0x20;
    } else if (major == 2) {
        if (n < 0x34) return ZR_TAPE_FAIL("Damaged CSW file: the header is cut short");
        rate = zr_rd32(d + 0x19);
        compression = d[0x21];
        flags = d[0x22];
        data = 0x34 + (size_t)d[0x23];
        if (data > n) return ZR_TAPE_FAIL("Damaged CSW file: the header is cut short");
    } else {
        return ZR_TAPE_FAIL("Unsupported CSW version %u.%u", major, d[0x18]);
    }
    if (!rate) return ZR_TAPE_FAIL("Damaged CSW file: the sample rate is zero");
    const uint8_t *rle = d + data;
    size_t rle_length = n - data;
    uint8_t *inflated = NULL;
    if (compression == 2 && major == 2) {
        char why[160];
        if (!zr_inflate_zlib(rle, rle_length, ZR_MEDIA_LIMIT, &inflated, &rle_length, why, sizeof why))
            return ZR_TAPE_FAIL("Damaged CSW file: %s", why);
        rle = inflated;
    } else if (compression != 1) {
        return ZR_TAPE_FAIL("Unsupported CSW compression %u", compression);
    }
    uint32_t first = 0, count = 0;
    char why[160];
    const int ok = zr_tape_csw_runs(t, rle, rle_length, rate, &first, &count, why, sizeof why);
    free(inflated);
    if (!ok) return ZR_TAPE_FAIL("Damaged CSW file: %s", why);
    zr_tape_block *b = zr_tape_add_block(t, ZR_TB_AUDIO);
    if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
    b->flags = ZR_TF_LEVEL;
    b->level = flags & 1u;
    b->u.r.first = first;
    b->u.r.count = count;
    return 1;
}
