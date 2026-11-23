/* ZetRo - Zilog Z80-SIO (z80sio.h). Portable C11. Written from the Z80-SIO data sheet. */
#include "z80sio.h"

#include "state.h"

#include <string.h>

enum { ERR_PARITY = 1, ERR_FRAMING = 2 };
enum { RX_IDLE, RX_FRAME, RX_RECOVER };

/* --- clocks ------------------------------------------------------------------------------------- */

/* floor(v * mul / div), exact while mul * div < 2^64. */
static uint64_t muldiv(uint64_t v, uint32_t mul, uint32_t div) {
    return (v / div) * mul + (v % div) * mul / div;
}
/* The master time of edge k of a running clock (edge k is at epoch + ceil(k * num / den)). */
static uint64_t edge_time(const zr_sio_clock *c, uint64_t k) {
    const uint64_t q = k / c->den, r = k % c->den;
    return c->epoch + q * c->num + (r * c->num + c->den - 1u) / c->den;
}
/* The index of the first edge at or after master time t (after t when `strict`). */
static uint64_t edge_after(const zr_sio_clock *c, uint64_t t, int strict) {
    const uint64_t r = strict ? t + 1u : t;
    if (r <= c->epoch) return 0;
    return muldiv(r - c->epoch - 1u, c->den, c->num) + 1u;
}

/* --- small helpers ------------------------------------------------------------------------------ */

static unsigned divisor(const zr_sio_chan *c) {
    static const uint8_t d[4] = {1, 16, 32, 64};
    return d[c->wr[4] >> 6];
}

/* 5, 7, 6 or 8 bits for the register encodings 0-3. */
static unsigned char_bits(unsigned v) {
    static const uint8_t b[4] = {5, 7, 6, 8};
    return b[v & 3u];
}

/* Receiver enable (WR3 bit 0) and, with auto enables (WR3 bit 5), DCD. */
static int rx_enabled(const zr_sio_chan *c) {
    return (c->wr[3] & 1u) && (!(c->wr[3] & 0x20u) || c->in_dcd);
}

/* Transmitter enable (WR5 bit 3) and, with auto enables, CTS. */
static int tx_enabled(const zr_sio_chan *c) {
    return (c->wr[5] & 8u) && (!(c->wr[3] & 0x20u) || c->in_cts);
}

static uint8_t ext_inputs(const zr_sio_chan *c) {
    return (uint8_t)((c->in_cts ? ZR_SIO_RR0_CTS : 0) | (c->in_dcd ? ZR_SIO_RR0_DCD : 0) |
                     (c->in_sync ? ZR_SIO_RR0_SYNC : 0) | (c->rx_in_break ? ZR_SIO_RR0_BREAK : 0));
}

/* An external/status source changed: with the interrupt enabled the new
 * state is latched (frozen in RR0) and the interrupt pending until the
 * reset external/status command; a second change while latched is not seen. */
static void ext_refresh(zr_sio_chan *c) {
    const uint8_t now = ext_inputs(c);
    if (now == c->ext_live) return;
    c->ext_live = now;
    if ((c->wr[1] & 1u) && !c->ext_latched) {
        c->ext_latched = 1;
        c->ext_snap = now;
        c->ext_ip = 1;
    }
}

static void chan_reset(zr_sio_chan *c) {
    const uint8_t cts = c->in_cts, dcd = c->in_dcd, sync = c->in_sync, hold = c->rx_hold, rts = c->rts_out, dtr = c->dtr_out;
    const zr_sio_clock rxc = c->rx_clk, txc = c->tx_clk;
    memset(c, 0, sizeof *c);
    c->rts_out = rts;                           /* the pins as last reported: update_outputs brings them down */
    c->dtr_out = dtr;
    c->in_cts = cts;
    c->in_dcd = dcd;
    c->in_sync = sync;
    c->rx_hold = hold;
    c->rx_clk = rxc;
    c->tx_clk = txc;
    c->rx_snap.den = 1;
    c->rx_armed = 1;
    c->rx_done_at = c->rx_free_at = c->tx_start_at = c->tx_end_at = ZR_NEVER;
    c->ext_live = ext_inputs(c);
}

/* --- interrupt logic ------------------------------------------------------------------------------ */

/* The character at the head of the FIFO carries a special receive condition:
 * overrun, framing error, and parity error only in the "parity affects
 * vector" mode (the data sheet: not in first-character mode after the first). */
static int rx_special(const zr_sio_chan *c) {
    if (!c->used) return 0;
    if (c->overrun) return 1;
    const uint8_t e = c->fifo_err[c->head];
    if (e & ERR_FRAMING) return 1;
    return (e & ERR_PARITY) && ((c->wr[1] >> 3) & 3u) == 2u;
}

static int rx_pending(const zr_sio_chan *c) {
    const unsigned mode = (c->wr[1] >> 3) & 3u;
    if (mode == 0 || !c->used) return 0;
    return mode >= 2 || c->rx_armed || rx_special(c);
}

/* Interrupt sources in priority order: bit 0 receive A, 1 transmit A,
 * 2 external A, 3 receive B, 4 transmit B, 5 external B. */
static unsigned pending_mask(const zr_sio *s) {
    unsigned m = 0;
    for (unsigned i = 0; i < 2; i++) {
        const zr_sio_chan *c = &s->ch[i];
        if (rx_pending(c)) m |= 1u << (3 * i);
        if ((c->wr[1] & 2u) && c->tx_ip) m |= 1u << (3 * i + 1);
        if ((c->wr[1] & 1u) && c->ext_ip) m |= 1u << (3 * i + 2);
    }
    return m;
}

/* The level an acknowledge would serve: the highest source that is pending
 * and not blocked by an equal or higher one under service; -1 for none. */
static int winner(const zr_sio *s) {
    const unsigned p = pending_mask(s);
    for (unsigned j = 0; j < 6; j++) {
        if (s->in_service >> j & 1u) return -1;
        if (p >> j & 1u) return (int)j;
    }
    return -1;
}

static uint8_t vector_for(const zr_sio *s, int w) {
    const uint8_t v = s->ch[1].wr[2];
    if (!(s->ch[1].wr[1] & 4u)) return v;
    unsigned code;
    switch (w) {
    case 0: code = rx_special(&s->ch[0]) ? 7u : 6u; break;
    case 1: code = 4; break;
    case 2: code = 5; break;
    case 3: code = rx_special(&s->ch[1]) ? 3u : 2u; break;
    case 4: code = 0; break;
    case 5: code = 1; break;
    default: code = 3; break;                   /* nothing pending: the special receive code of B */
    }
    return (uint8_t)((v & 0xF1u) | code << 1);
}

/* --- receiver -------------------------------------------------------------------------------------- */

static int rx_ready_now(const zr_sio_chan *c) {
    return c->rx_state == RX_IDLE && rx_enabled(c) && !c->rx_in_break && !c->rx_hold && c->rx_clk.num != 0;
}

static void rx_abort(zr_sio_chan *c) {
    c->rx_state = RX_IDLE;
    c->rx_done_at = c->rx_free_at = ZR_NEVER;
}

/* The line level a frame has at master time t (idle after its last bit). */
static unsigned frame_bit(const zr_sio_frame *f, uint64_t t) {
    if (t < f->start) return 1;
    const uint64_t i = (t - f->start) * f->bit_den / f->bit_num;
    if (i >= f->length) return 1;
    return (f->bits >> i) & 1u;
}

/* Starts receiving `f`: the first clock edge at or after its start finds the
 * low start bit; the middle of the start bit and of every later bit is
 * divisor/2 + n * divisor edges on. */
static void rx_accept(zr_sio *s, unsigned chn, const zr_sio_frame *f) {
    zr_sio_chan *c = &s->ch[chn];
    const uint64_t now = zr_dev_now(&s->dev);
    const uint64_t t = f->start > now ? f->start : now;
    const unsigned d = divisor(c), h = d > 1 ? d >> 1 : 0;
    c->rx_nb = (uint8_t)char_bits(c->wr[3] >> 6);
    c->rx_par = c->wr[4] & 1u;
    c->rx_d = (uint8_t)d;
    c->rx_frame = *f;
    c->rx_snap = c->rx_clk;
    c->rx_k0 = edge_after(&c->rx_clk, t, 0);
    c->rx_state = RX_FRAME;
    c->rx_done_at = edge_time(&c->rx_snap, c->rx_k0 + h + (uint64_t)(1u + c->rx_nb + c->rx_par) * d);
}

static void rx_push(const zr_sio *s, zr_sio_chan *c, uint8_t data, uint8_t err) {
    if (c->used < 3) {
        const unsigned i = (c->head + c->used) % 3u;
        c->fifo[i] = data;
        c->fifo_err[i] = err;
        c->used++;
        return;
    }
    c->overrun = 1;
    if (!s->overrun_keeps) {                    /* the data sheet: the new character replaces the last one */
        c->fifo[(c->head + 2u) % 3u] = data;
        c->fifo_err[(c->head + 2u) % 3u] = err;
    }
}

/* The stop bit of the frame has been sampled. */
static void rx_complete(zr_sio *s, unsigned chn) {
    zr_sio_chan *c = &s->ch[chn];
    const zr_sio_frame *f = &c->rx_frame;
    const uint64_t k0 = c->rx_k0;
    const unsigned d = c->rx_d, h = d > 1 ? d >> 1 : 0, nb = c->rx_nb, par = c->rx_par;
    const uint64_t t_done = c->rx_done_at;
    c->rx_done_at = ZR_NEVER;
    c->rx_state = RX_IDLE;
    if (!rx_enabled(c)) return;
    if (f->brk && !c->rx_hold) return;
    /* A transient low is not a start bit. */
    if (frame_bit(f, edge_time(&c->rx_snap, k0 + h))) return;
    unsigned data = 0, px = 0;
    for (unsigned i = 0; i < nb; i++) {
        const unsigned bit = frame_bit(f, edge_time(&c->rx_snap, k0 + h + (uint64_t)(1u + i) * d));
        data |= bit << i;
        px ^= bit;
    }
    unsigned pe = 0;
    if (par) {
        const unsigned pb = frame_bit(f, edge_time(&c->rx_snap, k0 + h + (uint64_t)(1u + nb) * d));
        pe = pb != (px ^ ((c->wr[4] >> 1 & 1u) ^ 1u));     /* even parity (WR4 bit 1) expects the XOR of the data */
    }
    const unsigned sb = frame_bit(f, edge_time(&c->rx_snap, k0 + h + (uint64_t)(1u + nb + par) * d));
    const unsigned fe = !sb;
    rx_push(s, c, (uint8_t)data, (uint8_t)((pe ? ERR_PARITY : 0) | (fe ? ERR_FRAMING : 0)));
    if (pe) c->parity_latch = 1;
    if (f->brk && fe) {
        c->rx_in_break = 1;
        ext_refresh(c);
    }
    if (fe && !f->brk) {                        /* a framing error delays the search for the next start bit by half a bit */
        c->rx_state = RX_RECOVER;
        c->rx_free_at = t_done + f->bit_num / (2u * f->bit_den);
    }
}

/* A held break on RxD becomes one all-zero frame once the receiver is free. */
static void rx_break_poll(zr_sio *s, unsigned chn) {
    zr_sio_chan *c = &s->ch[chn];
    if (!c->rx_hold || c->rx_in_break || c->rx_state != RX_IDLE || !rx_enabled(c) || c->rx_clk.num == 0) return;
    zr_sio_frame f;
    memset(&f, 0, sizeof f);
    f.length = (uint8_t)(1u + char_bits(c->wr[3] >> 6) + (c->wr[4] & 1u) + 1u);
    f.brk = 1;
    f.start = zr_dev_now(&s->dev);
    f.bit_num = (uint64_t)divisor(c) * c->rx_clk.num;
    f.bit_den = c->rx_clk.den;
    rx_accept(s, chn, &f);
}

/* --- transmitter ----------------------------------------------------------------------------------- */

/* A character waiting in the buffer starts on the first clock edge after now. */
static void tx_arm(zr_sio *s, unsigned chn) {
    zr_sio_chan *c = &s->ch[chn];
    if (c->tx_active || !c->tx_full || !tx_enabled(c) || c->tx_clk.num == 0) {
        c->tx_start_at = ZR_NEVER;
        return;
    }
    if (c->tx_start_at != ZR_NEVER) return;
    c->tx_start_at = edge_time(&c->tx_clk, edge_after(&c->tx_clk, zr_dev_now(&s->dev), 1));
}

/* The character in the buffer goes to the shift register at the clock edge at
 * time `t`: the buffer is empty again (Tx interrupt), the start bit is on the
 * line and the frame is handed to the link. */
static void tx_begin(zr_sio *s, unsigned chn, uint64_t t) {
    zr_sio_chan *c = &s->ch[chn];
    c->tx_start_at = ZR_NEVER;
    if (c->tx_active || !c->tx_full || !tx_enabled(c) || c->tx_clk.num == 0) return;
    const unsigned d = divisor(c), nb = char_bits(c->wr[5] >> 5), par = c->wr[4] & 1u, even = c->wr[4] >> 1 & 1u;
    const unsigned data = c->tx_buf & ((1u << nb) - 1u);
    zr_sio_frame f;
    memset(&f, 0, sizeof f);
    unsigned px = 0, pos = 1;
    for (unsigned i = 0; i < nb; i++) {
        const unsigned bit = data >> i & 1u;
        f.bits |= (uint16_t)(bit << pos);
        px ^= bit;
        pos++;
    }
    if (par) {
        f.bits |= (uint16_t)((even ? px : px ^ 1u) << pos);
        pos++;
    }
    f.bits |= (uint16_t)(1u << pos);            /* the stop bit */
    pos++;
    f.length = (uint8_t)pos;
    f.data = (uint8_t)data;
    f.nbits = (uint8_t)nb;
    f.start = t;
    f.bit_num = (uint64_t)d * c->tx_clk.num;
    f.bit_den = c->tx_clk.den;
    const unsigned sm = c->wr[4] >> 2 & 3u;
    const unsigned stop = sm == 2 ? d + (d >> 1) : sm == 3 ? 2u * d : d;
    const uint64_t k = edge_after(&c->tx_clk, t, 0);
    c->tx_end_at = edge_time(&c->tx_clk, k + (uint64_t)(pos - 1u) * d + stop);
    c->tx_active = 1;
    c->tx_full = 0;
    c->tx_ip = 1;
    if (s->link.tx_frame) s->link.tx_frame(s->link.ctx, chn, &f);
}

/* --- outputs, deadline, notification ---------------------------------------------------------------- */

static void update_outputs(zr_sio *s) {
    const uint8_t irq = winner(s) >= 0;
    if (irq != s->int_out) {
        s->int_out = irq;
        zr_dev_line(&s->dev, ZR_SIO_OUT_INT, irq);
    }
    for (unsigned i = 0; i < 2; i++) {
        zr_sio_chan *c = &s->ch[i];
        /* RTS follows its bit; when the bit is reset the output stays low until the transmitter is empty. */
        const uint8_t rts = (uint8_t)((c->wr[5] & 2u) || (c->rts_out && (c->tx_full || c->tx_active)));
        const uint8_t dtr = (c->wr[5] & 0x80u) != 0;
        if (rts != c->rts_out) {
            c->rts_out = rts;
            zr_dev_line(&s->dev, ZR_SIO_OUT_RTS(i), rts);
        }
        if (dtr != c->dtr_out) {
            c->dtr_out = dtr;
            zr_dev_line(&s->dev, ZR_SIO_OUT_DTR(i), dtr);
        }
    }
}

static void rearm(zr_sio *s) {
    uint64_t t = ZR_NEVER;
    for (unsigned i = 0; i < 2; i++) {
        const zr_sio_chan *c = &s->ch[i];
        if (c->tx_start_at < t) t = c->tx_start_at;
        if (c->tx_end_at < t) t = c->tx_end_at;
        if (c->rx_done_at < t) t = c->rx_done_at;
        if (c->rx_free_at < t) t = c->rx_free_at;
    }
    zr_dev_schedule(&s->dev, t);
}

static void settle(zr_sio *s) {
    for (unsigned i = 0; i < 2; i++) rx_break_poll(s, i);
    update_outputs(s);
    rearm(s);
}

/* Tells the link when a receiver has become able to take a frame. */
static void notify_ready(zr_sio *s) {
    for (unsigned i = 0; i < 2; i++) {
        zr_sio_chan *c = &s->ch[i];
        if (!rx_ready_now(c)) {
            c->rx_ready_reported = 0;
        } else if (!c->rx_ready_reported) {
            c->rx_ready_reported = 1;
            if (s->link.rx_ready) s->link.rx_ready(s->link.ctx, i);
        }
    }
}

static void finish(zr_sio *s) {
    settle(s);
    notify_ready(s);
}

/* --- registers ---------------------------------------------------------------------------------------- */

static void channel_reset(zr_sio *s, unsigned chn) {
    zr_sio_chan *c = &s->ch[chn];
    const int was_break = c->tx_break_on;
    s->in_service &= (uint8_t)~(7u << (3 * chn));
    chan_reset(c);
    if (was_break && s->link.tx_break) s->link.tx_break(s->link.ctx, chn, 0);
}

static void return_from_interrupt(zr_sio *s) {
    for (unsigned j = 0; j < 6; j++) {
        if (s->in_service >> j & 1u) {
            s->in_service &= (uint8_t)~(1u << j);
            return;
        }
    }
}

static void write_register(zr_sio *s, unsigned chn, unsigned reg, uint8_t v) {
    zr_sio_chan *c = &s->ch[chn];
    const uint8_t old = c->wr[reg];
    c->wr[reg] = v;
    if (reg == 3 || reg == 5) {
        if (!rx_enabled(c) && c->rx_state != RX_IDLE) rx_abort(c);
        if (!tx_enabled(c)) c->tx_start_at = ZR_NEVER;
        tx_arm(s, chn);
    }
    if (reg == 5 && ((old ^ v) & 0x10u)) {
        c->tx_break_on = (v >> 4) & 1u;
        if (s->link.tx_break) s->link.tx_break(s->link.ctx, chn, c->tx_break_on);
    }
}

static void write_control(zr_sio *s, unsigned chn, uint8_t v) {
    zr_sio_chan *c = &s->ch[chn];
    if (c->ptr != 0) {
        const unsigned reg = c->ptr;
        c->ptr = 0;
        write_register(s, chn, reg, v);
        return;
    }
    c->wr[0] = v;
    c->ptr = v & 7u;
    switch ((v >> 3) & 7u) {
    case 2:                                     /* reset external/status interrupts */
        c->ext_latched = 0;
        c->ext_ip = 0;
        break;
    case 3:                                     /* channel reset */
        channel_reset(s, chn);
        break;
    case 4:                                     /* enable interrupt on next receive character */
        c->rx_armed = 1;
        break;
    case 5:                                     /* reset Tx interrupt pending */
        c->tx_ip = 0;
        break;
    case 6:                                     /* error reset */
        c->overrun = 0;
        c->parity_latch = 0;
        if (c->used) c->fifo_err[c->head] = 0;
        break;
    case 7:                                     /* return from interrupt */
        return_from_interrupt(s);
        break;
    default:                                    /* null, send abort (synchronous only) */
        break;
    }
}

static uint8_t read_control(zr_sio *s, unsigned chn) {
    zr_sio_chan *c = &s->ch[chn];
    const unsigned reg = c->ptr;
    c->ptr = 0;
    switch (reg) {
    case 0: {
        const uint8_t ext = (uint8_t)((c->ext_latched ? c->ext_snap : c->ext_live) & 0xB8u);
        return (uint8_t)(ext | (c->tx_full ? 0 : ZR_SIO_RR0_TXEMPTY) | (c->used ? ZR_SIO_RR0_RXAVAIL : 0) |
                         (chn == 0 && pending_mask(s) ? ZR_SIO_RR0_INTPEND : 0));
    }
    case 1: {
        uint8_t v = (!c->tx_full && !c->tx_active) ? ZR_SIO_RR1_ALLSENT : 0;
        const uint8_t e = c->used ? c->fifo_err[c->head] : 0;
        if (c->parity_latch || (e & ERR_PARITY)) v |= ZR_SIO_RR1_PARITY;
        if (c->overrun) v |= ZR_SIO_RR1_OVERRUN;
        if (e & ERR_FRAMING) v |= ZR_SIO_RR1_FRAMING;
        return v;
    }
    case 2:
        return vector_for(s, winner(s));
    default:
        return 0;
    }
}

static uint8_t read_data(zr_sio_chan *c) {
    if (!c->used) return 0;
    const uint8_t v = c->fifo[c->head];
    c->head = (uint8_t)((c->head + 1u) % 3u);
    c->used--;
    c->rx_armed = 0;
    return v;
}

/* --- bus and events ------------------------------------------------------------------------------------ */

static uint8_t sio_read(void *d, unsigned space, uint32_t offset) {
    zr_sio *s = d;
    (void)space;
    const unsigned chn = offset >> 1 & 1u;
    const uint8_t v = (offset & 1u) ? read_control(s, chn) : read_data(&s->ch[chn]);
    finish(s);
    return v;
}

static void sio_write(void *d, unsigned space, uint32_t offset, uint8_t value) {
    zr_sio *s = d;
    (void)space;
    const unsigned chn = offset >> 1 & 1u;
    zr_sio_chan *c = &s->ch[chn];
    if (offset & 1u) {
        write_control(s, chn, value);
    } else {
        c->tx_buf = value;
        c->tx_full = 1;
        c->tx_ip = 0;
        tx_arm(s, chn);
    }
    finish(s);
}

static void sio_event(void *d, uint64_t now) {
    zr_sio *s = d;
    for (;;) {
        uint64_t best = ZR_NEVER;
        unsigned bc = 0, kind = 0;
        for (unsigned i = 0; i < 2; i++) {
            const zr_sio_chan *c = &s->ch[i];
            if (c->tx_start_at < best) { best = c->tx_start_at; bc = i; kind = 0; }
            if (c->tx_end_at < best) { best = c->tx_end_at; bc = i; kind = 1; }
            if (c->rx_done_at < best) { best = c->rx_done_at; bc = i; kind = 2; }
            if (c->rx_free_at < best) { best = c->rx_free_at; bc = i; kind = 3; }
        }
        if (best > now) break;
        zr_sio_chan *c = &s->ch[bc];
        switch (kind) {
        case 0:
            tx_begin(s, bc, best);
            break;
        case 1:
            c->tx_end_at = ZR_NEVER;
            c->tx_active = 0;
            tx_begin(s, bc, best);
            break;
        case 2:
            rx_complete(s, bc);
            break;
        default:
            c->rx_free_at = ZR_NEVER;
            c->rx_state = RX_IDLE;
            break;
        }
    }
    finish(s);
}

static void sio_reset(void *d) {
    zr_sio *s = d;
    for (unsigned i = 0; i < 2; i++) channel_reset(s, i);
    s->in_service = 0;
    finish(s);
}

static void sio_input(void *d, int pin, int state) {
    zr_sio *s = d;
    if (pin < 0 || pin > 5) return;
    const unsigned chn = (unsigned)pin & 1u;
    zr_sio_chan *c = &s->ch[chn];
    const uint8_t v = state != 0;
    switch (pin >> 1) {
    case 0: c->in_cts = v; break;
    case 1: c->in_dcd = v; break;
    default: c->in_sync = v; break;
    }
    ext_refresh(c);
    if (!rx_enabled(c) && c->rx_state != RX_IDLE) rx_abort(c);
    if (!tx_enabled(c)) c->tx_start_at = ZR_NEVER;
    tx_arm(s, chn);
    finish(s);
}

/* --- interrupts ---------------------------------------------------------------------------------------- */

static unsigned sio_status(const void *d) {
    const zr_sio *s = d;
    return (winner(s) >= 0 ? ZR_DAISY_REQ : 0u) | (s->in_service ? ZR_DAISY_IUS : 0u);
}

static uint8_t sio_ack(void *d) {
    zr_sio *s = d;
    const int w = winner(s);
    const uint8_t v = vector_for(s, w);
    if (w >= 0) {
        s->in_service |= (uint8_t)(1u << w);
        finish(s);
    }
    return v;
}

static void sio_reti(void *d) {
    zr_sio *s = d;
    return_from_interrupt(s);
    finish(s);
}

static const zr_daisy_ops SIO_DAISY_OPS = {.status = sio_status, .ack = sio_ack, .reti = sio_reti};

/* --- state --------------------------------------------------------------------------------------------- */

enum { CHAN_BYTES = 41 };

/* The one-byte fields of a channel in the order of the state chunk. */
static void chan_bytes(zr_sio_chan *c, uint8_t *p[CHAN_BYTES]) {
    size_t n = 0;
    for (unsigned i = 0; i < 8; i++) p[n++] = &c->wr[i];
    uint8_t *const rest[] = {&c->ptr, &c->in_cts, &c->in_dcd, &c->in_sync, &c->ext_live, &c->ext_snap, &c->ext_latched,
                             &c->ext_ip, &c->tx_ip, &c->rx_armed, &c->fifo[0], &c->fifo[1], &c->fifo[2],
                             &c->fifo_err[0], &c->fifo_err[1], &c->fifo_err[2], &c->head, &c->used, &c->overrun,
                             &c->parity_latch, &c->rx_state, &c->rx_hold, &c->rx_in_break, &c->rx_ready_reported,
                             &c->rx_d, &c->rx_nb, &c->rx_par, &c->tx_buf, &c->tx_full, &c->tx_active, &c->tx_break_on,
                             &c->rts_out, &c->dtr_out};
    for (size_t i = 0; i < sizeof rest / sizeof rest[0]; i++) p[n++] = rest[i];
}

static void put_clock(zr_state_writer *w, const zr_sio_clock *k) {
    zr_sw_u32(w, k->num); zr_sw_u32(w, k->den); zr_sw_u64(w, k->epoch);
}
static void get_clock(zr_state_reader *r, zr_sio_clock *k) {
    k->num = zr_sr_u32(r); k->den = zr_sr_u32(r); k->epoch = zr_sr_u64(r);
}

static void sio_save(const void *d, zr_state_writer *w) {
    const zr_sio *s = d;
    zr_sw_u32(w, s->master_hz);
    zr_sw_u8(w, s->in_service);
    zr_sw_u8(w, s->int_out);
    for (unsigned i = 0; i < 2; i++) {
        zr_sio_chan c = s->ch[i];                   /* a copy to read the fields through pointers */
        uint8_t *p[CHAN_BYTES];
        uint8_t b[CHAN_BYTES];
        chan_bytes(&c, p);
        for (size_t k = 0; k < CHAN_BYTES; k++) b[k] = *p[k];
        zr_sw_bytes(w, b, sizeof b);
        zr_sw_u16(w, c.rx_frame.bits);
        zr_sw_u8(w, c.rx_frame.length); zr_sw_u8(w, c.rx_frame.data); zr_sw_u8(w, c.rx_frame.nbits); zr_sw_u8(w, c.rx_frame.brk);
        zr_sw_u64(w, c.rx_frame.start); zr_sw_u64(w, c.rx_frame.bit_num); zr_sw_u64(w, c.rx_frame.bit_den);
        put_clock(w, &c.rx_snap); put_clock(w, &c.rx_clk); put_clock(w, &c.tx_clk);
        zr_sw_u64(w, c.rx_k0); zr_sw_u64(w, c.rx_done_at); zr_sw_u64(w, c.rx_free_at);
        zr_sw_u64(w, c.tx_start_at); zr_sw_u64(w, c.tx_end_at);
    }
}

static int chan_valid(const zr_sio_chan *c) {
    if (c->ptr > 7 || c->head > 2 || c->used > 3 || c->rx_state > RX_RECOVER) return 0;
    const uint8_t flags[] = {c->in_cts, c->in_dcd, c->in_sync, c->ext_latched, c->ext_ip, c->tx_ip, c->rx_armed, c->overrun,
                             c->parity_latch, c->rx_hold, c->rx_in_break, c->rx_ready_reported, c->tx_full, c->tx_active,
                             c->tx_break_on, c->rts_out, c->dtr_out, c->rx_par, c->rx_frame.brk};
    for (size_t i = 0; i < sizeof flags; i++) if (flags[i] > 1) return 0;
    if (c->rx_frame.length > 16 || c->rx_nb > 8 || c->rx_d > 64) return 0;
    if (!c->rx_snap.den || !c->rx_clk.den || !c->tx_clk.den) return 0;
    if (c->rx_state == RX_FRAME && (!c->rx_snap.num || !c->rx_frame.bit_num || !c->rx_frame.bit_den || !c->rx_d)) return 0;
    if (c->rx_state == RX_RECOVER && !c->rx_frame.bit_den) return 0;
    return 1;
}

static int sio_load(void *d, zr_state_reader *r) {
    zr_sio *s = d;
    const uint32_t master_hz = zr_sr_u32(r);
    const uint8_t in_service = zr_sr_u8(r), int_out = zr_sr_u8(r);
    zr_sio_chan in[2];
    for (unsigned i = 0; i < 2; i++) {
        zr_sio_chan *c = &in[i];
        uint8_t *p[CHAN_BYTES];
        uint8_t b[CHAN_BYTES];
        memset(c, 0, sizeof *c);
        zr_sr_bytes(r, b, sizeof b);
        chan_bytes(c, p);
        for (size_t k = 0; k < CHAN_BYTES; k++) *p[k] = b[k];
        c->rx_frame.bits = zr_sr_u16(r);
        c->rx_frame.length = zr_sr_u8(r); c->rx_frame.data = zr_sr_u8(r); c->rx_frame.nbits = zr_sr_u8(r); c->rx_frame.brk = zr_sr_u8(r);
        c->rx_frame.start = zr_sr_u64(r); c->rx_frame.bit_num = zr_sr_u64(r); c->rx_frame.bit_den = zr_sr_u64(r);
        get_clock(r, &c->rx_snap); get_clock(r, &c->rx_clk); get_clock(r, &c->tx_clk);
        c->rx_k0 = zr_sr_u64(r); c->rx_done_at = zr_sr_u64(r); c->rx_free_at = zr_sr_u64(r);
        c->tx_start_at = zr_sr_u64(r); c->tx_end_at = zr_sr_u64(r);
        if (!zr_sr_ok(r) || !chan_valid(c)) return 0;
    }
    if (!zr_sr_ok(r) || in_service > 0x3F || int_out > 1) return 0;
    s->master_hz = master_hz;
    s->in_service = in_service;
    s->int_out = int_out;
    memcpy(s->ch, in, sizeof in);
    rearm(s);
    return 1;
}

static const zr_dev_ops SIO_OPS = {
    .read = sio_read, .write = sio_write, .event = sio_event, .reset = sio_reset, .input = sio_input,
    .save = sio_save, .load = sio_load,
};

/* --- the board's calls ------------------------------------------------------------------------------------ */

void zr_sio_init(zr_sio *s, const zr_hw_host *host, const char *name, uint32_t master_hz) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &SIO_OPS;
    s->dev.host = host;
    s->dev.name = name ? name : "z80sio";
    s->daisy.ops = &SIO_DAISY_OPS;
    s->daisy.chip = s;
    s->master_hz = master_hz;
    for (unsigned i = 0; i < 2; i++) {
        s->ch[i].rx_clk.den = s->ch[i].tx_clk.den = 1;
        chan_reset(&s->ch[i]);
    }
}

void zr_sio_set_link(zr_sio *s, const zr_sio_link *link) {
    if (link) s->link = *link;
    else memset(&s->link, 0, sizeof s->link);
}

void zr_sio_set_overrun_keeps(zr_sio *s, int keeps) {
    s->overrun_keeps = keeps != 0;
}

void zr_sio_set_clock(zr_sio *s, unsigned channel, unsigned which, uint32_t num, uint32_t den) {
    if (channel > 1) return;
    zr_sio_chan *c = &s->ch[channel];
    zr_sio_clock n = {0, 1, zr_dev_now(&s->dev)};
    if (num && den) {                           /* the smallest terms: the frames' bit times stay small numbers */
        uint32_t a = num, b = den;
        while (b) { const uint32_t r = a % b; a = b; b = r; }
        n.num = num / a;
        n.den = den / a;
    }
    if (which & ZR_SIO_RXC) c->rx_clk = n;
    if (which & ZR_SIO_TXC) {
        c->tx_clk = n;
        c->tx_start_at = ZR_NEVER;
        tx_arm(s, channel);
    }
    finish(s);
}

void zr_sio_set_clock_hz(zr_sio *s, unsigned channel, unsigned which, uint32_t hz) {
    if (hz && s->master_hz) zr_sio_set_clock(s, channel, which, s->master_hz, hz);
    else zr_sio_set_clock(s, channel, which, 0, 0);
}

int zr_sio_rx_ready(const zr_sio *s, unsigned channel) {
    return channel < 2 && rx_ready_now(&s->ch[channel]);
}

int zr_sio_rx_frame(zr_sio *s, unsigned channel, const zr_sio_frame *frame) {
    if (channel > 1 || !frame->bit_num || !frame->bit_den || !rx_ready_now(&s->ch[channel])) return 0;
    rx_accept(s, channel, frame);
    finish(s);
    return 1;
}

void zr_sio_make_frame(const zr_sio *s, unsigned channel, uint8_t value, uint64_t start, zr_sio_frame *frame) {
    const zr_sio_chan *c = &s->ch[channel & 1u];
    const unsigned nb = char_bits(c->wr[3] >> 6), par = c->wr[4] & 1u, even = c->wr[4] >> 1 & 1u;
    const unsigned data = value & ((1u << nb) - 1u);
    unsigned px = 0, pos = 1;
    memset(frame, 0, sizeof *frame);
    for (unsigned i = 0; i < nb; i++) {
        const unsigned bit = data >> i & 1u;
        frame->bits |= (uint16_t)(bit << pos);
        px ^= bit;
        pos++;
    }
    if (par) {
        frame->bits |= (uint16_t)((even ? px : px ^ 1u) << pos);
        pos++;
    }
    frame->bits |= (uint16_t)(1u << pos);
    pos++;
    frame->length = (uint8_t)pos;
    frame->data = (uint8_t)data;
    frame->nbits = (uint8_t)nb;
    frame->start = start;
    frame->bit_num = (uint64_t)divisor(c) * c->rx_clk.num;
    frame->bit_den = c->rx_clk.den;
}

void zr_sio_rx_break(zr_sio *s, unsigned channel, int on) {
    if (channel > 1) return;
    zr_sio_chan *c = &s->ch[channel];
    c->rx_hold = on != 0;
    if (!on) {
        if (c->rx_state == RX_FRAME && c->rx_frame.brk) rx_abort(c);
        c->rx_in_break = 0;
        ext_refresh(c);
    }
    finish(s);
}

uint8_t zr_sio_vector(const zr_sio *s) { return vector_for(s, winner(s)); }

unsigned zr_sio_rx_count(const zr_sio *s, unsigned channel) {
    return channel < 2 ? s->ch[channel].used : 0;
}

int zr_sio_irq(const zr_sio *s) { return winner(s) >= 0; }
int zr_sio_rts(const zr_sio *s, unsigned channel) { return channel < 2 && s->ch[channel].rts_out; }
int zr_sio_dtr(const zr_sio *s, unsigned channel) { return channel < 2 && s->ch[channel].dtr_out; }
