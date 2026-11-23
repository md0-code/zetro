/* ZetRo - the Intel 8255A programmable peripheral interface.
 *
 * Mode 0 (basic input/output), mode 1 (strobed input/output, group A and B)
 * and mode 2 (bidirectional bus, group A), the control word, the port C bit
 * set/reset, the status word of port C and the handshake lines (STB, IBF,
 * OBF, ACK, INTR) with their interrupt enables. The Romanian machines read
 * their keyboards and joysticks through it, mostly in mode 0.
 *
 * Pins. A pin is numbered port * 8 + bit (ZR_I8255_PIN): 0-7 PA0-PA7, 8-15
 * PB0-PB7, 16-23 PC0-PC7. State is the pin's level, 1 high.
 *  - Outputs reach the board through host->line, one call per pin whose
 *    driven level changes. A pin the chip does not drive (an input port,
 *    a handshake input, port A of mode 2 while ACK is high) is reported as 0.
 *    After reset every pin is an input, so every pin reports 0.
 *  - Inputs come from the board through ops->input: the same pin numbers
 *    with state 0 or 1, or ZR_I8255_PORT(port) (32-34) with state the whole
 *    port's byte at once (the way a board loads port A from the keyboard
 *    matrix row the program selected through port C: the board sees the
 *    change of port C on host->line and answers with the new input byte).
 *    Input levels are the pins' wire levels; a pin nobody drives reads 1
 *    (hw.h: open bus is 0xFF), the idle level of the active-low handshake
 *    inputs STB and ACK. Mode 0 inputs are not latched: a read sees the level
 *    at that moment.
 *  - zr_i8255_out / zr_i8255_driven give the levels and the mask of driven
 *    pins at any time.
 *
 * Bus: offsets 0-3 (A, B, C, control) in whatever space the board maps the
 * chip into; a read of the control register is the data sheet's illegal
 * condition and returns 0xFF. Reading an output port returns its latch.
 * Writing a control word (bit 7 set) resets all latches, flip-flops and
 * interrupt enables, as the data sheet requires on a mode change; bit 7 clear
 * is the port C bit set/reset.
 *
 * Handshake model (data sheet, mode 1 and 2):
 *  - Input: STB falling loads the input latch and sets IBF; the latch
 *    follows the port while STB stays low; STB rising sets INTR when IBF
 *    and INTE are set; a read of the port clears IBF and INTR.
 *  - Output: a write sets OBF (the pin goes low) and clears INTR; ACK
 *    falling releases OBF (and, in mode 2, enables port A's outputs); ACK
 *    rising sets INTR when OBF is released and INTE is set.
 *  - INTE flip-flops are the port C bits PC4 (input group A, mode 2 INTE2),
 *    PC6 (output group A, mode 2 INTE1) and PC2 (group B), set and reset
 *    by the bit set/reset command. Setting INTE while the condition for
 *    INTR already holds raises INTR; clearing INTE lowers it.
 *  - A read of port C in mode 1 or 2 returns the status word of the data
 *    sheet: INTR, IBF and OBF as the pin levels (OBF low when full) and the
 *    INTE flags in the bit positions of STB and ACK.
 *
 * Save and load: the whole state, including the levels last reported, so a
 * load makes no host call; the board restores its own side of the pins (it
 * can read them with zr_i8255_out).
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_I8255_H
#define ZR_I8255_H

#include "hw.h"

enum { ZR_I8255_PORT_A, ZR_I8255_PORT_B, ZR_I8255_PORT_C };
#define ZR_I8255_PIN(port, bit) ((int)((port) * 8 + (bit)))
#define ZR_I8255_PORT(port)     (32 + (int)(port))

typedef struct zr_i8255 {
    zr_dev dev;
    /* configuration from the control word: modes and directions (1 input) */
    uint8_t mode_a, mode_b, dir_a, dir_b, dir_cu, dir_cl;
    uint8_t latch[3];       /* output latches of A, B, C */
    uint8_t inlatch[2];     /* strobed input latches of A and B */
    uint8_t pin[3];         /* input levels */
    uint8_t reported[3];    /* output levels last sent through host->line */
    uint8_t ibf[2], obf[2]; /* buffer full flags (obf 1: full, the pin is low) */
    uint8_t intr_in[2], intr_out[2];
    uint8_t inte;           /* INTE flags, by their PC bit: 0x04, 0x10, 0x40 */
} zr_i8255;

/* Fills in the chip (all pins inputs, as after reset); no host call is
 * made. The board then calls dev.ops->reset to bring the lines in step. */
void zr_i8255_init(zr_i8255 *d, const zr_hw_host *host, const char *name);
/* The level of each pin of a port as the chip drives it (0 for pins it
 * does not drive), and the mask of the pins it drives. */
uint8_t zr_i8255_out(const zr_i8255 *d, int port);
uint8_t zr_i8255_driven(const zr_i8255 *d, int port);

#endif /* ZR_I8255_H */
