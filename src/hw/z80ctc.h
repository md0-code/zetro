/* ZetRo - Zilog Z80 CTC counter/timer circuit (Z8430).
 *
 * Four channels, each a down-counter of a time constant 1..256 (0 means
 * 256) in one of two modes, programmed with a channel control word and a
 * time constant word:
 *  - timer: the system clock divided by a prescaler of 16 or 256 decrements
 *    the counter; it starts by itself when the time constant is loaded, or
 *    (control bit 3) on the active edge of CLK/TRG;
 *  - counter: each active edge of CLK/TRG decrements it.
 * At zero the counter reloads from the time constant register at once and
 * goes on, the ZC/TO output pulses (channels 0-2: channel 3 has none) and,
 * when the channel's interrupt is enabled, an interrupt is pending. The
 * down-counter can be read at any time. Written from Zilog's Z80 CTC
 * product specification and the Z80 CPU peripherals manual, and checked
 * against this project's rp_z80ctc.sv / cubz_ctc.sv for how the machines use
 * it.
 *
 * Bus: offset bits 1-0 select the channel (0-3), in whatever space the board
 * maps the chip into. A write is the control word (bit 0 set), the time
 * constant (when the last control word said one follows) or, to channel 0
 * only (the data sheet: "interrupt vectors are always addressed to channel
 * 0"), the interrupt vector (bit 0 clear; bits 7-3 are kept, the channel
 * number goes into bits 2-1 when the vector is read at an acknowledge). A
 * read returns the down-counter (256 reads as 0).
 *
 * Event driven: nothing runs per clock. The chip knows the time of each
 * system clock edge from the host's now(), keeps each channel at its last
 * visit and replays the clocks since on every access, so a count read is
 * exact at any `now`. The host deadline is the next zero count of a channel
 * that is observed (its interrupt enabled, or watched: below) and the end of
 * a ZC/TO pulse.
 *
 * The system clock is a ratio of the master clock: num / den master periods
 * per clock edge, exact (edge k at epoch + ceil(k * num / den), the epoch
 * the time the ratio was set; an edge at exactly `now` has already happened
 * when an access at `now` runs, so no clock is gained or lost however seldom
 * the board looks). num / den should be 1 or more. A turbo board sets the
 * ratio again (zr_ctc_set_clock).
 *
 * Pins (hw.h). The board drives CLK/TRG0-3 through ops->input (state the wire
 * level, 1 high; they start high). The chip drives ZC/TO0-2 through
 * host->line (state 1 = the pulse is high) and INT, which is the chip's
 * request flag (state 1 = asking for an interrupt: the INT pin low on the
 * wire), one call at every change; the board then asks the daisy chain
 * (z80daisy.h). A board wires a channel to another by itself: it sees ZC/TO
 * on host->line and answers with input on the other channel's CLK/TRG (or
 * sets a SIO's clock from zr_ctc_zc_period).
 *
 * Interrupts are the daisy interface of z80daisy.h (zr_ctc_daisy): the
 * channels are in priority order 0-3, a channel with its interrupt under
 * service blocks itself and the channels below it until its RETI, a higher
 * channel may interrupt a lower one's service.
 *
 * Settled where the data sheet leaves room: the timer's
 * prescaler starts at the first system clock edge after the time constant
 * is written (or after the trigger edge), so the zero count comes
 * prescaler x time constant edges later; a counter decrements at the instant
 * of the CLK/TRG edge, not at the next system clock; a ZC/TO pulse is high
 * from the zero count to the next system clock edge; a time constant
 * written while the channel runs is loaded at the next zero count (data
 * sheet); a software reset clears the channel's pending and under-service
 * flags (the RTL does); changing the CLK/TRG slope in a control word acts as
 * an active edge (data sheet).
 *
 * Save and load: the whole state, the clock and its phase, so a load
 * continues exactly; load makes no host call except arming the deadline, the
 * board reads the pins with zr_ctc_zcto and zr_ctc_irq.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_Z80CTC_H
#define ZR_Z80CTC_H

#include "hw.h"
#include "z80daisy.h"

/* Input pins (ops->input) and output pins (host->line). */
enum { ZR_CTC_TRG0, ZR_CTC_TRG1, ZR_CTC_TRG2, ZR_CTC_TRG3 };
enum { ZR_CTC_OUT_ZCTO0, ZR_CTC_OUT_ZCTO1, ZR_CTC_OUT_ZCTO2, ZR_CTC_OUT_INT };

/* Channel control word bits. */
enum { ZR_CTC_INT = 0x80, ZR_CTC_COUNTER = 0x40, ZR_CTC_P256 = 0x20, ZR_CTC_RISING = 0x10, ZR_CTC_TRIGGER = 0x08,
       ZR_CTC_TC_FOLLOWS = 0x04, ZR_CTC_RESET = 0x02, ZR_CTC_CONTROL = 0x01 };

typedef struct zr_ctc_channel {
    uint8_t control;            /* bits 7-3 of the last control word */
    uint8_t tc;                 /* the time constant register */
    uint8_t state;              /* 0 stopped, 1 timer waiting for its trigger, 2 running */
    uint8_t expect;             /* the next word is the time constant */
    uint8_t ip, ius;            /* interrupt pending, under service */
    uint8_t trg;                /* the CLK/TRG pin's level */
    uint8_t zc, zc_reported;    /* the ZC/TO pulse (a watched channel) and the level last sent */
    uint16_t count;             /* the down-counter, 1..256, as of `tick` */
    uint16_t pre;               /* timer: system clock edges since the last decrement */
    uint64_t tick;              /* the system clock edge count the state is valid at */
    uint64_t pulse_off;         /* the master time the ZC/TO pulse ends */
} zr_ctc_channel;

typedef struct zr_ctc {
    zr_dev dev;
    zr_daisy daisy;
    uint32_t clk_num, clk_den;  /* master periods per system clock edge = num / den */
    uint64_t epoch;             /* the master time of edge 0 */
    uint8_t vector;             /* the vector register (bits 7-3) */
    uint8_t watch;              /* channels whose ZC/TO pulses are scheduled and reported (bit n) */
    uint8_t int_reported;
    zr_ctc_channel ch[4];
    uint32_t trg_num[4], trg_den[4];    /* a periodic CLK/TRG (zr_ctc_set_trigger_clock); num 0: the pin */
    uint64_t trg_epoch[4];
} zr_ctc;

/* Sets the chip up on `host` with the system clock at clk_num / clk_den
 * master periods per edge, everything as after reset; no host call is made.
 * The board then calls dev.ops->reset. */
void zr_ctc_init(zr_ctc *t, const zr_hw_host *host, const char *name, uint32_t clk_num, uint32_t clk_den);
/* The system clock at num / den master periods per edge from now on. */
void zr_ctc_set_clock(zr_ctc *t, uint32_t num, uint32_t den);
/* CLK/TRG of `channel` as a periodic clock of num / den master periods per active edge from now on (edge k at the time of
 * the call + ceil(k * num / den)), for a board whose trigger input is a free-running oscillator; num 0 gives the pin back to
 * ops->input. A counter channel then counts those edges as a timer counts its prescaler's, without an event per edge, and
 * zr_ctc_zc_period gives its ZC/TO period; a timer waiting for its trigger is not started by it. */
void zr_ctc_set_trigger_clock(zr_ctc *t, unsigned channel, uint32_t num, uint32_t den);
/* Which channels' ZC/TO pulses the chip schedules and reports through
 * host->line (default 7: all three). A channel with its interrupt enabled is
 * scheduled anyway; one that is neither is only replayed when it is
 * accessed, which spares the host an event per zero count. */
void zr_ctc_watch(zr_ctc *t, unsigned mask);
/* The ZC/TO level of a channel (0-2) and the chip's interrupt request, as
 * of the last report. */
int zr_ctc_zcto(const zr_ctc *t, unsigned channel);
int zr_ctc_irq(const zr_ctc *t);
/* The period of a running timer's ZC/TO pulses (or a counter's on a periodic
 * CLK/TRG) as master periods num / den; 1 when the channel is one, else 0. */
int zr_ctc_zc_period(const zr_ctc *t, unsigned channel, uint64_t *num, uint64_t *den);

#endif /* ZR_Z80CTC_H */
