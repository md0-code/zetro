/* The tape's inside, shared by the format readers and writers, the pulse
 * generator and the deck (tape.h is the interface). ZetRo's own code. */
#ifndef ZR_TAPE_IMPL_H
#define ZR_TAPE_IMPL_H

#include <stdio.h>

#include "tape.h"

#define ZR_TAPE_MAX_BLOCKS (1u << 18)           /* a tape with more is refused */
#define ZR_TAPE_MAX_RUNS (1u << 24)             /* pulse runs over all the blocks */
#define ZR_TAPE_MAX_ARENA ((size_t)256 << 20)   /* bytes of data over all the blocks */

/* Flags of a block. */
enum {
    ZR_TF_LEVEL = 1,                    /* the block starts at `level` whatever came before */
    ZR_TF_KEEP = 2,                     /* the level after the block is that of its last pulse (no edge is owed) */
    ZR_TF_PAUSE = 4                     /* a pulse block that is a pause (for its name) */
};

/* `count` pulses of `duration` T-states, one after the other with the level toggling. */
typedef struct zr_pulse {
    uint32_t duration, count;
} zr_pulse;

typedef struct zr_tape_block {
    uint8_t kind;                       /* ZR_TB_* */
    uint8_t flags;                      /* ZR_TF_* */
    uint8_t used;                       /* data: bits used in the last byte, 1..8 */
    uint8_t level;                      /* the starting level (ZR_TF_LEVEL), the level of a level block */
    uint32_t pause;                     /* silence after the block, ms */
    uint64_t tstates;                   /* the block's length in T-states, as far as it is known (tape.h) */
    size_t off, len;                    /* arena: the data bytes, or the text of a text block */
    size_t raw, raw_len;                /* arena: the block as it is in a TZX file (0 long: none) */
    union {
        struct {                        /* STANDARD, TURBO, TONE, DATA: pilot, sync, bits, tail */
            uint32_t pilot, pilot_count, sync1, sync2, tail;
            uint16_t zero_n, one_n;     /* pulses of a 0 bit and of a 1 bit */
            uint8_t sync_n;             /* 0 or 2 sync pulses */
            size_t table;               /* arena: the 16-bit pulses of the 0 bit, then of the 1 bit */
        } d;
        struct { uint32_t sample; } direct;                     /* DIRECT: T-states per sample */
        struct { uint32_t first, count; } r;                    /* PULSES, AUDIO: runs of the tape's run array */
        struct {                        /* GENERAL */
            uint32_t totp, totd, asp, asd;
            uint8_t npp, npd, nb;
            size_t sym_p, prle, sym_d, stream;
        } g;
        struct { int32_t value; } f;    /* JUMP: the offset; LOOP: the repeats; CALL: how many calls */
    } u;
} zr_tape_block;

struct zr_tape {
    int format;
    int modified;
    zr_tape_block *blocks;
    uint32_t n, cap;
    uint8_t *arena;
    size_t arena_n, arena_cap;
    zr_pulse *runs;
    size_t runs_n, runs_cap;
    size_t rom_table;                   /* arena offset + 1 of the 855/1710 bit table, 0 until made */
};

/* Reports a failure in the reader's own `error` and `capacity`; the value is 0. */
#define ZR_TAPE_FAIL(...) (((capacity) ? (void)snprintf(error, capacity, __VA_ARGS__) : (void)0), 0)

static inline uint32_t zr_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static inline uint32_t zr_rd24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }
static inline uint32_t zr_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* --- the container (tape.c) ---------------------------------------------------------------- */

/* A zeroed block of this kind appended; NULL when the tape is full or memory is out. The pointer
 * is good until the next block is added. */
zr_tape_block *zr_tape_add_block(zr_tape *t, uint8_t kind);
/* Copies bytes into the arena; *off is where. 0 when the arena is full or memory is out. */
int zr_tape_put(zr_tape *t, const void *data, size_t n, size_t *off);
/* The table of the 0 and 1 bits made of two equal pulses each: zero, zero, one, one. */
int zr_tape_put_bits(zr_tape *t, uint32_t zero, uint32_t one, size_t *off);
/* The same table for the ROM's own 855 and 1710. */
int zr_tape_rom_bits(zr_tape *t, size_t *off);
/* Appends a run to the run array, joined to the one before when equal and not before `first`. */
int zr_tape_run(zr_tape *t, uint32_t first, uint32_t duration, uint32_t count);
/* Makes `b` a standard-speed data block over len bytes at `off` of the arena, with the ROM's
 * timings, the pilot of the flag byte and `pause_ms` of silence after. 0 when memory is out. */
int zr_tape_fill_standard(zr_tape *t, zr_tape_block *b, size_t off, size_t len, unsigned pause_ms);
/* Works out a block's length in T-states (block->tstates). */
void zr_tape_measure(const zr_tape *t, zr_tape_block *b);

/* --- readers: each fills an empty tape, 0 with the reason for a damaged file ---------------- */

int zr_tape_read_tap(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity);
int zr_tape_read_tzx(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity);
int zr_tape_read_pzx(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity);
int zr_tape_read_csw(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity);
int zr_tape_read_wav(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity);
/* Run-length coded CSW pulses (in samples at `rate`) become runs in T-states; the block's
 * first and count are set. Used by CSW files and TZX block 0x18. */
int zr_tape_csw_runs(zr_tape *t, const uint8_t *rle, size_t n, uint32_t rate, uint32_t *first, uint32_t *count,
                     char *error, size_t capacity);

/* --- the pulse generator (tape_gen.c) -------------------------------------------------------- */

/* A stretch of constant level. */
typedef struct zr_seg {
    uint8_t level;
    uint64_t tstates;
} zr_seg;

/* What zr_tape_flow_step found. */
enum { ZR_FLOW_BLOCK, ZR_FLOW_STOP, ZR_FLOW_STOP48, ZR_FLOW_TEXT, ZR_FLOW_END };
/* How it treats the blocks that are not sound: PLAY skips text and 2A, PLAY48K stops at 2A,
 * WALK reports 2A and the text and group blocks (for writers). */
enum { ZR_FLOW_PLAY, ZR_FLOW_PLAY48K, ZR_FLOW_WALK };

/* Follows jumps, loops and calls from the flow's next block to the next block that makes sound
 * (returned in *block, the flow's next is then the one after it) or to a stop or the end.
 * Level blocks are applied to *level; *spin counts the steps that took no time. */
int zr_tape_flow_step(const zr_tape *t, zr_tape_flow *f, int mode, uint8_t *level, uint32_t *spin, uint32_t *block);
/* Starts the generator on a block. */
void zr_tape_gen_begin(const zr_tape *t, zr_tape_gen *g, uint32_t block, uint8_t *level);
/* The next stretch of the block, 0 when it is over. *level is the level of the next pulse. */
int zr_tape_gen_next(const zr_tape *t, zr_tape_gen *g, uint8_t *level, zr_seg *seg);
/* Whether a block is a data block at the ROM's own timings (for traps and TAP). */
int zr_tape_block_is_rom_data(const zr_tape *t, const zr_tape_block *b);

/* --- recording (tape_rec.c) ------------------------------------------------------------------ */

/* Appends what was recorded: `pulses` are the T-states between edges, the first starting at
 * `first_level`; `tail` the T-states from the last edge to the end of the recording. 1 if blocks were added. */
int zr_tape_add_recording(zr_tape *t, const uint32_t *pulses, size_t n, uint8_t first_level, uint64_t tail);

#endif /* ZR_TAPE_IMPL_H */
