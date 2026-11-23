/* The CPU conformance runner: the SingleStepTests Z80 suite as
 * tests/cpu_suite.py converts it (its header gives the format).
 *
 *   cpu_suite FOLDER [--only NAME] [--show N] [--no-cycles]
 *   cpu_suite --com FILE.com
 *
 * Every test sets the registers and memory, runs one instruction and
 * compares the registers, the memory and, T-state by T-state, the address,
 * the data and the control pins of the bus. --only runs one file ("dd cb __
 * 46"), --show prints the first N failures in full, --no-cycles leaves the
 * bus out. --com runs a CP/M program instead, to its end (the instruction
 * exercisers ZEXDOC and ZEXALL). Exit status 0 when every test passes. Not a ctest test: the
 * suite is downloaded on request. ZetRo's own code. */
#include "z80.h"
#include "../src/host/host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PIN_RD = 1, PIN_WR = 2, PIN_MREQ = 4, PIN_IORQ = 8, PIN_NO_DATA = 0x80 };
#define MAX_CYCLES 64

typedef struct cycle { uint16_t address; uint8_t data, pins; } cycle;
typedef struct state {
    uint16_t pc, sp, wz, ix, iy, af_, bc_, de_, hl_;
    uint8_t a, b, c, d, e, f, h, l, i, r, ei, im, p, q, iff1, iff2;
    uint16_t ram_count;
    const uint8_t *ram;                 /* ram_count x (u16 address, u8 byte) */
} state;

static uint8_t memory[65536];
static zr_z80 cpu;
static cycle trace[MAX_CYCLES];
static unsigned trace_count, fetch_at, read_at;
static const uint8_t *ports;
static unsigned port_count, port_next;

/* --- the bus: memory, the test's port values, and the pins of every T-state ------------------- */

static void pin(uint16_t address, uint8_t pins) {
    if (trace_count < MAX_CYCLES) { trace[trace_count].address = address; trace[trace_count].data = 0; trace[trace_count].pins = pins | PIN_NO_DATA; }
    ++trace_count;
}
static void data_at(unsigned index, uint8_t value) {
    if (index < MAX_CYCLES) { trace[index].data = value; trace[index].pins &= (uint8_t)~PIN_NO_DATA; }
}

static unsigned bus_stretch(void *ctx, int kind, uint16_t address) {
    (void)ctx;
    switch (kind) {
    case ZR_Z80_FETCH: {
        const uint16_t refresh = (uint16_t)(cpu.i << 8 | cpu.r);
        pin(address, 0); pin(address, PIN_RD | PIN_MREQ);
        fetch_at = trace_count;
        pin(refresh, 0); pin(refresh, 0);
        break;
    }
    case ZR_Z80_READ:
        pin(address, 0); pin(address, PIN_RD | PIN_MREQ);
        read_at = trace_count;
        pin(address, 0);
        break;
    case ZR_Z80_WRITE:
        pin(address, 0);
        read_at = trace_count;
        pin(address, PIN_WR | PIN_MREQ); pin(address, 0);
        break;
    case ZR_Z80_IN:
        pin(address, 0); pin(address, 0); pin(address, PIN_RD | PIN_IORQ);
        read_at = trace_count;
        pin(address, 0);
        break;
    case ZR_Z80_OUT:
        pin(address, 0); pin(address, 0);
        read_at = trace_count;
        pin(address, PIN_WR | PIN_IORQ); pin(address, 0);
        break;
    default:
        pin(address, 0);
        break;
    }
    return 0;
}
static uint8_t bus_read(void *ctx, uint16_t address, int m1) {
    (void)ctx;
    data_at(m1 ? fetch_at : read_at, memory[address]);
    return memory[address];
}
static void bus_write(void *ctx, uint16_t address, uint8_t value) {
    (void)ctx;
    data_at(read_at, value);
    memory[address] = value;
}
static uint8_t bus_in(void *ctx, uint16_t port) {
    (void)ctx; (void)port;
    const uint8_t value = port_next < port_count ? ports[port_next * 4 + 2] : 0xFF;
    ++port_next;
    data_at(read_at, value);
    return value;
}
static void bus_out(void *ctx, uint16_t port, uint8_t value) {
    (void)ctx; (void)port;
    data_at(read_at, value);
}
static const zr_z80_bus BUS = {NULL, bus_read, bus_write, bus_in, bus_out, NULL, bus_stretch};

/* --- a test ----------------------------------------------------------------------------- */

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static const uint8_t *read_state(const uint8_t *p, state *s) {
    uint16_t *const words[9] = {&s->pc, &s->sp, &s->wz, &s->ix, &s->iy, &s->af_, &s->bc_, &s->de_, &s->hl_};
    uint8_t *const bytes[16] = {&s->a, &s->b, &s->c, &s->d, &s->e, &s->f, &s->h, &s->l, &s->i, &s->r, &s->ei, &s->im, &s->p, &s->q, &s->iff1, &s->iff2};
    for (int i = 0; i < 9; ++i, p += 2) *words[i] = get16(p);
    for (int i = 0; i < 16; ++i) *bytes[i] = *p++;
    s->ram_count = get16(p);
    s->ram = p + 2;
    return s->ram + (size_t)s->ram_count * 3u;
}

static void load(const state *s) {
    cpu.pc = s->pc; cpu.sp = s->sp; cpu.wz = s->wz;
    cpu.ixh = (uint8_t)(s->ix >> 8); cpu.ixl = (uint8_t)s->ix; cpu.iyh = (uint8_t)(s->iy >> 8); cpu.iyl = (uint8_t)s->iy;
    cpu.a_ = (uint8_t)(s->af_ >> 8); cpu.f_ = (uint8_t)s->af_; cpu.b_ = (uint8_t)(s->bc_ >> 8); cpu.c_ = (uint8_t)s->bc_;
    cpu.d_ = (uint8_t)(s->de_ >> 8); cpu.e_ = (uint8_t)s->de_; cpu.h_ = (uint8_t)(s->hl_ >> 8); cpu.l_ = (uint8_t)s->hl_;
    cpu.a = s->a; cpu.b = s->b; cpu.c = s->c; cpu.d = s->d; cpu.e = s->e; cpu.f = s->f; cpu.h = s->h; cpu.l = s->l;
    cpu.i = s->i; cpu.r = s->r; cpu.ei = s->ei; cpu.im = s->im; cpu.p = s->p; cpu.q = s->q; cpu.iff1 = s->iff1; cpu.iff2 = s->iff2;
    cpu.halted = 0; cpu.nmi = 0; cpu.irq = 0;
    for (unsigned i = 0; i < s->ram_count; ++i) memory[get16(s->ram + i * 3u)] = s->ram[i * 3u + 2];
}

/* What differs from the expected final state, as text ("" when nothing does). */
static void compare(const state *s, char *out, size_t capacity) {
    size_t n = 0;
    out[0] = 0;
#define DIFF(name, have, want) if ((have) != (want) && n < capacity) n += (size_t)snprintf(out + n, capacity - n, " %s=%X(want %X)", name, (unsigned)(have), (unsigned)(want))
    DIFF("pc", cpu.pc, s->pc); DIFF("sp", cpu.sp, s->sp); DIFF("wz", cpu.wz, s->wz);
    DIFF("ix", zr_z80_ix(&cpu), s->ix); DIFF("iy", zr_z80_iy(&cpu), s->iy);
    DIFF("af'", (uint16_t)(cpu.a_ << 8 | cpu.f_), s->af_); DIFF("bc'", (uint16_t)(cpu.b_ << 8 | cpu.c_), s->bc_);
    DIFF("de'", (uint16_t)(cpu.d_ << 8 | cpu.e_), s->de_); DIFF("hl'", (uint16_t)(cpu.h_ << 8 | cpu.l_), s->hl_);
    DIFF("a", cpu.a, s->a); DIFF("f", cpu.f, s->f); DIFF("b", cpu.b, s->b); DIFF("c", cpu.c, s->c);
    DIFF("d", cpu.d, s->d); DIFF("e", cpu.e, s->e); DIFF("h", cpu.h, s->h); DIFF("l", cpu.l, s->l);
    DIFF("i", cpu.i, s->i); DIFF("r", cpu.r, s->r); DIFF("ei", cpu.ei, s->ei); DIFF("im", cpu.im, s->im);
    DIFF("p", cpu.p, s->p); DIFF("q", cpu.q, s->q); DIFF("iff1", cpu.iff1, s->iff1); DIFF("iff2", cpu.iff2, s->iff2);
    for (unsigned i = 0; i < s->ram_count; ++i) {
        const uint16_t address = get16(s->ram + i * 3u);
        if (memory[address] != s->ram[i * 3u + 2] && n < capacity)
            n += (size_t)snprintf(out + n, capacity - n, " [%04X]=%02X(want %02X)", address, memory[address], s->ram[i * 3u + 2]);
    }
#undef DIFF
}

/* The first T-state whose pins differ; -1 when the bus matches. */
static int compare_cycles(const uint8_t *want, unsigned count) {
    for (unsigned i = 0; i < count || i < trace_count; ++i) {
        if (i >= count || i >= trace_count || i >= MAX_CYCLES) return (int)i;
        const uint16_t address = get16(want + i * 4u);
        const uint8_t data = want[i * 4u + 2], pins = want[i * 4u + 3];
        if (address != trace[i].address || (pins & 15u) != (trace[i].pins & 15u)) return (int)i;
        if (!(pins & PIN_NO_DATA) && ((trace[i].pins & PIN_NO_DATA) || trace[i].data != data)) return (int)i;
    }
    return -1;
}

static void print_cycles(const uint8_t *want, unsigned count) {
    static const char *const NAMES[16] = {"----", "r---", "-w--", "rw--", "--m-", "r-m-", "-wm-", "rwm-", "---i", "r--i", "-w-i", "rw-i", "--mi", "r-mi", "-wmi", "rwmi"};
    for (unsigned i = 0; i < count || i < trace_count; ++i) {
        printf("      T%-2u", i + 1);
        if (i < count) {
            printf(" want %04X ", get16(want + i * 4u));
            if (want[i * 4u + 3] & PIN_NO_DATA) printf("--"); else printf("%02X", want[i * 4u + 2]);
            printf(" %s", NAMES[want[i * 4u + 3] & 15u]);
        } else printf(" want              ");
        if (i < trace_count && i < MAX_CYCLES) {
            printf("   have %04X ", trace[i].address);
            if (trace[i].pins & PIN_NO_DATA) printf("--"); else printf("%02X", trace[i].data);
            printf(" %s", NAMES[trace[i].pins & 15u]);
        }
        printf("\n");
    }
}

/* --- the files -------------------------------------------------------------------------- */

static unsigned long total, bad_state, bad_cycles, files, bad_files;
static unsigned show;
static int with_cycles = 1;

static void run_file(const char *folder, const char *name) {
    char path[1200], diff[1024];
    uint8_t *data = NULL;
    size_t size = 0;
    int writable = 0;
    snprintf(path, sizeof path, "%s/%s", folder, name);
    if (!zr_host_read_file(path, 256u * 1024u * 1024u, &data, &size, &writable, diff, sizeof diff)) { printf("%s: %s\n", path, diff); ++bad_files; return; }
    if (size < 8 || memcmp(data, "ZRS1", 4)) { printf("%s: not a converted suite file\n", path); free(data); ++bad_files; return; }
    const unsigned count = data[4] | data[5] << 8 | (unsigned)data[6] << 16 | (unsigned)data[7] << 24;
    const uint8_t *p = data + 8;
    unsigned failed_state = 0, failed_cycles = 0;
    for (unsigned t = 0; t < count; ++t) {
        state initial, final;
        const unsigned name_length = *p++;
        const char *test_name = (const char *)p;
        p += name_length;
        p = read_state(p, &initial);
        p = read_state(p, &final);
        const unsigned cycle_count = get16(p);
        const uint8_t *cycles = p + 2;
        p = cycles + (size_t)cycle_count * 4u;
        port_count = *p++;
        ports = p;
        p += (size_t)port_count * 4u;
        port_next = 0;
        trace_count = 0;
        load(&initial);
        zr_z80_step(&cpu);
        compare(&final, diff, sizeof diff);
        const int cycle_diff = with_cycles ? compare_cycles(cycles, cycle_count) : -1;
        failed_state += diff[0] != 0;
        failed_cycles += cycle_diff >= 0;
        if ((diff[0] || cycle_diff >= 0) && show) {
            --show;
            printf("  %.*s:%s%s\n", (int)name_length, test_name, diff[0] ? diff : " state ok", cycle_diff >= 0 ? "; bus differs" : "");
            if (cycle_diff >= 0) print_cycles(cycles, cycle_count);
        }
    }
    free(data);
    ++files;
    total += count;
    bad_state += failed_state;
    bad_cycles += failed_cycles;
    if (failed_state || failed_cycles) {
        ++bad_files;
        printf("%-22s %4u state, %4u bus of %u\n", name, failed_state, failed_cycles, count);
    }
}

static char names[2048][40];
static unsigned name_count;
static int add_name(void *ctx, const char *name) {
    const size_t n = strlen(name);
    (void)ctx;
    if (n > 4 && n < sizeof names[0] && !strcmp(name + n - 4, ".bin") && name_count < 2048) snprintf(names[name_count++], sizeof names[0], "%s", name);
    return 1;
}
static int by_name(const void *a, const void *b) { return strcmp(a, b); }

/* A CP/M program (the instruction exercisers ZEXDOC and ZEXALL) loaded at 0100h and run to its end: a call to
 * 0005h is CP/M's BDOS, of which the programs use two functions, the printing of a character (C = 2, in E) and
 * of a string that ends in '$' (C = 9, at DE); a jump to 0000h ends the program. 0 when its output has no
 * "ERROR" in it. */
static int run_com(const char *path) {
    uint8_t *data = NULL;
    size_t size = 0;
    int writable = 0, errors = 0;
    char why[256], tail[8] = "";
    if (!zr_host_read_file(path, 0xFE00, &data, &size, &writable, why, sizeof why)) { printf("%s: %s\n", path, why); return 2; }
    memset(memory, 0, sizeof memory);
    memcpy(memory + 0x100, data, size);
    free(data);
    memory[5] = 0xC9;                                           /* BDOS returns */
    zr_z80_init(&cpu, &BUS, NULL, 0);
    cpu.pc = 0x100;
    cpu.sp = 0xF000;
    while (cpu.pc != 0) {
        if (cpu.pc == 5) {
            const char *text = cpu.c == 9 ? (const char *)memory + zr_z80_de(&cpu) : (const char *)&cpu.e;
            for (size_t i = 0; cpu.c == 9 ? text[i] != '$' : i < (cpu.c == 2); ++i) {
                putchar(text[i]);
                memmove(tail, tail + 1, 4);
                tail[4] = text[i];
                errors += !memcmp(tail, "ERROR", 5);
            }
            fflush(stdout);
        }
        trace_count = 0;
        zr_z80_step(&cpu);
    }
    printf("\n%s: %llu T-states, %s\n", path, (unsigned long long)cpu.t, errors ? "ERRORS" : "no errors");
    return errors ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *folder = NULL, *only = NULL;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--com") && i + 1 < argc) return run_com(argv[++i]);
        else if (!strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!strcmp(argv[i], "--show") && i + 1 < argc) show = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--no-cycles")) with_cycles = 0;
        else if (!folder) folder = argv[i];
        else { printf("cpu_suite FOLDER [--only NAME] [--show N] [--no-cycles]\n"); return 2; }
    }
    if (!folder) { printf("cpu_suite FOLDER [--only NAME] [--show N] [--no-cycles]\n"); return 2; }
    zr_z80_init(&cpu, &BUS, NULL, 0);
    if (only) {
        char name[64];
        snprintf(name, sizeof name, "%s.bin", only);
        run_file(folder, name);
    } else {
        if (!zr_host_list_files(folder, add_name, NULL) || !name_count) { printf("%s: no converted suite (python tests/cpu_suite.py fetch)\n", folder); return 2; }
        qsort(names, name_count, sizeof names[0], by_name);
        for (unsigned i = 0; i < name_count; ++i) run_file(folder, names[i]);
    }
    printf("%lu tests in %lu files: %lu state failures, %lu bus failures, %lu files with failures\n", total, files, bad_state, bad_cycles, bad_files);
    return bad_state || bad_cycles || bad_files ? 1 : 0;
}
