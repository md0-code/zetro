/* The TAP format: a two-byte length (little-endian) and that many bytes, one
 * after the other, each block what the ROM's tape routines write (a flag
 * byte, the data, a parity byte). A TAP has no pauses of its own; each block
 * is played at the ROM's speed with one second of silence after it. ZetRo's
 * own code. */
#include "tape_impl.h"

int zr_tape_read_tap(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity) {
    size_t pos = 0;
    unsigned index = 0;
    while (pos < n) {
        ++index;
        if (n - pos < 2) return ZR_TAPE_FAIL("Damaged TAP file: block %u is cut short", index);
        const size_t len = zr_rd16(d + pos);
        pos += 2;
        if (n - pos < len) return ZR_TAPE_FAIL("Damaged TAP file: block %u runs past the end of the file", index);
        size_t off;
        if (!zr_tape_put(t, d + pos, len, &off)) return ZR_TAPE_FAIL("The tape is too large");
        zr_tape_block *b = zr_tape_add_block(t, ZR_TB_STANDARD);
        if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
        if (!zr_tape_fill_standard(t, b, off, len, 1000)) return ZR_TAPE_FAIL("Out of memory");
        pos += len;
    }
    return 1;
}
