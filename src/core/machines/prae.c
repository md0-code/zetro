/* The PRAE-M (ITC Cluj-Napoca, 1983; machine_impl.h): the family "prae" (named "PRAE") with its two models, BASIC 3.6 and 3.5, a Z80
 * microcomputer of its own as this project's PRAE core documents it (MiSTerRO/docs/prae/README.md, hardware.md, validation.md, the
 * manual, whose figure 5 is the keyboard; core-prae/rtl/machine_core.sv with the shared rp_native_machine.sv, rp_native_keyboard.sv
 * and rp_mono_video.sv). The drawing the core calls a schematic is a logical reconstruction from the manual and the firmware, not a
 * factory drawing. Wired from the chips, not from the Spectrum core: the PRAE is not a Spectrum.
 *
 *   CPU      Z80 at 2.5 MHz from the 10 MHz master clock
 *   memory   16 KiB ROM (BASIC 3.6 or 3.5, the CPU-visible dumps whose EPROM data lines D3 and D4 were crossed) at 0000-3FFF; 48 KiB
 *            of RAM at 4000-FFFF; the bitmap is the last 8 KiB of it, E000-FFFF (the firmware finds the top of the RAM and keeps the
 *            screen above its BASIC, and draws from the bottom up)
 *   port     one, 80h (A0-A7 decoded, the upper address byte is the keyboard's): read D0-D4 the keyboard, D5 high, D6 the serial line
 *            RX, D7 the cassette input; write D6 the serial TX, D7 the cassette output and the loudspeaker (the latch starts at 40h:
 *            TX idle high). The keyboard is a matrix of 8 selected lines (address bits A8-A15, active low) by 5 return lines; a key
 *            pressed reads 0
 *   serial   the line is driven by the firmware bit by bit (no UART chip): it goes out as bytes at the rate the board is told
 *   interrupt  50 Hz, from the picture's frame (raster line 256), 8 CPU T-states long; the NMI is a key of the shell
 *   video    hw/monovideo: 256 x 256, a set bit is dark (black on white as the firmware draws), 640 x 312 dots at 10 MHz
 *   tape     a recorder on the cassette input and output, real time
 *
 * ZetRo's own code. */
#include "native.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define TX_LOG 32
#define RX_QUEUE 16
#define KEY_PRESS_GAP 800000u                   /* 80 ms: the firmware reads one key at a time (its beep and its wait for the key's release) and misses a key pressed meanwhile */
#define KEY_RELEASE_GAP 500000u                 /* 50 ms: a key stays down while the firmware reads it (its scan, with a delay of its own) */
#define KEY_CHORD_GAP 500000u                   /* 50 ms: the firmware must see SHIFT alone first (its scan finds the lowest row first, and SHIFT is the last) */

enum { OPTION_BAUD, OPTION_BITS, OPTION_MONITOR, OPTION_COUNT };
enum { MODEL_36, MODEL_35 };

/* --- the family's tables ---------------------------------------------------------------------------- */

static const char *const ROMS_36[] = {"prae36", NULL};
static const char *const ROMS_35[] = {"prae35", NULL};
/* The start is Enter at the console question, Enter at the welcome page and Enter at HIGHEST MEMORY?; each is lost while the ROM draws
 * the one before, so a few more follow (an empty line at BASIC's prompt only shows READY again). Then the command that loads the
 * tape, made from the tape's own first header (prae_autoload); when the tape has none that can be read, ALOAD "", which only lists
 * the names it finds: the ROM wants the name of the file. */
#define AUTOLOAD_START "\n\n\n\n\n\n\n\n\n\n\n"
#define AUTOLOAD AUTOLOAD_START "ALOAD \"\"\n"
#define AUTOLOAD_TEXT 64
static const zr_model_desc MODELS[] = {
    {"prae36", "PRAE-M BASIC 3.6", ROMS_36, AUTOLOAD},
    {"prae35", "PRAE-M BASIC 3.5", ROMS_35, AUTOLOAD},
};

static const char *const BAUD_WORDS[] = {"300", "600", "1200", "2400", "4800", "9600", "19200", NULL};
static const unsigned BAUDS[] = {300, 600, 1200, 2400, 4800, 9600, 19200};
static const char *const BITS_WORDS[] = {"7", "8", NULL};
static const char *const MONITOR_WORDS[] = {"white", "green", NULL};
static const int MONITOR_LABELS[] = {ZR_T_NAT_MONITOR_WHITE, ZR_T_NAT_MONITOR_GREEN};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    {"serial_baud", ZR_T_PRAE_BAUD, BAUD_WORDS, NULL, 0, ZR_MODELS_ALL, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_PORT, ZR_T_PRAE_BAUD_HINT + 1},
    {"serial_bits", ZR_T_PRAE_BITS, BITS_WORDS, NULL, 0, ZR_MODELS_ALL, ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_PORT, ZR_T_PRAE_BITS_HINT + 1},
    {"monitor", ZR_T_NAT_MONITOR, MONITOR_WORDS, MONITOR_LABELS, 0, ZR_MODELS_ALL, 0, ZR_T_NAT_MONITOR_HINT + 1},
};
static const zr_media_desc MEDIA[] = {{"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL}};
static const zr_port_desc PORTS[] = {{"serial", ZR_T_PRAE_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL}};

/* The keyboard (figure 5 of the manual, the matrix of the firmware's scan at 3275h and its table at 3634h): a key's code is its
 * selected line (the address bit A8 + row, active low) * 8 + its return line (the data bit). CTRL and SHIFT are keys of the matrix
 * (CTRL on row 4, SHIFT on row 7, both on return line 0). The shifted symbols are on the keycaps above the digits and letters. */
#define PRAE_KEYS(X) \
    X(1, "1", "1", "1", 0, 0, 4, 0x00) X(2, "2", "2", "2", 4, 0, 4, 0x08) X(3, "3", "3", "3", 8, 0, 4, 0x01) \
    X(4, "4", "4", "4", 12, 0, 4, 0x09) X(5, "5", "5", "5", 16, 0, 4, 0x02) X(6, "6", "6", "6", 20, 0, 4, 0x0A) \
    X(7, "7", "7", "7", 24, 0, 4, 0x03) X(8, "8", "8", "8", 28, 0, 4, 0x0B) X(9, "9", "9", "9", 32, 0, 4, 0x04) \
    X(0, "0", "0", "0", 36, 0, 4, 0x0C) \
    X(Q, "Q", "Q", "Q", 2, 4, 4, 0x10) X(W, "W", "W", "W", 6, 4, 4, 0x18) X(E, "E", "E", "E", 10, 4, 4, 0x11) \
    X(R, "R", "R", "R", 14, 4, 4, 0x19) X(T, "T", "T", "T", 18, 4, 4, 0x12) X(Y, "Y", "Y", "Y", 22, 4, 4, 0x1A) \
    X(U, "U", "U", "U", 26, 4, 4, 0x13) X(I, "I", "I", "I", 30, 4, 4, 0x1B) X(O, "O", "O", "O", 34, 4, 4, 0x14) \
    X(P, "P", "P", "P", 38, 4, 4, 0x1C) \
    X(CTRL, "CTRL", "Left Ctrl", "CTRL", 0, 8, 5, 0x20) X(A, "A", "A", "A", 5, 8, 4, 0x28) X(S, "S", "S", "S", 9, 8, 4, 0x21) \
    X(D, "D", "D", "D", 13, 8, 4, 0x29) X(F, "F", "F", "F", 17, 8, 4, 0x22) X(G, "G", "G", "G", 21, 8, 4, 0x2A) \
    X(H, "H", "H", "H", 25, 8, 4, 0x23) X(J, "J", "J", "J", 29, 8, 4, 0x2B) X(K, "K", "K", "K", 33, 8, 4, 0x24) \
    X(L, "L", "L", "L", 37, 8, 4, 0x2C) \
    X(SHIFT, "SHIFT", "Left Shift", "SHIFT", 0, 12, 6, 0x38) X(Z, "Z", "Z", "Z", 6, 12, 4, 0x30) X(X, "X", "X", "X", 10, 12, 4, 0x39) \
    X(C, "C", "C", "C", 14, 12, 4, 0x31) X(V, "V", "V", "V", 18, 12, 4, 0x3A) X(B, "B", "B", "B", 22, 12, 4, 0x32) \
    X(N, "N", "N", "N", 26, 12, 4, 0x3B) X(M, "M", "M", "M", 30, 12, 4, 0x33) X(SPACE, "SPACE", "Space", "SPACE", 34, 12, 4, 0x3C) \
    X(ENTER, "ENTER", "Return", "ENTER", 38, 12, 6, 0x34)

enum {
#define X(id, name, host, legend, x, y, w, code) KEY_##id,
    PRAE_KEYS(X)
#undef X
    KEYS
};
static const zr_key_desc KEY_TABLE[KEYS] = {
#define X(id, name, host, legend, x, y, w, code) {name, host, legend, x, y, w, 4},
    PRAE_KEYS(X)
#undef X
};
static const uint8_t MATRIX[KEYS] = {
#define X(id, name, host, legend, x, y, w, code) code,
    PRAE_KEYS(X)
#undef X
};

#define N_ ZR_KEY_NONE
#define SH KEY_SHIFT
/* What is typed for each character, from the firmware's table (3634h): a key alone gives its capital or digit, with SHIFT the symbol on
 * its keycap (the letters give the block graphics and a few control codes the firmware draws or obeys, not letters). Both cases of a
 * letter type the key alone, so a script written for the core's simulator, which presses the keys, types the same. Backspace is
 * CTRL and H (08h). */
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
    {' ', KEY_SPACE, N_}, {'\n', KEY_ENTER, N_}, {'\r', KEY_ENTER, N_}, {'\b', KEY_H, KEY_CTRL}, {'\x03', KEY_C, KEY_CTRL},
    /* with SHIFT: the symbols on the keycaps */
    {'!', KEY_1, SH}, {'^', KEY_2, SH}, {'#', KEY_3, SH}, {'$', KEY_4, SH}, {'%', KEY_5, SH}, {'&', KEY_6, SH}, {'*', KEY_7, SH},
    {'(', KEY_8, SH}, {')', KEY_9, SH}, {'\x7F', KEY_0, SH}, {'\x1B', KEY_E, SH}, {'@', KEY_T, SH}, {'-', KEY_Y, SH}, {'+', KEY_U, SH},
    {'=', KEY_I, SH}, {':', KEY_O, SH}, {';', KEY_P, SH}, {'\'', KEY_L, SH}, {'"', KEY_G, SH}, {',', KEY_K, SH}, {'<', KEY_H, SH},
    {'.', KEY_M, SH}, {'>', KEY_J, SH}, {'/', KEY_N, SH}, {'?', KEY_B, SH},
};

/* Host keys the matrix has no key for, as the core's keyboard (rp_native_keyboard.sv) maps the PC's: each punctuation key its own
 * character with SHIFT (with the host's Shift held the character a PC keyboard has above it where the PRAE has one), Backspace
 * CTRL and H, Delete SHIFT and 0, Escape SHIFT and E. */
static const zr_combo_desc COMBOS[] = {
    {"Right Shift", KEY_SHIFT, N_}, {"Right Ctrl", KEY_CTRL, N_}, {"Left Alt", KEY_CTRL, N_},
    {"Backspace", KEY_H, KEY_CTRL}, {"Delete", KEY_0, SH}, {"Escape", KEY_E, SH},
    {"'", KEY_L, SH}, {"Shift+'", KEY_G, SH}, {"=", KEY_I, SH}, {"Shift+=", KEY_U, SH}, {"-", KEY_Y, SH}, {"Shift+-", KEY_Y, SH},
    {",", KEY_K, SH}, {"Shift+,", KEY_H, SH}, {".", KEY_M, SH}, {"Shift+.", KEY_J, SH}, {"/", KEY_N, SH}, {"Shift+/", KEY_B, SH},
    {";", KEY_P, SH}, {"Shift+;", KEY_O, SH},
};

/* --- the board -------------------------------------------------------------------------------------------- */

typedef struct prae {
    zr_native n;
    unsigned baud, bits;
    uint8_t rows[8];                            /* the keys held, a bit for each return line of a selected line */
    uint8_t latch;                              /* the last write to port 80h */
    /* the serial line */
    zr_port_host ports;
    uint8_t has_ports, loopback;
    uint8_t tx_active;
    uint8_t tx_l[TX_LOG];
    uint64_t tx_t[TX_LOG];
    unsigned tx_n;
    zr_dev tx_dev, rx_dev;
    uint8_t rx_line;
    int rx_bit;                                 /* -1 idle, 0 the start bit, 1-bits the data bits, then the two stop bits */
    uint8_t rx_byte, rx_queue[RX_QUEUE];
    unsigned rx_count;
    /* the text read-back */
    int32_t font_at;
    char autoload[AUTOLOAD_TEXT];               /* the auto-load text made for the tape in the recorder (prae_autoload), not state */
} prae;

#define P(n) ((prae *)(n))

/* --- the keys ----------------------------------------------------------------------------------------------- */

static void key_apply(zr_native *n, unsigned key, int down) {
    prae *p = P(n);
    if (key >= KEYS) return;
    const uint8_t bit = (uint8_t)(1u << (MATRIX[key] & 7u));
    uint8_t *row = &p->rows[MATRIX[key] >> 3];
    *row = down ? (uint8_t)(*row | bit) : (uint8_t)(*row & ~bit);
}

/* --- the serial line ------------------------------------------------------------------------------------------ */

/* The firmware times every bit itself, so the line is a level over time: its falling edge from idle is a start bit, and the data bits
 * are the levels at the middle of each bit time (the logged edges kept until the first stop bit's middle, when the byte goes out). The
 * rate is the option's: the firmware's own is a variable of its RAM (300 baud, 7 data bits and 2 stop bits at the start, which is the
 * default here). */
static uint64_t bit_ticks(const prae *p) { return ZR_NATIVE_CLOCK_HZ / (p->baud ? p->baud : 300u); }

static void rx_start(prae *p, uint64_t now) {
    p->rx_byte = p->rx_queue[0];
    memmove(p->rx_queue, p->rx_queue + 1, --p->rx_count);
    p->rx_bit = 0;
    p->rx_line = 0;
    zr_dev_schedule(&p->rx_dev, now + bit_ticks(p));
}

/* The next bit of the byte on its way to the guest: the start bit, the data bits (the least significant first), two stop bits. */
static void rx_event(void *dev, uint64_t now) {
    prae *p = P(((zr_dev *)dev)->host->ctx);
    ++p->rx_bit;
    if ((unsigned)p->rx_bit <= p->bits) {
        p->rx_line = (uint8_t)(p->rx_byte >> (p->rx_bit - 1) & 1u);
        zr_dev_schedule(&p->rx_dev, now + bit_ticks(p));
    } else if ((unsigned)p->rx_bit <= p->bits + 2u) {
        p->rx_line = 1;
        zr_dev_schedule(&p->rx_dev, now + bit_ticks(p));
    } else if (p->rx_count) rx_start(p, now);
    else p->rx_bit = -1;
}

static void rx_put(prae *p, const uint8_t *data, size_t count, size_t *taken) {
    *taken = 0;
    while (*taken < count && p->rx_count < RX_QUEUE) p->rx_queue[p->rx_count++] = data[(*taken)++];
    if (p->rx_bit < 0 && p->rx_count) rx_start(p, zr_native_now(&p->n));
}

/* A change of the TX line at time t: a falling edge starts a frame, its edges are kept until the first stop bit's middle, when the
 * byte is taken from them. */
static void tx_change(prae *p, int level, uint64_t t) {
    if (!p->tx_active) {
        if (level) return;
        p->tx_active = 1;
        p->tx_n = 1;
        p->tx_t[0] = t;
        p->tx_l[0] = 0;
        zr_dev_schedule(&p->tx_dev, t + bit_ticks(p) * (2u * p->bits + 3u) / 2u);
    } else if (p->tx_n < TX_LOG) {
        p->tx_t[p->tx_n] = t;
        p->tx_l[p->tx_n++] = (uint8_t)level;
    }
}

static void tx_event(void *dev, uint64_t now) {
    prae *p = P(((zr_dev *)dev)->host->ctx);
    const uint64_t t0 = p->tx_t[0], bit = bit_ticks(p);
    unsigned value = 0;
    for (unsigned i = 1; i <= p->bits; ++i) {
        const uint64_t sample = t0 + bit * (2u * i + 1u) / 2u;
        unsigned level = 0;
        for (unsigned j = 0; j < p->tx_n && p->tx_t[j] <= sample; ++j) level = p->tx_l[j];
        value |= level << (i - 1u);
    }
    uint64_t later_t[TX_LOG];
    uint8_t later_l[TX_LOG];
    unsigned later = 0;
    for (unsigned j = 0; j < p->tx_n; ++j)
        if (p->tx_t[j] > now) {
            later_t[later] = p->tx_t[j];
            later_l[later++] = p->tx_l[j];
        }
    p->tx_active = 0;
    p->tx_n = 0;
    if (p->loopback) {
        const uint8_t byte = (uint8_t)value;
        size_t taken;
        rx_put(p, &byte, 1, &taken);
    } else if (p->has_ports && p->ports.transmit) p->ports.transmit(p->ports.ctx, 0, (uint8_t)value);
    for (unsigned j = 0; j < later; ++j) tx_change(p, later_l[j], later_t[j]);
}

static uint8_t dev_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void dev_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void dev_reset(void *dev) { (void)dev; }
static const zr_dev_ops TX_OPS = {.read = dev_read, .write = dev_write, .event = tx_event, .reset = dev_reset};
static const zr_dev_ops RX_OPS = {.read = dev_read, .write = dev_write, .event = rx_event, .reset = dev_reset};

/* --- the port ---------------------------------------------------------------------------------------------------- */

static uint8_t port_read(void *ctx, uint16_t port) {
    prae *p = ctx;
    uint8_t keys = 0x1Fu;                       /* the selected lines (address bits A8-A15 low) pull the keys they hold down to 0 */
    for (unsigned r = 0; r < 8u; ++r)
        if (!(port >> (8u + r) & 1u)) keys &= (uint8_t)~p->rows[r];
    return (uint8_t)((zr_native_ear(&p->n) ? 0x80u : 0u) | (p->rx_line ? 0x40u : 0u) | 0x20u | (keys & 0x1Fu));
}

static void port_write(void *ctx, uint16_t port, uint8_t value) {
    prae *p = ctx;
    (void)port;
    const uint64_t done = zr_native_at(&p->n, 4u);
    const uint8_t changed = (uint8_t)(value ^ p->latch);
    p->latch = value;
    if (changed & 0x40u) tx_change(p, value >> 6 & 1u, done);
    if (changed & 0x80u) {
        zr_beeper_set(&p->n.beeper, ZR_BEEPER_SPEAKER, value >> 7 & 1u, done);
        zr_native_mic(&p->n, value >> 7 & 1u, done);
    }
}

/* --- the serial port (the host's side) -------------------------------------------------------------------------- */

static void prae_set_ports(void *self, const zr_port_host *host) {
    prae *p = self;
    p->has_ports = host != NULL;
    if (host) p->ports = *host;
    else memset(&p->ports, 0, sizeof p->ports);
    p->loopback = host && host->loopback && host->loopback(host->ctx, 0) ? 1u : 0u;
}

static size_t prae_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    size_t taken = 0;
    if (port != 0) return count;
    rx_put(self, data, count, &taken);
    return taken;
}

static void prae_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    (void)self;
    (void)port;
    (void)dcd;
    (void)cts;
    (void)dsr;
    (void)ri;
}

/* --- the text on the screen ----------------------------------------------------------------------------------- */

/* The firmware draws a character into an 8 x 8 cell of the bitmap (32 rows of 32 byte columns, a row 100h bytes, the first at the foot
 * of the screen, the first two columns and the last unused): a glyph is 5 columns of 8 dots from the ROM's table at 34BDh (5 bytes a
 * glyph from 20h, the least significant bit of a byte the top line), the first column at bit 5 of the cell's byte; set dots are
 * dark, and the cursor and the inverse mode swap them. */
static const uint8_t FONT_SIGNATURE[15] = {0, 0, 0, 0, 0, 0, 0, 0xBE, 0, 0, 0, 0x0E, 0, 0x0E, 0};

static int32_t find_font(const uint8_t *rom) {
    for (uint32_t a = 0; a + 96u * 5u <= ZR_NATIVE_ROM; ++a)
        if (!memcmp(rom + a, FONT_SIGNATURE, sizeof FONT_SIGNATURE)) return (int32_t)a;
    return -1;
}

static size_t prae_text(void *self, char *out, size_t capacity) {
    prae *p = self;
    const uint8_t *font = p->font_at >= 0 ? p->n.rom + p->font_at : NULL;
    uint8_t glyph[96][8];
    for (unsigned g = 0; font && g < 96u; ++g)
        for (unsigned l = 0; l < 8u; ++l) {
            unsigned bits = 0;
            for (unsigned k = 0; k < 5u; ++k) bits |= (unsigned)(font[g * 5u + k] >> l & 1u) << (5u - k);
            glyph[g][l] = (uint8_t)bits;
        }
    size_t n = 0;
    for (unsigned row = 0; row < 32u; ++row) {
        char line[32];
        unsigned last = 0;
        for (unsigned col = 0; col < 32u; ++col) {
            uint8_t cell[8];
            for (unsigned l = 0; l < 8u; ++l) cell[l] = p->n.ram[0xE000u + row * 256u + l * 32u + col];
            char ch = '?';
            for (unsigned g = 0; font && g < 96u; ++g) {
                unsigned same = 1, complement = 1;
                for (unsigned l = 0; l < 8u; ++l) {
                    if (cell[l] != glyph[g][l]) same = 0;
                    const uint8_t inverse = (uint8_t)(glyph[g][l] ^ 0xFFu);
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

/* --- the auto-load command, from the tape's first header ----------------------------------------------------- */

/* A file on the tape is records of 134 bytes (the saver at 3A86h, the loader at 3B58h). A record is a leader of 80 pulses of one
 * width (the loader measures it), one pulse twice as long, then Manchester cells, each two halves of the leader's width, the first
 * half high for a 1 and low for a 0: 24 cells of 0 and one of 1, then the 134 bytes, the least significant bit first, the long pulse
 * being the junction of the leader's last cell and the first 0. Byte 0 of a record is its kind (0 the header, 1 a data record, FFh the
 * last), 1 a checksum that makes bytes 1 to 133 sum to 0, 2-3 the record's number, 4 the count of data bytes (80h), 6 to 133 the data. The
 * header's data is the name (16 characters, padded with spaces), the file's type (0 a BASIC program, saved by ASAVE and loaded by
 * ALOAD, 1 a memory image, saved by SAVE and loaded by LOAD), 55h and the length (two bytes): the address of an image is not on the tape,
 * LOAD takes it from the command. ALOAD finds a file by type and name together (3DC8h): another one is named on the screen and the ROM
 * looks on. A tape with the long pulse high (an inverted recording) is read as the loader reads it, the bits complemented. */
#define REC_SIZE 134u
#define REC_NAME 6u
#define REC_NAME_SIZE 16u
#define REC_TYPE 22u
#define REC_LENGTH 24u
#define LEADER_MIN 24u                          /* pulses of one width before the long one (the saver sends 80) */
#define SYNC_ZEROS_MIN 8u                       /* cells of 0 before the 1 (the saver sends 24) */
#define SYNC_ZEROS_MAX 40u
#define HALF_MIN 400u                           /* the leader's pulse in master clock ticks: 40 microseconds to a millisecond (the saver's is 171) */
#define HALF_MAX 10000u
#define SCAN_PULSES (1u << 20)                  /* how far into the tape the first header is looked for */
#define IMAGE_AT 0x6000u                        /* where an image is loaded: the manual's own example for a 48K machine, below the bitmap */
#define IMAGE_END 0xE000u
#define IMAGE_LOWEST 0x4400u                    /* above the firmware's variables and the start of BASIC's memory */

typedef struct tape_header {
    char name[REC_NAME_SIZE + 1];
    unsigned type, length;
} tape_header;

/* The tape read ahead as the machine hears it (media/tape.h): the pulses from the start, and the halves of cells made of them. */
typedef struct cell_reader {
    zr_tape_edges edges;
    uint64_t half;                              /* the leader's pulse width, ticks */
    unsigned budget;                            /* pulses left to look at */
    unsigned left;                              /* halves left of the pulse in hand */
    int level;                                  /* its level */
} cell_reader;

static int next_pulse(cell_reader *r, int *level, uint64_t *width) {
    if (!r->budget) return 0;
    --r->budget;
    return zr_tape_edges_next(&r->edges, level, width);
}

/* The end of a leader: pulses of one width (within 30% of their mean), then one about twice as long (1.5 to 2.5 times). */
static int find_sync(cell_reader *r, int *sync_level) {
    uint64_t run = 0, sum = 0, w;
    int level;
    while (next_pulse(r, &level, &w)) {
        const uint64_t mean = run ? sum / run : 0;
        if (run >= LEADER_MIN && mean >= HALF_MIN && mean <= HALF_MAX && w * 2 > mean * 3 && w * 2 < mean * 5) {
            r->half = mean;
            *sync_level = level;
            return 1;
        }
        if (run && w * 10 >= mean * 7 && w * 10 <= mean * 13) {
            ++run;
            sum += w;
        } else {
            run = 1;
            sum = w;
        }
    }
    return 0;
}

/* The level of the next half cell: a pulse of about the leader's width is one half, of about twice that two; -1 at the end of the tape
 * or for a pulse that is neither. */
static int next_half(cell_reader *r) {
    if (!r->left) {
        int level;
        uint64_t w;
        if (!next_pulse(r, &level, &w) || w * 2 < r->half || w * 2 >= r->half * 5) return -1;
        r->left = w * 2 < r->half * 3 ? 1u : 2u;
        r->level = level;
    }
    --r->left;
    return r->level;
}

/* The next cell's bit; -1 when its halves are not one high and one low. */
static int next_bit(cell_reader *r, int inverted) {
    const int a = next_half(r), b = next_half(r);
    if (a < 0 || b < 0 || a == b) return -1;
    return (a == 1) != (inverted != 0);
}

/* The record after a sync pulse of level `sync_level`: its second half is the first half of the first cell of 0. 0 when it is not one. */
static int read_record(cell_reader *r, int sync_level, uint8_t *rec) {
    unsigned zeros = 0;
    int bit;
    r->left = 1;
    r->level = sync_level;
    while ((bit = next_bit(r, sync_level)) == 0)
        if (++zeros > SYNC_ZEROS_MAX) return 0;
    if (bit < 0 || zeros < SYNC_ZEROS_MIN) return 0;
    unsigned sum = 0;
    for (unsigned i = 0; i < REC_SIZE; ++i) {
        unsigned value = 0;
        for (unsigned b = 0; b < 8u; ++b) {
            bit = next_bit(r, sync_level);
            if (bit < 0) return 0;
            value |= (unsigned)bit << b;
        }
        rec[i] = (uint8_t)value;
        if (i) sum += value;
    }
    return (sum & 0xFFu) == 0;
}

/* The first header of the tape, found in the pulses (the long ones the loader would take for a leader's end, a record that adds up). */
static int read_header(const zr_tape *tape, tape_header *h) {
    cell_reader r;
    uint8_t rec[REC_SIZE];
    int sync;
    memset(&r, 0, sizeof r);
    r.budget = SCAN_PULSES;
    zr_tape_edges_begin(&r.edges, tape, ZR_NATIVE_CLOCK_HZ, 1);
    while (find_sync(&r, &sync)) {
        if (!read_record(&r, sync, rec) || rec[0] != 0) continue;
        memcpy(h->name, rec + REC_NAME, REC_NAME_SIZE);
        h->name[REC_NAME_SIZE] = 0;
        h->type = rec[REC_TYPE];
        h->length = rec[REC_LENGTH] | (unsigned)rec[REC_LENGTH + 1] << 8;
        return 1;
    }
    return 0;
}

/* The text for the header: the start's Enters and ALOAD "NAME" for a program, LOAD "NAME" &6000 for an image (the address that ends it
 * at the bitmap when it is too long to end below it from 6000h). 0 for a type that is not one, or a name the keyboard cannot type as the
 * ROM compares it (capitals, digits and the symbols; the name's trailing spaces are the padding). */
static int compose(const prae *p, const tape_header *h, char *out, size_t capacity) {
    unsigned length = REC_NAME_SIZE;
    while (length && h->name[length - 1] == ' ') --length;
    for (unsigned i = 0; i < length; ++i) {
        const char c = h->name[i];
        uint8_t key, shift;
        if (c < ' ' || c > '~' || c == '"' || c == '\\' || (c >= 'a' && c <= 'z') || !zr_family_typing(p->n.family, c, &key, &shift)) return 0;
    }
    if (h->type == 0) return snprintf(out, capacity, AUTOLOAD_START "ALOAD \"%.*s\"\n", (int)length, h->name) < (int)capacity;
    if (h->type != 1) return 0;
    unsigned at = IMAGE_AT;
    if (h->length > IMAGE_END - at) at = h->length <= IMAGE_END - IMAGE_LOWEST ? IMAGE_END - h->length : at;
    return snprintf(out, capacity, AUTOLOAD_START "LOAD \"%.*s\" &%04X\n", (int)length, h->name, at) < (int)capacity;
}

/* What auto-load types for the tape in the recorder (zr_machine_autoload): the command for its first file, and always a power cycle
 * (BASIC starts from the console question). The model's own text when no header can be read. */
static const char *prae_autoload(const void *self, unsigned slot, int *power_cycle) {
    prae *p = (prae *)self;                     /* only the cache of the text is written */
    tape_header h;
    (void)slot;
    *power_cycle = 1;
    if (p->n.tape && read_header(p->n.tape, &h) && compose(p, &h, p->autoload, sizeof p->autoload)) return p->autoload;
    return p->n.family->models[p->n.model].autoload;
}

/* --- life cycle ---------------------------------------------------------------------------------------------- */

/* Power on: the memory cleared, the processor as it wakes; RESET: the processor's pin, which puts the port's latch back to 40h (TX
 * idle high, the cassette output low) and stops the serial line. The keys held stay held. */
static void reset(void *self, int power_on) {
    prae *p = self;
    zr_native_reset(&p->n, power_on);
    p->latch = 0x40u;
    p->tx_active = 0;
    p->tx_n = 0;
    zr_dev_schedule(&p->tx_dev, ZR_NEVER);
    zr_dev_schedule(&p->rx_dev, ZR_NEVER);
    p->rx_bit = -1;
    p->rx_line = 1;
    p->rx_count = 0;
    if (power_on) memset(p->rows, 0, sizeof p->rows);
}

static void *prae_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    prae *p = calloc(1, sizeof *p);
    if (!p) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    p->baud = BAUDS[setup->options[OPTION_BAUD] < sizeof BAUDS / sizeof BAUDS[0] ? setup->options[OPTION_BAUD] : 0u];
    p->bits = setup->options[OPTION_BITS] == 1u ? 8u : 7u;
    p->n.tape_sound = 1;
    p->n.accelerate = 1;
    p->n.key_press_gap = KEY_PRESS_GAP;
    p->n.key_release_gap = KEY_RELEASE_GAP;
    p->n.key_chord_gap = KEY_CHORD_GAP;
    p->n.key_apply = key_apply;
    p->rx_line = 1;
    p->rx_bit = -1;
    /* The tape input is bit 7 of port 80h. */
    if (!zr_native_init(&p->n, setup, setup->model == MODEL_35 ? "prae35" : "prae36", 0xE000u, 1, 0x00FFu, 0x0080u, error, capacity)) {
        free(p);
        return NULL;
    }
    p->font_at = find_font(p->n.rom);
    snprintf(p->n.description, sizeof p->n.description, "%s", setup->family->models[setup->model].name);
    zr_native *n = &p->n;
    /* 48 KiB of RAM at 4000h; the bitmap is the last 8 KiB. */
    zr_bus_map_ram(&n->bus, 0x4000u, 0xC000u, n->ram + 0x4000u);
    zr_native_watch_video(n, 0xE000u, 0x2000u);
    /* The one port, decoded on A0-A7 (the upper byte selects the keyboard's lines). */
    zr_bus_add_io(&n->bus, 0x00FFu, 0x0080u, port_read, port_write, p);
    p->tx_dev.ops = &TX_OPS;
    p->tx_dev.host = &n->host;
    p->tx_dev.name = "serial tx";
    p->rx_dev.ops = &RX_OPS;
    p->rx_dev.host = &n->host;
    p->rx_dev.name = "serial rx";
    zr_monovideo_set_colour(&n->video, zr_native_phosphor(setup->options[OPTION_MONITOR]));
    reset(p, 1);
    return p;
}

/* --- states -------------------------------------------------------------------------------------------------- */

static void prae_save(const void *self, zr_state_writer *w) {
    const prae *p = self;
    zr_sw_begin(w, "PRAE", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_bytes(w, p->rows, sizeof p->rows);
    zr_sw_u8(w, p->latch);
    zr_sw_u8(w, p->tx_active);
    zr_sw_u32(w, p->tx_n);
    for (unsigned i = 0; i < TX_LOG; ++i) {
        zr_sw_u64(w, p->tx_t[i]);
        zr_sw_u8(w, p->tx_l[i]);
    }
    zr_sw_u8(w, p->rx_line);
    zr_sw_u32(w, (uint32_t)p->rx_bit);
    zr_sw_u8(w, p->rx_byte);
    zr_sw_u32(w, p->rx_count);
    zr_sw_bytes(w, p->rx_queue, sizeof p->rx_queue);
    zr_sw_end(w);
    zr_native_save(&p->n, w);
}

static int prae_load(void *self, zr_state_reader *r) {
    prae *p = self;
    if (!zr_native_load(&p->n, r)) return 0;
    if (!zr_sr_chunk(r, "PRAE", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_sr_bytes(r, p->rows, sizeof p->rows);
    for (unsigned i = 0; i < 8u; ++i) p->rows[i] &= 0x1Fu;
    p->latch = zr_sr_u8(r);
    p->tx_active = zr_sr_u8(r) & 1u;
    p->tx_n = zr_sr_u32(r);
    if (p->tx_n > TX_LOG) return 0;
    for (unsigned i = 0; i < TX_LOG; ++i) {
        p->tx_t[i] = zr_sr_u64(r);
        p->tx_l[i] = zr_sr_u8(r) & 1u;
    }
    p->rx_line = zr_sr_u8(r) & 1u;
    p->rx_bit = (int)(int32_t)zr_sr_u32(r);
    p->rx_byte = zr_sr_u8(r);
    p->rx_count = zr_sr_u32(r);
    if (p->rx_count > RX_QUEUE || p->rx_bit < -1 || p->rx_bit > (int)p->bits + 2) return 0;
    zr_sr_bytes(r, p->rx_queue, sizeof p->rx_queue);
    zr_dev_schedule(&p->tx_dev, p->tx_active ? p->tx_t[0] + bit_ticks(p) * (2u * p->bits + 3u) / 2u : ZR_NEVER);
    zr_dev_schedule(&p->rx_dev, ZR_NEVER);
    if (p->rx_bit >= 0) zr_dev_schedule(&p->rx_dev, zr_native_now(&p->n) + bit_ticks(p));
    return zr_sr_ok(r);
}

static const zr_machine_impl PRAE_IMPL = {
    .destroy = zr_native_destroy, .description = zr_native_description, .error = zr_native_error,
    .reset = reset, .nmi = zr_native_nmi,
    .clock = zr_native_clock, .run = zr_native_run, .time = zr_native_time, .frame_period = zr_native_frame_period,
    .frame = zr_native_frame, .text = prae_text, .audio_rate = zr_native_audio_rate, .audio_pull = zr_native_audio_pull,
    .key = zr_native_key, .joystick = NULL,
    .insert = zr_native_insert, .media = zr_native_media, .eject = zr_native_eject, .activity = zr_native_activity, .control = zr_native_control, .tape_options = zr_native_tape_options,
    .set_ports = prae_set_ports, .port_receive = prae_port_receive, .port_modem = prae_port_modem,
    .save = prae_save, .load = prae_load, .rom_hashes = zr_native_rom_hashes,
    .set_trace = zr_native_set_trace, .peek = zr_native_peek, .poke = zr_native_poke, .tape_deck = zr_native_tape_deck,
    .autoload = prae_autoload,
};

const zr_family zr_prae_family = {
    .id = "prae", .name = "PRAE",
    .models = MODELS, .model_count = 2,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = 1,
    .ports = PORTS, .port_count = 1,
    .keys = KEY_TABLE, .key_count = KEYS,
    .typing = TYPING, .typing_count = sizeof TYPING / sizeof TYPING[0],
    .combos = COMBOS, .combo_count = sizeof COMBOS / sizeof COMBOS[0],
    .joysticks = 0,
    .impl = &PRAE_IMPL, .create = prae_create,
};
