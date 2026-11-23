/* The running set of cheats: the trainers of a .pok file (core/media/pok.h), which of them are
 * on, and what switching one on or off does to the machine. No SDL: the shell (desktop.c) owns one set, loads a file
 * into it, and calls zr_cheats_frame before every frame; the setup screen's Cheats page and the harness's `cheat`
 * command switch the trainers through the shell.
 *
 * Switching a trainer on writes each of its pokes through zr_machine_poke, after remembering the byte that was there
 * (zr_machine_peek); switching it off puts back the original value the file gives, or where the file gives none (0)
 * the byte that was remembered, the last poke first. While a trainer is on its pokes are written again once a frame,
 * before the frame runs, because programs write over their own memory. A poke whose value is 256 uses the value the
 * user gives when switching the trainer on (0 to 255, one for all such pokes of the trainer). A poke of a bank other
 * than 8 is a byte of a 128K RAM page, reached as machine.h's ZR_PAGE_ADDRESS says: a machine that does not have the
 * page (a 48K machine has 5, 2 and 0) makes the trainer unusable. A trainer is also unusable when the file damaged it
 * (pok.h). The set is not part of a saved state and is untouched by loading one or by rewinding: the frame's
 * writes keep it on.
 *
 * ZetRo's own code. */
#ifndef ZR_CHEATS_H
#define ZR_CHEATS_H

#include "../core/machine.h"
#include "../core/media.h"
#include "../core/media/pok.h"

typedef struct zr_cheat {
    int usable;                         /* 0: see `problem` and `line` */
    unsigned line;                      /* the line of the .pok file that spoiled the trainer */
    char problem[64];
    int asks;                           /* a poke has the value 256: switching on needs a value */
    int on;
    unsigned value;                     /* the value given for 256 while it is on */
    uint8_t *saved;                     /* the bytes the pokes replaced (one for each poke) */
} zr_cheat;

typedef struct zr_cheats {
    zr_pok pok;
    zr_cheat *cheat;                    /* one for each trainer of `pok` */
    char file[ZR_MEDIA_PATH];           /* the .pok file's path; "" when nothing is loaded */
} zr_cheats;

void zr_cheats_init(zr_cheats *c);
/* Drops the set: every trainer that is on is switched off first when `machine` is given (NULL: the machine is gone). */
void zr_cheats_clear(zr_cheats *c, zr_machine *machine);

/* Loads a .pok file in place of the set (what was on is switched off first). 0 with the reason, the old set kept. The
 * trainers the machine cannot take are marked unusable. */
int zr_cheats_load(zr_cheats *c, zr_machine *machine, const char *path, char *error, size_t capacity);
/* The same from the bytes of a file, called `path` (checks, and the shell's loading). */
int zr_cheats_load_bytes(zr_cheats *c, zr_machine *machine, const char *path, const uint8_t *bytes, size_t size, char *error, size_t capacity);

/* The .pok file that belongs to a tape: NAME.pok beside it, else POKES/NAME.pok or pokes/NAME.pok in its folder (NAME
 * the tape's file name without its ending; the case is the host's). 1 with its path in `out`, 0 when there is none. */
int zr_cheats_find(const char *tape_path, char *out, size_t capacity);

size_t zr_cheats_count(const zr_cheats *c);
const char *zr_cheats_name(const zr_cheats *c, size_t index);

/* Switches trainer `index` on (with `value` 0 to 255 for a trainer that asks, else -1) or off. On failure 0 with the reason and
 * the trainer as it was. */
int zr_cheats_set(zr_cheats *c, zr_machine *machine, size_t index, int on, int value, char *error, size_t capacity);
/* Before a frame: the pokes of every trainer that is on, written again. */
void zr_cheats_frame(zr_cheats *c, zr_machine *machine);

#endif /* ZR_CHEATS_H */
