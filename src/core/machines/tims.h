/* The TIM-S family's shared parts (tims.c): what the standard TIM-S and the TIM-S Plus (timsplus.c, the family's
 * second model) have in common: the family's tables, the CPU clock, the keyboard's added keys, the serial decoder.
 *
 * The CPU clock. The TIM-S has a 14 MHz crystal (the 3.5 MHz and 7 MHz clocks) and a 12 MHz one (6 MHz); the
 * core adds an 8 MHz setting to its "CPU switch" (3.5, 6, 7, 8 MHz: the speed of the auxiliary clock). One master
 * clock that every CPU speed divides is 168 MHz: a T-state is 48, 28, 24 or 21 ticks. The video (a pixel is 24
 * ticks, 7 MHz), the frame interrupt, the tape, the beeper and the AY chip are counted in that master clock and so
 * keep their rates whatever speed the CPU runs at; only the CPU's T-state changes (zr_tclock_set_rate, sched.h).
 * The board's logic says when the auxiliary clock is in use (the standard model: port C bit 3 high and the startup
 * ROM paged out); this file does the rest.
 *
 * During every I/O cycle the board puts the CPU back on the 3.5 MHz clock (the manual, "Comutarea tactului": the
 * clock is switched for each I/O operation, to use slow programmable I/O circuits): at the auxiliary clock an IN or
 * OUT cycle lasts as long as one of the 3.5 MHz clock, to a whole T-state of the clock in use.
 *
 * The bit-banged serial line. The standard model's firmware sends its serial bytes by writing bit 0 of port C itself
 * (seven data bits, least significant first, after a start bit, then two stop bits: the "A" channels). tims_uart
 * turns the line into bytes the way a receiver at the firmware's rate would: a falling edge starts a byte (a pulse
 * shorter than half a bit is not a start), the bits are sampled in their middles, a byte whose first stop bit is low is
 * dropped.
 *
 * ZetRo's own code, written from the TIM-S manual and schematic and this project's TIM-S core (MiSTerRO/docs/tims,
 * core-tims). */
#ifndef ZR_TIMS_H
#define ZR_TIMS_H

#include "spectrum.h"

#define TIMS_CLOCK_HZ 168000000u
#define TIMS_TICKS_3_5 48u                  /* ticks of the master clock in a 3.5 MHz T-state */

/* --- the family's models and tables (tims.c), which the TIM-S Plus (timsplus.c) shares -------------------- */

enum { TIMS_MODEL_STANDARD, TIMS_MODEL_PLUS };
#define TIMS_STANDARD (1u << TIMS_MODEL_STANDARD)   /* `models` masks */
#define TIMS_PLUS (1u << TIMS_MODEL_PLUS)

/* The options, in the family's table order (the standard model's first, as it had them). */
enum {
    TIMS_OPTION_JOYSTICK, TIMS_OPTION_JOYSTICK2, TIMS_OPTION_CPU, TIMS_OPTION_AY,
    TIMS_OPTION_TIMING,
    TIMS_OPTION_PLUS_AY, TIMS_OPTION_UART, TIMS_OPTION_PRINTER, TIMS_OPTION_DRIVE_SOUND, TIMS_OPTION_COUNT
};
/* The TIM-S Plus's AY wiring, UART channel and printer, in the order of the options' values. */
enum { TIMS_AY_STEREO, TIMS_AY_MONO };
enum { TIMS_UART_NATIVE, TIMS_UART_EXP1, TIMS_UART_EXP2, TIMS_UART_EXP3, TIMS_UART_EXP4, TIMS_UARTS };
enum { TIMS_PRINTER_VIRTUAL, TIMS_PRINTER_PHYSICAL };
/* The media slots and the byte ports. */
enum { TIMS_SLOT_TAPE, TIMS_SLOT_A, TIMS_SLOT_B, TIMS_SLOT_COUNT };
enum { TIMS_PORT_SERIAL, TIMS_PORT_PRINTER, TIMS_PORT_COUNT };

/* The keys: the Spectrum's 40 (spectrum.h's numbering), then the TIM-S Plus's eleven of its two added matrix lines, which
 * port FE reads on D5 (the first five, by the address lines A8-A12) and D7 (the other six, A8-A13). Names after the
 * keys the core maps to them where the documents give none (the four function keys). */
enum {
    TIMS_KEY_CTRL = ZR_SPECTRUM_KEYS, TIMS_KEY_TAB, TIMS_KEY_CAPS_LOCK, TIMS_KEY_F4, TIMS_KEY_F2,
    TIMS_KEY_ESC, TIMS_KEY_DEL, TIMS_KEY_SCROLL, TIMS_KEY_BACKSPACE, TIMS_KEY_F1, TIMS_KEY_F3, TIMS_KEYS
};
#define TIMS_EXTRA_KEYS (TIMS_KEYS - ZR_SPECTRUM_KEYS)
#define TIMS_EXTRA_D5 5u                    /* the first line's keys: CTRL to F2 */
/* The bits 0-4 the extra keys held (bit n: key ZR_SPECTRUM_KEYS + n) pull low on the Spectrum's matrix for the rows `high`
 * selects, as the core's keyboard gives them on a Spectrum-mode machine: CTRL is SYMBOL SHIFT, ESC is BREAK (CAPS SHIFT and
 * SPACE), BACKSPACE (and DEL on the Plus) CAPS SHIFT and 0; CAPS LOCK is CAPS SHIFT and 2 (ZetRo's, as on the other
 * Spectrum-compatible machines). */
uint8_t tims_extra_matrix(unsigned held, uint8_t high, int plus);
/* What the two added lines read for the rows `high` selects: bit 5 (D5) and bit 7 (D7), low for a key held. */
uint8_t tims_extra_lines(unsigned held, uint8_t high);

/* The TIM-S Plus (timsplus.c): its board, built by the family's create for the model, and its impl. */
void *tims_plus_create(const struct zr_machine_setup *setup, char *error, size_t capacity);
extern const zr_machine_impl tims_plus_impl;

/* The CPU switch's settings, in the order of the option's values. */
enum { TIMS_CPU_3_5, TIMS_CPU_6, TIMS_CPU_7, TIMS_CPU_8, TIMS_CPU_KINDS };
extern const char *const tims_cpu_words[TIMS_CPU_KINDS + 1];
extern const int tims_cpu_labels[TIMS_CPU_KINDS];
/* The option row of the CPU switch (a change builds the machine again, as the core's does). */
#define TIMS_OPTION_CPU(models) \
    {"cpu_switch", ZR_T_TIMS_CPU_SWITCH, tims_cpu_words, tims_cpu_labels, 0, models, ZR_OPTION_RESET, ZR_T_TIMS_CPU_HINT + 1}

/* Ticks of the master clock in one T-state at a CPU switch setting. */
uint32_t tims_cpu_ticks(unsigned cpu);
/* Puts the CPU on the clock of `ticks` ticks a T-state from the T-state it is at now (a no-op when it already is). */
void tims_set_cpu_clock(zr_spectrum *s, uint32_t ticks);
/* The T-states an IN or OUT cycle is lengthened by while the CPU runs on the auxiliary clock of setting `cpu`. */
unsigned tims_io_extra(unsigned cpu);

/* --- the serial line's UART ------------------------------------------------------------------------------ */

#define TIMS_UART_BITS 7u                   /* data bits of a byte */

typedef struct tims_uart {
    zr_dev dev;                             /* the deadline of the next sample */
    uint32_t bit_ticks;                     /* master-clock ticks in a bit */
    void (*byte_out)(void *ctx, uint8_t byte);
    void *ctx;
    /* state (saved) */
    uint8_t line;                           /* the line's level now (1: mark) */
    uint8_t busy, count, byte;
    uint64_t at;                            /* the next sample */
} tims_uart;

/* The decoder on `host` (its deadline goes through host->schedule); `byte_out` gets each byte received. */
void tims_uart_init(tims_uart *u, const zr_hw_host *host, uint32_t bit_ticks, void (*byte_out)(void *ctx, uint8_t byte), void *ctx);
/* The line idle (mark) and nothing in progress. */
void tims_uart_reset(tims_uart *u);
/* The line is `level` from guest time `now`. */
void tims_uart_line(tims_uart *u, int level, uint64_t now);
void tims_uart_save(const tims_uart *u, zr_state_writer *w);
/* 0 when the fields are not a decoder's; arms the deadline again. */
int tims_uart_load(tims_uart *u, zr_state_reader *r);

#endif /* ZR_TIMS_H */
