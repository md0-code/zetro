/* ZetRo - Zilog Z80-SIO dual serial channel (Z8440).
 * Written from the Z80-SIO data sheet for ZetRo's chip interface (byte-wide cycles, state save and load, the
 * interrupts on the daisy chain, clocks as ratios of a master clock whose
 * rate is given at init). Checked against this project's cubz_sio.sv for how
 * the CUB Z uses it.
 *
 * What is modelled: both channels in the asynchronous modes, in full. Write
 * registers 0-7 are accepted, 0-5 act (WR0 pointer and the commands null,
 * reset external/status interrupts, channel reset, enable interrupt on next
 * receive character, reset Tx interrupt pending, error reset, return from
 * interrupt; WR1 external/Tx/Rx interrupt enables, status affects vector and
 * the three receive interrupt modes; WR2 the vector; WR3 receiver enable,
 * auto enables and 5/6/7/8 bits; WR4 parity, stop bits and the x1/x16/x32/x64
 * clock; WR5 DTR, RTS, send break, Tx enable and 5/6/7/8 bits). Read
 * registers 0-2. The interrupt logic with its internal priority chain
 * (receive A, transmit A, external A, receive B, transmit B, external B),
 * the interrupt-under-service state, vectors with status-affects-vector,
 * the 3-character receive FIFO with parity, framing and overrun, the
 * transmit buffer and shift register, break send and detect, DCD, CTS and
 * SYNC as inputs with their external/status latching, auto enables, RTS
 * (held until the transmitter is empty) and DTR.
 *
 * Not modelled: the synchronous modes (monosync, bisync, SDLC; the CUB Z and
 * the other machines of this project do not use them): their register writes
 * are accepted and stored, but WR4 bits 5-4 do not change the character
 * engine (a channel whose WR4 stop-bit field is 00 still runs
 * asynchronously with one stop bit, as the Junior's RTL does), the CRC
 * generator and checker, hunt phase, residue codes (RR1 bits 3-1 read 0),
 * end of frame (RR1 bit 7), Tx underrun/EOM (RR0 bit 6), send abort,
 * WAIT/READY.
 *
 * Interface to the board
 *   Bus (ops->read, ops->write; offset bits: 0 = control (1) or data (0),
 *   1 = channel B (1) or A (0)): 0 A data, 1 A control, 2 B data, 3 B
 *   control, in whatever space the board maps the chip into. A control read
 *   of a register other than RR0 follows a pointer write to WR0 and the
 *   pointer returns to 0 after every control access. RR2 reads the vector
 *   from either channel's control port.
 *
 *   Output pins (host->line, state 1 asserted):
 *     ZR_SIO_OUT_INT      the chip's interrupt request flag (the INT pin
 *                         low on the wire); the board then asks the daisy
 *                         chain (z80daisy.h)
 *     ZR_SIO_OUT_RTS(ch)  RTSA/RTSB
 *     ZR_SIO_OUT_DTR(ch)  DTRA/DTRB
 *   Input pins (ops->input, state 1 asserted, that is the pin low on the wire):
 *     ZR_SIO_IN_CTS(ch), ZR_SIO_IN_DCD(ch), ZR_SIO_IN_SYNC(ch)
 *   They start deasserted. Each is a status bit of RR0 (CTS 5, SYNC 4, DCD 3)
 *   whose change is an external/status interrupt, and CTS and DCD gate the
 *   transmitter and receiver while WR3 bit 5 (auto enables) is set.
 *
 *   Interrupts are the daisy interface of z80daisy.h (zr_sio.daisy): the
 *   acknowledge returns the vector of the highest-priority source that is
 *   pending and marks it under service; the RETI of the bus (zr_daisy_reti)
 *   and the WR0 command (either channel's: the data sheet says channel A,
 *   the Junior's RTL accepts both) end the highest source under service.
 *   zr_sio_vector gives the vector without the marking.
 *
 *   Clocks: the receive and transmit clock inputs are not edge-by-edge pins
 *   but periodic signals described by a ratio of the master clock
 *   (zr_sio_set_clock): num / den master periods per clock edge, exact: edge
 *   k of a clock is at epoch + ceil(k * num / den), the epoch being the time
 *   the clock was set; a stopped clock has num 0. The board (or the card
 *   around the chip) sets it, at any time, from whatever generates the
 *   clock: a fixed crystal (zr_sio_set_clock_hz), or a CTC channel whose
 *   ZC/TO pulses are the clock (zr_ctc_zc_period gives their ratio, which
 *   the board passes on again whenever the CTC's channel is reprogrammed).
 *   The x1/x16/x32/x64 divisor of WR4 then gives the bit time. A character
 *   starts on the first clock edge after the data was written (or the
 *   previous character ended) and each bit lasts the divisor in clock edges
 *   (the stop bit 1, 1.5 or 2 bits). The receiver detects a start bit at the
 *   first clock edge, samples the middle of the start bit (a transient is
 *   rejected), and then each bit in its middle, exactly as the divisor says;
 *   a framing error delays the search for the next start bit by half a bit
 *   time. A character in flight keeps the timing it started with. All of it
 *   is event driven through the host's schedule; nothing runs per clock.
 *
 *   The serial lines (TxD, RxD) are carried as characters on the wire, so a
 *   whole character is one host call instead of ten pin changes: the
 *   transmitter hands each character frame to the link's tx_frame at the
 *   time of its start bit, and a character is fed to a receiver with
 *   zr_sio_rx_frame(). A frame is the exact line levels (start bit,
 *   data, parity, stop bit) with its start time and bit time, so the
 *   receiver samples the real waveform: a mismatch of character length,
 *   parity or baud rate between the two ends gives wrong data and errors
 *   as it would on a wire. The link's tx_break reports the transmitter's
 *   TxD held low (WR5 bit 4), zr_sio_rx_break() holds RxD low: the
 *   receiver loads one null character with a framing error, sets the
 *   Break bit and interrupts (external/status) at the start and again at the
 *   end. rx_ready tells the board that the receiver can take a frame
 *   (enabled, clocked, idle) so that a byte end with flow control can pull
 *   its next byte at the receiver's pace.
 *
 * Choices where the data sheet leaves room: the receiver
 * keeps the first characters on overrun when `overrun_keeps` is set (the
 * adapter's RTL: the new character is lost) and otherwise follows the data
 * sheet (the new character replaces the last one in the FIFO); RR1 reports
 * parity and overrun as latched until an error reset and framing for the
 * character at the head of the FIFO; the Tx interrupt comes when the
 * transmit buffer becomes empty (not on enabling); a break sets Break/Abort
 * (RR0 bit 7) at its start.
 *
 * Save and load: the whole state, the clocks and the frame in the receiver,
 * so a load continues exactly. The link and `overrun_keeps` are the board's
 * and are not saved; load makes no host call except arming the deadline, the
 * board reads the pins with zr_sio_irq, zr_sio_rts and zr_sio_dtr.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_Z80SIO_H
#define ZR_Z80SIO_H

#include "hw.h"
#include "z80daisy.h"

enum { ZR_SIO_A = 0, ZR_SIO_B = 1 };

/* Output pins (host->line) and input pins (ops->input). */
#define ZR_SIO_OUT_INT     0
#define ZR_SIO_OUT_RTS(ch) (1 + (ch))
#define ZR_SIO_OUT_DTR(ch) (3 + (ch))
#define ZR_SIO_IN_CTS(ch)  (0 + (ch))
#define ZR_SIO_IN_DCD(ch)  (2 + (ch))
#define ZR_SIO_IN_SYNC(ch) (4 + (ch))

/* The clocks of a channel, for zr_sio_set_clock (a mask). */
enum { ZR_SIO_RXC = 1, ZR_SIO_TXC = 2 };

/* RR0 bits. */
enum {
    ZR_SIO_RR0_RXAVAIL = 0x01, ZR_SIO_RR0_INTPEND = 0x02, ZR_SIO_RR0_TXEMPTY = 0x04, ZR_SIO_RR0_DCD = 0x08,
    ZR_SIO_RR0_SYNC = 0x10, ZR_SIO_RR0_CTS = 0x20, ZR_SIO_RR0_UNDERRUN = 0x40, ZR_SIO_RR0_BREAK = 0x80
};
/* RR1 bits. */
enum { ZR_SIO_RR1_ALLSENT = 0x01, ZR_SIO_RR1_PARITY = 0x10, ZR_SIO_RR1_OVERRUN = 0x20, ZR_SIO_RR1_FRAMING = 0x40 };

/* A clock input: edge k at master time epoch + ceil(k * num / den); num 0 stops it. */
typedef struct zr_sio_clock {
    uint32_t num, den;
    uint64_t epoch;
} zr_sio_clock;

/* One character on a line: the levels from the start bit on (bit 0 of
 * `bits` is the start bit, 0), padded by the idle level (1) after `length`
 * bits; `start` is the master time of the start bit's leading edge and a bit
 * lasts bit_num / bit_den master periods. `data` and `nbits` are the data
 * bits of a transmitted character, right-justified, for a byte end. `brk`
 * marks the frame of a held break (all zero). */
typedef struct zr_sio_frame {
    uint16_t bits;
    uint8_t length;
    uint8_t data, nbits;
    uint8_t brk;
    uint64_t start;
    uint64_t bit_num, bit_den;
} zr_sio_frame;

/* What the chip tells the board about the lines (all optional). */
typedef struct zr_sio_link {
    void *ctx;
    /* A character starts on TxD of `channel`. */
    void (*tx_frame)(void *ctx, unsigned channel, const zr_sio_frame *frame);
    /* TxD is held low (on 1) or released. */
    void (*tx_break)(void *ctx, unsigned channel, int on);
    /* The receiver of `channel` can take a frame now. */
    void (*rx_ready)(void *ctx, unsigned channel);
} zr_sio_link;

typedef struct zr_sio_chan {
    uint8_t wr[8];
    uint8_t ptr;
    uint8_t in_cts, in_dcd, in_sync;            /* input pins, 1 asserted */
    uint8_t ext_live, ext_snap, ext_latched, ext_ip;
    uint8_t tx_ip, rx_armed;
    /* receiver */
    uint8_t fifo[3], fifo_err[3], head, used;
    uint8_t overrun, parity_latch;
    uint8_t rx_state;                           /* 0 idle, 1 receiving a frame, 2 recovering after a framing error */
    uint8_t rx_hold, rx_in_break, rx_ready_reported;
    uint8_t rx_d, rx_nb, rx_par;                /* divisor, data bits and parity of the frame in progress */
    zr_sio_frame rx_frame;
    zr_sio_clock rx_snap;
    uint64_t rx_k0;
    uint64_t rx_done_at, rx_free_at;
    /* transmitter */
    uint8_t tx_buf, tx_full, tx_active, tx_break_on;
    uint64_t tx_start_at, tx_end_at;
    zr_sio_clock rx_clk, tx_clk;
    uint8_t rts_out, dtr_out;
} zr_sio_chan;

typedef struct zr_sio {
    zr_dev dev;
    zr_daisy daisy;
    uint32_t master_hz;
    zr_sio_chan ch[2];
    uint8_t in_service;                         /* bit j: level j under service (priority order, 0 highest) */
    uint8_t int_out;
    uint8_t overrun_keeps;                      /* 1: an overrun loses the new character (the RTL); 0: it replaces the last one */
    zr_sio_link link;
} zr_sio;

/* Sets the chip up on `host` (default data sheet overrun behaviour, no link,
 * clocks stopped), the master clock at `master_hz`, and resets it. No host
 * call is made; the board then calls dev.ops->reset. */
void zr_sio_init(zr_sio *s, const zr_hw_host *host, const char *name, uint32_t master_hz);
void zr_sio_set_link(zr_sio *s, const zr_sio_link *link);
/* Overrun keeps the first characters (1) or the data sheet's (0, the default). */
void zr_sio_set_overrun_keeps(zr_sio *s, int keeps);
/* Sets the receive clock (`which` bit 0), the transmit clock (bit 1) or both
 * of a channel, at num / den master periods per clock edge from now on; num
 * 0 stops it. A character in flight keeps its timing. */
void zr_sio_set_clock(zr_sio *s, unsigned channel, unsigned which, uint32_t num, uint32_t den);
/* The same at `hz` clock edges per second (0 stops it). */
void zr_sio_set_clock_hz(zr_sio *s, unsigned channel, unsigned which, uint32_t hz);

/* The receiver of a channel can take a frame (enabled, clocked, not busy). */
int zr_sio_rx_ready(const zr_sio *s, unsigned channel);
/* Feeds a frame to the receiver; 0 when it is not ready (the frame is lost
 * on a wire, a byte end keeps its byte). The frame's start time is taken as
 * `now` when it lies in the past. */
int zr_sio_rx_frame(zr_sio *s, unsigned channel, const zr_sio_frame *frame);
/* Builds the well-formed frame of `value` in the channel's receive format
 * (data bits, parity, one stop bit) at the receive clock's bit time. */
void zr_sio_make_frame(const zr_sio *s, unsigned channel, uint8_t value, uint64_t start, zr_sio_frame *frame);
/* RxD held low (on 1) or released. */
void zr_sio_rx_break(zr_sio *s, unsigned channel, int on);

/* The vector an acknowledge would give now (without marking anything under
 * service: the acknowledge is zr_daisy_ack on the chain). */
uint8_t zr_sio_vector(const zr_sio *s);
/* Characters in a channel's receive FIFO, for flow control. */
unsigned zr_sio_rx_count(const zr_sio *s, unsigned channel);
/* The chip's interrupt request flag, and the levels of RTS and DTR (1 asserted). */
int zr_sio_irq(const zr_sio *s);
int zr_sio_rts(const zr_sio *s, unsigned channel);
int zr_sio_dtr(const zr_sio *s, unsigned channel);

#endif /* ZR_Z80SIO_H */
