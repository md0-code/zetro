/* The CIP-04's printer and serial ports against the machine's own interface: a few programs put into RAM through a +3 snapshot
 * (so that they run as the firmware's code would, on the CPU and the bus) send bytes out through port 0FFDh with the strobe on
 * 1FFDh's bit 4 and through the AY's register 14 and port FE's bit 6 at 9600 baud, and read a byte in through register 14's bit 7;
 * the port host collects what the machine sends. Skipped, with a message, when the CIP-04 ROM is not in roms/. No SDL. */
#include "config.h"
#include "machine.h"
#include "media/snap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

typedef struct host {
    uint8_t bytes[2][64];
    unsigned count[2];
} host;

static void on_transmit(void *ctx, unsigned port, uint8_t byte) {
    host *h = ctx;
    if (port < 2u && h->count[port] < sizeof h->bytes[port]) h->bytes[port][h->count[port]++] = byte;
}

/* The machine with `program` in RAM at 8000h (page 2), data at 8100h, run from there with the interrupts off. */
static zr_machine *start(const zr_rom_index *roms, host *h, const uint8_t *program, size_t size, const uint8_t *data, size_t data_size) {
    static zr_config config;
    static zr_port_host port_host;
    char error[256];
    const char *yaml = "machine:\n  model: cip04\n";
    if (!zr_config_parse(yaml, strlen(yaml), ZR_WORK "/tests/zetro.yaml", &config, error, sizeof error)) return NULL;
    zr_machine *m = zr_machine_create(&config, roms, error, sizeof error);
    if (!m) return NULL;
    memset(h, 0, sizeof *h);
    port_host.ctx = h;
    port_host.transmit = on_transmit;
    zr_machine_set_ports(m, &port_host);
    zr_snap *snap = zr_snap_new();
    zr_snap_set_machine(snap, ZR_SNAP_M_PLUS3, 5, "ZX Spectrum +3");
    snap->pc = 0x8000u;
    snap->sp = 0xBF00u;
    snap->im = 1;
    snap->pages = 0xFFu;
    memcpy(snap->ram[2], program, size);
    if (data) memcpy(snap->ram[2] + 0x100, data, data_size);
    char why[128];
    if (!zr_machine_snapshot_load(m, snap, why, sizeof why)) {
        printf("snapshot: %s\n", why);
        zr_machine_destroy(m);
        zr_snap_free(snap);
        return NULL;
    }
    zr_snap_free(snap);
    return m;
}

static uint8_t at(zr_machine *m, uint16_t address) {
    uint8_t v = 0;
    zr_machine_peek(m, address, &v);
    return v;
}

/* The printer: three bytes with the strobe low then high on 1FFDh's bit 4 (data on 0FFDh), then BUSY read over and over. */
static const uint8_t PRINTER[] = {
    0xF3,                                           /* DI */
    0x21, 0x00, 0x81, 0x16, 0x03,                   /* LD HL,8100h; LD D,3 */
    0x01, 0xFD, 0x0F, 0x7E, 0xED, 0x79, 0x23,       /* next: LD BC,0FFDh; LD A,(HL); OUT (C),A; INC HL */
    0x01, 0xFD, 0x1F, 0xAF, 0xED, 0x79,             /* LD BC,1FFDh; XOR A; OUT (C),A (strobe low) */
    0x3E, 0x10, 0xED, 0x79, 0x15, 0x20, 0xEC,       /* LD A,10h; OUT (C),A (strobe high); DEC D; JR NZ,next */
    0x01, 0xFD, 0x0F, 0xED, 0x78, 0x32, 0x00, 0x90, 0x18, 0xF9,   /* LD BC,0FFDh; busy: IN A,(C); LD (9000h),A; JR busy */
};

/* The serial transmitter: the line values at 8100h (11 of them), one every 363 T-states (9600 baud). `select` is the AY's
 * register 14 sequence for register 14 (the first program) or none (the second, which drives port FE). */
static const uint8_t SERIAL_AY[] = {
    0xF3,                                           /* DI */
    0x01, 0xFD, 0xFF, 0x3E, 0x0E, 0xED, 0x79,       /* LD BC,FFFDh; LD A,14; OUT (C),A */
    0x01, 0xFD, 0xBF, 0x3E, 0xFA, 0xED, 0x79,       /* LD BC,BFFDh; LD A,FAh; OUT (C),A (register 14: TXD high) */
    0x01, 0xFD, 0xFF, 0x3E, 0x07, 0xED, 0x79,       /* LD BC,FFFDh; LD A,7; OUT (C),A */
    0x01, 0xFD, 0xBF, 0x3E, 0xFF, 0xED, 0x79,       /* LD BC,BFFDh; LD A,FFh; OUT (C),A (register 7: port A an output) */
    0x01, 0xFD, 0xFF, 0x3E, 0x0E, 0xED, 0x79,       /* LD BC,FFFDh; LD A,14; OUT (C),A */
    0x01, 0xFD, 0xBF, 0x21, 0x00, 0x81, 0x16, 0x0B, /* LD BC,BFFDh; LD HL,8100h; LD D,11 */
    0x7E, 0x23, 0xED, 0x79, 0x1E, 0x14,             /* bit: LD A,(HL); INC HL; OUT (C),A; LD E,20 */
    0x1D, 0x20, 0xFD, 0x15, 0x20, 0xF4,             /* wait: DEC E; JR NZ,wait; DEC D; JR NZ,bit */
    0x18, 0xFE,                                     /* JR $ */
};
static const uint8_t SERIAL_FE[] = {
    0xF3,                                           /* DI */
    0x01, 0xFE, 0xBF, 0x21, 0x00, 0x81, 0x16, 0x0B, /* LD BC,BFFEh; LD HL,8100h; LD D,11 */
    0x7E, 0x23, 0xED, 0x79, 0x1E, 0x14,             /* bit: LD A,(HL); INC HL; OUT (C),A; LD E,20 */
    0x1D, 0x20, 0xFD, 0x15, 0x20, 0xF4,             /* wait: DEC E; JR NZ,wait; DEC D; JR NZ,bit */
    0x18, 0xFE,                                     /* JR $ */
};
/* The receiver: register 14 written with CTS low (ready; the pins carry it once register 7 makes the port an output), then register 14
 * read until bit 7 (RXD) falls and sampled in the middle of each bit, the byte stored at 9000h. */
static const uint8_t SERIAL_RX[] = {
    0xF3,                                           /* DI */
    0x01, 0xFD, 0xFF, 0x3E, 0x0E, 0xED, 0x79,       /* LD BC,FFFDh; LD A,14; OUT (C),A */
    0x01, 0xFD, 0xBF, 0x3E, 0xFB, 0xED, 0x79,       /* LD BC,BFFDh; LD A,FBh; OUT (C),A (register 14: CTS low, ready to receive) */
    0x01, 0xFD, 0xFF, 0x3E, 0x07, 0xED, 0x79,       /* LD BC,FFFDh; LD A,7; OUT (C),A */
    0x01, 0xFD, 0xBF, 0x3E, 0xFF, 0xED, 0x79,       /* LD BC,BFFDh; LD A,FFh; OUT (C),A (register 7: the pins now carry it) */
    0x01, 0xFD, 0xFF, 0x3E, 0x0E, 0xED, 0x79,       /* LD BC,FFFDh; LD A,14; OUT (C),A */
    0xED, 0x78, 0x17, 0x38, 0xFB,                   /* wait: IN A,(C); RLA; JR C,wait (until the start bit) */
    0x1E, 0x1F, 0x1D, 0x20, 0xFD,                   /* LD E,31; d1: DEC E; JR NZ,d1 (a bit and a half) */
    0x16, 0x08, 0x26, 0x00,                         /* LD D,8; LD H,0 */
    0xED, 0x78, 0x17, 0xCB, 0x1C, 0x1E, 0x14,      /* rx: IN A,(C); RLA; RR H; LD E,20 */
    0x1D, 0x20, 0xFD, 0x15, 0x20, 0xF3,             /* w2: DEC E; JR NZ,w2; DEC D; JR NZ,rx */
    0x7C, 0x32, 0x00, 0x90, 0x18, 0xFE,             /* LD A,H; LD (9000h),A; JR $ */
};

/* A frame of line values for a byte: start, eight data bits LSB first, two stop bits. `mark` and `space` are the line's two values. */
static void frame(uint8_t *out, uint8_t byte, uint8_t mark, uint8_t space) {
    out[0] = space;
    for (unsigned i = 0; i < 8u; ++i) out[1 + i] = byte >> i & 1u ? mark : space;
    out[9] = out[10] = mark;
}

int main(void) {
    static zr_rom_index roms;
    const char *folders[2] = {ZR_WORK "/../roms", ZR_WORK "/../../../../roms"};
    zr_rom_index_init(&roms);
    zr_rom_index_scan(&roms, folders, 2);
    host h;
    uint8_t line[11];
    frame(line, 0xA5, 0xFA, 0xF2);                  /* register 14: bit 3 is TXD, mark high */
    zr_machine *m = start(&roms, &h, PRINTER, sizeof PRINTER, (const uint8_t *)"Hi!", 3);
    if (!m) {
        printf("no CIP-04 ROM (python tools/fetch_roms.py): the ports are not checked\n");
        zr_rom_index_free(&roms);
        return 0;
    }
    const uint64_t period = zr_machine_frame_period(m);
    zr_machine_run(m, period * 4);
    CHECK(h.count[1] == 3 && !memcmp(h.bytes[1], "Hi!", 3));            /* the printer's bytes, on the strobe's rising edge */
    CHECK(h.count[0] == 0);
    CHECK((at(m, 0x9000) & 1u) == 0);                                  /* not BUSY: the printer is on line */
    zr_machine_port_modem(m, 1, 0, 0, 0, 0);                           /* off line */
    zr_machine_run(m, period);
    CHECK((at(m, 0x9000) & 1u) == 1);                                  /* BUSY */
    zr_machine_port_modem(m, 1, 0, 0, 1, 0);
    zr_machine_run(m, period);
    CHECK((at(m, 0x9000) & 1u) == 0);
    zr_machine_destroy(m);
    m = start(&roms, &h, SERIAL_AY, sizeof SERIAL_AY, line, sizeof line);
    CHECK(m != NULL);
    if (m) {
        zr_machine_run(m, period * 4);
        CHECK(h.count[0] == 1 && h.bytes[0][0] == 0xA5);               /* the byte, framed on register 14's TXD */
        CHECK(h.count[1] == 0);
        zr_machine_destroy(m);
    }
    frame(line, 0x5A, 0x00, 0x40);                  /* the firmware's own transmitter: port FE's bit 6 high is a space */
    m = start(&roms, &h, SERIAL_FE, sizeof SERIAL_FE, line, sizeof line);
    CHECK(m != NULL);
    if (m) {
        zr_machine_run(m, period * 4);
        CHECK(h.count[0] == 1 && h.bytes[0][0] == 0x5A);
        zr_machine_destroy(m);
    }
    m = start(&roms, &h, SERIAL_RX, sizeof SERIAL_RX, NULL, 0);
    CHECK(m != NULL);
    if (m) {
        const uint8_t byte = 0x3C;
        CHECK(zr_machine_port_receive(m, 0, &byte, 1) == 1);
        zr_machine_run(m, period * 4);
        CHECK(at(m, 0x9000) == 0x3C);                                  /* read in through register 14's bit 7 */
        zr_machine_destroy(m);
    }
    zr_rom_index_free(&roms);
    if (failures) printf("%d failures\n", failures);
    else printf("the CIP-04's printer and serial ports: ok\n");
    return failures ? 1 : 0;
}
