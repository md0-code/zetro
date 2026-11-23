/* The memory of the DivIDE and DivMMC (divmem.h). ZetRo's own code, written from the DivIDE's published hardware description. */
#include "divmem.h"
#include "state.h"

#include <string.h>

#define STATE_VERSION 1u

/* The automatic mapping's addresses: the entry points that map in after their fetch, the area that maps in at once, the area
 * that maps out. */
static const uint16_t ENTRIES[] = {0x0000u, 0x0008u, 0x0038u, 0x0066u, 0x04C6u, 0x0562u};
#define INSTANT_FIRST 0x3D00u
#define INSTANT_LAST 0x3DFFu
#define OFF_FIRST 0x1FF8u
#define OFF_LAST 0x1FFFu

int zr_divmem_mapped(const zr_divmem *d) { return d->conmem || d->automap; }

/* RAM bank 3 stands in for the EEPROM while MAPRAM is set and only the automatic mapping has the memory in (CONMEM has the
 * higher priority). */
static int low_is_ram(const zr_divmem *d) { return d->mapram && !d->conmem; }

int zr_divmem_page(const zr_divmem *d, unsigned page, const uint8_t **bytes) {
    if (!zr_divmem_mapped(d) || page >= 16u) return 0;
    if (page < 8u) *bytes = (low_is_ram(d) ? d->ram + 3u * ZR_DIVMEM_BANK : d->eeprom) + page * 1024u;
    else *bytes = d->ram + d->bank * ZR_DIVMEM_BANK + (page - 8u) * 1024u;
    return 1;
}

static void announce(zr_divmem *d) {
    if (d->board.map) d->board.map(d->board.ctx);
}

void zr_divmem_write(zr_divmem *d, uint16_t address, uint8_t value) {
    if (!zr_divmem_mapped(d) || address >= 0x4000u) return;
    if (address < 0x2000u) return;                                  /* the EEPROM, or RAM bank 3 standing in for it: read-only */
    /* The bank at 2000h-3FFFh: always writable with CONMEM; under MAPRAM it is not when it is bank 3 (the bank at 0000h too). */
    if (!d->conmem && d->mapram && d->bank == 3u) return;
    d->ram[d->bank * ZR_DIVMEM_BANK + (address - 0x2000u)] = value;
}

void zr_divmem_control(zr_divmem *d, uint8_t value) {
    const int was = zr_divmem_mapped(d);
    const uint8_t conmem = d->conmem, mapram = d->mapram, bank = d->bank;
    d->conmem = value >> 7 & 1u;
    d->mapram = (uint8_t)(d->mapram | (value >> 6 & 1u));
    d->bank = (uint8_t)(value & (d->banks - 1u));
    if (conmem != d->conmem || mapram != d->mapram || bank != d->bank || was != zr_divmem_mapped(d)) announce(d);
}

int zr_divmem_button(zr_divmem *d) {
    if (zr_divmem_mapped(d)) return 0;
    if (d->board.nmi) d->board.nmi(d->board.ctx);
    return 1;
}

/* --- the automatic mapping ---------------------------------------------------------------------------- */

static void enter(zr_divmem *d) {
    if (d->automap) return;
    if (d->board.automap_ok && !d->board.automap_ok(d->board.ctx)) return;
    const int was = zr_divmem_mapped(d);
    d->automap = 1;
    if (!was) announce(d);
}

static void leave(zr_divmem *d) {
    if (!d->automap) return;
    const int was = zr_divmem_mapped(d);
    d->automap = 0;
    if (was != zr_divmem_mapped(d)) announce(d);
}

/* Before the opcode is read: the area that maps in at once. */
static int before_fetch(void *ctx, uint16_t pc) {
    zr_divmem *d = ctx;
    if (pc >= INSTANT_FIRST && pc <= INSTANT_LAST) enter(d);
    return 0;
}

/* After it: the entry points map in, the off area maps out. */
static void after_fetch(void *ctx, uint16_t pc, uint8_t opcode) {
    zr_divmem *d = ctx;
    (void)opcode;
    if (pc >= OFF_FIRST && pc <= OFF_LAST) {
        leave(d);
        return;
    }
    for (unsigned i = 0; i < sizeof ENTRIES / sizeof ENTRIES[0]; ++i)
        if (pc == ENTRIES[i]) {
            enter(d);
            return;
        }
}

const zr_cpu_hooks *zr_divmem_hooks(zr_divmem *d) { return &d->hooks; }

void zr_divmem_init(zr_divmem *d, const zr_divmem_board *board, const uint8_t *eeprom, unsigned banks) {
    memset(d, 0, sizeof *d);
    d->board = *board;
    d->eeprom = eeprom;
    d->banks = banks >= ZR_DIVMEM_MAX_BANKS ? ZR_DIVMEM_MAX_BANKS : 4u;
    for (unsigned i = 0; i < sizeof ENTRIES / sizeof ENTRIES[0]; ++i) d->watch[ENTRIES[i] >> 3] |= (uint8_t)(1u << (ENTRIES[i] & 7u));
    for (unsigned a = INSTANT_FIRST; a <= INSTANT_LAST; ++a) d->watch[a >> 3] |= (uint8_t)(1u << (a & 7u));
    for (unsigned a = OFF_FIRST; a <= OFF_LAST; ++a) d->watch[a >> 3] |= (uint8_t)(1u << (a & 7u));
    d->hooks.ctx = d;
    d->hooks.watch = d->watch;
    d->hooks.before_fetch = before_fetch;
    d->hooks.after_fetch = after_fetch;
}

void zr_divmem_reset(zr_divmem *d, int power_on) {
    d->conmem = 0;
    d->bank = 0;
    d->automap = 0;
    if (power_on) {
        d->mapram = 0;
        memset(d->ram, 0, sizeof d->ram);
    }
}

/* --- state -------------------------------------------------------------------------------------------- */

void zr_divmem_save(const zr_divmem *d, zr_state_writer *w) {
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, d->conmem);
    zr_sw_u8(w, d->mapram);
    zr_sw_u8(w, d->bank);
    zr_sw_u8(w, d->automap);
    zr_sw_u8(w, (uint8_t)d->banks);
    zr_sw_packed(w, d->ram, d->banks * ZR_DIVMEM_BANK);
}

int zr_divmem_load(zr_divmem *d, zr_state_reader *r) {
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    const uint8_t conmem = zr_sr_u8(r), mapram = zr_sr_u8(r), bank = zr_sr_u8(r), automap = zr_sr_u8(r), banks = zr_sr_u8(r);
    if (!zr_sr_ok(r) || banks != d->banks || conmem > 1u || mapram > 1u || automap > 1u || bank >= d->banks) return 0;
    zr_sr_packed(r, d->ram, d->banks * ZR_DIVMEM_BANK);
    if (!zr_sr_ok(r)) return 0;
    d->conmem = conmem;
    d->mapram = mapram;
    d->bank = bank;
    d->automap = automap;
    announce(d);
    return 1;
}
