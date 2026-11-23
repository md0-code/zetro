/* The keyboard joystick (joykeys.h). ZetRo's own code. */
#include "joykeys.h"
#include "../core/machine.h"
#include "../core/stick.h"

#include <string.h>

void zr_joykeys_set_keys(zr_joykeys *j, const int keys[ZR_CONFIG_STICK_KEYS]) {
    memcpy(j->key, keys, sizeof j->key);
    memset(j->down, 0, sizeof j->down);
}

void zr_joykeys_init(zr_joykeys *j, const int keys[ZR_CONFIG_STICK_KEYS], int on) {
    memset(j, 0, sizeof *j);
    zr_joykeys_set_keys(j, keys);
    j->on = on != 0;
}

static int function_of(const zr_joykeys *j, int scancode) {
    if (scancode < 0) return -1;
    for (int i = 0; i < ZR_CONFIG_STICK_KEYS; ++i) if (j->key[i] == scancode) return i;
    return -1;
}

int zr_joykeys_key(zr_joykeys *j, int scancode, int down) {
    const int f = function_of(j, scancode);
    if (f < 0) return 0;
    if (down) {
        if (!j->on) return 0;
        j->down[f] = 1;
        return 1;
    }
    if (!j->down[f]) return 0;
    j->down[f] = 0;
    return 1;
}

void zr_joykeys_set(zr_joykeys *j, int on) {
    j->on = on != 0;
    memset(j->down, 0, sizeof j->down);
}

int zr_joykeys_switch(zr_joykeys *j) {
    zr_joykeys_set(j, !j->on);
    return j->on;
}

int zr_joykeys_on(const zr_joykeys *j) { return j->on; }

unsigned zr_joykeys_held(const zr_joykeys *j) {
    static const unsigned BITS[ZR_CONFIG_STICK_KEYS] = {ZR_JOY_UP, ZR_JOY_DOWN, ZR_JOY_LEFT, ZR_JOY_RIGHT, ZR_JOY_FIRE, ZR_JOY_FIRE2};
    unsigned held = 0;
    for (int i = 0; i < ZR_CONFIG_STICK_KEYS; ++i) if (j->down[i]) held |= BITS[i];
    return zr_stick_resolve(held);
}
