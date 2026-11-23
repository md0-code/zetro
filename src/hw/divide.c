/* The DivIDE (divide.h). ZetRo's own code, written from the DivIDE's published programming model. */
#include "divide.h"
#include "state.h"

#include <string.h>

#define STATE_VERSION 1u

void zr_divide_init(zr_divide *d, const zr_hw_host *host, const zr_divmem_board *board, const uint8_t *eeprom, uint32_t clock_hz) {
    memset(d, 0, sizeof *d);
    zr_divmem_init(&d->mem, board, eeprom, ZR_DIVIDE_BANKS);
    zr_ata_init(&d->ata, host, "divide", clock_hz);
}

void zr_divide_reset(zr_divide *d, int power_on) {
    zr_divmem_reset(&d->mem, power_on);
    d->ata.dev.ops->reset(&d->ata);
    d->flip = 0;
    d->latch = 0xFFu;
}

void zr_divide_attach(zr_divide *d, unsigned unit, zr_card *card) { zr_ata_attach(&d->ata, unit, card); }

uint8_t zr_divide_in(zr_divide *d, uint16_t port) {
    if ((port & ZR_DIVIDE_IDE_MASK) != ZR_DIVIDE_IDE_VALUE) return 0xFFu;
    const unsigned reg = (port >> 2) & 7u;
    if (reg) {
        d->flip = 0;
        return zr_ata_read(&d->ata, reg);
    }
    if (!d->flip) {
        const uint16_t word = zr_ata_read_data16(&d->ata);
        d->latch = (uint8_t)(word >> 8);
        d->flip = 1;
        return (uint8_t)word;
    }
    d->flip = 0;
    return d->latch;
}

void zr_divide_out(zr_divide *d, uint16_t port, uint8_t value) {
    if ((port & ZR_DIVIDE_CONTROL_MASK) == ZR_DIVIDE_CONTROL_VALUE) {
        d->flip = 0;
        zr_divmem_control(&d->mem, value);
        return;
    }
    if ((port & ZR_DIVIDE_IDE_MASK) != ZR_DIVIDE_IDE_VALUE) return;
    const unsigned reg = (port >> 2) & 7u;
    if (reg) {
        d->flip = 0;
        zr_ata_write(&d->ata, reg, value);
    } else if (!d->flip) {
        d->latch = value;
        d->flip = 1;
    } else {
        d->flip = 0;
        zr_ata_write_data16(&d->ata, (uint16_t)(d->latch | (uint16_t)value << 8));
    }
}

int zr_divide_light(const zr_divide *d) { return zr_ata_light(&d->ata); }
uint32_t zr_divide_activity(const zr_divide *d, unsigned unit) { return zr_ata_activity(&d->ata, unit); }

void zr_divide_save(const zr_divide *d, zr_state_writer *w) {
    zr_sw_begin(w, "DIVI", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, d->flip);
    zr_sw_u8(w, d->latch);
    zr_divmem_save(&d->mem, w);
    zr_sw_end(w);
    zr_sw_begin(w, "ATA ", 0);
    d->ata.dev.ops->save(&d->ata, w);
    zr_sw_end(w);
}

int zr_divide_load(zr_divide *d, zr_state_reader *r) {
    if (!zr_sr_chunk(r, "DIVI", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    const uint8_t flip = zr_sr_u8(r), latch = zr_sr_u8(r);
    if (!zr_sr_ok(r) || flip > 1u || !zr_divmem_load(&d->mem, r)) return 0;
    if (!zr_sr_chunk(r, "ATA ", 0) || !d->ata.dev.ops->load(&d->ata, r)) return 0;
    d->flip = flip;
    d->latch = latch;
    return 1;
}
