/* The window's texts in English and Romanian (text_table.h): the setup
 * screen, its pop-ups and file picker, the messages over the machine's
 * picture, the test screen and the refusals of a disk image. One table of
 * ids; every sentence is a row of its own, so the word order of a language
 * is never built from pieces. Command-line help and the diagnostics on
 * stderr are not here: they stay English.
 *
 * The rows are kept in UTF-8; the texts handed out are one byte for each
 * cell of the font they are drawn with. ZetRo's own character set
 * (font8x8.h) holds the ten Romanian letters (ă â î ș ț and the capitals)
 * at the codes of ROM_LETTERS (text.c), and zr_text_set_font maps them when
 * the font has them; a font that has not gets the plain letters (ă and â
 * fold to a, î to i, ș to s, ț to t, and the capitals likewise), as the
 * software of the day did.
 *
 * The language in use is a setting of the process (the shell sets it from
 * ui.language and the setup screen as the user changes it): zr_t(id) is a
 * text in that language. zr_text(lang, id) is one in a given language, for
 * the checks. ZetRo's own code. */
#ifndef ZR_TEXT_H
#define ZR_TEXT_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>

enum {
#define X(id, en, ro) ZR_T_##id,
#include "text_table.h"
#undef X
    ZR_T_COUNT
};

/* English until set; a value that is not a language is English. */
void zr_text_set_language(uint32_t language);
uint32_t zr_text_language(void);

/* The text in a language, ready to draw: a pointer that stays valid until
 * zr_text_set_font is called again. An unknown id gives "". */
const char *zr_text(uint32_t language, int id);
/* The text in the language in use. */
const char *zr_t(int id);
#define ZR_TXT(id) zr_t(ZR_T_##id)
/* The row as written (UTF-8, with its diacritics). */
const char *zr_text_source(uint32_t language, int id);
/* snprintf of the text of id in the language in use with the given values;
 * the conversions are those of the row. */
int zr_text_format(char *out, size_t capacity, int id, ...);

/* UTF-8 turned into the bytes of the cells: ASCII as it is; the Romanian
 * letters as `letters` has them (ten codes: ă â î ș ț and their capitals,
 * 0 for a letter the font does not hold) or, where it has none, the plain
 * letter; any other character as '?'. letters NULL: no diacritics at all.
 * Returns the length of the result (cut to capacity - 1). */
size_t zr_text_convert(const char *utf8, const uint8_t letters[10], char *out, size_t capacity);

/* Tells the table which character ROM the texts are drawn with: when it
 * holds every Romanian letter at the codes of ROM_LETTERS they are drawn,
 * else all diacritics are dropped. 1 when the letters are used. NULL: the
 * font has none (the fallback font). */
int zr_text_set_font(const uint8_t *font);

#endif /* ZR_TEXT_H */
