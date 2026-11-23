/* The TIM-S Plus: (1) its video (hw/timsvideo.h) against a fake host: the single and double resolutions (the double one's bytes
 * from the page and its twin by turns), the hardware scroll, the wide window's ninth to fortieth cells, the attribute inhibit,
 * the frame's geometry, the interrupt (128 dots at line 248) and a state round trip; (2) from the host's side (the harness has no
 * command for the byte ports), on the replacement bootstrap and the +3 system: each of the five 8251 channels, selected by the
 * "UART channel" option and the multiplexer at AFFDh and clocked by its 8253 counter, sends a byte through a loopback plug on the
 * serial port and receives it with DSR (from its own DTR) up, as the core's native diagnostic does; and a byte written to 0FFDh
 * reaches the printer port at the strobe's falling edge (1FFDh bit 4). Part 2 is skipped without the ROMs in roms/. No SDL. */
#include "config.h"
#include "machine.h"
#include "romdb.h"
#include "state.h"
#include "timsvideo.h"
#include "../src/app/typist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* --- (1) the video ------------------------------------------------------------------------------------------------ */

#define DOTS ZR_TIMSVIDEO_DOTS
#define TOTAL ((uint64_t)ZR_TIMSVIDEO_DOTS * ZR_TIMSVIDEO_LINES)

static struct {
    uint64_t now, deadline;
    int asserts, releases;
    uint64_t first_assert, release;
} fake;

static uint64_t f_now(void *ctx) { (void)ctx; return fake.now; }
static void f_schedule(void *ctx, void *dev, uint64_t when) { (void)ctx; (void)dev; fake.deadline = when; }
static void f_line(void *ctx, void *dev, int pin, int state) {
    (void)ctx; (void)dev; (void)pin;
    if (state && ++fake.asserts == 1) fake.first_assert = fake.now;
    if (!state && ++fake.releases == 1) fake.release = fake.now;
}
static const zr_hw_host HOST = {NULL, f_now, f_schedule, f_line, NULL};

static void run_to(zr_timsvideo *v, uint64_t t) {
    while (fake.deadline <= t) {
        fake.now = fake.deadline;
        v->dev.ops->event(v, fake.now);
    }
    fake.now = t;
    zr_timsvideo_catch_up(v, t);
}

/* The colour of the picture's dot (x, y) in the frame drawn (the window starts 32 dots and 48 lines before the picture). */
static uint8_t dot(const zr_timsvideo *v, unsigned x, unsigned y) { return v->pixels[(size_t)(y + 48u) * ZR_TIMSVIDEO_WIDTH + x + 32u]; }

/* The Spectrum's offset of a line's bitmap byte. */
static unsigned line_offset(unsigned y, unsigned cell) { return ((y & 0xC0u) << 5) | ((y & 7u) << 8) | ((y & 0x38u) << 2) | cell; }

static void test_video(void) {
    static uint8_t vram[ZR_TIMSVIDEO_VRAM];
    static zr_timsvideo v;
    memset(vram, 0, sizeof vram);
    /* page 0: line 0 cell 0 = 80h (one pixel), line 8 cell 0 = FFh, attributes ink 2 paper 1 (row 0) and ink 4 paper 6 (row 1);
     * page 1: line 0 cell 0 = 01h, attribute ink 5 paper 0 */
    vram[line_offset(0, 0)] = 0x80u;
    vram[line_offset(8, 0)] = 0xFFu;
    vram[0x1800] = 0x0Au;
    vram[0x1820] = 0x34u;
    vram[0x2000 + line_offset(0, 0)] = 0x01u;
    vram[0x2000 + 0x1800] = 0x05u;
    vram[0x1800 | 1u << 8 | 0u << 5 | 0u << 3 | 0u] = 0xAAu;     /* the wide window's cell 32 of line 1 */
    memset(&fake, 0, sizeof fake);
    fake.deadline = ZR_NEVER;
    zr_timsvideo_init(&v, &HOST, 1, vram);
    zr_timsvideo_set_border(&v, 3);
    zr_timsvideo_set_mode(&v, 0, 0, 0);
    run_to(&v, 2u * TOTAL);
    /* single: a pixel is two dots */
    CHECK(dot(&v, 0, 0) == 2 && dot(&v, 1, 0) == 2 && dot(&v, 2, 0) == 1);
    CHECK(dot(&v, 0, 8) == 4 && dot(&v, 15, 8) == 4);
    CHECK(dot(&v, 512, 0) == 3 && dot(&v, 0, 192) == 3);                        /* the border */
    CHECK(v.pixels[0] == 0 && v.pixels[40] == 3);                               /* the first scan line's left end is blanked */
    /* the interrupt: line 248's first dot processed, 128 dots long */
    CHECK(fake.asserts >= 1 && fake.first_assert == 248u * DOTS + 1u + 1u && fake.release == fake.first_assert + 128u);
    /* double: the cell's bytes from page 0 and page 1 by turns, each with its own attribute */
    zr_timsvideo_set_mode(&v, 0, 1, 0);
    run_to(&v, 3u * TOTAL);
    CHECK(dot(&v, 0, 0) == 2 && dot(&v, 1, 0) == 1 && dot(&v, 15, 0) == 5 && dot(&v, 14, 0) == 0);
    /* the scroll: the picture's line 0 shows the memory's line 8 */
    zr_timsvideo_set_mode(&v, 0, 0, 0);
    zr_timsvideo_set_scroll(&v, 8);
    run_to(&v, 4u * TOTAL);
    CHECK(dot(&v, 0, 0) == 4 && dot(&v, 0, 184) == 2);
    /* wide: cell 32 of line 1 from 1800h + (1 << 8) */
    zr_timsvideo_set_scroll(&v, 0);
    zr_timsvideo_set_mode(&v, 1, 0, 0);
    CHECK(zr_timsvideo_cell_address(&v, 1, 32) == 0x1900u);
    run_to(&v, 5u * TOTAL);
    CHECK(dot(&v, 512, 0) != 3 && dot(&v, 639, 0) != 3 && dot(&v, 640, 0) == 3);
    /* the inhibit: every cell keeps the attribute last taken */
    zr_timsvideo_set_mode(&v, 0, 0, 1);
    run_to(&v, 6u * TOTAL);
    const uint8_t held = dot(&v, 2, 8);
    CHECK(dot(&v, 2, 0) == held && dot(&v, 300, 100) == held);
}

/* A plain round trip of the chip's chunk (the state writer frames chunks; the chip's own bytes are what is compared). */
static void test_video_state(void) {
    static uint8_t vram[ZR_TIMSVIDEO_VRAM];
    static zr_timsvideo a, b;
    memset(vram, 0x55, sizeof vram);
    memset(&fake, 0, sizeof fake);
    fake.deadline = ZR_NEVER;
    zr_timsvideo_init(&a, &HOST, 1, vram);
    zr_timsvideo_set_mode(&a, 1, 1, 0);
    zr_timsvideo_set_scroll(&a, 17);
    run_to(&a, TOTAL + 12345u);
    zr_state_writer w;
    zr_sw_init(&w);
    zr_sw_header(&w, "test", "test", NULL, 0);
    zr_sw_begin(&w, "TVID", 0);
    a.dev.ops->save(&a, &w);
    zr_sw_end(&w);
    CHECK(zr_sw_finish(&w));
    zr_timsvideo_init(&b, &HOST, 1, vram);
    zr_state_reader r;
    char error[64];
    CHECK(zr_sr_open(&r, w.data, w.size, error, sizeof error) && zr_sr_chunk(&r, "TVID", 0) && b.dev.ops->load(&b, &r));
    zr_sw_free(&w);
    run_to(&a, 2u * TOTAL);
    zr_timsvideo_catch_up(&b, 2u * TOTAL);
    CHECK(!memcmp(a.pixels, b.pixels, sizeof a.pixels) && a.scroll == b.scroll && a.hires == b.hires && a.drawn == b.drawn);
}

/* --- (2) the 8251 channels and the printer on the machine ---------------------------------------------------------------- */

enum { PORT_SERIAL, PORT_PRINTER };

typedef struct host {
    uint8_t printer[16], serial[16];
    unsigned printer_count, serial_count;
} host;

static void transmit(void *ctx, unsigned port, uint8_t byte) {
    host *h = ctx;
    if (port == PORT_PRINTER && h->printer_count < sizeof h->printer) h->printer[h->printer_count++] = byte;
    if (port == PORT_SERIAL && h->serial_count < sizeof h->serial) h->serial[h->serial_count++] = byte;
}
static int loopback(void *ctx, unsigned port) { (void)ctx; return port == PORT_SERIAL; }

static zr_machine *build(const zr_rom_index *roms, unsigned channel) {
    static zr_config config;
    static const char *const CHANNELS[] = {"native", "exp1", "exp2", "exp3", "exp4"};
    char yaml[256], error[256];
    snprintf(yaml, sizeof yaml, "machine:\n  model: timsplus\n  options:\n    uart_channel: %s\n", CHANNELS[channel]);
    if (!zr_config_parse(yaml, strlen(yaml), ZR_WORK "/tests/zetro.yaml", &config, error, sizeof error)) return NULL;
    zr_machine *m = zr_machine_create(&config, roms, error, sizeof error);
    if (m && (strcmp(zr_machine_family(m)->id, "tims") || zr_machine_model(m) != 1u)) {
        zr_machine_destroy(m);
        return NULL;
    }
    return m;
}

static void frames(zr_machine *m, zr_typist *typist, unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        if (typist) zr_typist_step(typist, m);
        zr_machine_run(m, zr_machine_frame_period(m));
    }
}
static int screen_has(zr_machine *m, const char *text) {
    static char buffer[4096];
    zr_machine_text(m, buffer, sizeof buffer);
    return strstr(buffer, text) != NULL;
}
static int until(zr_machine *m, zr_typist *typist, const char *text, unsigned limit) {
    for (unsigned i = 0; i < limit && !screen_has(m, text); ++i) frames(m, typist, 1);
    return screen_has(m, text);
}
static void key(zr_machine *m, const char *name, int down) {
    const int k = zr_family_key(zr_machine_family(m), name);
    CHECK(k >= 0);
    if (k >= 0) zr_machine_key(m, (unsigned)k, down);
}

/* A program for +3 BASIC's RANDOMIZE USR at 9000h; RPASS! at B000h when every check holds, 0EEh there and the step at B007h
 * when one fails. */
typedef struct code {
    uint8_t b[512];
    unsigned n;
    unsigned fails[32], fail_count;
} code;
static void emit(code *c, const uint8_t *bytes, unsigned count) {
    memcpy(c->b + c->n, bytes, count);
    c->n += count;
}
static void out(code *c, uint16_t port, uint8_t value) {
    const uint8_t b[] = {0x01, (uint8_t)port, (uint8_t)(port >> 8), 0x3E, value, 0xED, 0x79};
    emit(c, b, sizeof b);
}
/* IN A,(port); AND mask; CP value; JP NZ,fail (D: the step) */
static void expect(code *c, uint8_t step, uint16_t port, uint8_t mask, uint8_t value) {
    const uint8_t b[] = {0x16, step, 0x01, (uint8_t)port, (uint8_t)(port >> 8), 0xED, 0x78, 0xE6, mask, 0xFE, value, 0xC2, 0, 0};
    emit(c, b, sizeof b);
    c->fails[c->fail_count++] = c->n - 2u;
}

static void channel_program(code *c, unsigned channel) {
    const uint16_t data = channel ? 0xCFFDu : 0xDEFDu, command = channel ? 0xEFFDu : 0xDFFDu;
    c->n = c->fail_count = 0;
    const uint8_t di = 0xF3;
    emit(c, &di, 1);
    /* the channel's clock: mode 3, 13 counts of 2 MHz (9615 baud at x16) */
    if (channel == 0) { out(c, 0x9FFDu, 0x36); out(c, 0x9CFDu, 13); out(c, 0x9CFDu, 0); }
    else if (channel < 4) { out(c, 0x8FFDu, (uint8_t)(((channel - 1u) << 6) | 0x36u)); out(c, (uint16_t)(0x8CFDu + ((channel - 1u) << 8)), 13); out(c, (uint16_t)(0x8CFDu + ((channel - 1u) << 8)), 0); }
    else { out(c, 0x9FFDu, 0xB6); out(c, 0x9EFDu, 13); out(c, 0x9EFDu, 0); }
    if (channel) out(c, 0xAFFDu, (uint8_t)(channel - 1u));
    out(c, command, 0); out(c, command, 0); out(c, command, 0); out(c, command, 0x40); out(c, command, 0x4E); out(c, command, 0x37);
    out(c, data, 0xA5);
    /* wait for RxRDY: B'C' would do; a count in HL bounds it */
    const uint8_t wait[] = {0x21, 0x00, 0x00, 0x01, (uint8_t)command, (uint8_t)(command >> 8), 0xED, 0x78, 0xE6, 0x02, 0x20, 0x05, 0x2B, 0x7C, 0xB5,
                            0x20, 0xF5};
    emit(c, wait, sizeof wait);
    expect(c, 1, data, 0xFF, 0xA5);
    expect(c, 2, command, 0x80, 0x80);                                  /* DSR: the plug's DTR */
    if (channel == 0) {
        /* the printer: 'P' to 0FFDh, the strobe high and low */
        out(c, 0x0FFDu, 'P');
        const uint8_t strobe[] = {0x3A, 0x67, 0x5B, 0xF6, 0x10, 0x01, 0xFD, 0x1F, 0xED, 0x79, 0xE6, 0xEF, 0xED, 0x79};
        emit(c, strobe, sizeof strobe);
    }
    /* passed: copy RPASS! to B000h; failed: EEh to B000h, the step to B007h; then EI, RET */
    const unsigned passed_at = c->n;
    const uint8_t pass[] = {0x21, 0, 0, 0x11, 0x00, 0xB0, 0x01, 0x06, 0x00, 0xED, 0xB0, 0xFB, 0xC9};
    emit(c, pass, sizeof pass);
    const unsigned fail_at = c->n;
    const uint8_t fail[] = {0x7A, 0x32, 0x07, 0xB0, 0x3E, 0xEE, 0x32, 0x00, 0xB0, 0xFB, 0xC9};
    emit(c, fail, sizeof fail);
    const unsigned text_at = c->n;
    emit(c, (const uint8_t *)"RPASS!", 6);
    c->b[passed_at + 1] = (uint8_t)(0x9000u + text_at);
    c->b[passed_at + 2] = (uint8_t)((0x9000u + text_at) >> 8);
    for (unsigned i = 0; i < c->fail_count; ++i) {
        c->b[c->fails[i]] = (uint8_t)(0x9000u + fail_at);
        c->b[c->fails[i] + 1] = (uint8_t)((0x9000u + fail_at) >> 8);
    }
}

static void test_channels(const zr_rom_index *roms) {
    for (unsigned channel = 0; channel < 5u; ++channel) {
        static host h;
        memset(&h, 0, sizeof h);
        zr_typist typist;
        zr_typist_init(&typist);
        zr_machine *m = build(roms, channel);
        CHECK(m != NULL);
        if (!m) return;
        const zr_port_host ports = {.ctx = &h, .transmit = transmit, .loopback = loopback};
        zr_machine_set_ports(m, &ports);
        CHECK(until(m, &typist, "Calculator", 500));
        frames(m, &typist, 10);
        key(m, "CAPS SHIFT", 1);
        key(m, "6", 1);
        frames(m, &typist, 3);
        key(m, "6", 0);
        key(m, "CAPS SHIFT", 0);
        frames(m, &typist, 5);
        CHECK(zr_typist_start(&typist, m, "\n"));
        frames(m, &typist, 50);
        static code c;
        channel_program(&c, channel);
        for (unsigned i = 0; i < c.n; ++i) zr_machine_poke(m, 0x9000u + i, c.b[i]);
        CHECK(zr_typist_start(&typist, m, "randomize usr 36864\n"));
        CHECK(until(m, &typist, "0 OK", 400));
        uint8_t mark[8] = {0};
        for (unsigned i = 0; i < 8u; ++i) zr_machine_peek(m, 0xB000u + i, &mark[i]);
        printf("channel %u: %.6s (step %u), %u bytes to the host, %u to the printer\n", channel, mark[0] == 0xEE ? "failed" : (const char *)mark,
               mark[7], h.serial_count, h.printer_count);
        CHECK(!memcmp(mark, "RPASS!", 6));
        CHECK(h.serial_count == 0);                                     /* the plug took it */
        if (channel == 0) CHECK(h.printer_count == 1 && h.printer[0] == 'P');
        zr_machine_destroy(m);
    }
}

int main(void) {
    test_video();
    test_video_state();
    static zr_rom_index roms;
    const char *folders[2] = {ZR_WORK "/../roms", ZR_WORK "/../../../../roms"};
    zr_rom_index_init(&roms);
    zr_rom_index_scan(&roms, folders, 2);
    zr_machine *probe = build(&roms, 0);
    if (!probe) {
        printf("the machine part skipped: no TIM-S Plus ROMs in roms/ (python tools/fetch_roms.py)\n");
    } else {
        zr_machine_destroy(probe);
        test_channels(&roms);
    }
    zr_rom_index_free(&roms);
    if (failures) printf("%d failures\n", failures);
    else printf("test_timsplus passed\n");
    return failures ? 1 : 0;
}
