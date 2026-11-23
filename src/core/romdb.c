/* ROM identification (romdb.h): SHA-256 as FIPS 180-4 specifies it, the
 * table of known images, and the index of the roms folders. ZetRo's own
 * code. */
#include "romdb.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The images tools/fetch_roms.py puts in roms/, by the hashes the MiSTer
 * cores' lock files record. */
static const zr_known_rom ROMS[] = {
    {"jet", "JET BASIC", 16384u, "f3c220e4ebbce7a2e21890ce1735bc073e90a08c51019cf5c5b7132de5cc9232", "jet.rom"},
    {"hc85", "HC-85 BASIC", 16384u, "e3b92f2362b67c8ea5697f1eea89eab6319c5bcb255be3bdaa314f10b602d1dc", "hc85.rom"},
    {"hc90", "HC-90 BASIC", 16384u, "b23d93245be833ce9b2e00c7e9f37fad5ae075c1642a01222f5f86fb915a60cf", "hc90.rom"},
    {"hc91", "HC-91 BASIC", 16384u, "0902690c7b69871a2791165fb06f699bc676985d867dbaa0d76c1ef89d40a9ca", "hc91.rom"},
    {"hc88", "HC-88 bootstrap", 2048u, "73fc1804f9a7c110533141d23e940f1af3a390155d0ccbbdcc254a21358431c4", "hc88.rom"},
    {"hc128", "HC-91 Plus (HC-128) ROM", 32768u, "15193b404dfe4c6b242c8406e96a8494d0dd837323e043c93b696a28f4579b0f", "hc128.rom"},
    {"hc2000-0", "HC-2000 BASIC", 16384u, "fcb2da8daa8e1698b898a13b8baf9556d7fe2c7f2039bd1b7a6033185e0798b8", "hc2000-0.rom"},
    {"hc2000-1", "HC-2000 CP/M", 16384u, "4e5a52e98cbe5f9d42bcb2dcdd38cc6095f42dc0fb437be218dacac3da3830fa", "hc2000-1.rom"},
    {"if1-hc85", "HC-85 Interface 1", 10240u, "05256118e74a3ae63a7d15368a3b353e95f3268981ccba27e03325d6aac4b538", "if85.rom"},
    {"if1-hc90", "HC-90 Interface 1", 16384u, "d93d799036971fa5960dd6ef3787ba4aa3e97941ab5e5714783df1ec3c8f2475", "if90.rom"},
    {"hc-cpm", "HC family CP/M bridge (replacement)", 16384u, "9f71f5d5116efc8a824fac18378c61ab43e371d54c6a6db1c1374e92e5ea8bf4", "hc-family-cpm.rom"},
    {"cip01", "CIP-01 copier", 2048u, "4b75de3a67253cd06a428b9cdce6e586ed59c0ce1fd4526275b89e9866982fff", "cip01.rom"},
    {"cip03", "CIP-03 BASIC", 16384u, "1a05ac4f7bd67521c7f2b51b617d2fe086e28e297ec7749049d544c3ec3e1bb6", "cip03.rom"},
    {"cip04", "CIP-04 ROM", 65536u, "e8e822cdd205ed3cee95f8be51071cd7566c814019ffdfde5d268bf8f4037910", "cip04.rom"},
    {"tims", "TIM-S ROM", 16384u, "89df0794d0b6b59c3a8d789b6906cbc960f0e2cb8c207bea7ac300b92fc1df56", "tims-standard.rom"},
    {"tims-plus-loader", "TIM-S Plus loader (replacement)", 2048u, "877a669698b880e6367ae9968c0ac0c5a0e93edf917b424cc6490bb9e2e01b86", "plus-loader.rom"},
    {"plus3", "+3 system 4.1", 65536u, "5e7d9b3355ca76f4a99cbcc0bc1c90118ecda6a1549c2cb0f02e98af17694f3e", "plus3-system.rom"},
    {"cobra", "CoBra system ROMs", 81920u, "befb9116772456bbbabe9b44258f3fe5ecf3b33049be9612073707716c7f456a", "cobra.rom"},
    {"amic", "aMIC monitor and BASIC (repaired)", 16384u, "7fe40b1a70def2afc6f8c7c5eb5d1668a6dbe6d377d25b8e0676ebeb978fbef4", "amic.rom"},
    {"prae36", "PRAE-M BASIC 3.6", 16384u, "dc8c646c4eaf515bcb75bcca950df3e9242ffeb0058ead48d134e6d753c33b09", "prae-3.6.rom"},
    {"prae35", "PRAE-M BASIC 3.5", 16384u, "46fdb6719b9965492e2abcb00adf7f0d8d8f0d7896b026e4e5e70781e768eed1", "prae-3.5.rom"},
    {"cubz14", "CUB Z monitor 1.4", 8192u, "a49e7e2435987011b54405333851116678996fb865c66da2fe28e5dc9a34d4f3", "cubz-v14.rom"},
    {"cubz13", "CUB Z monitor 1.3", 8192u, "716065d441e02b5ee8fdd214d300fd557c742d829a2a749c02cf4ba34d1806b0", "cubz-v13.rom"},
    /* The firmware of the DivMMC and the DivIDE, the 8 KiB images of esxDOS 0.8.9 (esxdos.org); tools/fetch_roms.py fetches them from
     * the distribution, not from the MiSTerRO repository. */
    {"esxdos-mmc", "esxDOS 0.8.9 firmware for the DivMMC", 8192u, "89706de560d66432fe19ddb895e7cde49e252e932687dcb57fb92259a0259339", "esxmmc.bin"},
    {"esxdos-ide", "esxDOS 0.8.9 firmware for the DivIDE", 8192u, "771d5f21de48a3e9969adef7708ddb9a2834ba8729c5f702e5c0bb6067ebaa57", "esxide.bin"},
};
#define ROM_COUNT (sizeof ROMS / sizeof ROMS[0])
/* Nothing larger is read whole. */
#define LARGEST 131072u

size_t zr_romdb_count(void) { return ROM_COUNT; }
const zr_known_rom *zr_romdb_entry(size_t i) { return i < ROM_COUNT ? &ROMS[i] : NULL; }

const zr_known_rom *zr_romdb_find(const char *key) {
    for (size_t i = 0; i < ROM_COUNT; ++i) if (!strcmp(ROMS[i].key, key)) return &ROMS[i];
    return NULL;
}
static const zr_known_rom *by_hash(const char *hex, uint64_t size) {
    for (size_t i = 0; i < ROM_COUNT; ++i) if (ROMS[i].size == size && !strcmp(ROMS[i].sha256, hex)) return &ROMS[i];
    return NULL;
}
const zr_known_rom *zr_romdb_identify(const uint8_t *data, size_t size) {
    char hex[65];
    if (size > LARGEST) return NULL;
    zr_sha256_hex(data, size, hex);
    return by_hash(hex, size);
}

/* --- SHA-256 (FIPS 180-4, sections 4.1.2, 4.2.2, 5.1.1, 6.2) ---------------------------- */

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (unsigned t = 0; t < 16; ++t)
        w[t] = (uint32_t)p[4 * t] << 24 | (uint32_t)p[4 * t + 1] << 16 | (uint32_t)p[4 * t + 2] << 8 | p[4 * t + 3];
    for (unsigned t = 16; t < 64; ++t) {
        const uint32_t s0 = rotr(w[t - 15], 7) ^ rotr(w[t - 15], 18) ^ (w[t - 15] >> 3);
        const uint32_t s1 = rotr(w[t - 2], 17) ^ rotr(w[t - 2], 19) ^ (w[t - 2] >> 10);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (unsigned t = 0; t < 64; ++t) {
        const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[t] + w[t];
        const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

void zr_sha256(const uint8_t *data, size_t size, uint8_t digest[32]) {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    size_t i = 0;
    for (; size - i >= 64; i += 64) block(h, data + i);
    /* The padding: a 1 bit, zeros, the length in bits as 64 big-endian bits. */
    uint8_t tail[128];
    const size_t rest = size - i, total = rest < 56 ? 64 : 128;
    memset(tail, 0, sizeof tail);
    if (rest) memcpy(tail, data + i, rest);
    tail[rest] = 0x80;
    const uint64_t bits = (uint64_t)size * 8u;
    for (unsigned b = 0; b < 8; ++b) tail[total - 1 - b] = (uint8_t)(bits >> (8 * b));
    block(h, tail);
    if (total == 128) block(h, tail + 64);
    for (unsigned b = 0; b < 8; ++b) {
        digest[4 * b] = (uint8_t)(h[b] >> 24);
        digest[4 * b + 1] = (uint8_t)(h[b] >> 16);
        digest[4 * b + 2] = (uint8_t)(h[b] >> 8);
        digest[4 * b + 3] = (uint8_t)h[b];
    }
}

void zr_sha256_hex(const uint8_t *data, size_t size, char hex[65]) {
    static const char digits[] = "0123456789abcdef";
    uint8_t d[32];
    zr_sha256(data, size, d);
    for (unsigned i = 0; i < 32; ++i) {
        hex[2 * i] = digits[d[i] >> 4];
        hex[2 * i + 1] = digits[d[i] & 15];
    }
    hex[64] = 0;
}

/* --- the index ------------------------------------------------------------------- */

void zr_rom_index_init(zr_rom_index *ix) { memset(ix, 0, sizeof *ix); }

static zr_rom_file *new_file(zr_rom_index *ix) {
    if (ix->count == ix->capacity) {
        const size_t capacity = ix->capacity ? ix->capacity * 2 : 16;
        zr_rom_file *files = realloc(ix->files, capacity * sizeof *files);
        if (!files) return NULL;
        ix->files = files;
        ix->capacity = capacity;
    }
    zr_rom_file *f = &ix->files[ix->count];
    memset(f, 0, sizeof *f);
    f->duplicate = -1;
    return f;
}

/* Identifies the newest file and settles which of the same ROM is used. */
static void settle(zr_rom_index *ix, zr_rom_file *f, const uint8_t *data) {
    if (f->size <= LARGEST && data) {
        zr_sha256_hex(data, (size_t)f->size, f->sha256);
        f->rom = by_hash(f->sha256, f->size);
    }
    if (!f->rom) return;
    f->standard = !strcmp(f->name, f->rom->file);
    f->data = malloc((size_t)f->size);
    if (f->data) memcpy(f->data, data, (size_t)f->size);
    else f->rom = NULL;
    const size_t self = (size_t)(f - ix->files);
    for (size_t i = 0; f->rom && i < self; ++i) {
        zr_rom_file *o = &ix->files[i];
        if (o->rom != f->rom || o->duplicate >= 0) continue;
        if (f->standard && !o->standard) o->duplicate = (int)self;   /* the standard name wins */
        else f->duplicate = (int)i;
        break;
    }
    /* Duplicates of a file that just lost point to the winner. */
    for (size_t i = 0; i < self; ++i)
        if (ix->files[i].duplicate >= 0 && ix->files[ix->files[i].duplicate].duplicate >= 0)
            ix->files[i].duplicate = ix->files[ix->files[i].duplicate].duplicate;
}

int zr_rom_index_add(zr_rom_index *ix, const char *name, const uint8_t *data, size_t size) {
    zr_rom_file *f = new_file(ix);
    if (!f) return 0;
    snprintf(f->path, sizeof f->path, "%s", name);
    f->name = f->path;
    f->size = size;
    ix->count++;
    settle(ix, f, data);
    return 1;
}

typedef struct names { char (*list)[256]; size_t count, capacity; int failed; } names;
static int collect(void *ctx, const char *name) {
    names *n = ctx;
    if (strlen(name) >= 256) return 1;
    if (n->count == n->capacity) {
        const size_t capacity = n->capacity ? n->capacity * 2 : 32;
        char (*list)[256] = realloc(n->list, capacity * sizeof *list);
        if (!list) { n->failed = 1; return 0; }
        n->list = list;
        n->capacity = capacity;
    }
    snprintf(n->list[n->count++], 256, "%s", name);
    return 1;
}
static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* 1 read, 0 out of memory, -1 no such folder. */
static int scan_folder(zr_rom_index *ix, const char *folder) {
    names n = {NULL, 0, 0, 0};
    if (!zr_host_list_files(folder, collect, &n)) { free(n.list); return -1; }
    if (n.failed) { free(n.list); return 0; }
    if (n.count) qsort(n.list, n.count, sizeof *n.list, by_name);
    int ok = 1;
    for (size_t i = 0; ok && i < n.count; ++i) {
        zr_rom_file *f = new_file(ix);
        if (!f) { ok = 0; break; }
        if (snprintf(f->path, sizeof f->path, "%s%s", folder, n.list[i]) >= (int)sizeof f->path) continue;
        f->name = f->path + strlen(folder);
        if (!zr_host_file_size(f->path, &f->size)) continue;
        uint8_t *buffer = NULL;
        size_t size = 0;
        if (f->size <= LARGEST &&
            zr_host_read_file(f->path, LARGEST, &buffer, &size, NULL, NULL, 0))
            f->size = size;
        ix->count++;
        settle(ix, f, buffer);
        free(buffer);
    }
    free(n.list);
    return ok;
}

int zr_rom_index_scan(zr_rom_index *ix, const char *const *folders, size_t count) {
    for (size_t k = 0; k < count && ix->folder_count < 3; ++k) {
        if (!folders[k] || !folders[k][0]) continue;
        int seen = 0;
        for (size_t j = 0; j < ix->folder_count; ++j) seen |= zr_host_same_folder(ix->folders[j], folders[k]);
        if (seen) continue;
        char *folder = ix->folders[ix->folder_count++];
        const size_t n = strlen(folders[k]);
        const int separated = folders[k][n - 1] == '/' || folders[k][n - 1] == '\\';
        snprintf(folder, ZR_ROM_PATH, "%s%s", folders[k], separated ? "" : (char[2]){zr_host_separator(), 0});
        const int read = scan_folder(ix, folder);
        if (!read) return 0;
        if (read < 0) ix->folder_count--;      /* missing: not one of the folders */
    }
    return 1;
}

void zr_rom_index_free(zr_rom_index *ix) {
    for (size_t i = 0; i < ix->count; ++i) free(ix->files[i].data);
    free(ix->files);
    memset(ix, 0, sizeof *ix);
}

const zr_rom_file *zr_rom_index_find(const zr_rom_index *ix, const zr_known_rom *rom) {
    if (!ix || !rom) return NULL;
    for (size_t i = 0; i < ix->count; ++i)
        if (ix->files[i].rom == rom && ix->files[i].duplicate < 0 && ix->files[i].data) return &ix->files[i];
    return NULL;
}

const char *zr_rom_file_describe(const zr_rom_index *ix, const zr_rom_file *f, char *buffer, size_t capacity) {
    if (!f->rom) {
        if (!f->sha256[0]) snprintf(buffer, capacity, "unknown (%llu bytes, too large for a ROM)", (unsigned long long)f->size);
        else snprintf(buffer, capacity, "unknown (%llu bytes, SHA-256 %.16s...)", (unsigned long long)f->size, f->sha256);
    } else if (f->duplicate >= 0)
        snprintf(buffer, capacity, "%s, a copy of %s (not used)", f->rom->name, ix->files[f->duplicate].name);
    else if (!f->standard) snprintf(buffer, capacity, "%s (standard name %s)", f->rom->name, f->rom->file);
    else snprintf(buffer, capacity, "%s", f->rom->name);
    return buffer;
}
