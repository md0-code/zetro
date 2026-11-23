/* The shell's typist (typist.h). ZetRo's own code; the typing steps are
 * the harness's (harness.c), kept in step with them so that a text typed
 * here and one typed by a script reach the machine the same way. */
#include "typist.h"

#include <string.h>

enum { IDLE, WAITING, TYPING };
#define SCREEN_MAX 8192                 /* the screen's text compared for "ready" */

void zr_typist_init(zr_typist *t) { memset(t, 0, sizeof *t); }

/* `text` with its escapes resolved into out; 0 when it does not fit or the keyboard cannot type a character. */
static int resolve(const zr_family *f, const char *text, char *out, size_t *length) {
    size_t n = 0;
    for (const char *p = text; *p; ++p) {
        char ch = *p;
        if (ch == '\\' && p[1]) {
            ++p;
            ch = *p == 'n' ? '\n' : *p == 'b' ? '\b' : *p == 't' ? '\t' : *p;
        }
        uint8_t key, shift;
        if (n + 1 >= ZR_TYPIST_TEXT || !zr_family_typing(f, ch, &key, &shift)) return 0;
        out[n++] = ch;
    }
    out[n] = 0;
    *length = n;
    return 1;
}

/* The key typed now, let go (with its shift key). */
static void release(zr_typist *t, zr_machine *m) {
    if (!t->held) return;
    zr_machine_key(m, t->key, 0);
    if (t->shift != ZR_KEY_NONE) zr_machine_key(m, t->shift, 0);
    t->held = 0;
}

void zr_typist_cancel(zr_typist *t, zr_machine *m) {
    release(t, m);
    t->state = IDLE;
    t->autoload = 0;
    t->length = t->at = 0;
    t->phase = 0;
}

int zr_typist_busy(const zr_typist *t) { return t->state != IDLE; }

static void begin_typing(zr_typist *t) {
    t->state = TYPING;
    t->at = 0;
    t->phase = 0;
    t->held = 0;
}

int zr_typist_start(zr_typist *t, zr_machine *m, const char *text) {
    char resolved[ZR_TYPIST_TEXT];
    size_t length = 0;
    if (!resolve(zr_machine_family(m), text, resolved, &length)) return 0;
    zr_typist_cancel(t, m);
    if (!length) return 1;
    memcpy(t->text, resolved, length + 1);
    t->length = length;
    begin_typing(t);
    return 1;
}

int zr_typist_autoload(zr_typist *t, zr_machine *m, const zr_config *config, unsigned slot) {
    const zr_family *f = zr_machine_family(m);
    int power_cycle = 1;
    const char *text = zr_machine_autoload(m, slot, &power_cycle);       /* the board's, from its state, else the model's */
    char resolved[ZR_TYPIST_TEXT];
    size_t length = 0;
    zr_typist_cancel(t, m);
    if (!text || !*text || slot >= f->media_count || f->media[slot].kind != ZR_MEDIA_TAPE) return 0;
    const zr_config_media *settings = zr_config_media_find(config, f->media[slot].key);
    if (settings && !settings->autoload) return 0;
    if (!(f->media[slot].models >> zr_machine_model(m) & 1u) || !resolve(f, text, resolved, &length) || !length) return 0;
    memcpy(t->text, resolved, length + 1);
    t->length = length;
    t->autoload = 1;
    t->slot = slot;
    t->frames = t->stable = 0;
    t->hash = 0;
    t->hash_length = 0;
    t->state = WAITING;
    if (power_cycle) zr_machine_reset(m, 1);
    return 1;
}

/* A frame of waiting for the machine to be ready; typing starts in the frame it is. */
static void wait_step(zr_typist *t, zr_machine *m) {
    char screen[SCREEN_MAX];
    const size_t n = zr_machine_text(m, screen, sizeof screen);
    uint64_t hash = 14695981039346656037ull;                    /* FNV-1a */
    for (size_t i = 0; i < n && i < sizeof screen; ++i) hash = (hash ^ (uint8_t)screen[i]) * 1099511628211ull;
    /* A screen without a letter or a digit on it says nothing yet: the machine is still testing its memory. */
    int readable = 0;
    for (size_t i = 0; i < n && i < sizeof screen && !readable; ++i)
        readable = (screen[i] >= '0' && screen[i] <= '9') || (screen[i] >= 'A' && screen[i] <= 'Z') || (screen[i] >= 'a' && screen[i] <= 'z');
    if (readable && t->frames && hash == t->hash && n == t->hash_length) ++t->stable;
    else t->stable = 0;
    t->hash = hash;
    t->hash_length = n;
    ++t->frames;
    if (t->frames >= ZR_TYPIST_READY_LIMIT || (t->frames >= ZR_TYPIST_READY_MIN && t->stable >= ZR_TYPIST_READY_STABLE)) {
        begin_typing(t);
        zr_typist_step(t, m);
    }
}

/* A frame of typing: a key goes down (its shift first), stays ZR_TYPIST_KEY_FRAMES frames, goes up and stays up as long. */
static void type_step(zr_typist *t, zr_machine *m) {
    if (t->phase == 0) {
        uint8_t key = 0, shift = ZR_KEY_NONE;
        zr_family_typing(zr_machine_family(m), t->text[t->at], &key, &shift);
        if (shift != ZR_KEY_NONE) zr_machine_key(m, shift, 1);
        zr_machine_key(m, key, 1);
        t->key = key;
        t->shift = shift;
        t->held = 1;
    } else if (t->phase == ZR_TYPIST_KEY_FRAMES) release(t, m);
    /* The same key again is left up longer: a ROM's keyboard routine takes a key a second time only after it
     * has seen it up for several frame interrupts (five on the Spectrum-compatibles). */
    unsigned frames = 2 * ZR_TYPIST_KEY_FRAMES;
    if (t->at + 1 < t->length) {
        uint8_t next = ZR_KEY_NONE, next_shift = ZR_KEY_NONE;
        zr_family_typing(zr_machine_family(m), t->text[t->at + 1], &next, &next_shift);
        if (next == t->key) frames += 6;
    }
    if (++t->phase < frames) return;
    t->phase = 0;
    if (++t->at < t->length) return;
    /* The text is typed: the tape starts. */
    t->state = IDLE;
    if (t->autoload) zr_machine_control(m, t->slot, ZR_MEDIA_PLAY, 0);
    t->autoload = 0;
}

void zr_typist_step(zr_typist *t, zr_machine *m) {
    if (t->state == WAITING) wait_step(t, m);
    else if (t->state == TYPING) type_step(t, m);
}

void zr_typist_save(const zr_typist *t, zr_state_writer *w)
{
    zr_sw_u8(w, (uint8_t)t->state);
    zr_sw_str(w, t->text);
    zr_sw_u32(w, (uint32_t)t->at);
    zr_sw_u32(w, t->phase);
    zr_sw_u8(w, (uint8_t)t->held);
    zr_sw_u8(w, t->key);
    zr_sw_u8(w, t->shift);
    zr_sw_u8(w, (uint8_t)t->autoload);
    zr_sw_u32(w, t->slot);
    zr_sw_u32(w, t->frames);
    zr_sw_u32(w, t->stable);
    zr_sw_u64(w, t->hash);
    zr_sw_u32(w, (uint32_t)t->hash_length);
}

int zr_typist_load(zr_typist *t, zr_state_reader *r, const zr_family *family)
{
    zr_typist_init(t);
    t->state = zr_sr_u8(r);
    zr_sr_str(r, t->text, sizeof t->text);
    t->length = strlen(t->text);
    t->at = zr_sr_u32(r);
    t->phase = zr_sr_u32(r);
    t->held = zr_sr_u8(r);
    t->key = zr_sr_u8(r);
    t->shift = zr_sr_u8(r);
    t->autoload = zr_sr_u8(r);
    t->slot = zr_sr_u32(r);
    t->frames = zr_sr_u32(r);
    t->stable = zr_sr_u32(r);
    t->hash = zr_sr_u64(r);
    t->hash_length = zr_sr_u32(r);
    if (!zr_sr_ok(r) || t->state > TYPING || t->held > 1 || t->autoload > 1 ||
        (t->state != IDLE && (!t->length || t->at >= t->length)) ||
        t->phase >= 2 * ZR_TYPIST_KEY_FRAMES + 6 ||
        (t->autoload && (t->slot >= family->media_count || family->media[t->slot].kind != ZR_MEDIA_TAPE)) ||
        (t->held && (t->state != TYPING || t->key >= family->key_count ||
                     (t->shift != ZR_KEY_NONE && t->shift >= family->key_count))))
        return 0;
    for (size_t i = 0; t->state != IDLE && i < t->length; ++i) {
        uint8_t key, shift;
        if (!zr_family_typing(family, t->text[i], &key, &shift))
            return 0;
    }
    return 1;
}
