/* Snapshots (snap.h): the neutral structure, the format detection and the checks every format's reader ends with.
 * ZetRo's own code. */
#include "snap_impl.h"

#include <stdarg.h>
#include <stdio.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
int zr_snap_fail(char *error, size_t capacity, const char *format, ...) {
    if (capacity) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, capacity, format, args);
        va_end(args);
    }
    return 0;
}

zr_snap *zr_snap_new(void) { return calloc(1, sizeof(zr_snap)); }
void zr_snap_free(zr_snap *s) { free(s); }

void zr_snap_set_machine(zr_snap *s, unsigned machine, unsigned id, const char *name) {
    s->machine = machine;
    s->id = id;
    snprintf(s->name, sizeof s->name, "%s", name);
}

int zr_snap_is_128k(const zr_snap *s) {
    return s->machine == ZR_SNAP_M_128K || s->machine == ZR_SNAP_M_PLUS2 || s->machine == ZR_SNAP_M_PLUS2A || s->machine == ZR_SNAP_M_PLUS3;
}

unsigned zr_snap_pages_of(unsigned machine) {
    switch (machine) {
    case ZR_SNAP_M_48K: return 1u << 5 | 1u << 2 | 1u << 0;
    case ZR_SNAP_M_16K: return 1u << 5;
    case ZR_SNAP_M_128K:
    case ZR_SNAP_M_PLUS2:
    case ZR_SNAP_M_PLUS2A:
    case ZR_SNAP_M_PLUS3: return 0xFFu;
    default: return 0;
    }
}

/* --- formats ------------------------------------------------------------------------------- */

static int same_ending(const char *name, const char *ending) {
    const size_t n = strlen(name), e = strlen(ending);
    if (n < e + 1 || name[n - e - 1] != '.') return 0;
    for (size_t i = 0; i < e; ++i) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        if (c != ending[i]) return 0;
    }
    return 1;
}

unsigned zr_snap_format_of_name(const char *name) {
    if (!name) return ZR_SNAP_NONE;
    if (same_ending(name, "sna")) return ZR_SNAP_SNA;
    if (same_ending(name, "z80")) return ZR_SNAP_Z80;
    if (same_ending(name, "szx")) return ZR_SNAP_SZX;
    return ZR_SNAP_NONE;
}

const char *zr_snap_extension(unsigned format) {
    return format == ZR_SNAP_SNA ? "sna" : format == ZR_SNAP_Z80 ? "z80" : format == ZR_SNAP_SZX ? "szx" : "";
}

/* --- reading -------------------------------------------------------------------------------- */

/* What every reader ends with: the values in range, the RAM the machine class needs all there. */
static int check(const zr_snap *s, char *error, size_t capacity) {
    if (s->im > 2) return zr_snap_fail(error, capacity, "Damaged snapshot: interrupt mode %u", s->im);
    const unsigned need = zr_snap_pages_of(s->machine);
    for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page)
        if ((need & ~s->pages) >> page & 1u)
            return zr_snap_fail(error, capacity, "Damaged snapshot: RAM page %u of the %s is missing", page, s->name);
    return 1;
}

int zr_snap_read(const uint8_t *bytes, size_t size, const char *name, zr_snap *snap, char *error, size_t capacity) {
    memset(snap, 0, sizeof *snap);
    if (!bytes || !size) return zr_snap_fail(error, capacity, "The file is empty");
    unsigned format = ZR_SNAP_NONE;
    if (size >= 4 && !memcmp(bytes, "ZXST", 4)) format = ZR_SNAP_SZX;
    else {
        const unsigned named = zr_snap_format_of_name(name);
        if (named == ZR_SNAP_SZX) return zr_snap_fail(error, capacity, "Not an SZX snapshot: it does not start with ZXST");
        if (named != ZR_SNAP_NONE) format = named;
        else if (size == 49179u || size == 131103u || size == 147487u) format = ZR_SNAP_SNA;
        else if (size >= 30) format = ZR_SNAP_Z80;
        else return zr_snap_fail(error, capacity, "Not a snapshot file");
    }
    int ok = 0;
    if (format == ZR_SNAP_SNA) ok = zr_snap_read_sna(bytes, size, snap, error, capacity);
    else if (format == ZR_SNAP_Z80) ok = zr_snap_read_z80(bytes, size, snap, error, capacity);
    else ok = zr_snap_read_szx(bytes, size, snap, error, capacity);
    if (ok) {
        snap->format = format;
        ok = check(snap, error, capacity);
    }
    if (!ok) memset(snap, 0, sizeof *snap);
    return ok;
}

/* --- writing -------------------------------------------------------------------------------- */

int zr_snap_write(const zr_snap *snap, unsigned format, uint8_t **bytes, size_t *size, char *error, size_t capacity) {
    *bytes = NULL;
    *size = 0;
    const unsigned need = zr_snap_pages_of(snap->machine);
    if ((need & ~snap->pages) && snap->machine != ZR_SNAP_M_OTHER) return zr_snap_fail(error, capacity, "The snapshot lacks RAM it needs");
    if (format == ZR_SNAP_SNA) return zr_snap_write_sna(snap, bytes, size, error, capacity);
    if (format == ZR_SNAP_Z80) return zr_snap_write_z80(snap, bytes, size, error, capacity);
    if (format == ZR_SNAP_SZX) return zr_snap_write_szx(snap, bytes, size, error, capacity);
    return zr_snap_fail(error, capacity, "A snapshot is written as .sna, .z80 or .szx");
}
