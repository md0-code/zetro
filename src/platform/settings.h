/* The presenter's settings.
 *
 * ZetRo reads its settings from zetro.yaml (src/core/config.c); the
 * shell fills this structure from the `video` section and hands it to the
 * presenter, which needs nothing else. */

#ifndef ZR_SETTINGS_H
#define ZR_SETTINGS_H

#include <stdbool.h>

#define ZR_PATH_MAX 1024

typedef enum zr_filter { ZR_FILTER_NEAREST, ZR_FILTER_LINEAR, ZR_FILTER_SHARP } zr_filter;
typedef enum zr_aspect { ZR_ASPECT_ORIGINAL, ZR_ASPECT_SQUARE } zr_aspect;

typedef struct zr_settings {
    bool fullscreen;            /* borderless, on the desktop */
    int window_scale;           /* the window at start: the picture times this */
    int integer_scale;          /* 0: fill the window; N: whole multiples up to N */
    int aspect;                 /* zr_aspect */
    int filter;                 /* zr_filter */
    bool crt;
    bool vsync;
} zr_settings;

#endif /* ZR_SETTINGS_H */
