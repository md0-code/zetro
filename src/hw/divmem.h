/* ZetRo - the memory of the DivIDE and DivMMC interfaces (not original to any machine ZetRo runs: added interfaces, off in a
 * new configuration; spectrum.h plugs them into the Spectrum-compatible boards). The two interfaces page the same way and
 * differ in the RAM they carry and in the ports, so the paging is here once; divide.h and divmmc.h add the ports and the
 * drive or card.
 *
 * Written from the DivIDE's published hardware description (the author's documentation of the programming model and of the
 * memory, velesoft.speccy.cz/zx/divide) and the DivMMC's own (the same paging with more RAM).
 *
 * What the interface holds: an 8 KiB EEPROM (the firmware, ESXDOS; read-only here: it is the file fetched into roms/), and
 * `banks` banks of 8 KiB RAM (the DivIDE's 4, the DivMMC's 16). The control register (port E3h, write-only):
 *   bit 7 CONMEM   the interface's memory is mapped by hand: the EEPROM at 0000h-1FFFh, the bank BANK at 2000h-3FFFh (always
 *                  writable), whatever the automatic mapping is doing;
 *   bit 6 MAPRAM   it can only be set: only power on clears it (a reset does not). While the memory is mapped by the automatic
 *                  mapping, RAM bank 3 replaces the EEPROM at 0000h-1FFFh, write-protected;
 *   low bits BANK  the bank at 2000h-3FFFh (bits 1-0 on the DivIDE, bits 3-0 on the DivMMC).
 * Priority, lowest to highest: the EEPROM, MAPRAM, CONMEM.
 *
 * The automatic mapping watches the processor's opcode fetches (hw.h, zr_cpu_hooks):
 *   - an opcode fetched at 0000h, 0008h, 0038h, 0066h, 04C6h or 0562h maps the memory in after that fetch (the opcode itself is
 *     the board's ROM's, the rest of the instruction and the next instruction come from the interface);
 *   - an opcode fetched at 3D00h-3DFFh maps it in at once (that opcode is the interface's);
 *   - an opcode fetched at 1FF8h-1FFFh (the "off area") maps it out after that fetch (the instruction's own operands are the
 *     board's ROM's again).
 * The board may refuse the mapping while its own ROM is not at 0000h (a 128K board with another ROM page, RAM at 0000h): its
 * `automap_ok` callback.
 *
 * The NMI button: pressing it gives the processor an NMI unless the interface's memory is mapped (the documents disable the
 * button from an entry point's fetch until the off area's, which keeps an NMI from nesting; here it is the same while the memory
 * is mapped).
 *
 * Reset: the control register's CONMEM and BANK clear, the automatic mapping goes out; MAPRAM and the RAM stay (power on clears
 * them). The documents do not describe the reset button; this is nominal.
 *
 * Time: nothing here has any. Portable C11. ZetRo's own code. */
#ifndef ZR_HW_DIVMEM_H
#define ZR_HW_DIVMEM_H

#include "hw.h"

#define ZR_DIVMEM_EEPROM 0x2000u                /* the firmware: 8 KiB */
#define ZR_DIVMEM_BANK 0x2000u                  /* one RAM bank: 8 KiB */
#define ZR_DIVMEM_MAX_BANKS 16u

typedef struct zr_divmem_board {
    void *ctx;
    void (*map)(void *ctx);                     /* what 0000h-3FFFh shows changed: ask zr_divmem_page again */
    int (*automap_ok)(void *ctx);               /* NULL: always; else 1 while the board's own ROM is at 0000h */
    void (*nmi)(void *ctx);                     /* the NMI line */
} zr_divmem_board;

typedef struct zr_divmem {
    zr_divmem_board board;
    const uint8_t *eeprom;                      /* ZR_DIVMEM_EEPROM bytes, the board's */
    unsigned banks;                             /* 4 or 16 */
    uint8_t conmem, mapram, bank;               /* the control register */
    uint8_t automap;                            /* the automatic mapping has the memory in */
    uint8_t ram[ZR_DIVMEM_MAX_BANKS * ZR_DIVMEM_BANK];
    zr_cpu_hooks hooks;
    uint8_t watch[8192];
} zr_divmem;

/* The interface with `banks` banks (4 or 16) and its firmware (kept by the caller: at least ZR_DIVMEM_EEPROM bytes); the
 * hooks table is ready (zr_divmem_hooks). zr_divmem_reset brings it to power on (power_on 1: MAPRAM and the RAM cleared) or
 * to the reset line's state. Neither calls board.map: the board asks for the pages after it. */
void zr_divmem_init(zr_divmem *d, const zr_divmem_board *board, const uint8_t *eeprom, unsigned banks);
void zr_divmem_reset(zr_divmem *d, int power_on);
const zr_cpu_hooks *zr_divmem_hooks(zr_divmem *d);

/* The control register written (port E3h): the memory the new value shows is announced through board.map. */
void zr_divmem_control(zr_divmem *d, uint8_t value);
/* 1 while the interface's memory is at 0000h-3FFFh. */
int zr_divmem_mapped(const zr_divmem *d);
/* What 1 KiB page `page` (0-15) of 0000h-3FFFh is now: 0 (the board's own), or 1 with *bytes the page's bytes (read directly;
 * they change only by a write to the page or a call of the interface, each followed by board.map when the map changes). */
int zr_divmem_page(const zr_divmem *d, unsigned page, const uint8_t **bytes);
/* A write of the processor's to 0000h-3FFFh (the board's map decides where else it lands): the interface's RAM takes it where
 * it is mapped and writable. */
void zr_divmem_write(zr_divmem *d, uint16_t address, uint8_t value);
/* The NMI button: the NMI line goes unless the memory is mapped; 1 when it went. */
int zr_divmem_button(zr_divmem *d);

/* The state as the fields of a chunk the caller opened, and back (0: damaged). The firmware is the file's. */
void zr_divmem_save(const zr_divmem *d, struct zr_state_writer *w);
int zr_divmem_load(zr_divmem *d, struct zr_state_reader *r);

#endif /* ZR_HW_DIVMEM_H */
