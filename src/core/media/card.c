/* Block devices over image files (card.h). ZetRo's own code. */
#include "card.h"
#include "media.h"
#include "../../host/host.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HDF_SIGNATURE "RS-IDE"
#define HDF_FIXED_HEADER 0x16u              /* signature, marker, version, flags, data offset, reserved */
#define HDF_V10_OFFSET 0x80u                /* header of a version 1.0 file: 22 bytes and 106 of IDENTIFY data */
#define HDF_V10_IDENTIFY 106u
#define HDF_FLAG_HALVED 0x01u
#define VHD_FOOTER 512u

static int fail(char *error, size_t capacity, const char *format, ...) {
    if (error && capacity) {
        va_list args;
        va_start(args, format);
        vsnprintf(error, capacity, format, args);
        va_end(args);
    }
    return 0;
}

static uint16_t get_word(const uint8_t *b, unsigned w) { return (uint16_t)(b[2 * w] | (uint16_t)b[2 * w + 1] << 8); }
static void put_word(uint8_t *b, unsigned w, unsigned v) { b[2 * w] = (uint8_t)v; b[2 * w + 1] = (uint8_t)(v >> 8); }

/* --- geometry and IDENTIFY DEVICE data ------------------------------------------------------- */

void zr_card_default_geometry(uint64_t sectors, unsigned *cylinders, unsigned *heads, unsigned *spt) {
    unsigned c, h, s;
    if (sectors >= 16u * 63u) {
        h = 16; s = 63;
        const uint64_t cyl = sectors / (16u * 63u);
        c = cyl > 65535u ? 65535u : (unsigned)cyl;
    } else {
        h = 1;
        s = sectors > 63u ? 63u : (unsigned)(sectors ? sectors : 1u);
        c = (unsigned)(sectors / s);
        if (c == 0) c = 1;
    }
    if (cylinders) *cylinders = c;
    if (heads) *heads = h;
    if (spt) *spt = s;
}

/* ATA strings: two characters to a word, the first in the high byte. */
static void put_string(uint8_t *b, unsigned first_word, unsigned words, const char *text) {
    const size_t n = strlen(text);
    for (unsigned i = 0; i < words * 2; i++) {
        const uint8_t ch = i < n ? (uint8_t)text[i] : (uint8_t)' ';
        b[2 * (first_word + i / 2) + ((i & 1) ? 0 : 1)] = ch;
    }
}

/* Word 255: the signature A5h and the checksum that makes the 512 bytes add to zero. */
static void seal(uint8_t *b) {
    b[510] = 0xA5;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 511; i++) sum = (uint8_t)(sum + b[i]);
    b[511] = (uint8_t)(0u - sum);
}

static void identify_geometry(uint8_t *b, uint64_t sectors, unsigned c, unsigned h, unsigned s) {
    const uint64_t capacity = sectors > 0x0FFFFFFFu ? 0x0FFFFFFFu : sectors;
    const uint32_t chs = (uint32_t)((uint64_t)c * h * s);
    put_word(b, 1, c);
    put_word(b, 3, h);
    put_word(b, 6, s);
    put_word(b, 49, get_word(b, 49) | 0x0200u);            /* LBA supported */
    put_word(b, 53, get_word(b, 53) | 0x0001u);            /* words 54 to 58 are valid */
    put_word(b, 54, c);
    put_word(b, 55, h);
    put_word(b, 56, s);
    put_word(b, 57, chs & 0xFFFFu);
    put_word(b, 58, chs >> 16);
    put_word(b, 60, (unsigned)(capacity & 0xFFFFu));
    put_word(b, 61, (unsigned)(capacity >> 16));
}

static void identify_build(uint8_t *b, uint64_t sectors, unsigned c, unsigned h, unsigned s) {
    char serial[24];
    memset(b, 0, ZR_CARD_SECTOR);
    put_word(b, 0, 0x0040u);                                /* a fixed disk */
    put_word(b, 4, s * ZR_CARD_SECTOR);                     /* unformatted bytes a track (obsolete) */
    put_word(b, 5, ZR_CARD_SECTOR);                         /* and a sector */
    snprintf(serial, sizeof serial, "ZR%012llX", (unsigned long long)sectors);
    put_string(b, 10, 10, serial);
    put_word(b, 20, 3);                                     /* a buffer that caches reads */
    put_word(b, 21, 2);                                     /* its size in sectors */
    put_word(b, 22, 4);                                     /* ECC bytes of the long commands */
    put_string(b, 23, 4, "1.00");
    put_string(b, 27, 20, "ZetRo IDE disk");
    put_word(b, 51, 0x0200u);                               /* PIO transfer timing mode 2 */
    identify_geometry(b, sectors, c, h, s);
    seal(b);
}

void zr_card_identify(const zr_card *card, uint8_t out[ZR_CARD_SECTOR]) {
    if (card->identify_len >= 14) {
        memcpy(out, card->identify, ZR_CARD_SECTOR);
        identify_geometry(out, card->sectors, card->cylinders, card->heads, card->spt);
        seal(out);
    } else {
        identify_build(out, card->sectors, card->cylinders, card->heads, card->spt);
    }
}

/* --- opening ------------------------------------------------------------------------------------ */

static void be_read(const uint8_t *p, unsigned n, uint64_t *value) {
    uint64_t v = 0;
    for (unsigned i = 0; i < n; i++) v = v << 8 | p[i];
    *value = v;
}

/* A raw image, with its VHD footer when it has one. */
static int parse_raw(zr_card *c, uint64_t length, char *error, size_t capacity) {
    uint64_t data = length;
    if (length == 0) return fail(error, capacity, "The file is empty");
    if (length >= 2 * VHD_FOOTER && length % ZR_CARD_SECTOR == 0) {
        uint8_t footer[VHD_FOOTER];
        if (zr_host_file_read_at(c->file, length - VHD_FOOTER, footer, sizeof footer) && !memcmp(footer, "conectix", 8)) {
            uint64_t type, current;
            be_read(footer + 60, 4, &type);
            be_read(footer + 48, 8, &current);
            if (type != 2) return fail(error, capacity, "A dynamic or differencing VHD cannot be used, only a fixed one");
            data = length - VHD_FOOTER;
            if (current > data) return fail(error, capacity, "The VHD is truncated: its footer says %llu bytes, the file holds %llu", (unsigned long long)current, (unsigned long long)data);
            if (current >= ZR_CARD_SECTOR) data = current / ZR_CARD_SECTOR * ZR_CARD_SECTOR;
        }
    }
    if (data % ZR_CARD_SECTOR)
        return fail(error, capacity, "The size, %llu bytes, is not a multiple of 512: the image is truncated or not a disk image", (unsigned long long)data);
    c->format = ZR_CARD_RAW;
    c->offset = 0;
    c->sectors = data / ZR_CARD_SECTOR;
    zr_card_default_geometry(c->sectors, &c->cylinders, &c->heads, &c->spt);
    return 1;
}

static int parse_hdf(zr_card *c, uint64_t length, char *error, size_t capacity) {
    uint8_t head[HDF_FIXED_HEADER];
    if (length < HDF_FIXED_HEADER) return fail(error, capacity, "The HDF image is truncated: %llu bytes, less than its header", (unsigned long long)length);
    if (!zr_host_file_read_at(c->file, 0, head, sizeof head)) return fail(error, capacity, "Cannot read the HDF header");
    if (memcmp(head, HDF_SIGNATURE, 6)) return fail(error, capacity, "Not an HDF image: the RS-IDE signature is missing");
    if (head[6] != 0x1A) return fail(error, capacity, "The HDF header is damaged: its marker byte is not 1Ah");
    if (head[7] != 0x10 && head[7] != 0x11) return fail(error, capacity, "HDF version %u.%u is not supported (1.0 and 1.1 are)", head[7] >> 4, head[7] & 15u);
    const unsigned offset = head[9] | (unsigned)head[10] << 8;
    if (offset < HDF_FIXED_HEADER) return fail(error, capacity, "The HDF header is damaged: the data offset %u is inside the header", offset);
    if (offset >= length) return fail(error, capacity, "The HDF image is truncated: it ends within or right after its header");
    c->format = ZR_CARD_HDF;
    c->version = head[7];
    c->halved = (head[8] & HDF_FLAG_HALVED) != 0;
    c->offset = offset;
    unsigned have = offset - HDF_FIXED_HEADER;
    if (have > ZR_CARD_SECTOR) have = ZR_CARD_SECTOR;
    if (have && !zr_host_file_read_at(c->file, HDF_FIXED_HEADER, c->identify, have)) return fail(error, capacity, "Cannot read the HDF header");
    c->identify_len = have;
    const unsigned bytes = c->halved ? ZR_CARD_SECTOR / 2 : ZR_CARD_SECTOR;
    const uint64_t data = length - offset;
    if (data % bytes) return fail(error, capacity, "The HDF image is truncated or damaged: its data is not a whole number of %u-byte sectors", bytes);
    c->sectors = data / bytes;
    if (c->sectors == 0) return fail(error, capacity, "The HDF image holds no sectors");
    /* What the header says the drive was: the geometry when it is a plausible one, and the capacity. */
    unsigned cyl = 0, heads = 0, spt = 0;
    uint64_t declared = 0;
    if (c->identify_len >= 14) {
        cyl = get_word(c->identify, 1); heads = get_word(c->identify, 3); spt = get_word(c->identify, 6);
        if (cyl == 0 || heads == 0 || heads > 16 || spt == 0 || spt > 255) cyl = heads = spt = 0;
    }
    if (c->identify_len >= 124) declared = (uint64_t)get_word(c->identify, 60) | (uint64_t)get_word(c->identify, 61) << 16;
    if (cyl && (uint64_t)cyl * heads * spt > c->sectors) declared = (uint64_t)cyl * heads * spt;
    if (declared > c->sectors)
        return fail(error, capacity, "The HDF image is truncated: its header describes %llu sectors, the file holds %llu", (unsigned long long)declared, (unsigned long long)c->sectors);
    if (cyl) { c->cylinders = cyl; c->heads = heads; c->spt = spt; }
    else zr_card_default_geometry(c->sectors, &c->cylinders, &c->heads, &c->spt);
    return 1;
}

zr_card *zr_card_open(const char *path, int write_protect, char *error, size_t capacity) {
    if (strlen(path) >= ZR_CARD_PATH) { fail(error, capacity, "The path is too long"); return NULL; }
    zr_card *c = calloc(1, sizeof *c);
    if (!c) { fail(error, capacity, "Out of memory"); return NULL; }
    snprintf(c->path, sizeof c->path, "%s", path);
    c->file = zr_host_file_open(path, &c->file_writable, error, capacity);
    if (!c->file) { free(c); return NULL; }
    const uint64_t length = zr_host_file_length(c->file);
    uint8_t signature[8] = {0};
    if (length >= sizeof signature) zr_host_file_read_at(c->file, 0, signature, sizeof signature);
    const int hdf = (length >= 7 && !memcmp(signature, HDF_SIGNATURE "\x1A", 7)) || zr_media_extension_in(path, "hdf");
    if (!(hdf ? parse_hdf(c, length, error, capacity) : parse_raw(c, length, error, capacity))) {
        zr_host_file_close(c->file);
        free(c);
        return NULL;
    }
    c->writable = c->file_writable && !write_protect;
    return c;
}

/* Another machine can prepare a restored controller while the running one owns the file. */
zr_card *zr_card_clone(const zr_card *source, int write_protect, char *error, size_t capacity) {
    zr_card *card = malloc(sizeof *card);
    if (!card) { fail(error, capacity, "Out of memory"); return NULL; }
    *card = *source;
    card->file = zr_host_file_retain(source->file);
    card->writable = card->file_writable && !write_protect;
    return card;
}

void zr_card_close(zr_card *c) {
    if (!c) return;
    if (c->file) {
        if (c->writable) zr_host_file_flush(c->file);
        zr_host_file_close(c->file);
    }
    free(c);
}

/* --- creating --------------------------------------------------------------------------------- */

int zr_card_create(const char *path, int format, uint64_t bytes, int overwrite, char *error, size_t capacity) {
    if (format != ZR_CARD_RAW && format != ZR_CARD_HDF) return fail(error, capacity, "Unknown image format");
    if (bytes % ZR_CARD_SECTOR || bytes < ZR_CARD_MIN_BYTES || bytes > ZR_CARD_MAX_BYTES)
        return fail(error, capacity, "An image is a multiple of 512 bytes, from 64 KiB to 2 TiB");
    const uint64_t sectors = bytes / ZR_CARD_SECTOR;
    if (format == ZR_CARD_RAW) return zr_host_file_create(path, bytes, overwrite, error, capacity);
    uint8_t header[HDF_V10_OFFSET], identify[ZR_CARD_SECTOR];
    unsigned c, h, s;
    zr_card_default_geometry(sectors, &c, &h, &s);
    identify_build(identify, sectors, c, h, s);
    memset(header, 0, sizeof header);
    memcpy(header, HDF_SIGNATURE, 6);
    header[6] = 0x1A;
    header[7] = 0x10;
    header[9] = (uint8_t)HDF_V10_OFFSET;
    header[10] = (uint8_t)(HDF_V10_OFFSET >> 8);
    memcpy(header + HDF_FIXED_HEADER, identify, HDF_V10_IDENTIFY);
    if (!zr_host_file_create(path, bytes + HDF_V10_OFFSET, overwrite, error, capacity)) return 0;
    int writable = 0;
    zr_host_file *f = zr_host_file_open(path, &writable, error, capacity);
    if (!f) { zr_host_delete_file(path); return 0; }
    const int ok = writable && zr_host_file_write_at(f, 0, header, sizeof header) && zr_host_file_flush(f);
    zr_host_file_close(f);
    if (!ok) {
        zr_host_delete_file(path);
        return fail(error, capacity, "Cannot write the HDF header of %s", path);
    }
    return 1;
}

/* --- sectors ---------------------------------------------------------------------------------- */

int zr_card_read(zr_card *c, uint64_t lba, void *data) {
    if (lba >= c->sectors) return 0;
    if (!c->halved) return zr_host_file_read_at(c->file, c->offset + lba * ZR_CARD_SECTOR, data, ZR_CARD_SECTOR);
    uint8_t half[ZR_CARD_SECTOR / 2];
    uint8_t *out = data;
    if (!zr_host_file_read_at(c->file, c->offset + lba * (ZR_CARD_SECTOR / 2), half, sizeof half)) return 0;
    for (unsigned i = 0; i < sizeof half; i++) { out[2 * i] = half[i]; out[2 * i + 1] = 0; }
    return 1;
}

int zr_card_write(zr_card *c, uint64_t lba, const void *data) {
    if (lba >= c->sectors || !c->writable) return 0;
    if (!c->halved) return zr_host_file_write_at(c->file, c->offset + lba * ZR_CARD_SECTOR, data, ZR_CARD_SECTOR);
    uint8_t half[ZR_CARD_SECTOR / 2];
    const uint8_t *in = data;
    for (unsigned i = 0; i < sizeof half; i++) half[i] = in[2 * i];
    return zr_host_file_write_at(c->file, c->offset + lba * (ZR_CARD_SECTOR / 2), half, sizeof half);
}

int zr_card_flush(zr_card *c) { return c->writable ? zr_host_file_flush(c->file) : 1; }

int zr_card_set_protect(zr_card *c, int protect) {
    if (!protect && !c->file_writable) return 0;
    c->writable = !protect;
    return 1;
}

void zr_card_describe(const zr_card *c, char *out, size_t capacity) {
    char kind[16], size[32];
    const uint64_t bytes = c->sectors * ZR_CARD_SECTOR;
    if (c->format == ZR_CARD_HDF) snprintf(kind, sizeof kind, "HDF %u.%u%s", c->version >> 4, c->version & 15u, c->halved ? " halved" : "");
    else snprintf(kind, sizeof kind, "Raw image");
    if (bytes % ((uint64_t)1 << 30) == 0) snprintf(size, sizeof size, "%llu GiB", (unsigned long long)(bytes >> 30));
    else if (bytes % ((uint64_t)1 << 20) == 0) snprintf(size, sizeof size, "%llu MiB", (unsigned long long)(bytes >> 20));
    else snprintf(size, sizeof size, "%llu KiB", (unsigned long long)(bytes >> 10));
    snprintf(out, capacity, "%s, %s, %llu sectors, CHS %u/%u/%u%s", kind, size, (unsigned long long)c->sectors, c->cylinders, c->heads, c->spt,
             c->writable ? "" : ", read-only");
}
