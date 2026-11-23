/* ZetRo - the sound of the diskette drives (drivesnd.h). Portable C11. ZetRo's own code: damped resonances, noise bursts
 * and slow oscillators summed, their numbers taken from the analysis of recordings
 * of a real drive, nothing recorded played, no C library math. */
#include "drivesnd.h"

#include <string.h>

#define PI 3.14159265358979323846

/* What a queued event does. */
enum { K_MOTOR, K_STEP, K_LOAD, K_UNLOAD, K_STAGE, K_CLUNK };

/* --- the levels ---------------------------------------------------------------------------------- */

/* Every number of the voices below comes from the analysis of recordings of an Alps Electric DF354H148F 3.5-inch drive (the
 * sample set 86Box plays, from the RetroDriveSounds recordings): the resonances of a
 * single step, of the click at spin-up and of the running motor, their decay times, their levels and when in the sound they
 * start. Amplitudes are in the recordings' units, where a single step peaks at about 1100 and the running motor has an RMS of
 * about 23 above 150 Hz; REF_GAIN takes them to the source's level (a step's peak of about 4500 at volume 100). No sample of
 * the recordings is used. */
#define REF_GAIN 4.1
#define SKIP_LIMIT 4800u                /* a gap in the frames is skipped by running only this many (100 ms at 48 kHz) */
#define RES_FLOOR 16.0                  /* a resonance below an amplitude of 4 is over */
#define BURST_FLOOR 2.0

/* --- own math ---------------------------------------------------------------------------------- */

/* sin(pi * x), summed as a Taylor series after reducing x to [-1/2, 1/2] (error below 1e-11). */
static double sinpi_(double x) {
    double n = x + 1.0;
    n -= 2.0 * (double)(long long)(n / 2.0);
    if (n < 0) n += 2.0;
    x = n - 1.0;
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

/* e^-x for x >= 0: halve x down to 1/2, sum the series, square back. */
static double exp_neg(double x) {
    int k = 0;
    while (x > 0.5) { x *= 0.5; k++; }
    double term = 1.0, sum = 1.0;
    for (int i = 1; i < 14; i++) {
        term *= -x / i;
        sum += term;
    }
    while (k--) sum *= sum;
    return sum;
}

/* The pseudo-random sequence: xorshift32 from a fixed seed, so the same events give the same sound. */
static double rnd(zr_drivesnd *s) {                 /* -1 to 1 */
    uint32_t x = s->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s->rng = x;
    return (double)(int32_t)x / 2147483648.0;
}

static uint32_t gcd32(uint32_t a, uint32_t b) {
    while (b) { const uint32_t t = a % b; a = b; b = t; }
    return a;
}

/* Microseconds as master-clock ticks. */
static uint64_t ticks_of(const zr_drivesnd *s, uint32_t us) { return (uint64_t)s->clock_hz * us / 1000000u; }

/* The frame that master-clock time `t` falls in. */
static uint64_t frame_of(const zr_drivesnd *s, uint64_t t) { return t * s->num / s->den; }

/* The sine table read at a 32-bit phase, interpolated. */
static double osc(const zr_drivesnd *s, uint32_t phase) {
    const unsigned i = phase >> 22;
    const double frac = (double)(phase & 0x3FFFFFu) * (1.0 / 4194304.0);
    return s->table[i] + (s->table[i + 1u] - s->table[i]) * frac;
}

/* --- where the drives sit ------------------------------------------------------------------- */

static const double PAN[ZR_DRIVESND_DRIVES] = {-0.2, 0.2, -0.1, 0.1};
static double left_of(unsigned d) { return 1.0 - (PAN[d] > 0.0 ? PAN[d] : 0.0); }
static double right_of(unsigned d) { return 1.0 + (PAN[d] < 0.0 ? PAN[d] : 0.0); }

/* --- the voices --------------------------------------------------------------------------------- */

/* A damped resonance of `f` Hz that falls to 1/e in `tau` seconds, of amplitude `amp`: it starts at its zero crossing, as a
 * struck part does (soft 1), or at its peak, as a crack does (soft 0). */
static void add_res(zr_drivesnd *s, double f, double tau, double amp, unsigned d, int soft) {
    zr_drivesnd_res *v = NULL;
    for (unsigned i = 0; i < ZR_DRIVESND_RES; i++) {
        const unsigned k = (s->res_next + i) % ZR_DRIVESND_RES;
        if (!s->res[k].live) { v = &s->res[k]; s->res_next = k + 1u; break; }
    }
    if (!v) {                                       /* all busy: the oldest gives way */
        v = &s->res[s->res_next % ZR_DRIVESND_RES];
        s->res_next++;
        s->nres--;
    }
    const double rate = (double)s->rate;
    const double w = 2.0 * f / rate;                /* the angle per sample in units of pi */
    v->c = sinpi_(w + 0.5);
    v->s = sinpi_(w);
    v->r = exp_neg(1.0 / (tau * rate));
    v->x = soft ? 0.0 : amp;
    v->y = soft ? -amp : 0.0;
    v->left = left_of(d);
    v->right = right_of(d);
    v->live = 1;
    s->nres++;
}

/* A burst of noise low-passed by a one-pole filter (coefficient `k`), falling to 1/e in `tau` seconds. */
static void add_burst(zr_drivesnd *s, double k, double tau, double amp, unsigned d) {
    zr_drivesnd_burst *v = NULL;
    for (unsigned i = 0; i < ZR_DRIVESND_BURSTS; i++) {
        const unsigned n = (s->burst_next + i) % ZR_DRIVESND_BURSTS;
        if (!s->burst[n].live) { v = &s->burst[n]; s->burst_next = n + 1u; break; }
    }
    if (!v) {
        v = &s->burst[s->burst_next % ZR_DRIVESND_BURSTS];
        s->burst_next++;
        s->nburst--;
    }
    v->env = amp;
    v->decay = exp_neg(1.0 / (tau * (double)s->rate));
    v->lp = 0.0;
    v->k = k;
    v->left = left_of(d);
    v->right = right_of(d);
    v->live = 1;
    s->nburst++;
}

/* A part of a sound: a resonance (Hz, the time it falls to 1/e in seconds, its amplitude in the recordings' units). */
typedef struct part { double f, tau, amp; } part;
#define PARTS(a) (a), (unsigned)(sizeof(a) / sizeof((a)[0]))

/* The parts sound together: each a little off its pitch (`spread`, a fraction) so that no two hits are the same. */
static void hit(zr_drivesnd *s, unsigned d, const part *p, unsigned n, double level, double pitch, double stretch, double spread, int soft) {
    for (unsigned i = 0; i < n; i++)
        add_res(s, p[i].f * pitch * (1.0 + spread * rnd(s)), p[i].tau * stretch, REF_GAIN * p[i].amp * level, d, soft);
}

/* A step, as the recordings have it: not one click but about 40 ms of sound in five stages. The carriage starts with a low
 * knock (405 and 553 Hz) under a rasp of dull noise; 7 ms on the motor's ring comes in (1.4 and 1.7 kHz) with the first of
 * the frame's resonance near 811 Hz; at 13 ms the strike proper, that resonance and its neighbours at 905 and 1010 Hz with a
 * low thud (118 and 194 Hz); at 23 and 32 ms the carriage settles on the same resonance, softer. Most of the energy is
 * between 400 Hz and 1.6 kHz. The two settling stages are dropped when the next step comes first, so a seek is a run of
 * knocks and strikes that ends in one settle. A step inwards is a little louder; one against the end stop is heavier and
 * rings longer. */
static const part STEP0[] = {{405.0, 0.0027, 230.0}, {553.0, 0.0053, 330.0}};
static const part STEP1[] = {{1379.0, 0.0040, 230.0}, {1661.0, 0.0035, 230.0}, {292.0, 0.0086, 150.0}, {811.0, 0.0060, 320.0}, {905.0, 0.0050, 200.0}};
static const part STEP2[] = {{811.0, 0.0065, 420.0}, {905.0, 0.0050, 180.0}, {1010.0, 0.0045, 240.0}, {1550.0, 0.0010, 150.0},
                             {1790.0, 0.0010, 150.0}, {194.0, 0.0062, 110.0}, {118.0, 0.0210, 80.0}};
static const part STEP3[] = {{811.0, 0.0060, 300.0}, {985.0, 0.0040, 200.0}};
static const part STEP4[] = {{811.0, 0.0055, 300.0}, {553.0, 0.0050, 160.0}};
#define STEP_STAGES 5u
#define STEP_SETTLE 3u                  /* the first of the settling stages */
static const unsigned STEP_AT_US[STEP_STAGES] = {0u, 7000u, 13000u, 23000u, 32000u};

static void trigger_step(zr_drivesnd *s, unsigned d, unsigned stage, int in, int stop) {
    const double level = (1.0 + 0.12 * rnd(s)) * (in ? 1.0 : 0.87) * (stop ? 1.4 : 1.0), stretch = stop ? 1.5 : 1.0;
    const double pitch = stop ? 0.9 : 1.0;
    switch (stage) {
    case 0:
        hit(s, d, PARTS(STEP0), level * (stop ? 1.6 : 1.0), pitch, stretch, 0.03, 1);
        add_burst(s, 0.12, 0.012, REF_GAIN * 650.0 * level, d);
        break;
    case 1:
        hit(s, d, PARTS(STEP1), level, pitch, stretch, 0.03, 1);
        add_burst(s, 0.20, 0.008, REF_GAIN * 700.0 * level, d);
        break;
    case 2: hit(s, d, PARTS(STEP2), level, pitch, stretch, 0.03, 1); break;
    case 3:
        hit(s, d, PARTS(STEP3), level, pitch, stretch, 0.03, 1);
        add_burst(s, 0.15, 0.006, REF_GAIN * 450.0 * level, d);
        break;
    default: hit(s, d, PARTS(STEP4), level, pitch, stretch, 0.03, 1); break;
    }
}

/* The click of the mechanism engaging, 43 ms after the motor starts in the recordings: bright (most of it between 1.6 and
 * 6.4 kHz, its strongest resonances at 2.15 to 2.45 kHz) and over in a few milliseconds, with a crack of noise. The head
 * load of a drive that has a head-load solenoid is the same sound at a third of the level, the unload lower and softer. */
static const part CLUNK[] = {{2445.0, 0.0022, 1080.0}, {2149.0, 0.0021, 790.0}, {2288.0, 0.0022, 500.0}, {1535.0, 0.0019, 470.0}, {1431.0, 0.0019, 400.0},
                             {1805.0, 0.0006, 430.0}, {1090.0, 0.0060, 200.0}, {716.0, 0.0023, 200.0}, {875.0, 0.0023, 240.0}};
#define CLUNK_AT_US 43000u
static void trigger_clunk(zr_drivesnd *s, unsigned d, double level, double pitch) {
    level *= 1.0 + 0.10 * rnd(s);
    hit(s, d, PARTS(CLUNK), level, pitch, 1.0, 0.04, 1);
    add_burst(s, 0.5, 0.003, REF_GAIN * 1500.0 * level, d);
}

/* The motor's kick as the current comes on: a short low thump. */
static const part KICK[] = {{240.0, 0.0040, 350.0}, {120.0, 0.0060, 250.0}};

/* --- the motors ---------------------------------------------------------------------------------- */

/* A motor at speed fraction v, as the running recording has it: the tones of a 240 Hz series (240, 480, 720, 960, 1200 and
 * 1440 Hz) and one near 816 Hz, which carry 70% of the power above 150 Hz, over a noise that is strongest between 600 Hz and
 * 5 kHz, the whole swelling and fading with the disk's turn (5 Hz at 300 rpm, a depth of 0.27). The pitch follows the speed
 * and the loudness its square. While the motor runs down the sound comes in pulses (12.5 a second in the recording). */
static double floppy_motor(zr_drivesnd *s, zr_drivesnd_spin *p, unsigned d, double v) {
    p->phase[0] += (uint32_t)((240.0 + 3.0 * (double)d) * v * s->inc);
    p->phase[1] += (uint32_t)(5.0 * v * s->inc);
    p->phase[2] += (uint32_t)(816.0 * v * s->inc);
    const uint32_t ph = p->phase[0];
    const double tone = 13.2 * osc(s, ph) + 9.8 * osc(s, ph * 2u) + 10.5 * osc(s, ph * 3u) + 12.9 * osc(s, ph * 4u) + 4.4 * osc(s, ph * 5u) +
                        6.2 * osc(s, ph * 6u) + 11.7 * osc(s, p->phase[2]);
    const double flutter = 1.0 + 0.27 * osc(s, p->phase[1]);
    const double w = rnd(s);
    p->lp1 += 0.03 * (w - p->lp1);                  /* what is below about 230 Hz is taken out */
    p->lp2 += 0.15 * ((w - p->lp1) - p->lp2);       /* and what is above about 1.2 kHz falls off: an RMS of 0.148 per unit */
    double y = REF_GAIN * v * v * flutter * (tone + 83.6 * p->lp2);
    if (p->target == 0.0) {
        p->phase[3] += (uint32_t)(12.5 * s->inc);
        y *= 1.5 * (1.0 + 0.6 * osc(s, p->phase[3]));
    }
    return y;
}

static void spin_frame(zr_drivesnd *s, unsigned d, double *l, double *r) {
    zr_drivesnd_spin *p = &s->spin[d];
    p->level += (p->target - p->level) * (p->target > p->level ? s->atk : s->rel);
    if (p->target == 0.0 && p->level < 0.003) {
        p->level = 0.0;
        s->spin_mask = (uint8_t)(s->spin_mask & ~(1u << d));
        return;
    }
    const double y = floppy_motor(s, p, d, p->level);
    *l += y * left_of(d);
    *r += y * right_of(d);
}

/* --- one frame -------------------------------------------------------------------------------------- */

static void synth_frame(zr_drivesnd *s, double *outl, double *outr) {
    double l = 0.0, r = 0.0;
    if (s->spin_mask)
        for (unsigned d = 0; d < ZR_DRIVESND_DRIVES; d++)
            if (s->spin_mask & 1u << d) spin_frame(s, d, &l, &r);
    if (s->nres) {
        for (unsigned i = 0; i < ZR_DRIVESND_RES; i++) {
            zr_drivesnd_res *v = &s->res[i];
            if (!v->live) continue;
            l += v->x * v->left;
            r += v->x * v->right;
            const double nx = v->r * (v->x * v->c - v->y * v->s), ny = v->r * (v->x * v->s + v->y * v->c);
            v->x = nx;
            v->y = ny;
            if (nx * nx + ny * ny < RES_FLOOR) { v->live = 0; s->nres--; }
        }
    }
    if (s->nburst) {
        for (unsigned i = 0; i < ZR_DRIVESND_BURSTS; i++) {
            zr_drivesnd_burst *v = &s->burst[i];
            if (!v->live) continue;
            v->lp += v->k * (rnd(s) * v->env - v->lp);
            l += v->lp * v->left;
            r += v->lp * v->right;
            v->env *= v->decay;
            if (v->env < BURST_FLOOR) { v->live = 0; s->nburst--; }
        }
    }
    *outl = l * s->gain;
    *outr = r * s->gain;
}

static int16_t to_s16(double v) {
    v = v >= 0.0 ? v + 0.5 : v - 0.5;
    if (v > 32767.0) return 32767;
    if (v < -32768.0) return -32768;
    return (int16_t)(long)v;
}

/* --- events ------------------------------------------------------------------------------------------ */

/* An event takes effect. */
static void apply(zr_drivesnd *s, const zr_drivesnd_item *e) {
    const unsigned d = e->drive;
    switch (e->kind) {
    case K_MOTOR:
        if (e->a && s->spin[d].target == 0.0) hit(s, d, PARTS(KICK), 1.0, 1.0, 1.0, 0.05, 1);
        s->spin[d].target = e->a ? 1.0 : 0.0;
        s->spin[d].phase[3] = 0;
        if (e->a) s->spin_mask = (uint8_t)(s->spin_mask | 1u << d);
        break;
    case K_STEP:
        trigger_step(s, d, 0, (int)e->a, (int)e->b);
        break;
    case K_STAGE:
        trigger_step(s, d, e->b >> 1, (int)e->a, (int)(e->b & 1u));
        break;
    case K_CLUNK:
        if (s->spin[d].target != 0.0) trigger_clunk(s, d, 1.0, 1.0);    /* a motor switched off again before it engaged makes none */
        break;
    case K_LOAD:
        trigger_clunk(s, d, 0.30, 1.0);
        break;
    case K_UNLOAD:
        s->loaded = (uint8_t)(s->loaded & ~(1u << d));
        trigger_clunk(s, d, 0.18, 0.85);
        break;
    default: break;
    }
}

static void pop_apply(zr_drivesnd *s) {
    const zr_drivesnd_item e = s->q[s->qh++];
    if (s->qh == s->qn) {
        s->qh = s->qn = 0;
        s->next_frame = UINT64_MAX;
    } else s->next_frame = frame_of(s, s->q[s->qh].t);
    if (!e.dead) apply(s, &e);
}

/* The event into the queue, in time order (events arrive in order but for a head unload, which the source
 * places itself after the hold time). A full queue plays its oldest event at once. */
static void push(zr_drivesnd *s, const zr_drivesnd_item *e) {
    if (s->qn - s->qh >= ZR_DRIVESND_QUEUE) pop_apply(s);
    if (s->qn == ZR_DRIVESND_QUEUE) {
        memmove(s->q, s->q + s->qh, (s->qn - s->qh) * sizeof s->q[0]);
        s->qn -= s->qh;
        s->qh = 0;
    }
    unsigned i = s->qn++;
    while (i > s->qh && s->q[i - 1u].t > e->t) {
        s->q[i] = s->q[i - 1u];
        i--;
    }
    s->q[i] = *e;
    s->next_frame = frame_of(s, s->q[s->qh].t);
}

/* The head unload that is pending for drive `d` is cancelled when something uses the head at time `t`, before it; one
 * that is already due stays. */
static void cancel_hold(zr_drivesnd *s, unsigned d, uint64_t t) {
    if (s->hold_t[d] == ZR_NEVER) return;
    if (s->hold_t[d] > t)
        for (unsigned i = s->qh; i < s->qn; i++)
            if (s->q[i].kind == K_UNLOAD && s->q[i].drive == d && s->q[i].t == s->hold_t[d]) s->q[i].dead = 1;
    s->hold_t[d] = ZR_NEVER;
}

void zr_drivesnd_event(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b) {
    zr_drivesnd *s = ctx;
    if (!s->enabled || drive >= ZR_DRIVESND_DRIVES) return;
    zr_drivesnd_item e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.a = a;
    e.b = b;
    e.drive = (uint8_t)drive;
    switch (event) {
    case ZR_DRIVE_MOTOR: {
        e.kind = K_MOTOR;
        e.a = a != 0;
        const uint8_t bit = (uint8_t)(1u << drive);
        if (e.a && !(s->motor & bit)) {                 /* a motor that starts: the mechanism engages a little later */
            zr_drivesnd_item c = e;
            c.kind = K_CLUNK;
            c.t = t + ticks_of(s, CLUNK_AT_US);
            push(s, &e);
            push(s, &c);
            s->motor |= bit;
            return;
        }
        s->motor = (uint8_t)(e.a ? s->motor | bit : s->motor & ~bit);
        break;
    }
    case ZR_DRIVE_STEP:
        /* The step's later stages are events of their own; the settling of the step before is dropped when it has not sounded yet. */
        for (unsigned i = s->qh; i < s->qn; i++)
            if (s->q[i].kind == K_STAGE && s->q[i].drive == drive && (s->q[i].b >> 1) >= STEP_SETTLE && s->q[i].t > t) s->q[i].dead = 1;
        e.kind = K_STEP;
        push(s, &e);
        for (unsigned stage = 1; stage < STEP_STAGES; stage++) {
            zr_drivesnd_item g = e;
            g.kind = K_STAGE;
            g.t = t + ticks_of(s, STEP_AT_US[stage]);
            g.b = stage << 1 | (b ? 1u : 0u);
            push(s, &g);
        }
        return;
    case ZR_DRIVE_HEAD_LOAD:
        cancel_hold(s, drive, t);
        s->loaded = (uint8_t)(s->loaded | 1u << drive);
        e.kind = K_LOAD;
        break;
    case ZR_DRIVE_HEAD_HOLD:
        if (!(s->loaded & 1u << drive)) return;
        cancel_hold(s, drive, t);
        e.t = t + a;
        s->hold_t[drive] = e.t;
        e.kind = K_UNLOAD;
        break;
    default: return;
    }
    push(s, &e);
}

/* --- the source ---------------------------------------------------------------------------------------- */

static void clear(zr_drivesnd *s) {
    memset(s->q, 0, sizeof s->q);
    memset(s->spin, 0, sizeof s->spin);
    memset(s->res, 0, sizeof s->res);
    memset(s->burst, 0, sizeof s->burst);
    for (unsigned d = 0; d < ZR_DRIVESND_DRIVES; d++) s->hold_t[d] = ZR_NEVER;
    s->qh = s->qn = 0;
    s->next_frame = UINT64_MAX;
    s->loaded = 0;
    s->motor = 0;
    s->spin_mask = 0;
    s->res_next = s->burst_next = s->nres = s->nburst = 0;
}

void zr_drivesnd_init(zr_drivesnd *s, uint32_t clock_hz, uint32_t rate) {
    memset(s, 0, sizeof *s);
    s->clock_hz = clock_hz ? clock_hz : 1u;
    s->rate = rate ? rate : 48000u;
    const uint32_t g = gcd32(s->clock_hz, s->rate);
    s->num = s->rate / g;
    s->den = s->clock_hz / g;
    s->inc = 4294967296.0 / (double)s->rate;
    s->rng = 0x2545F491u;
    for (unsigned i = 0; i <= ZR_DRIVESND_TABLE; i++) s->table[i] = sinpi_(2.0 * (double)i / (double)ZR_DRIVESND_TABLE);
    s->atk = 1.0 - exp_neg(1.0 / (0.03 * (double)s->rate));       /* at speed within a tenth of a second, as recorded */
    s->rel = 1.0 - exp_neg(1.0 / (0.15 * (double)s->rate));       /* and stopped within three */
    s->gain = 0.5;
    clear(s);
}

void zr_drivesnd_set(zr_drivesnd *s, int enabled, unsigned volume) {
    const int on = enabled && volume > 0;
    s->gain = (volume > 100u ? 100u : volume) / 100.0;
    if (on != s->enabled) {
        s->enabled = on;
        clear(s);
    }
}

void zr_drivesnd_reset(zr_drivesnd *s, uint64_t t) {
    clear(s);
    s->done = frame_of(s, t);
}

int zr_drivesnd_busy(const zr_drivesnd *s) { return s->nres || s->nburst || s->spin_mask || s->qh < s->qn; }

/* Frames that nobody takes: the source goes on without output, so its events and its decays stay in step. */
static void skip(zr_drivesnd *s, uint64_t frames) {
    if (frames > SKIP_LIMIT) {
        s->done += frames - SKIP_LIMIT;
        frames = SKIP_LIMIT;
    }
    while (frames--) {
        while (s->next_frame <= s->done) pop_apply(s);
        double l, r;
        synth_frame(s, &l, &r);
        s->done++;
    }
}

static size_t render(zr_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames, int add) {
    const uint64_t due = frame_of(s, until);
    const uint64_t ready = due > ZR_DRIVESND_LATENCY ? due - ZR_DRIVESND_LATENCY : 0;      /* frames before this are final */
    if (ready <= s->done) return 0;
    if (ready - s->done > ZR_DRIVESND_BACKLOG) {
        if (s->enabled) skip(s, ready - ZR_DRIVESND_BACKLOG - s->done);
        else s->done = ready - ZR_DRIVESND_BACKLOG;
    }
    const uint64_t avail = ready - s->done;
    const size_t n = avail < frames ? (size_t)avail : frames;
    if (!s->enabled) {
        s->done += n;
        if (!add) memset(stereo, 0, n * 2u * sizeof stereo[0]);
        return n;
    }
    if (!(s->nres || s->nburst || s->spin_mask) && s->next_frame >= s->done + n) {         /* nothing to make */
        s->done += n;
        if (!add) memset(stereo, 0, n * 2u * sizeof stereo[0]);
        return n;
    }
    for (size_t i = 0; i < n; i++) {
        while (s->next_frame <= s->done) pop_apply(s);
        double l, r;
        synth_frame(s, &l, &r);
        s->done++;
        if (add) {
            for (size_t c = 0; c < 2; c++) {
                const int sum = (int)stereo[2u * i + c] + (int)to_s16(c ? r : l);
                stereo[2u * i + c] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
            }
        } else {
            stereo[2u * i] = to_s16(l);
            stereo[2u * i + 1u] = to_s16(r);
        }
    }
    return n;
}

size_t zr_drivesnd_render(zr_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames) { return render(s, until, stereo, frames, 0); }
size_t zr_drivesnd_render_add(zr_drivesnd *s, uint64_t until, int16_t *stereo, size_t frames) { return render(s, until, stereo, frames, 1); }
