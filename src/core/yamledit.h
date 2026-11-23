/* Editing zetro.yaml in place (the setup screen's save): the file is kept
 * as its lines, and an edit changes only the lines of the key it touches,
 * so comments, blank lines, the order of sections and keys and quoting stay
 * as they were. Written for the block style the
 * template uses (indented mappings and sequences, flow collections on one
 * or more lines, comments); a flow mapping that an edit reaches into is
 * rewritten in block style. ZetRo reads the result with libyaml as usual
 * (config.c). */

#ifndef ZR_YAMLEDIT_H
#define ZR_YAMLEDIT_H

#include <stddef.h>

typedef struct zr_ydoc {
    char **lines;              /* without line ends */
    size_t count, capacity;
    int crlf;                  /* the file's lines end in CR LF */
} zr_ydoc;

/* The text as lines (any of LF, CR LF). 0 when out of memory. */
int zr_ydoc_parse(zr_ydoc *doc, const char *text, size_t length);
int zr_ydoc_load(zr_ydoc *doc, const char *path);
/* The document as text (malloc'd, NUL-terminated); *length without the NUL. */
char *zr_ydoc_text(const zr_ydoc *doc, size_t *length);
/* Writes the file through a temporary file in its folder, replaced at once. */
int zr_ydoc_save(const zr_ydoc *doc, const char *path);
void zr_ydoc_free(zr_ydoc *doc);

/* A path is a list of mapping keys from the top (unquoted, as libyaml reads
 * them). */
int zr_ydoc_has(const zr_ydoc *doc, const char *const *path, size_t depth);
/* The scalar value of the key, unquoted; 0 when the key is missing or its
 * value is not a scalar on its line. */
int zr_ydoc_get(const zr_ydoc *doc, const char *const *path, size_t depth, char *out, size_t capacity);
/* Sets the key to a value written in YAML on its line (a scalar, quoted as
 * needed by the caller with zr_ydoc_quote, or a flow collection); creates the
 * key and the mappings above it when missing, uncommenting the template's
 * commented-out line for the key ("# key: ...") when there is one. */
int zr_ydoc_set(zr_ydoc *doc, const char *const *path, size_t depth, const char *value);
/* Rewrites the mapping at path, and every mapping inside it, in block
 * style (depth 0: every top-level mapping), so that each of their keys has
 * a line of its own. */
int zr_ydoc_expand(zr_ydoc *doc, const char *const *path, size_t depth);
/* Removes the key and its value; 1 when it was not there either. */
int zr_ydoc_remove(zr_ydoc *doc, const char *const *path, size_t depth);
/* The keys of the mapping at path (depth 0: the top), in order: up to max
 * names of up to 128 bytes. Returns how many. */
size_t zr_ydoc_keys(const zr_ydoc *doc, const char *const *path, size_t depth, char keys[][128], size_t max);

/* A string as a YAML scalar: plain when that reads back as the same string,
 * else in double quotes. */
const char *zr_ydoc_quote(const char *s, char *buffer, size_t capacity);
/* A path as a YAML scalar: single quotes keep backslashes as they are. */
const char *zr_ydoc_quote_path(const char *s, char *buffer, size_t capacity);

#endif /* ZR_YAMLEDIT_H */
