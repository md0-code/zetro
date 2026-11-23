/* ZetRo's own character set: ZetRo's own drawing of the ZX Spectrum's, 8x8 cells, in the manner of the Spectrum
 * (the setup screen, the status line and the test machine draw with it, so no ROM is needed to show them).
 *
 *   20h-7Fh   the Spectrum's order and manner: a pound sign at 60h, an up arrow at 5Eh, a copyright sign at 7Fh
 *   80h-8Fh   the sixteen block graphics: bit 0 the top right quarter, 1 the top left, 2 the bottom right, 3 the
 *             bottom left (8Fh the whole cell)
 *   90h-99h   the ten Romanian letters (zr_font8x8_letters: a-breve, a-circumflex, i-circumflex, s-comma,
 *             t-comma, then the capitals), the codes text.c maps them to
 *   9Ah-9Fh   arrows and marks, the slanted stripe of the title bar
 *   A0h-ABh   thin lines for frames and rules, a shade
 *
 * zr_font8x8_build makes a whole 256-glyph table in the character ROMs' layout (8 bytes per glyph, bit 7 the
 * leftmost pixel): 00h, 20h and FFh blank, DBh a full block, every other code without a glyph a small hollow
 * square, so the gaps show. */
#ifndef ZR_FONT8X8_H
#define ZR_FONT8X8_H

#include <stdint.h>

/* The glyph codes of the marks and pieces the shell draws with. */
enum {
    ZR_GLYPH_UP = 0x5E, ZR_GLYPH_POUND = 0x60, ZR_GLYPH_COPYRIGHT = 0x7F,
    ZR_GLYPH_BLOCK = 0x80,              /* + the quarters wanted (1 top right, 2 top left, 4 bottom right, 8 bottom left) */
    ZR_GLYPH_FULL = 0x8F,
    ZR_GLYPH_DOWN = 0x9A, ZR_GLYPH_POINT_RIGHT = 0x9B, ZR_GLYPH_POINT_LEFT = 0x9C, ZR_GLYPH_ELLIPSIS = 0x9D,
    ZR_GLYPH_BULLET = 0x9E, ZR_GLYPH_STRIPE = 0x9F,
    ZR_GLYPH_LINE_H = 0xA0, ZR_GLYPH_LINE_V = 0xA1,
    ZR_GLYPH_CORNER_TL = 0xA2, ZR_GLYPH_CORNER_TR = 0xA3, ZR_GLYPH_CORNER_BL = 0xA4, ZR_GLYPH_CORNER_BR = 0xA5,
    ZR_GLYPH_TEE_L = 0xA6, ZR_GLYPH_TEE_R = 0xA7, ZR_GLYPH_TEE_DOWN = 0xA8, ZR_GLYPH_TEE_UP = 0xA9, ZR_GLYPH_CROSS = 0xAA,
    ZR_GLYPH_SHADE = 0xAB
};

/* A glyph code as a char (the codes above 7Fh do not fit a signed one: the value is the code less 256). */
#define ZR_GLYPH_CHAR(code) ((char)((code) > 127 ? (code) - 256 : (code)))

/* 20h-7Fh. */
extern const uint8_t zr_font8x8_ascii[96][8];
/* The codes of the Romanian letters ă â î ș ț and their capitals, in that order. */
extern const uint8_t zr_font8x8_letters[10];

void zr_font8x8_build(uint8_t font[2048]);

#endif /* ZR_FONT8X8_H */
