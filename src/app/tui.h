/* The text-cell toolkit of the setup screen: a ZX Spectrum screen of
 * 32 x 24 cells of 8 x 8 pixels, each a character code and an attribute byte as the Spectrum has it (ink in
 * bits 0-2, paper in bits 3-5, BRIGHT in bit 6, FLASH in bit 7), in a border: a 320 x 240 frame, shown 4:3.
 * The frame is a picture of the eight Spectrum colours with BRIGHT (sixteen palette entries, as the test
 * machine's), so it passes through the same presenter and CRT look as a machine's. Drawn with ZetRo's own
 * character set (core/font8x8.h), the same on every machine. On top of the cells: thin-line frames, title bars
 * with the Romanian flag's stripes in the manner of the 128K menus, lists and forms with choices, toggles and actions, message boxes
 * and a file picker over an abstract file system. Input is abstracted into events (the shell translates the
 * host's keys), so the window and a headless run drive it alike.
 *
 * No SDL and no OS call here: the host's folders reach the picker through zr_tui_fs, filled in by the setup
 * screen. ZetRo's own code. */
#ifndef ZR_TUI_H
#define ZR_TUI_H

#include "../core/machine.h"

#include <stddef.h>
#include <stdint.h>

#define ZR_TUI_COLS 32
#define ZR_TUI_ROWS 24
#define ZR_TUI_BORDER_X 32              /* the border around the 256 x 192 picture: 32 pixels at the sides */
#define ZR_TUI_BORDER_Y 24              /* and 24 above and below */
#define ZR_TUI_WIDTH (ZR_TUI_COLS * 8 + 2 * ZR_TUI_BORDER_X)
#define ZR_TUI_HEIGHT (ZR_TUI_ROWS * 8 + 2 * ZR_TUI_BORDER_Y)

/* The Spectrum's eight colours in the order of its attribute bits. */
enum { ZR_BLACK, ZR_BLUE, ZR_RED, ZR_MAGENTA, ZR_GREEN, ZR_CYAN, ZR_YELLOW, ZR_WHITE };
#define ZR_FLASH 0x80u
#define ZR_ATTR(ink, paper, bright) ((uint8_t)((ink) | (paper) << 3 | (bright) << 6))
/* The sixteen colours of the picture (0x00RRGGBB): the eight colours, then the same BRIGHT. */
extern const uint32_t zr_tui_palette[16];

typedef struct zr_tui_rect { int row, col, rows, cols; } zr_tui_rect;

typedef struct zr_tui_cell { uint8_t ch, attr; } zr_tui_cell;

typedef struct zr_tui {
    zr_tui_cell cell[ZR_TUI_ROWS][ZR_TUI_COLS];
    uint8_t border;                     /* the border's colour, 0-7 */
    uint8_t font[2048];                 /* 256 glyphs of 8 rows, bit 7 the leftmost pixel */
    uint8_t pixels[ZR_TUI_WIDTH * ZR_TUI_HEIGHT];
    uint64_t serial;                    /* changes whenever the pixels change */
} zr_tui;

/* --- the font ---------------------------------------------------------------------- */

/* A font of 2048 bytes, NULL for ZetRo's own (core/font8x8.h). The cells are cleared. */
void zr_tui_init(zr_tui *t, const uint8_t *font);

/* --- cells -------------------------------------------------------------------------- */

zr_tui_rect zr_tui_rect_make(int row, int col, int rows, int cols);
/* The rectangle of a given size centred on the screen between rows top and bottom (inclusive). */
zr_tui_rect zr_tui_centered(int rows, int cols, int top, int bottom);
void zr_tui_clear(zr_tui *t, uint8_t attr);
void zr_tui_fill(zr_tui *t, zr_tui_rect r, uint8_t ch, uint8_t attr);
/* Text from (row, col), clipped at the screen's edge; the bytes are the codes of the cells. Returns the cells written. */
int zr_tui_put(zr_tui *t, int row, int col, uint8_t attr, const char *text);
/* Text in exactly `width` cells (blank padded), cut where too long with an ellipsis in the middle ("C:\ai...\a.tap"),
 * or at its end (ZR_FIT_CUT_END) or its start (ZR_FIT_CUT_START). ZR_FIT_RIGHT and ZR_FIT_CENTER align a short text. */
enum { ZR_FIT_RIGHT = 1, ZR_FIT_CENTER = 2, ZR_FIT_CUT_END = 4, ZR_FIT_CUT_START = 8 };
void zr_tui_put_fit(zr_tui *t, int row, int col, int width, uint8_t attr, const char *text, int flags);
/* Text broken into lines of `width` cells at spaces, at most `rows` lines (the rest is cut); each line is blank
 * padded to the width. Returns the lines used. */
int zr_tui_put_wrapped(zr_tui *t, int row, int col, int width, int rows, uint8_t attr, const char *text);
/* A name from outside the program's own tables (a file name, a path, a block's name) as cells: UTF-8 Romanian letters
 * as the font has them, any other character beyond ASCII, and the control codes, as '?'. */
void zr_tui_clean(const char *utf8, char *out, size_t capacity);

/* A frame of thin lines on the edge of r (the inside is not touched). */
void zr_tui_box(zr_tui *t, zr_tui_rect r, uint8_t attr);
/* A horizontal rule across row `row` of r, joined to the frame's sides by tees. */
void zr_tui_rule(zr_tui *t, zr_tui_rect r, int row, uint8_t attr);
/* A title bar across `width` cells from (row, col) in the colours `attr` with the text from its second cell; `stripes` puts the
 * three stripes in the manner of the 128K menus, in the Romanian flag's blue, yellow and red, at its right end (four cells). */
void zr_tui_bar(zr_tui *t, int row, int col, int width, uint8_t attr, const char *text, int stripes);
/* A vertical bar in column `col`: up and down arrows, a track and a thumb showing top/visible of total. */
void zr_tui_scrollbar(zr_tui *t, int row, int col, int height, size_t top, size_t visible, size_t total, uint8_t attr);
/* The cell at (row, col), NULL outside the screen. */
const zr_tui_cell *zr_tui_cell_at(const zr_tui *t, int row, int col);

/* The cells drawn into the 320 x 240 pixels (one byte a pixel, a colour 0-15); `flash_on` is the phase of the
 * FLASH attribute (cells with it show ink and paper swapped while it is on). */
void zr_tui_render(zr_tui *t, int flash_on);
/* The frame as a machine's: 320 x 240, shown 4:3, the Spectrum palette. */
void zr_tui_frame(const zr_tui *t, zr_frame *frame);

/* The status line: `text` as a message in cells of the font over the bottom row of an ARGB picture of `width` x `height`
 * (any size: a machine's frame), from its left edge, a cell of margin on each side, cut where it does not fit. Only the
 * cells in use are drawn, so the machine's picture shows round them. `attr` is the cells' attribute. 0 when nothing could be
 * drawn (a picture narrower than a few cells). */
int zr_tui_status_line(const uint8_t font[2048], uint32_t *argb, int width, int height, const char *text, uint8_t attr);

/* --- input -------------------------------------------------------------------------- */

typedef enum zr_tui_key {
    ZR_TUI_KEY_NONE, ZR_TUI_KEY_UP, ZR_TUI_KEY_DOWN, ZR_TUI_KEY_LEFT, ZR_TUI_KEY_RIGHT, ZR_TUI_KEY_ENTER, ZR_TUI_KEY_ESCAPE,
    ZR_TUI_KEY_PAGE_UP, ZR_TUI_KEY_PAGE_DOWN, ZR_TUI_KEY_HOME, ZR_TUI_KEY_END, ZR_TUI_KEY_TAB, ZR_TUI_KEY_BACKTAB,
    ZR_TUI_KEY_BACKSPACE, ZR_TUI_KEY_CHAR
} zr_tui_key;

typedef struct zr_tui_event {
    zr_tui_key key;
    int ch;                             /* ZR_TUI_KEY_CHAR: the character, 0x20-0x7E */
} zr_tui_event;

/* PREV and NEXT: the value of a CHOICE or TOGGLE row of a form goes back or on. */
typedef enum zr_tui_result { ZR_TUI_NONE, ZR_TUI_MOVED, ZR_TUI_ACTIVATE, ZR_TUI_PREV, ZR_TUI_NEXT, ZR_TUI_CANCEL } zr_tui_result;

/* --- colours -------------------------------------------------------------------------- */

typedef struct zr_tui_theme {
    uint8_t normal, selected, disabled, heading, value, action, frame, title, scroll, mark;
} zr_tui_theme;
/* The page's colours (white on black, the 128K menu's) and the pop-ups' (black on white). */
const zr_tui_theme *zr_tui_window_theme(void);
const zr_tui_theme *zr_tui_popup_theme(void);

/* --- lists and forms ------------------------------------------------------------------- */

/* One row of a list. TEXT: a plain entry (its value, when any, at the right edge); HEADING and INFO are never
 * selected; CHOICE is changed with Page Up and Page Down or + and - in a form (and Enter); TOGGLE flips; ACTION runs.
 * A disabled row is drawn dim and skipped. `mark` (0: none) is a character drawn in the first cell: the "not original"
 * mark. */
enum { ZR_ITEM_TEXT, ZR_ITEM_HEADING, ZR_ITEM_INFO, ZR_ITEM_CHOICE, ZR_ITEM_TOGGLE, ZR_ITEM_ACTION };
typedef struct zr_tui_item {
    int kind;
    int enabled;
    int on;                             /* TOGGLE */
    char mark;
    const char *label;
    const char *value;                  /* CHOICE, INFO, TEXT (may be NULL) */
} zr_tui_item;

typedef struct zr_tui_list {
    const zr_tui_item *items;           /* not owned */
    size_t count;
    size_t selected, top;
    int rows;                           /* rows shown */
    int form;                           /* Page Up, Page Down, + and - change the selected row's value instead of paging (the setup pages) */
} zr_tui_list;

int zr_tui_item_selectable(const zr_tui_item *item);
/* Sets the rows; the selection stays when it still can be selected, else moves to the nearest row that can. */
void zr_tui_list_set(zr_tui_list *list, const zr_tui_item *items, size_t count, int rows);
void zr_tui_list_select(zr_tui_list *list, size_t index);
/* Up, down, home, end move, and so do Page Up and Page Down, a page at a time; in a form (list->form) Page Up, Page
 * Down, + and - on a CHOICE or TOGGLE give PREV or NEXT instead (left and right are the form's own: the page of the
 * screen); Enter gives ACTIVATE (a selected row only); Escape CANCEL; a letter jumps to the next row whose label
 * starts with it. */
zr_tui_result zr_tui_list_event(zr_tui_list *list, zr_tui_event event);
void zr_tui_list_draw(zr_tui *t, zr_tui_list *list, zr_tui_rect area, const zr_tui_theme *theme);

/* --- message and confirm boxes ------------------------------------------------------------- */

typedef struct zr_tui_dialog {
    char title[32], text[256];
    char button[3][16];
    int buttons, selected;
    int cancel;                         /* the button Escape picks, -1 none */
    int result;                         /* the button picked, after ACTIVATE */
} zr_tui_dialog;
/* buttons: names separated by '|' ("Restart|Back"); at most three. */
void zr_tui_dialog_set(zr_tui_dialog *d, const char *title, const char *text, const char *buttons, int selected, int cancel);
zr_tui_result zr_tui_dialog_event(zr_tui_dialog *d, zr_tui_event event);
/* Centred between rows top and bottom. */
void zr_tui_dialog_draw(zr_tui *t, const zr_tui_dialog *d, int top, int bottom);

/* --- the file picker -------------------------------------------------------------------------- */

typedef int (*zr_tui_entry_fn)(void *ctx, const char *name, int is_folder, uint64_t size);
/* The file system as the picker sees it. Paths are opaque strings: a root ("C:\" or "/") keeps its separator, other
 * folders have none at the end. */
typedef struct zr_tui_fs {
    void *ctx;
    /* The entries of a folder; 0 when it cannot be read. */
    int (*list)(void *ctx, const char *folder, zr_tui_entry_fn fn, void *fn_ctx);
    /* The roots (drives): each as a folder name. */
    int (*roots)(void *ctx, zr_tui_entry_fn fn, void *fn_ctx);
    /* The folder above; 0 at a root (the picker then shows the roots). */
    int (*parent)(void *ctx, const char *folder, char *out, size_t capacity);
    /* The path of a name inside a folder. */
    void (*join)(void *ctx, const char *folder, const char *name, char *out, size_t capacity);
} zr_tui_fs;

#define ZR_TUI_PATH 1024
typedef struct zr_tui_picker_entry {
    char *name;
    char shown[64];                     /* the name as cells */
    int is_folder;
    uint64_t size;
    char size_text[16];
} zr_tui_picker_entry;

typedef struct zr_tui_picker {
    zr_tui_fs fs;
    char title[32];
    char filter[64];                    /* extensions with their dot, lower case, ';' apart: ".tap;.tzx"; empty: every file */
    char folder[ZR_TUI_PATH];           /* "" while the roots are shown */
    char picked[ZR_TUI_PATH];           /* the file, after ACTIVATE */
    char error[64];
    zr_tui_picker_entry *entries;
    zr_tui_item *items;
    size_t count, capacity;
    zr_tui_list list;
} zr_tui_picker;
/* Lists `folder` (the roots when it cannot be read and has no parent); 0 when out of memory. */
int zr_tui_picker_open(zr_tui_picker *p, const zr_tui_fs *fs, const char *title, const char *folder, const char *filter);
void zr_tui_picker_close(zr_tui_picker *p);
/* Enter on a folder opens it, on a file picks it (ACTIVATE, p->picked); Backspace goes up; Escape CANCEL. */
zr_tui_result zr_tui_picker_event(zr_tui_picker *p, zr_tui_event event);
void zr_tui_picker_draw(zr_tui *t, zr_tui_picker *p, int top, int bottom);

#endif /* ZR_TUI_H */
