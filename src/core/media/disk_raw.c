/* Raw sector images (disk.h): the sectors one after the other, cylinder by
 * cylinder, head 0 then head 1, sectors 1..n, in a geometry that the size
 * names. ZetRo's own code. */
#include "disk_impl.h"

#include <stdlib.h>
#include <string.h>

static int all_zero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return 0;
    return 1;
}

int zr_disk_read_raw(zr_disk *d, const uint8_t *p, size_t n, const zr_disk_geom *g, char *error, size_t capacity) {
    if (zr_disk_raw_size(g) != n) return ZR_DISK_FAIL("The image is not the size of a %u x %u x %u x %u disk", g->cylinders, g->sides, g->sectors, g->sector_size);
    const int sizecode = zr_disk_size_code(g->sector_size);
    const int blank = all_zero(p, n);
    d->geom = *g;
    d->cylinders = g->cylinders;
    d->sides = g->sides;
    for (unsigned c = 0; c < g->cylinders; c++)
        for (unsigned h = 0; h < g->sides; h++) {
            zr_track *t = &d->track[c][h];
            t->present = 1;
            t->encoding = g->encoding;
            t->rate = ZR_RATE_UNKNOWN;
            t->n = (uint8_t)sizecode;
            t->gap3 = 0;
            t->filler = ZR_DISK_STD_SIZE_FILL;
            if (blank) continue;
            if (!zr_disk_alloc_track(t, g->sectors)) return ZR_DISK_FAIL("Out of memory");
            for (unsigned k = 0; k < g->sectors; k++) {
                zr_sector *s = &t->sector[k];
                s->c = (uint8_t)c; s->h = (uint8_t)h; s->r = (uint8_t)(k + 1u); s->n = (uint8_t)sizecode;
                s->data = malloc(g->sector_size);
                if (!s->data) return ZR_DISK_FAIL("Out of memory");
                memcpy(s->data, p + (((size_t)c * g->sides + h) * g->sectors + k) * g->sector_size, g->sector_size);
                s->size = s->stored = g->sector_size;
                s->copies = 1;
            }
        }
    return 1;
}

size_t zr_disk_raw_file_size(const zr_disk *d, char *error, size_t capacity) {
    const zr_disk_geom *g = &d->geom;
    const int sizecode = zr_disk_size_code(g->sector_size);
    if (d->cylinders != g->cylinders || d->sides != g->sides)
        return (size_t)ZR_DISK_FAIL("The disk no longer has the tracks of a raw image: save it as a DSK instead");
    for (unsigned c = 0; c < g->cylinders; c++)
        for (unsigned h = 0; h < g->sides; h++) {
            const zr_track *t = &d->track[c][h];
            if (!t->count) continue;
            if (t->count != g->sectors)
                return (size_t)ZR_DISK_FAIL("Track %u side %u has %u sectors, a raw image of this size has %u: save it as a DSK instead", c, h, t->count, g->sectors);
            uint8_t seen[256];
            memset(seen, 0, sizeof seen);
            for (unsigned k = 0; k < t->count; k++) {
                const zr_sector *s = &t->sector[k];
                if (s->n != sizecode || s->c != c || s->h != h || s->r < 1 || s->r > g->sectors || seen[s->r] || s->size > g->sector_size)
                    return (size_t)ZR_DISK_FAIL("Track %u side %u no longer has the sectors of a raw image: save it as a DSK instead", c, h);
                seen[s->r] = 1;
            }
        }
    return zr_disk_raw_size(g);
}

void zr_disk_raw_write(const zr_disk *d, uint8_t *out) {
    const zr_disk_geom *g = &d->geom;
    for (unsigned c = 0; c < g->cylinders; c++)
        for (unsigned h = 0; h < g->sides; h++) {
            const zr_track *t = &d->track[c][h];
            for (unsigned k = 0; k < t->count; k++) {
                const zr_sector *s = &t->sector[k];
                zr_disk_read_sector(s, 0, out + (((size_t)c * g->sides + h) * g->sectors + (s->r - 1u)) * g->sector_size, g->sector_size, t->filler);
            }
        }
}
