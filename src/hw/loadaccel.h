/* ZetRo - loader acceleration ("Fast tape loading"; not original; ZetRo's own design).
 *
 * A tape loader, the ROM's or a program's own, spends nearly all its time in a short loop that reads the
 * tape input and counts until the level changes. Loader acceleration recognises such a loop by what it does
 * as it runs (there is no table of loaders and no byte pattern of any) and, instead of running its turns one
 * by one, moves the CPU straight to a later turn, leaving it, and the machine, exactly as running the turns
 * would have: with acceleration on or off the machine is in the same state at the same guest time.
 *
 * The board runs the CPU through zr_loadaccel_run_z80 (the scheduler's run, sched.h, with the recogniser
 * in it) and tells the recogniser, from its bus callbacks, of every read of the tape input
 * (zr_loadaccel_sample) and of everything else that touches the outside of the CPU: memory writes and
 * ports that may page memory (zr_loadaccel_changed), other port cycles and interrupt acknowledges
 * (zr_loadaccel_disturb). Deadlines that fall due count as disturbances too; a board whose deadlines
 * change memory calls zr_loadaccel_changed from them.
 *
 * The rule:
 *   - A turn is what the CPU runs from just after one read of the tape input to the end of the next one,
 *     when nothing but instructions ran in it (no write, no other port, no acknowledge, no deadline).
 *   - The instructions of the turn are decoded from memory (they cannot have changed: nothing wrote) and
 *     must all be of a small set that touches only registers, flags and plain memory reads; the read of
 *     the input must be the turn's last instruction and its only port cycle; no CPU hook may watch them.
 *   - The registers that change from turn to turn (the loop's counter: one register or a pair) are those
 *     read before written in the turn that differ between the two reads; a data-flow pass follows what
 *     depends on them. Instructions that depend on the counter may only be increments, decrements, DJNZ,
 *     copies, ALU operations, BIT and conditional branches; ZetRo evaluates exactly those (the "slice")
 *     for any value of the counter. Everything that does not depend on the counter repeats unchanged
 *     every turn and is not evaluated at all: it is as the turn just observed left it.
 *   - The turn after the jump is run by the CPU itself, so the flags and registers that depend on the
 *     counter but are overwritten in every turn before they are read (and before any exit the counter can
 *     cause) need no prediction.
 *   - A jump of j turns needs: turns 0..j-1 continue the loop for the counter values they start with
 *     (by the slice); j+1 turns end before the next deadline, the end of the run and the next tape edge;
 *     no cycle of them can be lengthened (the board's `quiet`, for contention); no interrupt can be taken.
 *     It sets the counter, R, the T-state counter and nothing else.
 *
 * Nothing of the recogniser is part of a state: zr_loadaccel_restart forgets everything (the board calls it
 * at the start of each run, so after a state is loaded, a poke or a key the recogniser starts afresh), and a
 * run never ends between a jump and the end of the turn that follows it.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_HW_LOADACCEL_H
#define ZR_HW_LOADACCEL_H

#include "hw.h"
#include "sched.h"
#include "z80.h"

#define ZR_LOADACCEL_PATH 32                /* the longest turn recognised, in instructions */

typedef struct zr_loadaccel_host {
    void *ctx;
    /* The byte at `address` when reading it is plain memory (no side effect, the same until something
     * writes it): 1 and *value; 0 otherwise. */
    int (*peek)(void *ctx, uint16_t address, uint8_t *value);
    /* The guest time before which the tape input's level cannot change (ZR_NEVER: never). */
    uint64_t (*level_until)(void *ctx);
    /* Optional (NULL: no cycle is ever lengthened): the first CPU T-state in [t, limit) at which a cycle
     * may be lengthened (contention), `limit` when there is none. */
    uint64_t (*quiet)(void *ctx, uint64_t t, uint64_t limit);
} zr_loadaccel_host;

/* A loop the recogniser has analysed: its path and what the analysis found. */
typedef struct zr_loadaccel_insn {
    uint16_t pc;
    uint8_t len, kind, op, dst, src, imm;
    uint8_t branch, cflag, csense, expect, mem;
    uint16_t target, rd, wr;
    uint8_t frd, fwr;
    int8_t disp;
} zr_loadaccel_insn;

typedef struct zr_loadaccel_loop {
    uint16_t pc[ZR_LOADACCEL_PATH];
    unsigned n;
    uint16_t code_first, code_last;         /* the addresses its instructions' bytes span (wrapping: the whole space) */
    uint16_t carried;                      /* registers read before written in the turn and written in it */
    uint8_t fcarried;                       /* the same of the flags */
    uint16_t seed;                          /* of these, the registers that differed between the two reads */
    uint8_t fseed;                          /* and the flags */
    int ok;                                /* 1: the loop can be jumped through */
    uint16_t counter, tainted;              /* registers: the counter; depending on it at the end of a turn */
    uint8_t tflags;                         /* flags depending on it at the end of a turn */
    uint8_t hi, lo, wide;                   /* the counter: one register (lo) or a pair (hi, lo); none: hi = lo = 0xFF */
    zr_loadaccel_insn slice[ZR_LOADACCEL_PATH];
    unsigned slice_n;
    zr_loadaccel_insn mem[ZR_LOADACCEL_PATH];   /* the instructions that read memory (checked again at each use) */
    unsigned mem_n;
} zr_loadaccel_loop;

typedef struct zr_loadaccel {
    zr_loadaccel_host host;
    /* Set by the board's bus callbacks during a step. */
    unsigned sampled;                       /* reads of the tape input */
    unsigned disturbed;                     /* anything else outside the CPU */
    /* The instructions run since the last read. */
    uint16_t path[ZR_LOADACCEL_PATH];
    unsigned n;
    /* The CPU just after the last read (or after the last jump). */
    int have_prev;
    zr_z80 prev;
    /* The last analysis, valid while nothing outside the CPU happened since. */
    int cached;
    zr_loadaccel_loop loop;
    /* Totals since the board was built. */
    uint64_t jumps, tstates;
} zr_loadaccel;

void zr_loadaccel_init(zr_loadaccel *a, const zr_loadaccel_host *host);
/* Forgets every observation (the start of a run). */
void zr_loadaccel_restart(zr_loadaccel *a);
/* From the board's bus callbacks: the cycle read the tape input; the cycle was something else outside the CPU that
 * changes neither memory nor the memory map (a port of its own, an acknowledge); the cycle may have changed memory or the
 * memory map (a write, a paging port): the loop analysed last is forgotten. */
static inline void zr_loadaccel_sample(zr_loadaccel *a) { ++a->sampled; }
static inline void zr_loadaccel_disturb(zr_loadaccel *a) { a->disturbed = 1; }
static inline void zr_loadaccel_changed(zr_loadaccel *a) { a->disturbed = 1; a->cached = 0; }
/* A memory write: the loop analysed last is forgotten only when the write lands on its code. */
static inline void zr_loadaccel_write(zr_loadaccel *a, uint16_t address) {
    a->disturbed = 1;
    if (address >= a->loop.code_first && address <= a->loop.code_last) a->cached = 0;
}

/* zr_sched_run_z80 (sched.h) with the recogniser: the same instructions, deadlines and result, with the loops
 * it recognises jumped through. */
uint64_t zr_loadaccel_run_z80(zr_loadaccel *a, zr_sched *s, zr_z80 *z, const zr_tclock *c, uint64_t until);

#endif /* ZR_HW_LOADACCEL_H */
