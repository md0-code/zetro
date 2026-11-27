/* State files, the quick-save slots and the rewind ring. No SDL: the shell (desktop.c) puts
 * the media back and says what happened; the setup screen's States page reads the slots through here.
 *
 * A state file is the machine's state (zr_machine_save, state.h) with chunks of the shell's after the machine's:
 *
 *   "MDIA" slot   str path, u8 flags (1: the hash follows, 2: write-protected), 32 bytes SHA-256 of the content
 *                 (an older state has the path alone: it reads as no hash, not protected)
 *   "THMB" 0      u16 width, u16 height, u16 colours, colours x u32 0x00RRGGBB, the pixels as palette indices
 *                 packed (zr_sw_packed): the machine's picture at a quarter of its size, at most 96 x 72
 *   "TIME" 0      u16 year, u8 month, day, hour, minute, second: the host's local time of the save
 *   "VERS" 0      str ZetRo's version that saved it
 *
 * The slots are files states/<model id>/slot-N.zrs (N 1 to 10) in a states folder beside the configuration file.
 *
 * The rewind ring keeps the machine's state alone (no thumbnail, no media) every half second of guest time, in a
 * memory budget, the oldest dropped first. ZetRo's own code. */
#ifndef ZR_STATES_H
#define ZR_STATES_H

#include "../core/config.h"
#include "../core/machine.h"
#include "../core/state.h"

#include <stddef.h>
#include <stdint.h>

#define ZR_STATES_SLOTS 10
#define ZR_STATES_THUMB_W 96            /* the largest thumbnail */
#define ZR_STATES_THUMB_H 72
#define ZR_STATES_VERSION 32

typedef struct zr_states_media {
    int present;
    char path[ZR_MEDIA_PATH];
    int hashed;                         /* sha256 is the content's at the save (an older state has none) */
    uint8_t sha256[32];
    int write_protect;
} zr_states_media;

typedef struct zr_states_thumb {
    uint32_t width, height, colors;
    uint32_t palette[256];
    uint8_t pixels[ZR_STATES_THUMB_W * ZR_STATES_THUMB_H];
} zr_states_thumb;

typedef struct zr_states_time {
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} zr_states_time;

/* What a state file says besides the machine's own chunks. */
typedef struct zr_states_info {
    char family[ZR_STATE_ID], model[ZR_STATE_ID];
    char version[ZR_STATES_VERSION];    /* "" when the state does not say */
    int has_time;
    zr_states_time time;
    int has_thumb;
    zr_states_thumb thumb;
    zr_states_media media[ZR_CONFIG_MEDIA];
} zr_states_info;

/* The thumbnail of a frame: a quarter of its size (at most ZR_STATES_THUMB_W x ZR_STATES_THUMB_H, its shape kept),
 * each pixel the frame's colour nearest to the average of the pixels it stands for. 0 when the frame has no pixels. */
int zr_states_thumb_make(const zr_frame *frame, zr_states_thumb *thumb);

/* The machine's state into w (zr_sw_init'ed), then the chunks of `extra` (NULL: none, as the rewind ring keeps it) and
 * the end mark. 0 when out of memory. */
int zr_states_write(const zr_machine *m, const zr_states_info *extra, zr_state_writer *w);
/* Opens a state for zr_machine_load (r) and reads the shell's chunks into info. 0 with the reason when it is not a state
 * ZetRo reads. `data` must outlive r. */
int zr_states_read(const uint8_t *data, size_t size, zr_states_info *info, zr_state_reader *r, char *error, size_t capacity);

/* --- files and slots ------------------------------------------------------------------- */

/* states/<model>/slot-<slot>.zrs beside the configuration file `config_source` (slot 1 to ZR_STATES_SLOTS). */
void zr_states_slot_path(const char *config_source, const char *model, unsigned slot, char *out, size_t capacity);
/* A state file written whole, its folders made first. 0 with the reason. */
int zr_states_save_file(const char *path, const uint8_t *data, size_t size, char *error, size_t capacity);
/* A state file read whole (the caller frees *data). 0 with the reason. */
/* Shell operations on mounted media preserve changes before replacing a slot or changing its tab. */
int zr_states_flush_media(zr_machine *m, unsigned slot, char *error, size_t capacity);
int zr_states_close_media(zr_machine *m, unsigned slot, zr_media **owned, const char *base, char *error, size_t capacity);
int zr_states_flush_all(zr_machine *m, char *error, size_t capacity);
int zr_states_protect_media(zr_machine *m, unsigned slot, int protect, char *error, size_t capacity);
int zr_states_replace_media(zr_machine *m, unsigned slot, zr_media **owned, const char *path, int protect,
                            char *error, size_t capacity);
int zr_states_load_session(zr_machine *m, const zr_config *config, const zr_rom_index *roms, zr_media **media, const char *path,
                           char *warning, size_t warning_capacity, char *error, size_t capacity);

int zr_states_load_file(const char *path, uint8_t **data, size_t *size, char *error, size_t capacity);
/* What a slot file says (info), 0 when the slot is empty or its file is not a state. */
int zr_states_slot_info(const char *path, zr_states_info *info);
/* "2026-11-13 14:05" */
void zr_states_time_text(const zr_states_time *t, char *out, size_t capacity);

/* --- the rewind ring ------------------------------------------------------------------- */

#define ZR_REWIND_BUDGET (64u * 1024u * 1024u)      /* bytes of states kept */
#define ZR_REWIND_INTERVAL_MS 500u                  /* guest time between two states */

typedef struct zr_rewind_entry {
    uint8_t *data;
    size_t size, capacity;
    uint64_t time;                      /* the guest time of the state */
} zr_rewind_entry;

typedef struct zr_rewind {
    zr_rewind_entry *entries;           /* a ring of `room` entries from `first`, `count` in use */
    size_t room, first, count;
    size_t budget, bytes;               /* the bytes the entries hold, at most the budget */
    uint64_t next;                      /* the guest time the next state is due at */
    zr_state_writer scratch;            /* the serialisation's buffer, kept from one state to the next */
} zr_rewind;

void zr_rewind_init(zr_rewind *r, size_t budget);
void zr_rewind_free(zr_rewind *r);
/* Drops every state; the next is due at once. */
void zr_rewind_clear(zr_rewind *r);
/* Takes a state when one is due (guest time past r->next); 1 when one was taken, -1 when out of memory. `interval` is the
 * guest time between two states, in ticks. */
int zr_rewind_take(zr_rewind *r, const zr_machine *m, uint64_t interval);
/* The state `index` (0 the oldest, count - 1 the newest): its guest time, and the machine put back to it. */
uint64_t zr_rewind_time(const zr_rewind *r, size_t index);
int zr_rewind_restore(const zr_rewind *r, zr_machine *m, size_t index, char *error, size_t capacity);
/* The machine goes on from state `index`: the states after it are dropped, the next is due an interval after it. */
void zr_rewind_cut(zr_rewind *r, size_t index, uint64_t interval);
/* The newest state at or before guest time `time`; 0 when there is none. */
int zr_rewind_find(const zr_rewind *r, uint64_t time, size_t *index);

#endif /* ZR_STATES_H */
