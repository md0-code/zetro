/* ZetRo - the Z80 family's interrupt daisy chain.
 *
 * The CTC, the PIO and the SIO answer the Z80's mode 2 interrupts through a
 * priority chain wired from device to device: IEI (interrupt enable in) and
 * IEO (interrupt enable out). The device nearest the CPU has the highest
 * priority; inside a device the sources have their own order (CTC channel 0
 * first, PIO port A before B, SIO receive A, transmit A, external A, then B).
 * Each chip of this package implements the three small operations below, and
 * the board lists its devices in priority order in an array of zr_daisy and
 * asks the helpers at the end of this file.
 *
 * What a device reports (the data sheets' IP and IUS flip-flops):
 *  - ZR_DAISY_REQ: it asks for an interrupt: a source has a pending interrupt
 *    that is enabled and that no source of the device with the same or a
 *    higher priority has under service. A device asks only while its IEI is
 *    high; the helpers apply IEI.
 *  - ZR_DAISY_IUS: a source of the device is under service (acknowledged, its
 *    RETI not seen yet).
 *
 * IEO, as the data sheets give it:
 *  - for the interrupt acknowledge, IEO = IEI and not REQ and not IUS: a
 *    device that asks, or is being served, holds its IEO low so that no
 *    device below it answers;
 *  - for the RETI, a device that only asks raises IEO for the one M1 cycle
 *    that follows the opcode ED, so that the device below that is being
 *    served sees the whole two-byte instruction (CTC and PIO data sheets):
 *    IEO = IEI and not IUS.
 *
 * The chips tell the board when to look again: each has an INT output pin
 * (host->line, named in its header) that follows its REQ flag, one call at
 * every change, and nothing else changes the chain except the board's own
 * calls to ack and reti. So the board's wiring is
 *   host->line(INT pin)  ->  zr_z80_int(cpu, zr_daisy_int(chain, n))
 * and in the CPU's bus callbacks
 *   ack(ctx)             ->  v = zr_daisy_ack(chain, n); zr_z80_int(cpu, zr_daisy_int(chain, n));
 * and on a RETI
 *   zr_daisy_reti(chain, n); zr_z80_int(cpu, zr_daisy_int(chain, n));
 * (zr_z80_int takes 1 for INT low, which is what zr_daisy_int returns).
 *
 * The RETI. The chips decode it from the bus themselves; here the board does:
 * zr_reti_m1 is given the byte of every opcode fetch (M1) cycle, the bus
 * read callback of the CPU is the place (read with m1 set), and returns 1 on
 * the 4Dh that follows an EDh. That is the Z80's RETI as the peripherals see
 * it: RETN (ED 45) and the other ED opcodes do not release anything. The SIO
 * also has the RETI command in WR0, which the chip handles by itself.
 *
 * Header only, portable C11. ZetRo's own, from the Z80 CTC, PIO and SIO data
 * sheets (the interrupt priority sections). */
#ifndef ZR_Z80DAISY_H
#define ZR_Z80DAISY_H

#include <stddef.h>
#include <stdint.h>

enum { ZR_DAISY_REQ = 1, ZR_DAISY_IUS = 2 };

/* What a chip provides. `chip` is the chip's own struct. */
typedef struct zr_daisy_ops {
    /* ZR_DAISY_* flags now. */
    unsigned (*status)(const void *chip);
    /* The interrupt acknowledge cycle reached this device (it has IEI high and
     * REQ): returns the vector and marks the source under service (IP to IUS). */
    uint8_t  (*ack)(void *chip);
    /* The RETI reached this device (IEI high and IUS): ends the highest-priority
     * source that is under service. */
    void     (*reti)(void *chip);
} zr_daisy_ops;

/* One position of the chain. */
typedef struct zr_daisy {
    const zr_daisy_ops *ops;
    void *chip;
} zr_daisy;

/* Whether a device asks for an interrupt with the given IEI (1 high), and its
 * IEO for the interrupt acknowledge and for the RETI (see above). */
static inline int zr_daisy_request(const zr_daisy *d, int iei) {
    return iei && (d->ops->status(d->chip) & ZR_DAISY_REQ);
}
static inline int zr_daisy_ieo(const zr_daisy *d, int iei) {
    return iei && !(d->ops->status(d->chip) & (ZR_DAISY_REQ | ZR_DAISY_IUS));
}
static inline int zr_daisy_ieo_reti(const zr_daisy *d, int iei) {
    return iei && !(d->ops->status(d->chip) & ZR_DAISY_IUS);
}

/* The INT line of the chain of `n` devices, nearest the CPU first: 1 when a
 * device asks and every device above it is neither asking nor being served. */
static inline int zr_daisy_int(const zr_daisy *chain, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned s = chain[i].ops->status(chain[i].chip);
        if (s & ZR_DAISY_REQ) return 1;
        if (s & ZR_DAISY_IUS) return 0;
    }
    return 0;
}

/* The byte the interrupt acknowledge cycle reads: the vector of the highest
 * device that asks, which is marked under service. FFh when none does (the
 * data bus floats high). */
static inline uint8_t zr_daisy_ack(const zr_daisy *chain, size_t n) {
    for (size_t i = 0; i < n; i++) {
        const unsigned s = chain[i].ops->status(chain[i].chip);
        if (s & ZR_DAISY_REQ) return chain[i].ops->ack(chain[i].chip);
        if (s & ZR_DAISY_IUS) break;
    }
    return 0xFFu;
}

/* A RETI: the highest device under service ends its highest source. Returns
 * 1 when a device did. */
static inline int zr_daisy_reti(const zr_daisy *chain, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (chain[i].ops->status(chain[i].chip) & ZR_DAISY_IUS) {
            chain[i].ops->reti(chain[i].chip);
            return 1;
        }
    }
    return 0;
}

/* Watches the opcode fetches for the RETI. */
typedef struct zr_reti_watch {
    uint8_t prefix;             /* the last M1 byte was EDh */
} zr_reti_watch;

static inline int zr_reti_m1(zr_reti_watch *w, uint8_t opcode) {
    const int reti = w->prefix && opcode == 0x4Du;
    w->prefix = opcode == 0xEDu;
    return reti;
}

#endif /* ZR_Z80DAISY_H */
