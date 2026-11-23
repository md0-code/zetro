/* The text-cell toolkit (tui.h). ZetRo's own code: the cell buffer and the
 * widgets, fitted to the Spectrum's 32 x 24 cells, its attributes and ZetRo's own character set. */
#include "tui.h"
#include "../core/font8x8.h"
#include "../core/text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The Spectrum's colours: the normal level and the BRIGHT one, as the test machine's. */
const uint32_t zr_tui_palette[16] = {
    0x000000, 0x0000CD, 0xCD0000, 0xCD00CD, 0x00CD00, 0x00CDCD, 0xCDCD00, 0xCDCDCD,
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF};

void zr_tui_init(zr_tui *t, const uint8_t *font) {
    memset(t, 0, sizeof *t);
    if (font) memcpy(t->font, font, sizeof t->font);
    else zr_font8x8_build(t->font);
    t->border = ZR_BLUE;
    zr_tui_clear(t, ZR_ATTR(ZR_WHITE, ZR_BLACK, 0));
}

/* --- cells ------------------------------------------------------------------------------- */

zr_tui_rect zr_tui_rect_make(int row, int col, int rows, int cols) {
    zr_tui_rect r = {row, col, rows, cols};
    return r;
}

zr_tui_rect zr_tui_centered(int rows, int cols, int top, int bottom) {
    zr_tui_rect r;
    r.rows = rows;
    r.cols = cols;
    r.row = top + (bottom - top + 1 - rows) / 2;
    r.col = (ZR_TUI_COLS - cols) / 2;
    if (r.row < 0) r.row = 0;
    if (r.col < 0) r.col = 0;
    return r;
}

static zr_tui_cell *cell(zr_tui *t, int row, int col) {
    return row >= 0 && row < ZR_TUI_ROWS && col >= 0 && col < ZR_TUI_COLS ? &t->cell[row][col] : NULL;
}
const zr_tui_cell *zr_tui_cell_at(const zr_tui *t, int row, int col) {
    return row >= 0 && row < ZR_TUI_ROWS && col >= 0 && col < ZR_TUI_COLS ? &t->cell[row][col] : NULL;
}

void zr_tui_clear(zr_tui *t, uint8_t attr) {
    for (int r = 0; r < ZR_TUI_ROWS; ++r)
        for (int c = 0; c < ZR_TUI_COLS; ++c) {
            t->cell[r][c].ch = ' ';
            t->cell[r][c].attr = attr;
        }
}

void zr_tui_fill(zr_tui *t, zr_tui_rect r, uint8_t ch, uint8_t attr) {
    for (int y = 0; y < r.rows; ++y)
        for (int x = 0; x < r.cols; ++x) {
            zr_tui_cell *c = cell(t, r.row + y, r.col + x);
            if (c) { c->ch = ch; c->attr = attr; }
        }
}

static void put_cell(zr_tui *t, int row, int col, uint8_t ch, uint8_t attr) {
    zr_tui_cell *c = cell(t, row, col);
    if (c) { c->ch = ch; c->attr = attr; }
}

int zr_tui_put(zr_tui *t, int row, int col, uint8_t attr, const char *text) {
    int n = 0;
    for (; *text; ++text, ++col) {
        zr_tui_cell *c = cell(t, row, col);
        if (!c) continue;
        c->ch = (uint8_t)*text;
        c->attr = attr;
        ++n;
    }
    return n;
}

/* `text` cut to `width` cells with an ellipsis, where it is too long. */
static void fit_text(char *shown, size_t capacity, const char *text, int width, int flags) {
    const size_t length = strlen(text);
    if (width < 1) { shown[0] = 0; return; }
    if (length <= (size_t)width) { snprintf(shown, capacity, "%s", text); return; }
    const size_t keep = (size_t)width - 1;             /* the cells left for the text beside the ellipsis */
    size_t head, tail;
    if (flags & ZR_FIT_CUT_END) { head = keep; tail = 0; }
    else if (flags & ZR_FIT_CUT_START) { head = 0; tail = keep; }
    else { head = (keep + 1) / 2; tail = keep - head; }
    size_t n = 0;
    for (size_t i = 0; i < head && n + 1 < capacity; ++i) shown[n++] = text[i];
    if (n + 1 < capacity) shown[n++] = ZR_GLYPH_CHAR(ZR_GLYPH_ELLIPSIS);
    for (size_t i = 0; i < tail && n + 1 < capacity; ++i) shown[n++] = text[length - tail + i];
    shown[n] = 0;
}

void zr_tui_put_fit(zr_tui *t, int row, int col, int width, uint8_t attr, const char *text, int flags) {
    if (width <= 0) return;
    char shown[ZR_TUI_PATH + 8];
    fit_text(shown, sizeof shown, text, width, flags);
    const int length = (int)strlen(shown);
    zr_tui_fill(t, zr_tui_rect_make(row, col, 1, width), ' ', attr);
    int offset = 0;
    if (flags & ZR_FIT_RIGHT) offset = width - length;
    else if (flags & ZR_FIT_CENTER) offset = (width - length) / 2;
    zr_tui_put(t, row, col + offset, attr, shown);
}

void zr_tui_clean(const char *utf8, char *out, size_t capacity) {
    zr_text_convert(utf8, zr_font8x8_letters, out, capacity);
    for (char *c = out; *c; ++c) if ((unsigned char)*c < 0x20 || (unsigned char)*c == 0x7F) *c = '?';
}

/* The text broken into lines of at most `width` characters at spaces ('\n' breaks too). */
#define WRAP_LINES 12
#define WRAP_LINE 64
static int wrap(const char *text, int width, char lines[WRAP_LINES][WRAP_LINE]) {
    int n = 0;
    char line[WRAP_LINE] = "";
    size_t used = 0;
    const char *p = text;
    if (width >= WRAP_LINE) width = WRAP_LINE - 1;
    while (*p && n < WRAP_LINES) {
        if (*p == '\n') { snprintf(lines[n++], WRAP_LINE, "%s", line); line[0] = 0; used = 0; ++p; continue; }
        if (*p == ' ') { ++p; continue; }
        size_t word = 0;
        while (p[word] && p[word] != ' ' && p[word] != '\n') ++word;
        if (word > (size_t)width) word = (size_t)width;
        if (used && used + 1 + word > (size_t)width) { snprintf(lines[n++], WRAP_LINE, "%s", line); line[0] = 0; used = 0; if (n == WRAP_LINES) break; }
        if (used) line[used++] = ' ';
        memcpy(line + used, p, word);
        used += word;
        line[used] = 0;
        p += word;
        while (*p && *p != ' ' && *p != '\n') ++p;      /* the rest of a word too long for a line is dropped */
    }
    if (used && n < WRAP_LINES) snprintf(lines[n++], WRAP_LINE, "%s", line);
    return n;
}

int zr_tui_put_wrapped(zr_tui *t, int row, int col, int width, int rows, uint8_t attr, const char *text) {
    char lines[WRAP_LINES][WRAP_LINE];
    int n = wrap(text, width, lines);
    if (n > rows) n = rows;
    for (int i = 0; i < rows; ++i) zr_tui_put_fit(t, row + i, col, width, attr, i < n ? lines[i] : "", ZR_FIT_CUT_END);
    return n;
}

void zr_tui_box(zr_tui *t, zr_tui_rect r, uint8_t attr) {
    if (r.rows < 2 || r.cols < 2) return;
    const int bottom = r.row + r.rows - 1, right = r.col + r.cols - 1;
    for (int x = r.col + 1; x < right; ++x) {
        put_cell(t, r.row, x, ZR_GLYPH_LINE_H, attr);
        put_cell(t, bottom, x, ZR_GLYPH_LINE_H, attr);
    }
    for (int y = r.row + 1; y < bottom; ++y) {
        put_cell(t, y, r.col, ZR_GLYPH_LINE_V, attr);
        put_cell(t, y, right, ZR_GLYPH_LINE_V, attr);
    }
    put_cell(t, r.row, r.col, ZR_GLYPH_CORNER_TL, attr);
    put_cell(t, r.row, right, ZR_GLYPH_CORNER_TR, attr);
    put_cell(t, bottom, r.col, ZR_GLYPH_CORNER_BL, attr);
    put_cell(t, bottom, right, ZR_GLYPH_CORNER_BR, attr);
}

void zr_tui_rule(zr_tui *t, zr_tui_rect r, int row, uint8_t attr) {
    const int y = r.row + row, right = r.col + r.cols - 1;
    for (int x = r.col + 1; x < right; ++x) put_cell(t, y, x, ZR_GLYPH_LINE_H, attr);
    put_cell(t, y, r.col, ZR_GLYPH_TEE_L, attr);
    put_cell(t, y, right, ZR_GLYPH_TEE_R, attr);
}

void zr_tui_bar(zr_tui *t, int row, int col, int width, uint8_t bar, const char *text, int stripes) {
    zr_tui_fill(t, zr_tui_rect_make(row, col, 1, width), ' ', bar);
    int room = width - 2;
    if (stripes) room -= 4;
    zr_tui_put_fit(t, row, col + 1, room > 0 ? room : 0, bar, text, ZR_FIT_CUT_END);
    if (!stripes) return;
    /* The flag's three stripes in the manner of the 128K menus: one slanted glyph over four cells, each cell's paper the stripe that
     * fills it and its ink the next stripe, whose slice shows at the cell's right edge, so the three slant as one flag: the Romanian
     * flag's blue, yellow and red, BRIGHT. */
    const uint8_t back = (uint8_t)(bar >> 3 & 7u);                /* the bar's own paper, either side of the flag */
    const uint8_t COLOURS[5] = {back, ZR_BLUE, ZR_YELLOW, ZR_RED, back};
    for (int i = 0; i < 4; ++i) put_cell(t, row, col + width - 4 + i, ZR_GLYPH_STRIPE, ZR_ATTR(COLOURS[i + 1], COLOURS[i], 1));
}

void zr_tui_scrollbar(zr_tui *t, int row, int col, int height, size_t top, size_t visible, size_t total, uint8_t attr) {
    if (height < 3 || total <= visible) return;
    const int track = height - 2;
    put_cell(t, row, col, ZR_GLYPH_UP, attr);
    put_cell(t, row + height - 1, col, ZR_GLYPH_DOWN, attr);
    const size_t range = total - visible;
    const int thumb = (int)((size_t)(track - 1) * (top > range ? range : top) / range);
    for (int i = 0; i < track; ++i) put_cell(t, row + 1 + i, col, i == thumb ? ZR_GLYPH_FULL : ZR_GLYPH_SHADE, attr);
}

/* --- the picture ------------------------------------------------------------------------- */

void zr_tui_render(zr_tui *t, int flash_on) {
    memset(t->pixels, t->border & 7u, sizeof t->pixels);
    for (int r = 0; r < ZR_TUI_ROWS; ++r)
        for (int c = 0; c < ZR_TUI_COLS; ++c) {
            const uint8_t ch = t->cell[r][c].ch, attr = t->cell[r][c].attr;
            const uint8_t bright = attr & 0x40u ? 8u : 0u;
            uint8_t ink = (uint8_t)((attr & 7u) | bright), paper = (uint8_t)((attr >> 3 & 7u) | bright);
            if ((attr & ZR_FLASH) && flash_on) { const uint8_t swap = ink; ink = paper; paper = swap; }
            for (int y = 0; y < 8; ++y) {
                const uint8_t bits = t->font[ch * 8 + y];
                uint8_t *p = t->pixels + (size_t)(ZR_TUI_BORDER_Y + r * 8 + y) * ZR_TUI_WIDTH + ZR_TUI_BORDER_X + (size_t)c * 8;
                for (int x = 0; x < 8; ++x) p[x] = bits & (0x80u >> x) ? ink : paper;
            }
        }
    ++t->serial;
}

void zr_tui_frame(const zr_tui *t, zr_frame *f) {
    f->width = ZR_TUI_WIDTH;
    f->height = ZR_TUI_HEIGHT;
    f->pitch = ZR_TUI_WIDTH;
    f->pixels = t->pixels;
    f->palette = zr_tui_palette;
    f->colors = 16;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = t->serial;
    f->picture_x = ZR_TUI_BORDER_X;
    f->picture_y = ZR_TUI_BORDER_Y;
    f->picture_w = ZR_TUI_COLS * 8;
    f->picture_h = ZR_TUI_ROWS * 8;
}

int zr_tui_status_line(const uint8_t font[2048], uint32_t *argb, int width, int height, const char *text, uint8_t attr) {
    const int cells = width / 8;
    if (cells < 6 || height < 8 || !*text) return 0;
    char shown[128];
    fit_text(shown, sizeof shown, text, cells - 2 < (int)sizeof shown - 1 ? cells - 2 : (int)sizeof shown - 1, 0);
    const int n = (int)strlen(shown) + 2;                       /* a cell of margin each side */
    const int x0 = (width - cells * 8) / 2, y0 = height - 8;
    const uint8_t bright = attr & 0x40u ? 8u : 0u;
    const uint32_t ink = 0xFF000000u | zr_tui_palette[(attr & 7u) | bright], paper = 0xFF000000u | zr_tui_palette[(attr >> 3 & 7u) | bright];
    for (int i = 0; i < n; ++i) {
        const uint8_t ch = i == 0 || i == n - 1 ? (uint8_t)' ' : (uint8_t)shown[i - 1];
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) argb[(size_t)(y0 + y) * (size_t)width + (size_t)(x0 + i * 8 + x)] = font[ch * 8 + y] & (0x80u >> x) ? ink : paper;
    }
    return 1;
}

/* --- colours ----------------------------------------------------------------------------- */

static const zr_tui_theme WINDOW = {
    ZR_ATTR(ZR_WHITE, ZR_BLACK, 0),     /* normal */
    ZR_ATTR(ZR_BLACK, ZR_CYAN, 1),      /* selected: the highlighted bar */
    ZR_ATTR(ZR_MAGENTA, ZR_BLACK, 0),   /* disabled: reads on black, and is no colour the page uses for anything else */
    ZR_ATTR(ZR_YELLOW, ZR_BLACK, 1),    /* heading */
    ZR_ATTR(ZR_WHITE, ZR_BLACK, 1),     /* value */
    ZR_ATTR(ZR_CYAN, ZR_BLACK, 1),      /* action */
    ZR_ATTR(ZR_CYAN, ZR_BLACK, 0),      /* frame */
    ZR_ATTR(ZR_WHITE, ZR_BLACK, 1),     /* title */
    ZR_ATTR(ZR_CYAN, ZR_BLACK, 0),      /* scroll */
    ZR_ATTR(ZR_MAGENTA, ZR_BLACK, 1),   /* mark: not original */
};
static const zr_tui_theme POPUP = {
    ZR_ATTR(ZR_BLACK, ZR_WHITE, 1),     /* normal */
    ZR_ATTR(ZR_BLACK, ZR_CYAN, 1),      /* selected */
    ZR_ATTR(ZR_BLUE, ZR_WHITE, 1),      /* disabled */
    ZR_ATTR(ZR_RED, ZR_WHITE, 1),       /* heading */
    ZR_ATTR(ZR_BLACK, ZR_WHITE, 1),     /* value */
    ZR_ATTR(ZR_BLUE, ZR_WHITE, 1),      /* action */
    ZR_ATTR(ZR_BLACK, ZR_WHITE, 1),     /* frame */
    ZR_ATTR(ZR_WHITE, ZR_BLUE, 1),      /* title: a blue bar, which shows against the black page and the white window */
    ZR_ATTR(ZR_BLACK, ZR_WHITE, 1),     /* scroll */
    ZR_ATTR(ZR_MAGENTA, ZR_WHITE, 1),   /* mark: not original */
};
const zr_tui_theme *zr_tui_window_theme(void) { return &WINDOW; }
const zr_tui_theme *zr_tui_popup_theme(void) { return &POPUP; }

/* A pop-up window: white, framed in thin lines, its title in a black bar in place of the top edge. */
static void window(zr_tui *t, zr_tui_rect r, const char *title) {
    const zr_tui_theme *th = &POPUP;
    zr_tui_fill(t, r, ' ', th->normal);
    zr_tui_box(t, r, th->frame);
    zr_tui_bar(t, r.row, r.col, r.cols, th->title, title, 0);
}

/* --- lists and forms ----------------------------------------------------------------------- */

int zr_tui_item_selectable(const zr_tui_item *item) {
    return item->enabled && item->kind != ZR_ITEM_HEADING && item->kind != ZR_ITEM_INFO;
}

static int selectable_at(const zr_tui_list *l, size_t i) { return i < l->count && zr_tui_item_selectable(&l->items[i]); }

/* Whether any row but `index` can be selected before it (direction -1) or after it (1). */
static int selectable_beyond(const zr_tui_list *l, size_t index, int direction) {
    if (direction < 0) { for (size_t i = 0; i < index; ++i) if (selectable_at(l, i)) return 1; }
    else for (size_t i = index + 1; i < l->count; ++i) if (selectable_at(l, i)) return 1;
    return 0;
}

/* Moves the window so that the selected row is in it. Rows that cannot be selected (headings, notes, dimmed rows) are shown only
 * by scrolling, so they come with the selection next to them: the ones right above a selected row at the top of the window (its
 * section's heading), and, when no row before the selected one can be selected, all of the page above it (the page's top); the same
 * below the last row that can be selected (the page's end). */
static void scroll_into_view(zr_tui_list *l) {
    if (l->rows <= 0) return;
    const size_t rows = (size_t)l->rows;
    if (l->count <= rows) { l->top = 0; return; }
    if (l->selected < l->top) l->top = l->selected;
    if (l->selected >= l->top + rows) l->top = l->selected - rows + 1;
    if (l->top > l->count - rows) l->top = l->count - rows;
    if (!selectable_at(l, l->selected)) return;
    if (!selectable_beyond(l, l->selected, -1)) {
        l->top = l->selected < rows ? 0 : l->selected - rows + 1;
        return;
    }
    if (!selectable_beyond(l, l->selected, 1) && l->top + rows < l->count)
        l->top = l->selected < l->count - rows ? l->selected : l->count - rows;
    if (l->top == l->selected) {
        size_t above = 0;
        while (above + 1 < rows && above < l->top && !selectable_at(l, l->top - above - 1)) ++above;
        l->top -= above;
    }
}

void zr_tui_list_set(zr_tui_list *l, const zr_tui_item *items, size_t count, int rows) {
    l->items = items;
    l->count = count;
    l->rows = rows;
    if (l->selected >= count) l->selected = count ? count - 1 : 0;
    if (!selectable_at(l, l->selected)) {
        size_t down = l->selected, up = l->selected;
        int found = 0;
        for (size_t i = l->selected; i < count && !found; ++i) if (selectable_at(l, i)) { down = i; found = 1; }
        if (!found) {
            for (size_t i = l->selected; i-- > 0;) if (selectable_at(l, i)) { up = i; found = 1; break; }
            if (found) down = up;
        }
        if (found) l->selected = down;
    }
    scroll_into_view(l);
}

void zr_tui_list_select(zr_tui_list *l, size_t index) {
    if (selectable_at(l, index)) l->selected = index;
    scroll_into_view(l);
}

/* The next selectable row in a direction, wrapping round; `from` when there is none. */
static size_t step(const zr_tui_list *l, size_t from, int direction) {
    for (size_t n = 1; n <= l->count; ++n) {
        const size_t i = direction > 0 ? (from + n) % l->count : (from + l->count - n % l->count) % l->count;
        if (selectable_at(l, i)) return i;
    }
    return from;
}

zr_tui_result zr_tui_list_event(zr_tui_list *l, zr_tui_event e) {
    if (!l->count) return e.key == ZR_TUI_KEY_ESCAPE ? ZR_TUI_CANCEL : ZR_TUI_NONE;
    const size_t before = l->selected;
    const zr_tui_item *current = selectable_at(l, l->selected) ? &l->items[l->selected] : NULL;
    switch (e.key) {
    case ZR_TUI_KEY_UP: l->selected = step(l, l->selected, -1); break;
    case ZR_TUI_KEY_DOWN: l->selected = step(l, l->selected, 1); break;
    case ZR_TUI_KEY_HOME:
        for (size_t i = 0; i < l->count; ++i) if (selectable_at(l, i)) { l->selected = i; break; }
        break;
    case ZR_TUI_KEY_END:
        for (size_t i = l->count; i-- > 0;) if (selectable_at(l, i)) { l->selected = i; break; }
        break;
    case ZR_TUI_KEY_PAGE_UP:
    case ZR_TUI_KEY_PAGE_DOWN: {
        if (l->form) {
            if (current && (current->kind == ZR_ITEM_CHOICE || current->kind == ZR_ITEM_TOGGLE)) return e.key == ZR_TUI_KEY_PAGE_UP ? ZR_TUI_PREV : ZR_TUI_NEXT;
            return ZR_TUI_NONE;
        }
        const size_t jump = l->rows > 1 ? (size_t)l->rows - 1 : 1;
        const int up = e.key == ZR_TUI_KEY_PAGE_UP;
        size_t target = up ? (l->selected > jump ? l->selected - jump : 0) : (l->selected + jump < l->count ? l->selected + jump : l->count - 1);
        /* a row that cannot be selected gives way to the nearest one back towards where we came from */
        while (target != l->selected && !selectable_at(l, target)) target = up ? target + 1 : target - 1;
        if (selectable_at(l, target)) l->selected = target;
        break;
    }
    case ZR_TUI_KEY_ENTER: return current ? ZR_TUI_ACTIVATE : ZR_TUI_NONE;
    case ZR_TUI_KEY_ESCAPE: return ZR_TUI_CANCEL;
    case ZR_TUI_KEY_CHAR:
        if (l->form && (e.ch == '+' || e.ch == '-')) {
            if (current && (current->kind == ZR_ITEM_CHOICE || current->kind == ZR_ITEM_TOGGLE)) return e.ch == '+' ? ZR_TUI_NEXT : ZR_TUI_PREV;
            return ZR_TUI_NONE;
        }
        if (e.ch == ' ' && current && current->kind == ZR_ITEM_TOGGLE) return ZR_TUI_ACTIVATE;
        if (e.ch > ' ' && !l->form) {
            const int want = e.ch >= 'A' && e.ch <= 'Z' ? e.ch - 'A' + 'a' : e.ch;
            for (size_t n = 1; n <= l->count; ++n) {
                const size_t i = (l->selected + n) % l->count;
                const char first = selectable_at(l, i) && l->items[i].label ? l->items[i].label[0] : 0;
                const int lower = first >= 'A' && first <= 'Z' ? first - 'A' + 'a' : first;
                if (lower && lower == want) { l->selected = i; break; }
            }
        }
        break;
    default: return ZR_TUI_NONE;
    }
    scroll_into_view(l);
    return l->selected != before ? ZR_TUI_MOVED : ZR_TUI_NONE;
}

void zr_tui_list_draw(zr_tui *t, zr_tui_list *l, zr_tui_rect area, const zr_tui_theme *theme) {
    l->rows = area.rows;
    scroll_into_view(l);
    const int scrolled = l->count > (size_t)area.rows;
    const int width = area.cols - (scrolled ? 1 : 0);
    for (int i = 0; i < area.rows; ++i) {
        const int row = area.row + i;
        const size_t index = l->top + (size_t)i;
        zr_tui_fill(t, zr_tui_rect_make(row, area.col, 1, width), ' ', theme->normal);
        if (index >= l->count) continue;
        const zr_tui_item *it = &l->items[index];
        const int selected = index == l->selected && zr_tui_item_selectable(it);
        uint8_t attr = it->kind == ZR_ITEM_HEADING ? theme->heading : !it->enabled ? theme->disabled : it->kind == ZR_ITEM_ACTION ? theme->action : theme->normal;
        uint8_t vattr = !it->enabled ? theme->disabled : theme->value;
        if (selected) {
            attr = vattr = theme->selected;
            zr_tui_fill(t, zr_tui_rect_make(row, area.col, 1, width), ' ', theme->selected);
        }
        if (it->mark) {
            char mark[2] = {it->mark, 0};
            zr_tui_put(t, row, area.col, selected ? theme->selected : it->enabled ? theme->mark : theme->disabled, mark);
        }
        const char *label = it->label ? it->label : "";
        if (it->kind == ZR_ITEM_HEADING || it->kind == ZR_ITEM_ACTION) {
            zr_tui_put_fit(t, row, area.col + 1, width - 2, attr, label, 0);
            continue;
        }
        /* the value at the right edge: a choice between pointers while selected, a toggle as [X], any other as it is */
        char text[ZR_TUI_PATH + 16];
        const char *value = it->value ? it->value : "";
        if (it->kind == ZR_ITEM_CHOICE && selected) snprintf(text, sizeof text, "%c%s%c", ZR_GLYPH_CHAR(ZR_GLYPH_POINT_LEFT), value, ZR_GLYPH_CHAR(ZR_GLYPH_POINT_RIGHT));
        else if (it->kind == ZR_ITEM_TOGGLE) snprintf(text, sizeof text, "[%c]%s%s", it->on ? 'X' : ' ', *value ? " " : "", value);
        else snprintf(text, sizeof text, "%s", value);
        /* the label and the value share the row (a cell of margin each side and one between): each keeps what it needs, and
         * where they do not both fit the shorter keeps all of it and the other is cut, or both are cut in half */
        const int room = width - 2, own = (int)strlen(label);
        int value_width = (int)strlen(text), label_width = own;
        if (!value_width) label_width = room;
        else if (own + 1 + value_width > room) {
            if (value_width <= room / 2) label_width = room - 1 - value_width;
            else if (own <= room / 2) value_width = room - 1 - own;
            else { label_width = (room - 1) / 2; value_width = room - 1 - label_width; }
        }
        zr_tui_put_fit(t, row, area.col + 1, label_width > 0 ? label_width : 0, attr, label, 0);
        if (value_width) zr_tui_put_fit(t, row, area.col + width - 1 - value_width, value_width, selected ? attr : vattr, text, ZR_FIT_RIGHT);
    }
    if (scrolled) zr_tui_scrollbar(t, area.row, area.col + area.cols - 1, area.rows, l->top, (size_t)area.rows, l->count, theme->scroll);
}

/* --- message and confirm boxes -------------------------------------------------------------- */

void zr_tui_dialog_set(zr_tui_dialog *d, const char *title, const char *text, const char *buttons, int selected, int cancel) {
    memset(d, 0, sizeof *d);
    snprintf(d->title, sizeof d->title, "%s", title);
    snprintf(d->text, sizeof d->text, "%s", text);
    while (*buttons && d->buttons < 3) {
        const char *end = strchr(buttons, '|');
        const size_t n = end ? (size_t)(end - buttons) : strlen(buttons);
        snprintf(d->button[d->buttons++], sizeof d->button[0], "%.*s", (int)n, buttons);
        buttons += n + (end ? 1 : 0);
    }
    d->selected = selected >= 0 && selected < d->buttons ? selected : 0;
    d->cancel = cancel < d->buttons ? cancel : -1;
}

zr_tui_result zr_tui_dialog_event(zr_tui_dialog *d, zr_tui_event e) {
    switch (e.key) {
    case ZR_TUI_KEY_LEFT:
    case ZR_TUI_KEY_BACKTAB:
    case ZR_TUI_KEY_UP: d->selected = (d->selected + d->buttons - 1) % d->buttons; return ZR_TUI_MOVED;
    case ZR_TUI_KEY_RIGHT:
    case ZR_TUI_KEY_TAB:
    case ZR_TUI_KEY_DOWN: d->selected = (d->selected + 1) % d->buttons; return ZR_TUI_MOVED;
    case ZR_TUI_KEY_ENTER: d->result = d->selected; return ZR_TUI_ACTIVATE;
    case ZR_TUI_KEY_ESCAPE:
        if (d->cancel < 0) return ZR_TUI_NONE;
        d->result = d->cancel;
        return ZR_TUI_ACTIVATE;
    default: return ZR_TUI_NONE;
    }
}

void zr_tui_dialog_draw(zr_tui *t, const zr_tui_dialog *d, int top, int bottom) {
    char lines[WRAP_LINES][WRAP_LINE];
    const int n = wrap(d->text, 26, lines);
    int inner = (int)strlen(d->title), buttons = 0;
    for (int i = 0; i < n; ++i) if ((int)strlen(lines[i]) > inner) inner = (int)strlen(lines[i]);
    for (int i = 0; i < d->buttons; ++i) buttons += (int)strlen(d->button[i]) + 4 + (i ? 1 : 0);
    if (buttons > inner) inner = buttons;
    if (inner > ZR_TUI_COLS - 4) inner = ZR_TUI_COLS - 4;
    const zr_tui_theme *th = &POPUP;
    const zr_tui_rect r = zr_tui_centered(n + 5, inner + 4, top, bottom);
    window(t, r, d->title);
    for (int i = 0; i < n; ++i) zr_tui_put_fit(t, r.row + 2 + i, r.col + 2, inner, th->normal, lines[i], ZR_FIT_CENTER | ZR_FIT_CUT_END);
    int col = r.col + 2 + (inner - buttons) / 2;
    for (int i = 0; i < d->buttons; ++i) {
        char text[40];
        snprintf(text, sizeof text, "[ %s ]", d->button[i]);
        col += zr_tui_put(t, r.row + 2 + n + 1, col, i == d->selected ? th->selected : th->normal, text) + 1;
    }
}

/* --- the file picker --------------------------------------------------------------------------- */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }
static int compare_names(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if (lower((unsigned char)*a) != lower((unsigned char)*b)) break;
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

static int compare_names_n(const char *a, const char *b, size_t n) {
    for (; n; ++a, ++b, --n) if (lower((unsigned char)*a) != lower((unsigned char)*b)) return lower((unsigned char)*a) - lower((unsigned char)*b);
    return 0;
}

static int entry_order(const void *x, const void *y) {
    const zr_tui_picker_entry *a = x, *b = y;
    if (a->is_folder != b->is_folder) return b->is_folder - a->is_folder;
    return compare_names(a->name, b->name);
}

static void clear_entries(zr_tui_picker *p) {
    for (size_t i = 0; i < p->count; ++i) free(p->entries[i].name);
    p->count = 0;
}

/* Whether `name` has one of the filter's extensions. */
static int matches(const char *filter, const char *name) {
    if (!*filter) return 1;
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    const size_t n = strlen(dot);
    for (const char *f = filter; *f;) {
        const char *end = strchr(f, ';');
        const size_t length = end ? (size_t)(end - f) : strlen(f);
        if (length == n && !compare_names_n(dot, f, n)) return 1;
        f += length + (end ? 1 : 0);
    }
    return 0;
}

typedef struct collect { zr_tui_picker *p; int failed; } collect;

static int add_entry(zr_tui_picker *p, const char *name, int is_folder, uint64_t size) {
    if (p->count == p->capacity) {
        const size_t capacity = p->capacity ? p->capacity * 2 : 64;
        zr_tui_picker_entry *e = realloc(p->entries, capacity * sizeof *e);
        if (!e) return 0;
        p->entries = e;
        p->capacity = capacity;
    }
    zr_tui_picker_entry *e = &p->entries[p->count];
    const size_t n = strlen(name);
    e->name = malloc(n + 1);
    if (!e->name) return 0;
    memcpy(e->name, name, n + 1);
    zr_tui_clean(name, e->shown, sizeof e->shown);
    e->is_folder = is_folder;
    e->size = size;
    e->size_text[0] = 0;
    if (is_folder) snprintf(e->size_text, sizeof e->size_text, "%s", ZR_TXT(PICK_DIR));
    else if (size >= 10u * 1024 * 1024) snprintf(e->size_text, sizeof e->size_text, "%uM", (unsigned)(size / (1024 * 1024)));
    else snprintf(e->size_text, sizeof e->size_text, "%uK", (unsigned)((size + 1023) / 1024));
    ++p->count;
    return 1;
}

static int collect_entry(void *ctx, const char *name, int is_folder, uint64_t size) {
    collect *c = ctx;
    if (!is_folder && !matches(c->p->filter, name)) return 1;
    if (c->p->count >= 20000) return 0;
    if (!add_entry(c->p, name, is_folder, size)) { c->failed = 1; return 0; }
    return 1;
}

/* Reads `folder` ("" the roots) into the list; `select` names the entry to leave selected. 0 when out of memory. */
static int load(zr_tui_picker *p, const char *folder, const char *select) {
    collect c = {p, 0};
    clear_entries(p);
    p->error[0] = 0;
    snprintf(p->folder, sizeof p->folder, "%s", folder);
    int listed = 1;
    if (!*folder) listed = p->fs.roots(p->fs.ctx, collect_entry, &c);
    else listed = p->fs.list(p->fs.ctx, folder, collect_entry, &c);
    if (c.failed) return 0;
    if (!listed) snprintf(p->error, sizeof p->error, "%s", *folder ? ZR_TXT(PICK_CANNOT_READ) : ZR_TXT(PICK_NO_DRIVES));
    qsort(p->entries, p->count, sizeof *p->entries, entry_order);
    if (*folder) {                                              /* ".." first */
        if (!add_entry(p, "..", 1, 0)) return 0;
        p->entries[p->count - 1].size_text[0] = 0;
        zr_tui_picker_entry up = p->entries[p->count - 1];
        memmove(p->entries + 1, p->entries, (p->count - 1) * sizeof *p->entries);
        p->entries[0] = up;
    }
    zr_tui_item *items = realloc(p->items, (p->count ? p->count : 1) * sizeof *items);
    if (!items) return 0;
    p->items = items;
    size_t chosen = *folder && p->count > 1 ? 1 : 0;
    for (size_t i = 0; i < p->count; ++i) {
        p->items[i].kind = ZR_ITEM_TEXT;
        p->items[i].enabled = 1;
        p->items[i].on = 0;
        p->items[i].mark = 0;
        p->items[i].label = p->entries[i].shown;
        p->items[i].value = p->entries[i].size_text;
        if (select && !compare_names(p->entries[i].name, select)) chosen = i;
    }
    memset(&p->list, 0, sizeof p->list);
    zr_tui_list_set(&p->list, p->items, p->count, 12);
    zr_tui_list_select(&p->list, chosen);
    return 1;
}

int zr_tui_picker_open(zr_tui_picker *p, const zr_tui_fs *fs, const char *title, const char *folder, const char *filter) {
    memset(p, 0, sizeof *p);
    p->fs = *fs;
    snprintf(p->title, sizeof p->title, "%s", title);
    snprintf(p->filter, sizeof p->filter, "%s", filter ? filter : "");
    for (char *f = p->filter; *f; ++f) *f = (char)lower((unsigned char)*f);
    if (load(p, folder, NULL)) {
        /* A folder that cannot be read falls back to the roots. */
        if (p->error[0] && *folder) return load(p, "", NULL);
        return 1;
    }
    zr_tui_picker_close(p);
    return 0;
}

void zr_tui_picker_close(zr_tui_picker *p) {
    clear_entries(p);
    free(p->entries);
    free(p->items);
    p->entries = NULL;
    p->items = NULL;
    p->capacity = 0;
}

/* The last part of a folder's path (after the final separator). */
static const char *last_part(const char *path) {
    const char *name = path;
    for (const char *s = path; *s; ++s) if ((*s == '/' || *s == '\\') && s[1]) name = s + 1;
    return name;
}

static void go_up(zr_tui_picker *p) {
    if (!*p->folder) return;
    char parent[ZR_TUI_PATH];
    char came_from[ZR_TUI_PATH];
    snprintf(came_from, sizeof came_from, "%s", last_part(p->folder));
    if (p->fs.parent(p->fs.ctx, p->folder, parent, sizeof parent)) load(p, parent, came_from);
    else load(p, "", p->folder);
}

zr_tui_result zr_tui_picker_event(zr_tui_picker *p, zr_tui_event e) {
    if (e.key == ZR_TUI_KEY_BACKSPACE) { go_up(p); return ZR_TUI_MOVED; }
    if (e.key == ZR_TUI_KEY_ENTER && p->count) {
        const zr_tui_picker_entry *entry = &p->entries[p->list.selected];
        if (!strcmp(entry->name, "..") && *p->folder) { go_up(p); return ZR_TUI_MOVED; }
        char path[ZR_TUI_PATH];
        if (!*p->folder) snprintf(path, sizeof path, "%s", entry->name);
        else p->fs.join(p->fs.ctx, p->folder, entry->name, path, sizeof path);
        if (entry->is_folder) { load(p, path, NULL); return ZR_TUI_MOVED; }
        snprintf(p->picked, sizeof p->picked, "%s", path);
        return ZR_TUI_ACTIVATE;
    }
    const zr_tui_result r = zr_tui_list_event(&p->list, e);
    return r == ZR_TUI_CANCEL ? ZR_TUI_CANCEL : r == ZR_TUI_MOVED ? ZR_TUI_MOVED : ZR_TUI_NONE;
}

void zr_tui_picker_draw(zr_tui *t, zr_tui_picker *p, int top, int bottom) {
    const zr_tui_theme *th = &POPUP;
    const int rows = bottom - top + 1 < 20 ? bottom - top + 1 : 20;
    const zr_tui_rect r = zr_tui_centered(rows, 30, top, bottom);
    window(t, r, p->title);
    char folder[ZR_TUI_PATH];
    zr_tui_clean(*p->folder ? p->folder : ZR_TXT(PICK_DRIVES), folder, sizeof folder);
    zr_tui_put_fit(t, r.row + 1, r.col + 1, r.cols - 2, th->action, folder, 0);
    zr_tui_rule(t, r, 2, th->frame);
    zr_tui_rule(t, r, r.rows - 3, th->frame);
    zr_tui_list_draw(t, &p->list, zr_tui_rect_make(r.row + 3, r.col + 1, r.rows - 6, r.cols - 2), th);
    char hint[80];
    if (p->error[0]) snprintf(hint, sizeof hint, "%s", p->error);
    else if (p->filter[0]) zr_text_format(hint, sizeof hint, ZR_T_PICK_FILES, p->filter);
    else snprintf(hint, sizeof hint, "%s", ZR_TXT(PICK_ALL_FILES));
    zr_tui_put_fit(t, r.row + r.rows - 2, r.col + 1, r.cols - 2, p->error[0] ? th->heading : th->normal, hint, ZR_FIT_CUT_END);
}
