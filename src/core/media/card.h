/* Block devices over image files: the hard disk of the IDE channel (hw/ata.h) and the
 * SD or MMC card (hw/mmc.h) are both a zr_card, a run of 512-byte sectors in a file that is used in place (the host's
 * zr_host_file_*): never read whole into memory, a sector is read or written where it lies. ZetRo's own code, written
 * from the HDF format description (the "RS-IDE" header, document revision 0.3 of 2001), the ATA-1 IDENTIFY DEVICE
 * layout and the Virtual Hard Disk footer.
 *
 * Formats, told by content (and by the ending .hdf, which then must have the signature):
 *   raw  .img, .vhd or any name: the sectors one after the other from the first byte, which is what a disk
 *        copied with dd is. A file that ends with a 512-byte VHD footer (its first bytes "conectix") is a fixed
 *        VHD: the footer is not data and is never written. A dynamic or differencing VHD is refused. Any other size
 *        that is not a multiple of 512 is refused as truncated or not a disk.
 *   HDF  the header (22 bytes: "RS-IDE", 1Ah, the version 10h or 11h, the flags byte whose bit 0 says the sectors
 *        are halved, the data offset as 16 bits little-endian, 11 reserved bytes) and then the IDENTIFY DEVICE data
 *        of the drive the image was made from: 106 bytes (words 0 to 52) in version 1.0, whose data offset is 80h,
 *        the whole 512 bytes in version 1.1 (as written by common tools; the offset field, never the version, says
 *        where the data begins, so a header of any length from 22 bytes up is read the same way). Halved: the
 *        file keeps 256 bytes of a sector, the low byte of each of its 256 words (the interface that made the
 *        image had an 8-bit data bus); the high bytes read as zero and are not written.
 *
 * Sectors are 512 bytes (the sector size of the IDENTIFY data and of SD cards). There is no cache: a write goes to the
 * file at once (the operating system's own cache is the only one), zr_card_flush makes it reach the disk.
 * Read-only when the file is, or when the caller asked (the tab, zr_card_set_protect).
 *
 * The CHS geometry the IDENTIFY data reports: the image's own (an HDF header that carries a plausible one) or made
 * from the size: 16 heads of 63 sectors and cylinders = sectors / 1008, at most 65535 (ATA-1's 16-bit cylinder
 * number); an image below 1008 sectors has one head of up to 63 sectors. The cylinders times heads times sectors never
 * exceed the image (the sectors beyond are reached by LBA only).
 *
 * Never trusts a file: every size and offset is checked, a damaged, truncated or odd-sized image gives a message and
 * no card. */
#ifndef ZR_CARD_H
#define ZR_CARD_H

#include <stddef.h>
#include <stdint.h>

#define ZR_CARD_SECTOR 512u
#define ZR_CARD_PATH 1024
#define ZR_CARD_MIN_BYTES (64u * 1024u)             /* the smallest image zr_card_create makes */
#define ZR_CARD_MAX_BYTES ((uint64_t)1 << 41)       /* and the largest (2 TiB) */

enum { ZR_CARD_RAW, ZR_CARD_HDF };

struct zr_host_file;

typedef struct zr_card {
    struct zr_host_file *file;
    char path[ZR_CARD_PATH];
    int format;                         /* ZR_CARD_RAW or ZR_CARD_HDF */
    int version;                        /* HDF: 0x10 or 0x11; 0 for raw */
    int halved;                         /* HDF: 256 bytes of a sector are kept */
    int file_writable;                  /* the file lets itself be written */
    int writable;                       /* and the tab is open */
    uint64_t offset;                    /* where sector 0 begins in the file */
    uint64_t sectors;                   /* how many there are */
    unsigned cylinders, heads, spt;     /* the CHS geometry of the IDENTIFY data */
    unsigned identify_len;              /* HDF: bytes of the drive's IDENTIFY data the header holds (0: none) */
    uint8_t identify[ZR_CARD_SECTOR];   /* and the data (zero beyond identify_len) */
} zr_card;

/* Opens an image; `write_protect` makes it read-only whatever the file allows. NULL with the reason. */
zr_card *zr_card_open(const char *path, int write_protect, char *error, size_t capacity);
/* Makes a blank image of `bytes` (a multiple of 512, ZR_CARD_MIN_BYTES to ZR_CARD_MAX_BYTES; 16 MiB to 4 GiB are the
 * sizes of the setup screen), raw or HDF (a version 1.0 header with the IDENTIFY data of a drive of that size; the
 * file is `bytes` plus 128), sparse where the host allows. An existing file is replaced only with `overwrite`. 0 with
 * the reason. */
int zr_card_create(const char *path, int format, uint64_t bytes, int overwrite, char *error, size_t capacity);
void zr_card_close(zr_card *card);

/* One sector: 512 bytes. 1 when done; 0 for a sector beyond the image, a write to a protected image or a file
 * that fails. */
int zr_card_read(zr_card *card, uint64_t lba, void *data);
int zr_card_write(zr_card *card, uint64_t lba, const void *data);
/* What was written reaches the disk. */
int zr_card_flush(zr_card *card);

static inline uint64_t zr_card_sectors(const zr_card *card) { return card->sectors; }
static inline int zr_card_write_protected(const zr_card *card) { return !card->writable; }
/* Closes or opens the tab; 0 when the file is read-only and the tab cannot be opened. */
int zr_card_set_protect(zr_card *card, int protect);

/* The geometry an image of `sectors` gets (see above). */
void zr_card_default_geometry(uint64_t sectors, unsigned *cylinders, unsigned *heads, unsigned *spt);
/* The IDENTIFY DEVICE data of the image as a drive reports it (256 little-endian words): the HDF header's, made to
 * agree with the image (geometry, capacity, LBA support, current geometry, checksum), or one built from the size:
 * model "ZetRo IDE disk", firmware "1.00", serial "ZR" and the sector count, fixed disk, LBA, PIO mode 2. */
void zr_card_identify(const zr_card *card, uint8_t out[ZR_CARD_SECTOR]);
/* "HDF 1.0, 32 MiB, 65536 sectors, CHS 65/16/63, read-only" for the setup screen. */
void zr_card_describe(const zr_card *card, char *out, size_t capacity);

#endif /* ZR_CARD_H */
