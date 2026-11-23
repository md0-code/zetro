/* The SNA format: a 27-byte header (I, the alternate HL, DE, BC and AF, HL, DE, BC, IY, IX, bit 2 of a byte as IFF2, R,
 * AF, SP, the interrupt mode, the border) and the RAM. The 48K file (49179 bytes) holds 48 KiB from 4000h and has no PC:
 * the program counter is on the stack, as if an interrupt had pushed it, and a RETN starts the program. The 128K file
 * holds the pages 5 and 2, the page that is paged in at C000h, PC, the value of port 7FFD, a TR-DOS flag, and then the
 * other pages in ascending order (131103 bytes, or 147487 when the paged page is 5 or 2 and so is in the file twice).
 * ZetRo's own code, written from the format description. */
#include "snap_impl.h"

#define HEADER 27u
#define SIZE_48K 49179u
#define SIZE_128K 131103u
#define SIZE_128K_DUPLICATE 147487u
#define EXTRA_128K 4u                           /* PC, port 7FFD, the TR-DOS flag */

/* The pages of the 128K file after the three first ones: not 5, not 2, not the paged one, ascending. */
static unsigned rest_pages(unsigned paged, unsigned out[ZR_SNAP_PAGES]) {
    unsigned n = 0;
    for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page)
        if (page != 5 && page != 2 && page != paged) out[n++] = page;
    return n;
}

int zr_snap_read_sna(const uint8_t *d, size_t n, zr_snap *s, char *error, size_t capacity) {
    if (n != SIZE_48K && n != SIZE_128K && n != SIZE_128K_DUPLICATE)
        return zr_snap_fail(error, capacity, "Not an SNA file: %zu bytes is not the size of a 48K (%u) or 128K (%u, %u) one", n, SIZE_48K,
                            SIZE_128K, SIZE_128K_DUPLICATE);
    s->i = d[0];
    s->hl2 = (uint16_t)zr_snap_rd16(d + 1);
    s->de2 = (uint16_t)zr_snap_rd16(d + 3);
    s->bc2 = (uint16_t)zr_snap_rd16(d + 5);
    s->af2 = (uint16_t)zr_snap_rd16(d + 7);
    s->hl = (uint16_t)zr_snap_rd16(d + 9);
    s->de = (uint16_t)zr_snap_rd16(d + 11);
    s->bc = (uint16_t)zr_snap_rd16(d + 13);
    s->iy = (uint16_t)zr_snap_rd16(d + 15);
    s->ix = (uint16_t)zr_snap_rd16(d + 17);
    s->iff2 = d[19] >> 2 & 1u;
    s->iff1 = s->iff2;                          /* the RETN that starts the program copies IFF2 to IFF1 */
    s->r = d[20];
    s->af = (uint16_t)zr_snap_rd16(d + 21);
    s->sp = (uint16_t)zr_snap_rd16(d + 23);
    s->im = d[25];
    if (s->im > 2) return zr_snap_fail(error, capacity, "Damaged SNA file: interrupt mode %u", s->im);
    s->border = d[26] & 7u;
    if (n == SIZE_48K) {
        zr_snap_set_machine(s, ZR_SNAP_M_48K, 0, "ZX Spectrum 48K");
        const unsigned order[3] = {5, 2, 0};
        for (unsigned k = 0; k < 3; ++k) {
            memcpy(s->ram[order[k]], d + HEADER + (size_t)k * ZR_SNAP_PAGE_SIZE, ZR_SNAP_PAGE_SIZE);
            s->pages |= (uint8_t)(1u << order[k]);
        }
        /* PC is the word at the stack pointer; the stack must be in RAM for it to be in the file. */
        if (s->sp < 0x4000u || s->sp > 0xFFFEu)
            return zr_snap_fail(error, capacity, "Damaged SNA file: the stack pointer %04X is not in RAM, where this format keeps PC", s->sp);
        const unsigned at = (unsigned)s->sp - 0x4000u;
        const uint8_t *ram = d + HEADER;
        s->pc = (uint16_t)((unsigned)ram[at] | (unsigned)ram[at + 1] << 8);
        s->sp = (uint16_t)(s->sp + 2u);
        return 1;
    }
    /* the 128K file */
    zr_snap_set_machine(s, ZR_SNAP_M_128K, 0, "ZX Spectrum 128K");
    s->pc = (uint16_t)zr_snap_rd16(d + HEADER + 3u * ZR_SNAP_PAGE_SIZE);
    s->port_7ffd = d[HEADER + 3u * ZR_SNAP_PAGE_SIZE + 2];
    if (d[HEADER + 3u * ZR_SNAP_PAGE_SIZE + 3]) s->extras |= ZR_SNAP_X_ROM_PAGED;      /* TR-DOS's ROM */
    s->ay_present = 1;
    const unsigned paged = s->port_7ffd & 7u;
    unsigned rest[ZR_SNAP_PAGES];
    const unsigned count = rest_pages(paged, rest);
    const size_t expect = (size_t)HEADER + (3u + (size_t)count) * ZR_SNAP_PAGE_SIZE + EXTRA_128K;
    if (n != expect) return zr_snap_fail(error, capacity, "Damaged SNA file: with page %u paged in it should be %zu bytes, not %zu", paged, expect, n);
    memcpy(s->ram[5], d + HEADER, ZR_SNAP_PAGE_SIZE);
    memcpy(s->ram[2], d + HEADER + ZR_SNAP_PAGE_SIZE, ZR_SNAP_PAGE_SIZE);
    if (paged != 5 && paged != 2) memcpy(s->ram[paged], d + HEADER + 2u * ZR_SNAP_PAGE_SIZE, ZR_SNAP_PAGE_SIZE);
    s->pages |= (uint8_t)(1u << 5 | 1u << 2 | 1u << paged);
    const uint8_t *at = d + HEADER + 3u * ZR_SNAP_PAGE_SIZE + EXTRA_128K;
    for (unsigned k = 0; k < count; ++k, at += ZR_SNAP_PAGE_SIZE) {
        memcpy(s->ram[rest[k]], at, ZR_SNAP_PAGE_SIZE);
        s->pages |= (uint8_t)(1u << rest[k]);
    }
    return 1;
}

int zr_snap_write_sna(const zr_snap *s, uint8_t **bytes, size_t *size, char *error, size_t capacity) {
    const int is48 = s->machine == ZR_SNAP_M_48K || s->machine == ZR_SNAP_M_16K;
    /* A +2A or +3 with its special mode off and ROM 0 or 1 (1FFD's bits 0-2 clear) is a 128K's memory and ROM: its SNA is the 128K's. */
    const int plus3_as_128 = (s->machine == ZR_SNAP_M_PLUS2A || s->machine == ZR_SNAP_M_PLUS3) && !(s->port_1ffd & 7u);
    const int is128 = s->machine == ZR_SNAP_M_128K || s->machine == ZR_SNAP_M_PLUS2 || plus3_as_128;
    if (!is48 && !is128) {
        if (s->machine == ZR_SNAP_M_PLUS2A || s->machine == ZR_SNAP_M_PLUS3)
            return zr_snap_fail(error, capacity, "An SNA file cannot hold a %s (port 1FFD): save it as .z80 or .szx", s->name);
        return zr_snap_fail(error, capacity, "An SNA file cannot hold a %s: save it as .z80 or .szx", s->name);
    }
    unsigned paged = s->port_7ffd & 7u, rest[ZR_SNAP_PAGES], count = 0;
    size_t total = SIZE_48K;
    if (is128) {
        count = rest_pages(paged, rest);
        total = (size_t)HEADER + (3u + (size_t)count) * ZR_SNAP_PAGE_SIZE + EXTRA_128K;
    }
    uint8_t *out = calloc(1, total);
    if (!out) return zr_snap_fail(error, capacity, "Out of memory");
    uint16_t sp = s->sp;
    const uint16_t pc = zr_snap_pc_for_file(s);
    out[0] = s->i;
    zr_snap_put16(out + 1, s->hl2);
    zr_snap_put16(out + 3, s->de2);
    zr_snap_put16(out + 5, s->bc2);
    zr_snap_put16(out + 7, s->af2);
    zr_snap_put16(out + 9, s->hl);
    zr_snap_put16(out + 11, s->de);
    zr_snap_put16(out + 13, s->bc);
    zr_snap_put16(out + 15, s->iy);
    zr_snap_put16(out + 17, s->ix);
    out[19] = (uint8_t)(s->iff2 ? 4 : 0);
    out[20] = s->r;
    zr_snap_put16(out + 21, s->af);
    out[25] = s->im;
    out[26] = s->border & 7u;
    if (is48) {
        /* PC goes onto the stack of the copy, not of the machine; the pages 5, 2 and 0 follow the header in this order. */
        const unsigned order[3] = {5, 2, 0};
        for (unsigned k = 0; k < 3; ++k)
            if (s->pages >> order[k] & 1u) memcpy(out + HEADER + (size_t)k * ZR_SNAP_PAGE_SIZE, s->ram[order[k]], ZR_SNAP_PAGE_SIZE);
        const uint16_t low = (uint16_t)(sp - 2u), high = (uint16_t)(sp - 1u);
        if (low < 0x4000u || high < 0x4000u) {
            free(out);
            return zr_snap_fail(error, capacity, "An SNA file keeps PC on the stack, and the stack pointer %04X leaves no room for it in RAM: save it as .z80 or .szx", sp);
        }
        out[HEADER + low - 0x4000u] = (uint8_t)pc;
        out[HEADER + high - 0x4000u] = (uint8_t)(pc >> 8);
        sp = low;
        zr_snap_put16(out + 23, sp);
    } else {
        zr_snap_put16(out + 23, sp);
        memcpy(out + HEADER, s->ram[5], ZR_SNAP_PAGE_SIZE);
        memcpy(out + HEADER + ZR_SNAP_PAGE_SIZE, s->ram[2], ZR_SNAP_PAGE_SIZE);
        memcpy(out + HEADER + 2u * ZR_SNAP_PAGE_SIZE, s->ram[paged], ZR_SNAP_PAGE_SIZE);
        zr_snap_put16(out + HEADER + 3u * ZR_SNAP_PAGE_SIZE, pc);
        out[HEADER + 3u * ZR_SNAP_PAGE_SIZE + 2] = s->port_7ffd;
        out[HEADER + 3u * ZR_SNAP_PAGE_SIZE + 3] = 0;                       /* TR-DOS's ROM is not paged */
        uint8_t *at = out + HEADER + 3u * ZR_SNAP_PAGE_SIZE + EXTRA_128K;
        for (unsigned k = 0; k < count; ++k, at += ZR_SNAP_PAGE_SIZE) memcpy(at, s->ram[rest[k]], ZR_SNAP_PAGE_SIZE);
    }
    *bytes = out;
    *size = total;
    return 1;
}
