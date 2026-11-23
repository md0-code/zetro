/* Inside the snapshot modules (snap.h): the helpers the three formats share. Not for the shell or the boards. */
#ifndef ZR_SNAP_IMPL_H
#define ZR_SNAP_IMPL_H

#include "snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Writes the message and returns 0, so that a failure is one line: return zr_snap_fail(error, capacity, "..."). */
int zr_snap_fail(char *error, size_t capacity, const char *format, ...);

static inline unsigned zr_snap_rd16(const uint8_t *p) { return (unsigned)p[0] | (unsigned)p[1] << 8; }
static inline uint32_t zr_snap_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline void zr_snap_put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void zr_snap_put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* 1 when the class holds the RAM pages 0 to 7 as the 128K machine does (the 128K family and the clones that copy it). */
int zr_snap_is_128k(const zr_snap *s);

/* The readers and writers of the formats: the same contract as zr_snap_read and zr_snap_write; `snap` is zeroed. */
int zr_snap_read_sna(const uint8_t *bytes, size_t size, zr_snap *snap, char *error, size_t capacity);
int zr_snap_read_z80(const uint8_t *bytes, size_t size, zr_snap *snap, char *error, size_t capacity);
int zr_snap_read_szx(const uint8_t *bytes, size_t size, zr_snap *snap, char *error, size_t capacity);
int zr_snap_write_sna(const zr_snap *snap, uint8_t **bytes, size_t *size, char *error, size_t capacity);
int zr_snap_write_z80(const zr_snap *snap, uint8_t **bytes, size_t *size, char *error, size_t capacity);
int zr_snap_write_szx(const zr_snap *snap, uint8_t **bytes, size_t *size, char *error, size_t capacity);

/* The address of the HALT a snapshot's PC is past, for the formats that cannot say HALT. */
static inline uint16_t zr_snap_pc_for_file(const zr_snap *s) { return s->halted ? (uint16_t)(s->pc - 1u) : s->pc; }

#endif /* ZR_SNAP_IMPL_H */
