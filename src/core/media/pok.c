/* The .pok reader (pok.h). ZetRo's own code, written from the format description. */
#include "pok.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct reader {
    zr_pok *pok;
    size_t room, poke_room;                 /* trainers allocated; pokes allocated for the open trainer */
    int open;                               /* a trainer is open: its N has been read and its Z has not */
} reader;

static void spoil(zr_pok_trainer *t, unsigned line, const char *problem) {
    if (!t->usable) return;                 /* the first thing wrong is the one reported */
    t->usable = 0;
    t->line = line;
    snprintf(t->problem, sizeof t->problem, "%s", problem);
}

/* A new trainer after the last one; 0 when out of memory or too many. */
static zr_pok_trainer *start(reader *r, const char *name, size_t length, unsigned line, char *error, size_t capacity) {
    zr_pok *pok = r->pok;
    if (pok->count >= ZR_POK_MAX_TRAINERS) {
        snprintf(error, capacity, "The file has more than %d trainers", ZR_POK_MAX_TRAINERS);
        return NULL;
    }
    if (pok->count == r->room) {
        const size_t room = r->room ? r->room * 2 : 16;
        zr_pok_trainer *grown = realloc(pok->trainers, room * sizeof *grown);
        if (!grown) {
            snprintf(error, capacity, "Out of memory");
            return NULL;
        }
        pok->trainers = grown;
        r->room = room;
    }
    zr_pok_trainer *t = &pok->trainers[pok->count++];
    memset(t, 0, sizeof *t);
    if (length > ZR_POK_NAME) length = ZR_POK_NAME;
    memcpy(t->name, name, length);
    t->name[length] = 0;
    t->usable = 1;
    t->line = line;
    r->poke_room = 0;
    r->open = 1;
    return t;
}

/* A decimal number of at most six digits; 0 when the text is not one. */
static int number(const char *p, size_t n, unsigned *out) {
    if (!n || n > 6) return 0;
    unsigned v = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10u + (unsigned)(p[i] - '0');
    }
    *out = v;
    return 1;
}

/* One M or Z line's fields, after its letter; the problem as a text, NULL when it is a good poke. */
static const char *poke_of(const char *p, size_t n, unsigned line, zr_pok_poke *out) {
    unsigned f[4] = {0, 0, 0, 0};
    unsigned count = 0;
    size_t i = 0;
    while (i < n && count < 4) {
        while (i < n && (p[i] == ' ' || p[i] == '\t')) ++i;
        if (i >= n) break;
        const size_t start = i;
        while (i < n && p[i] != ' ' && p[i] != '\t') ++i;
        if (!number(p + start, i - start, &f[count])) return "a number is wrong";
        ++count;
    }
    if (count < 3) return "a poke needs a bank, an address and a value";
    if (f[0] > 15) return "the bank is not 0 to 15";
    if (f[1] > 65535u) return "the address is not 0 to 65535";
    if (f[2] > ZR_POK_ASK) return "the value is not 0 to 256";
    if (f[3] > 255) return "the original value is not 0 to 255";
    if (!(f[0] & ZR_POK_CPU_BANK) && f[1] > 0x3FFFu) return "the address is not inside a 16 KiB page";
    out->bank = (uint8_t)f[0];
    out->address = (uint16_t)f[1];
    out->value = (uint16_t)f[2];
    out->original = (uint8_t)f[3];
    out->line = line;
    return NULL;
}

static int add_poke(reader *r, zr_pok_trainer *t, const zr_pok_poke *poke, unsigned line, char *error, size_t capacity) {
    if (t->count >= ZR_POK_MAX_POKES) {
        spoil(t, line, "too many pokes");
        return 1;
    }
    if (t->count == r->poke_room) {
        const size_t room = r->poke_room ? r->poke_room * 2 : 8;
        zr_pok_poke *grown = realloc(t->pokes, room * sizeof *grown);
        if (!grown) {
            snprintf(error, capacity, "Out of memory");
            return 0;
        }
        t->pokes = grown;
        r->poke_room = room;
    }
    t->pokes[t->count++] = *poke;
    return 1;
}

/* The trainer is complete (its Z, the next N or the end of the file). */
static void close_trainer(reader *r, int ended, unsigned line) {
    if (!r->open) return;
    zr_pok_trainer *t = &r->pok->trainers[r->pok->count - 1];
    if (!t->count) spoil(t, t->line, "it has no pokes");
    else if (!ended) spoil(t, line, "no Z line ends it");
    r->open = 0;
}

int zr_pok_parse(const uint8_t *bytes, size_t size, zr_pok *pok, char *error, size_t capacity) {
    memset(pok, 0, sizeof *pok);
    if (!bytes || !size) {
        snprintf(error, capacity, "The file is empty");
        return 0;
    }
    if (size > ZR_POK_LIMIT) {
        snprintf(error, capacity, "The file is too large for a .pok file");
        return 0;
    }
    reader r = {pok, 0, 0, 0};
    const char *text = (const char *)bytes;
    size_t pos = 0;
    unsigned line = 0, previous = 0;                    /* the line being read and the last one that was not empty */
    int ok = 1, finished = 0;
    if (size >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) pos = 3;                  /* a byte order mark */
    while (ok && !finished && pos < size) {
        const size_t begin = pos;
        while (pos < size && text[pos] != '\n' && text[pos] != '\r') ++pos;
        const char *l = text + begin;
        size_t n = pos - begin;
        ++line;
        if (pos < size) {
            if (text[pos] == '\r' && pos + 1 < size && text[pos + 1] == '\n') ++pos;
            ++pos;
        }
        while (n && (*l == ' ' || *l == '\t')) { ++l; --n; }
        if (!n) continue;
        const unsigned before = previous;
        previous = line;
        const char c = (char)(*l >= 'a' && *l <= 'z' ? *l - 32 : *l);
        ++l;
        --n;
        if (c == 'Y') {
            finished = 1;
        } else if (c == 'N') {
            close_trainer(&r, 0, before);
            while (n && (*l == ' ' || *l == '\t')) { ++l; --n; }
            size_t length = 0;
            while (length < n && (unsigned char)l[length] >= 0x20) ++length;
            while (length && (l[length - 1] == ' ' || l[length - 1] == '\t')) --length;
            ok = start(&r, l, length, line, error, capacity) != NULL;
        } else if (c == 'M' || c == 'Z') {
            if (!r.open) {
                /* A poke that belongs to no trainer: kept as a trainer of its own that cannot be used. */
                zr_pok_trainer *t = start(&r, "?", 1, line, error, capacity);
                if (!t) { ok = 0; break; }
                spoil(t, line, "a poke outside a trainer");
            }
            zr_pok_trainer *t = &pok->trainers[pok->count - 1];
            zr_pok_poke poke;
            const char *problem = poke_of(l, n, line, &poke);
            if (problem) spoil(t, line, problem);
            else ok = add_poke(&r, t, &poke, line, error, capacity);
            if (ok && c == 'Z') close_trainer(&r, 1, line);
        } else if (r.open) {
            spoil(&pok->trainers[pok->count - 1], line, "a line is neither M, Z nor N");
        }
    }
    if (ok) close_trainer(&r, 0, previous);
    if (ok && !pok->count) {
        snprintf(error, capacity, "The file holds no trainers");
        ok = 0;
    }
    if (!ok) zr_pok_free(pok);
    return ok;
}

void zr_pok_free(zr_pok *pok) {
    if (!pok) return;
    for (size_t i = 0; i < pok->count; ++i) free(pok->trainers[i].pokes);
    free(pok->trainers);
    memset(pok, 0, sizeof *pok);
}
