/* A joystick's directions from host positions (stick.h). ZetRo's own code. */
#include "stick.h"

unsigned zr_stick_directions(int x, int y) {
    unsigned held = 0;
    if (x <= -ZR_STICK_DEAD_ZONE) held |= ZR_JOY_LEFT;
    else if (x >= ZR_STICK_DEAD_ZONE) held |= ZR_JOY_RIGHT;
    if (y <= -ZR_STICK_DEAD_ZONE) held |= ZR_JOY_UP;
    else if (y >= ZR_STICK_DEAD_ZONE) held |= ZR_JOY_DOWN;
    return held;
}

unsigned zr_stick_resolve(unsigned held) {
    if ((held & (ZR_JOY_LEFT | ZR_JOY_RIGHT)) == (ZR_JOY_LEFT | ZR_JOY_RIGHT)) held &= ~(unsigned)(ZR_JOY_LEFT | ZR_JOY_RIGHT);
    if ((held & (ZR_JOY_UP | ZR_JOY_DOWN)) == (ZR_JOY_UP | ZR_JOY_DOWN)) held &= ~(unsigned)(ZR_JOY_UP | ZR_JOY_DOWN);
    return held;
}
