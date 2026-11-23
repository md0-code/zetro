/* ZetRo - the DivIDE interface (not original to any machine ZetRo runs: an added interface, off in a new configuration;
 * spectrum.h plugs it into the Spectrum-compatible boards). An IDE channel with up to two drives (hw/ata.h) on the Z80's ports,
 * and 8 KiB of EEPROM (the firmware, ESXDOS) with 32 KiB of RAM paged over 0000h-3FFFh (hw/divmem.h).
 *
 * Written from the DivIDE's published programming model (the author's documentation, velesoft.speccy.cz/zx/divide):
 *   - ports, A0-A7 only: the eight registers of the IDE command block at 101r rr11 (data A3h, error and features A7h, sector
 *     count ABh, sector number AFh, cylinder low B3h, cylinder high B7h, drive and head BBh, status and command BFh); the
 *     control register at E3h, write-only (divmem.h);
 *   - the data register is 16 bits wide on an 8-bit bus: an odd access to port A3h reads the low byte and latches the high one
 *     (or latches the byte written), an even access gives the latched high byte (or writes the word, the latched byte low and
 *     the byte written high). Any access to registers 1-7 or the control register makes the next data access odd; other ports do
 *     not touch it; after power on or reset the state is unknown here (odd: the documents leave it open);
 *   - the IDE interrupt line and the alternate status and device control registers are not wired.
 * The IDE RESET line follows the machine's reset (nominal: the documents do not say). The drives are zr_card images
 * (media/card.h) used in place: they are not part of a state. */
#ifndef ZR_HW_DIVIDE_H
#define ZR_HW_DIVIDE_H

#include "ata.h"
#include "divmem.h"

#define ZR_DIVIDE_BANKS 4u                      /* 32 KiB of RAM */

/* The decoders of the board's bus: (port & mask) == value. */
#define ZR_DIVIDE_IDE_MASK 0x00E3u
#define ZR_DIVIDE_IDE_VALUE 0x00A3u
#define ZR_DIVIDE_CONTROL_MASK 0x00FFu
#define ZR_DIVIDE_CONTROL_VALUE 0x00E3u

typedef struct zr_divide {
    zr_divmem mem;
    zr_ata ata;
    uint8_t flip;                               /* 1: the next data access is even */
    uint8_t latch;                              /* the high byte of a read, the low byte of a write */
} zr_divide;

void zr_divide_init(zr_divide *d, const zr_hw_host *host, const zr_divmem_board *board, const uint8_t *eeprom, uint32_t clock_hz);
/* Power on (1) or the machine's reset (0). */
void zr_divide_reset(zr_divide *d, int power_on);
/* The drive `unit` (0 or 1) on the channel; NULL: none. */
void zr_divide_attach(zr_divide *d, unsigned unit, zr_card *card);

/* The ports: the IDE registers (port & 0xE3 == 0xA3) and the control register (port & 0xFF == 0xE3). */
uint8_t zr_divide_in(zr_divide *d, uint16_t port);
void zr_divide_out(zr_divide *d, uint16_t port, uint8_t value);

/* 1 while a drive is at work (its light); the sectors drive `unit` has moved (not saved). */
int zr_divide_light(const zr_divide *d);
uint32_t zr_divide_activity(const zr_divide *d, unsigned unit);

/* The chunks "DIVI" (the paging, the RAM, the latch) and "ATA " (the channel and its drives). Loading refuses a state whose drives
 * are not the ones attached. */
void zr_divide_save(const zr_divide *d, struct zr_state_writer *w);
int zr_divide_load(zr_divide *d, struct zr_state_reader *r);

#endif /* ZR_HW_DIVIDE_H */
