/* The harness (harness.h). ZetRo's own code. */
#include "harness.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_ 1024
#define TEXT_MAX 8192
#define REPLY_MAX (TEXT_MAX * 2 + 256)
#define KEY_FRAMES 2u                   /* a typed key is held this many frames, then released as many */
#define CLICK_HOLD 3u                   /* a click on the on-screen keyboard holds the key this many frames ... */
#define CLICK_REST 8u                   /* ... and the command lasts this many more, so the same key can be clicked again */

enum { DONE, FAILED, STARTED };

struct zr_harness {
    zr_harness_host host;
    /* the script */
    char **lines;
    size_t *numbers;                    /* each kept line's number in the file */
    size_t count, next;
    /* the command in progress */
    int busy, from_control;
    uint64_t wait;                      /* frames still to run */
    char typing[LINE_MAX_];
    size_t type_at, type_len;
    unsigned type_phase;
    int click;                          /* the on-screen keyboard's press in progress: 1 the pointer's, 2 the pad's A */
    unsigned click_phase;
    char until[LINE_MAX_];
    int until_active;
    /* the result of the last command, for the answer */
    char result[TEXT_MAX];
    int result_kind;                    /* 0 none, 1 "text", 2 "data" */
    uint64_t frame;                     /* frames run so far */
    FILE *trace;
    int quit, status;
    char error[512];
};

/* --- small things ------------------------------------------------------------------- */

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static int failf(zr_harness *h, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(h->error, sizeof h->error, format, args);
    va_end(args);
    return FAILED;
}

static zr_machine *machine(zr_harness *h) { return h->host.machine(h->host.ctx); }

/* The first word of *text into word (cut to its room); *text moves past it and the spaces after. */
static void take_word(const char **text, char *word, size_t capacity) {
    size_t n = 0;
    const char *p = *text;
    while (*p && *p != ' ' && *p != '\t') { if (n + 1 < capacity) word[n++] = *p; ++p; }
    word[n] = 0;
    while (*p == ' ' || *p == '\t') ++p;
    *text = p;
}

static uint64_t frames_of(zr_harness *h, double seconds) {
    zr_machine *m = machine(h);
    const double frames = ceil(seconds * (double)zr_machine_clock(m) / (double)zr_machine_frame_period(m));
    return frames < 1.0 ? 1u : (uint64_t)frames;
}

static void trace_line(void *ctx, int kind, uint32_t address, uint32_t value) {
    FILE *f = ctx;
    if (kind == ZR_TRACE_IO_READ) fprintf(f, "IN %04X %02X\n", (unsigned)address, (unsigned)value);
    else if (kind == ZR_TRACE_IO_WRITE) fprintf(f, "OUT %04X %02X\n", (unsigned)address, (unsigned)value);
    else if (kind == ZR_TRACE_INTERRUPT) fprintf(f, "INT %02X\n", (unsigned)address);
}

static void trace_stop(zr_harness *h) {
    if (!h->trace) return;
    zr_machine_set_trace(machine(h), NULL, NULL);
    fclose(h->trace);
    h->trace = NULL;
}

/* The slot a command names, by its key in the family's table; -1 with the error set. */
static int slot_of(zr_harness *h, const char *key) {
    const zr_family *f = zr_machine_family(machine(h));
    const int slot = zr_family_media(f, key);
    if (slot < 0 || !(f->media[slot].models >> zr_machine_model(machine(h)) & 1u)) {
        failf(h, "This machine has no media slot %s", key);
        return -1;
    }
    return slot;
}

static int tape_slot(zr_harness *h) {
    const zr_family *f = zr_machine_family(machine(h));
    for (unsigned i = 0; i < f->media_count; ++i)
        if (f->media[i].kind == ZR_MEDIA_TAPE && (f->media[i].models >> zr_machine_model(machine(h)) & 1u)) return (int)i;
    failf(h, "This machine has no tape");
    return -1;
}

/* --- the commands ------------------------------------------------------------------- */

static int start_typing(zr_harness *h, const char *text) {
    const zr_family *f = zr_machine_family(machine(h));
    size_t n = 0;
    for (const char *p = text; *p && n + 1 < sizeof h->typing; ++p) {
        char ch = *p;
        if (ch == '\\' && p[1]) {
            ++p;
            ch = *p == 'n' ? '\n' : *p == 'b' ? '\b' : *p == 't' ? '\t' : *p;
        }
        uint8_t key, shift;
        if (!zr_family_typing(f, ch, &key, &shift)) return failf(h, "This keyboard cannot type '%c'", ch);
        h->typing[n++] = ch;
    }
    if (!n) return DONE;
    h->type_len = n;
    h->type_at = 0;
    h->type_phase = 0;
    return STARTED;
}

/* The characters on the screen: the setup screen's while it is open, else the machine's. */
static void screen_text(zr_harness *h, char *out, size_t capacity) {
    if (h->host.screen_text && h->host.screen_text(h->host.ctx, out, capacity)) return;
    zr_machine_text(machine(h), out, capacity);
}

static int on_screen(zr_harness *h, const char *text) {
    static char screen[TEXT_MAX];
    screen_text(h, screen, sizeof screen);
    return strstr(screen, text) != NULL;
}

static int hex(const char **text, unsigned long limit, unsigned long *out) {
    char word[32], *end = NULL;
    take_word(text, word, sizeof word);
    if (!word[0]) return 0;
    *out = strtoul(word, &end, 16);
    return end && !*end && *out <= limit;
}

static int execute(zr_harness *h, const char *line) {
    char cmd[32], word[64], error[256] = "";
    const char *arg = line;
    zr_machine *m = machine(h);
    take_word(&arg, cmd, sizeof cmd);
    h->result_kind = 0;
    if (!strcmp(cmd, "wait")) {
        char *end = NULL;
        const double seconds = strtod(arg, &end);
        if (end == arg || *end || seconds < 0 || seconds > 86400) return failf(h, "wait needs a number of seconds");
        h->wait = frames_of(h, seconds);
        return STARTED;
    }
    if (!strcmp(cmd, "frames")) {
        char *end = NULL;
        const unsigned long long n = strtoull(arg, &end, 10);
        if (end == arg || *end || !n) return failf(h, "frames needs a whole number");
        h->wait = n;
        return STARTED;
    }
    if (!strcmp(cmd, "type")) return start_typing(h, arg);
    if (!strcmp(cmd, "enter")) return start_typing(h, "\n");
    if (!strcmp(cmd, "key") || !strcmp(cmd, "release")) {
        const int key = zr_family_key(zr_machine_family(m), arg);
        if (key < 0) return failf(h, "This keyboard has no key %s", arg);
        zr_machine_key(m, (unsigned)key, cmd[0] == 'k');
        return DONE;
    }
    if (!strcmp(cmd, "joy")) {
        unsigned long stick = 0;
        unsigned held = 0;
        take_word(&arg, word, sizeof word);
        char *end = NULL;
        stick = strtoul(word, &end, 10);
        if (!word[0] || *end || stick >= zr_machine_family(m)->joysticks) return failf(h, "This machine has no joystick %s", word);
        while (*arg) {
            take_word(&arg, word, sizeof word);
            if (!strcmp(word, "up")) held |= ZR_JOY_UP;
            else if (!strcmp(word, "down")) held |= ZR_JOY_DOWN;
            else if (!strcmp(word, "left")) held |= ZR_JOY_LEFT;
            else if (!strcmp(word, "right")) held |= ZR_JOY_RIGHT;
            else if (!strcmp(word, "fire")) held |= ZR_JOY_FIRE;
            else if (!strcmp(word, "fire2")) held |= ZR_JOY_FIRE2;
            else return failf(h, "joy takes up, down, left, right, fire and fire2, not %s", word);
        }
        zr_machine_joystick(m, (unsigned)stick, held);
        return DONE;
    }
    if (!strcmp(cmd, "insert") || !strcmp(cmd, "eject")) {
        take_word(&arg, word, sizeof word);
        const int slot = slot_of(h, word);
        if (slot < 0) return FAILED;
        if (cmd[0] == 'e') return h->host.eject(h->host.ctx, (unsigned)slot) ? DONE : failf(h, "The slot %s is empty", word);
        if (!*arg) return failf(h, "insert needs a slot and a file");
        return h->host.insert(h->host.ctx, (unsigned)slot, arg, error, sizeof error) ? DONE : failf(h, "%s: %s", arg, error);
    }
    if (!strcmp(cmd, "drop")) {
        if (!*arg) return failf(h, "drop needs a file");
        if (!h->host.drop) return failf(h, "This run takes no drops");
        return h->host.drop(h->host.ctx, arg, error, sizeof error) ? DONE : failf(h, "%s: %s", arg, error);
    }
    if (!strcmp(cmd, "play")) {
        const int slot = tape_slot(h);
        if (slot < 0) return FAILED;
        if (!*arg) return failf(h, "play needs a tape file");
        if (!h->host.insert(h->host.ctx, (unsigned)slot, arg, error, sizeof error)) return failf(h, "%s: %s", arg, error);
        zr_machine_control(machine(h), (unsigned)slot, ZR_MEDIA_PLAY, 0);
        return DONE;
    }
    if (!strcmp(cmd, "tape")) {
        static const char *const KEYS[] = {"play", "stop", "rewind", "next", "previous", NULL, "record"};
        const int slot = tape_slot(h);
        if (slot < 0) return FAILED;
        for (unsigned i = 0; i < sizeof KEYS / sizeof *KEYS; ++i)
            if (KEYS[i] && !strcmp(arg, KEYS[i]))
                return zr_machine_control(m, (unsigned)slot, i, 0) ? DONE : failf(h, "The tape has no %s here", arg);
        return failf(h, "tape takes play, stop, rewind, next, previous or record");
    }
    if (!strcmp(cmd, "osk")) {
        take_word(&arg, word, sizeof word);
        if (!h->host.osk) return failf(h, "This run has no on-screen keyboard");
        char answer[TEXT_MAX];
        answer[0] = 0;
        int op = -1;
        if (!strcmp(word, "on")) op = ZR_HARNESS_OSK_ON;
        else if (!strcmp(word, "off")) op = ZR_HARNESS_OSK_OFF;
        else if (!strcmp(word, "keys")) op = ZR_HARNESS_OSK_KEYS;
        else if (!strcmp(word, "click") && *arg) op = ZR_HARNESS_OSK_MOUSE_DOWN;
        else if (!strcmp(word, "pad")) {
            if (strcmp(arg, "up") && strcmp(arg, "down") && strcmp(arg, "left") && strcmp(arg, "right") && strcmp(arg, "a") && strcmp(arg, "b"))
                return failf(h, "osk pad takes up, down, left, right, a or b");
            op = ZR_HARNESS_OSK_PAD;
        }
        if (op < 0) return failf(h, "osk takes on, off, keys, click KEY or pad up|down|left|right|a|b");
        if (!h->host.osk(h->host.ctx, op, arg, answer, sizeof answer)) return failf(h, "%s", answer);
        if (op == ZR_HARNESS_OSK_KEYS || (op == ZR_HARNESS_OSK_PAD && answer[0])) {
            snprintf(h->result, sizeof h->result, "%s", answer);
            h->result_kind = op == ZR_HARNESS_OSK_KEYS ? 1 : 2;
        }
        if (op == ZR_HARNESS_OSK_MOUSE_DOWN || (op == ZR_HARNESS_OSK_PAD && arg[0] == 'a')) {
            h->click = op == ZR_HARNESS_OSK_MOUSE_DOWN ? 1 : 2;
            h->click_phase = 0;
            return STARTED;
        }
        return DONE;
    }
    if (!strcmp(cmd, "nmi")) { zr_machine_nmi(m); return DONE; }
    if (!strcmp(cmd, "reset")) {
        if (*arg && strcmp(arg, "power") && strcmp(arg, "keys")) return failf(h, "reset takes nothing, power or keys");
        h->host.reset(h->host.ctx, !*arg ? 0 : arg[0] == 'p' ? 1 : 2);
        return DONE;
    }
    if (!strcmp(cmd, "shot")) {
        if (!*arg) return failf(h, "shot needs a file");
        return h->host.shot(h->host.ctx, arg, error, sizeof error) ? DONE : failf(h, "%s: %s", arg, error);
    }
    if (!strcmp(cmd, "text")) {
        screen_text(h, h->result, sizeof h->result);
        h->result_kind = 1;
        return DONE;
    }
    if (!strcmp(cmd, "expect")) {
        if (!*arg) return failf(h, "expect needs a text");
        return on_screen(h, arg) ? DONE : failf(h, "\"%s\" is not on the screen", arg);
    }
    if (!strcmp(cmd, "until")) {
        take_word(&arg, word, sizeof word);
        char *end = NULL;
        const double seconds = strtod(word, &end);
        if (!word[0] || *end || seconds <= 0 || seconds > 86400 || !*arg) return failf(h, "until needs a number of seconds and a text");
        snprintf(h->until, sizeof h->until, "%s", arg);
        h->until_active = 1;
        h->wait = frames_of(h, seconds);
        return STARTED;
    }
    if (!strcmp(cmd, "state")) {
        take_word(&arg, word, sizeof word);
        if (!*arg || (strcmp(word, "save") && strcmp(word, "load"))) return failf(h, "state needs save or load and a file");
        const int ok = word[0] == 's' ? h->host.state_save(h->host.ctx, arg, error, sizeof error) : h->host.state_load(h->host.ctx, arg, error, sizeof error);
        return ok ? DONE : failf(h, "%s: %s", arg, error);
    }
    if (!strcmp(cmd, "rewind")) {
        if (!h->host.rewind) return failf(h, "This run cannot rewind");
        return h->host.rewind(h->host.ctx, arg, error, sizeof error) ? DONE : failf(h, "%s", error);
    }
    if (!strcmp(cmd, "speed")) {
        if (strcmp(arg, "full") && strcmp(arg, "normal")) return failf(h, "speed takes full or normal");
        if (h->host.speed) h->host.speed(h->host.ctx, arg[0] == 'f');
        return DONE;
    }
    if (!strcmp(cmd, "peek")) {
        unsigned long address = 0, count = 1;
        if (!hex(&arg, 0xFFFFFFul, &address) || (*arg && !hex(&arg, 1024, &count)) || !count) return failf(h, "peek needs a hex address and at most 400 (hex) bytes");
        size_t n = 0;
        for (unsigned long i = 0; i < count; ++i) {
            uint8_t value;
            if (!zr_machine_peek(m, (uint32_t)(address + i), &value)) return failf(h, "This machine has no memory at %lX", address + i);
            n += (size_t)snprintf(h->result + n, sizeof h->result - n, "%s%02X", i ? " " : "", value);
        }
        h->result_kind = 2;
        return DONE;
    }
    if (!strcmp(cmd, "poke")) {
        unsigned long address = 0, value = 0;
        if (!hex(&arg, 0xFFFFFFul, &address) || !*arg) return failf(h, "poke needs a hex address and bytes");
        for (; *arg; ++address) {
            if (!hex(&arg, 0xFF, &value)) return failf(h, "poke takes hex bytes");
            if (!zr_machine_poke(m, (uint32_t)address, (uint8_t)value)) return failf(h, "This machine has no memory at %lX", address);
        }
        return DONE;
    }
    if (!strcmp(cmd, "trace")) {
        trace_stop(h);
        if (!*arg) return failf(h, "trace needs a file, or off");
        if (!strcmp(arg, "off")) return DONE;
        if (!(h->trace = fopen(arg, "w"))) return failf(h, "Cannot write %s", arg);
        zr_machine_set_trace(m, trace_line, h->trace);
        return DONE;
    }
    if (!strcmp(cmd, "snapshot")) {
        take_word(&arg, word, sizeof word);
        if (!*arg || (strcmp(word, "load") && strcmp(word, "save"))) return failf(h, "snapshot needs load or save and a file");
        const int ok = word[0] == 'l' ? h->host.snapshot_load && h->host.snapshot_load(h->host.ctx, arg, error, sizeof error)
                                      : h->host.snapshot_save && h->host.snapshot_save(h->host.ctx, arg, error, sizeof error);
        if (!ok && !error[0]) snprintf(error, sizeof error, "This run takes no snapshots");
        return ok ? DONE : failf(h, "%s: %s", arg, error);
    }
    if (!strcmp(cmd, "cheat")) {
        take_word(&arg, word, sizeof word);
        if (!strcmp(word, "load")) {
            if (!*arg) return failf(h, "cheat load needs a .pok file");
            if (!h->host.cheat_load || !h->host.cheat_load(h->host.ctx, arg, error, sizeof error)) return failf(h, "%s: %s", arg, *error ? error : "This run has no cheats");
            return DONE;
        }
        if (strcmp(word, "on") && strcmp(word, "off")) return failf(h, "cheat takes load FILE, on N [VALUE] or off N");
        char number[32], *end = NULL;
        take_word(&arg, number, sizeof number);
        const unsigned long n = strtoul(number, &end, 10);
        if (!number[0] || *end || !n) return failf(h, "cheat %s needs the number of a trainer, the first is 1", word);
        long value = -1;
        if (*arg) {
            take_word(&arg, number, sizeof number);
            value = strtol(number, &end, 10);
            if (!number[0] || *end || value < 0 || value > 255 || *arg) return failf(h, "cheat on takes a value from 0 to 255 after the number");
        }
        if (!h->host.cheat_set || !h->host.cheat_set(h->host.ctx, (size_t)(n - 1), word[1] == 'n', (int)value, error, sizeof error))
            return failf(h, "%s", *error ? error : "This run has no cheats");
        return DONE;
    }
    if (!strcmp(cmd, "quit")) {
        char *end = NULL;
        const long status = *arg ? strtol(arg, &end, 10) : 0;
        if (*arg && (*end || status < 0 || status > 255)) return failf(h, "quit takes an exit status from 0 to 255");
        h->quit = 1;
        h->status = (int)status;
        return DONE;
    }
    return failf(h, "Unknown command %s", cmd);
}

/* --- the answers on the control channel -------------------------------------------------- */

static size_t json_text(char *out, size_t capacity, const char *text) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n + 8 < capacity; ++p) {
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if (*p < 0x20 || *p >= 0x7F) n += (size_t)snprintf(out + n, capacity - n, "\\u%04x", *p);
        else out[n++] = (char)*p;
    }
    out[n] = 0;
    return n;
}

static void answer(zr_harness *h, int ok) {
    static char reply[REPLY_MAX];
    if (!h->from_control || !h->host.control_write) return;
    size_t n;
    if (ok) {
        n = (size_t)snprintf(reply, sizeof reply, "{\"ok\":true,\"frame\":%llu", (unsigned long long)h->frame);
        if (h->result_kind) {
            n += (size_t)snprintf(reply + n, sizeof reply - n, ",\"%s\":\"", h->result_kind == 1 ? "text" : "data");
            n += json_text(reply + n, sizeof reply - n - 4, h->result);
            n += (size_t)snprintf(reply + n, sizeof reply - n, "\"");
        }
    } else {
        n = (size_t)snprintf(reply, sizeof reply, "{\"ok\":false,\"error\":\"");
        n += json_text(reply + n, sizeof reply - n - 4, h->error);
        n += (size_t)snprintf(reply + n, sizeof reply - n, "\"");
    }
    snprintf(reply + n, sizeof reply - n, "}");
    h->host.control_write(h->host.ctx, reply);
}

/* The command in progress is over. A script that fails ends the run. */
static void finish(zr_harness *h, int ok) {
    h->busy = 0;
    h->until_active = 0;
    h->wait = 0;
    h->type_len = 0;
    h->click = 0;
    if (ok) h->error[0] = 0;
    if (h->from_control) { answer(h, ok); return; }
    if (h->result_kind && ok) { fputs(h->result, stdout); if (h->result_kind == 2) fputc('\n', stdout); fflush(stdout); }
    if (!ok) {
        char message[sizeof h->error];
        snprintf(message, sizeof message, "%s", h->error);
        snprintf(h->error, sizeof h->error, "script line %zu: %s", h->numbers[h->next - 1], message);
        h->quit = 1;
        h->status = 1;
    }
}

/* --- the frame ---------------------------------------------------------------------- */

/* The typing in progress: a key goes down (its shift first), stays KEY_FRAMES frames, goes up and stays up as long. */
static void type_frame(zr_harness *h) {
    zr_machine *m = machine(h);
    uint8_t key = 0, shift = ZR_KEY_NONE;
    zr_family_typing(zr_machine_family(m), h->typing[h->type_at], &key, &shift);
    if (h->type_phase == 0) {
        if (shift != ZR_KEY_NONE) zr_machine_key(m, shift, 1);
        zr_machine_key(m, key, 1);
    } else if (h->type_phase == KEY_FRAMES) {
        zr_machine_key(m, key, 0);
        if (shift != ZR_KEY_NONE) zr_machine_key(m, shift, 0);
    }
    /* The same key again is left up longer: a ROM's keyboard routine takes a key a second time only after it
     * has seen it up for several frame interrupts (five on the Spectrum-compatibles). */
    unsigned frames = 2 * KEY_FRAMES;
    if (h->type_at + 1 < h->type_len) {
        uint8_t next = ZR_KEY_NONE, next_shift = ZR_KEY_NONE;
        zr_family_typing(zr_machine_family(m), h->typing[h->type_at + 1], &next, &next_shift);
        if (next == key) frames += 6;
    }
    if (++h->type_phase == frames) {
        h->type_phase = 0;
        ++h->type_at;
    }
}

/* The on-screen keyboard's click in progress: the key stays down CLICK_HOLD frames, then comes up and the command rests CLICK_REST. */
static void click_frame(zr_harness *h) {
    if (h->click_phase == CLICK_HOLD) {
        char text[64];
        h->host.osk(h->host.ctx, h->click == 1 ? ZR_HARNESS_OSK_MOUSE_UP : ZR_HARNESS_OSK_PAD_RELEASE, "", text, sizeof text);
    }
    if (++h->click_phase == CLICK_HOLD + CLICK_REST) h->click = 0;
}

int zr_harness_step(zr_harness *h) {
    for (;;) {
        if (h->quit) return ZR_HARNESS_QUIT;
        if (h->busy) {
            if (h->type_at < h->type_len) { type_frame(h); ++h->frame; return ZR_HARNESS_RUN; }
            if (h->click) { click_frame(h); ++h->frame; return ZR_HARNESS_RUN; }
            if (h->until_active && on_screen(h, h->until)) h->wait = 0;
            else if (h->wait) { --h->wait; ++h->frame; return ZR_HARNESS_RUN; }
            else if (h->until_active) { failf(h, "\"%s\" did not come on the screen", h->until); finish(h, 0); continue; }
            finish(h, 1);
            continue;
        }
        char line[LINE_MAX_];
        if (h->next < h->count) {
            snprintf(line, sizeof line, "%s", h->lines[h->next++]);
            h->from_control = 0;
        } else if (h->host.control_read) {
            const int got = h->host.control_read(h->host.ctx, line, sizeof line);
            if (got < 0) { h->quit = 1; continue; }
            if (!got) {
                if (h->host.deterministic) return ZR_HARNESS_IDLE;
                ++h->frame;
                return ZR_HARNESS_RUN;
            }
            h->from_control = 1;
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
            if (!n || line[0] == '#') continue;
        } else { h->quit = 1; continue; }
        const int r = execute(h, line);
        if (r == STARTED) h->busy = 1;
        else finish(h, r == DONE);
    }
}

int zr_harness_status(const zr_harness *h) { return h->status; }
const char *zr_harness_error(const zr_harness *h) { return h->error; }

/* --- life cycle --------------------------------------------------------------------- */

zr_harness *zr_harness_open(const zr_harness_host *host, const char *script, char *error, size_t capacity) {
    zr_harness *h = calloc(1, sizeof *h);
    if (!h) { snprintf(error, capacity, "Out of memory"); return NULL; }
    h->host = *host;
    if (!script) return h;
    FILE *f = fopen(script, "rb");
    if (!f) { snprintf(error, capacity, "Cannot open the script %s", script); free(h); return NULL; }
    char line[LINE_MAX_];
    size_t number = 0, room = 0;
    int ok = 1;
    while (ok && fgets(line, sizeof line, f)) {
        ++number;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        const char *start = line;
        while (*start == ' ' || *start == '\t') ++start;
        if (!*start || *start == '#') continue;
        if (h->count == room) {
            room = room ? room * 2 : 64;
            char **lines = realloc(h->lines, room * sizeof *lines);
            if (lines) h->lines = lines;
            size_t *numbers = realloc(h->numbers, room * sizeof *numbers);
            if (numbers) h->numbers = numbers;
            if (!lines || !numbers) { ok = 0; break; }
        }
        const size_t length = strlen(start) + 1;
        if (!(h->lines[h->count] = malloc(length))) { ok = 0; break; }
        memcpy(h->lines[h->count], start, length);
        h->numbers[h->count++] = number;
    }
    fclose(f);
    if (!ok) { snprintf(error, capacity, "Out of memory"); zr_harness_close(h); return NULL; }
    return h;
}

void zr_harness_close(zr_harness *h) {
    if (!h) return;
    trace_stop(h);
    for (size_t i = 0; i < h->count; ++i) free(h->lines[i]);
    free(h->lines);
    free(h->numbers);
    free(h);
}
