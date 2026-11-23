/* The CoBra of ITCI Brasov (machine.h, machine_impl.h): the family "cobra" with its two models, the original 64K board
 * and the documented 80K upgrade, a Spectrum-compatible with CP/M on the shared core (spectrum.h), as this project's
 * CoBra core documents it (MiSTerRO/docs/cobra: README.md, hardware.md, the reconstructed schematic and the hardware and
 * CP/M manuals; core-cobra/rtl/machine_core.sv with the shared rp_cobra_video.sv, rp_i8255.sv, rp_z80ctc.sv):
 *   - the Z80 at 3.5 MHz from the 14 MHz master clock (the core's 56 MHz divided by 16); no memory contention;
 *   - the startup overlay: a D flip-flop (U36A, sheet config_selection) takes A7 of the address bus in every refresh
 *     cycle (bit 7 of R), set at power on and by RESET and held set for a moment after it (the core's 4095 clocks: 256
 *     T-states); while it is set the bootstrap (2 KiB, mirrored over 0000h-3FFFh) and a 16 KiB system ROM (4000h-7FFFh)
 *     are read below 8000h and nothing is written there; when it falls, a second flip-flop (U36B) latches PC6: 1 is
 *     CP/M, 0 is BASIC, which also clears the first flip-flop for good (until RESET) and write-protects RAM bank 0;
 *   - the memory maps of the 64K board (one 64 KiB DRAM, banks moved by 8 KiB in the overlay and in CP/M, the screen in
 *     physical 4000h-7FFFh) and of the 80K (64 KiB plus 16 KiB of video RAM of its own, which PC6 maps at C000h in the
 *     overlay and at 4000h in CP/M, and which BASIC's writes to 4000h-7FFFh also reach); the 80K's system ROM is four
 *     quarters chosen by PC3 and PC7, the 64K's its fixed BASIC EPROM;
 *   - the video (cobravideo.h), with the 80-column modification (PC5, in CP/M, when the option has it fitted);
 *   - the 8255: port A on FE read (the five key bits, the sixth native input, EAR on bit 6, serial RX on bit 7), port C
 *     on FE write (border, PC3 MIC and quarter, PC4 speaker, PC5 80 columns, PC6 BASIC/CP/M, PC7 serial TX), port B on
 *     1F read (the Kempston-compatible joystick or eight external parallel lines, and the frame pulse on the 80K), the
 *     control word on DF;
 *   - the Z80 CTC at E3/EB/F3/FB: channel 0 triggered by the 8272's INT or DRQ, 1 and 2 by the ZC/TO of the one before,
 *     3 by the frame pulse; ZC/TO2 is the 8272's terminal count; its interrupts in mode 2 through the daisy chain, RETI
 *     seen on the bus; the frame pulse is also a direct interrupt while BASIC runs;
 *   - the 8272 at F5 (status) and FD (data) with four drives, each 5.25-inch (300 rpm, 250 kbit/s) or 8-inch (360 rpm,
 *     500 kbit/s), motors always on, READY from the drives;
 *   - the keyboard: the Spectrum's matrix plus the native keys on the sixth input, TAB as EXTEND then P;
 *   - beeper and MIC, the tape, the NMI button, a bit-banged serial line, no AY (the AY board is not original).
 * ZetRo's own code. */
#include "spectrum.h"
#include "cobravideo.h"
#include "i8255.h"
#include "z80ctc.h"
#include "upd765.h"
#include "fdd.h"
#include "drivesnd.h"
#include "media/disk.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define CLOCK_HZ 14000000u
#define BANK 0x4000u
#define ROM_BOOT64 0x3000u                      /* where the combined image has each part */
#define ROM_BOOT80 0x3800u
#define ROM_SYSTEM 0x4000u
#define ROM_BASIC (ROM_SYSTEM + BANK)           /* quarter 1: the 48K BASIC both models start with B */
#define LATCH_HOLD 256u                         /* T-states the latch ignores refresh after RESET (the core's 4095 clocks) */
#define TAB_PHASE_TICKS 560000u                 /* 40 ms: the core's TAB sequence (EXTEND, nothing, P) */
#define DISK_FLUSH_TICKS (CLOCK_HZ / 2u)
#define TX_LOG 32
#define RX_QUEUE 16

static const char *const ROMS[] = {"cobra", NULL};
/* LOAD "" in BASIC (J, then SYMBOL SHIFT and P twice). */
enum { MODEL_80K, MODEL_64K, MODEL_COUNT };
static const zr_model_desc MODELS[MODEL_COUNT] = {
    {"cobra80", "CoBra 80K", ROMS, "j\"\"\n"},
    {"cobra64", "CoBra 64K", ROMS, "j\"\"\n"},
};

static const char *const SWITCH[] = {"off", "on", NULL};
static const char *const STARTUPS[] = {"menu", "basic", "cassette", "cpm", NULL};
static const int STARTUP_LABELS[] = {ZR_T_COBRA_STARTUP_MENU, ZR_T_COBRA_STARTUP_BASIC, ZR_T_COBRA_STARTUP_CASSETTE, ZR_T_COBRA_STARTUP_CPM};
static const char *const DRIVES[] = {"5in", "8in", NULL};
static const int DRIVE_LABELS[] = {ZR_T_COBRA_DRIVE_525, ZR_T_COBRA_DRIVE_8};
static const int STICK_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_KEMPSTON);

enum { OPTION_JOYSTICK, OPTION_COL80, OPTION_PARALLEL, OPTION_STARTUP, OPTION_DRIVE_A, OPTION_DRIVE_B,
       OPTION_DRIVE_C, OPTION_DRIVE_D, OPTION_DRIVE_SOUND, OPTION_AY,
       OPTION_COUNT };
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK, ZR_SPECTRUM_JOY_KEMPSTON, STICK_LABELS),
    {"col80", ZR_T_COBRA_COL80, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET, ZR_T_COBRA_COL80_HINT + 1},
    {"parallel", ZR_T_COBRA_PARALLEL, SWITCH, NULL, 0, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_PORT, ZR_T_COBRA_PARALLEL_HINT + 1},
    {"startup", ZR_T_COBRA_STARTUP, STARTUPS, STARTUP_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET, ZR_T_COBRA_STARTUP_HINT + 1},
    {"drive_a", ZR_T_COBRA_DRIVE_A, DRIVES, DRIVE_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
    {"drive_b", ZR_T_COBRA_DRIVE_B, DRIVES, DRIVE_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
    {"drive_c", ZR_T_COBRA_DRIVE_C, DRIVES, DRIVE_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
    {"drive_d", ZR_T_COBRA_DRIVE_D, DRIVES, DRIVE_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
    {"drive_sound", ZR_T_COBRA_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_NOT_ORIGINAL, ZR_T_COBRA_DRIVE_SOUND_HINT + 1},
    ZR_SPECTRUM_OPTION_AY(ZR_OPTION_NOT_ORIGINAL),
};

enum { SLOT_TAPE, SLOT_A, SLOT_B, SLOT_C, SLOT_D, SLOT_COUNT };
static const zr_media_desc MEDIA[SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    {"a", ZR_T_COBRA_FLOPPY_A, ZR_MEDIA_FLOPPY, "dsk,edsk", ZR_MODELS_ALL},
    {"b", ZR_T_COBRA_FLOPPY_B, ZR_MEDIA_FLOPPY, "dsk,edsk", ZR_MODELS_ALL},
    {"c", ZR_T_COBRA_FLOPPY_C, ZR_MEDIA_FLOPPY, "dsk,edsk", ZR_MODELS_ALL},
    {"d", ZR_T_COBRA_FLOPPY_D, ZR_MEDIA_FLOPPY, "dsk,edsk", ZR_MODELS_ALL},
};

static const zr_port_desc PORTS[] = {{"serial", ZR_T_COBRA_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL}};

/* --- the keyboard ---------------------------------------------------------------------------------------------------- */

/* The Spectrum's 40 keys (SYMBOL SHIFT on Alt: the core gives Ctrl to the native CTRL), the eight native keys of the sixth
 * input, one a row (A8 to A15, as the core's `extra` wires them: CTRL, PRINT, SCROLL, ESC, F1-F4, on the PC keys the core
 * uses, but PRINT on Page Down and F2-F4 on Insert, Home and Page Up, as the shell keeps the PC's Print Screen and F2-F4), and TAB. */
#define K(name, x, y) {name, name, name, x, y, 4, 4}
enum { KEY_NATIVE = 40, NATIVES = 8, KEY_TAB = KEY_NATIVE + NATIVES, KEYS };
static const zr_key_desc KEY_TABLE[KEYS] = {
    {"CAPS SHIFT", "Left Shift", "CAPS SHIFT", 0, 12, 6, 4}, K("Z", 6, 12), K("X", 10, 12), K("C", 14, 12), K("V", 18, 12),
    K("A", 3, 8), K("S", 7, 8), K("D", 11, 8), K("F", 15, 8), K("G", 19, 8),
    K("Q", 2, 4), K("W", 6, 4), K("E", 10, 4), K("R", 14, 4), K("T", 18, 4),
    K("1", 0, 0), K("2", 4, 0), K("3", 8, 0), K("4", 12, 0), K("5", 16, 0),
    K("0", 36, 0), K("9", 32, 0), K("8", 28, 0), K("7", 24, 0), K("6", 20, 0),
    K("P", 38, 4), K("O", 34, 4), K("I", 30, 4), K("U", 26, 4), K("Y", 22, 4),
    {"ENTER", "Return", "ENTER", 39, 8, 5, 4}, K("L", 35, 8), K("K", 31, 8), K("J", 27, 8), K("H", 23, 8),
    {"SPACE", "Space", "BREAK SPACE", 38, 12, 6, 4}, {"SYMBOL SHIFT", "Left Alt", "SYMBOL SHIFT", 34, 12, 4, 4},
    K("M", 30, 12), K("N", 26, 12), K("B", 22, 12),
    {"CTRL", "Left Ctrl", "CTRL", 0, 16, 5, 4}, {"PRINT", "PageDown", "PRINT", 29, 16, 5, 4},
    {"SCROLL", "ScrollLock", "SCROLL", 34, 16, 5, 4}, {"ESC", "Escape", "ESC", 5, 16, 4, 4},
    {"F1", "Delete", "F1", 9, 16, 4, 4}, {"F2", "Insert", "F2", 13, 16, 4, 4}, {"F3", "Home", "F3", 17, 16, 4, 4},
    {"F4", "PageUp", "F4", 21, 16, 4, 4}, {"TAB", "Tab", "TAB", 39, 16, 5, 4},
};
#undef K

enum { CS = ZR_SPECTRUM_KEY_CAPS_SHIFT, SS = ZR_SPECTRUM_KEY_SYMBOL_SHIFT, N_ = ZR_KEY_NONE };
enum { KZ = 1, KX, KC, KV, KA, KS, KD, KF, KG, KQ, KW, KE, KR, KT, K1, K2, K3, K4, K5, K0, K9, K8, K7, K6,
       KP, KO, KI, KU, KY, KENTER, KL, KK, KJ, KH, KSPACE, KSS, KM, KN, KB };

/* As the core's keyboard maps the PC's keys (rp_spectrum_keyboard.sv, with Ctrl and Escape taken off the matrix). */
static const zr_combo_desc COMBOS[] = {
    {"Backspace", K0, CS}, {"Left", K5, CS}, {"Down", K6, CS}, {"Up", K7, CS}, {"Right", K8, CS}, {"CapsLock", K2, CS},
    {".", KM, SS}, {",", KN, SS}, {"'", K7, SS}, {";", KO, SS}, {"-", KJ, SS}, {"=", KL, SS}, {"/", KV, SS},
    {"Shift+.", KT, SS}, {"Shift+,", KR, SS}, {"Shift+'", KP, SS}, {"Shift+;", KZ, SS}, {"Shift+-", K0, SS}, {"Shift+=", KK, SS},
    {"Shift+/", KC, SS},
    {"Right Shift", CS, N_}, {"Right Alt", SS, N_}, {"Right Ctrl", KEY_NATIVE, N_},
};

/* --- the board ------------------------------------------------------------------------------------------------------- */

typedef struct cobra {
    zr_spectrum s;                          /* first: the shared core; its RAM is physical 4000h-FFFFh */
    uint8_t model80;
    uint8_t col80, parallel, kempston, startup;
    uint8_t drive8[4];
    /* the startup latch and port C (saved) */
    uint8_t po, cpm, lock;
    uint64_t hold_t;                        /* refresh cycles do not clock the latch before this T-state */
    uint8_t pc;                             /* port C's pins (an input pin reads high) */
    uint8_t bank0[BANK];                    /* physical 0000h-3FFFh */
    uint8_t vram[BANK];                     /* the 80K's video RAM */
    const uint8_t *rom;
    /* the chips */
    zr_cobravideo video;
    zr_i8255 ppi;
    zr_ctc ctc;
    zr_daisy chain[1];
    zr_reti_watch reti;
    zr_upd765 fdc;
    zr_fdd fdd[4];
    zr_drivesnd sound;
    uint8_t trg0;                           /* the 8272's INT or DRQ on CTC TRG0 */
    zr_media *disk_media[4];
    zr_disk *disk[4];
    uint64_t flush_at;
    /* port FE of the core, taken over by the 8255 */
    zr_bus_read_fn fe_read;
    zr_bus_write_fn fe_write;
    int fdc_dec;
    /* the keys of the sixth input (bit n: row n), TAB's sequence, the startup key held */
    uint8_t native;
    zr_dev tab_dev;
    uint64_t tab_t;                         /* when the sequence started */
    uint8_t tab_phase;                      /* 0 idle, 1 EXTEND, 2 nothing, 3 P */
    int8_t boot_key;                        /* the key held for the bootstrap, -1 none */
    /* the serial line: PC7 out, PA7 in */
    zr_port_host ports;
    uint8_t has_ports, loopback;
    zr_dev tx_dev, rx_dev;
    uint8_t tx_active, tx_n;
    uint64_t tx_t[TX_LOG];
    uint8_t tx_l[TX_LOG];
    uint64_t tx_bit;                        /* the bit time the guest sends at, as measured (0: not known yet) */
    uint8_t rx_line, rx_byte, rx_count;
    int8_t rx_bit;
    uint8_t rx_queue[RX_QUEUE];
    /* the 80-column text: where the CP/M BIOS has its 4 x 8 font (found in RAM), -1 not found */
    int font_at;
    uint8_t font[96 * 8];
    zr_cpu_hooks hooks;
} cobra;

static uint64_t board_now(const cobra *c) { return zr_spectrum_now(&c->s); }

/* Physical RAM: 0000h-3FFFh is bank0, the rest the core's 48 KiB. */
static uint8_t *phys(cobra *c, uint32_t p) {
    p &= 0xFFFFu;
    return p < BANK ? c->bank0 + p : c->s.ram + (p - BANK);
}
static const uint8_t *video_memory(cobra *c) { return c->model80 ? c->vram : c->s.ram; }
static int pc6(const cobra *c) { return c->pc >> 6 & 1u; }
static int basic(const cobra *c) { return !c->po && !c->cpm; }

/* --- the interrupt and the memory map ------------------------------------------------------------------------------------ */

static void update_int(cobra *c) {
    zr_z80_int(&c->s.z80, zr_daisy_int(c->chain, 1) || (basic(c) && zr_cobravideo_int(&c->video)));
}

/* Where a write of the 8 KiB block `blk` lands (NULL: nowhere) and where its read comes from, outside the overlay's ROM. */
static uint8_t *block(cobra *c, unsigned blk) {
    static const uint16_t MAP64_PO[8] = {0, 0, 0, 0, 0x0000u, 0x6000u, 0x4000u, 0x2000u};
    static const uint16_t MAP64_CPM[8] = {0x8000u, 0xA000u, 0xC000u, 0xE000u, 0x0000u, 0x6000u, 0x4000u, 0x2000u};
    if (c->model80) {
        if (c->po) {
            if (blk < 6u) return phys(c, (blk - 4u) * 0x2000u);
            return pc6(c) ? c->vram + (blk - 6u) * 0x2000u : phys(c, 0x4000u + (blk - 6u) * 0x2000u);
        }
        if (c->cpm && (blk == 2u || blk == 3u) && pc6(c)) return c->vram + (blk - 2u) * 0x2000u;
        return phys(c, blk * 0x2000u);
    }
    if (c->po) return phys(c, MAP64_PO[blk]);
    if (c->cpm) return phys(c, MAP64_CPM[blk]);
    return phys(c, blk * 0x2000u);
}

/* The AY board (not original) is on the ports only while BASIC runs; the 8272 then leaves it the addresses with A15 high. */
static void ay_gate(cobra *c) {
    zr_spectrum *s = &c->s;
    if (!s->cfg.ay) return;
    const int on = basic(c);
    for (unsigned i = 0; i < 2u; ++i)
        if (s->ay_dec[i] >= 0) zr_bus_enable_io(&s->bus, s->ay_dec[i], on);
    s->bus.dec[c->fdc_dec].mask = on ? 0x80F7u : 0x00F7u;
}

static void video_mode(cobra *c) {
    zr_spectrum *s = &c->s;
    const int upper = !c->po && c->cpm;
    const int extended = upper && (c->pc & 0x20u) && c->col80;
    if (extended != c->video.extended || upper != c->video.upper) {
        zr_cobravideo_catch_up(&c->video, board_now(c));
        zr_cobravideo_set_mode(&c->video, extended, upper);
    }
    const uint8_t *screen = video_memory(c) + (upper ? 0x2000u : 0u);
    if (screen != s->screen) {
        zr_ula_catch_up(&s->ula, board_now(c));
        s->screen = screen;
    }
}

static void remap(cobra *c) {
    zr_bus *b = &c->s.bus;
    const uint8_t *video = video_memory(c);
    zr_bus_watch(b, 0x0000u, 0x10000u, 0);
    for (unsigned blk = 0; blk < 8u; ++blk) {
        const uint16_t at = (uint16_t)(blk * 0x2000u);
        if (c->po && blk < 4u) {
            if (blk < 2u) {
                const uint8_t *boot = c->rom + (c->model80 ? ROM_BOOT80 : ROM_BOOT64);
                for (unsigned k = 0; k < 4u; ++k) zr_bus_map_read(b, (uint16_t)(at + k * 0x800u), 0x800u, boot);
            } else {
                const unsigned quarter = c->model80 ? ((c->pc >> 3 & 1u) << 1 | (c->pc >> 7 & 1u)) : 1u;
                zr_bus_map_read(b, at, 0x2000u, c->rom + ROM_SYSTEM + quarter * BANK + (blk - 2u) * 0x2000u);
            }
            zr_bus_map_write(b, at, 0x2000u, NULL);
            continue;
        }
        uint8_t *p = block(c, blk);
        zr_bus_map_read(b, at, 0x2000u, p);
        zr_bus_map_write(b, at, 0x2000u, basic(c) && blk < 2u ? NULL : p);
        const int mirror = c->model80 && basic(c) && (blk == 2u || blk == 3u);
        if (mirror || (p >= video && p < video + BANK)) zr_bus_watch(b, at, 0x2000u, 1);
    }
    video_mode(c);
    ay_gate(c);
}

/* Writes to the video memory: the picture is drawn up to them first; BASIC's writes to 4000h-7FFFh also reach the 80K's video RAM. */
static void video_hook(void *ctx, uint16_t address, uint8_t value) {
    cobra *c = ctx;
    zr_cobravideo_catch_up(&c->video, board_now(c));
    const uint8_t *p = zr_bus_target(&c->s.bus, address);
    if (p && p >= c->s.screen && p < c->s.screen + ZR_SPECTRUM_SCREEN) zr_ula_catch_up(&c->s.ula, board_now(c));
    if (c->model80 && basic(c) && (address >> 14) == 1u) c->vram[address & 0x3FFFu] = value;
}

/* The latch: A7 of every refresh cycle, unless BASIC has locked it or the power-on hold lasts. */
static void on_refresh(void *ctx, uint16_t address) {
    cobra *c = ctx;
    const uint8_t a7 = (uint8_t)(address >> 7 & 1u);
    if (c->lock || a7 == c->po || c->s.z80.t < c->hold_t) return;
    c->po = a7;
    if (!a7) {
        c->cpm = (uint8_t)pc6(c);
        if (!c->cpm) c->lock = 1;
        c->boot_key = -1;                   /* the bootstrap has left: the startup key is let go */
    }
    remap(c);
    update_int(c);
}

/* --- port C, the serial line ------------------------------------------------------------------------------------------- */

static uint64_t rx_bit_ticks(const cobra *c) { return c->tx_bit ? c->tx_bit : CLOCK_HZ / 9600u; }

static void rx_start(cobra *c, uint64_t now) {
    c->rx_byte = c->rx_queue[0];
    memmove(c->rx_queue, c->rx_queue + 1, --c->rx_count);
    c->rx_bit = 0;
    c->rx_line = 0;
    zr_dev_schedule(&c->rx_dev, now + rx_bit_ticks(c));
}

static void rx_event(void *dev, uint64_t now) {
    cobra *c = ((zr_dev *)dev)->host->ctx;
    ++c->rx_bit;
    if (c->rx_bit <= 8) {
        c->rx_line = (uint8_t)(c->rx_byte >> (c->rx_bit - 1) & 1u);
        zr_dev_schedule(&c->rx_dev, now + rx_bit_ticks(c));
    } else if (c->rx_bit == 9) {
        c->rx_line = 1;
        zr_dev_schedule(&c->rx_dev, now + rx_bit_ticks(c));
    } else if (c->rx_count) rx_start(c, now);
    else c->rx_bit = -1;
}

static size_t rx_put(cobra *c, const uint8_t *data, size_t count) {
    size_t taken = 0;
    while (taken < count && c->rx_count < RX_QUEUE) c->rx_queue[c->rx_count++] = data[taken++];
    if (c->rx_bit < 0 && c->rx_count) rx_start(c, board_now(c));
    return taken;
}

/* The bit time of a run of the line: the standard rate it is within 12% of, else the run itself. */
static uint64_t snap_bit(uint64_t run) {
    static const uint32_t RATES[] = {50, 75, 110, 150, 300, 600, 1200, 2400, 4800, 9600, 19200, 38400, 57600};
    for (unsigned i = 0; i < sizeof RATES / sizeof RATES[0]; ++i) {
        const uint64_t ticks = CLOCK_HZ / RATES[i];
        const uint64_t d = run > ticks ? run - ticks : ticks - run;
        if (d * 8u < ticks) return ticks;
    }
    return run ? run : 1u;
}

static void tx_arm(cobra *c) {
    zr_dev_schedule(&c->tx_dev, c->tx_active && c->tx_bit ? c->tx_t[0] + c->tx_bit * 19u / 2u : ZR_NEVER);
}

/* A change of TX: a falling edge from idle starts a frame; the shortest run seen is the bit time (the firmware times its bits
 * itself); the byte is read at the middle of each bit once the stop bit's middle has come. */
static void tx_change(cobra *c, int level, uint64_t t) {
    if (!c->tx_active) {
        if (level) return;
        c->tx_active = 1;
        c->tx_n = 1;
        c->tx_t[0] = t;
        c->tx_l[0] = 0;
    } else {
        const uint64_t run = t - c->tx_t[c->tx_n - 1u];
        if (run && (!c->tx_bit || run * 4u < c->tx_bit * 3u)) c->tx_bit = snap_bit(run);
        if (c->tx_n < TX_LOG) {
            c->tx_t[c->tx_n] = t;
            c->tx_l[c->tx_n++] = (uint8_t)level;
        }
    }
    tx_arm(c);
}

static void tx_event(void *dev, uint64_t now) {
    cobra *c = ((zr_dev *)dev)->host->ctx;
    const uint64_t t0 = c->tx_t[0], bit = c->tx_bit;
    unsigned value = 0;
    for (unsigned i = 1; i <= 8u; ++i) {
        const uint64_t sample = t0 + bit * (2u * i + 1u) / 2u;
        unsigned level = 0;
        for (unsigned j = 0; j < c->tx_n && c->tx_t[j] <= sample; ++j) level = c->tx_l[j];
        value |= level << (i - 1u);
    }
    uint64_t later_t[TX_LOG];
    uint8_t later_l[TX_LOG];
    unsigned later = 0;
    for (unsigned j = 0; j < c->tx_n; ++j)
        if (c->tx_t[j] > now) {
            later_t[later] = c->tx_t[j];
            later_l[later++] = c->tx_l[j];
        }
    c->tx_active = 0;
    c->tx_n = 0;
    const uint8_t byte = (uint8_t)value;
    if (c->loopback) (void)rx_put(c, &byte, 1);
    else if (c->has_ports && c->ports.transmit) c->ports.transmit(c->ports.ctx, 0, byte);
    for (unsigned j = 0; j < later; ++j) tx_change(c, later_l[j], later_t[j]);
    tx_arm(c);
}

/* Port C's pins as the 8255 leaves them (an input pin is pulled high), and what they drive. */
static void port_c_sync(cobra *c) {
    const uint8_t driven = zr_i8255_driven(&c->ppi, ZR_I8255_PORT_C);
    const uint8_t pins = (uint8_t)((zr_i8255_out(&c->ppi, ZR_I8255_PORT_C) & driven) | ~driven);
    const uint8_t old = c->pc;
    if (pins == old) return;
    c->pc = pins;
    const uint64_t now = board_now(c);
    if ((pins ^ old) & 7u) {
        zr_cobravideo_catch_up(&c->video, now);
        zr_cobravideo_set_border(&c->video, pins & 7u);
    }
    if ((pins ^ old) & 0x1Fu) c->fe_write(&c->s, 0x00FEu, (uint8_t)(pins & 0x1Fu));      /* speaker, MIC, the tape's record */
    if ((pins ^ old) & 0xE8u) remap(c);
    if ((pins ^ old) & 0x80u) tx_change(c, pins >> 7 & 1u, now);
}

/* --- the ports -------------------------------------------------------------------------------------------------------------- */

static int tape_ear(cobra *c) {
    zr_spectrum *s = &c->s;
    return s->tape && zr_tape_deck_playing(&s->deck) ? zr_tape_deck_level(&s->deck, board_now(c)) : 1;
}

/* Port A: the matrix rows the address's high byte selects (with the core's joystick-on-keys), the sixth input of those rows,
 * TAB's keys and the startup key, EAR (high while no tape plays: the core's), serial RX. */
static uint8_t port_a(cobra *c, uint16_t port) {
    const uint8_t high = (uint8_t)(port >> 8);
    uint8_t keys = (uint8_t)(c->fe_read(&c->s, port) & 0x1Fu);
    uint8_t extra = 0x20u;
    if (c->parallel) extra = 0x20u;         /* PA5 is the external handshake, idle high */
    else if (c->native & (uint8_t)~high) extra = 0;
    /* TAB: CAPS SHIFT (row 0) and SYMBOL SHIFT (row 7, bit 1) first, P (row 5, bit 0) last. */
    if (c->tab_phase == 1u) {
        if (!(high & 0x01u)) keys &= 0xFEu;
        if (!(high & 0x80u)) keys &= 0xFDu;
    } else if (c->tab_phase == 3u && !(high & 0x20u)) keys &= 0xFEu;
    if (c->boot_key >= 0 && !(high >> (c->boot_key / 5) & 1u)) keys &= (uint8_t)~(1u << (c->boot_key % 5));
    return (uint8_t)(keys | extra | (tape_ear(c) ? 0x40u : 0u) | (c->rx_line ? 0x80u : 0u));
}

static uint8_t port_b(cobra *c) {
    if (c->parallel) return 0xFFu;
    uint8_t v = 0;
    if (c->kempston) {
        const unsigned h = c->s.stick;
        v = (uint8_t)((h & ZR_JOY_RIGHT ? 1u : 0u) | (h & ZR_JOY_LEFT ? 2u : 0u) | (h & ZR_JOY_DOWN ? 4u : 0u) |
                      (h & ZR_JOY_UP ? 8u : 0u) | (h & ZR_JOY_FIRE ? 16u : 0u));
    }
    if (c->model80 && zr_cobravideo_int(&c->video)) v |= 0x80u;
    return v;
}

static uint8_t fe_in(void *ctx, uint16_t port) {
    cobra *c = ctx;
    c->ppi.dev.ops->input(&c->ppi, ZR_I8255_PORT(ZR_I8255_PORT_A), port_a(c, port));
    return c->ppi.dev.ops->read(&c->ppi, ZR_SPACE_IO, 0);
}
static void fe_out(void *ctx, uint16_t port, uint8_t value) {
    cobra *c = ctx;
    (void)port;
    c->ppi.dev.ops->write(&c->ppi, ZR_SPACE_IO, 2, value);
    port_c_sync(c);
}
static uint8_t b_in(void *ctx, uint16_t port) {
    cobra *c = ctx;
    (void)port;
    c->ppi.dev.ops->input(&c->ppi, ZR_I8255_PORT(ZR_I8255_PORT_B), port_b(c));
    return c->ppi.dev.ops->read(&c->ppi, ZR_SPACE_IO, 1);
}
static void control_out(void *ctx, uint16_t port, uint8_t value) {
    cobra *c = ctx;
    (void)port;
    c->ppi.dev.ops->write(&c->ppi, ZR_SPACE_IO, 3, value);
    port_c_sync(c);
}
static uint8_t ctc_in(void *ctx, uint16_t port) {
    cobra *c = ctx;
    return c->ctc.dev.ops->read(&c->ctc, ZR_SPACE_IO, (uint32_t)(port >> 3 & 3u));
}
static void ctc_out(void *ctx, uint16_t port, uint8_t value) {
    cobra *c = ctx;
    c->ctc.dev.ops->write(&c->ctc, ZR_SPACE_IO, (uint32_t)(port >> 3 & 3u), value);
}
static uint8_t fdc_in(void *ctx, uint16_t port) {
    cobra *c = ctx;
    return c->fdc.dev.ops->read(&c->fdc, ZR_SPACE_IO, (uint32_t)(port >> 3 & 1u));
}
static void fdc_out(void *ctx, uint16_t port, uint8_t value) {
    cobra *c = ctx;
    if (port & 8u) c->fdc.dev.ops->write(&c->fdc, ZR_SPACE_IO, 1, value);
}

/* --- the CPU's bus: the RETI and the acknowledge ------------------------------------------------------------------------------ */

static uint8_t cpu_read(void *ctx, uint16_t address, int m1) {
    cobra *c = ctx;
    const uint8_t v = zr_bus_read(&c->s.bus, address);
    if (m1 && zr_reti_m1(&c->reti, v) && zr_daisy_reti(c->chain, 1)) update_int(c);
    return v;
}

/* The CTC's vector (the data bus floats high when it does not answer: the frame interrupt of BASIC runs in mode 1). */
static uint8_t cpu_ack(void *ctx) {
    cobra *c = ctx;
    if (c->s.cfg.tape_accelerate) zr_loadaccel_disturb(&c->s.accel);
    const uint8_t v = zr_daisy_ack(c->chain, 1);
    zr_bus_trace(&c->s.bus, ZR_BUS_TRACE_INTERRUPT, v, v);
    update_int(c);
    return v;
}

/* --- the lines between the chips ----------------------------------------------------------------------------------------------- */

static void fdc_trigger(cobra *c) {
    const uint8_t level = (uint8_t)(zr_upd765_int(&c->fdc) || zr_upd765_drq(&c->fdc));
    if (level == c->trg0) return;
    c->trg0 = level;
    c->ctc.dev.ops->input(&c->ctc, ZR_CTC_TRG0, level);
}

static void board_line(void *ctx, void *dev, int pin, int state) {
    cobra *c = ctx;
    if (dev == &c->video) {
        c->ctc.dev.ops->input(&c->ctc, ZR_CTC_TRG3, state);
        update_int(c);
    } else if (dev == &c->ctc) {
        if (pin == ZR_CTC_OUT_ZCTO0) c->ctc.dev.ops->input(&c->ctc, ZR_CTC_TRG1, state);
        else if (pin == ZR_CTC_OUT_ZCTO1) c->ctc.dev.ops->input(&c->ctc, ZR_CTC_TRG2, state);
        else if (pin == ZR_CTC_OUT_ZCTO2) c->fdc.dev.ops->input(&c->fdc, ZR_UPD765_PIN_TC, state);
        else update_int(c);
    } else if (dev == &c->fdc) fdc_trigger(c);
    /* The core's ULA (its interrupt is not wired: the CoBra's frame pulse is the video's) and the 8255's pins (read back). */
}

/* --- the keys: TAB's sequence ----------------------------------------------------------------------------------------------- */

static void tab_arm(cobra *c) {
    zr_dev_schedule(&c->tab_dev, c->tab_phase ? c->tab_t + (uint64_t)c->tab_phase * TAB_PHASE_TICKS : ZR_NEVER);
}
static void tab_event(void *dev, uint64_t now) {
    cobra *c = ((zr_dev *)dev)->host->ctx;
    while (c->tab_phase && now >= c->tab_t + (uint64_t)c->tab_phase * TAB_PHASE_TICKS) c->tab_phase = c->tab_phase == 3u ? 0u : (uint8_t)(c->tab_phase + 1u);
    tab_arm(c);
}

static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void no_reset(void *dev) { (void)dev; }
static const zr_dev_ops TAB_OPS = {.read = no_read, .write = no_write, .event = tab_event, .reset = no_reset};
static const zr_dev_ops TX_OPS = {.read = no_read, .write = no_write, .event = tx_event, .reset = no_reset};
static const zr_dev_ops RX_OPS = {.read = no_read, .write = no_write, .event = rx_event, .reset = no_reset};

static void cobra_key(void *self, unsigned key, int down) {
    cobra *c = self;
    if (key < ZR_SPECTRUM_KEYS) zr_spectrum_key(self, key, down);
    else if (key < KEY_TAB) {
        const uint8_t bit = (uint8_t)(1u << (key - KEY_NATIVE));
        c->native = (uint8_t)(down ? c->native | bit : c->native & ~bit);
    } else if (key == KEY_TAB && down) {
        c->tab_t = board_now(c);
        c->tab_phase = 1;
        tab_arm(c);
    }
}

/* --- the drives --------------------------------------------------------------------------------------------------------------- */

static void drive_rates(cobra *c) {
    for (unsigned u = 0; u < 4u; ++u) zr_fdd_set_rate(&c->fdd[u], c->drive8[u] ? 500u : 250u);
}

/* A changed disk goes into its media when the controller is between commands, at most twice a second (always when forced). */
static void flush_disks(cobra *c, uint64_t now, int force) {
    if (!force && (now < c->flush_at || c->fdc.phase != 0)) return;
    for (unsigned u = 0; u < 4u; ++u) {
        if (!c->disk[u] || !c->disk[u]->dirty || !c->disk_media[u]) continue;
        char why[128];
        if (zr_disk_save_media(c->disk[u], c->disk_media[u], why, sizeof why)) c->flush_at = now + DISK_FLUSH_TICKS;
        else snprintf(c->s.error, sizeof c->s.error, "%s", why);
    }
}

/* --- life cycle -------------------------------------------------------------------------------------------------------------- */

static const int8_t BOOT_KEYS[] = {-1, KB, KC, KD};

static void cobra_reset(void *self, int power_on) {
    cobra *c = self;
    zr_spectrum *s = &c->s;
    zr_spectrum_reset(s, power_on);
    const uint64_t now = board_now(c);
    if (power_on) {
        memset(c->bank0, 0, sizeof c->bank0);
        memset(c->vram, 0, sizeof c->vram);
    }
    c->po = 1;
    c->cpm = 1;
    c->lock = 0;
    c->hold_t = s->z80.t + LATCH_HOLD;
    c->ppi.dev.ops->reset(&c->ppi);
    c->ctc.dev.ops->reset(&c->ctc);
    c->fdc.dev.ops->reset(&c->fdc);
    c->video.dev.ops->reset(&c->video);
    zr_cobravideo_set_border(&c->video, 7u);
    c->reti.prefix = 0;
    c->tab_phase = 0;
    tab_arm(c);
    c->boot_key = BOOT_KEYS[c->startup < 4u ? c->startup : 0u];
    for (unsigned u = 0; u < 4u; ++u)
        if (!c->fdd[u].motor) zr_fdd_set_motor(&c->fdd[u], 1, now);
    zr_upd765_drive_changed(&c->fdc);
    c->pc = 0;
    port_c_sync(c);                         /* every pin an input: all high */
    remap(c);
    fdc_trigger(c);
    update_int(c);
}

static void *cobra_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.fe_mask = 0x00FFu;                  /* the 8255 at FE (the core's decoder is taken over below) */
    cfg.fe_value = 0x00FEu;
    cfg.fe_idle = 0;
    cfg.ram_size = 0xC000u;
    cfg.charset = (uint16_t)(ROM_BASIC + 0x3D00u);
    cfg.traps_rom = ROM_BASIC;
    cfg.nmi = 1;
    const unsigned stick = o[OPTION_JOYSTICK];
    cfg.joystick = (uint8_t)(stick == ZR_SPECTRUM_JOY_KEMPSTON ? ZR_SPECTRUM_JOY_NONE : stick);
    cfg.ay = (uint8_t)o[OPTION_AY];
    cobra *c = (cobra *)zr_spectrum_create(&cfg, setup, sizeof *c, error, capacity);
    if (!c) return NULL;
    zr_spectrum *s = &c->s;
    if (s->rom_size < ROM_SYSTEM + 4u * BANK) {
        snprintf(error, capacity, "The CoBra ROM image is not whole");
        zr_spectrum_destroy(c);
        return NULL;
    }
    c->model80 = setup->model == MODEL_80K;
    c->col80 = o[OPTION_COL80] != 0;
    c->parallel = o[OPTION_PARALLEL] != 0;
    c->kempston = stick == ZR_SPECTRUM_JOY_KEMPSTON;
    c->startup = (uint8_t)o[OPTION_STARTUP];
    c->rom = s->rom;
    c->font_at = -1;
    c->rx_line = 1;
    c->rx_bit = -1;
    for (unsigned u = 0; u < 4u; ++u) c->drive8[u] = o[OPTION_DRIVE_A + u] != 0;
    /* The board's lines and memory hook in place of the core's. */
    s->host.line = board_line;
    zr_bus_set_hook(&s->bus, video_hook, c);
    /* Port FE: the core's decoder stands aside; its read (the keys) and write (speaker, MIC, tape) serve the 8255's ports. */
    for (unsigned i = 0; i < s->bus.decoders; ++i) {
        zr_bus_decoder *d = &s->bus.dec[i];
        if (d->mask == cfg.fe_mask && d->value == cfg.fe_value && d->ctx == s && d->read && d->write) {
            c->fe_read = d->read;
            c->fe_write = d->write;
            zr_bus_enable_io(&s->bus, (int)i, 0);
            break;
        }
    }
    zr_bus_add_io(&s->bus, 0x00FFu, 0x00FEu, fe_in, fe_out, c);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x001Fu, b_in, NULL, c);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x00DFu, NULL, control_out, c);
    zr_bus_add_io(&s->bus, 0x00E7u, 0x00E3u, ctc_in, ctc_out, c);
    c->fdc_dec = zr_bus_add_io(&s->bus, 0x00F7u, 0x00F5u, fdc_in, fdc_out, c);
    /* The CPU: the RETI on its fetches, the CTC's vector, the latch on its refresh cycles. */
    s->cpu_bus.read = cpu_read;
    s->cpu_bus.ack = cpu_ack;
    c->hooks.ctx = c;
    c->hooks.refresh = on_refresh;
    zr_spectrum_set_hooks(s, &c->hooks);
    /* The chips. */
    zr_cobravideo_init(&c->video, &s->host, 2u, video_memory(c));
    zr_i8255_init(&c->ppi, &s->host, "8255");
    zr_ctc_init(&c->ctc, &s->host, "ctc", 4u, 1u);
    zr_ctc_watch(&c->ctc, 7u);
    c->chain[0] = c->ctc.daisy;
    zr_upd765_init(&c->fdc, &s->host, "8272", CLOCK_HZ);
    zr_upd765_set_rate(&c->fdc, 250u);
    zr_upd765_set_clock(&c->fdc, c->drive8[0] ? 8000u : 4000u);
    zr_upd765_ready_wiring(&c->fdc, ZR_UPD765_READY_DRIVE);
    zr_drivesnd_init(&c->sound, CLOCK_HZ, ZR_SPECTRUM_AUDIO_RATE);
    zr_drivesnd_set(&c->sound, o[OPTION_DRIVE_SOUND] != 0, 50);
    for (unsigned u = 0; u < 4u; ++u) {
        zr_fdd_init(&c->fdd[u], CLOCK_HZ, 1, c->drive8[u] ? 77u : 80u, c->drive8[u] ? 360u : 300u);
        if (zr_drivesnd_enabled(&c->sound)) zr_fdd_set_hook(&c->fdd[u], zr_drivesnd_event, &c->sound, u);
        zr_upd765_attach(&c->fdc, u, &c->fdd[u]);
    }
    drive_rates(c);
    if (zr_drivesnd_enabled(&c->sound)) s->drives = &c->sound;
    c->tab_dev.ops = &TAB_OPS;
    c->tab_dev.host = &s->host;
    c->tab_dev.name = "tab";
    c->tx_dev.ops = &TX_OPS;
    c->tx_dev.host = &s->host;
    c->tx_dev.name = "serial tx";
    c->rx_dev.ops = &RX_OPS;
    c->rx_dev.host = &s->host;
    c->rx_dev.name = "serial rx";
    snprintf(s->description, sizeof s->description, "%s", MODELS[setup->model].name);
    cobra_reset(c, 1);
    return c;
}

static void cobra_destroy(void *self) {
    cobra *c = self;
    if (!c) return;
    flush_disks(c, board_now(c), 1);
    for (unsigned u = 0; u < 4u; ++u) {
        zr_fdd_eject(&c->fdd[u]);
        zr_disk_close(c->disk[u]);
    }
    zr_spectrum_destroy(c);
}

static zr_run_status cobra_run(void *self, uint64_t ticks) {
    cobra *c = self;
    const zr_run_status status = zr_spectrum_run(self, ticks);
    const uint64_t now = board_now(c);
    zr_cobravideo_catch_up(&c->video, now);
    flush_disks(c, now, 0);
    return status;
}

static int cobra_frame(void *self, zr_frame *frame) { return zr_cobravideo_frame(&((cobra *)self)->video, frame); }
static uint64_t cobra_frame_period(const void *self) { return zr_cobravideo_frame_ticks(&((const cobra *)self)->video); }

/* --- the screen as text ----------------------------------------------------------------------------------------------------- */

/* The picture's pixel (x, y) as the serializer reads it. */
static unsigned screen_pixel(cobra *c, unsigned x, unsigned y) {
    const uint8_t *m = video_memory(c);
    return m[zr_cobravideo_address(c->video.extended, c->video.upper, x, y, 0)] >> (7u - (x & 7u)) & 1u;
}

/* The CP/M BIOS draws its 80 columns in cells of 4 x 8 pixels from a font of its own in its memory: 96 glyphs (20h-7Fh) of
 * eight bytes, the pixels in the high nibble (the 80K's BIOS keeps it at 7B00h). It is found where the CPU sees it, by the
 * space and the exclamation mark, and copied. */
static int find_font(cobra *c) {
    static const uint8_t START[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00};
    const zr_bus *b = &c->s.bus;
    if (c->font_at >= 0) {
        unsigned i = 0;
        while (i < 16u && zr_bus_peek(b, (uint16_t)(c->font_at + (int)i)) == START[i]) ++i;
        if (i == 16u) return 1;
        c->font_at = -1;
    }
    for (uint32_t at = 0; at + 96u * 8u <= 0x10000u; ++at) {
        unsigned i = 0;
        while (i < 16u && zr_bus_peek(b, (uint16_t)(at + i)) == START[i]) ++i;
        if (i < 16u) continue;
        c->font_at = (int)at;
        for (unsigned k = 0; k < sizeof c->font; ++k) c->font[k] = zr_bus_peek(b, (uint16_t)(at + k));
        return 1;
    }
    return 0;
}

static size_t text80(cobra *c, char *out, size_t capacity) {
    size_t n = 0;
    const uint8_t *font = find_font(c) ? c->font : NULL;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[80];
        unsigned last = 0;
        for (unsigned col = 0; col < 80u; ++col) {
            uint8_t cell[8];
            for (unsigned y = 0; y < 8u; ++y) {
                unsigned v = 0;
                for (unsigned x = 0; x < 4u; ++x) v = v << 1 | screen_pixel(c, col * 4u + x, row * 8u + y);
                cell[y] = (uint8_t)v;
            }
            char ch = '?';
            for (unsigned g = 0; font && g < 95u && ch == '?'; ++g) {
                unsigned same = 1, inverted = 1;
                for (unsigned y = 0; y < 8u; ++y) {
                    const uint8_t want = (uint8_t)(font[g * 8u + y] >> 4);
                    if (cell[y] != want) same = 0;
                    if (cell[y] != (uint8_t)(~want & 15u)) inverted = 0;
                }
                if (same || inverted) ch = (char)(0x20u + g);
            }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* BASIC, the bootstrap's screen and CP/M in 32 columns through the BASIC ROM's character set; the 80 columns through the BIOS's font. */
static size_t cobra_text(void *self, char *out, size_t capacity) {
    cobra *c = self;
    zr_cobravideo_catch_up(&c->video, board_now(c));
    if (c->video.extended) return text80(c, out, capacity);
    return zr_spectrum_text(self, out, capacity);
}

/* --- media ----------------------------------------------------------------------------------------------------------------- */

static int cobra_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    cobra *c = self;
    if (slot == SLOT_TAPE) return zr_spectrum_insert(self, slot, media, error, capacity);
    if (slot < SLOT_A || slot > SLOT_D) {
        if (capacity) snprintf(error, capacity, "There is no such slot");
        return 0;
    }
    const unsigned u = slot - SLOT_A;
    if (c->disk_media[u]) {
        if (capacity) snprintf(error, capacity, "The slot is not empty");
        return 0;
    }
    zr_disk *disk = zr_disk_open(media, NULL, error, capacity);
    if (!disk) return 0;
    if (!zr_fdd_insert(&c->fdd[u], disk, error, capacity)) {
        zr_disk_close(disk);
        return 0;
    }
    c->disk[u] = disk;
    c->disk_media[u] = media;
    zr_upd765_drive_changed(&c->fdc);
    return 1;
}

static zr_media *cobra_media(const void *self, unsigned slot) {
    const cobra *c = self;
    if (slot >= SLOT_A && slot <= SLOT_D) return c->disk_media[slot - SLOT_A];
    return zr_spectrum_media(self, slot);
}

static int cobra_eject(void *self, unsigned slot) {
    cobra *c = self;
    if (slot < SLOT_A || slot > SLOT_D) return zr_spectrum_eject(self, slot);
    const unsigned u = slot - SLOT_A;
    if (!c->disk_media[u]) return 0;
    flush_disks(c, board_now(c), 1);
    zr_fdd_eject(&c->fdd[u]);
    zr_disk_close(c->disk[u]);
    c->disk[u] = NULL;
    c->disk_media[u] = NULL;
    zr_upd765_drive_changed(&c->fdc);
    return 1;
}

/* A drive works while the controller has a command on it or its head is moving (the motors always turn). */
static unsigned cobra_activity(const void *self) {
    const cobra *c = self;
    unsigned bits = zr_spectrum_activity(self);
    for (unsigned u = 0; u < 4u; ++u)
        if (c->disk[u] && (c->fdc.seeking[u] || (c->fdc.phase != 0 && (c->fdc.x.unit & 3u) == u))) bits |= 1u << (SLOT_A + u);
    return bits;
}

/* --- the serial port ------------------------------------------------------------------------------------------------------- */

static void cobra_set_ports(void *self, const zr_port_host *host) {
    cobra *c = self;
    c->has_ports = host != NULL;
    if (host) c->ports = *host;
    else memset(&c->ports, 0, sizeof c->ports);
    c->loopback = host && host->loopback && host->loopback(host->ctx, 0) ? 1u : 0u;
}
static size_t cobra_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    return port ? count : rx_put(self, data, count);
}
static void cobra_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    (void)self; (void)port; (void)dcd; (void)cts; (void)dsr; (void)ri;
}

/* --- states ------------------------------------------------------------------------------------------------------------------ */

static void cobra_save(const void *self, zr_state_writer *w) {
    const cobra *c = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "COBR", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, c->po);
    zr_sw_u8(w, c->cpm);
    zr_sw_u8(w, c->lock);
    zr_sw_u64(w, c->hold_t);
    zr_sw_u8(w, c->pc);
    zr_sw_u8(w, c->native);
    zr_sw_u64(w, c->tab_t);
    zr_sw_u8(w, c->tab_phase);
    zr_sw_u8(w, (uint8_t)c->boot_key);
    zr_sw_u8(w, c->trg0);
    zr_sw_u8(w, c->reti.prefix);
    zr_sw_u8(w, c->tx_active);
    zr_sw_u8(w, c->tx_n);
    for (unsigned i = 0; i < TX_LOG; ++i) {
        zr_sw_u64(w, c->tx_t[i]);
        zr_sw_u8(w, c->tx_l[i]);
    }
    zr_sw_u64(w, c->tx_bit);
    zr_sw_u8(w, c->rx_line);
    zr_sw_u8(w, (uint8_t)c->rx_bit);
    zr_sw_u8(w, c->rx_byte);
    zr_sw_u8(w, c->rx_count);
    zr_sw_bytes(w, c->rx_queue, sizeof c->rx_queue);
    zr_sw_u64(w, zr_sched_get(&c->s.sched, &c->rx_dev));
    zr_sw_packed(w, c->bank0, sizeof c->bank0);
    if (c->model80) zr_sw_packed(w, c->vram, sizeof c->vram);
    zr_sw_end(w);
    zr_sw_begin(w, "VID ", 0);
    c->video.dev.ops->save(&c->video, w);
    zr_sw_end(w);
    zr_sw_begin(w, "8255", 0);
    c->ppi.dev.ops->save(&c->ppi, w);
    zr_sw_end(w);
    zr_sw_begin(w, "CTC ", 0);
    c->ctc.dev.ops->save(&c->ctc, w);
    zr_sw_end(w);
    zr_sw_begin(w, "8272", 0);
    c->fdc.dev.ops->save(&c->fdc, w);
    zr_sw_end(w);
    for (unsigned u = 0; u < 4u; ++u) {
        zr_sw_begin(w, "FDD ", u);
        zr_fdd_save(&c->fdd[u], w);
        zr_sw_end(w);
    }
}

static int cobra_load(void *self, zr_state_reader *r) {
    cobra *c = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "COBR", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    c->po = zr_sr_u8(r) & 1u;
    c->cpm = zr_sr_u8(r) & 1u;
    c->lock = zr_sr_u8(r) & 1u;
    c->hold_t = zr_sr_u64(r);
    c->pc = zr_sr_u8(r);
    c->native = zr_sr_u8(r);
    c->tab_t = zr_sr_u64(r);
    c->tab_phase = zr_sr_u8(r) & 3u;
    c->boot_key = (int8_t)zr_sr_u8(r);
    if (c->boot_key >= ZR_SPECTRUM_KEYS) c->boot_key = -1;
    c->trg0 = zr_sr_u8(r) & 1u;
    c->reti.prefix = zr_sr_u8(r) & 1u;
    c->tx_active = zr_sr_u8(r) & 1u;
    c->tx_n = zr_sr_u8(r);
    if (c->tx_n > TX_LOG) return 0;
    for (unsigned i = 0; i < TX_LOG; ++i) {
        c->tx_t[i] = zr_sr_u64(r);
        c->tx_l[i] = zr_sr_u8(r) & 1u;
    }
    c->tx_bit = zr_sr_u64(r);
    c->rx_line = zr_sr_u8(r) & 1u;
    c->rx_bit = (int8_t)zr_sr_u8(r);
    c->rx_byte = zr_sr_u8(r);
    c->rx_count = zr_sr_u8(r);
    if (c->rx_count > RX_QUEUE || c->rx_bit < -1 || c->rx_bit > 9) return 0;
    zr_sr_bytes(r, c->rx_queue, sizeof c->rx_queue);
    const uint64_t rx_at = zr_sr_u64(r);
    zr_sr_packed(r, c->bank0, sizeof c->bank0);
    if (c->model80) zr_sr_packed(r, c->vram, sizeof c->vram);
    if (!zr_sr_ok(r)) return 0;
    if (!zr_sr_chunk(r, "VID ", 0) || !c->video.dev.ops->load(&c->video, r)) return 0;
    if (!zr_sr_chunk(r, "8255", 0) || !c->ppi.dev.ops->load(&c->ppi, r)) return 0;
    if (!zr_sr_chunk(r, "CTC ", 0) || !c->ctc.dev.ops->load(&c->ctc, r)) return 0;
    if (!zr_sr_chunk(r, "8272", 0) || !c->fdc.dev.ops->load(&c->fdc, r)) return 0;
    for (unsigned u = 0; u < 4u; ++u)
        if (!zr_sr_chunk(r, "FDD ", u) || !zr_fdd_load(&c->fdd[u], r)) return 0;
    /* The controller's command in progress gets its drive back (upd765's load leaves it to the board), the drives their rates. */
    c->fdc.x.fdd = c->fdc.drive[c->fdc.x.unit & 3u];
    drive_rates(c);
    c->font_at = -1;
    remap(c);
    tab_arm(c);
    tx_arm(c);
    zr_dev_schedule(&c->rx_dev, c->rx_bit >= 0 ? rx_at : ZR_NEVER);
    update_int(c);
    const zr_beeper *b = &c->s.beeper;
    zr_drivesnd_reset(&c->sound, b->num ? (b->done * b->den + b->num - 1u) / b->num : 0u);
    c->flush_at = 0;
    return zr_sr_ok(r);
}

/* --- snapshots ------------------------------------------------------------------------------------------------------------- */

/* A snapshot is of the 48K machine: the board is put in BASIC first, as the bootstrap's B leaves it (the 8255 in its mode,
 * port C at 0, the 48K BASIC in bank 0, the latch locked). */
static int cobra_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    cobra *c = self;
    const uint8_t po = c->po, cpm = c->cpm, lock = c->lock;
    const zr_i8255 ppi = c->ppi;
    uint8_t *saved = malloc(sizeof c->bank0);
    if (!saved) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return 0;
    }
    memcpy(saved, c->bank0, sizeof c->bank0);
    c->po = c->cpm = 0;
    c->lock = 1;
    c->boot_key = -1;
    c->ppi.dev.ops->write(&c->ppi, ZR_SPACE_IO, 3, 0x92u);
    c->ppi.dev.ops->write(&c->ppi, ZR_SPACE_IO, 2, 0x00u);
    port_c_sync(c);
    memcpy(c->bank0, c->rom + ROM_BASIC, BANK);
    remap(c);
    const int ok = zr_spectrum_snapshot_load(self, snap, error, capacity);
    if (ok) {
        if (c->model80) memcpy(c->vram, c->s.ram, BANK);
        update_int(c);
    } else {
        c->po = po;
        c->cpm = cpm;
        c->lock = lock;
        c->ppi = ppi;
        memcpy(c->bank0, saved, sizeof c->bank0);
        c->pc = 0;
        port_c_sync(c);
        remap(c);
        update_int(c);
    }
    free(saved);
    return ok;
}

static int cobra_snapshot_save(void *self, struct zr_snap *snap) {
    cobra *c = self;
    return basic(c) ? zr_spectrum_snapshot_save(self, snap) : 0;
}

static const zr_machine_impl COBRA_IMPL = {
    .destroy = cobra_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = cobra_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = cobra_run, .time = zr_spectrum_time, .frame_period = cobra_frame_period,
    .frame = cobra_frame, .text = cobra_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = cobra_key, .joystick = zr_spectrum_joystick,
    .insert = cobra_insert, .media = cobra_media, .eject = cobra_eject, .activity = cobra_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .set_ports = cobra_set_ports, .port_receive = cobra_port_receive, .port_modem = cobra_port_modem,
    .save = cobra_save, .load = cobra_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = zr_spectrum_peek, .poke = zr_spectrum_poke,
    .snapshot_load = cobra_snapshot_load, .snapshot_save = cobra_snapshot_save,
};

const zr_family zr_cobra_family = {
    .id = "cobra", .name = "CoBra",
    .models = MODELS, .model_count = MODEL_COUNT,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .ports = PORTS, .port_count = 1,
    .keys = KEY_TABLE, .key_count = KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = COMBOS, .combo_count = sizeof COMBOS / sizeof COMBOS[0],
    .joysticks = 1,
    .impl = &COBRA_IMPL, .create = cobra_create,
};
