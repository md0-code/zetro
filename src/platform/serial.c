/* The host ends of the guest's byte ports (serial.h). The ends are those the
 * configuration names for the ports of the running machine's model, found by the key of the family's port table. */
#include "serial.h"

#include <stdlib.h>
#include <string.h>

void zr_host_ports_pipe_path(const char *name, char *out, size_t capacity) {
    if (strchr(name, '/') || strchr(name, '\\')) snprintf(out, capacity, "%s", name);
    else if (zr_host_separator() == '\\') snprintf(out, capacity, "\\\\.\\pipe\\%s", name);
    else snprintf(out, capacity, "/tmp/%s", name);
}

static void reset_lines(zr_port_end *e) { e->dcd = e->cts = e->dsr = e->ri = -1; }

/* What the end has queued for the line goes out: a file at once, a stream as far as it takes it. */
static void flush(zr_port_end *e) {
    if (!e->out_count) return;
    if (e->file) {
        fwrite(e->out, 1, e->out_count, e->file);
        fflush(e->file);
        e->out_count = 0;
        return;
    }
    if (!e->stream) { e->out_count = 0; return; }
    const long wrote = zr_host_stream_write(e->stream, e->out, e->out_count);
    if (wrote < 0) { e->out_count = 0; return; }                    /* the other end went away */
    if (wrote > 0) {
        memmove(e->out, e->out + wrote, e->out_count - (size_t)wrote);
        e->out_count -= (size_t)wrote;
    }
}

static void read_in(zr_port_end *e) {
    if (!e->stream) return;
    const size_t room = sizeof e->in - e->in_count;
    if (!room) return;
    const long got = zr_host_stream_read(e->stream, e->in + e->in_count, room);
    if (got > 0) e->in_count += (size_t)got;
}

static void close_end(zr_port_end *e) {
    flush(e);
    if (e->file) fclose(e->file);
    if (e->stream) zr_host_stream_close(e->stream);
    memset(e, 0, sizeof *e);
}

/* The folder of an output file is made when it is not there. */
static void make_folder_of(const char *path) {
    char folder[ZR_CONFIG_PATH];
    snprintf(folder, sizeof folder, "%s", path);
    char *cut = NULL;
    for (char *c = folder; *c; ++c) if (*c == '/' || *c == '\\') cut = c;
    if (cut && cut != folder) {
        *cut = 0;
        zr_host_make_folders(folder);
    }
}

/* Opens an end; 0 with the reason. A loopback plug is wired by the machine (the shell loops the bytes itself for a machine that
 * does not). */
static int open_end(zr_port_end *e, const char *key, uint32_t type, const char *path, char *error, size_t capacity) {
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", key);
    e->type = type;
    snprintf(e->path, sizeof e->path, "%s", path);
    reset_lines(e);
    if (type == ZR_END_FILE) {
        make_folder_of(path);
        e->file = fopen(path, "ab");
        if (!e->file) snprintf(error, capacity, "Cannot write the file %s", path);
        return e->file != NULL;
    }
    if (type == ZR_END_SERIAL || type == ZR_END_PIPE || type == ZR_END_TCP || type == ZR_END_TCP_LISTEN) {
        char name[ZR_CONFIG_PATH];
        const int kind = type == ZR_END_SERIAL ? ZR_STREAM_SERIAL : type == ZR_END_PIPE ? ZR_STREAM_PIPE
                       : type == ZR_END_TCP ? ZR_STREAM_TCP_CLIENT : ZR_STREAM_TCP_LISTEN;
        if (type == ZR_END_PIPE) zr_host_ports_pipe_path(path, name, sizeof name);
        else snprintf(name, sizeof name, "%s", path);
        e->stream = zr_host_stream_open(kind, name, error, capacity);
        return e->stream != NULL;
    }
    return 1;                           /* a loopback plug */
}

static zr_port_end *end_of(zr_host_ports *p, unsigned port) {
    return port < ZR_PORTS_MAX && p->map[port] >= 0 ? &p->end[p->map[port]] : NULL;
}

/* --- what the machine is given ----------------------------------------------------------------------------------- */

static void transmit(void *ctx, unsigned port, uint8_t byte) {
    zr_port_end *e = end_of(ctx, port);
    if (!e) return;                                                 /* no end: the byte is lost */
    if (e->type == ZR_END_LOOPBACK) {                               /* a machine that did not wire the plug itself */
        if (e->in_count < sizeof e->in) e->in[e->in_count++] = byte;
        return;
    }
    if (e->out_count == sizeof e->out) flush(e);
    if (e->out_count < sizeof e->out) e->out[e->out_count++] = byte;       /* else the line overran: lost */
}

static void outputs(void *ctx, unsigned port, int dtr, int rts) {
    zr_port_end *e = end_of(ctx, port);
    if (!e) return;
    e->dtr = dtr;
    e->rts = rts;
    if (e->stream) zr_host_stream_outputs(e->stream, dtr, rts);
}

static int loopback(void *ctx, unsigned port) {
    const zr_port_end *e = end_of(ctx, port);
    return e && e->type == ZR_END_LOOPBACK;
}

/* --- the ends ------------------------------------------------------------------------------------------------------ */

void zr_host_ports_init(zr_host_ports *p) {
    memset(p, 0, sizeof *p);
    for (unsigned i = 0; i < ZR_PORTS_MAX; ++i) p->map[i] = -1;
}

const char *zr_host_ports_error(const zr_host_ports *p, unsigned port) {
    return port < ZR_PORTS_MAX && p->error[port][0] ? p->error[port] : NULL;
}

/* One slot of the ends: the open one for the key with this type and path, -1 when there is none. */
static int find_end(const zr_host_ports *p, const char *key, uint32_t type, const char *path) {
    for (int i = 0; i < ZR_PORTS_MAX; ++i) {
        const zr_port_end *e = &p->end[i];
        if (e->key[0] && !strcmp(e->key, key) && e->type == type && !strcmp(e->path, path)) return i;
    }
    return -1;
}

unsigned zr_host_ports_configure(zr_host_ports *p, const zr_config *config, zr_machine *m) {
    const zr_family *f = zr_machine_family(m);
    const unsigned model = zr_machine_model(m);
    const unsigned n = f->port_count < ZR_PORTS_MAX ? f->port_count : ZR_PORTS_MAX;
    struct { char key[ZR_CONFIG_NAME]; uint32_t type; const char *path; } want[ZR_PORTS_MAX];
    for (unsigned i = 0; i < ZR_PORTS_MAX; ++i) {
        want[i].key[0] = 0;
        want[i].type = ZR_END_NONE;
        want[i].path = "";
        p->error[i][0] = 0;
        if (i >= n || !(f->ports[i].models >> model & 1u)) continue;
        const zr_config_port *c = zr_config_port_find(config, f->ports[i].key);
        snprintf(want[i].key, sizeof want[i].key, "%s", f->ports[i].key);
        if (!c) continue;
        if (c->type == ZR_END_LOOPBACK && f->ports[i].kind == ZR_PORT_PARALLEL) continue;       /* a printer has nothing to loop back */
        want[i].type = c->type;
        want[i].path = c->path;
    }
    /* An end that is not wanted as it is goes: its modem inputs fall first when the machine had been given them. */
    for (int slot = 0; slot < ZR_PORTS_MAX; ++slot) {
        zr_port_end *e = &p->end[slot];
        if (!e->key[0]) continue;
        int keep = 0;
        for (unsigned i = 0; i < n && !keep; ++i)
            keep = want[i].type == e->type && !strcmp(want[i].key, e->key) && !strcmp(want[i].path, e->path);
        if (keep) continue;
        for (unsigned i = 0; i < ZR_PORTS_MAX; ++i)
            if (p->map[i] == slot && p->attached && e->dsr >= 0) zr_machine_port_modem(m, i, 0, 0, 0, 0);
        close_end(e);
    }
    unsigned failed = 0;
    for (unsigned i = 0; i < ZR_PORTS_MAX; ++i) p->map[i] = -1;
    for (unsigned i = 0; i < n; ++i) {
        if (want[i].type == ZR_END_NONE) continue;
        int slot = find_end(p, want[i].key, want[i].type, want[i].path);
        if (slot < 0) {
            for (int s = 0; s < ZR_PORTS_MAX && slot < 0; ++s) if (!p->end[s].key[0]) slot = s;
            if (slot < 0 || !open_end(&p->end[slot], want[i].key, want[i].type, want[i].path, p->error[i], sizeof p->error[i])) {
                if (slot >= 0) { close_end(&p->end[slot]); }
                if (!p->error[i][0]) snprintf(p->error[i], sizeof p->error[i], "Too many ports open");
                ++failed;
                continue;
            }
        }
        p->map[i] = slot;
    }
    p->count = n;
    p->host.ctx = p;
    p->host.transmit = transmit;
    p->host.outputs = outputs;
    p->host.loopback = loopback;
    for (int slot = 0; slot < ZR_PORTS_MAX; ++slot) reset_lines(&p->end[slot]);
    zr_machine_set_ports(m, &p->host);
    p->attached = 1;
    return failed;
}

/* The modem inputs: a device's lines; a pipe's and a TCP end's are its connection; an output file or a loopback plug is always
 * ready. */
static void modem(zr_port_end *e, unsigned port, zr_machine *m) {
    int dcd = 1, cts = 1, dsr = 1, ri = 0;
    if (e->stream && !zr_host_stream_modem(e->stream, &dcd, &cts, &dsr, &ri)) return;
    if (dcd != e->dcd || cts != e->cts || dsr != e->dsr || ri != e->ri) {
        e->dcd = dcd;
        e->cts = cts;
        e->dsr = dsr;
        e->ri = ri;
        zr_machine_port_modem(m, port, dcd, cts, dsr, ri);
    }
}

void zr_host_ports_pump(zr_host_ports *p, zr_machine *m) {
    for (unsigned port = 0; port < p->count; ++port) {
        zr_port_end *e = end_of(p, port);
        if (!e) continue;
        flush(e);
        read_in(e);
        if (e->in_count) {
            size_t took = zr_machine_port_receive(m, port, e->in, e->in_count);
            if (took > e->in_count) took = e->in_count;
            memmove(e->in, e->in + took, e->in_count - took);
            e->in_count -= took;
        }
        modem(e, port, m);
    }
}

void zr_host_ports_resend(zr_host_ports *p) {
    for (int slot = 0; slot < ZR_PORTS_MAX; ++slot) reset_lines(&p->end[slot]);
}

void zr_host_ports_detach(zr_host_ports *p, zr_machine *m) {
    if (m && p->attached) zr_machine_set_ports(m, NULL);
    p->attached = 0;
    for (int slot = 0; slot < ZR_PORTS_MAX; ++slot) {
        if (p->end[slot].key[0]) flush(&p->end[slot]);
        reset_lines(&p->end[slot]);
    }
}

void zr_host_ports_close(zr_host_ports *p, zr_machine *m) {
    zr_host_ports_detach(p, m);
    for (int slot = 0; slot < ZR_PORTS_MAX; ++slot) if (p->end[slot].key[0]) close_end(&p->end[slot]);
    for (unsigned i = 0; i < ZR_PORTS_MAX; ++i) p->map[i] = -1;
}
