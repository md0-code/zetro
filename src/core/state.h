/* Saved states: ZetRo's own format, the same for
 * every machine. A state is a header and a list of chunks:
 *
 *   "ZETROST1"                      magic
 *   u32  format version             ZR_STATE_VERSION
 *   str  family id, str model id    ("jet", "jet"); str is a u16 length and the bytes
 *   u32  count, then count x 32     the SHA-256 of each ROM the machine runs
 *   chunks                          tag (4 bytes), u32 instance, u32 length, the bytes
 *   "END ", 0, 0
 *
 * A chunk holds one chip's or one memory block's state (tag "Z80 ", "ULA ",
 * "RAM "; the instance tells two chips of a kind apart). A reader skips the
 * chunks it does not know and finds the ones it wants by tag and instance,
 * whatever their order, so a later version may add chunks, and a chip may
 * add fields at the end of its chunk (a reader that runs off the end of an
 * older chunk reads zeros). Media are not in a state: a state names them by
 * path and hash ("MDIA" chunks, the shell's), and the shell puts them back.
 *
 * All numbers are little-endian. Memory is run-length packed (zr_sw_packed).
 * The writer grows a buffer in memory, so the same serialisation feeds the
 * state files and the rewind ring. No OS call here: the shell reads and
 * writes the files. ZetRo's own code. */
#ifndef ZR_STATE_H
#define ZR_STATE_H

#include <stddef.h>
#include <stdint.h>

#define ZR_STATE_VERSION 1u
#define ZR_STATE_ID 32                  /* a family or model id with its end */

/* --- writing ---------------------------------------------------------------------- */

typedef struct zr_state_writer {
    uint8_t *data;
    size_t size, capacity;
    size_t chunk;                       /* where the open chunk's length field is; 0: none open */
    int failed;                         /* out of memory: everything after is dropped */
} zr_state_writer;

void zr_sw_init(zr_state_writer *w);
void zr_sw_free(zr_state_writer *w);
/* The header: what machine this is a state of. `hashes` is count x 32 bytes. */
void zr_sw_header(zr_state_writer *w, const char *family, const char *model, const uint8_t *hashes, uint32_t count);
/* A chunk: begin, the fields, end. Chunks do not nest. */
void zr_sw_begin(zr_state_writer *w, const char tag[4], uint32_t instance);
void zr_sw_end(zr_state_writer *w);
void zr_sw_u8(zr_state_writer *w, uint8_t v);
void zr_sw_u16(zr_state_writer *w, uint16_t v);
void zr_sw_u32(zr_state_writer *w, uint32_t v);
void zr_sw_u64(zr_state_writer *w, uint64_t v);
void zr_sw_bytes(zr_state_writer *w, const void *data, size_t length);
void zr_sw_str(zr_state_writer *w, const char *text);
/* `length` bytes of memory, run-length packed: a u32 length, then runs. */
void zr_sw_packed(zr_state_writer *w, const uint8_t *data, size_t length);
/* The end mark. 1 when the whole state is in w->data (w->size bytes). */
int zr_sw_finish(zr_state_writer *w);

/* --- reading ---------------------------------------------------------------------- */

typedef struct zr_state_reader {
    const uint8_t *data;
    size_t size;
    size_t body;                        /* where the chunks start */
    size_t at, end;                     /* the open chunk's read position and end */
    int failed;                         /* a field was read that the state does not have right */
    char family[ZR_STATE_ID], model[ZR_STATE_ID];
    const uint8_t *hashes;
    uint32_t hash_count;
} zr_state_reader;

/* Reads the header; 0 with the reason when this is not a state ZetRo reads. */
int zr_sr_open(zr_state_reader *r, const uint8_t *data, size_t size, char *error, size_t capacity);
/* Opens the chunk with this tag and instance for the calls below; 0 when the
 * state has none (the fields then read as zero). */
int zr_sr_chunk(zr_state_reader *r, const char tag[4], uint32_t instance);
/* How many chunks carry this tag (the instances are not always 0..n-1). */
uint32_t zr_sr_count(const zr_state_reader *r, const char tag[4]);
uint8_t  zr_sr_u8(zr_state_reader *r);
uint16_t zr_sr_u16(zr_state_reader *r);
uint32_t zr_sr_u32(zr_state_reader *r);
uint64_t zr_sr_u64(zr_state_reader *r);
void zr_sr_bytes(zr_state_reader *r, void *data, size_t length);
/* A string of at most capacity - 1 bytes (a longer one is cut). */
void zr_sr_str(zr_state_reader *r, char *out, size_t capacity);
/* Memory packed by zr_sw_packed: exactly `length` bytes or the reader fails. */
void zr_sr_packed(zr_state_reader *r, uint8_t *data, size_t length);
/* 1 while every field read so far was there in the right shape. */
int zr_sr_ok(const zr_state_reader *r);

#endif /* ZR_STATE_H */
