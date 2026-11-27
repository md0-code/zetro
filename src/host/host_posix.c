/* ZetRo - the host layer on POSIX systems (host.h): termios serial
 * devices, Unix sockets for pipes, opendir for folders, as POSIX.1-2008
 * documents them (TCP ends on BSD sockets). Not built during development: the Linux targets are first built in the release
 * phase. ZetRo's own code. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "host.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/serial.h>
#endif

/* --- process ----------------------------------------------------------------- */

int zr_host_console_init(void) { return 0; }

void zr_host_alert(const char *title, const char *text) { fprintf(stderr, "%s: %s\n", title, text); }

int zr_host_user_locale(char *out, size_t capacity) {
    static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        const char *value = getenv(names[i]);
        if (value && *value && strcmp(value, "C") && strcmp(value, "POSIX")) return snprintf(out, capacity, "%s", value) < (int)capacity;
    }
    return 0;
}

void zr_host_sleep_us(uint32_t us) {
    struct timespec t;
    t.tv_sec = (time_t)(us / 1000000u);
    t.tv_nsec = (long)(us % 1000000u) * 1000;
    nanosleep(&t, NULL);
}

int zr_host_program_folder(char *out, size_t capacity) {
    char path[ZR_HOST_PATH];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    char *slash = strrchr(path, '/');
    if (!slash) return 0;
    slash[1] = 0;
    return snprintf(out, capacity, "%s", path) < (int)capacity;
}

/* --- files ------------------------------------------------------------------- */

char zr_host_separator(void) { return '/'; }

int zr_host_list_files(const char *folder, zr_host_file_fn fn, void *ctx) {
    DIR *d = opendir(folder);
    if (!d) return 0;
    struct dirent *e;
    char path[ZR_HOST_PATH];
    const size_t n = strlen(folder);
    const int separated = n && folder[n - 1] == '/';
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (snprintf(path, sizeof path, "%s%s%s", folder, separated ? "" : "/", e->d_name) >= (int)sizeof path) continue;
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        if (!fn(ctx, e->d_name)) break;
    }
    closedir(d);
    return 1;
}

int zr_host_list_folder(const char *folder, zr_host_entry_fn fn, void *ctx) {
    DIR *d = opendir(folder);
    if (!d) return 0;
    struct dirent *e;
    char path[ZR_HOST_PATH];
    const size_t n = strlen(folder);
    const int separated = n && folder[n - 1] == '/';
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        if (e->d_name[0] == '.') continue;     /* hidden entries, "." and ".." */
        if (snprintf(path, sizeof path, "%s%s%s", folder, separated ? "" : "/", e->d_name) >= (int)sizeof path) continue;
        if (stat(path, &st) || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) continue;
        if (!fn(ctx, e->d_name, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0u : (uint64_t)st.st_size)) break;
    }
    closedir(d);
    return 1;
}

int zr_host_list_roots(zr_host_entry_fn fn, void *ctx) { fn(ctx, "/", 1, 0); return 1; }

int zr_host_file_size(const char *path, uint64_t *size) {
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    *size = (uint64_t)st.st_size;
    return 1;
}

int zr_host_folder_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int zr_host_full_path(const char *path, char *out, size_t capacity) {
    char full[PATH_MAX], joined[PATH_MAX];
    /* realpath wants the path to exist: resolve the folder part and keep the name. */
    if (realpath(path, full)) return snprintf(out, capacity, "%s", full) < (int)capacity;
    const char *slash = strrchr(path, '/');
    if (!slash) {
        if (!getcwd(full, sizeof full)) return 0;
        return snprintf(out, capacity, "%s/%s", full, path) < (int)capacity;
    }
    if (snprintf(joined, sizeof joined, "%.*s", (int)(slash - path) + 1, path) >= (int)sizeof joined || !realpath(joined, full)) return 0;
    return snprintf(out, capacity, "%s/%s", full, slash + 1) < (int)capacity;
}

int zr_host_make_folders(const char *path) {
    char p[PATH_MAX];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p || !p[0]) return 0;
    for (char *c = p + 1; *c; ++c)
        if (*c == '/' && c[-1] != '/') {
            *c = 0;
            if (!zr_host_folder_exists(p)) mkdir(p, 0777);
            *c = '/';
        }
    if (!zr_host_folder_exists(p)) mkdir(p, 0777);
    return zr_host_folder_exists(p);
}

int zr_host_delete_file(const char *path) { return unlink(path) == 0 || errno == ENOENT; }

int zr_host_local_time(zr_host_time *t) {
    const time_t now = time(NULL);
    struct tm tm;
    if (now == (time_t)-1 || !localtime_r(&now, &tm)) return 0;
    t->year = (uint16_t)(tm.tm_year + 1900);
    t->month = (uint8_t)(tm.tm_mon + 1);
    t->day = (uint8_t)tm.tm_mday;
    t->hour = (uint8_t)tm.tm_hour;
    t->minute = (uint8_t)tm.tm_min;
    t->second = (uint8_t)tm.tm_sec;
    return 1;
}

int zr_host_replace_file(const char *temporary, const char *path) { return rename(temporary, path) == 0; }

int zr_host_same_folder(const char *a, const char *b) {
    char fa[PATH_MAX], fb[PATH_MAX];
    if (!realpath(a, fa) || !realpath(b, fb)) return !strcmp(a, b);
    return !strcmp(fa, fb);
}

/* --- byte streams ------------------------------------------------------------ */

/* TCP ends retry a connection this long after the last attempt. */
#define RETRY_MS 1000

struct zr_host_stream {
    int kind;
    int fd;                     /* the device, or the connected socket (-1 none) */
    int listener;               /* a served pipe's or a TCP server's listening socket (-1 none) */
    char path[ZR_HOST_PATH];
    struct sockaddr_in peer;    /* TCP_CLIENT: where to connect */
    int connecting;             /* TCP_CLIENT: a connection attempt is under way */
    int connected;              /* TCP: a connection is up */
    long long retry_at;         /* TCP_CLIENT: when to try again (milliseconds) */
    unsigned local_port;
};

static zr_host_stream *fail(zr_host_stream *s, char *error, size_t capacity, const char *what, const char *path) {
    snprintf(error, capacity, "%s %s (%s)", what, path, strerror(errno));
    if (s->fd >= 0) close(s->fd);
    if (s->listener >= 0) close(s->listener);
    free(s);
    return NULL;
}

static void accept_client(zr_host_stream *s) {
    if (s->listener < 0 || s->fd >= 0) return;
    int fd = accept(s->listener, NULL, NULL);
    if (fd >= 0) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        s->fd = fd;
    }
}

/* --- TCP: non-blocking sockets polled by the caller's reads and writes ---------- */

static int is_tcp(const zr_host_stream *s) { return s->kind == ZR_STREAM_TCP_CLIENT || s->kind == ZR_STREAM_TCP_LISTEN; }

static long long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* "HOST:PORT", "PORT" or ":PORT" into a host name (empty when absent) and a port. */
static int split_target(const char *path, char *host, size_t capacity, unsigned *port) {
    const char *colon = strrchr(path, ':');
    const char *digits = colon ? colon + 1 : path;
    char *end = NULL;
    if (!*digits) return 0;
    const unsigned long value = strtoul(digits, &end, 10);
    if (*end || value > 65535) return 0;
    *port = (unsigned)value;
    const size_t n = colon ? (size_t)(colon - path) : 0;
    if (n >= capacity) return 0;
    memcpy(host, path, n);
    host[n] = 0;
    return 1;
}

static int resolve_ipv4(const char *host, unsigned port, struct sockaddr_in *out) {
    struct addrinfo hints, *found = NULL;
    char service[8];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof service, "%u", port);
    if (getaddrinfo(host, service, &hints, &found) || !found) return 0;
    memcpy(out, found->ai_addr, sizeof *out);
    freeaddrinfo(found);
    return 1;
}

static void tcp_options(int fd) {
    int nodelay = 1;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof nodelay);
}

/* The connection is gone: a client tries again in a second, a listener takes the next one. */
static void tcp_drop(zr_host_stream *s) {
    if (s->fd >= 0) close(s->fd);
    s->fd = -1;
    s->connected = 0;
    s->connecting = 0;
    s->retry_at = now_ms() + RETRY_MS;
}

static void tcp_begin(zr_host_stream *s) {
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd < 0) { s->retry_at = now_ms() + RETRY_MS; return; }
    tcp_options(s->fd);
    if (connect(s->fd, (const struct sockaddr *)&s->peer, sizeof s->peer) == 0) s->connected = 1;
    else if (errno == EINPROGRESS) s->connecting = 1;
    else tcp_drop(s);
}

/* Moves a connection along: a listener accepts, a client finishes or starts its attempt. */
static void tcp_service(zr_host_stream *s) {
    if (s->kind == ZR_STREAM_TCP_LISTEN) {
        if (s->connected) return;
        const int client = accept(s->listener, NULL, NULL);
        if (client < 0) return;
        tcp_options(client);
        s->fd = client;
        s->connected = 1;
    } else if (s->connecting) {
        struct pollfd p = {s->fd, POLLOUT, 0};
        if (poll(&p, 1, 0) <= 0) return;
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) tcp_drop(s);
        else { s->connecting = 0; s->connected = 1; }
    } else if (!s->connected && now_ms() >= s->retry_at) tcp_begin(s);
}

static zr_host_stream *tcp_open(zr_host_stream *s, const char *path, char *error, size_t capacity) {
    char host[256];
    unsigned port = 0;
    if (!split_target(path, host, sizeof host, &port) || (s->kind == ZR_STREAM_TCP_CLIENT && (!*host || !port))) {
        snprintf(error, capacity, "Cannot use the TCP address %s", path);
        free(s);
        return NULL;
    }
    if (s->kind == ZR_STREAM_TCP_CLIENT) {
        if (!resolve_ipv4(host, port, &s->peer)) {
            snprintf(error, capacity, "Cannot find the address %s", path);
            free(s);
            return NULL;
        }
        tcp_begin(s);
        return s;
    }
    struct sockaddr_in local;
    socklen_t length = sizeof local;
    int reuse = 1;
    if (!resolve_ipv4(*host ? host : "127.0.0.1", port, &local)) {
        snprintf(error, capacity, "Cannot use the address %s", path);
        free(s);
        return NULL;
    }
    s->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listener < 0) return fail(s, error, capacity, "Cannot listen on", path);
    setsockopt(s->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
    if (bind(s->listener, (const struct sockaddr *)&local, sizeof local) || listen(s->listener, 1)) return fail(s, error, capacity, "Cannot listen on", path);
    fcntl(s->listener, F_SETFL, fcntl(s->listener, F_GETFL) | O_NONBLOCK);
    if (getsockname(s->listener, (struct sockaddr *)&local, &length) == 0) s->local_port = ntohs(local.sin_port);
    return s;
}

zr_host_stream *zr_host_stream_open(int kind, const char *path, char *error, size_t capacity) {
    zr_host_stream *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return NULL; }
    s->kind = kind;
    s->fd = s->listener = -1;
    snprintf(s->path, sizeof s->path, "%s", path);
    if (kind == ZR_STREAM_TCP_CLIENT || kind == ZR_STREAM_TCP_LISTEN) return tcp_open(s, path, error, capacity);
    if (kind == ZR_STREAM_SERIAL) {
        s->fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (s->fd < 0) return fail(s, error, capacity, "Cannot open the serial port", path);
        struct termios t;
        if (!tcgetattr(s->fd, &t)) {
            cfmakeraw(&t);
            tcsetattr(s->fd, TCSANOW, &t);
        }
        return s;
    }
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) { errno = ENAMETOOLONG; return fail(s, error, capacity, "Cannot open the pipe", path); }
    strcpy(a.sun_path, path);
    /* A socket someone else serves is joined; otherwise ZetRo serves it. */
    s->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->fd < 0) return fail(s, error, capacity, "Cannot open the pipe", path);
    if (connect(s->fd, (struct sockaddr *)&a, sizeof a) == 0) {
        fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
        return s;
    }
    close(s->fd);
    s->fd = -1;
    unlink(path);
    s->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s->listener < 0 || bind(s->listener, (struct sockaddr *)&a, sizeof a) || listen(s->listener, 1))
        return fail(s, error, capacity, "Cannot open the pipe", path);
    fcntl(s->listener, F_SETFL, fcntl(s->listener, F_GETFL) | O_NONBLOCK);
    accept_client(s);
    return s;
}

static void broken(zr_host_stream *s) {
    if (s->listener >= 0 && s->fd >= 0) { close(s->fd); s->fd = -1; }
}

long zr_host_stream_read(zr_host_stream *s, uint8_t *data, size_t capacity) {
    if (!s || !capacity) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return 0;
        const ssize_t got = recv(s->fd, data, capacity, 0);
        if (got > 0) return (long)got;
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        tcp_drop(s);                    /* closed by the other side, or broken */
        return -1;
    }
    accept_client(s);
    if (s->fd < 0) return 0;
    ssize_t n = read(s->fd, data, capacity);
    if (n > 0) return (long)n;
    if (n == 0 && s->kind == ZR_STREAM_PIPE) { broken(s); return -1; }
    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { broken(s); return -1; }
    return 0;
}

long zr_host_stream_write(zr_host_stream *s, const uint8_t *data, size_t count) {
    if (!s || !count) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return (long)count;          /* nobody listens: the bytes are gone */
        const ssize_t sent = send(s->fd, data, count, MSG_NOSIGNAL);
        if (sent >= 0) return (long)sent;
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
        tcp_drop(s);
        return -1;
    }
    accept_client(s);
    if (s->fd < 0) return (long)count;          /* nobody listens: the bytes are gone */
    ssize_t n = write(s->fd, data, count);
    if (n >= 0) return (long)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    broken(s);
    return -1;
}

int zr_host_stream_modem(zr_host_stream *s, int *dcd, int *cts, int *dsr, int *ri) {
    if (!s) return 0;
    if (s->kind == ZR_STREAM_SERIAL) {
        int bits = 0;
        if (ioctl(s->fd, TIOCMGET, &bits)) return 0;
        *dcd = (bits & TIOCM_CAR) != 0;
        *cts = (bits & TIOCM_CTS) != 0;
        *dsr = (bits & TIOCM_DSR) != 0;
        *ri = (bits & TIOCM_RNG) != 0;
        return 1;
    }
    if (is_tcp(s)) {
        tcp_service(s);
        *dcd = *cts = *dsr = s->connected;
    } else *dcd = *cts = *dsr = s->fd >= 0;
    *ri = 0;
    return 1;
}

void zr_host_stream_outputs(zr_host_stream *s, int dtr, int rts) {
    if (!s || s->kind != ZR_STREAM_SERIAL) return;
    int bits = 0;
    if (ioctl(s->fd, TIOCMGET, &bits)) return;
    bits = dtr ? bits | TIOCM_DTR : bits & ~TIOCM_DTR;
    bits = rts ? bits | TIOCM_RTS : bits & ~TIOCM_RTS;
    ioctl(s->fd, TIOCMSET, &bits);
}

unsigned zr_host_stream_local_port(const zr_host_stream *s) { return s ? s->local_port : 0; }

void zr_host_stream_close(zr_host_stream *s) {
    if (!s) return;
    if (s->fd >= 0) close(s->fd);
    if (s->listener >= 0) {
        close(s->listener);
        if (s->kind == ZR_STREAM_PIPE) unlink(s->path);
    }
    free(s);
}

/* ttyS2 before ttyS10: shorter names first. */
static int name_order(const void *a, const void *b) {
    const char *x = a, *y = b;
    const size_t lx = strlen(x), ly = strlen(y);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x, y);
}

/* A ttyS device is a real UART when the driver knows its type. */
static int uart_exists(const char *path) {
#ifdef __linux__
    struct serial_struct info;
    const int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return 0;
    const int known = ioctl(fd, TIOCGSERIAL, &info) == 0 && info.type != 0;
    close(fd);
    return known;
#else
    (void)path;
    return 1;
#endif
}

unsigned zr_host_list_serial(zr_host_name_fn fn, void *ctx) {
    enum { MAX_PORTS = 64, NAME = 48 };
    static char names[MAX_PORTS][NAME];
    unsigned count = 0;
    DIR *dir = opendir("/dev");
    if (!dir) return 0;
    struct dirent *e;
    while (count < MAX_PORTS && (e = readdir(dir)) != NULL) {
        const char *n = e->d_name;
        char path[NAME];
        if (strncmp(n, "ttyUSB", 6) && strncmp(n, "ttyACM", 6) && strncmp(n, "ttyS", 4) && strncmp(n, "cu.", 3)) continue;
        if (strlen(n) + 5 >= sizeof path) continue;
        snprintf(path, sizeof path, "/dev/%.*s", (int)(sizeof path - 6), n);
        if (!strncmp(n, "ttyS", 4) && !uart_exists(path)) continue;
        memcpy(names[count++], path, sizeof path);
    }
    closedir(dir);
    qsort(names, count, NAME, name_order);
    for (unsigned i = 0; i < count; ++i)
        if (fn && !fn(ctx, names[i])) break;
    return count;
}

/* --- whole files (disk images) ------------------------------------------------ */

static void file_error(char *error, size_t capacity, const char *what, const char *path) {
    if (error && capacity) snprintf(error, capacity, "%s %s (%s)", what, path, strerror(errno));
}

int zr_host_file_exists(const char *path) { return access(path, F_OK) == 0; }

int zr_host_read_file(const char *path, size_t limit, uint8_t **data, size_t *size, int *writable, char *error, size_t capacity) {
    *data = NULL;
    *size = 0;
    if (writable) *writable = access(path, W_OK) == 0 || errno == ENOENT;
    const int fd = open(path, O_RDONLY);
    if (fd < 0) { file_error(error, capacity, "Cannot read", path); return 0; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) {
        if (error && capacity) snprintf(error, capacity, "%s is not a regular file", path);
        close(fd);
        return 0;
    }
    if ((unsigned long long)st.st_size > limit) {
        if (error && capacity) snprintf(error, capacity, "%s is too large", path);
        close(fd);
        return 0;
    }
    const size_t n = (size_t)st.st_size;
    uint8_t *buffer = malloc(n ? n : 1);
    if (!buffer) { if (error && capacity) snprintf(error, capacity, "Out of memory"); close(fd); return 0; }
    size_t done = 0;
    while (done < n) {
        const ssize_t got = read(fd, buffer + done, n - done);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { file_error(error, capacity, "Cannot read", path); free(buffer); close(fd); return 0; }
        done += (size_t)got;
    }
    close(fd);
    *data = buffer;
    *size = n;
    return 1;
}

int zr_host_write_file(const char *path, const uint8_t *data, size_t size, char *error, size_t capacity) {
    char temporary[ZR_HOST_PATH + 16];
    if (snprintf(temporary, sizeof temporary, "%s.saving", path) >= (int)sizeof temporary) {
        if (error && capacity) snprintf(error, capacity, "The path %s is too long", path);
        return 0;
    }
    const int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { file_error(error, capacity, "Cannot write", path); return 0; }
    size_t done = 0;
    int ok = 1;
    while (ok && done < size) {
        const ssize_t put = write(fd, data + done, size - done);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) ok = 0;
        else done += (size_t)put;
    }
    if (ok && fsync(fd)) ok = 0;
    if (!ok) file_error(error, capacity, "Cannot write", path);
    if (close(fd) && ok) { ok = 0; file_error(error, capacity, "Cannot write", path); }
    if (ok && rename(temporary, path)) { ok = 0; file_error(error, capacity, "Cannot replace", path); }
    if (!ok) unlink(temporary);
    return ok;
}

/* --- files read and written in place ---------------------------------------- */

struct zr_host_file {
    unsigned references;
    int fd;
    uint64_t length;
};

zr_host_file *zr_host_file_open(const char *path, int *writable, char *error, size_t capacity) {
    int fd = open(path, O_RDWR);
    const int can_write = fd >= 0;
    if (fd < 0) fd = open(path, O_RDONLY);
    if (fd < 0) { if (error && capacity) snprintf(error, capacity, "Cannot read %s (%s)", path, strerror(errno)); return NULL; }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        if (error && capacity) snprintf(error, capacity, "Cannot read %s (not a file)", path);
        close(fd);
        return NULL;
    }
    zr_host_file *f = malloc(sizeof *f);
    if (!f) { if (error && capacity) snprintf(error, capacity, "Out of memory"); close(fd); return NULL; }
    f->fd = fd;
    f->references = 1;
    f->length = (uint64_t)st.st_size;
    if (writable) *writable = can_write;
    return f;
}

int zr_host_file_create(const char *path, uint64_t size, int overwrite, char *error, size_t capacity) {
    const int fd = open(path, O_RDWR | O_CREAT | (overwrite ? O_TRUNC : O_EXCL), 0666);
    if (fd < 0) { if (error && capacity) snprintf(error, capacity, "Cannot create %s (%s)", path, strerror(errno)); return 0; }
    /* ftruncate makes a sparse file on the file systems that have them. */
    const int ok = ftruncate(fd, (off_t)size) == 0 && fsync(fd) == 0;
    if (!ok && error && capacity) snprintf(error, capacity, "Cannot create %s (%s)", path, strerror(errno));
    close(fd);
    if (!ok) unlink(path);
    return ok;
}

int zr_host_file_read_at(zr_host_file *f, uint64_t offset, void *data, size_t length) {
    uint8_t *p = data;
    while (length) {
        const ssize_t n = pread(f->fd, p, length, (off_t)offset);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return 0; }
        p += n;
        offset += (uint64_t)n;
        length -= (size_t)n;
    }
    return 1;
}
int zr_host_file_write_at(zr_host_file *f, uint64_t offset, const void *data, size_t length) {
    const uint8_t *p = data;
    while (length) {
        const ssize_t n = pwrite(f->fd, p, length, (off_t)offset);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; return 0; }
        p += n;
        offset += (uint64_t)n;
        length -= (size_t)n;
        if (offset > f->length) f->length = offset;
    }
    return 1;
}
int zr_host_file_flush(zr_host_file *f) { return fsync(f->fd) == 0; }
uint64_t zr_host_file_length(const zr_host_file *f) { return f->length; }
zr_host_file *zr_host_file_retain(zr_host_file *f) {
    ++f->references;
    return f;
}

void zr_host_file_close(zr_host_file *f) {
    if (!f || --f->references) return;
    close(f->fd);
    free(f);
}
