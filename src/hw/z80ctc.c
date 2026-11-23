/* ZetRo - Zilog Z80 CTC (z80ctc.h). Written from Zilog's Z80 CTC product
 * specification and peripherals manual; ZetRo's own code.
 *
 * A channel is kept at a system clock edge count `tick` and replayed to the
 * edge count of `now` on every visit. A running timer is a deterministic
 * machine between visits: the replay jumps from one zero count to the next,
 * or, for a channel nobody observes, just subtracts. */
#include "z80ctc.h"

#include "state.h"

#include <string.h>

enum { ST_STOPPED, ST_WAITING, ST_RUNNING };

/* --- time ------------------------------------------------------------------------- */

/* floor(v * mul / div), exact while mul * div < 2^64. */
static uint64_t muldiv(uint64_t v, uint32_t mul, uint32_t div) {
    return (v / div) * mul + (v % div) * mul / div;
}
/* The system clock edges at or before `now` (edge k is at epoch + ceil(k * num / den)). */
static uint64_t ticks_at(const zr_ctc *t, uint64_t now) {
    return now > t->epoch ? muldiv(now - t->epoch, t->clk_den, t->clk_num) : 0;
}
/* The master time of edge k. */
static uint64_t time_of_tick(const zr_ctc *t, uint64_t k) {
    const uint64_t q = k / t->clk_den, r = k % t->clk_den;
    return t->epoch + q * t->clk_num + (r * t->clk_num + t->clk_den - 1u) / t->clk_den;
}

/* --- a channel ---------------------------------------------------------------------- */

static int is_timer(const zr_ctc_channel *c) { return !(c->control & ZR_CTC_COUNTER); }
/* A counter whose CLK/TRG is a periodic clock (zr_ctc_set_trigger_clock): it counts like a timer with a prescaler of 1 on
 * that clock. */
static int external(const zr_ctc *t, unsigned i) { return !is_timer(&t->ch[i]) && t->trg_num[i]; }
static unsigned prescale(const zr_ctc_channel *c) { return is_timer(c) ? ((c->control & ZR_CTC_P256) ? 256u : 16u) : 1u; }
static unsigned tcv(const zr_ctc_channel *c) { return c->tc ? c->tc : 256u; }
/* A channel that counts on a clock the chip knows: a timer, or a counter on a periodic CLK/TRG. */
static int clocked(const zr_ctc *t, unsigned i) { return t->ch[i].state == ST_RUNNING && (is_timer(&t->ch[i]) || external(t, i)); }
/* The edges of the channel's clock at or before `now`, and the master time of edge k. */
static uint64_t ch_ticks_at(const zr_ctc *t, unsigned i, uint64_t now) {
    if (!external(t, i)) return ticks_at(t, now);
    return now > t->trg_epoch[i] ? muldiv(now - t->trg_epoch[i], t->trg_den[i], t->trg_num[i]) : 0;
}
static uint64_t ch_time_of_tick(const zr_ctc *t, unsigned i, uint64_t k) {
    if (!external(t, i)) return time_of_tick(t, k);
    const uint64_t q = k / t->trg_den[i], r = k % t->trg_den[i];
    return t->trg_epoch[i] + q * t->trg_num[i] + (r * t->trg_num[i] + t->trg_den[i] - 1u) / t->trg_den[i];
}
static int observed(const zr_ctc *t, unsigned i) {
    return (t->ch[i].control & ZR_CTC_INT) || (i < 3u && (t->watch >> i & 1u));
}
/* System clock edges from the channel's state to its next zero count. */
static uint64_t edges_to_zero(const zr_ctc_channel *c) {
    const uint64_t p = prescale(c);
    return (uint64_t)(c->count - 1u) * p + (p - c->pre);
}

/* The chip's interrupt flags: a request (an enabled pending channel that no
 * channel of the same or a higher priority has under service) and any under
 * service. */
static unsigned ctc_status(const void *d) {
    const zr_ctc *t = d;
    unsigned s = 0;
    int blocked = 0;
    for (unsigned k = 0; k < 4; k++) {
        const zr_ctc_channel *c = &t->ch[k];
        if (c->ius) { s |= ZR_DAISY_IUS; blocked = 1; }
        if (!blocked && c->ip && (c->control & ZR_CTC_INT)) s |= ZR_DAISY_REQ;
    }
    return s;
}

/* Reports every output pin that changed, ZC/TO first. The level is recorded
 * before the call, so the board may call back into the chip. */
static void report(zr_ctc *t) {
    for (;;) {
        unsigned i = 0;
        while (i < 3u && t->ch[i].zc == t->ch[i].zc_reported) i++;
        if (i < 3u) {
            t->ch[i].zc_reported = t->ch[i].zc;
            zr_dev_line(&t->dev, ZR_CTC_OUT_ZCTO0 + (int)i, t->ch[i].zc);
            continue;
        }
        const uint8_t req = (ctc_status(t) & ZR_DAISY_REQ) != 0;
        if (req == t->int_reported) return;
        t->int_reported = req;
        zr_dev_line(&t->dev, ZR_CTC_OUT_INT, req);
    }
}

/* A channel counted down to zero: its interrupt is pending when enabled, the
 * pulse is high until `off`. */
static void zero_count(zr_ctc *t, unsigned i, uint64_t off) {
    zr_ctc_channel *c = &t->ch[i];
    if (c->control & ZR_CTC_INT) c->ip = 1;
    if (i < 3u && (t->watch >> i & 1u)) { c->zc = 1; c->pulse_off = off; }
}

/* `n` system clock edges of a running timer that contain no zero count the
 * caller wants to see: the arithmetic of the prescaler and the counter,
 * zero counts included (the counter reloads and goes on). */
static void run_edges(zr_ctc_channel *c, uint64_t n) {
    const uint64_t p = prescale(c), total = c->pre + n, dec = total / p;
    c->pre = (uint16_t)(total % p);
    if (dec < c->count) {
        c->count = (uint16_t)(c->count - dec);
    } else {
        const uint64_t r = dec - c->count, v = tcv(c);
        c->count = (uint16_t)(v - r % v);
    }
}

/* Replays a channel to `now`. */
static void advance(zr_ctc *t, unsigned i, uint64_t now) {
    zr_ctc_channel *c = &t->ch[i];
    const uint64_t target = ch_ticks_at(t, i, now);
    if (clocked(t, i) && !observed(t, i)) {
        if (target > c->tick) { run_edges(c, target - c->tick); c->tick = target; }
    } else if (clocked(t, i)) {
        while (clocked(t, i)) {
            const uint64_t zt = c->tick + edges_to_zero(c);
            if (zt > target) {
                if (target > c->tick) { run_edges(c, target - c->tick); c->tick = target; }
                break;
            }
            const uint64_t ztime = ch_time_of_tick(t, i, zt);
            if (c->zc && c->pulse_off <= ztime) {   /* the pulse before ends first */
                c->zc = 0;
                report(t);
                continue;
            }
            c->tick = zt;
            c->pre = 0;
            c->count = (uint16_t)tcv(c);
            zero_count(t, i, ch_time_of_tick(t, i, zt + 1u));
            report(t);
            /* A free-running channel observed across a very long gap: skip whole cycles (the pulses are lost). */
            const uint64_t cycle = (uint64_t)tcv(c) * prescale(c);
            if (clocked(t, i) && c->tick < target && target - c->tick > cycle << 20) {
                c->tick += ((target - c->tick) / cycle - 1u) * cycle;
            }
        }
    } else if (target > c->tick) {
        c->tick = target;
    }
    if (c->zc && c->pulse_off <= now) {
        c->zc = 0;
        report(t);
    }
}

static void sync(zr_ctc *t, uint64_t now) {
    for (unsigned i = 0; i < 4; i++) advance(t, i, now);
}

/* Arms the host deadline: the next zero count of an observed running timer
 * and the end of the nearest pulse. */
static void reschedule(zr_ctc *t) {
    uint64_t best = ZR_NEVER;
    for (unsigned i = 0; i < 4; i++) {
        const zr_ctc_channel *c = &t->ch[i];
        if (clocked(t, i) && observed(t, i)) {
            const uint64_t w = ch_time_of_tick(t, i, c->tick + edges_to_zero(c));
            if (w < best) best = w;
        }
        if (c->zc && c->pulse_off < best) best = c->pulse_off;
    }
    zr_dev_schedule(&t->dev, best);
}

static void settle(zr_ctc *t) {
    report(t);
    reschedule(t);
}

/* An active edge of CLK/TRG (or a change of the slope): starts a waiting
 * timer, decrements a running counter. */
static void trigger(zr_ctc *t, unsigned i, uint64_t now) {
    zr_ctc_channel *c = &t->ch[i];
    if (is_timer(c)) {
        if (c->state == ST_WAITING) {
            c->state = ST_RUNNING;
            c->count = (uint16_t)tcv(c);
            c->pre = 0;
            c->tick = ticks_at(t, now);             /* the prescaler counts the edges after this instant */
        }
    } else if (c->state == ST_RUNNING) {
        if (c->count <= 1u) {
            c->count = (uint16_t)tcv(c);
            zero_count(t, i, time_of_tick(t, ticks_at(t, now) + 1u));
        } else {
            c->count--;
        }
    }
}

/* --- bus --------------------------------------------------------------------------------- */

static void write_control(zr_ctc *t, unsigned i, uint8_t v, uint64_t now) {
    zr_ctc_channel *c = &t->ch[i];
    const uint8_t old = c->control;
    c->control = (uint8_t)(v & 0xF8u);
    c->pre = (uint16_t)(c->pre % prescale(c));      /* the prescaler's phase under a new factor */
    c->expect = (v >> 2) & 1u;
    if (v & ZR_CTC_RESET) {                         /* software reset: it stops, a new time constant is needed */
        c->state = ST_STOPPED;
        c->ip = c->ius = 0;
        c->zc = 0;
    } else if (((old ^ v) & ZR_CTC_RISING) && !external(t, i)) {
        trigger(t, i, now);
    }
    if ((old ^ v) & ZR_CTC_COUNTER) c->tick = ch_ticks_at(t, i, now);   /* the channel counts on another clock now */
}

static void write_time_constant(zr_ctc *t, unsigned i, uint8_t v, uint64_t now) {
    zr_ctc_channel *c = &t->ch[i];
    c->tc = v;
    c->expect = 0;
    if (c->state != ST_STOPPED) return;             /* a running channel loads it at its next zero count */
    c->count = (uint16_t)tcv(c);
    c->pre = 0;
    if (is_timer(c) && (c->control & ZR_CTC_TRIGGER)) {
        c->state = ST_WAITING;
    } else {
        c->state = ST_RUNNING;
        c->tick = ch_ticks_at(t, i, now);           /* the first decrement of the prescaler is the next edge */
    }
}

static uint8_t ctc_read(void *d, unsigned space, uint32_t offset) {
    zr_ctc *t = d;
    (void)space;
    const unsigned i = offset & 3u;
    advance(t, i, zr_dev_now(&t->dev));
    const uint8_t v = (uint8_t)t->ch[i].count;
    settle(t);
    return v;
}

static void ctc_write(void *d, unsigned space, uint32_t offset, uint8_t value) {
    zr_ctc *t = d;
    (void)space;
    const unsigned i = offset & 3u;
    const uint64_t now = zr_dev_now(&t->dev);
    sync(t, now);
    if (t->ch[i].expect) write_time_constant(t, i, value, now);
    else if (value & ZR_CTC_CONTROL) write_control(t, i, value, now);
    else if (i == 0) t->vector = (uint8_t)(value & 0xF8u);
    settle(t);
}

static void ctc_event(void *d, uint64_t now) {
    zr_ctc *t = d;
    sync(t, now);
    settle(t);
}

static void ctc_reset(void *d) {
    zr_ctc *t = d;
    const uint64_t now = zr_dev_now(&t->dev);
    sync(t, now);
    for (unsigned i = 0; i < 4; i++) {
        zr_ctc_channel *c = &t->ch[i];
        const uint8_t trg = c->trg, reported = c->zc_reported;   /* the wiring and what the board was told */
        memset(c, 0, sizeof *c);
        c->trg = trg;
        c->zc_reported = reported;
        c->count = 256;
        c->tick = ch_ticks_at(t, i, now);
    }
    settle(t);
}

static void ctc_input(void *d, int pin, int state) {
    zr_ctc *t = d;
    if (pin < ZR_CTC_TRG0 || pin > ZR_CTC_TRG3) return;
    const unsigned i = (unsigned)pin;
    const uint64_t now = zr_dev_now(&t->dev);
    sync(t, now);
    zr_ctc_channel *c = &t->ch[i];
    const uint8_t level = state != 0;
    const uint8_t was = c->trg;
    c->trg = level;
    if (level != was && level == ((c->control & ZR_CTC_RISING) ? 1u : 0u) && !t->trg_num[i]) trigger(t, i, now);
    settle(t);
}

/* --- interrupts ----------------------------------------------------------------------------- */

static uint8_t ctc_ack(void *d) {
    zr_ctc *t = d;
    for (unsigned k = 0; k < 4; k++) {
        zr_ctc_channel *c = &t->ch[k];
        if (c->ius) break;
        if (c->ip && (c->control & ZR_CTC_INT)) {
            c->ip = 0;
            c->ius = 1;
            const uint8_t v = (uint8_t)((t->vector & 0xF8u) | k << 1);
            settle(t);
            return v;
        }
    }
    return t->vector;
}

static void ctc_reti(void *d) {
    zr_ctc *t = d;
    for (unsigned k = 0; k < 4; k++) {
        if (t->ch[k].ius) {
            t->ch[k].ius = 0;
            break;
        }
    }
    settle(t);
}

static const zr_daisy_ops CTC_DAISY_OPS = {.status = ctc_status, .ack = ctc_ack, .reti = ctc_reti};

/* --- state ------------------------------------------------------------------------------------ */

static void ctc_save(const void *d, zr_state_writer *w) {
    const zr_ctc *t = d;
    zr_sw_u32(w, t->clk_num); zr_sw_u32(w, t->clk_den); zr_sw_u64(w, t->epoch);
    zr_sw_u8(w, t->vector); zr_sw_u8(w, t->watch); zr_sw_u8(w, t->int_reported);
    for (unsigned i = 0; i < 4; i++) {
        const zr_ctc_channel *c = &t->ch[i];
        const uint8_t flags[] = {c->control, c->tc, c->state, c->expect, c->ip, c->ius, c->trg, c->zc, c->zc_reported};
        zr_sw_bytes(w, flags, sizeof flags);
        zr_sw_u16(w, c->count); zr_sw_u16(w, c->pre);
        zr_sw_u64(w, c->tick); zr_sw_u64(w, c->pulse_off);
    }
    for (unsigned i = 0; i < 4; i++) {             /* the periodic CLK/TRG clocks (a later addition: optional on load) */
        zr_sw_u32(w, t->trg_num[i]); zr_sw_u32(w, t->trg_den[i]); zr_sw_u64(w, t->trg_epoch[i]);
    }
}

static int ctc_load(void *d, zr_state_reader *r) {
    zr_ctc *t = d;
    const uint32_t num = zr_sr_u32(r), den = zr_sr_u32(r);
    const uint64_t epoch = zr_sr_u64(r);
    const uint8_t vector = zr_sr_u8(r), watch = zr_sr_u8(r), int_reported = zr_sr_u8(r);
    zr_ctc_channel in[4];
    for (unsigned i = 0; i < 4; i++) {
        zr_ctc_channel *c = &in[i];
        uint8_t f[9];
        memset(c, 0, sizeof *c);
        zr_sr_bytes(r, f, sizeof f);
        c->control = f[0]; c->tc = f[1]; c->state = f[2]; c->expect = f[3]; c->ip = f[4]; c->ius = f[5];
        c->trg = f[6]; c->zc = f[7]; c->zc_reported = f[8];
        c->count = zr_sr_u16(r); c->pre = zr_sr_u16(r);
        c->tick = zr_sr_u64(r); c->pulse_off = zr_sr_u64(r);
        if (!zr_sr_ok(r) || c->state > ST_RUNNING || c->expect > 1 || c->ip > 1 || c->ius > 1 || c->trg > 1 || c->zc > 1 ||
            c->zc_reported > 1 || c->count < 1 || c->count > 256 || c->pre >= prescale(c))
            return 0;
    }
    if (!zr_sr_ok(r) || !num || !den || int_reported > 1) return 0;
    uint32_t trg_num[4] = {0, 0, 0, 0}, trg_den[4] = {1, 1, 1, 1};
    uint64_t trg_epoch[4] = {0, 0, 0, 0};
    if (r->at < r->end) {
        for (unsigned i = 0; i < 4; i++) { trg_num[i] = zr_sr_u32(r); trg_den[i] = zr_sr_u32(r); trg_epoch[i] = zr_sr_u64(r); }
        for (unsigned i = 0; i < 4; i++)
            if (!zr_sr_ok(r) || !trg_den[i]) return 0;
    }
    memcpy(t->trg_num, trg_num, sizeof trg_num);
    memcpy(t->trg_den, trg_den, sizeof trg_den);
    memcpy(t->trg_epoch, trg_epoch, sizeof trg_epoch);
    t->clk_num = num; t->clk_den = den; t->epoch = epoch;
    t->vector = vector; t->watch = watch & 7u; t->int_reported = int_reported;
    memcpy(t->ch, in, sizeof in);
    reschedule(t);
    return 1;
}

static const zr_dev_ops CTC_OPS = {
    .read = ctc_read, .write = ctc_write, .event = ctc_event, .reset = ctc_reset, .input = ctc_input,
    .save = ctc_save, .load = ctc_load,
};

/* --- the board's calls --------------------------------------------------------------------------- */

void zr_ctc_init(zr_ctc *t, const zr_hw_host *host, const char *name, uint32_t clk_num, uint32_t clk_den) {
    memset(t, 0, sizeof *t);
    t->dev.ops = &CTC_OPS;
    t->dev.host = host;
    t->dev.name = name ? name : "z80ctc";
    t->daisy.ops = &CTC_DAISY_OPS;
    t->daisy.chip = t;
    t->clk_num = clk_num ? clk_num : 1u;
    t->clk_den = clk_den ? clk_den : 1u;
    t->watch = 7;
    for (unsigned i = 0; i < 4; i++) {
        t->ch[i].count = 256;
        t->ch[i].trg = 1;
        t->trg_den[i] = 1;
    }
}

void zr_ctc_set_clock(zr_ctc *t, uint32_t num, uint32_t den) {
    if (!num || !den) return;
    const uint64_t now = zr_dev_now(&t->dev);
    sync(t, now);                                   /* what happened before now ran at the old rate */
    t->clk_num = num;
    t->clk_den = den;
    t->epoch = now;
    for (unsigned i = 0; i < 4; i++)
        if (!external(t, i)) t->ch[i].tick = 0;
    settle(t);
}

void zr_ctc_set_trigger_clock(zr_ctc *t, unsigned channel, uint32_t num, uint32_t den) {
    if (channel > 3u) return;
    const uint64_t now = zr_dev_now(&t->dev);
    sync(t, now);
    const int was = external(t, channel);
    t->trg_num[channel] = den ? num : 0u;
    t->trg_den[channel] = den ? den : 1u;
    t->trg_epoch[channel] = now;
    if (external(t, channel) || was) t->ch[channel].tick = ch_ticks_at(t, channel, now);
    settle(t);
}

void zr_ctc_watch(zr_ctc *t, unsigned mask) {
    sync(t, zr_dev_now(&t->dev));
    t->watch = (uint8_t)(mask & 7u);
    for (unsigned i = 0; i < 3; i++) {
        if (!(t->watch >> i & 1u)) t->ch[i].zc = 0;
    }
    settle(t);
}

int zr_ctc_zcto(const zr_ctc *t, unsigned channel) {
    return channel < 3u ? t->ch[channel].zc : 0;
}

int zr_ctc_irq(const zr_ctc *t) {
    return (ctc_status(t) & ZR_DAISY_REQ) != 0;
}

int zr_ctc_zc_period(const zr_ctc *t, unsigned channel, uint64_t *num, uint64_t *den) {
    if (channel > 3u || !clocked(t, channel)) return 0;
    const zr_ctc_channel *c = &t->ch[channel];
    const int ext = external(t, channel);
    *num = (uint64_t)prescale(c) * tcv(c) * (ext ? t->trg_num[channel] : t->clk_num);
    *den = ext ? t->trg_den[channel] : t->clk_den;
    return 1;
}
