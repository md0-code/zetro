/* The host's game controllers (SDL's GameController API) as joysticks of
 * the machine: a controller's d-pad and left stick (beyond a dead zone,
 * stick.h) are the directions, A is fire and B is fire 2. They are opened
 * in a visible window only; a headless run has none. A controller keeps its
 * number (1 to ZR_PADS_MAX, in the order the host reported them) while it
 * is plugged in; one that is unplugged leaves its number empty, and its
 * socket released, until the next controller takes the first free one.
 *
 * zr_pads_event and zr_pads_repeat also turn controller input into the
 * events a setup screen takes (zr_pad_key: d-pad and stick move, A accepts,
 * B goes back, the shoulder buttons change a value, Start opens and closes
 * the screen); nothing is wired to them yet.
 *
 * pads.c is the only file besides the desktop that uses SDL; this header
 * needs none. ZetRo's own code. */
#ifndef ZR_PADS_H
#define ZR_PADS_H

#include "../core/config.h"

#include <stdint.h>

#define ZR_PADS_MAX ZR_CONFIG_PADS

/* What a setup screen takes from a controller. */
typedef enum zr_pad_key {
    ZR_PAD_NONE,
    ZR_PAD_UP, ZR_PAD_DOWN, ZR_PAD_LEFT, ZR_PAD_RIGHT,
    ZR_PAD_A,                           /* accept */
    ZR_PAD_B,                           /* back */
    ZR_PAD_SHOULDER_LEFT,               /* the value of the selected option goes back */
    ZR_PAD_SHOULDER_RIGHT,              /* ... and on */
    ZR_PAD_START,                       /* open and close the setup screen (the Guide button too) */
    ZR_PAD_BACK                         /* Select / Back: open and close the on-screen keyboard (osk.h) */
} zr_pad_key;

typedef struct zr_pads {
    void *pad[ZR_PADS_MAX];             /* SDL_GameController *, NULL for a number with no controller */
    int held;                           /* the direction held (a zr_pad_key), ZR_PAD_NONE for none */
    uint32_t next_ms;                   /* when a held direction repeats */
} zr_pads;

typedef enum zr_pads_result {
    ZR_PADS_NONE,
    ZR_PADS_CHANGED,                    /* a controller came or went */
    ZR_PADS_KEY                         /* a setup screen key is set */
} zr_pads_result;

/* Starts the controller subsystem and opens those present; 0 when SDL has none to offer. */
int zr_pads_open(zr_pads *p);
void zr_pads_close(zr_pads *p);

/* A controller event (device added or removed, button down); anything else is ignored. `key` may be NULL. */
union SDL_Event;
zr_pads_result zr_pads_event(zr_pads *p, const union SDL_Event *e, zr_pad_key *key);
/* A direction held on a d-pad or stick repeats like a key: after 400 ms, then every 120. 1 when `key` is set. */
int zr_pads_repeat(zr_pads *p, uint32_t now_ms, zr_pad_key *key);

/* 1 when controller `index` (0 to ZR_PADS_MAX - 1: the number minus one) is plugged in; its name ("" when it is not). */
int zr_pads_present(const zr_pads *p, unsigned index);
const char *zr_pads_name(const zr_pads *p, unsigned index);
/* What controller `index` holds as a joystick: ZR_JOY_* bits (machine.h), 0 for none or an empty number. */
unsigned zr_pads_joystick(const zr_pads *p, unsigned index);

/* Whether the host has a key of this name (SDL's names: "Up", "Left Ctrl"); what the configuration's
 * reader checks controllers.keys with (zr_config_set_key_check). Needs no zr_pads_open. */
int zr_host_key_known(const char *name);

#endif /* ZR_PADS_H */
