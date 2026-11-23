/* ZetRo - a Z80 board's bus: the 64 KiB memory space and the 64 Ki port
 * I/O space, as one board wires them.
 *
 * Memory is 64 pages of 1 KiB. Each page has a read side and a write side,
 * set independently and changed at any time (paged ROMs and RAM banks,
 * interfaces that overlay the ROM, a relocated screen):
 *   - direct: a byte array the bus reads or writes itself (RAM: both sides
 *     on the same bytes; ROM: the read side only);
 *   - a handler: a function pair of the board's (a chip that answers in
 *     memory, a write to the ROM area that a cartridge watches);
 *   - nothing: reads give FFh (no floating bus), writes are lost.
 * The read path of a direct page is one table lookup (zr_bus_read). Pages
 * marked "watched" call the board's write hook before a write lands, with
 * the address and the byte: the board brings the video up to the cycle's
 * time there (zr_ula_catch_up) before video memory changes. A board marks
 * the pages its video reads and nothing else, so the other writes pay one
 * test.
 *
 * I/O is a list of decoders, each a mask/value match on the 16-bit port
 * address ((port & mask) == value), with a read and a write handler (either
 * NULL: that side does not answer). These machines decode ports partially,
 * and several devices may answer one address: every matching decoder's
 * write runs, in the order they were added; a read gives the AND of the
 * bytes of every matching reader (each pulls its 0 bits down; FFh when
 * none answers). A decoder can be switched off and on (an interface that is
 * fitted or not, a port a latch disables).
 *
 * A trace tap (zr_trace_fn, machine.h's kinds) sees each port cycle and
 * each interrupt acknowledge the board reports (zr_bus_trace).
 *
 * The overlay (zr_bus_overlay_*) is a layer above the board's own map, for
 * an added interface that takes the ROM area as a cartridge does (its
 * ROMCS line silences the board's ROM): its read side covers a page while
 * it is set there, except where the board has RAM (the same bytes on both
 * sides: a paging mode with RAM at 0000h keeps it), and the board keeps
 * mapping the page underneath as it always does, which shows again when
 * the overlay goes. Its write side is a tap: the interface sees each write
 * to the page, which then lands as the board's map says (RAM under a ROM
 * gets it). With no overlay set nothing of it is on the read path.
 *
 * The bus holds no time: the board's callbacks know the cycle's time.
 * Portable C11. ZetRo's own code. */
#ifndef ZR_HW_BUS_H
#define ZR_HW_BUS_H

#include "hw.h"

#define ZR_BUS_PAGE_BITS 10u
#define ZR_BUS_PAGE_SIZE (1u << ZR_BUS_PAGE_BITS)
#define ZR_BUS_PAGES (0x10000u >> ZR_BUS_PAGE_BITS)
#define ZR_BUS_DECODERS 32

/* The kinds of a trace record: the same numbers as machine.h's ZR_TRACE_*. */
#define ZR_BUS_TRACE_IN 0
#define ZR_BUS_TRACE_OUT 1
#define ZR_BUS_TRACE_FETCH 2
#define ZR_BUS_TRACE_INTERRUPT 3
typedef void (*zr_bus_trace_fn)(void *ctx, int kind, uint32_t address, uint32_t value);

typedef uint8_t (*zr_bus_read_fn)(void *ctx, uint16_t address);
typedef void (*zr_bus_write_fn)(void *ctx, uint16_t address, uint8_t value);

typedef struct zr_bus_handler {
    zr_bus_read_fn read;
    zr_bus_write_fn write;
    void *ctx;
} zr_bus_handler;

typedef struct zr_bus_decoder {
    uint16_t mask, value;
    uint8_t enabled;
    zr_bus_read_fn read;                /* NULL: does not answer reads */
    zr_bus_write_fn write;              /* NULL: does not take writes */
    void *ctx;
} zr_bus_decoder;

typedef struct zr_bus {
    /* Memory: direct pages by pointer to the page's first byte (NULL: a handler or nothing). */
    const uint8_t *rd[ZR_BUS_PAGES];
    uint8_t *wr_fast[ZR_BUS_PAGES];     /* direct and not watched: written without a call */
    uint8_t *wr[ZR_BUS_PAGES];          /* direct, watched or not */
    zr_bus_handler rh[ZR_BUS_PAGES], wh[ZR_BUS_PAGES];  /* handlers where the side is one */
    uint8_t watched[ZR_BUS_PAGES];
    zr_bus_write_fn hook;               /* the write hook of watched pages */
    void *hook_ctx;
    /* I/O */
    zr_bus_decoder dec[ZR_BUS_DECODERS];
    unsigned decoders;
    /* the trace */
    zr_bus_trace_fn trace;
    void *trace_ctx;
    /* the overlay, when one is attached (NULL: none, nothing of it anywhere) */
    struct zr_bus_overlay *ov;
} zr_bus;

/* The overlay's storage (the caller's, zr_bus_overlay_attach): the board's own read side under it, the overlay's read
 * side (direct, or a handler) and its write tap, per page; `on` bit 0 the read side is set, bit 1 the tap. */
typedef struct zr_bus_overlay {
    const uint8_t *low_rd[ZR_BUS_PAGES];
    zr_bus_handler low_rh[ZR_BUS_PAGES];
    const uint8_t *rd[ZR_BUS_PAGES];
    zr_bus_handler rh[ZR_BUS_PAGES], wh[ZR_BUS_PAGES];
    uint8_t on[ZR_BUS_PAGES];
} zr_bus_overlay;

/* An empty bus: every page and port unmapped. */
void zr_bus_init(zr_bus *b);

/* --- the memory map ------------------------------------------------------------------- */

/* The sides a mapping sets. */
enum { ZR_BUS_READ = 1, ZR_BUS_WRITE = 2, ZR_BUS_BOTH = 3 };
/* Direct memory: `size` bytes of `data` at `first` (both multiples of
 * ZR_BUS_PAGE_SIZE) on the read side, or on the write side; NULL data
 * leaves the side with nothing. The bytes stay the caller's. 0 when
 * misaligned. */
int zr_bus_map_read(zr_bus *b, uint16_t first, uint32_t size, const uint8_t *data);
int zr_bus_map_write(zr_bus *b, uint16_t first, uint32_t size, uint8_t *data);
/* RAM: both sides on the same bytes. ROM: the read side; writes are lost
 * (unless a handler takes them: zr_bus_map_handler with ZR_BUS_WRITE). */
static inline int zr_bus_map_ram(zr_bus *b, uint16_t first, uint32_t size, uint8_t *data) {
    return zr_bus_map_read(b, first, size, data) && zr_bus_map_write(b, first, size, data);
}
static inline int zr_bus_map_rom(zr_bus *b, uint16_t first, uint32_t size, const uint8_t *data) {
    return zr_bus_map_read(b, first, size, data) && zr_bus_map_write(b, first, size, NULL);
}
/* A handler on the sides named (its NULL functions: reads FFh, writes lost). */
int zr_bus_map_handler(zr_bus *b, uint16_t first, uint32_t size, const zr_bus_handler *h, unsigned sides);
/* Nothing on the sides named. */
static inline int zr_bus_unmap(zr_bus *b, uint16_t first, uint32_t size, unsigned sides) {
    return (!(sides & ZR_BUS_READ) || zr_bus_map_read(b, first, size, NULL)) && (!(sides & ZR_BUS_WRITE) || zr_bus_map_write(b, first, size, NULL));
}
/* The write hook, and the pages that call it (on 1, off 0). */
void zr_bus_set_hook(zr_bus *b, zr_bus_write_fn hook, void *ctx);
int zr_bus_watch(zr_bus *b, uint16_t first, uint32_t size, int on);

/* An overlay on the bus, empty, in `storage` (which stays the caller's while the bus lives). The overlay calls below
 * need it. */
void zr_bus_overlay_attach(zr_bus *b, zr_bus_overlay *storage);
/* The overlay's read side on the pages: `data` read directly, else the handler's read (both NULL: the overlay leaves
 * the pages, the board's map shows again). Not over RAM (see above). */
int zr_bus_overlay_read(zr_bus *b, uint16_t first, uint32_t size, const uint8_t *data, const zr_bus_handler *h);
/* The overlay's write tap on the pages: h->write sees every write there before it lands (NULL: none). */
int zr_bus_overlay_watch(zr_bus *b, uint16_t first, uint32_t size, const zr_bus_handler *h);

/* --- cycles ----------------------------------------------------------------------------- */

uint8_t zr_bus_read_slow(zr_bus *b, uint16_t address);
void zr_bus_write_slow(zr_bus *b, uint16_t address, uint8_t value);
static inline uint8_t zr_bus_read(zr_bus *b, uint16_t address) {
    const uint8_t *p = b->rd[address >> ZR_BUS_PAGE_BITS];
    return p ? p[address & (ZR_BUS_PAGE_SIZE - 1u)] : zr_bus_read_slow(b, address);
}
static inline void zr_bus_write(zr_bus *b, uint16_t address, uint8_t value) {
    uint8_t *p = b->wr_fast[address >> ZR_BUS_PAGE_BITS];
    if (p) p[address & (ZR_BUS_PAGE_SIZE - 1u)] = value;
    else zr_bus_write_slow(b, address, value);
}
/* The byte a read would give, without side effects: a handler page reads FFh. */
uint8_t zr_bus_peek(const zr_bus *b, uint16_t address);
/* Where a write to `address` lands in direct memory, NULL when it does not. */
static inline uint8_t *zr_bus_target(const zr_bus *b, uint16_t address) {
    uint8_t *p = b->wr[address >> ZR_BUS_PAGE_BITS];
    return p ? p + (address & (ZR_BUS_PAGE_SIZE - 1u)) : NULL;
}

/* --- ports ------------------------------------------------------------------------------- */

/* A decoder, enabled; its index, or -1 when the table is full. */
int zr_bus_add_io(zr_bus *b, uint16_t mask, uint16_t value, zr_bus_read_fn read, zr_bus_write_fn write, void *ctx);
void zr_bus_enable_io(zr_bus *b, int decoder, int on);
/* A port cycle (traced when the tap is set). */
uint8_t zr_bus_in(zr_bus *b, uint16_t port);
void zr_bus_out(zr_bus *b, uint16_t port, uint8_t value);

/* --- the trace ----------------------------------------------------------------------------- */

void zr_bus_set_trace(zr_bus *b, zr_bus_trace_fn fn, void *ctx);
static inline void zr_bus_trace(zr_bus *b, int kind, uint32_t address, uint32_t value) {
    if (b->trace) b->trace(b->trace_ctx, kind, address, value);
}

#endif /* ZR_HW_BUS_H */
