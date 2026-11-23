/* ZetRo - the Intel 8251A USART (i8251.h). Written from Intel's 8251A data
 * sheet (the mode and command instructions, the status register, the pins,
 * the asynchronous and synchronous modes, hunt). ZetRo's own code. */
#include "i8251.h"

#include "state.h"

#include <string.h>

/* --- the programmed format ---------------------------------------------------------------- */

static int is_sync(const zr_i8251 *u) { return (u->mode & 3u) == 0; }
/* Clock periods per bit: 1, 16 or 64 (synchronous: 1). */
static uint32_t factor(const zr_i8251 *u) {
    switch (u->mode & 3u) { case 2: return 16; case 3: return 64; default: return 1; }
}
static unsigned data_bits(const zr_i8251 *u) { return 5u + ((u->mode >> 2) & 3u); }
static unsigned has_parity(const zr_i8251 *u) { return (u->mode >> 4) & 1u; }
static uint8_t data_mask(const zr_i8251 *u) { return (uint8_t)(0xFFu >> (8u - data_bits(u))); }
static int external_sync(const zr_i8251 *u) { return is_sync(u) && (u->mode & 0x40); }
static int double_sync(const zr_i8251 *u) { return !(u->mode & 0x80); }

/* The clock periods of a character: asynchronous, start bit, data, parity,
 * the programmed stop bits (the receiver needs only one); synchronous, data
 * and parity. */
static uint64_t tx_periods(const zr_i8251 *u) {
    const uint32_t f = factor(u);
    if (is_sync(u)) return data_bits(u) + has_parity(u);
    uint32_t stop;
    switch ((u->mode >> 6) & 3u) { case 3: stop = 2u * f; break; case 2: stop = f + f / 2u; break; default: stop = f; break; }
    return (uint64_t)(1u + data_bits(u) + has_parity(u)) * f + stop;
}
static uint64_t rx_periods(const zr_i8251 *u) {
    if (is_sync(u)) return data_bits(u) + has_parity(u);
    return (uint64_t)(2u + data_bits(u) + has_parity(u)) * factor(u);
}

/* --- clocks --------------------------------------------------------------------------------- */

/* floor(v * mul / div), exact while mul * div < 2^64. */
static uint64_t muldiv(uint64_t v, uint32_t mul, uint32_t div) {
    return (v / div) * mul + (v % div) * mul / div;
}
/* The edges of the clock at or before `now` (edge k is at epoch + ceil(k * num / den)). */
static uint64_t clk_ticks(const zr_i8251_clock *c, uint64_t now) {
    return (c->num && now > c->epoch) ? muldiv(now - c->epoch, c->den, c->num) : 0;
}
/* The master time of edge k (ZR_NEVER on a stopped clock). */
static uint64_t clk_time(const zr_i8251_clock *c, uint64_t k) {
    if (!c->num) return ZR_NEVER;
    const uint64_t q = k / c->den, r = k % c->den;
    return c->epoch + q * c->num + (r * c->num + c->den - 1u) / c->den;
}
/* The first edge at or after `now`. */
static uint64_t clk_ceil(const zr_i8251_clock *c, uint64_t now) {
    return (c->num && now > c->epoch) ? muldiv(now - c->epoch - 1u, c->den, c->num) + 1u : 0;
}
/* The edges that are left of a character ending at edge `end`, from `now`. */
static uint64_t clk_left(const zr_i8251_clock *c, uint64_t end, uint64_t now) {
    const uint64_t t = clk_ticks(c, now);
    return end > t ? end - t : 0;
}

/* --- the status and the pins -------------------------------------------------------------- */

static int tx_enabled(const zr_i8251 *u) { return (u->command & 1u) && u->cts; }
static int tx_break(const zr_i8251 *u) { return !is_sync(u) && (u->command & 8u); }
static int rx_ready(const zr_i8251 *u) { return u->rx_full && (u->command & 4u); }
/* No character to send: nothing in the shifter but a filler, and nothing the
 * transmitter would take from the buffer (a disabled one holds on to it and
 * TxEMPTY stays high). */
static int tx_empty(const zr_i8251 *u) {
    return !(u->tx_busy && u->tx_data) && !(u->tx_full && (tx_enabled(u) || u->tx_drain));
}
/* The SYNDET/BRKDET output: BRKDET in asynchronous mode, SYNDET in
 * synchronous mode unless it is an input. */
static int syndet_pin(const zr_i8251 *u) {
    if (!is_sync(u)) return u->brk;
    return !external_sync(u) && u->syndet;
}

static uint8_t pin_levels(const zr_i8251 *u) {
    uint8_t p = 0;
    if (!u->tx_full && tx_enabled(u)) p |= 1u << ZR_I8251_PIN_TXRDY;
    if (rx_ready(u)) p |= 1u << ZR_I8251_PIN_RXRDY;
    if (tx_empty(u)) p |= 1u << ZR_I8251_PIN_TXEMPTY;
    if (u->command & 2u) p |= 1u << ZR_I8251_PIN_DTR;
    if (u->command & 0x20u) p |= 1u << ZR_I8251_PIN_RTS;
    if (syndet_pin(u)) p |= 1u << ZR_I8251_PIN_SYNDET;
    if (!tx_break(u)) p |= 1u << ZR_I8251_PIN_TXD;
    return p;
}

/* Reports every output pin that changed, lowest pin first. The level is
 * recorded before the call and the levels are worked out again after it, so
 * the board may call back into the chip. */
static void update(zr_i8251 *u) {
    for (;;) {
        const uint8_t levels = pin_levels(u);
        const uint8_t diff = (uint8_t)(levels ^ u->reported);
        if (!diff) return;
        int pin = 0;
        while (!((diff >> pin) & 1)) ++pin;
        u->reported ^= (uint8_t)(1u << pin);
        zr_dev_line(&u->dev, pin, (levels >> pin) & 1);
    }
}

static uint8_t status(const zr_i8251 *u) {
    const int sd = is_sync(u) ? (external_sync(u) ? u->syndet_in : u->syndet) : u->brk;
    return (uint8_t)((!u->tx_full) | rx_ready(u) << 1 | tx_empty(u) << 2 | u->pe << 3 | u->oe << 4 | u->fe << 5 | sd << 6 | u->dsr << 7);
}

/* --- the receiver --------------------------------------------------------------------------- */

/* A character has been assembled (by the receiver, or by the loopback). */
static void rx_complete(zr_i8251 *u, uint8_t byte, unsigned flags) {
    if (!(u->command & 4u)) return;                 /* the receiver was disabled meanwhile */
    const uint8_t mask = data_mask(u);
    byte &= mask;
    if (is_sync(u)) {
        const uint8_t s1 = u->sync[0] & mask, s2 = u->sync[1] & mask;
        if (u->hunting) {
            if (external_sync(u)) {                 /* the SYNDET pin releases the hunt */
                if (!u->syndet_in) return;
                u->hunting = 0;
            } else {                                /* the sync characters are consumed, not read */
                if (!double_sync(u)) {
                    if (byte == s1) { u->hunting = 0; u->syndet = 1; }
                } else if (u->hunt_match && byte == s2) {
                    u->hunting = 0;
                    u->syndet = 1;
                    u->hunt_match = 0;
                } else {
                    u->hunt_match = byte == s1;
                }
                return;
            }
        } else if (!external_sync(u)) {             /* still detected at the character boundaries */
            if (!double_sync(u)) {
                if (byte == s1) u->syndet = 1;
            } else {
                if (u->hunt_match && byte == s2) u->syndet = 1;
                u->hunt_match = byte == s1;
            }
        }
    } else if (flags & ZR_I8251_ERR_BREAK) {
        u->brk = 1;
        flags |= ZR_I8251_ERR_FE;
        byte = 0;
    } else {
        u->brk = 0;                                 /* RxD is back at one */
    }
    if ((flags & ZR_I8251_ERR_PE) && has_parity(u)) u->pe = 1;
    if ((flags & ZR_I8251_ERR_FE) && !is_sync(u)) u->fe = 1;
    if (u->rx_full) u->oe = 1;                      /* the unread character is written over and lost */
    u->rx_data = byte;
    u->rx_full = 1;
}

static void rx_finish(zr_i8251 *u) {
    u->rx_busy = 0;
    rx_complete(u, u->rx_byte, u->rx_flags);
}

/* --- the transmitter ------------------------------------------------------------------------ */

/* The next character the transmitter takes, if it may start one: a filler
 * sync character, the buffered data, or (synchronous, after the first
 * character) a sync filler when nothing is waiting. */
static int tx_pick(zr_i8251 *u, uint8_t *byte, int *is_data) {
    const int en = tx_enabled(u);
    const int sync = is_sync(u);
    if (sync && u->tx_sync_next && en) {
        *byte = u->sync[1]; *is_data = 0; u->tx_sync_next = 0;
        return 1;
    }
    if (u->tx_full && (en || u->tx_drain) && !tx_break(u)) {
        *byte = u->tx_buf; *is_data = 1; u->tx_full = 0; u->tx_drain = 0;
        if (sync) u->tx_primed = 1;
        return 1;
    }
    if (sync && u->tx_primed && en) {
        *byte = u->sync[0]; *is_data = 0;
        if (double_sync(u)) u->tx_sync_next = 1;
        return 1;
    }
    return 0;
}

/* Starts the next character, if there is one, at TxC edge `start`. */
static void tx_begin(zr_i8251 *u, uint64_t start) {
    uint8_t byte;
    int is_data;
    if (!tx_pick(u, &byte, &is_data)) return;
    u->tx_busy = 1;
    u->tx_data = (uint8_t)is_data;
    u->tx_byte = (uint8_t)(byte & data_mask(u));
    u->tx_end = start + tx_periods(u);
}

/* Starts a character now if the shifter is idle. */
static void tx_kick(zr_i8251 *u, uint64_t now) {
    if (!u->tx_busy) tx_begin(u, clk_ceil(&u->clk[ZR_I8251_TXC], now));
}

/* The character in the shifter has gone out. */
static void tx_finish(zr_i8251 *u) {
    const uint8_t byte = u->tx_byte;
    const int is_data = u->tx_data;
    u->tx_busy = 0;
    tx_begin(u, u->tx_end);                         /* the next one follows on the same clock edge */
    if (is_data && u->transmit) u->transmit(u->transmit_ctx, byte);
    if (u->loopback) rx_complete(u, byte, 0);
}

/* The transmitter was enabled and now is not (TxEN or CTS fell): a
 * character already written still goes out (data sheet). */
static void note_disable(zr_i8251 *u, int was_enabled) {
    if (was_enabled && !tx_enabled(u) && u->tx_busy && u->tx_full) u->tx_drain = 1;
}

/* --- time ------------------------------------------------------------------------------------ */

static uint64_t tx_time(const zr_i8251 *u) { return u->tx_busy ? clk_time(&u->clk[ZR_I8251_TXC], u->tx_end) : ZR_NEVER; }
static uint64_t rx_time(const zr_i8251 *u) { return u->rx_busy ? clk_time(&u->clk[ZR_I8251_RXC], u->rx_end) : ZR_NEVER; }

/* Runs every character end at or before `now`, in time order. */
static void catch_up(zr_i8251 *u, uint64_t now) {
    for (;;) {
        const uint64_t tt = tx_time(u), rt = rx_time(u);
        if (tt <= now && tt <= rt) tx_finish(u);
        else if (rt <= now) rx_finish(u);
        else return;
    }
}

static void reschedule(zr_i8251 *u) {
    const uint64_t tt = tx_time(u), rt = rx_time(u);
    zr_dev_schedule(&u->dev, tt < rt ? tt : rt);
}

/* --- bus ------------------------------------------------------------------------------------- */

/* The RESET pin and the internal reset: the format and command are forgotten,
 * the buffers are empty and the line idle; what is wired (clocks, input pins,
 * the loopback plug, the callback) stays. */
static void state_reset(zr_i8251 *u) {
    u->mode = u->command = u->step = 0;
    u->tx_buf = u->tx_full = u->tx_busy = u->tx_data = u->tx_byte = 0;
    u->tx_drain = u->tx_primed = u->tx_sync_next = 0;
    u->rx_data = u->rx_full = u->rx_busy = u->rx_byte = u->rx_flags = 0;
    u->pe = u->oe = u->fe = u->brk = u->syndet = u->hunting = u->hunt_match = 0;
    u->tx_end = u->rx_end = 0;
}

static void command_write(zr_i8251 *u, uint8_t v, uint64_t now) {
    if (v & 0x40) { state_reset(u); return; }       /* internal reset: back to the mode instruction */
    const int was_enabled = tx_enabled(u);
    const uint8_t old = u->command;
    u->command = (uint8_t)(v & 0x2Fu);              /* TxEN, DTR, RxE, SBRK, RTS; ER and EH are acts, not state */
    if (v & 0x10u) u->pe = u->oe = u->fe = 0;
    if ((v & 0x80u) && is_sync(u)) {                /* enter hunt */
        u->hunting = 1;
        u->syndet = 0;
        u->hunt_match = 0;
        u->rx_data = data_mask(u);                  /* the buffer is set to ones: no false SYNDET from old data */
    }
    if ((old & 4u) && !(v & 4u)) u->rx_busy = 0;    /* the receiver is switched off: what it was assembling is lost */
    note_disable(u, was_enabled);
    tx_kick(u, now);                                /* TxEN set, or SBRK cleared, with a character waiting */
}

static void control_write(zr_i8251 *u, uint8_t v, uint64_t now) {
    switch (u->step) {
    case 0:
        u->mode = v;
        u->step = is_sync(u) ? 1 : 3;
        break;
    case 1:
        u->sync[0] = v;
        u->step = double_sync(u) ? 2 : 3;
        break;
    case 2:
        u->sync[1] = v;
        u->step = 3;
        break;
    default:
        command_write(u, v, now);
        break;
    }
}

static uint8_t i_read(void *dev, unsigned space, uint32_t offset) {
    zr_i8251 *u = dev;
    (void)space;
    catch_up(u, zr_dev_now(&u->dev));
    uint8_t v;
    if (offset & 1u) {
        v = status(u);
        u->syndet = 0;                              /* SYNDET is reset by a status read */
    } else {
        v = u->rx_data;
        u->rx_full = 0;
    }
    update(u);
    reschedule(u);
    return v;
}

static void i_write(void *dev, unsigned space, uint32_t offset, uint8_t value) {
    zr_i8251 *u = dev;
    (void)space;
    const uint64_t now = zr_dev_now(&u->dev);
    catch_up(u, now);
    if (offset & 1u) {
        control_write(u, value, now);
    } else {
        u->tx_buf = value;                          /* a character still in the buffer is written over */
        u->tx_full = 1;
        u->tx_drain = 0;
        tx_kick(u, now);
    }
    update(u);
    reschedule(u);
}

static void i_event(void *dev, uint64_t now) {
    zr_i8251 *u = dev;
    catch_up(u, now);
    update(u);
    reschedule(u);
}

static void i_reset(void *dev) {
    zr_i8251 *u = dev;
    state_reset(u);
    update(u);
    reschedule(u);
}

static void i_input(void *dev, int pin, int state) {
    zr_i8251 *u = dev;
    const uint64_t now = zr_dev_now(&u->dev);
    const uint8_t level = state != 0;
    catch_up(u, now);
    switch (pin) {
    case ZR_I8251_IN_CTS: {
        const int was_enabled = tx_enabled(u);
        u->cts = level;
        note_disable(u, was_enabled);
        tx_kick(u, now);
        break;
    }
    case ZR_I8251_IN_DSR:
        u->dsr = level;
        break;
    case ZR_I8251_IN_SYNDET:
        u->syndet_in = level;
        if (level && external_sync(u)) u->hunting = 0;  /* a high level on the pin ends the hunt */
        break;
    default:
        return;
    }
    update(u);
    reschedule(u);
}

/* --- state ------------------------------------------------------------------------------------ */

static void i_save(const void *dev, zr_state_writer *w) {
    const zr_i8251 *u = dev;
    zr_sw_u32(w, u->master_hz);
    for (int i = 0; i < 2; i++) {
        zr_sw_u32(w, u->clk[i].num); zr_sw_u32(w, u->clk[i].den); zr_sw_u64(w, u->clk[i].epoch);
    }
    const uint8_t bytes[] = {
        u->mode, u->command, u->step, u->sync[0], u->sync[1], u->loopback, u->cts, u->dsr, u->syndet_in,
        u->tx_buf, u->tx_full, u->tx_busy, u->tx_data, u->tx_byte, u->tx_drain, u->tx_primed, u->tx_sync_next,
        u->rx_data, u->rx_full, u->rx_busy, u->rx_byte, u->rx_flags,
        u->pe, u->oe, u->fe, u->brk, u->syndet, u->hunting, u->hunt_match, u->reported};
    zr_sw_bytes(w, bytes, sizeof bytes);
    zr_sw_u64(w, u->tx_end);
    zr_sw_u64(w, u->rx_end);
}

static int i_load(void *dev, zr_state_reader *r) {
    zr_i8251 *u = dev;
    uint32_t master_hz = zr_sr_u32(r);
    zr_i8251_clock clk[2];
    for (int i = 0; i < 2; i++) {
        clk[i].num = zr_sr_u32(r); clk[i].den = zr_sr_u32(r); clk[i].epoch = zr_sr_u64(r);
    }
    uint8_t b[30];
    zr_sr_bytes(r, b, sizeof b);
    const uint64_t tx_end = zr_sr_u64(r);
    const uint64_t rx_end = zr_sr_u64(r);
    if (!zr_sr_ok(r) || b[2] > 3 || b[5] > 1 || b[6] > 1 || b[7] > 1 || b[8] > 1 || b[10] > 1 || b[11] > 1 || b[12] > 1 ||
        b[14] > 1 || b[15] > 1 || b[16] > 1 || b[18] > 1 || b[19] > 1 || b[22] > 1 || b[23] > 1 || b[24] > 1 || b[25] > 1 ||
        b[26] > 1 || b[27] > 1 || b[28] > 1 || b[29] > 0x7F || (clk[0].num && !clk[0].den) || (clk[1].num && !clk[1].den))
        return 0;
    u->master_hz = master_hz;
    u->clk[0] = clk[0];
    u->clk[1] = clk[1];
    uint8_t *const fields[] = {
        &u->mode, &u->command, &u->step, &u->sync[0], &u->sync[1], &u->loopback, &u->cts, &u->dsr, &u->syndet_in,
        &u->tx_buf, &u->tx_full, &u->tx_busy, &u->tx_data, &u->tx_byte, &u->tx_drain, &u->tx_primed, &u->tx_sync_next,
        &u->rx_data, &u->rx_full, &u->rx_busy, &u->rx_byte, &u->rx_flags,
        &u->pe, &u->oe, &u->fe, &u->brk, &u->syndet, &u->hunting, &u->hunt_match, &u->reported};
    for (size_t i = 0; i < sizeof b; ++i) *fields[i] = b[i];
    u->tx_end = tx_end;
    u->rx_end = rx_end;
    reschedule(u);
    return 1;
}

static const zr_dev_ops I8251_OPS = {
    .read = i_read, .write = i_write, .event = i_event, .reset = i_reset, .input = i_input,
    .save = i_save, .load = i_load,
};

/* --- the board's calls ---------------------------------------------------------------------------- */

void zr_i8251_init(zr_i8251 *u, const zr_hw_host *host, const char *name, uint32_t master_hz, uint32_t txc_hz, uint32_t rxc_hz) {
    memset(u, 0, sizeof *u);
    u->dev.ops = &I8251_OPS;
    u->dev.host = host;
    u->dev.name = name ? name : "i8251";
    u->master_hz = master_hz;
    const uint32_t hz[2] = {txc_hz, rxc_hz};
    for (int i = 0; i < 2; i++) {
        if (master_hz && hz[i]) { u->clk[i].num = master_hz; u->clk[i].den = hz[i]; }
    }
    u->cts = 1;
    state_reset(u);
}

void zr_i8251_set_transmit(zr_i8251 *u, zr_i8251_transmit_fn fn, void *ctx) {
    u->transmit = fn;
    u->transmit_ctx = ctx;
}

void zr_i8251_set_clock(zr_i8251 *u, int which, uint32_t num, uint32_t den) {
    if ((which != ZR_I8251_TXC && which != ZR_I8251_RXC) || (num && !den)) return;
    const uint64_t now = zr_dev_now(&u->dev);
    catch_up(u, now);                               /* what ended before now ended at the old rate */
    zr_i8251_clock *c = &u->clk[which];
    /* The character in progress keeps the clock periods it has left; the
     * edges are counted from now on at the new rate. */
    if (which == ZR_I8251_TXC && u->tx_busy) u->tx_end = clk_left(c, u->tx_end, now);
    if (which == ZR_I8251_RXC && u->rx_busy) u->rx_end = clk_left(c, u->rx_end, now);
    c->num = num;
    c->den = den;
    c->epoch = now;
    if (which == ZR_I8251_TXC) tx_kick(u, now);
    update(u);
    reschedule(u);
}

void zr_i8251_set_clock_hz(zr_i8251 *u, int which, uint32_t hz) {
    if (hz && u->master_hz) zr_i8251_set_clock(u, which, u->master_hz, hz);
    else zr_i8251_set_clock(u, which, 0, 0);
}

void zr_i8251_set_loopback(zr_i8251 *u, int on) {
    u->loopback = on != 0;
}

int zr_i8251_receive_flags(zr_i8251 *u, uint8_t byte, unsigned flags) {
    if (u->loopback || !(u->command & 4u)) return 0;
    const uint64_t now = zr_dev_now(&u->dev);
    catch_up(u, now);
    if (u->rx_busy) return 0;
    u->rx_busy = 1;
    u->rx_byte = byte;
    u->rx_flags = (uint8_t)(flags & (ZR_I8251_ERR_PE | ZR_I8251_ERR_FE | ZR_I8251_ERR_BREAK));
    u->rx_end = clk_ceil(&u->clk[ZR_I8251_RXC], now) + rx_periods(u);
    reschedule(u);
    return 1;
}

int zr_i8251_receive(zr_i8251 *u, uint8_t byte) {
    return zr_i8251_receive_flags(u, byte, 0);
}

int zr_i8251_out(const zr_i8251 *u, int pin) {
    return (pin_levels(u) >> pin) & 1;
}
