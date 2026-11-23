/* The DivMMC (divmmc.h). ZetRo's own code, written from the DivMMC's published description and the SD specification's SPI mode. */
#include "divmmc.h"
#include "state.h"

#include <string.h>

#define STATE_VERSION 1u

void zr_divmmc_init(zr_divmmc *d, const zr_hw_host *host, const zr_divmem_board *board, const uint8_t *eeprom) {
    memset(d, 0, sizeof *d);
    zr_divmem_init(&d->mem, board, eeprom, ZR_DIVMMC_BANKS);
    zr_mmc_init(&d->mmc, host, "divmmc");
    d->rx = 0xFFu;
}

void zr_divmmc_reset(zr_divmmc *d, int power_on) {
    zr_divmem_reset(&d->mem, power_on);
    if (power_on) d->mmc.dev.ops->reset(&d->mmc);       /* the card has its own power: only power on starts it again */
    else zr_mmc_select(&d->mmc, 0);
    d->rx = 0xFFu;
}

int zr_divmmc_attach(zr_divmmc *d, zr_card *card, char *error, size_t capacity) {
    if (!card) {
        zr_mmc_detach(&d->mmc);
        d->rx = 0xFFu;
        return 1;
    }
    d->rx = 0xFFu;
    return zr_mmc_attach(&d->mmc, card, ZR_MMC_SD, error, capacity);
}

uint8_t zr_divmmc_in(zr_divmmc *d, uint16_t port) {
    if ((port & ZR_DIVMMC_MASK) != ZR_DIVMMC_SPI) return 0xFFu;
    const uint8_t byte = d->rx;
    d->rx = zr_mmc_exchange(&d->mmc, 0xFFu);
    return byte;
}

void zr_divmmc_out(zr_divmmc *d, uint16_t port, uint8_t value) {
    switch (port & ZR_DIVMMC_MASK) {
    case ZR_DIVMMC_CONTROL: zr_divmem_control(&d->mem, value); break;
    case ZR_DIVMMC_SELECT: zr_mmc_select(&d->mmc, !(value & 1u)); break;
    case ZR_DIVMMC_SPI: d->rx = zr_mmc_exchange(&d->mmc, value); break;
    default: break;
    }
}

int zr_divmmc_light(const zr_divmmc *d) { return zr_mmc_light(&d->mmc); }
uint32_t zr_divmmc_activity(const zr_divmmc *d) { return zr_mmc_activity(&d->mmc); }

void zr_divmmc_save(const zr_divmmc *d, zr_state_writer *w) {
    zr_sw_begin(w, "DIVM", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, d->rx);
    zr_divmem_save(&d->mem, w);
    zr_sw_end(w);
    zr_sw_begin(w, "MMC ", 0);
    d->mmc.dev.ops->save(&d->mmc, w);
    zr_sw_end(w);
}

int zr_divmmc_load(zr_divmmc *d, zr_state_reader *r) {
    if (!zr_sr_chunk(r, "DIVM", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    const uint8_t rx = zr_sr_u8(r);
    if (!zr_sr_ok(r) || !zr_divmem_load(&d->mem, r)) return 0;
    if (!zr_sr_chunk(r, "MMC ", 0) || !d->mmc.dev.ops->load(&d->mmc, r)) return 0;
    d->rx = rx;
    return 1;
}
