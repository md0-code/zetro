/* ZetRo - the DivMMC interface (not original to any machine ZetRo runs: an added interface, off in a new configuration;
 * spectrum.h plugs it into the Spectrum-compatible boards). An SD card on an SPI port (hw/mmc.h), and 8 KiB of EEPROM (the
 * firmware, ESXDOS) with 128 KiB of RAM (16 banks of 8 KiB) paged over 0000h-3FFFh exactly as the DivIDE's is (hw/divmem.h).
 *
 * Written from the DivMMC's published description (it pages as the DivIDE does, with more RAM) and the SD specification's SPI
 * mode. The ports (A0-A7 only):
 *   E3h  the control register, write-only: bit 7 CONMEM, bit 6 MAPRAM, bits 3-0 the RAM bank at 2000h-3FFFh (divmem.h);
 *   E7h  the card select, write-only: bit 0 low selects the card (the chip select is active low); the other bits would select
 *        a second card, which this interface does not have;
 *   EBh  the SPI data port: a write sends the byte (the card's answer in the same eight clocks is kept); a read gives the byte
 *        kept and starts another exchange sending FFh, so a run of reads is a run of the card's bytes (the SPI transfer takes
 *        its eight clocks after the cycle that started it, which is why a read can only give the byte before).
 * Reset deselects the card and keeps its state (the card has its own power); power on starts it as a card is powered. The card
 * is a zr_card image (media/card.h) used in place: it is not part of a state. */
#ifndef ZR_HW_DIVMMC_H
#define ZR_HW_DIVMMC_H

#include "divmem.h"
#include "mmc.h"

#define ZR_DIVMMC_BANKS 16u                     /* 128 KiB of RAM */

#define ZR_DIVMMC_MASK 0x00FFu                  /* the control register, the card select and the SPI port */
#define ZR_DIVMMC_CONTROL 0x00E3u
#define ZR_DIVMMC_SELECT 0x00E7u
#define ZR_DIVMMC_SPI 0x00EBu

typedef struct zr_divmmc {
    zr_divmem mem;
    zr_mmc mmc;
    uint8_t rx;                                 /* the byte the last exchange brought */
} zr_divmmc;

void zr_divmmc_init(zr_divmmc *d, const zr_hw_host *host, const zr_divmem_board *board, const uint8_t *eeprom);
/* Power on (1) or the machine's reset (0). */
void zr_divmmc_reset(zr_divmmc *d, int power_on);
/* The card (an SD card; 0 with the reason when the image does not suit one); NULL takes it out (the caller closes it after). */
int zr_divmmc_attach(zr_divmmc *d, zr_card *card, char *error, size_t capacity);

/* The ports (mask ZR_DIVMMC_MASK): the control register, the select and the SPI data. */
uint8_t zr_divmmc_in(zr_divmmc *d, uint16_t port);
void zr_divmmc_out(zr_divmmc *d, uint16_t port, uint8_t value);

int zr_divmmc_light(const zr_divmmc *d);
uint32_t zr_divmmc_activity(const zr_divmmc *d);

/* The chunks "DIVM" (the paging, the RAM, the byte kept) and "MMC " (the card's protocol). Loading refuses a state of another card. */
void zr_divmmc_save(const zr_divmmc *d, struct zr_state_writer *w);
int zr_divmmc_load(zr_divmmc *d, struct zr_state_reader *r);

#endif /* ZR_HW_DIVMMC_H */
