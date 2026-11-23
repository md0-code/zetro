/* What the aMIC and the PRAE-M have in common (native.h). Wired from the chips, as hc88.c and cobra.c are: nothing of the Spectrum
 * core is used, these machines are not Spectrums. ZetRo's own code. */
#include "native.h"
#include "romdb.h"
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ZR_BUS_TRACE_IN == ZR_TRACE_IO_READ && ZR_BUS_TRACE_OUT == ZR_TRACE_IO_WRITE &&
               ZR_BUS_TRACE_FETCH == ZR_TRACE_FETCH && ZR_BUS_TRACE_INTERRUPT == ZR_TRACE_INTERRUPT,
               "the bus traces in machine.h's kinds");

#define STATE_VERSION 1u

static void key_arm(zr_native *n);

/* The guest time of the cycle in progress (or of the event running). */
uint64_t zr_native_now(const zr_native *n) { return n->sched.in_event ? n->sched.event_time : zr_tclock_time(&n->clock, n->z80.t); }
/* The guest time `tstates` T-states into the cycle starting now. */
uint64_t zr_native_at(const zr_native *n, unsigned tstates) {
    return n->sched.in_event ? n->sched.event_time : zr_tclock_time(&n->clock, n->z80.t + tstates);
}

static uint64_t host_now(void *ctx) { return zr_native_now(ctx); }
static void host_schedule(void *ctx, void *dev, uint64_t when) { zr_sched_set(&((zr_native *)ctx)->sched, dev, when); }
static void host_line(void *ctx, void *dev, int pin, int state) {
    zr_native *n = ctx;
    if (dev == (void *)&n->video) {
        if (pin == ZR_MONOVIDEO_INT) zr_z80_int(&n->z80, state);
    } else if (n->line) n->line(n, dev, pin, state);
}

/* --- the CPU's bus -------------------------------------------------------------------------------------- */

static uint8_t cpu_read(void *ctx, uint16_t address, int m1) {
    (void)m1;
    return zr_bus_read(&((zr_native *)ctx)->bus, address);
}
static void cpu_write(void *ctx, uint16_t address, uint8_t value) { zr_bus_write(&((zr_native *)ctx)->bus, address, value); }
static uint8_t cpu_in(void *ctx, uint16_t port) { return zr_bus_in(&((zr_native *)ctx)->bus, port); }
static void cpu_out(void *ctx, uint16_t port, uint8_t value) { zr_bus_out(&((zr_native *)ctx)->bus, port, value); }
/* Nothing drives the data bus in the acknowledge cycle: FFh. */
static uint8_t cpu_ack(void *ctx) {
    zr_native *n = ctx;
    zr_bus_trace(&n->bus, ZR_BUS_TRACE_INTERRUPT, 0xFFu, 0xFFu);
    return 0xFFu;
}

/* Loader acceleration's view of the CPU's bus (hw/loadaccel.h): a read of the port that carries the tape input is a sample of it, every
 * other cycle that leaves the CPU (a write, another port, an acknowledge) tells the recogniser that more than a loop ran. The memory
 * map never changes on these machines, so a port cycle cannot change the code it analysed. */
static void cpu_write_accel(void *ctx, uint16_t address, uint8_t value) {
    zr_loadaccel_write(&((zr_native *)ctx)->accel, address);
    cpu_write(ctx, address, value);
}
static void cpu_out_accel(void *ctx, uint16_t port, uint8_t value) {
    zr_loadaccel_disturb(&((zr_native *)ctx)->accel);
    cpu_out(ctx, port, value);
}
static uint8_t cpu_ack_accel(void *ctx) {
    zr_loadaccel_disturb(&((zr_native *)ctx)->accel);
    return cpu_ack(ctx);
}
static uint8_t cpu_in_accel(void *ctx, uint16_t port) {
    zr_native *n = ctx;
    if ((port & n->ear_mask) == n->ear_value) zr_loadaccel_sample(&n->accel);
    else zr_loadaccel_disturb(&n->accel);
    return cpu_in(ctx, port);
}

/* Plain memory: a page the bus reads directly (ROM or RAM), which nothing but a write changes. */
static int accel_peek(void *ctx, uint16_t address, uint8_t *value) {
    const zr_native *n = ctx;
    const uint8_t *page = n->bus.rd[address >> ZR_BUS_PAGE_BITS];
    if (!page) return 0;
    *value = page[address & (ZR_BUS_PAGE_SIZE - 1u)];
    return 1;
}
/* The EAR level changes only at the recorder's edges, each the deadline of tape_dev (tape_arm). */
static uint64_t accel_level_until(void *ctx) {
    zr_native *n = ctx;
    return n->tape ? zr_sched_get(&n->sched, &n->tape_dev) : ZR_NEVER;
}

/* The video memory's pages: the picture is drawn up to the write before it lands (a write lands in the cycle's second T-state). */
static void video_write(void *ctx, uint16_t address, uint8_t value) {
    zr_native *n = ctx;
    (void)address;
    (void)value;
    zr_monovideo_catch_up(&n->video, zr_native_at(n, 1u));
}

/* --- the tape recorder ---------------------------------------------------------------------------------- */

/* The deadline of the next edge while the tape runs: the EAR sound follows it, and the deck stops at its end. */
static void tape_arm(zr_native *n) {
    uint64_t next = n->tape ? zr_tape_deck_next_edge(&n->deck) : ZR_NEVER;
    const uint64_t now = zr_native_now(n);
    if (next != ZR_NEVER && next <= now) next = now + 1u;
    zr_sched_set(&n->sched, &n->tape_dev, next);
}
static void tape_event(void *dev, uint64_t now) {
    zr_native *n = ((zr_dev *)dev)->host->ctx;
    const int level = zr_tape_deck_level(&n->deck, now);
    if (n->tape_sound) zr_beeper_set(&n->beeper, ZR_BEEPER_EAR, level, now);
    tape_arm(n);
}
static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void no_reset(void *dev) { (void)dev; }
static const zr_dev_ops TAPE_OPS = {.read = no_read, .write = no_write, .event = tape_event, .reset = no_reset};

/* With no tape in the recorder the input is idle, high (the diagnostics read it as FFh). */
int zr_native_ear(zr_native *n) { return n->tape ? zr_tape_deck_level(&n->deck, zr_native_now(n)) : 1; }

void zr_native_mic(zr_native *n, int level, uint64_t t) {
    n->mic = (uint8_t)(level != 0);
    zr_beeper_set(&n->beeper, ZR_BEEPER_MIC, level, t);
    if (n->tape && zr_tape_deck_recording(&n->deck)) zr_tape_deck_output(&n->deck, level, t);
}

/* A recording under way goes onto the tape, and the tape into its file. */
static void finish_recording(zr_native *n) {
    if (!n->tape || !zr_tape_deck_recording(&n->deck)) return;
    char why[128];
    if (zr_tape_deck_record(&n->deck, 0, zr_native_now(n)) && zr_tape_modified(n->tape))
        if (!zr_tape_save_media(n->tape, n->tape_media, NULL, why, sizeof why)) snprintf(n->error, sizeof n->error, "%s", why);
}

int zr_native_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    zr_native *n = self;
    (void)slot;
    if (n->tape_media) {
        if (capacity) snprintf(error, capacity, "The slot is not empty");
        return 0;
    }
    zr_tape *tape = zr_tape_open(media, error, capacity);
    if (!tape) return 0;
    n->tape_media = media;
    n->tape = tape;
    zr_tape_deck_set_tape(&n->deck, tape);
    tape_arm(n);
    return 1;
}

zr_media *zr_native_media(const void *self, unsigned slot) {
    (void)slot;
    return ((const zr_native *)self)->tape_media;
}

int zr_native_eject(void *self, unsigned slot) {
    zr_native *n = self;
    (void)slot;
    if (!n->tape_media) return 0;
    n->play_pending = 0;
    finish_recording(n);
    zr_tape_deck_set_tape(&n->deck, NULL);
    zr_tape_close(n->tape);
    n->tape = NULL;
    n->tape_media = NULL;
    tape_arm(n);
    if (n->tape_sound) zr_beeper_set(&n->beeper, ZR_BEEPER_EAR, 0, zr_native_now(n));
    return 1;
}

unsigned zr_native_activity(const void *self) {
    const zr_native *n = self;
    return n->tape && (zr_tape_deck_playing(&n->deck) || zr_tape_deck_recording(&n->deck)) ? 1u : 0u;
}

unsigned zr_native_tape_options(void *self, unsigned slot, unsigned bits) {
    zr_native *n = self;
    (void)slot;
    if (bits & ZR_TAPE_QUERY) return ZR_TAPE_SOUND | ZR_TAPE_ACCELERATE;
    if (n->tape_sound && !(bits & ZR_TAPE_SOUND)) zr_beeper_set(&n->beeper, ZR_BEEPER_EAR, 0, zr_native_now(n));
    n->tape_sound = (bits & ZR_TAPE_SOUND) != 0;
    n->accelerate = (bits & ZR_TAPE_ACCELERATE) != 0;
    return ZR_TAPE_SOUND | ZR_TAPE_ACCELERATE;
}

int zr_native_control(void *self, unsigned slot, unsigned command, uint32_t value) {
    zr_native *n = self;
    (void)slot;
    if (!n->tape) return 0;
    const uint64_t now = zr_native_now(n);
    if (command != ZR_MEDIA_RECORD) finish_recording(n);
    if (command != ZR_MEDIA_PLAY) n->play_pending = 0;
    switch (command) {
    case ZR_MEDIA_PLAY:
        /* PLAY is pressed once the keys typed have been read and the command has had time to start (the keys come at the
         * keyboard's pace, behind a typist's: a tape that starts earlier is past its leader before the machine listens). */
        if (n->key_count || now < n->key_last + ZR_NATIVE_PLAY_DELAY) {
            n->play_pending = 1;
            key_arm(n);
            return 1;
        }
        if (!zr_tape_deck_play(&n->deck, now)) return 0;
        break;
    case ZR_MEDIA_STOP: zr_tape_deck_stop(&n->deck, now); break;
    case ZR_MEDIA_REWIND: zr_tape_deck_rewind(&n->deck, now); break;
    case ZR_MEDIA_NEXT: zr_tape_deck_next(&n->deck, now); break;
    case ZR_MEDIA_PREVIOUS: zr_tape_deck_previous(&n->deck, now); break;
    case ZR_MEDIA_SEEK: zr_tape_deck_seek(&n->deck, value, now); break;
    case ZR_MEDIA_RECORD:
        if (zr_tape_deck_recording(&n->deck)) finish_recording(n);
        else if (!zr_tape_deck_record(&n->deck, 1, now)) return 0;
        else zr_tape_deck_output(&n->deck, n->mic, now);
        break;
    default: return 0;
    }
    tape_arm(n);
    return 1;
}

struct zr_tape_deck *zr_native_tape_deck(void *self, unsigned slot) {
    zr_native *n = self;
    (void)slot;
    return n->tape ? &n->deck : NULL;
}

/* --- the keyboard's timing ------------------------------------------------------------------------------ */

/* The next deadline of the keyboard: the next change, or (with the queue empty) the moment a PLAY that waited is pressed. */
static void key_arm(zr_native *n) {
    uint64_t when = ZR_NEVER;
    if (n->key_count) when = n->key_t[0];
    else if (n->play_pending) when = n->key_last + ZR_NATIVE_PLAY_DELAY;
    zr_dev_schedule(&n->key_dev, when);
}

static void key_pop(zr_native *n) {
    --n->key_count;
    memmove(n->key_t, n->key_t + 1, n->key_count * sizeof n->key_t[0]);
    memmove(n->key_n, n->key_n + 1, n->key_count);
    memmove(n->key_down, n->key_down + 1, n->key_count);
}

static void key_event(void *dev, uint64_t now) {
    zr_native *n = ((zr_dev *)dev)->host->ctx;
    while (n->key_count && n->key_t[0] <= now) {
        n->key_apply(n, n->key_n[0], n->key_down[0]);
        n->key_last = n->key_t[0];
        key_pop(n);
    }
    if (!n->key_count && n->play_pending && now >= n->key_last + ZR_NATIVE_PLAY_DELAY) {
        n->play_pending = 0;
        zr_tape_deck_play(&n->deck, now);
        tape_arm(n);
    }
    key_arm(n);
}
static const zr_dev_ops KEY_OPS = {.read = no_read, .write = no_write, .event = key_event, .reset = no_reset};

void zr_native_key(void *self, unsigned key, int down) {
    zr_native *n = self;
    const uint64_t now = zr_native_now(n);
    uint64_t t;
    if (n->key_arrival == now) {                /* the same instant: after the change before it */
        t = n->key_group + n->key_chord_gap;
        if (t < now) t = now;
    } else if (down) {                          /* a press: a press gap after the last change (the keys are all up for that long) */
        t = n->key_group + n->key_press_gap;
        if (t < now) t = now;
    } else {                                    /* a release: a release gap after the last change */
        t = n->key_group + n->key_release_gap;
        if (t < now) t = now;
    }
    n->key_group = t;
    n->key_arrival = now;
    if (!n->key_count && t <= now) {
        n->key_apply(n, key, down);
        n->key_last = now;
        return;
    }
    if (n->key_count == ZR_NATIVE_KEY_QUEUE) {
        /* Never so many in a typed line: the oldest change goes in at once. */
        n->key_apply(n, n->key_n[0], n->key_down[0]);
        n->key_last = now;
        key_pop(n);
    }
    n->key_t[n->key_count] = t;
    n->key_n[n->key_count] = (uint8_t)key;
    n->key_down[n->key_count++] = (uint8_t)(down != 0);
    key_arm(n);
}

/* --- time, the picture, the sound ----------------------------------------------------------------------- */

uint32_t zr_native_clock(const void *self) { (void)self; return ZR_NATIVE_CLOCK_HZ; }
uint64_t zr_native_time(const void *self) { return zr_native_now(self); }
uint64_t zr_native_frame_period(const void *self) { return zr_monovideo_frame_ticks(&((const zr_native *)self)->video); }
int zr_native_frame(void *self, zr_frame *frame) { return zr_monovideo_frame(&((zr_native *)self)->video, frame); }
const char *zr_native_description(const void *self) { return ((const zr_native *)self)->description; }
const char *zr_native_error(const void *self) { return ((const zr_native *)self)->error; }

zr_run_status zr_native_run(void *self, uint64_t ticks) {
    zr_native *n = self;
    n->target += ticks;
    uint64_t now;
    /* Loader acceleration starts afresh with every run (whatever the shell did between runs: a state loaded, a poke, a key), and
     * stands aside while the bus is traced (the trace would miss the reads it jumps over). */
    if (n->accelerate && !n->bus.trace) {
        zr_loadaccel_restart(&n->accel);
        now = zr_loadaccel_run_z80(&n->accel, &n->sched, &n->z80, &n->clock, n->target);
    } else now = zr_sched_run_z80(&n->sched, &n->z80, &n->clock, n->target);
    zr_monovideo_catch_up(&n->video, now);
    if (n->tape) zr_tape_deck_advance(&n->deck, now);
    return ZR_RUN_OK;
}

uint32_t zr_native_audio_rate(const void *self) { (void)self; return ZR_NATIVE_AUDIO_RATE; }
size_t zr_native_audio_pull(void *self, int16_t *stereo, size_t frames) {
    zr_native *n = self;
    return zr_beeper_render(&n->beeper, zr_native_now(n), stereo, frames);
}

void zr_native_nmi(void *self) { zr_z80_nmi(&((zr_native *)self)->z80); }

uint32_t zr_native_phosphor(unsigned which) { return which == 1u ? 0x33FF66u : 0xFFFFFFu; }

/* --- life cycle ----------------------------------------------------------------------------------------- */

void zr_native_watch_video(zr_native *n, uint16_t first, uint32_t size) { zr_bus_watch(&n->bus, first, size, 1); }

void zr_native_destroy(void *self) {
    zr_native *n = self;
    if (!n) return;
    /* For the checks (tools/verify.py): how much loader acceleration jumped, when ZETRO_ACCEL_STATS is set. */
    if (n->accelerate && getenv("ZETRO_ACCEL_STATS"))
        fprintf(stderr, "loadaccel: %llu jumps, %llu of %llu T-states jumped\n", (unsigned long long)n->accel.jumps,
                (unsigned long long)n->accel.tstates, (unsigned long long)n->z80.t);
    if (n->tape && n->tape_media) finish_recording(n);
    zr_tape_deck_release(&n->deck);
    zr_tape_close(n->tape);
    free(n);
}

/* The part of a reset the two machines share: power on clears the RAM and the processor wakes as it does; RESET is the
 * processor's pin and leaves the memory. The video's inversion and interrupt go back to the start and the sound to silence. */
void zr_native_reset(zr_native *n, int power_on) {
    const uint64_t now = zr_native_now(n);
    zr_monovideo_catch_up(&n->video, now);
    if (power_on) {
        /* The RAM powers up indeterminate: a fixed pseudo-random fill (the PRAE's firmware reads its key-click flag at 4020h without
         * ever setting it, so a RAM that powers up to zeros has no click, and the click is what holds the firmware back until a key is
         * let go: without it a key held for two frames of a typist is read twice). */
        uint32_t x = 0x2545F491u;
        for (size_t i = 0; i < sizeof n->ram; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            n->ram[i] = (uint8_t)(x >> 11);
        }
        zr_z80_power(&n->z80);
    } else zr_z80_reset(&n->z80);
    zr_z80_int(&n->z80, 0);
    n->video.dev.ops->reset(&n->video);
    zr_beeper_set(&n->beeper, ZR_BEEPER_SPEAKER, 0, now);
    zr_native_mic(n, 0, now);
    if (power_on) {
        n->key_count = 0;
        n->key_arrival = ZR_NEVER;
        n->key_group = 0;
        n->key_last = 0;
        n->play_pending = 0;
        zr_dev_schedule(&n->key_dev, ZR_NEVER);
    }
}

int zr_native_init(zr_native *n, const zr_machine_setup *setup, const char *rom_key, uint32_t video_base, int interrupt, uint16_t ear_mask,
                   uint16_t ear_value, char *error, size_t capacity) {
    const zr_known_rom *rom = zr_romdb_find(rom_key);
    const zr_rom_file *file = rom && setup->roms ? zr_rom_index_find(setup->roms, rom) : NULL;
    if (!file || !file->data || file->size != ZR_NATIVE_ROM) {
        if (capacity) snprintf(error, capacity, "The ROM %s is missing", rom ? rom->file : rom_key);
        return 0;
    }
    n->family = setup->family;
    n->model = setup->model;
    n->ear_mask = ear_mask;
    n->ear_value = ear_value;
    n->key_arrival = ZR_NEVER;
    memcpy(n->rom, file->data, sizeof n->rom);
    zr_sha256(n->rom, sizeof n->rom, n->rom_hash);
    /* The board: host, scheduler, clock. */
    n->host.ctx = n;
    n->host.now = host_now;
    n->host.schedule = host_schedule;
    n->host.line = host_line;
    zr_sched_init(&n->sched);
    zr_tclock_init(&n->clock, ZR_NATIVE_TICKS_PER_TSTATE, 0, 0);
    /* The bus: the ROM, the pages of the video watched by the machine (zr_native_watch_video). */
    zr_bus_init(&n->bus);
    zr_bus_set_hook(&n->bus, video_write, n);
    zr_bus_map_rom(&n->bus, 0x0000u, ZR_NATIVE_ROM, n->rom);
    /* The CPU, its bus through the recogniser's callbacks whether loader acceleration is on or not (the tape slot's setting
     * switches it while the machine runs). */
    n->cpu_bus.ctx = n;
    n->cpu_bus.read = cpu_read;
    n->cpu_bus.write = cpu_write_accel;
    n->cpu_bus.in = cpu_in_accel;
    n->cpu_bus.out = cpu_out_accel;
    n->cpu_bus.ack = cpu_ack_accel;
    {
        const zr_loadaccel_host accel = {.ctx = n, .peek = accel_peek, .level_until = accel_level_until, .quiet = NULL};
        zr_loadaccel_init(&n->accel, &accel);
    }
    zr_z80_init(&n->z80, &n->cpu_bus, NULL, 0);
    /* The chips. */
    zr_monovideo_init(&n->video, &n->host, 2u, n->ram + video_base, interrupt);
    zr_beeper_init(&n->beeper, &n->host, ZR_NATIVE_CLOCK_HZ, ZR_NATIVE_AUDIO_RATE);
    zr_beeper_set_weight(&n->beeper, ZR_BEEPER_SPEAKER, 8000);
    zr_beeper_set_weight(&n->beeper, ZR_BEEPER_MIC, 4000);
    zr_tape_deck_init(&n->deck, ZR_NATIVE_CLOCK_HZ);
    zr_tape_deck_set_48k(&n->deck, 1);
    n->tape_dev.ops = &TAPE_OPS;
    n->tape_dev.host = &n->host;
    n->tape_dev.name = "tape";
    n->key_dev.ops = &KEY_OPS;
    n->key_dev.host = &n->host;
    n->key_dev.name = "keys";
    return 1;
}

/* --- checks --------------------------------------------------------------------------------------------- */

uint32_t zr_native_rom_hashes(const void *self, uint8_t *hashes, uint32_t max) {
    if (!max) return 0;
    memcpy(hashes, ((const zr_native *)self)->rom_hash, 32);
    return 1;
}

void zr_native_set_trace(void *self, zr_trace_fn fn, void *ctx) { zr_bus_set_trace(&((zr_native *)self)->bus, fn, ctx); }

int zr_native_peek(void *self, uint32_t address, uint8_t *value) {
    if (address > 0xFFFFu) return 0;
    *value = zr_bus_peek(&((zr_native *)self)->bus, (uint16_t)address);
    return 1;
}
int zr_native_poke(void *self, uint32_t address, uint8_t value) {
    zr_native *n = self;
    if (address > 0xFFFFu) return 0;
    zr_bus_write(&n->bus, (uint16_t)address, value);
    return 1;
}

/* --- states --------------------------------------------------------------------------------------------- */

void zr_native_save(const zr_native *n, zr_state_writer *w) {
    zr_sw_begin(w, "NATV", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_tclock_save(&n->clock, w);
    zr_sw_u64(w, n->target);
    zr_sw_u8(w, n->mic);
    zr_sw_u32(w, n->key_count);
    for (unsigned i = 0; i < ZR_NATIVE_KEY_QUEUE; ++i) {
        zr_sw_u64(w, n->key_t[i]);
        zr_sw_u8(w, n->key_n[i]);
        zr_sw_u8(w, n->key_down[i]);
    }
    zr_sw_u64(w, n->key_arrival);
    zr_sw_u64(w, n->key_group);
    zr_sw_u64(w, n->key_last);
    zr_sw_u8(w, n->play_pending);
    zr_sw_end(w);
    zr_sw_begin(w, "Z80 ", 0);
    zr_z80_save(&n->z80, w);
    zr_sw_end(w);
    zr_sw_begin(w, "RAM ", 0);
    zr_sw_packed(w, n->ram, sizeof n->ram);
    zr_sw_end(w);
    zr_sw_begin(w, "VID ", 0);
    n->video.dev.ops->save(&n->video, w);
    zr_sw_end(w);
    zr_sw_begin(w, "BEEP", 0);
    n->beeper.dev.ops->save(&n->beeper, w);
    zr_sw_end(w);
    if (n->tape) {
        zr_sw_begin(w, "TAPE", 0);
        zr_tape_deck_save(&n->deck, w);
        zr_sw_end(w);
    }
}

int zr_native_load(zr_native *n, zr_state_reader *r) {
    /* The clock and the CPU first: the chips' loads arm their deadlines at the time these give. */
    if (!zr_sr_chunk(r, "NATV", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_tclock_load(&n->clock, r);
    n->target = zr_sr_u64(r);
    n->mic = zr_sr_u8(r) & 1u;
    n->key_count = zr_sr_u32(r);
    if (n->key_count > ZR_NATIVE_KEY_QUEUE) return 0;
    for (unsigned i = 0; i < ZR_NATIVE_KEY_QUEUE; ++i) {
        n->key_t[i] = zr_sr_u64(r);
        n->key_n[i] = zr_sr_u8(r);
        n->key_down[i] = zr_sr_u8(r) & 1u;
    }
    n->key_arrival = zr_sr_u64(r);
    n->key_group = zr_sr_u64(r);
    n->key_last = zr_sr_u64(r);
    n->play_pending = zr_sr_u8(r) & 1u;
    if (!zr_sr_chunk(r, "Z80 ", 0)) return 0;
    zr_z80_load(&n->z80, r);
    if (!zr_sr_chunk(r, "RAM ", 0)) return 0;
    zr_sr_packed(r, n->ram, sizeof n->ram);
    if (!zr_sr_chunk(r, "VID ", 0) || !n->video.dev.ops->load(&n->video, r)) return 0;
    if (!zr_sr_chunk(r, "BEEP", 0) || !n->beeper.dev.ops->load(&n->beeper, r)) return 0;
    /* The recorder's position, when the state has one and a tape is in. A state of another tape leaves this one rewound. */
    if (n->tape && (!zr_sr_chunk(r, "TAPE", 0) || !zr_tape_deck_load(&n->deck, r))) zr_tape_deck_rewind(&n->deck, zr_native_now(n));
    key_arm(n);
    tape_arm(n);
    return zr_sr_ok(r);
}
