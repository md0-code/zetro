/* ZetRo - the host layer on Windows (host.h): Win32 calls as the Windows
 * API documents them. The TCP ends are on Winsock. */
#include "host.h"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- process ----------------------------------------------------------------- */

int zr_host_console_init(void) {
    int hidden = 0;
    /* A program of the Windows subsystem has no console of its own: its
     * messages go to the console it was started from, or to the pipes a
     * script gave it. */
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && !GetStdHandle(STD_ERROR_HANDLE)) {
        FILE *ignored;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen_s(&ignored, "CONOUT$", "w", stdout);
            freopen_s(&ignored, "CONOUT$", "w", stderr);
        } else hidden = 1;
    }
    SetConsoleOutputCP(CP_UTF8);
    return hidden;
}

void zr_host_alert(const char *title, const char *text) {
    MessageBoxA(NULL, text, title, MB_OK | MB_ICONERROR);
}

int zr_host_user_locale(char *out, size_t capacity) {
    wchar_t name[LOCALE_NAME_MAX_LENGTH];
    const int n = GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH);
    if (n <= 1 || (size_t)n > capacity) return 0;
    for (int i = 0; i < n; ++i) out[i] = name[i] < 128 ? (char)name[i] : '?';
    out[n - 1] = 0;
    return 1;
}

void zr_host_sleep_us(uint32_t us) {
    /* A high-resolution waitable timer (Windows 10 version 1803 and later) keeps its resolution when the
     * process's window is hidden or covered; older systems fall back to Sleep. */
    static HANDLE timer;
    static int tried;
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)us * 10;          /* 100 ns units, relative */
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((us + 999u) / 1000u);
}

int zr_host_program_folder(char *out, size_t capacity) {
    char path[ZR_HOST_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, sizeof path);
    if (!n || n >= sizeof path) return 0;
    char *slash = strrchr(path, '\\'), *forward = strrchr(path, '/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (!slash) return 0;
    slash[1] = 0;
    return snprintf(out, capacity, "%s", path) < (int)capacity;
}

/* --- files ------------------------------------------------------------------- */

char zr_host_separator(void) { return '\\'; }

/* The libretro frontend owns the process and can have a non-UTF-8 ANSI
 * code page. Keep paths UTF-8 across the host interface. */
static int wide_path(const char *path, WCHAR out[ZR_HOST_PATH]) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, ZR_HOST_PATH) != 0;
}

int zr_host_list_files(const char *folder, zr_host_file_fn fn, void *ctx) {
    char pattern[ZR_HOST_PATH];
    const size_t n = strlen(folder);
    const int separated = n && (folder[n - 1] == '\\' || folder[n - 1] == '/');
    if (snprintf(pattern, sizeof pattern, "%s%s*", folder, separated ? "" : "\\") >= (int)sizeof pattern) return 0;
    WCHAR wide[ZR_HOST_PATH];
    if (!wide_path(pattern, wide)) return 0;
    WIN32_FIND_DATAW found;
    HANDLE h = FindFirstFileW(wide, &found);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_DEVICE)) continue;
        char name[ZR_HOST_PATH];
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, found.cFileName, -1, name, sizeof name, NULL, NULL))
            continue;
        if (!fn(ctx, name)) break;
    } while (FindNextFileW(h, &found));
    FindClose(h);
    return 1;
}

int zr_host_list_folder(const char *folder, zr_host_entry_fn fn, void *ctx) {
    char pattern[ZR_HOST_PATH];
    const size_t n = strlen(folder);
    const int separated = n && (folder[n - 1] == '\\' || folder[n - 1] == '/');
    if (snprintf(pattern, sizeof pattern, "%s%s*", folder, separated ? "" : "\\") >= (int)sizeof pattern) return 0;
    WIN32_FIND_DATAA found;
    HANDLE h = FindFirstFileA(pattern, &found);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const int folder_entry = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const uint64_t size = folder_entry ? 0 : ((uint64_t)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        if (!fn(ctx, found.cFileName, folder_entry, size)) break;
    } while (FindNextFileA(h, &found));
    FindClose(h);
    return 1;
}

int zr_host_list_roots(zr_host_entry_fn fn, void *ctx) {
    const DWORD drives = GetLogicalDrives();
    int any = 0;
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        const char root[4] = {(char)('A' + i), ':', '\\', 0};
        any = 1;
        if (!fn(ctx, root, 1, 0)) break;
    }
    return any;
}

int zr_host_file_size(const char *path, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    WCHAR wide[ZR_HOST_PATH];
    if (!wide_path(path, wide) || !GetFileAttributesExW(wide, GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    return 1;
}

int zr_host_folder_exists(const char *path) {
    WCHAR wide[ZR_HOST_PATH];
    const DWORD a = wide_path(path, wide) ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int zr_host_full_path(const char *path, char *out, size_t capacity) {
    char full[ZR_HOST_PATH];
    const DWORD n = GetFullPathNameA(path, sizeof full, full, NULL);
    if (!n || n >= sizeof full) return 0;
    return snprintf(out, capacity, "%s", full) < (int)capacity;
}

int zr_host_make_folders(const char *path) {
    char p[ZR_HOST_PATH];
    if (snprintf(p, sizeof p, "%s", path) >= (int)sizeof p || !p[0]) return 0;
    /* Each folder on the way, from the top: the ones there already are left as they are. */
    WCHAR wide[ZR_HOST_PATH];
    for (char *c = p + 1; *c; ++c)
        if ((*c == '\\' || *c == '/') && c[-1] != ':' && c[-1] != '\\' && c[-1] != '/') {
            const char keep = *c;
            *c = 0;
            if (!zr_host_folder_exists(p) && wide_path(p, wide)) CreateDirectoryW(wide, NULL);
            *c = keep;
        }
    if (!zr_host_folder_exists(p) && wide_path(p, wide)) CreateDirectoryW(wide, NULL);
    return zr_host_folder_exists(p);
}

int zr_host_delete_file(const char *path) {
    WCHAR wide[ZR_HOST_PATH];
    if (!wide_path(path, wide)) return 0;
    return DeleteFileW(wide) != 0 || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}

int zr_host_local_time(zr_host_time *t) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    t->year = st.wYear;
    t->month = (uint8_t)st.wMonth;
    t->day = (uint8_t)st.wDay;
    t->hour = (uint8_t)st.wHour;
    t->minute = (uint8_t)st.wMinute;
    t->second = (uint8_t)st.wSecond;
    return 1;
}

int zr_host_replace_file(const char *temporary, const char *path) {
    WCHAR from[ZR_HOST_PATH + 16], to[ZR_HOST_PATH];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, temporary, -1, from, ZR_HOST_PATH + 16) || !wide_path(path, to)) return 0;
    return MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

int zr_host_same_folder(const char *a, const char *b) {
    char fa[ZR_HOST_PATH], fb[ZR_HOST_PATH];
    if (!_fullpath(fa, a, sizeof fa) || !_fullpath(fb, b, sizeof fb)) return !strcmp(a, b);
    for (char *s = fa; *s; ++s) if (*s == '/') *s = '\\';
    for (char *s = fb; *s; ++s) if (*s == '/') *s = '\\';
    size_t la = strlen(fa), lb = strlen(fb);
    if (la && fa[la - 1] == '\\') fa[--la] = 0;
    if (lb && fb[lb - 1] == '\\') fb[--lb] = 0;
    return !_stricmp(fa, fb);
}

/* --- byte streams ------------------------------------------------------------ */

/* TCP ends retry a connection this long after the last attempt. */
#define RETRY_MS 1000

struct zr_host_stream {
    int kind;
    HANDLE handle;
    int server;                 /* the pipe was created here (waits for a client) */
    int connected;              /* pipe: a client is (or the server was) there; TCP: a connection is up */
    SOCKET sock;                /* TCP: the connection, or the client accepted by a listener */
    SOCKET listener;            /* TCP_LISTEN: the listening socket */
    struct sockaddr_in peer;    /* TCP_CLIENT: where to connect */
    int connecting;             /* TCP_CLIENT: a connection attempt is under way */
    ULONGLONG retry_at;         /* TCP_CLIENT: when to try again */
    unsigned local_port;
};

static zr_host_stream *fail(zr_host_stream *s, char *error, size_t capacity, const char *what, const char *path) {
    snprintf(error, capacity, "%s %s (Windows error %lu)", what, path, (unsigned long)GetLastError());
    free(s);
    return NULL;
}

/* A server end waits for a client without blocking. */
static void listen_pipe(zr_host_stream *s) {
    if (ConnectNamedPipe(s->handle, NULL) || GetLastError() == ERROR_PIPE_CONNECTED) s->connected = 1;
}

/* --- TCP: non-blocking sockets polled by the caller's reads and writes ---------- */

static int net_users;

static int net_start(void) {
    if (!net_users) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data)) return 0;
    }
    net_users++;
    return 1;
}

static void net_stop(void) {
    if (net_users && !--net_users) WSACleanup();
}

static int is_tcp(const zr_host_stream *s) { return s->kind == ZR_STREAM_TCP_CLIENT || s->kind == ZR_STREAM_TCP_LISTEN; }

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

static void tcp_options(SOCKET sock) {
    u_long nonblocking = 1;
    BOOL nodelay = TRUE;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof nodelay);
}

/* The connection is gone: a client tries again in a second, a listener takes the next one. */
static void tcp_drop(zr_host_stream *s) {
    if (s->sock != INVALID_SOCKET) closesocket(s->sock);
    s->sock = INVALID_SOCKET;
    s->connected = 0;
    s->connecting = 0;
    s->retry_at = GetTickCount64() + RETRY_MS;
}

static void tcp_begin(zr_host_stream *s) {
    s->sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s->sock == INVALID_SOCKET) { s->retry_at = GetTickCount64() + RETRY_MS; return; }
    tcp_options(s->sock);
    if (connect(s->sock, (const struct sockaddr *)&s->peer, sizeof s->peer) == 0) s->connected = 1;
    else if (WSAGetLastError() == WSAEWOULDBLOCK) s->connecting = 1;
    else tcp_drop(s);
}

/* Moves a connection along: a listener accepts, a client finishes or starts its attempt. */
static void tcp_service(zr_host_stream *s) {
    if (s->kind == ZR_STREAM_TCP_LISTEN) {
        if (s->connected) return;
        const SOCKET client = accept(s->listener, NULL, NULL);
        if (client == INVALID_SOCKET) return;
        tcp_options(client);
        s->sock = client;
        s->connected = 1;
    } else if (s->connecting) {
        fd_set writable, failed;
        struct timeval none = {0, 0};
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s->sock, &writable);
        FD_SET(s->sock, &failed);
        if (select(0, NULL, &writable, &failed, &none) <= 0) return;
        if (FD_ISSET(s->sock, &failed) || !FD_ISSET(s->sock, &writable)) tcp_drop(s);
        else { s->connecting = 0; s->connected = 1; }
    } else if (!s->connected && GetTickCount64() >= s->retry_at) tcp_begin(s);
}

static zr_host_stream *tcp_open(zr_host_stream *s, const char *path, char *error, size_t capacity) {
    char host[256];
    unsigned port = 0;
    s->sock = s->listener = INVALID_SOCKET;
    if (!split_target(path, host, sizeof host, &port) || (s->kind == ZR_STREAM_TCP_CLIENT && (!*host || !port))) {
        snprintf(error, capacity, "Cannot use the TCP address %s", path);
        free(s);
        return NULL;
    }
    if (!net_start()) { snprintf(error, capacity, "Cannot start the network (Windows error %lu)", (unsigned long)GetLastError()); free(s); return NULL; }
    if (s->kind == ZR_STREAM_TCP_CLIENT) {
        if (!resolve_ipv4(host, port, &s->peer)) {
            snprintf(error, capacity, "Cannot find the address %s", path);
            net_stop();
            free(s);
            return NULL;
        }
        tcp_begin(s);
        return s;
    }
    if (!resolve_ipv4(*host ? host : "127.0.0.1", port, &s->peer)) {
        snprintf(error, capacity, "Cannot use the address %s", path);
        net_stop();
        free(s);
        return NULL;
    }
    s->listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL exclusive = TRUE;
    int length = (int)sizeof s->peer;
    if (s->listener == INVALID_SOCKET) goto failed;
    setsockopt(s->listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&exclusive, sizeof exclusive);
    if (bind(s->listener, (const struct sockaddr *)&s->peer, sizeof s->peer) || listen(s->listener, 1)) goto failed;
    tcp_options(s->listener);
    if (getsockname(s->listener, (struct sockaddr *)&s->peer, &length) == 0) s->local_port = ntohs(s->peer.sin_port);
    return s;
failed:
    snprintf(error, capacity, "Cannot listen on %s (Windows error %lu)", path, (unsigned long)WSAGetLastError());
    if (s->listener != INVALID_SOCKET) closesocket(s->listener);
    net_stop();
    free(s);
    return NULL;
}

zr_host_stream *zr_host_stream_open(int kind, const char *path, char *error, size_t capacity) {
    zr_host_stream *s = calloc(1, sizeof *s);
    if (!s) { snprintf(error, capacity, "Out of memory"); return NULL; }
    s->kind = kind;
    if (kind == ZR_STREAM_TCP_CLIENT || kind == ZR_STREAM_TCP_LISTEN) return tcp_open(s, path, error, capacity);
    if (kind == ZR_STREAM_SERIAL) {
        char name[64];
        snprintf(name, sizeof name, "\\\\.\\%s", path);
        HANDLE h = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) return fail(s, error, capacity, "Cannot open the serial port", path);
        /* Reads return what is there at once; writes give up after 100 ms. */
        COMMTIMEOUTS t = {MAXDWORD, 0, 0, 0, 100};
        SetCommTimeouts(h, &t);
        s->handle = h;
        return s;
    }
    /* A pipe someone else serves is joined; otherwise ZetRo serves it. */
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD mode = PIPE_READMODE_BYTE | PIPE_NOWAIT;
        SetNamedPipeHandleState(h, &mode, NULL, NULL);
        s->handle = h;
        s->connected = 1;
        return s;
    }
    h = CreateNamedPipeA(path, PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT, 1, 4096, 4096, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return fail(s, error, capacity, "Cannot open the pipe", path);
    s->handle = h;
    s->server = 1;
    listen_pipe(s);
    return s;
}

/* A pipe whose other end went away: a server listens again; a client is
 * done (the pipe is gone with its server). */
static void pipe_broken(zr_host_stream *s) {
    s->connected = 0;
    if (s->server) {
        DisconnectNamedPipe(s->handle);
        listen_pipe(s);
    }
}

long zr_host_stream_read(zr_host_stream *s, uint8_t *data, size_t capacity) {
    if (!s || !capacity) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return 0;
        const int got = recv(s->sock, (char *)data, capacity > 0x10000 ? 0x10000 : (int)capacity, 0);
        if (got > 0) return got;
        if (got < 0 && WSAGetLastError() == WSAEWOULDBLOCK) return 0;
        tcp_drop(s);                    /* closed by the other side, or broken */
        return -1;
    }
    DWORD avail = capacity > 0x10000 ? 0x10000 : (DWORD)capacity;
    if (s->kind == ZR_STREAM_PIPE) {
        if (!s->connected) {
            if (s->server) listen_pipe(s);
            if (!s->connected) return 0;
        }
        DWORD there = 0;
        if (!PeekNamedPipe(s->handle, NULL, 0, NULL, &there, NULL)) { pipe_broken(s); return -1; }
        if (!there) return 0;
        if (there < avail) avail = there;
    }
    DWORD got = 0;
    if (!ReadFile(s->handle, data, avail, &got, NULL)) {
        if (s->kind == ZR_STREAM_PIPE && GetLastError() != ERROR_NO_DATA) { pipe_broken(s); return -1; }
        return 0;
    }
    return (long)got;
}

long zr_host_stream_write(zr_host_stream *s, const uint8_t *data, size_t count) {
    if (!s || !count) return 0;
    if (is_tcp(s)) {
        tcp_service(s);
        if (!s->connected) return (long)count;          /* nobody listens: the bytes are gone */
        const int sent = send(s->sock, (const char *)data, count > 0x10000 ? 0x10000 : (int)count, 0);
        if (sent >= 0) return sent;
        if (WSAGetLastError() == WSAEWOULDBLOCK) return 0;
        tcp_drop(s);
        return -1;
    }
    if (s->kind == ZR_STREAM_PIPE && !s->connected) return (long)count;   /* nobody listens: the bytes are gone */
    DWORD wrote = 0;
    if (!WriteFile(s->handle, data, count > 0x10000 ? 0x10000 : (DWORD)count, &wrote, NULL)) {
        if (s->kind == ZR_STREAM_PIPE) { pipe_broken(s); return -1; }
        return 0;
    }
    return (long)wrote;
}

int zr_host_stream_modem(zr_host_stream *s, int *dcd, int *cts, int *dsr, int *ri) {
    if (!s) return 0;
    if (s->kind == ZR_STREAM_SERIAL) {
        DWORD st = 0;
        if (!GetCommModemStatus(s->handle, &st)) return 0;
        *dcd = (st & MS_RLSD_ON) != 0;
        *cts = (st & MS_CTS_ON) != 0;
        *dsr = (st & MS_DSR_ON) != 0;
        *ri = (st & MS_RING_ON) != 0;
        return 1;
    }
    if (is_tcp(s)) tcp_service(s);
    *dcd = *cts = *dsr = s->connected;
    *ri = 0;
    return 1;
}

void zr_host_stream_outputs(zr_host_stream *s, int dtr, int rts) {
    if (!s || s->kind != ZR_STREAM_SERIAL) return;
    EscapeCommFunction(s->handle, dtr ? SETDTR : CLRDTR);
    EscapeCommFunction(s->handle, rts ? SETRTS : CLRRTS);
}

unsigned zr_host_stream_local_port(const zr_host_stream *s) { return s ? s->local_port : 0; }

void zr_host_stream_close(zr_host_stream *s) {
    if (!s) return;
    if (is_tcp(s)) {
        if (s->sock != INVALID_SOCKET) closesocket(s->sock);
        if (s->listener != INVALID_SOCKET) closesocket(s->listener);
        net_stop();
    } else if (s->handle && s->handle != INVALID_HANDLE_VALUE) CloseHandle(s->handle);
    free(s);
}

/* COM3 before COM10: shorter names first. */
static int name_order(const void *a, const void *b) {
    const char *x = a, *y = b;
    const size_t lx = strlen(x), ly = strlen(y);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x, y);
}

unsigned zr_host_list_serial(zr_host_name_fn fn, void *ctx) {
    enum { MAX_PORTS = 64, NAME = 32 };
    static char names[MAX_PORTS][NAME];
    HKEY key;
    unsigned count = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) return 0;
    for (DWORD i = 0; count < MAX_PORTS; ++i) {
        char value[128], data[NAME];
        DWORD value_size = sizeof value, data_size = sizeof data - 1, type = 0;
        const LONG rc = RegEnumValueA(key, i, value, &value_size, NULL, &type, (BYTE *)data, &data_size);
        if (rc == ERROR_NO_MORE_ITEMS) break;
        if (rc != ERROR_SUCCESS || type != REG_SZ) continue;
        data[data_size < sizeof data ? data_size : sizeof data - 1] = 0;
        if (strncmp(data, "COM", 3)) continue;
        memcpy(names[count++], data, sizeof data);
    }
    RegCloseKey(key);
    qsort(names, count, NAME, name_order);
    for (unsigned i = 0; i < count; ++i)
        if (fn && !fn(ctx, names[i])) break;
    return count;
}

/* --- whole files (disk images) ------------------------------------------------ */

/* The system's words for the last error of a file call. */
static void file_error(char *error, size_t capacity, const char *what, const char *path) {
    if (!error || !capacity) return;
    char text[160] = "";
    const DWORD code = GetLastError();
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, text, sizeof text, NULL);
    while (n && (text[n - 1] == '\r' || text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '.')) text[--n] = 0;
    snprintf(error, capacity, "%s %s (%s)", what, path, n ? text : "unknown error");
}

int zr_host_file_exists(const char *path) {
    WCHAR wide[ZR_HOST_PATH];
    return wide_path(path, wide) && GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
}

int zr_host_read_file(const char *path, size_t limit, uint8_t **data, size_t *size, int *writable, char *error, size_t capacity) {
    *data = NULL;
    *size = 0;
    WCHAR wide[ZR_HOST_PATH];
    if (!wide_path(path, wide)) { file_error(error, capacity, "Cannot read", path); return 0; }
    const DWORD attributes = GetFileAttributesW(wide);
    if (writable) *writable = attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_READONLY);
    HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { file_error(error, capacity, "Cannot read", path); return 0; }
    LARGE_INTEGER length;
    if (!GetFileSizeEx(h, &length)) { file_error(error, capacity, "Cannot read", path); CloseHandle(h); return 0; }
    if (length.QuadPart < 0 || (unsigned long long)length.QuadPart > limit) {
        if (error && capacity) snprintf(error, capacity, "%s is too large", path);
        CloseHandle(h);
        return 0;
    }
    const size_t n = (size_t)length.QuadPart;
    uint8_t *buffer = malloc(n ? n : 1);
    if (!buffer) { if (error && capacity) snprintf(error, capacity, "Out of memory"); CloseHandle(h); return 0; }
    size_t done = 0;
    while (done < n) {
        DWORD got = 0;
        const DWORD want = (DWORD)(n - done > 0x100000u ? 0x100000u : n - done);
        if (!ReadFile(h, buffer + done, want, &got, NULL) || !got) {
            file_error(error, capacity, "Cannot read", path);
            free(buffer);
            CloseHandle(h);
            return 0;
        }
        done += got;
    }
    CloseHandle(h);
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
    WCHAR wide[ZR_HOST_PATH + 16];
    HANDLE h = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, temporary, -1, wide, ZR_HOST_PATH + 16)
        ? CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) { file_error(error, capacity, "Cannot write", path); return 0; }
    size_t done = 0;
    int ok = 1;
    while (ok && done < size) {
        DWORD put = 0;
        const DWORD want = (DWORD)(size - done > 0x100000u ? 0x100000u : size - done);
        ok = WriteFile(h, data + done, want, &put, NULL) && put;
        done += put;
    }
    if (ok) ok = FlushFileBuffers(h);
    if (!ok) file_error(error, capacity, "Cannot write", path);
    if (!CloseHandle(h) && ok) { ok = 0; file_error(error, capacity, "Cannot write", path); }
    if (ok && !zr_host_replace_file(temporary, path)) { ok = 0; file_error(error, capacity, "Cannot replace", path); }
    if (!ok) DeleteFileW(wide);
    return ok;
}

/* --- files read and written in place ---------------------------------------- */

struct zr_host_file {
    unsigned references;
    HANDLE h;
    uint64_t length;
};

zr_host_file *zr_host_file_open(const char *path, int *writable, char *error, size_t capacity) {
    WCHAR wide[ZR_HOST_PATH];
    if (!wide_path(path, wide)) { file_error(error, capacity, "Cannot read", path); return NULL; }
    HANDLE h = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    int can_write = h != INVALID_HANDLE_VALUE;
    if (!can_write) h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { file_error(error, capacity, "Cannot read", path); return NULL; }
    LARGE_INTEGER length;
    if (!GetFileSizeEx(h, &length) || length.QuadPart < 0) { file_error(error, capacity, "Cannot read", path); CloseHandle(h); return NULL; }
    zr_host_file *f = malloc(sizeof *f);
    if (!f) { if (error && capacity) snprintf(error, capacity, "Out of memory"); CloseHandle(h); return NULL; }
    f->h = h;
    f->references = 1;
    f->length = (uint64_t)length.QuadPart;
    if (writable) *writable = can_write;
    return f;
}

int zr_host_file_create(const char *path, uint64_t size, int overwrite, char *error, size_t capacity) {
    WCHAR wide[ZR_HOST_PATH];
    HANDLE h = wide_path(path, wide)
        ? CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL, overwrite ? CREATE_ALWAYS : CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)
        : INVALID_HANDLE_VALUE;
    if (h == INVALID_HANDLE_VALUE) { file_error(error, capacity, "Cannot create", path); return 0; }
    DWORD returned = 0;
    DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL);      /* where the file system allows it */
    LARGE_INTEGER end;
    end.QuadPart = (LONGLONG)size;
    int ok = SetFilePointerEx(h, end, NULL, FILE_BEGIN) && SetEndOfFile(h) && FlushFileBuffers(h);
    if (!ok) file_error(error, capacity, "Cannot create", path);
    CloseHandle(h);
    if (!ok) DeleteFileW(wide);
    return ok;
}

static int file_move(zr_host_file *f, uint64_t offset, void *data, size_t length, int write) {
    uint8_t *p = data;
    while (length) {
        OVERLAPPED at;
        memset(&at, 0, sizeof at);
        at.Offset = (DWORD)offset;
        at.OffsetHigh = (DWORD)(offset >> 32);
        const DWORD want = (DWORD)(length > 0x100000u ? 0x100000u : length);
        DWORD moved = 0;
        const BOOL ok = write ? WriteFile(f->h, p, want, &moved, &at) : ReadFile(f->h, p, want, &moved, &at);
        if (!ok || !moved) return 0;
        p += moved;
        offset += moved;
        length -= moved;
    }
    return 1;
}

int zr_host_file_read_at(zr_host_file *f, uint64_t offset, void *data, size_t length) { return file_move(f, offset, data, length, 0); }
int zr_host_file_write_at(zr_host_file *f, uint64_t offset, const void *data, size_t length) {
    if (!file_move(f, offset, (void *)data, length, 1)) return 0;
    if (offset + length > f->length) f->length = offset + length;
    return 1;
}
int zr_host_file_flush(zr_host_file *f) { return FlushFileBuffers(f->h) != 0; }
uint64_t zr_host_file_length(const zr_host_file *f) { return f->length; }
zr_host_file *zr_host_file_retain(zr_host_file *f) {
    ++f->references;
    return f;
}

void zr_host_file_close(zr_host_file *f) {
    if (!f || --f->references) return;
    CloseHandle(f->h);
    free(f);
}
