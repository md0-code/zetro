/* The host's game controllers (pads.h). ZetRo's own code. */
#include "pads.h"
#include "../core/machine.h"
#include "../core/stick.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>

#define REPEAT_FIRST_MS 400
#define REPEAT_NEXT_MS 120

static SDL_GameController *controller(const zr_pads *p, unsigned index) {
    return index < ZR_PADS_MAX ? (SDL_GameController *)p->pad[index] : NULL;
}

static int known(const zr_pads *p, SDL_JoystickID id) {
    for (unsigned i = 0; i < ZR_PADS_MAX; ++i)
        if (controller(p, i) && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller(p, i))) == id) return 1;
    return 0;
}

/* The host's device `device_index` into the first free number; 1 when a controller was added. */
static int add(zr_pads *p, int device_index) {
    if (!SDL_IsGameController(device_index)) return 0;
    unsigned free_number = 0;
    while (free_number < ZR_PADS_MAX && p->pad[free_number]) ++free_number;
    if (free_number == ZR_PADS_MAX) return 0;
    SDL_GameController *c = SDL_GameControllerOpen(device_index);
    if (!c) return 0;
    if (known(p, SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c)))) { SDL_GameControllerClose(c); return 0; }
    p->pad[free_number] = c;
    return 1;
}

int zr_pads_open(zr_pads *p) {
    memset(p, 0, sizeof *p);
    p->held = ZR_PAD_NONE;
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) return 0;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) add(p, i);
    return 1;
}

void zr_pads_close(zr_pads *p) {
    for (unsigned i = 0; i < ZR_PADS_MAX; ++i) {
        if (controller(p, i)) SDL_GameControllerClose(controller(p, i));
        p->pad[i] = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}

zr_pads_result zr_pads_event(zr_pads *p, const union SDL_Event *e, zr_pad_key *key) {
    zr_pad_key ignored;
    if (!key) key = &ignored;
    *key = ZR_PAD_NONE;
    if (e->type == SDL_CONTROLLERDEVICEADDED) return add(p, e->cdevice.which) ? ZR_PADS_CHANGED : ZR_PADS_NONE;
    if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        for (unsigned i = 0; i < ZR_PADS_MAX; ++i) {
            if (!controller(p, i) || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller(p, i))) != e->cdevice.which) continue;
            SDL_GameControllerClose(controller(p, i));
            p->pad[i] = NULL;
            p->held = ZR_PAD_NONE;
            return ZR_PADS_CHANGED;
        }
        return ZR_PADS_NONE;
    }
    if (e->type != SDL_CONTROLLERBUTTONDOWN) return ZR_PADS_NONE;
    switch (e->cbutton.button) {
    case SDL_CONTROLLER_BUTTON_A: *key = ZR_PAD_A; break;
    case SDL_CONTROLLER_BUTTON_B: *key = ZR_PAD_B; break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: *key = ZR_PAD_SHOULDER_LEFT; break;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: *key = ZR_PAD_SHOULDER_RIGHT; break;
    case SDL_CONTROLLER_BUTTON_START:
    case SDL_CONTROLLER_BUTTON_GUIDE: *key = ZR_PAD_START; break;
    case SDL_CONTROLLER_BUTTON_BACK: *key = ZR_PAD_BACK; break;
    default: return ZR_PADS_NONE;       /* the d-pad repeats through zr_pads_repeat */
    }
    return ZR_PADS_KEY;
}

/* The direction a d-pad or stick of any controller holds, ZR_PAD_NONE for none. */
static int direction(const zr_pads *p) {
    for (unsigned i = 0; i < ZR_PADS_MAX; ++i) {
        SDL_GameController *c = controller(p, i);
        if (!c) continue;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) return ZR_PAD_UP;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) return ZR_PAD_DOWN;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) return ZR_PAD_LEFT;
        if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) return ZR_PAD_RIGHT;
        const int x = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(x) >= ZR_STICK_DEAD_ZONE || abs(y) >= ZR_STICK_DEAD_ZONE) {
            if (abs(y) >= abs(x)) return y < 0 ? ZR_PAD_UP : ZR_PAD_DOWN;
            return x < 0 ? ZR_PAD_LEFT : ZR_PAD_RIGHT;
        }
    }
    return ZR_PAD_NONE;
}

int zr_pads_repeat(zr_pads *p, uint32_t now_ms, zr_pad_key *key) {
    const int d = direction(p);
    if (d == ZR_PAD_NONE) { p->held = ZR_PAD_NONE; return 0; }
    if (d != p->held) {
        p->held = d;
        p->next_ms = now_ms + REPEAT_FIRST_MS;
    } else if ((int32_t)(now_ms - p->next_ms) >= 0) p->next_ms = now_ms + REPEAT_NEXT_MS;
    else return 0;
    *key = (zr_pad_key)d;
    return 1;
}

int zr_pads_present(const zr_pads *p, unsigned index) { return controller(p, index) != NULL; }

const char *zr_pads_name(const zr_pads *p, unsigned index) {
    SDL_GameController *c = controller(p, index);
    const char *name = c ? SDL_GameControllerName(c) : NULL;
    if (!c) return "";
    return name && *name ? name : "Game controller";
}

unsigned zr_pads_joystick(const zr_pads *p, unsigned index) {
    SDL_GameController *c = controller(p, index);
    if (!c) return 0;
    unsigned held = 0;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP)) held |= ZR_JOY_UP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) held |= ZR_JOY_DOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) held |= ZR_JOY_LEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) held |= ZR_JOY_RIGHT;
    held |= zr_stick_directions(SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX), SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY));
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A)) held |= ZR_JOY_FIRE;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B)) held |= ZR_JOY_FIRE2;
    return zr_stick_resolve(held);
}

int zr_host_key_known(const char *name) { return SDL_GetScancodeFromName(name) != SDL_SCANCODE_UNKNOWN; }
