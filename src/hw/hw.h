/* ZetRo - the chip interface.
 *
 * Every chip is a plain struct that starts with a zr_dev and has a
 * zr_dev_ops table. A chip never calls the bus, the scheduler or another
 * chip: it talks to the board only through the zr_hw_host callbacks it is
 * given at init, so each chip is unit-tested against a fake host.
 *
 * Time is a count of periods of the board's master clock (the crystal the
 * board divides everything from: 14 MHz on the Spectrum-compatible boards,
 * 10 MHz on the native ones; each board says which in its own header). A
 * chip's deadlines and the host's now() are in these periods; a chip that
 * needs real time is told the clock's rate at init.
 *
 * Address spaces: the Z80 has a memory space (16 bits) and an I/O space (16
 * bits: the full address bus is on it during IN and OUT). The board maps a
 * chip into either or both and hands read and write the space and the
 * offset inside the chip's window. Cycles are one byte wide.
 *
 * Signals: a chip's output pins reach the board through host->line (INT,
 * DRQ, a port's bits...); the board drives a chip's input pins through
 * ops->input. Pin numbers are the chip's own, listed in its header.
 *
 * State: every chip writes all of its state into a chunk with ops->save
 * and takes it back with ops->load (core/state.h), so that a machine can
 * be saved, restored and rewound. A chip without both is not complete.
 *
 * Portable C11 only: no compiler extensions, no OS or SDL header. ZetRo's own code. */
#ifndef ZR_HW_H
#define ZR_HW_H

#include <stddef.h>
#include <stdint.h>

#define ZR_NEVER UINT64_MAX

/* The address spaces of a cycle. */
enum { ZR_SPACE_MEM = 0, ZR_SPACE_IO = 1 };

struct zr_state_writer;
struct zr_state_reader;

typedef struct zr_dev_ops {
    /* A bus cycle on the chip: `space` ZR_SPACE_*, `offset` inside the
     * chip's window. A pin the chip does not drive reads as 1. */
    uint8_t (*read)(void *dev, unsigned space, uint32_t offset);
    void    (*write)(void *dev, unsigned space, uint32_t offset, uint8_t value);
    /* The deadline the chip set with schedule() has come (`now` is it). */
    void    (*event)(void *dev, uint64_t now);
    /* The RESET pin: the chip's power-on / reset state. */
    void    (*reset)(void *dev);
    /* Optional (NULL: none). An input pin changed: state 1 asserted (the
     * chip's header says which level that is on the wire). */
    void    (*input)(void *dev, int pin, int state);
    /* Optional (NULL: no wait states). Extra master-clock periods the cycle
     * that is about to run on the chip is held (WAIT low). */
    uint32_t (*wait)(void *dev, unsigned space, uint32_t offset, int write, uint64_t now);
    /* Optional (NULL: the chip takes no part in DMA). A DMA cycle with the
     * chip's DACK asserted: with `to_device` the byte `value` read from
     * memory goes to the chip; otherwise the chip supplies the byte, which
     * is returned. `tc` is the controller's terminal count on this cycle. */
    uint8_t (*dack)(void *dev, int channel, int to_device, uint8_t value, int tc);
    /* The chip's whole state into the chunk the board opened for it, and
     * back (state.h). load returns 0 when the chunk is not the chip's. */
    void    (*save)(const void *dev, struct zr_state_writer *w);
    int     (*load)(void *dev, struct zr_state_reader *r);
} zr_dev_ops;

/* What the board provides to a chip. */
typedef struct zr_hw_host {
    void *ctx;
    /* Current guest time in master-clock periods. */
    uint64_t (*now)(void *ctx);
    /* Arms the chip's single deadline (absolute time); ZR_NEVER cancels.
     * Re-arming replaces the previous deadline. */
    void (*schedule)(void *ctx, void *dev, uint64_t when);
    /* An output pin of the chip changed; state 1 asserted. */
    void (*line)(void *ctx, void *dev, int pin, int state);
    /* Optional (NULL when the board has no bus master): the chip as bus
     * master reads (write 0) or writes (write 1) `length` bytes of the
     * memory space at `address`, in address order. Returns 0 when nothing
     * answers. */
    int (*dma)(void *ctx, uint32_t address, void *data, uint32_t length, int write);
} zr_hw_host;

/* The common header, first in every chip struct. */
typedef struct zr_dev {
    const zr_dev_ops *ops;
    const zr_hw_host *host;
    const char *name;
} zr_dev;

static inline uint64_t zr_dev_now(const zr_dev *d) { return d->host->now(d->host->ctx); }
static inline void zr_dev_schedule(zr_dev *d, uint64_t when) { d->host->schedule(d->host->ctx, d, when); }
static inline void zr_dev_line(zr_dev *d, int pin, int state) { d->host->line(d->host->ctx, d, pin, state); }
static inline int zr_dev_dma(zr_dev *d, uint32_t address, void *data, uint32_t length, int write) {
    return d->host->dma && d->host->dma(d->host->ctx, address, data, length, write);
}

/* --- the CPU's hooks ------------------------------------------------------------------
 *
 * What a board may hang on the Z80 besides its bus, for the hardware that
 * watches the processor rather than answers it: Interface 1 and the DivIDE
 * and DivMMC interfaces page themselves in on an opcode fetch at certain
 * addresses, the Dandanator counts the instructions that write to the ROM
 * area, CoBra's startup latch is clocked by the refresh address, and the
 * tape traps (not original) take over at the entry of a ROM routine. A
 * board without any of them leaves the table NULL and pays nothing.
 *
 * `watch` is one bit for each address (8192 bytes, bit (a & 7) of byte
 * a >> 3): before_fetch and after_fetch are called only for an opcode fetch
 * (M1 of the first byte of an instruction, prefixes included) at an address
 * whose bit is set, so a board marks only the few addresses it cares about.
 * NULL watches nothing. */
typedef struct zr_cpu_hooks {
    void *ctx;
    const uint8_t *watch;
    /* Before the opcode at `pc` is read: the hook may change the memory map
     * (the opcode is then read through the new one) or the CPU's registers
     * (a trap: when it returns 1 the fetch is abandoned and the CPU fetches
     * again at its new PC). */
    int  (*before_fetch)(void *ctx, uint16_t pc);
    /* After the opcode at `pc` was read, before it runs (the paging-out of
     * an interface on the instruction after the one that asked for it). */
    void (*after_fetch)(void *ctx, uint16_t pc, uint8_t opcode);
    /* Optional (NULL: none): every refresh cycle, with the address on the
     * bus then (I in the high byte, R in the low one). */
    void (*refresh)(void *ctx, uint16_t address);
} zr_cpu_hooks;

static inline int zr_cpu_watched(const zr_cpu_hooks *h, uint16_t pc) {
    return h && h->watch && (h->watch[pc >> 3] >> (pc & 7u) & 1u);
}

/* The mechanical events of a disk drive, for whatever listens to them (the
 * drive sound source). A chip makes one call of its hook for each event,
 * stamped with the guest time it happens at; a chip whose hook is NULL does
 * nothing at all. `a` and `b` by event:
 *   ZR_DRIVE_MOTOR      a 1 the spindle motor is switched on, 0 off
 *   ZR_DRIVE_STEP       a 1 one step towards the spindle, 0 towards track 0;
 *                       b 1 when the carriage was against its end stop
 *   ZR_DRIVE_HEAD_LOAD  the head is loaded against the disk
 *   ZR_DRIVE_HEAD_HOLD  a command that used the head ended at `t`; the
 *                       controller unloads it a periods later unless used again */
enum { ZR_DRIVE_MOTOR, ZR_DRIVE_STEP, ZR_DRIVE_HEAD_LOAD, ZR_DRIVE_HEAD_HOLD };
typedef void (*zr_drive_hook)(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b);

/* Little-endian byte access to guest memory images (no unaligned casts). */
static inline uint16_t zr_get16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline void zr_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

#endif /* ZR_HW_H */
