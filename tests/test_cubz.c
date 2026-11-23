/* The CUB Z's own chips and its byte ports (P7b). The video against a fake host: the column-major bitmap with the scroll offset
 * in the frame, the enable, the WAIT half of the window, a state round trip. The keyboard: its codes alone, with SHIFT, CTRL and
 * CAPS LOCK, the busy time, the nine interrogations, the repeat, a state round trip. Then, with the monitor 1.4 EPROM in roms/,
 * the machine from the host's side (the harness has no command for the guest's byte ports): a program started from the monitor
 * sends a byte through SIO A (clocked by CTC channel 0 counting the 583.125 kHz clock) and one through the user PIO's port A
 * (the "User port" option on PIO port A), and receives one on each from the host; with the option on Serial B, a byte sent
 * through SIO B (as the monitor sets it up) reaches the user port. Without the EPROM that part is skipped. No SDL. */
#include "config.h"
#include "cubzkbd.h"
#include "cubzvideo.h"
#include "machine.h"
#include "romdb.h"
#include "state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* --- a fake host -------------------------------------------------------------------------------------------------------------- */

typedef struct fake {
    zr_hw_host host;
    uint64_t now, deadline;
    int data, clock;
} fake;
static uint64_t f_now(void *ctx) { return ((fake *)ctx)->now; }
static void f_schedule(void *ctx, void *dev, uint64_t when) { (void)dev; ((fake *)ctx)->deadline = when; }
static void f_line(void *ctx, void *dev, int pin, int state) {
    fake *f = ctx;
    (void)dev;
    if (pin == ZR_CUBZKBD_OUT_DATA) f->data = state;
    else f->clock = state;
}
static void fake_init(fake *f) {
    memset(f, 0, sizeof *f);
    f->host.ctx = f;
    f->host.now = f_now;
    f->host.schedule = f_schedule;
    f->host.line = f_line;
    f->deadline = ZR_NEVER;
    f->data = f->clock = 1;
}

/* --- the video -------------------------------------------------------------------------------------------------------------- */

static void test_video(void) {
    static uint8_t vram[ZR_CUBZVIDEO_VRAM];
    static zr_cubzvideo v, w;
    fake f;
    fake_init(&f);
    memset(vram, 0, sizeof vram);
    vram[3 << 8 | 10] = 0x80;                       /* column 3 (dots 24-31), line 10: dot 24 */
    zr_cubzvideo_init(&v, &f.host, vram, ZR_CUBZVIDEO_GREEN);
    const uint64_t frame = zr_cubzvideo_frame_ticks(&v);
    CHECK(frame == 640u * 313u);
    zr_cubzvideo_catch_up(&v, frame);
    zr_frame fr;
    CHECK(zr_cubzvideo_frame(&v, &fr) && fr.width == 544u && fr.height == 288u && fr.palette[1] == 0x30FF30u);
    CHECK(fr.pixels[(16u + 10u) * 544u + 16u + 24u] == 0);         /* the display is not enabled */
    zr_cubzvideo_set_enable(&v, 1);
    zr_cubzvideo_catch_up(&v, 2u * frame);
    CHECK(fr.pixels[(16u + 10u) * 544u + 16u + 24u] == 1 && fr.pixels[(16u + 10u) * 544u + 16u + 25u] == 0);
    zr_cubzvideo_set_scroll(&v, 4);                 /* screen line 6 shows RAM line 10 */
    zr_cubzvideo_catch_up(&v, 3u * frame);
    CHECK(fr.pixels[(16u + 6u) * 544u + 16u + 24u] == 1 && fr.pixels[(16u + 10u) * 544u + 16u + 24u] == 0);
    CHECK(zr_cubzvideo_address(24, 6, 4) == (3 << 8 | 10));
    CHECK(zr_cubzvideo_wait(0) == 0 && zr_cubzvideo_wait(4) == 1 && zr_cubzvideo_wait(8) == 0 && zr_cubzvideo_wait(639) == 1);
    zr_state_writer sw;
    zr_sw_init(&sw);
    zr_sw_header(&sw, "cubz", "cubz14", NULL, 0);
    zr_sw_begin(&sw, "VID ", 0);
    v.dev.ops->save(&v, &sw);
    zr_sw_end(&sw);
    CHECK(zr_sw_finish(&sw));
    zr_cubzvideo_init(&w, &f.host, vram, ZR_CUBZVIDEO_WHITE);
    zr_state_reader sr;
    char error[128];
    CHECK(zr_sr_open(&sr, sw.data, sw.size, error, sizeof error) && zr_sr_chunk(&sr, "VID ", 0) && w.dev.ops->load(&w, &sr));
    CHECK(w.scroll == 4 && w.enable == 1 && w.drawn == v.drawn && !memcmp(w.pixels, v.pixels, sizeof v.pixels));
    zr_sw_free(&sw);
}

/* --- the keyboard ----------------------------------------------------------------------------------------------------------- */

/* One interrogation pulse; the level the processor reads after it. */
static int interrogate(zr_cubzkbd *k, fake *f) {
    k->dev.ops->input(k, ZR_CUBZKBD_IN_INTERROGATE, 1);
    CHECK(f->data == 0);                            /* the line reads 0 while it is interrogated */
    k->dev.ops->input(k, ZR_CUBZKBD_IN_INTERROGATE, 0);
    return f->data;
}
/* The monitor's routine: a start bit, then eight bits; -1 when no code is ready. */
static int read_code(zr_cubzkbd *k, fake *f) {
    if (interrogate(k, f)) return -1;
    int v = 0;
    for (int i = 0; i < 8; ++i) v |= interrogate(k, f) << i;
    return v;
}
static void advance(zr_cubzkbd *k, fake *f, uint64_t to) {
    while (f->deadline <= to) {
        f->now = f->deadline;
        f->deadline = ZR_NEVER;
        k->dev.ops->event(k, f->now);
    }
    f->now = to;
}

static void test_keyboard(void) {
    static zr_cubzkbd k;
    fake f;
    fake_init(&f);
    zr_cubzkbd_init(&k, &f.host, "keyboard", 10000000u);
    k.dev.ops->reset(&k);
    CHECK(read_code(&k, &f) == -1);
    zr_cubzkbd_key(&k, 0x20, 1);                    /* A */
    CHECK(f.clock == 0);                            /* busy for 100 us: the link answers 1 */
    CHECK(read_code(&k, &f) == -1);
    advance(&k, &f, 1000u);
    CHECK(f.clock == 1 && read_code(&k, &f) == 'a' && read_code(&k, &f) == -1);
    zr_cubzkbd_key(&k, 0x20, 0);
    zr_cubzkbd_key(&k, ZR_CUBZKBD_SHIFT_LEFT, 1);
    zr_cubzkbd_key(&k, 0x20, 1);
    zr_cubzkbd_key(&k, 0x20, 0);
    zr_cubzkbd_key(&k, ZR_CUBZKBD_SHIFT_LEFT, 0);
    zr_cubzkbd_key(&k, ZR_CUBZKBD_CTRL, 1);
    zr_cubzkbd_key(&k, 0x20, 1);
    zr_cubzkbd_key(&k, 0x20, 0);
    zr_cubzkbd_key(&k, ZR_CUBZKBD_CTRL, 0);
    zr_cubzkbd_key(&k, ZR_CUBZKBD_CAPS, 1);
    zr_cubzkbd_key(&k, ZR_CUBZKBD_CAPS, 0);
    zr_cubzkbd_key(&k, 0x20, 1);
    zr_cubzkbd_key(&k, 0x20, 0);
    zr_cubzkbd_key(&k, 0x01, 1);                    /* 2: CAPS LOCK leaves the digits */
    advance(&k, &f, f.now + 1000u);
    CHECK(read_code(&k, &f) == 'A' && read_code(&k, &f) == 0x01 && read_code(&k, &f) == 'A' && read_code(&k, &f) == '2');
    /* Held: the first repeat 400 ms after the press, then 10 a second. */
    advance(&k, &f, f.now + 3990000u);
    CHECK(read_code(&k, &f) == -1);
    advance(&k, &f, f.now + 1020000u);
    CHECK(read_code(&k, &f) == '2' && read_code(&k, &f) == '2' && read_code(&k, &f) == -1);
    zr_cubzkbd_key(&k, 0x01, 0);
    CHECK(zr_cubzkbd_code(&k, 0xD0) == 0xA0 && zr_cubzkbd_code(&k, 0x9B) == 0xD3 && zr_cubzkbd_code(&k, 0x0E) == -1);   /* F1, pad 0 (CAPS on) */
    /* A state with a code half read. */
    zr_cubzkbd_key(&k, 0x93, 1);                    /* SPACE */
    advance(&k, &f, f.now + 1000u);
    CHECK(interrogate(&k, &f) == 0 && interrogate(&k, &f) == 0);
    zr_state_writer sw;
    zr_sw_init(&sw);
    zr_sw_header(&sw, "cubz", "cubz14", NULL, 0);
    zr_sw_begin(&sw, "KBD ", 0);
    k.dev.ops->save(&k, &sw);
    zr_sw_end(&sw);
    CHECK(zr_sw_finish(&sw));
    static zr_cubzkbd l;
    zr_cubzkbd_init(&l, &f.host, "keyboard", 10000000u);
    zr_state_reader sr;
    char error[128];
    CHECK(zr_sr_open(&sr, sw.data, sw.size, error, sizeof error) && zr_sr_chunk(&sr, "KBD ", 0) && l.dev.ops->load(&l, &sr));
    int v = 0;
    for (int i = 1; i < 8; ++i) v |= interrogate(&l, &f) << i;
    CHECK(v == 0x20);
    zr_sw_free(&sw);
}

/* --- the machine's byte ports ------------------------------------------------------------------------------------------------- */

typedef struct ports {
    uint8_t got[2][16];
    unsigned count[2];
} ports;
static void transmit(void *ctx, unsigned port, uint8_t byte) {
    ports *p = ctx;
    if (port < 2u && p->count[port] < 16u) p->got[port][p->count[port]++] = byte;
}

static zr_machine *build(const zr_rom_index *roms, const char *user_port) {
    static zr_config config;
    char yaml[256], error[256];
    snprintf(yaml, sizeof yaml, "machine:\n  model: cubz14\n  options:\n    user_port: %s\n    drive_sound: off\n", user_port);
    if (!zr_config_parse(yaml, strlen(yaml), ZR_WORK "/tests/zetro.yaml", &config, error, sizeof error)) return NULL;
    zr_machine *m = zr_machine_create(&config, roms, error, sizeof error);
    if (m && strcmp(zr_machine_family(m)->id, "cubz")) {
        zr_machine_destroy(m);
        return NULL;
    }
    return m;
}
static void frames(zr_machine *m, unsigned count) {
    for (unsigned i = 0; i < count; ++i) zr_machine_run(m, zr_machine_frame_period(m));
}
static void type(zr_machine *m, const char *text) {
    for (; *text; ++text) {
        uint8_t key, shift;
        CHECK(zr_family_typing(zr_machine_family(m), *text, &key, &shift));
        if (shift != ZR_KEY_NONE) zr_machine_key(m, shift, 1);
        zr_machine_key(m, key, 1);
        frames(m, 2);
        zr_machine_key(m, key, 0);
        if (shift != ZR_KEY_NONE) zr_machine_key(m, shift, 0);
        frames(m, 2);
    }
}
static uint8_t peek(zr_machine *m, uint32_t a) {
    uint8_t v = 0;
    CHECK(zr_machine_peek(m, a, &v));
    return v;
}
static void run_program(zr_machine *m, const uint8_t *code, size_t size) {
    frames(m, 150);                                 /* the monitor's banner and prompt */
    for (size_t i = 0; i < size; ++i) CHECK(zr_machine_poke(m, 0x4000u + (uint32_t)i, code[i]));
    type(m, "G4000\n");
    frames(m, 10);
}

static void test_ports(const zr_rom_index *roms) {
    /* SIO A through CTC channel 0 (a counter of 4 on the 583.125 kHz clock: 145.8 kHz, x16), 8 bits, no parity; the user PIO's
     * port A in mode 0, then in mode 1. */
    static const uint8_t PROGRAM[] = {
        0xF3, 0x3E, 0x55, 0xD3, 0xD0, 0x3E, 0x04, 0xD3, 0xD0,         /* di; CTC 0: counter, time constant 4 */
        0x21, 0x40, 0x40, 0x01, 0xE1, 0x0A, 0xED, 0xB3,               /* the SIO A's set-up from 4040h */
        0x3E, 0x0F, 0xD3, 0xA1, 0x3E, 0x51, 0xD3, 0xA0,               /* user PIO A mode 0, 'Q' */
        0x3E, 0x5A, 0xD3, 0xE0,                                       /* 'Z' through SIO A */
        0xDB, 0xE1, 0xCB, 0x47, 0x28, 0xFA, 0xDB, 0xE0, 0x32, 0x00, 0x42,   /* wait for a byte, to 4200h */
        0x3E, 0x4F, 0xD3, 0xA1,                                       /* user PIO A mode 1 */
        0xDB, 0xA0, 0x32, 0x01, 0x42, 0x18, 0xF9,                     /* port A's register to 4201h, again */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0x18, 0x04, 0x44, 0x03, 0xC1, 0x05, 0x68, 0x01, 0x00, 0x00,   /* 4040h: reset, x16 1 stop, Rx 8 on, Tx 8 on, no interrupts */
    };
    ports p;
    memset(&p, 0, sizeof p);
    const zr_port_host host = {&p, transmit, NULL, NULL};
    zr_machine *m = build(roms, "pio_a");
    CHECK(m != NULL);
    if (!m) return;
    zr_machine_set_ports(m, &host);
    _Static_assert(sizeof PROGRAM == 0x4A, "the SIO's set-up at 4040h");
    run_program(m, PROGRAM, sizeof PROGRAM);
    CHECK(p.count[0] == 1 && p.got[0][0] == 'Z');
    CHECK(p.count[1] == 1 && p.got[1][0] == 'Q');
    const uint8_t k = 'k', w = 'w';
    CHECK(zr_machine_port_receive(m, 0, &k, 1) == 1);
    frames(m, 5);
    CHECK(peek(m, 0x4200) == 'k');
    CHECK(zr_machine_port_receive(m, 1, &w, 1) == 1);
    frames(m, 5);
    CHECK(peek(m, 0x4201) == 'w');
    zr_machine_destroy(m);
    /* SIO B as the monitor sets it up (CTC channel 1, 8 bits), the user port on Serial B. */
    static const uint8_t SEND_B[] = {0x3E, 0x50, 0xD3, 0xE2, 0x18, 0xFE};
    memset(&p, 0, sizeof p);
    m = build(roms, "serial_b");
    CHECK(m != NULL);
    if (!m) return;
    zr_machine_set_ports(m, &host);
    run_program(m, SEND_B, sizeof SEND_B);
    CHECK(p.count[1] == 0);                         /* the monitor sets auto enables: the printer has not said CTS yet */
    zr_machine_port_modem(m, 1, 1, 1, 1, 0);
    frames(m, 5);
    CHECK(p.count[1] == 1 && p.got[1][0] == 'P' && p.count[0] == 0);
    zr_machine_destroy(m);
}

int main(void) {
    test_video();
    test_keyboard();
    static zr_rom_index roms;
    const char *folders[2] = {ZR_WORK "/../roms", ZR_WORK "/../../../../roms"};
    zr_rom_index_init(&roms);
    zr_rom_index_scan(&roms, folders, 2);
    zr_machine *probe = build(&roms, "serial_b");
    if (probe) {
        zr_machine_destroy(probe);
        test_ports(&roms);
    } else printf("the byte ports skipped: no CUB Z EPROM in roms/ (python tools/fetch_roms.py)\n");
    zr_rom_index_free(&roms);
    if (failures) printf("%d failures\n", failures);
    else printf("test_cubz passed\n");
    return failures ? 1 : 0;
}
