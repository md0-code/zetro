/* The Timisoara TIM-S (machine.h, machine_impl.h): the family "tims" with its model "TIM-S", a ZX Spectrum 48K-
 * compatible with 64 KiB of RAM and an 8255, built on the shared core (spectrum.h). From this project's TIM-S core
 * (MiSTerRO/docs/tims: README, hardware.md; core-tims/rtl/machine_core.sv, shared/romanian/rtl/rp_i8255.sv,
 * rp_tims_video.sv, rp_spectrum_keyboard.sv), the manual and the schematic of the standard model
 * (docs/tims/reference/standard-manual.pdf, standard-schematic.pdf sheets 1, 5, 6, 7 and 9), and the ROM itself.
 * Where the core and the schematic differ the schematic is followed:
 *   - the Z80 on a 168 MHz master clock (tims.h): 3.5 MHz, or on the auxiliary clock (6, 7 or 8 MHz: the core's "CPU
 *     switch") while port C bit 3 is high and the startup ROM is paged out; the video, the tape and the sound keep
 *     the master clock; every I/O cycle runs at 3.5 MHz;
 *   - 16 KiB startup ROM at 0000h over 64 KiB of RAM: reads of 0000h-3FFFh give the ROM until the "ROM" flip-flop
 *     (a toggle, 7474 on sheet 7) is clocked by an I/O cycle at port F0h, then the RAM; the firmware copies itself
 *     into the RAM and write-protects it (port C bit 1);
 *   - the ports are decoded by A0, A3 and A4 alone (74LS138 "a", sheet 7): A4 = A3 = 0 and A0 = 0 the 8255 (E0h,
 *     E2h, E4h, E6h for its four registers, A1 and A2 select), A4 = A3 = 1 the keyboard port (a read gives the 8255's
 *     port A, a write the colour, MIC and speaker latch: FEh and F9h are two of them), A4 = 1, A3 = 0, A0 = 0 the ROM
 *     flip-flop (F0h);
 *   - the 8255: port A inputs the keyboard (bits 0-4, by the address lines A8-A15), the serial line's READY on bit 5
 *     (low: the device is ready, through an RS-232 receiver), EAR on bit 6, the printer's BUSY on bit 7 (inverted:
 *     high is ready); port B the printer data (bits 0-6) and its strobe (bit 7), inverted on the way to the printer
 *     connector; port C bit 0 the serial line, bit 1 low protects the RAM at 0000h-3FFFh from writes, bit 2 high
 *     keeps writes to 4000h-5FFFh out of the video buffer, bit 3 low holds the clock at 3.5 MHz, bit 4 low switches
 *     the RAM off;
 *   - the video reads its own 8 KiB, filled by the writes to 4000h-5FFFh the CPU makes while port C bit 2 is low;
 *   - the printer and serial ports to the host (ports: serial, printer): the printer connector's byte at each strobe,
 *     the serial line decoded as the firmware sends it (7 data bits, 2 stop bits, 2424 baud at 3.5 MHz);
 *   - the keyboard of the Spectrum, the two joystick sockets of the core's keyboard module on the matrix (the first
 *     on the keys 1-5, the second on 6-0), the AY board of the shared core (not original), a tape recorder.
 * The family's second model, the TIM-S Plus, is timsplus.c: the family's tables (options, slots, ports, the keys with the
 * Plus's eleven added ones) are here, and each call of the machine interface goes to the model's board.
 * ZetRo's own code. */
#include "tims.h"
#include "i8255.h"
#include "romdb.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u

/* The firmware's serial bit: the delay at 3981h and the loop around it take 1444 T-states of the 3.5 MHz clock (measured
 * from the guest time of the writes of port C: src/tests/test_tims.c), 2424 baud; the manual's "2400 bd". */
#define SERIAL_BIT_TICKS (1444u * TIMS_TICKS_3_5)


/* --- the model and its tables ---------------------------------------------------------------------------- */

static const char *const ROMS[] = {"tims", NULL};
/* The TIM-S Plus: the replacement bootstrap (the original 2 KiB EPROM is not archived) and the +3 system it starts. */
static const char *const PLUS_ROMS[] = {"tims-plus-loader", "plus3", NULL};
/* LOAD "": in keyword mode J gives LOAD, SYMBOL SHIFT and P a quote. The Plus: the +3 menu's first entry, Loader, is
 * chosen by Enter. */
static const zr_model_desc MODELS[] = {{"tims", "TIM-S", ROMS, "j\"\"\n"}, {"timsplus", "TIM-S Plus", PLUS_ROMS, "\n"}};

static const char *const SWITCH[] = {"off", "on", NULL};
/* The sockets of the core's keyboard module: the first on the keys 1-5, the second on 6-0; the other kinds are not original. */
#define STICK_LABELS(original)                                                                                         \
    {ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_NONE, 1),                                                                 \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR1, (original) == ZR_SPECTRUM_JOY_SINCLAIR1),                      \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR2, (original) == ZR_SPECTRUM_JOY_SINCLAIR2),                      \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_CURSOR, 0), ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_FULLER, 0),           \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_KEMPSTON, 0)}
static const int STICK1_LABELS[ZR_SPECTRUM_JOY_KINDS] = STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR2);
static const int STICK2_LABELS[ZR_SPECTRUM_JOY_KINDS] = STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR1);

const char *const tims_cpu_words[TIMS_CPU_KINDS + 1] = {"3.5", "6", "7", "8", NULL};
const int tims_cpu_labels[TIMS_CPU_KINDS] = {ZR_T_TIMS_CPU_35, ZR_T_TIMS_CPU_6, ZR_T_TIMS_CPU_7, ZR_T_TIMS_CPU_8};

static const char *const PLUS_AYS[] = {"stereo", "mono", NULL};
static const int PLUS_AY_LABELS[] = {ZR_T_TIMSP_AY_STEREO, ZR_T_TIMSP_AY_MONO};
static const char *const UARTS[] = {"native", "exp1", "exp2", "exp3", "exp4", NULL};
static const int UART_LABELS[] = {ZR_T_TIMSP_UART_NATIVE, ZR_T_TIMSP_UART_EXP1, ZR_T_TIMSP_UART_EXP2, ZR_T_TIMSP_UART_EXP3,
                                  ZR_T_TIMSP_UART_EXP4};
static const char *const PRINTERS[] = {"virtual", "physical", NULL};
static const int PRINTER_LABELS[] = {ZR_T_TIMSP_PRINTER_VIRTUAL, ZR_T_TIMSP_PRINTER_PHYSICAL};

/* The standard model's rows first, as it had them (the AY board and Spectrum timing are its alone: the Plus has its own AY,
 * and its Sinclair counterpart, the +3, has a contention the shared ULA does not draw); then the Plus's. */
static const zr_option_desc OPTIONS[TIMS_OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK1, ZR_SPECTRUM_JOY_KEMPSTON, STICK1_LABELS),
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick2", ZR_T_SPEC_JOYSTICK2, ZR_SPECTRUM_JOY_NONE, STICK2_LABELS),
    TIMS_OPTION_CPU(ZR_MODELS_ALL),
    {"ay", ZR_T_SPEC_AY, zr_spectrum_ays, zr_spectrum_ay_labels, 0, TIMS_STANDARD, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET},
    {"timing", ZR_T_SPEC_TIMING, zr_spectrum_timings, zr_spectrum_timing_labels, 0, TIMS_STANDARD,
     ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET, ZR_T_SPEC_TIMING_HINT + 1},
    /* the Plus: its own AY (stereo: A left, C right, as the board mixes them; mono: the three to both sides) */
    {"plus_ay", ZR_T_TIMSP_AY, PLUS_AYS, PLUS_AY_LABELS, TIMS_AY_STEREO, TIMS_PLUS, ZR_OPTION_RESET},
    /* the core's OSD: which of the five 8251 channels is the machine's serial connector, and the printer's BUSY */
    {"uart_channel", ZR_T_TIMSP_UART, UARTS, UART_LABELS, TIMS_UART_NATIVE, TIMS_PLUS, ZR_OPTION_RESET | ZR_OPTION_PORT, ZR_T_TIMSP_UART_HINT + 1},
    {"printer", ZR_T_TIMSP_PRINTER, PRINTERS, PRINTER_LABELS, TIMS_PRINTER_VIRTUAL, TIMS_PLUS, ZR_OPTION_RESET | ZR_OPTION_PORT,
     ZR_T_TIMSP_PRINTER_HINT + 1},
    {"drive_sound", ZR_T_TIMSP_DRIVE_SOUND, SWITCH, NULL, 1, TIMS_PLUS, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_RESET,
     ZR_T_TIMSP_DRIVE_SOUND_HINT + 1},
};

static const zr_media_desc MEDIA[TIMS_SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    {"a", ZR_T_MEDIA_FLOPPY_A, ZR_MEDIA_FLOPPY, "dsk", TIMS_PLUS},
    {"b", ZR_T_MEDIA_FLOPPY_B, ZR_MEDIA_FLOPPY, "dsk", TIMS_PLUS},
};
static const zr_port_desc PORTS[TIMS_PORT_COUNT] = {
    {"serial", ZR_T_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL},
    {"printer", ZR_T_TIMS_PORT_PRINTER, ZR_PORT_PARALLEL, ZR_MODELS_ALL},
};

/* --- the keyboard ------------------------------------------------------------------------------------------- */

/* The Spectrum's keys (SYMBOL SHIFT on the host's Alt: Ctrl is the CTRL key, which is SYMBOL SHIFT too in Spectrum mode, as
 * the core has it), then the Plus's two added lines, the group of keys on the right of the main block (the manual: CTRL,
 * ESCAPE and the others typical of CP/M, and four function keys). The host keys are the core's where the shell does not
 * take them (the function keys are the shell's). */
#define K(name, x, y) {name, name, name, x, y, 4, 4}
static const zr_key_desc KEYS[TIMS_KEYS] = {
    {"CAPS SHIFT", "Left Shift", "CAPS SHIFT", 0, 12, 6, 4}, K("Z", 6, 12), K("X", 10, 12), K("C", 14, 12), K("V", 18, 12),
    K("A", 3, 8), K("S", 7, 8), K("D", 11, 8), K("F", 15, 8), K("G", 19, 8),
    K("Q", 2, 4), K("W", 6, 4), K("E", 10, 4), K("R", 14, 4), K("T", 18, 4),
    K("1", 0, 0), K("2", 4, 0), K("3", 8, 0), K("4", 12, 0), K("5", 16, 0),
    K("0", 36, 0), K("9", 32, 0), K("8", 28, 0), K("7", 24, 0), K("6", 20, 0),
    K("P", 38, 4), K("O", 34, 4), K("I", 30, 4), K("U", 26, 4), K("Y", 22, 4),
    {"ENTER", "Return", "ENTER", 39, 8, 5, 4}, K("L", 35, 8), K("K", 31, 8), K("J", 27, 8), K("H", 23, 8),
    {"SPACE", "Space", "BREAK SPACE", 38, 12, 6, 4}, {"SYMBOL SHIFT", "Left Alt", "SYMBOL SHIFT", 34, 12, 4, 4},
    K("M", 30, 12), K("N", 26, 12), K("B", 22, 12),
    /* D5: A8-A12 */
    {"CTRL", "Left Ctrl", "CTRL", 46, 12, 5, 4}, {"TAB", "Tab", "TAB", 46, 4, 5, 4}, {"CAPS LOCK", "CapsLock", "CAPS LOCK", 46, 8, 5, 4},
    {"F4", NULL, "F4", 58, 0, 4, 4}, {"F2", NULL, "F2", 50, 0, 4, 4},
    /* D7: A8-A13 */
    {"ESC", "Escape", "ESC", 46, 0, 4, 4}, {"DEL", "Delete", "DEL", 51, 4, 5, 4}, {"SCROLL", "ScrollLock", "SCROLL", 51, 8, 5, 4},
    {"BACKSPACE", "Backspace", "BACKSPACE", 51, 12, 5, 4}, {"F1", NULL, "F1", 54, 0, 4, 4}, {"F3", NULL, "F3", 62, 0, 4, 4},
};
#undef K
/* The shared core's combinations but those the added keys take (Backspace, Escape, Caps Lock) and Alt (SYMBOL SHIFT's own). */
enum { KZ = 1, KC = 3, KV = 4, KR = 13, KT = 14, K2 = 16, K5 = 19, K0 = 20, K8 = 22, K7 = 23, K6 = 24, KP = 25, KO = 26,
       KL = 31, KK = 32, KJ = 33, KM = 37, KN = 38 };
#define CS ZR_SPECTRUM_KEY_CAPS_SHIFT
#define SS ZR_SPECTRUM_KEY_SYMBOL_SHIFT
static const zr_combo_desc COMBOS[] = {
    {"Left", K5, CS}, {"Down", K6, CS}, {"Up", K7, CS}, {"Right", K8, CS},
    {".", KM, SS}, {",", KN, SS}, {"'", K7, SS}, {";", KO, SS}, {"-", KJ, SS}, {"=", KL, SS}, {"/", KV, SS},
    {"Shift+.", KT, SS}, {"Shift+,", KR, SS}, {"Shift+'", KP, SS}, {"Shift+;", KZ, SS}, {"Shift+-", K0, SS}, {"Shift+=", KK, SS},
    {"Shift+/", KC, SS},
};
#define COMBO_COUNT ((unsigned)(sizeof COMBOS / sizeof COMBOS[0]))

/* A key of the matrix pulled low in `rows`. */
static void press(uint8_t rows[8], unsigned key) { rows[key / 5u] = (uint8_t)(rows[key / 5u] | 1u << (key % 5u)); }
#define HELD(key) (held >> ((key) - ZR_SPECTRUM_KEYS) & 1u)

uint8_t tims_extra_matrix(unsigned held, uint8_t high, int plus) {
    if (!held) return 0;
    uint8_t rows[8] = {0};
    if (HELD(TIMS_KEY_CTRL)) press(rows, SS);
    if (HELD(TIMS_KEY_ESC)) {
        press(rows, CS);
        press(rows, ZR_SPECTRUM_KEY_SPACE);
    }
    if (HELD(TIMS_KEY_BACKSPACE) || (plus && HELD(TIMS_KEY_DEL))) {
        press(rows, CS);
        press(rows, K0);
    }
    if (HELD(TIMS_KEY_CAPS_LOCK)) {
        press(rows, CS);
        press(rows, K2);
    }
    uint8_t bits = 0;
    for (unsigned r = 0; r < 8u; ++r)
        if (!(high >> r & 1u)) bits = (uint8_t)(bits | rows[r]);
    return bits;
}
#undef HELD
#undef CS
#undef SS

uint8_t tims_extra_lines(unsigned held, uint8_t high) {
    uint8_t value = 0xA0u;
    for (unsigned k = 0; k < TIMS_EXTRA_KEYS; ++k) {
        if (!(held >> k & 1u)) continue;
        const unsigned row = k < TIMS_EXTRA_D5 ? k : k - TIMS_EXTRA_D5;
        if (!(high >> row & 1u)) value = (uint8_t)(value & ~(k < TIMS_EXTRA_D5 ? 0x20u : 0x80u));
    }
    return value;
}

/* --- the CPU clock and the serial line's UART (tims.h) ---------------------------------------------------- */

uint32_t tims_cpu_ticks(unsigned cpu) {
    switch (cpu) {
    case TIMS_CPU_6: return TIMS_CLOCK_HZ / 6000000u;       /* 28 */
    case TIMS_CPU_7: return TIMS_CLOCK_HZ / 7000000u;       /* 24 */
    case TIMS_CPU_8: return TIMS_CLOCK_HZ / 8000000u;       /* 21 */
    default: return TIMS_TICKS_3_5;
    }
}

void tims_set_cpu_clock(zr_spectrum *s, uint32_t ticks) {
    if (s->clock.ticks_per_tstate != ticks) zr_tclock_set_rate(&s->clock, s->z80.t, ticks);
}

/* An I/O cycle lasts four T-states of the 3.5 MHz clock: the clock in use adds what is missing to a whole T-state. */
unsigned tims_io_extra(unsigned cpu) {
    const uint32_t ticks = tims_cpu_ticks(cpu);
    if (ticks >= TIMS_TICKS_3_5) return 0u;
    return (4u * TIMS_TICKS_3_5 + ticks / 2u) / ticks - 4u;
}

static void uart_arm(tims_uart *u) { zr_dev_schedule(&u->dev, u->busy ? u->at : ZR_NEVER); }

#define UART_START 0xFFu                    /* `count` while the start bit is waiting to be confirmed */

/* A sample falls due: the middle of the start bit (a line that has gone high again was a glitch, not a start), a data bit,
 * or the first stop bit that completes (or drops) the byte. */
static void uart_event(void *dev, uint64_t now) {
    tims_uart *u = dev;
    if (!u->busy || now < u->at) {
        uart_arm(u);
        return;
    }
    if (u->count == UART_START) {
        if (u->line) u->busy = 0;
        else {
            u->count = 0;
            u->at += u->bit_ticks;
        }
    } else if (u->count < TIMS_UART_BITS) {
        u->byte = (uint8_t)(u->byte | (unsigned)u->line << u->count);
        ++u->count;
        u->at += u->bit_ticks;
    } else {
        u->busy = 0;
        if (u->line && u->byte_out) u->byte_out(u->ctx, u->byte);
        if (!u->line) {
            /* A line still low after the stop bit is the start of the next byte (its start bit began a bit ago). */
            u->busy = 1;
            u->count = UART_START;
            u->byte = 0;
            u->at = now + u->bit_ticks / 2u;
        }
    }
    uart_arm(u);
}

static uint8_t uart_no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void uart_no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void uart_no_reset(void *dev) { (void)dev; }
static const zr_dev_ops UART_OPS = {.read = uart_no_read, .write = uart_no_write, .event = uart_event, .reset = uart_no_reset};

void tims_uart_init(tims_uart *u, const zr_hw_host *host, uint32_t bit_ticks, void (*byte_out)(void *ctx, uint8_t byte), void *ctx) {
    memset(u, 0, sizeof *u);
    u->dev.ops = &UART_OPS;
    u->dev.host = host;
    u->dev.name = "tims-serial";
    u->bit_ticks = bit_ticks;
    u->byte_out = byte_out;
    u->ctx = ctx;
    u->line = 1;
}

void tims_uart_reset(tims_uart *u) {
    u->line = 1;
    u->busy = u->count = u->byte = 0;
    u->at = 0;
    uart_arm(u);
}

void tims_uart_line(tims_uart *u, int level, uint64_t now) {
    const int before = u->line;
    u->line = (uint8_t)(level != 0);
    if (!u->busy && before && !u->line) {
        /* A falling edge: the start bit, looked at again in its middle, then the data bits sampled in theirs. */
        u->busy = 1;
        u->count = UART_START;
        u->byte = 0;
        u->at = now + u->bit_ticks / 2u;
        uart_arm(u);
    }
}

void tims_uart_save(const tims_uart *u, zr_state_writer *w) {
    zr_sw_u8(w, u->line);
    zr_sw_u8(w, u->busy);
    zr_sw_u8(w, u->count);
    zr_sw_u8(w, u->byte);
    zr_sw_u64(w, u->at);
}

int tims_uart_load(tims_uart *u, zr_state_reader *r) {
    const uint8_t line = zr_sr_u8(r), busy = zr_sr_u8(r), count = zr_sr_u8(r), byte = zr_sr_u8(r);
    const uint64_t at = zr_sr_u64(r);
    if (!zr_sr_ok(r) || line > 1u || busy > 1u || (count > TIMS_UART_BITS && count != UART_START)) return 0;
    u->line = line;
    u->busy = busy;
    u->count = count;
    u->byte = byte;
    u->at = at;
    uart_arm(u);
    return 1;
}

/* --- the board ---------------------------------------------------------------------------------------------- */

typedef struct tims {
    zr_spectrum s;                          /* first: the shared core */
    zr_hw_host chip_host;                   /* the 8255's and the serial line's host */
    zr_i8255 ppi;
    tims_uart uart;
    uint8_t cpu_switch;                     /* TIMS_CPU_* */
    /* the board's latches and memory (saved) */
    uint8_t rom_on;                         /* the startup ROM is paged in at 0000h-3FFFh (the "ROM" flip-flop low) */
    uint8_t pc;                             /* the pins of port C, as the board sees them (high where nothing drives them) */
    uint8_t strobe;                         /* the printer strobe (port B bit 7) as last seen */
    uint8_t low[0x4000];                    /* the RAM at 0000h-3FFFh */
    uint8_t vram[ZR_SPECTRUM_SCREEN];       /* the video buffer the picture reads */
    /* the host's end of the ports (inputs from it saved) */
    uint8_t serial_ready, printer_ready;    /* the device on the serial line is ready; the printer is on line */
    zr_port_host ports;
    int has_ports;
    /* the shared core's decoder of the keyboard port, whose write (the latch) the board keeps */
    zr_bus_write_fn latch_write;
    zr_bus_read_fn keys_read;
    unsigned extra;                         /* the family's added keys held (bit n: key ZR_SPECTRUM_KEYS + n) */
} tims;

/* The pins of port C: what the 8255 drives, and the level the pins float to where it does not (high). */
static uint8_t pc_pins(const tims *t) {
    const uint8_t drv = zr_i8255_driven(&t->ppi, ZR_I8255_PORT_C);
    return (uint8_t)((zr_i8255_out(&t->ppi, ZR_I8255_PORT_C) & drv) | (uint8_t)~drv);
}

/* The core's inputs to the 8255: port C reads 00h where nothing drives it (the core's wiring), the rest per cycle. */
static void ppi_idle_inputs(tims *t) {
    t->ppi.dev.ops->input(&t->ppi, ZR_I8255_PORT(ZR_I8255_PORT_C), 0x00);
}

/* --- the memory map --------------------------------------------------------------------------------------- */

/* 0000h-3FFFh: the ROM or the RAM, the RAM writable unless port C bit 1 is low; 4000h-FFFFh the RAM; port C bit 4 low
 * takes the RAM away (it reads FFh; the ROM stays). */
static void remap(tims *t) {
    zr_spectrum *s = &t->s;
    zr_bus *b = &s->bus;
    const int ram_on = (t->pc >> 4 & 1u) != 0, protect = !(t->pc >> 1 & 1u);
    if (t->rom_on) zr_bus_map_read(b, 0x0000u, 0x4000u, s->rom);
    else zr_bus_map_read(b, 0x0000u, 0x4000u, ram_on ? t->low : NULL);
    zr_bus_map_write(b, 0x0000u, 0x4000u, ram_on && !protect ? t->low : NULL);
    if (ram_on) zr_bus_map_ram(b, 0x4000u, 0xC000u, s->ram);
    else zr_bus_unmap(b, 0x4000u, 0xC000u, ZR_BUS_BOTH);
}

/* The CPU's clock: the auxiliary one when the firmware has paged the ROM out and port C bit 3 is high; with the
 * Spectrum timing option the CPU stays at 3.5 MHz (Sinclair's contention is counted in the 48K's own T-states). */
static int aux_clock(const tims *t) {
    return !t->rom_on && (t->pc >> 3 & 1u) && t->cpu_switch != TIMS_CPU_3_5 && !t->s.cfg.spectrum_timing;
}
static void clock_update(tims *t) {
    tims_set_cpu_clock(&t->s, aux_clock(t) ? tims_cpu_ticks(t->cpu_switch) : TIMS_TICKS_3_5);
}

/* --- video ----------------------------------------------------------------------------------------------- */

/* The CPU's writes to 4000h-5FFFh also go to the video buffer while port C bit 2 is low; the picture is drawn up to the
 * write first (the pages are watched, bus.h). */
static void video_write(void *ctx, uint16_t address, uint8_t value) {
    tims *t = ctx;
    zr_spectrum *s = &t->s;
    const unsigned offset = (unsigned)address - 0x4000u;
    if (address < 0x4000u || offset >= ZR_SPECTRUM_SCREEN) return;
    zr_ula_catch_up(&s->ula, s->cfg.spectrum_timing && !s->sched.in_event ? zr_tclock_time(&s->clock, s->z80.t + 1u)
                                                                           : zr_spectrum_now(s));
    if (!(t->pc & 4u)) t->vram[offset] = value;
}

/* --- the ports ------------------------------------------------------------------------------------------------ */

/* The ports an added board answers (the shared core's joystick and AY decoders): the keyboard decode leaves them to it. */
static int added_board_claims(const tims *t, uint16_t port) {
    const zr_spectrum_config *c = &t->s.cfg;
    const int kempston = c->joystick == ZR_SPECTRUM_JOY_KEMPSTON || c->joystick2 == ZR_SPECTRUM_JOY_KEMPSTON;
    const int fuller = c->joystick == ZR_SPECTRUM_JOY_FULLER || c->joystick2 == ZR_SPECTRUM_JOY_FULLER;
    if (kempston && (port & 0x00FFu) == 0x001Fu) return 1;
    if (fuller && (port & 0x00FFu) == 0x007Fu) return 1;
    return c->ay && (port & c->ay_mask) == c->ay_value;
}

/* The device ends of the port A inputs: bit 5 the serial line's READY (low: the device is ready: the RS-232 receiver
 * inverts its DTR), bit 6 EAR (high without a tape), bit 7 the printer's BUSY inverted (high: ready), bits 0-4 the keys. */
static uint8_t port_a_inputs(tims *t, uint16_t port) {
    zr_spectrum *s = &t->s;
    const uint8_t keys = (uint8_t)(t->keys_read(s, port) & 0x1Fu & ~tims_extra_matrix(t->extra, (uint8_t)(port >> 8), 0));
    const unsigned ear = s->tape ? (unsigned)(zr_tape_deck_level(&s->deck, zr_spectrum_now(s)) != 0) : 1u;
    return (uint8_t)(keys | ear << 6 | (t->serial_ready ? 0u : 0x20u) | (t->printer_ready ? 0x80u : 0u));
}

/* A read of the 8255's port A (the keyboard port and E0h): the pins are given to the chip as they are at this cycle. */
static uint8_t read_port_a(tims *t, uint16_t port) {
    t->ppi.dev.ops->input(&t->ppi, ZR_I8255_PORT(ZR_I8255_PORT_A), port_a_inputs(t, port));
    return t->ppi.dev.ops->read(&t->ppi, ZR_SPACE_IO, 0);
}

static uint8_t keyboard_read(void *ctx, uint16_t port) {
    tims *t = ctx;
    return added_board_claims(t, port) ? 0xFFu : read_port_a(t, port);
}
/* A write to a port of the keyboard decode is the colour, MIC and speaker latch (the shared core's); the 8255 is not written. */
static void keyboard_write(void *ctx, uint16_t port, uint8_t value) {
    tims *t = ctx;
    if (!added_board_claims(t, port)) t->latch_write(&t->s, port, value);
}

/* After every write to the 8255 (data or control): its pins moved, the board follows. */
static void pins_changed(tims *t) {
    const uint8_t pc = pc_pins(t), old = t->pc, strobe = (uint8_t)(t->ppi.latch[ZR_I8255_PORT_B] >> 7);
    t->pc = pc;
    if ((old ^ pc) & 0x12u) remap(t);
    if ((old ^ pc) & 0x08u) clock_update(t);
    tims_uart_line(&t->uart, pc & 1u, zr_spectrum_now(&t->s));
    /* The printer connector's strobe is the inverse of bit 7, its data the inverse of bits 0-6: the byte the printer
     * takes is on the connector when the strobe goes active (the bit rises); a printer that is not on line takes nothing. */
    if (strobe && !t->strobe && t->printer_ready && t->has_ports && t->ports.transmit)
        t->ports.transmit(t->ports.ctx, TIMS_PORT_PRINTER, (uint8_t)(~t->ppi.latch[ZR_I8255_PORT_B] & 0x7Fu));
    t->strobe = strobe;
}

static uint8_t ppi_read(void *ctx, uint16_t port) {
    tims *t = ctx;
    const unsigned reg = (unsigned)(port >> 1) & 3u;
    if (reg == 0) return read_port_a(t, port);
    if (reg == 1) t->ppi.dev.ops->input(&t->ppi, ZR_I8255_PORT(ZR_I8255_PORT_B), (uint8_t)(0xFEu | (t->printer_ready ? 0u : 1u)));
    return t->ppi.dev.ops->read(&t->ppi, ZR_SPACE_IO, reg);
}
static void ppi_write(void *ctx, uint16_t port, uint8_t value) {
    tims *t = ctx;
    t->ppi.dev.ops->write(&t->ppi, ZR_SPACE_IO, (unsigned)(port >> 1) & 3u, value);
    pins_changed(t);
}

/* An I/O cycle at a port of the ROM flip-flop (a read as much as a write) clocks it: the flip-flop toggles. */
static void rom_toggle(tims *t) {
    t->rom_on = (uint8_t)!t->rom_on;
    remap(t);
    clock_update(t);
}
static uint8_t rom_read(void *ctx, uint16_t port) {
    (void)port;
    rom_toggle(ctx);
    return 0xFFu;
}
static void rom_write(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    (void)value;
    rom_toggle(ctx);
}

/* --- the CPU's lengthened cycles --------------------------------------------------------------------------- */

static unsigned tims_stretch(zr_spectrum *s, int kind, uint16_t address) {
    (void)address;
    const tims *t = (const tims *)s;
    return aux_clock(t) && (kind == ZR_Z80_IN || kind == ZR_Z80_OUT) ? tims_io_extra(t->cpu_switch) : 0u;
}
/* Loader acceleration: no turn of a loop that reads a port can be jumped while the cycles are lengthened. */
static uint64_t tims_quiet(zr_spectrum *s, uint64_t t, uint64_t limit) { return aux_clock((const tims *)s) ? t : limit; }

/* --- the serial line and the printer to the host ------------------------------------------------------------ */

static void serial_byte(void *ctx, uint8_t byte) {
    tims *t = ctx;
    if (t->has_ports && t->ports.transmit) t->ports.transmit(t->ports.ctx, TIMS_PORT_SERIAL, byte);
}

static void tims_set_ports(void *self, const zr_port_host *host) {
    tims *t = self;
    t->has_ports = host != NULL;
    if (host) t->ports = *host;
    /* The serial line's DTR and RTS are held asserted (the core's wiring). */
    if (host && host->outputs) host->outputs(host->ctx, TIMS_PORT_SERIAL, 1, 1);
}
/* The TIM-S has no receive line: the bytes the host offers are taken and lost. */
static size_t tims_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    (void)self; (void)port; (void)data;
    return count;
}
/* The host's end: for the serial line DSR says the device is ready; for the printer it says it is on line. */
static void tims_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    tims *t = self;
    (void)dcd; (void)cts; (void)ri;
    if (port == TIMS_PORT_SERIAL) t->serial_ready = dsr != 0;
    else if (port == TIMS_PORT_PRINTER) t->printer_ready = dsr != 0;
}

/* --- life cycle ---------------------------------------------------------------------------------------------- */

static uint64_t chip_now(void *ctx) { return zr_spectrum_now(ctx); }
static void chip_schedule(void *ctx, void *dev, uint64_t when) {
    zr_spectrum *s = ctx;
    zr_sched_set(&s->sched, dev, when);
}
static void chip_line(void *ctx, void *dev, int pin, int state) { (void)ctx; (void)dev; (void)pin; (void)state; }

/* The board as the RESET line leaves it: the startup ROM paged in, the 8255 reset (every pin an input, floating high),
 * the clock at 3.5 MHz. */
static void board_reset(tims *t) {
    t->rom_on = 1;
    t->ppi.dev.ops->reset(&t->ppi);
    ppi_idle_inputs(t);
    t->pc = pc_pins(t);
    t->strobe = 0;
    tims_uart_reset(&t->uart);
    remap(t);
    clock_update(t);
}

static void tims_reset(void *self, int power_on) {
    tims *t = self;
    zr_spectrum_reset(&t->s, power_on);
    if (power_on) {
        memset(t->low, 0, sizeof t->low);
        memset(t->vram, 0, sizeof t->vram);
    }
    board_reset(t);
}

static void *tims_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.clock_hz = TIMS_CLOCK_HZ;
    cfg.ticks_per_tstate = TIMS_TICKS_3_5;
    cfg.ula.ticks_per_pixel = TIMS_CLOCK_HZ / 7000000u;         /* a 7 MHz pixel */
    cfg.ula.ticks_per_tstate = TIMS_TICKS_3_5;                  /* the frame interrupt's 32 T-states of 3.5 MHz */
    cfg.fe_mask = 0x0018u;                                      /* the keyboard port: A3 and A4 high (the 74LS138) */
    cfg.fe_value = 0x0018u;
    cfg.nmi = 1;                                                /* the core's NMI key (F10) */
    cfg.joystick = (uint8_t)o[TIMS_OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[TIMS_OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_48K;                         /* the TIM-S is a 48K */
    cfg.spectrum_timing = o[TIMS_OPTION_TIMING] != 0;
    cfg.ay = (uint8_t)o[TIMS_OPTION_AY];
    cfg.ay_mask = 0x80FFu;                                      /* A15 and the whole low byte: FDh, as an expansion board */
    cfg.ay_value = 0x80FDu;
    const unsigned cpu = o[TIMS_OPTION_CPU] < TIMS_CPU_KINDS ? o[TIMS_OPTION_CPU] : TIMS_CPU_3_5;
    if (cpu != TIMS_CPU_3_5 && !cfg.spectrum_timing) {
        cfg.stretch = tims_stretch;
        cfg.stretch_quiet = tims_quiet;
    }
    tims *t = (tims *)zr_spectrum_create(&cfg, setup, sizeof(tims), error, capacity);
    if (!t) return NULL;
    zr_spectrum *s = &t->s;
    t->cpu_switch = (uint8_t)cpu;
    /* The AY chip's clock is 1.75 MHz of the 168 MHz master clock (the shared core assumed 14 MHz). */
    if (cfg.ay) {
        const int mode = cfg.ay == ZR_SPECTRUM_AY_ABC ? ZR_AY_ABC : cfg.ay == ZR_SPECTRUM_AY_ACB ? ZR_AY_ACB : ZR_AY_MONO;
        zr_ay_init(&s->ay, &s->host, TIMS_CLOCK_HZ, 1750000u, ZR_SPECTRUM_AUDIO_RATE, mode, NULL);
    }
    /* The chips' host: guest time and the scheduler of the board. */
    t->chip_host.ctx = t;
    t->chip_host.now = chip_now;
    t->chip_host.schedule = chip_schedule;
    t->chip_host.line = chip_line;
    zr_i8255_init(&t->ppi, &t->chip_host, "tims-8255");
    tims_uart_init(&t->uart, &t->chip_host, SERIAL_BIT_TICKS, serial_byte, t);
    /* The video reads its own buffer, filled by the writes to 4000h-5FFFh. */
    s->screen = t->vram;
    zr_bus_set_hook(&s->bus, video_write, t);
    /* The ports (the 74LS138's A0, A3 and A4): the shared core registered the keyboard port first, as decoder 0. */
    t->keys_read = s->bus.dec[0].read;
    t->latch_write = s->bus.dec[0].write;
    s->bus.dec[0].read = keyboard_read;
    s->bus.dec[0].write = keyboard_write;
    s->bus.dec[0].ctx = t;
    zr_bus_add_io(&s->bus, 0x0019u, 0x0000u, ppi_read, ppi_write, t);     /* the 8255: A4 = A3 = A0 = 0 */
    zr_bus_add_io(&s->bus, 0x0019u, 0x0010u, rom_read, rom_write, t);     /* the ROM flip-flop: A4 = 1, A3 = A0 = 0 */
    if (cpu != TIMS_CPU_3_5)
        snprintf(s->description, sizeof s->description, "%s, %s MHz", setup->family->models[setup->model].name, tims_cpu_words[cpu]);
    tims_reset(t, 1);
    return t;
}

/* --- states ------------------------------------------------------------------------------------------------- */

static void tims_save(const void *self, zr_state_writer *w) {
    const tims *t = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "TIMS", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, t->rom_on);
    zr_sw_u8(w, t->strobe);
    zr_sw_u8(w, t->serial_ready);
    zr_sw_u8(w, t->printer_ready);
    tims_uart_save(&t->uart, w);
    zr_sw_packed(w, t->low, sizeof t->low);
    zr_sw_packed(w, t->vram, sizeof t->vram);
    zr_sw_end(w);
    zr_sw_begin(w, "PPI ", 0);
    t->ppi.dev.ops->save(&t->ppi, w);
    zr_sw_end(w);
}

static int tims_load(void *self, zr_state_reader *r) {
    tims *t = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "TIMS", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    const uint8_t rom_on = zr_sr_u8(r), strobe = zr_sr_u8(r), serial_ready = zr_sr_u8(r), printer_ready = zr_sr_u8(r);
    if (!zr_sr_ok(r) || rom_on > 1u || strobe > 1u || serial_ready > 1u || printer_ready > 1u) return 0;
    if (!tims_uart_load(&t->uart, r)) return 0;
    zr_sr_packed(r, t->low, sizeof t->low);
    zr_sr_packed(r, t->vram, sizeof t->vram);
    if (!zr_sr_ok(r)) return 0;
    if (!zr_sr_chunk(r, "PPI ", 0) || !t->ppi.dev.ops->load(&t->ppi, r)) return 0;
    t->rom_on = rom_on;
    t->strobe = strobe;
    t->serial_ready = serial_ready;
    t->printer_ready = printer_ready;
    ppi_idle_inputs(t);
    t->pc = pc_pins(t);
    remap(t);
    clock_update(t);
    return 1;
}

/* --- snapshots ---------------------------------------------------------------------------------------------- */

/* A snapshot is of a 48K Spectrum that has started: the machine in the state its firmware leaves it in (the startup ROM
 * out, the RAM at 0000h-3FFFh the ROM's copy, write-protected, port C F9h under control word 90h) with the snapshot's
 * RAM, the video buffer filled from it. A snapshot refused leaves the board as it was. */
static int tims_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    tims *t = self;
    zr_spectrum *s = &t->s;
    if (!zr_spectrum_snapshot_load(self, snap, error, capacity)) return 0;
    t->rom_on = 0;
    t->ppi.dev.ops->write(&t->ppi, ZR_SPACE_IO, 3, 0x90u);
    t->ppi.dev.ops->write(&t->ppi, ZR_SPACE_IO, 2, 0xF9u);
    t->pc = pc_pins(t);
    t->strobe = 0;
    tims_uart_reset(&t->uart);
    memcpy(t->low, s->rom, sizeof t->low);
    memcpy(t->vram, s->ram, sizeof t->vram);
    remap(t);
    clock_update(t);
    return 1;
}

/* The family's added keys: the standard model's keyboard has none of them, so they stand for the Spectrum keys the core gives
 * those host keys (tims_extra_matrix). */
static void tims_key(void *self, unsigned key, int down) {
    tims *t = self;
    if (key < ZR_SPECTRUM_KEYS) zr_spectrum_key(self, key, down);
    else if (key < TIMS_KEYS) {
        const unsigned bit = 1u << (key - ZR_SPECTRUM_KEYS);
        t->extra = down ? t->extra | bit : t->extra & ~bit;
    }
}

static const zr_machine_impl STANDARD_IMPL = {
    .destroy = zr_spectrum_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = tims_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = zr_spectrum_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = zr_spectrum_frame, .text = zr_spectrum_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = tims_key, .joystick = zr_spectrum_joystick,
    .insert = zr_spectrum_insert, .media = zr_spectrum_media, .eject = zr_spectrum_eject, .activity = zr_spectrum_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .set_ports = tims_set_ports, .port_receive = tims_port_receive, .port_modem = tims_port_modem,
    .save = tims_save, .load = tims_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = zr_spectrum_peek, .poke = zr_spectrum_poke,
    .snapshot_load = tims_snapshot_load, .snapshot_save = zr_spectrum_snapshot_save,
};

/* --- the family: each call goes to the model's board -------------------------------------------------------- */

#define IMPL(self) (((const zr_spectrum *)(self))->model == TIMS_MODEL_PLUS ? &tims_plus_impl : &STANDARD_IMPL)

static void f_destroy(void *self) { if (self) IMPL(self)->destroy(self); }
static void f_reset(void *self, int power_on) { IMPL(self)->reset(self, power_on); }
static zr_run_status f_run(void *self, uint64_t ticks) { return IMPL(self)->run(self, ticks); }
static int f_frame(void *self, zr_frame *frame) { return IMPL(self)->frame(self, frame); }
static size_t f_text(void *self, char *out, size_t capacity) { return IMPL(self)->text(self, out, capacity); }
static void f_key(void *self, unsigned key, int down) { IMPL(self)->key(self, key, down); }
static int f_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    return IMPL(self)->insert(self, slot, media, error, capacity);
}
static zr_media *f_media(const void *self, unsigned slot) { return IMPL(self)->media(self, slot); }
static int f_eject(void *self, unsigned slot) { return IMPL(self)->eject(self, slot); }
static unsigned f_activity(const void *self) { return IMPL(self)->activity(self); }
static void f_set_ports(void *self, const zr_port_host *host) { IMPL(self)->set_ports(self, host); }
static size_t f_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    return IMPL(self)->port_receive(self, port, data, count);
}
static void f_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) { IMPL(self)->port_modem(self, port, dcd, cts, dsr, ri); }
static void f_save(const void *self, zr_state_writer *w) { IMPL(self)->save(self, w); }
static int f_load(void *self, zr_state_reader *r) { return IMPL(self)->load(self, r); }
static int f_peek(void *self, uint32_t address, uint8_t *value) { return IMPL(self)->peek(self, address, value); }
static int f_poke(void *self, uint32_t address, uint8_t value) { return IMPL(self)->poke(self, address, value); }
static int f_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    return IMPL(self)->snapshot_load(self, snap, error, capacity);
}
static int f_snapshot_save(void *self, struct zr_snap *snap) { return IMPL(self)->snapshot_save(self, snap); }

static const zr_machine_impl FAMILY_IMPL = {
    .destroy = f_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = f_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = f_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = f_frame, .text = f_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = f_key, .joystick = zr_spectrum_joystick,
    .insert = f_insert, .media = f_media, .eject = f_eject, .activity = f_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .set_ports = f_set_ports, .port_receive = f_port_receive, .port_modem = f_port_modem,
    .save = f_save, .load = f_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = f_peek, .poke = f_poke,
    .snapshot_load = f_snapshot_load, .snapshot_save = f_snapshot_save,
};

static void *family_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    return setup->model == TIMS_MODEL_PLUS ? tims_plus_create(setup, error, capacity) : tims_create(setup, error, capacity);
}

const zr_family zr_tims_family = {
    .id = "tims", .name = "TIM-S",
    .models = MODELS, .model_count = 2,
    .options = OPTIONS, .option_count = TIMS_OPTION_COUNT,
    .media = MEDIA, .media_count = TIMS_SLOT_COUNT,
    .ports = PORTS, .port_count = TIMS_PORT_COUNT,
    .keys = KEYS, .key_count = TIMS_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = COMBOS, .combo_count = COMBO_COUNT,
    .joysticks = 2,
    .impl = &FAMILY_IMPL, .create = family_create,
};
