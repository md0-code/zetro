/* The Spectrum-compatible core: everything the boards that run Sinclair's
 * 48K BASIC (or their own copy of it) have in common, put together once.
 * A family (jet.c; later hc.c, cip.c, tims.c, cobra.c) is a thin
 * configuration of it: it fills a zr_spectrum_config, calls
 * zr_spectrum_create from its family's create, and uses zr_spectrum_impl
 * (or its own table built on the zr_spectrum_* calls) as the family's impl.
 *
 * What the core holds: the Z80 on a zr_bus (bus.h); the scheduler and the
 * CPU clock (sched.h); the ULA (ula.h) reading the screen through
 * `screen`, caught up before every write to the pages it reads and before
 * every border change; the beeper with the speaker (port FE bit 4), MIC
 * (bit 3) and, with the "tape sound" option, the tape's EAR signal; the
 * tape recorder (media/tape.h) whose EAR level port FE reads on bit 6 at
 * the guest time of the IN cycle, playing in real guest time (no trap: the
 * ROM loads the tape through the port, unless a ROM tape trap takes the block, hw/traps.h), recording from MIC; the 40-key
 * matrix; a Kempston joystick port; sound as stereo frames at 48 kHz; the
 * frame and the screen read back as text through the ROM's character set;
 * peek, poke, the trace, reset, power cycle, the NMI button, the ROM
 * hashes, and the whole state.
 *
 * Guest time is ticks of the master clock (cfg.clock_hz); the CPU runs at
 * clock_hz / ticks_per_tstate (zr_tclock, sched.h); zr_spectrum_run runs
 * the CPU and the chips' deadlines (zr_sched_run_z80) to a target that
 * advances by exactly what the shell asks, so the frames stay in step with
 * the raster however far the last instruction overshoots.
 *
 * A board that pages memory, adds ports or interfaces does it on s->bus
 * after zr_spectrum_create (and keeps s->screen and the watched pages
 * right when it moves the screen); its own state goes in chunks of its own
 * (a board impl that calls zr_spectrum_save and zr_spectrum_load, then
 * writes or reads its chunks).
 *
 * ZetRo's own code, written from this project's JET core (machine_core.sv,
 * rp_spectrum_keyboard.sv, rp_spectrum_video.sv) and hardware notes. */
#ifndef ZR_SPECTRUM_H
#define ZR_SPECTRUM_H

#include "machine_impl.h"
#include "media/tape.h"
#include "ay8912.h"
#include "beeper.h"
#include "drivesnd.h"
#include "bus.h"
#include "loadaccel.h"
#include "sched.h"
#include "traps.h"
#include "ula.h"
#include "z80.h"

#define ZR_SPECTRUM_ROMS 4                  /* ROM images a model may have */
#define ZR_SPECTRUM_AUDIO_RATE 48000u
#define ZR_SPECTRUM_SCREEN 0x1B00u          /* bytes of video memory: the bitmap and the attributes */

/* The key table every Spectrum-compatible keyboard shares: the 8 x 5
 * matrix, key index = row * 5 + column, rows as the address lines A8-A15
 * select them and columns as D0-D4 read them. */
enum {
    ZR_SPECTRUM_KEY_CAPS_SHIFT = 0,
    ZR_SPECTRUM_KEY_ENTER = 30,
    ZR_SPECTRUM_KEY_SPACE = 35,
    ZR_SPECTRUM_KEY_SYMBOL_SHIFT = 36,
    ZR_SPECTRUM_KEYS = 40
};
extern const zr_key_desc zr_spectrum_keys[ZR_SPECTRUM_KEYS];
/* What 48K BASIC types in the modes it is normally in (K, L and C): letters
 * (capitals with CAPS SHIFT), digits, space, ENTER, delete (\b) and the
 * SYMBOL SHIFT characters of the keycaps. */
#define ZR_SPECTRUM_TYPING 89
extern const zr_typing_desc zr_spectrum_typing[ZR_SPECTRUM_TYPING];
/* Host keys the keyboard has no key for (Backspace, the cursor keys, Caps
 * Lock, Escape as BREAK, punctuation as SYMBOL SHIFT and a key, Alt as
 * SYMBOL SHIFT), as the JET core maps them. */
#define ZR_SPECTRUM_COMBOS 22
extern const zr_combo_desc zr_spectrum_combos[ZR_SPECTRUM_COMBOS];

/* What is in a joystick socket (the family's option maps to these, in this order: zr_spectrum_sticks):
 *   Kempston     port 1F (the low byte; the family may give its own decode): bit 0 right, 1 left, 2 down, 3 up, 4 fire,
 *                high when held, the other bits 0
 *   Sinclair I   the keys 6 (left), 7 (right), 8 (down), 9 (up) and 0 (fire): Interface 2's first socket
 *   Sinclair II  the keys 1 (left), 2 (right), 3 (down), 4 (up) and 5 (fire): its second socket
 *   Cursor       the keys 5 (left), 6 (down), 7 (up), 8 (right) and 0 (fire)
 *   Fuller       port 7F (the low byte): bit 0 up, 1 down, 2 left, 3 right, 7 fire, low when held, the other bits high
 * The keys of a Sinclair or Cursor joystick are pressed on the matrix as port FE reads it (the keyboard's own keys are
 * not touched). */
enum { ZR_SPECTRUM_JOY_NONE, ZR_SPECTRUM_JOY_SINCLAIR1, ZR_SPECTRUM_JOY_SINCLAIR2, ZR_SPECTRUM_JOY_CURSOR,
       ZR_SPECTRUM_JOY_FULLER, ZR_SPECTRUM_JOY_KEMPSTON, ZR_SPECTRUM_JOY_KINDS };
#define ZR_SPECTRUM_SOCKETS 2                /* joystick sockets a Spectrum-compatible board can have */
/* The words of the joystick option ("none", "sinclair1", "sinclair2", "cursor", "fuller", "kempston"). A family
 * labels each value with zr_spectrum_stick_label: its own name, marked "not original" where the socket did not take
 * that kind on the real machine. */
extern const char *const zr_spectrum_sticks[ZR_SPECTRUM_JOY_KINDS + 1];
#define ZR_SPECTRUM_STICK_LABEL(kind, original) (ZR_T_SPEC_JOY_NONE + 2 * (kind) + ((original) ? 0 : 1))
/* The value labels of a socket whose real machine took one kind of joystick (`original`, ZR_SPECTRUM_JOY_*): that kind and
 * "none" are original, the others marked. */
#define ZR_SPECTRUM_STICK_LABELS(original)                                                                                     {ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_NONE, 1),                                                                          ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR1, (original) == ZR_SPECTRUM_JOY_SINCLAIR1),                               ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR2, (original) == ZR_SPECTRUM_JOY_SINCLAIR2),                               ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_CURSOR, (original) == ZR_SPECTRUM_JOY_CURSOR),                                     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_FULLER, (original) == ZR_SPECTRUM_JOY_FULLER),                                     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_KEMPSTON, (original) == ZR_SPECTRUM_JOY_KEMPSTON)}
/* A joystick socket's option row: `key` ("joystick", "joystick2"), its label, its initial kind and its value labels. */
#define ZR_SPECTRUM_OPTION_JOYSTICK(key, label, initial, labels) \
    {key, label, zr_spectrum_sticks, labels, initial, ZR_MODELS_ALL, 0}

/* The AY board (an AY-3-8912 clocked at 1.75 MHz from the 14 MHz master clock, mixed with the beeper): off, or its three
 * channels to both sides, A-B-C or A-C-B left to right. A family flags the option ZR_OPTION_NOT_ORIGINAL where the
 * machine's core has no AY. A change rebuilds the machine. */
enum { ZR_SPECTRUM_AY_OFF, ZR_SPECTRUM_AY_MONO, ZR_SPECTRUM_AY_ABC, ZR_SPECTRUM_AY_ACB };
extern const char *const zr_spectrum_ays[5];
extern const int zr_spectrum_ay_labels[4];
#define ZR_SPECTRUM_OPTION_AY(flags) \
    {"ay", ZR_T_SPEC_AY, zr_spectrum_ays, zr_spectrum_ay_labels, 0, ZR_MODELS_ALL, (flags) | ZR_OPTION_RESET}

extern const char *const zr_spectrum_switch[3];
/* "Spectrum timing" (not original, off in a new configuration, rebuilds the machine): the timing of Sinclair's own ULA,
 * the 48K's or the 128K's as the family's config says (sinclair), instead of the clone's. Values: 0 original, 1 spectrum. */
extern const char *const zr_spectrum_timings[3];
extern const int zr_spectrum_timing_labels[2];
#define ZR_SPECTRUM_OPTION_TIMING \
    {"timing", ZR_T_SPEC_TIMING, zr_spectrum_timings, zr_spectrum_timing_labels, 0, ZR_MODELS_ALL, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET, ZR_T_SPEC_TIMING_HINT + 1}

/* The added interfaces (not original, off in a new configuration): one at a time, chosen by
 * the option "interface", each with its media slots. A family puts the rows of ZR_SPECTRUM_OPTIONS_INTERFACE (its
 * ZR_SPECTRUM_INTERFACE_OPTIONS options, together, at the end of its table) and of ZR_SPECTRUM_MEDIA_INTERFACE (its
 * ZR_SPECTRUM_INTERFACE_SLOTS slots, together) in its tables for the models that take one, and passes the options and
 * the first slot's index on with zr_spectrum_interface_config; the core does the rest: the slots' insert, media and eject
 * (a board's own entries hand it the slots they do not know), the interface's memory over 0000h-3FFFh (bus.h's overlay:
 * over the board's ROM whatever its paging, never over RAM the board maps there), its CPU hooks (chained before the
 * board's and the tape traps'), its reset with the board's, its button on the NMI key (F10) in place of the board's NMI,
 * and its state (a chunk of its own). A board that pages Interface 1 into the same area refuses to be built with both
 * (zr_spectrum_interface_conflict).
 *   dandanator  the ZX Dandanator Mini (hw/dandanator.h): the slot "romset" takes its 512 KiB set (a ".rom" or ".bin";
 *               an empty file becomes a blank set); "dandanator_program" is its programming switch.
 *   divide      the DivIDE (hw/divide.h): an IDE channel with the slots "card" and "card2" as its two drives (raw or HDF
 *               images, used in place: media/card.h) and the ESXDOS firmware from the ROM index (key esxdos-ide).
 *   divmmc      the DivMMC (hw/divmmc.h): an SD card in the slot "card" ("card2" is not used) and the firmware key esxdos-mmc.
 * The firmware missing from the ROM index: the machine is not built, the message names the file and tools/fetch_roms.py.
 * The interfaces' automatic mapping watches fetches at the addresses of the 48K ROM; a board with another ROM page or RAM at
 * 0000h gives it up there (see `interface_rom` below). */
enum { ZR_SPECTRUM_IF_NONE, ZR_SPECTRUM_IF_DANDANATOR, ZR_SPECTRUM_IF_DIVIDE, ZR_SPECTRUM_IF_DIVMMC, ZR_SPECTRUM_IF_KINDS };
extern const char *const zr_spectrum_interfaces[ZR_SPECTRUM_IF_KINDS + 1];
extern const int zr_spectrum_interface_labels[ZR_SPECTRUM_IF_KINDS];
#define ZR_SPECTRUM_INTERFACE_OPTIONS 2
/* `when` and `forced` are the family's own rules for the option "interface" (NULL: none): a board whose Interface 1 takes the
 * same ROM area gives "interface1=off" and "interface1=on:none". */
#define ZR_SPECTRUM_OPTIONS_INTERFACE_RULES(models, when, forced)                                                               \
    {"interface", ZR_T_SPEC_INTERFACE, zr_spectrum_interfaces, zr_spectrum_interface_labels, ZR_SPECTRUM_IF_NONE, models,       \
     ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET, ZR_T_SPEC_INTERFACE_HINT + 1, when, forced},                                     \
    {"dandanator_program", ZR_T_SPEC_DAN_PROGRAM, zr_spectrum_switch, NULL, 0, models, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET, \
     ZR_T_SPEC_DAN_PROGRAM_HINT + 1, "interface=dandanator", NULL}
#define ZR_SPECTRUM_OPTIONS_INTERFACE(models) ZR_SPECTRUM_OPTIONS_INTERFACE_RULES(models, NULL, NULL)
#define ZR_SPECTRUM_INTERFACE_SLOTS 3
/* Each slot is there with the interface that uses it. */
#define ZR_SPECTRUM_MEDIA_INTERFACE(models)                                                                \
    {"romset", ZR_T_MEDIA_ROMSET, ZR_MEDIA_ROMSET, "rom,bin", models, "interface=dandanator"},             \
    {"card", ZR_T_MEDIA_CARD, ZR_MEDIA_CARD, "img,vhd,hdf,mmc", models, "interface=divide|divmmc"},        \
    {"card2", ZR_T_MEDIA_CARD2, ZR_MEDIA_CARD, "img,vhd,hdf,mmc", models, "interface=divide"}

struct zr_spectrum;
struct zr_dandanator;
struct zr_divide;
struct zr_divmmc;
struct zr_card;
typedef struct zr_spectrum_config {
    uint32_t clock_hz;                     /* the master clock: 14000000 */
    uint32_t ticks_per_tstate;              /* 4: the 3.5 MHz CPU */
    zr_ula_config ula;
    /* Port FE: the cycles with (port & fe_mask) == fe_value; a read gives
     * fe_idle in the bits nothing drives (5 and 7), EAR in bit 6, the keys
     * in bits 0-4. */
    uint16_t fe_mask, fe_value;
    uint8_t fe_idle;
    /* The Kempston port: (port & mask) == value; mask 0: none. */
    uint16_t kempston_mask, kempston_value;
    /* Memory: the model's ROMs, in the order of its `roms` keys, one after
     * the other in s->rom; the first 16 KiB at 0000h. RAM: ram_size bytes in
     * s->ram, the first 48 KiB at 4000h, the screen at its start. */
    uint32_t ram_size;
    uint16_t charset;                       /* where the character set is in the first ROM: 3D00h */
    uint8_t cmos;                           /* the Z80 is a CMOS part */
    uint8_t nmi;                            /* the board has an NMI button */
    uint8_t is_48k;                         /* the tape's "stop if 48K" blocks stop it */
    /* The options as the family's values gave them. */
    /* The tape slot's settings (zr_machine_tape_options), all on in a new machine: they change while it runs. */
    uint8_t tape_sound;                     /* the tape's signal is heard */
    uint8_t tape_traps, tape_save_traps;    /* the ROM's tape routines are done by the emulator (hw/traps.h) */
    uint8_t tape_accelerate;                /* loops that wait on the tape are jumped through (hw/loadaccel.h) */
    uint8_t joystick;                       /* ZR_SPECTRUM_JOY_* */
    unsigned tape_slot;                     /* the tape's slot in the family's media table */
    /* Spectrum timing: the Sinclair machine the model stands for (ZR_ULA_SINCLAIR_48K or _128K) and whether its timing
     * is on. On: the ULA as zr_ula_sinclair_config makes it, contended memory and I/O through the CPU's stretch, the
     * floating bus on unattached ports, port FE's undriven bits 5 and 7 high. Off: nothing of it, not even a callback. */
    uint8_t sinclair;
    uint8_t spectrum_timing;
    /* Added with the HC family; each is zero (or NULL) in zr_spectrum_default_config, which keeps the behaviour the
     * core had without it. */
    uint8_t joystick2;                      /* ZR_SPECTRUM_JOY_*: the second socket's kind (a family with two sockets) */
    /* The Fuller port when a socket is a Fuller joystick: (port & mask) == value; mask 0: the low byte 7F. A Kempston
     * socket on a board without kempston_mask gets the low byte 1F. */
    uint16_t fuller_mask, fuller_value;
    /* The AY board: ZR_SPECTRUM_AY_* (OFF: none). Its decode: (port & mask) == value selects the chip, A14 then picks
     * the register port (FFFD: latch the address, read the register) or the data port (BFFD: write it); mask 0: A15
     * high and A1 low, as the 128K's. */
    uint8_t ay;
    uint16_t ay_mask, ay_value;
    /* The board's own lengthening of the CPU's cycles (NULL: none), added to Spectrum timing's when that is on; and,
     * for loader acceleration, the first T-state in [t, limit) at which it may lengthen a cycle of the loop being
     * jumped through (NULL with a stretch: t, the loop is never jumped). */
    unsigned (*stretch)(struct zr_spectrum *s, int kind, uint16_t address);
    uint64_t (*stretch_quiet)(struct zr_spectrum *s, uint64_t t, uint64_t limit);
    /* Added with the HC-91 Plus and the HC-2000 (zero or NULL: as before). The ROM tape traps look for the 48K routines in
     * the 16 KiB of s->rom that starts at this offset (the ROM the board maps at 0000h when its BASIC runs; the 128K's
     * second page), not in the first; they fire only where their bytes are at the entry, so a page that is not mapped
     * never fires them. */
    uint32_t traps_rom;
    /* Added with the CIP-01 (zero or NULL: as before). The ROM images the board loads, in the order of the image (NULL: the keys of
     * the model's `roms`), and the bit of each key whose file may be missing: its bytes are then FFh, as an empty socket gives, and
     * its hash is that of those bytes. A model's `roms` stay the ROMs the board cannot run without. */
    const char *const *rom_keys;
    uint32_t rom_optional;
    /* RAM that is not the 48K machine's three areas: page(s, n) is the bytes of RAM page n (0-7, the 128K's numbering) or NULL
     * when the board has none; the pages are reached by ZR_PAGE_ADDRESS. With paging_get and paging_set also given the board is
     * a 128K machine (its RAM `ram_size` bytes, page n at ram + n * 16 KiB or as `page` says): its snapshots are the 128K's
     * (eight pages and the last byte written to the paging port 7FFD, which paging_get gives and paging_set puts back, the
     * board mapping its memory again), a 48K snapshot loads with paging_set(paging_48k) (the 48K machine: the 48 BASIC, the
     * paging locked). The AY's registers are the core's. */
    uint8_t *(*page)(struct zr_spectrum *s, unsigned page);
    uint8_t (*paging_get)(struct zr_spectrum *s);
    void (*paging_set)(struct zr_spectrum *s, uint8_t value);
    uint8_t paging_48k;
    /* Added with the CIP-04 (+3); each is zero (or NULL) in zr_spectrum_default_config, which keeps the behaviour the core had
     * without it.
     * A second paging port, the +2A/+3's 1FFD (a 128K board with paging2_set also takes the +2A and +3 snapshots): its last value
     * is paging2_get, paging2_set puts it back (the board maps its memory again, the way paging_set does) and a 48K snapshot
     * loads with paging2_set(paging2_48k). Snapshots written carry it (the +3 machine class and the 1FFD of the formats).
     * gate_array: with Spectrum timing on, the +2A/+3's gate array instead of the 128K's ULA (comp.sys.sinclair FAQ, 128K
     * reference): contention begins at T-state 14365 of the frame with the delays 1, 0, 7, 6, 5, 4, 3, 2 repeated for 128
     * T-states of each picture line, the board says which pages are contended (zr_spectrum_contend), the gate array contends
     * memory cycles only (a port cycle and an internal T-state wait for nothing) and an unattached port reads FFh (no floating
     * bus). The frame (228 x 311 T-states) and the interrupt are the 128K's. */
    uint8_t (*paging2_get)(struct zr_spectrum *s);
    void (*paging2_set)(struct zr_spectrum *s, uint8_t value);
    uint8_t paging2_48k;
    uint8_t gate_array;
    /* Added with the interfaces (zr_spectrum_interface_config sets them; zero: none, and no slots for one). */
    uint8_t interface;                      /* ZR_SPECTRUM_IF_* */
    uint8_t interface_program;              /* the Dandanator's programming switch */
    uint8_t interface_slots;                /* the family lists the interface's slots, from interface_slot on */
    unsigned interface_slot;
    /* The DivIDE's and DivMMC's automatic mapping acts only while this says 1 (NULL: always): the 128K boards answer 1 while the
     * 48 BASIC ROM is the one at 0000h, as the real interfaces' ROM-select line makes them. It is
     * not asked for where the board has RAM at 0000h-1FFFh, which the interface never covers. */
    int (*interface_rom)(struct zr_spectrum *s);
} zr_spectrum_config;

/* The interface options a family's rows of ZR_SPECTRUM_OPTIONS_INTERFACE gave (`options`: the first of them) and the index
 * of its first ZR_SPECTRUM_MEDIA_INTERFACE slot, into `c`. */
void zr_spectrum_interface_config(zr_spectrum_config *c, const unsigned *options, unsigned first_slot);
/* For a board that pages Interface 1 into 0000h-3FFFh: 1, with the reason, when `c` has an interface and Interface 1 is
 * fitted too (the machine is then not built; the shell says why and keeps the machine it had). */
int zr_spectrum_interface_conflict(const zr_spectrum_config *c, int interface1, char *error, size_t capacity);

/* The 48K values: 14 MHz, 3.5 MHz CPU, the ULA's defaults, port FE on A0
 * low, bits 5 and 7 high, no Kempston port, 48 KiB RAM, the character set
 * at 3D00h, NMOS, no NMI button, a 48K tape, tape sound on, no joystick,
 * the tape in slot 0. */
void zr_spectrum_default_config(zr_spectrum_config *c);

typedef struct zr_spectrum {
    zr_spectrum_config cfg;
    const zr_family *family;
    unsigned model;
    char description[160], error[160];
    /* the board */
    zr_hw_host host;
    zr_sched sched;
    zr_tclock clock;
    uint64_t target;                        /* where the runs asked so far end */
    zr_bus bus;
    zr_z80_bus cpu_bus;
    zr_z80 z80;
    zr_ula ula;
    zr_beeper beeper;
    /* memory */
    uint8_t *rom;
    uint32_t rom_size;
    uint8_t rom_hash[ZR_SPECTRUM_ROMS][32];
    unsigned rom_count;
    uint8_t *ram;
    const uint8_t *screen;                  /* the ZR_SPECTRUM_SCREEN bytes the ULA reads */
    uint8_t fe;                             /* the last byte written to port FE */
    /* the tape recorder */
    zr_media *tape_media;
    zr_tape *tape;
    zr_tape_deck deck;
    zr_dev tape_dev;                        /* the deadline of the tape's next edge */
    zr_traps traps;                         /* the ROM tape traps, found in the ROM at creation */
    zr_loadaccel accel;                     /* loader acceleration, when cfg.tape_accelerate */
    uint32_t trap_block;                    /* the block a load trap is taking (inside one trap only) */
    /* the (first) joystick socket: ZR_JOY_* held */
    unsigned stick;
    /* the text read-back: the 96 glyphs of the character set, 8 rows in a word */
    uint64_t glyph[96];
    /* Spectrum timing: which 16 KiB of the address space the ULA contends (a 128K board sets slot 3 by its paging,
     * zr_spectrum_contend), and the CPU's T-state at which the current frame of the ULA began */
    uint8_t contended[4];
    uint64_t frame_t;
    /* the second joystick socket: ZR_JOY_* held */
    unsigned stick2;
    /* the AY board (cfg.ay) */
    zr_ay ay;
    /* where the AY's two ports are in the bus's decoder table (zr_spectrum_ay_decode) */
    int ay_dec[2];
    /* A board's diskette drive sound, mixed into the sound when set (the board's to own, save and reset). */
    zr_drivesnd *drives;
    /* The CPU's hooks: an added interface's, a board's (zr_spectrum_set_hooks) and the tape traps', chained. */
    const zr_cpu_hooks *board_hooks;
    zr_cpu_hooks hooks;
    uint8_t watch[8192];
    /* The added interface (cfg.interface): its slots' files, its hooks, the Dandanator; iface_resetting while the
     * interface itself pulls the board's RESET (the interface is not reset by it). */
    zr_media *iface_media[ZR_SPECTRUM_INTERFACE_SLOTS];
    const zr_cpu_hooks *iface_hooks;
    struct zr_dandanator *dan;
    struct zr_divide *divide;               /* the DivIDE or the DivMMC (the two share the paging: div_mem) */
    struct zr_divmmc *divmmc;
    struct zr_divmem *div_mem;
    uint8_t *iface_firmware;                /* the ESXDOS image of the DivIDE or DivMMC (a copy of the file's) */
    struct zr_card *card[ZR_SPECTRUM_INTERFACE_SLOTS];     /* the images of the card slots, opened here and used in place */
    zr_bus_overlay *iface_overlay;          /* the bus's overlay (bus.h), attached with the interface */
    uint8_t iface_resetting;
} zr_spectrum;

/* A board's own CPU hooks (hw.h: an interface that pages itself in on a fetch), with the tape traps': both watch
 * their addresses, the board's hook runs first. NULL takes the board's away. The table stays the board's. */
void zr_spectrum_set_hooks(zr_spectrum *s, const zr_cpu_hooks *board);
/* The ROM tape traps look for the routines again, in `memory`, 16 KiB as the CPU sees them from 0000h (a board whose BASIC comes
 * into the RAM calls it when it has: the traps found in the ROM at creation stay otherwise), by their entry and exit alone
 * (zr_traps_init_loose: the BASIC the CIP-01 loads has a jump patched into its loader). The options say which are on. */
void zr_spectrum_traps_rescan(zr_spectrum *s, const uint8_t *memory, size_t size);
/* An added interface's CPU hooks (its automatic paging), chained before the board's: NULL takes them away. */
void zr_spectrum_set_interface_hooks(zr_spectrum *s, const zr_cpu_hooks *hooks);
/* 1 while an added interface shows its own memory at 0000h (the board's ROM is not what the CPU reads there). */
int zr_spectrum_interface_rom(const zr_spectrum *s);

/* The AY board's decode, changed while the machine runs (a board whose mode takes the chip off the ports of another device):
 * (port & mask) == value selects the chip as zr_spectrum_config.ay_mask and ay_value say; mask 0 is the 128K's (A15 high and
 * A1 low). */
void zr_spectrum_ay_decode(zr_spectrum *s, uint16_t mask, uint16_t value);

/* Spectrum timing on a board that pages: whether `slot` (0-3, 16 KiB each) holds contended memory now. */
static inline void zr_spectrum_contend(zr_spectrum *s, unsigned slot, int on) {
    if (slot < 4u) s->contended[slot] = on && s->cfg.spectrum_timing ? 1u : 0u;
}

/* The board for `setup` (machine_impl.h) as `cfg` says, in a block of
 * `size` bytes (at least sizeof(zr_spectrum): a family's own struct that
 * starts with a zr_spectrum), zeroed, powered on. NULL with the reason. */
zr_spectrum *zr_spectrum_create(const zr_spectrum_config *cfg, const zr_machine_setup *setup, size_t size,
                                char *error, size_t capacity);

/* The guest time of the cycle in progress (or of the event running). */
uint64_t zr_spectrum_now(const zr_spectrum *s);

/* The impl table (machine_impl.h) of a board that is the core alone; and
 * its entries, for a board that builds its own table. */
extern const zr_machine_impl zr_spectrum_impl;
void zr_spectrum_destroy(void *self);
void zr_spectrum_reset(void *self, int power_on);
zr_run_status zr_spectrum_run(void *self, uint64_t ticks);
void zr_spectrum_save(const void *self, zr_state_writer *w);
int zr_spectrum_load(void *self, zr_state_reader *r);
const char *zr_spectrum_description(const void *self);
const char *zr_spectrum_error(const void *self);
void zr_spectrum_nmi(void *self);
uint32_t zr_spectrum_clock(const void *self);
uint64_t zr_spectrum_time(const void *self);
uint64_t zr_spectrum_frame_period(const void *self);
int zr_spectrum_frame(void *self, zr_frame *frame);
size_t zr_spectrum_text(void *self, char *out, size_t capacity);
/* The screen as 24 lines of 64 characters of 4 x 8 pixels (the HC's CP/M BIOS draws so; the 4-pixel cells sit two to a byte of
 * the bitmap): `font` is the board's 95 glyphs of the characters 20h to 7Eh, 8 bytes each with the pixels in the low nibble, the
 * leftmost in bit 3; a cell is a glyph as it is or inverted, '?' when it is none. */
size_t zr_spectrum_text_narrow(void *self, const uint8_t *font, char *out, size_t capacity);
uint32_t zr_spectrum_audio_rate(const void *self);
size_t zr_spectrum_audio_pull(void *self, int16_t *stereo, size_t frames);
void zr_spectrum_key(void *self, unsigned key, int down);
void zr_spectrum_joystick(void *self, unsigned stick, unsigned held);
int zr_spectrum_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity);
zr_media *zr_spectrum_media(const void *self, unsigned slot);
int zr_spectrum_eject(void *self, unsigned slot);
unsigned zr_spectrum_activity(const void *self);
int zr_spectrum_control(void *self, unsigned slot, unsigned command, uint32_t value);
unsigned zr_spectrum_tape_options(void *self, unsigned slot, unsigned bits);
struct zr_tape_deck *zr_spectrum_tape_deck(void *self, unsigned slot);
uint32_t zr_spectrum_rom_hashes(const void *self, uint8_t *hashes, uint32_t max);
void zr_spectrum_set_trace(void *self, zr_trace_fn fn, void *ctx);
int zr_spectrum_peek(void *self, uint32_t address, uint8_t *value);
int zr_spectrum_poke(void *self, uint32_t address, uint8_t value);

/* Snapshots (spectrum_snap.c, media/snap.h): a snapshot of the 48K machine applied or captured, and the RAM pages of the
 * 128K machine's numbering reached by peek and poke (machine.h's ZR_PAGE_ADDRESS); a board with paged RAM says so in its
 * config (page, paging_get, paging_set). */
int zr_spectrum_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity);
int zr_spectrum_snapshot_save(void *self, struct zr_snap *snap);
int zr_spectrum_page_peek(void *self, uint32_t address, uint8_t *value);
int zr_spectrum_page_poke(void *self, uint32_t address, uint8_t value);

#endif /* ZR_SPECTRUM_H */
