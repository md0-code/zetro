/* The on-screen keyboard: the running machine's own key layout, drawn into the frame the presenter shows
 * (over the lower part of the machine's picture, so the CRT look applies to it), clickable and pad-driven.
 *
 * What is drawn comes from the family's key table (machine.h, zr_key_desc): each key with a position and a size is a keycap at its
 * place with its legend, in the Spectrum's look (dark keycaps with light legends, a bright edge on the key under the pointer or the
 * selection, held keys inverted, latched shift keys yellow) in ZetRo's own font. The layout is scaled to the frame's width whatever
 * the table's extent and the frame's size and shape (320 x 288, 352 x 288, 544 x 288, 704 x 288, ...): glyphs are drawn one pixel
 * wide on a frame of square-ish pixels and two wide where the pixels are narrow (the 80-column frames), the keyboard takes at most
 * 40 % of the frame's height above a footer line, and a legend is fitted to its keycap (proportional letter spacing, then two lines,
 * then abbreviated words). A keycap that has a symbol-shift character in the family's typing table shows it under the main legend
 * (in red); on the Spectrum-compatible layouts a keycap that has room shows its keyword too (in green).
 *
 * Input: the pointer presses the key under it while the button is held; a shift-type key (the keys the typing table uses as
 * `shift`, and those named CAPS SHIFT, SYMBOL SHIFT, SHIFT, RIGHT SHIFT and CTRL) latches on a press and stays held until the next
 * ordinary key has been released, a second press unlatches it; a pad's d-pad moves a selection to the nearest key in that direction,
 * A presses the selected key (held while A is held, shift keys latch the same way), B closes. Keys pressed on the host keyboard show
 * as held. The keyboard calls the machine's key function (`key_fn`) and nothing else of the machine's.
 *
 * No SDL and no OS call here: the shell (desktop.c) translates the mouse, key and controller events, so this can be unit-tested.
 * ZetRo's own code. */
#ifndef ZR_OSK_H
#define ZR_OSK_H

#include "../core/machine.h"

#include <stddef.h>
#include <stdint.h>

#define ZR_OSK_KEYS 128

/* The machine's key `key` of the family's table goes down (down 1) or up. */
typedef void (*zr_osk_key_fn)(void *ctx, unsigned key, int down);

typedef struct zr_osk_rect { int x, y, w, h; } zr_osk_rect;

typedef enum zr_osk_pad { ZR_OSK_PAD_UP, ZR_OSK_PAD_DOWN, ZR_OSK_PAD_LEFT, ZR_OSK_PAD_RIGHT, ZR_OSK_PAD_A, ZR_OSK_PAD_B } zr_osk_pad_key;
enum { ZR_OSK_MOUSE_MOVE, ZR_OSK_MOUSE_DOWN, ZR_OSK_MOUSE_UP };

typedef struct zr_osk {
    const zr_family *family;
    zr_osk_key_fn key_fn;
    void *ctx;
    unsigned count;                     /* the keys of the table (at most ZR_OSK_KEYS) */
    int open;
    int selected;                       /* the pad's key, -1 none */
    int hover;                          /* the key under the pointer, -1 none */
    int pad_mode;                       /* the selection is the one shown (a pad moved it last) rather than the pointer's */
    int mouse_key, pad_key;             /* the key held by the pointer's button, by the pad's A; -1 none */
    int sym_shift;                      /* the family's symbol-shift key (the one whose second legends are shown), -1 none */
    int keywords;                       /* the Spectrum-compatible keyword legends are shown */
    int extent_w, extent_h;             /* the table's extent in quarters of a key */
    uint8_t drawn[ZR_OSK_KEYS];         /* the key has a position and a size */
    uint8_t shift[ZR_OSK_KEYS];         /* a shift-type key: latches */
    uint8_t pressed[ZR_OSK_KEYS];       /* held down by the pointer or the pad */
    uint8_t latched[ZR_OSK_KEYS];
    uint8_t host[ZR_OSK_KEYS];          /* held down on the host's keyboard (a count) */
    char legend[ZR_OSK_KEYS][24];       /* the main legend (the key's name where the table gives none) */
    char sym[ZR_OSK_KEYS];              /* the symbol-shift character, 0 none */
    char keyword[ZR_OSK_KEYS][10];
    /* the layout for the frame (zr_osk_layout) */
    int fw, fh, faw, fah;
    int gx, gy;                         /* the glyphs' scale */
    zr_osk_rect panel, footer;
    zr_osk_rect rect[ZR_OSK_KEYS];
} zr_osk;

/* The keyboard of `family`, closed; `key_fn` is told of every key it presses and lets go. */
void zr_osk_init(zr_osk *o, const zr_family *family, zr_osk_key_fn key_fn, void *ctx);

int zr_osk_is_open(const zr_osk *o);
void zr_osk_open(zr_osk *o);
/* Closing lets go of everything the keyboard holds, latched shift keys included. */
void zr_osk_close(zr_osk *o);
void zr_osk_toggle(zr_osk *o);
/* Lets go of what the keyboard holds, whether it is open or not (the machine was released). */
void zr_osk_release(zr_osk *o);

/* The layout for a frame of `width` x `height` pixels shown `aspect_w`:`aspect_h` (0: 4:3): where each keycap is, the panel behind
 * them and the footer line. Done again only when the frame's geometry changes. */
void zr_osk_layout(zr_osk *o, int width, int height, int aspect_w, int aspect_h);
/* The keycap of a key in the frame's pixels, after the layout; 0 for a key that is not drawn. */
int zr_osk_key_rect(const zr_osk *o, unsigned key, zr_osk_rect *rect);
/* The key under a pixel of the frame, -1 for none. */
int zr_osk_key_at(const zr_osk *o, int x, int y);

/* The keyboard drawn into an ARGB picture of the frame's size (the layout is made for it): the panel, the keycaps, the footer. */
void zr_osk_draw(zr_osk *o, const uint8_t font[2048], uint32_t *argb, int width, int height, int aspect_w, int aspect_h);

/* The pointer at (x, y) of the frame (-1, -1: outside it) and its left button: ZR_OSK_MOUSE_MOVE, DOWN or UP. 1 when the keyboard
 * took it (it is open, and the pointer is on a key or the button's press is still held). */
int zr_osk_mouse(zr_osk *o, int x, int y, int action);
/* A pad's key: the d-pad moves the selection, A presses the selected key (`down` 1) and lets it go (0), B closes the keyboard. 1 when taken. */
int zr_osk_pad(zr_osk *o, zr_osk_pad_key key, int down);
/* A key of the family's table went down or up on the host's keyboard: it shows as held. */
void zr_osk_host(zr_osk *o, unsigned key, int down);

/* The key's name as it is on the keycap (the legend), for messages and tests. */
const char *zr_osk_legend(const zr_osk *o, unsigned key);

#endif /* ZR_OSK_H */
