/* The DSK and Extended DSK formats (disk.h). A file is a 256-byte disk
 * information block, then one block for each track, each starting with a
 * 256-byte track information block (more for over 29 sectors) followed by
 * the sectors' data. The standard format has one track size for all tracks;
 * the extended one a table of sizes (in 256-byte units, 0 for a track that
 * is not there) and the actual length of each sector's data, which may be
 * short, long (copies of a weak sector) or even carry the CRC and gap bytes.
 * Written from the format descriptions; ZetRo's own code. */
#include "disk_impl.h"

#include <stdlib.h>
#include <string.h>

#define INFO_SIZE 0x100u
#define EXT_TABLE 0x34u
#define EXT_TABLE_ENTRIES (INFO_SIZE - EXT_TABLE)       /* 204 */

static const char STD_MAGIC[] = "MV - CPCEMU Disk-File\r\nDisk-Info\r\n";
static const char EXT_MAGIC[] = "EXTENDED CPC DSK File\r\nDisk-Info\r\n";

/* --- reading ---------------------------------------------------------------------------------------- */

static int parse_track(zr_track *t, const uint8_t *b, uint32_t blk, unsigned cyl, unsigned head, int extended, char *error, size_t capacity) {
    if (blk < 0x18u || memcmp(b, "Track-Info", 10)) return ZR_DISK_FAIL("Damaged DSK file: track %u side %u has no Track-Info block", cyl, head);
    const unsigned count = b[0x15];
    const uint32_t hdr = zr_disk_track_header(count);
    if (hdr > blk) return ZR_DISK_FAIL("Damaged DSK file: the sector list of track %u side %u runs past its block", cyl, head);
    t->present = 1;
    t->rate = b[0x12] <= ZR_RATE_ED ? b[0x12] : ZR_RATE_UNKNOWN;
    t->encoding = b[0x13] <= ZR_ENC_MFM ? b[0x13] : ZR_ENC_UNKNOWN;
    t->n = b[0x14];
    t->gap3 = b[0x16];
    t->filler = b[0x17];
    if (!count) return 1;
    if (!zr_disk_alloc_track(t, count)) return ZR_DISK_FAIL("Out of memory");

    const uint32_t avail = blk - hdr;
    uint32_t stored[ZR_DISK_MAX_SECTORS];
    uint64_t sum_len = 0, sum_nominal = 0;
    unsigned zero_len = 0;
    for (unsigned k = 0; k < count; k++) {
        const uint8_t *e = b + 0x18 + 8u * k;
        zr_sector *s = &t->sector[k];
        s->c = e[0]; s->h = e[1]; s->r = e[2]; s->n = e[3]; s->st1 = e[4]; s->st2 = e[5];
        stored[k] = zr_disk_rd16(e + 6);
        if (!stored[k]) zero_len++;
        sum_len += stored[k];
        sum_nominal += zr_disk_nominal(s->n);
    }
    if (extended) {
        if (sum_len > avail) return ZR_DISK_FAIL("Damaged DSK file: the sectors of track %u side %u are longer than its block", cyl, head);
    } else if (!zero_len && sum_len <= avail) {
        /* the standard format leaves the length unused, but a file that fills it in is believed */
    } else {
        const uint32_t uniform = zr_disk_nominal(t->n);
        if ((uint64_t)count * uniform <= avail) for (unsigned k = 0; k < count; k++) stored[k] = uniform;
        else if (sum_nominal <= avail) for (unsigned k = 0; k < count; k++) stored[k] = zr_disk_nominal(t->sector[k].n);
        else return ZR_DISK_FAIL("Damaged DSK file: the sectors of track %u side %u are longer than its block", cyl, head);
    }
    size_t at = hdr;
    for (unsigned k = 0; k < count; k++) {
        zr_sector *s = &t->sector[k];
        if (stored[k]) {
            s->data = malloc(stored[k]);
            if (!s->data) return ZR_DISK_FAIL("Out of memory");
            memcpy(s->data, b + at, stored[k]);
        }
        zr_disk_set_stored(s, stored[k]);
        at += stored[k];
    }
    return 1;
}

int zr_disk_read_dsk(zr_disk *d, const uint8_t *p, size_t n, int extended, char *error, size_t capacity) {
    if (n < INFO_SIZE) return ZR_DISK_FAIL("Damaged DSK file: the disk information block is cut short");
    const unsigned cyls = p[0x30], sides = p[0x31];
    if (!cyls || cyls > ZR_DISK_MAX_CYL || sides < 1 || sides > 2)
        return ZR_DISK_FAIL("The DSK file has %u tracks and %u sides: this program reads 1 to %u tracks and 1 or 2 sides", cyls, sides, ZR_DISK_MAX_CYL);
    d->cylinders = (uint8_t)cyls;
    d->sides = (uint8_t)sides;
    d->geom.cylinders = d->cylinders;
    d->geom.sides = d->sides;
    d->creator[0] = 0;                          /* the file's own creator, empty or not, goes back as it was */
    for (unsigned i = 0; i < 14 && p[0x22 + i]; i++) d->creator[i] = p[0x22 + i] >= 32 && p[0x22 + i] < 127 ? (char)p[0x22 + i] : '?';
    uint32_t tsize = 0;
    if (!extended) {
        tsize = zr_disk_rd16(p + 0x32);
        if (tsize < 0x100u) return ZR_DISK_FAIL("Damaged DSK file: the track size is %u", tsize);
        d->std_track_size = tsize;
    } else if (cyls * sides > EXT_TABLE_ENTRIES) {
        return ZR_DISK_FAIL("Damaged DSK file: %u tracks of %u sides do not fit the track size table", cyls, sides);
    }
    size_t off = INFO_SIZE;
    for (unsigned c = 0; c < cyls; c++)
        for (unsigned h = 0; h < sides; h++) {
            const uint32_t blk = extended ? (uint32_t)p[EXT_TABLE + c * sides + h] * 256u : tsize;
            if (!blk) continue;
            if (off > n || blk > n - off) return ZR_DISK_FAIL("Damaged DSK file: the file ends before track %u side %u does", c, h);
            if (!parse_track(&d->track[c][h], p + off, blk, c, h, extended, error, capacity)) return 0;
            off += blk;
        }
    return 1;
}

/* --- writing ---------------------------------------------------------------------------------------- */

static uint32_t track_data(const zr_track *t) {
    uint32_t sum = 0;
    for (unsigned k = 0; k < t->count; k++) sum += t->sector[k].stored;
    return sum;
}

int zr_disk_dsk_standard_ok(const zr_disk *d) {
    if (d->format != ZR_DISK_DSK) return 0;
    for (unsigned c = 0; c < d->cylinders; c++)
        for (unsigned h = 0; h < d->sides; h++) {
            const zr_track *t = &d->track[c][h];
            if (!t->present) return 0;
            if (!t->count) continue;
            if (t->count > 29) return 0;
            const uint32_t uniform = zr_disk_nominal(t->n);
            for (unsigned k = 0; k < t->count; k++) {
                const zr_sector *s = &t->sector[k];
                if (s->copies != 1 || s->stored != uniform || s->size != uniform) return 0;
            }
            if (256u + track_data(t) > d->std_track_size) return 0;
        }
    return 1;
}

static uint32_t ext_block(const zr_track *t) {
    if (!t->count) return 0;
    return (zr_disk_track_header(t->count) + track_data(t) + 255u) & ~255u;
}

size_t zr_disk_dsk_size(const zr_disk *d, int extended, char *error, size_t capacity) {
    if (!extended) return INFO_SIZE + (size_t)d->cylinders * d->sides * d->std_track_size;
    if ((unsigned)d->cylinders * d->sides > EXT_TABLE_ENTRIES) return (size_t)ZR_DISK_FAIL("The disk has too many tracks for the extended format");
    size_t total = INFO_SIZE;
    for (unsigned c = 0; c < d->cylinders; c++)
        for (unsigned h = 0; h < d->sides; h++) {
            const zr_track *t = &d->track[c][h];
            const uint32_t blk = ext_block(t);
            if (blk > ZR_DISK_MAX_TRACK_BLOCK) return (size_t)ZR_DISK_FAIL("Track %u side %u (%u bytes) is too large for the extended DSK format", c, h, blk);
            for (unsigned k = 0; k < t->count; k++)
                if (t->sector[k].stored > ZR_DISK_MAX_STORED) return (size_t)ZR_DISK_FAIL("A sector of track %u side %u is too long for the extended DSK format", c, h);
            total += blk;
        }
    return total;
}

/* `lengths`: the sector information list carries each sector's data length (the extended format's; the standard
 * format leaves the bytes unused and they are written as zero, as the files of the tools that make it have them). */
static void put_track(const zr_track *t, unsigned c, unsigned h, uint8_t *out, int lengths) {
    memcpy(out, "Track-Info\r\n", 12);
    out[0x10] = (uint8_t)c;
    out[0x11] = (uint8_t)h;
    out[0x12] = t->rate;
    out[0x13] = t->encoding;
    out[0x14] = t->n;
    out[0x15] = (uint8_t)t->count;
    out[0x16] = t->gap3;
    out[0x17] = t->filler;
    size_t at = zr_disk_track_header(t->count);
    for (unsigned k = 0; k < t->count; k++) {
        const zr_sector *s = &t->sector[k];
        uint8_t *e = out + 0x18 + 8u * k;
        e[0] = s->c; e[1] = s->h; e[2] = s->r; e[3] = s->n; e[4] = s->st1; e[5] = s->st2;
        if (lengths) {
            e[6] = (uint8_t)s->stored;
            e[7] = (uint8_t)(s->stored >> 8);
        }
        if (s->stored) memcpy(out + at, s->data, s->stored);
        at += s->stored;
    }
}

void zr_disk_dsk_write(const zr_disk *d, int extended, uint8_t *out) {
    memcpy(out, extended ? EXT_MAGIC : STD_MAGIC, 34);
    memcpy(out + 0x22, d->creator, strlen(d->creator) < 14 ? strlen(d->creator) : 14);
    out[0x30] = d->cylinders;
    out[0x31] = d->sides;
    if (!extended) {
        out[0x32] = (uint8_t)d->std_track_size;
        out[0x33] = (uint8_t)(d->std_track_size >> 8);
    }
    size_t off = INFO_SIZE;
    for (unsigned c = 0; c < d->cylinders; c++)
        for (unsigned h = 0; h < d->sides; h++) {
            const zr_track *t = &d->track[c][h];
            if (extended) {
                const uint32_t blk = ext_block(t);
                out[EXT_TABLE + c * d->sides + h] = (uint8_t)(blk / 256u);
                if (blk) put_track(t, c, h, out + off, 1);
                off += blk;
            } else {
                put_track(t, c, h, out + off, 0);
                off += d->std_track_size;
            }
        }
}
