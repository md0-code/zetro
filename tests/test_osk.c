/* The on-screen keyboard's smoke test, without SDL (app/osk.h): on every family's key table the keycaps of a few frame sizes lie inside the
 * frame, do not overlap and are found again from a position; on the JET's keyboard the legends (a keyword and the symbol-shift character from the
 * typing table), the latching of the shift keys, the pad's selection by geometry and its A and B, and the keyboard drawn into a picture. */
#include "machine.h"
#include "../src/app/osk.h"
#include "../src/core/font8x8.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* What the keyboard tells the machine: "+7 -7 ..." as the key index and its direction. */
static char log_text[256];
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static int log_is(const char *format, ...) {
    char want[256];
    va_list args;
    va_start(args, format);
    vsnprintf(want, sizeof want, format, args);
    va_end(args);
    return !strcmp(log_text, want);
}
static void log_key(void *ctx, unsigned key, int down) {
    (void)ctx;
    char piece[16];
    snprintf(piece, sizeof piece, "%c%u ", down ? '+' : '-', key);
    if (strlen(log_text) + strlen(piece) < sizeof log_text) strcat(log_text, piece);
}

static void centre(const zr_osk *o, unsigned key, int *x, int *y) {
    zr_osk_rect r;
    CHECK(zr_osk_key_rect(o, key, &r));
    *x = r.x + r.w / 2;
    *y = r.y + r.h / 2;
}

/* Every family, the frames of the machines (and the setup frame): the keycaps are inside the frame, apart, and each is found from its middle. */
static void test_layouts(void) {
    static const int FRAMES[][4] = {{352, 288, 4, 3}, {704, 288, 4, 3}, {544, 288, 4, 3}, {320, 288, 4, 3}, {320, 240, 4, 3}};
    for (unsigned f = 0; f < zr_family_count(); ++f) {
        const zr_family *family = zr_family_at(f);
        for (unsigned s = 0; s < sizeof FRAMES / sizeof *FRAMES; ++s) {
            zr_osk o;
            zr_osk_init(&o, family, log_key, NULL);
            zr_osk_layout(&o, FRAMES[s][0], FRAMES[s][1], FRAMES[s][2], FRAMES[s][3]);
            for (unsigned a = 0; a < family->key_count; ++a) {
                zr_osk_rect ra;
                if (!zr_osk_key_rect(&o, a, &ra)) { CHECK(family->keys[a].w == 0 || family->keys[a].h == 0); continue; }
                CHECK(ra.x >= 0 && ra.y >= 0 && ra.x + ra.w <= FRAMES[s][0] && ra.y + ra.h <= FRAMES[s][1] && ra.w >= 8 && ra.h >= 8);
                CHECK(ra.y >= FRAMES[s][1] / 2 - 20);                   /* the lower part of the picture */
                CHECK(zr_osk_key_at(&o, ra.x + ra.w / 2, ra.y + ra.h / 2) == (int)a);
                for (unsigned b = a + 1; b < family->key_count; ++b) {
                    zr_osk_rect rb;
                    if (!zr_osk_key_rect(&o, b, &rb)) continue;
                    const int apart = ra.x + ra.w <= rb.x || rb.x + rb.w <= ra.x || ra.y + ra.h <= rb.y || rb.y + rb.h <= ra.y;
                    if (!apart) printf("%s: keys %s and %s overlap on %dx%d\n", family->id, family->keys[a].name, family->keys[b].name, FRAMES[s][0], FRAMES[s][1]);
                    CHECK(apart);
                }
            }
        }
    }
}

static void test_jet(void) {
    const zr_family *jet = zr_family_of_model("jet", NULL);
    CHECK(jet != NULL);
    if (!jet) return;
    zr_osk o;
    zr_osk_init(&o, jet, log_key, NULL);
    const int p = zr_family_key(jet, "P"), ss = zr_family_key(jet, "SYMBOL SHIFT"), cs = zr_family_key(jet, "CAPS SHIFT"), z = zr_family_key(jet, "Z");
    CHECK(p >= 0 && ss >= 0 && cs >= 0 && z >= 0);
    /* the legends: the symbol-shift character from the typing table, the keyword of the Spectrum layouts */
    CHECK(o.sym[p] == '"' && o.sym[z] == ':' && !strcmp(o.keyword[p], "PRINT") && !strcmp(zr_osk_legend(&o, (unsigned)ss), "SYMBOL SHIFT"));
    CHECK(o.shift[ss] && o.shift[cs] && !o.shift[p]);
    /* closed, nothing is taken */
    zr_osk_layout(&o, 352, 288, 4, 3);
    int x, y;
    centre(&o, (unsigned)p, &x, &y);
    CHECK(!zr_osk_mouse(&o, x, y, ZR_OSK_MOUSE_DOWN) && !zr_osk_pad(&o, ZR_OSK_PAD_A, 1) && !log_text[0]);
    zr_osk_open(&o);
    /* a click: the key is down while the button is, a shift key latches (a second click lets it go) and goes up after the next ordinary key */
    CHECK(zr_osk_mouse(&o, x, y, ZR_OSK_MOUSE_DOWN));
    CHECK(log_is("+%d ", p));
    zr_osk_mouse(&o, x, y, ZR_OSK_MOUSE_UP);
    CHECK(log_is("+%d -%d ", p, p));
    log_text[0] = 0;
    int sx, sy;
    centre(&o, (unsigned)ss, &sx, &sy);
    zr_osk_mouse(&o, sx, sy, ZR_OSK_MOUSE_DOWN);
    zr_osk_mouse(&o, sx, sy, ZR_OSK_MOUSE_UP);
    CHECK(o.latched[ss] && log_is("+%d ", ss));
    zr_osk_mouse(&o, x, y, ZR_OSK_MOUSE_DOWN);
    zr_osk_mouse(&o, x, y, ZR_OSK_MOUSE_UP);
    CHECK(!o.latched[ss] && log_is("+%d +%d -%d -%d ", ss, p, p, ss));
    log_text[0] = 0;
    zr_osk_mouse(&o, sx, sy, ZR_OSK_MOUSE_DOWN);
    zr_osk_mouse(&o, sx, sy, ZR_OSK_MOUSE_UP);
    zr_osk_mouse(&o, sx, sy, ZR_OSK_MOUSE_DOWN);
    CHECK(!o.latched[ss] && log_is("+%d -%d ", ss, ss));
    zr_osk_mouse(&o, sx, sy, ZR_OSK_MOUSE_UP);
    /* a key pressed on the host's keyboard shows as held */
    zr_osk_host(&o, (unsigned)z, 1);
    CHECK(o.host[z] == 1);
    zr_osk_host(&o, (unsigned)z, 0);
    CHECK(o.host[z] == 0);
    /* the pad: the selection starts in the middle of the layout; right and left bring it back; A holds the key, B closes and lets go */
    const int start = o.selected;
    CHECK(start >= 0);
    zr_osk_pad(&o, ZR_OSK_PAD_RIGHT, 1);
    CHECK(o.selected != start && jet->keys[o.selected].x > jet->keys[start].x && jet->keys[o.selected].y == jet->keys[start].y);
    zr_osk_pad(&o, ZR_OSK_PAD_LEFT, 1);
    CHECK(o.selected == start);
    zr_osk_pad(&o, ZR_OSK_PAD_UP, 1);
    CHECK(jet->keys[o.selected].y < jet->keys[start].y);
    for (int i = 0; i < 12; ++i) zr_osk_pad(&o, ZR_OSK_PAD_LEFT, 1);
    CHECK(!strcmp(jet->keys[o.selected].name, "1"));            /* Q's left is the row above's first key */
    log_text[0] = 0;
    zr_osk_pad(&o, ZR_OSK_PAD_A, 1);
    CHECK(o.pad_key == o.selected && strlen(log_text) > 0);
    zr_osk_pad(&o, ZR_OSK_PAD_A, 0);
    CHECK(o.pad_key < 0 && !o.pressed[o.selected]);
    zr_osk_pad(&o, ZR_OSK_PAD_A, 1);
    log_text[0] = 0;
    CHECK(zr_osk_pad(&o, ZR_OSK_PAD_B, 1) && !zr_osk_is_open(&o) && strlen(log_text) > 0 && !o.pressed[o.selected]);
    /* drawn into a picture: the panel, the keycaps and a legend's pixels */
    uint8_t font[2048];
    zr_font8x8_build(font);
    static uint32_t picture[352 * 288];
    for (size_t i = 0; i < sizeof picture / sizeof *picture; ++i) picture[i] = 0xFFCDCDCDu;
    zr_osk_draw(&o, font, picture, 352, 288, 4, 3);
    CHECK(picture[0] == 0xFFCDCDCDu);                          /* closed: the picture is left alone */
    zr_osk_open(&o);
    zr_osk_draw(&o, font, picture, 352, 288, 4, 3);
    CHECK(picture[0] == 0xFFCDCDCDu && picture[(size_t)287 * 352] == 0xFF000000u);        /* the machine's picture above, the panel below */
    zr_osk_rect r;
    CHECK(zr_osk_key_rect(&o, (unsigned)p, &r) && picture[(size_t)(r.y + 1) * 352 + (size_t)(r.x + 1)] == 0xFF0000CDu);
    int white = 0;
    for (int j = r.y; j < r.y + r.h; ++j) for (int i = r.x; i < r.x + r.w; ++i) white += picture[(size_t)j * 352 + (size_t)i] == 0xFFFFFFFFu;
    CHECK(white > 5);
    /* a frame too small for a keyboard draws nothing and does not fail */
    zr_osk_draw(&o, font, picture, 40, 30, 4, 3);
}

int main(void) {
    test_layouts();
    test_jet();
    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("test_osk: ok\n");
    return 0;
}
