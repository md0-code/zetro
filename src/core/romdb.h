/* ROM identification. Every file in a roms folder is known by the SHA-256
 * of its content, whatever its name: the firmware of each machine as the
 * MiSTer cores archive it (the hashes of MiSTerRO/docs/<core>/sources.lock.json),
 * one image for each ROM a board has. tools/fetch_roms.py puts them in roms/
 * under their standard names. A board asks for its ROMs by key. */

#ifndef ZR_ROMDB_H
#define ZR_ROMDB_H

#include <stddef.h>
#include <stdint.h>

#define ZR_ROM_PATH 1024

typedef struct zr_known_rom {
    const char *key;           /* "hc85", "if1-hc90" */
    const char *name;          /* "HC-85 BASIC" */
    uint32_t size;
    const char *sha256;        /* lower-case hex */
    const char *file;          /* standard file name in roms/ */
} zr_known_rom;

size_t zr_romdb_count(void);
const zr_known_rom *zr_romdb_entry(size_t index);
/* The known ROM with this content, or NULL. */
const zr_known_rom *zr_romdb_identify(const uint8_t *data, size_t size);
const zr_known_rom *zr_romdb_find(const char *key);

/* SHA-256 (FIPS 180-4) of data, as 32 bytes and as lower-case hex. */
void zr_sha256(const uint8_t *data, size_t size, uint8_t digest[32]);
void zr_sha256_hex(const uint8_t *data, size_t size, char hex[65]);

/* The files of the roms folders, read once at start and identified by
 * content. A recognised file's content is kept in memory. */
typedef struct zr_rom_file {
    char path[ZR_ROM_PATH];
    const char *name;          /* the file name, inside path */
    uint64_t size;
    char sha256[65];           /* empty for files too large to be a ROM */
    const zr_known_rom *rom;   /* NULL: not a known ROM */
    uint8_t *data;             /* a known ROM's content */
    int standard;              /* under its standard file name */
    int duplicate;             /* index of the file used for the same ROM, or -1 */
} zr_rom_file;

typedef struct zr_rom_index {
    zr_rom_file *files;
    size_t count, capacity;
    char folders[3][ZR_ROM_PATH];   /* the folders read (those that exist), each with its separator */
    size_t folder_count;
} zr_rom_index;

void zr_rom_index_init(zr_rom_index *index);
/* Reads every file of each folder (missing folders are skipped; the same
 * folder named twice is read once), the files of a folder in name order.
 * Of files holding the same ROM the one under the standard name is used,
 * else the first found. 0 when out of memory. */
int zr_rom_index_scan(zr_rom_index *index, const char *const *folders, size_t count);
/* Adds a file's content as if found under `name` (tests); 0 when out of
 * memory. */
int zr_rom_index_add(zr_rom_index *index, const char *name, const uint8_t *data, size_t size);
void zr_rom_index_free(zr_rom_index *index);
/* The file used for a known ROM, or NULL when it was not found. */
const zr_rom_file *zr_rom_index_find(const zr_rom_index *index, const zr_known_rom *rom);
/* What a file was recognised as, in words. */
const char *zr_rom_file_describe(const zr_rom_index *index, const zr_rom_file *file, char *buffer, size_t capacity);

#endif /* ZR_ROMDB_H */
