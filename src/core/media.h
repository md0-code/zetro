/* A media file as the machine holds it: the whole file in memory, with its
 * path and whether it may be written back. The formats (tapes, disk images,
 * cards, ROM sets) are read from these bytes by the modules of core/media/;
 * this is only the file. A machine that changes the content sets `dirty`,
 * and the shell writes the file back (zr_media_save) when the media is
 * ejected, at intervals and at exit. Large images used in place (cards) are
 * opened by their own module, not read through this: such a media only names
 * the file (in_place, zr_media_open_in_place). ZetRo's own code. */
#ifndef ZR_MEDIA_H
#define ZR_MEDIA_H

#include <stddef.h>
#include <stdint.h>

#define ZR_MEDIA_PATH 1024
#define ZR_MEDIA_LIMIT (64u * 1024u * 1024u)    /* nothing larger is read whole */

typedef struct zr_media {
    char path[ZR_MEDIA_PATH];
    uint8_t *data;
    size_t size;
    int writable;                       /* the file can be written and is not write-protected */
    int dirty;                          /* the content differs from the file */
    int in_place;                       /* a card image: only the path is held, the machine opens the file itself and uses it in place */
    char error[128];                    /* why the last save failed */
} zr_media;

/* Reads the file; NULL with the reason. `write_protect` makes it read-only
 * whatever the file allows. */
zr_media *zr_media_open(const char *path, int write_protect, char *error, size_t capacity);
/* The media of a file used in place (a card image: large, read and written a sector at a time by media/card.h): nothing is read,
 * `data` is NULL and `size` 0; the file must exist. `writable` says whether it may be written. NULL with the reason. */
zr_media *zr_media_open_in_place(const char *path, int write_protect, char *error, size_t capacity);
/* A media made of bytes (copied), for checks and for files made in memory. */
zr_media *zr_media_from(const char *path, const uint8_t *data, size_t size);
/* Makes the content `size` bytes (new bytes are zero); 0 when out of memory. */
int zr_media_resize(zr_media *m, size_t size);
/* Writes a changed, writable media back; 1 when the file is up to date. */
int zr_media_save(zr_media *m);
void zr_media_close(zr_media *m);
/* The file name's ending in lower case without its dot ("tzx"; "" for none). */
void zr_media_extension(const char *path, char *out, size_t capacity);
/* 1 when the ending is in a list such as "tap,tzx,pzx". */
int zr_media_extension_in(const char *path, const char *list);

#endif /* ZR_MEDIA_H */
