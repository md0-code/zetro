/* The on-screen keyboard (osk.h). ZetRo's own code. */
#include "osk.h"
#include "tui.h"
#include "../core/text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The look: the Spectrum's colours (tui.h's palette, as ARGB) on a black panel. */
static uint32_t colour(int index) { return 0xFF000000u | zr_tui_palette[index]; }
#define COL_PANEL colour(ZR_BLACK)
#define COL_KEY colour(ZR_BLUE)
#define COL_MAIN colour(ZR_WHITE | 8)
#define COL_KEYWORD colour(ZR_GREEN | 8)
#define COL_SYMBOL colour(ZR_YELLOW | 8)
#define COL_EDGE colour(ZR_CYAN | 8)
#define COL_HELD colour(ZR_WHITE | 8)
#define COL_LATCHED colour(ZR_GREEN | 8)
#define COL_INK colour(ZR_BLACK)

/* --- the legends ----------------------------------------------------------------------------------------- */

/* The keywords of the Spectrum's keys (the keycaps' markings in K mode), shown on the layouts that have SYMBOL SHIFT and CAPS SHIFT. */
static const struct { const char *key, *word; } KEYWORDS[] = {
    {"Q", "PLOT"}, {"W", "DRAW"}, {"E", "REM"}, {"R", "RUN"}, {"T", "RAND"}, {"Y", "RETURN"}, {"U", "IF"}, {"I", "INPUT"}, {"O", "POKE"},
    {"P", "PRINT"}, {"A", "NEW"}, {"S", "SAVE"}, {"D", "DIM"}, {"F", "FOR"}, {"G", "GOTO"}, {"H", "GOSUB"}, {"J", "LOAD"}, {"K", "LIST"},
    {"L", "LET"}, {"Z", "COPY"}, {"X", "CLEAR"}, {"C", "CONT"}, {"V", "CLS"}, {"B", "BORDER"}, {"N", "NEXT"}, {"M", "PAUSE"},
};

/* Words of long legends, shortened when a keycap has no room for them whole. */
static const struct { const char *word, *shorter; } SHORT[] = {
    {"SYMBOL", "SYM"}, {"SHIFT", "SHFT"}, {"SPACE", "SPC"}, {"BREAK", "BRK"}, {"CAPS", "CAP"}, {"LOCK", "LCK"}, {"SETUP", "SET"},
    {"STATUS", "STAT"}, {"BACK", "BK"}, {"RETURN", "RET"}, {"ENTER", "ENT"}, {"ESCAPE", "ESC"}, {"DELETE", "DEL"}, {"SCROLL", "SCRL"},
    {"BACKSPACE", "BKSP"}, {"CTRL", "CTL"}, {"INPUT", "INPT"}, {"PRINT", "PRNT"}, {"BORDER", "BORD"}, {"PAUSE", "PAUS"}, {"CLEAR", "CLR"},
    {"GOSUB", "GSUB"}, {"BKSP", "BS"}, {"SCRL", "SCR"}, {"SHFT", "SH"},
};

static int same(const char *a, const char *b) { return !strcmp(a, b); }

/* --- the keys ------------------------------------------------------------------------------------------------ */

static int is_shift_name(const char *name) {
    return same(name, "CAPS SHIFT") || same(name, "SYMBOL SHIFT") || same(name, "SHIFT") || same(name, "RIGHT SHIFT") || same(name, "CTRL");
}

void zr_osk_init(zr_osk *o, const zr_family *f, zr_osk_key_fn key_fn, void *ctx) {
    memset(o, 0, sizeof *o);
    o->family = f;
    o->key_fn = key_fn;
    o->ctx = ctx;
    o->count = f->key_count < ZR_OSK_KEYS ? f->key_count : ZR_OSK_KEYS;
    o->selected = o->hover = o->mouse_key = o->pad_key = o->sym_shift = -1;
    int caps = 0, symbol = 0, plain_shift = -1;
    for (unsigned i = 0; i < o->count; ++i) {
        const zr_key_desc *k = &f->keys[i];
        o->drawn[i] = k->w > 0 && k->h > 0;
        o->shift[i] = (uint8_t)is_shift_name(k->name);
        const char *legend = k->legend && *k->legend ? k->legend : k->name;
        snprintf(o->legend[i], sizeof o->legend[i], "%s", legend);
        if (same(k->name, "CAPS SHIFT")) caps = 1;
        if (same(k->name, "SYMBOL SHIFT")) { symbol = 1; o->sym_shift = (int)i; }
        if (same(k->name, "SHIFT")) plain_shift = (int)i;
        if (o->drawn[i]) {
            if (k->x + k->w > o->extent_w) o->extent_w = k->x + k->w;
            if (k->y + k->h > o->extent_h) o->extent_h = k->y + k->h;
        }
    }
    for (unsigned i = 0; i < f->typing_count; ++i)
        if (f->typing[i].shift != ZR_KEY_NONE && f->typing[i].shift < o->count) o->shift[f->typing[i].shift] = 1;
    if (o->sym_shift < 0) o->sym_shift = plain_shift;
    /* the character each key gives with the symbol-shift key (a letter's capital is no symbol) */
    if (o->sym_shift >= 0)
        for (unsigned i = 0; i < f->typing_count; ++i) {
            const zr_typing_desc *t = &f->typing[i];
            const unsigned char ch = (unsigned char)t->ch;
            if (t->shift != o->sym_shift || t->key >= o->count || o->sym[t->key] || ch <= ' ' || ch >= 0x7F) continue;
            if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) continue;
            o->sym[t->key] = (char)ch;
        }
    o->keywords = caps && symbol && strcmp(f->id, "hc88") != 0;
    if (o->keywords)
        for (unsigned i = 0; i < o->count; ++i)
            for (size_t k = 0; k < sizeof KEYWORDS / sizeof *KEYWORDS; ++k)
                if (same(f->keys[i].name, KEYWORDS[k].key)) { snprintf(o->keyword[i], sizeof o->keyword[i], "%s", KEYWORDS[k].word); break; }
    /* the selection starts on the key nearest the middle of the layout */
    long best = -1;
    for (unsigned i = 0; i < o->count; ++i) {
        if (!o->drawn[i]) continue;
        const long dx = 2L * f->keys[i].x + f->keys[i].w - o->extent_w, dy = 2L * f->keys[i].y + f->keys[i].h - o->extent_h;
        const long d = dx * dx + dy * dy;
        if (best < 0 || d < best) { best = d; o->selected = (int)i; }
    }
}

const char *zr_osk_legend(const zr_osk *o, unsigned key) { return key < o->count ? o->legend[key] : ""; }

/* --- pressing ------------------------------------------------------------------------------------------------ */

static void tell(zr_osk *o, unsigned key, int down) { if (o->key_fn) o->key_fn(o->ctx, key, down); }

/* A press on a key: an ordinary key goes down; a shift key latches or lets go. */
static void push(zr_osk *o, int k) {
    if (o->shift[k]) {
        o->latched[k] = (uint8_t)!o->latched[k];
        tell(o, (unsigned)k, o->latched[k]);
        return;
    }
    o->pressed[k] = 1;
    tell(o, (unsigned)k, 1);
}

/* The press ends: the key comes up, and the shift keys that were latched with it. */
static void release_press(zr_osk *o, int k) {
    if (k < 0 || !o->pressed[k]) return;
    o->pressed[k] = 0;
    tell(o, (unsigned)k, 0);
    for (unsigned i = 0; i < o->count; ++i)
        if (o->latched[i]) { o->latched[i] = 0; tell(o, i, 0); }
}

void zr_osk_release(zr_osk *o) {
    for (unsigned i = 0; i < o->count; ++i) {
        if (o->pressed[i] || o->latched[i]) tell(o, i, 0);
        o->pressed[i] = o->latched[i] = 0;
    }
    memset(o->host, 0, sizeof o->host);                         /* the host's keys were let go with the machine's: their ups may not come */
    o->mouse_key = o->pad_key = -1;
}

int zr_osk_is_open(const zr_osk *o) { return o->open; }
void zr_osk_open(zr_osk *o) { o->open = 1; o->hover = -1; o->pad_mode = 0; }
void zr_osk_close(zr_osk *o) { zr_osk_release(o); o->open = 0; o->hover = -1; }
void zr_osk_toggle(zr_osk *o) { if (o->open) zr_osk_close(o); else zr_osk_open(o); }

void zr_osk_host(zr_osk *o, unsigned key, int down) {
    if (key >= o->count) return;
    if (down) { if (o->host[key] < 255) ++o->host[key]; }
    else if (o->host[key]) --o->host[key];
}

/* --- the layout ---------------------------------------------------------------------------------------------- */

/* A frame's pixels are not always square: the pixel's width over its height is the frame's shape over the monitor's. */
void zr_osk_layout(zr_osk *o, int width, int height, int aspect_w, int aspect_h) {
    if (!aspect_w || !aspect_h) { aspect_w = 4; aspect_h = 3; }
    if (o->fw == width && o->fh == height && o->faw == aspect_w && o->fah == aspect_h) return;
    o->fw = width;
    o->fh = height;
    o->faw = aspect_w;
    o->fah = aspect_h;
    memset(o->rect, 0, sizeof o->rect);
    memset(&o->panel, 0, sizeof o->panel);
    memset(&o->footer, 0, sizeof o->footer);
    o->gx = o->gy = 1;
    if (o->extent_w <= 0 || o->extent_h <= 0 || width < 64 || height < 64) return;
    const double pw_ph = (double)aspect_w * height / ((double)aspect_h * width);
    o->gy = (height + 120) / 240 > 1 ? (height + 120) / 240 : 1;
    o->gx = (int)((double)o->gy / pw_ph + 0.5);
    if (o->gx < 1) o->gx = 1;
    const int margin = 2;
    const int avail = width - 2 * margin;
    const int top = 3, foot = 8 * o->gy + 2;
    /* a square key where the room allows (a little taller than square), the keyboard at most two fifths of the picture's height */
    const double qx = (double)avail / o->extent_w;
    double qy = qx * pw_ph * 1.3;
    const double cap = (double)height * 0.40 / o->extent_h;
    if (qy > cap) qy = cap;
    const int kb_h = (int)(qy * o->extent_h + 0.5);
    const int panel_h = top + kb_h + foot;
    o->panel.x = 0;
    o->panel.w = width;
    o->panel.h = panel_h;
    o->panel.y = height - panel_h;
    o->footer.x = margin;
    o->footer.w = width - 2 * margin;
    o->footer.h = foot;
    o->footer.y = height - foot;
    const int kx = margin, ky = o->panel.y + top;
    for (unsigned i = 0; i < o->count; ++i) {
        if (!o->drawn[i]) continue;
        const zr_key_desc *k = &o->family->keys[i];
        const int x0 = kx + (k->x * avail + o->extent_w / 2) / o->extent_w, x1 = kx + ((k->x + k->w) * avail + o->extent_w / 2) / o->extent_w;
        const int y0 = ky + (k->y * kb_h + o->extent_h / 2) / o->extent_h, y1 = ky + ((k->y + k->h) * kb_h + o->extent_h / 2) / o->extent_h;
        o->rect[i].x = x0 + 1;
        o->rect[i].y = y0 + 1;
        o->rect[i].w = x1 - x0 - 1;
        o->rect[i].h = y1 - y0 - 1;
    }
}

int zr_osk_key_rect(const zr_osk *o, unsigned key, zr_osk_rect *rect) {
    if (key >= o->count || !o->drawn[key] || o->rect[key].w <= 0) return 0;
    *rect = o->rect[key];
    return 1;
}

int zr_osk_key_at(const zr_osk *o, int x, int y) {
    for (unsigned i = 0; i < o->count; ++i) {
        if (!o->drawn[i] || o->rect[i].w <= 0) continue;
        const zr_osk_rect *r = &o->rect[i];
        if (x >= r->x - 1 && x < r->x + r->w && y >= r->y - 1 && y < r->y + r->h) return (int)i;     /* the gap before a key is its too */
    }
    return -1;
}

/* --- drawing ------------------------------------------------------------------------------------------------- */

typedef struct canvas { uint32_t *px; int w, h; } canvas;

static void fill(const canvas *c, int x, int y, int w, int h, uint32_t col) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) c->px[(size_t)(y + j) * (size_t)c->w + (size_t)(x + i)] = col;
}

/* A row of a glyph. The font's code 60h is the Spectrum's pound sign: the grave accent that ASCII has there is drawn here. */
static uint8_t glyph_row(const uint8_t *font, uint8_t ch, int row) {
    static const uint8_t GRAVE[8] = {0x00, 0x20, 0x10, 0x08, 0x00, 0x00, 0x00, 0x00};
    return ch == '`' ? GRAVE[row] : font[ch * 8 + row];
}

/* The columns a glyph uses: `lo` the first, the width. A blank glyph (a space) is three columns wide. */
static int span(const uint8_t *font, uint8_t ch, int *lo) {
    int first = 8, last = -1;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            if (glyph_row(font, ch, r) & (0x80u >> c)) { if (c < first) first = c; if (c > last) last = c; }
    if (last < 0) { *lo = 0; return 3; }
    *lo = first;
    return last - first + 1;
}

/* Text is spaced by its glyphs' own widths, a pixel between them (keycaps are small). */
static int text_width(const uint8_t *font, const char *s, int gx) {
    int w = 0, lo;
    for (const char *p = s; *p; ++p) w += (span(font, (uint8_t)*p, &lo) + (p[1] ? 1 : 0)) * gx;
    return w;
}

/* `y` is the top of the glyphs' cells: the capitals use rows 1 to 6. */
static void draw_text(const canvas *c, const uint8_t *font, int x, int y, const char *s, int gx, int gy, uint32_t col) {
    for (const char *p = s; *p; ++p) {
        int lo;
        const int w = span(font, (uint8_t)*p, &lo);
        for (int r = 0; r < 8; ++r)
            for (int i = 0; i < w; ++i)
                if (glyph_row(font, (uint8_t)*p, r) & (0x80u >> (lo + i))) fill(c, x + i * gx, y + r * gy, gx, gy, col);
        x += (w + 1) * gx;
    }
}

/* The largest scale up to `gx` at which the text fits `room` pixels; 0 at none. */
static int fit_scale(const uint8_t *font, const char *s, int room, int gx) {
    for (int g = gx; g >= 1; --g)
        if (text_width(font, s, g) <= room) return g;
    return 0;
}

typedef struct line { char text[24]; int g; uint32_t col; } line;

static const char *shorter(const char *word) {
    for (size_t i = 0; i < sizeof SHORT / sizeof *SHORT; ++i) if (same(SHORT[i].word, word)) return SHORT[i].shorter;
    return word;
}

/* The main legend as one line or two that fit the room, one that cannot be fitted whole cut short. Returns the lines (1 or 2). */
static int main_lines(const uint8_t *font, const char *legend, int room_w, int room_h, int gx, int gy, line out[2]) {
    int g = fit_scale(font, legend, room_w, gx);
    snprintf(out[0].text, sizeof out[0].text, "%s", legend);
    out[0].g = g;
    if (g) return 1;
    const char *space = strchr(legend, ' ');
    if (!space)                                                 /* a word the keycap cannot hold: its short forms */
        for (int step = 0; step < 2 && shorter(out[0].text) != out[0].text; ++step) {
            snprintf(out[0].text, sizeof out[0].text, "%s", shorter(out[0].text));
            out[0].g = fit_scale(font, out[0].text, room_w, gx);
            if (out[0].g) return 1;
        }
    if (space && room_h >= 13 * gy) {
        char a[24], b[24];
        snprintf(a, sizeof a, "%.*s", (int)(space - legend), legend);
        snprintf(b, sizeof b, "%s", space + 1);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (attempt) { snprintf(a, sizeof a, "%s", shorter(a)); snprintf(b, sizeof b, "%s", shorter(b)); }
            const int ga = fit_scale(font, a, room_w, gx), gb = fit_scale(font, b, room_w, gx);
            if (ga && gb) {
                const int common = ga < gb ? ga : gb;
                snprintf(out[0].text, sizeof out[0].text, "%s", a);
                snprintf(out[1].text, sizeof out[1].text, "%s", b);
                out[0].g = out[1].g = common;
                return 2;
            }
        }
    }
    /* cut short: the front of the legend that fits at the smallest scale */
    snprintf(out[0].text, sizeof out[0].text, "%s", legend);
    for (size_t n = strlen(out[0].text); n > 1 && text_width(font, out[0].text, 1) > room_w; ) out[0].text[--n] = 0;
    out[0].g = 1;
    return 1;
}

/* The pixels the lines take up, from the top of the first capital to the bottom of the last. */
static int lines_height(int n, int gy) { return ((n - 1) * 7 + 6) * gy; }

static void draw_key(const zr_osk *o, const canvas *c, const uint8_t *font, unsigned k) {
    const zr_osk_rect *r = &o->rect[k];
    const int held = o->pressed[k] || o->host[k], latched = o->latched[k];
    const int shown = o->pad_mode ? (int)k == o->selected : (int)k == o->hover;
    uint32_t fill_col = COL_KEY, main_col = COL_MAIN, kw_col = COL_KEYWORD, sym_col = COL_SYMBOL;
    if (latched) { fill_col = COL_LATCHED; main_col = kw_col = sym_col = COL_INK; }
    else if (held) { fill_col = COL_HELD; main_col = kw_col = sym_col = COL_INK; }
    fill(c, r->x, r->y, r->w, r->h, fill_col);
    if (shown) {
        const int t = r->h >= 18 && r->w >= 12 ? 2 : 1;
        fill(c, r->x, r->y, r->w, t, COL_EDGE);
        fill(c, r->x, r->y + r->h - t, r->w, t, COL_EDGE);
        fill(c, r->x, r->y, t, r->h, COL_EDGE);
        fill(c, r->x + r->w - t, r->y, t, r->h, COL_EDGE);
    }
    /* the room for the legends: the keycap but its edge (text may touch the sides of a narrow keycap, whether it is shown or not) */
    const int iw = r->w - 1, ih = r->h - 2;
    if (iw < 4 || ih < 6) return;
    line lines[3];
    int n = 0;
    const char *legend = o->legend[k];
    const int rich = strlen(legend) <= 2 && (o->sym[k] || o->keyword[k][0]);
    if (!rich) {
        /* a legend of a letter or two is drawn at the layout's scale; a longer one is a label, drawn small */
        line two[2];
        const int m = main_lines(font, legend, iw, ih, strlen(legend) <= 2 ? o->gx : 1, o->gy, two);
        for (int i = 0; i < m; ++i) { lines[n] = two[i]; lines[n].col = main_col; ++n; }
    } else {
        char sym[2] = {o->sym[k], 0};
        const int has_sym = o->sym[k] != 0;
        char keyword[10];
        snprintf(keyword, sizeof keyword, "%s", o->keyword[k]);
        if (keyword[0] && !fit_scale(font, keyword, iw, 1)) snprintf(keyword, sizeof keyword, "%s", shorter(o->keyword[k]));     /* the keyword short, where the whole does not fit */
        int want_kw = keyword[0] != 0 && ih >= lines_height(3, o->gy) && fit_scale(font, keyword, iw, 1);
        int want_sym = has_sym && ih >= lines_height(want_kw ? 3 : 2, o->gy);
        if (!want_sym && !want_kw && has_sym && text_width(font, legend, 1) + 3 + text_width(font, sym, 1) <= iw) {
            /* no second line: the symbol beside the legend */
            const int wm = text_width(font, legend, o->gx), ws = text_width(font, sym, o->gx);
            const int total = wm + 3 * o->gx + ws;
            const int x = r->x + (r->w - total) / 2, y = r->y + (r->h - lines_height(1, o->gy)) / 2 - o->gy;
            draw_text(c, font, x, y, legend, o->gx, o->gy, main_col);
            draw_text(c, font, x + wm + 3 * o->gx, y, sym, o->gx, o->gy, sym_col);
            return;
        }
        if (want_kw) {
            snprintf(lines[n].text, sizeof lines[n].text, "%s", keyword);
            lines[n].g = 1;
            lines[n++].col = kw_col;
        }
        snprintf(lines[n].text, sizeof lines[n].text, "%s", legend);
        lines[n].g = fit_scale(font, legend, iw, o->gx);
        if (!lines[n].g) lines[n].g = 1;
        lines[n++].col = main_col;
        if (want_sym) {
            snprintf(lines[n].text, sizeof lines[n].text, "%s", sym);
            lines[n].g = fit_scale(font, sym, iw, o->gx);
            lines[n++].col = sym_col;
        }
    }
    const int block = lines_height(n, o->gy);
    const int top = r->y + (r->h - block) / 2 - o->gy;               /* the cell's first row is blank: the capitals start on its second */
    for (int i = 0; i < n; ++i) {
        const int g = lines[i].g ? lines[i].g : 1;
        const int w = text_width(font, lines[i].text, g);
        draw_text(c, font, r->x + (r->w - w) / 2, top + i * 7 * o->gy, lines[i].text, g, o->gy, lines[i].col);
    }
}

void zr_osk_draw(zr_osk *o, const uint8_t font[2048], uint32_t *argb, int width, int height, int aspect_w, int aspect_h) {
    if (!o->open) return;
    zr_osk_layout(o, width, height, aspect_w, aspect_h);
    if (o->panel.h <= 0) return;
    const canvas c = {argb, width, height};
    fill(&c, o->panel.x, o->panel.y, o->panel.w, o->panel.h, COL_PANEL);
    /* the Romanian flag along the top edge of the panel: blue, yellow and red */
    const int third = width / 3;
    fill(&c, 0, o->panel.y, third, 2, colour(ZR_BLUE | 8));
    fill(&c, third, o->panel.y, third, 2, colour(ZR_YELLOW | 8));
    fill(&c, 2 * third, o->panel.y, width - 2 * third, 2, colour(ZR_RED | 8));
    for (unsigned k = 0; k < o->count; ++k)
        if (o->drawn[k] && o->rect[k].w > 0) draw_key(o, &c, font, k);
    /* the footer line: how to close it */
    char footer[96];
    zr_text_format(footer, sizeof footer, ZR_T_OSK_FOOTER);
    int g = fit_scale(font, footer, o->footer.w, o->gx);
    if (!g) {
        for (size_t n = strlen(footer); n > 1 && text_width(font, footer, 1) > o->footer.w; ) footer[--n] = 0;
        g = 1;
    }
    draw_text(&c, font, o->footer.x, o->footer.y + 1, footer, g, o->gy, colour(ZR_CYAN | 8));
}

/* --- input ------------------------------------------------------------------------------------------------- */

int zr_osk_mouse(zr_osk *o, int x, int y, int action) {
    if (!o->open) return 0;
    const int k = x < 0 || y < 0 ? -1 : zr_osk_key_at(o, x, y);
    o->pad_mode = 0;
    if (action == ZR_OSK_MOUSE_UP) {
        if (o->mouse_key < 0) return 0;
        release_press(o, o->mouse_key);
        o->mouse_key = -1;
        o->hover = k;
        return 1;
    }
    o->hover = k;
    if (action == ZR_OSK_MOUSE_DOWN && k >= 0) {
        if (o->mouse_key >= 0) release_press(o, o->mouse_key);
        push(o, k);
        o->mouse_key = o->shift[k] ? -1 : k;
    }
    return k >= 0;
}

/* The nearest key in a direction: along it first, a key off to the side counting for more the further off it is. */
static int neighbour(const zr_osk *o, int from, zr_osk_pad_key dir) {
    const zr_key_desc *f = &o->family->keys[from];
    const int cx = 2 * f->x + f->w, cy = 2 * f->y + f->h;
    int best = -1;
    long best_score = 0;
    for (unsigned i = 0; i < o->count; ++i) {
        if (!o->drawn[i] || (int)i == from) continue;
        const zr_key_desc *k = &o->family->keys[i];
        const int dx = 2 * k->x + k->w - cx, dy = 2 * k->y + k->h - cy;
        int along, aside;
        switch (dir) {
        case ZR_OSK_PAD_RIGHT: along = dx; aside = dy < 0 ? -dy : dy; break;
        case ZR_OSK_PAD_LEFT: along = -dx; aside = dy < 0 ? -dy : dy; break;
        case ZR_OSK_PAD_DOWN: along = dy; aside = dx < 0 ? -dx : dx; break;
        default: along = -dy; aside = dx < 0 ? -dx : dx; break;
        }
        if (along <= 0) continue;
        const long score = along + (dir == ZR_OSK_PAD_LEFT || dir == ZR_OSK_PAD_RIGHT ? 4L : 2L) * aside;
        if (best < 0 || score < best_score) { best = (int)i; best_score = score; }
    }
    return best;
}

int zr_osk_pad(zr_osk *o, zr_osk_pad_key key, int down) {
    if (!o->open) return 0;
    switch (key) {
    case ZR_OSK_PAD_A:
        o->pad_mode = 1;
        if (down) {
            if (o->selected < 0) return 1;
            if (o->pad_key >= 0) release_press(o, o->pad_key);
            push(o, o->selected);
            o->pad_key = o->shift[o->selected] ? -1 : o->selected;
        } else if (o->pad_key >= 0) {
            release_press(o, o->pad_key);
            o->pad_key = -1;
        }
        return 1;
    case ZR_OSK_PAD_B:
        if (down) zr_osk_close(o);
        return 1;
    default:
        if (!down) return 1;
        o->pad_mode = 1;
        if (o->selected >= 0) {
            const int next = neighbour(o, o->selected, key);
            if (next >= 0) o->selected = next;
        }
        return 1;
    }
}
