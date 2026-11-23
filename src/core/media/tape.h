/* Tapes: the formats TAP, TZX, PZX, CSW and WAV read
 * into a list of blocks, the deck that plays a tape as a stream of pulses
 * in guest time, recording of the machine's SAVE into new blocks, and
 * writing a tape back as TAP, TZX or PZX. ZetRo's own code, written from
 * the TZX 1.20, PZX 1.0 and CSW 1.01 and 2.00 specifications, the TAP
 * layout, RIFF WAVE and RFC 1951 and 1950.
 *
 * A tape is a copy: it keeps nothing of the zr_media it was read from, so
 * the media may change or go. The times of the formats are T-states of a
 * 3.5 MHz clock (ZR_TAPE_TSTATE_HZ); the deck turns them into guest time
 * (periods of the board's master clock) exactly, carrying the remainder,
 * so there is no drift however long the tape. Levels are 0 and 1; the
 * deck's level is the machine's EAR input, and the level fed to it while
 * recording is the MIC output.
 *
 * The tape's own functions are plain data work. The deck is advanced by
 * the guest time it is asked about, never by a clock of its own, and holds
 * no OS call. */
#ifndef ZR_TAPE_H
#define ZR_TAPE_H

#include <stddef.h>
#include <stdint.h>

#include "media.h"
#include "state.h"

#define ZR_TAPE_TSTATE_HZ 3500000u
#define ZR_TAPE_NEVER UINT64_MAX            /* "no edge" for zr_tape_deck_next_edge */
#define ZR_TAPE_NO_BLOCK 0xFFFFFFFFu

/* The format a tape was read from; the one it is written as is chosen at the writing. */
enum { ZR_TAPE_NONE, ZR_TAPE_TAP, ZR_TAPE_TZX, ZR_TAPE_PZX, ZR_TAPE_CSW, ZR_TAPE_WAV };

/* What a block is, for a browser to choose a mark for it. */
enum {
    ZR_TB_STANDARD,                         /* standard speed data (TZX 10, a TAP block) */
    ZR_TB_TURBO,                            /* turbo speed data (11) */
    ZR_TB_TONE,                             /* pure tone (12) */
    ZR_TB_PULSES,                           /* a pulse sequence (13, PZX PULS and PAUS, recorded pulses) */
    ZR_TB_DATA,                             /* pure data (14, PZX DATA) */
    ZR_TB_DIRECT,                           /* direct recording (15) */
    ZR_TB_AUDIO,                            /* a sampled recording (18, a CSW or WAV file) */
    ZR_TB_GENERAL,                          /* generalized data (19) */
    ZR_TB_PAUSE,                            /* silence (20) */
    ZR_TB_STOP,                             /* stop the tape (20 with no time, PZX STOP) */
    ZR_TB_STOP48,                           /* stop the tape on a 48K machine (2A) */
    ZR_TB_LEVEL,                            /* set the signal level (2B) */
    ZR_TB_GROUP,                            /* group start (21) */
    ZR_TB_GROUP_END,                        /* group end (22) */
    ZR_TB_JUMP,                             /* jump to block (23) */
    ZR_TB_LOOP,                             /* loop start (24) */
    ZR_TB_LOOP_END,                         /* loop end (25) */
    ZR_TB_CALL,                             /* call sequence (26) */
    ZR_TB_RETURN,                           /* return from sequence (27) */
    ZR_TB_SELECT,                           /* select block (28) */
    ZR_TB_TEXT,                             /* a text: description, message, PZX BRWS and title */
    ZR_TB_INFO,                             /* archive, hardware, custom info, glue and the rest */
    ZR_TB_KINDS
};

typedef struct zr_tape zr_tape;

/* --- the tape ------------------------------------------------------------------------ */

/* Builds a tape from a media's bytes, recognising the format by content (TAP has no
 * signature: it is taken by the file name's ending). An empty .tap, .tzx or .pzx file is
 * an empty tape, to record on. NULL with the reason for a file that is not a tape or is
 * damaged. */
zr_tape *zr_tape_open(const zr_media *media, char *error, size_t capacity);
/* An empty tape, to record on. */
zr_tape *zr_tape_new(void);
void zr_tape_close(zr_tape *tape);

int zr_tape_format(const zr_tape *tape);                            /* ZR_TAPE_* it was read from */
unsigned zr_tape_count(const zr_tape *tape);                        /* blocks */
int zr_tape_block_kind(const zr_tape *tape, unsigned index);        /* ZR_TB_*, -1 for a bad index */
/* A short name for the browser: the header's name of a standard header block ("Program:
 * GAME"), "Data 6912 bytes", "Turbo 41234 bytes", "Pause 1.0 s", a text block's text. */
void zr_tape_block_name(const zr_tape *tape, unsigned index, char *out, size_t capacity);
/* The length of a block's sound in T-states (3.5 MHz) as far as it can be told without
 * playing it: 0 for blocks that make none and for generalized data. */
uint64_t zr_tape_block_tstates(const zr_tape *tape, unsigned index);
/* The bytes of a standard-speed data block (a TZX 10 or TAP block, or a block 11 with the
 * ROM's own timings): the whole block as it is on the tape, flag byte first and parity
 * byte last, so that a ROM loading trap takes the block whole. `flag` is the first byte,
 * `pause_ms` the silence that follows. Returns 0 for a block that is not one (or a bad
 * index). The bytes stay valid until a recording is added to the tape. */
int zr_tape_block_data(const zr_tape *tape, unsigned index, const uint8_t **bytes, size_t *length,
                       unsigned *flag, unsigned *pause_ms);
/* 1 when a recording was added since the tape was read or last written. */
int zr_tape_modified(const zr_tape *tape);
/* Appends a standard-speed data block of `bytes` (flag byte first, parity byte last, as the ROM's saver writes them) with
 * `pause_ms` of silence after it, and marks the tape modified: what a ROM saving trap puts on the tape. 0 when the tape is
 * full or memory is out. */
int zr_tape_append_standard(zr_tape *tape, const uint8_t *bytes, size_t length, unsigned pause_ms);

/* --- writing ------------------------------------------------------------------------- */

/* Serialises the whole tape as ZR_TAPE_TAP, ZR_TAPE_TZX or ZR_TAPE_PZX: *bytes is malloc'd
 * (free() it), *size its length. A TAP holds standard-speed blocks only: any other block
 * that carries sound is left out and counted in *dropped (optional; PZX and TZX drop
 * nothing but what their format cannot say). 0 with the reason on failure. */
int zr_tape_write(const zr_tape *tape, int format, uint8_t **bytes, size_t *size, unsigned *dropped,
                  char *error, size_t capacity);
/* Writes the tape into a media in the format its file name's ending names (.tap, .tzx, .pzx),
 * and marks the media dirty for the shell to write back. 0 with the reason for another ending
 * (a CSW or WAV cannot be written) or on failure. Clears the tape's modified flag. */
int zr_tape_save_media(zr_tape *tape, zr_media *media, unsigned *dropped, char *error, size_t capacity);

/* --- the deck ------------------------------------------------------------------------ */

#define ZR_TAPE_STACK 8

/* Where the tape plays next: which block, and the loops and calls being run. */
typedef struct zr_tape_flow {
    uint32_t next;                          /* the next block to start */
    uint32_t depth;
    struct { uint32_t block; uint32_t left; uint8_t call; } frame[ZR_TAPE_STACK];
} zr_tape_flow;

/* Where inside a block the pulse generator is. */
typedef struct zr_tape_gen {
    uint32_t block;                         /* the block being played, ZR_TAPE_NO_BLOCK between blocks */
    uint8_t phase;
    uint32_t i, j, k;                       /* the position in the phase (what they count depends on the block) */
    uint64_t elapsed;                       /* T-states of the block generated so far */
} zr_tape_gen;

typedef struct zr_tape_deck {
    zr_tape *tape;                          /* borrowed; NULL: no tape */
    uint32_t clock;                         /* the master clock, Hz */
    int is_48k;                             /* "stop if 48K" blocks stop the tape */
    int playing, recording;
    /* The generator: the tape turned into stretches of constant level, one after the other. */
    zr_tape_flow flow;
    zr_tape_gen gen;
    uint8_t gen_level;                      /* the level of the next pulse */
    uint8_t end;                            /* 0 running, 1 a stop block was reached, 2 the tape ended */
    uint32_t spin;                          /* steps taken without time passing (an endless-loop guard) */
    uint64_t rem;                           /* the T-state to tick remainder, in T-state units of the clock */
    /* The stream: `level` holds until `edge` (guest time); then the pending stretch, if any. */
    uint8_t level;
    uint8_t fresh;                          /* nothing of the stream was shown yet since it was started */
    uint8_t has_pend, pend_level;
    uint64_t now, edge, rest;               /* last time asked; the end of the level; the part left while stopped */
    uint64_t pend_ticks, pend_elapsed;
    uint32_t pend_block;
    uint32_t shown_block;                   /* the block at the stream's position */
    uint64_t shown_elapsed;                 /* T-states into it */
    /* Recording: the pulses (T-states) between the MIC output's edges. */
    uint32_t *rec;
    size_t rec_n, rec_cap;
    uint8_t rec_started, rec_level, rec_first_level, rec_overflow;
    uint64_t rec_time, rec_rem;
} zr_tape_deck;

/* A deck for a board whose master clock runs at `clock_hz`. No tape, stopped. */
void zr_tape_deck_init(zr_tape_deck *deck, uint32_t clock_hz);
/* Frees what the deck holds (the recording buffer); the tape stays the caller's. */
void zr_tape_deck_release(zr_tape_deck *deck);
/* Puts a tape in (borrowed: close it only after taking it out), rewound; NULL takes it out. */
void zr_tape_deck_set_tape(zr_tape_deck *deck, zr_tape *tape);
/* Whether "stop if 48K" blocks stop the tape (the board's choice, off at first). */
void zr_tape_deck_set_48k(zr_tape_deck *deck, int is_48k);

/* The transport. `time` is the guest time of the key press. play returns 0 when there is no
 * tape, the tape is at its end or the deck is recording; stop and rewind stop the tape;
 * next, previous and seek move to a block and keep the tape running if it was (the block
 * numbers run 0..count, count being the end). rewind goes to the start. */
int zr_tape_deck_play(zr_tape_deck *deck, uint64_t time);
void zr_tape_deck_stop(zr_tape_deck *deck, uint64_t time);
void zr_tape_deck_rewind(zr_tape_deck *deck, uint64_t time);
void zr_tape_deck_next(zr_tape_deck *deck, uint64_t time);
void zr_tape_deck_previous(zr_tape_deck *deck, uint64_t time);
void zr_tape_deck_seek(zr_tape_deck *deck, uint32_t block, uint64_t time);

/* 1 while the tape runs. The deck stops by itself at the end of the tape and at a stop
 * block, at the time of that point; it notices when asked about a later time (zr_tape_deck_level
 * or zr_tape_deck_advance). */
int zr_tape_deck_playing(const zr_tape_deck *deck);
/* Brings the deck up to `time` (the time never goes back). */
void zr_tape_deck_advance(zr_tape_deck *deck, uint64_t time);
/* The block at the tape's position (the next one to play while no sound of a block is under
 * way) and the T-states into it. */
uint32_t zr_tape_deck_block(const zr_tape_deck *deck);
uint64_t zr_tape_deck_position(const zr_tape_deck *deck);
/* The first standard-speed data block at or after the position (see zr_tape_block_data),
 * ZR_TAPE_NO_BLOCK when there is none. */
uint32_t zr_tape_deck_find_data(const zr_tape_deck *deck);

/* The EAR level at a guest time. */
int zr_tape_deck_level(zr_tape_deck *deck, uint64_t time);
/* The guest time of the next level change, ZR_TAPE_NEVER when there is none (not playing, or
 * the level holds to the end of the tape). After 65536 stretches of the same level it returns
 * a time before which the level does not change instead of the change itself. */
uint64_t zr_tape_deck_next_edge(zr_tape_deck *deck);

/* Recording. record(on) starts taking the MIC output (it stops playback); record(off) closes
 * it and appends what was recorded to the tape as new blocks (a standard-speed block when the
 * pulses are a ROM-speed block, pulses otherwise) and leaves the deck at the end of the tape.
 * Returns 1 on success; 0 when there is no tape (on) or nothing could be added (off).
 * output() gives the MIC level whenever it may have changed, with its guest time. */
int zr_tape_deck_record(zr_tape_deck *deck, int on, uint64_t time);
void zr_tape_deck_output(zr_tape_deck *deck, int level, uint64_t time);
int zr_tape_deck_recording(const zr_tape_deck *deck);

/* --- the edges, read ahead -------------------------------------------------------------------- */

/* A reader of what a tape holds, for a board that wants to know what its loader will find (the PRAE-M's auto-load
 * reads the name of the first file): the stretches of constant level a deck on a master clock of `clock_hz` plays from the
 * start of the tape, in master-clock ticks, one after the other, through a deck of its own (the same generator and the same
 * rounding, so the edges are those the machine hears, WAV and every other format alike). Nothing of the machine's deck moves
 * and the tape is not written. */
typedef struct zr_tape_edges {
    zr_tape_deck deck;
    uint64_t time;                          /* guest time of the end of the last stretch given */
    int running;
} zr_tape_edges;

/* Starts at the tape's first block; `is_48k` as zr_tape_deck_set_48k. Nothing to release. */
void zr_tape_edges_begin(zr_tape_edges *edges, const zr_tape *tape, uint32_t clock_hz, int is_48k);
/* The next stretch: its level and its length in ticks (never 0). 0 when the tape ends or stops before another edge
 * (the stretch the level holds to the end is not given). */
int zr_tape_edges_next(zr_tape_edges *edges, int *level, uint64_t *ticks);

/* The deck's position and state, in the fields of a chunk the caller has begun. The tape is
 * not saved (a state names the media), nor is a recording under way. load returns 0 when the
 * fields do not fit the tape in the deck; the deck is then rewound. */
void zr_tape_deck_save(const zr_tape_deck *deck, zr_state_writer *w);
int zr_tape_deck_load(zr_tape_deck *deck, zr_state_reader *r);

#endif /* ZR_TAPE_H */
