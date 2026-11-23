/* The TIM-S against its ROM, from the host's side (the harness has no command for the guest's byte ports): a byte
 * printed with LPRINT arrives at the printer port's host end, and only while the host says the printer is on line;
 * the firmware's own serial routine sends its bytes to the serial port's host end once the host says the device is
 * ready, 1444 T-states a bit; a state saved in the middle of the start and one saved at the auxiliary clock load into a
 * new machine and run on to the same state, bit for bit, and a state of another CPU switch is refused. Skipped (exit
 * code 77) without the TIM-S ROM in roms/ (python tools/fetch_roms.py). No SDL. */
#include "config.h"
#include "machine.h"
#include "romdb.h"
#include "state.h"
#include "media/snap.h"
#include "../src/app/typist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

enum { PORT_SERIAL, PORT_PRINTER };

typedef struct host {
    zr_machine *machine;
    uint8_t printer[64], serial[64];
    unsigned printer_count, serial_count;
    uint64_t e4_time[64];                   /* guest time of each write to port E4h since `tracing` was set */
    unsigned e4_count;
    int tracing;
} host;

static void transmit(void *ctx, unsigned port, uint8_t byte) {
    host *h = ctx;
    if (port == PORT_PRINTER && h->printer_count < sizeof h->printer) h->printer[h->printer_count++] = byte;
    if (port == PORT_SERIAL && h->serial_count < sizeof h->serial) h->serial[h->serial_count++] = byte;
}
static void trace(void *ctx, int kind, uint32_t address, uint32_t value) {
    host *h = ctx;
    (void)value;
    if (h->tracing && kind == ZR_TRACE_IO_WRITE && (address & 0xFFu) == 0xE4u && h->e4_count < 64u)
        h->e4_time[h->e4_count++] = zr_machine_time(h->machine);
}

static zr_machine *build(const zr_rom_index *roms, const char *options) {
    static zr_config config;
    char yaml[256], error[256];
    snprintf(yaml, sizeof yaml, "machine:\n  model: tims\n  options:\n    tape_traps: off\n%s", options);
    if (!zr_config_parse(yaml, strlen(yaml), ZR_WORK "/tests/zetro.yaml", &config, error, sizeof error)) return NULL;
    zr_machine *m = zr_machine_create(&config, roms, error, sizeof error);
    if (m && strcmp(zr_machine_family(m)->id, "tims")) {
        zr_machine_destroy(m);
        return NULL;
    }
    return m;
}

static void frames(zr_machine *m, zr_typist *typist, unsigned count) {
    const uint64_t period = zr_machine_frame_period(m);
    for (unsigned i = 0; i < count; ++i) {
        if (typist) zr_typist_step(typist, m);
        zr_machine_run(m, period);
    }
}
static void key(zr_machine *m, const char *name, int down) {
    const int k = zr_family_key(zr_machine_family(m), name);
    CHECK(k >= 0);
    if (k >= 0) zr_machine_key(m, (unsigned)k, down);
}
static int screen_has(zr_machine *m, const char *text) {
    static char buffer[4096];
    zr_machine_text(m, buffer, sizeof buffer);
    return strstr(buffer, text) != NULL;
}
/* Runs until `text` is on the screen, at most `limit` frames. */
static int until(zr_machine *m, zr_typist *typist, const char *text, unsigned limit) {
    for (unsigned i = 0; i < limit; ++i) {
        if (screen_has(m, text)) return 1;
        frames(m, typist, 1);
    }
    return screen_has(m, text);
}

/* The machine's state as bytes. */
static int save(const zr_machine *m, zr_state_writer *w) {
    zr_sw_init(w);
    zr_machine_save(m, w);
    return zr_sw_finish(w);
}
static int load(zr_machine *m, const zr_state_writer *w) {
    zr_state_reader r;
    char error[128];
    return zr_sr_open(&r, w->data, w->size, error, sizeof error) && zr_machine_load(m, &r, error, sizeof error);
}

/* --- the printer and the serial line --------------------------------------------------------------------------- */

static void test_ports(const zr_rom_index *roms) {
    static host h;
    zr_typist typist;
    memset(&h, 0, sizeof h);
    zr_typist_init(&typist);
    zr_machine *m = build(roms, "");
    CHECK(m != NULL);
    if (!m) return;
    h.machine = m;
    const zr_port_host ports = {.ctx = &h, .transmit = transmit};
    zr_machine_set_ports(m, &ports);
    CHECK(until(m, &typist, "TIMISOARA", 300));
    frames(m, &typist, 20);

    /* LPRINT "A": CAPS SHIFT and SYMBOL SHIFT, then C (LPRINT in extended mode), the string, ENTER. The printer is not on
     * line: the firmware waits for its BUSY line (BREAK would end it) and sends nothing. */
    key(m, "CAPS SHIFT", 1);
    key(m, "SYMBOL SHIFT", 1);
    frames(m, &typist, 4);
    key(m, "SYMBOL SHIFT", 0);
    key(m, "CAPS SHIFT", 0);
    frames(m, &typist, 8);
    key(m, "C", 1);
    frames(m, &typist, 4);
    key(m, "C", 0);
    frames(m, &typist, 8);
    CHECK(zr_typist_start(&typist, m, "\"A\"\n"));
    frames(m, &typist, 150);
    CHECK(h.printer_count == 0);
    CHECK(!screen_has(m, "0 OK"));
    /* The printer comes on line: the byte goes out at the strobe, and the program ends. */
    zr_machine_port_modem(m, PORT_PRINTER, 0, 0, 1, 0);
    frames(m, &typist, 100);
    CHECK(h.printer_count >= 1 && h.printer[0] == 'A');
    CHECK(h.serial_count == 0);
    CHECK(until(m, &typist, "0 OK", 200));
    printf("LPRINT \"A\": %u bytes to the printer port:", h.printer_count);
    for (unsigned i = 0; i < h.printer_count; ++i) printf(" %02X", h.printer[i]);
    printf("\n");

    /* The firmware's serial routine (3B7Fh: B the character), called from a program in RAM: it waits for READY (the
     * host's DSR), then sends each byte as a start bit, seven data bits and two stop bits, the bits 1444 T-states apart. */
    zr_snap *snap = zr_snap_new();
    CHECK(snap != NULL);
    if (snap) {
        CHECK(zr_machine_snapshot_save(m, snap));
        static const uint8_t program[] = {0x06, 'A', 0xCD, 0x7F, 0x3B, 0x06, 'B', 0xCD, 0x7F, 0x3B, 0x18, 0xFE};
        memcpy(snap->ram[2], program, sizeof program);
        snap->pc = 0x8000u;
        snap->halted = 0;
        char error[128];
        CHECK(zr_machine_snapshot_load(m, snap, error, sizeof error));
        zr_snap_free(snap);
    }
    zr_machine_set_trace(m, trace, &h);
    h.tracing = 1;
    h.serial_count = 0;
    h.e4_count = 0;
    frames(m, NULL, 100);
    printf("serial before READY: %u bytes, %u writes of port C\n", h.serial_count, h.e4_count);
    CHECK(h.serial_count == 0 && h.e4_count == 0);
    zr_machine_port_modem(m, PORT_SERIAL, 0, 0, 1, 0);
    frames(m, NULL, 30);
    h.tracing = 0;
    CHECK(h.serial_count == 2 && h.serial[0] == 'A' && h.serial[1] == 'B');
    /* 'A' is 41h: the writes of port C are the start bit, the seven data bits (a bit apart) and the stop bit's. */
    CHECK(h.e4_count >= 9);
    unsigned spaced = 0;
    for (unsigned i = 1; i < 8 && i < h.e4_count; ++i)
        if (h.e4_time[i] - h.e4_time[i - 1] == 1444u * 48u) ++spaced;
    CHECK(spaced == 7);
    if (h.e4_count >= 2)
        printf("serial: %llu ticks (%llu T-states of 3.5 MHz) between the writes of port C: %.0f baud\n",
               (unsigned long long)(h.e4_time[1] - h.e4_time[0]), (unsigned long long)((h.e4_time[1] - h.e4_time[0]) / 48u),
               168000000.0 / (double)(h.e4_time[1] - h.e4_time[0]));
    zr_machine_destroy(m);
}

/* --- states ------------------------------------------------------------------------------------------------------ */

/* The offset of the first chunk (the header ends where the chunk "OPTS" begins). */
static size_t body_of(const zr_state_writer *w) {
    for (size_t i = 0; i + 4 < w->size; ++i)
        if (!memcmp(w->data + i, "OPTS", 4)) return i;
    return w->size;
}
/* The ULA's chunk keeps the frame counter the shell watches one higher after a load by design (the ULA says "what the shell
 * shows is not this frame"): it is not part of what a state must give back, so it is cleared before two states are compared. */
static void clear_frame_counter(zr_state_writer *w) {
    for (size_t p = body_of(w); p + 12 <= w->size;) {
        uint32_t length;
        memcpy(&length, w->data + p + 8, 4);
        if (!memcmp(w->data + p, "ULA ", 4) && length >= 38u && p + 12u + 38u <= w->size) memset(w->data + p + 12 + 30, 0, 8);
        p += 12u + length;
    }
}
/* The tag of the first chunk in which two states differ ("" when they do not). */
static const char *first_difference(const zr_state_writer *a, const zr_state_writer *b) {
    static char tag[8];
    for (size_t p = body_of(a); p + 12 <= a->size && p + 12 <= b->size;) {
        uint32_t la, lb;
        memcpy(&la, a->data + p + 8, 4);
        memcpy(&lb, b->data + p + 8, 4);
        if (la != lb || p + 12u + la > a->size || p + 12u + la > b->size || memcmp(a->data + p, b->data + p, (size_t)la + 12u)) {
            memcpy(tag, a->data + p, 4);
            tag[4] = 0;
            return tag;
        }
        p += 12u + la;
    }
    return "";
}

static void round_trip(const zr_rom_index *roms, const char *options, unsigned frames_before, unsigned frames_after, const char *what) {
    zr_machine *a = build(roms, options), *b = build(roms, options);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) return;
    frames(a, NULL, frames_before);
    zr_state_writer s1, sa, sb;
    CHECK(save(a, &s1));
    CHECK(load(b, &s1));
    frames(a, NULL, frames_after);
    frames(b, NULL, frames_after);
    CHECK(save(a, &sa));
    CHECK(save(b, &sb));
    clear_frame_counter(&sa);
    clear_frame_counter(&sb);
    const int same = sa.size == sb.size && !memcmp(sa.data, sb.data, sa.size);
    CHECK(same);
    CHECK(zr_machine_time(a) == zr_machine_time(b));
    if (same) printf("state round trip, %s: %zu bytes, the same after %u frames\n", what, sa.size, frames_after);
    else printf("state round trip, %s: the first chunk that differs is \"%s\"\n", what, first_difference(&sa, &sb));
    zr_sw_free(&s1);
    zr_sw_free(&sa);
    zr_sw_free(&sb);
    zr_machine_destroy(a);
    zr_machine_destroy(b);
}

static void test_states(const zr_rom_index *roms) {
    /* In the middle of the start (the startup ROM paged in, the 8255 not set up yet) and after it. */
    round_trip(roms, "", 3, 40, "the startup ROM paged in");
    round_trip(roms, "", 200, 60, "running BASIC at 3.5 MHz");
    /* At the auxiliary clock, in the middle of the start and after it: the clock in use is part of the state. */
    round_trip(roms, "    cpu_switch: 7\n", 30, 40, "7 MHz, in the start");
    round_trip(roms, "    cpu_switch: 7\n", 200, 60, "running BASIC at 7 MHz");
    /* A state of another CPU switch is refused. */
    zr_machine *seven = build(roms, "    cpu_switch: 7\n"), *plain = build(roms, "");
    CHECK(seven != NULL && plain != NULL);
    if (seven && plain) {
        zr_state_writer w;
        frames(seven, NULL, 10);
        CHECK(save(seven, &w));
        CHECK(!load(plain, &w));
        zr_sw_free(&w);
    }
    if (seven) zr_machine_destroy(seven);
    if (plain) zr_machine_destroy(plain);
}

int main(void) {
    static zr_rom_index roms;
    const char *folders[2] = {ZR_WORK "/../roms", ZR_WORK "/../../../../roms"};
    zr_rom_index_init(&roms);
    zr_rom_index_scan(&roms, folders, 2);
    zr_machine *probe = build(&roms, "");
    if (!probe) {
        printf("skipped: no TIM-S ROM in roms/ (python tools/fetch_roms.py)\n");
        zr_rom_index_free(&roms);
        return 77;
    }
    zr_machine_destroy(probe);
    test_ports(&roms);
    test_states(&roms);
    zr_rom_index_free(&roms);
    if (failures) printf("%d failures\n", failures);
    else printf("test_tims passed\n");
    return failures ? 1 : 0;
}
