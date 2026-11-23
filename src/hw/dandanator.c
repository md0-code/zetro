/* ZetRo - the ZX Dandanator Mini cartridge (dandanator.h). Written from the Dandanator developer
 * documentation; the flash's command sequences, times and status bits from the SST39SF040 data sheet. */
#include "dandanator.h"
#include "state.h"

#include <string.h>

#define STATE_VERSION 1u
#define VERSION_OFFSET 0x3FE0u                  /* the menu's version text in slot 0: "v10.3" */
#define LOCKED 0x04u
#define DISABLED 0x08u
#define REG_FIRST 0x2900u                       /* the PIC's registers over slot 0: commands 41 to 50 */
#define REG_END 0x3300u
#define VIEW_FIRST 0x2800u                      /* the 1 KiB pages that hold them */
#define VIEW_PAGE0 (VIEW_FIRST >> 10)
#define VIEW_PAGES 3u
#define SECTOR 0x1000u
#define SECTOR_BYTES 4096u                      /* bytes a program mode of command 48 takes */
#define ERASE_CYCLES 6u
enum { WMODE_NONE, WMODE_ERASE, WMODE_PROGRAM };
enum { PENDING_APPLY = 1, PENDING_RESET = 2, PENDING_NMI = 4 };
/* The flash's command sequence: after AA at 5555h, after 55 at 2AAAh, after 80, after 80 AA, after 80 AA 55, a byte to
 * program next. */
enum { F_READ, F_AA, F_AA55, F_80, F_80AA, F_80AA55, F_PROGRAM };

/* Times in microseconds. The PIC's: a pause this long after a pulse ends a number (nominal: longer than the 9-11 us
 * between the pulses of the menu's and the writer's loops, shorter than the pauses of 150 us and more they make between
 * numbers, at any of the boards' clocks); its delay before 36 and 40 act (dandanator.c: 35 T-states of the 3.5 MHz
 * Spectrum). The flash's typical byte program, sector erase and chip erase (SST39SF040 data sheet). */
#define PAUSE_US 50u
#define DELAY_US 10u
#define PROGRAM_US 14u
#define SECTOR_ERASE_US 18000u
#define CHIP_ERASE_US 70000u

static uint64_t ticks(const zr_dandanator *d, uint32_t us) { return (uint64_t)d->clock_hz * us / 1000000u; }

int zr_dandanator_detect(const uint8_t *data, size_t size) {
    if (!data || size != ZR_DANDANATOR_SIZE) return ZR_DANDANATOR_NOT_A_SET;
    const uint8_t v = data[VERSION_OFFSET];
    if (v == 'v' || v == 'V') {
        for (unsigned i = 1; i < 8u; ++i) {
            const uint8_t c = data[VERSION_OFFSET + i];
            if (!c) break;
            if (c < 0x20u || c > 0x7Eu) return ZR_DANDANATOR_NOT_A_SET;
        }
        return ZR_DANDANATOR_SET;
    }
    for (size_t i = 0; i < size; ++i)
        if (data[i] != 0xFFu) return ZR_DANDANATOR_NOT_A_SET;
    return ZR_DANDANATOR_BLANK;
}

/* --- the chip's deadline ------------------------------------------------------------------------------------------ */

static void arm(zr_dandanator *d) {
    uint64_t next = d->pulse_end;
    if (d->pending && d->apply_at < next) next = d->apply_at;
    if (d->busy_until && d->busy_until < next) next = d->busy_until;
    zr_dev_schedule(&d->dev, next);
}

static void map(zr_dandanator *d) { if (d->board.map) d->board.map(d->board.ctx); }

/* Slot 0's pages with the registers: the flash's bytes, a register written over its byte, 2900h-29FFh 00h elsewhere. */
static void refresh_view(zr_dandanator *d) {
    if (!d->set) return;
    memcpy(d->view, d->set + VIEW_FIRST, sizeof d->view);
    for (uint32_t a = REG_FIRST; a < REG_END; ++a) {
        const uint32_t i = a - REG_FIRST;
        if (d->reg_valid[i >> 3] >> (i & 7u) & 1u) d->view[a - VIEW_FIRST] = d->reg[i];
        else if (a < 0x2A00u) d->view[a - VIEW_FIRST] = 0x00u;
    }
}

/* --- the flash ---------------------------------------------------------------------------------------------------- */

static void busy(zr_dandanator *d, uint64_t now, uint32_t us, uint8_t data) {
    d->busy_until = now + ticks(d, us);
    if (!d->busy_until) d->busy_until = 1;
    d->fdata = data;
    d->ftoggle = 0;
    map(d);
}

static void changed(zr_dandanator *d, uint32_t first) {
    if (first < ZR_DANDANATOR_SLOT_SIZE) refresh_view(d);
    if (d->board.changed) d->board.changed(d->board.ctx);
}

static void erase(zr_dandanator *d, uint32_t first, uint32_t size, uint64_t now, uint32_t us) {
    int differs = 0;
    for (uint32_t i = 0; i < size; ++i) differs |= d->set[first + i] != 0xFFu;
    memset(d->set + first, 0xFF, size);
    busy(d, now, us, 0xFFu);
    if (differs) changed(d, first);
}

/* A write cycle on the flash at its address `a` (A0-A18): the data sheet's sequences. A14-A0 alone name the command
 * addresses; F0h outside a byte program goes back to reading (and leaves the software ID). */
static void flash_write(zr_dandanator *d, uint32_t a, uint8_t v, uint64_t now) {
    a &= ZR_DANDANATOR_SIZE - 1u;
    const uint32_t low = a & 0x7FFFu;
    if (v == 0xF0u && d->fstate != F_PROGRAM) {
        d->fstate = F_READ;
        if (d->fid) {
            d->fid = 0;
            map(d);
        }
        return;
    }
    switch (d->fstate) {
    case F_READ: d->fstate = low == 0x5555u && v == 0xAAu ? F_AA : F_READ; break;
    case F_AA: d->fstate = low == 0x2AAAu && v == 0x55u ? F_AA55 : F_READ; break;
    case F_AA55:
        d->fstate = F_READ;
        if (low != 0x5555u) break;
        if (v == 0xA0u) d->fstate = F_PROGRAM;
        else if (v == 0x80u) d->fstate = F_80;
        else if (v == 0x90u && !d->fid) {
            d->fid = 1;
            map(d);
        }
        break;
    case F_80: d->fstate = low == 0x5555u && v == 0xAAu ? F_80AA : F_READ; break;
    case F_80AA: d->fstate = low == 0x2AAAu && v == 0x55u ? F_80AA55 : F_READ; break;
    case F_80AA55:
        d->fstate = F_READ;
        if (v == 0x30u) erase(d, a & ~(SECTOR - 1u), SECTOR, now, SECTOR_ERASE_US);
        else if (v == 0x10u && low == 0x5555u) erase(d, 0, ZR_DANDANATOR_SIZE, now, CHIP_ERASE_US);
        break;
    default: {                                  /* F_PROGRAM: a bit goes from 1 to 0, never back */
        const uint8_t was = d->set[a];
        d->fstate = F_READ;
        d->set[a] = (uint8_t)(was & v);
        busy(d, now, PROGRAM_US, v);
        if (d->set[a] != was) changed(d, a);
        break;
    }
    }
}

/* A cycle the PIC passes to the flash in its write mode: A14-A18 its own (the command cycles at the JEDEC addresses,
 * A14 the complement of A13: 1555h is 5555h, 2AAAh stays; the last cycle of a sequence in the sector's slot). */
static void flash_cycle(zr_dandanator *d, uint16_t address, uint8_t value, uint64_t now) {
    const uint32_t sector_slot = (uint32_t)(d->wsector >> 2) << 14;
    const uint32_t jedec = address & 0x2000u ? address : address | 0x4000u;
    uint32_t chip;
    ++d->wcycle;
    if (d->wmode == WMODE_ERASE) {
        chip = d->wcycle == ERASE_CYCLES ? sector_slot | address : jedec;
        if (d->wcycle >= ERASE_CYCLES) d->wmode = WMODE_NONE;
    } else {
        chip = (d->wcycle & 3u) == 0 ? sector_slot | address : jedec;
        if (d->wcycle >= 4u * SECTOR_BYTES) d->wmode = WMODE_NONE;
    }
    if (d->program && (!d->board.writable || d->board.writable(d->board.ctx))) flash_write(d, chip, value, now);
}

/* --- the PIC ----------------------------------------------------------------------------------------------------- */

static void clear_command(zr_dandanator *d) {
    memset(d->cmd, 0, sizeof d->cmd);
    d->stage = 0;
    d->pulse_end = ZR_NEVER;
}

/* Slot 0 selected unlocks the commands. */
static void select_slot(zr_dandanator *d, unsigned slot) {
    d->slot = (uint8_t)(slot > ZR_DANDANATOR_HIDDEN ? ZR_DANDANATOR_HIDDEN : slot);
    if (!d->slot) d->sleep = (uint8_t)(d->sleep & ~(LOCKED | DISABLED));
}

static void show(zr_dandanator *d) {
    if (d->mapped == d->slot) return;
    d->mapped = d->slot;
    map(d);
}

static void store_register(zr_dandanator *d, unsigned command, unsigned value, unsigned offset) {
    const uint32_t a = (uint32_t)command << 8 | (offset & 0xFFu);
    if (a < REG_FIRST || a >= REG_END) return;
    const uint32_t i = a - REG_FIRST;
    d->reg[i] = (uint8_t)value;
    d->reg_valid[i >> 3] |= (uint8_t)(1u << (i & 7u));
    if (command == 50u) d->pic_ram[offset & 0xFFu] = (uint8_t)value;
    d->view[a - VIEW_FIRST] = (uint8_t)value;
}

static void finish(zr_dandanator *d, uint64_t now) {
    const unsigned command = d->cmd[0], p1 = d->cmd[1], p2 = d->cmd[2];
    clear_command(d);
    if (d->sleep & DISABLED) return;
    if ((d->sleep & LOCKED) && command != 46u) return;
    if (command == 46u) {
        if (p1 == p2) {
            if (p1 == 1u) d->sleep = (uint8_t)((d->sleep & ~DISABLED) | LOCKED);
            else if (p1 == 16u) d->sleep = (uint8_t)(d->sleep & ~(LOCKED | DISABLED));
            else if (p1 == 31u) d->sleep = (uint8_t)((d->sleep & ~LOCKED) | DISABLED);
        }
        return;
    }
    if (command > 0u && command < 34u) {
        select_slot(d, command - 1u);
        show(d);
        return;
    }
    switch (command) {
    case 34:
        d->slot = ZR_DANDANATOR_HIDDEN;
        d->sleep = (uint8_t)((d->sleep & ~DISABLED) | LOCKED);
        show(d);
        break;
    case 36:
        d->pending = PENDING_APPLY | PENDING_RESET;
        d->apply_at = now + ticks(d, DELAY_US);
        break;
    case 40:
        select_slot(d, p1 ? p1 - 1u : 0u);
        if (p2 & 0x08u) d->sleep = (uint8_t)((d->sleep & ~LOCKED) | DISABLED);
        else if (p2 & 0x04u) d->sleep = (uint8_t)((d->sleep & ~DISABLED) | LOCKED);
        d->pending = (uint8_t)(PENDING_APPLY | (p2 & 0x01u ? PENDING_RESET : 0u) | (p2 & 0x02u ? PENDING_NMI : 0u));
        d->apply_at = now + ticks(d, DELAY_US);
        break;
    case 48:
        if (p1 == 16u || p1 == 32u) {
            d->wmode = p1 == 16u ? WMODE_ERASE : WMODE_PROGRAM;
            d->wsector = (uint8_t)(p2 & 0x7Fu);
            d->wcycle = 0;
        }
        break;
    case 39:
        d->pointer0 = d->pointer1 = d->mode = 0;
        break;
    case 41: case 42: case 43: case 49: case 50:
        store_register(d, command, p1, p2);
        break;
    case 45:
        store_register(d, command, p1, p2);
        d->mode = (uint16_t)(p1 << 8 | p2);
        break;
    default:
        break;
    }
}

/* The pause after a train of pulses: the number is complete; a command below 40 is then, a special one after its fourth. */
static void pause_ended(zr_dandanator *d, uint64_t now) {
    if (d->stage & 1u) ++d->stage;
    if (d->stage > 6u || (d->cmd[0] > 0u && d->cmd[0] < 40u)) finish(d, now);
}

static void apply(zr_dandanator *d) {
    const uint8_t pending = d->pending;
    d->pending = 0;
    d->mapped = d->slot;
    map(d);
    if ((pending & PENDING_RESET) && d->board.reset) d->board.reset(d->board.ctx);
    if ((pending & PENDING_NMI) && d->board.nmi) d->board.nmi(d->board.ctx);
}

void zr_dandanator_write(zr_dandanator *d, uint16_t address, uint8_t value, uint64_t now) {
    if (!d->set) return;
    address &= 0x3FFFu;
    if (d->wmode) {
        flash_cycle(d, address, value, now);
        arm(d);
        return;
    }
    if (address >= 4u || (d->sleep & DISABLED)) return;
    const unsigned index = (d->stage | 1u) / 2u;
    if (index >= 4u) return;
    ++d->cmd[index];
    d->stage |= 1u;
    d->pulse_end = now + ticks(d, PAUSE_US);
    /* The confirmation of a special command: a write to 0000h after its three numbers. */
    if (address == 0u && d->cmd[0] >= 40u && d->stage >= 6u) finish(d, now);
    arm(d);
}

static void event(void *dev, uint64_t now) {
    zr_dandanator *d = dev;
    if (d->pulse_end <= now) {
        d->pulse_end = ZR_NEVER;
        pause_ended(d, now);
    }
    if (d->pending && d->apply_at <= now) apply(d);
    if (d->busy_until && d->busy_until <= now) {
        d->busy_until = 0;
        map(d);
    }
    arm(d);
}

/* --- the processor's reads ------------------------------------------------------------------------------------------- */

int zr_dandanator_page(const zr_dandanator *d, unsigned page, const uint8_t **bytes) {
    *bytes = NULL;
    if (!d->set || d->mapped >= ZR_DANDANATOR_HIDDEN || page >= 16u) return ZR_DANDANATOR_OUT;
    if (d->busy_until || d->fid) return ZR_DANDANATOR_STATUS;
    if (!d->mapped && page >= VIEW_PAGE0 && page < VIEW_PAGE0 + VIEW_PAGES) *bytes = d->view + (page - VIEW_PAGE0) * 1024u;
    else *bytes = d->set + (uint32_t)d->mapped * ZR_DANDANATOR_SLOT_SIZE + page * 1024u;
    return ZR_DANDANATOR_DIRECT;
}

/* While the flash works: DQ7 the complement of the byte's bit 7, DQ6 toggling from read to read; in the software ID the
 * maker (BFh, SST) and the device (B7h, SST39SF040) at even and odd addresses. */
uint8_t zr_dandanator_read(zr_dandanator *d, uint16_t address) {
    if (d->busy_until) {
        d->ftoggle ^= 0x40u;
        return (uint8_t)((~d->fdata & 0x80u) | d->ftoggle);
    }
    if (d->fid) return address & 1u ? 0xB7u : 0xBFu;
    return d->set && d->mapped < ZR_DANDANATOR_HIDDEN ? d->set[(uint32_t)d->mapped * ZR_DANDANATOR_SLOT_SIZE + (address & 0x3FFFu)] : 0xFFu;
}

/* --- life cycle ----------------------------------------------------------------------------------------------------- */

void zr_dandanator_reset(zr_dandanator *d) {
    clear_command(d);
    memset(d->reg, 0, sizeof d->reg);
    memset(d->reg_valid, 0, sizeof d->reg_valid);
    memset(d->pic_ram, 0, sizeof d->pic_ram);
    d->pointer0 = d->pointer1 = d->mode = 0;
    d->sleep = 0;
    d->pending = 0;
    d->wmode = WMODE_NONE;
    d->wcycle = 0;
    d->fstate = F_READ;
    d->fid = 0;
    d->blank = (uint8_t)(d->set && zr_dandanator_detect(d->set, ZR_DANDANATOR_SIZE) == ZR_DANDANATOR_BLANK);
    /* Hidden, it is locked as command 34 leaves it: the board's ROM writes to 0000h-0003h too (the 48K ROM's calculator
     * does), which must not select a slot; a writer unlocks it with 46,16,16. */
    d->slot = d->mapped = (uint8_t)(d->program || d->blank ? ZR_DANDANATOR_HIDDEN : 0u);
    if (d->slot) d->sleep = LOCKED;
    refresh_view(d);
    arm(d);
    map(d);
}

void zr_dandanator_insert(zr_dandanator *d, uint8_t *set) {
    d->set = set;
    d->busy_until = 0;
    zr_dandanator_reset(d);
}

static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void dev_reset(void *dev) { zr_dandanator_reset(dev); }

static void save_fields(const void *dev, zr_state_writer *w) {
    const zr_dandanator *d = dev;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, d->slot);
    zr_sw_u8(w, d->mapped);
    zr_sw_u8(w, d->sleep);
    zr_sw_bytes(w, d->cmd, sizeof d->cmd);
    zr_sw_u8(w, d->stage);
    zr_sw_u8(w, d->pending);
    zr_sw_u64(w, d->pulse_end);
    zr_sw_u64(w, d->apply_at);
    zr_sw_u8(w, d->wmode);
    zr_sw_u8(w, d->wsector);
    zr_sw_u32(w, d->wcycle);
    zr_sw_bytes(w, d->reg, sizeof d->reg);
    zr_sw_bytes(w, d->reg_valid, sizeof d->reg_valid);
    zr_sw_bytes(w, d->pic_ram, sizeof d->pic_ram);
    zr_sw_u16(w, d->pointer0);
    zr_sw_u16(w, d->pointer1);
    zr_sw_u16(w, d->mode);
    zr_sw_u8(w, d->fstate);
    zr_sw_u8(w, d->fid);
    zr_sw_u8(w, d->fdata);
    zr_sw_u8(w, d->ftoggle);
    zr_sw_u64(w, d->busy_until);
}

static int load_fields(void *dev, zr_state_reader *r) {
    zr_dandanator *d = dev;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    d->slot = zr_sr_u8(r);
    d->mapped = zr_sr_u8(r);
    d->sleep = zr_sr_u8(r) & (LOCKED | DISABLED);
    zr_sr_bytes(r, d->cmd, sizeof d->cmd);
    d->stage = zr_sr_u8(r);
    d->pending = zr_sr_u8(r) & (PENDING_APPLY | PENDING_RESET | PENDING_NMI);
    d->pulse_end = zr_sr_u64(r);
    d->apply_at = zr_sr_u64(r);
    d->wmode = zr_sr_u8(r);
    d->wsector = zr_sr_u8(r) & 0x7Fu;
    d->wcycle = zr_sr_u32(r);
    zr_sr_bytes(r, d->reg, sizeof d->reg);
    zr_sr_bytes(r, d->reg_valid, sizeof d->reg_valid);
    zr_sr_bytes(r, d->pic_ram, sizeof d->pic_ram);
    d->pointer0 = zr_sr_u16(r);
    d->pointer1 = zr_sr_u16(r);
    d->mode = zr_sr_u16(r);
    d->fstate = zr_sr_u8(r);
    d->fid = zr_sr_u8(r) & 1u;
    d->fdata = zr_sr_u8(r);
    d->ftoggle = zr_sr_u8(r) & 0x40u;
    d->busy_until = zr_sr_u64(r);
    if (!zr_sr_ok(r) || d->slot > ZR_DANDANATOR_HIDDEN || d->mapped > ZR_DANDANATOR_HIDDEN || d->stage > 8u ||
        d->wmode > WMODE_PROGRAM || d->fstate > F_PROGRAM)
        return 0;
    if (!d->set) d->busy_until = 0;
    refresh_view(d);
    arm(d);
    map(d);
    return 1;
}

static const zr_dev_ops OPS = {.read = no_read, .write = no_write, .event = event, .reset = dev_reset,
                               .save = save_fields, .load = load_fields};

void zr_dandanator_init(zr_dandanator *d, const zr_hw_host *host, const zr_dandanator_board *board, uint32_t clock_hz,
                        int program) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &OPS;
    d->dev.host = host;
    d->dev.name = "dandanator";
    d->board = *board;
    d->clock_hz = clock_hz;
    d->program = (uint8_t)(program != 0);
    d->pulse_end = ZR_NEVER;
    d->slot = d->mapped = ZR_DANDANATOR_HIDDEN;
}

void zr_dandanator_save(const zr_dandanator *d, zr_state_writer *w) {
    zr_sw_begin(w, "DAN ", 0);
    save_fields(d, w);
    zr_sw_end(w);
}

int zr_dandanator_load(zr_dandanator *d, zr_state_reader *r) { return zr_sr_chunk(r, "DAN ", 0) && load_fields(d, r); }
