/* The Z80 family's CTC, PIO and SIO, the daisy chain and the 8257 against a
 * fake host: a few checks each, no tables. The CTC's timer (exact to the
 * clock edge on a non-integer ratio), counter, reload, down-counter read and
 * a save that continues exactly; the PIO's output and input handshakes, bit
 * control and its interrupt on a false-to-true change; the SIO's character
 * time, the frame on the line, a received frame, interrupt priority and
 * vector, and a save in mid-character; the daisy chain's priority and RETI;
 * the 8257's transfers with terminal count, TC stop, auto load, rotating
 * priority and the first/last flip-flop. No SDL. */
#include "i8257.h"
#include "state.h"
#include "z80ctc.h"
#include "z80daisy.h"
#include "z80pio.h"
#include "z80sio.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* --- the fake board: a clock, the chips' deadlines, what their pins did, a memory ------------------- */

typedef struct fake {
    zr_hw_host host;
    uint64_t now;
    zr_dev *dev[4];
    uint64_t when[4];
    unsigned devices;
    int level[4][24];           /* the last level of each output pin, by chip */
    unsigned count[4][24][2];   /* how many times each pin went to 0 and to 1 */
    uint64_t last[4][24][2];    /* and when it last did */
    uint8_t mem[65536];
    void (*tap)(struct fake *f, zr_dev *dev, int pin, int state);   /* the board's wiring of an output to an input */
} fake;

static unsigned slot(fake *f, void *dev) {
    for (unsigned i = 0; i < f->devices; i++) if (f->dev[i] == dev) return i;
    f->dev[f->devices] = dev;
    f->when[f->devices] = ZR_NEVER;
    return f->devices++;
}
static uint64_t h_now(void *ctx) { return ((fake *)ctx)->now; }
static void h_schedule(void *ctx, void *dev, uint64_t when) { fake *f = ctx; f->when[slot(f, dev)] = when; }
static void h_line(void *ctx, void *dev, int pin, int state) {
    fake *f = ctx;
    const unsigned s = slot(f, dev);
    f->level[s][pin] = state;
    f->count[s][pin][state != 0]++;
    f->last[s][pin][state != 0] = f->now;
    if (f->tap) f->tap(f, dev, pin, state);
}
static int h_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    fake *f = ctx;
    if (address + length > sizeof f->mem) return 0;
    if (write) memcpy(f->mem + address, data, length);
    else memcpy(data, f->mem + address, length);
    return 1;
}
static void fake_init(fake *f) {
    memset(f, 0, sizeof *f);
    f->host.ctx = f;
    f->host.now = h_now;
    f->host.schedule = h_schedule;
    f->host.line = h_line;
    f->host.dma = h_dma;
}
#define LVL(f, chip, pin) ((f)->level[slot((f), &(chip)->dev)][pin])
#define CNT(f, chip, pin, to) ((f)->count[slot((f), &(chip)->dev)][pin][to])
#define LAST(f, chip, pin, to) ((f)->last[slot((f), &(chip)->dev)][pin][to])

/* Runs the chips' deadlines up to `t` in time order, then sets the time to t. */
static void run_to(fake *f, uint64_t t) {
    for (unsigned guard = 0;; ++guard) {
        if (guard > 100000) { CHECK(!"a deadline keeps coming at the same time"); break; }
        unsigned best = f->devices;
        for (unsigned i = 0; i < f->devices; i++)
            if (f->when[i] <= t && (best == f->devices || f->when[i] < f->when[best])) best = i;
        if (best == f->devices) break;
        f->now = f->when[best];
        f->when[best] = ZR_NEVER;
        f->dev[best]->ops->event(f->dev[best], f->now);
    }
    f->now = t;
}

static uint8_t rd(zr_dev *d, uint32_t reg) { return d->ops->read(d, ZR_SPACE_IO, reg); }
static void wr(zr_dev *d, uint32_t reg, uint8_t v) { d->ops->write(d, ZR_SPACE_IO, reg, v); }
static void drive(zr_dev *d, int p, int s) { d->ops->input(d, p, s); }

/* A chip's state out and back in (the board's job: it opens the chunk). */
static void save_to(zr_dev *d, zr_state_writer *w) {
    zr_sw_init(w);
    zr_sw_header(w, "test", "test", NULL, 0);
    zr_sw_begin(w, "TEST", 0);
    d->ops->save(d, w);
    zr_sw_end(w);
    CHECK(zr_sw_finish(w));
}
static int load_from(zr_dev *d, const zr_state_writer *w) {
    zr_state_reader r;
    char error[64];
    if (!zr_sr_open(&r, w->data, w->size, error, sizeof error) || !zr_sr_chunk(&r, "TEST", 0)) return 0;
    return d->ops->load(d, &r);
}

/* --- CTC ------------------------------------------------------------------------------------- */

static void test_ctc(void) {
    fake f;
    zr_ctc c;
    fake_init(&f);
    zr_ctc_init(&c, &f.host, "ctc", 1, 1);                         /* the system clock is the master clock */
    c.dev.ops->reset(&c);
    wr(&c.dev, 0, 0x20);                                           /* the vector, to channel 0 only */
    wr(&c.dev, 1, 0x28);                                           /* ... a vector word to channel 1 is ignored */

    /* Timer, prescaler 16, count 3, interrupt on: the first decrement is the first clock edge after the write,
     * so the zero count comes at edge 48 and the pulse lasts one clock. */
    wr(&c.dev, 0, 0x85);
    wr(&c.dev, 0, 3);
    run_to(&f, 47);
    CHECK(LVL(&f, &c, ZR_CTC_OUT_ZCTO0) == 0 && LVL(&f, &c, ZR_CTC_OUT_INT) == 0);
    run_to(&f, 48);
    CHECK(LVL(&f, &c, ZR_CTC_OUT_ZCTO0) == 1 && LAST(&f, &c, ZR_CTC_OUT_ZCTO0, 1) == 48 && LVL(&f, &c, ZR_CTC_OUT_INT) == 1);
    run_to(&f, 49);
    CHECK(LVL(&f, &c, ZR_CTC_OUT_ZCTO0) == 0 && LAST(&f, &c, ZR_CTC_OUT_ZCTO0, 0) == 49);
    CHECK(zr_daisy_ack(&c.daisy, 1) == 0x20 && LVL(&f, &c, ZR_CTC_OUT_INT) == 0);     /* vector | channel << 1, under service */
    run_to(&f, 60);
    CHECK(rd(&c.dev, 0) == 3);                                     /* reloaded at 48, 12 prescaler edges since */
    run_to(&f, 65);
    CHECK(rd(&c.dev, 0) == 2);
    run_to(&f, 96);
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO0, 1) == 2 && LVL(&f, &c, ZR_CTC_OUT_INT) == 0);   /* pending, blocked by its own service */
    CHECK(zr_daisy_reti(&c.daisy, 1) && LVL(&f, &c, ZR_CTC_OUT_INT) == 1);               /* RETI releases it */
    CHECK(zr_daisy_ack(&c.daisy, 1) == 0x20);
    zr_daisy_reti(&c.daisy, 1);
    wr(&c.dev, 0, 0x03);                                           /* software reset: channel 0 stops */
    run_to(&f, 400);
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO0, 1) == 2);

    /* Counter mode: two falling edges of CLK/TRG1 give the zero count at the second. */
    wr(&c.dev, 1, 0x45);
    wr(&c.dev, 1, 2);
    drive(&c.dev, ZR_CTC_TRG1, 0);
    CHECK(rd(&c.dev, 1) == 1 && CNT(&f, &c, ZR_CTC_OUT_ZCTO1, 1) == 0);
    drive(&c.dev, ZR_CTC_TRG1, 1);
    drive(&c.dev, ZR_CTC_TRG1, 0);
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO1, 1) == 1 && rd(&c.dev, 1) == 2);   /* reloaded */
    run_to(&f, 411);
    CHECK(LVL(&f, &c, ZR_CTC_OUT_ZCTO1) == 0);

    /* Trigger-started timer, prescaler 256, count 0 = 256: waits for the rising edge, then 65536 clocks. */
    wr(&c.dev, 2, 0x3D);
    wr(&c.dev, 2, 0);
    run_to(&f, 500);
    CHECK(rd(&c.dev, 2) == 0 && CNT(&f, &c, ZR_CTC_OUT_ZCTO2, 1) == 0);
    drive(&c.dev, ZR_CTC_TRG2, 0);
    drive(&c.dev, ZR_CTC_TRG2, 1);
    run_to(&f, 500 + 65535);
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO2, 1) == 0);
    run_to(&f, 500 + 65536);
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO2, 1) == 1 && LAST(&f, &c, ZR_CTC_OUT_ZCTO2, 1) == 500 + 65536);

    /* A system clock of 7/2 master periods: prescaler 16, count 1 on channel 2 pulses every 56 periods, exact over 100 pulses. */
    fake g;
    zr_ctc d;
    fake_init(&g);
    zr_ctc_init(&d, &g.host, "ctc", 7, 2);
    d.dev.ops->reset(&d);
    wr(&d.dev, 2, 0x05);
    wr(&d.dev, 2, 1);
    run_to(&g, 3000);
    zr_state_writer w;
    save_to(&d.dev, &w);
    const unsigned at_save = CNT(&g, &d, ZR_CTC_OUT_ZCTO2, 1);
    run_to(&g, 5600);
    CHECK(CNT(&g, &d, ZR_CTC_OUT_ZCTO2, 1) == 100 && LAST(&g, &d, ZR_CTC_OUT_ZCTO2, 1) == 5600);
    uint64_t pn, pd;
    CHECK(zr_ctc_zc_period(&d, 2, &pn, &pd) && pn == 16u * 7u && pd == 2u);

    /* The same saved at 3000 and loaded into a second chip goes on exactly the same. */
    fake h;
    zr_ctc e;
    fake_init(&h);
    zr_ctc_init(&e, &h.host, "ctc", 1, 1);
    h.now = 3000;
    CHECK(load_from(&e.dev, &w));
    run_to(&h, 5600);
    CHECK(CNT(&h, &e, ZR_CTC_OUT_ZCTO2, 1) == 100 - at_save && LAST(&h, &e, ZR_CTC_OUT_ZCTO2, 1) == 5600);
    zr_sw_free(&w);
}

/* One channel's ZC/TO wired to another's CLK/TRG by the board, inside the line callback. */
static void cascade(fake *f, zr_dev *dev, int pin, int state) {
    (void)f;
    if (pin == ZR_CTC_OUT_ZCTO0) drive(dev, ZR_CTC_TRG1, state);
}

static void test_ctc_cascade(void) {
    fake f;
    zr_ctc c;
    fake_init(&f);
    f.tap = cascade;
    zr_ctc_init(&c, &f.host, "ctc", 1, 1);
    c.dev.ops->reset(&c);
    wr(&c.dev, 1, 0x45);                                           /* channel 1: counter on the falling edge, count 3 */
    wr(&c.dev, 1, 3);
    wr(&c.dev, 0, 0x05);                                           /* channel 0: timer, prescaler 16, count 2: a pulse every 32 clocks */
    wr(&c.dev, 0, 2);
    run_to(&f, 96);
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO0, 1) == 3 && CNT(&f, &c, ZR_CTC_OUT_ZCTO1, 1) == 0);
    run_to(&f, 97);                                                /* the third pulse ends: the third falling edge */
    CHECK(CNT(&f, &c, ZR_CTC_OUT_ZCTO1, 1) == 1 && LAST(&f, &c, ZR_CTC_OUT_ZCTO1, 1) == 97);
}

/* --- PIO ------------------------------------------------------------------------------------- */

static void test_pio(void) {
    fake f;
    zr_pio p;
    fake_init(&f);
    zr_pio_init(&p, &f.host, "pio");
    p.dev.ops->reset(&p);
    CHECK(zr_pio_driven(&p, 0) == 0 && zr_pio_driven(&p, 1) == 0 && LVL(&f, &p, ZR_PIO_OUT_ARDY) == 0);   /* reset: mode 1, nothing driven */

    /* Mode 0 on port A: a write drives the pins and raises ARDY, the strobe's rising edge drops it and interrupts. */
    wr(&p.dev, 1, 0x0F);
    wr(&p.dev, 1, 0x40);                                           /* the vector */
    wr(&p.dev, 1, 0x83);                                           /* interrupt enable */
    wr(&p.dev, 0, 0xA5);
    CHECK(zr_pio_out(&p, 0) == 0xA5 && zr_pio_driven(&p, 0) == 0xFF && LVL(&f, &p, ZR_PIO_PIN(0, 0)) == 1 && LVL(&f, &p, ZR_PIO_PIN(0, 1)) == 0);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_ARDY) == 1 && rd(&p.dev, 0) == 0xA5);
    const unsigned rises = CNT(&f, &p, ZR_PIO_OUT_ARDY, 1);
    wr(&p.dev, 0, 0xA6);                                           /* a write while ARDY is high pulses it */
    CHECK(CNT(&f, &p, ZR_PIO_OUT_ARDY, 1) == rises + 1 && LVL(&f, &p, ZR_PIO_OUT_ARDY) == 1);
    drive(&p.dev, ZR_PIO_IN_ASTB, 0);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_INT) == 0 && LVL(&f, &p, ZR_PIO_OUT_ARDY) == 1);
    drive(&p.dev, ZR_PIO_IN_ASTB, 1);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_ARDY) == 0 && LVL(&f, &p, ZR_PIO_OUT_INT) == 1);
    CHECK(zr_daisy_ack(&p.daisy, 1) == 0x40 && LVL(&f, &p, ZR_PIO_OUT_INT) == 0);
    zr_daisy_reti(&p.daisy, 1);

    /* Mode 1 on port B: the read starts the handshake, STB low loads, STB high interrupts and drops BRDY. */
    wr(&p.dev, 3, 0x4F);
    wr(&p.dev, 3, 0x42);
    wr(&p.dev, 3, 0x83);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_BRDY) == 0);
    rd(&p.dev, 2);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_BRDY) == 1);
    drive(&p.dev, ZR_PIO_IN_BSTB, 0);
    drive(&p.dev, ZR_PIO_PORT(ZR_PIO_PORT_B), 0x3C);
    drive(&p.dev, ZR_PIO_IN_BSTB, 1);
    drive(&p.dev, ZR_PIO_PORT(ZR_PIO_PORT_B), 0xE7);              /* the register kept the strobed byte */
    CHECK(LVL(&f, &p, ZR_PIO_OUT_BRDY) == 0 && LVL(&f, &p, ZR_PIO_OUT_INT) == 1);
    zr_state_writer w;
    save_to(&p.dev, &w);
    CHECK(rd(&p.dev, 2) == 0x3C && LVL(&f, &p, ZR_PIO_OUT_BRDY) == 1);
    CHECK(load_from(&p.dev, &w) && rd(&p.dev, 2) == 0x3C);
    zr_sw_free(&w);
    CHECK(zr_daisy_ack(&p.daisy, 1) == 0x42);                      /* port B's own vector */
    zr_daisy_reti(&p.daisy, 1);

    /* Mode 3 on port A: high nibble inputs, low nibble outputs; an interrupt when bit 7 goes high (OR, active high,
     * only bit 7 monitored), again only after the equation has gone false and true. */
    drive(&p.dev, ZR_PIO_PORT(ZR_PIO_PORT_A), 0x00);
    wr(&p.dev, 1, 0xCF);
    wr(&p.dev, 1, 0xF0);
    wr(&p.dev, 1, 0xB7);
    wr(&p.dev, 1, 0x7F);
    wr(&p.dev, 0, 0x05);
    CHECK(zr_pio_driven(&p, 0) == 0x0F && zr_pio_out(&p, 0) == 0x05 && LVL(&f, &p, ZR_PIO_PIN(0, 4)) == 0 && LVL(&f, &p, ZR_PIO_PIN(0, 2)) == 1);
    drive(&p.dev, ZR_PIO_PORT(ZR_PIO_PORT_A), 0x70);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_INT) == 0);
    drive(&p.dev, ZR_PIO_PIN(0, 7), 1);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_INT) == 1 && rd(&p.dev, 0) == 0xF5);   /* inputs from the pins, outputs from the register */
    CHECK(zr_daisy_ack(&p.daisy, 1) == 0x40);
    zr_daisy_reti(&p.daisy, 1);
    drive(&p.dev, ZR_PIO_PIN(0, 7), 1);
    drive(&p.dev, ZR_PIO_PORT(ZR_PIO_PORT_A), 0xF0);               /* no change in the equation: no new interrupt */
    CHECK(LVL(&f, &p, ZR_PIO_OUT_INT) == 0);
    drive(&p.dev, ZR_PIO_PIN(0, 7), 0);
    drive(&p.dev, ZR_PIO_PIN(0, 7), 1);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_INT) == 1);
    zr_daisy_ack(&p.daisy, 1);
    zr_daisy_reti(&p.daisy, 1);

    /* Mode 2 on port A (port B in bit control, every bit an input): the output byte is on the pins only while ASTB is low;
     * the input side uses port B's strobe, ready and vector. */
    wr(&p.dev, 3, 0xCF);
    wr(&p.dev, 3, 0xFF);
    wr(&p.dev, 1, 0x8F);
    wr(&p.dev, 0, 0x12);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_ARDY) == 1 && zr_pio_driven(&p, 0) == 0);
    drive(&p.dev, ZR_PIO_IN_ASTB, 0);
    CHECK(zr_pio_driven(&p, 0) == 0xFF && LVL(&f, &p, ZR_PIO_PIN(0, 1)) == 1 && LVL(&f, &p, ZR_PIO_PIN(0, 0)) == 0);
    drive(&p.dev, ZR_PIO_IN_ASTB, 1);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_ARDY) == 0 && zr_pio_driven(&p, 0) == 0 && zr_daisy_ack(&p.daisy, 1) == 0x40);
    zr_daisy_reti(&p.daisy, 1);
    rd(&p.dev, 0);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_BRDY) == 1);
    drive(&p.dev, ZR_PIO_IN_BSTB, 0);
    drive(&p.dev, ZR_PIO_PORT(ZR_PIO_PORT_A), 0x77);
    drive(&p.dev, ZR_PIO_IN_BSTB, 1);
    CHECK(LVL(&f, &p, ZR_PIO_OUT_BRDY) == 0 && zr_daisy_ack(&p.daisy, 1) == 0x42 && rd(&p.dev, 0) == 0x77);
}

/* --- SIO ------------------------------------------------------------------------------------- */

typedef struct line_log {
    unsigned frames;
    zr_sio_frame last;
    unsigned channel;
} line_log;
static void log_frame(void *ctx, unsigned channel, const zr_sio_frame *frame) {
    line_log *l = ctx;
    l->frames++;
    l->last = *frame;
    l->channel = channel;
}

static uint8_t sio_rr(zr_sio *s, unsigned channel, unsigned reg) {
    wr(&s->dev, channel * 2 + 1, (uint8_t)reg);
    return rd(&s->dev, channel * 2 + 1);
}
static void sio_wr(zr_sio *s, unsigned channel, unsigned reg, uint8_t v) {
    wr(&s->dev, channel * 2 + 1, (uint8_t)reg);
    wr(&s->dev, channel * 2 + 1, v);
}

/* The SIO in the asynchronous x1 mode, 8N1 on channel A, clocks of 100 kHz on a 1 MHz master (10 periods per edge). */
static void sio_start(zr_sio *s, fake *f, line_log *l) {
    fake_init(f);
    memset(l, 0, sizeof *l);
    zr_sio_init(s, &f->host, "sio", 1000000);
    const zr_sio_link link = {l, log_frame, NULL, NULL};
    zr_sio_set_link(s, &link);
    s->dev.ops->reset(s);
    zr_sio_set_clock_hz(s, ZR_SIO_A, ZR_SIO_RXC | ZR_SIO_TXC, 100000);
    sio_wr(s, 1, 2, 0x10);                                         /* the vector, in channel B */
    sio_wr(s, 1, 1, 0x04);                                         /* status affects vector */
    sio_wr(s, 0, 4, 0x04);                                         /* x1, 1 stop bit, no parity */
    sio_wr(s, 0, 3, 0xC1);                                         /* receiver enabled, 8 bits */
    sio_wr(s, 0, 5, 0x68);                                         /* transmitter enabled, 8 bits */
    sio_wr(s, 0, 1, 0x1A);                                         /* Tx interrupt, receive interrupt on all characters */
}

static void test_sio(void) {
    fake f;
    line_log l;
    zr_sio s;
    sio_start(&s, &f, &l);
    CHECK((sio_rr(&s, 0, 0) & ZR_SIO_RR0_TXEMPTY) && LVL(&f, &s, ZR_SIO_OUT_INT) == 0);

    /* A character starts on the first clock edge after the write and takes 10 bit times; the Tx interrupt comes when the buffer empties. */
    wr(&s.dev, 0, 0x5A);
    run_to(&f, 10);
    CHECK(l.frames == 1 && l.channel == 0 && l.last.start == 10 && l.last.data == 0x5A && l.last.nbits == 8 && l.last.length == 10);
    CHECK(l.last.bits == (0x5Au << 1 | 1u << 9) && l.last.bit_num == 10 && l.last.bit_den == 1);
    CHECK(LVL(&f, &s, ZR_SIO_OUT_INT) == 1 && (sio_rr(&s, 0, 1) & ZR_SIO_RR1_ALLSENT) == 0);
    CHECK(zr_daisy_ack(&s.daisy, 1) == (0x10 | 4u << 1) && LVL(&f, &s, ZR_SIO_OUT_INT) == 0);   /* B's vector, transmit A code, under service */
    run_to(&f, 109);
    CHECK((sio_rr(&s, 0, 1) & ZR_SIO_RR1_ALLSENT) == 0);
    run_to(&f, 110);
    CHECK((sio_rr(&s, 0, 1) & ZR_SIO_RR1_ALLSENT) != 0);

    /* A frame on the line is sampled in the middle of its bits; the receive interrupt (higher priority) nests in the Tx service. */
    zr_sio_frame fr;
    zr_sio_make_frame(&s, 0, 0xA7, 200, &fr);
    CHECK(zr_sio_rx_ready(&s, 0) && zr_sio_rx_frame(&s, 0, &fr) && !zr_sio_rx_frame(&s, 0, &fr));
    run_to(&f, 289);
    CHECK((sio_rr(&s, 0, 0) & ZR_SIO_RR0_RXAVAIL) == 0);
    run_to(&f, 290);
    CHECK((sio_rr(&s, 0, 0) & ZR_SIO_RR0_RXAVAIL) && LVL(&f, &s, ZR_SIO_OUT_INT) == 1);
    CHECK(zr_daisy_ack(&s.daisy, 1) == (0x10 | 6u << 1));          /* receive A */
    CHECK(rd(&s.dev, 0) == 0xA7);
    zr_daisy_reti(&s.daisy, 1);
    zr_daisy_reti(&s.daisy, 1);
    CHECK(s.in_service == 0);

    /* A frame at another baud rate gives wrong data with a framing error, as on a wire. */
    zr_sio_make_frame(&s, 0, 0x00, 400, &fr);
    fr.bits = 0x0AAu;                                              /* a pattern whose stop bit is low */
    zr_sio_rx_frame(&s, 0, &fr);
    run_to(&f, 600);
    CHECK((sio_rr(&s, 0, 1) & ZR_SIO_RR1_FRAMING) != 0);
    rd(&s.dev, 0);

    /* A save in mid-character, loaded into another chip, ends the character at the same moment. */
    sio_wr(&s, 0, 0, 0x28);                                        /* reset Tx interrupt pending */
    wr(&s.dev, 0, 0x33);
    run_to(&f, 650);
    zr_state_writer w;
    save_to(&s.dev, &w);
    fake g;
    zr_sio t;
    fake_init(&g);
    zr_sio_init(&t, &g.host, "sio", 1000000);
    g.now = 650;
    CHECK(load_from(&t.dev, &w));
    run_to(&g, 709);
    CHECK((sio_rr(&t, 0, 1) & ZR_SIO_RR1_ALLSENT) == 0);
    run_to(&g, 710);
    CHECK((sio_rr(&t, 0, 1) & ZR_SIO_RR1_ALLSENT) != 0);
    zr_sw_free(&w);
}

/* --- the daisy chain ----------------------------------------------------------------------------- */

static void test_daisy(void) {
    fake f;
    zr_ctc c;
    zr_pio p;
    fake_init(&f);
    zr_ctc_init(&c, &f.host, "ctc", 1, 1);
    zr_pio_init(&p, &f.host, "pio");
    c.dev.ops->reset(&c);
    p.dev.ops->reset(&p);
    const zr_daisy chain[2] = {c.daisy, p.daisy};                  /* the CTC has the priority */
    wr(&c.dev, 0, 0x30);
    wr(&p.dev, 1, 0x4F);
    wr(&p.dev, 1, 0x50);
    wr(&p.dev, 1, 0x83);
    wr(&c.dev, 3, 0x85);                                           /* channel 3: timer, prescaler 16, count 1, interrupt */
    wr(&c.dev, 3, 1);
    CHECK(!zr_daisy_int(chain, 2));
    drive(&p.dev, ZR_PIO_IN_ASTB, 0);                              /* the PIO's mode 1 strobe: it asks first */
    drive(&p.dev, ZR_PIO_IN_ASTB, 1);
    CHECK(zr_daisy_int(chain, 2));
    run_to(&f, 16);                                                /* now the CTC asks too, and it is above the PIO */
    CHECK(zr_daisy_ieo(&c.daisy, 1) == 0 && zr_daisy_ieo(&p.daisy, 0) == 0);
    CHECK(zr_daisy_ack(chain, 2) == (0x30 | 3u << 1));
    CHECK(zr_daisy_int(chain, 2) == 0);                            /* the CTC is under service: the PIO waits */
    zr_reti_watch rw = {0};
    CHECK(!zr_reti_m1(&rw, 0xED) && !zr_reti_m1(&rw, 0x45) && !zr_reti_m1(&rw, 0xED) && zr_reti_m1(&rw, 0x4D));
    CHECK(zr_daisy_reti(chain, 2));
    CHECK(zr_daisy_int(chain, 2) == 1);
    CHECK(zr_daisy_ack(chain, 2) == 0x50 && zr_daisy_ack(chain, 2) == 0xFF);   /* then the PIO answers, and nothing else asks */
    CHECK(zr_daisy_reti(chain, 2) && !zr_daisy_reti(chain, 2));
}

/* --- 8257 ---------------------------------------------------------------------------------------- */

typedef struct floppy {
    uint8_t got[16];
    unsigned n;
    int tc_at;                  /* the cycle number (1 based) that had TC */
    unsigned channel;
    uint8_t next;
} floppy;
static uint8_t floppy_dack(void *ctx, int channel, int to_device, uint8_t value, int tc) {
    floppy *d = ctx;
    d->channel = (unsigned)channel;
    if (to_device) { d->got[d->n % 16] = value; d->n++; if (tc) d->tc_at = (int)d->n; return 0xFF; }
    d->n++;
    if (tc) d->tc_at = (int)d->n;
    return d->next++;
}

static void dma_regs(zr_i8257 *d, unsigned channel, unsigned address, unsigned count_and_type) {
    wr(&d->dev, channel * 2, (uint8_t)address);
    wr(&d->dev, channel * 2, (uint8_t)(address >> 8));
    wr(&d->dev, channel * 2 + 1, (uint8_t)count_and_type);
    wr(&d->dev, channel * 2 + 1, (uint8_t)(count_and_type >> 8));
}

static void test_8257(void) {
    fake f;
    floppy fl;
    zr_i8257 d;
    fake_init(&f);
    memset(&fl, 0, sizeof fl);
    zr_i8257_init(&d, &f.host, "dma", floppy_dack, &fl);
    d.dev.ops->reset(&d);
    for (unsigned i = 0; i < 16; i++) f.mem[0x1000 + i] = (uint8_t)(0xC0 + i);

    /* Channel 2 reads 4 bytes from memory to the device (type 10, count 3), TC stop: HRQ with DRQ2, TC on the last byte only. */
    dma_regs(&d, 2, 0x1000, 0x8003);
    wr(&d.dev, 8, 0x44);
    CHECK(LVL(&f, &d, ZR_I8257_OUT_HRQ) == 0 && zr_i8257_cycle(&d) == 0);
    drive(&d.dev, ZR_I8257_IN_DRQ2, 1);
    CHECK(LVL(&f, &d, ZR_I8257_OUT_HRQ) == 1 && zr_i8257_hrq(&d));
    CHECK(zr_i8257_cycle(&d) == 4 && zr_i8257_cycle(&d) == 4 && zr_i8257_cycle(&d) == 4 && fl.tc_at == 0);
    CHECK(CNT(&f, &d, ZR_I8257_OUT_TC, 1) == 0 && zr_i8257_cycle(&d) == 4);
    CHECK(fl.n == 4 && fl.tc_at == 4 && fl.channel == 2 && fl.got[0] == 0xC0 && fl.got[3] == 0xC3 && CNT(&f, &d, ZR_I8257_OUT_TC, 1) == 1);
    CHECK(LVL(&f, &d, ZR_I8257_OUT_HRQ) == 0 && zr_i8257_cycle(&d) == 0);      /* TC stop cleared the enable bit */
    CHECK(rd(&d.dev, 8) == 0x04 && rd(&d.dev, 8) == 0);                       /* TC flag of channel 2, cleared by the read */
    wr(&d.dev, 8, 0);                                                         /* also resets the first/last flip-flop */
    CHECK(rd(&d.dev, 4) == 0x04 && rd(&d.dev, 4) == 0x10);                    /* address 1004h: low, then high */
    CHECK(rd(&d.dev, 5) == 0xFF && rd(&d.dev, 5) == 0xBF);                    /* count wrapped to 3FFFh, the type bits stay */
    drive(&d.dev, ZR_I8257_IN_DRQ2, 0);

    /* Channel 0 writes the device's bytes to memory (type 01), auto load off. */
    fl.next = 0x70;
    dma_regs(&d, 0, 0x2000, 0x4001);
    wr(&d.dev, 8, 0x01);
    drive(&d.dev, ZR_I8257_IN_DRQ0, 1);
    zr_i8257_cycle(&d);
    zr_i8257_cycle(&d);
    CHECK(f.mem[0x2000] == 0x70 && f.mem[0x2001] == 0x71 && (rd(&d.dev, 8) & 1));
    drive(&d.dev, ZR_I8257_IN_DRQ0, 0);

    /* Auto load: channel 2 goes on from channel 3's registers at its terminal count; the update flag is set. */
    dma_regs(&d, 2, 0x1000, 0x8001);
    dma_regs(&d, 3, 0x1008, 0x8001);
    wr(&d.dev, 8, 0x84);                                           /* auto load, channel 2 enabled */
    fl.n = 0;
    drive(&d.dev, ZR_I8257_IN_DRQ2, 1);
    for (unsigned i = 0; i < 3; i++) zr_i8257_cycle(&d);
    CHECK(fl.got[2] == 0xC8 && rd(&d.dev, 8) == (0x04 | ZR_I8257_ST_UPDATE) && zr_i8257_hrq(&d));
    drive(&d.dev, ZR_I8257_IN_DRQ2, 0);

    /* Rotating priority: with DRQ0 and DRQ1 both up the channels alternate. */
    dma_regs(&d, 0, 0x1000, 0x8007);
    dma_regs(&d, 1, 0x1008, 0x8007);
    wr(&d.dev, 8, 0x13);
    drive(&d.dev, ZR_I8257_IN_DRQ0, 1);
    drive(&d.dev, ZR_I8257_IN_DRQ1, 1);
    zr_i8257_cycle(&d);
    CHECK(fl.channel == 0);                                        /* the last served was channel 2: the order is 3, 0, 1, 2 */
    zr_i8257_cycle(&d);
    CHECK(fl.channel == 1);
    zr_i8257_cycle(&d);
    CHECK(fl.channel == 0);

    /* A save in mid-transfer continues the same. */
    zr_state_writer w;
    save_to(&d.dev, &w);
    fake g;
    zr_i8257 e;
    fake_init(&g);
    memcpy(g.mem, f.mem, sizeof g.mem);
    zr_i8257_init(&e, &g.host, "dma", floppy_dack, &fl);
    CHECK(load_from(&e.dev, &w));
    fl.n = 0;
    CHECK(zr_i8257_cycle(&e) == 4 && fl.channel == 1 && e.addr[1] == d.addr[1] + 1);
    zr_sw_free(&w);
}

int main(void) {
    test_ctc();
    test_ctc_cascade();
    test_pio();
    test_sio();
    test_daisy();
    test_8257();
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
