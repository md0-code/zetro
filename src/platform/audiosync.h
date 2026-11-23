/* Keeping the sound output in step with the machine: the machine makes sound at the rate of guest time, which the
 * shell paces to the host's monotonic clock; the sound device consumes at
 * its own clock. The two differ by some parts in ten thousand, so a queue
 * between them would grow until it overflows or run dry and crackle. The
 * resampler here converts the machine's rate to the device's with a ratio
 * nudged a hair either way (at most 0.5 %, well under what a speaker's tone
 * shows) by the queue's fill, which holds the latency at a target without
 * ever dropping or repeating a sample. No SDL: the queue's length is passed
 * in, so a test drives it with a simulated device. ZetRo's own code. */
#ifndef ZR_AUDIOSYNC_H
#define ZR_AUDIOSYNC_H

#include <stddef.h>
#include <stdint.h>

/* The queue is kept this long, in milliseconds of sound. */
#define ZR_AUDIOSYNC_TARGET_MS 60
/* Above this many times the target in the queue, input is dropped (the device stopped consuming). */
#define ZR_AUDIOSYNC_OVERFLOW 4

typedef struct zr_audiosync {
    double in_rate, out_rate;   /* machine and device rates, Hz */
    double target;              /* frames to keep queued */
    double level;               /* the queue's length, smoothed */
    double adjust;              /* the ratio's nudge now, -0.005 to 0.005 */
    double pos;                 /* input position of the next output frame, past `prev` */
    int16_t prev[2];            /* the last input frame */
    int primed;
} zr_audiosync;

void zr_audiosync_init(zr_audiosync *a, uint32_t in_rate, uint32_t out_rate);
/* The queue now holds `queued` output frames: updates the nudge. Call once per pump. */
void zr_audiosync_observe(zr_audiosync *a, size_t queued);
/* The most output frames `in` input frames can make (size the output buffer with it). */
size_t zr_audiosync_capacity(const zr_audiosync *a, size_t in);
/* Converts `in` stereo frames to the output rate at the ratio now in force, times
 * `gain` (0 to 256, 256 unity); returns the frames written to `out`. */
size_t zr_audiosync_convert(zr_audiosync *a, const int16_t *in, size_t frames, int16_t *out, unsigned gain);
/* The output frames of silence that bring an empty queue up to the target. */
size_t zr_audiosync_refill(const zr_audiosync *a, size_t queued);
/* Free running: the machine's sound is not a stream of its own then (guest time runs faster than the clock), so the
 * input frames to take from a source that makes sound at the pace of the clock, to bring a queue of `queued` output
 * frames up to the target and no further: none while the queue is full, so the source runs as fast as the device
 * consumes. At most `limit`. */
size_t zr_audiosync_wanted(const zr_audiosync *a, size_t queued, size_t limit);

#endif /* ZR_AUDIOSYNC_H */
