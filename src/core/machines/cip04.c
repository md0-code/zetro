/* The Electronica CIP-04 (machine.h, machine_impl.h): the family "cip04" (named "CIP" on the setup screen) with its
 * one model, a ZX Spectrum +3-compatible on the shared core (spectrum.h) as this project's CIP core documents it (docs/cip/
 * models.md, hardware.md, validation.md; core-cip/rtl/cip04_ports.sv) and as the +3's published hardware documentation gives the
 * rest. What is not known of the CIP-04 is not invented
 * (the 256 KiB the CIP-04 installs against the +3's 128 KiB, and port 5FFDh's bits beyond the zero the firmware writes at startup,
 * docs/NOTES.md).
 *   - the Z80 at 3.5 MHz from the 14 MHz master clock; the original 64 KiB ROM as four 16 KiB pages (0 the editor and menu, 1 the
 *     syntax checker, 2 the DOS, 3 the 48 BASIC); 128 KiB of RAM as eight 16 KiB pages; port FE on A0 low; no memory contention and
 *     no floating bus unless the not original Spectrum timing is on (the +3's: contended pages 4 to 7);
 *   - port 7FFDh (written; A15 low, A14 high, A1 low): bits 0-2 the RAM page at C000h, bit 3 the screen (page 5 or 7), bit 4 the ROM's
 *     low bit, bit 5 the lock; port 1FFDh (written; A15-A13 low, A12 high, A1 low): bit 0 the special all-RAM mode, bits 1 and 2 the
 *     mode's configuration (0: pages 0 1 2 3, 1: 4 5 6 7, 2: 4 5 6 3, 3: 4 7 6 3 at 0000h to C000h) or, outside it, bit 2 the ROM's
 *     high bit, bit 3 the disk motor, bit 4 the printer strobe; the lock holds the paging bits of both ports;
 *   - the uPD765A at 2FFDh (main status) and 3FFDh (data), polled (A15 and A14 low, A13 high, A12 the register, A1 low), with one
 *     drive (slot a: the 3-inch drive, 40 tracks, 300 rpm, 250 kbit/s) whose motor is 1FFDh's bit 3; the drive's sound;
 *   - the printer: data on 0FFDh (A15-A12 low, A1 low; bit 0 read is BUSY); the byte goes out when 1FFDh's bit 4, the strobe, rises:
 *     the firmware prints a character with one XOR of the bit (its copy of the latch is never set to 1 at startup), a rising edge in
 *     every case, a falling one only when the bit started high;
 *   - the AY-3-8912 at FFFDh and BFFDh, the machine's own, and its register 14 (the serial port): reading it gives the RXD line in bit
 *     7 and the peripheral's ready line in bit 6 above the written bits, whatever the chip's port direction; TXD is bit 3 and CTS
 *     (low: ready) bit 2; the firmware's own transmit routine (ROM 1) drives the line by bit 6 of its writes to port FE and tests bit 7 of
 *     its reads, which the board takes as the same TXD and as the peripheral's ready line;
 *   - port 5FFDh: a write latch the firmware clears at startup, nothing else known (the +3's decoding also routes the write to 7FFDh);
 *   - two joystick sockets, their kinds options as on the other CIPs; a tape recorder.
 * ZetRo's own code. */
#include "spectrum.h"
#include "fdd.h"
#include "upd765.h"
#include "drivesnd.h"
#include "media/disk.h"
#include "media/snap.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define PAGE 0x4000u
#define ROM_PAGES 4u
#define QUEUE 256u                          /* serial bytes from the host waiting to be played */

static const char *const ROMS[] = {"cip04", NULL};
/* The +3's menu starts on its first entry, Loader, and Enter chooses it (tape or disk): the auto-load text is that one key. */
static const zr_model_desc MODELS[] = {{"cip04", "CIP-04", ROMS, "\n"}};

static const char *const SWITCH[] = {"off", "on", NULL};
/* The sockets are wired to the matrix as on the other CIPs: the first to the keys 1-5, the second to 6-0. */
static const int STICK1_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR2);
static const int STICK2_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR1);
static const char *const AYS[] = {"mono", "abc", "acb", NULL};
static const int AY_LABELS[] = {ZR_T_SPEC_AY_MONO, ZR_T_SPEC_AY_ABC, ZR_T_SPEC_AY_ACB};
static const char *const BAUDS[] = {"110", "300", "600", "1200", "2400", "4800", "9600", "19200", NULL};
static const uint32_t BAUD_RATES[] = {110, 300, 600, 1200, 2400, 4800, 9600, 19200};

enum {
    OPTION_JOYSTICK, OPTION_JOYSTICK2, OPTION_DRIVE_SOUND, OPTION_BAUD, OPTION_AY,
    OPTION_TIMING,
    OPTION_INTERFACE, OPTION_COUNT = OPTION_INTERFACE + ZR_SPECTRUM_INTERFACE_OPTIONS
};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK1, ZR_SPECTRUM_JOY_KEMPSTON, STICK1_LABELS),
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick2", ZR_T_SPEC_JOYSTICK2, ZR_SPECTRUM_JOY_NONE, STICK2_LABELS),
    {"drive_sound", ZR_T_CIP04_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET},
    /* The bytes of the serial port to and from the host: ZetRo's UART on the firmware's bits (not original). */
    {"serial_baud", ZR_T_CIP04_BAUD, BAUDS, NULL, 6, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_PORT, ZR_T_CIP04_BAUD_HINT + 1},
    /* The AY is the machine's own (mono, as the +3's); the stereo modes are the core's. */
    {"ay", ZR_T_CIP04_AY, AYS, AY_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
    ZR_SPECTRUM_OPTION_TIMING,
    ZR_SPECTRUM_OPTIONS_INTERFACE(ZR_MODELS_ALL),
};

enum { SLOT_TAPE, SLOT_A, SLOT_INTERFACE, SLOT_COUNT = SLOT_INTERFACE + ZR_SPECTRUM_INTERFACE_SLOTS };
static const zr_media_desc MEDIA[SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    {"a", ZR_T_MEDIA_FLOPPY_A, ZR_MEDIA_FLOPPY, "dsk", ZR_MODELS_ALL},
    ZR_SPECTRUM_MEDIA_INTERFACE(ZR_MODELS_ALL),
};
enum { PORT_SERIAL, PORT_PRINTER };
static const zr_port_desc PORTS[] = {
    {"serial", ZR_T_CIP04_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL},
    {"printer", ZR_T_CIP04_PORT_PRINTER, ZR_PORT_PARALLEL, ZR_MODELS_ALL},
};

typedef struct cip04 {
    zr_spectrum s;                          /* first: the shared core */
    uint32_t baud;
    /* the board's latches (saved) */
    uint8_t paging;                         /* the last byte written to port 7FFDh */
    uint8_t paging2;                        /* the last byte written to port 1FFDh (bits 0-4) */
    uint8_t latch5;                         /* the last byte written to port 5FFDh */
    uint8_t printer_data;                   /* 0FFDh */
    uint8_t fe6;                            /* bit 6 of the last byte written to port FE */
    /* the peripherals' lines from the host (saved) */
    uint8_t printer_online, ready_in;
    /* the floppy: controller, drive, sound */
    zr_upd765 fdc;
    zr_fdd fdd;
    zr_drivesnd sound;
    zr_media *disk_media;
    zr_disk *disk;
    /* the serial port's UART (saved): a byte coming in from the TX line, bits sampled so far; the byte played onto RXD */
    zr_dev uart;
    uint8_t tx_line, tx_busy, tx_count, tx_byte;
    uint64_t tx_at;
    uint8_t rx_line, rx_busy, rx_bit, rx_byte;
    uint64_t rx_at;
    uint8_t queue[QUEUE];
    unsigned queue_head, queue_count;
    uint8_t cts_reported;
    zr_port_host ports;
    int has_ports;
} cip04;

static uint64_t now_of(const cip04 *h) { return zr_spectrum_now(&h->s); }

/* --- the memory map ---------------------------------------------------------------------------------------- */

/* The memory as 7FFDh and 1FFDh leave it: normally the ROM page 7FFDh bit 4 and 1FFDh bit 2 give at 0000h, RAM page 5 at 4000h, 2 at
 * 8000h and the page 7FFDh names at C000h; in the special mode four RAM pages (the configuration of 1FFDh bits 1 and 2) and no ROM. The
 * screen is page 5 or 7 by 7FFDh's bit 3 whatever is mapped. Spectrum timing contends the pages 4 to 7. */
static void remap(cip04 *h) {
    static const uint8_t SPECIAL[4][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {4, 5, 6, 3}, {4, 7, 6, 3}};
    zr_spectrum *s = &h->s;
    zr_bus *b = &s->bus;
    int bank[4];
    if (h->paging2 & 1u) {
        const uint8_t *c = SPECIAL[h->paging2 >> 1 & 3u];
        for (unsigned i = 0; i < 4u; ++i) bank[i] = c[i];
    } else {
        bank[0] = -1;
        bank[1] = 5;
        bank[2] = 2;
        bank[3] = h->paging & 7;
    }
    const unsigned rom = (h->paging >> 4 & 1u) | (h->paging2 >> 1 & 2u);
    for (unsigned i = 0; i < 4u; ++i) {
        if (bank[i] < 0) zr_bus_map_rom(b, (uint16_t)(i * PAGE), PAGE, s->rom + rom * PAGE);
        else zr_bus_map_ram(b, (uint16_t)(i * PAGE), PAGE, s->ram + (unsigned)bank[i] * PAGE);
        zr_spectrum_contend(s, i, bank[i] >= 4);
    }
    const uint8_t *screen = s->ram + (h->paging & 8u ? 7u : 5u) * PAGE;
    if (screen != s->screen) {
        zr_ula_catch_up(&s->ula, zr_spectrum_now(s));   /* the picture so far is of the screen that goes */
        s->screen = screen;
    }
    /* The tape's "stop if 48K" blocks stop it in the locked 48K mode. */
    zr_tape_deck_set_48k(&s->deck, (h->paging & 0x20u) != 0);
}

/* The DivIDE's and DivMMC's automatic mapping acts only while the 48 BASIC ROM (page 3: 7FFDh bit 4 and 1FFDh bit 2) is the one at
 * 0000h, as the real interfaces do through the ROM-select line; not in the special all-RAM modes (the RAM gate covers them too). */
static int interface_rom(zr_spectrum *s) {
    const cip04 *h = (const cip04 *)s;
    return !(h->paging2 & 1u) && ((h->paging >> 4 & 1u) | (h->paging2 >> 1 & 2u)) == 3u;
}

/* --- the floppy ---------------------------------------------------------------------------------------------- */

static void motor(cip04 *h, int on) {
    zr_fdd_set_motor(&h->fdd, on, now_of(h));
    zr_upd765_drive_changed(&h->fdc);
}

static uint8_t fdc_read(void *ctx, uint16_t port) {
    cip04 *h = ctx;
    return h->fdc.dev.ops->read(&h->fdc, ZR_SPACE_IO, port >> 12 & 1u);     /* 2FFDh the main status, 3FFDh the data */
}
static void fdc_write(void *ctx, uint16_t port, uint8_t value) {
    cip04 *h = ctx;
    h->fdc.dev.ops->write(&h->fdc, ZR_SPACE_IO, port >> 12 & 1u, value);
}

/* --- the printer ---------------------------------------------------------------------------------------------- */

static uint8_t printer_read(void *ctx, uint16_t port) {
    (void)port;
    return (uint8_t)(0xFEu | (((cip04 *)ctx)->printer_online ? 0u : 1u));  /* bit 0 BUSY: the printer is not on line */
}
static void printer_write(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    ((cip04 *)ctx)->printer_data = value;
}

/* --- the serial port -------------------------------------------------------------------------------------------- */

/* A bit in master-clock periods. */
static uint64_t bit_ticks(const cip04 *h) { return (uint64_t)h->s.cfg.clock_hz / (h->baud ? h->baud : 9600u); }

/* The TX line, 1 the mark (idle): AY register 14's bit 3 (the AY's pins: the pull-ups when the port is an input), and the firmware's
 * own transmit routine, which sets bit 6 of port FE for a space. */
static int tx_level(const cip04 *h) { return (h->s.ay.pins & 8u) != 0 && !h->fe6; }
/* The guest's CTS out (register 14's bit 2): low, ready to receive. */
static int cts_ready(const cip04 *h) { return !(h->s.ay.pins & 4u); }

static void uart_rearm(cip04 *h) {
    uint64_t next = ZR_NEVER;
    if (h->tx_busy && h->tx_at < next) next = h->tx_at;
    if (h->rx_busy && h->rx_at < next) next = h->rx_at;
    zr_dev_schedule(&h->uart, next);
}

static void report_cts(cip04 *h) {
    const uint8_t cts = (uint8_t)cts_ready(h);
    if (cts == h->cts_reported) return;
    h->cts_reported = cts;
    if (h->has_ports && h->ports.outputs) h->ports.outputs(h->ports.ctx, PORT_SERIAL, cts, cts);
}

/* The next byte from the host onto RXD: its start bit begins now. */
static void rx_start(cip04 *h, uint64_t now) {
    if (h->rx_busy || !h->queue_count || !cts_ready(h)) return;
    h->rx_byte = h->queue[h->queue_head];
    h->queue_head = (h->queue_head + 1u) % QUEUE;
    --h->queue_count;
    h->rx_busy = 1;
    h->rx_bit = 0;
    h->rx_line = 0;
    h->rx_at = now + bit_ticks(h);
}

/* A falling edge of TX while idle is a start bit; the bits are sampled in their middles. */
static void tx_edge(cip04 *h, int before, uint64_t now) {
    if (!h->tx_busy && before && !tx_level(h)) {
        h->tx_busy = 1;
        h->tx_count = 0;
        h->tx_byte = 0;
        h->tx_at = now + bit_ticks(h) * 3u / 2u;
    }
}

/* The lines changed (register 14 written, port FE written): TX and CTS follow. */
static void lines_changed(cip04 *h) {
    const uint64_t now = now_of(h);
    const int before = h->tx_line;
    h->tx_line = (uint8_t)tx_level(h);
    tx_edge(h, before, now);
    report_cts(h);
    rx_start(h, now);
    uart_rearm(h);
}

static void uart_event(void *dev, uint64_t now) {
    cip04 *h = ((zr_dev *)dev)->host->ctx;
    if (h->tx_busy && now >= h->tx_at) {
        const int bit = tx_level(h);
        if (h->tx_count < 8u) {
            h->tx_byte = (uint8_t)(h->tx_byte | (unsigned)bit << h->tx_count);
            ++h->tx_count;
            h->tx_at += bit_ticks(h);
        } else {
            /* The stop bit: a byte framed right goes out; a line already low again starts the next one now. */
            h->tx_busy = 0;
            if (bit && h->has_ports && h->ports.transmit) h->ports.transmit(h->ports.ctx, PORT_SERIAL, h->tx_byte);
            if (!tx_level(h)) tx_edge(h, 1, now);
        }
    }
    if (h->rx_busy && now >= h->rx_at) {
        ++h->rx_bit;
        h->rx_line = h->rx_bit <= 8u ? (uint8_t)(h->rx_byte >> (h->rx_bit - 1u) & 1u) : 1u;
        if (h->rx_bit < 10u) h->rx_at += bit_ticks(h);
        else {
            h->rx_busy = 0;
            rx_start(h, now);
        }
    }
    uart_rearm(h);
}
static uint8_t uart_no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void uart_no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void uart_no_reset(void *dev) { (void)dev; }
static const zr_dev_ops UART_OPS = {.read = uart_no_read, .write = uart_no_write, .event = uart_event, .reset = uart_no_reset};

/* Port A of the AY changed its pins (register 14 or 7 written). */
static void ay_pins(void *ctx, uint8_t pins) {
    (void)pins;
    lines_changed(ctx);
}

/* Register 14 read at FFFDh: the RXD line in bit 7, the peripheral's ready line in bit 6, the written bits below, whatever the chip's
 * port direction (the +3 reads its inputs through a port the firmware sets as an output). */
static uint8_t ay_read(void *ctx, uint16_t port) {
    cip04 *h = ctx;
    (void)port;
    zr_ay *ay = &h->s.ay;
    if (ay->valid && ay->selected == 14u) {
        zr_ay_read(ay);                                 /* the chip runs to now */
        return (uint8_t)((h->rx_line ? 0x80u : 0u) | (h->ready_in ? 0x40u : 0u) | (ay->reg[14] & 0x3Fu));
    }
    return zr_ay_read(ay);
}

/* Port FE: its bit 7 read is the peripheral's ready line (the firmware's transmit routine waits for it), its bit 6 written the TX line. */
static uint8_t fe_read(void *ctx, uint16_t port) {
    (void)port;
    return ((cip04 *)ctx)->ready_in ? 0xFFu : 0x7Fu;
}
static void fe_write(void *ctx, uint16_t port, uint8_t value) {
    cip04 *h = ctx;
    (void)port;
    const uint8_t bit = (uint8_t)(value >> 6 & 1u);
    if (bit == h->fe6) return;
    h->fe6 = bit;
    lines_changed(h);
}

/* --- the paging ports ---------------------------------------------------------------------------------------------- */

static void printer_strobe(cip04 *h) {
    if (h->has_ports && h->ports.transmit && h->printer_online) h->ports.transmit(h->ports.ctx, PORT_PRINTER, h->printer_data);
}

/* 7FFDh: A15 low, A14 high, A1 low (any such port: 5FFDh among them); locked until reset by bit 5. */
static void paging_write(void *ctx, uint16_t port, uint8_t value) {
    cip04 *h = ctx;
    (void)port;
    if (h->paging & 0x20u) return;
    h->paging = value;
    remap(h);
}

/* 1FFDh: the lock holds the paging bits (0 to 2); the disk motor and the printer's strobe (the byte goes out as it rises) go on. `silent`: a value put back by a
 * snapshot, which strobes nothing. */
static void paging2_apply(cip04 *h, uint8_t value, int silent) {
    const uint8_t old = h->paging2;
    uint8_t now = (uint8_t)(value & 0x1Fu);
    if (h->paging & 0x20u) now = (uint8_t)((now & 0x18u) | (old & 7u));
    h->paging2 = now;
    if ((now ^ old) & 8u) motor(h, now >> 3 & 1);
    if (!silent && !(old & 0x10u) && (now & 0x10u)) printer_strobe(h);
    remap(h);
}
static void paging2_write(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    paging2_apply(ctx, value, 0);
}

/* 5FFDh: the firmware writes 0 here at startup; only the latch is known. */
static void latch5_write(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    ((cip04 *)ctx)->latch5 = value;
}

/* --- the core's paging hooks (snapshots, ZR_PAGE_ADDRESS) ------------------------------------------------------------- */

static uint8_t *page_of(zr_spectrum *s, unsigned page) { return page < 8u ? s->ram + page * PAGE : NULL; }
static uint8_t paging_get(zr_spectrum *s) { return ((cip04 *)s)->paging; }
static void paging_set(zr_spectrum *s, uint8_t value) {
    cip04 *h = (cip04 *)s;
    h->paging = value;
    remap(h);
}
static uint8_t paging2_get(zr_spectrum *s) { return ((cip04 *)s)->paging2; }
static void paging2_set(zr_spectrum *s, uint8_t value) {
    cip04 *h = (cip04 *)s;
    const uint8_t locked = h->paging;
    h->paging = (uint8_t)(h->paging & 0xDFu);                       /* a snapshot puts the paging bits back whatever the lock says */
    paging2_apply(h, value, 1);
    h->paging = locked;
    remap(h);
}

/* --- life cycle ---------------------------------------------------------------------------------------------------- */

static void cip_reset(void *self, int power_on) {
    cip04 *h = self;
    zr_spectrum_reset(&h->s, power_on);
    h->paging = h->paging2 = h->latch5 = 0;
    h->fe6 = 0;
    h->printer_data = 0;
    h->tx_busy = h->rx_busy = 0;
    h->rx_line = 1;
    h->queue_head = h->queue_count = 0;
    h->tx_line = (uint8_t)tx_level(h);
    h->fdc.dev.ops->reset(&h->fdc);                     /* the controller's RESET is the system's */
    motor(h, 0);
    remap(h);
    h->cts_reported = (uint8_t)cts_ready(h);
    uart_rearm(h);
}

static void *cip_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.ram_size = 8u * PAGE;
    cfg.is_48k = 0;
    cfg.charset = 3u * PAGE + 0x3D00u;                  /* the 48 BASIC page's character set, which the editor and the menu draw with */
    cfg.traps_rom = 3u * PAGE;                          /* the 48 BASIC page */
    cfg.page = page_of;
    cfg.paging_get = paging_get;
    cfg.paging_set = paging_set;
    cfg.paging_48k = 0x30u;                             /* ROM 3 with the lock: 7FFDh 30h and 1FFDh 04h */
    cfg.paging2_get = paging2_get;
    cfg.paging2_set = paging2_set;
    cfg.paging2_48k = 0x04u;
    cfg.joystick = (uint8_t)o[OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_128K;                /* the model stands for a Sinclair +3 in the timing option (its gate array) */
    cfg.gate_array = 1;
    cfg.spectrum_timing = o[OPTION_TIMING] != 0;
    cfg.ay = (uint8_t)(1u + (o[OPTION_AY] < 3u ? o[OPTION_AY] : 0u));   /* the 128K's decode: A15 high, A1 low */
    zr_spectrum_interface_config(&cfg, o + OPTION_INTERFACE, SLOT_INTERFACE);
    cfg.interface_rom = interface_rom;
    cip04 *h = (cip04 *)zr_spectrum_create(&cfg, setup, sizeof *h, error, capacity);
    if (!h) return NULL;
    zr_spectrum *s = &h->s;
    h->baud = BAUD_RATES[o[OPTION_BAUD] < sizeof BAUD_RATES / sizeof BAUD_RATES[0] ? o[OPTION_BAUD] : 6u];
    h->printer_online = 1;
    h->ready_in = 1;
    h->rx_line = 1;
    h->cts_reported = 0xFFu;
    /* The floppy: the controller on the master clock (its 4 MHz follows the 250 kbit/s rate), the 3-inch drive, the sounds. */
    zr_upd765_init(&h->fdc, &s->host, "765", cfg.clock_hz);
    zr_upd765_attach(&h->fdc, 0, &h->fdd);
    zr_upd765_ready_wiring(&h->fdc, ZR_UPD765_READY_DRIVE);
    zr_fdd_init(&h->fdd, cfg.clock_hz, 1, 40, 300);
    zr_drivesnd_init(&h->sound, cfg.clock_hz, ZR_SPECTRUM_AUDIO_RATE);
    zr_drivesnd_set(&h->sound, o[OPTION_DRIVE_SOUND] != 0, 50);
    if (o[OPTION_DRIVE_SOUND]) zr_fdd_set_hook(&h->fdd, zr_drivesnd_event, &h->sound, 0);
    if (zr_drivesnd_enabled(&h->sound)) s->drives = &h->sound;
    /* The UART and the AY's port A. */
    h->uart.ops = &UART_OPS;
    h->uart.host = &s->host;
    h->uart.name = "uart";
    s->ay.io.ctx = h;
    s->ay.io.out = ay_pins;
    s->bus.dec[s->ay_dec[0]].read = ay_read;
    /* The ports. */
    zr_bus_add_io(&s->bus, 0xC002u, 0x4000u, NULL, paging_write, h);                   /* 7FFDh */
    zr_bus_add_io(&s->bus, 0xF002u, 0x1000u, NULL, paging2_write, h);                  /* 1FFDh */
    zr_bus_add_io(&s->bus, 0xE002u, 0x2000u, fdc_read, fdc_write, h);                  /* 2FFDh, 3FFDh */
    zr_bus_add_io(&s->bus, 0xF002u, 0x0000u, printer_read, printer_write, h);          /* 0FFDh */
    zr_bus_add_io(&s->bus, 0xFFFFu, 0x5FFDu, NULL, latch5_write, h);                   /* 5FFDh */
    zr_bus_add_io(&s->bus, cfg.fe_mask, cfg.fe_value, fe_read, fe_write, h);           /* FE's serial bits */
    zr_bus_watch(&s->bus, 0xC000u, 0x1C00u, 1);         /* page 7 shows at C000h too */
    cip_reset(h, 1);
    return h;
}

/* The disk goes back into its media when it has changed. */
static void flush_disk(cip04 *h) {
    if (!h->disk || !h->disk->dirty || !h->disk_media) return;
    char why[128];
    if (!zr_disk_save_media(h->disk, h->disk_media, why, sizeof why)) snprintf(h->s.error, sizeof h->s.error, "%s", why);
}

static void cip_destroy(void *self) {
    cip04 *h = self;
    if (!h) return;
    flush_disk(h);
    zr_fdd_eject(&h->fdd);
    zr_disk_close(h->disk);
    zr_spectrum_destroy(h);
}

/* After each run: a disk the guest wrote goes back into its media once its drive has stopped. */
static zr_run_status cip_run(void *self, uint64_t ticks) {
    cip04 *h = self;
    const zr_run_status status = zr_spectrum_run(h, ticks);
    if (h->disk && h->disk->dirty && !h->fdd.motor) flush_disk(h);
    return status;
}

/* --- media ------------------------------------------------------------------------------------------------------------- */

static int cip_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    cip04 *h = self;
    if (slot == SLOT_TAPE) return zr_spectrum_insert(self, slot, media, error, capacity);
    if (slot != SLOT_A) return zr_spectrum_insert(self, slot, media, error, capacity);
    if (h->disk_media) {
        if (capacity) snprintf(error, capacity, "The slot is not empty");
        return 0;
    }
    zr_disk *disk = zr_disk_open(media, NULL, error, capacity);
    if (!disk) return 0;
    h->fdd.tracks = (uint8_t)(disk->cylinders > 42u ? 80u : 40u);
    if (!zr_fdd_insert(&h->fdd, disk, error, capacity)) {
        zr_disk_close(disk);
        return 0;
    }
    h->disk = disk;
    h->disk_media = media;
    zr_upd765_drive_changed(&h->fdc);
    return 1;
}

static zr_media *cip_media(const void *self, unsigned slot) {
    return slot == SLOT_A ? ((const cip04 *)self)->disk_media : zr_spectrum_media(self, slot);
}

static int cip_eject(void *self, unsigned slot) {
    cip04 *h = self;
    if (slot != SLOT_A) return zr_spectrum_eject(self, slot);
    if (!h->disk_media) return 0;
    flush_disk(h);
    zr_fdd_eject(&h->fdd);
    zr_upd765_drive_changed(&h->fdc);
    zr_disk_close(h->disk);
    h->disk = NULL;
    h->disk_media = NULL;
    return 1;
}

static unsigned cip_activity(const void *self) {
    const cip04 *h = self;
    return zr_spectrum_activity(self) | (h->fdd.motor && h->fdd.disk ? 1u << SLOT_A : 0u);
}

/* --- the host's ends of the ports ------------------------------------------------------------------------------------- */

static void cip_set_ports(void *self, const zr_port_host *host) {
    cip04 *h = self;
    h->has_ports = host != NULL;
    if (host) h->ports = *host;
    if (host && host->outputs) {
        const int ready = cts_ready(h);
        h->cts_reported = (uint8_t)ready;
        host->outputs(host->ctx, PORT_SERIAL, ready, ready);
    }
}
static size_t cip_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    cip04 *h = self;
    if (port != PORT_SERIAL) return count;              /* the printer port only sends */
    size_t n = 0;
    for (; n < count && h->queue_count < QUEUE; ++n) {
        h->queue[(h->queue_head + h->queue_count) % QUEUE] = data[n];
        ++h->queue_count;
    }
    rx_start(h, now_of(h));
    uart_rearm(h);
    return n;
}
static void cip_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    cip04 *h = self;
    (void)dcd; (void)ri;
    if (port == PORT_SERIAL) h->ready_in = cts != 0;     /* the peripheral's ready line: FE bit 7, register 14 bit 6 */
    else h->printer_online = dsr != 0;                   /* BUSY while the printer is off line */
}

/* --- states ------------------------------------------------------------------------------------------------------------- */

static void cip_save(const void *self, zr_state_writer *w) {
    const cip04 *h = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "CP04", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, h->paging);
    zr_sw_u8(w, h->paging2);
    zr_sw_u8(w, h->latch5);
    zr_sw_u8(w, h->printer_data);
    zr_sw_u8(w, h->fe6);
    zr_sw_u8(w, h->printer_online);
    zr_sw_u8(w, h->ready_in);
    zr_sw_u8(w, h->tx_busy);
    zr_sw_u8(w, h->tx_count);
    zr_sw_u8(w, h->tx_byte);
    zr_sw_u64(w, h->tx_at);
    zr_sw_u8(w, h->rx_line);
    zr_sw_u8(w, h->rx_busy);
    zr_sw_u8(w, h->rx_bit);
    zr_sw_u8(w, h->rx_byte);
    zr_sw_u64(w, h->rx_at);
    zr_sw_u32(w, h->queue_count);
    for (unsigned i = 0; i < h->queue_count; ++i) zr_sw_u8(w, h->queue[(h->queue_head + i) % QUEUE]);
    zr_sw_end(w);
    zr_sw_begin(w, "765 ", 0);
    h->fdc.dev.ops->save(&h->fdc, w);
    zr_sw_end(w);
    zr_sw_begin(w, "FDD ", 0);
    zr_fdd_save(&h->fdd, w);
    zr_sw_end(w);
}

static int cip_load(void *self, zr_state_reader *r) {
    cip04 *h = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "CP04", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    h->paging = zr_sr_u8(r);
    h->paging2 = zr_sr_u8(r) & 0x1Fu;
    h->latch5 = zr_sr_u8(r);
    h->printer_data = zr_sr_u8(r);
    h->fe6 = zr_sr_u8(r) & 1u;
    h->printer_online = zr_sr_u8(r) & 1u;
    h->ready_in = zr_sr_u8(r) & 1u;
    h->tx_busy = zr_sr_u8(r) & 1u;
    h->tx_count = zr_sr_u8(r);
    h->tx_byte = zr_sr_u8(r);
    h->tx_at = zr_sr_u64(r);
    h->rx_line = zr_sr_u8(r) & 1u;
    h->rx_busy = zr_sr_u8(r) & 1u;
    h->rx_bit = zr_sr_u8(r);
    h->rx_byte = zr_sr_u8(r);
    h->rx_at = zr_sr_u64(r);
    const uint32_t count = zr_sr_u32(r);
    if (count > QUEUE || h->tx_count > 8u || h->rx_bit > 10u) return 0;
    h->queue_head = 0;
    h->queue_count = count;
    for (unsigned i = 0; i < count; ++i) h->queue[i] = zr_sr_u8(r);
    if (!zr_sr_ok(r)) return 0;
    if (!zr_sr_chunk(r, "FDD ", 0) || !zr_fdd_load(&h->fdd, r)) return 0;
    if (!zr_sr_chunk(r, "765 ", 0) || !h->fdc.dev.ops->load(&h->fdc, r)) return 0;
    h->tx_line = (uint8_t)tx_level(h);
    h->cts_reported = (uint8_t)cts_ready(h);
    remap(h);
    uart_rearm(h);
    /* The drive's sound starts afresh, in step with the beeper's next frame. */
    const zr_beeper *b = &h->s.beeper;
    zr_drivesnd_reset(&h->sound, b->num ? (b->done * b->den + b->num - 1u) / b->num : 0u);
    return zr_sr_ok(r);
}

static const zr_machine_impl CIP04_IMPL = {
    .destroy = cip_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = cip_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = cip_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = zr_spectrum_frame, .text = zr_spectrum_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = zr_spectrum_key, .joystick = zr_spectrum_joystick,
    .insert = cip_insert, .media = cip_media, .eject = cip_eject, .activity = cip_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .set_ports = cip_set_ports, .port_receive = cip_port_receive, .port_modem = cip_port_modem,
    .save = cip_save, .load = cip_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = zr_spectrum_peek, .poke = zr_spectrum_poke,
    .snapshot_load = zr_spectrum_snapshot_load, .snapshot_save = zr_spectrum_snapshot_save,
};

const zr_family zr_cip04_family = {
    .id = "cip04", .name = "CIP",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .ports = PORTS, .port_count = 2,
    .keys = zr_spectrum_keys, .key_count = ZR_SPECTRUM_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = zr_spectrum_combos, .combo_count = ZR_SPECTRUM_COMBOS,
    .joysticks = 2,
    .impl = &CIP04_IMPL, .create = cip_create,
};
