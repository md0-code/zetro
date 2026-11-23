/* ZetRo - the 1-bit sound (see beeper.h). The textbook method of summing
 * band-limited step responses at an edge's exact position (the "BLEP"
 * idea), so the output is the lowpass-filtered square wave sampled at frame
 * instants, computed edge by edge. ZetRo's own code. */
#include "beeper.h"
#include "state.h"

#include <string.h>

#define LAT ZR_BEEPER_LATENCY
#define RES ZR_BEEPER_STEP_RES
#define RING ZR_BEEPER_RING
#define MASK (RING - 1u)
#define CUTOFF 0.36                 /* lowpass corner, cycles per frame (17.3 kHz at 48 kHz) */
#define HP_CORNER 30.0              /* the high-pass corner in Hz */
#define PI 3.14159265358979323846
#define STATE_VERSION 1u

/* sin(pi * x) without the C library's math: x is reduced to [-1/2, 1/2]
 * and summed as a Taylor series (error below 1e-11). */
static double sinpi(double x) {
    double n = x + 1.0;
    n -= 2.0 * (double)(long long)(n / 2.0);        /* x + 1 modulo 2 ... */
    if (n < 0) n += 2.0;
    x = n - 1.0;                                    /* ... so x is in [-1, 1) */
    if (x > 0.5) x = 1.0 - x;
    else if (x < -0.5) x = -1.0 - x;
    const double z = PI * x, z2 = z * z;
    double term = z, sum = z;
    for (int i = 1; i < 12; i++) {
        term *= -z2 / ((2.0 * i) * (2.0 * i + 1.0));
        sum += term;
    }
    return sum;
}

/* The step response of a Blackman-windowed sinc lowpass over [-LAT, LAT]
 * frames, RES entries per frame, 0 at the first and 1 at the last. */
static void build_table(double *tab) {
    const unsigned n = 2u * LAT * RES;
    for (unsigned j = 0; j <= n; j++) {
        const double t = ((double)j - (double)(LAT * RES)) / (double)RES;        /* frames from the edge */
        const double x = 2.0 * CUTOFF * t;
        const double sinc = x == 0.0 ? 1.0 : sinpi(x) / (PI * x);
        const double y = t / (double)LAT;
        const double window = 0.42 + 0.5 * sinpi(y + 0.5) + 0.08 * sinpi(2.0 * y + 0.5);
        tab[j] = sinc * window;
    }
    double prev = tab[0];
    tab[0] = 0.0;
    for (unsigned j = 1; j <= n; j++) {         /* the running integral of the kernel */
        const double cur = tab[j];
        tab[j] = tab[j - 1] + 0.5 * (prev + cur);
        prev = cur;
    }
    const double total = tab[n];
    for (unsigned j = 0; j <= n; j++) tab[j] /= total;
    tab[n] = 1.0;
}

static uint32_t gcd32(uint32_t a, uint32_t b) {
    while (b) { const uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* The frame that master-clock time `t` falls in (floor), and the fraction
 * of the frame the time is past it, in 1/den. */
static uint64_t frame_of(const zr_beeper *b, uint64_t t, uint32_t *rem) {
    const uint64_t scaled = t * b->num;
    if (rem) *rem = (uint32_t)(scaled % b->den);
    return scaled / b->den;
}

static uint64_t d2u(double v) { uint64_t u; memcpy(&u, &v, sizeof u); return u; }
static double u2d(uint64_t u) { double v; memcpy(&v, &u, sizeof v); return v; }

/* The next frame's value; the filter state moves on. */
static double next_value(zr_beeper *b) {
    const unsigned i = (unsigned)(b->done & MASK);
    b->naive += b->step[i];
    b->step[i] = 0;
    const double x = (double)b->naive + b->acc[i];
    b->acc[i] = 0.0;
    const double y = x - b->hp_x + b->hp_keep * b->hp_y;
    b->hp_x = x;
    b->hp_y = y;
    b->done++;
    return y;
}

/* Frames that are skipped (the shell is not taking them): the pending
 * changes are taken in, then a long gap is silence. */
static void skip_to(zr_beeper *b, uint64_t target) {
    while (b->done < target && (b->done < b->top || target - b->done <= 64u)) (void)next_value(b);
    if (b->done < target) {
        b->done = target;
        b->hp_x = (double)b->naive;
        b->hp_y = 0.0;
    }
}

static int16_t to_sample(double v) {
    v = v >= 0.0 ? v + 0.5 : v - 0.5;
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)(long)v;
}

/* The mixed level changed at master-clock time t by `delta` output units. */
static void edge(zr_beeper *b, uint64_t t, int32_t delta) {
    uint32_t rem;
    uint64_t ip = frame_of(b, t, &rem);
    if (ip < b->done + LAT) { ip = b->done + LAT; rem = 0; }        /* an edge older than what was rendered: now */
    if (ip + LAT >= b->done + RING) skip_to(b, ip + LAT + 1u - RING);
    const double frac = (double)rem / (double)b->den;
    const double up = (double)delta;
    /* The unfiltered step begins at the first frame at or after the edge... */
    b->step[(unsigned)((ip + (rem != 0)) & MASK)] += delta;
    /* ...and the filter's difference from it is summed over the frames the kernel reaches. */
    const double fr = frac * (double)RES;
    for (int k = -(int)LAT + 1; k <= (int)LAT; k++) {
        const double p = (double)((k + (int)LAT) * (int)RES) - fr;
        const unsigned i0 = (unsigned)p;
        double v;
        if (i0 >= ZR_BEEPER_TABLE - 1u) v = b->table[ZR_BEEPER_TABLE - 1u];
        else v = b->table[i0] + (b->table[i0 + 1u] - b->table[i0]) * (p - (double)i0);
        const int after = k > 0 || (k == 0 && rem == 0);        /* the unfiltered step is already in force */
        b->acc[(unsigned)((ip + (uint64_t)(int64_t)k) & MASK)] += up * (v - (after ? 1.0 : 0.0));
    }
    if (ip + LAT + 1u > b->top) b->top = ip + LAT + 1u;
}

static size_t render(zr_beeper *b, uint64_t until, int16_t *stereo, size_t frames, int add) {
    const uint64_t due = frame_of(b, until, NULL);
    const uint64_t ready = due > LAT ? due - LAT : 0;               /* frames before this are final */
    if (ready <= b->done) return 0;
    if (ready - b->done > ZR_BEEPER_BACKLOG) skip_to(b, ready - ZR_BEEPER_BACKLOG);
    const uint64_t avail = ready - b->done;
    const size_t n = avail < frames ? (size_t)avail : frames;
    for (size_t i = 0; i < n; i++) {
        const int16_t s = to_sample(next_value(b));
        if (add) {
            for (size_t c = 0; c < 2; c++) {
                const int sum = (int)stereo[2u * i + c] + (int)s;
                stereo[2u * i + c] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
            }
        } else stereo[2u * i] = stereo[2u * i + 1u] = s;
    }
    return n;
}

size_t zr_beeper_render(zr_beeper *b, uint64_t until, int16_t *stereo, size_t frames) { return render(b, until, stereo, frames, 0); }
size_t zr_beeper_render_add(zr_beeper *b, uint64_t until, int16_t *stereo, size_t frames) { return render(b, until, stereo, frames, 1); }

static int32_t drive(const zr_beeper *b) {
    int32_t sum = 0;
    for (unsigned i = 0; i < ZR_BEEPER_INPUTS; i++) sum += b->level[i] ? b->weight[i] : 0;
    return sum;
}

void zr_beeper_set(zr_beeper *b, unsigned input, int level, uint64_t t) {
    if (input >= ZR_BEEPER_INPUTS) return;
    const uint8_t l = level != 0;
    if (b->level[input] == l) return;
    b->level[input] = l;
    if (b->weight[input]) edge(b, t, l ? b->weight[input] : -b->weight[input]);
}

void zr_beeper_set_weight(zr_beeper *b, unsigned input, int32_t weight) {
    if (input >= ZR_BEEPER_INPUTS) return;
    const int32_t old = b->weight[input];
    b->weight[input] = weight;
    if (b->level[input] && weight != old) edge(b, zr_dev_now(&b->dev), weight - old);
}

/* --- the dev --------------------------------------------------------------------------- */

static void bp_input(void *d, int pin, int state) {
    zr_beeper *b = d;
    if (pin >= 0) zr_beeper_set(b, (unsigned)pin, state, zr_dev_now(&b->dev));
}

static uint8_t bp_read(void *d, unsigned space, uint32_t offset) { (void)d; (void)space; (void)offset; return 0xFFu; }
static void bp_write(void *d, unsigned space, uint32_t offset, uint8_t value) { (void)d; (void)space; (void)offset; (void)value; }
static void bp_event(void *d, uint64_t now) { (void)d; (void)now; }

/* Power-on / reset: silence from now on (the inputs keep their levels). */
static void bp_reset(void *d) {
    zr_beeper *b = d;
    memset(b->step, 0, sizeof b->step);
    memset(b->acc, 0, sizeof b->acc);
    b->done = b->top = frame_of(b, zr_dev_now(&b->dev), NULL);
    b->naive = drive(b);
    b->hp_x = (double)b->naive;
    b->hp_y = 0.0;
}

static void bp_save(const void *d, struct zr_state_writer *w) {
    const zr_beeper *b = d;
    zr_sw_u8(w, STATE_VERSION);
    for (unsigned i = 0; i < ZR_BEEPER_INPUTS; i++) zr_sw_u8(w, b->level[i]);
    zr_sw_u64(w, b->done);
    zr_sw_u32(w, (uint32_t)b->naive);
    zr_sw_u64(w, d2u(b->hp_x));
    zr_sw_u64(w, d2u(b->hp_y));
    const uint32_t pending = b->top > b->done ? (uint32_t)(b->top - b->done) : 0u;
    zr_sw_u32(w, pending);
    for (uint32_t i = 0; i < pending; i++) {
        const unsigned j = (unsigned)((b->done + i) & MASK);
        zr_sw_u32(w, (uint32_t)b->step[j]);
        zr_sw_u64(w, d2u(b->acc[j]));
    }
}

static int bp_load(void *d, struct zr_state_reader *r) {
    zr_beeper *b = d;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    for (unsigned i = 0; i < ZR_BEEPER_INPUTS; i++) b->level[i] = zr_sr_u8(r) != 0;
    b->done = zr_sr_u64(r);
    b->naive = (int32_t)zr_sr_u32(r);
    b->hp_x = u2d(zr_sr_u64(r));
    b->hp_y = u2d(zr_sr_u64(r));
    uint32_t pending = zr_sr_u32(r);
    if (pending > RING) { pending = 0; r->failed = 1; }
    memset(b->step, 0, sizeof b->step);
    memset(b->acc, 0, sizeof b->acc);
    for (uint32_t i = 0; i < pending; i++) {
        const unsigned j = (unsigned)((b->done + i) & MASK);
        b->step[j] = (int32_t)zr_sr_u32(r);
        b->acc[j] = u2d(zr_sr_u64(r));
    }
    b->top = b->done + pending;
    return zr_sr_ok(r);
}

static const zr_dev_ops BEEPER_OPS = {
    .read = bp_read, .write = bp_write, .event = bp_event, .reset = bp_reset, .input = bp_input,
    .save = bp_save, .load = bp_load,
};

void zr_beeper_init(zr_beeper *b, const zr_hw_host *host, uint32_t clock_hz, uint32_t rate) {
    memset(b, 0, sizeof *b);
    b->dev.ops = &BEEPER_OPS;
    b->dev.host = host;
    b->dev.name = "beeper";
    b->clock_hz = clock_hz;
    b->rate = rate;
    const uint32_t g = gcd32(clock_hz, rate);
    b->num = rate / g;
    b->den = clock_hz / g;
    b->hp_keep = 1.0 - 2.0 * PI * HP_CORNER / (double)rate;
    b->weight[ZR_BEEPER_SPEAKER] = 10000;
    b->weight[ZR_BEEPER_MIC] = 1500;
    b->weight[ZR_BEEPER_EAR] = 3000;
    build_table(b->table);
    bp_reset(b);
}
