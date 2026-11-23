/* The floppy parts against in-memory images and a fake host: the disk formats (DSK, Extended DSK with its status
 * bytes and weak sectors, raw images; write back; damaged files), the drive's spindle and track timing, the
 * uPD765 with and without DMA (reads, errors recorded in the image, writes, format, scan, terminal count, overrun,
 * a state saved in the middle of a transfer) and the drive sound. No SDL, no files. */
#include "drivesnd.h"
#include "fdd.h"
#include "media/disk.h"
#include "state.h"
#include "upd765.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

#define CLOCK 14000000u

/* --- building images ------------------------------------------------------------------------ */

typedef struct buf { uint8_t d[8192]; size_t n; } buf;

static void sector_info(uint8_t *e, unsigned c, unsigned h, unsigned r, unsigned n, unsigned st1, unsigned st2, unsigned len) {
    e[0] = (uint8_t)c; e[1] = (uint8_t)h; e[2] = (uint8_t)r; e[3] = (uint8_t)n; e[4] = (uint8_t)st1; e[5] = (uint8_t)st2;
    e[6] = (uint8_t)len; e[7] = (uint8_t)(len >> 8);
}

/* Extended DSK: 2 cylinders, 1 side; track 0 has four sectors (256 bytes, a CRC error in the data, three copies of
 * 128 bytes, a deleted 512), track 1 is not there. */
static void build_edsk(buf *b) {
    memset(b, 0, sizeof *b);
    memcpy(b->d, "EXTENDED CPC DSK File\r\nDisk-Info\r\n", 34);
    memcpy(b->d + 0x22, "TEST", 4);
    b->d[0x30] = 2; b->d[0x31] = 1; b->d[0x34] = 7;
    uint8_t *t = b->d + 256;
    memcpy(t, "Track-Info\r\n", 12);
    t[0x12] = 1; t[0x13] = 2; t[0x14] = 1; t[0x15] = 4; t[0x16] = 0x4E; t[0x17] = 0xE5;
    sector_info(t + 0x18, 0, 0, 1, 1, 0, 0, 256);
    sector_info(t + 0x20, 0, 0, 2, 1, 0x20, 0x20, 256);
    sector_info(t + 0x28, 0, 0, 3, 0, 0, 0, 384);
    sector_info(t + 0x30, 0, 0, 4, 2, 0, 0x40, 512);
    memset(t + 0x100, 0x11, 256);
    memset(t + 0x200, 0x22, 256);
    for (unsigned i = 0; i < 384; i++) t[0x300 + i] = (uint8_t)(0x30 + i / 128);
    memset(t + 0x480, 0x44, 512);
    b->n = 256 + 7 * 256;
}

/* Standard DSK: 2 cylinders, 1 side, 3 sectors of 256 bytes a track, track size 1024. */
static void build_dsk(buf *b) {
    memset(b, 0, sizeof *b);
    memcpy(b->d, "MV - CPCEMU Disk-File\r\nDisk-Info\r\n", 34);
    b->d[0x30] = 2; b->d[0x31] = 1; b->d[0x33] = 4;
    for (unsigned c = 0; c < 2; c++) {
        uint8_t *t = b->d + 256 + c * 1024;
        memcpy(t, "Track-Info\r\n", 12);
        t[0x10] = (uint8_t)c; t[0x14] = 1; t[0x15] = 3; t[0x16] = 0x4E; t[0x17] = 0xE5;
        for (unsigned s = 0; s < 3; s++) {
            sector_info(t + 0x18 + 8 * s, c, 0, s + 1, 1, 0, 0, 0);
            memset(t + 0x100 + 256 * s, (int)(c * 16 + s + 1), 256);
        }
    }
    b->n = 256 + 2 * 1024;
}

static zr_disk *open_buf(const uint8_t *d, size_t n, const zr_disk_geom *hint, int writable, char *error) {
    zr_media *m = zr_media_from("test.img", d, n);
    m->writable = writable;
    zr_disk *disk = zr_disk_open(m, hint, error, 160);
    zr_media_close(m);
    return disk;
}

/* --- the disk -------------------------------------------------------------------------------- */

static uint32_t lcg_state = 12345;
static uint32_t lcg(void) { return lcg_state = lcg_state * 1664525u + 1013904223u; }

static void test_disk(void) {
    char error[160];
    buf b;
    build_edsk(&b);
    zr_disk *d = open_buf(b.d, b.n, NULL, 1, error);
    CHECK(d && d->format == ZR_DISK_EDSK && d->cylinders == 2 && d->sides == 1);
    if (!d) return;
    const zr_track *t = zr_disk_track(d, 0, 0);
    CHECK(t && t->count == 4 && t->encoding == ZR_ENC_MFM && t->rate == ZR_RATE_DD && t->gap3 == 0x4E && t->filler == 0xE5);
    CHECK(zr_disk_track(d, 1, 0) == NULL);
    if (t) {
        CHECK(t->sector[1].st1 == 0x20 && t->sector[1].st2 == 0x20);
        CHECK(t->sector[2].copies == 3 && t->sector[2].size == 128 && t->sector[2].stored == 384);
        CHECK(t->sector[3].st2 == 0x40 && t->sector[3].size == 512);
        uint8_t one[128];
        zr_disk_read_sector(&t->sector[2], 1, one, 128, 0);
        CHECK(one[0] == 0x31 && one[127] == 0x31);
        uint8_t pad[300];
        zr_disk_read_sector(&t->sector[0], 0, pad, 300, 0xE5);       /* a recorded length shorter than asked: padded */
        CHECK(pad[255] == 0x11 && pad[256] == 0xE5 && pad[299] == 0xE5);
    }
    /* A sector write changes the disk and is written back; the weak sector and the status bytes survive. */
    uint8_t data[256];
    memset(data, 0x77, sizeof data);
    CHECK(zr_disk_write_sector(d, 0, 0, 0, data, 256, 0) == ZR_DISK_OK && d->dirty);
    CHECK(zr_disk_write_sector(d, 0, 0, 1, data, 256, 0) == ZR_DISK_OK && d->track[0][0].sector[1].st1 == 0 && d->track[0][0].sector[1].st2 == 0);
    zr_media *out = zr_media_from("out.dsk", NULL, 0);
    CHECK(zr_disk_save_media(d, out, error, sizeof error) && out->dirty && !d->dirty && out->size > 256);
    zr_disk *e = open_buf(out->data, out->size, NULL, 1, error);
    CHECK(e && e->format == ZR_DISK_EDSK && e->track[0][0].count == 4 && e->track[0][0].sector[0].data[5] == 0x77);
    CHECK(e && e->track[0][0].sector[2].copies == 3 && e->track[0][0].sector[3].st2 == 0x40);
    zr_disk_close(e);
    zr_media_close(out);
    /* Write protection follows the media. */
    zr_disk *ro = open_buf(b.d, b.n, NULL, 0, error);
    CHECK(ro && zr_disk_write_protected(ro) && !zr_disk_set_protect(ro, 0) && zr_disk_write_sector(ro, 0, 0, 0, data, 256, 0) == ZR_DISK_PROTECTED);
    zr_disk_close(ro);
    CHECK(zr_disk_set_protect(d, 1) && zr_disk_write_sector(d, 0, 0, 0, data, 256, 0) == ZR_DISK_PROTECTED);
    zr_disk_close(d);

    /* The standard format stays standard until a track no longer fits it. */
    build_dsk(&b);
    d = open_buf(b.d, b.n, NULL, 1, error);
    CHECK(d && d->format == ZR_DISK_DSK && d->track[1][0].sector[2].data[0] == 0x13);
    if (d) {
        out = zr_media_from("out.dsk", NULL, 0);
        CHECK(zr_disk_write_sector(d, 1, 0, 2, data, 256, 1) == ZR_DISK_OK);
        CHECK(zr_disk_save_media(d, out, error, sizeof error) && out->size == b.n && !memcmp(out->data, "MV - CPC", 8));
        zr_media_close(out);
        zr_disk_id ids[10];
        for (unsigned i = 0; i < 10; i++) { ids[i].c = 0; ids[i].h = 0; ids[i].r = (uint8_t)(i + 1); ids[i].n = 1; }
        zr_disk_format f = {ZR_ENC_MFM, ZR_RATE_DD, 1, 0x20, 0xF6, 10, ids};
        CHECK(zr_disk_format_track(d, 0, 0, &f) == ZR_DISK_OK);
        out = zr_media_from("out.dsk", NULL, 0);
        CHECK(zr_disk_save_media(d, out, error, sizeof error) && !memcmp(out->data, "EXTENDED", 8) && d->format == ZR_DISK_EDSK);
        zr_disk *again = open_buf(out->data, out->size, NULL, 1, error);
        CHECK(again && again->track[0][0].count == 10 && again->track[0][0].sector[9].data[100] == 0xF6 && again->track[1][0].sector[2].st2 == 0x40);
        zr_disk_close(again);
        zr_media_close(out);
        zr_disk_close(d);
    }

    /* Raw images: the size names the geometry, a hint chooses among several, zero is blank, a changed layout is refused. */
    static uint8_t raw[368640];
    memset(raw, 0, sizeof raw);
    d = open_buf(raw, 327680, NULL, 1, error);
    CHECK(d && d->format == ZR_DISK_RAW && zr_disk_track(d, 0, 0) == NULL && d->geom.sectors == 16);          /* blank: unformatted */
    zr_disk_close(d);
    raw[0] = 1;
    d = open_buf(raw, 327680, NULL, 1, error);
    CHECK(d && d->geom.sectors == 16 && d->geom.sector_size == 256 && zr_disk_track(d, 0, 0)->count == 16);
    zr_disk_close(d);
    const zr_disk_geom pc = {40, 2, 8, 512, ZR_ENC_MFM};
    d = open_buf(raw, 327680, &pc, 1, error);
    CHECK(d && d->geom.sectors == 8 && d->geom.sector_size == 512);
    zr_disk_close(d);
    d = open_buf(raw, 163840, NULL, 1, error);
    CHECK(d && d->geom.cylinders == 40 && d->geom.sides == 1 && d->geom.sectors == 8);
    if (d) {
        memset(data, 0x5A, sizeof data);
        uint8_t big[512];
        memset(big, 0x5A, sizeof big);
        CHECK(zr_disk_write_sector(d, 3, 0, 4, big, 512, 0) == ZR_DISK_OK);
        out = zr_media_from("out.img", NULL, 0);
        CHECK(zr_disk_save_media(d, out, error, sizeof error) && out->size == 163840 && out->data[(3 * 8 + 4) * 512 + 100] == 0x5A && out->data[0] == 1);
        zr_media_close(out);
        zr_disk_id ids[5];
        for (unsigned i = 0; i < 5; i++) { ids[i].c = 2; ids[i].h = 0; ids[i].r = (uint8_t)(i + 1); ids[i].n = 2; }
        zr_disk_format f = {ZR_ENC_MFM, ZR_RATE_DD, 2, 0x20, 0xE5, 5, ids};
        CHECK(zr_disk_format_track(d, 2, 0, &f) == ZR_DISK_OK);
        out = zr_media_from("out.img", NULL, 0);
        CHECK(!zr_disk_check_save(d, error, sizeof error) && error[0] && !zr_disk_save_media(d, out, error, sizeof error) && !out->dirty);
        zr_media_close(out);
        zr_disk_close(d);
    }
    /* A new disk, in each format, reads back as made. */
    const zr_disk_geom g = {40, 2, 9, 512, ZR_ENC_MFM};
    for (int format = ZR_DISK_RAW; format <= ZR_DISK_EDSK; format++) {
        d = zr_disk_new(&g, format, 1, 0xE5);
        CHECK(d != NULL);
        if (!d) continue;
        out = zr_media_from("new", NULL, 0);
        CHECK(zr_disk_save_media(d, out, error, sizeof error));
        zr_disk *back = open_buf(out->data, out->size, NULL, 1, error);
        CHECK(back && back->format == format && back->cylinders == 40 && zr_disk_track(back, 39, 1)->count == 9 && zr_disk_track(back, 39, 1)->sector[8].data[511] == 0xE5);
        zr_disk_close(back);
        zr_media_close(out);
        zr_disk_close(d);
    }

    /* Damaged files give a message or a disk, never a crash: every truncation and a run of corruptions. */
    unsigned refused = 0;
    for (int kind = 0; kind < 2; kind++) {
        if (kind) build_dsk(&b); else build_edsk(&b);
        for (size_t n = 0; n < b.n; n++) {
            zr_disk *x = open_buf(b.d, n, NULL, 1, error);
            if (!x) refused++;
            zr_disk_close(x);
        }
        for (unsigned i = 0; i < 400; i++) {
            buf c = b;
            for (unsigned k = 0; k < 1 + i % 4; k++) c.d[lcg() % 600u] = (uint8_t)lcg();
            zr_disk *x = open_buf(c.d, c.n, NULL, 1, error);
            if (!x) CHECK(error[0] != 0);
            zr_disk_close(x);
        }
    }
    CHECK(refused > 100);
}

/* --- the drive ------------------------------------------------------------------------------- */

typedef struct hooks { unsigned motor, step, stop, load, hold; uint64_t last; } hooks;
static void on_drive(void *ctx, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b) {
    hooks *h = ctx;
    (void)drive; (void)a;
    switch (event) {
    case ZR_DRIVE_MOTOR: h->motor++; break;
    case ZR_DRIVE_STEP: h->step++; h->stop += b; break;
    case ZR_DRIVE_HEAD_LOAD: h->load++; break;
    case ZR_DRIVE_HEAD_HOLD: h->hold++; break;
    default: break;
    }
    h->last = t;
}

static zr_disk *test_disk_9x512(void) {
    const zr_disk_geom g = {5, 2, 9, 512, ZR_ENC_MFM};
    return zr_disk_new(&g, ZR_DISK_EDSK, 1, 0xE5);
}

static void test_fdd(void) {
    zr_fdd f;
    hooks h;
    memset(&h, 0, sizeof h);
    zr_fdd_init(&f, CLOCK, 1, 80, 300);
    zr_fdd_set_hook(&f, on_drive, &h, 0);
    zr_disk *disk = test_disk_9x512();
    char error[160];
    CHECK(zr_fdd_insert(&f, disk, error, sizeof error) && zr_fdd_changed(&f) && zr_fdd_has_disk(&f));
    CHECK(!zr_fdd_rotating(&f, 0) && zr_fdd_rotating_at(&f) == ZR_NEVER);
    zr_fdd_set_motor(&f, 1, 1000);
    CHECK(f.speed_at == 1000 + 5600000u && !zr_fdd_rotating(&f, f.speed_at - 1) && zr_fdd_rotating(&f, f.speed_at));
    CHECK(zr_fdd_index(&f, f.speed_at) && !zr_fdd_index(&f, f.speed_at + 100000) && zr_fdd_index(&f, f.speed_at + zr_fdd_rev(&f)));
    CHECK(zr_fdd_rev(&f) == 2800000u && zr_fdd_rev_units(&f) == 6250u && zr_fdd_ticks(&f, 1) == 448);
    /* Steps: towards the spindle, away, against the stop. */
    CHECK(zr_fdd_step(&f, 1, 2000) == 1 && !zr_fdd_changed(&f) && !zr_fdd_track0(&f));
    CHECK(zr_fdd_step(&f, -1, 2001) == 0 && zr_fdd_track0(&f));
    zr_fdd_step(&f, -1, 2002);
    CHECK(h.motor == 1 && h.step == 3 && h.stop == 1);
    /* The sectors pass in order, once a revolution, and an FM read sees none on an MFM track. */
    const uint64_t t0 = f.speed_at;
    uint64_t from = t0;
    zr_fdd_slot s;
    unsigned in_order = 1;
    for (unsigned i = 0; i < 10; i++) {
        if (!zr_fdd_find_id(&f, 0, 0, from, &s) || s.index != i % 9 || (unsigned)s.sector->r != i % 9 + 1) { in_order = 0; break; }
        const uint64_t id_end = zr_fdd_slot_time(&s, s.id_end), data = zr_fdd_slot_time(&s, s.data), end = zr_fdd_slot_time(&s, s.data_end);
        if (!(s.t0 >= from && id_end > s.t0 && data > id_end && end > data)) in_order = 0;
        if (end - data < 514 * 448 - 4 || end - data > 514 * 448 + 4) in_order = 0;
        from = id_end;
    }
    CHECK(in_order && s.t0 >= t0 + zr_fdd_rev(&f));
    CHECK(!zr_fdd_find_id(&f, 0, 1, t0, &s) && zr_fdd_find_id(&f, 1, 0, t0, &s));
    f.cyl = 9;
    CHECK(!zr_fdd_find_id(&f, 0, 0, t0, &s));                       /* off the disk */
    f.cyl = 0;
    CHECK(zr_fdd_eject(&f) == disk && zr_fdd_changed(&f) && !zr_fdd_find_id(&f, 0, 0, t0, &s));
    zr_disk_close(disk);
}

/* --- a fake board for the controller -------------------------------------------------------------- */

typedef struct fake {
    zr_hw_host host;
    uint64_t now, when;
    zr_dev *dev;
    int level[2];
    unsigned rises[2];
} fake;

static uint64_t h_now(void *ctx) { return ((fake *)ctx)->now; }
static void h_schedule(void *ctx, void *dev, uint64_t when) { (void)dev; ((fake *)ctx)->when = when; }
static void h_line(void *ctx, void *dev, int pin, int state) {
    fake *f = ctx;
    (void)dev;
    f->level[pin] = state;
    if (state) f->rises[pin]++;
}
static void run_to(fake *f, uint64_t t) {
    for (unsigned guard = 0; f->when <= t; guard++) {
        if (guard > 4000000) { CHECK(!"a deadline keeps coming at the same time"); break; }
        f->now = f->when;
        f->when = ZR_NEVER;
        f->dev->ops->event(f->dev, f->now);
    }
    f->now = t;
}

typedef struct rig {
    fake f;
    zr_upd765 c;
    zr_fdd d;
    zr_disk *disk;
    hooks h;
} rig;

#define TICK 56u        /* 4 us */

static void tick(rig *r) { run_to(&r->f, r->f.now + TICK); }
static uint8_t msr(rig *r) { return r->c.dev.ops->read(&r->c.dev, ZR_SPACE_IO, 0); }
static uint8_t rdd(rig *r) { return r->c.dev.ops->read(&r->c.dev, ZR_SPACE_IO, 1); }
static void wrd(rig *r, uint8_t v) { r->c.dev.ops->write(&r->c.dev, ZR_SPACE_IO, 1, v); }
static void tc(rig *r, int level) { r->c.dev.ops->input(&r->c.dev, ZR_UPD765_PIN_TC, level); }

static void rig_init(rig *r, int dma) {
    memset(r, 0, sizeof *r);
    r->f.host.ctx = &r->f;
    r->f.host.now = h_now;
    r->f.host.schedule = h_schedule;
    r->f.host.line = h_line;
    r->f.when = ZR_NEVER;
    zr_upd765_init(&r->c, &r->f.host, "fdc", CLOCK);
    r->f.dev = &r->c.dev;
    zr_fdd_init(&r->d, CLOCK, 1, 80, 300);
    zr_fdd_set_hook(&r->d, on_drive, &r->h, 0);
    zr_upd765_attach(&r->c, 0, &r->d);
    r->disk = test_disk_9x512();
    char error[160];
    zr_fdd_insert(&r->d, r->disk, error, sizeof error);
    /* sector patterns, and the odd ones: a weak sector (R5), a CRC error (R6), a deleted mark (R7), no data mark (R8) */
    for (unsigned s = 0; s < 9; s++) {
        uint8_t data[512];
        for (unsigned i = 0; i < 512; i++) data[i] = (uint8_t)(i * 7 + (s + 1) * 13);
        zr_disk_write_sector(r->disk, 0, 0, s, data, 512, 0);
    }
    zr_sector *w = &r->disk->track[0][0].sector[4];
    free(w->data);
    w->data = malloc(1536);
    for (unsigned i = 0; i < 1536; i++) w->data[i] = (uint8_t)(0xA0 + i / 512);
    w->copies = 3; w->size = 512; w->stored = 1536;
    r->disk->track[0][0].sector[5].st1 = 0x20; r->disk->track[0][0].sector[5].st2 = 0x20;
    r->disk->track[0][0].sector[6].st2 = 0x40;
    r->disk->track[0][0].sector[7].st1 = 0x01; r->disk->track[0][0].sector[7].st2 = 0x01;
    r->disk->dirty = 0;
    r->c.dev.ops->reset(&r->c.dev);
    zr_fdd_set_motor(&r->d, 1, 0);
    zr_upd765_drive_changed(&r->c);
    run_to(&r->f, r->d.speed_at + 1000);
    const uint8_t spec[3] = {0x03, 0xC1, (uint8_t)(0x02 | (dma ? 0 : 1))};       /* SRT 12, HUT 1, HLT 1, ND */
    for (unsigned i = 0; i < 3; i++) { while ((msr(r) & 0xC0) != 0x80) tick(r); wrd(r, spec[i]); }
}

static int wait_msr(rig *r, unsigned mask, unsigned value) {
    for (unsigned g = 0; g < 8000000; g++) {
        if ((msr(r) & mask) == value) return 1;
        tick(r);
    }
    CHECK(!"the controller never reached the status that was waited for");
    return 0;
}
static void send(rig *r, const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n; i++) { if (!wait_msr(r, 0xC0, 0x80)) return; wrd(r, b[i]); }
}
static void recv(rig *r, uint8_t *out, unsigned n) {
    for (unsigned i = 0; i < n; i++) { if (!wait_msr(r, 0xC0, 0xC0)) return; out[i] = rdd(r); }
}

/* The polled execution phase of a read: the bytes taken as they come (TC after `tc_at` of them), then the result. */
static unsigned poll_read(rig *r, uint8_t *out, unsigned max, unsigned tc_at, uint8_t *res) {
    unsigned n = 0;
    for (unsigned g = 0; g < 8000000; g++) {
        const unsigned m = msr(r) & 0xF0u;
        if (m == 0xF0u) {
            const uint8_t v = rdd(r);
            if (out && n < max) out[n] = v;
            if (++n == tc_at) tc(r, 1);
        } else if (m == 0xD0u) {
            recv(r, res, 7);
            tc(r, 0);
            return n;
        } else tick(r);
    }
    CHECK(!"no result phase");
    return n;
}
static void poll_write(rig *r, const uint8_t *in, unsigned count, unsigned tc_at, uint8_t *res) {
    unsigned n = 0;
    for (unsigned g = 0; g < 8000000; g++) {
        const unsigned m = msr(r) & 0xF0u;
        if (m == 0xB0u) {
            wrd(r, n < count ? in[n] : 0);
            if (++n == tc_at) tc(r, 1);
        } else if (m == 0xD0u) {
            recv(r, res, 7);
            tc(r, 0);
            return;
        } else tick(r);
    }
    CHECK(!"no result phase");
}
/* The DMA way: DRQ is answered by a cycle of the DMA controller, TC on the byte number `tc_at`. */
static unsigned dma_read(rig *r, uint8_t *out, unsigned tc_at, uint8_t *res) {
    unsigned n = 0;
    for (unsigned g = 0; g < 8000000; g++) {
        if (r->f.level[ZR_UPD765_PIN_DRQ]) {
            const uint8_t v = r->c.dev.ops->dack(&r->c.dev, 2, 0, 0, n + 1 == tc_at);
            if (out) out[n] = v;
            n++;
        } else if ((msr(r) & 0xF0u) == 0xD0u) {
            recv(r, res, 7);
            return n;
        } else tick(r);
    }
    CHECK(!"no result phase");
    return n;
}

static void command(rig *r, unsigned op, unsigned head_unit, unsigned c, unsigned h, unsigned rr, unsigned n, unsigned eot, unsigned dtl) {
    const uint8_t b[9] = {(uint8_t)op, (uint8_t)head_unit, (uint8_t)c, (uint8_t)h, (uint8_t)rr, (uint8_t)n, (uint8_t)eot, 0x2A, (uint8_t)dtl};
    send(r, b, 9);
}
#define MFM_READ 0x46
static int pattern_ok(const uint8_t *d, unsigned sector_r) {
    for (unsigned i = 0; i < 512; i++)
        if (d[i] != (uint8_t)(i * 7 + sector_r * 13)) return 0;
    return 1;
}

static void test_upd765(void) {
    static rig r;
    uint8_t res[8], data[600];
    rig_init(&r, 0);
    /* After the reset: an interrupt, and Sense Interrupt Status says the first drive's READY changed. */
    CHECK(r.f.level[ZR_UPD765_PIN_INT] == 1);
    const uint8_t sense = 0x08;
    send(&r, &sense, 1);
    recv(&r, res, 2);
    CHECK(res[0] == 0xC0 && res[1] == 0);
    /* Seek and Recalibrate step the drive and end in an interrupt. */
    const uint8_t seek[3] = {0x0F, 0x00, 0x02};
    send(&r, seek, 3);
    CHECK(wait_msr(&r, 0x0F, 0x00) && wait_msr(&r, 0x10, 0x00));
    for (unsigned g = 0; g < 100000 && !r.f.level[ZR_UPD765_PIN_INT]; g++) tick(&r);
    send(&r, &sense, 1);
    recv(&r, res, 2);
    CHECK(res[0] == 0x20 && res[1] == 2 && r.d.cyl == 2 && r.h.step >= 2);
    const uint8_t recal[2] = {0x07, 0x00};
    send(&r, recal, 2);
    for (unsigned g = 0; g < 100000 && !r.f.level[ZR_UPD765_PIN_INT]; g++) tick(&r);
    send(&r, &sense, 1);
    recv(&r, res, 2);
    CHECK(res[0] == 0x20 && res[1] == 0 && r.d.cyl == 0);

    /* Read Data, no DMA: a sector, then TC after its last byte ends the command normally. */
    command(&r, MFM_READ, 0, 0, 0, 3, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 512, res) == 512 && pattern_ok(data, 3));
    CHECK(res[0] == 0x00 && res[1] == 0 && res[2] == 0 && res[3] == 0 && res[4] == 0 && res[5] == 4 && res[6] == 2);
    CHECK(r.h.load >= 1);
    /* Two sectors up to EOT without TC: abnormal termination, end of cylinder, the registers wrapped to the next cylinder. */
    command(&r, MFM_READ, 0, 0, 0, 1, 2, 2, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 1024);
    CHECK(res[0] == 0x40 && res[1] == 0x80 && res[3] == 1 && res[5] == 1);
    /* The other recording than the track's: no address mark to be found. */
    command(&r, MFM_READ & ~0x40u, 0, 0, 0, 1, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 0 && res[0] == 0x40 && res[1] == 0x05);
    /* A sector that is not there. */
    command(&r, MFM_READ, 0, 0, 0, 20, 2, 20, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 0 && res[0] == 0x40 && res[1] == 0x04);
    /* What the image records: a CRC error in the data, a deleted mark, no data mark, weak copies. */
    command(&r, MFM_READ, 0, 0, 0, 6, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 512 && res[0] == 0x40 && res[1] == 0x20 && res[2] == 0x20 && res[5] == 6);
    command(&r, MFM_READ, 0, 0, 0, 7, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 512 && pattern_ok(data, 7) && res[0] == 0x00 && res[2] == 0x40 && res[5] == 8);
    command(&r, 0x4C, 0, 0, 0, 7, 2, 9, 0xFF);                                  /* Read Deleted Data takes it as it is */
    CHECK(poll_read(&r, data, 600, 512, res) == 512 && res[0] == 0x00 && res[2] == 0);
    command(&r, MFM_READ, 0, 0, 0, 8, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 0 && res[0] == 0x40 && res[1] == 0x01 && res[2] == 0x01);
    uint8_t first[2];
    for (unsigned i = 0; i < 2; i++) {
        command(&r, MFM_READ, 0, 0, 0, 5, 2, 5, 0xFF);
        poll_read(&r, data, 600, 1, res);
        first[i] = data[0];
    }
    CHECK(first[0] != first[1] && first[0] >= 0xA0 && first[1] >= 0xA0 && first[0] <= 0xA2);
    /* A processor that does not come: overrun. */
    command(&r, MFM_READ, 0, 0, 0, 1, 2, 9, 0xFF);
    for (unsigned g = 0; g < 8000000 && (msr(&r) & 0xF0u) != 0xD0u; g++) tick(&r);
    recv(&r, res, 7);
    CHECK(res[0] == 0x40 && res[1] == 0x10);
    /* Read ID. */
    const uint8_t rid[2] = {0x4A, 0x00};
    send(&r, rid, 2);
    recv(&r, res, 7);
    CHECK(res[0] == 0 && res[3] == 0 && res[4] == 0 && res[5] >= 1 && res[5] <= 9 && res[6] == 2);
    /* Write Data, with TC after the last byte; then on a protected disk: not writable. */
    for (unsigned i = 0; i < 512; i++) data[i] = (uint8_t)(255 - i);
    command(&r, 0x45, 0, 0, 0, 2, 2, 9, 0xFF);
    poll_write(&r, data, 512, 512, res);
    CHECK(res[0] == 0x00 && res[1] == 0 && r.disk->track[0][0].sector[1].data[3] == 252 && r.disk->dirty);
    zr_disk_set_protect(r.disk, 1);
    command(&r, 0x45, 0, 0, 0, 2, 2, 9, 0xFF);
    poll_write(&r, data, 0, 0, res);
    CHECK(res[0] == 0x40 && res[1] == 0x02);
    zr_disk_set_protect(r.disk, 0);
    /* Format a track on side 1 with four 256-byte sectors. */
    const uint8_t fmt[6] = {0x4D, 0x04, 1, 4, 0x20, 0xF6};
    send(&r, fmt, 6);
    uint8_t ids[16];
    for (unsigned i = 0; i < 4; i++) { ids[4 * i] = 0; ids[4 * i + 1] = 1; ids[4 * i + 2] = (uint8_t)(4 - i); ids[4 * i + 3] = 1; }
    poll_write(&r, ids, 16, 0, res);
    CHECK(res[0] == 0x04 && res[1] == 0 && r.disk->track[0][1].count == 4 && r.disk->track[0][1].n == 1 &&
          r.disk->track[0][1].sector[0].r == 4 && r.disk->track[0][1].sector[3].data[255] == 0xF6);
    /* Scan Equal: the host's bytes against the sector's. */
    const uint8_t scan[9] = {0x51, 0x00, 0, 0, 3, 2, 3, 0x2A, 1};
    send(&r, scan, 9);
    for (unsigned i = 0; i < 512; i++) data[i] = (uint8_t)(i * 7 + 3 * 13);
    poll_write(&r, data, 512, 0, res);
    CHECK(res[0] == 0x00 && (res[2] & 0x0C) == 0x08);
    send(&r, scan, 9);
    data[100] ^= 0x55;
    poll_write(&r, data, 512, 0, res);
    CHECK((res[2] & 0x0C) == 0x04);
    /* FM: format a side 1 track of three 128-byte sectors, read it as FM (DTL for N = 0), see nothing of it as MFM,
     * and let a multi-track read run over from side 0 into it. */
    const uint8_t fmtfm[6] = {0x0D, 0x04, 0, 3, 0x1B, 0xF6};
    send(&r, fmtfm, 6);
    for (unsigned i = 0; i < 3; i++) { ids[4 * i] = 0; ids[4 * i + 1] = 1; ids[4 * i + 2] = (uint8_t)(i + 1); ids[4 * i + 3] = 0; }
    poll_write(&r, ids, 12, 0, res);
    CHECK(res[0] == 0x04 && r.disk->track[0][1].encoding == ZR_ENC_FM && r.disk->track[0][1].count == 3 && r.disk->track[0][1].sector[2].size == 128);
    command(&r, 0x06, 0x04, 0, 1, 1, 0, 3, 128);
    CHECK(poll_read(&r, data, 600, 0, res) == 384 && res[0] == 0x44 && res[1] == 0x80 && data[0] == 0xF6 && data[383] == 0xF6);
    command(&r, 0x06, 0x04, 0, 1, 2, 0, 3, 64);
    CHECK(poll_read(&r, data, 600, 0, res) == 128 && res[0] == 0x44);              /* 64 bytes of each of sectors 2 and 3 */
    command(&r, MFM_READ, 0x04, 0, 1, 1, 0, 3, 128);
    CHECK(poll_read(&r, data, 600, 0, res) == 0 && res[0] == 0x44 && res[1] == 0x05);
    command(&r, MFM_READ | 0x80, 0x00, 0, 0, 9, 2, 9, 0xFF);                         /* MT: sector 9 of side 0, then side 1 (FM: nothing to find) */
    CHECK(poll_read(&r, data, 600, 0, res) == 512 && res[0] == 0x44 && res[1] == 0x05 && res[4] == 1);
    /* Read a Track: every sector from the index, errors noted but not stopping it. */
    command(&r, 0x42, 0x00, 0, 0, 1, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 9 * 512 && res[0] == 0x40 && (res[1] & 0x20) && pattern_ok(data, 1));
    /* READY from the pins: a drive that is not ready is NR, and a pin going high is a ready change. */
    zr_upd765_ready_wiring(&r.c, ZR_UPD765_READY_PINS);
    command(&r, MFM_READ, 0, 0, 0, 1, 2, 9, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 0 && res[0] == 0x48);
    r.c.dev.ops->input(&r.c.dev, ZR_UPD765_PIN_READY0, 1);
    send(&r, &sense, 1);
    recv(&r, res, 2);
    CHECK(res[0] == 0xC0 && res[1] == 0);
    command(&r, MFM_READ, 0, 0, 0, 1, 2, 1, 0xFF);
    CHECK(poll_read(&r, data, 600, 0, res) == 512 && res[0] == 0x40 && res[1] == 0x80);
    zr_upd765_ready_wiring(&r.c, ZR_UPD765_READY_TIED);
    /* An invalid command. */
    const uint8_t bad = 0x1F;
    send(&r, &bad, 1);
    recv(&r, res, 1);
    CHECK(res[0] == 0x80);

    /* A state saved in the middle of a transfer continues to the same bytes and result. */
    command(&r, MFM_READ, 0, 0, 0, 4, 2, 9, 0xFF);
    unsigned got = 0;
    for (unsigned g = 0; g < 8000000 && got < 100; g++) {
        if ((msr(&r) & 0xF0u) == 0xF0u) { data[got++] = rdd(&r); } else tick(&r);
    }
    zr_state_writer w;
    zr_sw_init(&w);
    zr_sw_header(&w, "test", "test", NULL, 0);
    zr_sw_begin(&w, "UPD ", 0);
    r.c.dev.ops->save(&r.c.dev, &w);
    zr_sw_end(&w);
    zr_sw_begin(&w, "FDD ", 0);
    zr_fdd_save(&r.d, &w);
    zr_sw_end(&w);
    CHECK(zr_sw_finish(&w));
    const uint64_t saved_at = r.f.now;
    static uint8_t a[600], b[600];
    uint8_t ra[8], rb[8];
    memcpy(a, data, got);
    memcpy(b, data, got);
    CHECK(poll_read(&r, a + got, 600 - got, 512 - got, ra) == 512 - got);                      /* TC comes with the last byte */
    zr_state_reader rd;
    char error[64];
    CHECK(zr_sr_open(&rd, w.data, w.size, error, sizeof error) && zr_sr_chunk(&rd, "UPD ", 0) && r.c.dev.ops->load(&r.c.dev, &rd));
    CHECK(zr_sr_chunk(&rd, "FDD ", 0) && zr_fdd_load(&r.d, &rd));
    r.f.now = saved_at;
    r.f.level[ZR_UPD765_PIN_INT] = zr_upd765_int(&r.c);
    CHECK(poll_read(&r, b + got, 600 - got, 512 - got, rb) == 512 - got);
    CHECK(!memcmp(a, b, 512) && !memcmp(ra, rb, 7) && pattern_ok(a, 4) && ra[0] == 0);
    /* A damaged state is refused or loaded into something that still runs without harm. */
    unsigned loaded = 0;
    for (unsigned i = 0; i < 150; i++) {
        uint8_t *copy = malloc(w.size);
        memcpy(copy, w.data, w.size);
        for (unsigned k = 0; k < 1 + i % 3; k++) copy[lcg() % w.size] = (uint8_t)lcg();
        zr_state_reader broken;
        if (zr_sr_open(&broken, copy, w.size, error, sizeof error) && zr_sr_chunk(&broken, "UPD ", 0) && r.c.dev.ops->load(&r.c.dev, &broken)) {
            loaded++;
            run_to(&r.f, r.f.now + 400000);
            msr(&r);
            rdd(&r);
            wrd(&r, 0x12);
            run_to(&r.f, r.f.now + 100000);
        }
        free(copy);
    }
    CHECK(loaded > 0);
    zr_sw_free(&w);
    zr_disk_close(r.disk);

    /* DMA: DRQ for each byte, TC with the last byte ends the command although EOT is beyond the track's sectors. */
    rig_init(&r, 1);
    command(&r, MFM_READ, 0, 0, 0, 3, 2, 26, 0xFF);
    CHECK(dma_read(&r, data, 512, res) == 512 && pattern_ok(data, 3) && res[0] == 0x00 && res[1] == 0 && res[5] == 4);
    CHECK(r.f.rises[ZR_UPD765_PIN_DRQ] >= 512 && r.f.level[ZR_UPD765_PIN_DRQ] == 0 && r.f.rises[ZR_UPD765_PIN_INT] >= 1);
    /* Without TC a read of the last sector asked for ends with end of cylinder. */
    command(&r, MFM_READ, 0, 0, 0, 1, 2, 1, 0xFF);
    CHECK(dma_read(&r, NULL, 0, res) == 512 && res[0] == 0x40 && res[1] == 0x80);
    zr_disk_close(r.disk);
}

/* --- the drive sound ------------------------------------------------------------------------------ */

static void play(zr_drivesnd *s, int16_t *out, unsigned frames, unsigned chunk, uint64_t until) {
    for (unsigned done = 0; done < frames;) {
        const size_t n = zr_drivesnd_render(s, until, out + 2 * done, frames - done < chunk ? frames - done : chunk);
        if (!n) break;
        done += (unsigned)n;
    }
}

static void test_drivesnd(void) {
    zr_drivesnd a, b;
    static int16_t oa[2 * 4800], ob[2 * 4800], mix[2 * 4800];
    zr_drivesnd_init(&a, CLOCK, 48000);
    zr_drivesnd_init(&b, CLOCK, 48000);
    zr_drivesnd_set(&a, 1, 100);
    zr_drivesnd_set(&b, 1, 100);
    CHECK(zr_drivesnd_enabled(&a) && !zr_drivesnd_busy(&a));
    for (int i = 0; i < 2; i++) {
        zr_drivesnd *s = i ? &b : &a;
        zr_drivesnd_event(s, 0, ZR_DRIVE_MOTOR, 1000, 1, 0);
        zr_drivesnd_event(s, 1, ZR_DRIVE_HEAD_LOAD, 200000, 0, 0);
        zr_drivesnd_event(s, 1, ZR_DRIVE_STEP, 400000, 1, 0);
        zr_drivesnd_event(s, 1, ZR_DRIVE_STEP, 400000 + 1000000, 0, 1);
        zr_drivesnd_event(s, 1, ZR_DRIVE_HEAD_HOLD, 2000000, 700000, 0);
    }
    const uint64_t until = CLOCK / 10u;                                  /* 0.1 s: 4800 frames less the latency */
    memset(oa, 0, sizeof oa);
    memset(ob, 0, sizeof ob);
    const size_t n = zr_drivesnd_render(&a, until, oa, 4800);
    CHECK(n == 4800 - ZR_DRIVESND_LATENCY && zr_drivesnd_position(&a) == n);
    play(&b, ob, (unsigned)n, 37, until);                               /* the same sound whatever the size of the pulls */
    CHECK(!memcmp(oa, ob, n * 4u));
    long energy = 0;
    for (size_t i = 0; i < 2 * n; i++) energy += oa[i] < 0 ? -oa[i] : oa[i];
    CHECK(energy > 100000 && zr_drivesnd_busy(&a));
    /* _add adds to what is there (the beeper's frames), saturating. */
    zr_drivesnd c;
    zr_drivesnd_init(&c, CLOCK, 48000);
    zr_drivesnd_set(&c, 1, 100);
    zr_drivesnd_event(&c, 0, ZR_DRIVE_MOTOR, 1000, 1, 0);
    zr_drivesnd_event(&c, 1, ZR_DRIVE_HEAD_LOAD, 200000, 0, 0);
    zr_drivesnd_event(&c, 1, ZR_DRIVE_STEP, 400000, 1, 0);
    zr_drivesnd_event(&c, 1, ZR_DRIVE_STEP, 1400000, 0, 1);
    zr_drivesnd_event(&c, 1, ZR_DRIVE_HEAD_HOLD, 2000000, 700000, 0);
    for (unsigned i = 0; i < 2 * 4800; i++) mix[i] = 1000;
    CHECK(zr_drivesnd_render_add(&c, until, mix, 4800) == n);
    unsigned same = 1;
    for (size_t i = 0; i < 2 * n; i++) {
        int want = oa[i] + 1000;
        want = want > 32767 ? 32767 : want < -32768 ? -32768 : want;
        if (mix[i] != want) same = 0;
    }
    CHECK(same);
    /* A disabled source is silent and keeps its place. */
    zr_drivesnd_set(&c, 0, 100);
    CHECK(zr_drivesnd_render(&c, until * 2, oa, 4800) == 4800 && oa[100] == 0 && !zr_drivesnd_busy(&c));
}

int main(void) {
    test_disk();
    test_fdd();
    test_upd765();
    test_drivesnd();
    if (failures) printf("%d checks failed\n", failures);
    else printf("floppy: ok\n");
    return failures ? 1 : 0;
}
