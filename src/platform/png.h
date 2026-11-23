/* PNG screenshots without a library: an 8-bit RGB image written with the
 * Sub filter and a deflate stream of fixed Huffman codes and hashed
 * matches (RFC 1951, 1950 and 2083). No SDL. ZetRo's own code. */
#ifndef ZR_PNG_H
#define ZR_PNG_H

#include <stddef.h>
#include <stdint.h>

/* The ARGB8888 pixels (`pitch` bytes a row; the alpha is dropped) as a PNG
 * file; `flip` writes the rows bottom to top (a GL read-back). 0 when the
 * file cannot be written. */
int zr_png_write(const char *path, const uint32_t *argb, int width, int height, size_t pitch, int flip);

/* 1 when the file name ends in .png (any case). */
int zr_png_name(const char *path);

#endif /* ZR_PNG_H */
