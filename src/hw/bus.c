/* ZetRo - a Z80 board's bus (bus.h). ZetRo's own code. */
#include "bus.h"

#include <string.h>

void zr_bus_init(zr_bus *b) { memset(b, 0, sizeof *b); }

static int aligned(uint16_t first, uint32_t size) {
    return !(first & (ZR_BUS_PAGE_SIZE - 1u)) && !(size & (ZR_BUS_PAGE_SIZE - 1u)) && (uint32_t)first + size <= 0x10000u;
}

/* The read side the cycles see: the overlay's where it is set and the board has no RAM there, else the board's own. */
static void apply_read(zr_bus *b, unsigned page) {
    const zr_bus_overlay *o = b->ov;
    const int ram = o->low_rd[page] && o->low_rd[page] == b->wr[page];
    if ((o->on[page] & 1u) && !ram) {
        b->rd[page] = o->rd[page];
        b->rh[page] = o->rh[page];
    } else {
        b->rd[page] = o->low_rd[page];
        b->rh[page] = o->low_rh[page];
    }
}

/* The fast write pointer follows the direct pointer, the watch and the overlay's tap. */
static void settle(zr_bus *b, unsigned page) {
    b->wr_fast[page] = b->watched[page] || (b->ov && (b->ov->on[page] & 2u)) ? NULL : b->wr[page];
    if (b->ov) apply_read(b, page);
}

/* The board's read side of a page: under the overlay when there is one. */
static void set_read(zr_bus *b, unsigned page, const uint8_t *data, const zr_bus_handler *h) {
    const uint8_t **rd = b->ov ? &b->ov->low_rd[page] : &b->rd[page];
    zr_bus_handler *rh = b->ov ? &b->ov->low_rh[page] : &b->rh[page];
    *rd = data;
    if (h) *rh = *h;
    else memset(rh, 0, sizeof *rh);
    if (b->ov) apply_read(b, page);
}

int zr_bus_map_read(zr_bus *b, uint16_t first, uint32_t size, const uint8_t *data) {
    if (!aligned(first, size)) return 0;
    for (uint32_t i = 0; i < size >> ZR_BUS_PAGE_BITS; ++i)
        set_read(b, (first >> ZR_BUS_PAGE_BITS) + i, data ? data + (i << ZR_BUS_PAGE_BITS) : NULL, NULL);
    return 1;
}

int zr_bus_map_write(zr_bus *b, uint16_t first, uint32_t size, uint8_t *data) {
    if (!aligned(first, size)) return 0;
    for (uint32_t i = 0; i < size >> ZR_BUS_PAGE_BITS; ++i) {
        const unsigned page = (first >> ZR_BUS_PAGE_BITS) + i;
        b->wr[page] = data ? data + (i << ZR_BUS_PAGE_BITS) : NULL;
        memset(&b->wh[page], 0, sizeof b->wh[page]);
        settle(b, page);
    }
    return 1;
}

int zr_bus_map_handler(zr_bus *b, uint16_t first, uint32_t size, const zr_bus_handler *h, unsigned sides) {
    if (!aligned(first, size)) return 0;
    for (uint32_t i = 0; i < size >> ZR_BUS_PAGE_BITS; ++i) {
        const unsigned page = (first >> ZR_BUS_PAGE_BITS) + i;
        if (sides & ZR_BUS_READ) set_read(b, page, NULL, h);
        if (sides & ZR_BUS_WRITE) {
            b->wr[page] = NULL;
            b->wh[page] = *h;
            settle(b, page);
        }
    }
    return 1;
}

void zr_bus_overlay_attach(zr_bus *b, zr_bus_overlay *storage) {
    memset(storage, 0, sizeof *storage);
    for (unsigned page = 0; page < ZR_BUS_PAGES; ++page) {
        storage->low_rd[page] = b->rd[page];
        storage->low_rh[page] = b->rh[page];
    }
    b->ov = storage;
}

int zr_bus_overlay_read(zr_bus *b, uint16_t first, uint32_t size, const uint8_t *data, const zr_bus_handler *h) {
    zr_bus_overlay *o = b->ov;
    if (!o || !aligned(first, size)) return 0;
    for (uint32_t i = 0; i < size >> ZR_BUS_PAGE_BITS; ++i) {
        const unsigned page = (first >> ZR_BUS_PAGE_BITS) + i;
        o->rd[page] = data ? data + (i << ZR_BUS_PAGE_BITS) : NULL;
        if (!data && h) o->rh[page] = *h;
        else memset(&o->rh[page], 0, sizeof o->rh[page]);
        o->on[page] = (uint8_t)((o->on[page] & ~1u) | (data || h ? 1u : 0u));
        apply_read(b, page);
    }
    return 1;
}

int zr_bus_overlay_watch(zr_bus *b, uint16_t first, uint32_t size, const zr_bus_handler *h) {
    zr_bus_overlay *o = b->ov;
    if (!o || !aligned(first, size)) return 0;
    for (uint32_t i = 0; i < size >> ZR_BUS_PAGE_BITS; ++i) {
        const unsigned page = (first >> ZR_BUS_PAGE_BITS) + i;
        if (h && h->write) o->wh[page] = *h;
        else memset(&o->wh[page], 0, sizeof o->wh[page]);
        o->on[page] = (uint8_t)((o->on[page] & ~2u) | (h && h->write ? 2u : 0u));
        settle(b, page);
    }
    return 1;
}

void zr_bus_set_hook(zr_bus *b, zr_bus_write_fn hook, void *ctx) {
    b->hook = hook;
    b->hook_ctx = ctx;
}

int zr_bus_watch(zr_bus *b, uint16_t first, uint32_t size, int on) {
    if (!aligned(first, size)) return 0;
    for (uint32_t i = 0; i < size >> ZR_BUS_PAGE_BITS; ++i) {
        const unsigned page = (first >> ZR_BUS_PAGE_BITS) + i;
        b->watched[page] = on ? 1u : 0u;
        settle(b, page);
    }
    return 1;
}

uint8_t zr_bus_read_slow(zr_bus *b, uint16_t address) {
    const zr_bus_handler *h = &b->rh[address >> ZR_BUS_PAGE_BITS];
    return h->read ? h->read(h->ctx, address) : 0xFFu;
}

void zr_bus_write_slow(zr_bus *b, uint16_t address, uint8_t value) {
    const unsigned page = address >> ZR_BUS_PAGE_BITS;
    if (b->ov && (b->ov->on[page] & 2u)) b->ov->wh[page].write(b->ov->wh[page].ctx, address, value);
    if (b->watched[page] && b->hook) b->hook(b->hook_ctx, address, value);
    if (b->wr[page]) b->wr[page][address & (ZR_BUS_PAGE_SIZE - 1u)] = value;
    else if (b->wh[page].write) b->wh[page].write(b->wh[page].ctx, address, value);
}

uint8_t zr_bus_peek(const zr_bus *b, uint16_t address) {
    const uint8_t *p = b->rd[address >> ZR_BUS_PAGE_BITS];
    return p ? p[address & (ZR_BUS_PAGE_SIZE - 1u)] : 0xFFu;
}

int zr_bus_add_io(zr_bus *b, uint16_t mask, uint16_t value, zr_bus_read_fn read, zr_bus_write_fn write, void *ctx) {
    if (b->decoders == ZR_BUS_DECODERS) return -1;
    zr_bus_decoder *d = &b->dec[b->decoders];
    d->mask = mask;
    d->value = (uint16_t)(value & mask);
    d->enabled = 1;
    d->read = read;
    d->write = write;
    d->ctx = ctx;
    return (int)b->decoders++;
}

void zr_bus_enable_io(zr_bus *b, int decoder, int on) {
    if (decoder >= 0 && (unsigned)decoder < b->decoders) b->dec[decoder].enabled = on ? 1u : 0u;
}

uint8_t zr_bus_in(zr_bus *b, uint16_t port) {
    uint8_t value = 0xFFu;
    for (unsigned i = 0; i < b->decoders; ++i) {
        const zr_bus_decoder *d = &b->dec[i];
        if (d->enabled && d->read && (port & d->mask) == d->value) value &= d->read(d->ctx, port);
    }
    zr_bus_trace(b, ZR_BUS_TRACE_IN, port, value);
    return value;
}

void zr_bus_out(zr_bus *b, uint16_t port, uint8_t value) {
    zr_bus_trace(b, ZR_BUS_TRACE_OUT, port, value);
    for (unsigned i = 0; i < b->decoders; ++i) {
        const zr_bus_decoder *d = &b->dec[i];
        if (d->enabled && d->write && (port & d->mask) == d->value) d->write(d->ctx, port, value);
    }
}

void zr_bus_set_trace(zr_bus *b, zr_bus_trace_fn fn, void *ctx) {
    b->trace = fn;
    b->trace_ctx = ctx;
}
