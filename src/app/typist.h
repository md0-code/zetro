/* The shell's typist: types a text on the machine's keyboard through the
 * family's typing table (machine.h), one key after another, as the
 * harness's `type` does: a key goes down (its shift key first), stays down
 * two frames, goes up and stays up as long. One step per frame.
 *
 * Auto-load uses it. When a tape is put into the machine from the shell (a
 * file dropped on the window, later the setup screen), the slot's autoload is on
 * and the machine has an auto-load text (zr_machine_autoload: the model's
 * zr_model_desc.autoload, or what the board says from its state), the
 * machine is power cycled (unless the board says not), the typist waits
 * until it is ready, types the text and presses PLAY on the tape
 * recorder. The machine interface has
 * no "ready" signal, so ready is this: the screen's text has not changed
 * for READY_STABLE frames in a row and at least READY_MIN frames have run
 * since the power cycle; after READY_LIMIT frames it types anyway. Media
 * put in by the harness or named in the configuration never auto-load, so
 * scripts stay exact. No SDL. ZetRo's own code. */
#ifndef ZR_TYPIST_H
#define ZR_TYPIST_H

#include "../core/config.h"
#include "../core/machine.h"

#define ZR_TYPIST_TEXT 256              /* the longest text (characters) */
#define ZR_TYPIST_KEY_FRAMES 2u         /* a key is held this many frames, then up as many */
#define ZR_TYPIST_READY_MIN 50u
#define ZR_TYPIST_READY_STABLE 25u
#define ZR_TYPIST_READY_LIMIT 500u

typedef struct zr_typist {
    int state;                          /* idle, waiting for the machine to be ready, typing */
    char text[ZR_TYPIST_TEXT];          /* what is typed, escapes resolved */
    size_t length, at;
    unsigned phase;                     /* frames into the key at `at`, 0 to 2 * ZR_TYPIST_KEY_FRAMES */
    int held;                           /* the key (and its shift) of `at` is down */
    uint8_t key, shift;
    /* auto-load */
    int autoload;                       /* 1: press PLAY of `slot` when the text is typed */
    unsigned slot;
    unsigned frames, stable;            /* since the power cycle; frames the screen has not changed */
    uint64_t hash;
    size_t hash_length;
} zr_typist;

void zr_typist_init(zr_typist *t);

/* Starts typing `text` (the harness's notation: a line feed or \n is Enter, \b is delete, \\ a
 * backslash); what was being typed is cancelled. 0, with nothing started, when the text is too long
 * or has a character the keyboard cannot type. */
int zr_typist_start(zr_typist *t, zr_machine *m, const char *text);

/* A tape has been put into slot `slot` by the shell. Starts the auto-load when `config` has
 * the slot's autoload on, the slot is a tape slot of the machine and the machine has an auto-load text: the
 * machine is power cycled (zr_machine_reset(m, 1); the shell lets go of its own keys first) unless
 * the board says it must not be (a BASIC running in RAM), and the typist waits for a readable,
 * still screen, then types, then presses PLAY. 1 when it started. A new insert while one is
 * in progress starts it again. */
int zr_typist_autoload(zr_typist *t, zr_machine *m, const zr_config *config, unsigned slot);

/* Stops what is in progress: no more keys, no PLAY; a key held is let go. */
void zr_typist_cancel(zr_typist *t, zr_machine *m);
/* 1 while there is anything to wait for or type. */
int zr_typist_busy(const zr_typist *t);

/* Once per frame, before the machine runs it. */
void zr_typist_step(zr_typist *t, zr_machine *m);

/* Save and restore pending auto-load, including any key still held. */
void zr_typist_save(const zr_typist *t, zr_state_writer *w);
int zr_typist_load(zr_typist *t, zr_state_reader *r, const zr_family *family);

#endif /* ZR_TYPIST_H */
