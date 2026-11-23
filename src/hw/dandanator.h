/* ZetRo - the ZX Dandanator Mini cartridge (not original to any machine ZetRo runs: an added interface, off in a new
 * configuration; spectrum.h plugs it into the Spectrum-compatible boards).
 *
 * The cartridge is a 512 KiB flash chip (an SST39SF040: 128 sectors of 4 KiB) seen as 32 slots of 16 KiB, and a PIC
 * that watches the processor's writes to the ROM area and drives the flash's upper address lines and the edge
 * connector's ROMCS, RESET and NMI lines:
 *   - while a slot is selected the cartridge answers reads of 0000h-3FFFh with it in place of the board's ROM; "slot 32"
 *     (the hidden slot) takes it out of the way, the board's own ROM shows again;
 *   - a command is a train of write pulses to 0000h-0003h, counted by the PIC; a pause in the pulses ends a number. A
 *     command below 40 is one number (1-33 select slot 0-32, 34 the hidden slot with commands locked, 36 a reset, 39
 *     the PIC's pointers cleared); a special command (40 and up) is three numbers, the command and its two data, then
 *     a confirmation, a write to 0000h; it acts at once, or (36 and 40) about 10 microseconds later (the PIC's delay:
 *     the program that confirmed it runs on from RAM meanwhile);
 *   - 40 selects a slot (data 1, 1-32; 0 is slot 0) with flags in data 2: bit 0 reset, bit 1 NMI, bit 2 commands
 *     locked, bit 3 commands disabled until reset; 46 with two equal data locks (1), unlocks (16) or disables (31);
 *     a locked cartridge takes only 46, a disabled one nothing (slot 0 selected unlocks it); 41, 42, 43, 45, 49 and 50
 *     write the PIC's registers, which read back over slot 0 at (command x 100h) + data 2 (2900h-29FFh read 00h where
 *     nothing was written), 50 also the PIC's RAM; 48 puts the PIC in its flash write mode for one sector (data 1 16:
 *     erase, 32: program; data 2 the sector, 0-127);
 *   - in the write mode the PIC passes the next writes to the flash, driving A14-A18 itself: the command cycles of the
 *     flash's sequences at the JEDEC addresses (the CPU writes 1555h and 2AAAh, the PIC makes them 5555h and 2AAAh),
 *     the last cycle in the sector's slot (erase: six cycles; program: four cycles a byte, 4096 bytes, the sector);
 *   - the flash itself as its data sheet has it: the unlock cycles, byte program (bits only go from 1 to 0), sector
 *     erase and chip erase to FFh, the software ID, and the busy time of each, during which reads give the status
 *     (DQ7 the complement of the data's bit 7, DQ6 toggling) and writes are ignored;
 *   - the programming switch (ZetRo's, as the source module's): with it off the flash takes no write; with it on the
 *     cartridge also starts in the hidden slot, so that a writer can be loaded from tape. A blank set (all FFh) starts
 *     in the hidden slot too: there is nothing in slot 0 to run.
 * Times are in master-clock ticks (hw.h); the chip's one deadline is the earliest of the PIC's pause timeout, its delay
 * and the flash's busy time.
 *
 * Written from the Dandanator developer documentation: the PIC counts pulses in guest time, the flash is the data sheet's
 * state machine, and the PIC's write mode is ZetRo's reading of command 48. Portable C11. */
#ifndef ZR_HW_DANDANATOR_H
#define ZR_HW_DANDANATOR_H

#include "hw.h"

#define ZR_DANDANATOR_SLOTS 32u
#define ZR_DANDANATOR_SLOT_SIZE 0x4000u
#define ZR_DANDANATOR_SIZE (ZR_DANDANATOR_SLOTS * ZR_DANDANATOR_SLOT_SIZE)     /* 524288 */
#define ZR_DANDANATOR_HIDDEN 32u                /* the slot that is the board's own ROM */

/* What a file is: a set (512 KiB with "v" or "V" and printable characters at 3FE0h, the menu's version), a blank set
 * (512 KiB of FFh) or neither. */
enum { ZR_DANDANATOR_NOT_A_SET, ZR_DANDANATOR_SET, ZR_DANDANATOR_BLANK };
int zr_dandanator_detect(const uint8_t *data, size_t size);

/* What 0000h-3FFFh shows for a 1 KiB page (zr_dandanator_page). */
enum { ZR_DANDANATOR_OUT, ZR_DANDANATOR_DIRECT, ZR_DANDANATOR_STATUS };

/* The board's side: the cartridge's lines and its file. */
typedef struct zr_dandanator_board {
    void *ctx;
    void (*map)(void *ctx);                 /* what the cartridge shows changed: ask zr_dandanator_page again */
    void (*reset)(void *ctx);               /* the cartridge pulls RESET (the board resets, not the cartridge) */
    void (*nmi)(void *ctx);                 /* ... NMI */
    void (*changed)(void *ctx);             /* the flash's content changed: the file is to be written */
    int (*writable)(void *ctx);             /* the file can be written now */
} zr_dandanator_board;

typedef struct zr_dandanator {
    zr_dev dev;
    zr_dandanator_board board;
    uint32_t clock_hz;
    uint8_t *set;                           /* the 512 KiB (the file's bytes, the board's), NULL: no cartridge */
    uint8_t program;                        /* the programming switch */
    uint8_t blank;                          /* the set was blank when it went in */
    /* the PIC */
    uint8_t slot;                           /* selected: 0-31, ZR_DANDANATOR_HIDDEN */
    uint8_t mapped;                         /* shown (the selected one, after the delay of 36 and 40) */
    uint8_t sleep;                          /* 04h commands locked, 08h disabled */
    uint8_t cmd[4], stage;                  /* the numbers counted so far; stage odd while pulses come */
    uint8_t pending;                        /* 1: show `slot` at apply_at; 2 then a reset, 4 an NMI */
    uint64_t pulse_end;                     /* when the pause after the last pulse ends a number (ZR_NEVER: none) */
    uint64_t apply_at;
    uint8_t wmode, wsector;                 /* the flash write mode of command 48: 0 none, 1 erase, 2 program */
    uint32_t wcycle;                        /* cycles passed to the flash in it */
    uint8_t reg[0x0A00];                    /* the PIC's registers over slot 0's 2900h-32FFh, and which were written */
    uint8_t reg_valid[0x0A00 / 8];
    uint8_t pic_ram[256];
    uint16_t pointer0, pointer1, mode;      /* command 39 and 45's */
    /* slot 0's 2800h-33FFh as the processor reads it: the flash's bytes with the registers over them */
    uint8_t view[0x0C00];
    /* the flash */
    uint8_t fstate;                         /* the step of a command sequence */
    uint8_t fid;                            /* software ID mode */
    uint8_t fdata, ftoggle;                 /* busy: the byte being programmed (FFh erasing), DQ6 */
    uint64_t busy_until;                    /* 0: not busy */
} zr_dandanator;

void zr_dandanator_init(zr_dandanator *d, const zr_hw_host *host, const zr_dandanator_board *board, uint32_t clock_hz,
                        int program);
/* The set goes in (the bytes stay the board's and must be ZR_DANDANATOR_SIZE long), or out (NULL). The cartridge then
 * starts afresh as at power on. */
void zr_dandanator_insert(zr_dandanator *d, uint8_t *set);
/* Power on, or the board's RESET line: the PIC starts again in slot 0 (the hidden slot with the programming switch on
 * or a blank set), commands enabled; the flash back to reading. */
void zr_dandanator_reset(zr_dandanator *d);
/* A write of the processor's to 0000h-3FFFh (any board's mapping), at guest time `now`. */
void zr_dandanator_write(zr_dandanator *d, uint16_t address, uint8_t value, uint64_t now);
/* What 1 KiB page `page` (0-15) of 0000h-3FFFh is now: OUT (the board's ROM), DIRECT (*bytes; they change only by a
 * write or the chip's deadline, each followed by board.map) or STATUS (read through zr_dandanator_read). */
int zr_dandanator_page(const zr_dandanator *d, unsigned page, const uint8_t **bytes);
uint8_t zr_dandanator_read(zr_dandanator *d, uint16_t address);
/* The whole state but the set (the file's) as the chunk "DAN ", and back (0: the state has none, or a damaged one). */
void zr_dandanator_save(const zr_dandanator *d, struct zr_state_writer *w);
int zr_dandanator_load(zr_dandanator *d, struct zr_state_reader *r);

#endif /* ZR_HW_DANDANATOR_H */
