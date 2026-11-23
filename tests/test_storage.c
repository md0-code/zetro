/* The storage models against image files and a fake host: the card images (raw, HDF, VHD footer, halved; refused when
 * truncated or odd-sized), the ATA channel (CHS and LBA writes and reads, IDENTIFY, a missing slave, out-of-range
 * errors, software reset, a state saved in the middle of a multi-sector read) and the SD card in SPI mode (the
 * initialisation sequence, CSD and CID, single and multiple block writes and reads on a standard and a high capacity
 * card, CRC checking, a state saved in the middle of a block). The images are made under the work folder; the large
 * one is sparse. No SDL. */
#include "ata.h"
#include "mmc.h"
#include "media/card.h"
#include "state.h"
#include "../src/host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

#define CLOCK 14000000u
#define DIR ZR_WORK "/tests/storage/"

static uint32_t lcg_state = 4242;
static uint32_t lcg(void) { return lcg_state = lcg_state * 1664525u + 1013904223u; }

static void fill(uint8_t *p, unsigned n, unsigned seed) {
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(seed * 31u + i * 7u + (i >> 8));
}

/* --- a fake board ------------------------------------------------------------------------------------ */

typedef struct fake {
    zr_hw_host host;
    uint64_t now, when;
    zr_dev *dev;
    int level[2];
} fake;

static uint64_t h_now(void *ctx) { return ((fake *)ctx)->now; }
static void h_schedule(void *ctx, void *dev, uint64_t when) { (void)dev; ((fake *)ctx)->when = when; }
static void h_line(void *ctx, void *dev, int pin, int state) { (void)dev; ((fake *)ctx)->level[pin] = state; }

static void fake_init(fake *f, zr_dev *dev) {
    memset(f, 0, sizeof *f);
    f->host.ctx = f;
    f->host.now = h_now;
    f->host.schedule = h_schedule;
    f->host.line = h_line;
    f->when = ZR_NEVER;
    f->dev = dev;
}

static void run_to(fake *f, uint64_t t) {
    for (unsigned guard = 0; f->when <= t; guard++) {
        if (guard > 100000) { CHECK(!"a deadline keeps coming at the same time"); break; }
        f->now = f->when;
        f->when = ZR_NEVER;
        f->dev->ops->event(f->dev, f->now);
    }
    f->now = t;
}
static void advance(fake *f, unsigned us) { run_to(f, f->now + (uint64_t)us * CLOCK / 1000000u); }

static int save_chip(zr_dev *dev, const char tag[4], zr_state_writer *w) {
    zr_sw_init(w);
    zr_sw_header(w, "test", "test", NULL, 0);
    zr_sw_begin(w, tag, 0);
    dev->ops->save(dev, w);
    zr_sw_end(w);
    return zr_sw_finish(w);
}
static int load_chip(zr_dev *dev, const char tag[4], const uint8_t *data, size_t size) {
    zr_state_reader reader;
    char error[64];
    return zr_sr_open(&reader, data, size, error, sizeof error) && zr_sr_chunk(&reader, tag, 0) && dev->ops->load(dev, &reader);
}

/* --- the images ----------------------------------------------------------------------------------------- */

static int write_file(const char *name, const uint8_t *data, size_t size) {
    char path[256], error[128];
    snprintf(path, sizeof path, DIR "%s", name);
    return zr_host_write_file(path, data, size, error, sizeof error);
}

static void test_images(void) {
    char error[160], path[256];
    uint8_t *hdf = NULL;
    size_t size = 0;
    int writable = 0;
    uint8_t sector[512], back[512];

    /* An HDF made by the module: version 1.0, 128-byte header, the IDENTIFY data of its size, sparse. */
    snprintf(path, sizeof path, DIR "img.hdf");
    CHECK(zr_card_create(path, ZR_CARD_HDF, 4u << 20, 1, error, sizeof error));
    CHECK(!zr_card_create(path, ZR_CARD_HDF, 4u << 20, 0, error, sizeof error) && error[0]);            /* not over an existing file */
    CHECK(!zr_card_create(path, ZR_CARD_RAW, 1000, 1, error, sizeof error) && error[0]);
    zr_card *c = zr_card_open(path, 0, error, sizeof error);
    CHECK(c && c->format == ZR_CARD_HDF && c->version == 0x10 && c->sectors == 8192 && c->identify_len == 106 && !c->halved);
    CHECK(c && c->cylinders == 8 && c->heads == 16 && c->spt == 63);
    if (c) {
        zr_card_describe(c, error, sizeof error);
        CHECK(strstr(error, "HDF 1.0") && strstr(error, "4 MiB"));
        CHECK(!zr_card_read(c, 8192, sector) && zr_card_read(c, 8191, sector));
        fill(sector, 512, 3);
        CHECK(zr_card_write(c, 8191, sector) && zr_card_flush(c));
        CHECK(zr_card_set_protect(c, 1) && !zr_card_write(c, 0, sector) && zr_card_set_protect(c, 0));
        zr_card_close(c);
        c = zr_card_open(path, 1, error, sizeof error);                                                    /* read-only */
        CHECK(c && !c->writable && zr_card_read(c, 8191, back) && !memcmp(sector, back, 512) && !zr_card_write(c, 0, sector));
        zr_card_close(c);
    }

    /* Damaged HDF files: every one gives a message, none a card. */
    CHECK(zr_host_read_file(path, 64u << 20, &hdf, &size, &writable, error, sizeof error) && size == (4u << 20) + 128);
    if (hdf) {
        static const size_t cuts[] = {0, 5, 21, 22, 127, 128, 129, (4u << 20) + 128 - 100, (4u << 20) + 128 - 512 * 200};
        for (unsigned i = 0; i < sizeof cuts / sizeof cuts[0]; i++) {
            CHECK(write_file("cut.hdf", hdf, cuts[i]));
            snprintf(path, sizeof path, DIR "cut.hdf");
            c = zr_card_open(path, 0, error, sizeof error);
            CHECK(!c && error[0]);
            zr_card_close(c);
        }
        uint8_t *bad = malloc(size);
        for (unsigned i = 0; bad && i < 4; i++) {                       /* a damaged signature, marker, version and data offset */
            memcpy(bad, hdf, size);
            if (i == 0) bad[1] = 'X'; else if (i == 1) bad[6] = 0; else if (i == 2) bad[7] = 0x22; else bad[9] = 4;
            CHECK(write_file("bad.hdf", bad, size));
            snprintf(path, sizeof path, DIR "bad.hdf");
            c = zr_card_open(path, 0, error, sizeof error);
            CHECK(!c && error[0]);
        }
        /* A halved image keeps 256 bytes of each sector: the low bytes; the high bytes read as zero. */
        memcpy(bad, hdf, 128);
        bad[8] = 1;
        for (unsigned s = 0; s < 8192; s++) fill(bad + 128 + s * 256u, 256, s);
        CHECK(write_file("halved.hdf", bad, 128 + 8192u * 256u));
        snprintf(path, sizeof path, DIR "halved.hdf");
        c = zr_card_open(path, 0, error, sizeof error);
        CHECK(c && c->halved && c->sectors == 8192);
        if (c) {
            uint8_t want[256];
            fill(want, 256, 77);
            CHECK(zr_card_read(c, 77, back));
            for (unsigned i = 0; i < 256; i++) CHECK(back[2 * i] == want[i] && back[2 * i + 1] == 0);
            fill(sector, 512, 9);
            CHECK(zr_card_write(c, 5, sector) && zr_card_flush(c));
            zr_card_close(c);
            c = zr_card_open(path, 0, error, sizeof error);
            CHECK(c && zr_card_read(c, 5, back));
            for (unsigned i = 0; c && i < 256; i++) CHECK(back[2 * i] == sector[2 * i] && back[2 * i + 1] == 0);
            zr_card_close(c);
        }
        free(bad);
    }
    zr_host_delete_file(DIR "img.hdf");
    zr_host_delete_file(DIR "cut.hdf");
    zr_host_delete_file(DIR "bad.hdf");
    zr_host_delete_file(DIR "halved.hdf");
    free(hdf);

    /* Raw images: an odd size, an empty file, a fixed VHD (the footer is not data) and a dynamic one. */
    static uint8_t raw[8192];
    fill(raw, sizeof raw, 1);
    CHECK(write_file("odd.img", raw, 1000));
    snprintf(path, sizeof path, DIR "odd.img");
    c = zr_card_open(path, 0, error, sizeof error);
    CHECK(!c && strstr(error, "512"));
    CHECK(write_file("odd.img", raw, 0));
    c = zr_card_open(path, 0, error, sizeof error);
    CHECK(!c && error[0]);
    uint8_t footer[512];
    memset(footer, 0, sizeof footer);
    memcpy(footer, "conectix", 8);
    footer[63] = 2;
    memcpy(raw + 7680, footer, 512);
    CHECK(write_file("fixed.vhd", raw, sizeof raw));
    snprintf(path, sizeof path, DIR "fixed.vhd");
    c = zr_card_open(path, 0, error, sizeof error);
    CHECK(c && c->format == ZR_CARD_RAW && c->sectors == 15);
    zr_card_close(c);
    raw[7680 + 63] = 3;
    CHECK(write_file("fixed.vhd", raw, sizeof raw));
    c = zr_card_open(path, 0, error, sizeof error);
    CHECK(!c && strstr(error, "VHD"));
    zr_host_delete_file(DIR "odd.img");
    zr_host_delete_file(DIR "fixed.vhd");
}

/* --- the ATA channel --------------------------------------------------------------------------------------- */

typedef struct ata_rig { fake f; zr_ata a; } ata_rig;

static uint8_t rd(ata_rig *r, unsigned reg) { return r->a.dev.ops->read(&r->a.dev, ZR_SPACE_IO, reg); }
static void wr(ata_rig *r, unsigned reg, unsigned v) { r->a.dev.ops->write(&r->a.dev, ZR_SPACE_IO, reg, (uint8_t)v); }

static void ata_rig_init(ata_rig *r) {
    memset(r, 0, sizeof *r);
    zr_ata_init(&r->a, &r->f.host, "ide", CLOCK);
    fake_init(&r->f, &r->a.dev);
}

static void set_chs(ata_rig *r, unsigned unit, unsigned c, unsigned h, unsigned s, unsigned count) {
    wr(r, 2, count); wr(r, 3, s); wr(r, 4, c & 255u); wr(r, 5, c >> 8); wr(r, 6, 0xA0u | unit << 4 | h);
}
static void set_lba(ata_rig *r, unsigned unit, uint32_t lba, unsigned count) {
    wr(r, 2, count); wr(r, 3, lba & 255u); wr(r, 4, lba >> 8 & 255u); wr(r, 5, lba >> 16 & 255u); wr(r, 6, 0xE0u | unit << 4 | (lba >> 24 & 15u));
}
/* A command: BSY must be visible to the first read, then it runs. */
static void command(ata_rig *r, unsigned cmd) {
    wr(r, 7, cmd);
    CHECK(rd(r, 8) == 0x80);
    advance(&r->f, 100);
}
static unsigned read_sectors(ata_rig *r, uint8_t *buf, unsigned max) {
    unsigned n = 0;
    while (n < max && (rd(r, 7) & 0x08)) {
        for (unsigned i = 0; i < 512; i++) buf[n * 512 + i] = rd(r, 0);
        n++;
        advance(&r->f, 100);
    }
    return n;
}
static void write_sectors(ata_rig *r, const uint8_t *buf, unsigned count) {
    for (unsigned s = 0; s < count; s++) {
        CHECK((rd(r, 7) & 0x88) == 0x08);
        for (unsigned i = 0; i < 512; i++) wr(r, 0, buf[s * 512 + i]);
        CHECK(rd(r, 7) == 0x80);                                            /* the drive is writing it */
        advance(&r->f, 100);
    }
}

static void test_ata(int format, const char *name) {
    char error[160], path[256];
    static uint8_t a[1024], b[1536], got[2048], ref[2048], id[512], one[512];
    snprintf(path, sizeof path, DIR "%s", name);
    CHECK(zr_card_create(path, format, 4u << 20, 1, error, sizeof error));
    zr_card *card = zr_card_open(path, 0, error, sizeof error);
    CHECK(card != NULL);
    if (!card) return;
    ata_rig r;
    ata_rig_init(&r);
    zr_ata_attach(&r.a, 0, card);
    r.a.dev.ops->reset(&r.a.dev);
    CHECK(rd(&r, 7) == 0x80);                                               /* the reset is still running */
    advance(&r.f, 200);
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 1) == 1 && rd(&r, 2) == 1 && rd(&r, 3) == 1 && rd(&r, 4) == 0 && rd(&r, 5) == 0 && rd(&r, 6) == 0xA0);

    /* A missing slave: selected, it reads as absent; the master is back when it is selected again. */
    wr(&r, 6, 0xB0);
    CHECK(rd(&r, 7) == 0 && rd(&r, 8) == 0 && rd(&r, 2) == 0 && rd(&r, 0) == 0);
    wr(&r, 2, 0x55);
    wr(&r, 6, 0xA0);
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 2) == 0x55);

    /* IDENTIFY: capacity, geometry and model as the image says. */
    command(&r, 0xEC);
    CHECK(rd(&r, 8) == 0x58 && r.f.level[ZR_ATA_PIN_INTRQ] && r.f.level[ZR_ATA_PIN_LIGHT]);
    CHECK(read_sectors(&r, id, 1) == 1);
    CHECK(rd(&r, 7) == 0x50 && !r.f.level[ZR_ATA_PIN_INTRQ] && !r.f.level[ZR_ATA_PIN_LIGHT]);
    unsigned sum = 0;
    for (unsigned i = 0; i < 512; i++) sum += id[i];
    const uint32_t capacity = (uint32_t)(id[120] | id[121] << 8 | id[122] << 16 | (unsigned)id[123] << 24);
    CHECK((sum & 255u) == 0 && capacity == 8192 && capacity == card->sectors);
    CHECK((id[2] | id[3] << 8) == 8 && (id[6] | id[7] << 8) == 16 && (id[12] | id[13] << 8) == 63 && (id[98] | id[99] << 8) == 0x0200);
    CHECK(id[55] == 'Z' && id[54] == 'e' && id[57] == 't' && id[56] == 'R');                 /* "ZetRo IDE disk", two characters to a word, the first in the high byte */

    /* Writes in CHS and in LBA; read back the other way round. */
    fill(a, 1024, 1);
    fill(b, 1536, 2);
    set_chs(&r, 0, 1, 2, 5, 2);
    command(&r, 0x30);
    CHECK(rd(&r, 8) == 0x58 && !r.f.level[ZR_ATA_PIN_INTRQ]);               /* the first block comes with no interrupt */
    write_sectors(&r, a, 2);
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 2) == 0 && rd(&r, 3) == 6 && rd(&r, 4) == 1 && rd(&r, 6) == 0xA2);
    set_lba(&r, 0, 5000, 3);
    command(&r, 0x30);
    write_sectors(&r, b, 3);
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 2) == 0 && rd(&r, 3) == (5002 & 255) && rd(&r, 4) == (5002 >> 8));
    set_lba(&r, 0, 1138, 2);                                                /* (1 * 16 + 2) * 63 + 5 - 1 */
    command(&r, 0x20);
    CHECK(rd(&r, 8) == 0x58 && r.f.level[ZR_ATA_PIN_INTRQ]);
    CHECK(read_sectors(&r, got, 4) == 2 && !memcmp(got, a, 1024));
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 2) == 0);
    set_chs(&r, 0, 4, 15, 24, 1);                                           /* LBA 5000 */
    command(&r, 0x20);
    CHECK(read_sectors(&r, got, 4) == 1 && !memcmp(got, b, 512));
    CHECK(zr_card_read(card, 1139, one) && !memcmp(one, a + 512, 512) && zr_ata_activity(&r.a, 0) >= 8);
    command(&r, 0x40);                                                      /* READ VERIFY of the same address */
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 2) == 0);

    /* 8-bit mode and the word interface read the same bytes. */
    wr(&r, 1, 0x01);
    command(&r, 0xEF);
    CHECK(rd(&r, 7) == 0x50);
    set_lba(&r, 0, 5000, 1);
    command(&r, 0x20);
    for (unsigned i = 0; i < 512; i++) got[i] = rd(&r, 0);
    CHECK(!memcmp(got, b, 512));
    advance(&r.f, 100);
    wr(&r, 1, 0x81);
    command(&r, 0xEF);
    set_lba(&r, 0, 5000, 1);
    command(&r, 0x20);
    for (unsigned i = 0; i < 256; i++) { const uint16_t w = zr_ata_read_data16(&r.a); got[2 * i] = (uint8_t)w; got[2 * i + 1] = (uint8_t)(w >> 8); }
    CHECK(!memcmp(got, b, 512) && rd(&r, 7) == 0x50);

    /* Out of range: the end of the drive, the geometry, an unknown command, a protected image. */
    set_lba(&r, 0, 8192, 1);
    command(&r, 0x20);
    CHECK(rd(&r, 7) == 0x51 && rd(&r, 1) == 0x10);
    set_lba(&r, 0, 8191, 2);                                                /* the first sector is there, the second is not */
    command(&r, 0x20);
    CHECK(read_sectors(&r, got, 4) == 1 && rd(&r, 7) == 0x51 && rd(&r, 1) == 0x10 && rd(&r, 2) == 1 && rd(&r, 4) == 0x20);
    set_chs(&r, 0, 9, 0, 1, 1);
    command(&r, 0x20);
    CHECK(rd(&r, 7) == 0x51 && rd(&r, 1) == 0x10);
    command(&r, 0xFF);
    CHECK(rd(&r, 7) == 0x51 && rd(&r, 1) == 0x04);
    CHECK(zr_card_set_protect(card, 1));
    set_lba(&r, 0, 10, 1);
    command(&r, 0x30);
    CHECK(rd(&r, 7) == 0x51 && rd(&r, 1) == 0x04);
    CHECK(zr_card_set_protect(card, 0));
    /* INITIALIZE DEVICE PARAMETERS changes the geometry CHS is translated by. */
    wr(&r, 2, 32); wr(&r, 6, 0xA0 | 7);                                     /* 32 sectors a track, 8 heads */
    command(&r, 0x91);
    CHECK(rd(&r, 7) == 0x50);
    set_chs(&r, 0, 1, 1, 1, 1);                                             /* (1 * 8 + 1) * 32 = 288 */
    command(&r, 0x20);
    CHECK(read_sectors(&r, got, 1) == 1);
    CHECK(rd(&r, 3) == 1 && zr_card_read(card, 288, one) && !memcmp(got, one, 512));

    /* Software reset in the middle of a read, and the reset pin. */
    set_lba(&r, 0, 100, 4);
    command(&r, 0x20);
    CHECK(rd(&r, 8) == 0x58);
    wr(&r, 8, 0x04);
    CHECK(rd(&r, 7) == 0x80 && rd(&r, 0) == 0xFF);
    wr(&r, 8, 0x00);
    CHECK(rd(&r, 7) == 0x80);
    advance(&r.f, 100);
    CHECK(rd(&r, 7) == 0x50 && rd(&r, 1) == 1 && rd(&r, 2) == 1 && rd(&r, 3) == 1 && rd(&r, 4) == 0 && rd(&r, 5) == 0);
    r.a.dev.ops->input(&r.a.dev, ZR_ATA_PIN_RESET, 1);
    CHECK(rd(&r, 7) == 0x80);
    r.a.dev.ops->input(&r.a.dev, ZR_ATA_PIN_RESET, 0);
    advance(&r.f, 100);
    CHECK(rd(&r, 7) == 0x50);
    wr(&r, 8, 0x02);                                                        /* nIEN: the pin stays off */
    command(&r, 0xEC);
    CHECK(!r.f.level[ZR_ATA_PIN_INTRQ] && read_sectors(&r, id, 1) == 1);
    wr(&r, 8, 0x00);

    /* A state saved in the middle of a multi-sector read, with an odd byte read, continues to the same bytes. */
    for (unsigned s = 0; s < 4; s++) CHECK(zr_card_read(card, 100 + s, ref + 512 * s));
    set_lba(&r, 0, 100, 4);
    command(&r, 0x20);
    for (unsigned i = 0; i < 512; i++) got[i] = rd(&r, 0);
    advance(&r.f, 100);
    for (unsigned i = 0; i < 101; i++) got[512 + i] = rd(&r, 0);
    zr_state_writer w;
    CHECK(save_chip(&r.a.dev, "ATA ", &w));
    const uint64_t saved_at = r.f.now;
    for (unsigned i = 101; i < 512; i++) got[512 + i] = rd(&r, 0);
    advance(&r.f, 100);
    CHECK(read_sectors(&r, got + 1024, 2) == 2 && !memcmp(got, ref, 2048));
    memset(got, 0, sizeof got);
    CHECK(load_chip(&r.a.dev, "ATA ", w.data, w.size));
    r.f.now = saved_at;
    memcpy(got, ref, 512);
    memcpy(got + 512, ref + 512, 101);
    for (unsigned i = 101; i < 512; i++) got[512 + i] = rd(&r, 0);
    advance(&r.f, 100);
    CHECK(read_sectors(&r, got + 1024, 2) == 2 && !memcmp(got, ref, 2048) && rd(&r, 7) == 0x50 && rd(&r, 2) == 0);
    /* The same in a second channel on the same card, and a damaged state never harms. */
    ata_rig r2;
    ata_rig_init(&r2);
    zr_ata_attach(&r2.a, 0, card);
    CHECK(load_chip(&r2.a.dev, "ATA ", w.data, w.size));
    r2.f.now = saved_at;
    run_to(&r2.f, saved_at);
    unsigned loaded = 0;
    for (unsigned i = 0; i < 80; i++) {
        uint8_t *copy = malloc(w.size);
        memcpy(copy, w.data, w.size);
        for (unsigned k = 0; k < 1 + i % 3; k++) copy[lcg() % w.size] = (uint8_t)lcg();
        if (load_chip(&r2.a.dev, "ATA ", copy, w.size)) {
            loaded++;
            run_to(&r2.f, r2.f.now + 5000);
            rd(&r2, 7); rd(&r2, 0); wr(&r2, 0, 1); wr(&r2, 7, 0x20);
            run_to(&r2.f, r2.f.now + 5000);
        }
        free(copy);
    }
    CHECK(loaded > 0);
    zr_ata_attach(&r2.a, 0, NULL);                                          /* a state of a channel with a drive is not loaded into one without */
    CHECK(!load_chip(&r2.a.dev, "ATA ", w.data, w.size));
    zr_sw_free(&w);

    /* Closed and opened again, the sectors are as written. */
    zr_ata_attach(&r.a, 0, NULL);
    zr_card_close(card);
    card = zr_card_open(path, 0, error, sizeof error);
    CHECK(card && card->sectors == 8192);
    if (card) {
        for (unsigned s = 0; s < 2; s++) CHECK(zr_card_read(card, 1138 + s, got) && !memcmp(got, a + 512 * s, 512));
        for (unsigned s = 0; s < 3; s++) CHECK(zr_card_read(card, 5000 + s, got) && !memcmp(got, b + 512 * s, 512));
        zr_card_close(card);
    }
    zr_host_delete_file(path);
}

/* --- the SD card ------------------------------------------------------------------------------------------- */

typedef struct mmc_rig { fake f; zr_mmc m; } mmc_rig;

static uint8_t crc7_ref(const uint8_t *p, unsigned n) {
    unsigned crc = 0;
    for (unsigned i = 0; i < n; i++)
        for (int bit = 7; bit >= 0; bit--) {
            const unsigned in = (p[i] >> bit & 1u) ^ (crc >> 6 & 1u);
            crc = (crc << 1 & 0x7Fu) ^ (in ? 0x09u : 0u);
        }
    return (uint8_t)crc;
}
static unsigned crc16_ref(const uint8_t *p, unsigned n) {
    unsigned crc = 0;
    for (unsigned i = 0; i < n; i++)
        for (int bit = 7; bit >= 0; bit--) {
            const unsigned in = (p[i] >> bit & 1u) ^ (crc >> 15 & 1u);
            crc = (crc << 1 & 0xFFFFu) ^ (in ? 0x1021u : 0u);
        }
    return crc;
}
/* The byte a command frame ends with on the wire: the CRC7 of its first five bytes and the stop bit. */
static unsigned wire_crc(const uint8_t *frame) { return (unsigned)(crc7_ref(frame, 5) << 1 | 1u); }

static uint8_t xc(mmc_rig *r, uint8_t out) { return zr_mmc_exchange(&r->m, out); }

/* A command: the frame, then up to ten bytes for the R1 (it has bit 7 clear). `crc` -1: the right one, -2: a wrong one. */
static uint8_t sd_cmd(mmc_rig *r, unsigned cmd, uint32_t arg, int crc) {
    uint8_t f[6] = {(uint8_t)(0x40u | cmd), (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg, 0};
    f[5] = crc >= 0 ? (uint8_t)crc : (uint8_t)(wire_crc(f) ^ (crc == -2 ? 0x10u : 0u));
    for (unsigned i = 0; i < 6; i++) xc(r, f[i]);
    for (unsigned i = 0; i < 10; i++) {
        const uint8_t v = xc(r, 0xFF);
        if (!(v & 0x80)) return v;
    }
    return 0xFF;
}
static unsigned sd_acmd(mmc_rig *r, unsigned cmd, uint32_t arg) {
    sd_cmd(r, 55, 0, -1);
    return sd_cmd(r, cmd, arg, -1);
}
/* The data block after a command's R1: the token, `n` bytes, the CRC16; 1 when all of it is right. */
static int sd_block(mmc_rig *r, uint8_t *data, unsigned n) {
    unsigned i = 0;
    uint8_t v = 0xFF;
    for (; i < 20 && (v = xc(r, 0xFF)) == 0xFF; i++) {}
    if (v != 0xFE) return 0;
    for (i = 0; i < n; i++) data[i] = xc(r, 0xFF);
    const unsigned crc = xc(r, 0xFF) << 8;
    return (crc | xc(r, 0xFF)) == crc16_ref(data, n);
}
/* A block sent: the token, the data, the CRC16 (`bad` flips a bit of it), the response token, then wait out the busy bytes. */
static unsigned sd_send(mmc_rig *r, uint8_t token, const uint8_t *data, int bad) {
    unsigned crc = crc16_ref(data, 512);
    if (bad) crc ^= 1;
    xc(r, token);
    for (unsigned i = 0; i < 512; i++) xc(r, data[i]);
    xc(r, (uint8_t)(crc >> 8));
    xc(r, (uint8_t)crc);
    unsigned response = 0xFF;
    for (unsigned i = 0; i < 4 && response == 0xFF; i++) response = xc(r, 0xFF);
    unsigned busy = 0;
    while (xc(r, 0xFF) != 0xFF && busy < 1000) busy++;
    CHECK(busy > 0 && busy < 1000);
    return response & 0x1Fu;
}
static uint32_t bits(const uint8_t *reg, unsigned hi, unsigned lo) {
    uint32_t v = 0;
    for (unsigned i = hi + 1; i-- > lo;) v = v << 1 | (uint32_t)((reg[15 - i / 8] >> (i % 8)) & 1u);
    return v;
}

static void test_sd(const char *name, uint64_t bytes, int hc) {
    char error[160], path[256];
    static uint8_t a[512], b[4 * 512], got[4 * 512], ref[512], reg[16];
    uint8_t ext[5];
    snprintf(path, sizeof path, DIR "%s", name);
    CHECK(zr_card_create(path, ZR_CARD_RAW, bytes, 1, error, sizeof error));
    zr_card *card = zr_card_open(path, 0, error, sizeof error);
    CHECK(card && card->sectors == bytes / 512);
    if (!card) return;
    mmc_rig r;
    memset(&r, 0, sizeof r);
    zr_mmc_init(&r.m, &r.f.host, "sd");
    fake_init(&r.f, &r.m.dev);
    CHECK(zr_mmc_attach(&r.m, card, ZR_MMC_SD, error, sizeof error) && r.m.hc == hc);
    r.m.dev.ops->reset(&r.m.dev);
    zr_mmc_select(&r.m, 1);
    const uint64_t sectors = card->sectors;
    const uint32_t step = hc ? 1u : 512u;                                    /* an address of a block: its number or its byte */

    /* The standard sequence. */
    static const uint8_t cmd0[5] = {0x40, 0, 0, 0, 0}, cmd8[5] = {0x48, 0, 0, 1, 0xAA}, cmd55[5] = {0x77, 0, 0, 0, 0};
    CHECK(wire_crc(cmd0) == 0x95 && wire_crc(cmd8) == 0x87 && wire_crc(cmd55) == 0x65 && crc16_ref((const uint8_t *)"123456789", 9) == 0x31C3);
    for (unsigned i = 0; i < 10; i++) xc(&r, 0xFF);
    zr_mmc_select(&r.m, 0);
    CHECK(zr_mmc_exchange(&r.m, 0x40) == 0xFF);                              /* not selected: nothing happens */
    zr_mmc_select(&r.m, 1);
    CHECK(sd_cmd(&r, 0, 0, 0x95) == 0x01);
    CHECK(sd_cmd(&r, 17, 0, -1) == 0x05);                                    /* a read in the idle state is illegal */
    CHECK(sd_cmd(&r, 8, 0x1AA, 0x87) == 0x01);
    for (unsigned i = 0; i < 4; i++) ext[i] = xc(&r, 0xFF);
    CHECK(ext[0] == 0 && ext[1] == 0 && ext[2] == 1 && ext[3] == 0xAA);
    unsigned polls = 0;
    while (sd_acmd(&r, 41, 0x40000000u) != 0x00 && polls < 100) polls++;
    CHECK(polls == 2);
    CHECK(sd_cmd(&r, 58, 0, -1) == 0x00);
    for (unsigned i = 0; i < 4; i++) ext[i] = xc(&r, 0xFF);
    CHECK((ext[0] & 0x80) && ((ext[0] >> 6 & 1) == (hc != 0)) && ext[1] == 0xFF && ext[2] == 0x80);
    CHECK(sd_cmd(&r, 16, 512, -1) == 0x00);

    /* CSD and CID: the capacity of the image, the CRC7 of the register, the CRC16 of the block. */
    CHECK(sd_cmd(&r, 9, 0, -1) == 0x00 && sd_block(&r, reg, 16));
    CHECK((reg[15] >> 1) == crc7_ref(reg, 15) && (reg[15] & 1));
    uint64_t capacity;
    if (bits(reg, 127, 126) == 1) capacity = ((uint64_t)bits(reg, 69, 48) + 1) * 512u * 1024u;
    else capacity = ((uint64_t)bits(reg, 73, 62) + 1) << (bits(reg, 49, 47) + 2) << bits(reg, 83, 80);
    CHECK(capacity == bytes && (bits(reg, 127, 126) == 1) == (hc != 0) && bits(reg, 83, 80) == 9);
    CHECK(sd_cmd(&r, 10, 0, -1) == 0x00 && sd_block(&r, reg, 16) && reg[1] == 'Z' && reg[2] == 'R' && (reg[15] >> 1) == crc7_ref(reg, 15));

    /* A block written and read back at the end of the card; the file has it. */
    const uint64_t last = sectors - 1;
    fill(a, 512, 11);
    CHECK(sd_cmd(&r, 24, (uint32_t)(last * step), -1) == 0x00);
    CHECK(r.f.level[0] == 0);
    CHECK(sd_send(&r, 0xFE, a, 0) == 0x05);
    CHECK(zr_card_read(card, last, ref) && !memcmp(ref, a, 512));
    CHECK(sd_cmd(&r, 17, (uint32_t)(last * step), -1) == 0x00 && r.f.level[0] == 1);
    CHECK(sd_block(&r, got, 512) && !memcmp(got, a, 512) && r.f.level[0] == 0);
    CHECK(sd_cmd(&r, 17, (uint32_t)(sectors * step), -1) == 0x40);           /* beyond the card */
    if (!hc) CHECK(sd_cmd(&r, 17, 513, -1) == 0x20);                         /* not on a block boundary */
    CHECK(sd_cmd(&r, 13, 0, -1) == 0x00 && xc(&r, 0xFF) == 0x80);            /* the out of range seen above */

    /* A multiple block write, announced by ACMD23, and a multiple block read stopped by CMD12. */
    const uint64_t first = hc ? sectors - 100 : 2000;
    fill(b, sizeof b, 5);
    CHECK(sd_acmd(&r, 23, 4) == 0x00);
    CHECK(sd_cmd(&r, 25, (uint32_t)(first * step), -1) == 0x00);
    for (unsigned i = 0; i < 4; i++) CHECK(sd_send(&r, 0xFC, b + 512 * i, 0) == 0x05);
    xc(&r, 0xFD);
    unsigned busy = 0;
    while (xc(&r, 0xFF) != 0xFF && busy < 100) busy++;
    CHECK(busy > 0 && busy < 100);
    for (unsigned i = 0; i < 4; i++) CHECK(zr_card_read(card, first + i, ref) && !memcmp(ref, b + 512 * i, 512));
    CHECK(sd_acmd(&r, 22, 0) == 0x00 && sd_block(&r, ext, 4) && ext[3] == 4);
    CHECK(sd_cmd(&r, 18, (uint32_t)(first * step), -1) == 0x00);
    for (unsigned i = 0; i < 3; i++) CHECK(sd_block(&r, got + 512 * i, 512));
    CHECK(!memcmp(got, b, 3 * 512));
    sd_cmd(&r, 12, 0, -1);                                                   /* the stuff byte is the FF before the R1 */
    for (unsigned i = 0; i < 10; i++) xc(&r, 0xFF);
    CHECK(sd_cmd(&r, 13, 0, -1) == 0x00 && xc(&r, 0xFF) == 0x00);
    /* ACMD51 and ACMD13 answer with their blocks. */
    CHECK(sd_acmd(&r, 51, 0) == 0x00 && sd_block(&r, reg, 8) && reg[0] == 0x02);
    CHECK(sd_acmd(&r, 13, 0) == 0x00);
    xc(&r, 0xFF);
    CHECK(sd_block(&r, got, 64));

    /* CRC on: a command with a bad CRC is refused, so is a block with a bad CRC and it is not written. */
    CHECK(sd_cmd(&r, 59, 1, -1) == 0x00);
    CHECK(sd_cmd(&r, 17, (uint32_t)(first * step), -2) == 0x08);
    CHECK(sd_cmd(&r, 17, (uint32_t)(first * step), -1) == 0x00 && sd_block(&r, got, 512) && !memcmp(got, b, 512));
    fill(a, 512, 99);
    CHECK(sd_cmd(&r, 24, (uint32_t)(first * step), -1) == 0x00);
    CHECK(sd_send(&r, 0xFE, a, 1) == 0x0B);
    CHECK(zr_card_read(card, first, ref) && !memcmp(ref, b, 512));
    CHECK(sd_cmd(&r, 24, (uint32_t)(first * step), -1) == 0x00 && sd_send(&r, 0xFE, a, 0) == 0x05 && zr_card_read(card, first, ref) && !memcmp(ref, a, 512));
    /* A write-protected image: the block is refused. */
    CHECK(zr_card_set_protect(card, 1));
    CHECK(sd_cmd(&r, 9, 0, -1) == 0x00 && sd_block(&r, reg, 16) && bits(reg, 12, 12) == 1);
    CHECK(sd_cmd(&r, 24, (uint32_t)(first * step), -1) == 0x00 && sd_send(&r, 0xFE, b, 0) == 0x0D);
    CHECK(zr_card_set_protect(card, 0));

    /* A state saved in the middle of a block read continues to the same bytes; a damaged one never harms. */
    CHECK(sd_cmd(&r, 17, (uint32_t)(first * step), -1) == 0x00);
    unsigned v = 0xFF;
    for (unsigned i = 0; i < 20 && v == 0xFF; i++) v = xc(&r, 0xFF);
    CHECK(v == 0xFE);
    for (unsigned i = 0; i < 101; i++) got[i] = xc(&r, 0xFF);
    zr_state_writer w;
    CHECK(save_chip(&r.m.dev, "MMC ", &w));
    for (unsigned i = 101; i < 512; i++) got[i] = xc(&r, 0xFF);
    unsigned crc = xc(&r, 0xFF) << 8;
    crc |= xc(&r, 0xFF);
    CHECK(!memcmp(got, a, 512) && crc == crc16_ref(a, 512));
    CHECK(load_chip(&r.m.dev, "MMC ", w.data, w.size));
    memset(b, 0, 512);
    memcpy(b, got, 101);
    for (unsigned i = 101; i < 512; i++) b[i] = xc(&r, 0xFF);
    crc = xc(&r, 0xFF) << 8;
    crc |= xc(&r, 0xFF);
    CHECK(!memcmp(b, a, 512) && crc == crc16_ref(a, 512) && xc(&r, 0xFF) == 0xFF && r.f.level[0] == 0);
    unsigned loaded = 0;
    for (unsigned i = 0; i < 80; i++) {
        uint8_t *copy = malloc(w.size);
        memcpy(copy, w.data, w.size);
        for (unsigned k = 0; k < 1 + i % 3; k++) copy[lcg() % w.size] = (uint8_t)lcg();
        if (load_chip(&r.m.dev, "MMC ", copy, w.size)) {
            loaded++;
            for (unsigned k = 0; k < 600; k++) xc(&r, (uint8_t)(k & 1 ? 0xFF : 0x41));
        }
        free(copy);
    }
    CHECK(loaded > 0);
    zr_sw_free(&w);

    /* An MMC card: CMD1 initialises it, CMD8 and CMD55 are refused. */
    if (!hc) {
        CHECK(zr_mmc_attach(&r.m, card, ZR_MMC_MMC, error, sizeof error));
        zr_mmc_select(&r.m, 1);
        CHECK(sd_cmd(&r, 0, 0, 0x95) == 0x01 && sd_cmd(&r, 8, 0x1AA, 0x87) == 0x05 && sd_cmd(&r, 55, 0, -1) == 0x05);
        polls = 0;
        while (sd_cmd(&r, 1, 0, -1) != 0x00 && polls < 100) polls++;
        CHECK(polls == 2 && sd_cmd(&r, 17, (uint32_t)(first * step), -1) == 0x00 && sd_block(&r, got, 512) && !memcmp(got, a, 512));
        CHECK(sd_cmd(&r, 9, 0, -1) == 0x00 && sd_block(&r, reg, 16) && bits(reg, 127, 126) == 2);
    } else {
        CHECK(!zr_mmc_attach(&r.m, card, ZR_MMC_MMC, error, sizeof error) && error[0]);
    }
    zr_mmc_detach(&r.m);
    CHECK(zr_mmc_exchange(&r.m, 0xFF) == 0xFF);
    zr_card_close(card);
    zr_host_delete_file(path);
}

int main(void) {
    zr_host_make_folders(DIR);
    test_images();
    test_ata(ZR_CARD_RAW, "ide.img");
    test_ata(ZR_CARD_HDF, "ide.hdf");
    test_sd("sdsc.img", 16u << 20, 0);
    test_sd("sdhc.img", (uint64_t)3 << 30, 1);
    if (failures) printf("%d checks failed\n", failures);
    else printf("storage: ok\n");
    return failures ? 1 : 0;
}
