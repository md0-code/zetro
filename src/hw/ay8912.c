/* ZetRo - the AY-3-8912 (see ay8912.h). Written from the General Instrument
 * AY-3-8910/8912 Data Manual (the register array, the tone, noise and
 * envelope clocks and the 16 envelope shapes, the logarithmic DAC, the bus
 * control and the I/O port). ZetRo's own code. */
#include "ay8912.h"
#include "state.h"

#include <string.h>

#define RING ZR_AY_RING
#define MASK (RING - 1u)
#define LAT ZR_AY_LATENCY
#define HP_CORNER 30.0                  /* the high-pass corner in Hz */
#define PI 3.14159265358979323846
#define STATE_VERSION 1u

/* What a register keeps (data manual, figure 3). */
static const uint8_t REG_MASK[16] = {0xFF, 0x0F, 0xFF, 0x0F, 0xFF, 0x0F, 0x1F, 0xFF, 0x1F, 0x1F, 0x1F, 0xFF, 0xFF, 0x0F, 0xFF, 0xFF};

/* The DAC: 16 logarithmic levels, 3 dB a step from level 15 (full scale,
 * 8192) down to level 1; level 0 is off. 8192 * 2^(-(15 - n) / 2), rounded. */
static const int32_t DAC[16] = {0, 64, 91, 128, 181, 256, 362, 512, 724, 1024, 1448, 2048, 2896, 4096, 5793, 8192};

/* How much of channel A, B, C reaches the left and the right output, in
 * quarters, for each mode. */
static const uint8_t PAN[3][3][2] = {
    {{3, 3}, {3, 3}, {3, 3}},           /* mono */
    {{4, 0}, {2, 2}, {0, 4}},           /* ABC */
    {{4, 0}, {0, 4}, {2, 2}},           /* ACB */
};

static uint32_t gcd32(uint32_t a, uint32_t b) {
    while (b) { const uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* The frame that master-clock time `t` falls in, and the first guest time of frame n. */
static uint64_t frame_of(const zr_ay *a, uint64_t t) { return t * a->num / a->den; }
static uint64_t frame_start(const zr_ay *a, uint64_t n) { return (n * a->den + a->num - 1u) / a->num; }

static uint64_t d2u(double v) { uint64_t u; memcpy(&u, &v, sizeof u); return u; }
static double u2d(uint64_t u) { double v; memcpy(&v, &u, sizeof v); return v; }

static int16_t to_sample(double v) {
    v = v >= 0.0 ? v + 0.5 : v - 0.5;
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)(long)v;
}

/* --- the sound generators ---------------------------------------------------------- */

/* The channels' outputs, mixed. */
static void remix(zr_ay *a) {
    const unsigned mixer = a->reg[7];
    int32_t l = 0, r = 0;
    for (unsigned ch = 0; ch < 3; ch++) {
        const unsigned tone = (a->tone_out[ch] | (mixer >> ch)) & 1u;               /* a disabled source is held high */
        const unsigned noise = ((a->lfsr & 1u) | (mixer >> (3 + ch))) & 1u;
        if (!(tone & noise)) continue;
        const unsigned amplitude = a->reg[8 + ch];
        const int32_t level = DAC[amplitude & 0x10u ? a->env : amplitude & 0x0Fu];
        l += level * PAN[a->mode][ch][0];
        r += level * PAN[a->mode][ch][1];
    }
    a->out_l = l;
    a->out_r = r;
}

/* The envelope restarts from the first count of its shape. */
static void env_restart(zr_ay *a) {
    a->env_up = (a->reg[13] & 4u) != 0;
    a->env = a->env_up ? 0u : 15u;
    a->env_hold = 0;
    a->env_cnt = 0;
}

/* One step of the 16 of a cycle. At the end of a cycle: CONT 0 resets to 0
 * and holds; HOLD holds (the last count, or with ALT the first); ALT
 * reverses the direction; else the cycle starts again. */
static void env_step(zr_ay *a) {
    const unsigned shape = a->reg[13];
    if (a->env_up) {
        if (a->env < 15u) { a->env++; return; }
    } else if (a->env > 0u) { a->env--; return; }
    if (!(shape & 8u)) { a->env = 0; a->env_hold = 1; return; }
    if (shape & 1u) {
        if (shape & 2u) a->env = a->env_up ? 0u : 15u;
        a->env_hold = 1;
        return;
    }
    if (shape & 2u) a->env_up ^= 1u;
    else a->env = a->env_up ? 0u : 15u;
}

/* One tick of the /8 divider: 8 clocks of the chip. */
static void tick(zr_ay *a) {
    a->tick_at += a->tick_int;
    a->tick_acc += a->tick_rem;
    if (a->tick_acc >= a->tick_den) { a->tick_acc -= a->tick_den; a->tick_at++; }
    for (unsigned ch = 0; ch < 3; ch++) {
        unsigned period = (unsigned)a->reg[2 * ch] | ((unsigned)(a->reg[2 * ch + 1] & 0x0Fu) << 8);
        if (!period) period = 1;
        if (++a->tone_cnt[ch] >= period) { a->tone_cnt[ch] = 0; a->tone_out[ch] ^= 1u; }
    }
    /* The noise and the envelope run at half this rate for each count of their periods. */
    unsigned noise = a->reg[6] & 0x1Fu;
    if (!noise) noise = 1;
    if (++a->noise_cnt >= 2u * noise) {
        a->noise_cnt = 0;
        /* A 17-bit maximal-length register, x^17 + x^14 + 1. */
        const uint32_t feedback = (a->lfsr ^ (a->lfsr >> 3)) & 1u;
        a->lfsr = (a->lfsr >> 1) | (feedback << 16);
    }
    if (!a->env_hold) {
        unsigned period = (unsigned)a->reg[11] | ((unsigned)a->reg[12] << 8);
        if (!period) period = 1;
        if (++a->env_cnt >= 2u * period) { a->env_cnt = 0; env_step(a); }
    }
    remix(a);
}

/* One output frame is complete: its average through the high-pass. */
static void emit(zr_ay *a) {
    const double length = 4.0 * (double)(a->bound - a->fstart);
    const double x[2] = {(double)a->sum_l / length, (double)a->sum_r / length};
    if (a->frame - a->done >= RING) a->done++;                      /* the shell is not taking them: drop the oldest */
    int16_t *out = a->ring[a->frame & MASK];
    for (unsigned c = 0; c < 2; c++) {
        const double y = x[c] - a->hp_x[c] + a->hp_keep * a->hp_y[c];
        a->hp_x[c] = x[c];
        a->hp_y[c] = y;
        out[c] = to_sample(y);
    }
    a->frame++;
    a->fstart = a->bound;
    a->bound = frame_start(a, a->frame + 1u);
    a->sum_l = a->sum_r = 0;
}

/* The chip and the sound run on to guest time `until`. */
static void run(zr_ay *a, uint64_t until) {
    while (a->time < until) {
        uint64_t stop = until;
        if (a->tick_at < stop) stop = a->tick_at;
        if (a->bound < stop) stop = a->bound;
        const int64_t d = (int64_t)(stop - a->time);
        a->sum_l += (int64_t)a->out_l * d;
        a->sum_r += (int64_t)a->out_r * d;
        a->time = stop;
        if (stop == a->bound) emit(a);
        if (stop == a->tick_at) tick(a);
    }
}

/* --- the registers and the port ---------------------------------------------------- */

static uint8_t port_in(const zr_ay *a) { return a->io.in ? a->io.in(a->io.ctx) : 0xFFu; }

/* What the pins of port A carry: R14 when it is an output, else the pull-ups. */
static void update_pins(zr_ay *a) {
    const uint8_t pins = a->reg[7] & 0x40u ? a->reg[14] : 0xFFu;
    if (pins == a->pins) return;
    a->pins = pins;
    if (a->io.out) a->io.out(a->io.ctx, pins);
}

static void write_reg(zr_ay *a, unsigned r, uint8_t value) {
    a->reg[r] = value & REG_MASK[r];
    if (r == 13u) env_restart(a);
    else if (r == 7u || r == 14u) update_pins(a);
    remix(a);
}

void zr_ay_select(zr_ay *a, uint8_t address) {
    run(a, zr_dev_now(&a->dev));
    a->valid = (address & 0xF0u) == 0;
    if (a->valid) a->selected = address & 0x0Fu;
}

void zr_ay_write(zr_ay *a, uint8_t value) {
    run(a, zr_dev_now(&a->dev));
    if (a->valid) write_reg(a, a->selected, value);
}

uint8_t zr_ay_read(zr_ay *a) {
    run(a, zr_dev_now(&a->dev));
    if (!a->valid) return 0xFFu;
    if (a->selected == 14u) return a->reg[7] & 0x40u ? a->reg[14] : port_in(a);
    return a->reg[a->selected];
}

void zr_ay_set_mode(zr_ay *a, int mode) {
    run(a, zr_dev_now(&a->dev));
    a->mode = mode >= ZR_AY_MONO && mode <= ZR_AY_ACB ? mode : ZR_AY_MONO;
    remix(a);
}

/* --- rendering --------------------------------------------------------------------- */

static size_t render(zr_ay *a, uint64_t until, int16_t *stereo, size_t frames, int add) {
    run(a, until);
    uint64_t due = frame_of(a, until);
    if (due > a->frame) due = a->frame;
    const uint64_t ready = due > LAT ? due - LAT : 0;               /* the frames a pull may take now */
    if (ready <= a->done) return 0;
    if (ready - a->done > ZR_AY_BACKLOG) a->done = ready - ZR_AY_BACKLOG;
    const uint64_t avail = ready - a->done;
    const size_t n = avail < frames ? (size_t)avail : frames;
    for (size_t i = 0; i < n; i++) {
        const int16_t *in = a->ring[(a->done + i) & MASK];
        if (add) {
            for (size_t c = 0; c < 2; c++) {
                const int sum = (int)stereo[2u * i + c] + (int)in[c];
                stereo[2u * i + c] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
            }
        } else {
            stereo[2u * i] = in[0];
            stereo[2u * i + 1u] = in[1];
        }
    }
    a->done += n;
    return n;
}

size_t zr_ay_render(zr_ay *a, uint64_t until, int16_t *stereo, size_t frames) { return render(a, until, stereo, frames, 0); }
size_t zr_ay_render_add(zr_ay *a, uint64_t until, int16_t *stereo, size_t frames) { return render(a, until, stereo, frames, 1); }

/* --- the dev ----------------------------------------------------------------------- */

static uint8_t ay_read(void *d, unsigned space, uint32_t offset) {
    (void)space;
    return offset & 1u ? 0xFFu : zr_ay_read(d);
}

static void ay_write(void *d, unsigned space, uint32_t offset, uint8_t value) {
    (void)space;
    if (offset & 1u) zr_ay_write(d, value);
    else zr_ay_select(d, value);
}

static void ay_event(void *d, uint64_t now) { (void)d; (void)now; }

/* The RESET pin: every register 0 (all tones and noises on, every channel
 * off, the port an input); the envelope holds at 0 until R13 is written. */
static void ay_reset(void *d) {
    zr_ay *a = d;
    run(a, zr_dev_now(&a->dev));
    memset(a->reg, 0, sizeof a->reg);
    a->selected = 0;
    a->valid = 1;
    memset(a->tone_cnt, 0, sizeof a->tone_cnt);
    memset(a->tone_out, 0, sizeof a->tone_out);
    a->noise_cnt = 0;
    a->lfsr = 1u;
    a->env = 0;
    a->env_up = 0;
    a->env_hold = 1;
    a->env_cnt = 0;
    update_pins(a);
    remix(a);
}

static void ay_save(const void *d, struct zr_state_writer *w) {
    const zr_ay *a = d;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_bytes(w, a->reg, sizeof a->reg);
    zr_sw_u8(w, a->selected);
    zr_sw_u8(w, a->valid);
    for (unsigned i = 0; i < 3; i++) { zr_sw_u16(w, a->tone_cnt[i]); zr_sw_u8(w, a->tone_out[i]); }
    zr_sw_u8(w, a->noise_cnt);
    zr_sw_u32(w, a->lfsr);
    zr_sw_u32(w, a->env_cnt);
    zr_sw_u8(w, a->env);
    zr_sw_u8(w, a->env_up);
    zr_sw_u8(w, a->env_hold);
    zr_sw_u8(w, a->pins);
    zr_sw_u64(w, a->time);
    zr_sw_u64(w, a->tick_at);
    zr_sw_u32(w, a->tick_acc);
    zr_sw_u64(w, a->frame);
    zr_sw_u64(w, a->done);
    zr_sw_u64(w, (uint64_t)a->sum_l);
    zr_sw_u64(w, (uint64_t)a->sum_r);
    for (unsigned c = 0; c < 2; c++) { zr_sw_u64(w, d2u(a->hp_x[c])); zr_sw_u64(w, d2u(a->hp_y[c])); }
    const uint32_t count = (uint32_t)(a->frame - a->done);
    zr_sw_u32(w, count);
    for (uint32_t i = 0; i < count; i++) {
        const int16_t *f = a->ring[(a->done + i) & MASK];
        zr_sw_u16(w, (uint16_t)f[0]);
        zr_sw_u16(w, (uint16_t)f[1]);
    }
}

static int ay_load(void *d, struct zr_state_reader *r) {
    zr_ay *a = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_sr_bytes(r, a->reg, sizeof a->reg);
    a->selected = zr_sr_u8(r) & 0x0Fu;
    a->valid = zr_sr_u8(r) != 0;
    for (unsigned i = 0; i < 3; i++) { a->tone_cnt[i] = zr_sr_u16(r); a->tone_out[i] = zr_sr_u8(r) & 1u; }
    a->noise_cnt = zr_sr_u8(r);
    a->lfsr = zr_sr_u32(r) & 0x1FFFFu;
    if (!a->lfsr) a->lfsr = 1u;
    a->env_cnt = zr_sr_u32(r);
    a->env = zr_sr_u8(r) & 0x0Fu;
    a->env_up = zr_sr_u8(r) & 1u;
    a->env_hold = zr_sr_u8(r) & 1u;
    a->pins = zr_sr_u8(r);
    a->time = zr_sr_u64(r);
    a->tick_at = zr_sr_u64(r);
    a->tick_acc = zr_sr_u32(r);
    a->frame = zr_sr_u64(r);
    a->done = zr_sr_u64(r);
    a->sum_l = (int64_t)zr_sr_u64(r);
    a->sum_r = (int64_t)zr_sr_u64(r);
    for (unsigned c = 0; c < 2; c++) { a->hp_x[c] = u2d(zr_sr_u64(r)); a->hp_y[c] = u2d(zr_sr_u64(r)); }
    uint32_t count = zr_sr_u32(r);
    if (count > RING || a->done + count != a->frame) { count = 0; a->done = a->frame; r->failed = 1; }
    for (uint32_t i = 0; i < count; i++) {
        int16_t *f = a->ring[(a->done + i) & MASK];
        f[0] = (int16_t)zr_sr_u16(r);
        f[1] = (int16_t)zr_sr_u16(r);
    }
    a->fstart = frame_start(a, a->frame);
    a->bound = frame_start(a, a->frame + 1u);
    remix(a);
    return zr_sr_ok(r);
}

static const zr_dev_ops AY_OPS = {
    .read = ay_read, .write = ay_write, .event = ay_event, .reset = ay_reset,
    .save = ay_save, .load = ay_load,
};

void zr_ay_init(zr_ay *a, const zr_hw_host *host, uint32_t master_hz, uint32_t chip_hz, uint32_t rate, int mode, const zr_ay_io *io) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &AY_OPS;
    a->dev.host = host;
    a->dev.name = "ay8912";
    if (io) a->io = *io;
    a->mode = mode >= ZR_AY_MONO && mode <= ZR_AY_ACB ? mode : ZR_AY_MONO;
    const uint32_t g = gcd32(master_hz, rate);
    a->num = rate / g;
    a->den = master_hz / g;
    const uint64_t per_tick = 8u * (uint64_t)master_hz;
    a->tick_int = (uint32_t)(per_tick / chip_hz);
    if (!a->tick_int) a->tick_int = 1;
    a->tick_rem = (uint32_t)(per_tick % chip_hz);
    a->tick_den = chip_hz;
    a->hp_keep = 1.0 - 2.0 * PI * HP_CORNER / (double)rate;
    const uint64_t now = zr_dev_now(&a->dev);
    a->time = a->tick_at = now;
    a->frame = a->done = frame_of(a, now);
    a->fstart = frame_start(a, a->frame);
    a->bound = frame_start(a, a->frame + 1u);
    a->pins = 0xFFu;
    ay_reset(a);
}
