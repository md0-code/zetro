/* The ICE Felix HC-88 (machine_impl.h): the family "hc88" (named "HC", like the other HC machines) with its one model, the
 * CP/M machine of the HC family, as this project's HC core documents it (MiSTerRO/docs/hc/hc88.md, drawing 869.300.350;
 * core-hc/rtl/hc88_core.sv and hc_family_core.sv, hc88_video.sv; the benches sim/hc88_*_tb.sv as the guide to the
 * behaviour). Wired from the chips, not from the Spectrum core: the HC-88 is not a Spectrum.
 *
 *   CPU      Z80A at 3.5 MHz from the 14 MHz master clock (the dot clock of the video), with the half-slot stop: the clock
 *            is held in the second half of every four T-states while the CPU is on the video RAM
 *   memory   a 2 KiB bootstrap ROM, mirrored through 0000-3FFF while the command latch's LOAD bit is clear; 64 KiB of system
 *            RAM; a separate 16 KiB of video RAM. The latch (port 9F: A7 high, A6 low, A0 high; bits LOAD 0, LOCK 1, SCR 2,
 *            MOD1 3, MOD 4, MOD0 5) puts the video RAM at 4000-7FFF unless MOD0 removes it, and in every 16 KiB quadrant
 *            with MOD1; LOCK write-protects the system RAM at 0000-3FFF. With LOAD set an opcode fetch at 0008-000F or
 *            1708-170F switches on a shadow (the system RAM at 4000-7FFF seen at 0000-3FFF, writes bypassing LOCK) and a fetch at
 *            0700-0707 switches it off, as does a write to the latch with LOAD clear (the CPU's fetch hooks, hw.h)
 *   ports    FE keyboard, EAR, border, MIC, speaker; DD (status) and DF (data) the 8272 in polled use; E7 the control latch
 *            (TC, drive A and B select, motor, FDC reset release); EF and F7 the serial bit lines and the network line.
 *            Every port is decoded on its own address bits and independently of the others, so aliases overlap as the
 *            core's do (one decoder answers the whole port space)
 *   video    hw/hc88video: 256 or 512 pixels by 192, 896 x 312 dots at 14 MHz, the frame interrupt
 *   disk     the 8272 (hw/upd765) with two 80-track double-sided drives (hw/fdd) at 250 kbit/s, 300 rpm, its READY the
 *            mounted disk of the drive E7 selects, the motor a one-shot that every E7 write with bit 3 set restarts
 *   tape     a recorder on EAR (port FE bit 6) and MIC (bit 3), real time
 *   sound    the speaker and MIC (and the tape's EAR with the option), drive noises (not original)
 *   serial   EF and F7 carry the bit lines of a UART the guest times itself: the TX line is turned into bytes and RX bytes
 *            into the line at the speed the option names; the network line is idle
 *
 * ZetRo's own code. */
#include "machine_impl.h"
#include "media/disk.h"
#include "media/tape.h"
#include "beeper.h"
#include "bus.h"
#include "drivesnd.h"
#include "fdd.h"
#include "font8x8.h"
#include "hc88video.h"
#include "romdb.h"
#include "sched.h"
#include "text.h"
#include "upd765.h"
#include "z80.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ZR_BUS_TRACE_IN == ZR_TRACE_IO_READ && ZR_BUS_TRACE_OUT == ZR_TRACE_IO_WRITE &&
               ZR_BUS_TRACE_FETCH == ZR_TRACE_FETCH && ZR_BUS_TRACE_INTERRUPT == ZR_TRACE_INTERRUPT,
               "the bus traces in machine.h's kinds");

#define STATE_VERSION 1u
#define CLOCK_HZ 14000000u
#define TICKS_PER_TSTATE 4u                     /* the 3.5 MHz CPU */
#define AUDIO_RATE 48000u
#define MOTOR_TICKS (CLOCK_HZ / 10u * 17u)      /* the motor one-shot: 1.7 s (hc_family_core.sv) */
#define DISK_FLUSH_TICKS (CLOCK_HZ / 2u)        /* a changed disk goes into its media at most twice a second */
#define WAIT_TICKS 69888u                       /* network WAIT: a stretch of a frame at a time */
#define TX_LOG 32
#define RX_QUEUE 16
#define KEY_QUEUE 16
#define KEY_SPACING 28000u                      /* keys change 2 ms apart at the least: see hc88_key */

enum { SLOT_A, SLOT_B, SLOT_TAPE, SLOTS };
enum { OPTION_DRIVE_SOUND, OPTION_DISK, OPTION_BAUD, OPTION_COUNT };

/* --- the family's tables ---------------------------------------------------------------------------- */

static const char *const ROMS[] = {"hc88", NULL};
static const zr_model_desc MODELS[] = {{"hc88", "HC-88", ROMS, NULL}};
static const char *const SWITCH[] = {"off", "on", NULL};
static const char *const DISK_WORDS[] = {"original", "core", NULL};
static const int DISK_LABELS[] = {ZR_T_HC88_DISK_ORIGINAL, ZR_T_HC88_DISK_CORE};
static const char *const BAUD_WORDS[] = {"300", "600", "1200", "2400", "4800", "9600", "19200", "38400", NULL};
static const unsigned BAUDS[] = {300, 600, 1200, 2400, 4800, 9600, 19200, 38400};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    {"drive_sound", ZR_T_HC88_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_NOT_ORIGINAL, ZR_T_HC88_DRIVE_SOUND_HINT + 1},
    {"floppy", ZR_T_HC88_DISK, DISK_WORDS, DISK_LABELS, 1, ZR_MODELS_ALL, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET, ZR_T_HC88_DISK_HINT + 1},
    {"serial_baud", ZR_T_HC88_BAUD, BAUD_WORDS, NULL, 5, ZR_MODELS_ALL, ZR_OPTION_PORT, ZR_T_HC88_BAUD_HINT + 1},
};
static const zr_media_desc MEDIA[SLOTS] = {
    {"a", ZR_T_HC88_DRIVE_A, ZR_MEDIA_FLOPPY, "dsk,img", ZR_MODELS_ALL},     /* the machine starts from its disk */
    {"b", ZR_T_HC88_DRIVE_B, ZR_MEDIA_FLOPPY, "dsk,img", ZR_MODELS_ALL},
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
};
static const zr_port_desc PORTS[] = {{"serial", ZR_T_HC88_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL}};

/* The keyboard: the 8 x 5 matrix of the HC family (hc85_keyboard.sv), key index = row * 5 + column, rows as the address
 * lines A8-A15 select them, columns as D0-D4 read them. */
#define K(name, x, y) {name, name, name, x, y, 4, 4}
enum { KEYS = 40, CS = 0, KEY_ENTER = 30, KEY_SPACE = 35, SS = 36 };
static const zr_key_desc KEY_TABLE[KEYS] = {
    {"CAPS SHIFT", "Left Shift", "CAPS SHIFT", 0, 12, 6, 4}, K("Z", 6, 12), K("X", 10, 12), K("C", 14, 12), K("V", 18, 12),
    K("A", 3, 8), K("S", 7, 8), K("D", 11, 8), K("F", 15, 8), K("G", 19, 8),
    K("Q", 2, 4), K("W", 6, 4), K("E", 10, 4), K("R", 14, 4), K("T", 18, 4),
    K("1", 0, 0), K("2", 4, 0), K("3", 8, 0), K("4", 12, 0), K("5", 16, 0),
    K("0", 36, 0), K("9", 32, 0), K("8", 28, 0), K("7", 24, 0), K("6", 20, 0),
    K("P", 38, 4), K("O", 34, 4), K("I", 30, 4), K("U", 26, 4), K("Y", 22, 4),
    {"ENTER", "Return", "ENTER", 39, 8, 5, 4}, K("L", 35, 8), K("K", 31, 8), K("J", 27, 8), K("H", 23, 8),
    {"SPACE", "Space", "SPACE", 38, 12, 6, 4}, {"SYMBOL SHIFT", "Left Ctrl", "SYMBOL SHIFT", 34, 12, 4, 4},
    K("M", 30, 12), K("N", 26, 12), K("B", 22, 12),
};
#undef K

enum { KZ = 1, KX, KC, KV, KA, KS, KD, KF, KG, KQ, KW, KE, KR, KT, K1, K2, K3, K4, K5, K0, K9, K8, K7, K6,
       KP, KO, KI, KU, KY, KENTER, KL, KK, KJ, KH, KSPACE, KSS, KM, KN, KB, N_ = ZR_KEY_NONE };

/* What is typed for each character of CP/M. The replacement BIOS (core-hc/software/hc88_bios.asm, scan_key) turns the
 * matrix into ASCII: a letter key gives the capital whether CAPS SHIFT is held or not (CP/M's console command line takes
 * either), CAPS SHIFT and 0 give 8 (backspace) and CAPS SHIFT and SPACE give 3 (control-C); with SYMBOL SHIFT held a key
 * gives the character of its second table (a key's symbol as the BIOS has it, which is not always the Spectrum keycap's:
 * ? < > [ ] ^ ~ | \ { } : and control-C). A lower case letter is the key alone, a capital the key with CAPS SHIFT, so that a BIOS
 * that tells them apart gets them. */
static const zr_typing_desc TYPING[] = {
    {'a', KA, N_}, {'b', KB, N_}, {'c', KC, N_}, {'d', KD, N_}, {'e', KE, N_}, {'f', KF, N_}, {'g', KG, N_},
    {'h', KH, N_}, {'i', KI, N_}, {'j', KJ, N_}, {'k', KK, N_}, {'l', KL, N_}, {'m', KM, N_}, {'n', KN, N_},
    {'o', KO, N_}, {'p', KP, N_}, {'q', KQ, N_}, {'r', KR, N_}, {'s', KS, N_}, {'t', KT, N_}, {'u', KU, N_},
    {'v', KV, N_}, {'w', KW, N_}, {'x', KX, N_}, {'y', KY, N_}, {'z', KZ, N_},
    {'A', KA, CS}, {'B', KB, CS}, {'C', KC, CS}, {'D', KD, CS}, {'E', KE, CS}, {'F', KF, CS}, {'G', KG, CS},
    {'H', KH, CS}, {'I', KI, CS}, {'J', KJ, CS}, {'K', KK, CS}, {'L', KL, CS}, {'M', KM, CS}, {'N', KN, CS},
    {'O', KO, CS}, {'P', KP, CS}, {'Q', KQ, CS}, {'R', KR, CS}, {'S', KS, CS}, {'T', KT, CS}, {'U', KU, CS},
    {'V', KV, CS}, {'W', KW, CS}, {'X', KX, CS}, {'Y', KY, CS}, {'Z', KZ, CS},
    {'0', K0, N_}, {'1', K1, N_}, {'2', K2, N_}, {'3', K3, N_}, {'4', K4, N_},
    {'5', K5, N_}, {'6', K6, N_}, {'7', K7, N_}, {'8', K8, N_}, {'9', K9, N_},
    {' ', KSPACE, N_}, {'\n', KENTER, N_}, {'\r', KENTER, N_}, {'\b', K0, CS}, {'\x03', KC, SS},
    /* SYMBOL SHIFT and a key, from the BIOS's second table */
    {':', KZ, SS}, {'/', KV, SS}, {'~', KA, SS}, {'|', KS, SS}, {'\\', KD, SS}, {'{', KF, SS}, {'}', KG, SS},
    {'<', KQ, SS}, {'>', KW, SS}, {'[', KE, SS}, {']', KR, SS}, {'^', KT, SS},
    {'!', K1, SS}, {'@', K2, SS}, {'#', K3, SS}, {'$', K4, SS}, {'%', K5, SS},
    {'_', K0, SS}, {')', K9, SS}, {'(', K8, SS}, {'\'', K7, SS}, {'&', K6, SS},
    {'"', KP, SS}, {';', KO, SS}, {'?', KI, SS}, {'=', KL, SS}, {'+', KK, SS}, {'-', KJ, SS},
    {'.', KM, SS}, {',', KN, SS}, {'*', KB, SS},
};

/* Host keys the matrix has no key for, as the core's keyboard (hc85_keyboard.sv) maps the PC's: the cursor keys, delete and
 * break with CAPS SHIFT, each punctuation key its own character with SYMBOL SHIFT (and with the host's Shift held the
 * character a PC keyboard has above it), and Ctrl or Alt for SYMBOL SHIFT, either Shift for CAPS SHIFT. The shifted digits
 * are SYMBOL SHIFT and the digit's own key, which the Spectrum keycaps and the BIOS agree on. */
static const zr_combo_desc COMBOS[] = {
    {"Backspace", K0, CS}, {"Left", K5, CS}, {"Down", K6, CS}, {"Up", K7, CS}, {"Right", K8, CS}, {"Escape", KSPACE, CS},
    {".", KM, SS}, {",", KN, SS}, {"'", K7, SS}, {";", KO, SS}, {"-", KJ, SS}, {"=", KL, SS}, {"/", KV, SS},
    {"Shift+'", KP, SS}, {"Shift+;", KZ, SS}, {"Shift+-", K0, SS}, {"Shift+=", KK, SS}, {"Shift+/", KC, SS},
    {"Shift+1", K1, SS}, {"Shift+2", K2, SS}, {"Shift+3", K3, SS}, {"Shift+4", K4, SS}, {"Shift+5", K5, SS},
    {"Shift+7", K6, SS}, {"Shift+8", KB, SS}, {"Shift+9", K8, SS}, {"Shift+0", K9, SS},
    {"Right Shift", CS, N_}, {"Right Ctrl", SS, N_}, {"Left Alt", SS, N_}, {"Right Alt", SS, N_},
};

/* --- the board ------------------------------------------------------------------------------------------ */

typedef struct hc88 {
    const zr_family *family;
    unsigned model;
    char description[160], error[160];
    int tape_sound, drive_sound, core_disk;
    unsigned baud;
    /* the board */
    zr_hw_host host;
    zr_sched sched;
    zr_tclock clock;
    uint64_t target;                            /* where the runs asked so far end */
    zr_bus bus;
    zr_z80_bus cpu_bus;
    zr_z80 z80;
    zr_cpu_hooks hooks;
    uint8_t watch[8192];                        /* the fetch addresses the hooks watch (hw.h) */
    zr_hc88video video;
    zr_beeper beeper;
    zr_drivesnd drivesnd;
    zr_upd765 fdc;
    zr_fdd fdd[2];
    zr_dev motor_dev, tx_dev, rx_dev, tape_dev, drain_dev, key_dev;
    /* memory */
    uint8_t rom[2048];
    uint8_t rom_hash[32];
    uint8_t ram[65536];
    uint8_t vram[ZR_HC88VIDEO_VRAM];
    uint8_t video_quad[4];                      /* the quadrant is on the video RAM (the CPU is held there) */
    /* the latches */
    uint8_t command, shadow, floppy, serial, network, fe;
    uint64_t motor_until;                       /* the motor one-shot runs until this time */
    uint8_t fdc_reset, fdc_tc, ready_pin[2];    /* the levels the controller was last told */
    uint8_t keys[8];
    uint64_t key_t[KEY_QUEUE];                  /* key changes waiting for their time */
    uint8_t key_n[KEY_QUEUE], key_down[KEY_QUEUE];
    unsigned key_count;
    uint64_t key_last;                          /* when the last key change was (or will be) made */
    uint8_t network_wait;
    /* the disks */
    zr_media *media[2];
    zr_disk *disk[2];
    uint64_t flush_at;                          /* a changed disk goes into its media from this time */
    /* the serial line */
    zr_port_host ports;
    uint8_t has_ports, loopback, dsr, rts_sent, rts;
    uint8_t tx_level, tx_active;
    uint8_t tx_l[TX_LOG];
    uint64_t tx_t[TX_LOG];
    unsigned tx_n;
    uint8_t rx_line;
    int rx_bit;                                 /* -1 idle, 0 the start bit, 1-8 the data bits, 9 the stop bit */
    uint8_t rx_byte, rx_queue[RX_QUEUE];
    unsigned rx_count;
    /* the tape recorder */
    zr_media *tape_media;
    zr_tape *tape;
    zr_tape_deck deck;
    /* the text read-back */
    int32_t font_at;
} hc88;

/* The guest time of the cycle in progress (or of the event running). */
static uint64_t board_now(const hc88 *s) { return s->sched.in_event ? s->sched.event_time : zr_tclock_time(&s->clock, s->z80.t); }
/* The guest time `tstates` T-states into the cycle starting now. */
static uint64_t board_at(const hc88 *s, unsigned tstates) {
    return s->sched.in_event ? s->sched.event_time : zr_tclock_time(&s->clock, s->z80.t + tstates);
}
static uint64_t host_now(void *ctx) { return board_now(ctx); }
static void host_schedule(void *ctx, void *dev, uint64_t when) { zr_sched_set(&((hc88 *)ctx)->sched, dev, when); }
static void host_line(void *ctx, void *dev, int pin, int state) {
    hc88 *s = ctx;
    if (dev == &s->video && pin == ZR_HC88VIDEO_INT) zr_z80_int(&s->z80, state);
}

/* --- the memory map ----------------------------------------------------------------------------------- */

/* The map the command latch and the shadow make (hc88_core.sv): per quadrant of 16 KiB, in this priority: the ROM in
 * quadrant 0 while LOAD is clear (reads give the 2 KiB mirrored, writes are lost); the video RAM in quadrant 1 unless MOD0
 * removes it, and in every quadrant with MOD1; in quadrant 0 the shadow (system RAM at 4000-7FFF seen at 0000, whole,
 * writes bypass LOCK); in quadrant 0 with LOCK the system RAM read-only; else the system RAM. */
static void map_quadrant(hc88 *s, unsigned q) {
    const uint16_t first = (uint16_t)(q * 0x4000u);
    const int load = s->command & 1, lock = s->command >> 1 & 1, mod1 = s->command >> 3 & 1, mod0 = s->command >> 5 & 1;
    int video = 0;
    if (q == 0 && !load) {
        for (unsigned p = 0; p < 16u; ++p) zr_bus_map_rom(&s->bus, (uint16_t)(first + p * ZR_BUS_PAGE_SIZE), ZR_BUS_PAGE_SIZE, s->rom + (p & 1u) * ZR_BUS_PAGE_SIZE);
    } else if (mod1 || (!mod0 && q == 1)) {
        zr_bus_map_ram(&s->bus, first, 0x4000u, s->vram);
        video = 1;
    } else if (q == 0 && s->shadow) {
        zr_bus_map_ram(&s->bus, first, 0x4000u, s->ram + 0x4000u);
    } else if (q == 0 && lock) {
        zr_bus_map_read(&s->bus, first, 0x4000u, s->ram);
        zr_bus_map_write(&s->bus, first, 0x4000u, NULL);
    } else zr_bus_map_ram(&s->bus, first, 0x4000u, s->ram + first);
    s->video_quad[q] = (uint8_t)video;
    zr_bus_watch(&s->bus, first, 0x4000u, video);
}

static void map_all(hc88 *s) {
    for (unsigned q = 0; q < 4u; ++q) map_quadrant(s, q);
}

/* The video RAM's pages: the picture is drawn up to the write before it lands (a write lands in the cycle's second T-state). */
static void video_write(void *ctx, uint16_t address, uint8_t value) {
    hc88 *s = ctx;
    (void)address;
    (void)value;
    zr_hc88video_catch_up(&s->video, board_at(s, 1u));
}

/* The shadow mapping on opcode fetches (hw.h's hooks): with LOAD set, 0008-000F and 1708-170F switch it on, 0700-0707 off. The
 * opcode fetched is read through the new map. */
static int hook_before_fetch(void *ctx, uint16_t pc) {
    hc88 *s = ctx;
    if (!(s->command & 1u)) return 0;
    const unsigned group = pc >> 3;
    if (group == 0x001u || group == 0x2E1u) {
        if (!s->shadow) {
            s->shadow = 1;
            map_quadrant(s, 0);
        }
    } else if (group == 0x0E0u && s->shadow) {
        s->shadow = 0;
        map_quadrant(s, 0);
    }
    return 0;
}

static void watch_fetches(hc88 *s) {
    static const uint16_t FIRST[3] = {0x0008u, 0x1708u, 0x0700u};
    memset(s->watch, 0, sizeof s->watch);
    for (unsigned i = 0; i < 3u; ++i)
        for (unsigned a = FIRST[i]; a < FIRST[i] + 8u; ++a) s->watch[a >> 3] = (uint8_t)(s->watch[a >> 3] | 1u << (a & 7u));
    s->hooks.ctx = s;
    s->hooks.watch = s->watch;
    s->hooks.before_fetch = hook_before_fetch;
    s->hooks.after_fetch = NULL;
    s->hooks.refresh = NULL;
}

/* --- the CPU's bus -------------------------------------------------------------------------------------- */

static uint8_t cpu_read(void *ctx, uint16_t address, int m1) {
    (void)m1;
    return zr_bus_read(&((hc88 *)ctx)->bus, address);
}
static void cpu_write(void *ctx, uint16_t address, uint8_t value) { zr_bus_write(&((hc88 *)ctx)->bus, address, value); }
static uint8_t cpu_in(void *ctx, uint16_t port) { return zr_bus_in(&((hc88 *)ctx)->bus, port); }
static void cpu_out(void *ctx, uint16_t port, uint8_t value) { zr_bus_out(&((hc88 *)ctx)->bus, port, value); }
/* Nothing drives the data bus in the acknowledge cycle: FFh. */
static uint8_t cpu_ack(void *ctx) {
    hc88 *s = ctx;
    zr_bus_trace(&s->bus, ZR_BUS_TRACE_INTERRUPT, 0xFFu, 0xFFu);
    return 0xFFu;
}

static int network_tx(const hc88 *s) { return !(s->network & 1u) && !(s->serial & 1u); }
/* The network line is active when the machine itself pulls it or a station does (none is connected: the line is idle). */
static int network_active(const hc88 *s) { return network_tx(s); }

/* The half-slot stop and the network WAIT. The CPU's clock enable is held while the CPU has MREQ low on the video RAM and the
 * raster's phase (the T-state number modulo 4: the core's phase bit 5) is in the second half: 2 T-states of every 4. Each T-state of
 * a video RAM cycle (the two of an opcode fetch's read, the three of a memory read or write) is delayed to the first half. WAIT
 * holds every cycle for a frame at a time while the network line is active and the guest asked to wait for it to go passive. */
static unsigned cpu_stretch(void *ctx, int kind, uint16_t address) {
    hc88 *s = ctx;
    if (kind == ZR_Z80_IDLE) return 0;
    if (s->network_wait) {
        if (!network_active(s)) s->network_wait = 0;
        else return WAIT_TICKS / TICKS_PER_TSTATE;
    }
    if (kind > ZR_Z80_WRITE || !s->video_quad[address >> 14]) return 0;
    unsigned slot = (unsigned)(s->z80.t & 3u), added = 0;
    for (unsigned i = kind == ZR_Z80_FETCH ? 2u : 3u; i; --i) {
        if (slot >= 2u) {
            const unsigned wait = 4u - slot;
            added += wait;
            slot = 0;
        }
        ++slot;
        slot &= 3u;
    }
    return added;
}

/* --- the disks and the motor --------------------------------------------------------------------------------- */

static int motor_on(const hc88 *s, uint64_t now) { return now < s->motor_until; }

/* The compatibility option's other half. The core's controller never queues the interrupt results the data sheet gives for a reset and for
 * a change of READY (its INT is not connected: a program finds them only by asking), and the replacement BIOS, which polls Sense Interrupt
 * Status after a Recalibrate and takes any result but a seek end for an error, does not expect them. The board reads them out as the
 * original bootstrap would (Sense Interrupt Status until 80h), through the controller's bus interface, when the controller is idle and
 * has no seek end of its own to report. */
static void fdc_drain(hc88 *s) {
    zr_upd765 *f = &s->fdc;
    if (!s->core_disk) return;
    for (unsigned guard = 0; guard < 16u; ++guard) {
        int stale = f->reset_pos < f->reset_n, seek = 0;
        for (unsigned u = 0; u < 4u; ++u) {
            if (f->rc_pending[u]) stale = 1;
            if (f->seeking[u] || f->se_pending[u]) seek = 1;
        }
        if (f->in_reset || f->phase != 0 || !stale || seek) return;
        f->dev.ops->write(&f->dev, ZR_SPACE_IO, 1, 0x08);
        if (f->dev.ops->read(&f->dev, ZR_SPACE_IO, 1) == 0x80u) return;
        (void)f->dev.ops->read(&f->dev, ZR_SPACE_IO, 1);
    }
}

/* The reset results come 100 us (of the controller's clock) after the reset ends: the drain waits for them. */
static void drain_event(void *dev, uint64_t now) {
    hc88 *s = ((zr_dev *)dev)->host->ctx;
    fdc_drain(s);
    if (s->core_disk && s->fdc.phase != 0 && !s->fdc.in_reset && (s->fdc.reset_pos < s->fdc.reset_n)) zr_dev_schedule(&s->drain_dev, now + 140u);
}

static void drain_arm(hc88 *s) {
    if (s->core_disk && !s->fdc.in_reset && s->fdc.t_rint != ZR_NEVER) zr_dev_schedule(&s->drain_dev, s->fdc.t_rint + 1u);
}

/* The motors and the READY lines follow the E7 latch: a drive turns while the one-shot runs and its select bit is set (U95 gates
 * the motor with the select); its READY is a disk in the drive and its select bit. */
static void sync_drives(hc88 *s, uint64_t now) {
    for (unsigned u = 0; u < 2u; ++u) {
        const int selected = s->floppy >> (u + 1u) & 1;
        zr_fdd_set_motor(&s->fdd[u], selected && motor_on(s, now), now);
        const uint8_t ready = (uint8_t)(selected && s->disk[u] != NULL);
        if (ready != s->ready_pin[u]) {
            s->ready_pin[u] = ready;
            s->fdc.dev.ops->input(&s->fdc.dev, ZR_UPD765_PIN_READY0 + (int)u, ready);
        }
    }
    const uint8_t tc = s->floppy & 1u;
    if (tc != s->fdc_tc) {
        s->fdc_tc = tc;
        s->fdc.dev.ops->input(&s->fdc.dev, ZR_UPD765_PIN_TC, tc);
    }
    const uint8_t reset = !(s->floppy & 0x10u);
    if (reset != s->fdc_reset) {
        s->fdc_reset = reset;
        s->fdc.dev.ops->input(&s->fdc.dev, ZR_UPD765_PIN_RESET, reset);
    }
    zr_upd765_drive_changed(&s->fdc);
    drain_arm(s);
    fdc_drain(s);
}

static void motor_arm(hc88 *s) {
    zr_dev_schedule(&s->motor_dev, s->motor_until > board_now(s) ? s->motor_until : ZR_NEVER);
}

static void motor_event(void *dev, uint64_t now) {
    hc88 *s = ((zr_dev *)dev)->host->ctx;
    sync_drives(s, now);
}

static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void no_reset(void *dev) { (void)dev; }
static const zr_dev_ops MOTOR_OPS = {.read = no_read, .write = no_write, .event = motor_event, .reset = no_reset};
static const zr_dev_ops DRAIN_OPS = {.read = no_read, .write = no_write, .event = drain_event, .reset = no_reset};

/* --- the keys ----------------------------------------------------------------------------------------------- */

static void key_apply(hc88 *s, unsigned key, int down) {
    if (down) s->keys[key / 5u] = (uint8_t)(s->keys[key / 5u] | 1u << (key % 5u));
    else s->keys[key / 5u] = (uint8_t)(s->keys[key / 5u] & ~(1u << (key % 5u)));
}

static void key_arm(hc88 *s) { zr_dev_schedule(&s->key_dev, s->key_count ? s->key_t[0] : ZR_NEVER); }

static void key_event(void *dev, uint64_t now) {
    hc88 *s = ((zr_dev *)dev)->host->ctx;
    while (s->key_count && s->key_t[0] <= now) {
        key_apply(s, s->key_n[0], s->key_down[0]);
        --s->key_count;
        memmove(s->key_t, s->key_t + 1, s->key_count * sizeof s->key_t[0]);
        memmove(s->key_n, s->key_n + 1, s->key_count);
        memmove(s->key_down, s->key_down + 1, s->key_count);
    }
    key_arm(s);
}
static const zr_dev_ops KEY_OPS = {.read = no_read, .write = no_write, .event = key_event, .reset = no_reset};

/* The keys, as the shell presses them: a key and its shift together, at one instant. The replacement BIOS scans the matrix without
 * stopping (the shift keys first, then the rows, 0.6 ms in all), so a shift pressed between its reading the shift and the row of the key
 * pressed with it would give the key alone: the changes are made 2 ms apart, in the order they came (a switch contact takes
 * about as long to settle). */
static void hc88_key(void *self, unsigned key, int down) {
    hc88 *s = self;
    if (key >= KEYS) return;
    const uint64_t now = board_now(s);
    if (!s->key_count && now >= s->key_last + KEY_SPACING) {
        key_apply(s, key, down);
        s->key_last = now;
        return;
    }
    uint64_t t = s->key_last + KEY_SPACING;
    if (t < now) t = now;
    if (s->key_count == KEY_QUEUE) {
        /* Never so many in a second of typing: the oldest change goes in at once. */
        key_apply(s, s->key_n[0], s->key_down[0]);
        --s->key_count;
        memmove(s->key_t, s->key_t + 1, s->key_count * sizeof s->key_t[0]);
        memmove(s->key_n, s->key_n + 1, s->key_count);
        memmove(s->key_down, s->key_down + 1, s->key_count);
    }
    s->key_t[s->key_count] = t;
    s->key_n[s->key_count] = (uint8_t)key;
    s->key_down[s->key_count++] = (uint8_t)(down != 0);
    s->key_last = t;
    key_arm(s);
}

/* A changed disk goes into its media (the shell writes the file back), when the controller is between commands: at the end of
 * a run at most twice a second, and always when `force` (an eject, the end). */
static void flush_disks(hc88 *s, uint64_t now, int force) {
    if (!force && (now < s->flush_at || s->fdc.phase != 0)) return;
    for (unsigned u = 0; u < 2u; ++u) {
        if (!s->disk[u] || !s->disk[u]->dirty || !s->media[u]) continue;
        char why[128];
        if (zr_disk_save_media(s->disk[u], s->media[u], why, sizeof why)) s->flush_at = now + DISK_FLUSH_TICKS;
        else snprintf(s->error, sizeof s->error, "%s", why);
    }
}

/* --- the serial line ------------------------------------------------------------------------------------------ */

static uint64_t bit_ticks(const hc88 *s) { return CLOCK_HZ / (s->baud ? s->baud : 9600u); }
static int tx_line(const hc88 *s) { return (s->network & 1u) ? !(s->serial & 1u) : 1; }

static void rx_start(hc88 *s, uint64_t now) {
    s->rx_byte = s->rx_queue[0];
    memmove(s->rx_queue, s->rx_queue + 1, --s->rx_count);
    s->rx_bit = 0;
    s->rx_line = 0;
    zr_dev_schedule(&s->rx_dev, now + bit_ticks(s));
}

/* The next bit of the byte on its way to the guest: the start bit, eight data bits (the least significant first), the stop bit. */
static void rx_event(void *dev, uint64_t now) {
    hc88 *s = ((zr_dev *)dev)->host->ctx;
    ++s->rx_bit;
    if (s->rx_bit <= 8) {
        s->rx_line = (uint8_t)(s->rx_byte >> (s->rx_bit - 1) & 1u);
        zr_dev_schedule(&s->rx_dev, now + bit_ticks(s));
    } else if (s->rx_bit == 9) {
        s->rx_line = 1;
        zr_dev_schedule(&s->rx_dev, now + bit_ticks(s));
    } else if (s->rx_count) rx_start(s, now);
    else s->rx_bit = -1;
}

static void rx_put(hc88 *s, const uint8_t *data, size_t count, size_t *taken) {
    *taken = 0;
    while (*taken < count && s->rx_count < RX_QUEUE) s->rx_queue[s->rx_count++] = data[(*taken)++];
    if (s->rx_bit < 0 && s->rx_count) rx_start(s, board_now(s));
}

/* A change of the TX line at time t: a falling edge starts a frame, its edges are kept until the stop bit's middle, when the
 * byte is taken from them (the guest times the bits itself: the middle of each one is where the receiver looks). */
static void tx_change(hc88 *s, int level, uint64_t t) {
    if (!s->tx_active) {
        if (level) return;
        s->tx_active = 1;
        s->tx_n = 1;
        s->tx_t[0] = t;
        s->tx_l[0] = 0;
        zr_dev_schedule(&s->tx_dev, t + bit_ticks(s) * 19u / 2u);
    } else if (s->tx_n < TX_LOG) {
        s->tx_t[s->tx_n] = t;
        s->tx_l[s->tx_n++] = (uint8_t)level;
    }
}

static void tx_event(void *dev, uint64_t now) {
    hc88 *s = ((zr_dev *)dev)->host->ctx;
    const uint64_t t0 = s->tx_t[0], bit = bit_ticks(s);
    unsigned value = 0;
    for (unsigned i = 1; i <= 8u; ++i) {
        const uint64_t sample = t0 + bit * (2u * i + 1u) / 2u;
        unsigned level = 0;
        for (unsigned j = 0; j < s->tx_n && s->tx_t[j] <= sample; ++j) level = s->tx_l[j];
        value |= level << (i - 1u);
    }
    uint64_t later_t[TX_LOG];
    uint8_t later_l[TX_LOG];
    unsigned later = 0;
    for (unsigned j = 0; j < s->tx_n; ++j)
        if (s->tx_t[j] > now) {
            later_t[later] = s->tx_t[j];
            later_l[later++] = s->tx_l[j];
        }
    s->tx_active = 0;
    s->tx_n = 0;
    if (s->loopback) {
        const uint8_t byte = (uint8_t)value;
        size_t taken;
        rx_put(s, &byte, 1, &taken);
    } else if (s->has_ports && s->ports.transmit) s->ports.transmit(s->ports.ctx, 0, (uint8_t)value);
    for (unsigned j = 0; j < later; ++j) tx_change(s, later_l[j], later_t[j]);
}

static const zr_dev_ops TX_OPS = {.read = no_read, .write = no_write, .event = tx_event, .reset = no_reset};
static const zr_dev_ops RX_OPS = {.read = no_read, .write = no_write, .event = rx_event, .reset = no_reset};

/* The RTS output follows EF bit 4 (inverted: serial_cts = ~serial[4]). */
static void send_outputs(hc88 *s) {
    const uint8_t rts = (uint8_t)!(s->serial & 0x10u);
    if (s->rts_sent && rts == s->rts) return;
    s->rts = rts;
    s->rts_sent = 1;
    if (s->has_ports && s->ports.outputs) s->ports.outputs(s->ports.ctx, 0, 1, rts);
}

static void serial_lines_changed(hc88 *s, int tx_before, uint64_t t) {
    const int level = tx_line(s);
    if (level != tx_before) tx_change(s, level, t);
    s->tx_level = (uint8_t)level;
    send_outputs(s);
}

/* --- the tape recorder ---------------------------------------------------------------------------------------- */

static void tape_arm(hc88 *s) {
    uint64_t next = s->tape ? zr_tape_deck_next_edge(&s->deck) : ZR_NEVER;
    const uint64_t now = board_now(s);
    if (next != ZR_NEVER && next <= now) next = now + 1u;
    zr_sched_set(&s->sched, &s->tape_dev, next);
}
static void tape_event(void *dev, uint64_t now) {
    hc88 *s = ((zr_dev *)dev)->host->ctx;
    const int level = zr_tape_deck_level(&s->deck, now);
    if (s->tape_sound) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, level, now);
    tape_arm(s);
}
static const zr_dev_ops TAPE_OPS = {.read = no_read, .write = no_write, .event = tape_event, .reset = no_reset};
static int ear(hc88 *s, uint64_t now) { return s->tape ? zr_tape_deck_level(&s->deck, now) : 0; }

/* --- the ports ------------------------------------------------------------------------------------------------- */

/* Each port is decoded on its own address bits (hc88_core.sv), apart from the others: a port address may reach several. */
#define A(port, bit) ((unsigned)(port) >> (bit) & 1u)

static uint8_t port_read(void *ctx, uint16_t port) {
    hc88 *s = ctx;
    uint8_t di = 0xFFu;
    if (!A(port, 7)) return di;
    if (A(port, 6) && !A(port, 0)) {                     /* FE: the keys, EAR; bits 5 and 7 high */
        uint8_t keys = 0x1Fu;
        for (unsigned r = 0; r < 8u; ++r)
            if (!A(port, 8 + r)) keys &= (uint8_t)~s->keys[r];
        di = (uint8_t)(0xA0u | (ear(s, board_now(s)) ? 0x40u : 0u) | (keys & 0x1Fu));
    }
    if (A(port, 6) && A(port, 0) && !A(port, 5)) di = s->fdc.dev.ops->read(&s->fdc.dev, ZR_SPACE_IO, A(port, 1));
    if (!A(port, 4) && A(port, 3)) di = (uint8_t)(s->dsr ? di | 0x08u : di & ~0x08u);          /* EF: the handshake input in bit 3 */
    if (A(port, 4) && !A(port, 3)) {                                                             /* F7: the receiver in bit 7, the network in bit 0 */
        di = (uint8_t)(s->rx_line ? di & ~0x80u : di | 0x80u);
        di = (uint8_t)(network_active(s) ? di | 0x01u : di & ~0x01u);
    }
    if (!A(port, 4) && !A(port, 3)) di = (uint8_t)(motor_on(s, board_now(s)) ? di & ~0x01u : di | 0x01u);    /* E7: motor feedback */
    return di;
}

static void port_write(void *ctx, uint16_t port, uint8_t value) {
    hc88 *s = ctx;
    if (!A(port, 7)) return;
    /* The core's latches take a write when the cycle ends. */
    const uint64_t done = board_at(s, 4u);
    if (!A(port, 6) && A(port, 0)) {                     /* 9F: the command latch */
        zr_hc88video_catch_up(&s->video, done);
        s->command = value & 0x3Fu;
        if (!(value & 1u)) s->shadow = 0;
        zr_hc88video_set_hires(&s->video, s->command >> 4 & 1u);
        map_all(s);
    }
    if (A(port, 6) && !A(port, 0)) {                     /* FE: border, MIC, speaker */
        zr_hc88video_catch_up(&s->video, done);
        zr_hc88video_set_border(&s->video, value & 7u);
        zr_beeper_set(&s->beeper, ZR_BEEPER_SPEAKER, value >> 4 & 1, done);
        zr_beeper_set(&s->beeper, ZR_BEEPER_MIC, value >> 3 & 1, done);
        if (s->tape && zr_tape_deck_recording(&s->deck)) zr_tape_deck_output(&s->deck, value >> 3 & 1, done);
        s->fe = (uint8_t)(value & 0x1Fu);
    }
    if (A(port, 6) && A(port, 0) && !A(port, 5) && A(port, 1)) s->fdc.dev.ops->write(&s->fdc.dev, ZR_SPACE_IO, 1, value);
    if (!A(port, 4) && !A(port, 3)) {                    /* E7: terminal count, drive select, motor, FDC reset release */
        s->floppy = value;
        if (value & 8u) {
            s->motor_until = done + MOTOR_TICKS;
            motor_arm(s);
        }
        sync_drives(s, done);
    }
    if (!A(port, 4) && A(port, 3)) {                     /* EF: the serial transmitter and handshake */
        const int before = tx_line(s);
        s->serial = value;
        serial_lines_changed(s, before, done);
    }
    if (A(port, 4) && !A(port, 3)) {                     /* F7: the serial mode, the network line */
        const int before = tx_line(s);
        s->network = value;
        if (!(value & 0x20u) && network_active(s)) s->network_wait = 1;
        serial_lines_changed(s, before, done);
    }
}

/* --- the picture, the text ---------------------------------------------------------------------------------------- */

static int hc88_frame(void *self, zr_frame *frame) { return zr_hc88video_frame(&((hc88 *)self)->video, frame); }

/* The Spectrum's address of a line's bitmap byte of a cell, within a bank. */
static unsigned bitmap_offset(unsigned y, unsigned cell) {
    return ((y & 0xC0u) << 5) | ((y & 0x07u) << 8) | ((y & 0x38u) << 2) | cell;
}

/* The character set the system draws with: the HC-family's own (the Spectrum's, 20h-7Fh, 8 bytes a glyph, which the replacement
 * BIOS's terminal draws 6 dots wide from bits 6-1 and the BASIC the HC-85 interpreter loads draws 8 wide) is found in the system RAM
 * where the system keeps it, by the bytes of its first glyphs: the BIOS has it in its image, BASIC in its ROM copy. Where the RAM
 * holds none, ZetRo's own drawing of the character set stands in. */
static const uint8_t FONT_SIGNATURE[24] = {0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x10, 0x10, 0x10, 0x10, 0x00, 0x10, 0x00,
                                           0x00, 0x24, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00};

static const uint8_t *find_font(hc88 *s) {
    if (s->font_at >= 0 && !memcmp(s->ram + s->font_at, FONT_SIGNATURE, sizeof FONT_SIGNATURE)) return s->ram + s->font_at;
    s->font_at = -1;
    for (uint32_t a = 0; a + 768u <= 65536u; ++a)
        if (s->ram[a + 9] == 0x10u && !memcmp(s->ram + a, FONT_SIGNATURE, sizeof FONT_SIGNATURE)) {
            s->font_at = (int32_t)a;
            return s->ram + a;
        }
    return &zr_font8x8_ascii[0][0];
}

/* The pixel (px, py) of the picture as the serializer shows it: 512 wide in the 512-pixel mode, where bytes alternate between
 * the two banks (the bank of byte x/8 is its low bit, its column the rest), 256 wide in the other. */
static unsigned screen_pixel(const hc88 *s, unsigned px, unsigned py) {
    const unsigned byte = px >> 3;
    const unsigned bank = s->command & 0x10u ? byte & 1u : 0u, column = s->command & 0x10u ? byte >> 1 : byte;
    return s->vram[(bank << 13) + bitmap_offset(py, column)] >> (7u - (px & 7u)) & 1u;
}

/* Each cell of the bitmap matched against the character set, as it is or inverted: 80 columns of 6 dots in the 512-pixel mode (the
 * terminal of the CP/M system), 32 of 8 dots in the 256-pixel mode (BASIC); a cell that matches no glyph is '?'. */
static size_t hc88_text(void *self, char *out, size_t capacity) {
    hc88 *s = self;
    const int hires = (s->command & 0x10u) != 0;
    const unsigned cols = hires ? 80u : 32u, width = hires ? 6u : 8u;
    const uint64_t all = width == 8u ? ~(uint64_t)0 : ((uint64_t)1 << (width * 8u)) - 1u;
    const uint8_t *font = find_font(s);
    uint64_t glyph[96];
    for (unsigned g = 0; g < 96u; ++g) {
        uint64_t word = 0;
        for (unsigned y = 0; y < 8u; ++y) {
            const unsigned row = hires ? (font[g * 8u + y] >> 1) & 63u : font[g * 8u + y];
            word = word << width | row;
        }
        glyph[g] = word;
    }
    size_t n = 0;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[96];
        unsigned last = 0;
        for (unsigned col = 0; col < cols; ++col) {
            uint64_t cell = 0;
            for (unsigned y = 0; y < 8u; ++y) {
                unsigned bits = 0;
                for (unsigned k = 0; k < width; ++k) bits = bits << 1 | screen_pixel(s, col * width + k, row * 8u + y);
                cell = cell << width | bits;
            }
            char ch = '?';
            for (unsigned g = 0; g < 96u; ++g)
                if (glyph[g] == cell || glyph[g] == (~cell & all)) { ch = (char)(0x20u + g); break; }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* --- life cycle -------------------------------------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static int fail(char *error, size_t capacity, const char *message, const char *what) {
    if (capacity) snprintf(error, capacity, message, what);
    return 0;
}

static void hc88_destroy(void *self) {
    hc88 *s = self;
    if (!s) return;
    zr_tape_deck_release(&s->deck);
    zr_tape_close(s->tape);
    for (unsigned u = 0; u < 2u; ++u) {
        if (s->disk[u]) {
            if (s->media[u]) flush_disks(s, board_now(s), 1);
            zr_fdd_eject(&s->fdd[u]);
            zr_disk_close(s->disk[u]);
        }
    }
    free(s);
}

static const char *hc88_description(const void *self) { return ((const hc88 *)self)->description; }
static const char *hc88_error(const void *self) { return ((const hc88 *)self)->error; }

/* Power on: the memories cleared, the processor as it wakes; RESET: the processor's RESET pin, which clears the command latch and
 * the other latches too (command, shadow, E7, EF and F7, border, speaker, MIC) and stops the motor, the memories kept. */
static void hc88_reset(void *self, int power_on) {
    hc88 *s = self;
    const uint64_t now = board_now(s);
    zr_hc88video_catch_up(&s->video, now);
    if (power_on) {
        memset(s->ram, 0, sizeof s->ram);
        memset(s->vram, 0, sizeof s->vram);
        zr_z80_power(&s->z80);
    } else zr_z80_reset(&s->z80);
    zr_z80_int(&s->z80, 0);
    s->video.dev.ops->reset(&s->video);
    s->command = 0;
    s->shadow = 0;
    s->network_wait = 0;
    map_all(s);
    const int tx_before = tx_line(s);
    s->floppy = 0;
    s->serial = 0xFFu;
    s->network = 0;
    s->motor_until = 0;
    zr_dev_schedule(&s->motor_dev, ZR_NEVER);
    sync_drives(s, now);
    serial_lines_changed(s, tx_before, now);
    s->tx_active = 0;
    s->tx_n = 0;
    zr_dev_schedule(&s->tx_dev, ZR_NEVER);
    zr_dev_schedule(&s->rx_dev, ZR_NEVER);
    s->rx_bit = -1;
    s->rx_line = 1;
    s->rx_count = 0;
    s->fe = 0;
    zr_beeper_set(&s->beeper, ZR_BEEPER_SPEAKER, 0, now);
    zr_beeper_set(&s->beeper, ZR_BEEPER_MIC, 0, now);
    if (s->tape && zr_tape_deck_recording(&s->deck)) zr_tape_deck_output(&s->deck, 0, now);
    if (power_on) {
        memset(s->keys, 0, sizeof s->keys);
        s->key_count = 0;
        s->key_last = 0;
        zr_dev_schedule(&s->key_dev, ZR_NEVER);
    }
}

static hc88 *hc88_create_board(const zr_machine_setup *setup, char *error, size_t capacity) {
    hc88 *s = calloc(1, sizeof *s);
    if (!s) {
        fail(error, capacity, "%s", "Out of memory");
        return NULL;
    }
    s->family = setup->family;
    s->model = setup->model;
    s->tape_sound = 1;
    s->drive_sound = setup->options[OPTION_DRIVE_SOUND] != 0;
    s->core_disk = setup->options[OPTION_DISK] != 0;
    s->baud = BAUDS[setup->options[OPTION_BAUD] < sizeof BAUDS / sizeof BAUDS[0] ? setup->options[OPTION_BAUD] : 5u];
    s->font_at = -1;
    s->dsr = 1;
    s->rx_line = 1;
    s->rx_bit = -1;
    s->serial = 0xFFu;
    snprintf(s->description, sizeof s->description, "%s", setup->family->models[setup->model].name);
    /* The bootstrap. */
    const zr_known_rom *rom = zr_romdb_find("hc88");
    const zr_rom_file *file = rom && setup->roms ? zr_rom_index_find(setup->roms, rom) : NULL;
    if (!file || !file->data || file->size != 2048u) {
        fail(error, capacity, "The ROM %s is missing", rom ? rom->file : "hc88");
        free(s);
        return NULL;
    }
    memcpy(s->rom, file->data, sizeof s->rom);
    zr_sha256(s->rom, sizeof s->rom, s->rom_hash);
    /* The board: host, scheduler, clock. */
    s->host.ctx = s;
    s->host.now = host_now;
    s->host.schedule = host_schedule;
    s->host.line = host_line;
    zr_sched_init(&s->sched);
    zr_tclock_init(&s->clock, TICKS_PER_TSTATE, 0, 0);
    /* The bus: the map from the command latch, the video pages watched, one decoder for the whole port space. */
    zr_bus_init(&s->bus);
    zr_bus_set_hook(&s->bus, video_write, s);
    zr_bus_add_io(&s->bus, 0x0000u, 0x0000u, port_read, port_write, s);
    map_all(s);
    /* The CPU. */
    s->cpu_bus.ctx = s;
    s->cpu_bus.read = cpu_read;
    s->cpu_bus.write = cpu_write;
    s->cpu_bus.in = cpu_in;
    s->cpu_bus.out = cpu_out;
    s->cpu_bus.ack = cpu_ack;
    s->cpu_bus.stretch = cpu_stretch;
    watch_fetches(s);
    zr_z80_init(&s->z80, &s->cpu_bus, &s->hooks, 0);
    /* The chips. */
    zr_hc88video_init(&s->video, &s->host, 1u, s->vram);
    zr_beeper_init(&s->beeper, &s->host, CLOCK_HZ, AUDIO_RATE);
    zr_drivesnd_init(&s->drivesnd, CLOCK_HZ, AUDIO_RATE);
    zr_drivesnd_set(&s->drivesnd, s->drive_sound, 50);
    /* The floppy controller. Original: the 8272 on its 4 MHz CLK at 250 kbit/s (WRCLK 0.5 MHz), two 300 rpm drives, RDY tied high (sheets 10
     * and 11). Core-compatible (the default): what the core's controller does and the replacement BIOS needs: the
     * disks turn at 150 rpm at 125 kbit/s (a byte every 64 us, which the BIOS's polling loops keep up with; layout and sector times
     * scale together), the Specify times of an 8 MHz clock, READY the mounted disk of the drive E7 selects. */
    zr_upd765_init(&s->fdc, &s->host, "fdc", CLOCK_HZ);
    zr_upd765_set_rate(&s->fdc, s->core_disk ? 125u : 250u);
    zr_upd765_set_clock(&s->fdc, s->core_disk ? 8000u : 4000u);
    zr_upd765_ready_wiring(&s->fdc, s->core_disk ? ZR_UPD765_READY_PINS : ZR_UPD765_READY_TIED);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_fdd_init(&s->fdd[u], CLOCK_HZ, 1, 80, s->core_disk ? 150u : 300u);
        if (s->drive_sound) zr_fdd_set_hook(&s->fdd[u], zr_drivesnd_event, &s->drivesnd, u);
        zr_upd765_attach(&s->fdc, u, &s->fdd[u]);
    }
    /* E7 is 0 at power on: the controller is held in reset */
    s->fdc.dev.ops->input(&s->fdc.dev, ZR_UPD765_PIN_RESET, 1);
    s->fdc_reset = 1;
    s->motor_dev.ops = &MOTOR_OPS;
    s->motor_dev.host = &s->host;
    s->motor_dev.name = "motor";
    s->drain_dev.ops = &DRAIN_OPS;
    s->drain_dev.host = &s->host;
    s->drain_dev.name = "fdc drain";
    s->key_dev.ops = &KEY_OPS;
    s->key_dev.host = &s->host;
    s->key_dev.name = "keys";
    s->tx_dev.ops = &TX_OPS;
    s->tx_dev.host = &s->host;
    s->tx_dev.name = "serial tx";
    s->rx_dev.ops = &RX_OPS;
    s->rx_dev.host = &s->host;
    s->rx_dev.name = "serial rx";
    s->tape_dev.ops = &TAPE_OPS;
    s->tape_dev.host = &s->host;
    s->tape_dev.name = "tape";
    zr_tape_deck_init(&s->deck, CLOCK_HZ);
    zr_tape_deck_set_48k(&s->deck, 1);
    hc88_reset(s, 1);
    return s;
}

static void *hc88_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    return hc88_create_board(setup, error, capacity);
}

/* --- time ------------------------------------------------------------------------------------------------------- */

static uint32_t hc88_clock(const void *self) { (void)self; return CLOCK_HZ; }
static uint64_t hc88_time(const void *self) { return board_now(self); }
static uint64_t hc88_frame_period(const void *self) { return zr_hc88video_frame_ticks(&((const hc88 *)self)->video); }

static zr_run_status hc88_run(void *self, uint64_t ticks) {
    hc88 *s = self;
    s->target += ticks;
    const uint64_t now = zr_sched_run_z80(&s->sched, &s->z80, &s->clock, s->target);
    zr_hc88video_catch_up(&s->video, now);
    if (s->tape) zr_tape_deck_advance(&s->deck, now);
    flush_disks(s, now, 0);
    return ZR_RUN_OK;
}

/* --- sound ------------------------------------------------------------------------------------------------------ */

static uint32_t hc88_audio_rate(const void *self) { (void)self; return AUDIO_RATE; }
static size_t hc88_audio_pull(void *self, int16_t *stereo, size_t frames) {
    hc88 *s = self;
    const uint64_t now = board_now(s);
    const size_t n = zr_beeper_render(&s->beeper, now, stereo, frames);
    if (zr_drivesnd_enabled(&s->drivesnd)) zr_drivesnd_render_add(&s->drivesnd, now, stereo, n);
    return n;
}

/* --- input ------------------------------------------------------------------------------------------------------ */

/* --- media: the drives and the tape recorder ---------------------------------------------------------------------- */

static int hc88_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    hc88 *s = self;
    if (slot == SLOT_TAPE) {
        if (s->tape_media) return fail(error, capacity, "%s", "The slot is not empty");
        zr_tape *tape = zr_tape_open(media, error, capacity);
        if (!tape) return 0;
        s->tape_media = media;
        s->tape = tape;
        zr_tape_deck_set_tape(&s->deck, tape);
        tape_arm(s);
        return 1;
    }
    if (slot > SLOT_B) return fail(error, capacity, "%s", "There is no such slot");
    if (s->media[slot]) return fail(error, capacity, "%s", "The slot is not empty");
    static const zr_disk_geom GEOMETRY = {80, 2, 9, 512, ZR_ENC_MFM};
    zr_disk *disk = zr_disk_open(media, &GEOMETRY, error, capacity);
    if (!disk) return 0;
    if (!zr_fdd_insert(&s->fdd[slot], disk, error, capacity)) {
        zr_disk_close(disk);
        return 0;
    }
    s->media[slot] = media;
    s->disk[slot] = disk;
    sync_drives(s, board_now(s));
    return 1;
}

static zr_media *hc88_media(const void *self, unsigned slot) {
    const hc88 *s = self;
    return slot == SLOT_TAPE ? s->tape_media : slot < 2u ? s->media[slot] : NULL;
}

/* A recording under way goes onto the tape, and the tape into its file. */
static void finish_recording(hc88 *s) {
    if (!s->tape || !zr_tape_deck_recording(&s->deck)) return;
    char why[128];
    if (zr_tape_deck_record(&s->deck, 0, board_now(s)) && zr_tape_modified(s->tape))
        if (!zr_tape_save_media(s->tape, s->tape_media, NULL, why, sizeof why)) snprintf(s->error, sizeof s->error, "%s", why);
}

static int hc88_eject(void *self, unsigned slot) {
    hc88 *s = self;
    if (slot == SLOT_TAPE) {
        if (!s->tape_media) return 0;
        finish_recording(s);
        zr_tape_deck_set_tape(&s->deck, NULL);
        zr_tape_close(s->tape);
        s->tape = NULL;
        s->tape_media = NULL;
        tape_arm(s);
        if (s->tape_sound) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, 0, board_now(s));
        return 1;
    }
    if (slot > SLOT_B || !s->media[slot]) return 0;
    flush_disks(s, board_now(s), 1);
    zr_fdd_eject(&s->fdd[slot]);
    zr_disk_close(s->disk[slot]);
    s->disk[slot] = NULL;
    s->media[slot] = NULL;
    sync_drives(s, board_now(s));
    return 1;
}

static unsigned hc88_activity(const void *self) {
    const hc88 *s = self;
    unsigned bits = 0;
    for (unsigned u = 0; u < 2u; ++u)
        if (s->disk[u] && s->fdd[u].motor) bits |= 1u << u;
    if (s->tape && (zr_tape_deck_playing(&s->deck) || zr_tape_deck_recording(&s->deck))) bits |= 1u << SLOT_TAPE;
    return bits;
}

static unsigned hc88_tape_options(void *self, unsigned slot, unsigned bits) {
    hc88 *s = self;
    if (slot != SLOT_TAPE) return 0;
    if (bits & ZR_TAPE_QUERY) return ZR_TAPE_SOUND;
    if (s->tape_sound && !(bits & ZR_TAPE_SOUND)) zr_beeper_set(&s->beeper, ZR_BEEPER_EAR, 0, board_now(s));
    s->tape_sound = (bits & ZR_TAPE_SOUND) != 0;
    return ZR_TAPE_SOUND;
}

static int hc88_control(void *self, unsigned slot, unsigned command, uint32_t value) {
    hc88 *s = self;
    if (slot != SLOT_TAPE || !s->tape) return 0;
    const uint64_t now = board_now(s);
    if (command != ZR_MEDIA_RECORD) finish_recording(s);
    switch (command) {
    case ZR_MEDIA_PLAY: if (!zr_tape_deck_play(&s->deck, now)) return 0; break;
    case ZR_MEDIA_STOP: zr_tape_deck_stop(&s->deck, now); break;
    case ZR_MEDIA_REWIND: zr_tape_deck_rewind(&s->deck, now); break;
    case ZR_MEDIA_NEXT: zr_tape_deck_next(&s->deck, now); break;
    case ZR_MEDIA_PREVIOUS: zr_tape_deck_previous(&s->deck, now); break;
    case ZR_MEDIA_SEEK: zr_tape_deck_seek(&s->deck, value, now); break;
    case ZR_MEDIA_RECORD:
        if (zr_tape_deck_recording(&s->deck)) finish_recording(s);
        else if (!zr_tape_deck_record(&s->deck, 1, now)) return 0;
        else zr_tape_deck_output(&s->deck, s->fe >> 3 & 1u, now);
        break;
    default: return 0;
    }
    tape_arm(s);
    return 1;
}

static struct zr_tape_deck *hc88_tape_deck(void *self, unsigned slot) {
    hc88 *s = self;
    return slot == SLOT_TAPE && s->tape ? &s->deck : NULL;
}

/* --- the serial port ---------------------------------------------------------------------------------------------- */

static void hc88_set_ports(void *self, const zr_port_host *host) {
    hc88 *s = self;
    s->has_ports = host != NULL;
    if (host) s->ports = *host;
    else memset(&s->ports, 0, sizeof s->ports);
    s->loopback = host && host->loopback && host->loopback(host->ctx, 0) ? 1u : 0u;
    s->rts_sent = 0;
    send_outputs(s);
}

static size_t hc88_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    hc88 *s = self;
    size_t taken = 0;
    if (port != 0) return count;
    rx_put(s, data, count, &taken);
    return taken;
}

static void hc88_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    hc88 *s = self;
    (void)dcd;
    (void)cts;
    (void)ri;
    if (port == 0) s->dsr = dsr != 0;
}

/* --- states ------------------------------------------------------------------------------------------------------- */

static void hc88_save(const void *self, zr_state_writer *w) {
    const hc88 *s = self;
    zr_sw_begin(w, "HC88", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_tclock_save(&s->clock, w);
    zr_sw_u64(w, s->target);
    zr_sw_u8(w, s->command);
    zr_sw_u8(w, s->shadow);
    zr_sw_u8(w, s->floppy);
    zr_sw_u8(w, s->serial);
    zr_sw_u8(w, s->network);
    zr_sw_u8(w, s->fe);
    zr_sw_u64(w, s->motor_until);
    zr_sw_bytes(w, s->keys, sizeof s->keys);
    zr_sw_u64(w, s->key_last);
    zr_sw_u32(w, s->key_count);
    for (unsigned i = 0; i < KEY_QUEUE; ++i) {
        zr_sw_u64(w, s->key_t[i]);
        zr_sw_u8(w, s->key_n[i]);
        zr_sw_u8(w, s->key_down[i]);
    }
    zr_sw_u8(w, s->network_wait);
    zr_sw_u8(w, s->dsr);
    zr_sw_u8(w, s->tx_level);
    zr_sw_u8(w, s->tx_active);
    zr_sw_u32(w, s->tx_n);
    for (unsigned i = 0; i < TX_LOG; ++i) {
        zr_sw_u64(w, s->tx_t[i]);
        zr_sw_u8(w, s->tx_l[i]);
    }
    zr_sw_u8(w, s->rx_line);
    zr_sw_u32(w, (uint32_t)s->rx_bit);
    zr_sw_u8(w, s->rx_byte);
    zr_sw_u32(w, s->rx_count);
    zr_sw_bytes(w, s->rx_queue, sizeof s->rx_queue);
    zr_sw_end(w);
    zr_sw_begin(w, "Z80 ", 0);
    zr_z80_save(&s->z80, w);
    zr_sw_end(w);
    zr_sw_begin(w, "RAM ", 0);
    zr_sw_packed(w, s->ram, sizeof s->ram);
    zr_sw_end(w);
    zr_sw_begin(w, "VRAM", 0);
    zr_sw_packed(w, s->vram, sizeof s->vram);
    zr_sw_end(w);
    zr_sw_begin(w, "VID ", 0);
    s->video.dev.ops->save(&s->video, w);
    zr_sw_end(w);
    zr_sw_begin(w, "BEEP", 0);
    s->beeper.dev.ops->save(&s->beeper, w);
    zr_sw_end(w);
    zr_sw_begin(w, "FDC ", 0);
    s->fdc.dev.ops->save(&s->fdc, w);
    zr_sw_end(w);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_sw_begin(w, "FDD ", u);
        zr_fdd_save(&s->fdd[u], w);
        zr_sw_end(w);
    }
    if (s->tape) {
        zr_sw_begin(w, "TAPE", 0);
        zr_tape_deck_save(&s->deck, w);
        zr_sw_end(w);
    }
}

static int hc88_load(void *self, zr_state_reader *r) {
    hc88 *s = self;
    /* The clock and the CPU first: the chips' loads arm their deadlines at the time these give. */
    if (!zr_sr_chunk(r, "HC88", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_tclock_load(&s->clock, r);
    s->target = zr_sr_u64(r);
    s->command = zr_sr_u8(r) & 0x3Fu;
    s->shadow = zr_sr_u8(r) & 1u;
    s->floppy = zr_sr_u8(r);
    s->serial = zr_sr_u8(r);
    s->network = zr_sr_u8(r);
    s->fe = zr_sr_u8(r) & 0x1Fu;
    s->motor_until = zr_sr_u64(r);
    zr_sr_bytes(r, s->keys, sizeof s->keys);
    for (unsigned i = 0; i < 8u; ++i) s->keys[i] &= 0x1Fu;
    s->key_last = zr_sr_u64(r);
    s->key_count = zr_sr_u32(r);
    if (s->key_count > KEY_QUEUE) return 0;
    for (unsigned i = 0; i < KEY_QUEUE; ++i) {
        s->key_t[i] = zr_sr_u64(r);
        s->key_n[i] = zr_sr_u8(r);
        s->key_down[i] = zr_sr_u8(r) & 1u;
        if (s->key_n[i] >= KEYS) s->key_n[i] = 0;
    }
    s->network_wait = zr_sr_u8(r) & 1u;
    s->dsr = zr_sr_u8(r) & 1u;
    s->tx_level = zr_sr_u8(r) & 1u;
    s->tx_active = zr_sr_u8(r) & 1u;
    s->tx_n = zr_sr_u32(r);
    if (s->tx_n > TX_LOG) return 0;
    for (unsigned i = 0; i < TX_LOG; ++i) {
        s->tx_t[i] = zr_sr_u64(r);
        s->tx_l[i] = zr_sr_u8(r) & 1u;
    }
    s->rx_line = zr_sr_u8(r) & 1u;
    s->rx_bit = (int)(int32_t)zr_sr_u32(r);
    s->rx_byte = zr_sr_u8(r);
    s->rx_count = zr_sr_u32(r);
    if (s->rx_count > RX_QUEUE || s->rx_bit < -1 || s->rx_bit > 9) return 0;
    zr_sr_bytes(r, s->rx_queue, sizeof s->rx_queue);
    if (!zr_sr_chunk(r, "Z80 ", 0)) return 0;
    zr_z80_load(&s->z80, r);
    if (!zr_sr_chunk(r, "RAM ", 0)) return 0;
    zr_sr_packed(r, s->ram, sizeof s->ram);
    if (!zr_sr_chunk(r, "VRAM", 0)) return 0;
    zr_sr_packed(r, s->vram, sizeof s->vram);
    if (!zr_sr_chunk(r, "VID ", 0) || !s->video.dev.ops->load(&s->video, r)) return 0;
    if (!zr_sr_chunk(r, "BEEP", 0) || !s->beeper.dev.ops->load(&s->beeper, r)) return 0;
    if (!zr_sr_chunk(r, "FDC ", 0) || !s->fdc.dev.ops->load(&s->fdc, r)) return 0;
    for (unsigned u = 0; u < 2u; ++u)
        if (!zr_sr_chunk(r, "FDD ", u) || !zr_fdd_load(&s->fdd[u], r)) return 0;
    /* zr_upd765's load sets the drive of the command in progress on its own local copy, not on the chip (upd765.c, chip_load): the
     * board puts it where it belongs, or a command that was running when the state was saved would use no drive. */
    s->fdc.x.fdd = s->fdc.drive[s->fdc.x.unit & 3u];
    /* The recorder's position, when the state has one and a tape is in. */
    if (s->tape) {
        /* A state of another tape leaves this one rewound. */
        if (!zr_sr_chunk(r, "TAPE", 0) || !zr_tape_deck_load(&s->deck, r)) zr_tape_deck_rewind(&s->deck, board_now(s));
    }
    /* What the loads leave to the board: the map, the levels the controller holds, the deadlines of the board's own devices. */
    s->font_at = -1;
    map_all(s);
    s->fdc_reset = !(s->floppy & 0x10u);
    s->fdc_tc = s->floppy & 1u;
    for (unsigned u = 0; u < 2u; ++u) s->ready_pin[u] = (uint8_t)((s->floppy >> (u + 1u) & 1u) && s->disk[u] != NULL);
    zr_dev_schedule(&s->motor_dev, s->motor_until > board_now(s) ? s->motor_until : ZR_NEVER);
    drain_arm(s);
    key_arm(s);
    zr_dev_schedule(&s->tx_dev, s->tx_active ? s->tx_t[0] + bit_ticks(s) * 19u / 2u : ZR_NEVER);
    zr_dev_schedule(&s->rx_dev, ZR_NEVER);
    if (s->rx_bit >= 0) zr_dev_schedule(&s->rx_dev, board_now(s) + bit_ticks(s));
    s->rts_sent = 0;
    send_outputs(s);
    zr_drivesnd_reset(&s->drivesnd, board_now(s));
    tape_arm(s);
    s->flush_at = 0;
    return zr_sr_ok(r);
}

static uint32_t hc88_rom_hashes(const void *self, uint8_t *hashes, uint32_t max) {
    const hc88 *s = self;
    if (!max) return 0;
    memcpy(hashes, s->rom_hash, 32);
    return 1;
}

/* --- checks -------------------------------------------------------------------------------------------------------- */

static void hc88_set_trace(void *self, zr_trace_fn fn, void *ctx) { zr_bus_set_trace(&((hc88 *)self)->bus, fn, ctx); }

/* The memory as the CPU reads it now; from 100000h the RAM pages by number (machine.h's ZR_PAGE_ADDRESS): pages 0-3 the system RAM's
 * quadrants whatever is mapped over them, page 4 the video RAM. */
static uint8_t *page_byte(hc88 *s, uint32_t address) {
    const uint32_t page = (address - 0x100000u) >> 14, offset = address & 0x3FFFu;
    if (page < 4u) return s->ram + page * 0x4000u + offset;
    if (page == 4u) return s->vram + offset;
    return NULL;
}
static int hc88_peek(void *self, uint32_t address, uint8_t *value) {
    hc88 *s = self;
    if (address > 0xFFFFu) {
        const uint8_t *p = page_byte(s, address);
        if (!p) return 0;
        *value = *p;
        return 1;
    }
    *value = zr_bus_peek(&s->bus, (uint16_t)address);
    return 1;
}
static int hc88_poke(void *self, uint32_t address, uint8_t value) {
    hc88 *s = self;
    if (address > 0xFFFFu) {
        uint8_t *p = page_byte(s, address);
        if (!p) return 0;
        if (p >= s->vram && p < s->vram + sizeof s->vram) zr_hc88video_catch_up(&s->video, board_now(s));
        *p = value;
        return 1;
    }
    zr_bus_write(&s->bus, (uint16_t)address, value);
    return 1;
}

static const zr_machine_impl HC88_IMPL = {
    .destroy = hc88_destroy, .description = hc88_description, .error = hc88_error,
    .reset = hc88_reset, .nmi = NULL,
    .clock = hc88_clock, .run = hc88_run, .time = hc88_time, .frame_period = hc88_frame_period,
    .frame = hc88_frame, .text = hc88_text, .audio_rate = hc88_audio_rate, .audio_pull = hc88_audio_pull,
    .key = hc88_key, .joystick = NULL,
    .insert = hc88_insert, .media = hc88_media, .eject = hc88_eject, .activity = hc88_activity, .control = hc88_control, .tape_options = hc88_tape_options,
    .set_ports = hc88_set_ports, .port_receive = hc88_port_receive, .port_modem = hc88_port_modem,
    .save = hc88_save, .load = hc88_load, .rom_hashes = hc88_rom_hashes,
    .set_trace = hc88_set_trace, .peek = hc88_peek, .poke = hc88_poke, .tape_deck = hc88_tape_deck,
};

const zr_family zr_hc88_family = {
    .id = "hc88", .name = "HC",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOTS,
    .ports = PORTS, .port_count = 1,
    .keys = KEY_TABLE, .key_count = KEYS,
    .typing = TYPING, .typing_count = sizeof TYPING / sizeof TYPING[0],
    .combos = COMBOS, .combo_count = sizeof COMBOS / sizeof COMBOS[0],
    .joysticks = 0,
    .impl = &HC88_IMPL, .create = hc88_create,
};
