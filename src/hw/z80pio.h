/* ZetRo - Zilog Z80 PIO parallel I/O (Z8420).
 *
 * Two 8-bit ports, A and B, each in one of four modes set by a control word:
 *  - mode 0, output: a write to the data port puts the byte on the pins and
 *    raises RDY; the peripheral's strobe (STB rising) drops RDY and asks for
 *    an interrupt. A read returns the output register;
 *  - mode 1, input: STB low loads the input register (it follows the pins
 *    while STB stays low), STB rising drops RDY and asks for an interrupt;
 *    a read returns the register and raises RDY (the read is what starts the
 *    handshake: RDY is low until the first one);
 *  - mode 2, bidirectional (port A only): the port A pins carry the byte out
 *    while ASTB is low, with ARDY and ASTB as in mode 0 (interrupt of port
 *    A, its vector); and in, with BRDY and BSTB as in mode 1 (interrupt of
 *    port B, its vector). Port B is then set to mode 3 with every bit masked;
 *  - mode 3, bit control: a second control word gives the input/output
 *    register (1 = input); a read returns the pins of the input bits and the
 *    output register of the others; the handshake is off (RDY low); an
 *    interrupt is asked when the monitored bits satisfy the logic equation
 *    (AND or OR of the bits at the high or low level, mask bit 0 = monitored)
 *    and the equation changes from false to true.
 * Other control words: the interrupt vector (bit 0 clear), the interrupt
 * control word (enable, AND/OR, high/low, mask follows: 0xx0 0111 with bit 4
 * the mask word following, which also resets a pending interrupt) and the
 * enable word (x000 0011). Written from Zilog's Z80 PIO user's manual; the
 * machines' use (CUB Z, the user PIO) checked against this project's
 * cubz_pio.sv.
 *
 * Reset (the RESET pin and power on): mode 1, all pins inputs, the mask
 * registers inhibit every bit, the interrupt enables and output registers
 * cleared, RDY low; the vector registers are not reset (data sheet).
 *
 * Bus: offset bit 0 is C/D (1: control; a read of it returns FFh), offset
 * bit 1 is B/A (0 port A, 1 port B): 0 A data, 1 A control, 2 B data, 3 B
 * control, as the CUB Z wires it; a board that wires the address lines the
 * other way (Zilog's own example puts B/A on A0 and C/D on A1) swaps the bits
 * in the offset it passes.
 *
 * Pins. Port data pins are numbered port * 8 + bit (ZR_PIO_PIN): 0-7 PA0-PA7,
 * 8-15 PB0-PB7; state is the pin's level, 1 high.
 *  - Outputs reach the board through host->line, one call per pin whose
 *    driven level changes; a pin the chip does not drive is reported as 0
 *    (so after reset every pin is 0). ARDY and BRDY (state 1 = high) and
 *    INT (the chip's request flag, state 1 = asking for an interrupt: the
 *    INT pin low on the wire) are reported the same way. A write in mode 0 or
 *    2 while RDY is already high pulses RDY low and high again, as the real
 *    chip does (the peripheral may latch on its rising edge).
 *  - Inputs come through ops->input: the pin numbers with state 0 or 1, or
 *    ZR_PIO_PORT(port) (32, 33) with the whole port's byte; ZR_PIO_IN_ASTB
 *    and ZR_PIO_IN_BSTB (state the wire level, 1 high: the strobes are active
 *    low and idle high). A pin nobody drives reads 1 (hw.h).
 *  - zr_pio_out / zr_pio_driven give the levels and the mask of driven pins
 *    at any time.
 * The board answers the INT pin by asking the daisy chain (z80daisy.h).
 *
 * Interrupts are the daisy interface of z80daisy.h (zr_pio_daisy): port A
 * has priority over port B; a port under service blocks itself and, for A,
 * port B too, until its RETI. An interrupt that is pending while the port's
 * enable is off waits and is asked for when the enable is set (data sheet).
 *
 * Settled where the data sheet leaves room: the handshake signals change at
 * the instant of the access or strobe, without the data sheet's delays of
 * a clock edge; the logic equation is evaluated at every change of an input
 * pin or of a register, and not while a control word is expected to follow.
 *
 * Save and load: the whole state, the levels last reported included, so a
 * load makes no host call; the board reads the pins with zr_pio_out.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_Z80PIO_H
#define ZR_Z80PIO_H

#include "hw.h"
#include "z80daisy.h"

enum { ZR_PIO_PORT_A, ZR_PIO_PORT_B };
#define ZR_PIO_PIN(port, bit) ((int)((port) * 8 + (bit)))
#define ZR_PIO_PORT(port)     (32 + (int)(port))
/* Input pins (ops->input, besides the data pins). */
enum { ZR_PIO_IN_ASTB = 16, ZR_PIO_IN_BSTB = 17 };
/* Output pins (host->line, besides the data pins). */
enum { ZR_PIO_OUT_ARDY = 16, ZR_PIO_OUT_BRDY = 17, ZR_PIO_OUT_INT = 18 };

typedef struct zr_pio_port {
    uint8_t mode;               /* 0 output, 1 input, 2 bidirectional (A), 3 bit control */
    uint8_t iomask;             /* mode 3: 1 = input */
    uint8_t out;                /* the output register */
    uint8_t in;                 /* the input register (modes 1 and 2) */
    uint8_t pin;                /* the levels the board drives on the pins */
    uint8_t vector;
    uint8_t intctl;             /* bit 6 AND (1) / OR, bit 5 active high (1) / low */
    uint8_t mask;               /* mode 3: 1 = masked, not monitored */
    uint8_t ie, ip, ius;        /* interrupt enable, pending, under service */
    uint8_t ready;              /* the RDY output */
    uint8_t stb;                /* the STB pin's level (1 idle) */
    uint8_t match;              /* mode 3: the logic equation as last evaluated */
    uint8_t expect_io, expect_mask;     /* the next control word is the I/O register, the mask */
    uint8_t reported, rdy_reported;     /* data pin levels and RDY last sent through host->line */
} zr_pio_port;

typedef struct zr_pio {
    zr_dev dev;
    zr_daisy daisy;
    uint8_t int_reported;
    zr_pio_port p[2];
} zr_pio;

/* Fills in the chip (all pins inputs, as after reset); no host call is
 * made. The board then calls dev.ops->reset to bring the lines in step. */
void zr_pio_init(zr_pio *d, const zr_hw_host *host, const char *name);
/* The level of each pin of a port as the chip drives it (0 for pins it does
 * not drive), and the mask of the pins it drives. */
uint8_t zr_pio_out(const zr_pio *d, int port);
uint8_t zr_pio_driven(const zr_pio *d, int port);
/* The chip's interrupt request flag. */
int zr_pio_irq(const zr_pio *d);

#endif /* ZR_Z80PIO_H */
