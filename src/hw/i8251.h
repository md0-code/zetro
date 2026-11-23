/* ZetRo - the Intel 8251A USART.
 *
 * Written from Intel's 8251A data sheet: the mode instruction after reset
 * (asynchronous x1, x16, x64 with 5-8 data bits, parity and 1, 1.5 or 2 stop
 * bits; synchronous with one or two sync characters, internal or external
 * sync detect), the command instruction (TxEN, DTR, RxE, SBRK, error reset,
 * RTS, internal reset, enter hunt), the status register (TxRDY, RxRDY,
 * TxEMPTY, PE, OE, FE, SYNDET/BRKDET, DSR) and the pins.
 *
 * The chip works in whole characters and in real time. A character that is
 * written to the transmitter goes out over its character time (start bit,
 * data bits, parity, stop bits, at the TxC rate and the x1/x16/x64 factor)
 * and is handed to the transmit callback when its last bit has gone out;
 * the next one follows at once when the buffer holds it. A character offered
 * to the receiver with zr_i8251_receive becomes readable after its character
 * time at the RxC rate. A character starts on the first edge of its clock
 * (TxC, RxC) at or after the moment it is loaded or offered; the clocks are
 * ratios of the master clock, exact, so a stream of characters never drifts.
 * Nothing runs per bit: the chip has at most one deadline, the end of the
 * character in the transmitter or the receiver, whichever is first.
 *
 * Clocks. TxC and RxC are given at init in Hz (the master clock's rate is
 * given too) or set later, as hertz or as a ratio of master-clock periods per
 * clock edge, num / den (exact); a clock set to 0 is stopped and nothing is
 * sent or received until it runs. A character in progress when a clock
 * changes finishes after the clock periods it still had left, at the new
 * rate (the aMIC switches 1200, 600 and 300 baud this way).
 *
 * Pins (hw.h). Outputs reach the board through host->line, state 1
 * asserted: TxRDY, RxRDY, TxEMPTY, SYNDET/BRKDET are the active-high pins
 * (1 high); DTR and RTS are the active-low pins (1 low, asserted). Inputs
 * come through ops->input: CTS and DSR (1 asserted: the pin is low) and the
 * SYNDET pin when it is an input (external sync, 1 high). After init CTS is
 * asserted (the pin is tied low on the machines that do not wire it) and DSR
 * is not; the board changes them with input. TxRDY pin = buffer empty and
 * TxEN and CTS; the status bit is the buffer empty alone (data sheet).
 * TxD is reported as a level only for the idle and break state: 1 mark, 0
 * while SBRK holds it low (asynchronous mode); the data bits are not
 * pins, the characters go to the transmit callback.
 *
 * Bus: offset bit 0 is C/D: 0 the data register, 1 the control (write: mode,
 * sync characters, command in that order after a reset; read: status).
 *
 * The transmit callback receives the data characters the program wrote
 * (masked to the character length), not the sync fillers. With the loopback
 * on, TxD is wired to RxD inside the chip: every character that has gone
 * out, fillers included, is received by the chip's own receiver (so the
 * receiver follows the transmitter's clock and the character it received
 * is readable when its last bit has gone out), and zr_i8251_receive refuses
 * everything. The loopback survives a reset: it is a plug, not a state.
 *
 * Save and load: the whole state including the clocks' phase and the
 * character in progress; load makes no host call except arming the
 * deadline. The transmit callback is the board's and is not saved.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_I8251_H
#define ZR_I8251_H

#include "hw.h"

/* Output pins (host->line). */
enum { ZR_I8251_PIN_TXRDY, ZR_I8251_PIN_RXRDY, ZR_I8251_PIN_TXEMPTY, ZR_I8251_PIN_DTR, ZR_I8251_PIN_RTS,
       ZR_I8251_PIN_SYNDET, ZR_I8251_PIN_TXD };
/* Input pins (ops->input). */
enum { ZR_I8251_IN_CTS, ZR_I8251_IN_DSR, ZR_I8251_IN_SYNDET };
/* The two clocks. */
enum { ZR_I8251_TXC, ZR_I8251_RXC };
/* What a received character carries besides its data (zr_i8251_receive_flags);
 * the values are the status register's bits. */
enum { ZR_I8251_ERR_PE = 0x08, ZR_I8251_ERR_FE = 0x20, ZR_I8251_ERR_BREAK = 0x40 };

typedef void (*zr_i8251_transmit_fn)(void *ctx, uint8_t byte);

typedef struct zr_i8251_clock {
    uint32_t num, den;          /* master periods per clock edge = num / den; num 0: stopped */
    uint64_t epoch;             /* the master time of edge 0 */
} zr_i8251_clock;

typedef struct zr_i8251 {
    zr_dev dev;
    uint32_t master_hz;
    zr_i8251_clock clk[2];      /* ZR_I8251_TXC, ZR_I8251_RXC */
    zr_i8251_transmit_fn transmit;
    void *transmit_ctx;
    uint8_t mode, command;
    uint8_t step;               /* what the next control write is: 0 mode, 1 sync 1, 2 sync 2, 3 command */
    uint8_t sync[2];
    uint8_t loopback;
    uint8_t cts, dsr, syndet_in;    /* input pins, 1 asserted (SYNDET: 1 high) */
    uint8_t tx_buf, tx_full;    /* the transmit buffer */
    uint8_t tx_busy;            /* a character is in the shifter (until tx_end) */
    uint8_t tx_data;            /* ... a data character (0: a sync filler) */
    uint8_t tx_byte;            /* ... and what it is */
    uint8_t tx_drain;           /* the buffered character was written before the transmitter was disabled: it still goes */
    uint8_t tx_primed;          /* synchronous: a character has been sent, fillers follow when none is waiting */
    uint8_t tx_sync_next;       /* synchronous: the second sync character of a filler pair is owed */
    uint8_t rx_data, rx_full;   /* the receive data register */
    uint8_t rx_busy;            /* a character is being received (until rx_end) */
    uint8_t rx_byte, rx_flags;  /* ... and what it is */
    uint8_t pe, oe, fe, brk, syndet, hunting, hunt_match;
    uint8_t reported;           /* output pins last sent through host->line, by pin number */
    uint64_t tx_end, rx_end;    /* the clock edge count the character in progress ends at (TxC, RxC) */
} zr_i8251;

/* Sets the chip up on `host` as after reset, the master clock at `master_hz`
 * and TxC and RxC at txc_hz and rxc_hz (0: stopped); no host call is made.
 * The board then calls dev.ops->reset. */
void zr_i8251_init(zr_i8251 *u, const zr_hw_host *host, const char *name, uint32_t master_hz, uint32_t txc_hz, uint32_t rxc_hz);
/* The callback a transmitted character is handed to (NULL: dropped). */
void zr_i8251_set_transmit(zr_i8251 *u, zr_i8251_transmit_fn fn, void *ctx);
/* A clock (ZR_I8251_TXC or ZR_I8251_RXC) at `hz`, or at num / den master
 * periods per edge; 0 stops it. */
void zr_i8251_set_clock_hz(zr_i8251 *u, int which, uint32_t hz);
void zr_i8251_set_clock(zr_i8251 *u, int which, uint32_t num, uint32_t den);
/* TxD wired to RxD (1) or not (0). */
void zr_i8251_set_loopback(zr_i8251 *u, int on);
/* Offers a character to the receiver. Returns 1 when it is taken (it becomes
 * readable after its character time) and 0 when it is not: the receiver is
 * still busy with the previous character, is not enabled (RxE) or is
 * looped back; the board offers it again later or drops it. */
int zr_i8251_receive(zr_i8251 *u, uint8_t byte);
/* The same with what the line did to it (ZR_I8251_ERR_*): a parity error, a
 * framing error, or a break (the receiver sees the line low through a whole
 * character: the data is 0, framing error and BRKDET are set; BRKDET falls
 * again with the next character that is not a break). */
int zr_i8251_receive_flags(zr_i8251 *u, uint8_t byte, unsigned flags);
/* The level of an output pin (ZR_I8251_PIN_*), 1 asserted. */
int zr_i8251_out(const zr_i8251 *u, int pin);

#endif /* ZR_I8251_H */
