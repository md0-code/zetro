/* ZetRo - Intel 8253 programmable interval timer.
 *
 * Three independent 16-bit counters, modes 0-5 (interrupt on terminal
 * count, programmable one-shot, rate generator, square wave, software and
 * hardware triggered strobe), binary or BCD, LSB / MSB / LSB-then-MSB
 * access, counter latch, a GATE input and an OUT output each. Written from
 * the 8253 data sheet; the 8254's read-back command is not part of the
 * 8253, a control word with SC = 11 does nothing.
 *
 * Event driven: nothing runs per clock. The chip knows the time of every
 * clock edge from the host's now(), keeps each counter's state at its last
 * visit and, on every access, replays the clocks since, so a count read is
 * exact at any `now`. Its one host deadline is the next OUT transition (or
 * load) of the counters that are watched.
 *
 * Clocks. Each counter has its own clock, one of two kinds:
 *  - a rate, a ratio of master-clock periods per count clock, num / den,
 *    exact (the TIM-S Plus clocks its counters at 2 MHz and about 109 kHz
 *    from a 14 MHz master: 7/1 and 128/1). Edge k of a rate is at the master
 *    time epoch + ceil(k * num / den), where the epoch is the time the rate
 *    was set (zr_i8253_set_clock) or the chip initialised; an edge at
 *    exactly `now` has already happened when an access at `now` runs, so no
 *    clock is ever gained or lost however seldom the board looks;
 *  - num = 0: the board drives the counter's CLK pin (ZR_I8253_CLK0..2 on
 *    ops->input, state the wire level) and the counter counts each falling
 *    edge, the way one counter's OUT feeds another's clock (the board
 *    wires it: it sees OUT on host->line and answers with input).
 *
 * Pins (hw.h). The board drives GATE0-GATE2 and CLK0-CLK2 through
 * ops->input (state = the wire level, 1 high); the gates start high (enabled).
 * The chip drives OUT0-OUT2 through host->line (state 1 = high). OUT starts
 * low (mode 0 after power on) and only changes are reported.
 *
 * Bus cycles: offset bits 1-0: 0-2 the counters, 3 the control word (write
 * only: a read returns 0xFF), in whatever space the board maps the chip into.
 *
 * Details settled where the data sheet leaves room: a count written to a
 * counter is loaded on the first clock edge after the write (a read in
 * between still returns the old count; OUT of mode 0 goes high N + 1 clocks
 * after a count of N is written); in mode 3 the count falls by two per clock
 * (the data sheet's odd-count rule included) so reads show it; a count of 1
 * in modes 2 and 3 is illegal and leaves OUT high; invalid BCD digits in a
 * loaded count are taken as their binary value, modulo 10000.
 *
 * Save and load: the whole state of the three counters, each one's clock
 * and its phase (the epoch and the edge count), so a load continues exactly.
 * load makes no host call except arming the deadline; the board reads the
 * OUT levels with zr_i8253_out.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_I8253_H
#define ZR_I8253_H

#include "hw.h"

/* Input pins (ops->input) and output pins (host->line). */
enum { ZR_I8253_GATE0, ZR_I8253_GATE1, ZR_I8253_GATE2, ZR_I8253_CLK0, ZR_I8253_CLK1, ZR_I8253_CLK2 };
enum { ZR_I8253_OUT0, ZR_I8253_OUT1, ZR_I8253_OUT2 };

typedef struct zr_i8253_counter {
    uint8_t mode, bcd, rw;      /* rw: 1 LSB, 2 MSB, 3 LSB then MSB */
    uint8_t gate, out, out_reported;
    uint8_t loaded;             /* the counting element holds a loaded count */
    uint8_t pending;            /* the count register loads on the next clock edge */
    uint8_t armed;              /* modes 0, 1, 4, 5: the terminal count has not come yet */
    uint8_t strobe;             /* modes 4, 5: OUT is low for the strobe's clock */
    uint8_t phase;              /* mode 3: 0 the high half, 1 the low half */
    uint8_t write_hi, read_hi;  /* the byte flip-flops */
    uint8_t latched;            /* a latched count is waiting to be read */
    uint8_t wbyte;              /* the LSB waiting for its MSB */
    uint8_t clk_level;          /* the CLK pin's level (a counter clocked from its pin) */
    uint16_t cr, latch;         /* count register, latched count (as written / read: BCD digits in BCD) */
    uint32_t nload;             /* modes 2, 3: the count of the cycle running, 1..modulus */
    uint32_t ce;                /* the counting element, 0..modulus-1 (0 stands for the modulus) */
    uint32_t k;                 /* mode 3: clock edges into the current half */
    uint32_t clk_num, clk_den;  /* master periods per clock edge = num / den; num 0: the CLK pin clocks it */
    uint64_t epoch;             /* the master time of edge 0 of the rate */
    uint64_t tick;              /* the clock edge count the state is valid at */
    uint64_t edges;             /* a counter clocked from its pin: the falling edges seen so far */
} zr_i8253_counter;

typedef struct zr_i8253 {
    zr_dev dev;
    uint8_t watch;              /* counters whose OUT changes are scheduled and reported (bit n) */
    zr_i8253_counter c[3];
} zr_i8253;

/* Sets the chip up on `host` with every counter's clock at `clk_num / clk_den`
 * master periods per edge (see set_clock) and everything as after reset; no
 * host call is made. The board then calls dev.ops->reset. */
void zr_i8253_init(zr_i8253 *t, const zr_hw_host *host, const char *name, uint32_t clk_num, uint32_t clk_den);
/* Counter `counter` (0-2) clocked at num / den master periods per edge from
 * now on (its edges restart at the current time); num 0: by the CLK pin. */
void zr_i8253_set_clock(zr_i8253 *t, unsigned counter, uint32_t num, uint32_t den);
/* Which counters' OUT changes the chip schedules and reports through
 * host->line (default 7: all three). An unwatched counter is only replayed
 * when it is accessed, which spares the host an event per OUT edge on a
 * line nobody listens to; the board reads its level with zr_i8253_out. A
 * counter added to the mask has its OUT level reported at once if it
 * differs from the last report. */
void zr_i8253_watch(zr_i8253 *t, unsigned mask);
/* OUT of a counter (0-2) at the host's now(). */
int zr_i8253_out(zr_i8253 *t, unsigned counter);

#endif /* ZR_I8253_H */
