/* The aMIC (ITC Bucuresti, 1983; machine_impl.h): the family "amic" with its one model, a Z80 microcomputer of its own as this project's
 * aMIC core documents it (MiSTerRO/docs/amic/README.md, hardware.md, validation.md; the two-volume manual, whose figures 3.15 to 3.18 are
 * the keyboard, the video and the clock; core-amic/rtl/machine_core.sv with the shared rp_native_machine.sv, rp_native_keyboard.sv,
 * rp_mono_video.sv, rp_i8255.sv and rp_i8251.sv). Wired from the chips, not from the Spectrum core: the aMIC is not a Spectrum.
 *
 *   CPU      Z80 at 2.5 MHz from the 10 MHz master clock (the sync generator's crystal)
 *   memory   16 KiB ROM (monitor, BASIC with G0800) at 0000-3FFF; RAM from 4000h: 16 KiB (to 7FFF), 32 (to BFFF) or 48 (to FFFF), the
 *            rest reading FFh; the bitmap is the first 8 KiB of it, 4000-5FFF
 *   ports    00/01 the 8251 (data, status and command: A1-A7 low); 20-23 the keyboard 8255 (A, B, C, control: A2-A7 = 001000); 40-43 the
 *            external 8255. The upper address byte is not decoded
 *   8255 #1  mode 0 as the monitor sets it (control 92h): port A reads the keyboard row port C's bits 0-2 select (a pressed key reads
 *            0); port B bit 0 the cassette input, bit 5 SHIFT, bit 6 CTRL, bit 7 CAPS LOCK (0: pressed); port C bit 3 the loudspeaker,
 *            bit 5 inverts the picture, bit 7 the cassette output. An output bit the chip does not drive floats high
 *   8255 #2  the external lines: a joystick pulls PA0-PA4 low (right, left, down, up, fire); the other lines read high
 *   8251     the serial port; its transmit and receive clocks are 16 times the baud switch's setting (1200, 600 or 300)
 *   interrupt  none; the NMI is the INT key
 *   video    hw/monovideo: 256 x 256 white on black (a set bit is dark), 640 x 312 dots at 10 MHz
 *   tape     a recorder on the cassette input and output, real time
 *
 * ZetRo's own code. */
#include "native.h"
#include "i8251.h"
#include "i8255.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define KEY_PRESS_GAP 1000000u                  /* 100 ms: the monitor's key click lasts 90 ms and no key is read meanwhile */
#define KEY_RELEASE_GAP 200000u                 /* 20 ms: the least a key stays down */

enum { OPTION_MEMORY, OPTION_BAUD, OPTION_MONITOR, OPTION_COUNT };

/* --- the family's tables ---------------------------------------------------------------------------- */

static const char *const ROMS[] = {"amic", NULL};
/* G0800 starts BASIC (the monitor's G command), and then LOA loads from the tape. The monitor reads no key while it clears the screen
 * and BASIC measures its RAM (0.4 s with 32 KiB, 0.8 with 48), and a key typed then is lost: Enter does nothing at BASIC's empty
 * prompt, so a few of them wait for it. */
static const zr_model_desc MODELS[] = {{"amic", "aMIC", ROMS, "G0800\n\n\n\n\n\n\n\n\n\n\n\n\n\nLOA\n"}};

static const char *const MEMORY_WORDS[] = {"32", "16", "48", NULL};
static const int MEMORY_LABELS[] = {ZR_T_AMIC_MEM_32, ZR_T_AMIC_MEM_16, ZR_T_AMIC_MEM_48};
static const char *const BAUD_WORDS[] = {"1200", "600", "300", NULL};
static const unsigned BAUDS[] = {1200, 600, 300};
static const char *const MONITOR_WORDS[] = {"white", "green", NULL};
static const int MONITOR_LABELS[] = {ZR_T_NAT_MONITOR_WHITE, ZR_T_NAT_MONITOR_GREEN};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    {"memory", ZR_T_AMIC_MEMORY, MEMORY_WORDS, MEMORY_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET, 0},
    {"serial_baud", ZR_T_NAT_BAUD, BAUD_WORDS, NULL, 0, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_PORT, 0},
    {"monitor", ZR_T_NAT_MONITOR, MONITOR_WORDS, MONITOR_LABELS, 0, ZR_MODELS_ALL, 0, ZR_T_NAT_MONITOR_HINT + 1},
};
static const zr_media_desc MEDIA[] = {{"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL}};
static const zr_port_desc PORTS[] = {{"serial", ZR_T_AMIC_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL}};

/* The keyboard (figures 3.16 and 3.17 of the manual): a matrix of 8 scan lines (port C bits 0-2) by 8 return lines (port A), the
 * key's code here its row * 8 + column, and the three keys read apart on port B (SHIFT PB5, CTRL PB6, CAPS LOCK PB7). INT and RESET
 * are not in the matrix: they are the NMI and the reset switches. The codes are the monitor's own table (3DD8h in the ROM) read
 * back: the matrix_n indices of rp_native_keyboard.sv agree with it. */
enum { M_NONE = 0xFF, M_SHIFT = 0xF0, M_CTRL = 0xF1, M_CAPS = 0xF2, M_INT = 0xF3, M_RESET = 0xF4 };
#define AMIC_KEYS(X) \
    X(TAB, "TAB", "Tab", "TAB", 0, 0, 6, 0x00) X(1, "1", "1", "1", 6, 0, 4, 0x01) X(2, "2", "2", "2", 10, 0, 4, 0x02) \
    X(3, "3", "3", "3", 14, 0, 4, 0x03) X(4, "4", "4", "4", 18, 0, 4, 0x04) X(5, "5", "5", "5", 22, 0, 4, 0x05) \
    X(6, "6", "6", "6", 26, 0, 4, 0x06) X(7, "7", "7", "7", 30, 0, 4, 0x07) X(8, "8", "8", "8", 34, 0, 4, 0x08) \
    X(9, "9", "9", "9", 38, 0, 4, 0x09) X(0, "0", "0", "0", 42, 0, 4, 0x0A) X(MINUS, "-", "-", "-", 46, 0, 4, 0x0B) \
    X(EQUALS, "=", "=", "=", 50, 0, 4, 0x0C) X(BS, "BS", "Backspace", "BS", 54, 0, 6, 0x0D) \
    X(Q, "Q", "Q", "Q", 7, 4, 4, 0x11) X(W, "W", "W", "W", 11, 4, 4, 0x12) X(E, "E", "E", "E", 15, 4, 4, 0x13) \
    X(R, "R", "R", "R", 19, 4, 4, 0x14) X(T, "T", "T", "T", 23, 4, 4, 0x15) X(Y, "Y", "Y", "Y", 27, 4, 4, 0x16) \
    X(U, "U", "U", "U", 31, 4, 4, 0x17) X(I, "I", "I", "I", 35, 4, 4, 0x18) X(O, "O", "O", "O", 39, 4, 4, 0x19) \
    X(P, "P", "P", "P", 43, 4, 4, 0x1A) X(LBRACKET, "[", "[", "[", 47, 4, 4, 0x1B) X(BACKSLASH, "\\", "\\", "\\", 51, 4, 4, 0x1C) \
    X(LF, "LF", "Keypad Enter", "LF", 55, 4, 4, 0x1D) X(DEL, "DEL", "Delete", "DEL", 59, 4, 5, 0x1E) \
    X(A, "A", "A", "A", 9, 8, 4, 0x20) X(S, "S", "S", "S", 13, 8, 4, 0x21) X(D, "D", "D", "D", 17, 8, 4, 0x22) \
    X(F, "F", "F", "F", 21, 8, 4, 0x23) X(G, "G", "G", "G", 25, 8, 4, 0x24) X(H, "H", "H", "H", 29, 8, 4, 0x25) \
    X(J, "J", "J", "J", 33, 8, 4, 0x26) X(K, "K", "K", "K", 37, 8, 4, 0x27) X(L, "L", "L", "L", 41, 8, 4, 0x28) \
    X(SEMICOLON, ";", ";", ";", 45, 8, 4, 0x29) X(QUOTE, "'", "'", "'", 49, 8, 4, 0x2A) X(ESC, "ESC", "Escape", "ESC", 53, 8, 4, 0x2B) \
    X(RETURN, "RETURN", "Return", "RETURN", 57, 8, 7, 0x2C) \
    X(Z, "Z", "Z", "Z", 11, 12, 4, 0x30) X(X, "X", "X", "X", 15, 12, 4, 0x31) X(C, "C", "C", "C", 19, 12, 4, 0x32) \
    X(V, "V", "V", "V", 23, 12, 4, 0x33) X(B, "B", "B", "B", 27, 12, 4, 0x34) X(N, "N", "N", "N", 31, 12, 4, 0x35) \
    X(M, "M", "M", "M", 35, 12, 4, 0x36) X(COMMA, ",", ",", ",", 39, 12, 4, 0x37) X(PERIOD, ".", ".", ".", 43, 12, 4, 0x38) \
    X(SLASH, "/", "/", "/", 47, 12, 4, 0x39) X(SPACE, "SPACE", "Space", "SPACE BAR", 14, 16, 32, 0x3B) \
    X(SHIFT, "SHIFT", "Left Shift", "SHIFT", 0, 12, 11, M_SHIFT) X(CTRL, "CTRL", "Left Ctrl", "CTRL", 0, 8, 9, M_CTRL) \
    X(CAPS, "CAPS LOCK", "Caps Lock", "CAPS LOCK", 0, 4, 7, M_CAPS) X(INT, "INT", NULL, "INT", 62, 0, 4, M_INT) \
    X(RESET, "RESET", NULL, "RESET", 66, 0, 6, M_RESET)

enum {
#define X(id, name, host, legend, x, y, w, code) KEY_##id,
    AMIC_KEYS(X)
#undef X
    KEYS
};
static const zr_key_desc KEY_TABLE[KEYS] = {
#define X(id, name, host, legend, x, y, w, code) {name, host, legend, x, y, w, 4},
    AMIC_KEYS(X)
#undef X
};
static const uint8_t MATRIX[KEYS] = {
#define X(id, name, host, legend, x, y, w, code) code,
    AMIC_KEYS(X)
#undef X
};

#define N_ ZR_KEY_NONE
#define SH KEY_SHIFT
/* What is typed for each character: the monitor's table gives capitals for the keys alone and, with SHIFT, the symbols above the
 * digits and the shifted punctuation (and, on the letters, the block graphics that the screen draws from codes 61h and up, which
 * are not characters of the text). Both cases of a letter type the key alone, so a script written for the core's simulator, which
 * presses the keys, types the same. */
#define LETTER(lower, upper, key) {lower, key, N_}, {upper, key, N_}
static const zr_typing_desc TYPING[] = {
    LETTER('a', 'A', KEY_A), LETTER('b', 'B', KEY_B), LETTER('c', 'C', KEY_C), LETTER('d', 'D', KEY_D), LETTER('e', 'E', KEY_E),
    LETTER('f', 'F', KEY_F), LETTER('g', 'G', KEY_G), LETTER('h', 'H', KEY_H), LETTER('i', 'I', KEY_I), LETTER('j', 'J', KEY_J),
    LETTER('k', 'K', KEY_K), LETTER('l', 'L', KEY_L), LETTER('m', 'M', KEY_M), LETTER('n', 'N', KEY_N), LETTER('o', 'O', KEY_O),
    LETTER('p', 'P', KEY_P), LETTER('q', 'Q', KEY_Q), LETTER('r', 'R', KEY_R), LETTER('s', 'S', KEY_S), LETTER('t', 'T', KEY_T),
    LETTER('u', 'U', KEY_U), LETTER('v', 'V', KEY_V), LETTER('w', 'W', KEY_W), LETTER('x', 'X', KEY_X), LETTER('y', 'Y', KEY_Y),
    LETTER('z', 'Z', KEY_Z),
    {'0', KEY_0, N_}, {'1', KEY_1, N_}, {'2', KEY_2, N_}, {'3', KEY_3, N_}, {'4', KEY_4, N_}, {'5', KEY_5, N_}, {'6', KEY_6, N_},
    {'7', KEY_7, N_}, {'8', KEY_8, N_}, {'9', KEY_9, N_},
    {' ', KEY_SPACE, N_}, {'\n', KEY_RETURN, N_}, {'\r', KEY_RETURN, N_}, {'\t', KEY_TAB, N_}, {'\b', KEY_BS, N_},
    {'\x7F', KEY_DEL, N_}, {'\x1B', KEY_ESC, N_}, {'\x03', KEY_C, KEY_CTRL},
    {'-', KEY_MINUS, N_}, {'=', KEY_EQUALS, N_}, {'[', KEY_LBRACKET, N_}, {'\\', KEY_BACKSLASH, N_}, {';', KEY_SEMICOLON, N_},
    {'\'', KEY_QUOTE, N_}, {',', KEY_COMMA, N_}, {'.', KEY_PERIOD, N_}, {'/', KEY_SLASH, N_},
    /* with SHIFT: the keys' second table */
    {'!', KEY_1, SH}, {'@', KEY_2, SH}, {'#', KEY_3, SH}, {'$', KEY_4, SH}, {'%', KEY_5, SH}, {'^', KEY_6, SH}, {'&', KEY_7, SH},
    {'*', KEY_8, SH}, {'(', KEY_9, SH}, {')', KEY_0, SH}, {'+', KEY_EQUALS, SH}, {']', KEY_LBRACKET, SH}, {':', KEY_SEMICOLON, SH},
    {'"', KEY_QUOTE, SH}, {'<', KEY_COMMA, SH}, {'>', KEY_PERIOD, SH}, {'?', KEY_SLASH, SH},
};

/* Host keys the matrix has no key for: the second SHIFT and CTRL of a PC keyboard, and the PC's ] (SHIFT and [ on the aMIC). */
static const zr_combo_desc COMBOS[] = {
    {"Right Shift", KEY_SHIFT, N_}, {"Right Ctrl", KEY_CTRL, N_}, {"]", KEY_LBRACKET, SH}, {"Left Alt", KEY_CTRL, N_},
};

/* --- the board -------------------------------------------------------------------------------------------- */

typedef struct amic {
    zr_native n;
    zr_i8255 ppi1, ppi2;
    zr_i8251 uart;
    unsigned baud;
    uint8_t rows[8];                            /* the keys held, a bit for each column of a scan line */
    uint8_t shift, ctrl, caps;                  /* the keys read on port B; CAPS LOCK holds its state */
    uint8_t joy;                                /* the joystick: ZR_JOY_* */
    uint8_t pc;                                 /* the pins of port C as last seen */
    /* the serial port */
    zr_port_host ports;
    uint8_t has_ports, loopback, dsr, cts, dtr_sent, dtr, rts;
    /* the text read-back */
    int32_t font_at;
} amic;

#define A(n) ((amic *)(n))

/* The pins of port C: what the chip drives, and the level the pins float to where it does not (high). */
static uint8_t pc_pins(const amic *a) {
    const uint8_t drv = zr_i8255_driven(&a->ppi1, ZR_I8255_PORT_C);
    return (uint8_t)((zr_i8255_out(&a->ppi1, ZR_I8255_PORT_C) & drv) | (uint8_t)~drv);
}

/* What port C's pins do: the picture's inversion, the loudspeaker, the cassette output; at the time the write's cycle ends. */
static void pc_sync(amic *a, uint64_t t) {
    const uint8_t pins = pc_pins(a), changed = (uint8_t)(pins ^ a->pc);
    if (!changed) return;
    a->pc = pins;
    if (changed & 0x20u) {
        zr_monovideo_catch_up(&a->n.video, t);
        zr_monovideo_set_invert(&a->n.video, pins >> 5 & 1u);
    }
    if (changed & 0x08u) zr_beeper_set(&a->n.beeper, ZR_BEEPER_SPEAKER, pins >> 3 & 1u, t);
    if (changed & 0x80u) zr_native_mic(&a->n, pins >> 7 & 1u, t);
}

/* --- the ports --------------------------------------------------------------------------------------------- */

static uint8_t ppi1_read(void *ctx, uint16_t port) {
    amic *a = ctx;
    zr_i8255 *p = &a->ppi1;
    const unsigned reg = port & 3u;
    if (reg == 0u) p->dev.ops->input(&p->dev, ZR_I8255_PORT(ZR_I8255_PORT_A), (uint8_t)~a->rows[pc_pins(a) & 7u]);
    else if (reg == 1u) {
        const uint8_t flags = (uint8_t)((a->caps ? 0u : 0x80u) | (a->ctrl ? 0u : 0x40u) | (a->shift ? 0u : 0x20u) | 0x1Eu | (zr_native_ear(&a->n) ? 1u : 0u));
        p->dev.ops->input(&p->dev, ZR_I8255_PORT(ZR_I8255_PORT_B), flags);
    }
    return p->dev.ops->read(&p->dev, ZR_SPACE_IO, reg);
}
static void ppi1_write(void *ctx, uint16_t port, uint8_t value) {
    amic *a = ctx;
    a->ppi1.dev.ops->write(&a->ppi1.dev, ZR_SPACE_IO, port & 3u, value);
    pc_sync(a, zr_native_at(&a->n, 4u));
}

static uint8_t ppi2_read(void *ctx, uint16_t port) {
    amic *a = ctx;
    zr_i8255 *p = &a->ppi2;
    const unsigned reg = port & 3u;
    if (reg == 0u) {
        uint8_t pulled = 0;                     /* a joystick pulls PA0-PA4 low: right, left, down, up, fire */
        if (a->joy & ZR_JOY_RIGHT) pulled |= 0x01u;
        if (a->joy & ZR_JOY_LEFT) pulled |= 0x02u;
        if (a->joy & ZR_JOY_DOWN) pulled |= 0x04u;
        if (a->joy & ZR_JOY_UP) pulled |= 0x08u;
        if (a->joy & ZR_JOY_FIRE) pulled |= 0x10u;
        p->dev.ops->input(&p->dev, ZR_I8255_PORT(ZR_I8255_PORT_A), (uint8_t)~pulled);
    }
    return p->dev.ops->read(&p->dev, ZR_SPACE_IO, reg);
}
static void ppi2_write(void *ctx, uint16_t port, uint8_t value) {
    amic *a = ctx;
    a->ppi2.dev.ops->write(&a->ppi2.dev, ZR_SPACE_IO, port & 3u, value);
}

static uint8_t uart_read(void *ctx, uint16_t port) {
    amic *a = ctx;
    return a->uart.dev.ops->read(&a->uart.dev, ZR_SPACE_IO, port & 1u);
}
static void uart_write(void *ctx, uint16_t port, uint8_t value) {
    amic *a = ctx;
    a->uart.dev.ops->write(&a->uart.dev, ZR_SPACE_IO, port & 1u, value);
}

/* --- the keys ----------------------------------------------------------------------------------------------- */

static void reset(void *self, int power_on);

static void key_apply(zr_native *n, unsigned key, int down) {
    amic *a = A(n);
    if (key >= KEYS) return;
    const uint8_t code = MATRIX[key];
    switch (code) {
    case M_SHIFT: a->shift = (uint8_t)(down != 0); break;
    case M_CTRL: a->ctrl = (uint8_t)(down != 0); break;
    case M_CAPS: if (down) a->caps ^= 1u; break;                    /* the lock key: each press turns it over */
    case M_INT: if (down) zr_z80_nmi(&n->z80); break;
    case M_RESET: if (down) reset(a, 0); break;
    default:
        if (code < 64u) {
            const uint8_t bit = (uint8_t)(1u << (code & 7u));
            if (down) a->rows[code >> 3] = (uint8_t)(a->rows[code >> 3] | bit);
            else a->rows[code >> 3] = (uint8_t)(a->rows[code >> 3] & ~bit);
        }
    }
}

static void amic_joystick(void *self, unsigned stick, unsigned held) {
    if (stick == 0) A(self)->joy = (uint8_t)held;
}

/* --- the serial port ---------------------------------------------------------------------------------------- */

static void send_outputs(amic *a) {
    if (a->dtr_sent && a->has_ports && a->ports.outputs) a->ports.outputs(a->ports.ctx, 0, a->dtr, a->rts);
}

/* The 8251's pins: DTR and RTS go to the host's modem lines, or round to DSR and CTS in the loopback. */
static void amic_line(zr_native *n, void *dev, int pin, int state) {
    amic *a = A(n);
    if (dev != (void *)&a->uart) return;
    if (pin != ZR_I8251_PIN_DTR && pin != ZR_I8251_PIN_RTS) return;
    if (pin == ZR_I8251_PIN_DTR) a->dtr = (uint8_t)(state != 0);
    else a->rts = (uint8_t)(state != 0);
    a->dtr_sent = 1;
    if (a->loopback) a->uart.dev.ops->input(&a->uart.dev, pin == ZR_I8251_PIN_DTR ? ZR_I8251_IN_DSR : ZR_I8251_IN_CTS, state);
    else send_outputs(a);
}

static void uart_transmit(void *ctx, uint8_t byte) {
    amic *a = ctx;
    if (!a->loopback && a->has_ports && a->ports.transmit) a->ports.transmit(a->ports.ctx, 0, byte);
}

/* The host's side: a port host (or none: the bytes sent are lost), with the loopback plug where it asks for one. */
static void set_loopback(amic *a, int on) {
    a->loopback = (uint8_t)(on != 0);
    zr_i8251_set_loopback(&a->uart, on);
    if (on) {
        a->uart.dev.ops->input(&a->uart.dev, ZR_I8251_IN_DSR, a->dtr);
        a->uart.dev.ops->input(&a->uart.dev, ZR_I8251_IN_CTS, a->rts);
    } else {
        a->uart.dev.ops->input(&a->uart.dev, ZR_I8251_IN_DSR, a->dsr);
        a->uart.dev.ops->input(&a->uart.dev, ZR_I8251_IN_CTS, a->cts);
    }
}

static void amic_set_ports(void *self, const zr_port_host *host) {
    amic *a = self;
    a->has_ports = host != NULL;
    if (host) a->ports = *host;
    else memset(&a->ports, 0, sizeof a->ports);
    set_loopback(a, host && host->loopback && host->loopback(host->ctx, 0));
    a->dtr_sent = 1;
    send_outputs(a);
}

static size_t amic_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    amic *a = self;
    size_t taken = 0;
    if (port != 0) return count;
    while (taken < count && zr_i8251_receive(&a->uart, data[taken])) ++taken;
    return taken;
}

static void amic_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    amic *a = self;
    (void)dcd;
    (void)ri;
    if (port != 0) return;
    a->cts = (uint8_t)(cts != 0);
    a->dsr = (uint8_t)(dsr != 0);
    if (!a->loopback) {
        a->uart.dev.ops->input(&a->uart.dev, ZR_I8251_IN_CTS, a->cts);
        a->uart.dev.ops->input(&a->uart.dev, ZR_I8251_IN_DSR, a->dsr);
    }
}

/* --- the text on the screen ----------------------------------------------------------------------------------- */

/* The monitor draws a character into an 8 x 8 cell of the bitmap (32 rows of 30 columns, the cell's line 0 and 7 background): 6 lines
 * of its glyph from the ROM's table at 3E53h (6 bytes a glyph from 20h to 5Fh, a glyph's dots the high 6 bits of each byte), stored
 * complemented because a set bit is dark, and complemented again for the cursor and the inverse characters. The characters from 61h
 * are block graphics, drawn from the ROM's patterns of two halves: they read '?'. */
static const uint8_t FONT_SIGNATURE[12] = {0, 0, 0, 0, 0, 0, 0x10, 0x10, 0x10, 0x10, 0, 0x10};

static int32_t find_font(const uint8_t *rom) {
    for (uint32_t a = 0; a + 64u * 6u <= ZR_NATIVE_ROM; ++a)
        if (!memcmp(rom + a, FONT_SIGNATURE, sizeof FONT_SIGNATURE) && rom[a + 33u * 6u] == 0x10u && rom[a + 33u * 6u + 3u] == 0x7Cu) return (int32_t)a;
    return -1;
}

#define TEXT_FIRST_COLUMN 1u
#define TEXT_COLUMNS 30u
static size_t amic_text(void *self, char *out, size_t capacity) {
    amic *a = self;
    const uint8_t *font = a->font_at >= 0 ? a->n.rom + a->font_at : NULL;
    size_t n = 0;
    for (unsigned row = 0; row < 32u; ++row) {
        char line[TEXT_COLUMNS];
        unsigned last = 0;
        for (unsigned col = 0; col < TEXT_COLUMNS; ++col) {
            uint8_t cell[8];
            for (unsigned l = 0; l < 8u; ++l) cell[l] = a->n.ram[0x4000u + row * 256u + l * 32u + TEXT_FIRST_COLUMN + col];
            char ch = '?';
            for (unsigned g = 0; font && g < 64u; ++g) {
                unsigned same = 1, complement = 1;
                for (unsigned l = 0; l < 8u; ++l) {
                    const uint8_t want = l == 0u || l == 7u ? 0xFFu : (uint8_t)~font[g * 6u + l - 1u];
                    if (cell[l] != want) same = 0;
                    const uint8_t inverse = (uint8_t)(want ^ 0xFFu);
                    if (cell[l] != inverse) complement = 0;
                }
                if (same || complement) {
                    ch = (char)(0x20u + g);
                    break;
                }
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

/* --- life cycle ---------------------------------------------------------------------------------------------- */

/* Power on: the memory cleared, the processor as it wakes; RESET: the processor's RESET pin, which puts the 8255s and the 8251 back to
 * their start (every pin an input: port C floats high, so the picture is inverted until the monitor programs the chip) and clears the
 * CAPS LOCK latch. The keys held stay held. */
static void reset(void *self, int power_on) {
    amic *a = self;
    zr_native_reset(&a->n, power_on);
    a->ppi1.dev.ops->reset(&a->ppi1.dev);
    a->ppi2.dev.ops->reset(&a->ppi2.dev);
    a->uart.dev.ops->reset(&a->uart.dev);
    a->caps = 0;
    a->dtr = a->rts = 0;
    a->pc = 0;
    pc_sync(a, zr_native_now(&a->n));
    if (power_on) memset(a->rows, 0, sizeof a->rows);
}

static void *amic_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    amic *a = calloc(1, sizeof *a);
    if (!a) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    const unsigned memory = setup->options[OPTION_MEMORY] < 3u ? setup->options[OPTION_MEMORY] : 0u;
    static const uint32_t RAM_SIZE[3] = {0x8000u, 0x4000u, 0xC000u};
    static const unsigned RAM_KIB[3] = {32, 16, 48};
    a->baud = BAUDS[setup->options[OPTION_BAUD] < 3u ? setup->options[OPTION_BAUD] : 0u];
    a->n.tape_sound = 1;
    a->n.accelerate = 1;
    a->n.key_press_gap = KEY_PRESS_GAP;
    a->n.key_release_gap = KEY_RELEASE_GAP;
    a->n.key_apply = key_apply;
    a->n.line = amic_line;
    /* The tape input is port B of the keyboard 8255. */
    if (!zr_native_init(&a->n, setup, "amic", 0x4000u, 0, 0x00FFu, 0x0021u, error, capacity)) {
        free(a);
        return NULL;
    }
    a->font_at = find_font(a->n.rom);
    snprintf(a->n.description, sizeof a->n.description, "%s, %u KiB", setup->family->models[setup->model].name, RAM_KIB[memory]);
    zr_native *n = &a->n;
    /* The memory: the RAM from 4000h, as much as the board is fitted with, the rest reads FFh; the bitmap is its first 8 KiB. */
    zr_bus_map_ram(&n->bus, 0x4000u, RAM_SIZE[memory], n->ram + 0x4000u);
    zr_native_watch_video(n, 0x4000u, 0x2000u);
    /* The ports: A2-A7 and A1-A7 as the address decoders have them, the upper byte not decoded. */
    zr_bus_add_io(&n->bus, 0x00FEu, 0x0000u, uart_read, uart_write, a);
    zr_bus_add_io(&n->bus, 0x00FCu, 0x0020u, ppi1_read, ppi1_write, a);
    zr_bus_add_io(&n->bus, 0x00FCu, 0x0040u, ppi2_read, ppi2_write, a);
    /* The chips. */
    zr_i8255_init(&a->ppi1, &n->host, "8255 keyboard");
    zr_i8255_init(&a->ppi2, &n->host, "8255 external");
    /* The 8251's clock is 16 times the baud switch's setting. */
    zr_i8251_init(&a->uart, &n->host, "8251", ZR_NATIVE_CLOCK_HZ, 16u * a->baud, 16u * a->baud);
    zr_i8251_set_transmit(&a->uart, uart_transmit, a);
    zr_monovideo_set_colour(&n->video, zr_native_phosphor(setup->options[OPTION_MONITOR]));
    reset(a, 1);
    return a;
}

/* --- states -------------------------------------------------------------------------------------------------- */

static void amic_save(const void *self, zr_state_writer *w) {
    const amic *a = self;
    zr_sw_begin(w, "AMIC", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_bytes(w, a->rows, sizeof a->rows);
    zr_sw_u8(w, a->shift);
    zr_sw_u8(w, a->ctrl);
    zr_sw_u8(w, a->caps);
    zr_sw_u8(w, a->joy);
    zr_sw_u8(w, a->pc);
    zr_sw_u8(w, a->dtr);
    zr_sw_u8(w, a->rts);
    zr_sw_end(w);
    zr_native_save(&a->n, w);
    zr_sw_begin(w, "PPI ", 0);
    a->ppi1.dev.ops->save(&a->ppi1, w);
    zr_sw_end(w);
    zr_sw_begin(w, "PPI ", 1);
    a->ppi2.dev.ops->save(&a->ppi2, w);
    zr_sw_end(w);
    zr_sw_begin(w, "8251", 0);
    a->uart.dev.ops->save(&a->uart, w);
    zr_sw_end(w);
}

static int amic_load(void *self, zr_state_reader *r) {
    amic *a = self;
    if (!zr_native_load(&a->n, r)) return 0;
    if (!zr_sr_chunk(r, "AMIC", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_sr_bytes(r, a->rows, sizeof a->rows);
    a->shift = zr_sr_u8(r) & 1u;
    a->ctrl = zr_sr_u8(r) & 1u;
    a->caps = zr_sr_u8(r) & 1u;
    a->joy = zr_sr_u8(r);
    a->pc = zr_sr_u8(r);
    a->dtr = zr_sr_u8(r) & 1u;
    a->rts = zr_sr_u8(r) & 1u;
    if (!zr_sr_chunk(r, "PPI ", 0) || !a->ppi1.dev.ops->load(&a->ppi1, r)) return 0;
    if (!zr_sr_chunk(r, "PPI ", 1) || !a->ppi2.dev.ops->load(&a->ppi2, r)) return 0;
    if (!zr_sr_chunk(r, "8251", 0) || !a->uart.dev.ops->load(&a->uart, r)) return 0;
    a->dtr_sent = 1;
    send_outputs(a);
    return zr_sr_ok(r);
}

static const zr_machine_impl AMIC_IMPL = {
    .destroy = zr_native_destroy, .description = zr_native_description, .error = zr_native_error,
    .reset = reset, .nmi = zr_native_nmi,
    .clock = zr_native_clock, .run = zr_native_run, .time = zr_native_time, .frame_period = zr_native_frame_period,
    .frame = zr_native_frame, .text = amic_text, .audio_rate = zr_native_audio_rate, .audio_pull = zr_native_audio_pull,
    .key = zr_native_key, .joystick = amic_joystick,
    .insert = zr_native_insert, .media = zr_native_media, .eject = zr_native_eject, .activity = zr_native_activity, .control = zr_native_control, .tape_options = zr_native_tape_options,
    .set_ports = amic_set_ports, .port_receive = amic_port_receive, .port_modem = amic_port_modem,
    .save = amic_save, .load = amic_load, .rom_hashes = zr_native_rom_hashes,
    .set_trace = zr_native_set_trace, .peek = zr_native_peek, .poke = zr_native_poke, .tape_deck = zr_native_tape_deck,
};

const zr_family zr_amic_family = {
    .id = "amic", .name = "aMIC",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = 1,
    .ports = PORTS, .port_count = 1,
    .keys = KEY_TABLE, .key_count = KEYS,
    .typing = TYPING, .typing_count = sizeof TYPING / sizeof TYPING[0],
    .combos = COMBOS, .combo_count = sizeof COMBOS / sizeof COMBOS[0],
    .joysticks = 1,
    .impl = &AMIC_IMPL, .create = amic_create,
};
