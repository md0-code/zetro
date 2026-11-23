/* Map libretro keyboard codes to the host names in the board's key tables. */
#ifndef LIBRETRO_HOST_KEYS_H
#define LIBRETRO_HOST_KEYS_H

#include <libretro.h>
#include <string.h>

static unsigned host_key(const char *name)
{
    if (!name || !*name)
        return RETROK_UNKNOWN;
    if (!name[1]) {
        unsigned ch = (unsigned char)name[0];
        return ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch;
    }
    static const struct { const char *name; unsigned code; } special[] = {
        {"Space", RETROK_SPACE}, {"Return", RETROK_RETURN}, {"Escape", RETROK_ESCAPE},
        {"Tab", RETROK_TAB}, {"Backspace", RETROK_BACKSPACE}, {"Delete", RETROK_DELETE},
        {"Left Shift", RETROK_LSHIFT}, {"Right Shift", RETROK_RSHIFT},
        {"Left Ctrl", RETROK_LCTRL}, {"Right Ctrl", RETROK_RCTRL},
        {"Left Alt", RETROK_LALT}, {"Right Alt", RETROK_RALT},
        {"Left GUI", RETROK_LSUPER}, {"Right GUI", RETROK_RSUPER},
        {"CapsLock", RETROK_CAPSLOCK}, {"ScrollLock", RETROK_SCROLLOCK},
        {"Up", RETROK_UP}, {"Down", RETROK_DOWN}, {"Left", RETROK_LEFT}, {"Right", RETROK_RIGHT},
        {"Home", RETROK_HOME}, {"End", RETROK_END}, {"PageUp", RETROK_PAGEUP},
        {"PageDown", RETROK_PAGEDOWN}, {"Insert", RETROK_INSERT}, {"Pause", RETROK_PAUSE},
        {"Keypad Enter", RETROK_KP_ENTER}, {"Keypad .", RETROK_KP_PERIOD}, {"Keypad /", RETROK_KP_DIVIDE},
        {"Keypad *", RETROK_KP_MULTIPLY}, {"Keypad -", RETROK_KP_MINUS},
        {"Keypad +", RETROK_KP_PLUS}, {"Keypad =", RETROK_KP_EQUALS}
    };
    for (size_t i = 0; i < sizeof special / sizeof special[0]; ++i)
        if (!strcmp(special[i].name, name))
            return special[i].code;
    if (name[0] == 'F' && name[1] >= '1' && name[1] <= '9') {
        if (!name[2])
            return RETROK_F1 + (unsigned)(name[1] - '1');
        if (name[1] == '1' && name[2] >= '0' && name[2] <= '5' && !name[3])
            return RETROK_F10 + (unsigned)(name[2] - '0');
    }
    if (!strncmp(name, "Keypad ", 7) && name[7] >= '0' && name[7] <= '9' && !name[8])
        return RETROK_KP0 + (unsigned)(name[7] - '0');
    return RETROK_UNKNOWN;
}

#endif
