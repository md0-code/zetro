/* Tapes: the container of blocks, names for a browser, opening a file by
 * its content (tape.h). ZetRo's own code. */
#include "tape_impl.h"

#include <stdlib.h>
#include <string.h>

/* --- the container ------------------------------------------------------------------------ */

zr_tape *zr_tape_new(void) {
    zr_tape *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->arena = malloc(64);                  /* never NULL, so that an offset into it is always good */
    if (!t->arena) { free(t); return NULL; }
    t->arena_cap = 64;
    return t;
}

void zr_tape_close(zr_tape *t) {
    if (!t) return;
    free(t->blocks);
    free(t->arena);
    free(t->runs);
    free(t);
}

zr_tape_block *zr_tape_add_block(zr_tape *t, uint8_t kind) {
    if (t->n >= ZR_TAPE_MAX_BLOCKS) return NULL;
    if (t->n >= t->cap) {
        const uint32_t cap = t->cap ? t->cap * 2 : 64;
        zr_tape_block *p = realloc(t->blocks, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        t->blocks = p;
        t->cap = cap;
    }
    zr_tape_block *b = &t->blocks[t->n++];
    memset(b, 0, sizeof *b);
    b->kind = kind;
    b->used = 8;
    return b;
}

int zr_tape_put(zr_tape *t, const void *data, size_t n, size_t *off) {
    if (n > ZR_TAPE_MAX_ARENA || t->arena_n > ZR_TAPE_MAX_ARENA - n) return 0;
    if (t->arena_n + n > t->arena_cap) {
        size_t cap = t->arena_cap ? t->arena_cap : 4096;
        while (cap < t->arena_n + n) cap *= 2;
        if (cap > ZR_TAPE_MAX_ARENA) cap = ZR_TAPE_MAX_ARENA;
        uint8_t *p = realloc(t->arena, cap);
        if (!p) return 0;
        t->arena = p;
        t->arena_cap = cap;
    }
    if (n) memcpy(t->arena + t->arena_n, data, n);
    *off = t->arena_n;
    t->arena_n += n;
    return 1;
}

int zr_tape_put_bits(zr_tape *t, uint32_t zero, uint32_t one, size_t *off) {
    const uint8_t b[8] = {(uint8_t)zero, (uint8_t)(zero >> 8), (uint8_t)zero, (uint8_t)(zero >> 8),
                          (uint8_t)one, (uint8_t)(one >> 8), (uint8_t)one, (uint8_t)(one >> 8)};
    return zr_tape_put(t, b, sizeof b, off);
}

int zr_tape_rom_bits(zr_tape *t, size_t *off) {
    if (!t->rom_table) {
        size_t at;
        if (!zr_tape_put_bits(t, 855, 1710, &at)) return 0;
        t->rom_table = at + 1;
    }
    *off = t->rom_table - 1;
    return 1;
}

int zr_tape_run(zr_tape *t, uint32_t first, uint32_t duration, uint32_t count) {
    if (!count) return 1;
    if (t->runs_n > first) {
        zr_pulse *last = &t->runs[t->runs_n - 1];
        if (last->duration == duration && last->count <= 0x7FFFFFFFu && count <= 0x7FFFFFFFu) { last->count += count; return 1; }
    }
    if (t->runs_n >= ZR_TAPE_MAX_RUNS) return 0;
    if (t->runs_n >= t->runs_cap) {
        const size_t cap = t->runs_cap ? t->runs_cap * 2 : 1024;
        zr_pulse *p = realloc(t->runs, cap * sizeof *p);
        if (!p) return 0;
        t->runs = p;
        t->runs_cap = cap;
    }
    t->runs[t->runs_n].duration = duration;
    t->runs[t->runs_n].count = count;
    ++t->runs_n;
    return 1;
}

int zr_tape_fill_standard(zr_tape *t, zr_tape_block *b, size_t off, size_t len, unsigned pause_ms) {
    size_t table;
    if (!zr_tape_rom_bits(t, &table)) return 0;
    b->kind = ZR_TB_STANDARD;
    b->off = off;
    b->len = len;
    b->used = 8;
    b->pause = pause_ms;
    b->u.d.pilot = 2168;
    b->u.d.pilot_count = len && t->arena[off] >= 128 ? 3223 : 8063;
    b->u.d.sync1 = 667;
    b->u.d.sync2 = 735;
    b->u.d.sync_n = 2;
    b->u.d.zero_n = b->u.d.one_n = 2;
    b->u.d.table = table;
    return 1;
}

int zr_tape_append_standard(zr_tape *t, const uint8_t *bytes, size_t length, unsigned pause_ms) {
    size_t off;
    if (!zr_tape_put(t, bytes, length, &off)) return 0;
    zr_tape_block *b = zr_tape_add_block(t, ZR_TB_STANDARD);
    if (!b) return 0;
    if (!zr_tape_fill_standard(t, b, off, length, pause_ms)) {
        --t->n;
        return 0;
    }
    zr_tape_measure(t, b);
    t->modified = 1;
    return 1;
}

/* --- lengths ------------------------------------------------------------------------------- */

void zr_tape_measure(const zr_tape *t, zr_tape_block *b) {
    uint64_t total = 0;
    switch (b->kind) {
    case ZR_TB_STANDARD: case ZR_TB_TURBO: case ZR_TB_TONE: case ZR_TB_DATA: {
        total = (uint64_t)b->u.d.pilot * b->u.d.pilot_count;
        if (b->u.d.sync_n) total += (uint64_t)b->u.d.sync1 + b->u.d.sync2;
        if (b->len) {
            uint64_t zsum = 0, osum = 0, zeros = 0, ones = 0;
            for (unsigned k = 0; k < b->u.d.zero_n; ++k) zsum += zr_rd16(t->arena + b->u.d.table + 2 * k);
            for (unsigned k = 0; k < b->u.d.one_n; ++k) osum += zr_rd16(t->arena + b->u.d.table + 2 * ((size_t)b->u.d.zero_n + k));
            for (size_t i = 0; i < b->len; ++i) {
                const unsigned bits = i + 1 == b->len ? b->used : 8;
                const unsigned v = t->arena[b->off + i] >> (8 - bits);
                unsigned c = 0;
                for (unsigned m = v; m; m &= m - 1) ++c;
                ones += c;
                zeros += bits - c;
            }
            total += zeros * zsum + ones * osum;
        }
        total += b->u.d.tail;
        total += (uint64_t)b->pause * (ZR_TAPE_TSTATE_HZ / 1000u);
        break;
    }
    case ZR_TB_DIRECT:
        if (b->len) total = ((uint64_t)(b->len - 1) * 8 + b->used) * b->u.direct.sample;
        total += (uint64_t)b->pause * (ZR_TAPE_TSTATE_HZ / 1000u);
        break;
    case ZR_TB_PULSES: case ZR_TB_AUDIO:
        for (uint32_t i = 0; i < b->u.r.count; ++i) total += (uint64_t)t->runs[b->u.r.first + i].duration * t->runs[b->u.r.first + i].count;
        total += (uint64_t)b->pause * (ZR_TAPE_TSTATE_HZ / 1000u);
        break;
    case ZR_TB_PAUSE:
        total = (uint64_t)b->pause * (ZR_TAPE_TSTATE_HZ / 1000u);
        break;
    default:
        break;
    }
    b->tstates = total;
}

/* --- data blocks ---------------------------------------------------------------------------- */

int zr_tape_block_is_rom_data(const zr_tape *t, const zr_tape_block *b) {
    if (b->kind == ZR_TB_STANDARD) return 1;
    if (b->kind != ZR_TB_TURBO) return 0;
    if (b->u.d.pilot != 2168 || b->u.d.sync1 != 667 || b->u.d.sync2 != 735 || b->u.d.sync_n != 2 || b->used != 8 || b->u.d.tail) return 0;
    if (b->u.d.zero_n != 2 || b->u.d.one_n != 2) return 0;
    const uint8_t *tab = t->arena + b->u.d.table;
    return zr_rd16(tab) == 855 && zr_rd16(tab + 2) == 855 && zr_rd16(tab + 4) == 1710 && zr_rd16(tab + 6) == 1710;
}

int zr_tape_block_data(const zr_tape *t, unsigned index, const uint8_t **bytes, size_t *length, unsigned *flag, unsigned *pause_ms) {
    if (!t || index >= t->n) return 0;
    const zr_tape_block *b = &t->blocks[index];
    if (!zr_tape_block_is_rom_data(t, b)) return 0;
    *bytes = t->arena + b->off;
    *length = b->len;
    if (flag) *flag = b->len ? t->arena[b->off] : 0;
    if (pause_ms) *pause_ms = b->pause;
    return 1;
}

/* --- names ---------------------------------------------------------------------------------- */

/* Appends text (ISO 8859-1, printable ASCII kept, the rest a question mark, line ends spaces) to out. */
static void append_text(char *out, size_t capacity, const uint8_t *text, size_t n) {
    size_t at = strlen(out);
    for (size_t i = 0; i < n && at + 1 < capacity && i < 60; ++i) {
        const uint8_t c = text[i];
        out[at++] = (char)(c >= 0x20 && c < 0x7F ? c : (c == 0x0D || c == 0x0A || c == '\t') ? ' ' : '?');
    }
    out[at] = 0;
}

static void append_fmt_seconds(char *out, size_t capacity, const char *prefix, uint64_t tstates) {
    const uint64_t tenths = (tstates + ZR_TAPE_TSTATE_HZ / 20u) / (ZR_TAPE_TSTATE_HZ / 10u);
    const size_t at = strlen(out);
    if (at + 1 < capacity)
        snprintf(out + at, capacity - at, "%s%u.%u s", prefix, (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

static void appendf(char *out, size_t capacity, const char *text) {
    const size_t at = strlen(out);
    if (at + 1 < capacity) snprintf(out + at, capacity - at, "%s", text);
}

static void appendu(char *out, size_t capacity, const char *prefix, uint64_t value, const char *suffix) {
    const size_t at = strlen(out);
    if (at + 1 < capacity) snprintf(out + at, capacity - at, "%s%llu%s", prefix, (unsigned long long)value, suffix);
}

/* The title of an archive info block (the first entry of type 0), else the first entry. */
static void archive_title(const zr_tape *t, const zr_tape_block *b, char *out, size_t capacity) {
    const uint8_t *p = t->arena + b->raw;
    const size_t n = b->raw_len;
    size_t at = 4;                                          /* ID, WORD length, count */
    const unsigned count = n > 3 ? p[3] : 0;
    int first = 1;
    for (unsigned i = 0; i < count && at + 2 <= n; ++i) {
        const unsigned id = p[at], len = p[at + 1];
        at += 2;
        if (at + len > n) break;
        if (id == 0 || first) {
            out[0] = 0;
            append_text(out, capacity, p + at, len);
            first = 0;
            if (id == 0) return;
        }
        at += len;
    }
}

void zr_tape_block_name(const zr_tape *t, unsigned index, char *out, size_t capacity) {
    if (!capacity) return;
    out[0] = 0;
    if (!t || index >= t->n) return;
    const zr_tape_block *b = &t->blocks[index];
    const uint8_t *data = t->arena + b->off;
    switch (b->kind) {
    case ZR_TB_STANDARD:
        if (b->len == 19 && data[0] == 0 && data[1] <= 3) {
            static const char *const TYPES[4] = {"Program: ", "Number array: ", "Character array: ", "Bytes: "};
            appendf(out, capacity, TYPES[data[1]]);
            size_t n = 10;
            while (n && data[2 + n - 1] == ' ') --n;
            append_text(out, capacity, data + 2, n);
        } else {
            appendu(out, capacity, "Data ", b->len >= 2 ? b->len - 2 : b->len, " bytes");
        }
        break;
    case ZR_TB_TURBO:
        appendu(out, capacity, "Turbo ", b->len >= 2 ? b->len - 2 : b->len, " bytes");
        break;
    case ZR_TB_TONE:
        appendu(out, capacity, "Tone ", b->u.d.pilot_count, " x ");
        appendu(out, capacity, "", b->u.d.pilot, "");
        break;
    case ZR_TB_PULSES: {
        if (b->flags & ZR_TF_PAUSE) { append_fmt_seconds(out, capacity, "Pause ", b->tstates); break; }
        uint64_t pulses = 0;
        for (uint32_t i = 0; i < b->u.r.count; ++i) pulses += t->runs[b->u.r.first + i].count;
        appendu(out, capacity, "Pulses ", pulses, "");
        break;
    }
    case ZR_TB_DATA:
        appendu(out, capacity, "Pure data ", b->len, " bytes");
        break;
    case ZR_TB_DIRECT:
        append_fmt_seconds(out, capacity, "Direct recording ", b->tstates);
        break;
    case ZR_TB_AUDIO:
        append_fmt_seconds(out, capacity, "Audio ", b->tstates);
        break;
    case ZR_TB_GENERAL:
        appendf(out, capacity, "Generalized data");
        break;
    case ZR_TB_PAUSE:
        append_fmt_seconds(out, capacity, "Pause ", b->tstates);
        break;
    case ZR_TB_STOP:
        appendf(out, capacity, "Stop the tape");
        break;
    case ZR_TB_STOP48:
        appendf(out, capacity, "Stop the tape if 48K");
        break;
    case ZR_TB_LEVEL:
        appendf(out, capacity, b->level ? "Level high" : "Level low");
        break;
    case ZR_TB_GROUP:
        appendf(out, capacity, "Group: ");
        append_text(out, capacity, data, b->len);
        break;
    case ZR_TB_GROUP_END:
        appendf(out, capacity, "Group end");
        break;
    case ZR_TB_JUMP:
        snprintf(out, capacity, "Jump %+d", (int)b->u.f.value);
        break;
    case ZR_TB_LOOP:
        appendu(out, capacity, "Loop ", (uint64_t)b->u.f.value, " times");
        break;
    case ZR_TB_LOOP_END:
        appendf(out, capacity, "Loop end");
        break;
    case ZR_TB_CALL:
        appendf(out, capacity, "Call sequence");
        break;
    case ZR_TB_RETURN:
        appendf(out, capacity, "Return");
        break;
    case ZR_TB_SELECT:
        appendf(out, capacity, "Select");
        break;
    case ZR_TB_TEXT:
        append_text(out, capacity, data, b->len);
        if (!out[0]) appendf(out, capacity, "Text");
        break;
    case ZR_TB_INFO:
        if (b->raw_len) {
            const unsigned id = t->arena[b->raw];
            if (id == 0x32) {
                appendf(out, capacity, "Archive: ");
                archive_title(t, b, out + strlen(out), capacity - strlen(out));
                if (strlen(out) == 9) out[7] = 0;
            } else if (id == 0x33) appendf(out, capacity, "Hardware type");
            else if (id == 0x35) {
                appendf(out, capacity, "Custom: ");
                size_t n = 10;
                while (n && t->arena[b->raw + 1 + n - 1] == ' ') --n;
                if (b->raw_len >= 11) append_text(out, capacity, t->arena + b->raw + 1, n);
            } else if (id == 0x5A) appendf(out, capacity, "Glue");
            else if (id == 0x34) appendf(out, capacity, "Emulation info");
            else if (id == 0x40) appendf(out, capacity, "Snapshot");
            else {
                appendu(out, capacity, "Block ", id, "");
            }
        } else {
            appendf(out, capacity, "Info");
        }
        break;
    default:
        break;
    }
}

/* --- the rest of the tape's interface --------------------------------------------------------- */

int zr_tape_format(const zr_tape *t) { return t ? t->format : ZR_TAPE_NONE; }
unsigned zr_tape_count(const zr_tape *t) { return t ? t->n : 0; }
int zr_tape_modified(const zr_tape *t) { return t && t->modified; }

int zr_tape_block_kind(const zr_tape *t, unsigned index) {
    return t && index < t->n ? t->blocks[index].kind : -1;
}

uint64_t zr_tape_block_tstates(const zr_tape *t, unsigned index) {
    return t && index < t->n ? t->blocks[index].tstates : 0;
}

/* --- opening -------------------------------------------------------------------------------- */

zr_tape *zr_tape_open(const zr_media *media, char *error, size_t capacity) {
    static const char CSW_SIGNATURE[] = "Compressed Square Wave\x1A";
    const uint8_t *d = media->data;
    const size_t n = media->size;
    int format = ZR_TAPE_NONE;
    if (n >= 8 && !memcmp(d, "ZXTape!\x1A", 8)) format = ZR_TAPE_TZX;
    else if (n >= 4 && !memcmp(d, "PZXT", 4)) format = ZR_TAPE_PZX;
    else if (n >= 23 && !memcmp(d, CSW_SIGNATURE, 23)) format = ZR_TAPE_CSW;
    else if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WAVE", 4)) format = ZR_TAPE_WAV;
    else if (zr_media_extension_in(media->path, "tap")) format = ZR_TAPE_TAP;
    if (n == 0) {
        /* An empty file is a tape waiting for a recording, in the format its name says. */
        if (zr_media_extension_in(media->path, "tap")) format = ZR_TAPE_TAP;
        else if (zr_media_extension_in(media->path, "tzx")) format = ZR_TAPE_TZX;
        else if (zr_media_extension_in(media->path, "pzx")) format = ZR_TAPE_PZX;
        else {
            if (capacity) snprintf(error, capacity, "The file is empty");
            return NULL;
        }
    }
    if (format == ZR_TAPE_NONE) {
        if (capacity) snprintf(error, capacity, "Not a tape file (TAP, TZX, PZX, CSW or WAV)");
        return NULL;
    }
    zr_tape *t = zr_tape_new();
    if (!t) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    t->format = format;
    int ok = 1;
    if (n) {
        switch (format) {
        case ZR_TAPE_TAP: ok = zr_tape_read_tap(t, d, n, error, capacity); break;
        case ZR_TAPE_TZX: ok = zr_tape_read_tzx(t, d, n, error, capacity); break;
        case ZR_TAPE_PZX: ok = zr_tape_read_pzx(t, d, n, error, capacity); break;
        case ZR_TAPE_CSW: ok = zr_tape_read_csw(t, d, n, error, capacity); break;
        default: ok = zr_tape_read_wav(t, d, n, error, capacity); break;
        }
    }
    if (!ok) {
        zr_tape_close(t);
        return NULL;
    }
    for (uint32_t i = 0; i < t->n; ++i) zr_tape_measure(t, &t->blocks[i]);
    return t;
}
