/* The setup screen (setup.h). ZetRo's own code: rows built from the state, pop-ups in front of the page, changes made to a copy of the
 * configuration, and the pages built from the running machine's tables. */
#include "setup.h"
#include "logo_data.h"
#include "states.h"
#include "../core/font8x8.h"
#include "../core/media/card.h"
#include "../core/media/snap.h"
#include "../core/media/tape.h"
#include "../core/text.h"
#include "../core/yamledit.h"
#include "../host/host.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 96
#define VALUE_LEN (ZR_CONFIG_PATH + 32)
#define LIST_ROW 1                      /* the page's list: rows 1 to 21 */
#define LIST_ROWS 21
#define LEGEND_ROW 22
#define HINT_ROW 23
#define MENU_MAX 64
#define POPUP_TOP 2                     /* pop-ups are centred between these rows, a row of black under the title bar above them */
#define POPUP_BOTTOM 21
#define STATE_ROWS ZR_STATES_SLOTS      /* the States page: the slots in rows 1 to 10, the thumbnail's box under them */
#define THUMB_ROW 11                    /* the box: its title bar, 9 rows of picture (72 pixels), its bottom line */
#define THUMB_COL 9
#define THUMB_COLS 14                   /* 12 cells (96 pixels) of picture between its sides */
#define ABOUT_LOGO_ROW 2                /* the About page: the logo's panel, the version under it, then what is new in it */
#define ABOUT_VERSION_ROW (ABOUT_LOGO_ROW + ZR_LOGO_H / 8 + 1)
#define ABOUT_NEW_ROW (ABOUT_VERSION_ROW + 2)

static const char *const PAGE_NAMES[ZR_SETUP_PAGES] = {"machine", "media", "display", "sound", "controllers", "ports", "states", "cheats", "about"};
static const int PAGE_TITLES[ZR_SETUP_PAGES] = {ZR_T_PAGE_MACHINE, ZR_T_PAGE_MEDIA, ZR_T_PAGE_DISPLAY, ZR_T_PAGE_SOUND, ZR_T_PAGE_CONTROLLERS,
                                                 ZR_T_PAGE_PORTS, ZR_T_PAGE_STATES, ZR_T_PAGE_CHEATS, ZR_T_PAGE_ABOUT};

const char *zr_setup_page_name(int page) { return page >= 0 && page < ZR_SETUP_PAGES ? PAGE_NAMES[page] : "?"; }
int zr_setup_page_by_name(const char *name) {
    for (int i = 0; i < ZR_SETUP_PAGES; ++i) if (!strcmp(name, PAGE_NAMES[i])) return i;
    return -1;
}

/* --- what changed ---------------------------------------------------------------------- */

/* The value an option has in a configuration: its index among the option's values. */
static int option_index(const zr_config *c, const zr_family *f, unsigned option) {
    const char *word = zr_config_option_value(c, f->options[option].key);
    const int v = word ? zr_family_option_value(f, option, word) : -1;
    return v >= 0 ? v : (int)f->options[option].initial;
}

/* The value an option has for a model under a configuration once the family's rules are applied (a forced value), and whether
 * the model has it now and whether it is forced (ZR_OPTION_PRESENT, ZR_OPTION_FORCED). */
static int option_resolved(const zr_config *c, const zr_family *f, unsigned model, unsigned option, unsigned *state) {
    unsigned values[ZR_CONFIG_OPTIONS];
    uint8_t states[ZR_CONFIG_OPTIONS];
    zr_family_resolve(f, model, c, values, states);
    if (state) *state = option < ZR_CONFIG_OPTIONS ? states[option] : 0u;
    return option < ZR_CONFIG_OPTIONS ? (int)values[option] : 0;
}

/* A slot's settings as the configuration has them, the defaults for a slot it does not mention. */
static zr_config_media media_settings(const zr_config *c, const char *key) {
    static zr_config blank;
    const zr_config_media *m = zr_config_media_find(c, key);
    if (m) return *m;
    blank.media_count = 0;
    return *zr_config_media_slot(&blank, key);
}

static int media_differs(const zr_config *a, const zr_config *b) {
    for (int pass = 0; pass < 2; ++pass) {
        const zr_config *c = pass ? b : a;
        for (unsigned i = 0; i < c->media_count; ++i) {
            const zr_config_media ma = media_settings(a, c->media[i].key), mb = media_settings(b, c->media[i].key);
            if (strcmp(ma.image, mb.image) || ma.write_protect != mb.write_protect || ma.fast != mb.fast || ma.sound != mb.sound ||
                ma.autoload != mb.autoload || ma.full_speed != mb.full_speed || ma.traps != mb.traps || ma.save_traps != mb.save_traps ||
                ma.accelerate != mb.accelerate)
                return 1;
        }
    }
    return 0;
}

/* The host end the configuration gives a port: its type and target (none when it has no word for it). */
static void port_end(const zr_config *c, const char *key, uint32_t *type, const char **path) {
    const zr_config_port *p = zr_config_port_find(c, key);
    *type = p ? p->type : (uint32_t)ZR_END_NONE;
    *path = p ? p->path : "";
}

static int ports_differ(const zr_config *a, const zr_config *b) {
    for (int pass = 0; pass < 2; ++pass) {
        const zr_config *c = pass ? b : a;
        for (unsigned i = 0; i < c->port_count; ++i) {
            uint32_t ta, tb;
            const char *pa, *pb;
            port_end(a, c->ports[i].key, &ta, &pa);
            port_end(b, c->ports[i].key, &tb, &pb);
            if (ta != tb || (ta != ZR_END_NONE && strcmp(pa, pb))) return 1;
        }
    }
    return 0;
}

unsigned zr_setup_diff(const zr_config *a, const zr_config *b) {
    unsigned f = 0;
    /* A new model, or any option of it: the machine has no call to take an option while it runs (ZR_OPTION_RESET only says whether
     * the setup screen's hint tells so), so every change of one is a machine built again. */
    if (strcmp(a->model, b->model)) f |= ZR_CHANGE_RESTART;
    else {
        unsigned model = 0;
        const zr_family *family = zr_family_of_model(b->model, &model);
        for (unsigned i = 0; family && i < family->option_count; ++i)
            if ((family->options[i].models >> model & 1u) &&
                (option_index(a, family, i) != option_index(b, family, i) ||
                 option_resolved(a, family, model, i, NULL) != option_resolved(b, family, model, i, NULL)))
                f |= ZR_CHANGE_RESTART;
    }
    if (a->look != b->look || a->aspect != b->aspect || a->scale != b->scale || a->filter != b->filter || a->border != b->border ||
        a->fullscreen != b->fullscreen ||
        a->vsync != b->vsync)
        f |= ZR_CHANGE_VIDEO;
    if (a->sound != b->sound || a->volume != b->volume) f |= ZR_CHANGE_SOUND;
    if (a->language != b->language) f |= ZR_CHANGE_LANGUAGE;
    if (media_differs(a, b)) f |= ZR_CHANGE_MEDIA;
    for (unsigned i = 0; i < ZR_CONFIG_STICKS; ++i)
        if (a->sticks[i].device != b->sticks[i].device || (b->sticks[i].device == ZR_STICK_CONTROLLER && a->sticks[i].pad != b->sticks[i].pad))
            f |= ZR_CHANGE_CONTROLLERS;
    for (unsigned i = 0; i < ZR_CONFIG_STICK_KEYS; ++i)
        if (strcmp(a->stick_keys[i], b->stick_keys[i])) f |= ZR_CHANGE_CONTROLLERS;
    if (a->keyboard_joystick != b->keyboard_joystick) f |= ZR_CHANGE_CONTROLLERS;
    if (a->state_slot != b->state_slot) f |= ZR_CHANGE_STATE_SLOT;
    if (ports_differ(a, b)) f |= ZR_CHANGE_PORTS;
    return f;
}

/* --- the state ------------------------------------------------------------------------- */

typedef enum { MODAL_NONE, MODAL_MENU, MODAL_DIALOG, MODAL_PICKER, MODAL_TAPE, MODAL_ENTRY } modal_kind;
typedef enum {
    TAG_NONE, TAG_CHOICE, TAG_LEAVE, TAG_RESET, TAG_POWER, TAG_NOTE, TAG_PICK_FILE, TAG_SLOT, TAG_DELETE, TAG_PICK_SNAP, TAG_PICK_POK,
    TAG_ENTRY_SNAP, TAG_ENTRY_VALUE, TAG_PORT_PICK, TAG_PORT_HOW, TAG_PICK_PORT, TAG_ENTRY_PORT, TAG_NEW_CARD
} modal_tag;

/* Row ids: what a row is, whatever its place on the page. */
enum {
    ID_SLOT = 100,                      /* + 10 * slot + PART_* */
    ID_MODEL = 300, ID_RESET, ID_POWER, ID_EXIT,
    ID_OPTION = 320,                    /* + the option's index in the family's table */
    ID_LOOK = 400, ID_ASPECT, ID_SCALE, ID_FILTER, ID_FULLSCREEN, ID_VSYNC, ID_LANGUAGE, ID_BORDER,
    ID_SOUND = 500, ID_VOLUME,
    ID_STICK = 600,                     /* + the socket: the joystick's device */
    ID_STICK_KEY = 620,                 /* + the function (ZR_STICK_UP...): the keyboard joystick's key for it */
    ID_KEYBOARD_START = 640,
    ID_PORT = 660,                      /* + the port's index in the family's table: what it is connected to */
    ID_STATE = 700,                     /* + the slot (1 to 10) */
    ID_SNAP_LOAD = 720, ID_SNAP_SAVE,
    ID_CHEATS_LOAD = 750,
    ID_CHEAT = 800                      /* + the trainer's index */
};
enum { PART_HEAD, PART_FILE, PART_EJECT, PART_PROTECT, PART_FAST, PART_BROWSE, PART_RECORD, PART_NEW, PART_SOUND, PART_AUTOLOAD };

struct zr_setup {
    zr_setup_env env;
    zr_config edit, running;
    int saved_fullscreen, fullscreen_touched;
    int dirty;                          /* the configuration changed since it was written */
    zr_tui tui;
    int page;
    /* the page's rows */
    zr_tui_item items[MAX_ROWS];
    int ids[MAX_ROWS];
    char labels[MAX_ROWS][64];
    char values[MAX_ROWS][VALUE_LEN];
    char hints[MAX_ROWS][96];
    size_t rows;
    zr_tui_list list;
    int selected_id[ZR_SETUP_PAGES];
    /* the pop-up in front */
    modal_kind modal;
    modal_tag tag;
    int arg;
    zr_tui_item menu_items[MENU_MAX];
    char menu_labels[MENU_MAX][48];
    char menu_values[MENU_MAX][32];
    char menu_title[32];
    zr_tui_list menu;
    int key_wait;                       /* 1 + the keyboard joystick function that waits for its key, 0: none does */
    zr_tui_dialog dialog;
    int picker_open;
    zr_tui_picker picker;
    /* the entry pop-up: a snapshot's file name, a trainer's value or a port's target, typed */
    char entry[ZR_CONFIG_PATH];
    size_t entry_len;
    int entry_numeric;
    int entry_free;                     /* a target: any printable character, up to the buffer's end */
    char entry_title[32], entry_prompt[64];
    /* the tape browser */
    unsigned tape_slot;
    zr_tui_item *tape_items;
    char (*tape_names)[48];
    size_t tape_count;
    zr_tui_list tape_list;
    char last_folder[ZR_CONFIG_PATH];
    int done, reset;
    int quick;                          /* opened for one file picker (zr_setup_pick_media): left when the picker is */
    int want_exit, exit;                /* the program is to end once the screen is left */
    /* the States page: what each slot's file says, read when the page is opened and after a change */
    zr_states_info *slot_info;          /* ZR_STATES_SLOTS of them, slot 1 first */
    int slot_full[ZR_STATES_SLOTS];
    int slots_read;
};

/* --- the file system for the picker -------------------------------------------------------- */

static int is_root(const char *p) {
    const size_t n = strlen(p);
    if (zr_host_separator() == '/') return n == 1 && p[0] == '/';
    return n == 3 && p[1] == ':' && (p[2] == '\\' || p[2] == '/');
}

static int is_separator(char c) { return c == '/' || (zr_host_separator() == '\\' && c == '\\'); }

/* No separator at the end, except for a root. */
static void trim_folder(char *p) {
    size_t n = strlen(p);
    while (n > 1 && is_separator(p[n - 1]) && !is_root(p)) p[--n] = 0;
}

static int fs_list(void *ctx, const char *folder, zr_tui_entry_fn fn, void *fn_ctx) { (void)ctx; return zr_host_list_folder(folder, fn, fn_ctx); }
static int fs_roots(void *ctx, zr_tui_entry_fn fn, void *fn_ctx) { (void)ctx; return zr_host_list_roots(fn, fn_ctx); }

static int fs_parent(void *ctx, const char *folder, char *out, size_t capacity) {
    (void)ctx;
    char tmp[ZR_CONFIG_PATH];
    if (is_root(folder)) return 0;
    snprintf(tmp, sizeof tmp, "%s", folder);
    trim_folder(tmp);
    char *cut = NULL;
    for (char *c = tmp; *c; ++c) if (is_separator(*c)) cut = c;
    if (!cut) return 0;
    if (cut == tmp) tmp[1] = 0;                                                  /* "/x" -> "/" */
    else if (zr_host_separator() == '\\' && cut == tmp + 2 && tmp[1] == ':') cut[1] = 0;     /* "C:\x" -> "C:\" */
    else *cut = 0;
    snprintf(out, capacity, "%s", tmp);
    return 1;
}

static void fs_join(void *ctx, const char *folder, const char *name, char *out, size_t capacity) {
    (void)ctx;
    const size_t n = strlen(folder);
    snprintf(out, capacity, "%s%s%s", folder, n && is_separator(folder[n - 1]) ? "" : (const char[]){zr_host_separator(), 0}, name);
}

static zr_tui_fs host_fs(void) {
    zr_tui_fs fs = {NULL, fs_list, fs_roots, fs_parent, fs_join};
    return fs;
}

/* The folder of a file's path, with its separator ("" when it has none). */
static void folder_of(const char *path, char *out, size_t capacity) {
    snprintf(out, capacity, "%s", path);
    char *cut = NULL;
    for (char *c = out; *c; ++c) if (is_separator(*c)) cut = c;
    if (cut) cut[1] = 0; else out[0] = 0;
}

/* A path as the configuration holds it: relative to the configuration's folder when it lies inside it (as the reader
 * would give it back). */
static void config_path(const zr_setup *s, const char *absolute, char *out, size_t capacity) {
    char base[ZR_CONFIG_PATH], full_base[ZR_CONFIG_PATH];
    folder_of(s->edit.source, base, sizeof base);
    if (!zr_host_full_path(base[0] ? base : ".", full_base, sizeof full_base)) { snprintf(out, capacity, "%s", absolute); return; }
    trim_folder(full_base);
    const size_t n = strlen(full_base);
    if (!strncmp(absolute, full_base, n) && is_separator(absolute[n]) && !is_root(full_base)) snprintf(out, capacity, "%s%s", base, absolute + n + 1);
    else snprintf(out, capacity, "%s", absolute);
}

/* Where a file picker starts: the folder last used here, the media/ folder beside the configuration, the configuration's
 * own folder. */
static void media_folder(const zr_setup *s, char *out, size_t capacity) {
    char path[ZR_CONFIG_PATH], base[ZR_CONFIG_PATH];
    path[0] = 0;
    if (s->last_folder[0]) snprintf(path, sizeof path, "%s", s->last_folder);
    else {
        folder_of(s->edit.source, base, sizeof base);
        char media[ZR_CONFIG_PATH];
        snprintf(media, sizeof media, "%smedia", base);
        snprintf(path, sizeof path, "%s", zr_host_folder_exists(media) ? media : base[0] ? base : ".");
    }
    if (!zr_host_full_path(path, out, capacity)) snprintf(out, capacity, "%s", path);
    trim_folder(out);
}

/* The name of a file inside its path. */
static const char *leaf(const char *path) {
    const char *name = path;
    for (const char *p = path; *p; ++p) if (is_separator(*p)) name = p + 1;
    return name;
}

/* --- the machine's tables --------------------------------------------------------------------- */

/* The family and model the running machine is. */
static const zr_family *running_family(const zr_setup *s, unsigned *model) {
    *model = zr_machine_model(s->env.machine);
    return zr_machine_family(s->env.machine);
}

/* The family and model the edited configuration names. */
static const zr_family *edit_family(const zr_setup *s, unsigned *model) {
    *model = 0;
    return zr_family_of_model(s->edit.model, model);
}

/* 1 when the model lacks a ROM it cannot run without. */
static int roms_missing(const zr_setup *s, const zr_model_desc *model) {
    if (!s->env.roms) return 0;
    for (const char *const *key = model->roms; key && *key; ++key)
        if (!zr_rom_index_find(s->env.roms, zr_romdb_find(*key))) return 1;
    return 0;
}

/* A switch: an option of the two values off and on, shown as a toggle. */
static int is_switch(const zr_option_desc *o) {
    return o->values[0] && o->values[1] && !o->values[2] && !strcmp(o->values[0], "off") && !strcmp(o->values[1], "on");
}

/* The slot a row of the media page belongs to, or -1 for another id. */
static int slot_of(int id, int *part) {
    if (id < ID_SLOT || id >= ID_SLOT + 10 * ZR_CONFIG_MEDIA) return -1;
    *part = (id - ID_SLOT) % 10;
    return (id - ID_SLOT) / 10;
}

/* A media slot's key and settings in the edited configuration, creating its entry. */
static zr_config_media *slot_settings(zr_setup *s, unsigned slot) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    return slot < f->media_count ? zr_config_media_slot(&s->edit, f->media[slot].key) : NULL;
}

/* A model changed in the edited configuration: what the new model does not have goes (its options, its media slots), so the
 * file stays one the reader accepts. */
static void prune(zr_config *c) {
    unsigned model = 0;
    const zr_family *f = zr_family_of_model(c->model, &model);
    if (!f) return;
    unsigned n = 0;
    for (unsigned i = 0; i < c->option_count; ++i) {
        const int o = zr_family_option(f, c->options[i].key);
        if (o < 0 || !(f->options[o].models >> model & 1u) || zr_family_option_value(f, (unsigned)o, c->options[i].value) < 0) continue;
        c->options[n++] = c->options[i];
    }
    c->option_count = n;
    n = 0;
    for (unsigned i = 0; i < c->media_count; ++i) {
        const int m = zr_family_media(f, c->media[i].key);
        if (m < 0 || !(f->media[m].models >> model & 1u)) continue;
        c->media[n++] = c->media[i];
    }
    c->media_count = n;
}

/* --- the options of the choices -------------------------------------------------------------- */

typedef struct options {
    int count, current;
    char label[MENU_MAX][48];
    char value[MENU_MAX][32];
    int enabled[MENU_MAX];
    int family[MENU_MAX], model[MENU_MAX];
    char title[32];
} options;

typedef struct choice {
    int id;
    size_t offset;                      /* of a uint32_t in zr_config */
    unsigned count;
    const int *texts;
    const uint32_t *values;
    int title;
} choice;

static const int TXT_LOOK[] = {ZR_T_V_LOOK_CRT, ZR_T_V_LOOK_PLAIN};
static const uint32_t V_LOOK[] = {ZR_LOOK_CRT, ZR_LOOK_PLAIN};
static const int TXT_ASPECT[] = {ZR_T_V_ASPECT_ORIGINAL, ZR_T_V_ASPECT_SQUARE};
static const uint32_t V_ASPECT[] = {ZR_CONFIG_ASPECT_ORIGINAL, ZR_CONFIG_ASPECT_SQUARE};
static const int TXT_SCALE[] = {ZR_T_V_SCALE_FIT, ZR_T_V_SCALE_INTEGER};
static const uint32_t V_SCALE[] = {ZR_CONFIG_SCALE_FIT, ZR_CONFIG_SCALE_INTEGER};
static const int TXT_FILTER[] = {ZR_T_V_FILTER_NEAREST, ZR_T_V_FILTER_LINEAR, ZR_T_V_FILTER_SHARP};
static const uint32_t V_FILTER[] = {ZR_CONFIG_FILTER_NEAREST, ZR_CONFIG_FILTER_LINEAR, ZR_CONFIG_FILTER_SHARP};
static const int TXT_BORDER[] = {ZR_T_V_BORDER_FULL, ZR_T_V_BORDER_MEDIUM, ZR_T_V_BORDER_MINIMAL, ZR_T_V_BORDER_NONE};
static const uint32_t V_BORDER[] = {ZR_CONFIG_BORDER_FULL, ZR_CONFIG_BORDER_MEDIUM, ZR_CONFIG_BORDER_MINIMAL, ZR_CONFIG_BORDER_NONE};
static const int TXT_LANGUAGE[] = {ZR_T_V_LANGUAGE_EN, ZR_T_V_LANGUAGE_RO};
static const uint32_t V_LANGUAGE[] = {ZR_LANG_EN, ZR_LANG_RO};

#define CHOICE_T(id, field, texts, values, title) {id, offsetof(zr_config, field), sizeof texts / sizeof *texts, texts, values, title}
static const choice CHOICES[] = {
    CHOICE_T(ID_LOOK, look, TXT_LOOK, V_LOOK, ZR_T_L_LOOK),
    CHOICE_T(ID_ASPECT, aspect, TXT_ASPECT, V_ASPECT, ZR_T_L_ASPECT),
    CHOICE_T(ID_SCALE, scale, TXT_SCALE, V_SCALE, ZR_T_L_SCALE),
    CHOICE_T(ID_FILTER, filter, TXT_FILTER, V_FILTER, ZR_T_L_FILTER),
    CHOICE_T(ID_BORDER, border, TXT_BORDER, V_BORDER, ZR_T_L_BORDER),
    CHOICE_T(ID_LANGUAGE, language, TXT_LANGUAGE, V_LANGUAGE, ZR_T_L_LANGUAGE),
};

typedef struct toggle { int id; size_t offset; } toggle;
static const toggle TOGGLES[] = {
    {ID_FULLSCREEN, offsetof(zr_config, fullscreen)}, {ID_VSYNC, offsetof(zr_config, vsync)}, {ID_SOUND, offsetof(zr_config, sound)},
    {ID_KEYBOARD_START, offsetof(zr_config, keyboard_joystick)},
};

static const choice *find_choice(int id) {
    for (size_t i = 0; i < sizeof CHOICES / sizeof *CHOICES; ++i) if (CHOICES[i].id == id) return &CHOICES[i];
    return NULL;
}
static const toggle *find_toggle(int id) {
    for (size_t i = 0; i < sizeof TOGGLES / sizeof *TOGGLES; ++i) if (TOGGLES[i].id == id) return &TOGGLES[i];
    return NULL;
}
static uint32_t *choice_field(zr_setup *s, const choice *c) { return (uint32_t *)((char *)&s->edit + c->offset); }
static int *toggle_field(zr_setup *s, const toggle *t) { return (int *)((char *)&s->edit + t->offset); }

/* A machine option's row: its index in the family's table, or -1 for another id. */
static int option_of(int id) { return id >= ID_OPTION && id < ID_OPTION + ZR_CONFIG_OPTIONS ? id - ID_OPTION : -1; }

/* The joystick sockets the running machine has that the configuration knows (at most two). */
static unsigned joy_sockets(const zr_setup *s) {
    const unsigned n = zr_machine_family(s->env.machine)->joysticks;
    return n < ZR_CONFIG_STICKS ? n : ZR_CONFIG_STICKS;
}
/* Whether a socket has a joystick in it: the machine's option "joystick" (the first socket) or "joystick2" says what kind, and
 * "none" is an empty socket; a machine without such an option has its joystick always. */
static int socket_fitted(const zr_setup *s, unsigned socket) {
    const zr_family *f = zr_machine_family(s->env.machine);
    const int o = zr_family_option(f, socket ? "joystick2" : "joystick");
    if (o < 0 || o >= ZR_CONFIG_OPTIONS || !(f->options[o].models >> zr_machine_model(s->env.machine) & 1u)) return 1;
    return strcmp(f->options[o].values[zr_machine_options(s->env.machine)[o]], "none") != 0;
}
static int stick_of(int id) { return id >= ID_STICK && id < ID_STICK + ZR_CONFIG_STICKS ? id - ID_STICK : -1; }
static int stick_key_of(int id) { return id >= ID_STICK_KEY && id < ID_STICK_KEY + ZR_CONFIG_STICK_KEYS ? id - ID_STICK_KEY : -1; }
static const char *pad_name(const zr_setup *s, unsigned index) { return s->env.pad_name ? s->env.pad_name(s->env.ctx, index) : NULL; }

/* "Joystick" for a machine with one socket, "Joystick 1" and "Joystick 2" for two. */
static void stick_title(const zr_setup *s, unsigned socket, char *out, size_t capacity) {
    if (joy_sockets(s) > 1) zr_text_format(out, capacity, ZR_T_L_JOYSTICK_N, socket + 1);
    else snprintf(out, capacity, "%s", ZR_TXT(L_JOYSTICK));
}

/* The labels of the keyboard joystick's six functions, in the order of ZR_STICK_UP to ZR_STICK_FIRE2. */
static const int KEY_FUNCTIONS[ZR_CONFIG_STICK_KEYS] = {ZR_T_L_KEY_UP, ZR_T_L_KEY_DOWN, ZR_T_L_KEY_LEFT, ZR_T_L_KEY_RIGHT, ZR_T_L_KEY_FIRE, ZR_T_L_KEY_FIRE2};

/* The percentages of a setting in steps from lo to hi, with the file's own value among them when it is between two steps. */
static int percent_list(int lo, int hi, int step, int current, int values[MENU_MAX]) {
    int n = 0, placed = 0;
    for (int v = lo; v <= hi && n < MENU_MAX - 1; v += step) {
        if (!placed && current < v) { values[n++] = current; placed = 1; }
        if (v == current) placed = 1;
        values[n++] = v;
    }
    return n;
}

/* Text into a buffer of `capacity` bytes, cut where it does not fit. */
static void copy_text(char *to, size_t capacity, const char *from) {
    size_t n = strlen(from);
    if (n >= capacity) n = capacity - 1;
    memmove(to, from, n);
    to[n] = 0;
}

static void add_option(options *o, const char *label, int enabled) {
    if (o->count >= MENU_MAX) return;
    copy_text(o->label[o->count], sizeof o->label[0], label);
    o->value[o->count][0] = 0;
    o->enabled[o->count] = enabled;
    o->family[o->count] = o->model[o->count] = -1;
    ++o->count;
}

/* --- the byte ports ------------------------------------------------------------------------------------------ */

static int port_of(int id) { return id >= ID_PORT && id < ID_PORT + ZR_CONFIG_PORTS ? id - ID_PORT : -1; }

/* The kinds of end a port takes, in the order of the menu: a printer has no receive line to loop back. */
static unsigned port_kinds(const zr_port_desc *d, uint32_t types[ZR_END_KINDS], int texts[ZR_END_KINDS]) {
    static const struct { uint32_t type; int text; } ALL[] = {
        {ZR_END_NONE, ZR_T_V_PORT_NONE}, {ZR_END_LOOPBACK, ZR_T_V_PORT_LOOPBACK}, {ZR_END_SERIAL, ZR_T_V_PORT_SERIAL},
        {ZR_END_PIPE, ZR_T_V_PORT_PIPE}, {ZR_END_TCP, ZR_T_V_PORT_TCP}, {ZR_END_TCP_LISTEN, ZR_T_V_PORT_TCP_LISTEN},
        {ZR_END_FILE, ZR_T_V_PORT_FILE}};
    unsigned n = 0;
    for (size_t i = 0; i < sizeof ALL / sizeof *ALL; ++i) {
        if (d->kind == ZR_PORT_PARALLEL && ALL[i].type == ZR_END_LOOPBACK) continue;
        types[n] = ALL[i].type;
        texts[n++] = ALL[i].text;
    }
    return n;
}

#define MAX_SERIAL 16
typedef struct serial_names {
    char name[MAX_SERIAL][32];
    unsigned count;
} serial_names;

static int collect_serial(void *ctx, const char *name) {
    serial_names *l = ctx;
    if (l->count >= MAX_SERIAL) return 0;
    snprintf(l->name[l->count++], sizeof l->name[0], "%s", name);
    return 1;
}

/* The host's serial ports as they are now (a USB adapter may have come or gone). */
static void list_serial(serial_names *l) {
    l->count = 0;
    zr_host_list_serial(collect_serial, l);
}

/* The edited configuration's end of a port of the running machine, made when it has none yet. */
static zr_config_port *port_edit(zr_setup *s, unsigned index) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    return index < f->port_count ? zr_config_port_slot(&s->edit, f->ports[index].key) : NULL;
}

/* The configuration's folder with its separator ("" for the current one). */
static void config_folder(const zr_setup *s, char *out, size_t capacity) { folder_of(s->edit.source, out, capacity); }

/* A path as the file shows it: without the configuration's folder in front. */
static const char *shown_path(const zr_setup *s, const char *path) {
    char base[ZR_CONFIG_PATH];
    config_folder(s, base, sizeof base);
    const size_t n = strlen(base);
    return n && !strncmp(path, base, n) ? path + n : path;
}

/* What a file name typed for a port means: relative to the configuration's folder unless it is absolute (as the reader has it). */
static void typed_path(const zr_setup *s, const char *text, char *out, size_t capacity) {
    char base[ZR_CONFIG_PATH];
    config_folder(s, base, sizeof base);
    const int absolute = text[0] == '/' || text[0] == '\\' || (text[0] && text[1] == ':');
    snprintf(out, capacity, "%s%s", absolute ? "" : base, text);
}

/* The target a new kind of end starts with. */
static void port_default(const zr_setup *s, unsigned index, uint32_t type, char *out, size_t capacity) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    const char *key = index < f->port_count ? f->ports[index].key : "port";
    const int windows = zr_host_separator() == '\\';
    out[0] = 0;
    if (type == ZR_END_SERIAL) {
        serial_names l;
        list_serial(&l);
        if (l.count) snprintf(out, capacity, "%s", l.name[0]);
        else snprintf(out, capacity, "%s", windows ? "COM1" : "/dev/ttyS0");
    } else if (type == ZR_END_PIPE) snprintf(out, capacity, windows ? "\\\\.\\pipe\\zetro-%s" : "/tmp/zetro-%s", key);
    else if (type == ZR_END_FILE) {
        char base[ZR_CONFIG_PATH];
        config_folder(s, base, sizeof base);
        snprintf(out, capacity, "%s%s.out", base, key);
    } else if (type == ZR_END_TCP) snprintf(out, capacity, "127.0.0.1:%u", 2323u + index);
    else if (type == ZR_END_TCP_LISTEN) snprintf(out, capacity, "%u", 2323u + index);
}

/* Every model of the registry, the families of one name together (the HC's boards are several families of one name), their models in
 * the registry's order, and which is the edited configuration's. */
static void model_options(const zr_setup *s, options *o) {
    for (unsigned f = 0; f < zr_family_count(); ++f) {
        const zr_family *first = zr_family_at(f);
        int shown = 0;
        for (unsigned e = 0; e < f && !shown; ++e) shown = !strcmp(zr_family_at(e)->name, first->name);
        if (shown) continue;
        for (unsigned g = f; g < zr_family_count(); ++g) {
            const zr_family *family = zr_family_at(g);
            if (strcmp(family->name, first->name)) continue;
            for (unsigned k = 0; k < family->model_count; ++k) {
                add_option(o, family->models[k].name, 1);
                if (o->count >= MENU_MAX) return;
                o->family[o->count - 1] = (int)g;
                o->model[o->count - 1] = (int)k;
                if (roms_missing(s, &family->models[k])) copy_text(o->value[o->count - 1], sizeof o->value[0], ZR_TXT(V_NO_ROMS));
                if (!strcmp(family->models[k].id, s->edit.model)) o->current = o->count - 1;
            }
        }
    }
}

/* The choices of a CHOICE row and which is current. */
static void options_of(const zr_setup *s, int id, options *o) {
    memset(o, 0, sizeof *o);
    if (id == ID_MODEL) {
        snprintf(o->title, sizeof o->title, "%s", ZR_TXT(L_MODEL));
        model_options(s, o);
        return;
    }
    const int opt = option_of(id);
    if (opt >= 0) {
        unsigned model;
        const zr_family *f = edit_family(s, &model);
        if (!f || (unsigned)opt >= f->option_count) return;
        const zr_option_desc *d = &f->options[opt];
        snprintf(o->title, sizeof o->title, "%s", zr_t(d->label));
        for (int i = 0; d->values[i]; ++i) add_option(o, d->value_labels ? zr_t(d->value_labels[i]) : d->values[i], 1);
        o->current = option_resolved(&s->edit, f, model, (unsigned)opt, NULL);
        return;
    }
    const choice *c = find_choice(id);
    if (c) {
        const uint32_t now = *(const uint32_t *)((const char *)&s->edit + c->offset);
        snprintf(o->title, sizeof o->title, "%s", zr_t(c->title));
        for (unsigned i = 0; i < c->count; ++i) {
            add_option(o, zr_t(c->texts[i]), 1);
            if (c->values[i] == now) o->current = (int)i;
        }
        return;
    }
    const int port = port_of(id);
    if (port >= 0) {
        unsigned model;
        const zr_family *f = running_family(s, &model);
        if ((unsigned)port >= f->port_count) return;
        const zr_port_desc *d = &f->ports[port];
        uint32_t types[ZR_END_KINDS];
        int texts[ZR_END_KINDS];
        const unsigned n = port_kinds(d, types, texts);
        const zr_config_port *now = zr_config_port_find(&s->edit, d->key);
        serial_names l;
        list_serial(&l);
        snprintf(o->title, sizeof o->title, "%s", zr_t(d->label));
        for (unsigned i = 0; i < n; ++i) {
            /* The host's serial ports are offered when it has any (the one in use stays what it is). */
            const int found = types[i] != ZR_END_SERIAL || l.count || (now && now->type == ZR_END_SERIAL);
            char text[48];
            if (found) add_option(o, zr_t(texts[i]), 1);
            else {
                zr_text_format(text, sizeof text, ZR_T_V_NONE_FOUND, zr_t(texts[i]));
                add_option(o, text, 0);
            }
            if (types[i] == (now ? now->type : (uint32_t)ZR_END_NONE)) o->current = (int)i;
        }
        return;
    }
    const int stick = stick_of(id);
    if (stick >= 0) {
        const zr_config_stick *st = &s->edit.sticks[stick];
        char text[48];
        stick_title(s, (unsigned)stick, o->title, sizeof o->title);
        add_option(o, ZR_TXT(V_STICK_NONE), 1);
        add_option(o, ZR_TXT(V_STICK_KEYBOARD), 1);
        for (unsigned pad = 1; pad <= ZR_CONFIG_PADS; ++pad) {
            const char *name = pad_name(s, pad - 1);
            zr_text_format(text, sizeof text, ZR_T_V_STICK_CONTROLLER, pad);
            add_option(o, text, 1);
            if (name) zr_tui_clean(name, o->value[o->count - 1], sizeof o->value[0]);
        }
        o->current = st->device == ZR_STICK_KEYBOARD ? 1 : st->device == ZR_STICK_CONTROLLER ? 1 + (int)st->pad : 0;
        return;
    }
    if (id == ID_VOLUME) {
        int values[MENU_MAX];
        const int n = percent_list(0, 100, 10, s->edit.volume, values);
        snprintf(o->title, sizeof o->title, "%s", ZR_TXT(L_VOLUME));
        for (int i = 0; i < n; ++i) {
            char text[16];
            snprintf(text, sizeof text, "%d %%", values[i]);
            add_option(o, text, 1);
            if (values[i] == s->edit.volume) o->current = i;
        }
    }
}

/* Chooses the option `index` of the row `id`. */
static void apply_option(zr_setup *s, int id, int index) {
    options o;
    options_of(s, id, &o);
    if (index < 0 || index >= o.count || !o.enabled[index]) return;
    if (id == ID_MODEL) {
        const zr_family *f = zr_family_at((unsigned)o.family[index]);
        if (!f || !strcmp(s->edit.model, f->models[o.model[index]].id)) return;
        snprintf(s->edit.model, sizeof s->edit.model, "%s", f->models[o.model[index]].id);
        prune(&s->edit);
        s->dirty = 1;
        return;
    }
    const int opt = option_of(id);
    if (opt >= 0) {
        unsigned model;
        const zr_family *f = edit_family(s, &model);
        unsigned state = 0;
        if (f && (unsigned)opt < f->option_count) option_resolved(&s->edit, f, model, (unsigned)opt, &state);
        if ((state & ZR_OPTION_PRESENT) && !(state & ZR_OPTION_FORCED) && option_index(&s->edit, f, (unsigned)opt) != index) {
            zr_config_set_option(&s->edit, f->options[opt].key, f->options[opt].values[index]);
            s->dirty = 1;
        }
        return;
    }
    const choice *c = find_choice(id);
    if (c) {
        uint32_t *field = choice_field(s, c);
        if (*field != c->values[index]) {
            *field = c->values[index];
            s->dirty = 1;
            if (id == ID_LANGUAGE) zr_text_set_language(s->edit.language);       /* applied at once: the next frame is in it */
        }
        return;
    }
    const int port = port_of(id);
    if (port >= 0) {
        unsigned model;
        const zr_family *f = running_family(s, &model);
        if ((unsigned)port >= f->port_count) return;
        uint32_t types[ZR_END_KINDS];
        int texts[ZR_END_KINDS];
        const unsigned n = port_kinds(&f->ports[port], types, texts);
        zr_config_port *p = zr_config_port_slot(&s->edit, f->ports[port].key);
        if (!p || (unsigned)index >= n || p->type == types[index]) return;
        p->type = types[index];
        port_default(s, (unsigned)port, p->type, p->path, sizeof p->path);
        s->dirty = 1;
        return;
    }
    const int stick = stick_of(id);
    if (stick >= 0) {
        zr_config_stick want = {index == 0 ? (uint32_t)ZR_STICK_NONE : index == 1 ? (uint32_t)ZR_STICK_KEYBOARD : (uint32_t)ZR_STICK_CONTROLLER,
                                index >= 2 ? (uint32_t)index - 1u : 0u};
        zr_config_stick *st = &s->edit.sticks[stick];
        if (st->device == want.device && st->pad == want.pad) return;
        *st = want;
        /* A keyboard or a controller drives one socket: the other gives it up. */
        for (unsigned k = 0; k < ZR_CONFIG_STICKS; ++k) {
            zr_config_stick *other = &s->edit.sticks[k];
            if ((int)k != stick && want.device != ZR_STICK_NONE && other->device == want.device && other->pad == want.pad) other->device = ZR_STICK_NONE, other->pad = 0;
        }
        s->dirty = 1;
        return;
    }
    if (id == ID_VOLUME) {
        int values[MENU_MAX];
        const int n = percent_list(0, 100, 10, s->edit.volume, values);
        if (index < n && values[index] != s->edit.volume) { s->edit.volume = values[index]; s->dirty = 1; }
    }
}

/* Left or right on a choice: the next option that can be chosen. */
static void cycle(zr_setup *s, int id, int direction) {
    options o;
    options_of(s, id, &o);
    int index = o.current;
    for (int n = 0; n < o.count; ++n) {
        index = (index + direction + o.count) % o.count;
        if (o.enabled[index]) { apply_option(s, id, index); return; }
    }
}

/* --- pop-ups --------------------------------------------------------------------------------- */

static void tape_close(zr_setup *s) {
    free(s->tape_items);
    free(s->tape_names);
    s->tape_items = NULL;
    s->tape_names = NULL;
    s->tape_count = 0;
}

static void close_modal(zr_setup *s) {
    if (s->picker_open) { zr_tui_picker_close(&s->picker); s->picker_open = 0; }
    tape_close(s);
    s->modal = MODAL_NONE;
    s->tag = TAG_NONE;
}

static void open_menu(zr_setup *s, modal_tag tag, int arg, const options *o) {
    s->modal = MODAL_MENU;
    s->tag = tag;
    s->arg = arg;
    snprintf(s->menu_title, sizeof s->menu_title, "%s", o->title);
    for (int i = 0; i < o->count; ++i) {
        snprintf(s->menu_labels[i], sizeof s->menu_labels[0], "%s", o->label[i]);
        snprintf(s->menu_values[i], sizeof s->menu_values[0], "%s", o->value[i]);
        s->menu_items[i].kind = ZR_ITEM_TEXT;
        s->menu_items[i].enabled = o->enabled[i];
        s->menu_items[i].on = 0;
        s->menu_items[i].mark = 0;
        s->menu_items[i].label = s->menu_labels[i];
        s->menu_items[i].value = s->menu_values[i];
    }
    memset(&s->menu, 0, sizeof s->menu);
    const int shown = o->count < POPUP_BOTTOM - POPUP_TOP - 1 ? o->count : POPUP_BOTTOM - POPUP_TOP - 1;
    zr_tui_list_set(&s->menu, s->menu_items, (size_t)o->count, shown);
    zr_tui_list_select(&s->menu, (size_t)o->current);
}

static void open_dialog(zr_setup *s, modal_tag tag, int arg, const char *title, const char *text, const char *buttons, int selected, int cancel) {
    s->modal = MODAL_DIALOG;
    s->tag = tag;
    s->arg = arg;
    zr_tui_dialog_set(&s->dialog, title, text, buttons, selected, cancel);
}

static void note(zr_setup *s, const char *title, const char *text) { open_dialog(s, TAG_NOTE, 0, title, text, ZR_TXT(BUTTON_OK), 0, 0); }

static int open_picker(zr_setup *s, modal_tag tag, int arg, const char *title, const char *folder, const char *filter) {
    const zr_tui_fs fs = host_fs();
    s->tag = tag;
    s->arg = arg;
    s->modal = MODAL_PICKER;
    s->picker_open = zr_tui_picker_open(&s->picker, &fs, title, folder, filter);
    if (!s->picker_open) { close_modal(s); note(s, ZR_TXT(D_NO_MEMORY), ZR_TXT(D_NO_MEMORY)); return 0; }
    return 1;
}

/* --- saving and leaving --------------------------------------------------------------------- */

int zr_setup_write(const zr_config *config, char *error, size_t capacity) {
    if (!config->source[0]) return 1;
    zr_ydoc doc;
    memset(&doc, 0, sizeof doc);
    if (!zr_ydoc_load(&doc, config->source)) { snprintf(error, capacity, "Cannot read %s", config->source); return 0; }
    int ok = zr_config_store(config, &doc);
    /* The media slots the file has that the configuration no longer does (another model's) go, as the options do. */
    static char keys[ZR_CONFIG_MEDIA * 2][128];
    const char *media[1] = {"media"};
    const size_t had = ok ? zr_ydoc_keys(&doc, media, 1, keys, ZR_CONFIG_MEDIA * 2) : 0;
    for (size_t i = 0; ok && i < had; ++i)
        if (!zr_config_media_find(config, keys[i])) {
            const char *path[2] = {"media", keys[i]};
            ok = zr_ydoc_remove(&doc, path, 2);
        }
    ok = ok && zr_ydoc_save(&doc, config->source);
    zr_ydoc_free(&doc);
    if (!ok) snprintf(error, capacity, "Cannot write %s", config->source);
    return ok;
}

/* The changes made since the last write go to the file. */
static void persist(zr_setup *s) {
    if (!s->dirty) return;
    s->dirty = 0;
    char error[ZR_CONFIG_PATH + 64] = "", text[ZR_CONFIG_PATH + 80];
    zr_config out = s->edit;
    if (!s->fullscreen_touched) out.fullscreen = s->saved_fullscreen;
    if (zr_setup_write(&out, error, sizeof error)) return;
    zr_text_format(text, sizeof text, ZR_T_D_CANNOT_WRITE_FILE, s->edit.source);
    note(s, ZR_TXT(D_CANNOT_SAVE), text);
}

static void finish(zr_setup *s) {
    close_modal(s);
    persist(s);
    if (s->modal != MODAL_NONE) return;                 /* the file could not be written: the message first */
    s->done = 1;
    s->exit = s->want_exit;
}

void zr_setup_request_close(zr_setup *s) {
    s->key_wait = 0;
    close_modal(s);
    if (zr_setup_diff(&s->running, &s->edit) & ZR_CHANGE_RESTART)
        open_dialog(s, TAG_LEAVE, 0, ZR_TXT(D_LEAVE_TITLE), ZR_TXT(D_LEAVE_TEXT), ZR_TXT(D_LEAVE_BUTTONS), 0, 1);
    else finish(s);
}

void zr_setup_request_exit(zr_setup *s) {
    if (s->done) return;
    s->want_exit = 1;
    finish(s);                          /* the settings are saved as leaving does, with no question: nothing is rebuilt */
    if (!s->done) s->want_exit = 0;     /* the file could not be written: the message first, the program goes on */
}

/* --- the rows of the pages ----------------------------------------------------------------------- */

static zr_tui_item *add_row(zr_setup *s, int kind, int id, int enabled, const char *label, const char *hint) {
    if (s->rows >= MAX_ROWS) return &s->items[MAX_ROWS - 1];
    const size_t i = s->rows++;
    s->ids[i] = id;
    copy_text(s->labels[i], sizeof s->labels[i], label);
    s->values[i][0] = 0;
    copy_text(s->hints[i], sizeof s->hints[i], hint ? hint : "");
    s->items[i].kind = kind;
    s->items[i].enabled = enabled;
    s->items[i].on = 0;
    s->items[i].mark = 0;
    s->items[i].label = s->labels[i];
    s->items[i].value = s->values[i];
    return &s->items[i];
}

static void add_gap(zr_setup *s) { add_row(s, ZR_ITEM_INFO, 0, 1, "", NULL); }

/* The value of a row, as cells (the string table's texts are; a name from outside goes through zr_tui_clean first). */
static void set_value(zr_setup *s, zr_tui_item *item, const char *text) {
    const size_t i = (size_t)(item - s->items);
    copy_text(s->values[i], sizeof s->values[i], text);
}

static void add_choice(zr_setup *s, int id, const char *label, int enabled, const char *hint) {
    options o;
    options_of(s, id, &o);
    zr_tui_item *item = add_row(s, ZR_ITEM_CHOICE, id, enabled, label, hint);
    set_value(s, item, o.current >= 0 && o.current < o.count ? o.label[o.current] : "?");
}

static zr_tui_item *add_toggle(zr_setup *s, int id, int on, const char *label, int enabled, const char *hint) {
    zr_tui_item *item = add_row(s, ZR_ITEM_TOGGLE, id, enabled, label, hint);
    item->on = on != 0;
    return item;
}

static int restart_pending(const zr_setup *s) { return (zr_setup_diff(&s->running, &s->edit) & ZR_CHANGE_RESTART) != 0; }

/* The media of the running machine's model, each slot with its file, the keys of its settings and, for a tape, the browser. */
static void rows_media(zr_setup *s) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    const int pending = restart_pending(s);
    int first = 1;
    for (unsigned i = 0; i < f->media_count && i < ZR_CONFIG_MEDIA; ++i) {
        const zr_media_desc *d = &f->media[i];
        if (!zr_machine_has_media(s->env.machine, i)) continue;         /* a drive, a card the machine as built does not have */
        const int base = ID_SLOT + 10 * (int)i;
        const zr_media *loaded = zr_machine_media(s->env.machine, i);
        const char *hint;
        const zr_config_media settings = media_settings(&s->edit, d->key);
        const char *image = settings.image;
        const int tape = d->kind == ZR_MEDIA_TAPE;
        if (!first) add_gap(s);
        if (first && pending) add_row(s, ZR_ITEM_INFO, 0, 1, ZR_TXT(V_AFTER_RESTART), NULL);
        first = 0;
        add_row(s, ZR_ITEM_HEADING, base + PART_HEAD, 1, zr_t(d->label), NULL);
        hint = pending ? ZR_TXT(HINT_PENDING) : NULL;
        char shown[ZR_CONFIG_PATH], name[ZR_CONFIG_PATH];
        if (loaded) {
            zr_tui_clean(loaded->path, shown, sizeof shown);
            zr_tui_clean(leaf(loaded->path), name, sizeof name);
        }
        zr_tui_item *file = add_row(s, ZR_ITEM_TEXT, base + PART_FILE, !pending, ZR_TXT(L_FILE), hint ? hint : loaded ? shown : ZR_TXT(HINT_FILE));
        set_value(s, file, loaded ? name : ZR_TXT(V_EMPTY));
        add_row(s, ZR_ITEM_ACTION, base + PART_EJECT, (loaded || *image) && !pending, ZR_TXT(L_EJECT),
            hint ? hint : loaded || *image ? ZR_TXT(HINT_EJECT) : ZR_TXT(HINT_EMPTY));
        add_toggle(s, base + PART_PROTECT, settings.write_protect, ZR_TXT(L_PROTECT), !pending, hint ? hint : ZR_TXT(HINT_PROTECT));
        add_toggle(s, base + PART_FAST, settings.fast, tape ? ZR_TXT(L_FAST_TAPE) : ZR_TXT(L_FAST), !pending,
                   hint ? hint : tape ? ZR_TXT(HINT_FAST_TAPE) : ZR_TXT(HINT_FAST))->mark = '*';
        /* A tape recorder's own settings, where the machine has a use for them: whether the tape is heard, whether a tape
         * put in is loaded at once (a model with a load command to type). */
        if (tape && (zr_machine_tape_options(s->env.machine, i, ZR_TAPE_QUERY) & ZR_TAPE_SOUND))
            add_toggle(s, base + PART_SOUND, settings.sound, ZR_TXT(L_TAPE_SOUND), !pending, hint ? hint : ZR_TXT(HINT_TAPE_SOUND));
        if (tape && f->models[model].autoload)
            add_toggle(s, base + PART_AUTOLOAD, settings.autoload, ZR_TXT(L_AUTOLOAD), !pending, hint ? hint : ZR_TXT(HINT_AUTOLOAD))->mark = '*';
        if (d->kind == ZR_MEDIA_CARD)
            add_row(s, ZR_ITEM_ACTION, base + PART_NEW, !pending, ZR_TXT(L_NEW_CARD), hint ? hint : ZR_TXT(HINT_NEW_CARD));
        if (d->kind == ZR_MEDIA_TAPE) {
            struct zr_tape_deck *deck = zr_machine_tape_deck(s->env.machine, i);
            add_row(s, ZR_ITEM_ACTION, base + PART_BROWSE, deck != NULL && deck->tape != NULL && !pending, ZR_TXT(L_TAPE_BLOCKS),
                    hint ? hint : deck ? ZR_TXT(HINT_TAPE_BLOCKS) : ZR_TXT(HINT_EMPTY));
            const int recording = deck && zr_tape_deck_recording(deck);
            add_row(s, ZR_ITEM_ACTION, base + PART_RECORD, !pending && s->env.record != NULL, recording ? ZR_TXT(L_RECORD_STOP) : ZR_TXT(L_RECORD),
                    hint ? hint : recording ? ZR_TXT(HINT_RECORD_STOP) : ZR_TXT(HINT_RECORD));
        }
    }
}

/* The rows of the edited model's options that the model has now (its family's conditions), those of the byte ports (`ports` 1)
 * or the others (0). An option whose value another setting forces shows that value and cannot be changed. Returns how many. */
static unsigned rows_options(zr_setup *s, int ports) {
    unsigned model, values[ZR_CONFIG_OPTIONS], count = 0;
    uint8_t state[ZR_CONFIG_OPTIONS];
    const zr_family *f = edit_family(s, &model);
    if (!f) return 0;
    zr_family_resolve(f, model, &s->edit, values, state);
    for (unsigned i = 0; i < f->option_count && i < ZR_CONFIG_OPTIONS; ++i) {
        const zr_option_desc *d = &f->options[i];
        if (!(state[i] & ZR_OPTION_PRESENT) || ((d->flags & ZR_OPTION_PORT) != 0) != (ports != 0)) continue;
        const int original = !(d->flags & ZR_OPTION_NOT_ORIGINAL), restarts = (d->flags & ZR_OPTION_RESET) != 0;
        const int forced = (state[i] & ZR_OPTION_FORCED) != 0;
        const char *hint = forced ? ZR_TXT(HINT_FORCED) : d->hint ? zr_t(d->hint - 1)
                           : !original ? (restarts ? ZR_TXT(HINT_NOT_ORIGINAL_RESTARTS) : ZR_TXT(HINT_NOT_ORIGINAL))
                           : restarts ? ZR_TXT(HINT_RESTARTS) : is_switch(d) ? ZR_TXT(HINT_TOGGLE) : ZR_TXT(HINT_CHOICE);
        zr_tui_item *item;
        if (is_switch(d)) item = add_toggle(s, ID_OPTION + (int)i, values[i] == 1, zr_t(d->label), !forced, hint);
        else {
            add_choice(s, ID_OPTION + (int)i, zr_t(d->label), !forced, hint);
            item = &s->items[s->rows - 1];
        }
        if (!original) item->mark = '*';
        ++count;
    }
    return count;
}

/* The model, its options as its family's table gives them, and the machine's own actions. */
static void rows_machine(zr_setup *s) {
    unsigned model;
    const zr_family *f = edit_family(s, &model);
    const int missing = f && roms_missing(s, &f->models[model]);
    add_choice(s, ID_MODEL, ZR_TXT(L_MODEL), 1, missing ? ZR_TXT(HINT_MODEL_NO_ROMS) : ZR_TXT(HINT_MODEL));
    rows_options(s, 0);
    add_gap(s);
    add_row(s, ZR_ITEM_ACTION, ID_RESET, 1, ZR_TXT(L_RESET), ZR_TXT(HINT_RESET));
    add_row(s, ZR_ITEM_ACTION, ID_POWER, 1, ZR_TXT(L_POWER), ZR_TXT(HINT_POWER));
    add_row(s, ZR_ITEM_ACTION, ID_EXIT, 1, ZR_TXT(L_EXIT), ZR_TXT(HINT_EXIT));
}

static void rows_display(zr_setup *s) {
    add_choice(s, ID_LOOK, ZR_TXT(L_LOOK), 1, ZR_TXT(HINT_LOOK));
    add_choice(s, ID_ASPECT, ZR_TXT(L_ASPECT), 1, ZR_TXT(HINT_ASPECT));
    add_choice(s, ID_SCALE, ZR_TXT(L_SCALE), 1, ZR_TXT(HINT_SCALE));
    add_choice(s, ID_BORDER, ZR_TXT(L_BORDER), 1, ZR_TXT(HINT_BORDER));
    /* The CRT shader smooths the picture itself whatever the filter says (video.c): the row acts for the plain look only, and
     * says why in its place (a dimmed row cannot be selected, so its hint is not seen: the value is). */
    const int crt = s->edit.look == ZR_LOOK_CRT && !s->env.no_crt;
    add_choice(s, ID_FILTER, ZR_TXT(L_FILTER), !crt, crt ? ZR_TXT(HINT_FILTER_CRT) : ZR_TXT(HINT_FILTER));
    if (crt) set_value(s, &s->items[s->rows - 1], ZR_TXT(V_FILTER_CRT));
    add_toggle(s, ID_FULLSCREEN, s->edit.fullscreen, ZR_TXT(L_FULLSCREEN), 1, ZR_TXT(HINT_FULLSCREEN));
    add_toggle(s, ID_VSYNC, s->edit.vsync, ZR_TXT(L_VSYNC), 1, ZR_TXT(HINT_VSYNC));
    add_gap(s);
    add_choice(s, ID_LANGUAGE, ZR_TXT(L_LANGUAGE), 1, ZR_TXT(HINT_LANGUAGE));
}

static void rows_sound(zr_setup *s) {
    add_toggle(s, ID_SOUND, s->edit.sound, ZR_TXT(L_SOUND), 1, ZR_TXT(HINT_SOUND));
    add_choice(s, ID_VOLUME, ZR_TXT(L_VOLUME), s->edit.sound, s->edit.sound ? ZR_TXT(HINT_VOLUME) : ZR_TXT(HINT_SOUND_OFF));
}

/* The joystick sockets of the running machine, each with what feeds it; the keys of the keyboard joystick. */
static void rows_controllers(zr_setup *s) {
    unsigned sockets = 0;
    int keyboard = 0, shown = 0;
    for (unsigned j = 0; j < joy_sockets(s); ++j)
        if (socket_fitted(s, j)) {
            ++sockets;
            keyboard |= s->edit.sticks[j].device == ZR_STICK_KEYBOARD;
        }
    if (!sockets) add_row(s, ZR_ITEM_INFO, 0, 1, ZR_TXT(V_NO_JOYSTICK), NULL);
    for (unsigned j = 0; j < joy_sockets(s); ++j) {
        if (!socket_fitted(s, j)) continue;                             /* an empty socket has nothing to drive */
        const zr_config_stick *st = &s->edit.sticks[j];
        char title[48], hint[96];
        stick_title(s, j, title, sizeof title);
        if (shown++) add_gap(s);
        add_row(s, ZR_ITEM_HEADING, ID_STICK_KEY - 1, 1, title, NULL);
        if (st->device == ZR_STICK_CONTROLLER) {
            const char *name = pad_name(s, st->pad - 1);
            char clean[64];
            if (name) {
                zr_tui_clean(name, clean, sizeof clean);
                zr_text_format(hint, sizeof hint, ZR_T_HINT_PAD, (unsigned)st->pad, clean);
            } else zr_text_format(hint, sizeof hint, ZR_T_HINT_PAD_ABSENT, (unsigned)st->pad);
        } else snprintf(hint, sizeof hint, "%s", ZR_TXT(HINT_DEVICE));
        add_choice(s, ID_STICK + (int)j, ZR_TXT(L_DEVICE), 1, hint);
    }
    if (sockets) {
        add_gap(s);
        add_row(s, ZR_ITEM_HEADING, ID_STICK_KEY - 2, 1, ZR_TXT(HEAD_KEYBOARD_STICK), NULL);
        for (int f = 0; f < ZR_CONFIG_STICK_KEYS; ++f) {
            const int waiting = s->key_wait == f + 1;
            char key[ZR_CONFIG_KEY_NAME * 2];
            zr_tui_clean(s->edit.stick_keys[f], key, sizeof key);
            zr_tui_item *row = add_row(s, ZR_ITEM_TEXT, ID_STICK_KEY + f, keyboard, zr_t(KEY_FUNCTIONS[f]),
                                       !keyboard ? ZR_TXT(HINT_KEYS_NEEDS) : waiting ? ZR_TXT(HINT_KEY_WAIT) : ZR_TXT(HINT_KEY));
            set_value(s, row, waiting ? ZR_TXT(V_KEY_WAIT) : key);
        }
        add_toggle(s, ID_KEYBOARD_START, s->edit.keyboard_joystick, ZR_TXT(L_KEYBOARD_START), 1, ZR_TXT(HINT_KEYBOARD_START));
    }
}

/* The byte ports of the running machine's model, each with the host end it is connected to. */
static void rows_ports(zr_setup *s) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    const int pending = restart_pending(s);
    int any = 0;
    if (pending) add_row(s, ZR_ITEM_INFO, 0, 1, ZR_TXT(V_AFTER_RESTART), NULL);
    for (unsigned i = 0; i < f->port_count && i < ZR_CONFIG_PORTS; ++i) {
        const zr_port_desc *d = &f->ports[i];
        if (!zr_machine_has_port(s->env.machine, i)) continue;
        any = 1;
        const zr_config_port *p = zr_config_port_find(&s->edit, d->key);
        char text[ZR_CONFIG_PATH + 16], shown[ZR_CONFIG_PATH + 16], hint[96];
        if (p) zr_config_port_text(&s->edit, p, text, sizeof text);
        else snprintf(text, sizeof text, "none");
        zr_tui_clean(text, shown, sizeof shown);
        const char *error = s->env.port_error ? s->env.port_error(s->env.ctx, i) : NULL;
        if (pending) snprintf(hint, sizeof hint, "%s", ZR_TXT(HINT_PENDING));
        else if (error) zr_tui_clean(error, hint, sizeof hint);
        else if (p && p->type != ZR_END_NONE) snprintf(hint, sizeof hint, "%s", shown);
        else snprintf(hint, sizeof hint, "%s", ZR_TXT(HINT_PORT));
        zr_tui_item *item = add_row(s, ZR_ITEM_CHOICE, ID_PORT + (int)i, !pending, zr_t(d->label), hint);
        set_value(s, item, shown);
    }
    if (!any) add_row(s, ZR_ITEM_INFO, 0, 1, ZR_TXT(V_NO_PORTS), NULL);
    /* The ports' own settings (the speed, the wiring): options of the machine, kept here with what they belong to. */
    add_gap(s);
    if (!rows_options(s, 1)) --s->rows;
}

/* --- which pages there are ------------------------------------------------------------------------------------------- */

/* A page the running machine has nothing for is not among the pages: Cheats on a machine that takes no .pok files (the ones that
 * take no snapshots), Ports on one with no byte port fitted and no port setting, Controllers on one with no joystick. */
static int page_shown(const zr_setup *s, int page) {
    if (page == ZR_SETUP_CHEATS) return zr_machine_takes_snapshots(s->env.machine);
    if (page == ZR_SETUP_CONTROLLERS) {                 /* a joystick in a socket */
        for (unsigned j = 0; j < joy_sockets(s); ++j) if (socket_fitted(s, j)) return 1;
        return 0;
    }
    if (page == ZR_SETUP_PORTS) {
        const zr_family *f = zr_machine_family(s->env.machine);
        for (unsigned i = 0; i < f->port_count && i < ZR_CONFIG_PORTS; ++i) if (zr_machine_has_port(s->env.machine, i)) return 1;
        unsigned model, values[ZR_CONFIG_OPTIONS];
        uint8_t state[ZR_CONFIG_OPTIONS];
        const zr_family *e = zr_family_of_model(s->edit.model, &model);
        if (!e) return 0;
        zr_family_resolve(e, model, &s->edit, values, state);
        for (unsigned i = 0; i < e->option_count && i < ZR_CONFIG_OPTIONS; ++i)
            if ((state[i] & ZR_OPTION_PRESENT) && (e->options[i].flags & ZR_OPTION_PORT)) return 1;
        return 0;
    }
    return 1;
}

/* --- the States page ------------------------------------------------------------------------------------------------- */

static int state_of(int id) { return id > ID_STATE && id <= ID_STATE + ZR_STATES_SLOTS ? id - ID_STATE : 0; }

/* The file of a slot of the running model, beside the configuration. */
static void slot_path(const zr_setup *s, unsigned slot, char *out, size_t capacity) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    zr_states_slot_path(s->edit.source, f->models[model].id, slot, out, capacity);
}

/* What the files of the slots say, read again. */
static void read_slots(zr_setup *s) {
    if (!s->slot_info) return;
    for (unsigned i = 0; i < ZR_STATES_SLOTS; ++i) {
        char path[ZR_CONFIG_PATH + 64];
        slot_path(s, i + 1, path, sizeof path);
        s->slot_full[i] = zr_states_slot_info(path, &s->slot_info[i]);
    }
    s->slots_read = 1;
}

/* The ten slots of the running model: each with the date and time it was saved, the current one (F2 and F3) marked. */
static void rows_states(zr_setup *s) {
    if (!s->slots_read) read_slots(s);
    const int pending = restart_pending(s);
    for (unsigned i = 0; i < ZR_STATES_SLOTS; ++i) {
        char label[32], when[32];
        zr_text_format(label, sizeof label, ZR_T_L_SLOT, i + 1);
        const int current = s->edit.state_slot == (int)i + 1;
        zr_tui_item *row = add_row(s, ZR_ITEM_TEXT, ID_STATE + (int)i + 1, 1, label,
                                   pending ? ZR_TXT(HINT_PENDING) : current ? ZR_TXT(HINT_SLOT_CURRENT) : ZR_TXT(HINT_SLOT));
        const zr_states_info *info = s->slot_info ? &s->slot_info[i] : NULL;
        if (!info || !s->slot_full[i]) snprintf(when, sizeof when, "%s", ZR_TXT(V_EMPTY));
        else if (info->has_time) zr_states_time_text(&info->time, when, sizeof when);
        else snprintf(when, sizeof when, "?");
        set_value(s, row, when);
        if (current) row->mark = ZR_GLYPH_CHAR(ZR_GLYPH_POINT_RIGHT);
    }
    /* Snapshot files, for a machine that takes them: the list scrolls to them under the slots. */
    if (s->env.snapshot_load && s->env.snapshot_save && zr_machine_takes_snapshots(s->env.machine)) {
        add_row(s, ZR_ITEM_ACTION, ID_SNAP_LOAD, !pending, ZR_TXT(L_SNAP_LOAD), pending ? ZR_TXT(HINT_PENDING) : ZR_TXT(HINT_SNAP_LOAD));
        add_row(s, ZR_ITEM_ACTION, ID_SNAP_SAVE, !pending, ZR_TXT(L_SNAP_SAVE), pending ? ZR_TXT(HINT_PENDING) : ZR_TXT(HINT_SNAP_SAVE));
    }
}

/* --- the Cheats page ------------------------------------------------------------------------------------------------- */

static int cheat_of(int id) { return id >= ID_CHEAT && id < ID_CHEAT + 1000 ? id - ID_CHEAT : -1; }

/* The Load cheat file action, the file's name (or that there is none) and the trainers, a switch each; one the file or the machine
 * spoiled is a plain row that says so when Enter is pressed. */
static void rows_cheats(zr_setup *s) {
    const zr_cheats *c = s->env.cheats;
    const size_t count = c ? zr_cheats_count(c) : 0;
    add_row(s, ZR_ITEM_ACTION, ID_CHEATS_LOAD, s->env.cheats_load != NULL, ZR_TXT(L_CHEATS_LOAD), ZR_TXT(HINT_CHEATS_LOAD));
    if (!count) {
        add_row(s, ZR_ITEM_INFO, 0, 1, ZR_TXT(V_NO_CHEATS), NULL);
        return;
    }
    char name[ZR_MEDIA_PATH];
    zr_tui_clean(leaf(c->file), name, sizeof name);
    set_value(s, add_row(s, ZR_ITEM_INFO, 0, 1, ZR_TXT(L_FILE), NULL), name);
    add_gap(s);
    for (size_t i = 0; i < count && s->rows + 1 < MAX_ROWS; ++i) {
        const zr_cheat *ch = &c->cheat[i];
        char label[64], hint[96], problem[96], value[24];
        zr_tui_clean(zr_cheats_name(c, i), label, sizeof label);
        if (!ch->usable) {
            zr_tui_clean(ch->problem, problem, sizeof problem);
            zr_text_format(hint, sizeof hint, ZR_T_HINT_CHEAT_BAD, ch->line, problem);
            zr_tui_item *row = add_row(s, ZR_ITEM_TEXT, ID_CHEAT + (int)i, 1, label, hint);
            row->mark = '!';
            zr_text_format(value, sizeof value, ZR_T_V_CHEAT_LINE, ch->line);
            set_value(s, row, value);
            continue;
        }
        zr_tui_item *row = add_toggle(s, ID_CHEAT + (int)i, ch->on, label, 1, ch->asks ? ZR_TXT(HINT_CHEAT_ASKS) : ZR_TXT(HINT_CHEAT));
        if (ch->on && ch->asks) {
            snprintf(value, sizeof value, "%u", ch->value);
            set_value(s, row, value);
        }
    }
}

/* Enter on a slot: it becomes the current one, and a menu offers what can be done with it. */
static void open_slot_menu(zr_setup *s, unsigned slot) {
    if (s->edit.state_slot != (int)slot) { s->edit.state_slot = (int)slot; s->dirty = 1; }
    const int pending = restart_pending(s), full = slot >= 1 && slot <= ZR_STATES_SLOTS && s->slot_full[slot - 1];
    options o;
    memset(&o, 0, sizeof o);
    zr_text_format(o.title, sizeof o.title, ZR_T_L_SLOT, slot);
    add_option(&o, ZR_TXT(M_SAVE_HERE), !pending && s->env.slot_save != NULL);
    add_option(&o, ZR_TXT(M_LOAD), full && !pending && s->env.slot_load != NULL);
    add_option(&o, ZR_TXT(M_DELETE), full);
    o.current = full && !pending ? 1 : 0;
    open_menu(s, TAG_SLOT, (int)slot, &o);
}

static void slot_action(zr_setup *s, unsigned slot, int action) {
    char error[256] = "", shown[256], text[96];
    if (action == 0 && s->env.slot_save) {
        if (!s->env.slot_save(s->env.ctx, slot, error, sizeof error)) {
            zr_tui_clean(error, shown, sizeof shown);
            note(s, ZR_TXT(D_CANNOT_SAVE), shown);
        }
        s->slots_read = 0;
    } else if (action == 1 && s->env.slot_load) {
        if (s->env.slot_load(s->env.ctx, slot, error, sizeof error)) { finish(s); return; }
        zr_tui_clean(error, shown, sizeof shown);
        note(s, ZR_TXT(D_CANNOT_LOAD), shown);
    } else if (action == 2) {
        zr_text_format(text, sizeof text, ZR_T_D_DELETE_TEXT, slot);
        open_dialog(s, TAG_DELETE, (int)slot, ZR_TXT(D_DELETE_TITLE), text, ZR_TXT(D_DELETE_BUTTONS), 1, 1);
    }
}

static void delete_slot(zr_setup *s, unsigned slot) {
    char path[ZR_CONFIG_PATH + 64], text[ZR_CONFIG_PATH + 96];
    slot_path(s, slot, path, sizeof path);
    if (!zr_host_delete_file(path)) {
        zr_text_format(text, sizeof text, ZR_T_D_CANNOT_WRITE_FILE, path);
        note(s, ZR_TXT(D_CANNOT_SAVE), text);
    }
    s->slots_read = 0;
}

/* The selected slot's thumbnail in its box under the list (after the cells are drawn into pixels): each of its colours as
 * the nearest of the screen's fifteen. */
static void draw_thumbnail(zr_setup *s) {
    const int id = s->rows && s->list.selected < s->rows ? s->ids[s->list.selected] : 0;
    const int slot = state_of(id);
    if (s->page != ZR_SETUP_STATES || s->modal != MODAL_NONE || !slot || !s->slot_info || !s->slot_full[slot - 1]) return;
    const zr_states_thumb *t = &s->slot_info[slot - 1].thumb;
    if (!s->slot_info[slot - 1].has_thumb) return;
    uint8_t map[256];
    for (uint32_t i = 0; i < t->colors; ++i) {
        uint32_t best = 0, best_d = 0xFFFFFFFFu;
        for (uint32_t c = 0; c < 16; ++c) {
            const int dr = (int)(t->palette[i] >> 16 & 0xFFu) - (int)(zr_tui_palette[c] >> 16 & 0xFFu);
            const int dg = (int)(t->palette[i] >> 8 & 0xFFu) - (int)(zr_tui_palette[c] >> 8 & 0xFFu);
            const int db = (int)(t->palette[i] & 0xFFu) - (int)(zr_tui_palette[c] & 0xFFu);
            const uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 4 + db * db * 2);
            if (d < best_d) { best_d = d; best = c; }
        }
        map[i] = (uint8_t)best;
    }
    const int x0 = ZR_TUI_BORDER_X + (THUMB_COL + 1) * 8 + (ZR_STATES_THUMB_W - (int)t->width) / 2;
    const int y0 = ZR_TUI_BORDER_Y + (THUMB_ROW + 1) * 8 + (ZR_STATES_THUMB_H - (int)t->height) / 2;
    for (uint32_t y = 0; y < t->height; ++y)
        for (uint32_t x = 0; x < t->width; ++x)
            s->tui.pixels[(size_t)(y0 + (int)y) * ZR_TUI_WIDTH + (size_t)x0 + x] = map[t->pixels[y * t->width + x]];
}

/* The box the thumbnail is drawn in, with the selected slot's name; an empty slot says so inside it. */
static void draw_states_box(zr_setup *s) {
    zr_tui *t = &s->tui;
    const int id = s->rows && s->list.selected < s->rows ? s->ids[s->list.selected] : 0;
    const int slot = state_of(id);
    const zr_tui_rect r = zr_tui_rect_make(THUMB_ROW, THUMB_COL, ZR_STATES_THUMB_H / 8 + 2, THUMB_COLS);
    const int dim = s->modal != MODAL_NONE;
    const uint8_t frame = dim ? ZR_ATTR(ZR_BLUE, ZR_BLACK, 0) : ZR_ATTR(ZR_CYAN, ZR_BLACK, 0);
    zr_tui_fill(t, r, ' ', ZR_ATTR(ZR_WHITE, ZR_BLACK, 0));
    if (!slot) return;                  /* a snapshot row is selected: no slot, no box */
    zr_tui_box(t, r, frame);
    char title[32];
    zr_text_format(title, sizeof title, ZR_T_L_SLOT, (unsigned)slot);
    zr_tui_put_fit(t, r.row, r.col + 2, (int)strlen(title), dim ? frame : ZR_ATTR(ZR_WHITE, ZR_BLACK, 1), title, 0);
    const int full = s->slot_info && s->slot_full[slot - 1];
    if (!full || !s->slot_info[slot - 1].has_thumb)
        zr_tui_put_fit(t, r.row + r.rows / 2, r.col + 1, r.cols - 2, dim ? frame : ZR_ATTR(ZR_WHITE, ZR_BLACK, 0),
                       full ? ZR_TXT(V_NO_PICTURE) : ZR_TXT(V_EMPTY), ZR_FIT_CENTER);
}

/* The About page: the logo's white panel (its pixels go over the cells once they are rendered: draw_logo), the version, and
 * what is new in it (the text table's list). */
static void draw_about(zr_setup *s) {
    zr_tui *t = &s->tui;
    char line[128];
    zr_tui_fill(t, zr_tui_rect_make(ABOUT_LOGO_ROW, 0, ZR_LOGO_H / 8, ZR_TUI_COLS), ' ', ZR_ATTR(ZR_BLACK, ZR_WHITE, 1));
    zr_text_format(line, sizeof line, ZR_T_ABOUT_VERSION, s->env.version ? s->env.version : "");
    zr_tui_put_fit(t, ABOUT_VERSION_ROW, 0, ZR_TUI_COLS, ZR_ATTR(ZR_WHITE, ZR_BLACK, 1), line, ZR_FIT_CENTER);
    zr_tui_put(t, ABOUT_NEW_ROW, 1, ZR_ATTR(ZR_CYAN, ZR_BLACK, 1), ZR_TXT(ABOUT_NEW));
    int row = ABOUT_NEW_ROW + 1;
    for (const char *p = ZR_TXT(ABOUT_CHANGES); *p && row < LIST_ROW + LIST_ROWS;) {
        const char *end = strchr(p, '|');
        const size_t n = end ? (size_t)(end - p) : strlen(p);
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        zr_tui_put(t, row, 1, ZR_ATTR(ZR_YELLOW, ZR_BLACK, 1), "-");
        row += zr_tui_put_wrapped(t, row, 3, ZR_TUI_COLS - 4, LIST_ROW + LIST_ROWS - row, ZR_ATTR(ZR_WHITE, ZR_BLACK, 0), line);
        p += n + (end != NULL);
    }
}

/* The logo's pixels over its panel (not behind a pop-up: the page is dimmed then). The logo is in its own colours, with the
 * shades of its smoothed edges, which the screen's sixteen do not have: the page's picture carries the logo's palette after
 * them, and the logo's pixels are numbered from there. */
static uint32_t about_palette[16 + ZR_LOGO_COLORS];
static void draw_logo(zr_setup *s, zr_frame *frame) {
    if (s->page != ZR_SETUP_ABOUT || s->modal != MODAL_NONE) return;
    memcpy(about_palette, zr_tui_palette, 16 * sizeof *about_palette);
    memcpy(about_palette + 16, zr_logo_palette, sizeof zr_logo_palette);
    for (int y = 0; y < ZR_LOGO_H; ++y) {
        uint8_t *out = &s->tui.pixels[(size_t)(ZR_TUI_BORDER_Y + ABOUT_LOGO_ROW * 8 + y) * ZR_TUI_WIDTH + ZR_TUI_BORDER_X];
        for (int x = 0; x < ZR_LOGO_W; ++x) out[x] = (uint8_t)(16 + zr_logo_pixels[y * ZR_LOGO_W + x]);
    }
    frame->palette = about_palette;
    frame->colors = 16 + ZR_LOGO_COLORS;
}

static void remember(zr_setup *s) {
    if (s->rows && s->list.selected < s->rows) s->selected_id[s->page] = s->ids[s->list.selected];
}

static void rebuild(zr_setup *s) {
    remember(s);                        /* the row selected now, whatever moved it */
    s->rows = 0;
    switch (s->page) {
    case ZR_SETUP_MEDIA: rows_media(s); break;
    case ZR_SETUP_MACHINE: rows_machine(s); break;
    case ZR_SETUP_DISPLAY: rows_display(s); break;
    case ZR_SETUP_SOUND: rows_sound(s); break;
    case ZR_SETUP_PORTS: rows_ports(s); break;
    case ZR_SETUP_STATES: rows_states(s); break;
    case ZR_SETUP_CHEATS: rows_cheats(s); break;
    case ZR_SETUP_ABOUT: break;                         /* no rows: the page is drawn (draw_about) */
    default: rows_controllers(s); break;
    }
    zr_tui_list_set(&s->list, s->items, s->rows, s->page == ZR_SETUP_STATES ? STATE_ROWS : LIST_ROWS);
    for (size_t i = 0; i < s->rows; ++i)
        if (s->ids[i] == s->selected_id[s->page] && s->selected_id[s->page]) zr_tui_list_select(&s->list, i);
    remember(s);
}

/* Opens a page; one the machine has nothing for gives way to the next in the direction `step` (1 or -1). The page is remembered
 * in the configuration (ui.setup_page), so the screen opens on it the next time. */
static void set_page(zr_setup *s, int page, int step) {
    remember(s);
    page = (page + ZR_SETUP_PAGES) % ZR_SETUP_PAGES;
    for (int n = 0; n < ZR_SETUP_PAGES && !page_shown(s, page); ++n) page = (page + step + ZR_SETUP_PAGES) % ZR_SETUP_PAGES;
    s->page = page;
    if (strcmp(s->edit.setup_page, PAGE_NAMES[page])) {
        snprintf(s->edit.setup_page, sizeof s->edit.setup_page, "%s", PAGE_NAMES[page]);
        s->dirty = 1;
    }
    if (s->page == ZR_SETUP_STATES) {
        s->slots_read = 0;                                  /* the files as they are now */
        if (!s->selected_id[s->page]) s->selected_id[s->page] = ID_STATE + s->edit.state_slot;     /* first, the current slot */
    }
    s->rows = 0;
    s->list.selected = 0;
    s->list.top = 0;
    rebuild(s);
}

/* --- the tape browser ------------------------------------------------------------------------------ */

/* The browser's list from the tape in the deck, with block `select` chosen; 0 when memory is out. */
static int tape_fill(zr_setup *s, unsigned slot, size_t select) {
    struct zr_tape_deck *deck = zr_machine_tape_deck(s->env.machine, slot);
    tape_close(s);
    if (!deck || !deck->tape) return 0;
    const size_t count = zr_tape_count(deck->tape);
    s->tape_items = calloc(count ? count : 1, sizeof *s->tape_items);
    s->tape_names = calloc(count ? count : 1, sizeof *s->tape_names);
    if (!s->tape_items || !s->tape_names) { tape_close(s); return 0; }
    s->tape_count = count;
    s->tape_slot = slot;
    int digits = 1;
    for (size_t n = count; n >= 10; n /= 10) ++digits;
    for (size_t i = 0; i < count; ++i) {
        char raw[96], clean[96];
        zr_tape_block_name(deck->tape, (unsigned)i, raw, sizeof raw);
        zr_tui_clean(raw, clean, sizeof clean);
        snprintf(s->tape_names[i], sizeof s->tape_names[0], "%*u %s", digits, (unsigned)i + 1, clean);
        s->tape_items[i].kind = ZR_ITEM_TEXT;
        s->tape_items[i].enabled = 1;
        s->tape_items[i].label = s->tape_names[i];
    }
    memset(&s->tape_list, 0, sizeof s->tape_list);
    zr_tui_list_set(&s->tape_list, s->tape_items, count, 12);
    zr_tui_list_select(&s->tape_list, select < count ? select : count ? count - 1 : 0);
    return 1;
}

static void open_tape(zr_setup *s, unsigned slot) {
    struct zr_tape_deck *deck = zr_machine_tape_deck(s->env.machine, slot);
    if (!deck || !deck->tape) return;
    const size_t at = zr_tape_deck_block(deck);
    if (!tape_fill(s, slot, at < zr_tape_count(deck->tape) ? at : 0)) { note(s, ZR_TXT(D_NO_MEMORY), ZR_TXT(D_NO_MEMORY)); return; }
    s->modal = MODAL_TAPE;
    s->tag = TAG_NONE;
}

/* A recording that ended added blocks to the tape: the browser lists them (the deck is left at the end of the tape, which is
 * where the list goes). */
static void tape_refresh(zr_setup *s) {
    if (s->modal != MODAL_TAPE) return;
    struct zr_tape_deck *deck = zr_machine_tape_deck(s->env.machine, s->tape_slot);
    const size_t count = deck && deck->tape ? zr_tape_count(deck->tape) : 0;
    if (count == s->tape_count) return;
    if (!tape_fill(s, s->tape_slot, count)) close_modal(s);
}

/* The tape recorder's record key (the shell's: a slot with no tape gets a new one); a tape the shell made becomes the slot's file. */
static void record_tape(zr_setup *s, unsigned slot) {
    char error[256] = "", shown[256];
    if (!s->env.record) return;
    if (!s->env.record(s->env.ctx, slot, error, sizeof error)) {
        zr_tui_clean(error, shown, sizeof shown);
        note(s, ZR_TXT(D_RECORD_TITLE), shown);
        return;
    }
    zr_config_media *c = slot_settings(s, slot);
    const zr_media *loaded = zr_machine_media(s->env.machine, slot);
    if (c && loaded && strcmp(c->image, loaded->path)) {
        snprintf(c->image, sizeof c->image, "%s", loaded->path);
        s->dirty = 1;
    }
}

static void tape_event(zr_setup *s, zr_tui_event e) {
    zr_machine *m = s->env.machine;
    if (e.key == ZR_TUI_KEY_ESCAPE) { close_modal(s); return; }
    if (e.key == ZR_TUI_KEY_ENTER && s->tape_count) { zr_machine_control(m, s->tape_slot, ZR_MEDIA_SEEK, (uint32_t)s->tape_list.selected); return; }
    if (e.key == ZR_TUI_KEY_CHAR) {
        struct zr_tape_deck *deck = zr_machine_tape_deck(m, s->tape_slot);
        const int c = e.ch | 0x20;
        if (c == 'p') zr_machine_control(m, s->tape_slot, ZR_MEDIA_PLAY, 0);
        else if (c == 's') zr_machine_control(m, s->tape_slot, ZR_MEDIA_STOP, 0);
        else if (c == 'r') zr_machine_control(m, s->tape_slot, ZR_MEDIA_REWIND, 0);
        else if (c == 'w') record_tape(s, s->tape_slot);
        else if (e.ch == ' ') zr_machine_control(m, s->tape_slot, deck && zr_tape_deck_playing(deck) ? ZR_MEDIA_STOP : ZR_MEDIA_PLAY, 0);
        return;
    }
    zr_tui_list_event(&s->tape_list, e);
}

/* --- what the rows do --------------------------------------------------------------------------------------- */

/* Extensions as a picker's filter: "tap,tzx" as ".tap;.tzx". */
static void picker_filter(const char *extensions, char *out, size_t capacity) {
    size_t n = 0;
    out[0] = 0;
    for (const char *p = extensions; *p && n + 2 < capacity;) {
        if (n) out[n++] = ';';
        out[n++] = '.';
        while (*p && *p != ',' && n + 1 < capacity) out[n++] = *p++;
        if (*p == ',') ++p;
    }
    out[n] = 0;
}

static void insert_file(zr_setup *s, unsigned slot, const char *path);

/* The sizes "New blank card" offers, in MiB. */
static const unsigned CARD_MIB[] = {64u, 256u, 1024u};

/* A blank raw image of the size chosen, named card-NNNN.img (the first number free) in the media folder, then put in the slot. */
static void new_card(zr_setup *s, unsigned slot, int size_choice) {
    char folder[ZR_CONFIG_PATH], name[32], path[ZR_CONFIG_PATH + 40], error[256] = "", shown[256];
    if (size_choice < 0 || (size_t)size_choice >= sizeof CARD_MIB / sizeof CARD_MIB[0]) return;
    media_folder(s, folder, sizeof folder);
    path[0] = 0;
    for (unsigned n = 1; n < 10000; ++n) {
        snprintf(name, sizeof name, "card-%04u.img", n);
        snprintf(path, sizeof path, "%s%c%s", folder, zr_host_separator(), name);
        if (!zr_host_file_exists(path)) break;
    }
    if (!zr_card_create(path, ZR_CARD_RAW, (uint64_t)CARD_MIB[size_choice] << 20, 0, error, sizeof error)) {
        zr_tui_clean(error, shown, sizeof shown);
        note(s, ZR_TXT(D_CANNOT_CREATE), shown);
        return;
    }
    insert_file(s, slot, path);
}

static void insert_file(zr_setup *s, unsigned slot, const char *path) {
    zr_config_media *c = slot_settings(s, slot);
    char error[256] = "", stored[ZR_CONFIG_PATH], folder[ZR_CONFIG_PATH];
    if (!c) { note(s, ZR_TXT(D_CANNOT_INSERT), ZR_TXT(D_NO_SLOT)); return; }
    if (s->env.insert && !s->env.insert(s->env.ctx, slot, path, c->write_protect, error, sizeof error)) {
        char shown[sizeof error];
        zr_tui_clean(error, shown, sizeof shown);
        note(s, ZR_TXT(D_CANNOT_INSERT), shown);
        return;
    }
    config_path(s, path, stored, sizeof stored);
    snprintf(c->image, sizeof c->image, "%s", stored);
    if (fs_parent(NULL, path, folder, sizeof folder)) snprintf(s->last_folder, sizeof s->last_folder, "%s", folder);
    s->dirty = 1;
}

/* --- the snapshot files and the cheats' actions ------------------------------------------------------------------------ */

/* The folder snapshots are kept in, `snapshots` beside the configuration, with its separator at the end. */
static void snapshots_folder(const zr_setup *s, char *out, size_t capacity) {
    char base[ZR_CONFIG_PATH];
    folder_of(s->edit.source, base, sizeof base);
    snprintf(out, capacity, "%ssnapshots%c", base, zr_host_separator());
}

/* `<model>-NNNN.szx`: the first number whose file is not there yet. */
static void default_snapshot_name(const zr_setup *s, char *out, size_t capacity) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    char folder[ZR_CONFIG_PATH], path[ZR_CONFIG_PATH + 64];
    snapshots_folder(s, folder, sizeof folder);
    for (unsigned n = 1; n < 10000; ++n) {
        snprintf(out, capacity, "%s-%04u.szx", f->models[model].id, n);
        snprintf(path, sizeof path, "%s%s", folder, out);
        if (!zr_host_file_exists(path)) return;
    }
}

static void open_entry(zr_setup *s, modal_tag tag, int arg, int numeric, const char *title, const char *prompt, const char *initial) {
    s->modal = MODAL_ENTRY;
    s->tag = tag;
    s->arg = arg;
    s->entry_numeric = numeric;
    s->entry_free = tag == TAG_ENTRY_PORT;
    copy_text(s->entry_title, sizeof s->entry_title, title);
    copy_text(s->entry_prompt, sizeof s->entry_prompt, prompt);
    copy_text(s->entry, sizeof s->entry, initial);
    s->entry_len = strlen(s->entry);
}

/* The machine put into a snapshot file: the screen is left when it is done, a refusal is a note. */
static void snapshot_load(zr_setup *s, const char *path) {
    char error[256] = "", shown[256];
    if (s->env.snapshot_load && s->env.snapshot_load(s->env.ctx, path, error, sizeof error)) { finish(s); return; }
    zr_tui_clean(error, shown, sizeof shown);
    note(s, ZR_TXT(D_CANNOT_LOAD), shown);
}

/* The name typed: its ending (sna, z80 or szx; none is szx) says the format; the file is made in the snapshots folder. */
static void snapshot_save(zr_setup *s, const char *name) {
    char file[sizeof s->entry + 8], folder[ZR_CONFIG_PATH], path[ZR_CONFIG_PATH + 64], error[256] = "", shown[256], text[128];
    snprintf(file, sizeof file, "%s", name);
    if (!zr_snap_format_of_name(file) || file[0] == '.') {
        if (strchr(file, '.')) { note(s, ZR_TXT(D_CANNOT_SAVE), ZR_TXT(D_SNAP_BAD_NAME)); return; }
        snprintf(file + strlen(file), sizeof file - strlen(file), ".szx");
    }
    snapshots_folder(s, folder, sizeof folder);
    snprintf(path, sizeof path, "%s%s", folder, file);
    if (!s->env.snapshot_save || !s->env.snapshot_save(s->env.ctx, path, error, sizeof error)) {
        zr_tui_clean(error, shown, sizeof shown);
        note(s, ZR_TXT(D_CANNOT_SAVE), shown);
        return;
    }
    zr_text_format(text, sizeof text, ZR_T_D_SNAP_SAVED_TEXT, file);
    note(s, ZR_TXT(D_SNAP_SAVED), text);
}

/* Trainer `index` on (with the value asked for, or -1) or off. */
static void cheat_switch(zr_setup *s, size_t index, int on, int value) {
    char error[256] = "", shown[256];
    if (!s->env.cheats || zr_cheats_set(s->env.cheats, s->env.machine, index, on, value, error, sizeof error)) return;
    zr_tui_clean(error, shown, sizeof shown);
    note(s, ZR_TXT(D_CHEAT_TITLE), shown);
}

/* Enter on a trainer: off when it is on; on when it is not, after asking for its value when a poke wants one; a spoiled
 * trainer says what is wrong with it. */
static void cheat_toggle(zr_setup *s, size_t index) {
    const zr_cheats *c = s->env.cheats;
    if (!c || index >= zr_cheats_count(c)) return;
    const zr_cheat *ch = &c->cheat[index];
    if (!ch->usable) {
        char problem[96], text[160];
        zr_tui_clean(ch->problem, problem, sizeof problem);
        zr_text_format(text, sizeof text, ZR_T_HINT_CHEAT_BAD, ch->line, problem);
        note(s, ZR_TXT(D_CHEAT_TITLE), text);
    } else if (ch->on) cheat_switch(s, index, 0, -1);
    else if (ch->asks) {
        char title[ZR_POK_NAME + 1], initial[16];
        zr_tui_clean(zr_cheats_name(c, index), title, sizeof title);
        snprintf(initial, sizeof initial, "%u", ch->value);
        open_entry(s, TAG_ENTRY_VALUE, (int)index, 1, title, ZR_TXT(D_VALUE_TEXT), initial);
    } else cheat_switch(s, index, 1, -1);
}

/* What a port's new target is: checked against its kind; a refusal says why and leaves the target as it was. */
static void port_set_target(zr_setup *s, unsigned index, const char *text) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    zr_config_port *p = index < f->port_count ? zr_config_port_slot(&s->edit, f->ports[index].key) : NULL;
    if (!p) return;
    static const int PROBLEMS[] = {0, ZR_T_D_PORT_NEEDS_HOST_PORT, ZR_T_D_PORT_BAD_TCP_PORT, ZR_T_D_PORT_LISTEN_FORM, ZR_T_D_PORT_EMPTY};
    const int problem = zr_config_port_problem(p->type, text);
    if (problem) { note(s, ZR_TXT(D_PORT_TITLE), zr_t(PROBLEMS[problem])); return; }
    if (p->type == ZR_END_FILE) typed_path(s, text, p->path, sizeof p->path);
    else snprintf(p->path, sizeof p->path, "%s", text);
    s->dirty = 1;
}

/* After a kind of end was chosen: what the kind needs. A host serial port is picked from the host's list, a file by a typed name
 * or the file picker, the others are typed. */
static void port_ask(zr_setup *s, unsigned index) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    if (index >= f->port_count) return;
    const zr_config_port *p = zr_config_port_find(&s->edit, f->ports[index].key);
    if (!p) return;
    const char *label = zr_t(f->ports[index].label);
    if (p->type == ZR_END_SERIAL) {
        serial_names l;
        options o;
        list_serial(&l);
        memset(&o, 0, sizeof o);
        snprintf(o.title, sizeof o.title, "%s", ZR_TXT(D_PORT_PROMPT_SERIAL));
        int found = 0;
        for (unsigned i = 0; i < l.count; ++i) {
            add_option(&o, l.name[i], 1);
            if (!strcmp(l.name[i], p->path)) { o.current = o.count - 1; found = 1; }
        }
        if (!found && p->path[0]) {                              /* the configured one stays what it is until another is chosen */
            add_option(&o, p->path, 1);
            o.current = o.count - 1;
        }
        if (o.count) open_menu(s, TAG_PORT_PICK, (int)index, &o);
    } else if (p->type == ZR_END_FILE) {
        options o;
        memset(&o, 0, sizeof o);
        snprintf(o.title, sizeof o.title, "%s", label);
        add_option(&o, ZR_TXT(M_PORT_TYPE_NAME), 1);
        add_option(&o, ZR_TXT(M_PORT_PICK_FILE), 1);
        open_menu(s, TAG_PORT_HOW, (int)index, &o);
    } else if (p->type == ZR_END_PIPE || p->type == ZR_END_TCP || p->type == ZR_END_TCP_LISTEN) {
        const char *prompt = p->type == ZR_END_PIPE ? ZR_TXT(D_PORT_PROMPT_PIPE) : p->type == ZR_END_TCP ? ZR_TXT(D_PORT_PROMPT_TCP) : ZR_TXT(D_PORT_PROMPT_LISTEN);
        open_entry(s, TAG_ENTRY_PORT, (int)index, 0, label, prompt, p->path);
    }
}

static void activate(zr_setup *s, int id) {
    int part = 0;
    const int slot = slot_of(id, &part);
    unsigned model;
    const zr_family *f = running_family(s, &model);
    if (slot >= 0 && (unsigned)slot < f->media_count) {
        zr_config_media *c = slot_settings(s, (unsigned)slot);
        if (!c) return;
        if (part == PART_FILE) {
            char folder[ZR_CONFIG_PATH], filter[64], title[48];
            media_folder(s, folder, sizeof folder);
            picker_filter(f->media[slot].extensions, filter, sizeof filter);
            zr_text_format(title, sizeof title, ZR_T_PICK_TITLE, zr_t(f->media[slot].label));
            open_picker(s, TAG_PICK_FILE, slot, title, folder, filter);
        } else if (part == PART_EJECT) {
            if (zr_machine_media(s->env.machine, (unsigned)slot) && s->env.eject && !s->env.eject(s->env.ctx, (unsigned)slot)) return;
            c->image[0] = 0;
            s->dirty = 1;
        } else if (part == PART_PROTECT) {
            if (s->env.protect && !s->env.protect(s->env.ctx, (unsigned)slot, !c->write_protect)) return;
            c->write_protect = !c->write_protect;
            s->dirty = 1;
        } else if (part == PART_FAST) {
            c->fast = !c->fast;
            s->dirty = 1;
        } else if (part == PART_SOUND) {
            c->sound = !c->sound;
            s->dirty = 1;
        } else if (part == PART_AUTOLOAD) {
            c->autoload = !c->autoload;
            s->dirty = 1;
        } else if (part == PART_BROWSE) open_tape(s, (unsigned)slot);
        else if (part == PART_RECORD) record_tape(s, (unsigned)slot);
        else if (part == PART_NEW) {
            options o;
            memset(&o, 0, sizeof o);
            snprintf(o.title, sizeof o.title, "%s", ZR_TXT(L_NEW_CARD));
            add_option(&o, ZR_TXT(V_CARD_64), 1);
            add_option(&o, ZR_TXT(V_CARD_256), 1);
            add_option(&o, ZR_TXT(V_CARD_1024), 1);
            open_menu(s, TAG_NEW_CARD, slot, &o);
        }
        return;
    }
    if (stick_key_of(id) >= 0) { s->key_wait = stick_key_of(id) + 1; return; }
    if (state_of(id)) { open_slot_menu(s, (unsigned)state_of(id)); return; }
    if (id == ID_SNAP_LOAD) {
        char folder[ZR_CONFIG_PATH], full[ZR_CONFIG_PATH];
        snapshots_folder(s, folder, sizeof folder);
        if (zr_host_folder_exists(folder) && zr_host_full_path(folder, full, sizeof full)) trim_folder(full);
        else media_folder(s, full, sizeof full);
        open_picker(s, TAG_PICK_SNAP, 0, ZR_TXT(PICK_SNAP), full, ".sna;.z80;.szx");
        return;
    }
    if (id == ID_SNAP_SAVE) {
        char name[48];
        default_snapshot_name(s, name, sizeof name);
        open_entry(s, TAG_ENTRY_SNAP, 0, 0, ZR_TXT(D_SNAP_NAME), "", name);
        return;
    }
    if (id == ID_CHEATS_LOAD) {
        char folder[ZR_CONFIG_PATH];
        media_folder(s, folder, sizeof folder);
        open_picker(s, TAG_PICK_POK, 0, ZR_TXT(PICK_POK), folder, ".pok");
        return;
    }
    if (cheat_of(id) >= 0) { cheat_toggle(s, (size_t)cheat_of(id)); return; }
    const toggle *tg = find_toggle(id);
    const int opt = option_of(id);
    unsigned edit_model;
    const zr_family *ef = edit_family(s, &edit_model);
    if (tg) {
        int *field = toggle_field(s, tg);
        *field = !*field;
        s->dirty = 1;
        if (id == ID_FULLSCREEN) s->fullscreen_touched = 1;
    } else if (opt >= 0 && ef && (unsigned)opt < ef->option_count && is_switch(&ef->options[opt])) {
        apply_option(s, id, option_index(&s->edit, ef, (unsigned)opt) == 1 ? 0 : 1);
    } else if (id == ID_MODEL || opt >= 0 || find_choice(id) || id == ID_VOLUME || stick_of(id) >= 0 || port_of(id) >= 0) {
        options o;
        options_of(s, id, &o);
        open_menu(s, TAG_CHOICE, id, &o);
    } else if (id == ID_RESET) open_dialog(s, TAG_RESET, 0, ZR_TXT(D_RESET_TITLE), ZR_TXT(D_RESET_TEXT), ZR_TXT(D_RESET_BUTTONS), 0, 1);
    else if (id == ID_POWER) open_dialog(s, TAG_POWER, 0, ZR_TXT(D_POWER_TITLE), ZR_TXT(D_POWER_TEXT), ZR_TXT(D_POWER_BUTTONS), 0, 1);
    else if (id == ID_EXIT) zr_setup_request_exit(s);
}

static void change(zr_setup *s, int id, int direction) {
    int part;
    unsigned model;
    const zr_family *f = edit_family(s, &model);
    const int opt = option_of(id);
    const int switch_option = opt >= 0 && f && (unsigned)opt < f->option_count && is_switch(&f->options[opt]);
    if (state_of(id) || id == ID_SNAP_LOAD || id == ID_SNAP_SAVE || id == ID_CHEATS_LOAD) return;    /* a slot or an action has no value to turn */
    if (find_toggle(id) || switch_option || slot_of(id, &part) >= 0 || (cheat_of(id) >= 0 && direction)) { activate(s, id); return; }   /* a toggle flips whichever way it is turned */
    if (direction) cycle(s, id, direction);
}

/* --- pop-up results ------------------------------------------------------------------------------------------ */

static void dialog_done(zr_setup *s, int tag, int arg, int result) {
    if (tag == TAG_LEAVE) { if (result == 0) finish(s); }
    else if (tag == TAG_DELETE) { if (result == 0) delete_slot(s, (unsigned)arg); }
    else if (tag == TAG_RESET || tag == TAG_POWER) {
        if (result != 0) return;
        s->reset = tag == TAG_POWER ? ZR_SETUP_RESET_POWER : ZR_SETUP_RESET_WARM;
        finish(s);
    }
}

/* The entry pop-up: a number (digits, Page Up and Page Down, + and -) or a file name (letters, digits, . - _), Enter accepts. */
static void entry_event(zr_setup *s, zr_tui_event e) {
    int number = s->entry_len ? atoi(s->entry) : 0;
    switch (e.key) {
    case ZR_TUI_KEY_ESCAPE: close_modal(s); return;
    case ZR_TUI_KEY_ENTER:
        if (s->entry_len) {
            const modal_tag tag = s->tag;
            const int arg = s->arg;
            char text[sizeof s->entry];
            snprintf(text, sizeof text, "%s", s->entry);
            close_modal(s);
            if (tag == TAG_ENTRY_VALUE) cheat_switch(s, (size_t)arg, 1, atoi(text) > 255 ? 255 : atoi(text));
            else if (tag == TAG_ENTRY_PORT) port_set_target(s, (unsigned)arg, text);
            else snapshot_save(s, text);
        }
        return;
    case ZR_TUI_KEY_BACKSPACE:
        if (s->entry_len) s->entry[--s->entry_len] = 0;
        return;
    case ZR_TUI_KEY_PAGE_UP:
    case ZR_TUI_KEY_UP:
    case ZR_TUI_KEY_PAGE_DOWN:
    case ZR_TUI_KEY_DOWN:
        if (!s->entry_numeric) return;
        number += e.key == ZR_TUI_KEY_PAGE_UP || e.key == ZR_TUI_KEY_UP ? 1 : -1;
        snprintf(s->entry, sizeof s->entry, "%d", number < 0 ? 0 : number > 255 ? 255 : number);
        s->entry_len = strlen(s->entry);
        return;
    case ZR_TUI_KEY_CHAR: break;
    default: return;
    }
    const int c = e.ch;
    if (s->entry_numeric) {
        if (c == '+' || c == '-') {
            number += c == '+' ? 1 : -1;
            snprintf(s->entry, sizeof s->entry, "%d", number < 0 ? 0 : number > 255 ? 255 : number);
        } else if (c >= '0' && c <= '9') {
            if (s->entry_len == 1 && s->entry[0] == '0') s->entry_len = 0;      /* a typed digit replaces a lone 0 */
            if (s->entry_len >= 3) return;
            s->entry[s->entry_len++] = (char)c;
            s->entry[s->entry_len] = 0;
            if (atoi(s->entry) > 255) snprintf(s->entry, sizeof s->entry, "255");
        } else return;
        s->entry_len = strlen(s->entry);
        return;
    }
    if (s->entry_free) {
        if (c >= 0x20 && c < 0x7F && s->entry_len + 1 < sizeof s->entry) {
            s->entry[s->entry_len++] = (char)c;
            s->entry[s->entry_len] = 0;
        }
        return;
    }
    if (((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') && s->entry_len < 40) {
        s->entry[s->entry_len++] = (char)c;
        s->entry[s->entry_len] = 0;
    }
}

static void modal_event(zr_setup *s, zr_tui_event e) {
    switch (s->modal) {
    case MODAL_MENU: {
        const zr_tui_result r = zr_tui_list_event(&s->menu, e);
        if (r == ZR_TUI_ACTIVATE) {
            const int tag = s->tag, arg = s->arg, index = (int)s->menu.selected;
            close_modal(s);
            if (tag == TAG_CHOICE) {
                apply_option(s, arg, index);
                if (port_of(arg) >= 0) port_ask(s, (unsigned)port_of(arg));     /* the kind's target: asked again when the same kind is picked */
            } else if (tag == TAG_SLOT) slot_action(s, (unsigned)arg, index);
            else if (tag == TAG_NEW_CARD) new_card(s, (unsigned)arg, index);
            else if (tag == TAG_PORT_PICK) {
                zr_config_port *p = port_edit(s, (unsigned)arg);
                if (p && index >= 0 && index < MENU_MAX && s->menu_labels[index][0]) {
                    snprintf(p->path, sizeof p->path, "%s", s->menu_labels[index]);
                    s->dirty = 1;
                }
            } else if (tag == TAG_PORT_HOW) {
                const zr_config_port *p = port_edit(s, (unsigned)arg);
                if (p && index == 0) {
                    unsigned model;
                    const zr_family *f = running_family(s, &model);
                    open_entry(s, TAG_ENTRY_PORT, arg, 0, zr_t(f->ports[arg].label), ZR_TXT(D_PORT_PROMPT_FILE), shown_path(s, p->path));
                } else if (p) {
                    char folder[ZR_CONFIG_PATH], title[48];
                    media_folder(s, folder, sizeof folder);
                    snprintf(title, sizeof title, "%s", ZR_TXT(PICK_PORT_FILE));
                    open_picker(s, TAG_PICK_PORT, arg, title, folder, "");
                }
            }
        } else if (r == ZR_TUI_CANCEL) close_modal(s);
        break;
    }
    case MODAL_DIALOG:
        if (zr_tui_dialog_event(&s->dialog, e) == ZR_TUI_ACTIVATE) {
            const int tag = s->tag, arg = s->arg, result = s->dialog.result;
            close_modal(s);
            dialog_done(s, tag, arg, result);
        }
        break;
    case MODAL_PICKER: {
        const zr_tui_result r = zr_tui_picker_event(&s->picker, e);
        if (r == ZR_TUI_ACTIVATE) {
            const int arg = s->arg;
            const modal_tag tag = s->tag;
            char path[ZR_TUI_PATH], error[256] = "", shown[256];
            snprintf(path, sizeof path, "%s", s->picker.picked);
            close_modal(s);
            if (tag == TAG_PICK_PORT) {
                zr_config_port *p = port_edit(s, (unsigned)arg);
                if (p) {
                    config_path(s, path, p->path, sizeof p->path);
                    s->dirty = 1;
                }
            } else if (tag == TAG_PICK_SNAP) snapshot_load(s, path);
            else if (tag == TAG_PICK_POK) {
                if (!s->env.cheats_load || !s->env.cheats_load(s->env.ctx, path, error, sizeof error)) {
                    zr_tui_clean(error, shown, sizeof shown);
                    note(s, ZR_TXT(D_CANNOT_LOAD), shown);
                }
            } else insert_file(s, (unsigned)arg, path);
        } else if (r == ZR_TUI_CANCEL) close_modal(s);
        break;
    }
    case MODAL_TAPE:
        tape_event(s, e);
        break;
    case MODAL_ENTRY:
        entry_event(s, e);
        break;
    default: break;
    }
}

void zr_setup_event(zr_setup *s, zr_tui_event e) {
    if (s->done) return;
    if (s->key_wait && s->modal == MODAL_NONE) {            /* the next host key is the answer (zr_setup_host_key) */
        if (e.key == ZR_TUI_KEY_ESCAPE) { s->key_wait = 0; rebuild(s); }
        return;
    }
    if (s->modal != MODAL_NONE) {
        modal_event(s, e);
        tape_refresh(s);
        if (!s->done) { persist(s); rebuild(s); }
        if (s->quick && s->modal == MODAL_NONE && !s->done) finish(s);     /* the picker is gone (and the note it may have left) */
        return;
    }
    switch (e.key) {
    case ZR_TUI_KEY_TAB:
    case ZR_TUI_KEY_RIGHT: set_page(s, s->page + 1, 1); persist(s); return;
    case ZR_TUI_KEY_BACKTAB:
    case ZR_TUI_KEY_LEFT: set_page(s, s->page - 1, -1); persist(s); return;
    default: break;
    }
    const zr_tui_result r = zr_tui_list_event(&s->list, e);
    const int id = s->rows && s->list.selected < s->rows ? s->ids[s->list.selected] : 0;
    if (r == ZR_TUI_CANCEL) { zr_setup_request_close(s); return; }
    if (r == ZR_TUI_ACTIVATE) activate(s, id);
    else if (r == ZR_TUI_PREV) change(s, id, -1);
    else if (r == ZR_TUI_NEXT) change(s, id, 1);
    if (!s->done) { persist(s); rebuild(s); }
}

int zr_setup_wants_host_key(const zr_setup *s) { return s->key_wait != 0 && s->modal == MODAL_NONE && !s->done; }

void zr_setup_host_key(zr_setup *s, const char *name, unsigned modifiers) {
    if (!zr_setup_wants_host_key(s)) return;
    const unsigned function = (unsigned)s->key_wait - 1;
    s->key_wait = 0;
    if (strcmp(name, "Escape")) {
        const char *reserved = NULL;
        unsigned other = 0;
        char text[160], shown[ZR_CONFIG_KEY_NAME * 2];
        int problem;
        /* Alt+Enter is the window's full screen key, a key with a modifier. */
        if ((modifiers & ZR_SETUP_KEY_ALT) && !strcmp(name, "Return")) { problem = ZR_KEY_NAME_RESERVED; reserved = "Alt+Enter"; }
        else problem = zr_config_stick_key_problem(&s->edit, function, name, &reserved, &other);
        zr_tui_clean(name, shown, sizeof shown);
        if (problem == ZR_KEY_NAME_OK) {
            snprintf(s->edit.stick_keys[function], sizeof s->edit.stick_keys[function], "%s", name);
            s->dirty = 1;
        } else {
            if (problem == ZR_KEY_NAME_RESERVED) zr_text_format(text, sizeof text, ZR_T_D_KEY_RESERVED, reserved);
            else if (problem == ZR_KEY_NAME_USED) zr_text_format(text, sizeof text, ZR_T_D_KEY_USED, shown, zr_t(KEY_FUNCTIONS[other]));
            else snprintf(text, sizeof text, "%s", ZR_TXT(D_KEY_BAD));
            note(s, ZR_TXT(D_KEY_TITLE), text);
        }
    }
    persist(s);
    rebuild(s);
}

void zr_setup_refresh(zr_setup *s) { rebuild(s); }

/* --- drawing ------------------------------------------------------------------------------------------------ */

/* The keys line: pairs of a key's cells and the word of what it does, from the string table, the pairs ending with a NULL key. */
#define K_LEFT "\x9C"
#define K_RIGHT "\x9B"
#define K_UP "\x5E"
#define K_DOWN "\x9A"
typedef struct key_word { const char *key; int word; } key_word;

static void keys_line(zr_tui *t, const key_word *pairs) {
    const uint8_t bar = ZR_ATTR(ZR_CYAN, ZR_BLACK, 0), key = ZR_ATTR(ZR_YELLOW, ZR_BLACK, 1);
    zr_tui_fill(t, zr_tui_rect_make(LEGEND_ROW, 0, 1, ZR_TUI_COLS), ' ', bar);
    int col = 0;
    for (; pairs->key; ++pairs) {
        col += zr_tui_put(t, LEGEND_ROW, col, key, pairs->key);
        if (pairs->word >= 0) col += zr_tui_put(t, LEGEND_ROW, col, bar, " ");
        if (pairs->word >= 0) col += zr_tui_put(t, LEGEND_ROW, col, bar, zr_t(pairs->word));
        col += zr_tui_put(t, LEGEND_ROW, col, bar, " ");
    }
}

static void draw_menu(zr_setup *s) {
    zr_tui *t = &s->tui;
    const zr_tui_theme *th = zr_tui_popup_theme();
    int width = (int)strlen(s->menu_title) + 4;
    for (size_t i = 0; i < s->menu.count; ++i) {
        int w = (int)strlen(s->menu_labels[i]) + 4;
        if (s->menu_values[i][0]) w += (int)strlen(s->menu_values[i]) + 1;
        if (w > width) width = w;
    }
    if ((int)s->menu.count > s->menu.rows) ++width;             /* a list that scrolls gives a column to its marks */
    if (width > ZR_TUI_COLS - 2) width = ZR_TUI_COLS - 2;
    const int shown = s->menu.rows;
    const zr_tui_rect r = zr_tui_centered(shown + 2, width, POPUP_TOP, POPUP_BOTTOM);
    zr_tui_fill(t, r, ' ', th->normal);
    zr_tui_box(t, r, th->frame);
    zr_tui_bar(t, r.row, r.col, r.cols, th->title, s->menu_title, 0);
    zr_tui_list_draw(t, &s->menu, zr_tui_rect_make(r.row + 1, r.col + 1, shown, r.cols - 2), th);
}

static void draw_tape(zr_setup *s) {
    zr_tui *t = &s->tui;
    const zr_tui_theme *th = zr_tui_popup_theme();
    unsigned model;
    const zr_family *f = running_family(s, &model);
    struct zr_tape_deck *deck = zr_machine_tape_deck(s->env.machine, s->tape_slot);
    const zr_tui_rect r = zr_tui_centered(POPUP_BOTTOM - POPUP_TOP + 1, 30, POPUP_TOP, POPUP_BOTTOM);
    zr_tui_fill(t, r, ' ', th->normal);
    zr_tui_box(t, r, th->frame);
    zr_tui_bar(t, r.row, r.col, r.cols, th->title, s->tape_slot < f->media_count ? zr_t(f->media[s->tape_slot].label) : "", 0);
    /* where the tape is: the block and whether it runs */
    char state[64];
    const uint32_t at = deck ? zr_tape_deck_block(deck) : 0;
    const int playing = deck && zr_tape_deck_playing(deck);
    if (deck && zr_tape_deck_recording(deck)) snprintf(state, sizeof state, "%s", ZR_TXT(TAPE_RECORDING));
    else if (at >= s->tape_count) snprintf(state, sizeof state, "%s", ZR_TXT(TAPE_AT_END));
    else zr_text_format(state, sizeof state, playing ? ZR_T_TAPE_PLAYING : ZR_T_TAPE_STOPPED, (unsigned)at + 1, (unsigned)s->tape_count);
    zr_tui_put_fit(t, r.row + 1, r.col + 1, r.cols - 2, th->action, state, ZR_FIT_CUT_END);
    zr_tui_rule(t, r, 2, th->frame);
    zr_tui_rule(t, r, r.rows - 5, th->frame);
    const zr_tui_rect area = zr_tui_rect_make(r.row + 3, r.col + 1, r.rows - 8, r.cols - 2);
    zr_tui_list_draw(t, &s->tape_list, area, th);
    /* the block the tape is at, marked with a pointer in the cell before its name; it flashes while the tape runs */
    if (at < s->tape_count && at >= s->tape_list.top && at < s->tape_list.top + (size_t)area.rows) {
        const int row = area.row + (int)(at - s->tape_list.top);
        const int selected = at == s->tape_list.selected;
        zr_tui_fill(t, zr_tui_rect_make(row, area.col, 1, 1), ZR_GLYPH_POINT_RIGHT,
                    (uint8_t)(ZR_ATTR(ZR_RED, selected ? ZR_CYAN : ZR_WHITE, 1) | (playing ? ZR_FLASH : 0u)));
    }
    zr_tui_put_fit(t, r.row + r.rows - 4, r.col + 1, r.cols - 2, th->normal, ZR_TXT(TAPE_KEYS_GO), ZR_FIT_CUT_END);
    zr_tui_put_fit(t, r.row + r.rows - 3, r.col + 1, r.cols - 2, th->normal, ZR_TXT(TAPE_KEYS), ZR_FIT_CUT_END);
    zr_tui_put_fit(t, r.row + r.rows - 2, r.col + 1, r.cols - 2, th->normal, ZR_TXT(TAPE_KEYS_REC), ZR_FIT_CUT_END);
}

static void draw_entry(zr_setup *s) {
    zr_tui *t = &s->tui;
    const zr_tui_theme *th = zr_tui_popup_theme();
    const zr_tui_rect r = zr_tui_centered(7, ZR_TUI_COLS - 2, POPUP_TOP, POPUP_BOTTOM);
    zr_tui_fill(t, r, ' ', th->normal);
    zr_tui_box(t, r, th->frame);
    zr_tui_bar(t, r.row, r.col, r.cols, th->title, s->entry_title, 0);
    zr_tui_put_fit(t, r.row + 2, r.col + 2, r.cols - 4, th->normal, s->entry_prompt, ZR_FIT_CUT_END);
    /* the field: what is typed and a cursor after it, the end of a long name in sight */
    char shown[sizeof s->entry + 2];
    snprintf(shown, sizeof shown, "%s_", s->entry);
    zr_tui_put_fit(t, r.row + 4, r.col + 2, r.cols - 4, th->selected, shown, ZR_FIT_CUT_START);
}

static void draw(zr_setup *s) {
    zr_tui *t = &s->tui;
    const zr_tui_theme *w = zr_tui_window_theme();
    zr_tui_clear(t, w->normal);
    /* the title bar: the name ZetRo, the page's name between pointers (the keys that turn the pages), its number, the flag's stripes */
    char title[96];
    snprintf(title, sizeof title, "ZetRo %c %s %c", ZR_GLYPH_CHAR(ZR_GLYPH_POINT_LEFT), zr_t(PAGE_TITLES[s->page]), ZR_GLYPH_CHAR(ZR_GLYPH_POINT_RIGHT));      /* the table's text is cells already */
    zr_tui_bar(t, 0, 0, ZR_TUI_COLS, ZR_ATTR(ZR_WHITE, ZR_BLACK, 1), title, 1);
    char number[16];
    int place = 0, pages = 0;
    for (int p = 0; p < ZR_SETUP_PAGES; ++p)
        if (page_shown(s, p)) {
            ++pages;
            if (p <= s->page) ++place;
        }
    snprintf(number, sizeof number, "%d/%d", place, pages);
    zr_tui_put(t, 0, ZR_TUI_COLS - 5 - (int)strlen(number), ZR_ATTR(ZR_CYAN, ZR_BLACK, 1), number);
    zr_tui_list_draw(t, &s->list, zr_tui_rect_make(LIST_ROW, 0, s->page == ZR_SETUP_STATES ? STATE_ROWS : LIST_ROWS, ZR_TUI_COLS), w);
    if (s->page == ZR_SETUP_ABOUT) draw_about(s);
    /* the hint of the selected row, in the bottom line */
    const char *hint = s->rows && s->list.selected < s->rows ? s->hints[s->list.selected] : "";
    static const key_word form_keys[] = {{K_LEFT K_RIGHT, ZR_T_LEG_PAGE}, {K_UP K_DOWN, ZR_T_LEG_ROW}, {"+-", ZR_T_LEG_SET}, {"Esc", ZR_T_LEG_EXIT}, {NULL, 0}};
    static const key_word menu_keys[] = {{K_UP K_DOWN, ZR_T_LEG_CHOOSE}, {"Enter", ZR_T_LEG_PICK}, {"Esc", ZR_T_LEG_BACK}, {NULL, 0}};
    static const key_word picker_keys[] = {{"Enter", ZR_T_LEG_OPEN}, {"Bksp", ZR_T_LEG_UP}, {"Esc", ZR_T_LEG_BACK}, {NULL, 0}};
    static const key_word dialog_keys[] = {{K_LEFT K_RIGHT, ZR_T_LEG_CHOOSE}, {"Enter", ZR_T_LEG_PICK}, {NULL, 0}};
    static const key_word tape_keys[] = {{K_UP K_DOWN, ZR_T_LEG_MOVE}, {"Esc", ZR_T_LEG_BACK}, {NULL, 0}};
    static const key_word wait_keys[] = {{"Esc", ZR_T_LEG_CANCEL}, {NULL, 0}};
    static const key_word number_keys[] = {{"0-9", ZR_T_LEG_TYPE}, {"PgUp/Dn", ZR_T_LEG_NUMBER}, {"Enter", ZR_T_LEG_PICK}, {"Esc", ZR_T_LEG_BACK}, {NULL, 0}};
    static const key_word name_keys[] = {{"Enter", ZR_T_LEG_PICK}, {"Esc", ZR_T_LEG_BACK}, {"Bksp", ZR_T_LEG_ERASE}, {NULL, 0}};
    const key_word *keys = s->key_wait ? wait_keys : form_keys;
    if (s->modal != MODAL_NONE)         /* the page stays in sight behind a pop-up, dimmed */
        for (int r = LIST_ROW; r < LIST_ROW + LIST_ROWS; ++r)
            for (int c = 0; c < ZR_TUI_COLS; ++c) t->cell[r][c].attr = ZR_ATTR(ZR_BLUE, ZR_BLACK, 0);
    if (s->page == ZR_SETUP_STATES) draw_states_box(s);
    switch (s->modal) {
    case MODAL_MENU: draw_menu(s); keys = menu_keys; hint = ""; break;
    case MODAL_DIALOG: zr_tui_dialog_draw(t, &s->dialog, POPUP_TOP, POPUP_BOTTOM); keys = dialog_keys; hint = ""; break;
    case MODAL_PICKER: zr_tui_picker_draw(t, &s->picker, POPUP_TOP, POPUP_BOTTOM); keys = picker_keys; hint = ""; break;
    case MODAL_TAPE: draw_tape(s); keys = tape_keys; hint = ""; break;
    case MODAL_ENTRY: draw_entry(s); keys = s->entry_numeric ? number_keys : name_keys; hint = ""; break;
    default: break;
    }
    keys_line(t, keys);
    const uint8_t report = ZR_ATTR(ZR_BLACK, ZR_WHITE, 1);
    char shown[160];
    snprintf(shown, sizeof shown, " %s", hint);                 /* a cell of margin in front of it */
    zr_tui_put_fit(t, HINT_ROW, 0, ZR_TUI_COLS, report, shown, ZR_FIT_CUT_END);
}

void zr_setup_frame(zr_setup *s, int flash_on, zr_frame *frame) {
    draw(s);
    zr_tui_render(&s->tui, flash_on);
    draw_thumbnail(s);
    zr_tui_frame(&s->tui, frame);
    draw_logo(s, frame);
}

/* --- life cycle ----------------------------------------------------------------------------------------------------------- */

zr_setup *zr_setup_create(const zr_config *config, const zr_setup_env *env) {
    zr_setup *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->env = *env;
    s->edit = *config;
    s->saved_fullscreen = config->fullscreen;
    s->edit.fullscreen = env->fullscreen != 0;
    s->running = *config;
    s->running.fullscreen = s->edit.fullscreen;
    s->list.form = 1;                   /* Page Up, Page Down, + and - change the value of the selected row */
    s->slot_info = calloc(ZR_STATES_SLOTS, sizeof *s->slot_info);
    zr_tui_init(&s->tui, NULL);
    zr_text_set_font(s->tui.font);
    zr_text_set_language(s->edit.language);
    /* The page that was open last, when this machine has it. */
    const int last = zr_setup_page_by_name(s->edit.setup_page);
    s->page = last >= 0 && page_shown(s, last) ? last : ZR_SETUP_MACHINE;
    if (s->page == ZR_SETUP_STATES) s->selected_id[s->page] = ID_STATE + s->edit.state_slot;
    rebuild(s);
    return s;
}

void zr_setup_destroy(zr_setup *s) {
    if (!s) return;
    close_modal(s);
    free(s->slot_info);
    free(s);
}

void zr_setup_open_page(zr_setup *s, int page) {
    if (page < 0 || page >= ZR_SETUP_PAGES) return;
    set_page(s, page, 1);
    persist(s);
}

void zr_setup_pick_media(zr_setup *s, unsigned slot) {
    unsigned model;
    const zr_family *f = running_family(s, &model);
    if (slot >= f->media_count || slot >= ZR_CONFIG_MEDIA) return;
    remember(s);
    s->page = ZR_SETUP_MEDIA;
    s->rows = 0;
    s->list.selected = s->list.top = 0;
    rebuild(s);
    s->quick = 1;
    activate(s, ID_SLOT + 10 * (int)slot + PART_FILE);
    if (s->modal == MODAL_NONE) finish(s);              /* the picker could not open */
}

int zr_setup_page(const zr_setup *s) { return s->page; }
const zr_tui *zr_setup_screen(const zr_setup *s) { return &s->tui; }
int zr_setup_popup(const zr_setup *s) { return s->modal != MODAL_NONE; }
const zr_config *zr_setup_config(const zr_setup *s) { return &s->edit; }
int zr_setup_done(const zr_setup *s) { return s->done; }
int zr_setup_exit(const zr_setup *s) { return s->exit; }
int zr_setup_reset(const zr_setup *s) { return s->reset; }
