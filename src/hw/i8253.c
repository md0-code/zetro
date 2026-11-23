/* ZetRo - Intel 8253 programmable interval timer. Written from the 8253
 * data sheet (see i8253.h). ZetRo's own code.
 *
 * Each counter is kept at a clock edge count `tick` and replayed to the
 * edge count of `now` on every visit. Between visits a counter is a
 * deterministic machine, so the replay jumps from one interesting edge (an
 * OUT transition, a load) to the next and only subtracts in between. */
#include "i8253.h"

#include "state.h"

#include <string.h>

/* --- time ------------------------------------------------------------------------- */

/* floor(v * mul / div), exact while mul * div < 2^64. */
static uint64_t muldiv(uint64_t v, uint32_t mul, uint32_t div) {
    return (v / div) * mul + (v % div) * mul / div;
}
/* The edges of a rate-clocked counter at or before `now` (edge k is at
 * epoch + ceil(k * num / den)). */
static uint64_t ticks_at(const zr_i8253_counter *c, uint64_t now) {
    return now > c->epoch ? muldiv(now - c->epoch, c->clk_den, c->clk_num) : 0;
}
/* The master time of edge k. */
static uint64_t time_of_tick(const zr_i8253_counter *c, uint64_t k) {
    const uint64_t q = k / c->clk_den, r = k % c->clk_den;
    return c->epoch + q * c->clk_num + (r * c->clk_num + c->clk_den - 1u) / c->clk_den;
}
/* The edge count the counter has reached at `now`: by its rate, or the
 * falling edges of its pin so far. */
static uint64_t target(const zr_i8253_counter *c, uint64_t now) {
    return c->clk_num ? ticks_at(c, now) : c->edges;
}

/* --- a counter's arithmetic ------------------------------------------------------------ */

static uint32_t modulus(const zr_i8253_counter *c) { return c->bcd ? 10000u : 65536u; }

/* The count register's value, 0..modulus-1. */
static uint32_t cr_value(const zr_i8253_counter *c) {
    if (!c->bcd) return c->cr;
    return (((c->cr >> 12) & 15u) * 1000u + ((c->cr >> 8) & 15u) * 100u + ((c->cr >> 4) & 15u) * 10u + (c->cr & 15u)) % 10000u;
}
/* The count the counter runs: 0 stands for 65536 (or 10000). */
static uint32_t ncount(const zr_i8253_counter *c) {
    const uint32_t v = cr_value(c);
    return v ? v : modulus(c);
}
static uint16_t to_bcd(uint32_t v) {
    return (uint16_t)(((v / 1000u) % 10u) << 12 | ((v / 100u) % 10u) << 8 | ((v / 10u) % 10u) << 4 | (v % 10u));
}
static uint16_t to_wire(const zr_i8253_counter *c, uint32_t v) { return c->bcd ? to_bcd(v) : (uint16_t)v; }
/* v - n modulo the counter's modulus. */
static uint32_t minus(const zr_i8253_counter *c, uint32_t v, uint64_t n) {
    const uint32_t m = modulus(c);
    return (uint32_t)((v + m - n % m) % m);
}

static int counting(const zr_i8253_counter *c) {
    /* The one-shot and the hardware strobe run on once triggered whatever the gate does. */
    return c->loaded && (c->mode == 1 || c->mode == 5 || c->gate);
}
/* Mode 3: the length in clock edges of the current half. */
static uint32_t half_len(const zr_i8253_counter *c) { return c->phase ? c->nload / 2u : (c->nload + 1u) / 2u; }

/* The counting element's value now, 0..modulus-1. */
static uint32_t count_value(const zr_i8253_counter *c) {
    if (c->mode != 3 || !c->loaded || c->nload < 2) return c->ce;
    /* Mode 3 counts down by two (the first clock after the reload by one
     * or three for an odd count, as the data sheet describes). */
    const int64_t n = c->nload, k = c->k;
    int64_t v;
    if (k == 0) v = n;
    else if (!c->phase) v = n + (n & 1) - 2 * k;
    else v = n - (n & 1) - 2 * k;
    return (uint32_t)(v % modulus(c));
}

/* --- a counter's clock edges ------------------------------------------------------------ */

/* The count register goes into the counting element. */
static void load(zr_i8253_counter *c) {
    c->pending = 0;
    c->ce = cr_value(c);
    c->loaded = 1;
    switch (c->mode) {
    case 1: c->armed = 1; c->out = 0; break;
    case 2: c->nload = ncount(c); c->out = 1; break;
    case 3: c->nload = ncount(c); c->phase = 0; c->k = 0; c->out = 1; break;
    case 4: case 5: c->armed = 1; c->strobe = 0; c->out = 1; break;
    default: c->armed = 1; break;       /* mode 0: OUT went low when the count was written */
    }
}

/* One clock edge (the counter's state is valid just before it). */
static void edge(zr_i8253_counter *c) {
    if (c->pending) { load(c); return; }
    if (!counting(c)) return;
    switch (c->mode) {
    case 0: case 1:
        c->ce = minus(c, c->ce, 1);
        if (c->armed && c->ce == 0) { c->armed = 0; c->out = 1; }
        break;
    case 2:
        if (!c->out || c->nload < 2) {          /* the clock after the low one reloads */
            c->ce = cr_value(c);
            c->nload = ncount(c);
            c->out = 1;
        } else {
            c->ce = minus(c, c->ce, 1);
            if (c->ce == 1) c->out = 0;
        }
        break;
    case 3:
        if (c->nload < 2) break;
        if (++c->k == half_len(c)) {            /* the half is over: OUT flips, the count reloads */
            c->phase ^= 1u;
            c->out = !c->phase;
            c->nload = ncount(c);
            c->k = 0;
            if (c->nload < 2) { c->phase = 0; c->out = 1; }     /* a count of 1 is illegal: OUT stays high */
        }
        break;
    default:                                    /* 4, 5 */
        if (c->strobe) { c->out = 1; c->strobe = 0; }
        c->ce = minus(c, c->ce, 1);
        if (c->armed && c->ce == 0) { c->armed = 0; c->out = 0; c->strobe = 1; }
        break;
    }
}

/* Clock edges until the next one that does more than subtract (an OUT
 * transition, a load, a reload); 0 when there is none. */
static uint64_t events_in(const zr_i8253_counter *c) {
    if (c->pending) return 1;
    if (!counting(c)) return 0;
    const uint64_t to_zero = c->ce ? c->ce : modulus(c);
    switch (c->mode) {
    case 0: case 1: return c->armed ? to_zero : 0;
    case 2: return c->nload < 2 ? 0 : c->out ? to_zero - 1u : 1u;
    case 3: return c->nload < 2 ? 0 : half_len(c) - c->k;
    default: return c->strobe ? 1u : c->armed ? to_zero : 0;
    }
}

/* n clock edges that do nothing but subtract (n is below events_in). */
static void skip(zr_i8253_counter *c, uint64_t n) {
    if (!n || !counting(c)) return;
    if (c->mode == 3) { if (c->nload >= 2) c->k += (uint32_t)n; }
    else if (c->mode != 2 || c->nload >= 2) c->ce = minus(c, c->ce, n);
}

/* --- the chip ---------------------------------------------------------------------------- */

static void report(zr_i8253 *t, unsigned i) {
    zr_i8253_counter *c = &t->c[i];
    if ((t->watch >> i & 1u) && c->out != c->out_reported) {
        c->out_reported = c->out;
        zr_dev_line(&t->dev, ZR_I8253_OUT0 + (int)i, c->out);
    }
}

/* Replays counter i to clock edge count `to`, reporting each OUT change. */
static void advance(zr_i8253 *t, unsigned i, uint64_t to) {
    zr_i8253_counter *c = &t->c[i];
    while (c->tick < to) {
        uint64_t gap = to - c->tick;
        /* A free-running counter repeats itself every count: skip whole
         * cycles. A watched counter's transitions all reach the line unless
         * the board was away for over a million clocks. */
        const uint64_t worth = (t->watch >> i & 1u) ? (1u << 20) : 0u;
        if ((c->mode == 2 || c->mode == 3) && !c->pending && counting(c) && c->nload >= 2 && ncount(c) == c->nload &&
            gap >= 2u * c->nload && gap >= worth) {
            c->tick += (gap / c->nload - 1u) * c->nload;
            gap = to - c->tick;
        }
        const uint64_t d = events_in(c);
        if (d == 0 || d > gap) {
            skip(c, gap);
            c->tick = to;
            return;
        }
        skip(c, d - 1u);
        edge(c);
        c->tick += d;
        report(t, i);
    }
}

static void sync(zr_i8253 *t, uint64_t now) {
    for (unsigned i = 0; i < 3; i++) advance(t, i, target(&t->c[i], now));
}

/* Arms the host deadline at the next event of the watched counters that a
 * rate clocks (a pin-clocked counter changes only when the board drives its
 * pin). */
static void reschedule(zr_i8253 *t) {
    uint64_t best = ZR_NEVER;
    for (unsigned i = 0; i < 3; i++) {
        const zr_i8253_counter *c = &t->c[i];
        if (!(t->watch >> i & 1u) || !c->clk_num) continue;
        const uint64_t d = events_in(c);
        if (!d) continue;
        const uint64_t when = time_of_tick(c, c->tick + d);
        if (when < best) best = when;
    }
    zr_dev_schedule(&t->dev, best);
}

/* After anything that may have moved an OUT. */
static void settle(zr_i8253 *t) {
    for (unsigned i = 0; i < 3; i++) report(t, i);
    reschedule(t);
}

/* --- bus ----------------------------------------------------------------------------------- */

static void write_control(zr_i8253_counter *c, uint8_t v) {
    const unsigned rw = (v >> 4) & 3u;
    if (rw == 0) {                              /* the counter latch command */
        if (!c->latched) { c->latch = to_wire(c, count_value(c)); c->latched = 1; }
        return;
    }
    c->mode = (uint8_t)((v >> 1) & 7u);
    if (c->mode > 5) c->mode &= 3u;             /* X10 is mode 2, X11 mode 3 */
    c->bcd = v & 1u;
    c->rw = (uint8_t)rw;
    c->loaded = c->pending = c->armed = c->strobe = 0;
    c->phase = 0;
    c->k = 0;
    c->nload = 0;
    c->write_hi = c->read_hi = c->latched = 0;
    c->out = c->mode != 0;                      /* low in mode 0, high in the others */
}

/* A whole count has been written. */
static void count_written(zr_i8253_counter *c) {
    switch (c->mode) {
    case 0: c->out = 0; c->armed = 0; c->pending = 1; break;
    case 1: case 5: break;                      /* the next trigger loads it */
    case 4: c->armed = 0; c->pending = 1; break;
    default:                                    /* 2, 3: the first count loads at once, later ones at the next reload */
        if (!c->loaded || c->nload < 2) c->pending = 1;
        break;
    }
}

static void write_count(zr_i8253_counter *c, uint8_t byte) {
    if (c->rw == 1) { c->cr = byte; count_written(c); }
    else if (c->rw == 2) { c->cr = (uint16_t)(byte << 8); count_written(c); }
    else if (!c->write_hi) {
        c->wbyte = byte;
        c->write_hi = 1;
        if (c->mode == 0 || c->mode == 4) {     /* the first byte stops the counting */
            c->loaded = c->pending = c->armed = 0;
            if (c->mode == 0) c->out = 0;
        }
    } else {
        c->cr = (uint16_t)(c->wbyte | byte << 8);
        c->write_hi = 0;
        count_written(c);
    }
}

static uint8_t read_count(zr_i8253_counter *c) {
    const uint16_t v = c->latched ? c->latch : to_wire(c, count_value(c));
    if (c->rw == 1) { c->latched = 0; return (uint8_t)v; }
    if (c->rw == 2) { c->latched = 0; return (uint8_t)(v >> 8); }
    if (!c->read_hi) { c->read_hi = 1; return (uint8_t)v; }
    c->read_hi = 0;
    c->latched = 0;
    return (uint8_t)(v >> 8);
}

static uint8_t i8253_read(void *d, unsigned space, uint32_t offset) {
    zr_i8253 *t = d;
    (void)space;
    if ((offset & 3u) == 3u) return 0xFFu;
    sync(t, zr_dev_now(&t->dev));
    const uint8_t v = read_count(&t->c[offset & 3u]);
    settle(t);
    return v;
}

static void i8253_write(void *d, unsigned space, uint32_t offset, uint8_t value) {
    zr_i8253 *t = d;
    (void)space;
    sync(t, zr_dev_now(&t->dev));
    if ((offset & 3u) == 3u) {
        if ((value >> 6) != 3u) write_control(&t->c[value >> 6], value);
    } else {
        write_count(&t->c[offset & 3u], value);
    }
    settle(t);
}

static void i8253_event(void *d, uint64_t now) {
    zr_i8253 *t = d;
    sync(t, now);
    settle(t);
}

static void i8253_reset(void *d) {
    zr_i8253 *t = d;
    const uint64_t now = zr_dev_now(&t->dev);
    for (unsigned i = 0; i < 3; i++) {
        zr_i8253_counter *c = &t->c[i];
        const zr_i8253_counter keep = *c;       /* the wiring: gate, clock, what was reported */
        memset(c, 0, sizeof *c);
        c->gate = keep.gate;
        c->out_reported = keep.out_reported;
        c->clk_level = keep.clk_level;
        c->clk_num = keep.clk_num;
        c->clk_den = keep.clk_den;
        c->epoch = keep.epoch;
        c->edges = keep.edges;
        c->rw = 3;
        c->tick = target(c, now);
    }
    settle(t);
}

static void i8253_input(void *d, int pin, int state) {
    zr_i8253 *t = d;
    if (pin < ZR_I8253_GATE0 || pin > ZR_I8253_CLK2) return;
    sync(t, zr_dev_now(&t->dev));
    if (pin >= ZR_I8253_CLK0) {
        const unsigned i = (unsigned)(pin - ZR_I8253_CLK0);
        zr_i8253_counter *c = &t->c[i];
        const uint8_t level = state != 0;
        if (c->clk_level && !level && !c->clk_num) {    /* a falling edge counts */
            c->edges++;
            advance(t, i, c->edges);
        }
        c->clk_level = level;
        settle(t);
        return;
    }
    zr_i8253_counter *c = &t->c[pin];
    const uint8_t gate = state != 0;
    if (gate != c->gate) {
        c->gate = gate;
        if (gate) {
            /* A rising edge triggers the one-shot and the hardware strobe and
             * restarts the rate generator and the square wave. */
            if (c->mode == 1 || c->mode == 5 || ((c->mode == 2 || c->mode == 3) && c->loaded)) c->pending = 1;
        } else if (c->mode == 2 || c->mode == 3) {
            c->out = 1;                         /* a low gate holds OUT high at once */
        }
    }
    settle(t);
}

/* --- state ------------------------------------------------------------------------------- */

static void i8253_save(const void *d, zr_state_writer *w) {
    const zr_i8253 *t = d;
    zr_sw_u8(w, t->watch);
    for (unsigned i = 0; i < 3; i++) {
        const zr_i8253_counter *c = &t->c[i];
        const uint8_t flags[] = {c->mode, c->bcd, c->rw, c->gate, c->out, c->out_reported, c->loaded, c->pending,
                                 c->armed, c->strobe, c->phase, c->write_hi, c->read_hi, c->latched, c->wbyte, c->clk_level};
        zr_sw_bytes(w, flags, sizeof flags);
        zr_sw_u16(w, c->cr); zr_sw_u16(w, c->latch);
        zr_sw_u32(w, c->nload); zr_sw_u32(w, c->ce); zr_sw_u32(w, c->k);
        zr_sw_u32(w, c->clk_num); zr_sw_u32(w, c->clk_den);
        zr_sw_u64(w, c->epoch); zr_sw_u64(w, c->tick); zr_sw_u64(w, c->edges);
    }
}

static int counter_valid(const zr_i8253_counter *c) {
    if (c->mode > 5 || c->bcd > 1 || c->rw < 1 || c->rw > 3 || c->gate > 1 || c->out > 1 || c->out_reported > 1 ||
        c->loaded > 1 || c->pending > 1 || c->armed > 1 || c->strobe > 1 || c->phase > 1 || c->write_hi > 1 ||
        c->read_hi > 1 || c->latched > 1 || c->clk_level > 1)
        return 0;
    if (c->clk_num && !c->clk_den) return 0;
    const uint32_t m = modulus(c);
    if (c->nload > m || c->ce >= m) return 0;
    if (c->mode == 3 && c->nload >= 2 && c->k >= half_len(c)) return 0;
    return 1;
}

static int i8253_load(void *d, zr_state_reader *r) {
    zr_i8253 *t = d;
    zr_i8253_counter in[3];
    const uint8_t watch = zr_sr_u8(r);
    for (unsigned i = 0; i < 3; i++) {
        zr_i8253_counter *c = &in[i];
        uint8_t f[16];
        memset(c, 0, sizeof *c);
        zr_sr_bytes(r, f, sizeof f);
        c->mode = f[0]; c->bcd = f[1]; c->rw = f[2]; c->gate = f[3]; c->out = f[4]; c->out_reported = f[5];
        c->loaded = f[6]; c->pending = f[7]; c->armed = f[8]; c->strobe = f[9]; c->phase = f[10];
        c->write_hi = f[11]; c->read_hi = f[12]; c->latched = f[13]; c->wbyte = f[14]; c->clk_level = f[15];
        c->cr = zr_sr_u16(r); c->latch = zr_sr_u16(r);
        c->nload = zr_sr_u32(r); c->ce = zr_sr_u32(r); c->k = zr_sr_u32(r);
        c->clk_num = zr_sr_u32(r); c->clk_den = zr_sr_u32(r);
        c->epoch = zr_sr_u64(r); c->tick = zr_sr_u64(r); c->edges = zr_sr_u64(r);
        if (!zr_sr_ok(r) || !counter_valid(c)) return 0;
    }
    t->watch = watch & 7u;
    memcpy(t->c, in, sizeof in);
    reschedule(t);
    return 1;
}

static const zr_dev_ops I8253_OPS = {
    .read = i8253_read, .write = i8253_write, .event = i8253_event, .reset = i8253_reset, .input = i8253_input,
    .save = i8253_save, .load = i8253_load,
};

void zr_i8253_init(zr_i8253 *t, const zr_hw_host *host, const char *name, uint32_t clk_num, uint32_t clk_den) {
    memset(t, 0, sizeof *t);
    t->dev.ops = &I8253_OPS;
    t->dev.host = host;
    t->dev.name = name ? name : "i8253";
    t->watch = 7;
    if (clk_num && !clk_den) clk_den = 1;
    for (unsigned i = 0; i < 3; i++) {
        zr_i8253_counter *c = &t->c[i];
        c->gate = 1;
        c->rw = 3;
        c->clk_num = clk_num;
        c->clk_den = clk_den;
    }
}

void zr_i8253_set_clock(zr_i8253 *t, unsigned counter, uint32_t num, uint32_t den) {
    if (counter > 2 || (num && !den)) return;
    const uint64_t now = zr_dev_now(&t->dev);
    sync(t, now);                               /* everything up to now runs at the old rates */
    zr_i8253_counter *c = &t->c[counter];
    c->clk_num = num;
    c->clk_den = den;
    c->epoch = now;
    c->tick = 0;
    c->edges = 0;
    settle(t);
}

void zr_i8253_watch(zr_i8253 *t, unsigned mask) {
    sync(t, zr_dev_now(&t->dev));
    t->watch = (uint8_t)(mask & 7u);
    settle(t);
}

int zr_i8253_out(zr_i8253 *t, unsigned counter) {
    sync(t, zr_dev_now(&t->dev));
    settle(t);
    return t->c[counter % 3u].out;
}
