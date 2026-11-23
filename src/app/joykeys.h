/* The keyboard joystick: six keys of the host's keyboard (up, down, left,
 * right, fire, fire 2; controllers.keys in the configuration) that, while it
 * is switched on, are a joystick's switches and no longer reach the
 * machine's keyboard. The shell's F11 key switches it on and off at any
 * time. No SDL: keys are the host's scancodes as integers, resolved from the
 * configuration's key names by the shell. ZetRo's own code. */
#ifndef ZR_JOYKEYS_H
#define ZR_JOYKEYS_H

#include "../core/config.h"

typedef struct zr_joykeys {
    int key[ZR_CONFIG_STICK_KEYS];      /* the host key of each function (ZR_STICK_UP...), -1: none */
    uint8_t down[ZR_CONFIG_STICK_KEYS]; /* pressed while the joystick was on and not let go yet */
    int on;
} zr_joykeys;

/* `keys` are the host scancodes in the order of ZR_STICK_UP to ZR_STICK_FIRE2. */
void zr_joykeys_init(zr_joykeys *j, const int keys[ZR_CONFIG_STICK_KEYS], int on);
/* New keys (a changed configuration): nothing is held afterwards. */
void zr_joykeys_set_keys(zr_joykeys *j, const int keys[ZR_CONFIG_STICK_KEYS]);

/* A key of the host went down or up (not its repeats). 1 when the joystick took it, so it does
 * not reach the machine: a press while the joystick is on, and the release of a key it took. A key
 * pressed before the joystick was switched on is the machine's until it is let go. */
int zr_joykeys_key(zr_joykeys *j, int scancode, int down);

/* Switches it on or off; the directions and buttons held are let go either way. Returns 1 when on. */
int zr_joykeys_switch(zr_joykeys *j);
void zr_joykeys_set(zr_joykeys *j, int on);
int zr_joykeys_on(const zr_joykeys *j);

/* The directions and buttons held now, ZR_JOY_* (machine.h); opposite directions cancel. */
unsigned zr_joykeys_held(const zr_joykeys *j);

#endif /* ZR_JOYKEYS_H */
