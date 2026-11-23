/* The disk's inside, shared by the format readers and writers (disk.h is the
 * interface). ZetRo's own code. */
#ifndef ZR_DISK_IMPL_H
#define ZR_DISK_IMPL_H

#include <stdio.h>

#include "disk.h"

/* Reports a failure in the caller's own `error` and `capacity`; the value is 0. */
#define ZR_DISK_FAIL(...) (((capacity) ? (void)snprintf(error, capacity, __VA_ARGS__) : (void)0), 0)

#define ZR_DISK_MAX_STORED 65535u               /* the extended format's length field */
#define ZR_DISK_MAX_TRACK_BLOCK (255u * 256u)   /* ... and its track size table */
#define ZR_DISK_STD_SIZE_FILL 0xE5u

static inline uint32_t zr_disk_rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* The header of a track block of `count` sectors, rounded up to 256 bytes. */
static inline uint32_t zr_disk_track_header(unsigned count) { return (0x18u + 8u * count + 255u) & ~255u; }

/* The size code of the smallest nominal sector of `bytes` (a power of two from 128): -1 when it is none. */
int zr_disk_size_code(unsigned bytes);

/* Frees a track's sectors and empties it (the other fields stay). */
void zr_disk_clear_track(zr_track *track);
/* Gives a track `count` empty sectors (its old ones are freed); 0 when out of memory. */
int zr_disk_alloc_track(zr_track *track, unsigned count);
/* Sets how a sector of `stored` bytes of data is divided into copies and a recorded length. */
void zr_disk_set_stored(zr_sector *sector, uint32_t stored);

/* Readers: the disk is filled in; 0 with the reason on failure (the caller closes it). */
int zr_disk_read_dsk(zr_disk *disk, const uint8_t *data, size_t size, int extended, char *error, size_t capacity);
int zr_disk_read_raw(zr_disk *disk, const uint8_t *data, size_t size, const zr_disk_geom *geom, char *error, size_t capacity);

/* Writers: the size of the file and then the file itself (`out` is at least that long). 0 from the size
 * function when the disk cannot be written, with the reason. */
size_t zr_disk_dsk_size(const zr_disk *disk, int extended, char *error, size_t capacity);
void zr_disk_dsk_write(const zr_disk *disk, int extended, uint8_t *out);
/* 1 when the standard format can hold the disk as it is. */
int zr_disk_dsk_standard_ok(const zr_disk *disk);
size_t zr_disk_raw_file_size(const zr_disk *disk, char *error, size_t capacity);
void zr_disk_raw_write(const zr_disk *disk, uint8_t *out);

#endif /* ZR_DISK_IMPL_H */
