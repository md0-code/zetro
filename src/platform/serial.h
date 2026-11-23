/* The host ends of the guest's byte ports (ports.KEY in zetro.yaml): a host serial device, a local pipe, a
 * TCP connection or server, an output file, or a loopback plug. The ends are those the configuration names for the ports of
 * the running model (the family's port table, machine.h); bytes the guest sends are queued by the machine's transmit callback
 * and written out by zr_host_ports_pump, which also offers the bytes that arrived to the machine (as the port takes them) and
 * passes on the modem inputs; none of it blocks. A printer is on line while its end is open (a file always; a connection while
 * it is up); a serial device passes its own DSR, CTS and DCD. An end that cannot be opened leaves the port unconnected and says
 * why (zr_host_ports_error). The OS-specific parts are the host layer's streams (src/host/host.h); no SDL here, so a headless
 * run opens the ends too. The ports come from
 * the machine's table, not from a fixed list. */
#ifndef ZR_HOST_PORTS_H
#define ZR_HOST_PORTS_H

#include "../core/config.h"
#include "../core/machine.h"
#include "../host/host.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define ZR_PORTS_MAX 8                  /* ends open at once: at most as many ports as a family has (ZR_CONFIG_PORTS) */
#define ZR_PORTS_BUFFER 4096
#define ZR_PORTS_ERROR 192

typedef struct zr_port_end {
    char key[ZR_CONFIG_NAME];           /* the configuration's key of the port it is on; "" for a free slot */
    uint32_t type;                      /* ZR_END_* */
    char path[ZR_CONFIG_PATH];          /* what it was opened with */
    zr_host_stream *stream;             /* a serial device, a pipe or a TCP end */
    FILE *file;                         /* an output file */
    int dtr, rts;                       /* the guest's handshake outputs */
    int dcd, cts, dsr, ri;              /* the modem inputs last passed on (-1: none yet) */
    uint8_t out[ZR_PORTS_BUFFER];
    size_t out_count;
    uint8_t in[ZR_PORTS_BUFFER];
    size_t in_count;
} zr_port_end;

typedef struct zr_host_ports {
    zr_port_end end[ZR_PORTS_MAX];
    int map[ZR_PORTS_MAX];              /* for each port of the machine's family: the end it has, -1 none */
    char error[ZR_PORTS_MAX][ZR_PORTS_ERROR];   /* for each port: why its end could not be opened ("" when it could, or none is set) */
    unsigned count;                     /* the ports of the machine's family */
    zr_port_host host;                  /* what the machine is given */
    int attached;                       /* the machine has been given `host` */
} zr_host_ports;

void zr_host_ports_init(zr_host_ports *ports);
/* The ends brought to what the configuration says for the ports of the machine's model, and the machine given them: an end that
 * is as it was stays open (a connected terminal stays connected over a reset or a rebuild); one that changed is closed after its
 * queued bytes are written, and the new one opened. Returns how many could not be opened (their reasons: zr_host_ports_error).
 * Call it for a new machine after zr_host_ports_detach of the old one, and again when the configuration's ports changed. */
unsigned zr_host_ports_configure(zr_host_ports *ports, const zr_config *config, zr_machine *machine);
/* Why the end of port `port` (the family's table index) could not be opened; NULL when it could, or none is set. */
const char *zr_host_ports_error(const zr_host_ports *ports, unsigned port);
/* Writes what the guest sent, reads what arrived and gives it to the machine, passes on the modem inputs. After each run slice. */
void zr_host_ports_pump(zr_host_ports *ports, zr_machine *machine);
/* The next pump passes every port's modem inputs on again (a state was loaded, the machine was reset). */
void zr_host_ports_resend(zr_host_ports *ports);
/* The machine is taken off the ends (before it is destroyed); the ends stay open for the next one. */
void zr_host_ports_detach(zr_host_ports *ports, zr_machine *machine);
/* Everything flushed and closed. */
void zr_host_ports_close(zr_host_ports *ports, zr_machine *machine);
/* The name of a pipe as the host layer wants it: a bare NAME is \\.\pipe\NAME on Windows and /tmp/NAME elsewhere. */
void zr_host_ports_pipe_path(const char *name, char *out, size_t capacity);

#endif /* ZR_HOST_PORTS_H */
