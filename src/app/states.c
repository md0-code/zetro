/* State files, slots and the rewind ring (states.h). ZetRo's own code. */
#include "states.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MEDIA_HASHED = 1, MEDIA_PROTECTED = 2 };

/* --- the thumbnail ---------------------------------------------------------------------- */

int zr_states_thumb_make(const zr_frame *f, zr_states_thumb *t) {
    memset(t, 0, sizeof *t);
    if (!f->pixels || !f->palette || !f->colors || f->width < 4 || f->height < 4) return 0;
    uint32_t w = f->width / 4, h = f->height / 4;
    if (w > ZR_STATES_THUMB_W) { h = h * ZR_STATES_THUMB_W / w; w = ZR_STATES_THUMB_W; }
    if (h > ZR_STATES_THUMB_H) { w = w * ZR_STATES_THUMB_H / h; h = ZR_STATES_THUMB_H; }
    if (!w || !h) return 0;
    t->width = w;
    t->height = h;
    t->colors = f->colors > 256 ? 256 : f->colors;
    memcpy(t->palette, f->palette, t->colors * sizeof *t->palette);
    static uint32_t count[256];
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            /* The block of the frame this pixel stands for: its commonest colour, or the second commonest when that covers a
             * fifth of the block, so that thin lines (the strokes of text) stay in sight rather than vanish into the paper. */
            const uint32_t x0 = x * f->width / w, x1 = (x + 1) * f->width / w, y0 = y * f->height / h, y1 = (y + 1) * f->height / h;
            uint32_t n = 0, first = 0, second = 0;
            memset(count, 0, sizeof count);
            for (uint32_t sy = y0; sy < y1; ++sy)
                for (uint32_t sx = x0; sx < x1; ++sx) {
                    const uint8_t i = f->pixels[(size_t)sy * f->pitch + sx];
                    ++count[i < t->colors ? i : 0];
                    ++n;
                }
            for (uint32_t i = 1; i < t->colors; ++i)
                if (count[i] > count[first]) first = i;
            if (first == 0) second = 1;
            for (uint32_t i = 0; i < t->colors; ++i)
                if (i != first && count[i] > count[second]) second = i;
            t->pixels[y * w + x] = (uint8_t)(count[second] * 5 >= n && count[second] ? second : first);
        }
    return 1;
}

/* --- writing and reading ------------------------------------------------------------------- */

int zr_states_write(const zr_machine *m, const zr_states_info *x, zr_state_writer *w) {
    zr_machine_save(m, w);
    if (x) {
        for (unsigned i = 0; i < ZR_CONFIG_MEDIA; ++i) {
            const zr_states_media *md = &x->media[i];
            if (!md->present) continue;
            zr_sw_begin(w, "MDIA", i);
            zr_sw_str(w, md->path);
            zr_sw_u8(w, (uint8_t)((md->hashed ? MEDIA_HASHED : 0) | (md->write_protect ? MEDIA_PROTECTED : 0)));
            zr_sw_bytes(w, md->sha256, sizeof md->sha256);
            zr_sw_end(w);
        }
        if (x->has_thumb && x->thumb.width && x->thumb.height) {
            const zr_states_thumb *t = &x->thumb;
            zr_sw_begin(w, "THMB", 0);
            zr_sw_u16(w, (uint16_t)t->width);
            zr_sw_u16(w, (uint16_t)t->height);
            zr_sw_u16(w, (uint16_t)t->colors);
            for (uint32_t i = 0; i < t->colors; ++i) zr_sw_u32(w, t->palette[i]);
            zr_sw_packed(w, t->pixels, (size_t)t->width * t->height);
            zr_sw_end(w);
        }
        if (x->has_time) {
            zr_sw_begin(w, "TIME", 0);
            zr_sw_u16(w, x->time.year);
            zr_sw_u8(w, x->time.month);
            zr_sw_u8(w, x->time.day);
            zr_sw_u8(w, x->time.hour);
            zr_sw_u8(w, x->time.minute);
            zr_sw_u8(w, x->time.second);
            zr_sw_end(w);
        }
        if (x->version[0]) {
            zr_sw_begin(w, "VERS", 0);
            zr_sw_str(w, x->version);
            zr_sw_end(w);
        }
    }
    return zr_sw_finish(w);
}

int zr_states_read(const uint8_t *data, size_t size, zr_states_info *info, zr_state_reader *r, char *error, size_t capacity) {
    memset(info, 0, sizeof *info);
    if (!zr_sr_open(r, data, size, error, capacity)) return 0;
    snprintf(info->family, sizeof info->family, "%s", r->family);
    snprintf(info->model, sizeof info->model, "%s", r->model);
    for (unsigned i = 0; i < ZR_CONFIG_MEDIA; ++i) {
        zr_states_media *md = &info->media[i];
        if (!zr_sr_chunk(r, "MDIA", i)) continue;
        zr_sr_str(r, md->path, sizeof md->path);
        const uint8_t flags = zr_sr_u8(r);
        zr_sr_bytes(r, md->sha256, sizeof md->sha256);
        md->present = md->path[0] != 0;
        md->hashed = (flags & MEDIA_HASHED) != 0;
        md->write_protect = (flags & MEDIA_PROTECTED) != 0;
    }
    if (zr_sr_chunk(r, "THMB", 0)) {
        zr_states_thumb *t = &info->thumb;
        t->width = zr_sr_u16(r);
        t->height = zr_sr_u16(r);
        t->colors = zr_sr_u16(r);
        if (t->width && t->height && t->width <= ZR_STATES_THUMB_W && t->height <= ZR_STATES_THUMB_H && t->colors && t->colors <= 256) {
            for (uint32_t i = 0; i < t->colors; ++i) t->palette[i] = zr_sr_u32(r) & 0xFFFFFFu;
            zr_sr_packed(r, t->pixels, (size_t)t->width * t->height);
            info->has_thumb = zr_sr_ok(r);
            for (size_t i = 0; info->has_thumb && i < (size_t)t->width * t->height; ++i)
                if (t->pixels[i] >= t->colors) t->pixels[i] = 0;
        }
        r->failed = 0;                  /* a damaged thumbnail is no thumbnail; the machine's chunks decide the rest */
        if (!info->has_thumb) memset(t, 0, sizeof *t);
    }
    if (zr_sr_chunk(r, "TIME", 0)) {
        info->time.year = zr_sr_u16(r);
        info->time.month = zr_sr_u8(r);
        info->time.day = zr_sr_u8(r);
        info->time.hour = zr_sr_u8(r);
        info->time.minute = zr_sr_u8(r);
        info->time.second = zr_sr_u8(r);
        info->has_time = info->time.month >= 1 && info->time.month <= 12 && info->time.day >= 1 && info->time.day <= 31 &&
                         info->time.hour < 24 && info->time.minute < 60 && info->time.second < 61;
    }
    if (zr_sr_chunk(r, "VERS", 0)) zr_sr_str(r, info->version, sizeof info->version);
    r->failed = 0;
    return 1;
}

/* --- files and slots ---------------------------------------------------------------------- */

void zr_states_slot_path(const char *config_source, const char *model, unsigned slot, char *out, size_t capacity) {
    const char sep = zr_host_separator();
    size_t folder = 0;
    for (size_t i = 0; config_source[i]; ++i)
        if (config_source[i] == '/' || config_source[i] == sep) folder = i + 1;
    snprintf(out, capacity, "%.*sstates%c%s%cslot-%u.zrs", (int)folder, config_source, sep, model, sep, slot);
}

int zr_states_save_file(const char *path, const uint8_t *data, size_t size, char *error, size_t capacity) {
    char folder[ZR_HOST_PATH];
    snprintf(folder, sizeof folder, "%s", path);
    char *cut = NULL;
    for (char *c = folder; *c; ++c) if (*c == '/' || *c == zr_host_separator()) cut = c;
    if (cut && cut != folder) {
        *cut = 0;
        if (!zr_host_make_folders(folder)) { snprintf(error, capacity, "Cannot make the folder %s", folder); return 0; }
    }
    return zr_host_write_file(path, data, size, error, capacity);
}

int zr_states_load_file(const char *path, uint8_t **data, size_t *size, char *error, size_t capacity) {
    int writable = 0;
    return zr_host_read_file(path, ZR_MEDIA_LIMIT, data, size, &writable, error, capacity);
}

int zr_states_slot_info(const char *path, zr_states_info *info) {
    uint8_t *data = NULL;
    size_t size = 0;
    char error[256];
    zr_state_reader r;
    memset(info, 0, sizeof *info);
    if (!zr_host_file_exists(path) || !zr_states_load_file(path, &data, &size, error, sizeof error)) return 0;
    const int ok = zr_states_read(data, size, info, &r, error, sizeof error);
    free(data);
    return ok;
}

void zr_states_time_text(const zr_states_time *t, char *out, size_t capacity) {
    snprintf(out, capacity, "%04u-%02u-%02u %02u:%02u", (unsigned)t->year, (unsigned)t->month, (unsigned)t->day, (unsigned)t->hour,
             (unsigned)t->minute);
}

/* --- the rewind ring ---------------------------------------------------------------------- */

void zr_rewind_init(zr_rewind *r, size_t budget) {
    memset(r, 0, sizeof *r);
    r->budget = budget;
    zr_sw_init(&r->scratch);
}

void zr_rewind_free(zr_rewind *r) {
    for (size_t i = 0; i < r->room; ++i) free(r->entries[i].data);
    free(r->entries);
    zr_sw_free(&r->scratch);
    memset(r, 0, sizeof *r);
}

static zr_rewind_entry *entry(const zr_rewind *r, size_t index) { return &r->entries[(r->first + index) % r->room]; }

/* The oldest state goes; its buffer is kept for the next. */
static void drop_oldest(zr_rewind *r) {
    zr_rewind_entry *e = entry(r, 0);
    e->size = 0;
    r->first = (r->first + 1) % r->room;
    --r->count;
}

void zr_rewind_clear(zr_rewind *r) {
    for (size_t i = 0; i < r->room; ++i) {
        free(r->entries[i].data);
        memset(&r->entries[i], 0, sizeof r->entries[i]);
    }
    r->first = r->count = r->bytes = 0;
    r->next = 0;
}

/* Room for one more entry in the ring (the oldest dropped when the ring is full and cannot grow). */
static int make_room(zr_rewind *r) {
    if (r->count < r->room) return 1;
    const size_t room = r->room ? r->room * 2 : 256;
    zr_rewind_entry *grown = calloc(room, sizeof *grown);
    if (!grown) {
        if (!r->count) return 0;
        drop_oldest(r);
        return 1;
    }
    for (size_t i = 0; i < r->count; ++i) grown[i] = *entry(r, i);
    /* The entries not in use keep their buffers (counted in bytes) at the end. */
    size_t n = r->count;
    for (size_t i = r->count; i < r->room; ++i) grown[n++] = *entry(r, i);
    free(r->entries);
    r->entries = grown;
    r->room = room;
    r->first = 0;
    return 1;
}

int zr_rewind_take(zr_rewind *r, const zr_machine *m, uint64_t interval) {
    const uint64_t now = zr_machine_time(m);
    if (now < r->next) return 0;
    r->scratch.size = 0;
    r->scratch.chunk = 0;
    r->scratch.failed = 0;
    if (!zr_states_write(m, NULL, &r->scratch)) return -1;
    const size_t size = r->scratch.size;
    /* The oldest states go while the new one would not fit the budget. */
    while (r->count && r->bytes + size > r->budget) {
        zr_rewind_entry *old = entry(r, 0);
        r->bytes -= old->capacity;
        free(old->data);
        old->data = NULL;
        old->capacity = 0;
        drop_oldest(r);
    }
    if (!make_room(r)) return -1;
    zr_rewind_entry *e = &r->entries[(r->first + r->count) % r->room];
    if (e->capacity < size) {
        uint8_t *data = realloc(e->data, size);
        if (!data) return -1;
        r->bytes += size - e->capacity;
        e->data = data;
        e->capacity = size;
    }
    memcpy(e->data, r->scratch.data, size);
    e->size = size;
    e->time = now;
    ++r->count;
    r->next = now + interval;
    return 1;
}

uint64_t zr_rewind_time(const zr_rewind *r, size_t index) { return index < r->count ? entry(r, index)->time : 0; }

int zr_rewind_restore(const zr_rewind *r, zr_machine *m, size_t index, char *error, size_t capacity) {
    if (index >= r->count) { snprintf(error, capacity, "There is no such state"); return 0; }
    const zr_rewind_entry *e = entry(r, index);
    zr_state_reader rd;
    return zr_sr_open(&rd, e->data, e->size, error, capacity) && zr_machine_load(m, &rd, error, capacity);
}

void zr_rewind_cut(zr_rewind *r, size_t index, uint64_t interval) {
    if (index >= r->count) return;
    for (size_t i = index + 1; i < r->count; ++i) {
        zr_rewind_entry *e = entry(r, i);
        r->bytes -= e->capacity;
        free(e->data);
        memset(e, 0, sizeof *e);
    }
    r->count = index + 1;
    r->next = entry(r, index)->time + interval;
}

int zr_rewind_find(const zr_rewind *r, uint64_t time, size_t *index) {
    for (size_t i = r->count; i-- > 0;)
        if (entry(r, i)->time <= time) { *index = i; return 1; }
    return 0;
}
