/* ZetRo - the Z80 CPU.
 *
 * The whole processor as Zilog made it: every documented instruction and
 * every undocumented one (SLL, the halves of IX and IY, the DDCB and FDCB
 * forms that also load a register, IN F,(C) and OUT (C),0, the duplicate
 * and empty ED codes), the undocumented flags (bits 3 and 5 of F from the
 * result, from the operand, from the internal register WZ or from the
 * program counter, as each instruction does it; SCF and CCF through Q),
 * the three interrupt modes, NMI, HALT, the delay after EI, the refresh
 * register, and the NMOS processor's parity flag on an interrupt taken
 * right after LD A,I or LD A,R. `cmos` selects the CMOS processor where
 * the two differ (OUT (C),0 writes FFh, SCF and CCF take bits 3 and 5 from
 * A alone, LD A,I and LD A,R have no parity fault).
 *
 * The processor runs an instruction at a time and makes every machine
 * cycle of it on the bus, in order and with its real length, counted in
 * T-states (`t`): an opcode fetch of 4 T-states (M1: the address, then the
 * refresh address), memory reads and writes of 3, port cycles of 4, and
 * the T-states in which it works inside with an address left on the bus.
 * A callback is made at the start of its cycle, when `t` is the cycle's
 * first T-state. The board may lengthen any cycle through `stretch` (the
 * WAIT line, or a clock held by the video circuit): it is asked before
 * each cycle, and before each internal T-state with the address then on
 * the bus, and returns the T-states to add.
 *
 * Bus request is the board's: it takes the bus between instructions and
 * adds the T-states the processor stood still to `t`.
 *
 * ZetRo's own code, written from Zilog's Z80 CPU User Manual (the
 * instruction set, the timing of each machine cycle, the interrupt
 * response) and from the published descriptions of what the manual leaves
 * out (the undocumented instructions and flags, WZ and Q); fitted to the
 * SingleStepTests Z80 suite (test data, no code). No emulator source
 * consulted. */
#ifndef ZR_HW_Z80_H
#define ZR_HW_Z80_H

#include "hw.h"

/* The flags. */
enum { ZR_Z80_C = 0x01, ZR_Z80_N = 0x02, ZR_Z80_P = 0x04, ZR_Z80_X = 0x08, ZR_Z80_H = 0x10, ZR_Z80_Y = 0x20, ZR_Z80_Z = 0x40, ZR_Z80_S = 0x80 };

/* The kinds of cycle `stretch` is asked about. */
enum {
    ZR_Z80_FETCH,               /* an opcode fetch (M1), 4 T-states */
    ZR_Z80_READ,                /* a memory read, 3 */
    ZR_Z80_WRITE,               /* a memory write, 3 */
    ZR_Z80_IN,                  /* a port read, 4 */
    ZR_Z80_OUT,                 /* a port write, 4 */
    ZR_Z80_IDLE                 /* one T-state inside, the address left on the bus, MREQ and IORQ off */
};

typedef struct zr_z80_bus {
    void *ctx;
    /* A memory read; m1 1 for an opcode fetch. */
    uint8_t (*read)(void *ctx, uint16_t address, int m1);
    void (*write)(void *ctx, uint16_t address, uint8_t value);
    /* A port cycle: the whole address bus is the port. */
    uint8_t (*in)(void *ctx, uint16_t port);
    void (*out)(void *ctx, uint16_t port, uint8_t value);
    /* Optional (NULL: FFh): the byte on the data bus in an interrupt acknowledge cycle. */
    uint8_t (*ack)(void *ctx);
    /* Optional (NULL: no cycle is ever lengthened): T-states added to the cycle that starts now. */
    unsigned (*stretch)(void *ctx, int kind, uint16_t address);
} zr_z80_bus;

typedef struct zr_z80 {
    /* The registers. */
    uint16_t pc, sp;
    uint8_t a, f, b, c, d, e, h, l;
    uint8_t ixh, ixl, iyh, iyl;
    uint8_t a_, f_, b_, c_, d_, e_, h_, l_;
    uint8_t i, r;
    uint16_t wz;                /* the internal register (MEMPTR): seen in bits 3 and 5 of F after BIT n,(HL) */
    uint8_t iff1, iff2, im;
    uint8_t halted;             /* in HALT: fetching without going on, until an interrupt */
    uint8_t ei;                 /* the last instruction was EI: no interrupt is taken before the next one */
    uint8_t p;                  /* the last instruction was LD A,I or LD A,R */
    uint8_t q;                  /* F as the last instruction left it when it changed the flags, else 0 */
    /* The interrupt inputs. */
    uint8_t nmi;                /* an NMI edge is waiting */
    uint8_t irq;                /* the INT line is low */
    uint8_t cmos;
    uint8_t xy;                 /* inside an instruction: 0 HL, 1 IX, 2 IY */
    uint64_t t;                 /* T-states since power on */
    const zr_z80_bus *bus;
    const zr_cpu_hooks *hooks;  /* NULL: none (hw.h) */
} zr_z80;

/* The processor on its bus, as at power on. `hooks` NULL for none. */
void zr_z80_init(zr_z80 *z, const zr_z80_bus *bus, const zr_cpu_hooks *hooks, int cmos);
/* Power on: every register in the state the processor wakes in (AF and SP FFFFh, the rest 0). */
void zr_z80_power(zr_z80 *z);
/* The RESET pin: PC, I, R and the interrupt state cleared; the other registers keep what they held. */
void zr_z80_reset(zr_z80 *z);
/* One instruction, or the response to an interrupt, or a fetch in HALT.
 * Returns the T-states it took (0 only when a hook took the fetch over). */
unsigned zr_z80_step(zr_z80 *z);
/* The NMI pin's falling edge. */
void zr_z80_nmi(zr_z80 *z);
/* The INT pin: 1 low (asking), 0 high. */
void zr_z80_int(zr_z80 *z, int asserted);

/* The 16-bit pairs. */
static inline uint16_t zr_z80_af(const zr_z80 *z) { return (uint16_t)(z->a << 8 | z->f); }
static inline uint16_t zr_z80_bc(const zr_z80 *z) { return (uint16_t)(z->b << 8 | z->c); }
static inline uint16_t zr_z80_de(const zr_z80 *z) { return (uint16_t)(z->d << 8 | z->e); }
static inline uint16_t zr_z80_hl(const zr_z80 *z) { return (uint16_t)(z->h << 8 | z->l); }
static inline uint16_t zr_z80_ix(const zr_z80 *z) { return (uint16_t)(z->ixh << 8 | z->ixl); }
static inline uint16_t zr_z80_iy(const zr_z80 *z) { return (uint16_t)(z->iyh << 8 | z->iyl); }

/* The processor's state as a chunk's fields and back (state.h); the board opens the chunk. */
void zr_z80_save(const zr_z80 *z, struct zr_state_writer *w);
void zr_z80_load(zr_z80 *z, struct zr_state_reader *r);

#endif /* ZR_HW_Z80_H */
