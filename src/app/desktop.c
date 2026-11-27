/* The desktop shell (desktop.h). ZetRo's own code. */
#include "desktop.h"
#include "harness.h"
#include "cheats.h"
#include "joykeys.h"
#ifndef _WIN32
#include "icon_data.h"
#endif
#include "osk.h"
#include "pace.h"
#include "pads.h"
#include "typist.h"
#include "setup.h"
#include "states.h"
#include "tui.h"
#include "zr_version.h"
#include "../core/font8x8.h"
#include "../core/media.h"
#include "../core/media/snap.h"
#include "../core/media/tape.h"
#include "../core/state.h"
#include "../core/text.h"
#include "../host/host.h"
#include "../platform/audio.h"
#include "../platform/overlay.h"
#include "../platform/png.h"
#include "../platform/serial.h"
#include "../platform/video.h"

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TITLE "ZetRo"
/* A lag up to this much wall time is caught up; a longer one (a moved window) is dropped. */
#define CATCH_UP_MS 100
#define CONTROL_LINES 64
#define CONTROL_LINE 1024
/* While F4 is held the machine goes back this many times faster than it ran. */
#define REWIND_SPEED 10u

/* A host key as two of the machine's keys (ZR_KEY_NONE: no second one). */
typedef struct host_key { uint8_t key, shift; } host_key;

typedef struct shell {
    zr_machine *m;
    zr_machine **machine_out;
    zr_config *config;
    const zr_desktop_options *options;
    int hidden, det, paused, quit, resync;
    zr_video *video;
    uint32_t *pixels;
    size_t pixels_size;
    uint32_t width, height;
    zr_host_audio audio;
    int audio_open;
    zr_wav wav;
    zr_media *media[ZR_CONFIG_MEDIA];
    host_key keys[SDL_NUM_SCANCODES];
    host_key shifted[SDL_NUM_SCANCODES];    /* with the host's Shift held, where the family gives a combination for it */
    host_key down[SDL_NUM_SCANCODES];       /* what each host key that is down pressed on the machine */
    uint8_t lifted[SDL_NUM_SCANCODES];      /* it let go of the machine's key for the host's Shift */
    /* joysticks and auto-load (the section of that name below) */
    zr_joykeys joykeys;
    zr_pads pads;
    int pads_open;
    zr_osk osk;                 /* the on-screen keyboard (osk.h): F8, a pad's Select/Back */
    int aspect_w, aspect_h;     /* the frame's shape as the last picture had it */
    int osk_x, osk_y;           /* where the harness's click went down */
    unsigned joy_fed[ZR_CONFIG_STICKS];
    zr_typist typist;
    /* the cheats (cheats.h): the set, and whether the last tape put in brought a .pok file that the status line has not said yet */
    zr_cheats cheats;
    int cheats_new;
    int shot_wait;
    /* the host ends of the machine's byte ports (platform/serial.h) and the tape recorder's recording under way */
    zr_host_ports ports;
    int recording;              /* the first tape slot was recording at the last look (tape_watch) */
    unsigned rec_blocks;        /* the blocks the tape had when it started */
    int was_free;               /* the last pace step ran unthrottled (media.*.fast, F9) */
    int full_speed;             /* F9: the machine runs as fast as the host allows */
    /* the rewind ring (states.h): kept while ring_on (a visible window; headless, the harness's `rewind on`) */
    zr_rewind ring;
    int ring_on, rewinding;
    size_t rewind_at;           /* the ring's state shown while F4 is held (ring.count: none yet) */
    uint64_t rewind_debt;       /* guest time owed to going back, in ticks */
    uint64_t rewind_from;       /* the guest time F4 was pressed at */
    char error[1024];
    /* the setup screen, the status line and the font they are drawn with */
    zr_setup *setup;
    zr_osd osd;
    uint64_t shown;             /* pictures shown: the FLASH attribute's clock and, headless, the status line's */
    uint8_t font[2048];
    unsigned hostkey_next;
    /* the harness and its control channel */
    zr_harness *harness;
    zr_host_stream *tcp;
    char tcp_line[CONTROL_LINE];
    size_t tcp_used;
    SDL_Thread *reader;
    SDL_mutex *lock;
    char lines[CONTROL_LINES][CONTROL_LINE];
    unsigned line_read, line_write;
    int stdin_closed;
} shell;

/* --- messages ------------------------------------------------------------------------------ */

static uint32_t now_ms(const shell *s) { return s->det ? (uint32_t)(s->shown * 1000u / 60u) : SDL_GetTicks(); }

/* A message on the status line (a few seconds over the bottom of the picture): a row of the string table (text.h), in the
 * language in use, with its values. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static void say_id(shell *s, int id, ...) {
    char text[sizeof s->osd.text];
    va_list args;
    va_start(args, id);
    vsnprintf(text, sizeof text, zr_t(id), args);
    va_end(args);
    zr_osd_show(&s->osd, now_ms(s), "%s", text);
}

/* The name of a file inside its path, as cells for a message. */
static void file_name(const char *path, char *out, size_t capacity) {
    const char *name = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    zr_tui_clean(name, out, capacity);
}

/* --- the byte ports ------------------------------------------------------------------------ */

/* The host ends brought to what the configuration says for the machine that runs (the ends that are as they were stay open); an end
 * that cannot be opened leaves its port unconnected, and the status line and the console say why. */
static void ports_apply(shell *s) {
    if (!zr_host_ports_configure(&s->ports, s->config, s->m)) return;
    const zr_family *f = zr_machine_family(s->m);
    int said = 0;
    for (unsigned i = 0; i < f->port_count && i < ZR_PORTS_MAX; ++i) {
        const char *why = zr_host_ports_error(&s->ports, i);
        if (!why) continue;
        fprintf(stderr, "Warning: port %s: %s\n", f->ports[i].key, why);
        if (said) continue;
        char shown[ZR_PORTS_ERROR];
        zr_tui_clean(why, shown, sizeof shown);
        say_id(s, ZR_T_SAY_PORT_FAILED, zr_t(f->ports[i].label), shown);
        said = 1;
    }
}

/* The machine's state changed under it (a state or a rewind loaded, a reset): it has not heard the modem inputs of the ends. */
static void ports_resend(shell *s) { zr_host_ports_resend(&s->ports); }

/* --- keys ------------------------------------------------------------------------------- */

/* The family's key table as host scancodes: each key's own host key, then the combinations. */
static void map_keys(shell *s) {
    const zr_family *f = zr_machine_family(s->m);
    for (int i = 0; i < SDL_NUM_SCANCODES; ++i) {
        s->keys[i].key = s->keys[i].shift = s->shifted[i].key = s->shifted[i].shift = s->down[i].key = s->down[i].shift = ZR_KEY_NONE;
        s->lifted[i] = 0;
    }
    for (unsigned i = 0; i < f->key_count; ++i) {
        const SDL_Scancode sc = f->keys[i].host ? SDL_GetScancodeFromName(f->keys[i].host) : SDL_SCANCODE_UNKNOWN;
        if (sc != SDL_SCANCODE_UNKNOWN) s->keys[sc].key = (uint8_t)i;
    }
    for (unsigned i = 0; i < f->combo_count; ++i) {
        const int with_shift = !strncmp(f->combos[i].host, "Shift+", 6);
        const SDL_Scancode sc = SDL_GetScancodeFromName(f->combos[i].host + (with_shift ? 6 : 0));
        if (sc == SDL_SCANCODE_UNKNOWN) continue;
        host_key *k = with_shift ? &s->shifted[sc] : &s->keys[sc];
        k->key = f->combos[i].key;
        k->shift = f->combos[i].shift;
    }
    /* The right-hand twin of a modifier is the same key. */
    static const SDL_Scancode TWINS[][2] = {{SDL_SCANCODE_LSHIFT, SDL_SCANCODE_RSHIFT}, {SDL_SCANCODE_LCTRL, SDL_SCANCODE_RCTRL},
                                            {SDL_SCANCODE_LALT, SDL_SCANCODE_RALT}, {SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER}};
    for (size_t i = 0; i < sizeof TWINS / sizeof *TWINS; ++i)
        if (s->keys[TWINS[i][1]].key == ZR_KEY_NONE) s->keys[TWINS[i][1]] = s->keys[TWINS[i][0]];
}

/* A key of the machine pressed or let go for a host key: the on-screen keyboard shows it held. */
static void host_press(shell *s, unsigned key, int down) {
    zr_machine_key(s->m, key, down);
    zr_osk_host(&s->osk, key, down);
}

/* The on-screen keyboard's presses reach the machine here. */
static void osk_key(void *ctx, unsigned key, int down) { zr_machine_key(((shell *)ctx)->m, key, down); }

static void guest_key(shell *s, int sc, int down) {
    if (sc < 0 || sc >= SDL_NUM_SCANCODES) return;
    const uint8_t host_shift = s->keys[SDL_SCANCODE_LSHIFT].key;        /* the machine's key for the host's Shift */
    if (down) {
        const int with_shift = s->shifted[sc].key != ZR_KEY_NONE && (SDL_GetModState() & KMOD_SHIFT);
        const host_key k = with_shift ? s->shifted[sc] : s->keys[sc];
        if (k.key == ZR_KEY_NONE) return;
        /* A combination for Shift and a key stands for both: the machine does not see the Shift. */
        if (with_shift && host_shift != ZR_KEY_NONE) zr_machine_key(s->m, host_shift, 0);
        s->down[sc] = k;
        s->lifted[sc] = (uint8_t)with_shift;
        /* A combination holds its shift round the key. */
        if (k.shift != ZR_KEY_NONE) host_press(s, k.shift, 1);
        host_press(s, k.key, 1);
        return;
    }
    const host_key k = s->down[sc].key != ZR_KEY_NONE ? s->down[sc] : s->keys[sc];
    if (k.key == ZR_KEY_NONE) return;
    host_press(s, k.key, 0);
    if (k.shift != ZR_KEY_NONE) host_press(s, k.shift, 0);
    if (s->lifted[sc] && host_shift != ZR_KEY_NONE && (SDL_GetModState() & KMOD_SHIFT)) zr_machine_key(s->m, host_shift, 1);
    s->down[sc].key = s->down[sc].shift = ZR_KEY_NONE;
    s->lifted[sc] = 0;
}
static void release_all(shell *s) {
    const zr_family *f = zr_machine_family(s->m);
    for (unsigned i = 0; i < f->key_count; ++i) zr_machine_key(s->m, i, 0);
    zr_osk_release(&s->osk);                                    /* what the on-screen keyboard held, latched shift keys too */
    zr_joykeys_set(&s->joykeys, zr_joykeys_on(&s->joykeys));    /* the keyboard joystick's keys too */
}
/* A reset from the window: the machine lets go of its keys and takes again those the host keyboard still holds (a machine that
 * looks at the keyboard at a reset: BREAK held starts CP/M), but not Ctrl or Alt, which the reset's own keys held. A hidden run has no
 * keyboard of its own: the keys its events pressed are the held ones. */
static void release_all_but_held(shell *s) {
    release_all(s);
    const Uint8 *physical = s->hidden ? NULL : SDL_GetKeyboardState(NULL);
    for (int sc = 0; sc < SDL_NUM_SCANCODES; ++sc) {
        const host_key k = s->down[sc];
        if (k.key == ZR_KEY_NONE || sc == SDL_SCANCODE_LCTRL || sc == SDL_SCANCODE_RCTRL || sc == SDL_SCANCODE_LALT ||
            sc == SDL_SCANCODE_RALT || (physical && !physical[sc])) continue;
        if (k.shift != ZR_KEY_NONE) zr_machine_key(s->m, k.shift, 1);
        zr_machine_key(s->m, k.key, 1);
    }
}

/* --- the picture ------------------------------------------------------------------------ */

static void apply_look(shell *s) {
    static const int filters[] = {ZR_FILTER_NEAREST, ZR_FILTER_LINEAR, ZR_FILTER_SHARP};
    zr_video_set_filter(s->video, filters[s->config->filter]);
    /* integer: the largest whole multiple that fits the window. */
    zr_video_set_integer_scale(s->video, s->config->scale == ZR_CONFIG_SCALE_INTEGER ? 1 << 16 : 0);
    zr_video_set_aspect(s->video, s->config->aspect == ZR_CONFIG_ASPECT_SQUARE ? ZR_ASPECT_SQUARE : ZR_ASPECT_ORIGINAL);
    /* A hidden window never reaches a display: nothing to wait for. */
    zr_video_set_vsync(s->video, s->config->vsync && !s->hidden);
    if (!zr_video_set_look(s->video, s->config->look == ZR_LOOK_CRT ? ZR_VIDEO_LOOK_CRT : ZR_VIDEO_LOOK_PLAIN, ZR_VIDEO_PHOSPHOR_COLOR)) {
        fputs("video.look needs OpenGL, which is unavailable: the picture is shown plain\n", stderr);
        zr_video_set_look(s->video, ZR_VIDEO_LOOK_PLAIN, ZR_VIDEO_PHOSPHOR_COLOR);
    }
}

/* The status line over the bottom of the picture in s->pixels (`width` x `height`): "paused" while the machine is, else the
 * message of the last few seconds. */
static void status_line(shell *s, uint32_t width, uint32_t height) {
    if (s->rewinding) {
        /* how far back the picture is, while F4 is held */
        const uint64_t at = s->rewind_at < s->ring.count ? zr_rewind_time(&s->ring, s->rewind_at) : s->rewind_from;
        const uint64_t tenths = (s->rewind_from > at ? s->rewind_from - at : 0) * 10u / zr_machine_clock(s->m);
        char text[96], said[80];
        zr_text_format(said, sizeof said, ZR_T_SAY_REWIND, (unsigned)(tenths / 10u), (unsigned)(tenths % 10u));
        snprintf(text, sizeof text, "%c%c %s", ZR_GLYPH_CHAR(ZR_GLYPH_POINT_LEFT), ZR_GLYPH_CHAR(ZR_GLYPH_POINT_LEFT), said);
        zr_tui_status_line(s->font, s->pixels, (int)width, (int)height, text, ZR_ATTR(ZR_YELLOW, ZR_BLACK, 1));
    } else if (s->paused) zr_tui_status_line(s->font, s->pixels, (int)width, (int)height, ZR_TXT(SAY_PAUSED), ZR_ATTR(ZR_YELLOW, ZR_BLACK, 1));
    else if (s->osd.text[0] && (int32_t)(s->osd.until_ms - now_ms(s)) > 0)
        zr_tui_status_line(s->font, s->pixels, (int)width, (int)height, s->osd.text, ZR_ATTR(ZR_WHITE, ZR_BLACK, 1));
}

/* The picture: the setup screen's while it is open, else the machine's with its status line, as colours into s->pixels.
 * `cells` 0 leaves the status line out (the harness's shots). */
static int convert(shell *s, int cells) {
    zr_frame f;
    memset(&f, 0, sizeof f);
    if (s->setup) zr_setup_frame(s->setup, !((s->shown >> 4) & 1u), &f);
    else if (!zr_machine_frame(s->m, &f) || !f.pixels) {
        f.width = s->width ? s->width : 320;
        f.height = s->height ? s->height : 240;
        f.aspect_w = 4;
        f.aspect_h = 3;
        f.pixels = NULL;
    }
    zr_frame_crop(&f, s->config->border);
    const size_t n = (size_t)f.width * f.height;
    if (n > s->pixels_size) {
        uint32_t *p = realloc(s->pixels, n * sizeof *p);
        if (!p) { snprintf(s->error, sizeof s->error, "Out of memory"); return 0; }
        s->pixels = p;
        s->pixels_size = n;
    }
    if (f.pixels) zr_frame_argb(&f, s->pixels);
    else for (size_t i = 0; i < n; ++i) s->pixels[i] = 0xFF000000u;
    s->aspect_w = (int)f.aspect_w;
    s->aspect_h = (int)f.aspect_h;
    if (!s->setup) zr_osk_draw(&s->osk, s->font, s->pixels, (int)f.width, (int)f.height, s->aspect_w, s->aspect_h);   /* over the picture, under the status line */
    if (cells && !s->setup) status_line(s, f.width, f.height);
    s->width = f.width;
    s->height = f.height;
    zr_video_set_geometry(s->video, (int)f.width, (int)f.height, (int)f.aspect_w, (int)f.aspect_h);
    return 1;
}

/* The Print Screen key: the next picture shown goes to the first free zetro-NNNN.png. */
static void shot_key(shell *s) {
    char name[64];
    for (unsigned n = 1; n < 10000; ++n) {
        snprintf(name, sizeof name, "zetro-%04u.png", n);
        if (!zr_host_file_exists(name)) break;
    }
    zr_video_request_shot(s->video, name, s->config->look == ZR_LOOK_PLAIN);
    s->shot_wait = 1;
}

static void shot_done(shell *s) {
    char saved[1024];
    if (!s->shot_wait || !zr_video_take_shot_result(s->video, saved, sizeof saved)) return;
    s->shot_wait = 0;
    char name[ZR_MEDIA_PATH];
    zr_tui_clean(saved, name, sizeof name);
    if (saved[0]) say_id(s, ZR_T_SAY_SCREENSHOT, name);
    else say_id(s, ZR_T_SAY_SCREENSHOT_FAILED);
}

/* --- sound ------------------------------------------------------------------------------ */

/* The sound device opened or closed to match the configuration (a muted or
 * headless run holds none); the volume applies at every pump. */
static void audio_apply(shell *s) {
    const int want = !s->hidden && !s->options->wav && s->config->sound && zr_machine_audio_rate(s->m);
    if (want && !s->audio_open) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) || !zr_host_audio_open(&s->audio)) {
            fprintf(stderr, "Sound output unavailable: %s\n", SDL_GetError());
            memset(&s->audio, 0, sizeof s->audio);
        } else s->audio_open = 1;
    } else if (!want && s->audio_open) {
        zr_host_audio_close(&s->audio);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        s->audio_open = 0;
    }
}

/* --- joysticks and auto-load -------------------------------------------------------------- */

/* A message for the user, on the status line. */
static void input_say(shell *s, int id) { say_id(s, id); }

/* The keyboard joystick's keys from the configuration's key names (a name SDL does not know: the default key). */
static void joykeys_resolve(shell *s) {
    static const int DEFAULT[ZR_CONFIG_STICK_KEYS] = {SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
                                                      SDL_SCANCODE_LALT, SDL_SCANCODE_LCTRL};
    int keys[ZR_CONFIG_STICK_KEYS];
    for (int i = 0; i < ZR_CONFIG_STICK_KEYS; ++i) {
        const SDL_Scancode sc = SDL_GetScancodeFromName(s->config->stick_keys[i]);
        keys[i] = sc != SDL_SCANCODE_UNKNOWN ? (int)sc : DEFAULT[i];
    }
    zr_joykeys_set_keys(&s->joykeys, keys);
}

static void inputs_init(shell *s) {
    static const int NO_KEYS[ZR_CONFIG_STICK_KEYS] = {-1, -1, -1, -1, -1, -1};
    zr_typist_init(&s->typist);
    zr_joykeys_init(&s->joykeys, NO_KEYS, s->config->keyboard_joystick);
    joykeys_resolve(s);
}

/* The joystick sockets the machine has that the configuration knows (at most two). */
static unsigned joy_sockets(const shell *s) {
    const unsigned n = zr_machine_family(s->m)->joysticks;
    return n < ZR_CONFIG_STICKS ? n : ZR_CONFIG_STICKS;
}

/* F11: the keyboard joystick's keys go to the joystick or back to the machine. */
static void joystick_switch(shell *s) {
    int assigned = 0;
    for (unsigned i = 0; i < joy_sockets(s); ++i) assigned |= s->config->sticks[i].device == ZR_STICK_KEYBOARD;
    if (!assigned) { input_say(s, ZR_T_SAY_STICK_NONE); return; }
    if (zr_joykeys_switch(&s->joykeys)) {
        /* A key held at the switch is the joystick's from now on: the machine lets go of it. */
        for (int i = 0; i < ZR_CONFIG_STICK_KEYS; ++i) guest_key(s, s->joykeys.key[i], 0);
        input_say(s, ZR_T_SAY_STICK_ON);
    } else input_say(s, ZR_T_SAY_STICK_OFF);
}

/* What each socket of the machine is given: the keyboard joystick's keys, or the controller the configuration assigns. A
 * socket whose device holds nothing is left alone (after one release), so a script's `joy` stands until a key or a
 * controller takes over. */
static void feed_sticks(shell *s) {
    for (unsigned i = 0; i < joy_sockets(s); ++i) {
        const zr_config_stick *st = &s->config->sticks[i];
        unsigned held = 0;
        if (st->device == ZR_STICK_KEYBOARD) held = zr_joykeys_held(&s->joykeys);
        else if (st->device == ZR_STICK_CONTROLLER && s->pads_open && st->pad >= 1 && !zr_osk_is_open(&s->osk)) held = zr_pads_joystick(&s->pads, st->pad - 1);     /* the on-screen keyboard has the d-pad while it is open */
        if (!held && !s->joy_fed[i]) continue;
        s->joy_fed[i] = held;
        zr_machine_joystick(s->m, i, held);
    }
}

/* Once a frame, before the machine runs it: the typist's key and the joysticks. */
static void input_frame(shell *s) {
    if (s->paused) return;
    zr_typist_step(&s->typist, s->m);
    feed_sticks(s);
    zr_cheats_frame(&s->cheats, s->m);                          /* the trainers that are on, written again before the frame runs */
}

/* A file the user put into slot `slot` (dropped on the window; the setup screen calls this too): a tape is
 * loaded at once when the slot's autoload says so (typist.h). */
static void inserted_by_user(shell *s, unsigned slot) {
    if (!zr_typist_autoload(&s->typist, s->m, s->config, slot)) return;
    release_all(s);
    s->resync = 1;
    input_say(s, ZR_T_SAY_AUTOLOAD);
}

/* --- cheats and snapshots ------------------------------------------------------------------ */

/* A tape went into the machine: the cheats of the last one go, and the .pok file that belongs to this one comes in (cheats.h). */
static void cheats_for_tape(shell *s, const char *tape) {
    char file[ZR_MEDIA_PATH], error[256];
    s->cheats_new = 0;
    zr_cheats_clear(&s->cheats, s->m);
    if (!zr_cheats_find(tape, file, sizeof file)) return;
    if (zr_cheats_load(&s->cheats, s->m, file, error, sizeof error)) s->cheats_new = 1;
    else fprintf(stderr, "%s: %s\n", file, error);
}

/* The status line says a .pok file came with the tape: after the messages of the insert, which would cover it. */
static void cheats_announce(shell *s) {
    if (!s->cheats_new) return;
    s->cheats_new = 0;
    char name[ZR_MEDIA_PATH];
    file_name(s->cheats.file, name, sizeof name);
    say_id(s, ZR_T_SAY_CHEATS_LOADED, name, (unsigned)zr_cheats_count(&s->cheats));
}

/* A .pok file (dropped, chosen on the Cheats page or named by the harness) in place of the set. */
static int cheats_load(shell *s, const char *path, char *error, size_t capacity) {
    if (!zr_cheats_load(&s->cheats, s->m, path, error, capacity)) return 0;
    s->cheats_new = 0;
    char name[ZR_MEDIA_PATH];
    file_name(path, name, sizeof name);
    say_id(s, ZR_T_SAY_CHEATS_LOADED, name, (unsigned)zr_cheats_count(&s->cheats));
    return 1;
}

/* A snapshot file (media/snap.h) put into the machine; the keys, the typing and the rewind ring are of the time before it. */
static int snapshot_load(shell *s, const char *path, char *error, size_t capacity) {
    uint8_t *data = NULL;
    size_t size = 0;
    int writable = 0;
    if (!zr_host_read_file(path, ZR_MEDIA_LIMIT, &data, &size, &writable, error, capacity)) return 0;
    zr_snap *snap = zr_snap_new();
    if (!snap) {
        free(data);
        snprintf(error, capacity, "Out of memory");
        return 0;
    }
    const int ok = zr_snap_read(data, size, path, snap, error, capacity) && zr_machine_snapshot_load(s->m, snap, error, capacity);
    zr_snap_free(snap);
    free(data);
    if (ok) {
        zr_typist_cancel(&s->typist, s->m);
        release_all(s);
        s->resync = 1;
        zr_rewind_clear(&s->ring);
    }
    return ok;
}

/* The machine as a snapshot file of the path's ending (the folders are made). */
static int snapshot_save(shell *s, const char *path, char *error, size_t capacity) {
    char ending[8];
    zr_media_extension(path, ending, sizeof ending);
    const unsigned format = zr_snap_format_of_name(path);
    if (!format) {
        snprintf(error, capacity, "A snapshot is saved as .sna, .z80 or .szx, not .%s", ending);
        return 0;
    }
    zr_snap *snap = zr_snap_new();
    if (!snap) {
        snprintf(error, capacity, "Out of memory");
        return 0;
    }
    uint8_t *bytes = NULL;
    size_t size = 0;
    int ok = zr_machine_snapshot_save(s->m, snap);
    if (!ok) snprintf(error, capacity, "This machine takes no snapshots");
    else ok = zr_snap_write(snap, format, &bytes, &size, error, capacity) && zr_states_save_file(path, bytes, size, error, capacity);
    free(bytes);
    zr_snap_free(snap);
    return ok;
}

/* The same for the setup screen and the harness: the status line says what happened. */
static int snapshot_load_said(void *ctx, const char *path, char *error, size_t capacity) {
    shell *s = ctx;
    if (!snapshot_load(s, path, error, capacity)) return 0;
    char name[ZR_MEDIA_PATH];
    file_name(path, name, sizeof name);
    say_id(s, ZR_T_SAY_SNAP_LOADED, name);
    return 1;
}

static int snapshot_save_said(void *ctx, const char *path, char *error, size_t capacity) {
    shell *s = ctx;
    if (!snapshot_save(s, path, error, capacity)) return 0;
    char name[ZR_MEDIA_PATH];
    file_name(path, name, sizeof name);
    say_id(s, ZR_T_SAY_SNAP_SAVED, name);
    return 1;
}

/* --- media ------------------------------------------------------------------------------ */

static int media_eject(shell *s, unsigned slot) {
    if (slot >= ZR_CONFIG_MEDIA || !s->media[slot]) return 0;
    char error[512];
    if (!zr_states_replace_media(s->m, slot, &s->media[slot], NULL, 0, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 0;
    }
    return 1;
}

static int media_insert(shell *s, unsigned slot, const char *path, int write_protect, char *error, size_t capacity) {
    const zr_family *f = zr_machine_family(s->m);
    if (slot >= ZR_CONFIG_MEDIA) { snprintf(error, capacity, "No such slot"); return 0; }
    if (!zr_states_replace_media(s->m, slot, &s->media[slot], path, write_protect, error, capacity)) return 0;
    if (f->media[slot].kind == ZR_MEDIA_TAPE) cheats_for_tape(s, path);
    return 1;
}

/* The settings of the tape slots as the configuration has them, into the machine: the sound of the tape, the fast loading. */
static void tape_settings(shell *s) {
    const zr_family *f = zr_machine_family(s->m);
    for (unsigned i = 0; i < f->media_count && i < ZR_CONFIG_MEDIA; ++i)
        if (f->media[i].kind == ZR_MEDIA_TAPE) zr_machine_tape_options(s->m, i, zr_config_tape_bits(zr_config_media_find(s->config, f->media[i].key)));
}

/* The media the configuration names, each in its slot; one that cannot go in is a warning. */
static void media_configured(shell *s) {
    const zr_family *f = zr_machine_family(s->m);
    char error[512];
    for (unsigned i = 0; i < s->config->media_count; ++i) {
        const zr_config_media *c = &s->config->media[i];
        const int slot = zr_family_media(f, c->key);
        if (slot < 0 || !c->image[0] || !zr_machine_has_media(s->m, (unsigned)slot)) continue;   /* a slot the machine as built lacks keeps its file's name */
        if (!media_insert(s, (unsigned)slot, c->image, c->write_protect, error, sizeof error)) fprintf(stderr, "Warning: %s: %s\n", c->image, error);
    }
    tape_settings(s);
}

/* A file dropped on the window: into the first slot of the model that takes its kind. */
static void drop_file(shell *s, const char *path) {
    const zr_family *f = zr_machine_family(s->m);
    char error[512];
    if (zr_snap_format_of_name(path) || zr_media_extension_in(path, "pok")) {
        char shown[sizeof error];
        const int snapshot = zr_snap_format_of_name(path) != ZR_SNAP_NONE;
        if (snapshot ? snapshot_load_said(s, path, error, sizeof error) : cheats_load(s, path, error, sizeof error)) return;
        fprintf(stderr, "%s: %s\n", path, error);
        zr_tui_clean(error, shown, sizeof shown);
        say_id(s, snapshot ? ZR_T_SAY_SNAP_FAILED : ZR_T_SAY_CHEATS_FAILED, shown);
        return;
    }
    for (unsigned i = 0; i < f->media_count; ++i)
        if (zr_machine_has_media(s->m, i) && zr_media_extension_in(path, f->media[i].extensions)) {
            char name[ZR_MEDIA_PATH];
            file_name(path, name, sizeof name);
            if (!media_insert(s, i, path, 0, error, sizeof error)) {
                char shown[sizeof error];
                fprintf(stderr, "%s: %s\n", path, error);
                zr_tui_clean(error, shown, sizeof shown);
                say_id(s, ZR_T_SAY_MEDIA_FAILED, shown);
            } else {
                say_id(s, ZR_T_SAY_INSERTED, zr_t(f->media[i].label), name);
                inserted_by_user(s, i);
                cheats_announce(s);
            }
            return;
        }
    char name[ZR_MEDIA_PATH];
    file_name(path, name, sizeof name);
    fprintf(stderr, "%s cannot go in a drive of this machine\n", path);
    say_id(s, ZR_T_SAY_MEDIA_REFUSED, name);
}

/* --- states ----------------------------------------------------------------------------- */

/* A state file (states.h): the machine's chunks, then for each media slot in use its file's path, the SHA-256 of its content
 * and its write protection, a thumbnail of the picture, the date and time and ZetRo's version. */
static int state_save(shell *s, const char *path, char *error, size_t capacity) {
    static zr_states_info info;
    memset(&info, 0, sizeof info);
    snprintf(info.version, sizeof info.version, "%s", ZR_VERSION);
    zr_host_time now;
    if (zr_host_local_time(&now)) {
        info.has_time = 1;
        info.time.year = now.year;
        info.time.month = now.month;
        info.time.day = now.day;
        info.time.hour = now.hour;
        info.time.minute = now.minute;
        info.time.second = now.second;
    }
    zr_frame f;
    memset(&f, 0, sizeof f);
    if (zr_machine_frame(s->m, &f)) info.has_thumb = zr_states_thumb_make(&f, &info.thumb);
    const zr_family *fam = zr_machine_family(s->m);
    for (unsigned i = 0; i < ZR_CONFIG_MEDIA; ++i) {
        const zr_media *m = s->media[i];
        if (!m) continue;
        const zr_config_media *c = i < fam->media_count ? zr_config_media_find(s->config, fam->media[i].key) : NULL;
        info.media[i].present = 1;
        snprintf(info.media[i].path, sizeof info.media[i].path, "%s", m->path);
        /* A card is named by its path only: its content is not read to be hashed (it changes with every write the guest makes). */
        info.media[i].hashed = !m->in_place;
        if (!m->in_place) zr_sha256(m->data, m->size, info.media[i].sha256);
        info.media[i].write_protect = c ? c->write_protect : 0;
    }
    zr_state_writer w;
    zr_sw_init(&w);
    int ok = zr_states_write(s->m, &info, &w);
    if (!ok) snprintf(error, capacity, "Out of memory");
    else ok = zr_states_save_file(path, w.data, w.size, error, capacity);
    zr_sw_free(&w);
    return ok;
}

/* Puts a state file back: its media first, each in its slot, so that the machine's state finds them there (a tape recorder's
 * position is in its tape); what the guest wrote since stays in the files. `warning` (cells) says when a file is missing or its
 * content is no longer what the state was saved with ("" when all is as it was). */
static int state_load(shell *s, const char *path, char *warning, size_t warning_capacity, char *error, size_t capacity) {
    if (!zr_states_load_session(s->m, s->config, s->options->roms, s->media, path, warning, warning_capacity, error, capacity)) return 0;
    tape_settings(s);
    ports_apply(s);
    ports_resend(s);
    release_all(s);
    s->resync = 1;
    zr_rewind_clear(&s->ring);                                  /* the ring's states are of another time line now */
    return 1;
}

/* The quick-save slots (F2 and F3, the States page): files beside the configuration, for the running model. */
static void slot_file(const shell *s, unsigned slot, char *out, size_t capacity) {
    zr_states_slot_path(s->config->source, zr_machine_family(s->m)->models[zr_machine_model(s->m)].id, slot, out, capacity);
}

static int slot_save(shell *s, unsigned slot, char *error, size_t capacity) {
    char path[ZR_CONFIG_PATH + 64], shown[256];
    slot_file(s, slot, path, sizeof path);
    if (state_save(s, path, error, capacity)) { say_id(s, ZR_T_SAY_SLOT_SAVED, slot); return 1; }
    zr_tui_clean(error, shown, sizeof shown);
    say_id(s, ZR_T_SAY_SLOT_SAVE_FAILED, slot, shown);
    return 0;
}

static int slot_load(shell *s, unsigned slot, char *error, size_t capacity) {
    char path[ZR_CONFIG_PATH + 64], shown[256], warning[160];
    slot_file(s, slot, path, sizeof path);
    if (!zr_host_file_exists(path)) {
        snprintf(error, capacity, "The slot is empty");
        say_id(s, ZR_T_SAY_SLOT_EMPTY, slot);
        return 0;
    }
    if (!state_load(s, path, warning, sizeof warning, error, capacity)) {
        zr_tui_clean(error, shown, sizeof shown);
        say_id(s, ZR_T_SAY_SLOT_LOAD_FAILED, slot, shown);
        return 0;
    }
    if (warning[0]) zr_osd_show(&s->osd, now_ms(s), "%s", warning);
    else say_id(s, ZR_T_SAY_SLOT_LOADED, slot);
    return 1;
}

/* --- rewind and speed ----------------------------------------------------------------------- */

/* Guest time between two states of the ring. */
static uint64_t ring_interval(const shell *s) { return (uint64_t)zr_machine_clock(s->m) * ZR_REWIND_INTERVAL_MS / 1000u; }

/* After the machine ran: a state into the ring when one is due. */
static void ring_take(shell *s) {
    if (!s->ring_on || s->rewinding || zr_machine_time(s->m) < s->ring.next) return;
    if (zr_rewind_take(&s->ring, s->m, ring_interval(s)) < 0) {
        fputs("Rewind: out of memory, the ring is emptied\n", stderr);
        zr_rewind_clear(&s->ring);
    }
}

/* F4 pressed: the machine stands still and goes back a state at a time while it is held. */
static void rewind_start(shell *s) {
    if (s->setup || s->rewinding) return;
    if (!s->ring_on || !s->ring.count) { say_id(s, ZR_T_SAY_REWIND_EMPTY); return; }
    release_all(s);
    s->rewinding = 1;
    s->rewind_at = s->ring.count;
    s->rewind_debt = ring_interval(s);                          /* the first picture shows the newest state */
    s->rewind_from = zr_machine_time(s->m);
    s->osd.text[0] = 0;
}

/* F4 let go: the machine goes on from the state it is at; the states after it are dropped. */
static void rewind_stop(shell *s) {
    if (!s->rewinding) return;
    s->rewinding = 0;
    if (s->rewind_at < s->ring.count) zr_rewind_cut(&s->ring, s->rewind_at, ring_interval(s));
    release_all(s);
    s->resync = 1;
}

/* Each picture while F4 is held: back through the ring at REWIND_SPEED times the speed the machine ran, to the oldest state. */
static void rewind_frame(shell *s) {
    const uint64_t interval = ring_interval(s);
    size_t at = s->rewind_at;
    s->rewind_debt += REWIND_SPEED * zr_machine_frame_period(s->m);
    while (s->rewind_debt >= interval && at > 0) { --at; s->rewind_debt -= interval; }
    if (s->rewind_debt > interval) s->rewind_debt = interval;
    if (at != s->rewind_at && at < s->ring.count) {
        char error[256], shown[256];
        if (!zr_rewind_restore(&s->ring, s->m, at, error, sizeof error)) {
            s->rewinding = 0;
            zr_rewind_clear(&s->ring);
            zr_tui_clean(error, shown, sizeof shown);
            say_id(s, ZR_T_SAY_STATE_FAILED, shown);
            return;
        }
        s->rewind_at = at;
        ports_resend(s);
    }
}

/* F9 and the harness's `speed`: 100 % or as fast as the host allows (the sound is dropped then). */
static void set_speed(shell *s, int full) {
    s->full_speed = full;
    s->resync = 1;
    say_id(s, full ? ZR_T_SAY_SPEED_FULL : ZR_T_SAY_SPEED_NORMAL);
}

/* --- the tape keys and the media's speed ------------------------------------------------------ */

/* The first tape slot of the running machine's model, -1 when it has none. */
static int tape_slot(const shell *s) {
    const zr_family *f = zr_machine_family(s->m);
    const unsigned model = zr_machine_model(s->m);
    for (unsigned i = 0; i < f->media_count; ++i)
        if (f->media[i].kind == ZR_MEDIA_TAPE && (f->media[i].models >> model & 1u)) return (int)i;
    return -1;
}

/* The tape's position as a message. */
static void say_block(shell *s, const struct zr_tape_deck *deck) {
    const uint32_t at = zr_tape_deck_block(deck);
    const unsigned count = zr_tape_count(deck->tape);
    if (at >= count) say_id(s, ZR_T_SAY_TAPE_END);
    else say_id(s, ZR_T_SAY_TAPE_BLOCK, (unsigned)at + 1u, count);
}

/* F5 plays or stops the tape of the first tape slot, F6 and F7 go to the previous and the next block. */
static void tape_key(shell *s, int sc) {
    const int slot = tape_slot(s);
    struct zr_tape_deck *deck = slot >= 0 ? zr_machine_tape_deck(s->m, (unsigned)slot) : NULL;
    if (!deck || !deck->tape) { say_id(s, ZR_T_SAY_NO_TAPE); return; }
    if (sc == SDL_SCANCODE_F5) {
        const int playing = zr_tape_deck_playing(deck);
        zr_machine_control(s->m, (unsigned)slot, playing ? ZR_MEDIA_STOP : ZR_MEDIA_PLAY, 0);
        say_id(s, playing ? ZR_T_SAY_TAPE_STOP : ZR_T_SAY_TAPE_PLAY);
    } else {
        zr_machine_control(s->m, (unsigned)slot, sc == SDL_SCANCODE_F6 ? ZR_MEDIA_PREVIOUS : ZR_MEDIA_NEXT, 0);
        say_block(s, deck);
    }
}

/* A recording that ended (by any key, the setup screen or the harness) is written to the tape's file at once, and the status line says
 * how many blocks the tape has now. A recording that began (the harness's `tape record`) is noticed too. */
static void tape_watch(shell *s) {
    const int slot = tape_slot(s);
    struct zr_tape_deck *deck = slot >= 0 ? zr_machine_tape_deck(s->m, (unsigned)slot) : NULL;
    const int recording = deck && deck->tape && zr_tape_deck_recording(deck);
    if (recording && !s->recording) {
        s->recording = 1;
        s->rec_blocks = zr_tape_count(deck->tape);
    } else if (!recording && s->recording) {
        s->recording = 0;
        zr_media *media = slot >= 0 ? s->media[slot] : NULL;
        if (!deck || !deck->tape || !media) return;
        char name[ZR_MEDIA_PATH];
        file_name(media->path, name, sizeof name);
        if (zr_tape_count(deck->tape) == s->rec_blocks) say_id(s, ZR_T_SAY_TAPE_NOTHING);
        else if (zr_media_save(media)) say_id(s, ZR_T_SAY_TAPE_RECORDED, name, (unsigned)zr_tape_count(deck->tape));
        else {
            char shown[sizeof media->error];
            zr_tui_clean(media->error, shown, sizeof shown);
            fprintf(stderr, "Cannot save %s: %s\n", media->path, media->error);
            say_id(s, ZR_T_SAY_TAPE_SAVE_FAILED, shown);
        }
    }
}

/* An empty tape file for a recording: the first free media/tape-NNNN.tzx beside the configuration. */
static int new_tape_file(const shell *s, char *path, size_t capacity, char *error, size_t error_capacity) {
    char folder[ZR_CONFIG_PATH + 16];
    size_t base = 0;
    for (size_t i = 0; s->config->source[i]; ++i) if (s->config->source[i] == '/' || s->config->source[i] == '\\') base = i + 1;
    snprintf(folder, sizeof folder, "%.*smedia", (int)base, s->config->source);
    if (!zr_host_make_folders(folder)) { snprintf(error, error_capacity, "Cannot make the folder %s", folder); return 0; }
    for (unsigned n = 1; n < 10000; ++n) {
        snprintf(path, capacity, "%s%ctape-%04u.tzx", folder, zr_host_separator(), n);
        if (!zr_host_file_exists(path)) return zr_host_write_file(path, (const uint8_t *)"", 0, error, error_capacity);
    }
    snprintf(error, error_capacity, "Too many tape files in %s", folder);
    return 0;
}

/* The tape recorder's record key for a tape slot: starts recording on the tape in it, or ends the recording under way (tape_watch then
 * writes the file). A slot with no tape gets a new tape file, inserted, so that SAVE has a tape to write on. 0 with the reason. */
static int tape_record(shell *s, unsigned slot, char *error, size_t capacity) {
    struct zr_tape_deck *deck = zr_machine_tape_deck(s->m, slot);
    char fresh[ZR_MEDIA_PATH] = "";
    if (!deck || !deck->tape) {
        char path[ZR_MEDIA_PATH];
        if (!new_tape_file(s, path, sizeof path, error, capacity)) return 0;
        if (!media_insert(s, slot, path, 0, error, capacity)) { zr_host_delete_file(path); return 0; }
        deck = zr_machine_tape_deck(s->m, slot);
        if (!deck || !deck->tape) { snprintf(error, capacity, "The machine takes no tape here"); return 0; }
        file_name(path, fresh, sizeof fresh);
    }
    const int recording = zr_tape_deck_recording(deck);
    if (!recording && !(s->media[slot] && s->media[slot]->writable)) { snprintf(error, capacity, "The tape is write protected"); return 0; }
    if (!zr_machine_control(s->m, slot, ZR_MEDIA_RECORD, 0)) { snprintf(error, capacity, "The recorder cannot start"); return 0; }
    if (recording) tape_watch(s);                                   /* ended: written back, and said */
    else {
        tape_watch(s);
        if (fresh[0]) say_id(s, ZR_T_SAY_TAPE_REC_NEW, fresh);
        else say_id(s, ZR_T_SAY_TAPE_REC);
    }
    return 1;
}

/* Shift+F5: record on the first tape slot, or stop. */
static void tape_record_key(shell *s) {
    const int slot = tape_slot(s);
    if (slot < 0) { say_id(s, ZR_T_SAY_NO_TAPE); return; }
    char error[256], shown[256];
    if (tape_record(s, (unsigned)slot, error, sizeof error)) return;
    fprintf(stderr, "Cannot record: %s\n", error);
    zr_tui_clean(error, shown, sizeof shown);
    say_id(s, ZR_T_SAY_TAPE_REC_FAILED, shown);
}

/* 1 while a slot whose `fast` setting is on is at work (a tape runs, a drive's motor turns): the shell does not wait for the
 * clock then (a tape's `full_speed` can take that part of its `fast` out). A slot the configuration does not mention is fast as
 * a new one is. */
static int fast_active(const shell *s) {
    const unsigned active = zr_machine_activity(s->m);
    if (!active) return 0;
    const zr_family *f = zr_machine_family(s->m);
    for (unsigned i = 0; i < f->media_count && i < 32; ++i)
        if (active >> i & 1u) {
            const zr_config_media *c = zr_config_media_find(s->config, f->media[i].key);
            if (!c || (c->fast && c->full_speed)) return 1;
        }
    return 0;
}

/* --- the setup screen ------------------------------------------------------------------- */

/* A file put in a slot by the setup screen; the status line says so. */
static int setup_insert(void *ctx, unsigned slot, const char *path, int write_protect, char *error, size_t capacity) {
    shell *s = ctx;
    if (!media_insert(s, slot, path, write_protect, error, capacity)) return 0;
    char name[ZR_MEDIA_PATH];
    file_name(path, name, sizeof name);
    say_id(s, ZR_T_SAY_INSERTED, zr_t(zr_machine_family(s->m)->media[slot].label), name);
    inserted_by_user(s, slot);
    cheats_announce(s);
    return 1;
}

static int setup_eject(void *ctx, unsigned slot) {
    shell *s = ctx;
    if (!media_eject(s, slot)) return 0;
    say_id(s, ZR_T_SAY_EJECTED, zr_t(zr_machine_family(s->m)->media[slot].label));
    return 1;
}

/* A slot's write protection switched while a file is in it: on, the file is read-only at once; off, it is as the file
 * itself allows. */
static int setup_protect(void *ctx, unsigned slot, int on) {
    shell *s = ctx;
    char error[512];
    if (zr_states_protect_media(s->m, slot, on, error, sizeof error)) return 1;
    fprintf(stderr, "%s\n", error);
    return 0;
}

/* The name of game controller `index` (its number less one), NULL when none is plugged in. */
static const char *setup_pad_name(void *ctx, unsigned index) {
    shell *s = ctx;
    return s->pads_open && zr_pads_present(&s->pads, index) ? zr_pads_name(&s->pads, index) : NULL;
}

static int setup_record(void *ctx, unsigned slot, char *error, size_t capacity) { return tape_record(ctx, slot, error, capacity); }
static const char *setup_port_error(void *ctx, unsigned port) { return zr_host_ports_error(&((shell *)ctx)->ports, port); }

static int setup_slot_save(void *ctx, unsigned slot, char *error, size_t capacity) { return slot_save(ctx, slot, error, capacity); }
static int setup_slot_load(void *ctx, unsigned slot, char *error, size_t capacity) { return slot_load(ctx, slot, error, capacity); }
static int setup_cheats_load(void *ctx, const char *path, char *error, size_t capacity) { return cheats_load(ctx, path, error, capacity); }

static int setup_open(shell *s, int page) {
    if (s->setup) return 1;
    zr_osk_close(&s->osk);                                      /* the on-screen keyboard closes when the setup screen opens */
    zr_setup_env env;
    memset(&env, 0, sizeof env);
    env.ctx = s;
    env.machine = s->m;
    env.version = ZR_VERSION;
    env.roms = s->options->roms;
    env.fullscreen = !s->hidden && zr_video_fullscreen(s->video);
    env.no_crt = !zr_video_crt_available(s->video);
    env.insert = setup_insert;
    env.eject = setup_eject;
    env.protect = setup_protect;
    env.pad_name = setup_pad_name;
    env.slot_save = setup_slot_save;
    env.slot_load = setup_slot_load;
    env.snapshot_load = snapshot_load_said;
    env.snapshot_save = snapshot_save_said;
    env.cheats = &s->cheats;
    env.cheats_load = setup_cheats_load;
    env.record = setup_record;
    env.port_error = setup_port_error;
    s->setup = zr_setup_create(s->config, &env);
    if (!s->setup) { snprintf(s->error, sizeof s->error, "Out of memory"); return 0; }
    if (page >= 0) zr_setup_open_page(s->setup, page);
    release_all(s);
    return 1;
}

/* F1: the tape recorder's file picker, on a machine that has one (1: the key was taken). The setup screen is opened for the
 * picker alone and left with it; a tape chosen is loaded as one put in on the Media page is. */
static int tape_pick(shell *s) {
    const zr_family *f = zr_machine_family(s->m);
    for (unsigned i = 0; i < f->media_count && i < ZR_CONFIG_MEDIA; ++i)
        if (f->media[i].kind == ZR_MEDIA_TAPE && zr_machine_has_media(s->m, i)) {
            if (setup_open(s, -1)) zr_setup_pick_media(s->setup, i);
            return 1;
        }
    return 0;
}

/* The machine again from the configuration (a new model, an option that needs a reset). The old machine stays when the new one
 * cannot be built. */
static int rebuild_machine(shell *s) {
    char error[256] = "", shown[256];
    static zr_rom_index none;
    const zr_rom_index *roms = s->options->roms ? s->options->roms : &none;
    zr_machine *m = zr_machine_create(s->config, roms, error, sizeof error);
    if (!m || !zr_states_flush_all(s->m, error, sizeof error)) {
        if (m) zr_machine_destroy(m);
        zr_tui_clean(error, shown, sizeof shown);
        say_id(s, ZR_T_SAY_REBUILD_FAILED, shown);
        return 0;
    }
    /* The files go back to their places, then into the new machine. */
    for (unsigned i = 0; i < ZR_CONFIG_MEDIA; ++i) media_eject(s, i);
    zr_typist_cancel(&s->typist, s->m);
    memset(s->joy_fed, 0, sizeof s->joy_fed);
    release_all(s);
    s->rewinding = 0;
    zr_rewind_clear(&s->ring);
    zr_cheats_clear(&s->cheats, NULL);                          /* the machine they were poked into goes */
    zr_host_ports_detach(&s->ports, s->m);                      /* the ends stay open for the next machine */
    zr_machine_destroy(s->m);
    s->m = m;
    *s->machine_out = m;
    s->recording = 0;
    map_keys(s);
    zr_osk_init(&s->osk, zr_machine_family(m), osk_key, s);     /* the new machine's keyboard, closed */
    media_configured(s);
    ports_apply(s);
    audio_apply(s);
    return 1;
}

/* The picture, sound and language options the setup screen changed, at once. */
static void setup_sync(shell *s) {
    const zr_config *e = zr_setup_config(s->setup);
    const unsigned changes = zr_setup_diff(s->config, e);
    if (changes & ZR_CHANGE_LANGUAGE) {
        s->config->language = e->language;
        zr_text_set_language(s->config->language);
    }
    if (changes & ZR_CHANGE_VIDEO) {
        s->config->look = e->look;
        s->config->aspect = e->aspect;
        s->config->scale = e->scale;
        s->config->filter = e->filter;
        s->config->border = e->border;
        s->config->vsync = e->vsync;
        s->config->fullscreen = e->fullscreen;
        apply_look(s);
        if (!s->hidden && (zr_video_fullscreen(s->video) != 0) != (e->fullscreen != 0)) zr_video_set_fullscreen(s->video, e->fullscreen != 0);
    }
    if (changes & ZR_CHANGE_SOUND) {
        s->config->sound = e->sound;
        s->config->volume = e->volume;
        audio_apply(s);
    }
    if (changes & ZR_CHANGE_MEDIA) {
        s->config->media_count = e->media_count;
        memcpy(s->config->media, e->media, sizeof e->media);
        tape_settings(s);
    }
    if (changes & ZR_CHANGE_CONTROLLERS) {
        /* The sockets' devices and the keys at once: the keys are resolved again, a socket that was fed is let go. */
        const int switched = e->keyboard_joystick != s->config->keyboard_joystick;
        memcpy(s->config->sticks, e->sticks, sizeof e->sticks);
        memcpy(s->config->stick_keys, e->stick_keys, sizeof e->stick_keys);
        s->config->keyboard_joystick = e->keyboard_joystick;
        joykeys_resolve(s);
        if (switched) zr_joykeys_set(&s->joykeys, e->keyboard_joystick);
        for (unsigned i = 0; i < joy_sockets(s); ++i)
            if (s->joy_fed[i]) {
                zr_machine_joystick(s->m, i, 0);
                s->joy_fed[i] = 0;
            }
    }
    if (changes & ZR_CHANGE_PORTS) {
        /* The ends at once: the ones that changed are opened again, a connection that stays is kept. */
        s->config->port_count = e->port_count;
        memcpy(s->config->ports, e->ports, sizeof e->ports);
        ports_apply(s);
        zr_setup_refresh(s->setup);                             /* a failure shows on the page's row */
    }
    if (changes & ZR_CHANGE_STATE_SLOT) s->config->state_slot = e->state_slot;
}

/* The setup screen is left: the changes it made are applied, the machine runs again. */
static void setup_leave(shell *s) {
    static zr_config edit, before;
    edit = *zr_setup_config(s->setup);
    before = *s->config;
    const int reset = zr_setup_reset(s->setup);
    const unsigned changes = zr_setup_diff(&before, &edit);
    zr_setup_destroy(s->setup);
    s->setup = NULL;
    *s->config = edit;
    s->resync = 1;
    if (changes & ZR_CHANGE_VIDEO) apply_look(s);
    if (changes & ZR_CHANGE_RESTART) {
        if (!rebuild_machine(s)) {
            /* The machine stays as it was, and so does the file's account of it. */
            char error[ZR_CONFIG_PATH + 64];
            snprintf(s->config->model, sizeof s->config->model, "%s", before.model);
            s->config->option_count = before.option_count;
            memcpy(s->config->options, before.options, sizeof before.options);
            s->config->media_count = before.media_count;
            memcpy(s->config->media, before.media, sizeof before.media);
            if (!zr_setup_write(s->config, error, sizeof error)) fprintf(stderr, "%s\n", error);
        }
    } else if (reset) {
        release_all_but_held(s);
        zr_machine_reset(s->m, reset == ZR_SETUP_RESET_POWER);
        ports_resend(s);
        say_id(s, reset == ZR_SETUP_RESET_POWER ? ZR_T_SAY_POWER_CYCLE : ZR_T_SAY_RESET);
    }
    if (changes & ZR_CHANGE_SOUND) audio_apply(s);
    tape_settings(s);
}

/* After the setup screen took input. */
static void setup_after(shell *s) {
    if (!s->setup) return;
    if (zr_setup_done(s->setup)) {
        if (zr_setup_exit(s->setup)) s->quit = 1;       /* saved as leaving does; the machine is not rebuilt, the program ends */
        else setup_leave(s);
    } else setup_sync(s);
}

static void setup_event(shell *s, zr_tui_event e) {
    if (!s->setup) return;
    zr_setup_event(s->setup, e);
    setup_after(s);
}

static void setup_toggle(shell *s) {
    if (!s->setup) { setup_open(s, -1); return; }
    zr_setup_request_close(s->setup);
    setup_after(s);
}

/* The on-screen keyboard opened or closed (F8, a pad's Select/Back); not while the setup screen is open. */
static void osk_toggle(shell *s) {
    if (s->setup) return;
    zr_osk_toggle(&s->osk);
    if (zr_osk_is_open(&s->osk)) say_id(s, ZR_T_SAY_OSK);
    else s->osd.text[0] = 0;
}

/* A key of a game controller for the on-screen keyboard while it is open: the d-pad, A (held while it is down) and B. */
static void osk_pad(shell *s, zr_pad_key key, int down) {
    zr_osk_pad_key k;
    switch (key) {
    case ZR_PAD_UP: k = ZR_OSK_PAD_UP; break;
    case ZR_PAD_DOWN: k = ZR_OSK_PAD_DOWN; break;
    case ZR_PAD_LEFT: k = ZR_OSK_PAD_LEFT; break;
    case ZR_PAD_RIGHT: k = ZR_OSK_PAD_RIGHT; break;
    case ZR_PAD_A: k = ZR_OSK_PAD_A; break;
    case ZR_PAD_B: k = ZR_OSK_PAD_B; break;
    default: return;
    }
    zr_osk_pad(&s->osk, k, down);
    if (!zr_osk_is_open(&s->osk)) s->osd.text[0] = 0;
}

/* The pointer over the on-screen keyboard: the window's position as the picture's. */
static void osk_mouse(shell *s, const SDL_Event *e) {
    int wx, wy, gx = -1, gy = -1;
    if (e->type == SDL_MOUSEMOTION) { wx = e->motion.x; wy = e->motion.y; }
    else if (e->button.button == SDL_BUTTON_LEFT) { wx = e->button.x; wy = e->button.y; }
    else return;
    if (!zr_video_to_screen(s->video, wx, wy, &gx, &gy)) gx = gy = -1;
    zr_osk_mouse(&s->osk, gx, gy, e->type == SDL_MOUSEMOTION ? ZR_OSK_MOUSE_MOVE : e->type == SDL_MOUSEBUTTONDOWN ? ZR_OSK_MOUSE_DOWN : ZR_OSK_MOUSE_UP);
}

/* A key of a game controller: Start opens and closes the setup screen at any time and Select/Back the on-screen keyboard; the others
 * drive whichever is open (on the setup screen the d-pad and stick move and turn the pages, the shoulders change a value, A is Enter
 * and B is Escape). */
static void pad_key(shell *s, zr_pad_key key) {
    if (key == ZR_PAD_START) { setup_toggle(s); return; }
    if (key == ZR_PAD_BACK) { osk_toggle(s); return; }
    if (!s->setup && zr_osk_is_open(&s->osk)) { osk_pad(s, key, 1); return; }
    if (!s->setup) return;
    zr_tui_event ui;
    memset(&ui, 0, sizeof ui);
    switch (key) {
    case ZR_PAD_UP: ui.key = ZR_TUI_KEY_UP; break;
    case ZR_PAD_DOWN: ui.key = ZR_TUI_KEY_DOWN; break;
    case ZR_PAD_LEFT: ui.key = ZR_TUI_KEY_LEFT; break;
    case ZR_PAD_RIGHT: ui.key = ZR_TUI_KEY_RIGHT; break;
    case ZR_PAD_A: ui.key = ZR_TUI_KEY_ENTER; break;
    case ZR_PAD_B: ui.key = ZR_TUI_KEY_ESCAPE; break;
    case ZR_PAD_SHOULDER_LEFT: ui.key = ZR_TUI_KEY_PAGE_UP; break;
    case ZR_PAD_SHOULDER_RIGHT: ui.key = ZR_TUI_KEY_PAGE_DOWN; break;
    default: return;
    }
    setup_event(s, ui);
}

/* The keyboard joystick's key the setup screen waits for: any key, the window's own included, by its name. */
static void setup_key_taken(shell *s, int sc, unsigned mod) {
    const char *name = SDL_GetScancodeName((SDL_Scancode)sc);
    zr_setup_host_key(s->setup, name ? name : "", (mod & KMOD_CTRL ? ZR_SETUP_KEY_CTRL : 0u) | (mod & KMOD_ALT ? ZR_SETUP_KEY_ALT : 0u));
    setup_after(s);
}

/* A host key as an event of the setup screen: the keys that move, the characters that change a value or give a command. */
static int ui_key(const SDL_Keysym *k, zr_tui_event *e) {
    e->ch = 0;
    switch (k->sym) {
    case SDLK_UP: e->key = ZR_TUI_KEY_UP; return 1;
    case SDLK_DOWN: e->key = ZR_TUI_KEY_DOWN; return 1;
    case SDLK_LEFT: e->key = ZR_TUI_KEY_LEFT; return 1;
    case SDLK_RIGHT: e->key = ZR_TUI_KEY_RIGHT; return 1;
    case SDLK_RETURN:
    case SDLK_KP_ENTER: e->key = ZR_TUI_KEY_ENTER; return 1;
    case SDLK_ESCAPE: e->key = ZR_TUI_KEY_ESCAPE; return 1;
    case SDLK_PAGEUP: e->key = ZR_TUI_KEY_PAGE_UP; return 1;
    case SDLK_PAGEDOWN: e->key = ZR_TUI_KEY_PAGE_DOWN; return 1;
    case SDLK_HOME: e->key = ZR_TUI_KEY_HOME; return 1;
    case SDLK_END: e->key = ZR_TUI_KEY_END; return 1;
    case SDLK_TAB: e->key = k->mod & KMOD_SHIFT ? ZR_TUI_KEY_BACKTAB : ZR_TUI_KEY_TAB; return 1;
    case SDLK_BACKSPACE: e->key = ZR_TUI_KEY_BACKSPACE; return 1;
    case SDLK_PLUS:
    case SDLK_KP_PLUS: e->key = ZR_TUI_KEY_CHAR; e->ch = '+'; return 1;
    case SDLK_EQUALS:
        if (!(k->mod & KMOD_SHIFT)) return 0;
        e->key = ZR_TUI_KEY_CHAR;
        e->ch = '+';
        return 1;
    case SDLK_MINUS:
    case SDLK_KP_MINUS: e->key = ZR_TUI_KEY_CHAR; e->ch = (k->mod & KMOD_SHIFT) && k->sym == SDLK_MINUS ? '_' : '-'; return 1;
    case SDLK_PERIOD:
    case SDLK_KP_PERIOD: e->key = ZR_TUI_KEY_CHAR; e->ch = '.'; return 1;
    case SDLK_COLON: e->key = ZR_TUI_KEY_CHAR; e->ch = ':'; return 1;
    case SDLK_SEMICOLON:                                        /* a target's colon: Shift and the semicolon key */
        if (!(k->mod & KMOD_SHIFT)) return 0;
        e->key = ZR_TUI_KEY_CHAR;
        e->ch = ':';
        return 1;
    case SDLK_SLASH:
    case SDLK_KP_DIVIDE: e->key = ZR_TUI_KEY_CHAR; e->ch = '/'; return 1;
    case SDLK_BACKSLASH: e->key = ZR_TUI_KEY_CHAR; e->ch = '\\'; return 1;
    case SDLK_KP_0: e->key = ZR_TUI_KEY_CHAR; e->ch = '0'; return 1;
    case SDLK_SPACE: e->key = ZR_TUI_KEY_CHAR; e->ch = ' '; return 1;
    default:
        if (k->sym >= SDLK_a && k->sym <= SDLK_z && !(k->mod & (KMOD_CTRL | KMOD_ALT))) {
            e->key = ZR_TUI_KEY_CHAR;
            e->ch = (int)k->sym;
            return 1;
        }
        if (k->sym >= SDLK_0 && k->sym <= SDLK_9 && !(k->mod & (KMOD_CTRL | KMOD_ALT))) {      /* the number and name entries */
            e->key = ZR_TUI_KEY_CHAR;
            e->ch = (int)k->sym;
            return 1;
        }
        if (k->sym >= SDLK_KP_1 && k->sym <= SDLK_KP_9) {
            e->key = ZR_TUI_KEY_CHAR;
            e->ch = '1' + (int)(k->sym - SDLK_KP_1);
            return 1;
        }
        return 0;
    }
}

/* --- the harness's side ------------------------------------------------------------------- */

static zr_machine *h_machine(void *ctx) { return ((shell *)ctx)->m; }
static int h_insert(void *ctx, unsigned slot, const char *path, char *error, size_t capacity) { return media_insert(ctx, slot, path, 0, error, capacity); }
static int h_eject(void *ctx, unsigned slot) { return media_eject(ctx, slot); }
static int h_state_save(void *ctx, const char *path, char *error, size_t capacity) { return state_save(ctx, path, error, capacity); }
static int h_state_load(void *ctx, const char *path, char *error, size_t capacity) {
    shell *s = ctx;
    char warning[160];
    if (!state_load(s, path, warning, sizeof warning, error, capacity)) return 0;
    if (warning[0]) zr_osd_show(&s->osd, now_ms(s), "%s", warning);
    return 1;
}
/* rewind on|off|SECONDS: the ring kept or not; back through it by at least SECONDS of guest time, the states after dropped. */
static int h_rewind(void *ctx, const char *arg, char *error, size_t capacity) {
    shell *s = ctx;
    if (!strcmp(arg, "on")) { s->ring_on = 1; return 1; }
    if (!strcmp(arg, "off")) { s->ring_on = 0; zr_rewind_clear(&s->ring); return 1; }
    char *end = NULL;
    const double seconds = strtod(arg, &end);
    if (end == arg || *end || seconds <= 0 || seconds > 86400) { snprintf(error, capacity, "rewind takes on, off or a number of seconds"); return 0; }
    if (!s->ring.count) { snprintf(error, capacity, "Nothing to rewind (rewind on keeps the states)"); return 0; }
    const uint64_t back = (uint64_t)(seconds * (double)zr_machine_clock(s->m)), now = zr_machine_time(s->m);
    size_t at;
    if (!zr_rewind_find(&s->ring, now > back ? now - back : 0, &at)) {
        snprintf(error, capacity, "The ring goes back %.1f s only", (double)(now - zr_rewind_time(&s->ring, 0)) / zr_machine_clock(s->m));
        return 0;
    }
    if (!zr_rewind_restore(&s->ring, s->m, at, error, capacity)) { zr_rewind_clear(&s->ring); return 0; }
    zr_rewind_cut(&s->ring, at, ring_interval(s));
    ports_resend(s);
    release_all(s);
    s->resync = 1;
    return 1;
}
static void h_speed(void *ctx, int full) { set_speed(ctx, full); }
static int h_cheat_load(void *ctx, const char *path, char *error, size_t capacity) { return cheats_load(ctx, path, error, capacity); }
static int h_cheat_set(void *ctx, size_t index, int on, int value, char *error, size_t capacity) {
    shell *s = ctx;
    return zr_cheats_set(&s->cheats, s->m, index, on, value, error, capacity);
}
/* The setup screen's cells as text while it is open (what `text`, `expect` and `until` read then); 0 when it is not. */
static size_t h_screen_text(void *ctx, char *out, size_t capacity) {
    shell *s = ctx;
    if (!s->setup || !capacity) return 0;
    const zr_tui *t = zr_setup_screen(s->setup);
    size_t n = 0;
    for (int r = 0; r < ZR_TUI_ROWS; ++r) {
        size_t end = n;
        for (int c = 0; c < ZR_TUI_COLS && n + 2 < capacity; ++c) {
            const uint8_t ch = t->cell[r][c].ch;
            out[n++] = ch >= 0x20 && ch < 0x7F ? (char)ch : ' ';
            if (out[n - 1] != ' ') end = n;
        }
        n = end;                                                /* no spaces at the end of a line */
        if (n + 1 < capacity) out[n++] = '\n';
    }
    out[n] = 0;
    return n;
}
/* The on-screen keyboard for the harness: the pointer's click goes to the middle of the key's keycap, so the key is found from the position
 * as a real click's is. */
static int h_osk(void *ctx, int op, const char *arg, char *text, size_t capacity) {
    shell *s = ctx;
    text[0] = 0;
    if (op == ZR_HARNESS_OSK_ON || op == ZR_HARNESS_OSK_OFF) {
        if (op == ZR_HARNESS_OSK_ON && s->setup) { snprintf(text, capacity, "The setup screen is open"); return 0; }
        if ((op == ZR_HARNESS_OSK_ON) != (zr_osk_is_open(&s->osk) != 0)) osk_toggle(s);
        return 1;
    }
    if (!zr_osk_is_open(&s->osk)) { snprintf(text, capacity, "The on-screen keyboard is closed"); return 0; }
    zr_osk_layout(&s->osk, (int)s->width, (int)s->height, s->aspect_w, s->aspect_h);
    const zr_family *f = zr_machine_family(s->m);
    switch (op) {
    case ZR_HARNESS_OSK_MOUSE_DOWN: {
        zr_osk_rect r;
        const int key = zr_family_key(f, arg);
        if (key < 0) { snprintf(text, capacity, "This keyboard has no key %s", arg); return 0; }
        if (!zr_osk_key_rect(&s->osk, (unsigned)key, &r)) { snprintf(text, capacity, "The key %s has no keycap", arg); return 0; }
        s->osk_x = r.x + r.w / 2;
        s->osk_y = r.y + r.h / 2;
        zr_osk_mouse(&s->osk, s->osk_x, s->osk_y, ZR_OSK_MOUSE_MOVE);
        zr_osk_mouse(&s->osk, s->osk_x, s->osk_y, ZR_OSK_MOUSE_DOWN);
        return 1;
    }
    case ZR_HARNESS_OSK_MOUSE_UP:
        zr_osk_mouse(&s->osk, s->osk_x, s->osk_y, ZR_OSK_MOUSE_UP);
        return 1;
    case ZR_HARNESS_OSK_PAD:
    case ZR_HARNESS_OSK_PAD_RELEASE: {
        const char w = arg[0];
        const zr_osk_pad_key key = w == 'u' ? ZR_OSK_PAD_UP : w == 'd' ? ZR_OSK_PAD_DOWN : w == 'l' ? ZR_OSK_PAD_LEFT : w == 'r' ? ZR_OSK_PAD_RIGHT : w == 'b' ? ZR_OSK_PAD_B : ZR_OSK_PAD_A;
        zr_osk_pad(&s->osk, key, op == ZR_HARNESS_OSK_PAD);
        if (!zr_osk_is_open(&s->osk)) s->osd.text[0] = 0;
        else if (op == ZR_HARNESS_OSK_PAD && s->osk.selected >= 0) snprintf(text, capacity, "%s", f->keys[s->osk.selected].name);
        return 1;
    }
    default: {                                                  /* the keycaps */
        size_t n = 0;
        for (unsigned i = 0; i < f->key_count; ++i) {
            zr_osk_rect r;
            if (!zr_osk_key_rect(&s->osk, i, &r)) continue;
            const int put = snprintf(text + n, capacity - n, "%d %d %d %d %s\n", r.x, r.y, r.w, r.h, f->keys[i].name);
            if (put < 0 || (size_t)put >= capacity - n) break;
            n += (size_t)put;
        }
        return 1;
    }
    }
}
static int h_drop(void *ctx, const char *path, char *error, size_t capacity) {
    (void)error; (void)capacity;
    drop_file(ctx, path);
    return 1;
}
static void h_reset(void *ctx, int mode) {
    shell *s = ctx;
    if (mode != 2) release_all(s);
    zr_machine_reset(s->m, mode == 1);
    ports_resend(s);
}
/* The frame itself, at its own size, whatever the look. */
static int h_shot(void *ctx, const char *path, char *error, size_t capacity) {
    shell *s = ctx;
    if (!convert(s, 0)) { snprintf(error, capacity, "%s", s->error); return 0; }
    if (!zr_png_name(path)) { snprintf(error, capacity, "A screenshot is a .png file"); return 0; }
    if (!zr_png_write(path, s->pixels, (int)s->width, (int)s->height, (size_t)s->width * 4u, 0)) { snprintf(error, capacity, "Cannot write the file"); return 0; }
    return 1;
}

/* The control channel on standard input: a thread reads the lines, the loop takes them. */
static int stdin_reader(void *ctx) {
    shell *s = ctx;
    char line[CONTROL_LINE];
    while (fgets(line, sizeof line, stdin)) {
        for (;;) {                                              /* waits for room: no line is dropped */
            SDL_LockMutex(s->lock);
            const unsigned next = (s->line_write + 1) % CONTROL_LINES;
            const int room = next != s->line_read;
            if (room) {
                memcpy(s->lines[s->line_write], line, sizeof line);
                s->line_write = next;
            }
            SDL_UnlockMutex(s->lock);
            if (room) break;
            SDL_Delay(1);
        }
    }
    SDL_LockMutex(s->lock);
    s->stdin_closed = 1;
    SDL_UnlockMutex(s->lock);
    return 0;
}

static int h_control_read(void *ctx, char *line, size_t capacity) {
    shell *s = ctx;
    if (s->tcp) {
        /* A byte at a time up to the end of a line; a client that leaves is followed by the next one. */
        for (;;) {
            uint8_t byte;
            const long n = zr_host_stream_read(s->tcp, &byte, 1);
            if (n < 0) { s->tcp_used = 0; return 0; }
            if (n == 0) return 0;
            if (byte == '\n') {
                s->tcp_line[s->tcp_used] = 0;
                snprintf(line, capacity, "%s", s->tcp_line);
                s->tcp_used = 0;
                return 1;
            }
            if (s->tcp_used + 1 < sizeof s->tcp_line) s->tcp_line[s->tcp_used++] = (char)byte;
        }
    }
    int got = 0;
    SDL_LockMutex(s->lock);
    if (s->line_read != s->line_write) {
        snprintf(line, capacity, "%s", s->lines[s->line_read]);
        s->line_read = (s->line_read + 1) % CONTROL_LINES;
        got = 1;
    } else if (s->stdin_closed) got = -1;
    SDL_UnlockMutex(s->lock);
    return got;
}

static void h_control_write(void *ctx, const char *line) {
    shell *s = ctx;
    if (s->tcp) {
        zr_host_stream_write(s->tcp, (const uint8_t *)line, strlen(line));
        zr_host_stream_write(s->tcp, (const uint8_t *)"\n", 1);
        return;
    }
    puts(line);
    fflush(stdout);
}

/* Opens the control channel the options name; 0 with the reason. */
static int control_open(shell *s) {
    const char *c = s->options->control;
    if (!c) return 1;
    if (!strncmp(c, "tcp:", 4)) {
        char error[256];
        s->tcp = zr_host_stream_open(ZR_STREAM_TCP_LISTEN, c + 4, error, sizeof error);
        if (!s->tcp) { snprintf(s->error, sizeof s->error, "--control %s: %s", c, error); return 0; }
        printf("Control: TCP port %u\n", zr_host_stream_local_port(s->tcp));
        fflush(stdout);
        return 1;
    }
    if (strcmp(c, "stdin")) { snprintf(s->error, sizeof s->error, "--control needs stdin or tcp:PORT"); return 0; }
    s->lock = SDL_CreateMutex();
    s->reader = s->lock ? SDL_CreateThread(stdin_reader, "control", s) : NULL;
    if (!s->reader) { snprintf(s->error, sizeof s->error, "Cannot read the control channel: %s", SDL_GetError()); return 0; }
    SDL_DetachThread(s->reader);                                /* it may sit in a read when the run ends */
    return 1;
}

/* --- the window's keys and events ---------------------------------------------------------- */

static void toggle_pause(shell *s) {
    s->paused = !s->paused;
    if (s->paused) release_all(s);
    else s->resync = 1;
}

/* 1 when the key was the window's own. */
static int window_key(shell *s, int sc, unsigned mod) {
    if (sc == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) { zr_video_set_fullscreen(s->video, !zr_video_fullscreen(s->video)); return 1; }
    if (sc == SDL_SCANCODE_F12 && (mod & KMOD_SHIFT)) { s->quit = 1; return 1; }
    if (sc == SDL_SCANCODE_F12 && (mod & (KMOD_CTRL | KMOD_ALT))) {        /* Ctrl: off and on; Alt: the reset button */
        if (!s->setup) {
            const int power = (mod & KMOD_CTRL) != 0;
            release_all_but_held(s);
            zr_machine_reset(s->m, power);
            ports_resend(s);
            s->resync = 1;
            say_id(s, power ? ZR_T_SAY_POWER_CYCLE : ZR_T_SAY_RESET);
        }
        return 1;
    }
    if (sc == SDL_SCANCODE_F12) { setup_toggle(s); return 1; }
    if (sc == SDL_SCANCODE_PRINTSCREEN) { shot_key(s); return 1; }
    if (s->setup) return 0;                                     /* the setup screen has the other keys */
    if (sc == SDL_SCANCODE_F1 && !(mod & (KMOD_CTRL | KMOD_ALT | KMOD_SHIFT)) && tape_pick(s)) { setup_after(s); return 1; }
    if (sc == SDL_SCANCODE_F11) { joystick_switch(s); return 1; }
    if (sc == SDL_SCANCODE_F8) { osk_toggle(s); return 1; }
    if (sc == SDL_SCANCODE_F10) { zr_machine_nmi(s->m); return 1; }
    if (sc == SDL_SCANCODE_PAUSE) { toggle_pause(s); return 1; }
    if (sc == SDL_SCANCODE_F5 && (mod & KMOD_SHIFT)) { tape_record_key(s); return 1; }
    if (sc == SDL_SCANCODE_F5 || sc == SDL_SCANCODE_F6 || sc == SDL_SCANCODE_F7) { tape_key(s, sc); return 1; }
    if (sc == SDL_SCANCODE_F2 || sc == SDL_SCANCODE_F3) {
        char error[256];
        const unsigned slot = (unsigned)s->config->state_slot;
        rewind_stop(s);
        if (sc == SDL_SCANCODE_F2) slot_save(s, slot, error, sizeof error);
        else slot_load(s, slot, error, sizeof error);
        return 1;
    }
    if (sc == SDL_SCANCODE_F4) { rewind_start(s); return 1; }
    if (sc == SDL_SCANCODE_F9) { set_speed(s, !s->full_speed); return 1; }
    return 0;
}

static int event(shell *s, const SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT:
        return 0;
    case SDL_CONTROLLERDEVICEADDED:
    case SDL_CONTROLLERDEVICEREMOVED:
    case SDL_CONTROLLERBUTTONDOWN:
        if (s->pads_open) {
            zr_pad_key key;
            switch (zr_pads_event(&s->pads, e, &key)) {
            case ZR_PADS_CHANGED: if (s->setup) zr_setup_refresh(s->setup); break;      /* the names on the Controllers page */
            case ZR_PADS_KEY: pad_key(s, key); break;
            default: break;
            }
        }
        break;
    case SDL_CONTROLLERBUTTONUP:
        if (s->pads_open && !s->setup && zr_osk_is_open(&s->osk) && e->cbutton.button == SDL_CONTROLLER_BUTTON_A) osk_pad(s, ZR_PAD_A, 0);
        break;
    case SDL_MOUSEMOTION:
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (!s->setup && zr_osk_is_open(&s->osk)) osk_mouse(s, e);
        break;
    case SDL_KEYDOWN: {
        const int sc = e->key.keysym.scancode;
        const unsigned mod = e->key.keysym.mod;
        if (s->setup && zr_setup_wants_host_key(s->setup)) {
            if (!e->key.repeat) setup_key_taken(s, sc, mod);
            break;
        }
        if (s->setup) {
            /* The setup screen takes the keyboard, its repeats too; only the window's own keys stay the window's. */
            zr_tui_event ui;
            if ((sc == SDL_SCANCODE_RETURN && (mod & KMOD_ALT)) || sc == SDL_SCANCODE_F12 || sc == SDL_SCANCODE_PRINTSCREEN) {
                if (!e->key.repeat) window_key(s, sc, mod);
            } else if (ui_key(&e->key.keysym, &ui)) setup_event(s, ui);
            break;
        }
        if (e->key.repeat) break;
        zr_typist_cancel(&s->typist, s->m);                     /* a key of the user's stops the typing */
        if (window_key(s, sc, mod)) break;
        if (zr_joykeys_key(&s->joykeys, sc, 1)) break;
        if (!s->paused) guest_key(s, sc, 1);
        break;
    }
    case SDL_KEYUP:
        if (e->key.keysym.scancode == SDL_SCANCODE_F4) { rewind_stop(s); break; }
        /* Windows gives Print Screen no press, only a release: that is the key the setup screen waits for then. */
        if (s->setup && zr_setup_wants_host_key(s->setup) && e->key.keysym.scancode == SDL_SCANCODE_PRINTSCREEN) {
            setup_key_taken(s, e->key.keysym.scancode, e->key.keysym.mod);
            break;
        }
        if (zr_joykeys_key(&s->joykeys, e->key.keysym.scancode, 0)) break;
        if (!s->paused && !s->setup) guest_key(s, e->key.keysym.scancode, 0);
        break;
    case SDL_DROPFILE:
        drop_file(s, e->drop.file);
        SDL_free(e->drop.file);
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            rewind_stop(s);
            release_all(s);
        }
        break;
    default:
        break;
    }
    return 1;
}

/* A host key of the command line's --hostkey: pressed and released as the host's would be. */
static int hostkey_step(shell *s, const zr_desktop_hostkey *k) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    const SDL_Scancode sc = SDL_GetScancodeFromName(k->name);
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    e.key.keysym.mod = (Uint16)((k->mods & ZR_HOSTKEY_SHIFT ? KMOD_SHIFT : 0) | (k->mods & ZR_HOSTKEY_CTRL ? KMOD_CTRL : 0) |
                                (k->mods & ZR_HOSTKEY_ALT ? KMOD_ALT : 0));
    int r = 1;
    if (!(k->mods & ZR_HOSTKEY_RELEASE)) r = event(s, &e);
    e.type = SDL_KEYUP;
    return r && ((k->mods & ZR_HOSTKEY_PRESS) || event(s, &e));
}

int zr_desktop_key_known(const char *name) { return SDL_GetScancodeFromName(name) != SDL_SCANCODE_UNKNOWN; }

/* After each run of the machine: sound out. Silent (the machine's sound pulled and dropped, the device's queue emptied) while
 * the machine runs unthrottled (F9, a `fast` slot at work) or goes back (F4); it comes back with a fresh cushion. */
static int pump(shell *s) {
    const int silent = !s->config->sound || s->was_free || s->full_speed || s->rewinding;
    if (s->options->wav) zr_wav_pump(&s->wav, s->m);
    else if (!zr_host_audio_pump(&s->audio, s->m, (unsigned)s->config->volume, silent)) {
        snprintf(s->error, sizeof s->error, "Sound output: %s", SDL_GetError());
        return 0;
    }
    return 1;
}

static int run_status(shell *s, zr_run_status r) {
    if (r == ZR_RUN_ERROR) { snprintf(s->error, sizeof s->error, "%s", zr_machine_error(s->m)); return 0; }
    return 1;
}

int zr_desktop_run(zr_machine **machine, zr_config *config, const zr_desktop_options *options, char *error, size_t capacity) {
    shell *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return 1; }
    s->m = *machine;
    s->machine_out = machine;
    zr_cheats_init(&s->cheats);
    zr_host_ports_init(&s->ports);
    s->config = config;
    s->options = options;
    s->hidden = options->hidden;
    s->det = options->hidden && !options->realtime;
    zr_rewind_init(&s->ring, ZR_REWIND_BUDGET);
    s->ring_on = !options->hidden;                              /* headless, the harness's `rewind on` */
    zr_font8x8_build(s->font);
    zr_text_set_font(s->font);
    zr_text_set_language(config->language);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER)) {
        snprintf(error, capacity, "SDL: %s", SDL_GetError());
        free(s);
        return 1;
    }
    map_keys(s);
    zr_osk_init(&s->osk, zr_machine_family(s->m), osk_key, s);
    inputs_init(s);
    zr_frame f;
    memset(&f, 0, sizeof f);
    zr_machine_frame(s->m, &f);
    zr_frame_crop(&f, config->border);
    s->width = f.width ? f.width : 320;
    s->height = f.height ? f.height : 240;
    zr_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.window_scale = 3;
    settings.aspect = config->aspect == ZR_CONFIG_ASPECT_SQUARE ? ZR_ASPECT_SQUARE : ZR_ASPECT_ORIGINAL;
    settings.fullscreen = config->fullscreen && !s->hidden;
    settings.vsync = config->vsync && !s->hidden;
    s->video = zr_video_open(TITLE, &settings, (int)s->width, (int)s->height, f.aspect_w ? (int)f.aspect_w : 4,
                             f.aspect_h ? (int)f.aspect_h : 3, s->hidden != 0, error, capacity);
    if (!s->video) {
        SDL_Quit();
        free(s);
        return 1;
    }
#ifndef _WIN32                              /* on Windows the window has the program file's own icon (zetro.rc) */
    if (!s->hidden) zr_video_set_icon(s->video, zr_icon_pixels, ZR_ICON_SIZE);
#endif
    /* The control channel on standard output carries only the answers. */
    if (!options->control || strcmp(options->control, "stdin")) {
        printf("Presenter: %s\n", zr_video_backend(s->video));
        fflush(stdout);
    }
    apply_look(s);
    int status = 0;
    if (options->wav && !zr_wav_open(&s->wav, options->wav)) { snprintf(s->error, sizeof s->error, "Cannot write %s", options->wav); status = 1; }
    /* Headless runs make no sound on the host: the samples are discarded. */
    if (!status) audio_apply(s);
    if (!status) media_configured(s);
    if (!status) ports_apply(s);
    if (!status && !s->hidden) s->pads_open = zr_pads_open(&s->pads);       /* game controllers: a visible window only */
    if (!status && options->drop) drop_file(s, options->drop);
    if (!status && !control_open(s)) status = 1;
    if (!status && (options->script || options->control)) {
        zr_harness_host host = {s, h_machine, h_insert, h_eject, h_shot, h_state_save, h_state_load, h_reset,
                                options->control ? h_control_read : NULL, options->control ? h_control_write : NULL, s->det,
                                h_rewind, h_speed, h_screen_text, snapshot_load_said, snapshot_save_said, h_cheat_load, h_cheat_set, h_osk, h_drop};
        s->harness = zr_harness_open(&host, options->script, s->error, sizeof s->error);
        if (!s->harness) status = 1;
    }
    if (!status && options->setup_page >= 0 && !setup_open(s, options->setup_page)) status = 1;

    const uint64_t freq = SDL_GetPerformanceFrequency();
    zr_pace pace;
    zr_pace_init(&pace, freq, zr_machine_clock(s->m), CATCH_UP_MS);
    zr_pace_reset(&pace, SDL_GetPerformanceCounter(), zr_machine_time(s->m));
    uint64_t frames = 0;
    int running = !status;
    while (running) {
        SDL_Event e;
        while (running && SDL_PollEvent(&e)) running = event(s, &e) && !s->quit;
        if (!running || s->quit) break;
        tape_watch(s);
        if (s->setup && s->pads_open) {                         /* a direction held on a controller repeats like a key */
            zr_pad_key key;
            if (zr_pads_repeat(&s->pads, SDL_GetTicks(), &key)) pad_key(s, key);
        } else if (zr_osk_is_open(&s->osk) && s->pads_open) {   /* ... and on the on-screen keyboard */
            zr_pad_key key;
            if (zr_pads_repeat(&s->pads, SDL_GetTicks(), &key)) pad_key(s, key);
        }
        /* The machine stands still while the setup screen is open and while it goes back (F4 held). */
        const int stopped = s->paused || s->setup != NULL || s->rewinding;
        const uint64_t period = zr_machine_frame_period(s->m);
        const uint64_t now = s->det ? 0 : SDL_GetPerformanceCounter();
        if (s->resync) {
            zr_pace_init(&pace, freq, zr_machine_clock(s->m), CATCH_UP_MS);     /* another model may have another clock */
            zr_pace_reset(&pace, now, zr_machine_time(s->m));
            s->resync = 0;
        }
        if (s->det) {
            /* The harness first, then a frame period of guest time, then the frame. Guest time stands still while the
             * control channel is quiet, so a run is the same every time. */
            const int step = s->harness ? zr_harness_step(s->harness) : ZR_HARNESS_RUN;
            if (step == ZR_HARNESS_QUIT) break;
            if (step == ZR_HARNESS_IDLE) { zr_host_ports_pump(&s->ports, s->m); zr_host_sleep_us(1000); continue; }   /* the ends are served while the channel is quiet */
            if (!stopped) input_frame(s);
            if (!stopped && (!run_status(s, zr_machine_run(s->m, period)) || !pump(s))) { status = 1; break; }
            zr_host_ports_pump(&s->ports, s->m);
            if (!stopped) ring_take(s);
            if (s->rewinding) {
                rewind_frame(s);
                if (!pump(s)) { status = 1; break; }
            }
        } else {
            /* Unthrottled for as long as a slot with fast access (media.*.fast) works; back to the clock afterwards, the time
             * gained kept, not waited out. */
            const int free_running = !stopped && (s->full_speed || fast_active(s));
            if (s->was_free && !free_running) zr_pace_reset(&pace, now, zr_machine_time(s->m));
            s->was_free = free_running;
            if (stopped) zr_pace_hold(&pace, now, zr_machine_time(s->m));
            const zr_pace_step step = zr_pace_next(&pace, free_running, now, zr_machine_time(s->m), period);
            if (step.action == ZR_PACE_RUN) {
                if (!run_status(s, zr_machine_run(s->m, step.ticks)) || !pump(s)) { status = 1; break; }
                zr_host_ports_pump(&s->ports, s->m);
                ring_take(s);
                continue;
            }
            if (step.action == ZR_PACE_WAIT) {
                /* A sleep shorter than 100 us is not worth the call: the loop polls again. */
                const uint64_t us = step.ticks * 1000000u / freq;
                if (us >= 100u) zr_host_sleep_us((uint32_t)us);
                continue;
            }
            /* A picture is due: the harness has its turn once a frame. */
            if (s->harness && zr_harness_step(s->harness) == ZR_HARNESS_QUIT) break;
            zr_host_ports_pump(&s->ports, s->m);
            if (!stopped) input_frame(s);
            if (s->rewinding) {
                rewind_frame(s);
                if (!pump(s)) { status = 1; break; }
            }
        }
        /* Checks: host keys at given frames. */
        while (running && !s->quit && s->hostkey_next < options->hostkey_count && frames + 1 >= options->hostkeys[s->hostkey_next].at)
            running = hostkey_step(s, &options->hostkeys[s->hostkey_next++]);
        if (!running || s->quit) break;
        if (!convert(s, 1)) { status = 1; break; }
        zr_video_present(s->video, s->pixels, (int)s->width, (int)s->height);
        shot_done(s);
        ++s->shown;
        ++frames;
        if (options->frames && frames >= options->frames) running = 0;
    }
    if (!status && s->harness && zr_harness_status(s->harness)) {
        status = zr_harness_status(s->harness);
        snprintf(s->error, sizeof s->error, "%s", zr_harness_error(s->harness));
    }
    if (status == 0 && options->screenshot) {
        /* The frame itself when no look applies; else the picture as shown. */
        if (!convert(s, 1)) status = 1;
        else {
            zr_video_request_shot(s->video, options->screenshot, config->look == ZR_LOOK_PLAIN);
            zr_video_present(s->video, s->pixels, (int)s->width, (int)s->height);
            char saved[1024];
            if (!zr_video_take_shot_result(s->video, saved, sizeof saved) || !saved[0]) {
                snprintf(s->error, sizeof s->error, "Cannot save the screenshot %s", options->screenshot);
                status = 1;
            }
        }
    }
    if (options->wav && s->wav.file && !zr_wav_close(&s->wav) && !status) {
        snprintf(s->error, sizeof s->error, "Cannot write %s", options->wav);
        status = 1;
    }
    zr_harness_close(s->harness);
    zr_setup_destroy(s->setup);
    zr_cheats_clear(&s->cheats, NULL);
    zr_host_ports_close(&s->ports, s->m);                       /* what the guest sent last is written out */
    /* The media go back to their files. */
    for (unsigned i = 0; i < ZR_CONFIG_MEDIA; ++i)
        if (!zr_states_close_media(s->m, i, &s->media[i], s->config->source, error, capacity)) {
            fprintf(stderr, "%s\n", error);
            if (!status) snprintf(s->error, sizeof s->error, "%s", error);
            status = 1;
        }
    if (status) snprintf(error, capacity, "%s", s->error);
    if (s->tcp) zr_host_stream_close(s->tcp);
    zr_host_audio_close(&s->audio);
    if (s->pads_open) zr_pads_close(&s->pads);
    free(s->pixels);
    zr_rewind_free(&s->ring);
    zr_video_close(s->video);
    SDL_Quit();
    /* The reader thread, when there is one, may still hold the shell: it is left to the process's end. */
    if (!s->reader) free(s);
    zr_text_set_font(NULL);
    zr_text_set_language(ZR_LANG_EN);       /* what the program says after the window is gone is a diagnostic: English */
    return status;
}
