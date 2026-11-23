/* Editing zetro.yaml in place (yamledit.h). The file replacement goes through the host layer.
 *
 * The document is its lines. An entry is a line that holds a key ("key:",
 * "key: value", a quoted key) or a sequence item ("- ..."), with the lines
 * a flow collection continues onto. A key's block is the lines after it
 * that are more indented (or, for a block sequence written at the key's own
 * indent, its "- " items), with the comments among and after them that are
 * indented as deeply; blank lines belong to it only between such lines. */
#include "yamledit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/host.h"

/* ASCII letter case aside. */
static int same_text(const char *a, const char *b) {
    for (;; ++a, ++b) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* --- lines -------------------------------------------------------------------------- */

static char *duplicate(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) { memcpy(d, s, n); d[n] = 0; }
    return d;
}
static int insert_line(zr_ydoc *doc, size_t at, const char *text) {
    if (doc->count == doc->capacity) {
        const size_t capacity = doc->capacity ? doc->capacity * 2 : 64;
        char **lines = realloc(doc->lines, capacity * sizeof *lines);
        if (!lines) return 0;
        doc->lines = lines;
        doc->capacity = capacity;
    }
    char *copy = duplicate(text, strlen(text));
    if (!copy) return 0;
    memmove(doc->lines + at + 1, doc->lines + at, (doc->count - at) * sizeof *doc->lines);
    doc->lines[at] = copy;
    doc->count++;
    return 1;
}
static void delete_lines(zr_ydoc *doc, size_t at, size_t n) {
    for (size_t i = 0; i < n; ++i) free(doc->lines[at + i]);
    memmove(doc->lines + at, doc->lines + at + n, (doc->count - at - n) * sizeof *doc->lines);
    doc->count -= n;
}
static int replace_line(zr_ydoc *doc, size_t at, const char *text) {
    char *copy = duplicate(text, strlen(text));
    if (!copy) return 0;
    free(doc->lines[at]);
    doc->lines[at] = copy;
    return 1;
}

int zr_ydoc_parse(zr_ydoc *doc, const char *text, size_t length) {
    memset(doc, 0, sizeof *doc);
    size_t start = 0, crlf = 0, lf = 0;
    for (size_t i = 0; i <= length; ++i) {
        if (i < length && text[i] != '\n') continue;
        if (i == length && start == length) break;     /* no line after the last line end */
        size_t end = i;
        if (end > start && text[end - 1] == '\r') { end--; crlf++; } else if (i < length) lf++;
        char *line = duplicate(text + start, end - start);
        if (!line || !insert_line(doc, doc->count, "")) { free(line); zr_ydoc_free(doc); return 0; }
        free(doc->lines[doc->count - 1]);
        doc->lines[doc->count - 1] = line;
        start = i + 1;
    }
    doc->crlf = crlf > lf;
    return 1;
}
int zr_ydoc_load(zr_ydoc *doc, const char *path) {
    memset(doc, 0, sizeof *doc);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char *text = NULL;
    long size = -1;
    if (!fseek(f, 0, SEEK_END)) size = ftell(f);
    int ok = size >= 0 && size < 16 * 1024 * 1024 && !fseek(f, 0, SEEK_SET) && (text = malloc((size_t)size + 1)) != NULL &&
             fread(text, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    if (ok) ok = zr_ydoc_parse(doc, text, (size_t)size);
    free(text);
    return ok;
}
char *zr_ydoc_text(const zr_ydoc *doc, size_t *length) {
    size_t n = 1;
    for (size_t i = 0; i < doc->count; ++i) n += strlen(doc->lines[i]) + 2;
    char *text = malloc(n), *p = text;
    if (!text) return NULL;
    for (size_t i = 0; i < doc->count; ++i) {
        const size_t k = strlen(doc->lines[i]);
        memcpy(p, doc->lines[i], k);
        p += k;
        if (doc->crlf) *p++ = '\r';
        *p++ = '\n';
    }
    *p = 0;
    if (length) *length = (size_t)(p - text);
    return text;
}
int zr_ydoc_save(const zr_ydoc *doc, const char *path) {
    size_t length = 0;
    char *text = zr_ydoc_text(doc, &length);
    if (!text) return 0;
    char temporary[1100];
    snprintf(temporary, sizeof temporary, "%s.saving", path);
    FILE *f = fopen(temporary, "wb");
    int ok = f && fwrite(text, 1, length, f) == length;
    if (f && fclose(f)) ok = 0;
    free(text);
    if (ok) ok = zr_host_replace_file(temporary, path);
    if (!ok) remove(temporary);
    return ok;
}
void zr_ydoc_free(zr_ydoc *doc) {
    for (size_t i = 0; i < doc->count; ++i) free(doc->lines[i]);
    free(doc->lines);
    memset(doc, 0, sizeof *doc);
}

/* --- scalars --------------------------------------------------------------------- */

/* A quoted or plain scalar starting at s; its text into out, and the
 * position after it. */
static const char *read_scalar(const char *s, char *out, size_t capacity, const char *stop) {
    size_t n = 0;
    if (*s == '\'') {
        for (++s; *s; ++s) {
            if (*s == '\'' && s[1] == '\'') { s++; }
            else if (*s == '\'') { s++; break; }
            if (n + 1 < capacity) out[n++] = *s;
        }
    } else if (*s == '"') {
        for (++s; *s && *s != '"'; ++s) {
            char c = *s;
            if (c == '\\' && s[1]) {
                c = *++s;
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
            }
            if (n + 1 < capacity) out[n++] = c;
        }
        if (*s == '"') s++;
    } else {
        const char *start = s;
        for (; *s; ++s) {
            if (strchr(stop, *s) && (*s != ':' || s[1] == ' ' || s[1] == 0 || strchr(stop, s[1]))) break;
            if (*s == '#' && s > start && s[-1] == ' ') break;
        }
        const char *end = s;
        while (end > start && end[-1] == ' ') end--;
        for (const char *p = start; p < end; ++p) if (n + 1 < capacity) out[n++] = *p;
    }
    if (capacity) out[n] = 0;
    return s;
}

static int plain_safe(const char *s) {
    static const char *const special[] = {"true", "false", "null", "yes", "no", "on", "off", "y", "n", "~", NULL};
    if (!*s || strchr("-?:,[]{}#&*!|>'\"%@` ", *s)) return 0;
    for (const char *const *w = special; *w; ++w) if (same_text(s, *w)) return 0;
    for (const char *p = s; *p; ++p) {
        const unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0;
        if (c == ':' && (p[1] == ' ' || !p[1])) return 0;
        if (c == '#' && p > s && p[-1] == ' ') return 0;
        if (strchr("{}[],", c)) return 0;
    }
    return s[strlen(s) - 1] != ' ';
}
const char *zr_ydoc_quote(const char *s, char *buffer, size_t capacity) {
    if (plain_safe(s)) { snprintf(buffer, capacity, "%s", s); return buffer; }
    size_t n = 0;
    if (n + 1 < capacity) buffer[n++] = '"';
    for (const char *p = s; *p && n + 3 < capacity; ++p) {
        if (*p == '"' || *p == '\\') buffer[n++] = '\\';
        buffer[n++] = *p;
    }
    if (n + 1 < capacity) buffer[n++] = '"';
    buffer[n < capacity ? n : capacity - 1] = 0;
    return buffer;
}
const char *zr_ydoc_quote_path(const char *s, char *buffer, size_t capacity) {
    if (plain_safe(s) && !strchr(s, '\\') && !strchr(s, ':')) { snprintf(buffer, capacity, "%s", s); return buffer; }
    size_t n = 0;
    if (n + 1 < capacity) buffer[n++] = '\'';
    for (const char *p = s; *p && n + 3 < capacity; ++p) {
        if (*p == '\'') buffer[n++] = '\'';
        buffer[n++] = *p;
    }
    if (n + 1 < capacity) buffer[n++] = '\'';
    buffer[n < capacity ? n : capacity - 1] = 0;
    return buffer;
}

/* --- entries ----------------------------------------------------------------------- */

typedef struct entry {
    size_t first, last;        /* the lines of the entry itself */
    int indent;
    int item;                  /* "- ..." */
    char key[128];
    size_t value;              /* column of the value on the first line (after "key:" and spaces) */
} entry;

static int indent_of(const char *line) { int n = 0; while (line[n] == ' ') n++; return n; }
static int blank(const char *line) { return line[indent_of(line)] == 0; }
static int comment(const char *line) { return line[indent_of(line)] == '#'; }

/* The column where a line's comment starts (strlen when none), outside
 * quotes. */
static size_t comment_column(const char *line, size_t from) {
    char quote = 0;
    for (size_t i = from; line[i]; ++i) {
        const char c = line[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) return i;
    }
    return strlen(line);
}
/* Open brackets of the text before its comment. */
static int depth_change(const char *line, size_t from) {
    int depth = 0;
    char quote = 0;
    const size_t end = comment_column(line, from);
    for (size_t i = from; i < end; ++i) {
        const char c = line[i];
        if (quote) { if (c == quote) quote = 0; continue; }
        if (c == '\'' || c == '"') quote = c;
        else if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
    }
    return depth;
}

/* The entry at line i (a content line), and the lines it spans. */
static int entry_at(const zr_ydoc *doc, size_t i, entry *e) {
    memset(e, 0, sizeof *e);
    const char *line = doc->lines[i];
    e->first = e->last = i;
    e->indent = indent_of(line);
    const char *s = line + e->indent;
    if (s[0] == '-' && (s[1] == ' ' || s[1] == 0)) {
        e->item = 1;
        e->value = (size_t)(s - line) + 1;
        while (line[e->value] == ' ') e->value++;
    } else {
        const char *after = read_scalar(s, e->key, sizeof e->key, ":");
        while (*after == ' ') after++;
        if (*after != ':') return 0;
        after++;
        while (*after == ' ') after++;
        e->value = (size_t)(after - line);
    }
    int depth = depth_change(line, e->value);
    while (depth > 0 && e->last + 1 < doc->count) {
        e->last++;
        depth += depth_change(doc->lines[e->last], 0);
    }
    return 1;
}
/* The column after a key line's colon. */
static size_t colon_end(const char *line, int indent) {
    char scratch[128];
    const char *after = read_scalar(line + indent, scratch, sizeof scratch, ":");
    while (*after == ' ') after++;
    return (size_t)(after - line) + (*after == ':');
}
/* The value text on the entry's first line, without its comment. */
static void inline_value(const zr_ydoc *doc, const entry *e, char *out, size_t capacity) {
    const char *line = doc->lines[e->first];
    size_t end = comment_column(line, e->value);
    while (end > e->value && line[end - 1] == ' ') end--;
    snprintf(out, capacity, "%.*s", (int)(end - e->value), line + e->value);
}
/* The last line of the entry's block (its own last line when it has none). */
static size_t block_end(const zr_ydoc *doc, const entry *e) {
    size_t end = e->last;
    char value[8];
    inline_value(doc, e, value, sizeof value);
    const int open = !e->item && !value[0];      /* an indentless "- " sequence may follow */
    for (size_t i = e->last + 1; i < doc->count; ++i) {
        const char *line = doc->lines[i];
        if (blank(line)) continue;
        const int in = indent_of(line);
        if (comment(line)) {
            if (in > e->indent) { end = i; continue; }
            break;
        }
        if (in > e->indent || (open && in == e->indent && line[in] == '-' && (line[in + 1] == ' ' || !line[in + 1]))) {
            entry child;
            end = entry_at(doc, i, &child) ? child.last : i;
            i = end;
            continue;
        }
        break;
    }
    return end;
}

/* The children of a block: the entries at its first content indent.
 * parent NULL: the top level. Calls visit for each; stops when it returns 1. */
typedef int (*visitor)(const zr_ydoc *doc, const entry *e, void *ctx);
static int children(const zr_ydoc *doc, const entry *parent, visitor visit, void *ctx, int *child_indent) {
    size_t from = parent ? parent->last + 1 : 0, to = parent ? block_end(doc, parent) : doc->count - 1;
    if (!doc->count) return 0;
    int level = -1;
    for (size_t i = from; i <= to && i < doc->count; ++i) {
        const char *line = doc->lines[i];
        if (blank(line) || comment(line)) continue;
        entry e;
        if (!entry_at(doc, i, &e)) continue;
        if (level < 0) { level = e.indent; if (child_indent) *child_indent = level; }
        if (e.indent == level && visit && visit(doc, &e, ctx)) return 1;
        i = e.last;
    }
    if (level < 0 && child_indent) *child_indent = parent ? parent->indent + 2 : 0;
    return 0;
}
typedef struct finding { const char *key; entry found; int hit; } finding;
static int match(const zr_ydoc *doc, const entry *e, void *ctx) {
    (void)doc;
    finding *f = ctx;
    if (e->item || strcmp(e->key, f->key)) return 0;
    f->found = *e;
    f->hit = 1;
    return 1;
}
static int find(const zr_ydoc *doc, const char *const *path, size_t depth, entry *out) {
    entry parent, *at = NULL;
    memset(&parent, 0, sizeof parent);
    for (size_t k = 0; k < depth; ++k) {
        finding f = {path[k], {0}, 0};
        children(doc, at, match, &f, NULL);
        if (!f.hit) return 0;
        parent = f.found;
        at = &parent;
    }
    if (out && at) *out = parent;
    return depth > 0;
}
int zr_ydoc_has(const zr_ydoc *doc, const char *const *path, size_t depth) { return find(doc, path, depth, NULL); }

int zr_ydoc_get(const zr_ydoc *doc, const char *const *path, size_t depth, char *out, size_t capacity) {
    entry e;
    if (!find(doc, path, depth, &e)) return 0;
    char value[1100];
    inline_value(doc, &e, value, sizeof value);
    if (!value[0] || value[0] == '{' || value[0] == '[' || value[0] == '|' || value[0] == '>') return 0;
    read_scalar(value, out, capacity, "");
    return 1;
}

/* --- flow mappings into block style ------------------------------------------------- */

/* The items of a flow collection's text ("{a: 1, b: [2, 3]}"): the text
 * between its brackets split at the top-level commas. */
static size_t flow_items(const char *text, char items[][256], size_t max) {
    const char *s = text;
    while (*s == ' ') s++;
    if (*s != '{' && *s != '[') return 0;
    s++;
    size_t n = 0, k = 0;
    int depth = 0;
    char quote = 0;
    for (; *s; ++s) {
        const char c = *s;
        if (quote) { if (c == quote) quote = 0; }
        else if (c == '\'' || c == '"') quote = c;
        else if (c == '{' || c == '[') depth++;
        else if ((c == '}' || c == ']') && depth-- == 0) break;
        else if (c == ',' && depth == 0) {
            if (n < max) { items[n][k] = 0; n++; }
            k = 0;
            continue;
        }
        if (n < max && k + 1 < 256 && !(k == 0 && c == ' ')) items[n][k++] = c;
    }
    if (n < max) {
        while (k && items[n][k - 1] == ' ') k--;
        items[n][k] = 0;
        if (k) n++;
    }
    return n;
}

/* Rewrites an entry whose value is a flow mapping in block style. */
static int expand(zr_ydoc *doc, const entry *e) {
    char value[4096];
    const char *line = doc->lines[e->first];
    /* The whole flow text, over its lines. */
    size_t used = 0;
    for (size_t i = e->first; i <= e->last; ++i) {
        const char *s = doc->lines[i] + (i == e->first ? e->value : 0);
        const size_t end = comment_column(doc->lines[i], i == e->first ? e->value : 0) - (size_t)(s - doc->lines[i]);
        used += (size_t)snprintf(value + used, sizeof value - used, "%s%.*s", i == e->first ? "" : " ", (int)end, s);
        if (used >= sizeof value) return 0;
    }
    char items[64][256];
    const size_t n = flow_items(value, items, 64);
    char head[1100];
    const size_t comment_at = comment_column(line, e->value);
    snprintf(head, sizeof head, "%.*s", (int)e->value, line);
    /* "key: " keeps no trailing space; a comment stays after the colon. */
    size_t h = strlen(head);
    while (h && head[h - 1] == ' ') head[--h] = 0;
    if (line[comment_at]) snprintf(head + h, sizeof head - h, " %s", line + comment_at);
    if (!replace_line(doc, e->first, head)) return 0;
    if (e->last > e->first) delete_lines(doc, e->first + 1, e->last - e->first);
    for (size_t i = 0; i < n; ++i) {
        char text[560];
        size_t pad = (size_t)(e->indent + 2);
        if (pad > 300) pad = 300;
        const size_t length = strlen(items[i]);
        memset(text, ' ', pad);
        memcpy(text + pad, items[i], length);
        text[pad + length] = 0;
        if (!insert_line(doc, e->first + 1 + i, text)) return 0;
    }
    return 1;
}

/* --- edits -------------------------------------------------------------------------- */

/* Makes the mapping at path exist in block style; its entry into out
 * (depth 0: the top level, nothing). */
static int ensure_mapping(zr_ydoc *doc, const char *const *path, size_t depth, entry *out);

/* Writes "key: value" (value NULL: "key:") as a new child of the mapping at
 * path; returns its line. */
static long add_child(zr_ydoc *doc, const char *const *path, size_t depth, const char *key, const char *value) {
    entry parent;
    memset(&parent, 0, sizeof parent);
    if (!ensure_mapping(doc, path, depth, &parent)) return -1;
    int level = 0;
    children(doc, depth ? &parent : NULL, NULL, NULL, &level);
    char quoted[300], text[4400];
    zr_ydoc_quote(key, quoted, sizeof quoted);
    snprintf(text, sizeof text, "%*s%s:%s%s", level, "", quoted, value ? " " : "", value ? value : "");
    /* The template's commented-out line for the key. */
    const size_t from = depth ? parent.last + 1 : 0, to = depth ? block_end(doc, &parent) : (doc->count ? doc->count - 1 : 0);
    char hint[304];
    snprintf(hint, sizeof hint, "# %s:", quoted);
    for (size_t i = from; doc->count && i <= to; ++i) {
        const char *line = doc->lines[i];
        if (comment(line) && indent_of(line) == level && !strncmp(line + level, hint, strlen(hint)))
            return replace_line(doc, i, text) ? (long)i : -1;
    }
    size_t at = depth ? block_end(doc, &parent) + 1 : doc->count;
    /* At the top level a new section is set off by a blank line. */
    if (!depth && doc->count && !blank(doc->lines[doc->count - 1])) {
        if (!insert_line(doc, at, "")) return -1;
        at++;
    }
    return insert_line(doc, at, text) ? (long)at : -1;
}

static int ensure_mapping(zr_ydoc *doc, const char *const *path, size_t depth, entry *out) {
    if (!depth) return 1;
    entry e, parent;
    /* The mapping above in block style first: a flow one hides its keys. */
    if (depth > 1 && !ensure_mapping(doc, path, depth - 1, &parent)) return 0;
    if (!find(doc, path, depth, &e)) {
        if (add_child(doc, path, depth - 1, path[depth - 1], NULL) < 0) return 0;
        return find(doc, path, depth, out);
    }
    char value[256];
    inline_value(doc, &e, value, sizeof value);
    if (value[0] == '{') {
        if (!expand(doc, &e)) return 0;
        return find(doc, path, depth, out);
    }
    if (value[0]) {
        /* A scalar where a mapping belongs: replaced. */
        char head[1100];
        snprintf(head, sizeof head, "%.*s", (int)e.value, doc->lines[e.first]);
        size_t h = strlen(head);
        while (h && head[h - 1] == ' ') head[--h] = 0;
        if (!replace_line(doc, e.first, head)) return 0;
        if (e.last > e.first) delete_lines(doc, e.first + 1, e.last - e.first);
        return find(doc, path, depth, out);
    }
    *out = e;
    return 1;
}

/* Clears an entry's value: its inline text and its block. */
static int clear_value(zr_ydoc *doc, const entry *e) {
    size_t end = block_end(doc, e);
    /* Comment lines after the block's last real line go on with the entry's own comment (or begin the next one's): they stay. */
    while (end > e->last && (comment(doc->lines[end]) || blank(doc->lines[end]))) --end;
    const char *line = doc->lines[e->first];
    const size_t comment_at = comment_column(line, e->value);
    char head[1100];
    snprintf(head, sizeof head, "%.*s", (int)e->value, line);
    size_t h = strlen(head);
    while (h && head[h - 1] == ' ') head[--h] = 0;
    if (line[comment_at]) snprintf(head + h, sizeof head - h, " %s", line + comment_at);
    if (!replace_line(doc, e->first, head)) return 0;
    if (end > e->first) delete_lines(doc, e->first + 1, end - e->first);
    return 1;
}

int zr_ydoc_set(zr_ydoc *doc, const char *const *path, size_t depth, const char *value) {
    if (!depth) return 0;
    entry e, parent;
    if (depth > 1 && !ensure_mapping(doc, path, depth - 1, &parent)) return 0;
    if (!find(doc, path, depth, &e)) return add_child(doc, path, depth - 1, path[depth - 1], value) >= 0;
    if (!clear_value(doc, &e)) return 0;
    const char *line = doc->lines[e.first];
    const size_t h = colon_end(line, e.indent);
    const char *rest = line + h;
    while (*rest == ' ') rest++;
    char text[4400];
    snprintf(text, sizeof text, "%.*s %s%s%s", (int)h, line, value, *rest ? " " : "", rest);
    return replace_line(doc, e.first, text);
}

int zr_ydoc_expand(zr_ydoc *doc, const char *const *path, size_t depth) {
    const char *sub[16];
    if (depth >= 15) return 1;
    for (size_t i = 0; i < depth; ++i) sub[i] = path[i];
    if (depth) {
        entry e;
        if (!find(doc, path, depth, &e)) return 1;
        char value[8];
        inline_value(doc, &e, value, sizeof value);
        if (value[0] == '{' && !expand(doc, &e)) return 0;
    }
    static char keys[16][64][128];
    const size_t n = zr_ydoc_keys(doc, path, depth, keys[depth], 64);
    for (size_t i = 0; i < n; ++i) {
        sub[depth] = keys[depth][i];
        if (!zr_ydoc_expand(doc, sub, depth + 1)) return 0;
    }
    return 1;
}

/* Whether an entry holds a mapping with no keys left: no inline value and
 * no child entry (comments may remain). */
static int counts(const zr_ydoc *doc, const entry *e, void *ctx) { (void)doc; (void)e; ++*(int *)ctx; return 0; }
static int emptied(const zr_ydoc *doc, const entry *e) {
    char value[8];
    inline_value(doc, e, value, sizeof value);
    if (value[0]) return 0;
    int n = 0;
    children(doc, e, counts, &n, NULL);
    return n == 0;
}
static int remove_entry(zr_ydoc *doc, const char *const *path, size_t depth);
/* After a removal: a mapping left without keys would read as null, which
 * no section accepts ("Expected a mapping"). A mapping inside a section
 * goes with its last key; a top-level section keeps its comments as
 * "key: {}". */
static int prune(zr_ydoc *doc, const char *const *path, size_t depth) {
    entry e;
    if (!depth || !find(doc, path, depth, &e) || !emptied(doc, &e)) return 1;
    if (depth == 1) {
        const char *line = doc->lines[e.first];
        const size_t h = colon_end(line, e.indent), comment_at = comment_column(line, h);
        char text[4400];
        snprintf(text, sizeof text, "%.*s {}%s%s", (int)h, line, line[comment_at] ? " " : "", line + comment_at);
        return replace_line(doc, e.first, text);
    }
    return remove_entry(doc, path, depth);
}

static int remove_entry(zr_ydoc *doc, const char *const *path, size_t depth) {
    entry e;
    if (!find(doc, path, depth, &e)) return 1;
    if (depth > 1) {
        /* A key of a flow mapping: the mapping in block style first. */
        entry parent;
        if (find(doc, path, depth - 1, &parent)) {
            char value[8];
            inline_value(doc, &parent, value, sizeof value);
            if (value[0] == '{') {
                if (!expand(doc, &parent) || !find(doc, path, depth, &e)) return 0;
            }
        }
    }
    const size_t end = block_end(doc, &e);
    delete_lines(doc, e.first, end - e.first + 1);
    return prune(doc, path, depth - 1);
}
int zr_ydoc_remove(zr_ydoc *doc, const char *const *path, size_t depth) { return remove_entry(doc, path, depth); }

typedef struct listing { char (*keys)[128]; size_t count, max; } listing;
static int collect(const zr_ydoc *doc, const entry *e, void *ctx) {
    (void)doc;
    listing *l = ctx;
    if (!e->item && l->count < l->max) snprintf(l->keys[l->count++], 128, "%s", e->key);
    return 0;
}
size_t zr_ydoc_keys(const zr_ydoc *doc, const char *const *path, size_t depth, char keys[][128], size_t max) {
    listing l = {keys, 0, max};
    entry e;
    if (depth && !find(doc, path, depth, &e)) return 0;
    if (depth) {
        char value[4096];
        inline_value(doc, &e, value, sizeof value);
        if (value[0] == '{') {
            char items[64][256];
            const size_t n = flow_items(value, items, 64);
            for (size_t i = 0; i < n && l.count < max; ++i) read_scalar(items[i], keys[l.count++], 128, ":");
            return l.count;
        }
    }
    children(doc, depth ? &e : NULL, collect, &l, NULL);
    return l.count;
}
