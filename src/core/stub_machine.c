/* The test machine (machine.h, machine_impl.h): the family "stub" with the
 * one model "test". It runs no guest code: its time advances exactly as
 * asked, and it shows a 32x24 text screen in a border, in the Spectrum's
 * sixteen colours, drawn with ZetRo's own 8x8 font: the glyphs, the
 * colours, how many of the known ROMs were found, the tape in its slot and
 * the keys typed, each with a beep. It has every part a board has (models,
 * an option, a media slot, a keyboard table, typing, a state), so the
 * shell, the configuration, the presenter, the sound path, the states and
 * the harness are checked end to end without a ROM.
 *
 * It also stands in for a model whose ROMs are missing, and then says
 * which files to fetch.
 *
 * ZetRo's own code; no hardware is described
 * here. */
#include "machine_impl.h"
#include "font8x8.h"
#include "media/tape.h"
#include "text.h"
#include "zr_version.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COLS 32
#define ROWS 24
#define BORDER 32
#define WIDTH (COLS * 8 + 2 * BORDER)
#define HEIGHT (ROWS * 8 + 2 * 24)
#define TOP 24
/* A 14 MHz master clock and a Spectrum's frame: 448 x 312 dots of 7 MHz. */
#define CLOCK_HZ 14000000u
#define FRAME_PERIOD (448u * 312u * 2u)
/* Sound: 48 kHz, derived exactly from guest time: 48000 / 14000000 = 6 / 1750. */
#define AUDIO_RATE 48000u
#define AUDIO_NUM 6u
#define AUDIO_DEN 1750u
#define TONE_FRAMES (AUDIO_RATE / 10u)    /* 100 ms per key press */
#define TONE_HALF_PERIOD 27u              /* about 889 Hz */
#define ECHO_MAX 24
#define MISSING_MAX 6

/* Attributes as the Spectrum has them: ink in bits 0-2, paper in bits 3-5, bright in bit 6. */
#define ATTR(ink, paper, bright) ((uint8_t)((ink) | (paper) << 3 | (bright) << 6))
enum { BLACK, BLUE, RED, MAGENTA, GREEN, CYAN, YELLOW, WHITE };

static const uint32_t PALETTE[16] = {
    0x000000, 0x0000CD, 0xCD0000, 0xCD00CD, 0x00CD00, 0x00CDCD, 0xCDCD00, 0xCDCDCD,
    0x000000, 0x0000FF, 0xFF0000, 0xFF00FF, 0x00FF00, 0x00FFFF, 0xFFFF00, 0xFFFFFF};

/* --- the family's tables --------------------------------------------------------------- */

enum { K_ENTER = 29, K_SHIFT = 30, K_DELETE = 38, K_SPACE = 39, KEY_COUNT = 40 };
#define KEY(name, host, x, y) {name, host, name, x, y, 4, 4}
static const zr_key_desc KEYS[KEY_COUNT] = {
    KEY("1", "1", 0, 0), KEY("2", "2", 4, 0), KEY("3", "3", 8, 0), KEY("4", "4", 12, 0), KEY("5", "5", 16, 0),
    KEY("6", "6", 20, 0), KEY("7", "7", 24, 0), KEY("8", "8", 28, 0), KEY("9", "9", 32, 0), KEY("0", "0", 36, 0),
    KEY("Q", "Q", 2, 4), KEY("W", "W", 6, 4), KEY("E", "E", 10, 4), KEY("R", "R", 14, 4), KEY("T", "T", 18, 4),
    KEY("Y", "Y", 22, 4), KEY("U", "U", 26, 4), KEY("I", "I", 30, 4), KEY("O", "O", 34, 4), KEY("P", "P", 38, 4),
    KEY("A", "A", 3, 8), KEY("S", "S", 7, 8), KEY("D", "D", 11, 8), KEY("F", "F", 15, 8), KEY("G", "G", 19, 8),
    KEY("H", "H", 23, 8), KEY("J", "J", 27, 8), KEY("K", "K", 31, 8), KEY("L", "L", 35, 8),
    {"ENTER", "Return", "ENTER", 39, 8, 6, 4},
    {"SHIFT", "Left Shift", "SHIFT", 0, 12, 5, 4},
    KEY("Z", "Z", 5, 12), KEY("X", "X", 9, 12), KEY("C", "C", 13, 12), KEY("V", "V", 17, 12), KEY("B", "B", 21, 12),
    KEY("N", "N", 25, 12), KEY("M", "M", 29, 12),
    {"DELETE", "Backspace", "DELETE", 33, 12, 6, 4},
    {"SPACE", "Space", "SPACE", 39, 12, 6, 4},
};
/* What each key types: capitals, as the test screen echoes them. */
static const char KEY_CHAR[KEY_COUNT + 1] = "1234567890QWERTYUIOPASDFGHJKL\n\0ZXCVBNM\b ";

#define N ZR_KEY_NONE
static const zr_typing_desc TYPING[] = {
    {'1', 0, N}, {'2', 1, N}, {'3', 2, N}, {'4', 3, N}, {'5', 4, N}, {'6', 5, N}, {'7', 6, N}, {'8', 7, N}, {'9', 8, N}, {'0', 9, N},
    {'Q', 10, N}, {'W', 11, N}, {'E', 12, N}, {'R', 13, N}, {'T', 14, N}, {'Y', 15, N}, {'U', 16, N}, {'I', 17, N}, {'O', 18, N}, {'P', 19, N},
    {'A', 20, N}, {'S', 21, N}, {'D', 22, N}, {'F', 23, N}, {'G', 24, N}, {'H', 25, N}, {'J', 26, N}, {'K', 27, N}, {'L', 28, N},
    {'Z', 31, N}, {'X', 32, N}, {'C', 33, N}, {'V', 34, N}, {'B', 35, N}, {'N', 36, N}, {'M', 37, N},
    {'q', 10, N}, {'w', 11, N}, {'e', 12, N}, {'r', 13, N}, {'t', 14, N}, {'y', 15, N}, {'u', 16, N}, {'i', 17, N}, {'o', 18, N}, {'p', 19, N},
    {'a', 20, N}, {'s', 21, N}, {'d', 22, N}, {'f', 23, N}, {'g', 24, N}, {'h', 25, N}, {'j', 26, N}, {'k', 27, N}, {'l', 28, N},
    {'z', 31, N}, {'x', 32, N}, {'c', 33, N}, {'v', 34, N}, {'b', 35, N}, {'n', 36, N}, {'m', 37, N},
    {'\n', K_ENTER, N}, {'\b', K_DELETE, N}, {' ', K_SPACE, N},
};
#undef N

static const char *const NO_ROMS[] = {NULL};
static const zr_model_desc MODELS[] = {{"test", "Test screen", NO_ROMS, "LOAD\n"}};
static const char *const SWITCH[] = {"off", "on", NULL};
enum { OPTION_BEEP };
static const zr_option_desc OPTIONS[] = {{"beep", ZR_T_STUB_BEEP, SWITCH, NULL, 1, ZR_MODELS_ALL, 0}};
static const zr_media_desc MEDIA[] = {{"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL}};

typedef struct stub {
    char description[160], error[128];
    uint8_t font[2048];
    int beep;
    const zr_family *wanted;              /* the model that could not be built (NULL: the test screen was asked for) */
    unsigned wanted_model;
    char missing[MISSING_MAX][COLS + 1];
    unsigned missing_count, found, known;
    uint32_t composed_language;
    zr_media *tape;
    zr_tape *blocks;                      /* the tape as blocks, NULL when the file is no tape ZetRo reads */
    zr_tape_deck deck;
    unsigned stick;                       /* the joystick socket: ZR_JOY_* held */
    /* time */
    uint64_t time;
    /* the screen: characters and attributes, then the frame */
    uint8_t text[ROWS][COLS][2];
    uint8_t pixels[WIDTH * HEIGHT];
    uint64_t serial;
    uint64_t drawn_phase, drawn_input, input_serial;
    /* the echo */
    char echo[ECHO_MAX + 1];
    /* sound */
    uint64_t audio_done, tone_until;
} stub;

/* --- the text screen ---------------------------------------------------------------- */

static void put(stub *s, int row, int col, uint8_t attr, const char *text) {
    for (; *text && col < COLS; ++text, ++col) {
        s->text[row][col][0] = (uint8_t)*text;
        s->text[row][col][1] = attr;
    }
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static void putf(stub *s, int row, int col, uint8_t attr, const char *format, ...) {
    char line[COLS + 1];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    put(s, row, col, attr, line);
}
static void fill(stub *s, int row, int col, int count, uint8_t ch, uint8_t attr) {
    for (int i = 0; i < count && col + i < COLS; ++i) {
        s->text[row][col + i][0] = ch;
        s->text[row][col + i][1] = attr;
    }
}

/* The file name of a path. */
static const char *leaf(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; ++p) if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

/* Everything but the cursor, in the language in use. */
static void compose(stub *s) {
    const uint8_t plain = ATTR(BLACK, WHITE, 0), title = ATTR(WHITE, BLACK, 1), alert = ATTR(RED, WHITE, 0);
    s->composed_language = zr_text_language();
    for (int r = 0; r < ROWS; ++r) fill(s, r, 0, COLS, ' ', plain);
    /* The title bar, with the Romanian flag's stripes at its right end. */
    fill(s, 0, 0, COLS, ' ', title);
    putf(s, 0, 1, title, ZR_TXT(STUB_TITLE), ZR_VERSION);
    static const uint8_t STRIPES[3] = {BLUE, YELLOW, RED};
    for (int i = 0; i < 3; ++i) fill(s, 0, COLS - 5 + i, 1, ' ', ATTR(BLACK, STRIPES[i], 1));
    if (s->wanted) {
        putf(s, 2, 1, alert, ZR_TXT(STUB_MISSING), s->wanted->models[s->wanted_model].name);
        put(s, 3, 1, plain, ZR_TXT(STUB_FETCH));
    } else put(s, 2, 1, plain, ZR_TXT(STUB_NAME));
    /* The glyphs 20h to 7Fh, 32 to a row. */
    for (int i = 0; i < 96; ++i) fill(s, 5 + i / 32, i % 32, 1, (uint8_t)(0x20 + i), plain);
    /* The sixteen colours: as paper with their numbers, then as ink. */
    put(s, 9, 0, plain, ZR_TXT(STUB_PAPER));
    put(s, 11, 0, plain, ZR_TXT(STUB_INK));
    for (int c = 0; c < 16; ++c) {
        const uint8_t ink = (c & 7) < 4 ? WHITE : BLACK;
        putf(s, 10, 2 * c, ATTR(ink, c & 7, c >> 3), "%X ", c);
        fill(s, 12, 2 * c, 2, 0xDB, ATTR(c & 7, WHITE, c >> 3));
    }
    putf(s, 14, 0, plain, ZR_TXT(STUB_ROMS), s->found, s->known);
    for (unsigned i = 0; i < s->missing_count; ++i) put(s, 15 + (int)i, 2, alert, s->missing[i]);
    if (s->tape) {
        char name[16];
        snprintf(name, sizeof name, "%s", leaf(s->tape->path));
        putf(s, 21, 0, plain, ZR_TXT(STUB_TAPE), name, (unsigned)s->tape->size);
        if (s->blocks) putf(s, 22, 0, plain, ZR_TXT(STUB_BLOCKS), zr_tape_count(s->blocks), zr_tape_deck_playing(&s->deck) ? ">" : "");
    } else put(s, 21, 0, plain, ZR_TXT(STUB_NO_TAPE));
    {
        static const char NAMES[6] = {'U', 'D', 'L', 'R', 'F', 'G'};
        char held[8] = "";
        for (unsigned i = 0, n = 0; i < 6; ++i) if (s->stick >> i & 1u) { held[n++] = NAMES[i]; held[n] = 0; }
        putf(s, 19, 0, plain, ZR_TXT(STUB_STICK), held);
    }
    put(s, 23, 0, plain, ZR_TXT(STUB_KEYS));
    put(s, 23, 1 + (int)strlen(ZR_TXT(STUB_KEYS)), ATTR(BLUE, WHITE, 0), s->echo);
}

/* The frame: the border, then each cell's glyph rows in its colours; the
 * cursor a flashing block after the echo. */
static void render(stub *s, int cursor_on) {
    memset(s->pixels, WHITE, sizeof s->pixels);
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c) {
            const uint8_t ch = s->text[r][c][0], attr = s->text[r][c][1];
            const uint8_t bright = attr & 0x40u ? 8u : 0u, ink = (attr & 7u) | bright, paper = (attr >> 3 & 7u) | bright;
            for (int y = 0; y < 8; ++y) {
                const uint8_t bits = s->font[ch * 8 + y];
                uint8_t *p = s->pixels + (size_t)(TOP + r * 8 + y) * WIDTH + BORDER + (size_t)c * 8;
                for (int x = 0; x < 8; ++x) p[x] = bits & (0x80u >> x) ? ink : paper;
            }
        }
    if (cursor_on) {
        const int col = 1 + (int)strlen(ZR_TXT(STUB_KEYS)) + (int)strlen(s->echo);
        if (col < COLS)
            for (int y = 0; y < 8; ++y) memset(s->pixels + (size_t)(TOP + 23 * 8 + y) * WIDTH + BORDER + (size_t)col * 8, BLUE, 8);
    }
    ++s->serial;
}

static void refresh(stub *s) {
    const uint64_t phase = s->time / FRAME_PERIOD / 16u;
    if (s->composed_language != zr_text_language() || s->input_serial != s->drawn_input) {
        compose(s);
        s->drawn_phase = UINT64_MAX;
    }
    if (phase != s->drawn_phase || s->input_serial != s->drawn_input) {
        s->drawn_phase = phase;
        s->drawn_input = s->input_serial;
        render(s, !(phase & 1u));
    }
}

static int stub_frame(void *self, zr_frame *f) {
    stub *s = self;
    refresh(s);
    f->width = WIDTH;
    f->height = HEIGHT;
    f->pitch = WIDTH;
    f->pixels = s->pixels;
    f->palette = PALETTE;
    f->colors = 16;
    f->aspect_w = 4;
    f->aspect_h = 3;
    f->serial = s->serial;
    f->picture_x = BORDER;
    f->picture_y = TOP;
    f->picture_w = COLS * 8;
    f->picture_h = ROWS * 8;
    return 1;
}

/* The screen's characters, a line of the screen a line of the text, without the spaces at the ends of lines. */
static size_t stub_text(void *self, char *out, size_t capacity) {
    stub *s = self;
    size_t n = 0;
    refresh(s);
    for (int r = 0; r < ROWS; ++r) {
        int last = COLS;
        while (last > 0 && s->text[r][last - 1][0] == ' ') --last;
        for (int c = 0; c < last && n + 2 < capacity; ++c) {
            const uint8_t ch = s->text[r][c][0];
            out[n++] = ch >= 0x20 && ch < 0x7F ? (char)ch : '#';
        }
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n] = 0;
    return n;
}

/* --- life cycle ------------------------------------------------------------------ */

static void *stub_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    stub *s = calloc(1, sizeof *s);
    if (!s) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    zr_font8x8_build(s->font);
    zr_tape_deck_init(&s->deck, CLOCK_HZ);
    s->beep = setup->options[OPTION_BEEP] != 0;
    if (setup->wanted_family && setup->wanted_family != &zr_stub_family) {
        s->wanted = setup->wanted_family;
        s->wanted_model = setup->wanted_model;
        for (const char *const *key = s->wanted->models[s->wanted_model].roms; key && *key; ++key) {
            const zr_known_rom *rom = zr_romdb_find(*key);
            if (rom && setup->roms && zr_rom_index_find(setup->roms, rom)) continue;
            if (s->missing_count < MISSING_MAX) snprintf(s->missing[s->missing_count++], sizeof s->missing[0], "%s", rom ? rom->file : *key);
        }
        snprintf(s->description, sizeof s->description, "test screen: the ROMs of %s are missing", s->wanted->models[s->wanted_model].name);
    } else snprintf(s->description, sizeof s->description, "test screen");
    s->known = (unsigned)zr_romdb_count();
    for (size_t i = 0; i < zr_romdb_count(); ++i) s->found += setup->roms && zr_rom_index_find(setup->roms, zr_romdb_entry(i)) != NULL;
    s->drawn_phase = UINT64_MAX;
    compose(s);
    return s;
}

static void stub_destroy(void *self) {
    stub *s = self;
    zr_tape_deck_release(&s->deck);
    zr_tape_close(s->blocks);
    free(s);
}
static const char *stub_description(const void *self) { return ((const stub *)self)->description; }
static const char *stub_error(const void *self) { return ((const stub *)self)->error; }

static void stub_reset(void *self, int power_on) {
    stub *s = self;
    (void)power_on;
    s->echo[0] = 0;
    ++s->input_serial;
}

static uint32_t stub_clock(const void *self) { (void)self; return CLOCK_HZ; }
static zr_run_status stub_run(void *self, uint64_t ticks) {
    stub *s = self;
    s->time += ticks;
    if (s->blocks) zr_tape_deck_advance(&s->deck, s->time);
    return ZR_RUN_OK;
}
static uint64_t stub_time(const void *self) { return ((const stub *)self)->time; }
static uint64_t stub_frame_period(const void *self) { (void)self; return FRAME_PERIOD; }

/* --- sound ------------------------------------------------------------------------ */

static uint32_t stub_audio_rate(const void *self) { (void)self; return AUDIO_RATE; }

static size_t stub_audio_pull(void *self, int16_t *out, size_t frames) {
    stub *s = self;
    const uint64_t due = s->time * AUDIO_NUM / AUDIO_DEN;
    /* Frames the shell never took are dropped beyond one second. */
    if (due - s->audio_done > AUDIO_RATE) s->audio_done = due - AUDIO_RATE;
    size_t n = 0;
    for (; n < frames && s->audio_done < due; ++n, ++s->audio_done) {
        const int16_t v = s->audio_done < s->tone_until ? (s->audio_done / TONE_HALF_PERIOD & 1u ? 6000 : -6000) : 0;
        out[n * 2] = out[n * 2 + 1] = v;
    }
    return n;
}

/* --- input -------------------------------------------------------------------------- */

static void stub_key(void *self, unsigned key, int down) {
    stub *s = self;
    if (!down || key >= KEY_COUNT || key == K_SHIFT) return;
    ++s->input_serial;
    if (s->beep) s->tone_until = s->time * AUDIO_NUM / AUDIO_DEN + TONE_FRAMES;
    size_t n = strlen(s->echo);
    const char ch = KEY_CHAR[key];
    if (ch == '\b') { if (n) s->echo[n - 1] = 0; return; }
    if (ch == '\n') { s->echo[0] = 0; return; }                 /* ENTER starts a new line */
    if (n == ECHO_MAX) { memmove(s->echo, s->echo + 1, ECHO_MAX - 1); n--; }
    s->echo[n] = ch;
    s->echo[n + 1] = 0;
}

/* --- media: a tape slot that only holds its file --------------------------------------- */

static int stub_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    stub *s = self;
    (void)slot;
    if (s->tape) {
        if (capacity) snprintf(error, capacity, "The slot is not empty");
        return 0;
    }
    s->tape = media;
    /* A file ZetRo reads as a tape goes on the recorder; any other is only held. */
    char why[128];
    s->blocks = zr_tape_open(media, why, sizeof why);
    zr_tape_deck_set_tape(&s->deck, s->blocks);
    ++s->input_serial;
    return 1;
}
static zr_media *stub_media(const void *self, unsigned slot) { (void)slot; return ((const stub *)self)->tape; }
static int stub_eject(void *self, unsigned slot) {
    stub *s = self;
    (void)slot;
    if (!s->tape) return 0;
    zr_tape_deck_set_tape(&s->deck, NULL);
    zr_tape_close(s->blocks);
    s->blocks = NULL;
    s->tape = NULL;
    ++s->input_serial;
    return 1;
}
static unsigned stub_activity(const void *self) { return zr_tape_deck_playing(&((const stub *)self)->deck) ? 1u : 0u; }
static struct zr_tape_deck *stub_tape_deck(void *self, unsigned slot) {
    stub *s = self;
    (void)slot;
    return s->blocks ? &s->deck : NULL;
}
/* The recorder's keys. */
static int stub_control(void *self, unsigned slot, unsigned command, uint32_t value) {
    stub *s = self;
    (void)slot;
    if (!s->blocks) return 0;
    switch (command) {
    case ZR_MEDIA_PLAY: zr_tape_deck_play(&s->deck, s->time); break;
    case ZR_MEDIA_STOP: zr_tape_deck_stop(&s->deck, s->time); break;
    case ZR_MEDIA_REWIND: zr_tape_deck_rewind(&s->deck, s->time); break;
    case ZR_MEDIA_NEXT: zr_tape_deck_next(&s->deck, s->time); break;
    case ZR_MEDIA_PREVIOUS: zr_tape_deck_previous(&s->deck, s->time); break;
    case ZR_MEDIA_SEEK: zr_tape_deck_seek(&s->deck, value, s->time); break;
    default: return 0;
    }
    ++s->input_serial;
    return 1;
}
static void stub_joystick(void *self, unsigned stick, unsigned held) {
    stub *s = self;
    if (stick || held == s->stick) return;
    s->stick = held;
    ++s->input_serial;
}

/* --- states ------------------------------------------------------------------------- */

static void stub_save(const void *self, zr_state_writer *w) {
    const stub *s = self;
    zr_sw_begin(w, "STUB", 0);
    zr_sw_u64(w, s->time);
    zr_sw_u64(w, s->audio_done);
    zr_sw_u64(w, s->tone_until);
    zr_sw_str(w, s->echo);
    zr_sw_end(w);
    /* The frame as memory, to exercise the packing a board's RAM goes through. */
    zr_sw_begin(w, "RAM ", 0);
    zr_sw_packed(w, s->pixels, sizeof s->pixels);
    zr_sw_end(w);
}

static int stub_load(void *self, zr_state_reader *r) {
    stub *s = self;
    if (!zr_sr_chunk(r, "STUB", 0)) return 0;
    s->time = zr_sr_u64(r);
    s->audio_done = zr_sr_u64(r);
    s->tone_until = zr_sr_u64(r);
    zr_sr_str(r, s->echo, sizeof s->echo);
    if (!zr_sr_chunk(r, "RAM ", 0)) return 0;
    zr_sr_packed(r, s->pixels, sizeof s->pixels);
    ++s->input_serial;
    return zr_sr_ok(r);
}

static uint32_t stub_rom_hashes(const void *self, uint8_t *hashes, uint32_t max) { (void)self; (void)hashes; (void)max; return 0; }

static const zr_machine_impl IMPL = {
    .destroy = stub_destroy, .description = stub_description, .error = stub_error, .reset = stub_reset,
    .clock = stub_clock, .run = stub_run, .time = stub_time, .frame_period = stub_frame_period,
    .frame = stub_frame, .text = stub_text, .audio_rate = stub_audio_rate, .audio_pull = stub_audio_pull,
    .key = stub_key, .joystick = stub_joystick, .control = stub_control, .tape_deck = stub_tape_deck, .insert = stub_insert, .media = stub_media, .eject = stub_eject, .activity = stub_activity,
    .save = stub_save, .load = stub_load, .rom_hashes = stub_rom_hashes,
};

const zr_family zr_stub_family = {
    .id = "stub", .name = "ZetRo",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = 1,
    .media = MEDIA, .media_count = 1,
    .keys = KEYS, .key_count = KEY_COUNT,
    .typing = TYPING, .typing_count = sizeof TYPING / sizeof TYPING[0],
    .joysticks = 1,
    .impl = &IMPL, .create = stub_create,
};
