/* The machine interface: everything the shell (src/app, src/platform) knows
 * of an emulated computer.
 *
 * ZetRo runs many machines. Each is a board (core/machines/) that wires
 * chips together and describes itself in tables: its family and models,
 * the options of each model, its media slots, its ports, its keyboard and
 * how text is typed on it. The shell, the setup screen, the on-screen
 * keyboard and the harness read those tables, so a new machine adds no
 * shell code. The registry (zr_family_*) lists the families built in.
 *
 * The core has no SDL, no OS call and no wall clock. Guest time is a count
 * of periods of the board's master clock (zr_machine_clock): every run is
 * deterministic at any host speed, and the shell paces it to wall time (or,
 * headless, runs a frame period of guest time per frame).
 *
 * zr_machine_create builds the configured model; when one of its ROMs is
 * not in the index it builds the test machine (stub_machine.c) instead,
 * whose screen names what is missing. */

#ifndef ZR_MACHINE_H
#define ZR_MACHINE_H

#include "../hw/hw.h"
#include "config.h"
#include "media.h"
#include "romdb.h"
#include "state.h"

#include <stddef.h>
#include <stdint.h>

typedef struct zr_machine zr_machine;

/* --- what a machine says of itself ---------------------------------------------------- */

#define ZR_MODELS_ALL 0xFFFFFFFFu       /* a `models` mask: every model of the family */
#define ZR_KEY_NONE 0xFFu

/* A model: its id is what machine.model names ("hc85"), unique among all
 * families. `roms` are the keys (romdb.h) of the ROMs it cannot run without,
 * NULL-terminated. `autoload` is what is typed to load from tape ("LOAD
 * \"\"\n" in the notation of the harness's `type`; NULL: nothing). */
typedef struct zr_model_desc {
    const char *id;
    const char *name;                   /* "HC-85" */
    const char *const *roms;
    const char *autoload;
} zr_model_desc;

/* An option of the machine (machine.options.KEY in zetro.yaml): one of a
 * list of values, written in the file as the words of `values` (NULL-
 * terminated; a switch is {"off", "on"}). `label` and `value_labels` are
 * text ids (text.h) for the setup screen; value_labels NULL shows the words.
 * `models` is a bit for each model of the family that has the option.
 *
 * `when` (NULL: always) is the condition under which the model has the option
 * at all: clauses joined by '&', each "key=word|word" (the option `key` of the
 * same family has one of those values) or "key!=word|word" (it has none of
 * them): "interface=divide|divmmc", "interface1=on&startup=cpm". What does not
 * apply is not shown by the setup screen. Media slots and ports have the same.
 * The key "model" stands for the model's id ("model=cobra80").
 * `forced` (NULL: never) gives the value the option must have under a
 * condition, as "condition:word" rules joined by ';' ("startup=cpm:on"): the
 * machine is built with that value whatever the file says, and the setup
 * screen shows it and does not let it be changed (zr_family_resolve). */
enum {
    ZR_OPTION_RESET = 1,                /* a change builds the machine again */
    ZR_OPTION_NOT_ORIGINAL = 2,         /* the setup screen marks it so */
    ZR_OPTION_PORT = 4                  /* a setting of a byte port (its speed, its wiring): shown on the Ports page */
};
typedef struct zr_option_desc {
    const char *key;
    int label;
    const char *const *values;
    const int *value_labels;
    unsigned initial;                   /* the value a new configuration has */
    uint32_t models;
    unsigned flags;
    int hint;                           /* optional: the text id of a hint of its own for the setup screen, plus 1 (0: the general one) */
    const char *when;                   /* optional: the condition under which the model has the option */
    const char *forced;                 /* optional: "condition:word;..." the values it must have */
} zr_option_desc;

/* A media slot (media.KEY in zetro.yaml): `extensions` are the file name
 * endings it takes, lower case, separated by commas ("tap,tzx,pzx"). */
enum { ZR_MEDIA_TAPE, ZR_MEDIA_FLOPPY, ZR_MEDIA_CARD, ZR_MEDIA_ROMSET, ZR_MEDIA_KINDS };
typedef struct zr_media_desc {
    const char *key;                    /* "tape", "a", "b", "card" */
    int label;
    unsigned kind;                      /* ZR_MEDIA_* */
    const char *extensions;
    uint32_t models;
    const char *when;                   /* optional: the condition (as an option's) under which the model has the slot */
} zr_media_desc;

/* A byte port of the guest (ports.KEY in zetro.yaml). */
enum { ZR_PORT_SERIAL, ZR_PORT_PARALLEL };
typedef struct zr_port_desc {
    const char *key;                    /* "serial", "printer" */
    int label;
    unsigned kind;                      /* ZR_PORT_* */
    uint32_t models;
    const char *when;                   /* optional: the condition under which the model has the port */
} zr_port_desc;

/* A key of the machine's keyboard; its index in the table is what
 * zr_machine_key takes. `name` is how the harness and the file call it
 * ("A", "ENTER", "CAPS SHIFT"); `host` the host key in the same place (SDL's
 * scancode name, NULL: none); `legend` the keycap's main marking; x, y, w
 * and h place the key on the on-screen keyboard in quarters of a key. */
typedef struct zr_key_desc {
    const char *name;
    const char *host;
    const char *legend;
    uint8_t x, y, w, h;
} zr_key_desc;

/* How a character is typed: a key, alone or with a shift key held. */
typedef struct zr_typing_desc {
    char ch;
    uint8_t key, shift;                 /* shift ZR_KEY_NONE: alone */
} zr_typing_desc;

/* A host key the machine's keyboard has no key for, given as two of its
 * keys together (Backspace on a Spectrum is CAPS SHIFT and 0). A host that
 * starts with "Shift+" is the key with the host's Shift held (Shift+' is the
 * double quote: SYMBOL SHIFT and P); the machine's own key for the host's
 * Shift is then let go while the combination is down. */
typedef struct zr_combo_desc {
    const char *host;
    uint8_t key, shift;
} zr_combo_desc;

struct zr_machine_impl;
struct zr_machine_setup;

typedef struct zr_family {
    const char *id;                     /* "hc" */
    const char *name;                   /* "HC" */
    const zr_model_desc *models;    unsigned model_count;
    const zr_option_desc *options;  unsigned option_count;
    const zr_media_desc *media;     unsigned media_count;
    const zr_port_desc *ports;      unsigned port_count;
    const zr_key_desc *keys;        unsigned key_count;
    const zr_typing_desc *typing;   unsigned typing_count;
    const zr_combo_desc *combos;    unsigned combo_count;
    unsigned joysticks;                 /* joystick sockets the family's machines can have */
    const struct zr_machine_impl *impl;
    void *(*create)(const struct zr_machine_setup *setup, char *error, size_t capacity);
} zr_family;

/* The registry. */
unsigned zr_family_count(void);
const zr_family *zr_family_at(unsigned index);
/* The family that has model `id`, and which of its models it is; NULL when
 * there is no such model. */
const zr_family *zr_family_of_model(const char *id, unsigned *model);
/* The index of a named thing in a family's tables; -1 when it has none. */
int zr_family_option(const zr_family *f, const char *key);
int zr_family_option_value(const zr_family *f, unsigned option, const char *word);
int zr_family_media(const zr_family *f, const char *key);
int zr_family_key(const zr_family *f, const char *name);
/* The keys that type a character; 0 when the keyboard cannot. */
int zr_family_typing(const zr_family *f, int ch, uint8_t *key, uint8_t *shift);

/* The value of every option of the family (an index into its values) for a model under a configuration: the forced value where
 * a rule holds, else the file's, else the initial one. `state` (optional) says of each option whether the model has it now
 * (ZR_OPTION_PRESENT: its model bit and its `when`) and whether its value is forced (ZR_OPTION_FORCED). Both arrays have
 * ZR_CONFIG_OPTIONS entries. `config` NULL: the initial values. */
enum { ZR_OPTION_PRESENT = 1, ZR_OPTION_FORCED = 2 };
void zr_family_resolve(const zr_family *f, unsigned model, const zr_config *config, unsigned *values, uint8_t *state);
/* Whether a condition (an option's `when`) holds for those values; NULL holds. */
int zr_family_condition(const zr_family *f, unsigned model, const unsigned *values, const char *when);
/* Whether the model has a media slot or a port under those values. */
int zr_family_has_media(const zr_family *f, unsigned model, const unsigned *values, unsigned slot);
int zr_family_has_port(const zr_family *f, unsigned model, const unsigned *values, unsigned port);

/* --- life cycle ----------------------------------------------------------------- */

typedef enum zr_run_status {
    ZR_RUN_OK,          /* ran the time asked */
    ZR_RUN_ERROR        /* stopped: zr_machine_error says why */
} zr_run_status;

/* A machine for the configuration (machine.*), with its ROMs taken from the
 * index (copied: the index may go afterwards). NULL with the reason. Media
 * are inserted afterwards with zr_machine_insert. */
zr_machine *zr_machine_create(const zr_config *config, const zr_rom_index *roms, char *error, size_t capacity);
/* Exchange running machines, keeping the handles held by the shell and setup valid. */
void zr_machine_swap(zr_machine *a, zr_machine *b);
void zr_machine_destroy(zr_machine *m);
/* The family and the model that run (the test machine's when ROMs are missing). */
const zr_family *zr_machine_family(const zr_machine *m);
unsigned zr_machine_model(const zr_machine *m);
/* The value of each of the family's options the machine was built with (ZR_CONFIG_OPTIONS entries). */
const unsigned *zr_machine_options(const zr_machine *m);
/* Whether the machine as built has the media slot, the port. */
int zr_machine_has_media(const zr_machine *m, unsigned slot);
int zr_machine_has_port(const zr_machine *m, unsigned port);
/* What runs, in words ("HC-85, Interface 1"). */
const char *zr_machine_description(const zr_machine *m);
const char *zr_machine_error(const zr_machine *m);
/* Power cycle (power_on 1: the state at switching on, memory cleared) or
 * the board's RESET line (power_on 0: memory kept). */
void zr_machine_reset(zr_machine *m, int power_on);
/* The NMI button, where the board has one (else nothing). */
void zr_machine_nmi(zr_machine *m);

/* --- time ----------------------------------------------------------------------- */

/* The master clock's rate in Hz: what a tick of guest time is. */
uint32_t zr_machine_clock(const zr_machine *m);
/* Runs until guest time has advanced by at least `ticks` (the last
 * instruction may end past it). */
zr_run_status zr_machine_run(zr_machine *m, uint64_t ticks);
uint64_t zr_machine_time(const zr_machine *m);
/* The video frame's period in ticks: the shell presents one frame per period. */
uint64_t zr_machine_frame_period(const zr_machine *m);

/* --- the picture ---------------------------------------------------------------- */

/* A frame as the machine's video produced it, border included: one byte per
 * pixel, rows `pitch` bytes apart, each an index into `palette` (`colors`
 * entries of 0x00RRGGBB). `aspect_w:aspect_h` is the shape the whole frame
 * had on the monitor. `serial` changes whenever the pixels change. The
 * picture (the screen inside the border, in the current mode) is the
 * `picture_w` x `picture_h` pixels at (picture_x, picture_y); `picture_w` 0
 * when the frame has no border to tell apart. */
typedef struct zr_frame {
    uint32_t width, height, pitch;
    const uint8_t *pixels;
    const uint32_t *palette;
    uint32_t colors;
    uint32_t aspect_w, aspect_h;
    uint64_t serial;
    uint32_t picture_x, picture_y, picture_w, picture_h;
} zr_frame;
/* The part of `frame` the border setting shows, in place (the pixels a view
 * into the same memory, the aspect that of the part): the picture with 16
 * (medium) or 5 (minimal) lines of border around it, none, or all of it;
 * the border's sides twice as many pixels wide where they are half as wide. */
void zr_frame_crop(zr_frame *frame, uint32_t border);   /* ZR_CONFIG_BORDER_* */
/* 0 when the machine shows nothing (the frame is then black). */
int zr_machine_frame(zr_machine *m, zr_frame *frame);
/* The frame as ARGB8888 into out (width x height). */
void zr_frame_argb(const zr_frame *frame, uint32_t *out);
/* The characters on the screen as text, a line of the screen a line of the
 * text (the machine reads its own video memory back through its font):
 * what the harness's `text` and `expect` see. Returns the length; 0 when
 * the machine cannot say. */
size_t zr_machine_text(zr_machine *m, char *out, size_t capacity);

/* --- sound ---------------------------------------------------------------------- */

/* Interleaved stereo 16-bit frames at zr_machine_audio_rate (Hz; 0 when the
 * machine makes no sound): the frames the guest produced so far, at most
 * `frames`; returns how many were written. */
uint32_t zr_machine_audio_rate(const zr_machine *m);
size_t zr_machine_audio_pull(zr_machine *m, int16_t *stereo, size_t frames);

/* --- input ---------------------------------------------------------------------- */

/* A key of the family's key table pressed (down 1) or released. */
void zr_machine_key(zr_machine *m, unsigned key, int down);
/* Joystick socket `stick`: the directions and buttons held, ZR_JOY_* bits.
 * What kind of joystick is in the socket is an option of the machine. */
enum { ZR_JOY_UP = 1, ZR_JOY_DOWN = 2, ZR_JOY_LEFT = 4, ZR_JOY_RIGHT = 8, ZR_JOY_FIRE = 16, ZR_JOY_FIRE2 = 32 };
void zr_machine_joystick(zr_machine *m, unsigned stick, unsigned held);

/* --- media ---------------------------------------------------------------------- */

/* A file in a slot of the family's media table (media.h). The file stays
 * the caller's: the machine uses it until it is ejected or the machine is
 * destroyed, and marks it changed when the guest writes to it. 0 with the
 * reason when the slot does not take it. */
int zr_machine_insert(zr_machine *m, unsigned slot, zr_media *media, char *error, size_t capacity);
/* The file in a slot, NULL when empty. */
zr_media *zr_machine_media(const zr_machine *m, unsigned slot);
/* Takes the file out (the caller may close it then); 0 when empty. */
int zr_machine_eject(zr_machine *m, unsigned slot);
/* Bit n set while slot n works (a drive's motor turns, the tape runs). */
unsigned zr_machine_activity(const zr_machine *m);
/* The transport of a slot: the tape recorder's keys. `value` is the block
 * for ZR_MEDIA_SEEK. Returns 0 when the slot has no such control. */
enum { ZR_MEDIA_PLAY, ZR_MEDIA_STOP, ZR_MEDIA_REWIND, ZR_MEDIA_NEXT, ZR_MEDIA_PREVIOUS, ZR_MEDIA_SEEK, ZR_MEDIA_RECORD,
       ZR_MEDIA_PROTECT };
/* ZR_MEDIA_PROTECT: the write-protect tab of a slot whose file the machine uses in place (a card) moved: `value` 1 protects it
 * at once, 0 lets it be written as the file allows. Returns 1 when the image can be written now, 0 when it cannot (or when the
 * slot is not such a one). */
int zr_machine_control(zr_machine *m, unsigned slot, unsigned command, uint32_t value);
/* The tape recorder of a tape slot (media/tape.h: its tape's blocks, the block and position it is at, whether it
 * runs), for the tape browser; NULL when the slot is not a tape or holds none. */
struct zr_tape_deck;
struct zr_tape_deck *zr_machine_tape_deck(zr_machine *m, unsigned slot);
/* A tape slot's settings (config.h, ZR_TAPE_* bits), taken at once: whether the tape is heard, whether the ROM's load and
 * save routines are done by the emulator and whether the waits of loaders are skipped. Returns the bits this machine has
 * a use for (0: the slot is not a tape recorder's, or the machine has none of them); with ZR_TAPE_QUERY nothing changes. */
unsigned zr_machine_tape_options(zr_machine *m, unsigned slot, unsigned bits);
/* What auto-load does for a tape just put into tape slot `slot` (typist.h): the text to type, in the harness's notation for
 * `type`, or NULL when there is none (no auto-load); *power_cycle says whether the machine is power cycled before it is
 * typed. A board that knows better than its model's fixed text says so from its state at this moment (a BASIC that was
 * loaded into the RAM must not be lost: no power cycle, the BASIC's own LOAD); the others give the model's `autoload`
 * with a power cycle. */
const char *zr_machine_autoload(const zr_machine *m, unsigned slot, int *power_cycle);

/* --- ports ---------------------------------------------------------------------- */

/* The guest's byte ports (the family's port table). The machine calls
 * `transmit` for every byte the guest sends and `outputs` when its
 * handshake outputs change; bytes from the host go in with
 * zr_machine_port_receive, which returns how many the port took (offer the
 * rest again later), and the host side's modem inputs with
 * zr_machine_port_modem. A parallel port only transmits: its modem call says
 * whether a printer is on line (`dsr`). `loopback` (optional) is asked for
 * each port when the host is set: 1 wires the port's output to its input. */
typedef struct zr_port_host {
    void *ctx;
    void (*transmit)(void *ctx, unsigned port, uint8_t byte);
    void (*outputs)(void *ctx, unsigned port, int dtr, int rts);
    int (*loopback)(void *ctx, unsigned port);
} zr_port_host;
/* NULL: nothing connected (bytes sent are lost). */
void zr_machine_set_ports(zr_machine *m, const zr_port_host *host);
size_t zr_machine_port_receive(zr_machine *m, unsigned port, const uint8_t *data, size_t count);
void zr_machine_port_modem(zr_machine *m, unsigned port, int dcd, int cts, int dsr, int ri);

/* --- states --------------------------------------------------------------------- */

/* The machine's whole state as chunks into w, after the header this writes
 * (state.h): every chip, the memory, the time. Media are not in it. */
void zr_machine_save(const zr_machine *m, zr_state_writer *w);
/* Takes a state back. Refused, with the reason and the machine untouched,
 * when it is of another model or of other ROMs; a state that turns out
 * damaged part-way leaves the machine reset. */
int zr_machine_load(zr_machine *m, zr_state_reader *r, char *error, size_t capacity);

/* --- checks --------------------------------------------------------------------- */

/* A trace of the bus for headless checks and diagnosis: kind ZR_TRACE_*
 * with the address and value of each such cycle. NULL stops it. */
enum { ZR_TRACE_IO_READ, ZR_TRACE_IO_WRITE, ZR_TRACE_FETCH, ZR_TRACE_INTERRUPT };
typedef void (*zr_trace_fn)(void *ctx, int kind, uint32_t address, uint32_t value);
void zr_machine_set_trace(zr_machine *m, zr_trace_fn fn, void *ctx);
/* A byte of the memory space as the CPU would read it (no side effects),
 * and one written as the CPU would write it. 0 when the machine has no
 * memory there (the test machine). */
int zr_machine_peek(zr_machine *m, uint32_t address, uint8_t *value);
int zr_machine_poke(zr_machine *m, uint32_t address, uint8_t value);

/* --- snapshots ------------------------------------------------------------------ */

/* The Spectrum-compatible machines take snapshots (media/snap.h): the neutral
 * structure of a SNA, Z80 or SZX file. 1 when this machine does, 0 for one that
 * does not (every other family): the two calls below then refuse. */
struct zr_snap;
int zr_machine_takes_snapshots(const zr_machine *m);
/* Applies a snapshot: the processor, the RAM, the border and what else the board
 * has of it; the ROM is the machine's own and the media stay as they are. 0 with
 * the reason (a snapshot of a machine this one is not, a damaged one) and the
 * machine untouched or reset; call it between instructions, as every call here. */
int zr_machine_snapshot_load(zr_machine *m, const struct zr_snap *snap, char *error, size_t capacity);
/* Fills `snap` from the machine as it is now (between instructions); 0 when the
 * machine takes no snapshots. */
int zr_machine_snapshot_save(zr_machine *m, struct zr_snap *snap);

/* The RAM page of the 128K machine's numbering is reached through the memory calls
 * below too: an address of ZR_PAGE_ADDRESS(page, offset) is the byte `offset` (0 to
 * 3FFFh) of RAM page `page` whether or not the machine has it paged in; a machine
 * without that page answers 0. */
#define ZR_PAGE_ADDRESS(page, offset) (0x100000u + ((uint32_t)(page) << 14) + (uint32_t)(offset))

/* The core assumes a little-endian host with 8-bit bytes: 1 when this host
 * is one. zr_machine_create refuses to run otherwise. */
int zr_machine_host_ok(void);

#endif /* ZR_MACHINE_H */
