/* ZetRo - the Intel 8257 DMA controller (i8257.h). Written from Intel's
 * 8257 data sheet (the registers and their first/last flip-flop, the mode
 * set and status registers, the transfer types, the priority, the terminal
 * count, the auto load of channel 2); ZetRo's own code. */
#include "i8257.h"

#include "state.h"

#include <string.h>

/* The channel to serve now: enabled, DRQ up, the highest priority; -1 for none. */
static int pick(const zr_i8257 *d) {
    const unsigned ready = d->mode & d->drq & 0x0Fu;
    if (!ready) return -1;
    for (unsigned i = 0; i < 4; i++) {
        const unsigned ch = (d->mode & ZR_I8257_ROTATE) ? (d->last + 1u + i) & 3u : i;
        if (ready >> ch & 1u) return (int)ch;
    }
    return -1;
}

/* Reports HRQ when it changed. The level is recorded before the call, so the
 * board may call back into the chip. */
static void update_hrq(zr_i8257 *d) {
    const uint8_t hrq = pick(d) >= 0;
    if (hrq == d->hrq_reported) return;
    d->hrq_reported = hrq;
    zr_dev_line(&d->dev, ZR_I8257_OUT_HRQ, hrq);
}

static uint16_t *reg_of(zr_i8257 *d, uint32_t reg) {
    return (reg & 1u) ? &d->count[reg >> 1] : &d->addr[reg >> 1];
}

static uint8_t i_read(void *dev, unsigned space, uint32_t offset) {
    zr_i8257 *d = dev;
    (void)space;
    const uint32_t reg = offset & 15u;
    uint8_t v = 0xFFu;
    if (reg < 8u) {
        const uint16_t r = *reg_of(d, reg);
        v = d->ff ? (uint8_t)(r >> 8) : (uint8_t)r;
        d->ff ^= 1u;
    } else if (reg == 8u) {
        v = d->status;
        d->status = 0;
    }
    return v;
}

static void i_write(void *dev, unsigned space, uint32_t offset, uint8_t value) {
    zr_i8257 *d = dev;
    (void)space;
    const uint32_t reg = offset & 15u;
    if (reg < 8u) {
        uint16_t *r = reg_of(d, reg);
        *r = d->ff ? (uint16_t)((*r & 0x00FFu) | value << 8) : (uint16_t)((*r & 0xFF00u) | value);
        d->ff ^= 1u;
    } else if (reg == 8u) {
        d->mode = value;
        d->ff = 0;
    }
    update_hrq(d);
}

static void i_event(void *dev, uint64_t now) { (void)dev; (void)now; }     /* nothing is scheduled */

static void i_reset(void *dev) {
    zr_i8257 *d = dev;
    memset(d->addr, 0, sizeof d->addr);
    memset(d->count, 0, sizeof d->count);
    d->mode = d->status = d->ff = d->last = 0;
    update_hrq(d);
}

static void i_input(void *dev, int pin, int state) {
    zr_i8257 *d = dev;
    const uint8_t level = state != 0;
    if (pin >= ZR_I8257_IN_DRQ0 && pin <= ZR_I8257_IN_DRQ3) {
        d->drq = (uint8_t)((d->drq & ~(1u << pin)) | (unsigned)level << pin);
    } else if (pin == ZR_I8257_IN_HLDA) {
        d->hlda = level;
    } else {
        return;
    }
    update_hrq(d);
}

/* --- the transfers ------------------------------------------------------------------------------ */

unsigned zr_i8257_cycle(zr_i8257 *d) {
    const int ch = pick(d);
    if (ch < 0) return 0;
    const uint16_t cnt = d->count[ch], a = d->addr[ch];
    const unsigned type = cnt >> 14;
    const int tc = (cnt & 0x3FFFu) == 0;
    const int mark = (cnt & 0x7Fu) == 0x7Fu;
    if (mark) zr_dev_line(&d->dev, ZR_I8257_OUT_MARK, 1);
    if (tc) zr_dev_line(&d->dev, ZR_I8257_OUT_TC, 1);
    if (type == ZR_I8257_WRITE) {                   /* the device's byte goes to memory */
        uint8_t v = d->device ? d->device(d->device_ctx, ch, 0, 0, tc) : 0xFFu;
        zr_dev_dma(&d->dev, a, &v, 1, 1);
    } else if (type == ZR_I8257_READ) {             /* memory's byte goes to the device */
        uint8_t v = 0xFFu;
        if (!zr_dev_dma(&d->dev, a, &v, 1, 0)) v = 0xFFu;
        if (d->device) d->device(d->device_ctx, ch, 1, v, tc);
    }
    d->addr[ch] = (uint16_t)(a + 1u);
    d->count[ch] = (uint16_t)((cnt & 0xC000u) | ((cnt - 1u) & 0x3FFFu));
    d->last = (uint8_t)ch;
    if (tc) {
        d->status |= (uint8_t)(1u << ch);
        if (ch == 2 && (d->mode & ZR_I8257_AUTOLOAD)) {
            d->addr[2] = d->addr[3];
            d->count[2] = d->count[3];
            d->status |= ZR_I8257_ST_UPDATE;
        } else if (d->mode & ZR_I8257_TC_STOP) {
            d->mode &= (uint8_t)~(1u << ch);
        }
    }
    if (tc) zr_dev_line(&d->dev, ZR_I8257_OUT_TC, 0);
    if (mark) zr_dev_line(&d->dev, ZR_I8257_OUT_MARK, 0);
    update_hrq(d);
    return d->cycle_t;
}

/* --- state --------------------------------------------------------------------------------------- */

static void i_save(const void *dev, zr_state_writer *w) {
    const zr_i8257 *d = dev;
    for (unsigned i = 0; i < 4; i++) { zr_sw_u16(w, d->addr[i]); zr_sw_u16(w, d->count[i]); }
    const uint8_t b[] = {d->mode, d->status, d->ff, d->drq, d->hlda, d->last, d->hrq_reported, d->cycle_t};
    zr_sw_bytes(w, b, sizeof b);
}

static int i_load(void *dev, zr_state_reader *r) {
    zr_i8257 *d = dev;
    uint16_t addr[4], count[4];
    for (unsigned i = 0; i < 4; i++) { addr[i] = zr_sr_u16(r); count[i] = zr_sr_u16(r); }
    uint8_t b[8];
    zr_sr_bytes(r, b, sizeof b);
    if (!zr_sr_ok(r) || b[2] > 1 || b[3] > 0x0F || b[4] > 1 || b[5] > 3 || b[6] > 1 || b[7] < 1) return 0;
    memcpy(d->addr, addr, sizeof addr);
    memcpy(d->count, count, sizeof count);
    d->mode = b[0]; d->status = b[1]; d->ff = b[2]; d->drq = b[3]; d->hlda = b[4]; d->last = b[5];
    d->hrq_reported = b[6]; d->cycle_t = b[7];
    return 1;
}

static const zr_dev_ops I8257_OPS = {
    .read = i_read, .write = i_write, .event = i_event, .reset = i_reset, .input = i_input,
    .save = i_save, .load = i_load,
};

/* --- the board's calls ------------------------------------------------------------------------------ */

void zr_i8257_init(zr_i8257 *d, const zr_hw_host *host, const char *name, zr_i8257_device_fn device, void *device_ctx) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &I8257_OPS;
    d->dev.host = host;
    d->dev.name = name ? name : "i8257";
    d->device = device;
    d->device_ctx = device_ctx;
    d->cycle_t = 4;
}

void zr_i8257_set_cycle(zr_i8257 *d, unsigned tstates) {
    d->cycle_t = (uint8_t)(tstates < 1u ? 1u : tstates > 255u ? 255u : tstates);
}

int zr_i8257_hrq(const zr_i8257 *d) {
    return pick(d) >= 0;
}
