/* ZetRo - Zilog Z80 PIO (z80pio.h). Written from Zilog's Z80 PIO user's
 * manual (the modes, the handshake, the control words, the interrupt
 * servicing); ZetRo's own code. */
#include "z80pio.h"

#include "state.h"

#include <string.h>

/* --- the pins and the flags --------------------------------------------------------------- */

/* The pins the chip drives. */
static uint8_t driven_mask(const zr_pio *d, unsigned i) {
    const zr_pio_port *p = &d->p[i];
    switch (p->mode) {
    case 0: return 0xFFu;
    case 2: return (i == 0 && !p->stb) ? 0xFFu : 0u;     /* the output register is on the bus only while ASTB is low */
    case 3: return (uint8_t)~p->iomask;
    default: return 0u;
    }
}
static uint8_t out_level(const zr_pio *d, unsigned i) {
    return (uint8_t)(d->p[i].out & driven_mask(d, i));
}
/* The RDY output: forced low in mode 3 (port B excepted while port A is in mode 2). */
static uint8_t rdy_level(const zr_pio *d, unsigned i) {
    const zr_pio_port *p = &d->p[i];
    if (p->mode == 3 && !(i == 1 && d->p[0].mode == 2)) return 0;
    return p->ready;
}

/* The chip's interrupt flags: a request (an enabled pending port that no
 * port of the same or a higher priority has under service) and any under
 * service. */
static unsigned pio_status(const void *dev) {
    const zr_pio *d = dev;
    unsigned s = 0;
    int blocked = 0;
    for (unsigned k = 0; k < 2; k++) {
        const zr_pio_port *p = &d->p[k];
        if (p->ius) { s |= ZR_DAISY_IUS; blocked = 1; }
        if (!blocked && p->ip && p->ie) s |= ZR_DAISY_REQ;
    }
    return s;
}

/* Reports one output pin that changed (data pins, then RDY, then INT); 0
 * when none has. The level is recorded before the call, so the board may
 * call back into the chip. */
static int report_one(zr_pio *d) {
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t level = out_level(d, i), diff = (uint8_t)(level ^ d->p[i].reported);
        if (diff) {
            unsigned b = 0;
            while (!(diff >> b & 1u)) b++;
            d->p[i].reported ^= (uint8_t)(1u << b);
            zr_dev_line(&d->dev, ZR_PIO_PIN(i, b), level >> b & 1u);
            return 1;
        }
    }
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t level = rdy_level(d, i);
        if (level != d->p[i].rdy_reported) {
            d->p[i].rdy_reported = level;
            zr_dev_line(&d->dev, ZR_PIO_OUT_ARDY + (int)i, level);
            return 1;
        }
    }
    const uint8_t req = (pio_status(d) & ZR_DAISY_REQ) != 0;
    if (req != d->int_reported) {
        d->int_reported = req;
        zr_dev_line(&d->dev, ZR_PIO_OUT_INT, req);
        return 1;
    }
    return 0;
}
static void report(zr_pio *d) {
    while (report_one(d)) {}
}

/* --- mode 3 interrupt ----------------------------------------------------------------------- */

static int equation(const zr_pio_port *p) {
    const uint8_t value = (uint8_t)((p->pin & p->iomask) | (p->out & ~p->iomask));
    const uint8_t active = (p->intctl & 0x20u) ? value : (uint8_t)~value;
    const uint8_t monitored = (uint8_t)~p->mask;
    if (!monitored) return 0;
    return (p->intctl & 0x40u) ? (active & monitored) == monitored : (active & monitored) != 0;
}

/* An interrupt is asked when the equation turns from false to true. */
static void check_match(zr_pio_port *p) {
    if (p->mode != 3) { p->match = 0; return; }
    if (p->expect_io || p->expect_mask) return;     /* a register is being rewritten */
    const uint8_t m = (uint8_t)equation(p);
    if (m && !p->match) p->ip = 1;
    p->match = m;
}

static void settle(zr_pio *d) {
    check_match(&d->p[0]);
    check_match(&d->p[1]);
    report(d);
}

/* --- the handshake ---------------------------------------------------------------------------- */

/* The strobe pin of port `i` changed to `level`. */
static void strobe(zr_pio *d, unsigned i, uint8_t level) {
    zr_pio_port *p = &d->p[i], *a = &d->p[0], *b = &d->p[1];
    const uint8_t was = p->stb;
    p->stb = level;
    if (level == was) return;
    if (i == 1 && a->mode == 2) {                   /* BSTB is port A's input strobe */
        if (!level) a->in = a->pin;
        else { b->ready = 0; b->ip = 1; }
        return;
    }
    switch (p->mode) {
    case 0:
        if (level) { p->ready = 0; p->ip = 1; }
        break;
    case 1:
        if (!level) p->in = p->pin;
        else { p->ready = 0; p->ip = 1; }
        break;
    case 2:
        if (i == 0 && level) { a->ready = 0; a->ip = 1; }
        break;
    default:
        break;
    }
}

/* --- bus ------------------------------------------------------------------------------------------ */

static uint8_t read_data(zr_pio *d, unsigned i) {
    zr_pio_port *p = &d->p[i];
    switch (p->mode) {
    case 0:
        return p->out;
    case 1:
        p->ready = 1;                               /* the read starts the handshake for the next byte */
        return p->in;
    case 2:
        d->p[1].ready = 1;
        return p->in;
    default:
        return (uint8_t)((p->pin & p->iomask) | (p->out & ~p->iomask));
    }
}

static void write_data(zr_pio *d, unsigned i, uint8_t v) {
    zr_pio_port *p = &d->p[i];
    p->out = v;
    if (p->mode == 0 || (p->mode == 2 && i == 0)) {
        if (p->ready) { p->ready = 0; report(d); }  /* RDY is forced low for a moment, then high again */
        p->ready = 1;
    }
}

static void set_mode(zr_pio *d, unsigned i, unsigned mode) {
    zr_pio_port *p = &d->p[i];
    if (i == 1 && mode == 2) mode = 1;              /* only port A has a bidirectional mode */
    p->mode = (uint8_t)mode;
    if (!(mode == 3 && i == 1 && d->p[0].mode == 2)) p->ready = 0;
    p->expect_io = mode == 3;
    p->expect_mask = 0;
    p->match = 0;
}

static void write_control(zr_pio *d, unsigned i, uint8_t v) {
    zr_pio_port *p = &d->p[i];
    if (p->expect_io) { p->iomask = v; p->expect_io = 0; return; }
    if (p->expect_mask) { p->mask = v; p->expect_mask = 0; return; }
    if (!(v & 1u)) { p->vector = v; return; }
    switch (v & 0x0Fu) {
    case 0x0F:
        set_mode(d, i, v >> 6);
        break;
    case 0x07:
        p->ie = v >> 7;
        p->intctl = (uint8_t)(v & 0x60u);
        if (v & 0x10u) { p->expect_mask = 1; p->ip = 0; }
        break;
    case 0x03:
        p->ie = v >> 7;
        break;
    default:
        break;
    }
}

static uint8_t pio_read(void *dev, unsigned space, uint32_t offset) {
    zr_pio *d = dev;
    (void)space;
    uint8_t v = 0xFFu;
    if (!(offset & 1u)) v = read_data(d, (offset >> 1) & 1u);
    settle(d);
    return v;
}

static void pio_write(void *dev, unsigned space, uint32_t offset, uint8_t value) {
    zr_pio *d = dev;
    (void)space;
    const unsigned i = (offset >> 1) & 1u;
    if (offset & 1u) write_control(d, i, value);
    else write_data(d, i, value);
    settle(d);
}

static void pio_reset(void *dev) {
    zr_pio *d = dev;
    for (unsigned i = 0; i < 2; i++) {
        zr_pio_port *p = &d->p[i];
        const zr_pio_port keep = *p;                /* the wiring, the vector and what the board was told stay */
        memset(p, 0, sizeof *p);
        p->vector = keep.vector;
        p->stb = keep.stb;
        p->pin = keep.pin;
        p->reported = keep.reported;
        p->rdy_reported = keep.rdy_reported;
        p->mode = 1;
        p->iomask = 0xFFu;
        p->mask = 0xFFu;
    }
    settle(d);
}

static void pio_input(void *dev, int pin, int state) {
    zr_pio *d = dev;
    const uint8_t level = state != 0;
    if (pin >= ZR_PIO_IN_ASTB && pin <= ZR_PIO_IN_BSTB) {
        strobe(d, (unsigned)(pin - ZR_PIO_IN_ASTB), level);
        settle(d);
        return;
    }
    unsigned i;
    uint8_t value;
    if (pin >= 0 && pin < 16) {
        i = (unsigned)pin >> 3;
        const uint8_t bit = (uint8_t)(1u << (pin & 7));
        value = (uint8_t)((d->p[i].pin & ~bit) | (level ? bit : 0u));
    } else if (pin == ZR_PIO_PORT(ZR_PIO_PORT_A) || pin == ZR_PIO_PORT(ZR_PIO_PORT_B)) {
        i = (unsigned)(pin - 32);
        value = (uint8_t)state;
    } else {
        return;
    }
    zr_pio_port *p = &d->p[i];
    p->pin = value;
    if (p->mode == 1 && !p->stb) p->in = value;                      /* the register follows the pins while STB is low */
    if (i == 0 && p->mode == 2 && !d->p[1].stb) p->in = value;       /* ... port A's, while BSTB is low */
    settle(d);
}

/* --- interrupts ------------------------------------------------------------------------------------ */

static uint8_t pio_ack(void *dev) {
    zr_pio *d = dev;
    for (unsigned k = 0; k < 2; k++) {
        zr_pio_port *p = &d->p[k];
        if (p->ius) break;
        if (p->ip && p->ie) {
            p->ip = 0;
            p->ius = 1;
            const uint8_t v = p->vector;
            settle(d);
            return v;
        }
    }
    return 0xFFu;
}

static void pio_reti(void *dev) {
    zr_pio *d = dev;
    for (unsigned k = 0; k < 2; k++) {
        if (d->p[k].ius) {
            d->p[k].ius = 0;
            break;
        }
    }
    settle(d);
}

static const zr_daisy_ops PIO_DAISY_OPS = {.status = pio_status, .ack = pio_ack, .reti = pio_reti};

/* --- state --------------------------------------------------------------------------------------------- */

/* The one-byte fields of a port in the order of the state chunk; `a` is nothing (the values) or & (their addresses). */
#define PIO_FIELDS(a, p) {a (p)->mode, a (p)->iomask, a (p)->out, a (p)->in, a (p)->pin, a (p)->vector, a (p)->intctl, \
                          a (p)->mask, a (p)->ie, a (p)->ip, a (p)->ius, a (p)->ready, a (p)->stb, a (p)->match, \
                          a (p)->expect_io, a (p)->expect_mask, a (p)->reported, a (p)->rdy_reported}

static void pio_save(const void *dev, zr_state_writer *w) {
    const zr_pio *d = dev;
    zr_sw_u8(w, d->int_reported);
    for (unsigned i = 0; i < 2; i++) {
        const zr_pio_port *p = &d->p[i];
        const uint8_t f[] = PIO_FIELDS(, p);
        zr_sw_bytes(w, f, sizeof f);
    }
}

static int pio_load(void *dev, zr_state_reader *r) {
    zr_pio *d = dev;
    const uint8_t int_reported = zr_sr_u8(r);
    zr_pio_port in[2];
    for (unsigned i = 0; i < 2; i++) {
        zr_pio_port *p = &in[i];
        uint8_t f[18];
        memset(p, 0, sizeof *p);
        zr_sr_bytes(r, f, sizeof f);
        uint8_t *const fields[] = PIO_FIELDS(&, p);
        for (size_t k = 0; k < sizeof f; k++) *fields[k] = f[k];
        if (!zr_sr_ok(r) || p->mode > 3 || (i == 1 && p->mode == 2) || p->ie > 1 || p->ip > 1 || p->ius > 1 || p->ready > 1 ||
            p->stb > 1 || p->match > 1 || p->expect_io > 1 || p->expect_mask > 1 || p->rdy_reported > 1)
            return 0;
    }
    if (!zr_sr_ok(r) || int_reported > 1) return 0;
    d->int_reported = int_reported;
    memcpy(d->p, in, sizeof in);
    return 1;
}

static void pio_event(void *dev, uint64_t now) { (void)dev; (void)now; }     /* nothing is scheduled */

static const zr_dev_ops PIO_OPS = {
    .read = pio_read, .write = pio_write, .event = pio_event, .reset = pio_reset, .input = pio_input,
    .save = pio_save, .load = pio_load,
};

/* --- the board's calls ------------------------------------------------------------------------------------- */

void zr_pio_init(zr_pio *d, const zr_hw_host *host, const char *name) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &PIO_OPS;
    d->dev.host = host;
    d->dev.name = name ? name : "z80pio";
    d->daisy.ops = &PIO_DAISY_OPS;
    d->daisy.chip = d;
    for (unsigned i = 0; i < 2; i++) {
        zr_pio_port *p = &d->p[i];
        p->mode = 1;
        p->iomask = 0xFFu;
        p->mask = 0xFFu;
        p->pin = 0xFFu;
        p->stb = 1;
    }
}

uint8_t zr_pio_out(const zr_pio *d, int port) {
    return out_level(d, (unsigned)port & 1u);
}

uint8_t zr_pio_driven(const zr_pio *d, int port) {
    return driven_mask(d, (unsigned)port & 1u);
}

int zr_pio_irq(const zr_pio *d) {
    return (pio_status(d) & ZR_DAISY_REQ) != 0;
}
