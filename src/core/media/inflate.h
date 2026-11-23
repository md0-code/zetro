/* A small DEFLATE decoder (RFC 1951) with the zlib wrapper (RFC 1950), for
 * the Z-RLE form of CSW and for TZX block 0x18. Stored, fixed and dynamic
 * blocks; every length, distance and output size is checked, and a damaged
 * stream gives an error, never a crash or an endless loop. ZetRo's own
 * code, written from the two RFCs. */
#ifndef ZR_INFLATE_H
#define ZR_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* Decodes a raw DEFLATE stream. On success returns 1 with the bytes in
 * *out (malloc'd, free() them; never NULL) and their count in *out_length;
 * `used` (optional) receives how many input bytes the stream took. `limit`
 * is the most the output may hold. On failure returns 0 and writes the
 * reason. */
int zr_inflate(const uint8_t *src, size_t length, size_t limit, uint8_t **out, size_t *out_length, size_t *used,
               char *error, size_t capacity);

/* The same for a zlib stream: the two header bytes, the DEFLATE data and
 * the Adler-32 of the output, which is checked when it is present. A
 * stream whose header is not a valid zlib header is tried as raw DEFLATE
 * (some writers leave the wrapper out). */
int zr_inflate_zlib(const uint8_t *src, size_t length, size_t limit, uint8_t **out, size_t *out_length,
                    char *error, size_t capacity);

#endif /* ZR_INFLATE_H */
