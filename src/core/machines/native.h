/* What the two native tape machines have in common, the aMIC and the PRAE-M (amic.c, prae.c): the Z80 at 2.5 MHz from the 10 MHz master
 * clock, the 16 KiB ROM at 0000h and the RAM above it in one 64 KiB array, the monochrome bitmap video (hw/monovideo), the 1-bit
 * sound, the tape recorder on its slot with loader acceleration, the run loop, the keyboard's timing, and the common part of a state.
 * The board's own part (the memory map above 4000h, the ports, the keyboard matrix, the serial line, the text read back from the
 * screen, the options) is the machine's.
 *
 * A machine's struct starts with a zr_native, so the pointer the registry holds is both; the functions below that have the shape of
 * an entry of the impl table (machine_impl.h) are the machine's entries as they are.
 *
 * ZetRo's own code. */
#ifndef ZR_MACHINES_NATIVE_H
#define ZR_MACHINES_NATIVE_H

#include "machine_impl.h"
#include "media/tape.h"
#include "beeper.h"
#include "bus.h"
#include "loadaccel.h"
#include "monovideo.h"
#include "sched.h"
#include "z80.h"

#define ZR_NATIVE_CLOCK_HZ 10000000u            /* the master clock: the 640-dot line of 64 microseconds */
#define ZR_NATIVE_TICKS_PER_TSTATE 4u           /* the CPU's 2.5 MHz */
#define ZR_NATIVE_AUDIO_RATE 48000u
#define ZR_NATIVE_KEY_QUEUE 256
#define ZR_NATIVE_ROM 0x4000u
#define ZR_NATIVE_PLAY_DELAY 3000000u           /* 300 ms: the time a command typed takes to start (the monitor's key click is 90 ms) */

typedef struct zr_native zr_native;

struct zr_native {
    const zr_family *family;
    unsigned model;
    char description[160], error[160];
    int tape_sound, accelerate;
    /* the board */
    zr_hw_host host;
    zr_sched sched;
    zr_tclock clock;
    uint64_t target;                            /* where the runs asked so far end */
    zr_bus bus;
    zr_z80_bus cpu_bus;
    zr_z80 z80;
    zr_loadaccel accel;
    zr_monovideo video;
    zr_beeper beeper;
    uint8_t rom[ZR_NATIVE_ROM];
    uint8_t rom_hash[32];
    uint8_t ram[65536];                         /* the memory space's RAM: the machine maps what it has of it */
    /* The port cycle that reads the tape input: (port & ear_mask) == ear_value. */
    uint16_t ear_mask, ear_value;
    /* The tape recorder */
    zr_dev tape_dev;
    zr_media *tape_media;
    zr_tape *tape;
    zr_tape_deck deck;
    uint8_t mic;                                /* the level last put on the MIC output */
    /* The keyboard's timing (zr_native_key): key changes wait for their time in a queue. */
    zr_dev key_dev;
    uint64_t key_t[ZR_NATIVE_KEY_QUEUE];
    uint8_t key_n[ZR_NATIVE_KEY_QUEUE], key_down[ZR_NATIVE_KEY_QUEUE];
    unsigned key_count;
    uint64_t key_arrival, key_group;            /* when the last change came and when its group is (or was) applied */
    uint64_t key_press_gap, key_release_gap;    /* the least time between a change and the next press, and the next release */
    uint64_t key_chord_gap;                     /* the time between the changes of one instant (a shift key goes down before the key it shifts) */
    uint64_t key_last;                          /* when a key change was last applied */
    uint8_t play_pending;                       /* PLAY waits for the keys typed to be in (zr_native_control) */
    /* The machine's hooks. */
    void (*key_apply)(zr_native *n, unsigned key, int down);
    void (*line)(zr_native *n, void *dev, int pin, int state);      /* an output pin of a chip other than the video (may be NULL) */
};

/* The guest time of the cycle in progress (or of the event running), and `tstates` T-states into the cycle starting now. */
uint64_t zr_native_now(const zr_native *n);
uint64_t zr_native_at(const zr_native *n, unsigned tstates);

/* Builds the common part: the machine's ROM `rom_key` (romdb.h) copied in, the scheduler, the clock, the bus with the ROM at 0000h
 * (nothing else mapped, no port decoder), the CPU, the video (reading the bitmap at ram + video_base; `interrupt` for the PRAE's
 * frame interrupt), the sound and the tape recorder. 0 with the reason in error. The machine then adds its map and its chips
 * (n->host is theirs to use) and calls zr_native_reset(n, 1). */
int zr_native_init(zr_native *n, const zr_machine_setup *setup, const char *rom_key, uint32_t video_base, int interrupt, uint16_t ear_mask,
                   uint16_t ear_value, char *error, size_t capacity);
/* The bytes of the bitmap's pages are watched: a write catches the video up first. */
void zr_native_watch_video(zr_native *n, uint16_t first, uint32_t size);
/* The part of a reset the two share (the machine does its own too): the CPU, the video, the sound, the keyboard's queue. */
void zr_native_reset(zr_native *n, int power_on);

/* The tape's EAR level now (high with no tape running); the MIC output changed to `level` at guest time t. */
int zr_native_ear(zr_native *n);
void zr_native_mic(zr_native *n, int level, uint64_t t);
/* The speaker output (a 1-bit source of the sound) changed at guest time t. */
void zr_native_speaker(zr_native *n, int level, uint64_t t);

/* A key of the machine's table pressed or released, at the keyboard's pace: a change is applied (through n->key_apply) at once, or,
 * when it comes too soon after the one before (the ROM scans slowly: the aMIC's clicks for 90 ms after every key and no key is read
 * meanwhile), when the gaps `key_press_gap` and `key_release_gap` allow; changes of the same instant (a shift key and the key it
 * shifts) follow one another `key_chord_gap` apart (the PRAE's ROM sees a shift key only when it is pressed before the key it shifts). Not original: it is what a typist's pace needs of the ROM's. */
void zr_native_key(void *self, unsigned key, int down);

/* The impl entries the two machines share. */
void zr_native_destroy(void *self);
const char *zr_native_description(const void *self);
const char *zr_native_error(const void *self);
void zr_native_nmi(void *self);
uint32_t zr_native_clock(const void *self);
zr_run_status zr_native_run(void *self, uint64_t ticks);
uint64_t zr_native_time(const void *self);
uint64_t zr_native_frame_period(const void *self);
int zr_native_frame(void *self, zr_frame *frame);
uint32_t zr_native_audio_rate(const void *self);
size_t zr_native_audio_pull(void *self, int16_t *stereo, size_t frames);
int zr_native_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity);
zr_media *zr_native_media(const void *self, unsigned slot);
int zr_native_eject(void *self, unsigned slot);
unsigned zr_native_activity(const void *self);
int zr_native_control(void *self, unsigned slot, unsigned command, uint32_t value);
unsigned zr_native_tape_options(void *self, unsigned slot, unsigned bits);
struct zr_tape_deck *zr_native_tape_deck(void *self, unsigned slot);
uint32_t zr_native_rom_hashes(const void *self, uint8_t *hashes, uint32_t max);
void zr_native_set_trace(void *self, zr_trace_fn fn, void *ctx);
int zr_native_peek(void *self, uint32_t address, uint8_t *value);
int zr_native_poke(void *self, uint32_t address, uint8_t value);

/* The common part of a state: the clock, the CPU, the RAM, the video, the sound, the tape's position, the keyboard's queue. The
 * machine saves its own chunks too; on a load this comes first (the chips' loads arm their deadlines at the time it restores). */
void zr_native_save(const zr_native *n, zr_state_writer *w);
int zr_native_load(zr_native *n, zr_state_reader *r);

/* Where the monochrome video's lit colour comes from: 0 white, 1 green. */
uint32_t zr_native_phosphor(unsigned which);

#endif /* ZR_MACHINES_NATIVE_H */
