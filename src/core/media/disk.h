/* Floppy disks: a disk as tracks of sectors, each sector
 * with its ID (C, H, R, N), its data, the error bits (ST1 and ST2) recorded
 * with it and, for a weak sector, its copies; each track with its recording
 * (FM or MFM) and data rate. Read from DSK and Extended DSK files (found by
 * their signatures) and from raw sector images (found by their size); written
 * back in the format the disk came in. ZetRo's own code, written from the
 * descriptions of the DSK ("MV - CPCEMU Disk-File") and Extended DSK
 * ("EXTENDED CPC DSK File") formats with their documented extensions (the
 * data rate and recording mode bytes of the track header, actual data
 * lengths, 8K and 16K sectors, several copies of a weak sector, more than 29
 * sectors in a track).
 *
 * A disk is a copy: it keeps nothing of the zr_media it was read from, so the
 * media may change or go. Write protection follows the media: a media that is
 * not writable gives a disk that cannot be unprotected; a writable one has a
 * tab (zr_disk_set_protect). Sector writes and track formats change the disk
 * in memory and mark it dirty; zr_disk_save_media serialises it into a media
 * (and marks that dirty, for the shell to write the file back).
 *
 * What the sector fields mean (ST1 and ST2 are the FDC's status registers as
 * the sector gave them when the image was made): ST2 bit 6 a deleted data
 * mark, ST2 bit 5 (with ST1 bit 5) a CRC error in the data field, ST1 bit 5
 * without ST2 bit 5 a CRC error in the ID field, ST2 bit 0 (and ST1 bit 0)
 * no data address mark. `size` is the length of one copy of the data as
 * recorded, normally the nominal 128 << N; a shorter sector is read padded
 * with the track's filler byte. A sector made of several copies (a weak
 * sector) holds `copies` of `size` bytes one after the other.
 *
 * Raw images are the sectors one after the other, cylinder by cylinder, head 0
 * then head 1, sectors 1..n. Their geometry comes from the size (a table:
 * zr_disk_geometries lists every geometry a size can mean; when there are
 * several the caller may pass the one it wants as a hint). An image of all
 * zero bytes is a blank disk: its tracks have no sectors until formatted.
 *
 * Never trusts a file: every length and offset is checked, a damaged image
 * gives an error message and no disk. */
#ifndef ZR_DISK_H
#define ZR_DISK_H

#include <stddef.h>
#include <stdint.h>

#include "media.h"

#define ZR_DISK_MAX_CYL 100                 /* the most cylinders a disk has (two sides fill the extended format's size table) */
#define ZR_DISK_MAX_SECTORS 255             /* in a track */

/* The format a disk was read from, and is written as. */
enum { ZR_DISK_NONE, ZR_DISK_RAW, ZR_DISK_DSK, ZR_DISK_EDSK };
/* A track's recording (the extended format's recording mode byte). */
enum { ZR_ENC_UNKNOWN, ZR_ENC_FM, ZR_ENC_MFM };
/* A track's data rate (the extended format's data rate byte). */
enum { ZR_RATE_UNKNOWN, ZR_RATE_DD, ZR_RATE_HD, ZR_RATE_ED };   /* 250 or 300, 500, 1000 kbit/s */

/* Results of the sector operations. */
enum { ZR_DISK_OK, ZR_DISK_PROTECTED, ZR_DISK_NO_SECTOR, ZR_DISK_BAD_FORMAT, ZR_DISK_NO_MEMORY };

/* The sector's nominal data length for a size code N. */
static inline uint32_t zr_disk_nominal(unsigned n) { return 128u << (n & 7u); }

/* ST1 and ST2 bits the sector records. */
enum {
    ZR_DISK_ST1_MA = 0x01, ZR_DISK_ST1_DE = 0x20,
    ZR_DISK_ST2_MD = 0x01, ZR_DISK_ST2_DD = 0x20, ZR_DISK_ST2_CM = 0x40
};

typedef struct zr_sector {
    uint8_t c, h, r, n;                     /* the ID */
    uint8_t st1, st2;                       /* the error bits recorded with it */
    uint8_t copies;                         /* 1, or more for a weak sector */
    uint32_t size;                          /* bytes of one copy */
    uint32_t stored;                        /* bytes in data: copies * size, or more (the CRC and gap bytes some images keep) */
    uint8_t *data;                          /* NULL when stored is 0 */
} zr_sector;

typedef struct zr_track {
    uint8_t present;                        /* the file had a block for the track (the standard format's writer needs it) */
    uint8_t encoding;                       /* ZR_ENC_* */
    uint8_t rate;                           /* ZR_RATE_* */
    uint8_t n;                              /* the size code the track was formatted with */
    uint8_t gap3, filler;                   /* gap 3 length (0: unknown); the filler byte of the track */
    uint16_t count;                         /* sectors, in the order they pass the head */
    zr_sector *sector;
} zr_track;

/* A fixed geometry: what a raw image's size means. */
typedef struct zr_disk_geom {
    uint8_t cylinders, sides, sectors;
    uint16_t sector_size;                   /* bytes: 128, 256, 512, 1024 ... */
    uint8_t encoding;                       /* ZR_ENC_FM or ZR_ENC_MFM */
} zr_disk_geom;

typedef struct zr_disk {
    int format;                             /* ZR_DISK_* it was read from */
    uint8_t cylinders, sides;
    uint8_t readonly;                       /* the media could not be written: the tab cannot be opened */
    uint8_t protect;                        /* the tab: closed refuses writes */
    uint8_t dirty;                          /* changed since it was read or last saved */
    zr_disk_geom geom;                      /* raw: the fixed geometry */
    uint32_t std_track_size;                /* DSK: the track block size the file had */
    char creator[16];                       /* DSK and EDSK: the creator field of the disk information block */
    zr_track track[ZR_DISK_MAX_CYL][2];
} zr_disk;

/* One track's formatting, as a FORMAT TRACK command gives it: the IDs in the order they are written. */
typedef struct zr_disk_id { uint8_t c, h, r, n; } zr_disk_id;
typedef struct zr_disk_format {
    uint8_t encoding, rate;                 /* ZR_ENC_*, ZR_RATE_* */
    uint8_t n;                              /* the sector size code of the data fields */
    uint8_t gap3, filler;
    unsigned count;
    const zr_disk_id *id;                   /* count entries */
} zr_disk_format;

/* --- opening, making, closing ------------------------------------------------------------ */

/* Builds a disk from a media's bytes, recognising the format by content: the DSK and Extended DSK
 * signatures, else a raw image by size. `hint` (optional) is the geometry wanted when a size fits
 * several. NULL with the reason for a file that is not a floppy image or is damaged. */
zr_disk *zr_disk_open(const zr_media *media, const zr_disk_geom *hint, char *error, size_t capacity);
/* A new disk of a geometry, as ZR_DISK_RAW, ZR_DISK_DSK or ZR_DISK_EDSK. `formatted` 0: the tracks
 * have no sectors until formatted; 1: every sector is there, the data fields filled with `filler`.
 * NULL on a geometry that does not fit the format or when out of memory. */
zr_disk *zr_disk_new(const zr_disk_geom *geom, int format, int formatted, uint8_t filler);
void zr_disk_close(zr_disk *disk);

/* The raw geometries a size can mean, best guess first (at most `max`); returns how many. */
unsigned zr_disk_geometries(size_t size, zr_disk_geom *out, unsigned max);
/* The size in bytes of a raw image of a geometry. */
size_t zr_disk_raw_size(const zr_disk_geom *geom);
/* The gap 3 length a freshly formatted track of `count` sectors of size code n gets. */
unsigned zr_disk_default_gap3(unsigned n, unsigned count, int fm);

/* --- looking ---------------------------------------------------------------------------- */

/* The track, or NULL when it has no sectors (unformatted, or off the disk). */
const zr_track *zr_disk_track(const zr_disk *disk, unsigned cyl, unsigned head);
/* "EDSK, 80 tracks, 2 sides, 9 sectors of 512 bytes" for the setup screen. */
void zr_disk_describe(const zr_disk *disk, char *out, size_t capacity);
int zr_disk_write_protected(const zr_disk *disk);
/* Closes or opens the tab; 0 when the media is read-only and the tab cannot be opened. */
int zr_disk_set_protect(zr_disk *disk, int protect);
/* The data of one copy of a sector, `length` bytes: what the controller reads. A recorded length
 * shorter than `length` is padded with `filler`. `copy` is taken modulo the copies. */
void zr_disk_read_sector(const zr_sector *sector, unsigned copy, uint8_t *out, uint32_t length, uint8_t filler);

/* --- writing ---------------------------------------------------------------------------- */

/* Writes `length` bytes into sector `index` (its position in the track) of a track, with or without
 * a deleted data mark: the sector keeps its size, the rest of it is zero-filled, a weak sector
 * becomes one copy and the error bits of the data field are cleared. ZR_DISK_OK, _PROTECTED or
 * _NO_SECTOR. */
int zr_disk_write_sector(zr_disk *disk, unsigned cyl, unsigned head, unsigned index, const uint8_t *data, uint32_t length, int deleted);
/* Formats a track: it becomes the sectors of `format`, the data fields filled with its filler. A
 * cylinder beyond the disk's (up to ZR_DISK_MAX_CYL) makes the disk longer, except for a raw image.
 * ZR_DISK_OK, _PROTECTED, _BAD_FORMAT (head, cylinder or counts out of range, a raw image's cylinder
 * count) or _NO_MEMORY. A raw image takes any layout in memory but can be saved only if it still has
 * its geometry (zr_disk_check_save). */
int zr_disk_format_track(zr_disk *disk, unsigned cyl, unsigned head, const zr_disk_format *format);

/* 1 when the disk can be written back in the format it came in; else 0 with the reason (a raw image
 * that no longer has its geometry). */
int zr_disk_check_save(const zr_disk *disk, char *error, size_t capacity);
/* Serialises the disk into a media (its bytes are replaced) and marks the media dirty for the shell
 * to write back; clears the disk's dirty flag. A DSK that gained a track or a different sector
 * layout is written as an Extended DSK (the disk's format then becomes ZR_DISK_EDSK). 0 with the
 * reason on failure; the media is untouched then. */
int zr_disk_save_media(zr_disk *disk, zr_media *media, char *error, size_t capacity);

#endif /* ZR_DISK_H */
