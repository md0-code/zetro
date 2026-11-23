/* The string table (text.h). ZetRo's own code. */
#include "text.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *en, *ro; } TABLE[ZR_T_COUNT] = {
#define X(id, en, ro) [ZR_T_##id] = {en, ro},
#include "text_table.h"
#undef X
};

/* The Romanian letters: ă â î ș ț, then the capitals. Where the font holds each of them: the codes of
 * ZetRo's own character set (font8x8.h), which draws the whole set, so Romanian is shown with its diacritics
 * wherever that font is. A font without the set (zr_text_set_font(NULL), or a character ROM that has none of
 * them at these codes) gets the plain letters. */
static const uint8_t ROM_LETTERS[10] = {0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99};

static const struct { uint32_t code; char plain; int slot; } LETTERS[] = {
    {0x103, 'a', 0}, {0xE2, 'a', 1}, {0xEE, 'i', 2}, {0x219, 's', 3}, {0x21B, 't', 4},
    {0x102, 'A', 5}, {0xC2, 'A', 6}, {0xCE, 'I', 7}, {0x218, 'S', 8}, {0x21A, 'T', 9},
    {0x15F, 's', 3}, {0x163, 't', 4}, {0x15E, 'S', 8}, {0x162, 'T', 9},    /* the cedilla forms older keyboards give */
};

static uint32_t language_now;
static const uint8_t *letters_now;                   /* ROM_LETTERS while the font holds the set */
static char *cache[ZR_LANG_COUNT][ZR_T_COUNT];

void zr_text_set_language(uint32_t language) { language_now = language < ZR_LANG_COUNT ? language : ZR_LANG_EN; }
uint32_t zr_text_language(void) { return language_now; }

size_t zr_text_convert(const char *utf8, const uint8_t letters[10], char *out, size_t capacity) {
    size_t n = 0;
    if (!capacity) return 0;
    for (const unsigned char *p = (const unsigned char *)utf8; *p && n + 1 < capacity;) {
        uint32_t code = *p;
        unsigned extra = 0;
        if (code >= 0xF0) { code &= 7u; extra = 3; }
        else if (code >= 0xE0) { code &= 15u; extra = 2; }
        else if (code >= 0xC0) { code &= 31u; extra = 1; }
        ++p;
        for (; extra && (*p & 0xC0u) == 0x80u; --extra, ++p) code = code << 6 | (*p & 63u);
        if (code < 128) { out[n++] = (char)code; continue; }
        char c = '?';
        for (size_t i = 0; i < sizeof LETTERS / sizeof *LETTERS; ++i)
            if (LETTERS[i].code == code) {
                const uint8_t at = letters ? letters[LETTERS[i].slot] : 0;
                c = at ? (char)at : LETTERS[i].plain;
                break;
            }
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

int zr_text_set_font(const uint8_t *font) {
    int whole = font != NULL;
    for (int i = 0; whole && i < 10; ++i) {
        unsigned any = 0;
        for (int y = 0; y < 8; ++y) any |= ROM_LETTERS[i] ? font[ROM_LETTERS[i] * 8 + y] : 0u;
        whole = any != 0;
    }
    const uint8_t *now = whole ? ROM_LETTERS : NULL;
    if (now != letters_now) {
        for (int l = 0; l < ZR_LANG_COUNT; ++l)
            for (int i = 0; i < ZR_T_COUNT; ++i) { free(cache[l][i]); cache[l][i] = NULL; }
        letters_now = now;
    }
    return whole;
}

const char *zr_text_source(uint32_t language, int id) {
    if (id < 0 || id >= ZR_T_COUNT) return "";
    return language == ZR_LANG_RO ? TABLE[id].ro : TABLE[id].en;
}

const char *zr_text(uint32_t language, int id) {
    if (id < 0 || id >= ZR_T_COUNT) return "";
    if (language >= ZR_LANG_COUNT) language = ZR_LANG_EN;
    if (!cache[language][id]) {
        const char *source = zr_text_source(language, id);
        const size_t room = strlen(source) + 1;
        char *text = malloc(room);
        if (!text) return source;
        zr_text_convert(source, letters_now, text, room);
        cache[language][id] = text;
    }
    return cache[language][id];
}

const char *zr_t(int id) { return zr_text(language_now, id); }

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
int zr_text_format(char *out, size_t capacity, int id, ...) {
    va_list args;
    va_start(args, id);
    const int n = vsnprintf(out, capacity, zr_t(id), args);
    va_end(args);
    return n;
}
