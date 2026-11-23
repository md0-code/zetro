/* The SZX ("zx-state") format: the magic ZXST, a major and a minor version, a machine number and a flags byte, then
 * blocks, each a four-letter tag, a 32-bit size of its data and the data, all values little-endian. Z80R holds the
 * processor (37 bytes), SPCR the border and the paging ports, RAMP a 16 KiB page by its number with a flag that says
 * it is a zlib stream, AY the sound chip's registers; every other block (KEYB, JOY, CRTR, the interfaces and the rest)
 * is skipped by its size, as the format intends. The writer stores the pages uncompressed, which the flag allows.
 * ZetRo's own code, written from the format description. */
#include "snap_impl.h"
#include "inflate.h"

#define Z80R_SIZE 37u
#define SPCR_SIZE 8u
#define AY_SIZE 18u
#define MAX_TSTATES 100000u

static void machine_of(zr_snap *s, unsigned id) {
    char name[40];
    switch (id) {
    case 0: zr_snap_set_machine(s, ZR_SNAP_M_16K, id, "ZX Spectrum 16K"); return;
    case 1: zr_snap_set_machine(s, ZR_SNAP_M_48K, id, "ZX Spectrum 48K"); return;
    case 2: zr_snap_set_machine(s, ZR_SNAP_M_128K, id, "ZX Spectrum 128K"); return;
    case 3: zr_snap_set_machine(s, ZR_SNAP_M_PLUS2, id, "ZX Spectrum +2"); return;
    case 4: zr_snap_set_machine(s, ZR_SNAP_M_PLUS2A, id, "ZX Spectrum +2A"); return;
    case 5: zr_snap_set_machine(s, ZR_SNAP_M_PLUS3, id, "ZX Spectrum +3"); return;
    case 6: zr_snap_set_machine(s, ZR_SNAP_M_PLUS3, id, "ZX Spectrum +3e"); return;
    case 7: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Pentagon 128"); return;
    case 8: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Timex TC2048"); return;
    case 9: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Timex TC2068"); return;
    case 10: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Scorpion ZS-256"); return;
    case 11: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "ZX Spectrum SE"); return;
    case 12: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Timex TS2068"); return;
    case 13: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Pentagon 512"); return;
    case 14: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "Pentagon 1024"); return;
    case 15: zr_snap_set_machine(s, ZR_SNAP_M_48K, id, "ZX Spectrum 48K (NTSC)"); return;
    case 16: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, "ZX Spectrum 128Ke"); return;
    default: break;
    }
    snprintf(name, sizeof name, "SZX machine %u", id);
    zr_snap_set_machine(s, ZR_SNAP_M_OTHER, id, name);
}

int zr_snap_read_szx(const uint8_t *d, size_t n, zr_snap *s, char *error, size_t capacity) {
    if (n < 8 || memcmp(d, "ZXST", 4)) return zr_snap_fail(error, capacity, "Not an SZX file: it does not start with ZXST");
    if (d[4] != 1) return zr_snap_fail(error, capacity, "Unsupported SZX version %u.%u", d[4], d[5]);
    const unsigned minor = d[5];
    machine_of(s, d[6]);
    int have_regs = 0;
    size_t pos = 8;
    while (pos < n) {
        if (n - pos < 8) return zr_snap_fail(error, capacity, "Damaged SZX file: a block header is cut short");
        const uint8_t *tag = d + pos;
        const size_t size = zr_snap_rd32(tag + 4);
        pos += 8;
        if (size > n - pos) return zr_snap_fail(error, capacity, "Damaged SZX file: block %.4s runs past the end of the file", (const char *)tag);
        const uint8_t *p = d + pos;
        pos += size;
        if (!memcmp(tag, "Z80R", 4)) {
            if (size < Z80R_SIZE) return zr_snap_fail(error, capacity, "Damaged SZX file: the processor block (Z80R) is cut short");
            s->af = (uint16_t)zr_snap_rd16(p);
            s->bc = (uint16_t)zr_snap_rd16(p + 2);
            s->de = (uint16_t)zr_snap_rd16(p + 4);
            s->hl = (uint16_t)zr_snap_rd16(p + 6);
            s->af2 = (uint16_t)zr_snap_rd16(p + 8);
            s->bc2 = (uint16_t)zr_snap_rd16(p + 10);
            s->de2 = (uint16_t)zr_snap_rd16(p + 12);
            s->hl2 = (uint16_t)zr_snap_rd16(p + 14);
            s->ix = (uint16_t)zr_snap_rd16(p + 16);
            s->iy = (uint16_t)zr_snap_rd16(p + 18);
            s->sp = (uint16_t)zr_snap_rd16(p + 20);
            s->pc = (uint16_t)zr_snap_rd16(p + 22);
            s->i = p[24];
            s->r = p[25];
            s->iff1 = p[26] != 0;
            s->iff2 = p[27] != 0;
            s->im = p[28];
            if (s->im > 2) return zr_snap_fail(error, capacity, "Damaged SZX file: interrupt mode %u", s->im);
            const uint32_t cycles = zr_snap_rd32(p + 29);
            if (cycles < MAX_TSTATES) {
                s->has_tstates = 1;
                s->tstates = cycles;
            }
            s->ei = p[34] & 1u;
            s->halted = p[34] >> 1 & 1u;
            if (minor >= 4) {
                s->wz = (uint16_t)zr_snap_rd16(p + 35);
                s->has_wz = 1;
            }
            have_regs = 1;
        } else if (!memcmp(tag, "SPCR", 4)) {
            if (size < 4) return zr_snap_fail(error, capacity, "Damaged SZX file: the Spectrum block (SPCR) is cut short");
            s->border = p[0] & 7u;
            s->port_7ffd = p[1];
            s->port_1ffd = p[2];
            s->port_fe = p[3];
        } else if (!memcmp(tag, "AY\0\0", 4)) {
            if (size < AY_SIZE) return zr_snap_fail(error, capacity, "Damaged SZX file: the sound chip block (AY) is cut short");
            s->ay_present = 1;
            s->ay_current = p[1] & 15u;
            memcpy(s->ay, p + 2, 16);
        } else if (!memcmp(tag, "RAMP", 4)) {
            if (size < 3) return zr_snap_fail(error, capacity, "Damaged SZX file: a RAM page block (RAMP) is cut short");
            const unsigned flags = zr_snap_rd16(p), page = p[2];
            const uint8_t *data = p + 3;
            const size_t length = size - 3;
            if (page >= ZR_SNAP_PAGES) continue;                    /* a page of a machine with more RAM than the 128K */
            if (flags & 1u) {
                uint8_t *out = NULL;
                size_t out_length = 0;
                char why[128];
                if (!zr_inflate_zlib(data, length, ZR_SNAP_PAGE_SIZE, &out, &out_length, why, sizeof why))
                    return zr_snap_fail(error, capacity, "Damaged SZX file: RAM page %u: %s", page, why);
                if (out_length != ZR_SNAP_PAGE_SIZE) {
                    free(out);
                    return zr_snap_fail(error, capacity, "Damaged SZX file: RAM page %u is %zu bytes, not 16 KiB", page, out_length);
                }
                memcpy(s->ram[page], out, ZR_SNAP_PAGE_SIZE);
                free(out);
            } else {
                if (length != ZR_SNAP_PAGE_SIZE) return zr_snap_fail(error, capacity, "Damaged SZX file: RAM page %u is %zu bytes, not 16 KiB", page, length);
                memcpy(s->ram[page], data, ZR_SNAP_PAGE_SIZE);
            }
            s->pages |= (uint8_t)(1u << page);
        }
    }
    if (!have_regs) return zr_snap_fail(error, capacity, "Damaged SZX file: it has no processor block (Z80R)");
    return 1;
}

static uint8_t *block(uint8_t *at, const char *tag, size_t size) {
    memcpy(at, tag, 4);
    zr_snap_put32(at + 4, (uint32_t)size);
    return at + 8;
}

int zr_snap_write_szx(const zr_snap *s, uint8_t **bytes, size_t *size, char *error, size_t capacity) {
    unsigned id;
    switch (s->machine) {
    case ZR_SNAP_M_16K: id = 0; break;
    case ZR_SNAP_M_48K: id = 1; break;
    case ZR_SNAP_M_128K: id = 2; break;
    case ZR_SNAP_M_PLUS2: id = 3; break;
    case ZR_SNAP_M_PLUS2A: id = 4; break;
    case ZR_SNAP_M_PLUS3: id = 5; break;
    default:
        if (s->format != ZR_SNAP_SZX) return zr_snap_fail(error, capacity, "The SZX format has no number for a %s", s->name);
        id = s->id;
        break;
    }
    unsigned pages = 0;
    for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page) pages += s->pages >> page & 1u;
    const size_t total = 8u + (8u + Z80R_SIZE) + (8u + SPCR_SIZE) + (s->ay_present ? 8u + AY_SIZE : 0u) +
                         (size_t)pages * (8u + 3u + ZR_SNAP_PAGE_SIZE);
    uint8_t *out = calloc(1, total), *at = out;
    if (!out) return zr_snap_fail(error, capacity, "Out of memory");
    memcpy(at, "ZXST", 4);
    at[4] = 1;
    at[5] = 5;
    at[6] = (uint8_t)id;
    at += 8;
    /* Z80R */
    uint8_t *p = block(at, "Z80R", Z80R_SIZE);
    zr_snap_put16(p, s->af);
    zr_snap_put16(p + 2, s->bc);
    zr_snap_put16(p + 4, s->de);
    zr_snap_put16(p + 6, s->hl);
    zr_snap_put16(p + 8, s->af2);
    zr_snap_put16(p + 10, s->bc2);
    zr_snap_put16(p + 12, s->de2);
    zr_snap_put16(p + 14, s->hl2);
    zr_snap_put16(p + 16, s->ix);
    zr_snap_put16(p + 18, s->iy);
    zr_snap_put16(p + 20, s->sp);
    zr_snap_put16(p + 22, s->pc);
    p[24] = s->i;
    p[25] = s->r;
    p[26] = s->iff1 ? 1 : 0;
    p[27] = s->iff2 ? 1 : 0;
    p[28] = s->im;
    zr_snap_put32(p + 29, s->has_tstates ? s->tstates : 0u);
    p[34] = (uint8_t)((s->ei ? 1u : 0u) | (s->halted ? 2u : 0u));
    zr_snap_put16(p + 35, s->has_wz ? s->wz : 0u);
    at = p + Z80R_SIZE;
    /* SPCR */
    p = block(at, "SPCR", SPCR_SIZE);
    p[0] = s->border & 7u;
    p[1] = s->port_7ffd;
    p[2] = s->port_1ffd;
    p[3] = s->port_fe;
    at = p + SPCR_SIZE;
    if (s->ay_present) {
        p = block(at, "AY\0\0", AY_SIZE);
        p[0] = 0;
        p[1] = s->ay_current & 15u;
        memcpy(p + 2, s->ay, 16);
        at = p + AY_SIZE;
    }
    for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page) {
        if (!(s->pages >> page & 1u)) continue;
        p = block(at, "RAMP", 3u + ZR_SNAP_PAGE_SIZE);
        zr_snap_put16(p, 0);                                        /* not compressed */
        p[2] = (uint8_t)page;
        memcpy(p + 3, s->ram[page], ZR_SNAP_PAGE_SIZE);
        at = p + 3u + ZR_SNAP_PAGE_SIZE;
    }
    *bytes = out;
    *size = (size_t)(at - out);
    return 1;
}
