/* ZetRo - the floppy drive (fdd.h). Portable C11. ZetRo's own code. */
#include "fdd.h"

#include "state.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_SPINUP_US 400000u

/* The recorded layout, in raw bytes (an MFM byte is a unit, an FM byte two): see fdd.h. */
#define MFM_PRE 146u
#define MFM_SYNC 12u
#define MFM_ID 22u
#define MFM_GAP2 22u
#define MFM_DATA_PRE 16u
#define FM_PRE 73u
#define FM_SYNC 6u
#define FM_ID 13u
#define FM_GAP2 11u
#define FM_DATA_PRE 7u
#define CRC 2u

static void note(char *error, size_t capacity, const char *text) {
    if (error && capacity) snprintf(error, capacity, "%s", text);
}

void zr_fdd_init(zr_fdd *f, uint32_t clock_hz, unsigned installed, unsigned tracks, unsigned rpm) {
    memset(f, 0, sizeof *f);
    f->installed = installed != 0;
    f->tracks = (uint8_t)(tracks >= 1 && tracks < ZR_DISK_MAX_CYL ? tracks : 80);
    f->rpm = (uint16_t)(rpm ? rpm : 300);
    f->clock_hz = clock_hz ? clock_hz : 1u;
    f->rate = 250;
    f->spinup = (uint32_t)((uint64_t)f->clock_hz * DEFAULT_SPINUP_US / 1000000u);
}

void zr_fdd_set_hook(zr_fdd *f, zr_drive_hook hook, void *ctx, unsigned id) {
    f->hook = hook;
    f->hook_ctx = ctx;
    f->id = (uint8_t)id;
}

void zr_fdd_set_rate(zr_fdd *f, unsigned kbit_per_s) { f->rate = kbit_per_s ? kbit_per_s : 250u; }

int zr_fdd_insert(zr_fdd *f, zr_disk *disk, char *error, size_t capacity) {
    note(error, capacity, "");
    if (!f->installed) { note(error, capacity, "The drive is not installed"); return 0; }
    if (!disk) { note(error, capacity, "There is no disk"); return 0; }
    if (disk->cylinders > f->tracks + 2u) {
        if (error && capacity) snprintf(error, capacity, "The disk has %u tracks and the drive %u", disk->cylinders, f->tracks);
        return 0;
    }
    f->disk = disk;
    f->changed = 1;
    return 1;
}

zr_disk *zr_fdd_eject(zr_fdd *f) {
    zr_disk *d = f->disk;
    if (d) f->changed = 1;
    f->disk = NULL;
    return d;
}

/* --- the spindle ---------------------------------------------------------------------------- */

void zr_fdd_set_motor(zr_fdd *f, int on, uint64_t now) {
    if (!f->installed) return;
    const uint8_t was = f->motor;
    if (on && !f->motor) f->speed_at = now + f->spinup;
    f->motor = on != 0;
    if (f->motor != was) zr_fdd_notify(f, ZR_DRIVE_MOTOR, now, f->motor, 0);
}

int zr_fdd_at_speed(const zr_fdd *f, uint64_t now) { return f->installed && f->motor && now >= f->speed_at; }
int zr_fdd_rotating(const zr_fdd *f, uint64_t now) { return zr_fdd_at_speed(f, now) && f->disk; }
uint64_t zr_fdd_rotating_at(const zr_fdd *f) { return (f->installed && f->motor && f->disk) ? f->speed_at : ZR_NEVER; }

uint64_t zr_fdd_rev(const zr_fdd *f) {
    const uint64_t r = (uint64_t)f->clock_hz * 60u / (f->rpm ? f->rpm : 300u);
    return r ? r : 1u;
}
uint32_t zr_fdd_rev_units(const zr_fdd *f) {
    const uint32_t u = (uint32_t)((uint64_t)f->rate * 7500u / (f->rpm ? f->rpm : 300u));
    return u ? u : 1u;
}
uint64_t zr_fdd_ticks(const zr_fdd *f, uint64_t units) { return units * zr_fdd_rev(f) / zr_fdd_rev_units(f); }

int zr_fdd_index(const zr_fdd *f, uint64_t now) {
    return zr_fdd_rotating(f, now) && (now - f->speed_at) % zr_fdd_rev(f) < (uint64_t)f->clock_hz * ZR_FDD_INDEX_PULSE_US / 1000000u;
}

uint64_t zr_fdd_next_index(const zr_fdd *f, uint64_t from) {
    if (from <= f->speed_at) return f->speed_at;
    const uint64_t rev = zr_fdd_rev(f), off = from - f->speed_at;
    return f->speed_at + (off + rev - 1u) / rev * rev;
}

unsigned zr_fdd_step(zr_fdd *f, int dir, uint64_t now) {
    if (!f->installed) return 0;
    const uint8_t before = f->cyl;
    if (dir < 0) { if (f->cyl > 0) f->cyl--; }
    else if (f->cyl < f->tracks + 1u) f->cyl++;
    if (f->disk) f->changed = 0;
    zr_fdd_notify(f, ZR_DRIVE_STEP, now, dir > 0, f->cyl == before);
    return f->cyl;
}

/* --- the track under the head -------------------------------------------------------------------------- */

/* The track of `head` at the head's cylinder, or NULL; also when it is of the other recording. */
static const zr_track *track_of(const zr_fdd *f, unsigned head, int fm) {
    if (!f->installed || !f->disk || head >= f->disk->sides || f->cyl >= f->disk->cylinders) return NULL;
    const zr_track *t = zr_disk_track(f->disk, f->cyl, head);
    if (!t) return NULL;
    if (t->encoding != ZR_ENC_UNKNOWN && (t->encoding == ZR_ENC_FM) != (fm != 0)) return NULL;
    return t;
}

/* The track's layout for one recording: where each sector starts, in units after the index. */
typedef struct layout {
    uint32_t unit, pre, sync, id, gap2, data_pre, gap3, den;
} layout;

static void layout_of(const zr_fdd *f, const zr_track *t, int fm, layout *l) {
    l->unit = fm ? 2u : 1u;
    l->pre = (fm ? FM_PRE : MFM_PRE) * l->unit;
    l->sync = (fm ? FM_SYNC : MFM_SYNC) * l->unit;
    l->id = (fm ? FM_ID : MFM_ID) * l->unit;
    l->gap2 = (fm ? FM_GAP2 : MFM_GAP2) * l->unit;
    l->data_pre = (fm ? FM_DATA_PRE : MFM_DATA_PRE) * l->unit;
    uint64_t fixed = l->pre;
    for (unsigned k = 0; k < t->count; k++)
        fixed += l->id + l->gap2 + l->data_pre + (uint64_t)zr_disk_nominal(t->sector[k].n) * l->unit + CRC * l->unit;
    const uint32_t rev = zr_fdd_rev_units(f);
    uint64_t gap = (uint64_t)t->gap3 * l->unit;
    if (!gap || fixed + gap * t->count > rev) gap = fixed < rev ? (rev - fixed) / t->count : 0;
    l->gap3 = (uint32_t)gap;
    const uint64_t total = fixed + gap * t->count;
    l->den = (uint32_t)(total > rev ? total : rev);
}

int zr_fdd_find_id(const zr_fdd *f, unsigned head, int fm, uint64_t from, zr_fdd_slot *slot) {
    if (!f->motor) return 0;
    const zr_track *t = track_of(f, head, fm);
    if (!t) return 0;
    layout l;
    layout_of(f, t, fm, &l);
    if (from < f->speed_at) from = f->speed_at;
    const uint64_t rev_t = zr_fdd_rev(f), off = from - f->speed_at;
    uint64_t rev = off / rev_t;
    const uint64_t in = off % rev_t;
    unsigned k = 0;
    uint64_t at = l.pre;
    while (k < t->count && (at + l.sync) * rev_t / l.den < in) {        /* the address mark of sector k has started already */
        at += l.id + l.gap2 + l.data_pre + (uint64_t)zr_disk_nominal(t->sector[k].n) * l.unit + CRC * l.unit + l.gap3;
        k++;
    }
    if (k == t->count) { k = 0; at = l.pre; rev++; }
    const uint32_t nominal = zr_disk_nominal(t->sector[k].n);
    slot->track = t;
    slot->sector = &t->sector[k];
    slot->index = k;
    slot->fm = fm != 0;
    slot->unit = l.unit;
    slot->num = rev_t;
    slot->den = l.den;
    slot->t0 = f->speed_at + rev * rev_t + at * rev_t / l.den;
    slot->id_end = l.id;
    slot->data = l.id + l.gap2 + l.data_pre;
    slot->data_end = slot->data + nominal * l.unit + CRC * l.unit;
    /* t0 above is rounded down from `at`; the slot's own positions are measured from it. */
    return 1;
}

const zr_sector *zr_fdd_sector(const zr_fdd *f, unsigned head, unsigned index) {
    if (!f->installed || !f->disk || head >= f->disk->sides || f->cyl >= f->disk->cylinders) return NULL;
    const zr_track *t = zr_disk_track(f->disk, f->cyl, head);
    return t && index < t->count ? &t->sector[index] : NULL;
}

int zr_fdd_read_sector(zr_fdd *f, unsigned head, unsigned index, uint8_t *out, uint32_t length, const zr_sector **sector) {
    if (!f->installed || !f->disk || head >= f->disk->sides || f->cyl >= f->disk->cylinders) return 0;
    const zr_track *t = zr_disk_track(f->disk, f->cyl, head);
    if (!t || index >= t->count) return 0;
    const zr_sector *s = &t->sector[index];
    unsigned copy = 0;
    if (s->copies > 1) copy = f->weak++ % s->copies;
    zr_disk_read_sector(s, copy, out, length, t->filler);
    if (sector) *sector = s;
    return 1;
}

int zr_fdd_write_sector(zr_fdd *f, unsigned head, unsigned index, const uint8_t *data, uint32_t length, int deleted) {
    if (!f->installed || !f->disk) return ZR_DISK_NO_SECTOR;
    return zr_disk_write_sector(f->disk, f->cyl, head, index, data, length, deleted);
}

int zr_fdd_format_track(zr_fdd *f, unsigned head, const zr_disk_format *format) {
    if (!f->installed || !f->disk) return ZR_DISK_NO_SECTOR;
    return zr_disk_format_track(f->disk, f->cyl, head, format);
}

uint32_t zr_fdd_format_pre(int fm) { return (fm ? FM_PRE * 2u : MFM_PRE); }
uint32_t zr_fdd_format_pitch(int fm, unsigned n, unsigned gap3) {
    const uint32_t unit = fm ? 2u : 1u;
    return ((fm ? FM_ID : MFM_ID) + (fm ? FM_GAP2 : MFM_GAP2) + (fm ? FM_DATA_PRE : MFM_DATA_PRE) + CRC + gap3 + zr_disk_nominal(n)) * unit;
}

/* --- state ------------------------------------------------------------------------------------------------- */

void zr_fdd_save(const zr_fdd *f, zr_state_writer *w) {
    zr_sw_u8(w, f->cyl);
    zr_sw_u8(w, f->motor);
    zr_sw_u8(w, f->changed);
    zr_sw_u64(w, f->speed_at);
    zr_sw_u32(w, f->weak);
}

int zr_fdd_load(zr_fdd *f, zr_state_reader *r) {
    const uint8_t cyl = zr_sr_u8(r), motor = zr_sr_u8(r), changed = zr_sr_u8(r);
    const uint64_t speed_at = zr_sr_u64(r);
    const uint32_t weak = zr_sr_u32(r);
    if (!zr_sr_ok(r) || cyl > f->tracks + 1u || motor > 1 || changed > 1) return 0;
    f->cyl = cyl;
    f->motor = motor;
    f->changed = changed;
    f->speed_at = speed_at;
    f->weak = weak;
    return 1;
}
