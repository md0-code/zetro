/* ZetRo - the HC's Interface 1: the disk, serial and
 * network board of the HC-85 and of the HC-90/91, as this project's HC core
 * documents it (MiSTerRO/docs/hc/interfaces.md) and builds it
 * (core-hc/rtl/hc_family_core.sv), checked against the boards' schematics
 * where they show it.
 *
 * The board, from the processor's side:
 *   - an EPROM and 1 KiB of static RAM over the BASIC ROM at 0000h-3FFFh,
 *     paged in by an opcode fetch at 0008h-000Fh or 1708h-170Fh (that fetch
 *     already reads the EPROM) and out after an opcode fetch at
 *     0700h-0707h (the opcode itself still comes from the EPROM; the
 *     operands do not). The HC-85's board: 10 KiB of EPROM at 0000h-27FFh,
 *     the RAM at 2800h-2FFFh (twice), nothing at 3000h-3FFFh (FFh). The
 *     HC-90/91's: 16 KiB of EPROM, the RAM wherever A13 and A11 are high
 *     (2800h-2FFFh and 3800h-3FFFh, twice each). Writes reach the RAM only.
 *   - the 8272 floppy controller (upd765.h) on ports 85h (main status) and
 *     87h (data), polled (its INT and DRQ go nowhere), on a 4 MHz clock (the
 *     8 MHz crystal halved by the 74LS93 of the HC-90/91 board's sheet 5);
 *     two drives, A and B, its units 0 and 1.
 *   - the control latch on ports 05h/07h (A1 ignored): bit 0 the
 *     controller's terminal count, bit 1 selects drive A, bit 2 drive B, bit
 *     3 runs the motor (the 555 monostable's capacitor is held discharged
 *     while it is set, so the motor runs while it is set and for 1.7 s after
 *     it is cleared; the motors of the selected drives turn while it runs),
 *     bit 4 releases the controller's reset (0 holds it in reset, as at power
 *     on). A read gives bit 0 low while the motor runs, the other bits high.
 *     The controller's RDY input is tied high (both boards' sheets): a
 *     command on a drive that does not turn waits for it.
 *   - port EFh: written, bit 0 serial (1) or network (0) mode, bit 4 the
 *     serial port's CTS (high: the guest listens, the pin is ~bit 4; the receive routine polls with it high and drops it between its tries, the send routine keeps it low), bit 5 low arms the network
 *     WAIT; read, bit 3 the serial port's readiness (the other end's), bit 0
 *     the network line active, the other bits high.
 *   - port F7h: written, bit 0 the data bit (serial: the TX line is its
 *     inverse; network: the line is driven active while it is 0); read, bit
 *     7 the serial RX line inverted, bit 0 high, the other bits high.
 *   - the network WAIT: armed by a write of EFh with bit 5 low while the line
 *     is active, it holds the processor until the line is passive.
 * Ports decode the low address byte only.
 *
 * Serial bytes. The guest makes the bit timing itself (the EPROM's own
 * loops); the board here turns the TX line into bytes as a UART at
 * `baud` would (8 data bits, LSB first, a stop bit; a byte whose stop bit is
 * low is dropped), and plays the host's bytes onto the RX line at the same
 * rate, a byte at a time while the guest's CTS says it is ready.
 *
 * The network line: an input from outside (zr_if1_network_in; it reads
 * passive until a network package drives it) and the board's own driver
 * (zr_if1_network_out); the line is active when either is.
 *
 * The drives' mechanical sounds (drivesnd.h) are the board's, when enabled.
 *
 * Time is guest time (hw.h); the interface has one deadline of its own (the
 * motor delay and the serial bits) besides the controller's. Save and load
 * carry everything but the disks, which are the board's media.
 *
 * Portable C11. ZetRo's own code, written from interfaces.md, the RTL
 * named above and the schematics of the HC-90/91 Interface 1 (sheets 5 and
 * 6). */
#ifndef ZR_IF1_H
#define ZR_IF1_H

#include "hw.h"
#include "drivesnd.h"
#include "fdd.h"
#include "upd765.h"

/* The two boards. */
enum { ZR_IF1_HC85, ZR_IF1_HC90 };

#define ZR_IF1_QUEUE 256u               /* serial bytes from the host waiting to be played */

/* What the interface tells the machine. Any of the functions may be NULL. */
typedef struct zr_if1_board {
    void *ctx;
    /* The EPROM went in or out (zr_if1_paged says which): the machine maps 0000h-3FFFh again. */
    void (*paged)(void *ctx);
    /* A byte the guest sent on the serial port. */
    void (*serial_out)(void *ctx, uint8_t byte);
    /* The guest's readiness to receive (its CTS) changed. */
    void (*serial_ready)(void *ctx, int ready);
} zr_if1_board;

typedef struct zr_if1 {
    zr_dev dev;                         /* the motor delay and the serial bits */
    zr_if1_board board;
    uint8_t kind;                       /* ZR_IF1_* */
    const uint8_t *eprom;
    uint32_t eprom_size;
    uint32_t clock_hz, baud;
    zr_upd765 fdc;
    zr_fdd fdd[2];
    zr_drivesnd sound;
    /* the board's latches (saved) */
    uint8_t ram[1024];
    uint8_t phantom;                    /* the EPROM and RAM are paged in */
    uint8_t entry;                      /* fetches at the entries page it in (the machine may stop them) */
    uint8_t control;                    /* port 05h/07h */
    uint64_t motor_until;               /* the motor delay runs until then */
    uint8_t serial, network;            /* ports EFh and F7h as written */
    uint8_t net_wait;                   /* the network WAIT is armed */
    uint8_t net_in;                     /* the network line from outside is active */
    uint8_t ready_in;                   /* the other end of the serial port is ready (EFh bit 3) */
    /* the serial port's UART (saved) */
    uint8_t tx_busy, tx_count, tx_byte; /* a byte coming in from the TX line: bits sampled so far */
    uint64_t tx_at;                     /* the next sample */
    uint8_t rx_line;                    /* the RX line as played (1: mark) */
    uint8_t rx_busy, rx_bit, rx_byte;   /* the byte being played and the bit it is at (0 the start bit, 9 the stop bit) */
    uint64_t rx_at;                     /* the next bit */
    uint8_t queue[ZR_IF1_QUEUE];
    unsigned queue_head, queue_count;
    uint8_t cts_reported;
    uint8_t loopback;                   /* the serial port's TX is wired to its RX (the host's plug; not saved) */
} zr_if1;

/* The board of `kind` with its EPROM (`eprom_size` bytes, kept by the caller) on `host`, a master clock of `clock_hz`;
 * serial bytes at `baud`; the drives' sound on or off. Powered on: paged out, the controller held in reset. */
void zr_if1_init(zr_if1 *f, const zr_hw_host *host, const zr_if1_board *board, unsigned kind, const uint8_t *eprom,
                 uint32_t eprom_size, uint32_t clock_hz, uint32_t baud, int drive_sound);
/* The RESET line (power on, the reset key): paged out, the latches cleared, the controller in reset, the motors stopped. */
void zr_if1_reset(zr_if1 *f);

/* --- the processor's fetches (hw.h's CPU hooks) --------------------------------------------- */

/* Marks the entry and exit addresses in a hook's watch bitmap. */
void zr_if1_watch(uint8_t watch[8192]);
/* Before and after the opcode fetch at `pc`: the entries page in, the exit pages out (board->paged is called). */
void zr_if1_before_fetch(zr_if1 *f, uint16_t pc);
void zr_if1_after_fetch(zr_if1 *f, uint16_t pc);
static inline int zr_if1_paged(const zr_if1 *f) { return f->phantom; }
/* While paged in: what is at the 1 KiB page `page` (0-15) of 0000h-3FFFh: EPROM bytes (read only), the RAM, or nothing
 * (both NULL: reads FFh). */
const uint8_t *zr_if1_rom_page(const zr_if1 *f, unsigned page);
uint8_t *zr_if1_ram_page(zr_if1 *f, unsigned page);

/* --- ports ------------------------------------------------------------------------------------- */

/* 1 when the interface answers the port (the low byte). */
int zr_if1_decodes(uint16_t port);
uint8_t zr_if1_in(zr_if1 *f, uint16_t port);
void zr_if1_out(zr_if1 *f, uint16_t port, uint8_t value);
/* The processor is held by the network WAIT now. */
int zr_if1_held(zr_if1 *f);

/* --- the serial port and the network line -------------------------------------------------------- */

/* Bytes from the host, played onto the RX line in turn; returns how many the queue took. */
size_t zr_if1_receive(zr_if1 *f, const uint8_t *data, size_t count);
/* The other end's readiness (EFh bit 3). */
void zr_if1_serial_ready_in(zr_if1 *f, int ready);
/* A loopback plug on the serial port: the bytes the guest sends come back as bytes to receive (and the other end is always
 * ready); no byte goes to the board's serial_out. */
void zr_if1_set_loopback(zr_if1 *f, int on);
/* The guest is ready to receive (its CTS). */
int zr_if1_serial_cts(const zr_if1 *f);
/* The network line from outside (1 active), and what the board drives onto it. */
void zr_if1_network_in(zr_if1 *f, int active);
int zr_if1_network_out(const zr_if1 *f);

/* --- the drives ------------------------------------------------------------------------------------ */

/* A disk into drive 0 (A) or 1 (B): the drive is the 40-track kind for a disk of at most 42 cylinders, the 80-track kind
 * for a larger one (both double-sided, 300 rpm). 0 with the reason. */
int zr_if1_insert(zr_if1 *f, unsigned drive, zr_disk *disk, char *error, size_t capacity);
zr_disk *zr_if1_eject(zr_if1 *f, unsigned drive);
/* Bit n set while drive n's motor turns with a disk in it. */
unsigned zr_if1_activity(const zr_if1 *f);

/* --- states ------------------------------------------------------------------------------------------ */

/* The interface's chunks: its latches and UART, the controller, the two drives. */
void zr_if1_save(const zr_if1 *f, struct zr_state_writer *w);
int zr_if1_load(zr_if1 *f, struct zr_state_reader *r);

#endif /* ZR_IF1_H */
