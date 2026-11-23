/* The running set of cheats (cheats.h). ZetRo's own code. */
#include "cheats.h"
#include "../host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void zr_cheats_init(zr_cheats *c) { memset(c, 0, sizeof *c); }

/* The memory-space address of a poke: the CPU's own, or a byte of a RAM page (machine.h). */
static uint32_t address_of(const zr_pok_poke *p) {
    return p->bank & ZR_POK_CPU_BANK ? (uint32_t)p->address : ZR_PAGE_ADDRESS(p->bank & 7u, p->address);
}

static unsigned value_of(const zr_cheat *ch, const zr_pok_poke *p) { return p->value == ZR_POK_ASK ? ch->value : p->value; }

/* Puts the bytes of the first `count` pokes back, the last first. */
static void restore(const zr_cheat *ch, const zr_pok_trainer *t, zr_machine *m, size_t count) {
    while (count--) {
        const zr_pok_poke *p = &t->pokes[count];
        zr_machine_poke(m, address_of(p), p->original ? p->original : ch->saved[count]);
    }
}

static void free_set(zr_cheats *c) {
    for (size_t i = 0; c->cheat && i < c->pok.count; ++i) free(c->cheat[i].saved);
    free(c->cheat);
    zr_pok_free(&c->pok);
    c->cheat = NULL;
    c->file[0] = 0;
}

static void switch_all_off(zr_cheats *c, zr_machine *m) {
    for (size_t i = 0; m && c->cheat && i < c->pok.count; ++i)
        if (c->cheat[i].on) {
            restore(&c->cheat[i], &c->pok.trainers[i], m, c->pok.trainers[i].count);
            c->cheat[i].on = 0;
        }
}

void zr_cheats_clear(zr_cheats *c, zr_machine *machine) {
    switch_all_off(c, machine);
    free_set(c);
}

/* --- loading ------------------------------------------------------------------------------- */

int zr_cheats_load_bytes(zr_cheats *c, zr_machine *machine, const char *path, const uint8_t *bytes, size_t size, char *error, size_t capacity) {
    zr_pok pok;
    if (!zr_pok_parse(bytes, size, &pok, error, capacity)) return 0;
    zr_cheat *cheat = calloc(pok.count, sizeof *cheat);
    if (!cheat) {
        zr_pok_free(&pok);
        snprintf(error, capacity, "Out of memory");
        return 0;
    }
    for (size_t i = 0; i < pok.count; ++i) {
        const zr_pok_trainer *t = &pok.trainers[i];
        zr_cheat *ch = &cheat[i];
        ch->usable = t->usable;
        ch->line = t->line;
        snprintf(ch->problem, sizeof ch->problem, "%s", t->problem);
        for (size_t k = 0; k < t->count; ++k) {
            if (t->pokes[k].value == ZR_POK_ASK) ch->asks = 1;
            /* a RAM page the machine does not have: the trainer cannot be used here */
            uint8_t probe;
            if (ch->usable && !(t->pokes[k].bank & ZR_POK_CPU_BANK) && !zr_machine_peek(machine, address_of(&t->pokes[k]), &probe)) {
                ch->usable = 0;
                ch->line = t->pokes[k].line;
                snprintf(ch->problem, sizeof ch->problem, "this machine has no RAM page %u", t->pokes[k].bank & 7u);
            }
        }
        if (t->count) ch->saved = calloc(t->count, 1);
        if (t->count && !ch->saved) {
            for (size_t j = 0; j <= i; ++j) free(cheat[j].saved);
            free(cheat);
            zr_pok_free(&pok);
            snprintf(error, capacity, "Out of memory");
            return 0;
        }
    }
    zr_cheats_clear(c, machine);
    c->pok = pok;
    c->cheat = cheat;
    snprintf(c->file, sizeof c->file, "%s", path);
    return 1;
}

int zr_cheats_load(zr_cheats *c, zr_machine *machine, const char *path, char *error, size_t capacity) {
    zr_media *file = zr_media_open(path, 1, error, capacity);
    if (!file) return 0;
    const int ok = zr_cheats_load_bytes(c, machine, path, file->data, file->size, error, capacity);
    zr_media_close(file);
    return ok;
}

/* --- the file that belongs to a tape ---------------------------------------------------------------- */

int zr_cheats_find(const char *tape_path, char *out, size_t capacity) {
    const char sep = zr_host_separator();
    size_t folder = 0, dot = 0;
    for (size_t i = 0; tape_path[i]; ++i) {
        if (tape_path[i] == '/' || tape_path[i] == sep) folder = i + 1;
    }
    const size_t end = strlen(tape_path);
    dot = end;
    for (size_t i = folder; i < end; ++i)
        if (tape_path[i] == '.') dot = i;
    if (dot == folder) dot = end;                       /* a name that starts with a dot has no ending */
    const size_t length = dot - folder;
    if (!length) return 0;
    static const char *const SUBFOLDERS[] = {"", "POKES", "pokes"};
    static const char *const ENDINGS[] = {"pok", "POK"};
    for (size_t s = 0; s < sizeof SUBFOLDERS / sizeof *SUBFOLDERS; ++s)
        for (size_t e = 0; e < sizeof ENDINGS / sizeof *ENDINGS; ++e) {
            char path[ZR_MEDIA_PATH];
            const int n = snprintf(path, sizeof path, "%.*s%s%s%.*s.%s", (int)folder, tape_path, SUBFOLDERS[s], *SUBFOLDERS[s] ? (const char[]){sep, 0} : "",
                                   (int)length, tape_path + folder, ENDINGS[e]);
            if (n > 0 && (size_t)n < sizeof path && zr_host_file_exists(path)) {
                snprintf(out, capacity, "%s", path);
                return 1;
            }
        }
    return 0;
}

/* --- the trainers ------------------------------------------------------------------------------------- */

size_t zr_cheats_count(const zr_cheats *c) { return c->pok.count; }

const char *zr_cheats_name(const zr_cheats *c, size_t index) { return index < c->pok.count ? c->pok.trainers[index].name : ""; }

int zr_cheats_set(zr_cheats *c, zr_machine *m, size_t index, int on, int value, char *error, size_t capacity) {
    if (index >= c->pok.count) {
        snprintf(error, capacity, "There is no trainer %zu", index + 1);
        return 0;
    }
    zr_cheat *ch = &c->cheat[index];
    const zr_pok_trainer *t = &c->pok.trainers[index];
    if (!on) {
        if (ch->on) restore(ch, t, m, t->count);
        ch->on = 0;
        return 1;
    }
    if (!ch->usable) {
        snprintf(error, capacity, "Trainer %zu cannot be used: %s (line %u)", index + 1, ch->problem, ch->line);
        return 0;
    }
    if (ch->on) return 1;
    if (ch->asks && (value < 0 || value > 255)) {
        snprintf(error, capacity, "Trainer %zu needs a value from 0 to 255", index + 1);
        return 0;
    }
    ch->value = ch->asks ? (unsigned)value : 0u;
    for (size_t k = 0; k < t->count; ++k) {
        const uint32_t address = address_of(&t->pokes[k]);
        uint8_t was = 0, now = 0;
        const unsigned v = value_of(ch, &t->pokes[k]);
        /* A byte that cannot be read, written or read back as written is a machine that cannot take the trainer. */
        size_t done = k;
        int ok = zr_machine_peek(m, address, &was);
        if (ok) {
            ch->saved[k] = was;
            done = k + 1;                                   /* from here the byte may have changed */
            ok = zr_machine_poke(m, address, (uint8_t)v) && zr_machine_peek(m, address, &now) && now == (uint8_t)v;
        }
        if (!ok) {
            restore(ch, t, m, done);
            snprintf(error, capacity, "This machine cannot take the poke at %u", (unsigned)t->pokes[k].address);
            return 0;
        }
    }
    ch->on = 1;
    return 1;
}

void zr_cheats_frame(zr_cheats *c, zr_machine *m) {
    for (size_t i = 0; c->cheat && i < c->pok.count; ++i) {
        const zr_cheat *ch = &c->cheat[i];
        if (!ch->on) continue;
        const zr_pok_trainer *t = &c->pok.trainers[i];
        for (size_t k = 0; k < t->count; ++k) zr_machine_poke(m, address_of(&t->pokes[k]), (uint8_t)value_of(ch, &t->pokes[k]));
    }
}
