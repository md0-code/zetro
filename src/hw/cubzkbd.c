/* ZetRo - the CUB Z's keyboard (cubzkbd.h). The protocol from the manual's sections 2.3.3 and 3.4 and from cubz_keyboard.sv;
 * the codes from the keyboard's EPROM (keyboard.bin: eight pages of 256 codes, address bit 10 low with SHIFT, bit 9 low with
 * CTRL, bit 8 high with CAPS LOCK; the code of each position below is read from pages 6, 7, 2 and 0). ZetRo's own code. */
#include "cubzkbd.h"

#include "state.h"

#include <string.h>

#define STATE_VERSION 1u

/* Position, code alone, with CAPS LOCK, with SHIFT, with CTRL. */
static const uint8_t CODES[][5] = {
    {0x00, 0x31, 0x31, 0x21, 0x31}, {0x01, 0x32, 0x32, 0x40, 0x32}, {0x02, 0x33, 0x33, 0x23, 0x33}, {0x03, 0x34, 0x34, 0x24, 0x34}, {0x04, 0x35, 0x35, 0x25, 0x35},
    {0x05, 0x36, 0x36, 0x5E, 0x36}, {0x06, 0x37, 0x37, 0x26, 0x37}, {0x07, 0x38, 0x38, 0x2A, 0x38}, {0x08, 0x39, 0x39, 0x28, 0x39}, {0x09, 0x30, 0x30, 0x29, 0x30},
    {0x0A, 0x2D, 0x2D, 0x5F, 0x1F}, {0x0B, 0x3D, 0x3D, 0x2B, 0x3D}, {0x0C, 0x60, 0x60, 0x7E, 0x1E}, {0x10, 0x71, 0x51, 0x51, 0x11}, {0x11, 0x77, 0x57, 0x57, 0x17},
    {0x12, 0x65, 0x45, 0x45, 0x05}, {0x13, 0x72, 0x52, 0x52, 0x12}, {0x14, 0x74, 0x54, 0x54, 0x14}, {0x15, 0x79, 0x59, 0x59, 0x19}, {0x16, 0x75, 0x55, 0x55, 0x15},
    {0x17, 0x69, 0x49, 0x49, 0x09}, {0x18, 0x6F, 0x4F, 0x4F, 0x0F}, {0x19, 0x70, 0x50, 0x50, 0x10}, {0x1A, 0x5B, 0x5B, 0x7B, 0x1B}, {0x1B, 0x5D, 0x5D, 0x7D, 0x1D},
    {0x20, 0x61, 0x41, 0x41, 0x01}, {0x21, 0x73, 0x53, 0x53, 0x13}, {0x22, 0x64, 0x44, 0x44, 0x04}, {0x23, 0x66, 0x46, 0x46, 0x06}, {0x24, 0x67, 0x47, 0x47, 0x07},
    {0x25, 0x68, 0x48, 0x48, 0x08}, {0x26, 0x6A, 0x4A, 0x4A, 0x0A}, {0x27, 0x6B, 0x4B, 0x4B, 0x0B}, {0x28, 0x6C, 0x4C, 0x4C, 0x0C}, {0x29, 0x3B, 0x3B, 0x3A, 0x3B},
    {0x2A, 0x27, 0x27, 0x22, 0x27}, {0x30, 0x7A, 0x5A, 0x5A, 0x1A}, {0x31, 0x78, 0x58, 0x58, 0x18}, {0x32, 0x63, 0x43, 0x43, 0x03}, {0x33, 0x76, 0x56, 0x56, 0x16},
    {0x34, 0x62, 0x42, 0x42, 0x02}, {0x35, 0x6E, 0x4E, 0x4E, 0x0E}, {0x36, 0x6D, 0x4D, 0x4D, 0x0D}, {0x37, 0x2C, 0x2C, 0x3C, 0x2C}, {0x38, 0x2E, 0x2E, 0x3E, 0x2E},
    {0x39, 0x2F, 0x2F, 0x3F, 0x2F}, {0x3C, 0x7F, 0x7F, 0x7F, 0x7F}, {0x3D, 0x5C, 0x5C, 0x7C, 0x1C}, {0x49, 0xE7, 0xC7, 0xC7, 0x87}, {0x4A, 0xE4, 0xC4, 0xC4, 0x84},
    {0x4B, 0xF0, 0xD0, 0xD0, 0x90}, {0x4C, 0xF1, 0xD1, 0xD1, 0x91}, {0x4D, 0xF2, 0xD2, 0xD2, 0x92}, {0x4E, 0xE5, 0xC5, 0xC5, 0x85}, {0x4F, 0xE6, 0xC6, 0xC6, 0x86},
    {0x59, 0xEA, 0xCA, 0xCA, 0x8A}, {0x5A, 0x2D, 0x2D, 0x2D, 0x2D}, {0x5B, 0xFA, 0xDA, 0xDA, 0x9A}, {0x5C, 0xFB, 0xDB, 0xDB, 0x9B}, {0x5D, 0xFC, 0xDC, 0xDC, 0x9C},
    {0x5E, 0xE8, 0xC8, 0xC8, 0x88}, {0x5F, 0xE9, 0xC9, 0xC9, 0x89}, {0x6A, 0x2C, 0x2C, 0x2C, 0x2C}, {0x6B, 0xF7, 0xD7, 0xD7, 0x97}, {0x6C, 0xF8, 0xD8, 0xD8, 0x98},
    {0x6D, 0xF9, 0xD9, 0xD9, 0x99}, {0x6F, 0xEB, 0xCB, 0xCB, 0x8B}, {0x79, 0xEE, 0xCE, 0xCE, 0x8E}, {0x7A, 0xFE, 0xDE, 0xDE, 0x9E}, {0x7B, 0xF4, 0xD4, 0xD4, 0x94},
    {0x7C, 0xF5, 0xD5, 0xD5, 0x95}, {0x7D, 0xF6, 0xD6, 0xD6, 0x96}, {0x7E, 0xEC, 0xCC, 0xCC, 0x8C}, {0x7F, 0xED, 0xCD, 0xCD, 0x8D}, {0x90, 0x09, 0x09, 0x0B, 0x09},
    {0x91, 0x0A, 0x0A, 0x0A, 0x0A}, {0x92, 0x1B, 0x1B, 0x1B, 0x1B}, {0x93, 0x20, 0x20, 0x20, 0x00}, {0x94, 0x80, 0x80, 0x80, 0x80}, {0x9A, 0xFD, 0xDD, 0xDD, 0x9D},
    {0x9B, 0xF3, 0xD3, 0xD3, 0x93}, {0x9C, 0x0D, 0x0D, 0x0D, 0x0D}, {0x9D, 0x08, 0x08, 0x08, 0x08}, {0x9F, 0xEF, 0xCF, 0xCF, 0x8F}, {0xA9, 0xE0, 0xC0, 0xC0, 0x80},
    {0xAB, 0xE1, 0xC1, 0xC1, 0x81}, {0xAC, 0xE2, 0xC2, 0xC2, 0x82}, {0xAD, 0xE3, 0xC3, 0xC3, 0x83}, {0xD0, 0xA0, 0xA0, 0xB0, 0xA0}, {0xD1, 0xA1, 0xA1, 0xB1, 0xA1},
    {0xD2, 0xA2, 0xA2, 0xB2, 0xA2}, {0xD3, 0xA3, 0xA3, 0xB3, 0xA3}, {0xD4, 0xA4, 0xA4, 0xB4, 0xA4}, {0xD5, 0xA5, 0xA5, 0xB5, 0xA5}, {0xD6, 0xA6, 0xA6, 0xB6, 0xA6},
    {0xD7, 0xA7, 0xA7, 0xB7, 0xA7}, {0xD8, 0xA8, 0xA8, 0xB8, 0xA8}, {0xD9, 0xA9, 0xA9, 0xB9, 0xA9}, {0xDA, 0xAA, 0xAA, 0xBA, 0xAA}, {0xDB, 0xAB, 0xAB, 0xBB, 0xAB},
    {0xDC, 0xAC, 0xAC, 0xBC, 0xAC}, {0xDD, 0xAD, 0xAD, 0xBD, 0xAD}, {0xDE, 0xAE, 0xAE, 0xBE, 0xAE}, {0xDF, 0xAF, 0xAF, 0xBF, 0xAF},
};

static uint64_t ticks(const zr_cubzkbd *k, uint32_t num, uint32_t den) { return (uint64_t)k->clock_hz * num / den; }

int zr_cubzkbd_code(const zr_cubzkbd *k, unsigned position) {
    for (size_t i = 0; i < sizeof CODES / sizeof CODES[0]; ++i) {
        if (CODES[i][0] != position) continue;
        if (k->ctrl) return CODES[i][4];
        if (k->shift_l || k->shift_r) return CODES[i][3];
        return CODES[i][k->caps ? 2 : 1];
    }
    return -1;
}

int zr_cubzkbd_data(const zr_cubzkbd *k) { return k->interrogate ? 0 : k->response; }
int zr_cubzkbd_clock(const zr_cubzkbd *k) { return k->busy_until <= zr_dev_now(&k->dev); }

/* Reports the outputs that changed. The level is recorded before the call, so the board may call back. */
static void report(zr_cubzkbd *k) {
    const uint8_t data = (uint8_t)zr_cubzkbd_data(k);
    if (data != k->data_reported) {
        k->data_reported = data;
        zr_dev_line(&k->dev, ZR_CUBZKBD_OUT_DATA, data);
    }
    const uint8_t clock = (uint8_t)zr_cubzkbd_clock(k);
    if (clock != k->clock_reported) {
        k->clock_reported = clock;
        zr_dev_line(&k->dev, ZR_CUBZKBD_OUT_CLOCK, clock);
    }
}

static void arm(zr_cubzkbd *k) {
    const uint64_t now = zr_dev_now(&k->dev);
    uint64_t when = k->repeat_at;
    if (k->busy_until > now && k->busy_until < when) when = k->busy_until;
    zr_dev_schedule(&k->dev, when);
}

/* A code into the buffer; KBCLK goes low for 100 us. */
static void put(zr_cubzkbd *k, uint8_t code, uint64_t now) {
    if (k->count == ZR_CUBZKBD_BUFFER) {
        ++k->overflow;
        return;
    }
    k->fifo[(k->head + k->count) % ZR_CUBZKBD_BUFFER] = code;
    ++k->count;
    k->busy_until = now + ticks(k, 1, 10000);
}

void zr_cubzkbd_key(zr_cubzkbd *k, unsigned key, int down) {
    const uint64_t now = zr_dev_now(&k->dev);
    switch (key) {
    case ZR_CUBZKBD_SHIFT_LEFT: k->shift_l = down != 0; break;
    case ZR_CUBZKBD_SHIFT_RIGHT: k->shift_r = down != 0; break;
    case ZR_CUBZKBD_CTRL: k->ctrl = down != 0; break;
    case ZR_CUBZKBD_CAPS: if (down) k->caps ^= 1u; break;
    case ZR_CUBZKBD_REPEAT:
        k->rep = down != 0;
        if (k->held >= 0) k->repeat_at = now + (down ? ticks(k, 1, 10) : ticks(k, 4, 10));
        break;
    default: {
        if (key > 255u) return;
        if (!down) {
            if (k->held == (int16_t)key) {
                k->held = -1;
                k->repeat_at = ZR_NEVER;
            }
            break;
        }
        const int code = zr_cubzkbd_code(k, key);
        if (code < 0) return;
        put(k, (uint8_t)code, now);
        k->held = (int16_t)key;
        k->held_code = (uint8_t)code;
        k->repeat_at = now + (k->rep ? ticks(k, 1, 10) : ticks(k, 4, 10));
        break;
    }
    }
    report(k);
    arm(k);
}

static void kbd_event(void *dev, uint64_t now) {
    zr_cubzkbd *k = dev;
    while (k->repeat_at <= now) {
        put(k, k->held_code, k->repeat_at);
        k->repeat_at += ticks(k, 1, 10);
    }
    report(k);
    arm(k);
}

/* The end of an interrogation pulse: the keyboard answers with the start bit or the next bit of the code. */
static void kbd_input(void *dev, int pin, int state) {
    zr_cubzkbd *k = dev;
    if (pin != ZR_CUBZKBD_IN_INTERROGATE) return;
    const uint8_t level = state != 0;
    const uint8_t was = k->interrogate;
    k->interrogate = level;
    if (was && !level) {
        if (k->bitpos == 0) {
            if (k->count && zr_cubzkbd_clock(k)) {
                k->shifter = k->fifo[k->head];
                k->bitpos = 1;
                k->response = 0;
            } else k->response = 1;
        } else {
            k->response = k->shifter & 1u;
            k->shifter = (uint8_t)(k->shifter >> 1 | 0x80u);
            if (k->bitpos == 8) {
                k->bitpos = 0;
                k->head = (uint8_t)((k->head + 1u) % ZR_CUBZKBD_BUFFER);
                --k->count;
            } else ++k->bitpos;
        }
    }
    report(k);
}

static uint8_t kbd_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void kbd_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }

static void kbd_reset(void *dev) {
    zr_cubzkbd *k = dev;
    k->shift_l = k->shift_r = k->ctrl = k->caps = k->rep = 0;
    k->held = -1;
    k->repeat_at = ZR_NEVER;
    k->head = k->count = 0;
    k->busy_until = 0;
    k->bitpos = 0;
    k->shifter = 0;
    k->response = 1;
    report(k);
    arm(k);
}

static void kbd_save(const void *dev, zr_state_writer *w) {
    const zr_cubzkbd *k = dev;
    zr_sw_u8(w, STATE_VERSION);
    const uint8_t b[] = {k->shift_l, k->shift_r, k->ctrl, k->caps, k->rep, k->held_code, k->head, k->count, k->interrogate,
                         k->bitpos, k->shifter, k->response, k->data_reported, k->clock_reported};
    zr_sw_bytes(w, b, sizeof b);
    zr_sw_u16(w, (uint16_t)k->held);
    zr_sw_u64(w, k->repeat_at);
    zr_sw_u64(w, k->busy_until);
    zr_sw_u32(w, k->overflow);
    zr_sw_bytes(w, k->fifo, sizeof k->fifo);
}

static int kbd_load(void *dev, zr_state_reader *r) {
    zr_cubzkbd *k = dev;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    uint8_t b[14];
    zr_sr_bytes(r, b, sizeof b);
    const int16_t held = (int16_t)zr_sr_u16(r);
    const uint64_t repeat_at = zr_sr_u64(r), busy_until = zr_sr_u64(r);
    const uint32_t overflow = zr_sr_u32(r);
    uint8_t fifo[ZR_CUBZKBD_BUFFER];
    zr_sr_bytes(r, fifo, sizeof fifo);
    if (!zr_sr_ok(r) || b[6] >= ZR_CUBZKBD_BUFFER || b[7] > ZR_CUBZKBD_BUFFER || b[9] > 8u || held < -1 || held > 255) return 0;
    k->shift_l = b[0] & 1u; k->shift_r = b[1] & 1u; k->ctrl = b[2] & 1u; k->caps = b[3] & 1u; k->rep = b[4] & 1u;
    k->held_code = b[5]; k->head = b[6]; k->count = b[7]; k->interrogate = b[8] & 1u; k->bitpos = b[9]; k->shifter = b[10];
    k->response = b[11] & 1u; k->data_reported = b[12] & 1u; k->clock_reported = b[13] & 1u;
    k->held = held;
    k->repeat_at = repeat_at;
    k->busy_until = busy_until;
    k->overflow = overflow;
    memcpy(k->fifo, fifo, sizeof fifo);
    arm(k);
    return 1;
}

static const zr_dev_ops KBD_OPS = {
    .read = kbd_read, .write = kbd_write, .event = kbd_event, .reset = kbd_reset, .input = kbd_input,
    .save = kbd_save, .load = kbd_load,
};

void zr_cubzkbd_init(zr_cubzkbd *k, const zr_hw_host *host, const char *name, uint32_t clock_hz) {
    memset(k, 0, sizeof *k);
    k->dev.ops = &KBD_OPS;
    k->dev.host = host;
    k->dev.name = name ? name : "cubzkbd";
    k->clock_hz = clock_hz;
    k->held = -1;
    k->repeat_at = ZR_NEVER;
    k->response = 1;
    k->data_reported = 1;
    k->clock_reported = 1;
}
