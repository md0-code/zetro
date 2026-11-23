/* The desktop shell: the window, the loop that paces the machine to the
 * clock, the keyboard, sound, media and the harness's channel.
 *
 * In the window: F12 opens and closes the setup screen (setup.h; the machine
 * stands still while it is open), Alt+Enter toggles full screen, Pause stops
 * and starts the machine, Print Screen saves zetro-NNNN.png, F10 is the NMI
 * button, F8 opens and closes the on-screen keyboard (osk.h: the mouse and a
 * game controller's Select/Back, d-pad, A and B work it; the setup screen
 * closes it), F11 switches the keyboard joystick on and off (joykeys.h; game
 * controllers, pads.h, feed the sockets the configuration gives them), F5,
 * F6 and F7 play and stop the first tape and go to its previous and next
 * block, F1 opens the tape's file picker, Ctrl+F12 switches the machine off and on, Alt+F12 resets it,
 * Shift+F12 exits. Every
 * other key is the machine's key in the same place (the family's key
 * table). A file dropped on the window goes into the first media slot that
 * takes it, and a tape is loaded at once when its slot's autoload is on (typist.h).
 * Messages (a file inserted, a screenshot saved, the tape's keys) show for a
 * couple of seconds on a status line over the bottom of the picture, and
 * "paused" does while the machine is. */
#ifndef ZR_DESKTOP_H
#define ZR_DESKTOP_H

#include "../core/config.h"
#include "../core/machine.h"
#include "../core/romdb.h"

/* A host key pressed and released after a given frame, for the checks (--hostkey KEY@N): an SDL key name with the modifiers held. */
#define ZR_DESKTOP_HOSTKEYS 256
enum { ZR_HOSTKEY_CTRL = 1, ZR_HOSTKEY_SHIFT = 2, ZR_HOSTKEY_ALT = 4, ZR_HOSTKEY_PRESS = 8, ZR_HOSTKEY_RELEASE = 16 };   /* PRESS, RELEASE: only that half (+KEY, -KEY) */
typedef struct zr_desktop_hostkey {
    uint64_t at;
    char name[32];
    unsigned mods;
} zr_desktop_hostkey;
/* 1 when the host knows a key by this name (SDL's names: F12, Down, Return, A, Keypad +). */
int zr_desktop_key_known(const char *name);

typedef struct zr_desktop_options {
    int hidden;                 /* no visible window, no sound device: a frame period of guest time per frame */
    int realtime;               /* a hidden window paced to the clock as a visible one is */
    uint64_t frames;            /* stop after this many frames (0: run until closed) */
    const char *screenshot;     /* save the last frame here */
    const char *wav;            /* record the sound here instead of playing it */
    const char *script;         /* the harness's script (harness.h) */
    const char *control;        /* the harness's control channel: "stdin" or "tcp:PORT" */
    const char *drop;           /* a file put in as if dropped on the window when the run starts (for checks of auto-load) */
    const zr_rom_index *roms;
    /* The setup screen's page to open at the start (setup.h's ZR_SETUP_*; -1: none) and the host keys to press, in the order
     * of their frames (the checks drive the screen with them). */
    int setup_page;
    zr_desktop_hostkey hostkeys[ZR_DESKTOP_HOSTKEYS];
    unsigned hostkey_count;
} zr_desktop_options;

/* Runs until the window is closed, the frames are done or the harness ends
 * the run. *machine may be replaced (a power cycle keeps it; a later setup
 * screen builds it again). Returns the exit status: 0, or another with the
 * reason in `error` when there is one. */
int zr_desktop_run(zr_machine **machine, zr_config *config, const zr_desktop_options *options, char *error, size_t capacity);

#endif /* ZR_DESKTOP_H */
