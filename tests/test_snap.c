/* The snapshot formats (core/media/snap.h) and the .pok reader (core/media/pok.h): snapshots built in memory, written as SNA, Z80 and SZX and read
 * back; the compression's edge cases; an SZX with a compressed page and blocks to skip; a Z80 version 1 file; files cut and damaged (every read gives
 * an answer, none crashes); .pok files with damaged trainers. No machine, no SDL. */
#include "media/pok.h"
#include "media/snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

static uint32_t seed = 20261113u;
static unsigned rnd(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

/* A snapshot of a 48K or 128K machine with registers of every kind and RAM of runs, ED bytes and noise. */
static zr_snap *make(unsigned machine, unsigned paged) {
    zr_snap *s = zr_snap_new();
    if (!s) return NULL;
    zr_snap_set_machine(s, machine, 0, machine == ZR_SNAP_M_48K ? "ZX Spectrum 48K" : machine == ZR_SNAP_M_PLUS3 ? "ZX Spectrum +3" : "ZX Spectrum 128K");
    s->pc = 0x8123; s->sp = 0xFF40;
    s->af = 0x1234; s->bc = 0x2345; s->de = 0x3456; s->hl = 0x4567;
    s->af2 = 0x5678; s->bc2 = 0x6789; s->de2 = 0x789A; s->hl2 = 0x89AB; s->ix = 0x9ABC; s->iy = 0xABCD;
    s->i = 0x3F; s->r = 0xA5; s->iff1 = 1; s->iff2 = 1; s->im = 2;
    s->border = 5;
    s->port_fe = 0x15;
    s->has_tstates = 1;
    s->tstates = 12345;
    const unsigned order[8] = {5, 2, 0, 1, 3, 4, 6, 7};
    const unsigned count = zr_snap_pages_of(machine) == 0xFFu ? 8u : 3u;
    for (unsigned k = 0; k < count; ++k) {
        uint8_t *p = s->ram[order[k]];
        for (unsigned i = 0; i < ZR_SNAP_PAGE_SIZE; ++i) {
            const unsigned part = (i >> 8) & 7u;
            p[i] = part == 0 ? 0 : part == 1 ? 0xED : part == 2 ? (uint8_t)rnd() : part == 3 ? (uint8_t)(i & 1 ? 0xED : 0) : part == 4 ? 7 : (uint8_t)(i * 31u);
        }
        p[ZR_SNAP_PAGE_SIZE - 1] = 0xED;                    /* a single ED at the end of a page */
        p[ZR_SNAP_PAGE_SIZE - 2] = 0x11;
        s->pages |= (uint8_t)(1u << order[k]);
    }
    if (count == 8) {
        s->port_7ffd = (uint8_t)(0x10 | paged);
        s->ay_present = 1;
        s->ay_current = 7;
        for (unsigned i = 0; i < 16; ++i) s->ay[i] = (uint8_t)(i * 3);
    }
    if (machine == ZR_SNAP_M_PLUS3) s->port_1ffd = 4;
    return s;
}

static int same_cpu(const zr_snap *a, const zr_snap *b) {
    return a->sp == b->sp && a->af == b->af && a->bc == b->bc && a->de == b->de && a->hl == b->hl && a->af2 == b->af2 && a->bc2 == b->bc2 &&
           a->de2 == b->de2 && a->hl2 == b->hl2 && a->ix == b->ix && a->iy == b->iy && a->i == b->i && a->r == b->r && a->iff1 == b->iff1 &&
           a->iff2 == b->iff2 && a->im == b->im && a->border == b->border;
}

static int same_ram(const zr_snap *a, const zr_snap *b) {
    if (a->pages != b->pages) return 0;
    for (unsigned page = 0; page < ZR_SNAP_PAGES; ++page)
        if ((a->pages >> page & 1u) && memcmp(a->ram[page], b->ram[page], ZR_SNAP_PAGE_SIZE)) return 0;
    return 1;
}

static int roundtrip(const zr_snap *s, unsigned format, const char *name, zr_snap *out, uint8_t **file, size_t *size) {
    char error[256] = "";
    if (!zr_snap_write(s, format, file, size, error, sizeof error)) { printf("write %s: %s\n", name, error); return 0; }
    if (!zr_snap_read(*file, *size, name, out, error, sizeof error)) { printf("read %s: %s\n", name, error); return 0; }
    return 1;
}

/* --- the formats ------------------------------------------------------------------------ */

static void test_formats(void) {
    zr_snap *s = make(ZR_SNAP_M_48K, 0), *r = zr_snap_new();
    uint8_t *file;
    size_t size;
    /* 48K: SNA keeps PC on the stack (two bytes of RAM change), the other two keep every field */
    CHECK(roundtrip(s, ZR_SNAP_SNA, "a.sna", r, &file, &size));
    CHECK(size == 49179 && r->format == ZR_SNAP_SNA && r->machine == ZR_SNAP_M_48K && r->pc == 0x8123 && same_cpu(s, r) && r->sp == s->sp);
    CHECK(r->pages == s->pages && !memcmp(r->ram[5], s->ram[5], ZR_SNAP_PAGE_SIZE) && !memcmp(r->ram[2], s->ram[2], ZR_SNAP_PAGE_SIZE));
    CHECK(r->ram[0][0xFF3E - 0xC000] == 0x23 && r->ram[0][0xFF3F - 0xC000] == 0x81);       /* PC, pushed */
    r->ram[0][0xFF3E - 0xC000] = s->ram[0][0xFF3E - 0xC000];
    r->ram[0][0xFF3F - 0xC000] = s->ram[0][0xFF3F - 0xC000];
    CHECK(same_ram(s, r));
    free(file);
    CHECK(roundtrip(s, ZR_SNAP_Z80, "a.z80", r, &file, &size));
    CHECK(r->format == ZR_SNAP_Z80 && r->machine == ZR_SNAP_M_48K && r->pc == 0x8123 && same_cpu(s, r) && same_ram(s, r) && r->has_tstates && r->tstates == 12345);
    CHECK(file[6] == 0 && file[7] == 0 && file[30] == 54 && file[34] == 0);
    free(file);
    CHECK(roundtrip(s, ZR_SNAP_SZX, "a.szx", r, &file, &size));
    CHECK(r->format == ZR_SNAP_SZX && r->machine == ZR_SNAP_M_48K && r->pc == 0x8123 && same_cpu(s, r) && same_ram(s, r) && r->tstates == 12345);
    free(file);
    /* HALT: PC is past it in a snapshot; SNA and Z80 give the HALT's own address, SZX keeps the flag */
    s->halted = 1;
    s->ei = 1;
    CHECK(roundtrip(s, ZR_SNAP_Z80, "h.z80", r, &file, &size) && r->pc == 0x8122 && !r->halted);
    free(file);
    CHECK(roundtrip(s, ZR_SNAP_SZX, "h.szx", r, &file, &size) && r->pc == 0x8123 && r->halted && r->ei);
    free(file);
    s->halted = 0;
    /* a stack in ROM cannot go in an SNA */
    char error[256] = "";
    s->sp = 0x0100;
    CHECK(!zr_snap_write(s, ZR_SNAP_SNA, &file, &size, error, sizeof error) && strstr(error, "stack"));
    s->sp = 0x0001;
    CHECK(!zr_snap_write(s, ZR_SNAP_SNA, &file, &size, error, sizeof error));
    s->sp = 0x0000;
    CHECK(zr_snap_write(s, ZR_SNAP_SNA, &file, &size, error, sizeof error));
    free(file);
    /* 128K, paged page 4 and page 5 (the file holds that page twice: 131103 and 147487 bytes) */
    for (unsigned paged = 4; paged <= 5; ++paged) {
        zr_snap *m = make(ZR_SNAP_M_128K, paged);
        CHECK(roundtrip(m, ZR_SNAP_SNA, "m.sna", r, &file, &size));
        CHECK(size == (paged == 4 ? 131103u : 147487u) && r->machine == ZR_SNAP_M_128K && r->pc == 0x8123 && r->port_7ffd == m->port_7ffd && same_cpu(m, r) &&
              same_ram(m, r));
        free(file);
        CHECK(roundtrip(m, ZR_SNAP_Z80, "m.z80", r, &file, &size));
        CHECK(r->machine == ZR_SNAP_M_128K && file[34] == 4 && r->port_7ffd == m->port_7ffd && same_ram(m, r) && r->ay_present && !memcmp(r->ay, m->ay, 16) &&
              r->ay_current == 7 && same_cpu(m, r) && r->tstates == 12345);
        free(file);
        CHECK(roundtrip(m, ZR_SNAP_SZX, "m.szx", r, &file, &size));
        CHECK(r->machine == ZR_SNAP_M_128K && r->port_7ffd == m->port_7ffd && same_ram(m, r) && r->ay_present && !memcmp(r->ay, m->ay, 16) && same_cpu(m, r));
        free(file);
        zr_snap_free(m);
    }
    /* +3: port 1FFD, no SNA */
    zr_snap *p = make(ZR_SNAP_M_PLUS3, 1);
    CHECK(roundtrip(p, ZR_SNAP_Z80, "p.z80", r, &file, &size) && r->machine == ZR_SNAP_M_PLUS3 && r->port_1ffd == 4 && file[34] == 7 && file[30] == 55 && same_ram(p, r));
    free(file);
    CHECK(roundtrip(p, ZR_SNAP_SZX, "p.szx", r, &file, &size) && r->machine == ZR_SNAP_M_PLUS3 && r->port_1ffd == 4 && same_ram(p, r));
    free(file);
    CHECK(!zr_snap_write(p, ZR_SNAP_SNA, &file, &size, error, sizeof error) && strstr(error, "1FFD"));
    zr_snap_free(p);
    zr_snap_free(r);
    zr_snap_free(s);
}

/* --- the compression and the other versions ------------------------------------------------------ */

/* A zlib stream of one stored block (the tree has a decompressor and no compressor). */
static size_t zlib_stored(const uint8_t *data, size_t n, uint8_t *out) {
    size_t o = 0;
    out[o++] = 0x78;
    out[o++] = 0x01;
    out[o++] = 1;
    out[o++] = (uint8_t)n; out[o++] = (uint8_t)(n >> 8);
    out[o++] = (uint8_t)~n; out[o++] = (uint8_t)(~n >> 8);
    memcpy(out + o, data, n);
    o += n;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) { a = (a + data[i]) % 65521u; b = (b + a) % 65521u; }
    const uint32_t adler = b << 16 | a;
    out[o++] = (uint8_t)(adler >> 24); out[o++] = (uint8_t)(adler >> 16); out[o++] = (uint8_t)(adler >> 8); out[o++] = (uint8_t)adler;
    return o;
}

static void test_other_forms(void) {
    zr_snap *s = make(ZR_SNAP_M_48K, 0), *r = zr_snap_new();
    char error[256] = "";
    uint8_t *file;
    size_t size;
    CHECK(zr_snap_write(s, ZR_SNAP_SZX, &file, &size, error, sizeof error));
    /* An SZX whose page 2 is a zlib stream and that has blocks this reader does not know, ahead of and after the others. */
    uint8_t *out = malloc(size + 40000);
    size_t o = 8, at = 8;
    memcpy(out, file, 8);
    size_t z_end = 0;
    memcpy(out + o, "KEYB", 4); out[o + 4] = 5; memset(out + o + 5, 0, 3 + 5); o += 8 + 5;
    memcpy(out + o, "CRTR", 4); out[o + 4] = 2; memset(out + o + 5, 0, 3 + 2); o += 8 + 2;
    while (at < size) {
        const uint32_t length = file[at + 4] | file[at + 5] << 8 | file[at + 6] << 16 | (uint32_t)file[at + 7] << 24;
        if (!memcmp(file + at, "RAMP", 4) && file[at + 10] == 2) {
            uint8_t z[16384 + 32];
            const size_t zn = zlib_stored(file + at + 11, 16384, z);
            memcpy(out + o, "RAMP", 4);
            out[o + 4] = (uint8_t)(zn + 3); out[o + 5] = (uint8_t)((zn + 3) >> 8); out[o + 6] = 0; out[o + 7] = 0;
            out[o + 8] = 1; out[o + 9] = 0; out[o + 10] = 2;
            memcpy(out + o + 11, z, zn);
            o += 11 + zn;
            z_end = o;
        } else {
            memcpy(out + o, file + at, 8 + length);
            o += 8 + length;
        }
        at += 8 + length;
    }
    CHECK(zr_snap_read(out, o, "z.szx", r, error, sizeof error) && same_ram(s, r) && same_cpu(s, r));
    /* the same with the stream's Adler-32 damaged: refused */
    out[z_end - 1] ^= 0x40;
    CHECK(!zr_snap_read(out, o, "z.szx", r, error, sizeof error));
    free(out);
    free(file);
    /* A Z80 version 1 file: the 30-byte header, 48 KiB compressed with an end marker. The image has runs, a lone ED and ED ED. */
    static uint8_t image[49152], packed[2 * 49152 + 16], v1[30 + 2 * 49152 + 16];
    for (unsigned i = 0; i < sizeof image; ++i) image[i] = (uint8_t)(i % 3 == 0 ? 0 : i / 700u);
    image[100] = 0xED; image[101] = 0x41;                   /* a single ED and the byte after it */
    image[200] = 0xED; image[201] = 0xED;                   /* two of them */
    memset(image + 300, 0xED, 9);                           /* a run of ED */
    memset(image + 400, 0, 600);                            /* a long run */
    size_t n = 0;
    for (size_t i = 0; i < sizeof image;) {
        size_t run = 1;
        while (i + run < sizeof image && image[i + run] == image[i] && run < 255) ++run;
        if (run >= 5 || (image[i] == 0xED && run >= 2)) {
            packed[n++] = 0xED; packed[n++] = 0xED; packed[n++] = (uint8_t)run; packed[n++] = image[i];
            i += run;
        } else {
            packed[n++] = image[i++];
            if (packed[n - 1] == 0xED) packed[n++] = image[i++];
        }
    }
    memcpy(packed + n, "\x00\xED\xED\x00", 4);
    n += 4;
    memset(v1, 0, 30);
    v1[0] = 0x12; v1[1] = 0x34; v1[6] = 0x00; v1[7] = 0x90; v1[8] = 0x00; v1[9] = 0xFF; v1[10] = 0x3F; v1[11] = 0x25;
    v1[12] = (uint8_t)(1 | 3 << 1 | 1 << 5);                /* R bit 7, border 3, compressed */
    v1[29] = 1;
    memcpy(v1 + 30, packed, n);
    CHECK(zr_snap_read(v1, 30 + n, "v1.z80", r, error, sizeof error));
    CHECK(r->pc == 0x9000 && r->sp == 0xFF00 && r->af == 0x1234 && r->r == 0xA5 && r->border == 3 && r->im == 1 && r->machine == ZR_SNAP_M_48K);
    CHECK(!memcmp(r->ram[5], image, 16384) && !memcmp(r->ram[2], image + 16384, 16384) && !memcmp(r->ram[0], image + 32768, 16384));
    CHECK(zr_snap_read(v1, 30 + n - 4, "v1.z80", r, error, sizeof error));                 /* the end marker may be missing */
    CHECK(!zr_snap_read(v1, 30 + n - 40, "v1.z80", r, error, sizeof error));               /* a cut one is not */
    v1[12] &= (uint8_t)~(1 << 5);
    CHECK(!zr_snap_read(v1, 30 + n, "v1.z80", r, error, sizeof error));                    /* not compressed and not 48 KiB */
    /* the format found by content when the name says nothing */
    CHECK(zr_snap_format_of_name("x.SNA") == ZR_SNAP_SNA && zr_snap_format_of_name("x.Z80") == ZR_SNAP_Z80 && zr_snap_format_of_name("x.tap") == ZR_SNAP_NONE);
    zr_snap *c = make(ZR_SNAP_M_48K, 0);
    CHECK(zr_snap_write(c, ZR_SNAP_SNA, &file, &size, error, sizeof error) && zr_snap_read(file, size, "noname", r, error, sizeof error) && r->format == ZR_SNAP_SNA);
    free(file);
    CHECK(zr_snap_write(c, ZR_SNAP_SZX, &file, &size, error, sizeof error) && zr_snap_read(file, size, "x.sna", r, error, sizeof error) && r->format == ZR_SNAP_SZX);
    free(file);
    zr_snap_free(c);
    zr_snap_free(r);
    zr_snap_free(s);
}

/* --- damaged files -------------------------------------------------------------------------------- */

static void test_damaged(void) {
    zr_snap *s = make(ZR_SNAP_M_128K, 3), *r = zr_snap_new();
    zr_snap *small = make(ZR_SNAP_M_48K, 0);
    char error[256];
    const unsigned formats[3] = {ZR_SNAP_SNA, ZR_SNAP_Z80, ZR_SNAP_SZX};
    const char *names[3] = {"d.sna", "d.z80", "d.szx"};
    for (unsigned pass = 0; pass < 2; ++pass)
        for (unsigned f = 0; f < 3; ++f) {
            uint8_t *file;
            size_t size;
            CHECK(zr_snap_write(pass ? small : s, formats[f], &file, &size, error, sizeof error));
            /* cut anywhere short of the end: refused with a message, never a crash */
            for (size_t cut = 0; cut < size; cut += (cut < 200 ? 1 : 1 + size / 97)) {
                error[0] = 0;
                if (formats[f] == ZR_SNAP_SNA && (cut == 49179u || cut == 131103u)) continue;     /* the size of another valid file */
                if (zr_snap_read(file, cut, names[f], r, error, sizeof error)) { printf("a %s cut at %zu of %zu was accepted\n", names[f], cut, size); ++failures; }
                else CHECK(error[0] != 0);
            }
            /* bytes changed: any answer, no crash and no endless loop */
            for (unsigned k = 0; k < 400; ++k) {
                uint8_t *copy = malloc(size);
                memcpy(copy, file, size);
                for (unsigned j = 0; j < 1 + k % 4; ++j) copy[(rnd() % (k % 2 ? 120u : (unsigned)size)) % size] ^= (uint8_t)(1u << (rnd() & 7u));
                (void)zr_snap_read(copy, size, names[f], r, error, sizeof error);
                free(copy);
            }
            free(file);
        }
    /* length fields that point past the file */
    uint8_t *file;
    size_t size;
    CHECK(zr_snap_write(small, ZR_SNAP_Z80, &file, &size, error, sizeof error));
    file[86] = 0xFE; file[87] = 0xFF;                       /* the first page's length */
    CHECK(!zr_snap_read(file, size, "d.z80", r, error, sizeof error) && strstr(error, "Z80"));
    free(file);
    CHECK(zr_snap_write(small, ZR_SNAP_SZX, &file, &size, error, sizeof error));
    file[12] = 0xFF; file[13] = 0xFF; file[14] = 0xFF; file[15] = 0x7F;                  /* the first block's size */
    CHECK(!zr_snap_read(file, size, "d.szx", r, error, sizeof error) && strstr(error, "SZX"));
    free(file);
    zr_snap_free(small);
    zr_snap_free(r);
    zr_snap_free(s);
}

/* --- .pok ------------------------------------------------------------------------------------------ */

static zr_pok parse(const char *text, int *ok, char *error, size_t capacity) {
    zr_pok pok;
    *ok = zr_pok_parse((const uint8_t *)text, strlen(text), &pok, error, capacity);
    return pok;
}

static void test_pok(void) {
    char error[128] = "";
    int ok;
    /* a good file with CRLF line ends, then trainers that are not: kept, marked, with their lines */
    zr_pok pok = parse("NInfinite lives\r\nM  8 32768 201   0\r\nZ  8 32769 0 201\r\n\r\nNAsk\r\nZ 8 40000 256 0\r\nNNo end\r\nM 8 40001 1 0\r\n"
                       "NBad number\r\nM 8 4x 1 0\r\nZ 8 40002 1 0\r\nNHigh value\r\nZ 8 40003 257 0\r\nNPage\r\nZ 1 20000 1 0\r\nNEmpty\r\nNLast\r\nZ 5 16383 9 7\r\nY\r\nN After the end\r\nZ 8 1 1 0\r\n",
                       &ok, error, sizeof error);
    CHECK(ok && pok.count == 8);
    CHECK(!strcmp(pok.trainers[0].name, "Infinite lives") && pok.trainers[0].usable && pok.trainers[0].count == 2 && pok.trainers[0].pokes[0].value == 201 &&
          pok.trainers[0].pokes[1].original == 201 && pok.trainers[0].pokes[0].line == 2);
    CHECK(pok.trainers[1].usable && pok.trainers[1].pokes[0].value == ZR_POK_ASK);
    CHECK(!pok.trainers[2].usable && pok.trainers[2].line == 8 && strstr(pok.trainers[2].problem, "Z"));           /* no Z line: its last line is named */
    CHECK(!pok.trainers[3].usable && pok.trainers[3].line == 10 && pok.trainers[3].count == 1);                      /* the bad line, then its Z kept */
    CHECK(!pok.trainers[4].usable && pok.trainers[4].line == 13);
    CHECK(!pok.trainers[5].usable && pok.trainers[5].line == 15 && strstr(pok.trainers[5].problem, "16 KiB"));
    CHECK(!pok.trainers[6].usable && strstr(pok.trainers[6].problem, "no pokes"));
    CHECK(pok.trainers[7].usable && pok.trainers[7].pokes[0].bank == 5 && pok.trainers[7].pokes[0].original == 7);
    zr_pok_free(&pok);
    /* no Y, no final newline, tabs, lower case letters, a poke that belongs to nothing */
    pok = parse("Z 8 100 1 0\nnTabs\nz\t8\t200\t2\t0", &ok, error, sizeof error);
    CHECK(ok && pok.count == 2 && !pok.trainers[0].usable && pok.trainers[1].usable && pok.trainers[1].pokes[0].address == 200);
    zr_pok_free(&pok);
    /* nothing usable to read */
    pok = parse("", &ok, error, sizeof error);
    CHECK(!ok && error[0]);
    pok = parse("just text\nand more\n", &ok, error, sizeof error);
    CHECK(!ok && strstr(error, "no trainers"));
    pok = parse("Y\n", &ok, error, sizeof error);
    CHECK(!ok);
    /* a long name is cut, a file of junk bytes does not crash */
    char name[400] = "N";
    memset(name + 1, 'a', 300);
    strcpy(name + 301, "\nZ 8 1 1 0\n");
    pok = parse(name, &ok, error, sizeof error);
    CHECK(ok && strlen(pok.trainers[0].name) == ZR_POK_NAME);
    zr_pok_free(&pok);
    for (unsigned k = 0; k < 300; ++k) {
        uint8_t junk[200];
        for (unsigned i = 0; i < sizeof junk; ++i) junk[i] = (uint8_t)("NMZY \r\n0123456789xQ"[rnd() % 20] | (rnd() % 17 == 0 ? 0x80 : 0));
        if (zr_pok_parse(junk, sizeof junk, &pok, error, sizeof error)) zr_pok_free(&pok);
    }
}

int main(void) {
    test_formats();
    test_other_forms();
    test_damaged();
    test_pok();
    if (failures) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("snapshots and .pok: all checks passed\n");
    return 0;
}
