/* The setup screen: the pages Machine, Media, Display, Sound, Controllers, Ports, States, Cheats and About, drawn in the ZX
 * Spectrum look with the text-cell toolkit (tui.h) from the tables the running machine describes itself with
 * (machine.h): the media slots of its model, its models and their options, its joystick sockets, the state slots of its
 * model (states.h; the selected slot's thumbnail drawn on the page), its byte ports with the host end each is connected to. Changes are made to a copy of the configuration
 * and written back to zetro.yaml as they are made, through the comment-keeping editor (zr_config_store); the
 * shell applies what changed (zr_setup_diff): the display, the sound and the language at once, a new model or
 * an option that needs a reset when the screen is left, media as they are inserted. No SDL and no OS call
 * here beyond the host layer's folders; the shell (desktop.c) feeds it keys and shows its frame. ZetRo's own code. */
#ifndef ZR_SETUP_H
#define ZR_SETUP_H

#include "../core/config.h"
#include "../core/machine.h"
#include "../core/romdb.h"
#include "cheats.h"
#include "tui.h"

enum { ZR_SETUP_MACHINE, ZR_SETUP_MEDIA, ZR_SETUP_DISPLAY, ZR_SETUP_SOUND, ZR_SETUP_CONTROLLERS, ZR_SETUP_PORTS, ZR_SETUP_STATES, ZR_SETUP_CHEATS,
       ZR_SETUP_ABOUT, ZR_SETUP_PAGES };
/* "machine", "media", ...; the number of a name, -1 for an unknown one. */
const char *zr_setup_page_name(int page);
int zr_setup_page_by_name(const char *name);

/* What a change of the configuration asks of the shell. */
enum {
    ZR_CHANGE_RESTART = 1,      /* machine.*: the machine is built again (when the screen is left) */
    ZR_CHANGE_VIDEO = 2,        /* video.*: applied at once */
    ZR_CHANGE_SOUND = 4,        /* sound.*: applied at once */
    ZR_CHANGE_LANGUAGE = 8,     /* ui.language: applied at once (text.h) */
    ZR_CHANGE_MEDIA = 16,       /* media.*: the slots' settings (the files themselves go in when they are chosen) */
    ZR_CHANGE_CONTROLLERS = 32, /* controllers.*: applied at once */
    ZR_CHANGE_STATE_SLOT = 128, /* ui.state_slot: applied at once */
    ZR_CHANGE_PORTS = 256       /* ports.*: the ends are opened again at once */
};
unsigned zr_setup_diff(const zr_config *before, const zr_config *after);

typedef struct zr_setup zr_setup;

typedef struct zr_setup_env {
    void *ctx;
    zr_machine *machine;                /* the machine that runs: its family's tables, its media, its tape recorders */
    const char *version;                /* the program's version, for the About page */
    const zr_rom_index *roms;           /* the ROMs present (models that lack some are marked); NULL: unknown */
    int fullscreen;                     /* the window is full screen now */
    int no_crt;                         /* the CRT shader is not available: the CRT look is shown plain, so the filter acts */
    /* A file is put in a slot of the machine (with its write protection), or taken out. 0 with the reason when it
     * cannot be done. The shell keeps the files and says what happened on the status line. */
    int (*insert)(void *ctx, unsigned slot, const char *path, int write_protect, char *error, size_t capacity);
    int (*eject)(void *ctx, unsigned slot);
    /* A slot's write protection was switched while a file is in it. */
    int (*protect)(void *ctx, unsigned slot, int on);
    /* The name of the game controller with this number (0 to ZR_CONFIG_PADS - 1: the number minus one), NULL when none is
     * plugged in (NULL: the shell has no controllers). */
    const char *(*pad_name)(void *ctx, unsigned index);
    /* The States page: the running machine saved to the file of slot `slot` (1 to 10, states.h) with its media and
     * thumbnail, or put back from it (its media first); 0 with the reason. After a load the screen is left. */
    int (*slot_save)(void *ctx, unsigned slot, char *error, size_t capacity);
    int (*slot_load)(void *ctx, unsigned slot, char *error, size_t capacity);
    /* The States page's snapshot files (the rows are there while the machine takes snapshots, machine.h): a file of the
     * path put into the machine, or the machine written to it as the ending says (sna, z80, szx; the folders made). 0
     * with the reason. After a load the screen is left. */
    int (*snapshot_load)(void *ctx, const char *path, char *error, size_t capacity);
    int (*snapshot_save)(void *ctx, const char *path, char *error, size_t capacity);
    /* The Cheats page: the shell's set of trainers (cheats.h; NULL: none), which the page switches on and off itself, and a
     * .pok file loaded into it. 0 with the reason. */
    zr_cheats *cheats;
    int (*cheats_load)(void *ctx, const char *path, char *error, size_t capacity);
    /* The tape recorder's record key for a tape slot: starts recording, or ends the one under way (the tape is then written
     * to its file); a slot with no tape gets a new tape file, inserted. 0 with the reason. NULL: the shell has none. */
    int (*record)(void *ctx, unsigned slot, char *error, size_t capacity);
    /* Why the host end of port `port` (the family's table index) could not be opened, NULL when it could or none is set. */
    const char *(*port_error)(void *ctx, unsigned port);
} zr_setup_env;

zr_setup *zr_setup_create(const zr_config *config, const zr_setup_env *env);
void zr_setup_destroy(zr_setup *s);
void zr_setup_open_page(zr_setup *s, int page);
/* The file picker of media slot `slot` alone, in front of the Media page (the shell's F1 for the tape): the screen is left
 * when the picker is, with the file chosen in the slot or with nothing changed. The page the screen opens on the next time
 * stays what it was. */
void zr_setup_pick_media(zr_setup *s, unsigned slot);
int zr_setup_page(const zr_setup *s);

/* Input: Left and Right (and Tab) turn the pages, Up and Down select a row, Page Up, Page Down, + and - change its
 * value, Enter opens or flips it and Escape leaves. */
void zr_setup_event(zr_setup *s, zr_tui_event event);
/* The user asked to leave (F12): any pop-up closes and the screen is left, after the question leaving can raise. */
void zr_setup_request_close(zr_setup *s);
/* The rows are built again (a game controller came or went). */
void zr_setup_refresh(zr_setup *s);

/* The Controllers page's keys of the keyboard joystick are changed by pressing Enter on one and then the new key: while the
 * screen waits for it, the shell gives it every key (the window's own included) by its name, as SDL names it, with the
 * modifiers held, and the screen takes it, or refuses it with a message (a key the window keeps, or one another function has),
 * or, for Escape, stops waiting. */
enum { ZR_SETUP_KEY_CTRL = 1, ZR_SETUP_KEY_ALT = 2 };
int zr_setup_wants_host_key(const zr_setup *s);
void zr_setup_host_key(zr_setup *s, const char *name, unsigned modifiers);

/* The screen as a frame; `flash_on` is the phase of the FLASH attribute. */
void zr_setup_frame(zr_setup *s, int flash_on, zr_frame *frame);
/* The screen as it was last drawn (zr_setup_frame): its cells, for the checks. */
const zr_tui *zr_setup_screen(const zr_setup *s);
/* 1 while a pop-up (list, question, file picker, tape browser) is in front of the page. */
int zr_setup_popup(const zr_setup *s);

/* The edited configuration (the settings as they are now). */
const zr_config *zr_setup_config(const zr_setup *s);
/* 1 once the screen is left: the settings are saved and the shell may apply them; zr_setup_reset says whether the
 * user asked for a reset. */
int zr_setup_done(const zr_setup *s);
enum { ZR_SETUP_RESET_NONE, ZR_SETUP_RESET_WARM, ZR_SETUP_RESET_POWER };
int zr_setup_reset(const zr_setup *s);
/* Exit ZetRo, at once: the Machine page's Exit item. The screen is left as ever, with the settings saved and no question
 * (nothing is rebuilt), and zr_setup_exit is then 1: the shell ends the program. */
void zr_setup_request_exit(zr_setup *s);
int zr_setup_exit(const zr_setup *s);

/* Writes a configuration to the file it was read from, keeping the file's comments (nothing to write when it has no
 * file); 0 with a message when that fails. */
int zr_setup_write(const zr_config *config, char *error, size_t capacity);

#endif /* ZR_SETUP_H */
