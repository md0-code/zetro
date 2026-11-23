/* The host ends of the byte ports (platform/serial.h) on a real machine, from the host's side: the TIM-S (its serial line and its
 * printer) is given a loopback plug and an output file by the configuration; what the machine's transmit callback is handed reaches
 * the file when the layer is pumped, a plug the machine does not wire itself is looped back by the layer, an end that is as it was
 * stays open over a new configuration and one that changed is opened anew, and an end that cannot be opened (a TCP server on a port
 * somebody holds) leaves its port unconnected and says why while the other port is served. Skipped (exit code 77) without the TIM-S
 * ROM in roms/ (python tools/fetch_roms.py). No SDL. */
#include "config.h"
#include "machine.h"
#include "romdb.h"
#include "../src/host/host.h"
#include "../src/platform/serial.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

enum { PORT_SERIAL, PORT_PRINTER };

static zr_config config;

static int parse(const char *ports) {
    char yaml[2048], error[256];
    snprintf(yaml, sizeof yaml, "machine:\n  model: tims\nports:\n%s", ports);
    if (zr_config_parse(yaml, strlen(yaml), ZR_WORK "/tests/zetro.yaml", &config, error, sizeof error)) return 1;
    printf("the configuration: %s\n", error);
    return 0;
}

static size_t file_bytes(const char *path, char *out, size_t capacity) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const size_t n = fread(out, 1, capacity, f);
    fclose(f);
    return n;
}

int main(void) {
    static zr_rom_index roms;
    static zr_host_ports ports;
    const char *folders[2] = {ZR_WORK "/../roms", ZR_WORK "/../../../../roms"};
    zr_rom_index_init(&roms);
    zr_rom_index_scan(&roms, folders, 2);
    char error[256];
    if (!parse("  serial: loopback\n")) return 1;
    zr_machine *m = zr_machine_create(&config, &roms, error, sizeof error);
    if (!m || strcmp(zr_machine_family(m)->id, "tims")) {
        printf("skipped: no TIM-S ROM in roms/ (python tools/fetch_roms.py)\n");
        if (m) zr_machine_destroy(m);
        zr_rom_index_free(&roms);
        return 77;
    }
    const char *out_a = ZR_WORK "/tests/hostports-a.out", *out_b = ZR_WORK "/tests/hostports-b.out";
    zr_host_delete_file(out_a);
    zr_host_delete_file(out_b);

    /* The plug on the serial line and a file on the printer. */
    char text[512];
    snprintf(text, sizeof text, "  serial: loopback\n  printer: 'file:%s'\n", out_a);
    CHECK(parse(text));
    zr_host_ports_init(&ports);
    CHECK(zr_host_ports_configure(&ports, &config, m) == 0);
    CHECK(ports.count == 2 && ports.map[PORT_SERIAL] >= 0 && ports.map[PORT_PRINTER] >= 0);
    CHECK(ports.host.loopback(ports.host.ctx, PORT_SERIAL) == 1 && ports.host.loopback(ports.host.ctx, PORT_PRINTER) == 0);
    ports.host.transmit(ports.host.ctx, PORT_PRINTER, 'A');
    ports.host.transmit(ports.host.ctx, PORT_PRINTER, 'B');
    ports.host.transmit(ports.host.ctx, PORT_SERIAL, 0x55);          /* the TIM-S wires no plug: the layer loops the byte back */
    CHECK(ports.end[ports.map[PORT_SERIAL]].in_count == 1);
    zr_host_ports_pump(&ports, m);
    CHECK(ports.end[ports.map[PORT_SERIAL]].in_count == 0);          /* offered to the machine, which took it */
    char got[16];
    CHECK(file_bytes(out_a, got, sizeof got) == 2 && !memcmp(got, "AB", 2));

    /* The same configuration keeps the ends open; a changed printer file is opened anew and the old one is closed. */
    FILE *kept = ports.end[ports.map[PORT_PRINTER]].file;
    CHECK(zr_host_ports_configure(&ports, &config, m) == 0 && ports.end[ports.map[PORT_PRINTER]].file == kept);
    snprintf(text, sizeof text, "  serial: loopback\n  printer: 'file:%s'\n", out_b);
    CHECK(parse(text));
    CHECK(zr_host_ports_configure(&ports, &config, m) == 0 && ports.end[ports.map[PORT_PRINTER]].file != NULL);
    ports.host.transmit(ports.host.ctx, PORT_PRINTER, 'C');
    zr_host_ports_pump(&ports, m);
    CHECK(file_bytes(out_b, got, sizeof got) == 1 && got[0] == 'C');
    CHECK(file_bytes(out_a, got, sizeof got) == 2);                   /* nothing more reached the first file */

    /* An end that cannot be opened: a server on a port somebody holds. The serial line is still served. */
    zr_host_stream *holder = zr_host_stream_open(ZR_STREAM_TCP_LISTEN, "127.0.0.1:0", error, sizeof error);
    CHECK(holder != NULL);
    if (holder) {
        snprintf(text, sizeof text, "  serial: loopback\n  printer: tcp-listen:127.0.0.1:%u\n", zr_host_stream_local_port(holder));
        CHECK(parse(text));
        CHECK(zr_host_ports_configure(&ports, &config, m) == 1);
        CHECK(zr_host_ports_error(&ports, PORT_PRINTER) != NULL && zr_host_ports_error(&ports, PORT_SERIAL) == NULL);
        CHECK(ports.map[PORT_PRINTER] < 0 && ports.map[PORT_SERIAL] >= 0);
        ports.host.transmit(ports.host.ctx, PORT_PRINTER, 'D');       /* no end: the byte is lost, nothing breaks */
        zr_host_ports_pump(&ports, m);
        zr_host_stream_close(holder);
    }

    /* Everything is closed with the machine taken off first. */
    zr_host_ports_close(&ports, m);
    zr_machine_destroy(m);
    zr_rom_index_free(&roms);
    if (failures) printf("%d failures\n", failures);
    else printf("test_hostports passed\n");
    return failures ? 1 : 0;
}
