/* ZetRo - the ROM tape traps of the Spectrum-compatible boards ("Fast tape
 * loading"; not original).
 *
 * The 48K BASIC ROM loads and saves a block of bytes through two routines that every Spectrum-
 * compatible ROM of this project copies: the byte loader, entered with IX the address, DE the number
 * of data bytes, A the flag byte expected and carry set for LOAD (clear for VERIFY), and the byte
 * saver, entered with IX, DE and A the flag byte to write. Both finish through one exit that gives the
 * border back, enables the interrupts, tests BREAK and returns to the caller. They take the real
 * time of the tape: seconds for a block.
 *
 * A trap is the emulator doing, at a routine's entry, what the routine would have done: the loader's
 * trap takes the block the recorder would play next (a standard-speed data block) and does with its
 * bytes what the loader does with the bytes it measures; the saver's trap puts the block the saver
 * would write onto the tape. The registers and flags are left as the routine leaves them on that
 * path, and the CPU continues at the routine's own exit, so the ROM's code gives the border and the
 * interrupts back. The ROM is never changed.
 *
 * What the trap does not do is time: it takes ZR_TRAPS_NOMINAL_TSTATES of guest time, not the seconds
 * the routine takes, and the border stripes and the sound of the load are not made. What it cannot
 * know is the loader's timing counter and edge state: B and C are left as a load of a block played from
 * its first pulse leaves them (see traps.c).
 *
 * The routines are recognised by their content: at init the board gives the ROM as the CPU sees it
 * from 0000h, and a routine is found where the bytes of the routine, as the ROM of the JET has them,
 * are (a signature of its entry, then its whole code by a hash, and the exit by its bytes). A ROM
 * whose routines differ or sit elsewhere gets no trap for them and loads in real time. The trap
 * checks the live bytes at the entry when it fires, so a ROM paged out never fires it.
 *
 * Like every chip it talks to the board through a host table (here the memory, the BREAK key and the
 * tape) so that it can be tested against a fake one. ZetRo's own code, written from the disassembly of
 * the JET ROM's own tape routines. No other emulator's source consulted. */
#ifndef ZR_TRAPS_H
#define ZR_TRAPS_H

#include "hw.h"
#include "z80.h"

/* Guest time a trap takes, in T-states of the CPU (about 0.3 ms). */
#define ZR_TRAPS_NOMINAL_TSTATES 1000u

typedef struct zr_traps_host {
    void *ctx;
    /* A byte of the memory space as the CPU reads it (no side effects), and written as the CPU writes it. */
    uint8_t (*read)(void *ctx, uint16_t address);
    void (*write)(void *ctx, uint16_t address, uint8_t value);
    /* 1 while the key the ROM's BREAK test reads (SPACE) is down: the ROM's own routine then runs, and reports it. */
    int (*break_held)(void *ctx);
    /* The loader. The block the recorder would play to the loader next (the position decides which): 1 with its bytes, flag
     * byte first and parity byte last, valid until block_done; 0 when the loader would see no standard-speed data block next
     * (no tape, a pause, a turbo block, the end of the tape): the ROM then runs in real time. block_done: the loader took
     * the block, the recorder moves past it. */
    int (*block_next)(void *ctx, const uint8_t **bytes, size_t *length);
    void (*block_done)(void *ctx);
    /* The saver. Puts a standard-speed block (flag byte first, parity byte last) on the tape: 1 when it did; 0 when nothing
     * can take it (no tape in the slot, a file that cannot be written): the ROM then saves in real time, to nobody. */
    int (*block_put)(void *ctx, const uint8_t *bytes, size_t length);
} zr_traps_host;

typedef struct zr_traps {
    zr_traps_host host;
    zr_z80 *cpu;
    uint16_t load, load_exit;               /* the loader's entry and the exit it returns through, where found */
    uint16_t save, save_exit;
    uint8_t has_load, has_save;             /* the routine was found in the ROM */
    uint8_t load_on, save_on;               /* the trap is switched on */
    uint8_t watch[8192];                    /* the CPU hook's bitmap: the entries of the traps that are on */
    zr_cpu_hooks hooks;
} zr_traps;

/* Looks for the loader and the saver in `rom` (`size` bytes as the CPU sees them from 0000h; only the first 16 KiB are
 * searched). Both traps start off. */
void zr_traps_init(zr_traps *t, zr_z80 *cpu, const zr_traps_host *host, const uint8_t *rom, size_t size);
/* The same for a BASIC that has come into the RAM (from tape): a routine counts when its first bytes and its exit are the JET's
 * whatever the rest of its code is (a BASIC may patch a jump into one; the live check at the entry, when the trap fires, is
 * the same as for a ROM). */
void zr_traps_init_loose(zr_traps *t, zr_z80 *cpu, const zr_traps_host *host, const uint8_t *memory, size_t size);
/* Switches the load and the save trap on or off; a trap whose routine was not found stays off. */
void zr_traps_enable(zr_traps *t, int load, int save);
/* The table for zr_z80's `hooks`: the entries of the traps that are on are watched. NULL when none is. */
const zr_cpu_hooks *zr_traps_hooks(const zr_traps *t);

#endif /* ZR_TRAPS_H */
