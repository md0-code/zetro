/* ZetRo - the floppy drive: the mechanism that the
 * controller (upd765.h) reaches. A plain struct, not a chip: the board owns up
 * to four of them and hands them to the controller; a drive never schedules
 * anything, it answers questions about the head, the spindle and the disk at a
 * given time, so everything stays event driven in the controller. Written for disks of any
 * geometry (zr_disk, core/media/disk.h) and a clock of any rate.
 *
 * The drive. `tracks` cylinders (40, 77, 80), two heads, step in and out, the
 * TRK00, write-protect and index signals, the spindle motor at 300 or 360
 * rpm. The head reaches cylinders 0 to tracks + 1 (the stepper's end stops);
 * a 40-track disk in an 80-track drive is read at cylinders 0-39 with no
 * double stepping (the board's concern). The motor reaches full speed `spinup`
 * periods after it is switched on (default 400 ms) and stops at once when
 * switched off. The disk turns at the drive's speed and its index hole passes
 * the sensor at the instant the spindle reaches speed and every revolution
 * after. A drive without a disk delivers no index pulses and no data, as a
 * real one (the sensor sees no hole); neither does a stopped one. READY
 * (zr_fdd_rotating) is a disk turning at speed.
 *
 * The disk is the caller's (borrowed: the drive uses it until it is ejected,
 * then the caller closes it). Inserting or ejecting sets the disk-change
 * latch, which the next step pulse with a disk in clears. Writes go into the
 * zr_disk at once and mark it dirty; the shell writes it back
 * (zr_disk_save_media).
 *
 * The track as the controller sees it. A track turns once in rev units
 * (zr_fdd_rev_units): a unit is the time of a raw MFM byte (8 / rate seconds:
 * 32 us at 250 kbit/s), so a 300 rpm track holds 6250 of them at 250 kbit/s
 * (and 300 kbit/s at 360 rpm); an FM byte is two units. The `rate` is the
 * controller's data separator setting (zr_fdd_set_rate: 250, 300 or 500
 * kbit/s, in MFM terms), which the board sets and the controller passes
 * on. The recorded layout is the IBM one: MFM: 80 bytes of gap 4a, 12 of
 * sync, the index mark (4), 50 of gap 1; then for each sector an ID field
 * (12 sync, 3 + 1 address mark, C H R N, 2 CRC: 22 bytes), gap 2 (22), a
 * data field (12 sync, 3 + 1 address mark, the data, 2 CRC) and gap 3. FM:
 * 40 gap 4a, 6 sync, 1 index mark, 26 gap 1; the ID field 13 bytes (6 sync, 1
 * mark, CHRN, CRC), gap 2 11, the data field 6 sync, 1 mark, the data, 2 CRC.
 * The track's gap 3 byte sets the pitch of the sectors; one that is 0 or too
 * large shares the rest of the revolution out evenly. A track longer than a
 * revolution is played faster to fit it.
 *
 * Time is guest time (periods of the master clock given at init). Portable
 * C11. ZetRo's own code. */
#ifndef ZR_FDD_H
#define ZR_FDD_H

#include <stddef.h>
#include <stdint.h>

#include "hw.h"
#include "media/disk.h"

struct zr_state_writer;
struct zr_state_reader;

#define ZR_FDD_INDEX_PULSE_US 2000u

typedef struct zr_fdd {
    /* The drive (set at init). */
    uint8_t installed;
    uint8_t tracks;                 /* cylinders: 40, 77 or 80 */
    uint16_t rpm;                   /* 300 or 360 */
    uint32_t clock_hz;              /* the master clock's rate */
    uint32_t rate;                  /* kbit/s the controller's data separator runs at, MFM terms */
    uint32_t spinup;                /* periods from motor on to full speed */
    zr_drive_hook hook;             /* the mechanical events (hw.h), for the drive sound; NULL: none */
    void *hook_ctx;
    uint8_t id;                     /* the number the hook is given for this drive */
    /* The state (saved). */
    uint8_t cyl;                    /* where the head is */
    uint8_t motor;
    uint8_t changed;                /* the disk-change latch */
    uint64_t speed_at;              /* motor on: when the spindle is at speed (the index epoch) */
    uint32_t weak;                  /* counts the reads of weak sectors, to give a different copy each time */
    /* The disk. */
    zr_disk *disk;                  /* borrowed; NULL: none */
} zr_fdd;

/* The place of a sector in the turning track: when it passes the head. `t0` is the time its ID field
 * starts; the other positions are in rev units after it. Convert with zr_fdd_slot_time. */
typedef struct zr_fdd_slot {
    const zr_track *track;
    const zr_sector *sector;
    unsigned index;                 /* its position in the track, 0 .. count - 1 */
    int fm;
    uint64_t t0;
    uint32_t unit;                  /* units in a raw byte: 1 for MFM, 2 for FM */
    uint32_t id_end;                /* units to the end of the ID field's CRC */
    uint32_t data;                  /* ... to the first byte of the data */
    uint32_t data_end;              /* ... to the end of the data field's CRC */
    uint64_t num, den;              /* guest time of a position: t0 + units * num / den */
} zr_fdd_slot;

static inline uint64_t zr_fdd_slot_time(const zr_fdd_slot *s, uint32_t units) { return s->t0 + (uint64_t)units * s->num / s->den; }

/* Sets the drive up on a clock of `clock_hz` (empty, motor off, head at cylinder 0, 250 kbit/s). `tracks` is
 * 40, 77 or 80 (1 to 99 is taken); `rpm` 300 or 360. An uninstalled drive does nothing (no index, no TRK00). */
void zr_fdd_init(zr_fdd *f, uint32_t clock_hz, unsigned installed, unsigned tracks, unsigned rpm);
/* The mechanical events go to `hook` as drive number `id`. */
void zr_fdd_set_hook(zr_fdd *f, zr_drive_hook hook, void *ctx, unsigned id);
/* The data rate in kbit/s (250, 300, 500: MFM terms). */
void zr_fdd_set_rate(zr_fdd *f, unsigned kbit_per_s);
/* A mechanical event of the drive (hw.h) at time `t`, to its hook; nothing for a drive that is not installed. */
static inline void zr_fdd_notify(const zr_fdd *f, unsigned event, uint64_t t, uint32_t a, uint32_t b) {
    if (f->hook && f->installed) f->hook(f->hook_ctx, f->id, event, t, a, b);
}

/* Puts a disk in. 1 on success; 0 with the reason when the drive has no room for it (an uninstalled drive,
 * more cylinders than the head can reach). Replaces a disk already inside. */
int zr_fdd_insert(zr_fdd *f, zr_disk *disk, char *error, size_t capacity);
/* Takes the disk out; returns it (NULL when empty). */
zr_disk *zr_fdd_eject(zr_fdd *f);

/* Signals. */
static inline int zr_fdd_has_disk(const zr_fdd *f) { return f->installed && f->disk; }
static inline int zr_fdd_wprot(const zr_fdd *f) { return f->installed && f->disk && zr_disk_write_protected(f->disk); }
static inline int zr_fdd_track0(const zr_fdd *f) { return f->installed && f->cyl == 0; }
static inline int zr_fdd_changed(const zr_fdd *f) { return f->changed; }
static inline void zr_fdd_clear_changed(zr_fdd *f) { f->changed = 0; }

/* The spindle motor at time `now`: on and off, spin-up from `now`. */
void zr_fdd_set_motor(zr_fdd *f, int on, uint64_t now);
int zr_fdd_at_speed(const zr_fdd *f, uint64_t now);
/* A disk turning at speed at `now`: the index and the data are there (READY). */
int zr_fdd_rotating(const zr_fdd *f, uint64_t now);
/* The time a disk is turning at speed (already past when it is); ZR_NEVER without an installed drive, a
 * disk or the motor on. */
uint64_t zr_fdd_rotating_at(const zr_fdd *f);

/* One revolution in guest time, and in units; the guest time of `units`. */
uint64_t zr_fdd_rev(const zr_fdd *f);
uint32_t zr_fdd_rev_units(const zr_fdd *f);
uint64_t zr_fdd_ticks(const zr_fdd *f, uint64_t units);
/* The index signal at `now`: high for the 2 ms the hole takes to pass, once a revolution from the instant
 * the spindle reaches speed; low when the drive is not turning a disk. */
int zr_fdd_index(const zr_fdd *f, uint64_t now);
/* The next index pulse at or after `from` (the drive must be turning). */
uint64_t zr_fdd_next_index(const zr_fdd *f, uint64_t from);

/* One step pulse at time `now`: dir +1 towards the spindle, -1 away (towards TRK00). Returns the cylinder.
 * A step with a disk in clears the change latch. The time only stamps the mechanical event the drive
 * reports (ZR_DRIVE_STEP). */
unsigned zr_fdd_step(zr_fdd *f, int dir, uint64_t now);

/* The first ID field of `head` on the head's cylinder, recorded as `fm` (FM or MFM; a track of the other
 * recording is not seen, a track whose recording is not known is seen as either), whose address mark has
 * not started before `from`. 1 and the slot, or 0 when the track has no IDs for this head (no disk, not
 * turning, past the disk's last cylinder or side, unformatted, the other recording). */
int zr_fdd_find_id(const zr_fdd *f, unsigned head, int fm, uint64_t from, zr_fdd_slot *slot);
/* The sector at position `index` of the track under `head`, NULL when there is none (the disk may have changed). */
const zr_sector *zr_fdd_sector(const zr_fdd *f, unsigned head, unsigned index);
/* Reads `length` data bytes of sector `index` of the track under `head`, as the head reads them (a weak sector gives
 * a different copy at each read; a recorded length shorter than that is padded with the track's filler) and gives the
 * sector in *sector (valid until the disk is next written). 0 when there is no such sector. */
int zr_fdd_read_sector(zr_fdd *f, unsigned head, unsigned index, uint8_t *out, uint32_t length, const zr_sector **sector);
/* Writes sector `index` of the track under `head`: ZR_DISK_OK, _PROTECTED or _NO_SECTOR (zr_disk_write_sector). */
int zr_fdd_write_sector(zr_fdd *f, unsigned head, unsigned index, const uint8_t *data, uint32_t length, int deleted);
/* Formats the track under `head` (zr_disk_format_track). ZR_DISK_NO_SECTOR without a disk. */
int zr_fdd_format_track(zr_fdd *f, unsigned head, const zr_disk_format *format);
/* The units a track of `count` sectors of size code `n` takes before its first sector's ID field (the index
 * area) and from the start of one sector's ID field to the next with gap 3 `gap3`: for the controller's
 * format timing. */
uint32_t zr_fdd_format_pre(int fm);
uint32_t zr_fdd_format_pitch(int fm, unsigned n, unsigned gap3);

/* The drive's state (not the disk): the head, the motor, the spindle's phase. */
void zr_fdd_save(const zr_fdd *f, struct zr_state_writer *w);
int zr_fdd_load(zr_fdd *f, struct zr_state_reader *r);

#endif /* ZR_FDD_H */
