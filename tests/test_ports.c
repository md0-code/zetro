/* The 8255, 8253 and 8251 against a fake host: a few checks each, no tables.
 * The 8255's modes 0, 1 and 2 and its bit set/reset; the 8253's terminal
 * count, BCD, latch, gate, an exact non-integer clock ratio, a counter clocked
 * from another's OUT, and a save that continues exactly; the 8251's character
 * time, loopback, overrun, CTS, a clock change in mid-character, synchronous
 * hunt and a save in mid-character. No SDL. */
#include "i8251.h"
#include "i8253.h"
#include "i8255.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* --- the fake board: a clock, the chips' deadlines, what their pins did ---------------------- */

typedef struct fake {
    zr_hw_host host;
    uint64_t now;
    zr_dev *dev[2];
    uint64_t when[2];
    unsigned devices;
    int level[24];              /* the last level of each output pin */
    unsigned count[24][2];      /* how many times each pin went to 0 and to 1 */
    uint64_t last[24][2];       /* and when it last did */
    void (*tap)(struct fake *f, int pin, int state);   /* the board's wiring of an output to an input */
    void *tap_dev;
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
    (void)dev;
    f->level[pin] = state;
    f->count[pin][state != 0]++;
    f->last[pin][state != 0] = f->now;
    if (f->tap) f->tap(f, pin, state);
}
static void fake_init(fake *f) {
    memset(f, 0, sizeof *f);
    f->host.ctx = f;
    f->host.now = h_now;
    f->host.schedule = h_schedule;
    f->host.line = h_line;
}
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

/* --- 8255 ----------------------------------------------------------------------------------- */

static void test_8255(void) {
    fake f;
    zr_i8255 p;
    fake_init(&f);
    zr_i8255_init(&p, &f.host, "ppi");
    p.dev.ops->reset(&p);
    CHECK(f.count[0][1] == 0 && f.count[20][1] == 0);             /* every pin starts as an input */

    /* Mode 0 output: the latch drives the pins, a read returns the latch. */
    wr(&p.dev, 3, 0x80);
    wr(&p.dev, 0, 0xA5);
    CHECK(zr_i8255_out(&p, ZR_I8255_PORT_A) == 0xA5 && zr_i8255_driven(&p, ZR_I8255_PORT_A) == 0xFF);
    CHECK(f.level[0] == 1 && f.level[1] == 0 && f.level[7] == 1 && rd(&p.dev, 0) == 0xA5);
    wr(&p.dev, 3, 0x09);                                           /* bit set PC4 */
    CHECK(f.level[ZR_I8255_PIN(ZR_I8255_PORT_C, 4)] == 1);
    wr(&p.dev, 3, 0x08);                                           /* bit reset PC4 */
    CHECK(f.level[ZR_I8255_PIN(ZR_I8255_PORT_C, 4)] == 0);

    /* Mode 0 input: the pins are read as they are, no latching. */
    wr(&p.dev, 3, 0x9B);
    drive(&p.dev, ZR_I8255_PORT(ZR_I8255_PORT_A), 0x3C);
    CHECK(rd(&p.dev, 0) == 0x3C && zr_i8255_driven(&p, ZR_I8255_PORT_A) == 0);
    drive(&p.dev, ZR_I8255_PORT(ZR_I8255_PORT_A), 0xE7);
    CHECK(rd(&p.dev, 0) == 0xE7);

    /* Mode 1 strobed input: STB low latches, STB high raises INTR (INTE set), the read drops it. */
    const int intr = ZR_I8255_PIN(ZR_I8255_PORT_C, 3), ibf = ZR_I8255_PIN(ZR_I8255_PORT_C, 5), stb = ZR_I8255_PIN(ZR_I8255_PORT_C, 4);
    wr(&p.dev, 3, 0xB0);
    wr(&p.dev, 3, 0x09);                                           /* INTE A */
    drive(&p.dev, stb, 0);
    drive(&p.dev, ZR_I8255_PORT(ZR_I8255_PORT_A), 0x3C);
    CHECK(f.level[ibf] == 1 && f.level[intr] == 0);
    drive(&p.dev, stb, 1);
    drive(&p.dev, ZR_I8255_PORT(ZR_I8255_PORT_A), 0xE7);             /* the latch kept the strobed byte */
    CHECK(f.level[intr] == 1);
    zr_state_writer w;
    save_to(&p.dev, &w);
    CHECK(rd(&p.dev, 0) == 0x3C && f.level[intr] == 0 && f.level[ibf] == 0);
    CHECK(load_from(&p.dev, &w) && rd(&p.dev, 0) == 0x3C);         /* a load puts the latch and IBF back */
    zr_sw_free(&w);

    /* Mode 2: port A bidirectional, the handshake pins driven, OBF falls on a write. */
    const int obf = ZR_I8255_PIN(ZR_I8255_PORT_C, 7), ack = ZR_I8255_PIN(ZR_I8255_PORT_C, 6);
    drive(&p.dev, ack, 1);
    wr(&p.dev, 3, 0xC0);
    CHECK((zr_i8255_driven(&p, ZR_I8255_PORT_C) & 0xF8) == 0xA8 && f.level[obf] == 1);
    wr(&p.dev, 0, 0x77);
    CHECK(f.level[obf] == 0 && zr_i8255_driven(&p, ZR_I8255_PORT_A) == 0);   /* the bus drivers wait for ACK */
    drive(&p.dev, ack, 0);
    CHECK(f.level[obf] == 1 && zr_i8255_out(&p, ZR_I8255_PORT_A) == 0x77 && zr_i8255_driven(&p, ZR_I8255_PORT_A) == 0xFF);
}

/* --- 8253 ----------------------------------------------------------------------------------- */

static void cascade(fake *f, int pin_out, int state) {
    if (pin_out == ZR_I8253_OUT0) drive(f->tap_dev, ZR_I8253_CLK1, state);   /* OUT0 wired to CLK1 */
}

static void test_8253(void) {
    fake f;
    zr_i8253 t;
    fake_init(&f);
    zr_i8253_init(&t, &f.host, "pit", 1, 1);                       /* every count clock is a master period */
    t.dev.ops->reset(&t);

    /* Mode 0, count 3: OUT goes high N + 1 clocks after the write (the first clock loads the count). */
    wr(&t.dev, 3, 0x30);
    wr(&t.dev, 0, 3);
    wr(&t.dev, 0, 0);
    run_to(&f, 3);
    CHECK(f.level[ZR_I8253_OUT0] == 0);
    run_to(&f, 4);
    CHECK(f.level[ZR_I8253_OUT0] == 1 && f.last[ZR_I8253_OUT0][1] == 4);

    /* BCD with a latch, and a gate that pauses the count. */
    wr(&t.dev, 3, 0x71);                                           /* counter 1: mode 0, BCD, LSB then MSB */
    wr(&t.dev, 1, 0x00);
    wr(&t.dev, 1, 0x01);
    run_to(&f, 6);                                                 /* the load clock, then one count */
    wr(&t.dev, 3, 0x40);                                           /* latch counter 1 */
    CHECK(rd(&t.dev, 1) == 0x99 && rd(&t.dev, 1) == 0x00);
    drive(&t.dev, ZR_I8253_GATE1, 0);
    run_to(&f, 20);
    wr(&t.dev, 3, 0x40);
    CHECK(rd(&t.dev, 1) == 0x99 && rd(&t.dev, 1) == 0x00);
    drive(&t.dev, ZR_I8253_GATE1, 1);
    run_to(&f, 21);
    wr(&t.dev, 3, 0x40);
    CHECK(rd(&t.dev, 1) == 0x98 && rd(&t.dev, 1) == 0x00);

    /* Mode 3 on a clock of 7/2 master periods, count 4: a fall every 4 clocks = 14 periods, exact over 14000. */
    const uint64_t e = f.now;
    zr_i8253_set_clock(&t, 2, 7, 2);
    wr(&t.dev, 3, 0xB6);
    wr(&t.dev, 2, 4);
    wr(&t.dev, 2, 0);
    run_to(&f, e + 5000);
    zr_state_writer w;
    save_to(&t.dev, &w);
    const unsigned falls_at_save = f.count[ZR_I8253_OUT2][0];
    run_to(&f, e + 14000);
    CHECK(f.count[ZR_I8253_OUT2][0] == 1000 && f.last[ZR_I8253_OUT2][0] == e + 13997);
    wr(&t.dev, 3, 0x80);                                           /* latch counter 2 */
    const uint8_t lo = rd(&t.dev, 2), hi = rd(&t.dev, 2);

    /* The same saved at e + 5000 and loaded into a second chip goes on exactly the same. */
    fake g;
    zr_i8253 u;
    fake_init(&g);
    zr_i8253_init(&u, &g.host, "pit", 1, 1);
    g.now = e + 5000;
    CHECK(load_from(&u.dev, &w));
    run_to(&g, e + 14000);
    CHECK(g.count[ZR_I8253_OUT2][0] == 1000 - falls_at_save && g.last[ZR_I8253_OUT2][0] == f.last[ZR_I8253_OUT2][0]);
    wr(&u.dev, 3, 0x80);
    CHECK(rd(&u.dev, 2) == lo && rd(&u.dev, 2) == hi);
    zr_sw_free(&w);

    /* A counter clocked from another's OUT: counter 1 (pin clocked, mode 0, count 2) counts the falls of
     * counter 0 (mode 2, count 3), which come every 3 clocks; OUT1 rises on the third. */
    fake h;
    zr_i8253 c;
    fake_init(&h);
    zr_i8253_init(&c, &h.host, "pit", 1, 1);
    h.tap = cascade;
    h.tap_dev = &c;
    c.dev.ops->reset(&c);
    zr_i8253_set_clock(&c, 1, 0, 0);
    wr(&c.dev, 3, 0x70);
    wr(&c.dev, 1, 2);
    wr(&c.dev, 1, 0);
    wr(&c.dev, 3, 0x34);
    wr(&c.dev, 0, 3);
    wr(&c.dev, 0, 0);
    run_to(&h, 8);
    CHECK(h.level[ZR_I8253_OUT1] == 0);
    run_to(&h, 9);
    CHECK(h.level[ZR_I8253_OUT1] == 1 && h.last[ZR_I8253_OUT1][1] == 9);
}

/* --- 8251 ----------------------------------------------------------------------------------- */

typedef struct sink {
    fake *f;
    unsigned n;
    uint8_t last;
    uint64_t when;
} sink;
static void sink_byte(void *ctx, uint8_t byte) {
    sink *s = ctx;
    s->n++;
    s->last = byte;
    s->when = s->f->now;
}

/* The chip as after reset, programmed (a negative command: the mode only); clocks 100 kHz on a 1 MHz master (10 periods per clock edge). */
static void usart_start(zr_i8251 *u, fake *f, sink *s, uint8_t mode, int command) {
    fake_init(f);
    memset(s, 0, sizeof *s);
    s->f = f;
    zr_i8251_init(u, &f->host, "usart", 1000000, 100000, 100000);
    zr_i8251_set_transmit(u, sink_byte, s);
    u->dev.ops->reset(u);
    wr(&u->dev, 1, mode);
    if (command >= 0) wr(&u->dev, 1, (uint8_t)command);
}

static void test_8251(void) {
    fake f;
    sink s;
    zr_i8251 u;

    /* Asynchronous x1, 8N1: a character takes 10 clocks = 100 periods; TxRDY and TxEMPTY as the data sheet says. */
    usart_start(&u, &f, &s, 0x4D, 0x27);
    CHECK(rd(&u.dev, 1) == 0x05 && f.level[ZR_I8251_PIN_TXRDY] == 1 && f.level[ZR_I8251_PIN_TXEMPTY] == 1);
    wr(&u.dev, 0, 0xA5);
    CHECK(f.level[ZR_I8251_PIN_TXEMPTY] == 0 && f.level[ZR_I8251_PIN_TXRDY] == 1);   /* the buffer moved to the shifter */
    run_to(&f, 99);
    CHECK(s.n == 0);
    run_to(&f, 100);
    CHECK(s.n == 1 && s.last == 0xA5 && s.when == 100 && f.level[ZR_I8251_PIN_TXEMPTY] == 1);

    /* Loopback: the character is readable when its last bit has gone out; two unread ones overrun, the newer stays. */
    zr_i8251_set_loopback(&u, 1);
    CHECK(!zr_i8251_receive(&u, 1));
    wr(&u.dev, 0, 0x5A);
    run_to(&f, 199);
    CHECK((rd(&u.dev, 1) & 0x02) == 0);
    run_to(&f, 200);
    CHECK((rd(&u.dev, 1) & 0x02) && f.level[ZR_I8251_PIN_RXRDY] == 1 && rd(&u.dev, 0) == 0x5A && (rd(&u.dev, 1) & 0x02) == 0);
    wr(&u.dev, 0, 0x11);
    wr(&u.dev, 0, 0x22);
    run_to(&f, 400);
    CHECK((rd(&u.dev, 1) & 0x12) == 0x12 && rd(&u.dev, 0) == 0x22);
    wr(&u.dev, 1, 0x37);                                           /* error reset */
    CHECK((rd(&u.dev, 1) & 0x38) == 0);

    /* CTS gates the transmitter; the status TxRDY bit is not conditioned by it, the pin is. */
    zr_i8251_set_loopback(&u, 0);
    drive(&u.dev, ZR_I8251_IN_CTS, 0);
    CHECK(f.level[ZR_I8251_PIN_TXRDY] == 0 && (rd(&u.dev, 1) & 0x01));
    const unsigned before = s.n;
    wr(&u.dev, 0, 0x77);
    run_to(&f, 1400);
    CHECK(s.n == before && (rd(&u.dev, 1) & 0x01) == 0);
    drive(&u.dev, ZR_I8251_IN_CTS, 1);
    run_to(&f, 1500);
    CHECK(s.n == before + 1 && s.last == 0x77 && s.when == 1500);

    /* A received character is readable after its character time, and one at a time. */
    CHECK(zr_i8251_receive(&u, 0x3C) && !zr_i8251_receive(&u, 0x3D));
    run_to(&f, 1599);
    CHECK((rd(&u.dev, 1) & 0x02) == 0);
    run_to(&f, 1600);
    CHECK(rd(&u.dev, 0) == 0x3C && zr_i8251_receive(&u, 0x3D));

    /* A clock change in mid-character: 50 periods gone, 5 clocks left, which now take 5 periods each. */
    run_to(&f, 1700);
    wr(&u.dev, 0, 0x99);
    run_to(&f, 1750);
    zr_i8251_set_clock_hz(&u, ZR_I8251_TXC, 200000);
    run_to(&f, 1774);
    CHECK(s.last == 0x77);
    run_to(&f, 1775);
    CHECK(s.last == 0x99 && s.when == 1775);

    /* 7E2 at x16: (1 + 7 + 1) * 16 + 2 * 16 = 176 clocks to send, (1 + 7 + 1 + 1) * 16 = 160 to receive. */
    usart_start(&u, &f, &s, 0xFA, 0x27);
    wr(&u.dev, 0, 0xFF);
    run_to(&f, 1759);
    CHECK(s.n == 0);
    run_to(&f, 1760);
    CHECK(s.n == 1 && s.last == 0x7F);                             /* the eighth bit is not sent */
    CHECK(zr_i8251_receive(&u, 0x55));
    run_to(&f, 1760 + 1599);
    CHECK((rd(&u.dev, 1) & 0x02) == 0);
    run_to(&f, 1760 + 1600);
    CHECK(rd(&u.dev, 0) == 0x55);

    /* Synchronous, two sync characters, internal detect: hunt consumes them, SYNDET reads once. */
    usart_start(&u, &f, &s, 0x0C, -1);
    wr(&u.dev, 1, 0x16);
    wr(&u.dev, 1, 0x2A);
    wr(&u.dev, 1, 0x85);                                           /* TxEN, RxE, enter hunt */
    CHECK(zr_i8251_receive(&u, 0x16));
    run_to(&f, 80);
    CHECK(zr_i8251_receive(&u, 0x2A));
    run_to(&f, 160);
    CHECK(f.level[ZR_I8251_PIN_SYNDET] == 1 && (rd(&u.dev, 1) & 0x42) == 0x40 && (rd(&u.dev, 1) & 0x40) == 0);
    CHECK(zr_i8251_receive(&u, 0xB9));
    run_to(&f, 240);
    CHECK(rd(&u.dev, 0) == 0xB9);
    /* After the first character the transmitter fills with the sync characters; the program sees only its own. */
    zr_i8251_set_loopback(&u, 1);
    wr(&u.dev, 0, 0x55);
    run_to(&f, 240 + 80 * 6);
    CHECK(s.n == 1 && s.last == 0x55 && f.level[ZR_I8251_PIN_SYNDET] == 1);

    /* A save in mid-character, loaded into another chip, ends the character at the same moment. */
    usart_start(&u, &f, &s, 0x4D, 0x27);
    wr(&u.dev, 0, 0xC3);
    run_to(&f, 40);
    zr_state_writer w;
    save_to(&u.dev, &w);
    fake g;
    sink t;
    zr_i8251 v;
    fake_init(&g);
    memset(&t, 0, sizeof t);
    t.f = &g;
    zr_i8251_init(&v, &g.host, "usart", 1000000, 0, 0);
    zr_i8251_set_transmit(&v, sink_byte, &t);
    g.now = 40;
    CHECK(load_from(&v.dev, &w));
    run_to(&g, 99);
    CHECK(t.n == 0);
    run_to(&g, 100);
    CHECK(t.n == 1 && t.last == 0xC3 && t.when == 100);
    zr_sw_free(&w);
}

int main(void) {
    test_8255();
    test_8253();
    test_8251();
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
