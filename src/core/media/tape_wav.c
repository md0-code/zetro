/* WAV files (RIFF WAVE, PCM) as tapes: 8 or 16 bit samples, one or two
 * channels (averaged), any sample rate. The level is the sample above the
 * signal's midpoint, with a hysteresis so that noise around it does not make
 * edges: the level goes high above the midpoint plus the margin and low below
 * the midpoint minus it. Both come from the signal itself, not from the format
 * (a recording has a DC offset and any loudness, and its silences are noise):
 * the midpoint is halfway between the signal's low and high peaks (the 0.2 %
 * and 99.8 % points of the samples, so that a click does not decide), the margin
 * a quarter of the distance from there to a peak. A file with no signal worth
 * the name (peaks less than 1 % of full scale apart) keeps the format's midpoint
 * (128 for 8 bit, 0 for 16 bit) and a margin of 3 % of full scale. The pulses
 * are the runs between edges, in T-states of 3.5 MHz with the remainder of each
 * conversion carried on. ZetRo's own code, written from the RIFF WAVE layout. */
#include "tape_impl.h"

#include <stdint.h>
#include <string.h>

/* The frame at `p` as one number on the 16-bit scale (-32768 to 32767): the channels averaged, 8-bit samples centred and scaled. */
static int wav_sample(const uint8_t *p, unsigned channels, unsigned bits) {
    int v = 0;
    for (unsigned c = 0; c < channels; ++c) {
        if (bits == 8) v += ((int)p[c] - 128) * 256;
        else v += (int16_t)(uint16_t)zr_rd16(p + 2 * c);
    }
    return v / (int)channels;
}

int zr_tape_read_wav(zr_tape *t, const uint8_t *d, size_t n, char *error, size_t capacity) {
    size_t pos = 12;
    unsigned format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *samples = NULL;
    size_t sample_bytes = 0;
    int have_format = 0;
    while (n - pos >= 8) {
        const uint8_t *chunk = d + pos;
        size_t size = zr_rd32(chunk + 4);
        pos += 8;
        if (size > n - pos) size = n - pos;             /* a data chunk that claims more than the file has */
        if (!memcmp(chunk, "fmt ", 4)) {
            if (size < 16) return ZR_TAPE_FAIL("Damaged WAV file: the format chunk is too short");
            format = zr_rd16(d + pos);
            channels = zr_rd16(d + pos + 2);
            rate = zr_rd32(d + pos + 4);
            bits = zr_rd16(d + pos + 14);
            if (format == 0xFFFE && size >= 26) format = zr_rd16(d + pos + 24);     /* extensible: the sub-format */
            have_format = 1;
        } else if (!memcmp(chunk, "data", 4)) {
            samples = d + pos;
            sample_bytes = size;
            break;
        }
        pos += size + (size & 1u);
        if (pos > n) break;
    }
    if (!have_format) return ZR_TAPE_FAIL("Damaged WAV file: there is no format chunk");
    if (!samples) return ZR_TAPE_FAIL("Damaged WAV file: there is no sample data");
    if (format != 1) return ZR_TAPE_FAIL("Unsupported WAV file: only PCM is read");
    if (channels < 1 || channels > 2) return ZR_TAPE_FAIL("Unsupported WAV file: only mono and stereo are read");
    if (bits != 8 && bits != 16) return ZR_TAPE_FAIL("Unsupported WAV file: only 8 and 16 bit samples are read");
    if (!rate) return ZR_TAPE_FAIL("Damaged WAV file: the sample rate is zero");

    const size_t width = (size_t)channels * (bits / 8);
    const size_t frames = sample_bytes / width;
    /* The signal's midpoint and margin, on the 16-bit scale: from the histogram of the samples. */
    enum { BUCKETS = 1024, SHIFT = 6, QUIET = 655, FIXED_MARGIN = 1024 };
    uint32_t histogram[BUCKETS];
    memset(histogram, 0, sizeof histogram);
    for (size_t f = 0; f < frames; ++f) ++histogram[(wav_sample(samples + f * width, channels, bits) + 32768) >> SHIFT];
    int low = 0, high = 0;
    for (uint64_t seen = 0, i = 0; i < BUCKETS; ++i) {
        const uint64_t before = seen;
        seen += histogram[i];
        if (before * 500u <= frames && seen * 500u > frames) low = (int)(i << SHIFT) - 32768;          /* the 0.2 % point */
        if (before * 500u < frames * 499u && seen * 500u >= frames * 499u) high = (int)((i + 1) << SHIFT) - 32768;    /* the 99.8 % point */
    }
    int mid = 0, margin = FIXED_MARGIN;
    if (high - low >= QUIET * 2) {
        mid = (low + high) / 2;
        margin = (high - low) / 8;
    }
    const uint32_t first = (uint32_t)t->runs_n;
    uint64_t carry = 0;
    uint32_t run = 0;                                   /* samples since the last edge */
    int level = -1;
    uint8_t start_level = 0;
    for (size_t f = 0; f < frames; ++f) {
        const int v = wav_sample(samples + f * width, channels, bits) - mid;
        if (level < 0) { level = v >= 0; start_level = (uint8_t)level; }
        const int now = v > margin ? 1 : v < -margin ? 0 : level;
        if (now != level) {
            const uint64_t x = carry + (uint64_t)run * ZR_TAPE_TSTATE_HZ;
            carry = x % rate;
            const uint64_t tstates = x / rate;
            if (!zr_tape_run(t, first, tstates > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)tstates, 1)) {
                t->runs_n = first;
                return ZR_TAPE_FAIL("The tape has too many pulses");
            }
            level = now;
            run = 0;
        }
        if (run < 0xFFFFFFFFu) ++run;
    }
    if (run) {                                          /* the last run, up to the end of the data */
        const uint64_t x = carry + (uint64_t)run * ZR_TAPE_TSTATE_HZ;
        const uint64_t tstates = x / rate;
        if (!zr_tape_run(t, first, tstates > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)tstates, 1)) {
            t->runs_n = first;
            return ZR_TAPE_FAIL("The tape has too many pulses");
        }
    }
    zr_tape_block *b = zr_tape_add_block(t, ZR_TB_AUDIO);
    if (!b) return ZR_TAPE_FAIL("The tape has too many blocks");
    b->flags = ZR_TF_LEVEL;
    b->level = start_level;
    b->u.r.first = first;
    b->u.r.count = (uint32_t)(t->runs_n - first);
    return 1;
}
