/* The HC's Interface 1 (if1.h). ZetRo's own code, written from this project's
 * HC core (interfaces.md; hc_family_core.sv: the paging, the RAM mirrors,
 * the control latch, ports EFh and F7h, the motor delay, the network WAIT)
 * and the HC-90/91 Interface 1 schematics (the controller's clock, the
 * connectors). */
#include "if1.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

#define STATE_VERSION 1u
#define AUDIO_RATE 48000u
#define TX_START 9u                    /* tx_count while the start bit waits for its middle */

/* The motor delay: 95,200,000 periods of the core's 56 MHz clock. */
static uint64_t motor_ticks(const zr_if1 *f) { return (uint64_t)f->clock_hz * 17u / 10u; }
/* A serial bit in master-clock periods. */
static uint64_t bit_ticks(const zr_if1 *f) { return (uint64_t)f->clock_hz / (f->baud ? f->baud : 9600u); }

static uint64_t now_of(const zr_if1 *f) { return zr_dev_now(&f->dev); }
/* The motor runs while the latch's bit 3 is set and for the delay after it is cleared. */
static int motor_on(const zr_if1 *f, uint64_t now) { return (f->control & 8u) || now < f->motor_until; }

/* --- the lines ----------------------------------------------------------------------------------- */

/* Serial mode is EFh bit 0; the data bit is F7h bit 0. */
static int serial_mode(const zr_if1 *f) { return f->serial & 1u; }
static int net_driven(const zr_if1 *f) { return !serial_mode(f) && !(f->network & 1u); }
static int net_active(const zr_if1 *f) { return f->net_in || net_driven(f); }
/* The TX line: the data bit inverted in serial mode, the mark (1) otherwise. */
static int tx_line(const zr_if1 *f) { return serial_mode(f) ? !(f->network & 1u) : 1; }

/* The deadline: the motor delay's end, the next serial sample or bit. */
static void rearm(zr_if1 *f) {
    const uint64_t now = now_of(f);
    uint64_t next = ZR_NEVER;
    if (!(f->control & 8u) && f->motor_until > now) next = f->motor_until;
    if (f->tx_busy && f->tx_at < next) next = f->tx_at;
    if (f->rx_busy && f->rx_at < next) next = f->rx_at;
    zr_dev_schedule(&f->dev, next);
}

/* The drives as the latch and the motor delay leave them: a drive turns while it is selected and the motor runs. */
static void drives_follow(zr_if1 *f, uint64_t now) {
    const int on = motor_on(f, now);
    for (unsigned i = 0; i < 2u; ++i) {
        const int selected = f->control >> (1u + i) & 1u;
        zr_fdd_set_motor(&f->fdd[i], on && selected, now);
    }
    zr_upd765_drive_changed(&f->fdc);
}

static void report_cts(zr_if1 *f) {
    const uint8_t cts = (uint8_t)zr_if1_serial_cts(f);
    if (cts != f->cts_reported) {
        f->cts_reported = cts;
        if (f->board.serial_ready) f->board.serial_ready(f->board.ctx, cts);
    }
}

/* The next byte from the host onto the RX line: its start bit begins now. */
static void rx_start(zr_if1 *f, uint64_t now) {
    if (f->rx_busy || !f->queue_count || !zr_if1_serial_cts(f)) return;
    f->rx_byte = f->queue[f->queue_head];
    f->queue_head = (f->queue_head + 1u) % ZR_IF1_QUEUE;
    --f->queue_count;
    f->rx_busy = 1;
    f->rx_bit = 0;
    f->rx_line = 0;
    f->rx_at = now + bit_ticks(f);
}

/* A falling edge of the TX line while idle: a start bit, as a UART takes it: it counts only if the line is still low half a
 * bit later (the firmware changes the mode and the data bit in two writes, and the line dips for a few clocks), and then the
 * bits are sampled in their middles. */
static void tx_changed(zr_if1 *f, int before, uint64_t now) {
    if (!f->tx_busy && before && !tx_line(f)) {
        f->tx_busy = 1;
        f->tx_count = TX_START;
        f->tx_byte = 0;
        f->tx_at = now + bit_ticks(f) / 2u;
    }
}

/* A byte that came out of the TX line: to the host, or round to the RX line on a loopback plug. */
static void tx_byte_out(zr_if1 *f, uint8_t byte) {
    if (f->loopback) zr_if1_receive(f, &byte, 1);
    else if (f->board.serial_out) f->board.serial_out(f->board.ctx, byte);
}

static void if1_event(void *dev, uint64_t now) {
    zr_if1 *f = dev;
    if (!motor_on(f, now) && (f->fdd[0].motor || f->fdd[1].motor)) drives_follow(f, now);
    if (f->tx_busy && now >= f->tx_at) {
        const int bit = tx_line(f);
        if (f->tx_count == TX_START) {
            if (bit) f->tx_busy = 0;                    /* a glitch, not a start bit */
            else {
                f->tx_count = 0;
                f->tx_at += bit_ticks(f);
            }
        } else if (f->tx_count < 8u) {
            f->tx_byte = (uint8_t)(f->tx_byte | (unsigned)bit << f->tx_count);
            ++f->tx_count;
            f->tx_at += bit_ticks(f);
        } else {
            /* The stop bit: a byte framed right goes out. A line already low again starts the next byte now. */
            f->tx_busy = 0;
            if (bit) tx_byte_out(f, f->tx_byte);
            if (!tx_line(f)) tx_changed(f, 1, now);
        }
    }
    if (f->rx_busy && now >= f->rx_at) {
        ++f->rx_bit;
        if (f->rx_bit <= 8u) f->rx_line = (uint8_t)(f->rx_byte >> (f->rx_bit - 1u) & 1u);
        else f->rx_line = 1;
        if (f->rx_bit < 10u) f->rx_at += bit_ticks(f);
        else {
            f->rx_busy = 0;
            rx_start(f, now);
        }
    }
    rearm(f);
}

static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void if1_reset_op(void *dev) { zr_if1_reset(dev); }
static const zr_dev_ops IF1_OPS = {.read = no_read, .write = no_write, .event = if1_event, .reset = if1_reset_op};

/* --- life cycle ------------------------------------------------------------------------------------ */

void zr_if1_init(zr_if1 *f, const zr_hw_host *host, const zr_if1_board *board, unsigned kind, const uint8_t *eprom,
                 uint32_t eprom_size, uint32_t clock_hz, uint32_t baud, int drive_sound) {
    memset(f, 0, sizeof *f);
    f->dev.ops = &IF1_OPS;
    f->dev.host = host;
    f->dev.name = "if1";
    if (board) f->board = *board;
    f->kind = (uint8_t)(kind == ZR_IF1_HC85 ? ZR_IF1_HC85 : ZR_IF1_HC90);
    f->eprom = eprom;
    f->eprom_size = eprom_size;
    f->clock_hz = clock_hz;
    f->baud = baud ? baud : 9600u;
    f->entry = 1;
    zr_upd765_init(&f->fdc, host, "8272", clock_hz);
    zr_drivesnd_init(&f->sound, clock_hz, AUDIO_RATE);
    zr_drivesnd_set(&f->sound, drive_sound, 50);
    for (unsigned i = 0; i < 2u; ++i) {
        zr_fdd_init(&f->fdd[i], clock_hz, 1, 40, 300);
        if (drive_sound) zr_fdd_set_hook(&f->fdd[i], zr_drivesnd_event, &f->sound, i);
        zr_upd765_attach(&f->fdc, i, &f->fdd[i]);
    }
    zr_upd765_ready_wiring(&f->fdc, ZR_UPD765_READY_TIED);     /* RDY to +5 V on both boards' sheets */
    f->fdc.dev.ops->input(&f->fdc, ZR_UPD765_PIN_RESET, 1);
    f->ready_in = 1;
    f->rx_line = 1;
    f->cts_reported = 0xFFu;
    zr_if1_reset(f);
}

void zr_if1_reset(zr_if1 *f) {
    const uint64_t now = now_of(f);
    const uint8_t was = f->phantom;
    f->phantom = 0;
    f->control = 0;
    f->motor_until = 0;
    f->serial = 0xFEu;
    f->network = 0xFFu;
    f->net_wait = 0;
    f->tx_busy = f->rx_busy = 0;
    f->rx_line = 1;
    f->queue_head = f->queue_count = 0;
    f->fdc.dev.ops->input(&f->fdc, ZR_UPD765_PIN_TC, 0);
    f->fdc.dev.ops->input(&f->fdc, ZR_UPD765_PIN_RESET, 1);
    drives_follow(f, now);
    report_cts(f);
    rearm(f);
    if (was && f->board.paged) f->board.paged(f->board.ctx);
}

/* --- fetches ------------------------------------------------------------------------------------------- */

static int entry_address(uint16_t pc) { return (pc & 0xFFF8u) == 0x0008u || (pc & 0xFFF8u) == 0x1708u; }
static int exit_address(uint16_t pc) { return (pc & 0xFFF8u) == 0x0700u; }

void zr_if1_watch(uint8_t watch[8192]) {
    static const uint16_t FIRST[3] = {0x0008u, 0x1708u, 0x0700u};
    for (unsigned i = 0; i < 3u; ++i)
        for (uint16_t a = FIRST[i]; a < FIRST[i] + 8u; ++a) watch[a >> 3] |= (uint8_t)(1u << (a & 7u));
}

void zr_if1_before_fetch(zr_if1 *f, uint16_t pc) {
    if (!f->phantom && f->entry && entry_address(pc)) {
        f->phantom = 1;
        if (f->board.paged) f->board.paged(f->board.ctx);
    }
}

void zr_if1_after_fetch(zr_if1 *f, uint16_t pc) {
    if (f->phantom && exit_address(pc)) {
        f->phantom = 0;
        if (f->board.paged) f->board.paged(f->board.ctx);
    }
}

/* The RAM: on the HC-85's board where A13 and A11 are high and A12 low, on the HC-90/91's where A13 and A11 are high.
 * Page p holds A10-A13 in its bits 0-3. */
static int ram_at(const zr_if1 *f, unsigned page) {
    return (page & 0x0Au) == 0x0Au && (f->kind != ZR_IF1_HC85 || !(page & 4u));
}

const uint8_t *zr_if1_rom_page(const zr_if1 *f, unsigned page) {
    if (page >= 16u || ram_at(f, page)) return NULL;
    const uint32_t at = (uint32_t)page << 10;
    return at + 1024u <= f->eprom_size ? f->eprom + at : NULL;
}

uint8_t *zr_if1_ram_page(zr_if1 *f, unsigned page) { return page < 16u && ram_at(f, page) ? f->ram : NULL; }

/* --- ports ---------------------------------------------------------------------------------------------- */

int zr_if1_decodes(uint16_t port) {
    const unsigned low = port & 0xFFu;
    return (low & 0xFDu) == 0x05u || (low & 0xFDu) == 0x85u || low == 0xEFu || low == 0xF7u;
}

uint8_t zr_if1_in(zr_if1 *f, uint16_t port) {
    const unsigned low = port & 0xFFu;
    if ((low & 0xFDu) == 0x05u) return (uint8_t)(0xFEu | (motor_on(f, now_of(f)) ? 0u : 1u));
    if ((low & 0xFDu) == 0x85u) return f->fdc.dev.ops->read(&f->fdc, ZR_SPACE_IO, low >> 1 & 1u);
    if (low == 0xEFu) return (uint8_t)(0xF6u | (f->ready_in ? 8u : 0u) | (net_active(f) ? 1u : 0u));
    if (low == 0xF7u) return (uint8_t)(0x7Fu | (f->rx_line ? 0u : 0x80u));
    return 0xFFu;
}

void zr_if1_out(zr_if1 *f, uint16_t port, uint8_t value) {
    const unsigned low = port & 0xFFu;
    const uint64_t now = now_of(f);
    if ((low & 0xFDu) == 0x05u) {
        /* The 555's capacitor is held discharged while bit 3 is set; it times out the delay from the bit's clearing. */
        if ((f->control & 8u) && !(value & 8u)) f->motor_until = now + motor_ticks(f);
        f->control = value;
        f->fdc.dev.ops->input(&f->fdc, ZR_UPD765_PIN_TC, value & 1u);
        f->fdc.dev.ops->input(&f->fdc, ZR_UPD765_PIN_RESET, !(value & 0x10u));
        drives_follow(f, now);
    } else if ((low & 0xFDu) == 0x85u) {
        f->fdc.dev.ops->write(&f->fdc, ZR_SPACE_IO, low >> 1 & 1u, value);
    } else if (low == 0xEFu || low == 0xF7u) {
        const int before = tx_line(f);
        if (low == 0xEFu) {
            if (!(value & 0x20u) && net_active(f)) f->net_wait = 1;
            f->serial = value;
            report_cts(f);
            rx_start(f, now);
        } else f->network = value;
        tx_changed(f, before, now);
    }
    rearm(f);
}

int zr_if1_held(zr_if1 *f) {
    if (!net_active(f)) f->net_wait = 0;
    return f->net_wait;
}

/* --- serial and network ------------------------------------------------------------------------------------ */

size_t zr_if1_receive(zr_if1 *f, const uint8_t *data, size_t count) {
    size_t n = 0;
    for (; n < count && f->queue_count < ZR_IF1_QUEUE; ++n) {
        f->queue[(f->queue_head + f->queue_count) % ZR_IF1_QUEUE] = data[n];
        ++f->queue_count;
    }
    rx_start(f, now_of(f));
    rearm(f);
    return n;
}

void zr_if1_serial_ready_in(zr_if1 *f, int ready) { f->ready_in = f->loopback || ready != 0; }
void zr_if1_set_loopback(zr_if1 *f, int on) {
    f->loopback = on != 0;
    if (f->loopback) f->ready_in = 1;
}
int zr_if1_serial_cts(const zr_if1 *f) { return (f->serial & 0x10u) != 0; }
void zr_if1_network_in(zr_if1 *f, int active) { f->net_in = active != 0; }
int zr_if1_network_out(const zr_if1 *f) { return net_driven(f); }

/* --- drives ------------------------------------------------------------------------------------------------- */

int zr_if1_insert(zr_if1 *f, unsigned drive, zr_disk *disk, char *error, size_t capacity) {
    if (drive >= 2u) {
        if (capacity) snprintf(error, capacity, "There is no such drive");
        return 0;
    }
    zr_fdd *d = &f->fdd[drive];
    d->tracks = (uint8_t)(disk && disk->cylinders > 42u ? 80u : 40u);
    if (!zr_fdd_insert(d, disk, error, capacity)) return 0;
    drives_follow(f, now_of(f));
    rearm(f);
    return 1;
}

zr_disk *zr_if1_eject(zr_if1 *f, unsigned drive) {
    if (drive >= 2u) return NULL;
    zr_disk *disk = zr_fdd_eject(&f->fdd[drive]);
    drives_follow(f, now_of(f));
    rearm(f);
    return disk;
}

unsigned zr_if1_activity(const zr_if1 *f) {
    unsigned bits = 0;
    for (unsigned i = 0; i < 2u; ++i)
        if (f->fdd[i].motor && f->fdd[i].disk) bits |= 1u << i;
    return bits;
}

/* --- states ------------------------------------------------------------------------------------------------- */

void zr_if1_save(const zr_if1 *f, zr_state_writer *w) {
    zr_sw_begin(w, "IF1 ", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_bytes(w, f->ram, sizeof f->ram);
    zr_sw_u8(w, f->phantom);
    zr_sw_u8(w, f->entry);
    zr_sw_u8(w, f->control);
    zr_sw_u64(w, f->motor_until);
    zr_sw_u8(w, f->serial);
    zr_sw_u8(w, f->network);
    zr_sw_u8(w, f->net_wait);
    zr_sw_u8(w, f->net_in);
    zr_sw_u8(w, f->ready_in);
    zr_sw_u8(w, f->tx_busy);
    zr_sw_u8(w, f->tx_count);
    zr_sw_u8(w, f->tx_byte);
    zr_sw_u64(w, f->tx_at);
    zr_sw_u8(w, f->rx_line);
    zr_sw_u8(w, f->rx_busy);
    zr_sw_u8(w, f->rx_bit);
    zr_sw_u8(w, f->rx_byte);
    zr_sw_u64(w, f->rx_at);
    zr_sw_u32(w, f->queue_count);
    for (unsigned i = 0; i < f->queue_count; ++i) zr_sw_u8(w, f->queue[(f->queue_head + i) % ZR_IF1_QUEUE]);
    zr_sw_end(w);
    zr_sw_begin(w, "8272", 0);
    f->fdc.dev.ops->save(&f->fdc, w);
    zr_sw_end(w);
    for (unsigned i = 0; i < 2u; ++i) {
        zr_sw_begin(w, "FDD ", i);
        zr_fdd_save(&f->fdd[i], w);
        zr_sw_end(w);
    }
}

int zr_if1_load(zr_if1 *f, zr_state_reader *r) {
    if (!zr_sr_chunk(r, "IF1 ", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_sr_bytes(r, f->ram, sizeof f->ram);
    f->phantom = zr_sr_u8(r) & 1u;
    f->entry = zr_sr_u8(r) & 1u;
    f->control = zr_sr_u8(r);
    f->motor_until = zr_sr_u64(r);
    f->serial = zr_sr_u8(r);
    f->network = zr_sr_u8(r);
    f->net_wait = zr_sr_u8(r) & 1u;
    f->net_in = zr_sr_u8(r) & 1u;
    f->ready_in = zr_sr_u8(r) & 1u;
    f->tx_busy = zr_sr_u8(r) & 1u;
    f->tx_count = zr_sr_u8(r);
    f->tx_byte = zr_sr_u8(r);
    f->tx_at = zr_sr_u64(r);
    f->rx_line = zr_sr_u8(r) & 1u;
    f->rx_busy = zr_sr_u8(r) & 1u;
    f->rx_bit = zr_sr_u8(r);
    f->rx_byte = zr_sr_u8(r);
    f->rx_at = zr_sr_u64(r);
    const uint32_t count = zr_sr_u32(r);
    if (count > ZR_IF1_QUEUE || f->tx_count > TX_START || f->rx_bit > 10u) return 0;
    f->queue_head = 0;
    f->queue_count = count;
    for (unsigned i = 0; i < count; ++i) f->queue[i] = zr_sr_u8(r);
    if (!zr_sr_ok(r)) return 0;
    for (unsigned i = 0; i < 2u; ++i)
        if (!zr_sr_chunk(r, "FDD ", i) || !zr_fdd_load(&f->fdd[i], r)) return 0;
    if (!zr_sr_chunk(r, "8272", 0) || !f->fdc.dev.ops->load(&f->fdc, r)) return 0;
    f->cts_reported = (uint8_t)zr_if1_serial_cts(f);
    rearm(f);
    return zr_sr_ok(r);
}
