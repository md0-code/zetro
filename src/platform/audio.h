/* Host sound output: the machine's samples, pulled after each run slice,
 * converted to the output device's rate with a ratio that follows the
 * device's own clock (audiosync.h: the queue neither grows nor runs dry) and
 * queued at the volume asked for. */
#ifndef ZR_HOST_AUDIO_H
#define ZR_HOST_AUDIO_H

#include "../core/machine.h"
#include "audiosync.h"

#include <SDL.h>
#include <stdio.h>

typedef struct zr_host_audio {
    SDL_AudioDeviceID device;
    SDL_AudioSpec output;
    zr_audiosync sync;
    uint32_t rate;              /* the machine's rate the sync was set up for */
    int flowing;                /* samples have been queued since the last discard */
} zr_host_audio;

/* 0 when no output device opens; pumping then discards the samples. */
int zr_host_audio_open(zr_host_audio *audio);
/* Moves the machine's samples to the device at `volume` percent (0 mutes);
 * with `discard` (the machine runs faster than real time, or is not
 * running) they are taken and dropped. 0 on an SDL failure. */
int zr_host_audio_pump(zr_host_audio *audio, zr_machine *machine, unsigned volume, int discard);
void zr_host_audio_close(zr_host_audio *audio);

/* The machine's sound recorded to a WAV file instead (16-bit stereo PCM at
 * the machine's rate): zr_wav_pump after each run slice. */
typedef struct zr_wav {
    FILE *file;
    uint32_t rate, frames;
    int peak;                   /* loudest sample so far */
} zr_wav;
int zr_wav_open(zr_wav *wav, const char *path);
void zr_wav_pump(zr_wav *wav, zr_machine *machine);
/* Completes the header; 0 when the file could not be written. */
int zr_wav_close(zr_wav *wav);

#endif /* ZR_HOST_AUDIO_H */
