/* Short messages: the text and how long it stays on the status line. */

#ifndef ZR_OVERLAY_H
#define ZR_OVERLAY_H

#include <stdint.h>

typedef struct zr_osd {
    char text[128];
    uint32_t until_ms;
} zr_osd;

/* Shows a message for a couple of seconds from now_ms. */
void zr_osd_show(zr_osd *o, uint32_t now_ms, const char *fmt, ...);

#endif /* ZR_OVERLAY_H */
