/* The harness: the running machine driven by commands,
 * for headless checks and for development. No SDL and no OS call: the shell
 * (desktop.c) gives it the machine and the few things only the shell can do.
 *
 * Commands come from a script file (--script), one a line, and from a
 * control channel (--control stdin or tcp:PORT), a line each, answered with
 * one line of JSON. The script runs first. Lines starting with # and empty
 * lines are skipped. The language is the MiSTer simulators' (wait, type,
 * enter, key, release, play, nmi, reset), extended:
 *
 *   wait SECONDS              run guest time (a decimal number of seconds)
 *   frames N                  run N frames
 *   type TEXT                 type TEXT on the machine's keyboard (\n Enter, \b delete, \\ a backslash)
 *   enter                     the same as type \n
 *   key NAME / release NAME   press / release a key of the machine's key table (key A, key CAPS SHIFT)
 *   joy N [up] [down] [left] [right] [fire] [fire2]    joystick N holds these (none: released)
 *   insert SLOT PATH          a file into a media slot (insert tape demo.tzx)
 *   drop PATH                 a file as if it had been dropped on the window: into the slot that takes it, and a tape is then
 *                             auto-loaded (the slot's autoload; what insert never does)
 *   eject SLOT
 *   play PATH                 a tape into the first tape slot, and its PLAY key
 *   tape play|stop|rewind|next|previous|record      the tape recorder's keys
 *   osk on|off                the on-screen keyboard (osk.h) opened or closed; it is drawn into `shot` and the run's last frame
 *   osk click KEY             a click on the key's keycap (the pointer goes to the middle of the keycap, the button down for
 *                             a few frames, then up), through the path a mouse click takes: the position found, the key pressed
 *   osk pad up|down|left|right|a|b    a game controller's d-pad, A (held a few frames) or B: the selection moves (the answer is
 *                             the name of the key it is on), A presses it, B closes the keyboard
 *   osk keys                  one line for each keycap: x y width height in the frame's pixels, then the key's name
 *   nmi                       the NMI button
 *   reset [power|keys]        the reset button, or a power cycle; keys: the reset button with the keys as they are held (a machine
 *                             that looks at the keyboard at reset: BREAK held)
 *   shot PATH.png             the frame as it is now, as a PNG
 *   text                      the characters on the screen (the setup screen's while it is open)
 *   expect TEXT               fails unless TEXT is on the screen now
 *   until SECONDS TEXT        runs until TEXT is on the screen; fails after SECONDS
 *   state save|load PATH      a saved state
 *   rewind on|off             keep the rewind ring (a headless run does not, until this)
 *   rewind SECONDS            back through the ring by at least SECONDS of guest time, then on from there
 *   speed full|normal         as fast as the host allows, or 100 % (the sound is dropped at full speed)
 *   peek ADDR [COUNT]         bytes of memory (hex address; from 100000 the RAM pages by their 128K number, machine.h)
 *   poke ADDR BYTE...         writes bytes (hex)
 *   trace PATH | trace off    the I/O cycles and interrupts to a file
 *   snapshot load|save PATH   a snapshot file (sna, z80 or szx) put into the machine, or written from it
 *   cheat load PATH           a .pok file of trainers (numbered from 1)
 *   cheat on N [VALUE]        trainer N on (VALUE, 0 to 255, for a trainer that asks); cheat off N: off
 *   quit [STATUS]             ends the run with an exit status (0)
 *
 * A script stops at the first command that fails, with its line number,
 * and the run ends with status 1; its end ends the run with status 0
 * unless a control channel is open. On the control channel a command is
 * answered when it is done: {"ok":true,"frame":N} with "text" or "data"
 * where the command has a result, or {"ok":false,"error":"..."}; the
 * machine stands still between commands when the run is headless, so a run
 * is the same every time. ZetRo's own code. */
#ifndef ZR_HARNESS_H
#define ZR_HARNESS_H

#include "../core/machine.h"

typedef struct zr_harness zr_harness;

typedef struct zr_harness_host {
    void *ctx;
    /* The machine now (the shell may have built it again). */
    zr_machine *(*machine)(void *ctx);
    /* 0 with the reason when they cannot be done. */
    int (*insert)(void *ctx, unsigned slot, const char *path, char *error, size_t capacity);
    int (*eject)(void *ctx, unsigned slot);
    int (*shot)(void *ctx, const char *path, char *error, size_t capacity);
    int (*state_save)(void *ctx, const char *path, char *error, size_t capacity);
    int (*state_load)(void *ctx, const char *path, char *error, size_t capacity);
    /* 0 the reset button, 1 a power cycle, 2 the reset button with the keys left as they are held. */
    void (*reset)(void *ctx, int power_on);
    /* The control channel, NULL when there is none: a line that has come
     * (1), none yet (0) or the channel closed for good (-1); and a line out. */
    int (*control_read)(void *ctx, char *line, size_t capacity);
    void (*control_write)(void *ctx, const char *line);
    /* 1 when guest time stands still while the control channel is quiet (a headless run). */
    int deterministic;
    /* rewind's argument (on, off or seconds); 0 with the reason. */
    int (*rewind)(void *ctx, const char *arg, char *error, size_t capacity);
    void (*speed)(void *ctx, int full);
    /* What is on the screen when it is not the machine's (the setup screen), as text; 0: the machine's is. NULL: always. */
    size_t (*screen_text)(void *ctx, char *out, size_t capacity);
    /* snapshot load|save: a snapshot file put into the machine, or the machine written to one (the ending says the format).
     * cheat: a .pok file loaded; trainer `index` (0 for the first) switched on, with the value a trainer that asks needs
     * (-1: none given), or off. 0 with the reason. NULL: the run has none. */
    int (*snapshot_load)(void *ctx, const char *path, char *error, size_t capacity);
    int (*snapshot_save)(void *ctx, const char *path, char *error, size_t capacity);
    int (*cheat_load)(void *ctx, const char *path, char *error, size_t capacity);
    int (*cheat_set)(void *ctx, size_t index, int on, int value, char *error, size_t capacity);
    /* The on-screen keyboard: one of ZR_HARNESS_OSK_*, with its argument (a key's name, a pad word). 1 when done, the text it
     * answers in `text` (a key's name, the keycaps); 0 with the reason in `text`. NULL: this run has none. */
    int (*osk)(void *ctx, int op, const char *arg, char *text, size_t capacity);
    /* drop: the file as the window's drop takes it (the reasons it was not taken go to the status line and the console).
     * NULL: the run takes none. */
    int (*drop)(void *ctx, const char *path, char *error, size_t capacity);
} zr_harness_host;
enum { ZR_HARNESS_OSK_ON, ZR_HARNESS_OSK_OFF, ZR_HARNESS_OSK_MOUSE_DOWN, ZR_HARNESS_OSK_MOUSE_UP, ZR_HARNESS_OSK_PAD,
       ZR_HARNESS_OSK_PAD_RELEASE, ZR_HARNESS_OSK_KEYS };

/* `script` NULL: none. NULL with the reason when the script cannot be read. */
zr_harness *zr_harness_open(const zr_harness_host *host, const char *script, char *error, size_t capacity);
void zr_harness_close(zr_harness *h);

/* Before each frame: does what is due. */
enum {
    ZR_HARNESS_RUN,             /* run a frame of guest time */
    ZR_HARNESS_IDLE,            /* nothing to do yet: guest time stands still */
    ZR_HARNESS_QUIT             /* the run is over: zr_harness_status */
};
int zr_harness_step(zr_harness *h);
/* The exit status of a run that the harness ended, and why it failed ("" when it did not). */
int zr_harness_status(const zr_harness *h);
const char *zr_harness_error(const zr_harness *h);

#endif /* ZR_HARNESS_H */
