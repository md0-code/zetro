/* The Z80 format, versions 1, 2 and 3. A 30-byte header holds the registers (in an order of its own, with R's bit 7 and
 * the border in a flags byte, and a bit that says the 48 KiB after it are compressed: version 1, recognised by a PC that
 * is not 0). Versions 2 and 3 have PC 0 there, then a header length (23 or 54, 55) and an extra header: the real PC, the
 * hardware mode, the last values written to the paging ports, the AY's registers, and (version 3) the T-states into the
 * frame as two counters. The memory then follows as blocks, each a length (FFFFh: 16 KiB stored as they are), a page
 * number and the page, in any order. A page is compressed by runs: five or more equal bytes, and any two or more ED
 * bytes, become ED ED count byte, and the byte after a single ED is always left as it is; version 1's block ends with
 * 00 ED ED 00. The writer makes version 3 with every page compressed. ZetRo's own code, written from the format
 * description. */
#include "snap_impl.h"

#define V1_HEADER 30u
#define PAGE 16384u
#define QUARTER_48K 17472u                      /* a quarter of 69888 T-states */
#define QUARTER_128K 17727u                     /* a quarter of 70908 */

/* The page numbers a file gives its blocks, and the RAM page each is (the 128K machine's number; -1: not RAM here). */
static int ram_page_48k(unsigned page) { return page == 8 ? 5 : page == 4 ? 2 : page == 5 ? 0 : -1; }
static int ram_page_128k(unsigned page) { return page >= 3 && page <= 10 ? (int)page - 3 : -1; }

/* --- the compression ------------------------------------------------------------------------ */

/* The bytes of a compressed block expanded into exactly `capacity`; 0 when it is damaged or comes out another size. */
static int decompress(const uint8_t *src, size_t n, uint8_t *dst, size_t capacity) {
    size_t i = 0, o = 0;
    while (i < n) {
        if (src[i] == 0xED && i + 1 < n && src[i + 1] == 0xED) {
            if (n - i < 4) return 0;
            const unsigned count = src[i + 2];
            if (!count || count > capacity - o) return 0;
            memset(dst + o, src[i + 3], count);
            o += count;
            i += 4;
        } else {
            if (o >= capacity) return 0;
            dst[o++] = src[i++];
        }
    }
    return o == capacity;
}

/* A page compressed into `out` (at least 2 * PAGE + 8 bytes); the length. */
static size_t compress(const uint8_t *src, size_t n, uint8_t *out) {
    size_t i = 0, o = 0;
    while (i < n) {
        const uint8_t b = src[i];
        size_t run = 1;
        while (i + run < n && src[i + run] == b && run < 255) ++run;
        if (run >= 5 || (b == 0xED && run >= 2)) {
            out[o++] = 0xED;
            out[o++] = 0xED;
            out[o++] = (uint8_t)run;
            out[o++] = b;
            i += run;
        } else {
            out[o++] = b;
            ++i;
            if (b == 0xED && i < n) out[o++] = src[i++];        /* the byte after a single ED stays out of any run */
        }
    }
    return o;
}

/* --- reading ----------------------------------------------------------------------------------- */

static void machine_of(zr_snap *s, unsigned hw, int v3, int modify) {
    char name[40];
    switch (hw) {
    case 0: zr_snap_set_machine(s, modify ? ZR_SNAP_M_16K : ZR_SNAP_M_48K, hw, modify ? "ZX Spectrum 16K" : "ZX Spectrum 48K"); return;
    case 1: zr_snap_set_machine(s, modify ? ZR_SNAP_M_16K : ZR_SNAP_M_48K, hw, modify ? "ZX Spectrum 16K" : "ZX Spectrum 48K");
            s->extras |= ZR_SNAP_X_INTERFACE1; return;
    case 2: zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "SamRam"); return;
    case 3:
        if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_48K, hw, "ZX Spectrum 48K + MGT"); return; }
        zr_snap_set_machine(s, modify ? ZR_SNAP_M_PLUS2 : ZR_SNAP_M_128K, hw, modify ? "ZX Spectrum +2" : "ZX Spectrum 128K");
        return;
    case 4: zr_snap_set_machine(s, modify ? ZR_SNAP_M_PLUS2 : ZR_SNAP_M_128K, hw, modify ? "ZX Spectrum +2" : "ZX Spectrum 128K");
            if (!v3) s->extras |= ZR_SNAP_X_INTERFACE1;
            return;
    case 5: if (v3) { zr_snap_set_machine(s, modify ? ZR_SNAP_M_PLUS2 : ZR_SNAP_M_128K, hw, modify ? "ZX Spectrum +2" : "ZX Spectrum 128K");
                      s->extras |= ZR_SNAP_X_INTERFACE1; return; }
            break;
    case 6: if (v3) { zr_snap_set_machine(s, modify ? ZR_SNAP_M_PLUS2 : ZR_SNAP_M_128K, hw, modify ? "ZX Spectrum +2" : "ZX Spectrum 128K + MGT"); return; }
            break;
    case 7:
    case 8: if (v3) { zr_snap_set_machine(s, modify ? ZR_SNAP_M_PLUS2A : ZR_SNAP_M_PLUS3, hw, modify ? "ZX Spectrum +2A" : "ZX Spectrum +3"); return; }
            break;
    case 9: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "Pentagon 128"); return; }
            break;
    case 10: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "Scorpion ZS-256"); return; }
             break;
    case 11: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "Didaktik Kompakt"); return; }
             break;
    case 12: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_PLUS2, hw, "ZX Spectrum +2"); return; }
             break;
    case 13: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_PLUS2A, hw, "ZX Spectrum +2A"); return; }
             break;
    case 14: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "Timex TC2048"); return; }
             break;
    case 15: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "Timex TC2068"); return; }
             break;
    case 128: if (v3) { zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, "Timex TS2068"); return; }
              break;
    default: break;
    }
    snprintf(name, sizeof name, "Z80 hardware mode %u", hw);
    zr_snap_set_machine(s, ZR_SNAP_M_OTHER, hw, name);
}

int zr_snap_read_z80(const uint8_t *d, size_t n, zr_snap *s, char *error, size_t capacity) {
    if (n < V1_HEADER) return zr_snap_fail(error, capacity, "Damaged Z80 file: it is shorter than its header");
    unsigned flags = d[12];
    if (flags == 255) flags = 1;
    s->af = (uint16_t)(d[0] << 8 | d[1]);
    s->bc = (uint16_t)zr_snap_rd16(d + 2);
    s->hl = (uint16_t)zr_snap_rd16(d + 4);
    s->sp = (uint16_t)zr_snap_rd16(d + 8);
    s->i = d[10];
    s->r = (uint8_t)((d[11] & 0x7Fu) | (flags & 1u) << 7);
    s->border = (flags >> 1) & 7u;
    s->de = (uint16_t)zr_snap_rd16(d + 13);
    s->bc2 = (uint16_t)zr_snap_rd16(d + 15);
    s->de2 = (uint16_t)zr_snap_rd16(d + 17);
    s->hl2 = (uint16_t)zr_snap_rd16(d + 19);
    s->af2 = (uint16_t)(d[21] << 8 | d[22]);
    s->iy = (uint16_t)zr_snap_rd16(d + 23);
    s->ix = (uint16_t)zr_snap_rd16(d + 25);
    s->iff1 = d[27] != 0;
    s->iff2 = d[28] != 0;
    s->im = d[29] & 3u;
    if (s->im > 2) return zr_snap_fail(error, capacity, "Damaged Z80 file: interrupt mode 3");
    const unsigned pc = zr_snap_rd16(d + 6);

    if (pc) {
        /* Version 1: 48 KiB follow the header, compressed when flags bit 5 says so. */
        s->pc = (uint16_t)pc;
        zr_snap_set_machine(s, ZR_SNAP_M_48K, 0, "ZX Spectrum 48K");
        const uint8_t *data = d + V1_HEADER;
        size_t length = n - V1_HEADER;
        uint8_t *image = malloc(3u * PAGE);
        if (!image) return zr_snap_fail(error, capacity, "Out of memory");
        int ok;
        if (flags >> 5 & 1u) {
            if (length >= 4 && !memcmp(data + length - 4, "\x00\xED\xED\x00", 4)) length -= 4;
            ok = decompress(data, length, image, 3u * PAGE);
            if (!ok) zr_snap_fail(error, capacity, "Damaged Z80 file: its compressed memory does not come to 48 KiB");
        } else {
            ok = length == 3u * PAGE;
            if (ok) memcpy(image, data, 3u * PAGE);
            else zr_snap_fail(error, capacity, "Damaged Z80 file: its memory is %zu bytes, not 48 KiB", length);
        }
        if (ok) {
            const unsigned order[3] = {5, 2, 0};
            for (unsigned k = 0; k < 3; ++k) {
                memcpy(s->ram[order[k]], image + (size_t)k * PAGE, PAGE);
                s->pages |= (uint8_t)(1u << order[k]);
            }
        }
        free(image);
        return ok;
    }

    /* Versions 2 and 3. */
    if (n < 32) return zr_snap_fail(error, capacity, "Damaged Z80 file: it is cut short in its header");
    const unsigned extra = zr_snap_rd16(d + 30);
    if (extra != 23 && extra != 54 && extra != 55) return zr_snap_fail(error, capacity, "Unsupported Z80 file: its extra header is %u bytes long", extra);
    const size_t body = 32u + extra;
    if (n < body) return zr_snap_fail(error, capacity, "Damaged Z80 file: it is cut short in its header");
    const int v3 = extra != 23;
    s->pc = (uint16_t)zr_snap_rd16(d + 32);
    const unsigned hw = d[34];
    const int modify = d[37] >> 7 & 1u;
    machine_of(s, hw, v3, modify);
    const int is128 = zr_snap_is_128k(s) || (s->machine == ZR_SNAP_M_OTHER && (hw == 9 || hw == 10));
    if (zr_snap_is_128k(s)) {
        s->port_7ffd = d[35];
        s->ay_present = 1;
    }
    if ((s->extras & ZR_SNAP_X_INTERFACE1) && d[36] == 0xFF) s->extras |= ZR_SNAP_X_ROM_PAGED;
    if (v3 && (d[59] == 0xFF || d[60] == 0xFF)) s->extras |= ZR_SNAP_X_ROM_PAGED;          /* the MGT's or the Multiface's ROM */
    if (extra == 55) s->port_1ffd = d[86];
    if (d[37] >> 2 & 1u) s->ay_present = 1;
    if (s->ay_present) {
        s->ay_current = d[38] & 15u;
        memcpy(s->ay, d + 39, 16);
    }
    if (v3) {
        const unsigned quarter = is128 ? QUARTER_128K : QUARTER_48K, low = zr_snap_rd16(d + 55), high = d[57];
        if (low < quarter) {
            s->has_tstates = 1;
            s->tstates = ((high + 1u) & 3u) * quarter + (quarter - 1u - low);
        }
    }
    /* The blocks. */
    size_t pos = body;
    while (pos < n) {
        if (n - pos < 3) return zr_snap_fail(error, capacity, "Damaged Z80 file: a memory block is cut short");
        const unsigned length = zr_snap_rd16(d + pos), page = d[pos + 2];
        pos += 3;
        const size_t stored = length == 0xFFFFu ? PAGE : length;
        if (stored > n - pos) return zr_snap_fail(error, capacity, "Damaged Z80 file: a memory block runs past the end of the file");
        const int ram = is128 ? ram_page_128k(page) : ram_page_48k(page);
        if (ram >= 0) {
            if (length == 0xFFFFu) memcpy(s->ram[ram], d + pos, PAGE);
            else if (!decompress(d + pos, stored, s->ram[ram], PAGE))
                return zr_snap_fail(error, capacity, "Damaged Z80 file: memory page %u does not decompress to 16 KiB", page);
            s->pages |= (uint8_t)(1u << ram);
        }
        pos += stored;
    }
    return 1;
}

/* --- writing ----------------------------------------------------------------------------------- */

int zr_snap_write_z80(const zr_snap *s, uint8_t **bytes, size_t *size, char *error, size_t capacity) {
    unsigned hw = 0;
    int modify = 0;
    switch (s->machine) {
    case ZR_SNAP_M_48K: hw = 0; break;
    case ZR_SNAP_M_16K: hw = 0; modify = 1; break;
    case ZR_SNAP_M_128K: hw = 4; break;
    case ZR_SNAP_M_PLUS2: hw = 12; break;
    case ZR_SNAP_M_PLUS2A: hw = 13; break;
    case ZR_SNAP_M_PLUS3: hw = 7; break;
    default:
        if (s->format != ZR_SNAP_Z80) return zr_snap_fail(error, capacity, "The Z80 format has no number for a %s", s->name);
        hw = s->id;
        break;
    }
    const int is128 = zr_snap_is_128k(s) || (s->machine == ZR_SNAP_M_OTHER && (hw == 9 || hw == 10));
    const int has1ffd = s->machine == ZR_SNAP_M_PLUS2A || s->machine == ZR_SNAP_M_PLUS3;
    const unsigned extra = has1ffd ? 55u : 54u;
    const size_t header = 32u + extra;
    /* every page the snapshot has, compressed or as it is, whichever is shorter */
    uint8_t *scratch = malloc(2u * PAGE + 8u);
    uint8_t *out = malloc(header + (size_t)ZR_SNAP_PAGES * (PAGE + 3u));
    if (!scratch || !out) {
        free(scratch);
        free(out);
        return zr_snap_fail(error, capacity, "Out of memory");
    }
    memset(out, 0, header);
    out[0] = (uint8_t)(s->af >> 8);
    out[1] = (uint8_t)s->af;
    zr_snap_put16(out + 2, s->bc);
    zr_snap_put16(out + 4, s->hl);
    zr_snap_put16(out + 8, s->sp);
    out[10] = s->i;
    out[11] = s->r & 0x7Fu;
    out[12] = (uint8_t)((s->r >> 7 & 1u) | (s->border & 7u) << 1);
    zr_snap_put16(out + 13, s->de);
    zr_snap_put16(out + 15, s->bc2);
    zr_snap_put16(out + 17, s->de2);
    zr_snap_put16(out + 19, s->hl2);
    out[21] = (uint8_t)(s->af2 >> 8);
    out[22] = (uint8_t)s->af2;
    zr_snap_put16(out + 23, s->iy);
    zr_snap_put16(out + 25, s->ix);
    out[27] = s->iff1 ? 1 : 0;
    out[28] = s->iff2 ? 1 : 0;
    out[29] = s->im & 3u;
    zr_snap_put16(out + 30, extra);
    zr_snap_put16(out + 32, zr_snap_pc_for_file(s));
    out[34] = (uint8_t)hw;
    out[35] = s->port_7ffd;
    out[37] = (uint8_t)(1u | (s->ay_present ? 4u : 0u) | (modify ? 0x80u : 0u));
    if (s->ay_present) {
        out[38] = s->ay_current;
        memcpy(out + 39, s->ay, 16);
    }
    if (s->has_tstates) {
        const unsigned quarter = is128 ? QUARTER_128K : QUARTER_48K;
        const uint32_t t = s->tstates % (4u * quarter);
        zr_snap_put16(out + 55, quarter - 1u - t % quarter);
        out[57] = (uint8_t)((t / quarter + 3u) & 3u);
    }
    if (has1ffd) out[86] = s->port_1ffd;
    size_t o = header;
    for (unsigned ram = 0; ram < ZR_SNAP_PAGES; ++ram) {
        if (!(s->pages >> ram & 1u)) continue;
        int page = -1;
        if (is128) page = (int)ram + 3;
        else page = ram == 5 ? 8 : ram == 2 ? 4 : ram == 0 ? 5 : -1;
        if (page < 0) continue;
        const size_t length = compress(s->ram[ram], PAGE, scratch);
        if (length < PAGE) {
            zr_snap_put16(out + o, (unsigned)length);
            out[o + 2] = (uint8_t)page;
            memcpy(out + o + 3, scratch, length);
            o += 3 + length;
        } else {
            zr_snap_put16(out + o, 0xFFFFu);
            out[o + 2] = (uint8_t)page;
            memcpy(out + o + 3, s->ram[ram], PAGE);
            o += 3u + PAGE;
        }
    }
    free(scratch);
    *bytes = out;
    *size = o;
    return 1;
}
