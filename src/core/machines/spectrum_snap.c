/* The Spectrum-compatible core's snapshots (spectrum.h): a snapshot of the 48K machine (media/snap.h) applied to the
 * board, one captured from it, and the RAM pages of the 128K machine's numbering reached by peek and poke. This file
 * knows the three 16 KiB areas of the 48K machine (the pages 5, 2 and 0) and, for a board that gives its pages in its
 * config (spectrum.h: page, paging_get, paging_set), the 128K machine's. A snapshot never holds a ROM: the machine's own is kept. ZetRo's
 * own code. */
#include "spectrum.h"
#include "media/snap.h"

#include <stdio.h>
#include <string.h>

#define RAM_48K 0xC000u

/* The RAM page's place in the CPU's memory space on the 48K machine; -1 for a page it has not. */
static int base_of(unsigned page) { return page == 5 ? 0x4000 : page == 2 ? 0x8000 : page == 0 ? 0xC000 : -1; }

/* --- peek and poke by page --------------------------------------------------------------------- */

/* A board of the 128K kind (spectrum.h: page, paging_get and paging_set). */
static int is_128k(const zr_spectrum *s) { return s->cfg.page && s->cfg.paging_get && s->cfg.paging_set; }

/* The byte of RAM page `address` names (ZR_PAGE_ADDRESS), where the board has it: the board's own page, or on the 48K machine the
 * place the page has in the CPU's memory space. */
static uint8_t *page_byte(zr_spectrum *s, uint32_t address) {
    if (address < 0x100000u) return NULL;
    const uint32_t at = address - 0x100000u;
    const unsigned page = at >> 14, offset = at & 0x3FFFu;
    if (page >= ZR_SNAP_PAGES) return NULL;
    if (s->cfg.page) {
        uint8_t *bytes = s->cfg.page(s, page);
        return bytes ? bytes + offset : NULL;
    }
    if (s->cfg.ram_size != RAM_48K || base_of(page) < 0) return NULL;
    return s->ram + (size_t)(base_of(page) - 0x4000) + offset;
}

int zr_spectrum_page_peek(void *self, uint32_t address, uint8_t *value) {
    const uint8_t *at = page_byte(self, address);
    if (!at) return 0;
    *value = *at;
    return 1;
}

int zr_spectrum_page_poke(void *self, uint32_t address, uint8_t value) {
    zr_spectrum *s = self;
    uint8_t *at = page_byte(s, address);
    if (!at) return 0;
    /* The picture is drawn up to the write first. */
    if (at >= s->screen && at < s->screen + ZR_SPECTRUM_SCREEN) zr_ula_catch_up(&s->ula, zr_spectrum_now(s));
    *at = value;
    return 1;
}

/* The machine in a few characters, for a message that has to fit on the status line. */
static const char *short_name(const zr_snap *snap) {
    switch (snap->machine) {
    case ZR_SNAP_M_128K: return "128K";
    case ZR_SNAP_M_PLUS2: return "+2";
    case ZR_SNAP_M_PLUS2A: return "+2A";
    case ZR_SNAP_M_PLUS3: return "+3";
    default: return snap->name;
    }
}

/* --- applying a snapshot ------------------------------------------------------------------------- */

int zr_spectrum_snapshot_load(void *self, const zr_snap *snap, char *error, size_t capacity) {
    zr_spectrum *s = self;
    /* Everything that can be wrong is looked at before the machine is touched. */
    const int big_board = is_128k(s);
    if (s->cfg.ram_size != RAM_48K && !big_board) {
        if (capacity) snprintf(error, capacity, "Snapshots of the %s are not supported yet", s->description);
        return 0;
    }
    if (snap->machine == ZR_SNAP_M_OTHER) {
        if (capacity) snprintf(error, capacity, "%s: not for the %s", snap->name, s->description);
        return 0;
    }
    /* A board with a second paging port (the +2A/+3's 1FFD) takes the +2A and +3 snapshots too. */
    const int plus3_board = big_board && s->cfg.paging2_set;
    const int plus3_snap = plus3_board && (snap->machine == ZR_SNAP_M_PLUS2A || snap->machine == ZR_SNAP_M_PLUS3);
    const int big_snap = snap->machine == ZR_SNAP_M_128K || snap->machine == ZR_SNAP_M_PLUS2 || plus3_snap;
    if (snap->machine != ZR_SNAP_M_48K && snap->machine != ZR_SNAP_M_16K && !(big_board && big_snap)) {
        if (capacity) snprintf(error, capacity, "%s snapshot; the %s has %s", short_name(snap), s->description, big_board ? "128K" : "48K");
        return 0;
    }
    if (snap->extras & ZR_SNAP_X_ROM_PAGED) {
        if (capacity) snprintf(error, capacity, "Interface 1/MGT ROM was paged in");
        return 0;
    }
    const unsigned need = zr_snap_pages_of(snap->machine);
    if (need & ~(unsigned)snap->pages) {
        if (capacity) snprintf(error, capacity, "RAM is missing for the %s", s->description);
        return 0;
    }

    /* A warm reset brings the chips to a state the processor's registers can be put into: the interrupt line low, the
     * frame interrupt armed, the port FE latch cleared. Guest time goes on from where it is. */
    zr_spectrum_reset(s, 0);
    zr_ula_catch_up(&s->ula, zr_spectrum_now(s));       /* the picture so far is of the memory that goes */
    memset(s->ram, 0, s->cfg.ram_size);
    if (big_board) {
        for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page) {
            uint8_t *bytes = snap->pages >> page & 1u ? s->cfg.page(s, page) : NULL;
            if (bytes) memcpy(bytes, snap->ram[page], ZR_SNAP_PAGE_SIZE);
        }
        /* The paging as the file had it; a 48K machine's file runs in the 48 BASIC with the paging locked. */
        s->cfg.paging_set(s, big_snap ? snap->port_7ffd : s->cfg.paging_48k);
        if (plus3_board) s->cfg.paging2_set(s, plus3_snap ? snap->port_1ffd : big_snap ? 0u : s->cfg.paging2_48k);
    } else
        for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page)
            if (snap->pages >> page & 1u && base_of(page) >= 0)
                memcpy(s->ram + (size_t)(base_of(page) - 0x4000), snap->ram[page], ZR_SNAP_PAGE_SIZE);
    zr_z80 *z = &s->z80;
    z->a = (uint8_t)(snap->af >> 8); z->f = (uint8_t)snap->af;
    z->b = (uint8_t)(snap->bc >> 8); z->c = (uint8_t)snap->bc;
    z->d = (uint8_t)(snap->de >> 8); z->e = (uint8_t)snap->de;
    z->h = (uint8_t)(snap->hl >> 8); z->l = (uint8_t)snap->hl;
    z->a_ = (uint8_t)(snap->af2 >> 8); z->f_ = (uint8_t)snap->af2;
    z->b_ = (uint8_t)(snap->bc2 >> 8); z->c_ = (uint8_t)snap->bc2;
    z->d_ = (uint8_t)(snap->de2 >> 8); z->e_ = (uint8_t)snap->de2;
    z->h_ = (uint8_t)(snap->hl2 >> 8); z->l_ = (uint8_t)snap->hl2;
    z->ixh = (uint8_t)(snap->ix >> 8); z->ixl = (uint8_t)snap->ix;
    z->iyh = (uint8_t)(snap->iy >> 8); z->iyl = (uint8_t)snap->iy;
    z->sp = snap->sp;
    z->pc = snap->pc;
    z->i = snap->i;
    z->r = snap->r;
    z->iff1 = snap->iff1 != 0;
    z->iff2 = snap->iff2 != 0;
    z->im = snap->im;
    z->halted = snap->halted != 0;
    z->ei = snap->ei != 0;
    z->wz = snap->has_wz ? snap->wz : 0;
    z->p = z->q = 0;
    /* The border through port FE, as the processor would write it: the ULA, the speaker and MIC follow. */
    zr_bus_out(&s->bus, (uint16_t)((0x00FEu & ~(unsigned)s->cfg.fe_mask) | s->cfg.fe_value), (uint8_t)(snap->border & 7u));
    /* The AY board's registers, where the snapshot has them and the machine has the board. */
    if (s->cfg.ay && snap->ay_present) {
        for (unsigned r = 0; r < 16u; ++r) {
            zr_ay_select(&s->ay, (uint8_t)r);
            zr_ay_write(&s->ay, snap->ay[r]);
        }
        zr_ay_select(&s->ay, (uint8_t)(snap->ay_current & 15u));
    }
    return 1;
}

/* --- capturing one ------------------------------------------------------------------------------------ */

/* The T-states since the frame interrupt began, at the time of the run's end. */
static uint32_t frame_tstates(const zr_spectrum *s) {
    const zr_ula *u = &s->ula;
    unsigned line, pixel;
    zr_ula_position(u, zr_spectrum_now(s), &line, &pixel);
    const uint64_t total = u->total, per_line = u->cfg.pixels_per_line;
    const uint64_t at = (uint64_t)line * per_line + pixel, from = (uint64_t)u->cfg.int_y * per_line + u->cfg.int_x;
    const uint64_t pixels_per_tstate = u->cfg.ticks_per_pixel ? u->cfg.ticks_per_tstate / u->cfg.ticks_per_pixel : 1u;
    return (uint32_t)(((at + total - from % total) % total) / (pixels_per_tstate ? pixels_per_tstate : 1u));
}

int zr_spectrum_snapshot_save(void *self, zr_snap *snap) {
    zr_spectrum *s = self;
    const int big_board = is_128k(s);
    if (s->cfg.ram_size != RAM_48K && !big_board) return 0;
    memset(snap, 0, sizeof *snap);
    if (big_board && s->cfg.paging2_get) zr_snap_set_machine(snap, ZR_SNAP_M_PLUS3, 0, "ZX Spectrum +3");
    else if (big_board) zr_snap_set_machine(snap, ZR_SNAP_M_128K, 0, "ZX Spectrum 128K");
    else zr_snap_set_machine(snap, ZR_SNAP_M_48K, 0, "ZX Spectrum 48K");
    const zr_z80 *z = &s->z80;
    snap->pc = z->pc;
    snap->sp = z->sp;
    snap->af = zr_z80_af(z);
    snap->bc = zr_z80_bc(z);
    snap->de = zr_z80_de(z);
    snap->hl = zr_z80_hl(z);
    snap->af2 = (uint16_t)(z->a_ << 8 | z->f_);
    snap->bc2 = (uint16_t)(z->b_ << 8 | z->c_);
    snap->de2 = (uint16_t)(z->d_ << 8 | z->e_);
    snap->hl2 = (uint16_t)(z->h_ << 8 | z->l_);
    snap->ix = zr_z80_ix(z);
    snap->iy = zr_z80_iy(z);
    snap->i = z->i;
    snap->r = z->r;
    snap->iff1 = z->iff1 != 0;
    snap->iff2 = z->iff2 != 0;
    snap->im = z->im > 2 ? 2 : z->im;
    snap->halted = z->halted != 0;
    snap->ei = z->ei != 0;
    snap->wz = z->wz;
    snap->has_wz = 1;
    snap->border = s->ula.border & 7u;
    snap->port_fe = s->fe;
    snap->has_tstates = 1;
    snap->tstates = frame_tstates(s);
    if (s->cfg.ay) {
        snap->ay_present = 1;
        snap->ay_current = (uint8_t)(s->ay.selected & 15u);
        memcpy(snap->ay, s->ay.reg, sizeof snap->ay);
    }
    if (big_board) {
        snap->port_7ffd = s->cfg.paging_get(s);
        if (s->cfg.paging2_get) snap->port_1ffd = s->cfg.paging2_get(s);
        for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page) {
            const uint8_t *bytes = s->cfg.page(s, page);
            if (!bytes) continue;
            memcpy(snap->ram[page], bytes, ZR_SNAP_PAGE_SIZE);
            snap->pages |= (uint8_t)(1u << page);
        }
        return 1;
    }
    for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page)
        if (base_of(page) >= 0) {
            memcpy(snap->ram[page], s->ram + (size_t)(base_of(page) - 0x4000), ZR_SNAP_PAGE_SIZE);
            snap->pages |= (uint8_t)(1u << page);
        }
    return 1;
}
