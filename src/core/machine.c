/* The machine interface (machine.h): the registry of families, the public
 * calls dispatched to the board that runs, and the frame as colours.
 * ZetRo's own code. */
#include "machine_impl.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Fixed-width types and 8-bit bytes. */
_Static_assert(CHAR_BIT == 8, "ZetRo needs 8-bit bytes");
_Static_assert(sizeof(uint16_t) == 2 && sizeof(uint32_t) == 4 && sizeof(uint64_t) == 8, "fixed-width types");

/* --- the registry ---------------------------------------------------------------------- */

/* Every family built in; a board's package adds its own here. */
static const zr_family *const FAMILIES[] = {
    &zr_stub_family,
    &zr_hc_family,
    &zr_hc128_family,
    &zr_hc2000_family,
    &zr_hc88_family,
    &zr_cip_family,
    &zr_jet_family,                     /* listed after the CIPs (the CIP-04, of the same name, is listed with them) */
    &zr_tims_family,
    &zr_cobra_family,
    &zr_cip04_family,
    &zr_cubz_family,
    &zr_amic_family,
    &zr_prae_family,
};
#define FAMILY_COUNT (sizeof FAMILIES / sizeof FAMILIES[0])
#define MAX_HASHES 8u

unsigned zr_family_count(void) { return (unsigned)FAMILY_COUNT; }
const zr_family *zr_family_at(unsigned index) { return index < FAMILY_COUNT ? FAMILIES[index] : NULL; }

const zr_family *zr_family_of_model(const char *id, unsigned *model) {
    for (size_t i = 0; i < FAMILY_COUNT; ++i)
        for (unsigned k = 0; k < FAMILIES[i]->model_count; ++k)
            if (!strcmp(FAMILIES[i]->models[k].id, id)) {
                if (model) *model = k;
                return FAMILIES[i];
            }
    return NULL;
}

int zr_family_option(const zr_family *f, const char *key) {
    for (unsigned i = 0; i < f->option_count; ++i) if (!strcmp(f->options[i].key, key)) return (int)i;
    return -1;
}
int zr_family_option_value(const zr_family *f, unsigned option, const char *word) {
    if (option >= f->option_count) return -1;
    for (int i = 0; f->options[option].values[i]; ++i) if (!strcmp(f->options[option].values[i], word)) return i;
    return -1;
}
int zr_family_media(const zr_family *f, const char *key) {
    for (unsigned i = 0; i < f->media_count; ++i) if (!strcmp(f->media[i].key, key)) return (int)i;
    return -1;
}
/* Key names compare without regard to case. */
static int same_name(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) {
        const int x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}
int zr_family_key(const zr_family *f, const char *name) {
    for (unsigned i = 0; i < f->key_count; ++i) if (same_name(f->keys[i].name, name)) return (int)i;
    return -1;
}
int zr_family_typing(const zr_family *f, int ch, uint8_t *key, uint8_t *shift) {
    for (unsigned i = 0; i < f->typing_count; ++i)
        if (f->typing[i].ch == (char)ch) {
            *key = f->typing[i].key;
            *shift = f->typing[i].shift;
            return 1;
        }
    return 0;
}

/* --- what applies ---------------------------------------------------------------------- */

/* One clause "key=word|word" or "key!=word|word" of `n` characters. */
static int clause_holds(const zr_family *f, unsigned model, const unsigned *values, const char *clause, size_t n) {
    char key[ZR_CONFIG_NAME];
    size_t k = 0;
    while (k < n && clause[k] != '=' && clause[k] != '!') ++k;
    if (k == n || k >= sizeof key) return 0;
    memcpy(key, clause, k);
    key[k] = 0;
    const int negate = clause[k] == '!';
    size_t at = k + (negate ? 2u : 1u);
    const int option = zr_family_option(f, key);
    const int is_model = !strcmp(key, "model") && model < f->model_count;
    if ((!is_model && (option < 0 || option >= ZR_CONFIG_OPTIONS)) || at > n) return 0;
    const char *now = is_model ? f->models[model].id : f->options[option].values[values[option]];
    int found = 0;
    while (at <= n && !found) {
        size_t end = at;
        while (end < n && clause[end] != '|') ++end;
        found = strlen(now) == end - at && !memcmp(now, clause + at, end - at);
        at = end + 1;
    }
    return negate ? !found : found;
}

static int condition_holds(const zr_family *f, unsigned model, const unsigned *values, const char *when, size_t n) {
    for (size_t at = 0; at <= n;) {
        size_t end = at;
        while (end < n && when[end] != '&') ++end;
        if (end > at && !clause_holds(f, model, values, when + at, end - at)) return 0;
        at = end + 1;
    }
    return 1;
}

int zr_family_condition(const zr_family *f, unsigned model, const unsigned *values, const char *when) {
    return !when || condition_holds(f, model, values, when, strlen(when));
}

/* The word of the first "condition:word" rule (joined by ';') whose condition holds, into `word`; 0 when none does. */
static int rule_word(const zr_family *f, unsigned model, const unsigned *values, const char *rules, char *word, size_t capacity) {
    for (const char *rule = rules; rule && *rule;) {
        const char *end = strchr(rule, ';');
        const size_t n = end ? (size_t)(end - rule) : strlen(rule);
        size_t colon = 0;
        while (colon < n && rule[colon] != ':') ++colon;
        if (colon < n && condition_holds(f, model, values, rule, colon)) {
            const size_t w = n - colon - 1 < capacity ? n - colon - 1 : capacity - 1;
            memcpy(word, rule + colon + 1, w);
            word[w] = 0;
            return 1;
        }
        rule += n + (end ? 1u : 0u);
    }
    return 0;
}

void zr_family_resolve(const zr_family *f, unsigned model, const zr_config *config, unsigned *values, uint8_t *state) {
    uint8_t forced[ZR_CONFIG_OPTIONS] = {0};
    const unsigned count = f->option_count < ZR_CONFIG_OPTIONS ? f->option_count : ZR_CONFIG_OPTIONS;
    for (unsigned i = 0; i < ZR_CONFIG_OPTIONS; ++i) values[i] = 0;
    for (unsigned i = 0; i < count; ++i) {
        /* an option the model lacks is at its initial value, whatever a file written for another model says */
        const char *word = config && (f->options[i].models >> model & 1u) ? zr_config_option_value(config, f->options[i].key) : NULL;
        const int value = word ? zr_family_option_value(f, i, word) : -1;
        values[i] = value >= 0 ? (unsigned)value : f->options[i].initial;
    }
    /* A forced value can make another rule hold: a few rounds settle the short chains the tables have. */
    for (int round = 0; round < 3; ++round)
        for (unsigned i = 0; i < count; ++i) {
            const zr_option_desc *o = &f->options[i];
            if (!o->forced || !(o->models >> model & 1u)) continue;
            char word[ZR_CONFIG_NAME];
            const int value = rule_word(f, model, values, o->forced, word, sizeof word) ? zr_family_option_value(f, i, word) : -1;
            if (value >= 0) { values[i] = (unsigned)value; forced[i] = 1; }
        }
    if (!state) return;
    for (unsigned i = 0; i < ZR_CONFIG_OPTIONS; ++i) {
        state[i] = 0;
        if (i >= count || !(f->options[i].models >> model & 1u) || !zr_family_condition(f, model, values, f->options[i].when)) continue;
        state[i] = (uint8_t)(ZR_OPTION_PRESENT | (forced[i] ? ZR_OPTION_FORCED : 0));
    }
}

int zr_family_has_media(const zr_family *f, unsigned model, const unsigned *values, unsigned slot) {
    return slot < f->media_count && (f->media[slot].models >> model & 1u) && zr_family_condition(f, model, values, f->media[slot].when);
}
int zr_family_has_port(const zr_family *f, unsigned model, const unsigned *values, unsigned port) {
    return port < f->port_count && (f->ports[port].models >> model & 1u) && zr_family_condition(f, model, values, f->ports[port].when);
}

/* --- life cycle ------------------------------------------------------------------------ */

struct zr_machine {
    const zr_machine_impl *impl;
    void *self;
    const zr_family *family;
    unsigned model;
    unsigned options[ZR_CONFIG_OPTIONS];        /* the value of each of the family's options the machine was built with */
};

int zr_machine_host_ok(void) {
    const uint16_t one = 1;
    uint8_t first;
    memcpy(&first, &one, 1);
    return first == 1;
}

zr_machine *zr_machine_create(const zr_config *config, const zr_rom_index *roms, char *error, size_t capacity) {
    if (!zr_machine_host_ok()) {
        if (capacity) snprintf(error, capacity, "This host is not little-endian: ZetRo cannot run on it");
        return NULL;
    }
    zr_machine_setup setup;
    memset(&setup, 0, sizeof setup);
    setup.family = zr_family_of_model(config->model, &setup.model);
    if (!setup.family) {
        if (capacity) snprintf(error, capacity, "There is no model %s", config->model);
        return NULL;
    }
    setup.roms = roms;
    setup.wanted_family = setup.family;
    setup.wanted_model = setup.model;
    /* Without one of its ROMs the model cannot be built: the test machine's screen says which. */
    for (const char *const *key = setup.family->models[setup.model].roms; key && *key; ++key)
        if (!roms || !zr_rom_index_find(roms, zr_romdb_find(*key))) {
            setup.family = &zr_stub_family;
            setup.model = 0;
            break;
        }
    unsigned options[ZR_CONFIG_OPTIONS] = {0};
    zr_family_resolve(setup.family, setup.model, setup.family == setup.wanted_family ? config : NULL, options, NULL);
    setup.options = options;
    void *self = setup.family->create(&setup, error, capacity);
    if (!self) return NULL;
    zr_machine *m = malloc(sizeof *m);
    if (!m) {
        setup.family->impl->destroy(self);
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    m->impl = setup.family->impl;
    m->self = self;
    m->family = setup.family;
    m->model = setup.model;
    memcpy(m->options, options, sizeof m->options);
    return m;
}

void zr_machine_destroy(zr_machine *m) {
    if (!m) return;
    m->impl->destroy(m->self);
    free(m);
}

const zr_family *zr_machine_family(const zr_machine *m) { return m->family; }
unsigned zr_machine_model(const zr_machine *m) { return m->model; }
const unsigned *zr_machine_options(const zr_machine *m) { return m->options; }
int zr_machine_has_media(const zr_machine *m, unsigned slot) { return zr_family_has_media(m->family, m->model, m->options, slot); }
int zr_machine_has_port(const zr_machine *m, unsigned port) { return zr_family_has_port(m->family, m->model, m->options, port); }
const char *zr_machine_description(const zr_machine *m) { return m->impl->description(m->self); }
const char *zr_machine_error(const zr_machine *m) { return m->impl->error(m->self); }
void zr_machine_reset(zr_machine *m, int power_on) { m->impl->reset(m->self, power_on); }
void zr_machine_nmi(zr_machine *m) { if (m->impl->nmi) m->impl->nmi(m->self); }
uint32_t zr_machine_clock(const zr_machine *m) { return m->impl->clock(m->self); }
zr_run_status zr_machine_run(zr_machine *m, uint64_t ticks) { return m->impl->run(m->self, ticks); }
uint64_t zr_machine_time(const zr_machine *m) { return m->impl->time(m->self); }
uint64_t zr_machine_frame_period(const zr_machine *m) { return m->impl->frame_period(m->self); }
int zr_machine_frame(zr_machine *m, zr_frame *frame) { return m->impl->frame(m->self, frame); }
size_t zr_machine_text(zr_machine *m, char *out, size_t capacity) {
    if (capacity) out[0] = 0;
    return m->impl->text ? m->impl->text(m->self, out, capacity) : 0;
}
uint32_t zr_machine_audio_rate(const zr_machine *m) { return m->impl->audio_rate(m->self); }
size_t zr_machine_audio_pull(zr_machine *m, int16_t *stereo, size_t frames) { return m->impl->audio_pull(m->self, stereo, frames); }
void zr_machine_key(zr_machine *m, unsigned key, int down) { if (key < m->family->key_count) m->impl->key(m->self, key, down); }
void zr_machine_joystick(zr_machine *m, unsigned stick, unsigned held) { if (m->impl->joystick) m->impl->joystick(m->self, stick, held); }

int zr_machine_insert(zr_machine *m, unsigned slot, zr_media *media, char *error, size_t capacity) {
    if (slot >= m->family->media_count || !(m->family->media[slot].models >> m->model & 1u)) {
        if (capacity) snprintf(error, capacity, "There is no such slot");
        return 0;
    }
    return m->impl->insert(m->self, slot, media, error, capacity);
}
zr_media *zr_machine_media(const zr_machine *m, unsigned slot) { return slot < m->family->media_count ? m->impl->media(m->self, slot) : NULL; }
int zr_machine_eject(zr_machine *m, unsigned slot) {
    zr_media *media = zr_machine_media(m, slot);
    if (!media || !m->impl->eject(m->self, slot)) return 0;
    media->card = NULL;
    media->disk = NULL;
    return 1;
}

void zr_machine_swap(zr_machine *a, zr_machine *b) {
    const zr_machine tmp = *a;
    *a = *b;
    *b = tmp;
}
unsigned zr_machine_activity(const zr_machine *m) { return m->impl->activity(m->self); }
int zr_machine_control(zr_machine *m, unsigned slot, unsigned command, uint32_t value) {
    return m->impl->control && slot < m->family->media_count && m->impl->control(m->self, slot, command, value);
}

unsigned zr_machine_tape_options(zr_machine *m, unsigned slot, unsigned bits) {
    return m->impl->tape_options ? m->impl->tape_options(m->self, slot, bits) : 0u;
}
const char *zr_machine_autoload(const zr_machine *m, unsigned slot, int *power_cycle) {
    int cycle = 1;
    const char *text = m->impl->autoload ? m->impl->autoload(m->self, slot, &cycle) : m->family->models[m->model].autoload;
    if (power_cycle) *power_cycle = cycle;
    return text;
}

struct zr_tape_deck *zr_machine_tape_deck(zr_machine *m, unsigned slot) {
    return m->impl->tape_deck && slot < m->family->media_count ? m->impl->tape_deck(m->self, slot) : NULL;
}

void zr_machine_set_ports(zr_machine *m, const zr_port_host *host) { if (m->impl->set_ports) m->impl->set_ports(m->self, host); }
size_t zr_machine_port_receive(zr_machine *m, unsigned port, const uint8_t *data, size_t count) {
    return m->impl->port_receive ? m->impl->port_receive(m->self, port, data, count) : count;
}
void zr_machine_port_modem(zr_machine *m, unsigned port, int dcd, int cts, int dsr, int ri) {
    if (m->impl->port_modem) m->impl->port_modem(m->self, port, dcd, cts, dsr, ri);
}

/* --- states ---------------------------------------------------------------------------- */

void zr_machine_save(const zr_machine *m, zr_state_writer *w) {
    uint8_t hashes[MAX_HASHES * 32u];
    const uint32_t count = m->impl->rom_hashes(m->self, hashes, MAX_HASHES);
    zr_sw_header(w, m->family->id, m->family->models[m->model].id, hashes, count);
    /* The options that shape the machine: a state fits only a machine built with the same ones. */
    zr_sw_begin(w, "OPTS", 0);
    for (unsigned i = 0; i < m->family->option_count && i < ZR_CONFIG_OPTIONS; ++i) {
        const zr_option_desc *o = &m->family->options[i];
        if (!(o->flags & ZR_OPTION_RESET) || !(o->models >> m->model & 1u)) continue;
        zr_sw_str(w, o->key);
        zr_sw_str(w, o->values[m->options[i]]);
    }
    zr_sw_str(w, "");
    zr_sw_end(w);
    m->impl->save(m->self, w);
}

int zr_machine_load(zr_machine *m, zr_state_reader *r, char *error, size_t capacity) {
    uint8_t hashes[MAX_HASHES * 32u];
    const uint32_t count = m->impl->rom_hashes(m->self, hashes, MAX_HASHES);
    if (strcmp(r->family, m->family->id) || strcmp(r->model, m->family->models[m->model].id)) {
        if (capacity) snprintf(error, capacity, "The state is of another machine (%s)", r->model);
        return 0;
    }
    if (r->hash_count != count || (count && memcmp(r->hashes, hashes, (size_t)count * 32u))) {
        if (capacity) snprintf(error, capacity, "The state was saved with other ROMs");
        return 0;
    }
    if (zr_sr_chunk(r, "OPTS", 0))
        for (;;) {
            char key[ZR_CONFIG_NAME], word[ZR_CONFIG_NAME];
            zr_sr_str(r, key, sizeof key);
            if (!key[0] || !zr_sr_ok(r)) break;
            zr_sr_str(r, word, sizeof word);
            const int o = zr_family_option(m->family, key);
            if (o >= 0 && o < ZR_CONFIG_OPTIONS && strcmp(m->family->options[o].values[m->options[o]], word)) {
                if (capacity) snprintf(error, capacity, "The state was saved with %s: %s", key, word);
                return 0;
            }
        }
    if (!m->impl->load(m->self, r) || !zr_sr_ok(r)) {
        m->impl->reset(m->self, 1);
        if (capacity) snprintf(error, capacity, "The state is damaged");
        return 0;
    }
    return 1;
}

/* --- checks ---------------------------------------------------------------------------- */

void zr_machine_set_trace(zr_machine *m, zr_trace_fn fn, void *ctx) { if (m->impl->set_trace) m->impl->set_trace(m->self, fn, ctx); }
int zr_machine_peek(zr_machine *m, uint32_t address, uint8_t *value) {
    *value = 0xFF;
    return m->impl->peek && m->impl->peek(m->self, address, value);
}
int zr_machine_poke(zr_machine *m, uint32_t address, uint8_t value) { return m->impl->poke && m->impl->poke(m->self, address, value); }

/* --- snapshots ------------------------------------------------------------------------ */

int zr_machine_takes_snapshots(const zr_machine *m) { return m->impl->snapshot_load != NULL && m->impl->snapshot_save != NULL; }
int zr_machine_snapshot_load(zr_machine *m, const struct zr_snap *snap, char *error, size_t capacity) {
    if (!zr_machine_takes_snapshots(m)) {
        if (capacity) snprintf(error, capacity, "This machine takes no snapshots");
        return 0;
    }
    return m->impl->snapshot_load(m->self, snap, error, capacity);
}
int zr_machine_snapshot_save(zr_machine *m, struct zr_snap *snap) {
    return zr_machine_takes_snapshots(m) && m->impl->snapshot_save(m->self, snap);
}

/* --- the picture ----------------------------------------------------------------------- */

static uint64_t gcd64(uint64_t a, uint64_t b) { while (b) { const uint64_t t = a % b; a = b; b = t; } return a; }

void zr_frame_crop(zr_frame *f, uint32_t border) {
    if (border == ZR_CONFIG_BORDER_FULL || border > ZR_CONFIG_BORDER_NONE || !f->pixels || !f->picture_w || !f->picture_h) return;
    if (f->picture_x + f->picture_w > f->width || f->picture_y + f->picture_h > f->height) return;
    const uint32_t edge = border == ZR_CONFIG_BORDER_NONE ? 0u : border == ZR_CONFIG_BORDER_MINIMAL ? 5u : 16u;
    const uint64_t aw = f->aspect_w ? f->aspect_w : f->width, ah = f->aspect_h ? f->aspect_h : f->height;
    /* pixels about half as wide as they are high (the 704-dot frames): the sides' border in twice as many */
    const uint32_t side = edge * (2u * f->width * ah >= 3u * f->height * aw ? 2u : 1u);
    const uint32_t left = f->picture_x < side ? f->picture_x : side, top = f->picture_y < edge ? f->picture_y : edge;
    const uint32_t right_room = f->width - f->picture_x - f->picture_w, bottom_room = f->height - f->picture_y - f->picture_h;
    const uint32_t w = left + f->picture_w + (right_room < side ? right_room : side);
    const uint32_t h = top + f->picture_h + (bottom_room < edge ? bottom_room : edge);
    uint64_t new_w = aw * w * f->height, new_h = ah * h * f->width;
    const uint64_t g = gcd64(new_w, new_h);
    new_w /= g;
    new_h /= g;
    while (new_w > UINT32_MAX || new_h > UINT32_MAX) { new_w >>= 1; new_h >>= 1; }
    f->pixels += (size_t)(f->picture_y - top) * f->pitch + (f->picture_x - left);
    f->picture_x = left;
    f->picture_y = top;
    f->width = w;
    f->height = h;
    f->aspect_w = (uint32_t)new_w;
    f->aspect_h = (uint32_t)new_h;
}

void zr_frame_argb(const zr_frame *f, uint32_t *out) {
    uint32_t colors[256];
    for (unsigned i = 0; i < 256; ++i) colors[i] = 0xFF000000u | (i < f->colors ? f->palette[i] : 0u);
    for (uint32_t y = 0; y < f->height; ++y) {
        const uint8_t *row = f->pixels + (size_t)y * f->pitch;
        uint32_t *o = out + (size_t)y * f->width;
        for (uint32_t x = 0; x < f->width; ++x) o[x] = colors[row[x]];
    }
}
