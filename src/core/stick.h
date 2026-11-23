/* A joystick's directions from the positions a host device reports. Every
 * joystick of these machines is digital (a switch for each direction and
 * for each fire button: ZR_JOY_* in machine.h), so a game controller's
 * analogue stick becomes directions beyond a dead zone, and opposite
 * directions held together cancel as the switches of a real joystick
 * cannot close both. No SDL: the shell reads the host's axes and calls
 * these. ZetRo's own code (no calibration: the machines have no analogue joystick). */
#ifndef ZR_STICK_H
#define ZR_STICK_H

#include "machine.h"

/* A stick position runs from -32768 (left, up) to 32767 (right, down), 0 at rest. An axis counts
 * as a direction once it is this far (about half of the travel) from rest. */
#define ZR_STICK_DEAD_ZONE 16000

/* The directions of a stick position: ZR_JOY_LEFT or ZR_JOY_RIGHT for x, ZR_JOY_UP or ZR_JOY_DOWN
 * for y, both axes at once for a diagonal. */
unsigned zr_stick_directions(int x, int y);

/* `held` (ZR_JOY_* bits) with each pair of opposite directions both held removed. */
unsigned zr_stick_resolve(unsigned held);

#endif /* ZR_STICK_H */
