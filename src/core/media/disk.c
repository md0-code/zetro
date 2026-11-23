/* Floppy disks (disk.h): the container, the geometry table, the sector and
 * track operations and the format dispatch. ZetRo's own code. */
#include "disk_impl.h"

#include <stdlib.h>
#include <string.h>

/* --- the raw geometries ---------------------------------------------------------------------- */

/* A size may mean several; the first is the best guess. 327,680 bytes is the HC machines' 40 x 2 x 16 x 256 before the
 * PC's 320 KiB 40 x 2 x 8 x 512. The 8" entries are IBM 3740 (FM, 128 bytes) and 8" double density. */
static const zr_disk_geom GEOMS[] = {
    {40, 2, 16, 256, ZR_ENC_MFM},
    {40, 2, 8, 512, ZR_ENC_MFM},
    {40, 1, 8, 512, ZR_ENC_MFM},
    {40, 1, 9, 512, ZR_ENC_MFM},
    {40, 2, 9, 512, ZR_ENC_MFM},
    {80, 2, 9, 512, ZR_ENC_MFM},
    {80, 2, 16, 256, ZR_ENC_MFM},
    {80, 2, 8, 512, ZR_ENC_MFM},
    {77, 1, 26, 128, ZR_ENC_FM},
    {77, 2, 26, 128, ZR_ENC_FM},
    {77, 2, 8, 1024, ZR_ENC_MFM},
};

size_t zr_disk_raw_size(const zr_disk_geom *g) {
    return (size_t)g->cylinders * g->sides * g->sectors * g->sector_size;
}

unsigned zr_disk_geometries(size_t size, zr_disk_geom *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof GEOMS / sizeof GEOMS[0] && n < max; i++)
        if (zr_disk_raw_size(&GEOMS[i]) == size) out[n++] = GEOMS[i];
    return n;
}

int zr_disk_size_code(unsigned bytes) {
    for (unsigned n = 0; n < 8; n++)
        if (zr_disk_nominal(n) == bytes) return (int)n;
    return -1;
}

static int geom_valid(const zr_disk_geom *g) {
    return g->cylinders >= 1 && g->cylinders <= ZR_DISK_MAX_CYL && g->sides >= 1 && g->sides <= 2 && g->sectors >= 1 &&
           zr_disk_size_code(g->sector_size) >= 0 && (g->encoding == ZR_ENC_FM || g->encoding == ZR_ENC_MFM);
}

unsigned zr_disk_default_gap3(unsigned n, unsigned count, int fm) {
    /* The track's room (in MFM byte units: an FM byte is two) less the index area and the sectors, shared out, with a tenth kept
     * back so that a slow drive does not run the track into itself; at most 255, as the track header's byte. */
    const unsigned unit = fm ? 2u : 1u;
    const unsigned room = 6250u, pre = fm ? 73u * 2u : 146u;
    const unsigned field = fm ? (13u + 11u + 7u + 2u) * 2u : 22u + 22u + 16u + 2u;
    if (!count) return 0;
    const unsigned used = pre + count * (field + zr_disk_nominal(n) * unit);
    if (used >= room) return 1;
    unsigned g = (room - used) / count * 9u / 10u / unit;
    if (g < 1) g = 1;
    return g > 255u ? 255u : g;
}

/* --- tracks and sectors --------------------------------------------------------------------- */

void zr_disk_clear_track(zr_track *t) {
    if (t->sector) {
        for (unsigned i = 0; i < t->count; i++) free(t->sector[i].data);
        free(t->sector);
    }
    t->sector = NULL;
    t->count = 0;
}

int zr_disk_alloc_track(zr_track *t, unsigned count) {
    zr_disk_clear_track(t);
    if (!count) return 1;
    t->sector = calloc(count, sizeof *t->sector);
    if (!t->sector) return 0;
    t->count = (uint16_t)count;
    return 1;
}

void zr_disk_set_stored(zr_sector *s, uint32_t stored) {
    const uint32_t nominal = zr_disk_nominal(s->n);
    s->stored = stored;
    s->copies = 1;
    if (stored < nominal) s->size = stored;
    else {
        s->size = nominal;
        if (stored % nominal == 0) {                    /* whole copies: a weak sector */
            const uint32_t k = stored / nominal;
            s->copies = (uint8_t)(k > 255u ? 255u : k);
        }
    }
}

const zr_track *zr_disk_track(const zr_disk *d, unsigned cyl, unsigned head) {
    if (cyl >= ZR_DISK_MAX_CYL || head > 1) return NULL;
    const zr_track *t = &d->track[cyl][head];
    return t->count ? t : NULL;
}

void zr_disk_read_sector(const zr_sector *s, unsigned copy, uint8_t *out, uint32_t length, uint8_t filler) {
    uint32_t have = 0;
    if (s->data && s->size) {
        copy = s->copies > 1 ? copy % s->copies : 0;
        have = s->size < length ? s->size : length;
        memcpy(out, s->data + (size_t)copy * s->size, have);
    }
    if (have < length) memset(out + have, filler, length - have);
}

/* --- protection ------------------------------------------------------------------------------- */

int zr_disk_write_protected(const zr_disk *d) { return d->readonly || d->protect; }

int zr_disk_set_protect(zr_disk *d, int protect) {
    if (!protect && d->readonly) return 0;
    d->protect = protect != 0;
    return 1;
}

/* --- writing sectors and tracks ----------------------------------------------------------------- */

int zr_disk_write_sector(zr_disk *d, unsigned cyl, unsigned head, unsigned index, const uint8_t *data, uint32_t length, int deleted) {
    if (zr_disk_write_protected(d)) return ZR_DISK_PROTECTED;
    if (cyl >= ZR_DISK_MAX_CYL || head > 1) return ZR_DISK_NO_SECTOR;
    zr_track *t = &d->track[cyl][head];
    if (index >= t->count) return ZR_DISK_NO_SECTOR;
    zr_sector *s = &t->sector[index];
    const uint32_t nominal = zr_disk_nominal(s->n);
    uint8_t *buf = calloc(nominal, 1);
    if (!buf) return ZR_DISK_NO_MEMORY;
    memcpy(buf, data, length < nominal ? length : nominal);
    free(s->data);
    s->data = buf;
    s->size = s->stored = nominal;
    s->copies = 1;
    /* The data field is written afresh: its CRC and address mark are good, the ID's are not touched. */
    if (s->st2 & ZR_DISK_ST2_DD) s->st1 = (uint8_t)(s->st1 & ~ZR_DISK_ST1_DE);
    s->st2 = (uint8_t)(s->st2 & ~(ZR_DISK_ST2_DD | ZR_DISK_ST2_MD | ZR_DISK_ST2_CM));
    if (deleted) s->st2 |= ZR_DISK_ST2_CM;
    d->dirty = 1;
    return ZR_DISK_OK;
}

/* The bytes of sector data the disk holds. */
static uint64_t disk_bytes(const zr_disk *d) {
    uint64_t total = 0;
    for (unsigned c = 0; c < ZR_DISK_MAX_CYL; c++)
        for (unsigned h = 0; h < 2; h++)
            for (unsigned k = 0; k < d->track[c][h].count; k++) total += d->track[c][h].sector[k].stored;
    return total;
}

int zr_disk_format_track(zr_disk *d, unsigned cyl, unsigned head, const zr_disk_format *f) {
    if (zr_disk_write_protected(d)) return ZR_DISK_PROTECTED;
    if (cyl >= ZR_DISK_MAX_CYL || head > 1 || f->count > ZR_DISK_MAX_SECTORS || f->n > 7 || (f->count && !f->id))
        return ZR_DISK_BAD_FORMAT;
    if (d->format == ZR_DISK_RAW && (cyl >= d->cylinders || head >= d->sides)) return ZR_DISK_BAD_FORMAT;
    zr_track *t = &d->track[cyl][head];
    /* A guest may format as it likes, but not into more than a media can be: the disk is kept under the size limit. */
    uint64_t old_bytes = 0;
    for (unsigned k = 0; k < t->count; k++) old_bytes += t->sector[k].stored;
    if (disk_bytes(d) - old_bytes + (uint64_t)f->count * zr_disk_nominal(f->n) > ZR_MEDIA_LIMIT) return ZR_DISK_NO_MEMORY;
    zr_track fresh;
    memset(&fresh, 0, sizeof fresh);
    if (!zr_disk_alloc_track(&fresh, f->count)) return ZR_DISK_NO_MEMORY;
    const uint32_t size = zr_disk_nominal(f->n);
    for (unsigned i = 0; i < f->count; i++) {
        zr_sector *s = &fresh.sector[i];
        s->c = f->id[i].c; s->h = f->id[i].h; s->r = f->id[i].r; s->n = f->id[i].n;
        s->data = malloc(size);
        if (!s->data) { zr_disk_clear_track(&fresh); return ZR_DISK_NO_MEMORY; }
        memset(s->data, f->filler, size);
        s->size = s->stored = size;
        s->copies = 1;
    }
    zr_disk_clear_track(t);
    t->sector = fresh.sector;
    t->count = fresh.count;
    t->encoding = f->encoding;
    t->rate = f->rate;
    t->n = f->n;
    t->gap3 = f->gap3;
    t->filler = f->filler;
    if (f->count) {
        if (cyl >= d->cylinders) d->cylinders = (uint8_t)(cyl + 1u);
        if (head >= d->sides) d->sides = (uint8_t)(head + 1u);
    }
    d->dirty = 1;
    return ZR_DISK_OK;
}

/* --- making and closing --------------------------------------------------------------------------- */

zr_disk *zr_disk_new(const zr_disk_geom *g, int format, int formatted, uint8_t filler) {
    if (!geom_valid(g) || (format != ZR_DISK_RAW && format != ZR_DISK_DSK && format != ZR_DISK_EDSK)) return NULL;
    zr_disk *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->format = format;
    d->cylinders = g->cylinders;
    d->sides = g->sides;
    d->geom = *g;
    snprintf(d->creator, sizeof d->creator, "ZetRo");
    const int n = zr_disk_size_code(g->sector_size);
    if (format == ZR_DISK_DSK) {
        d->std_track_size = 256u + (uint32_t)g->sectors * g->sector_size;
        if (g->sectors > 29 || d->std_track_size > 0xFFFFu) { free(d); return NULL; }
    }
    for (unsigned c = 0; c < g->cylinders; c++)
        for (unsigned h = 0; h < g->sides; h++) {
            zr_track *t = &d->track[c][h];
            t->present = 1;
            t->encoding = g->encoding;
            t->rate = ZR_RATE_DD;
            t->n = (uint8_t)n;
            t->gap3 = (uint8_t)zr_disk_default_gap3((unsigned)n, g->sectors, g->encoding == ZR_ENC_FM);
            t->filler = filler;
            if (!formatted) continue;
            if (!zr_disk_alloc_track(t, g->sectors)) { zr_disk_close(d); return NULL; }
            for (unsigned i = 0; i < g->sectors; i++) {
                zr_sector *s = &t->sector[i];
                s->c = (uint8_t)c; s->h = (uint8_t)h; s->r = (uint8_t)(i + 1u); s->n = (uint8_t)n;
                s->data = malloc(g->sector_size);
                if (!s->data) { zr_disk_close(d); return NULL; }
                memset(s->data, filler, g->sector_size);
                s->size = s->stored = g->sector_size;
                s->copies = 1;
            }
        }
    return d;
}

void zr_disk_close(zr_disk *d) {
    if (!d) return;
    for (unsigned c = 0; c < ZR_DISK_MAX_CYL; c++)
        for (unsigned h = 0; h < 2; h++) zr_disk_clear_track(&d->track[c][h]);
    free(d);
}

/* --- opening -------------------------------------------------------------------------------------- */

/* Other image formats, by their first bytes: named in the refusal. */
static const char *other_format(const uint8_t *p, size_t n) {
    if (n >= 4 && !memcmp(p, "IMD ", 4)) return "ImageDisk (IMD)";
    if (n >= 2 && (!memcmp(p, "TD", 2) || !memcmp(p, "td", 2))) return "Teledisk (TD0)";
    if (n >= 8 && !memcmp(p, "HxCPICFE", 8)) return "HxC floppy emulator (HFE)";
    if (n >= 4 && !memcmp(p, "86BF", 4)) return "86Box (86F)";
    if (n >= 4 && !memcmp(p, "SCP", 3)) return "SuperCard Pro (SCP)";
    return NULL;
}

zr_disk *zr_disk_open(const zr_media *media, const zr_disk_geom *hint, char *error, size_t capacity) {
    const uint8_t *p = media->data;
    const size_t n = media->size;
    zr_disk *d = calloc(1, sizeof *d);
    if (!d) { (void)ZR_DISK_FAIL("Out of memory"); return NULL; }
    d->readonly = !media->writable;
    d->protect = d->readonly;
    snprintf(d->creator, sizeof d->creator, "ZetRo");
    int ok;
    if (n >= 8 && !memcmp(p, "EXTENDED", 8)) {
        d->format = ZR_DISK_EDSK;
        ok = zr_disk_read_dsk(d, p, n, 1, error, capacity);
    } else if (n >= 8 && !memcmp(p, "MV - CPC", 8)) {
        d->format = ZR_DISK_DSK;
        ok = zr_disk_read_dsk(d, p, n, 0, error, capacity);
    } else {
        zr_disk_geom list[8];
        const unsigned k = zr_disk_geometries(n, list, 8);
        if (!k) {
            const char *other = other_format(p, n);
            if (other) (void)ZR_DISK_FAIL("This is a %s disk image: only DSK, Extended DSK and raw sector images are read", other);
            else (void)ZR_DISK_FAIL("The file is not a disk image this program reads: it is not a DSK, and its size, %lu bytes, is not that of a raw floppy image",
                              (unsigned long)n);
            zr_disk_close(d);
            return NULL;
        }
        zr_disk_geom g = list[0];
        if (hint)
            for (unsigned i = 0; i < k; i++)
                if (list[i].cylinders == hint->cylinders && list[i].sides == hint->sides && list[i].sectors == hint->sectors &&
                    list[i].sector_size == hint->sector_size) g = list[i];
        d->format = ZR_DISK_RAW;
        ok = zr_disk_read_raw(d, p, n, &g, error, capacity);
    }
    if (!ok) { zr_disk_close(d); return NULL; }
    return d;
}

/* --- describing ------------------------------------------------------------------------------------ */

void zr_disk_describe(const zr_disk *d, char *out, size_t capacity) {
    static const char *const NAME[] = {"", "Raw image", "DSK", "EDSK"};
    unsigned count = 0, size = 0, uniform = 1, tracks = 0;
    int fm = 0;
    for (unsigned c = 0; c < d->cylinders; c++)
        for (unsigned h = 0; h < d->sides; h++) {
            const zr_track *t = &d->track[c][h];
            if (!t->count) continue;
            tracks++;
            if (t->encoding == ZR_ENC_FM) fm = 1;
            const unsigned sz = t->sector[0].size;
            if (tracks == 1) { count = t->count; size = sz; }
            else if (count != t->count || size != sz) uniform = 0;
        }
    if (!tracks) {
        snprintf(out, capacity, "%s, %u tracks, %u side%s, not formatted", NAME[d->format], d->cylinders, d->sides, d->sides == 1 ? "" : "s");
    } else if (uniform) {
        snprintf(out, capacity, "%s, %u tracks, %u side%s, %u sectors of %u bytes%s", NAME[d->format], d->cylinders, d->sides,
                 d->sides == 1 ? "" : "s", count, size, fm ? ", FM" : "");
    } else {
        snprintf(out, capacity, "%s, %u tracks, %u side%s, mixed layout%s", NAME[d->format], d->cylinders, d->sides,
                 d->sides == 1 ? "" : "s", fm ? ", FM" : "");
    }
}

/* --- saving ----------------------------------------------------------------------------------------- */

int zr_disk_check_save(const zr_disk *d, char *error, size_t capacity) {
    if (d->format == ZR_DISK_RAW) return zr_disk_raw_file_size(d, error, capacity) != 0;
    if (d->format == ZR_DISK_DSK && zr_disk_dsk_standard_ok(d)) return zr_disk_dsk_size(d, 0, error, capacity) != 0;
    return zr_disk_dsk_size(d, 1, error, capacity) != 0;
}

int zr_disk_save_media(zr_disk *d, zr_media *media, char *error, size_t capacity) {
    int format = d->format;
    int extended = 0;
    size_t size;
    if (format == ZR_DISK_RAW) size = zr_disk_raw_file_size(d, error, capacity);
    else {
        extended = !(format == ZR_DISK_DSK && zr_disk_dsk_standard_ok(d));
        size = zr_disk_dsk_size(d, extended, error, capacity);
    }
    if (!size) return 0;
    uint8_t *out = calloc(size, 1);
    if (!out) return ZR_DISK_FAIL("Out of memory");
    if (format == ZR_DISK_RAW) zr_disk_raw_write(d, out);
    else zr_disk_dsk_write(d, extended, out);
    if (!zr_media_resize(media, size)) { free(out); return ZR_DISK_FAIL("Out of memory"); }
    memcpy(media->data, out, size);
    free(out);
    media->dirty = 1;
    if (format != ZR_DISK_RAW) d->format = extended ? ZR_DISK_EDSK : ZR_DISK_DSK;
    d->dirty = 0;
    return 1;
}
