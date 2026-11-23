/* The Z80's smoke test: what the conformance suite (cpu_suite.c) does not
 * run, an instruction at a time on a 64 KiB memory: the interrupt
 * responses and their lengths, the delay after EI, HALT, the NMOS parity
 * fault, prefix chains, the empty ED codes, the CMOS differences, the
 * hooks and the state. */
#include "z80.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

static uint8_t memory[65536], port_value, ack_byte = 0xFF;
static zr_z80 cpu;

static uint8_t bus_read(void *ctx, uint16_t address, int m1) { (void)ctx; (void)m1; return memory[address]; }
static void bus_write(void *ctx, uint16_t address, uint8_t value) { (void)ctx; memory[address] = value; }
static uint8_t bus_in(void *ctx, uint16_t port) { (void)ctx; (void)port; return 0xFF; }
static void bus_out(void *ctx, uint16_t port, uint8_t value) { (void)ctx; (void)port; port_value = value; }
static uint8_t bus_ack(void *ctx) { (void)ctx; return ack_byte; }
static const zr_z80_bus BUS = {NULL, bus_read, bus_write, bus_in, bus_out, bus_ack, NULL};

/* A fresh processor with a program at 0000h. */
static void start(int cmos, const uint8_t *program, size_t length) {
    memset(memory, 0, sizeof memory);
    memcpy(memory, program, length);
    zr_z80_init(&cpu, &BUS, NULL, cmos);
    cpu.sp = 0x8000;
}
#define START(cmos, ...) do { static const uint8_t program[] = {__VA_ARGS__}; start(cmos, program, sizeof program); } while (0)

static unsigned before, after, refreshes;
static int hook_before(void *ctx, uint16_t pc) { (void)ctx; ++before; if (pc == 2) { cpu.pc = 0x10; return 1; } return 0; }
static void hook_after(void *ctx, uint16_t pc, uint8_t opcode) { (void)ctx; (void)pc; after += opcode == 0x3C; }
static void hook_refresh(void *ctx, uint16_t address) { (void)ctx; (void)address; ++refreshes; }

int main(void) {
    /* EI, then two NOPs, with INT held low in mode 1: the instruction after EI still runs. */
    START(0, 0xFB, 0x00, 0x00);
    zr_z80_int(&cpu, 1);
    CHECK(zr_z80_step(&cpu) == 4 && cpu.iff1);
    CHECK(zr_z80_step(&cpu) == 4 && cpu.pc == 2);
    cpu.im = 1;
    CHECK(zr_z80_step(&cpu) == 13 && cpu.pc == 0x38 && !cpu.iff1 && !cpu.iff2 && cpu.sp == 0x7FFE && memory[0x7FFE] == 2);

    /* Mode 2: the vector is read at I and the byte the device gives. */
    START(0, 0x00);
    cpu.iff1 = cpu.iff2 = 1; cpu.im = 2; cpu.i = 0x40; ack_byte = 0x10;
    memory[0x4010] = 0x34; memory[0x4011] = 0x12;
    zr_z80_int(&cpu, 1);
    CHECK(zr_z80_step(&cpu) == 19 && cpu.pc == 0x1234 && cpu.wz == 0x1234);
    /* Mode 0 with an RST on the bus. */
    START(0, 0x00);
    cpu.iff1 = 1; ack_byte = 0xD7;
    zr_z80_int(&cpu, 1);
    CHECK(zr_z80_step(&cpu) == 13 && cpu.pc == 0x10);
    ack_byte = 0xFF;

    /* HALT fetches without going on; an NMI wakes it, keeps IFF2 and returns after the HALT. */
    START(0, 0x76, 0x00);
    cpu.iff1 = cpu.iff2 = 1;
    CHECK(zr_z80_step(&cpu) == 4 && cpu.halted && cpu.pc == 1);
    CHECK(zr_z80_step(&cpu) == 4 && cpu.pc == 1 && cpu.r == 2);
    zr_z80_nmi(&cpu);
    CHECK(zr_z80_step(&cpu) == 11 && cpu.pc == 0x66 && !cpu.halted && !cpu.iff1 && cpu.iff2 && memory[0x7FFE] == 1);

    /* LD A,I with IFF2 set, interrupted at once: the NMOS processor loses P, the CMOS one keeps it. */
    for (int cmos = 0; cmos < 2; ++cmos) {
        START(cmos, 0xED, 0x57);
        cpu.iff1 = cpu.iff2 = 1; cpu.im = 1;
        zr_z80_step(&cpu);
        CHECK(cpu.f & ZR_Z80_P);
        zr_z80_int(&cpu, 1);
        zr_z80_step(&cpu);
        CHECK(cpu.pc == 0x38 && !(cpu.f & ZR_Z80_P) == !cmos);
    }

    /* A chain of prefixes: the last one counts (LD IY,1234h); an empty ED code is two fetches. */
    START(0, 0xDD, 0xFD, 0x21, 0x34, 0x12, 0xED, 0x00, 0xDD, 0xED, 0x6A);
    CHECK(zr_z80_step(&cpu) == 18 && zr_z80_iy(&cpu) == 0x1234 && zr_z80_ix(&cpu) == 0 && cpu.r == 3);
    CHECK(zr_z80_step(&cpu) == 8 && cpu.pc == 7);
    /* A prefix before ED does nothing: ADC HL,HL, not IX. */
    cpu.h = 0x40; cpu.l = 0; cpu.f = 0;
    CHECK(zr_z80_step(&cpu) == 19 && zr_z80_hl(&cpu) == 0x8000 && zr_z80_ix(&cpu) == 0);

    /* OUT (C),0: 00h from the NMOS processor, FFh from the CMOS one. */
    for (int cmos = 0; cmos < 2; ++cmos) {
        START(cmos, 0xED, 0x71);
        port_value = 0x55;
        CHECK(zr_z80_step(&cpu) == 12 && port_value == (cmos ? 0xFF : 0x00));
    }

    /* The hooks: only at watched addresses; before_fetch can take the fetch over; a refresh at every fetch. */
    START(0, 0x3C, 0x3C, 0x3C);
    static uint8_t watch[8192];
    watch[0] = 0x05;                                            /* addresses 0 and 2 */
    static const zr_cpu_hooks HOOKS = {NULL, watch, hook_before, hook_after, hook_refresh};
    cpu.hooks = &HOOKS;
    zr_z80_step(&cpu);
    zr_z80_step(&cpu);
    CHECK(zr_z80_step(&cpu) == 0 && cpu.pc == 0x10);
    CHECK(before == 2 && after == 1 && refreshes == 2 && cpu.a == 1);

    /* The state: saved, the processor run on, loaded. */
    START(0, 0x3C, 0x3C);
    zr_z80_step(&cpu);
    zr_state_writer w;
    zr_sw_init(&w);
    zr_sw_header(&w, "test", "test", NULL, 0);
    zr_sw_begin(&w, "Z80 ", 0);
    zr_z80_save(&cpu, &w);
    zr_sw_end(&w);
    CHECK(zr_sw_finish(&w));
    zr_z80_step(&cpu);
    zr_state_reader r;
    char error[64];
    CHECK(zr_sr_open(&r, w.data, w.size, error, sizeof error) && zr_sr_chunk(&r, "Z80 ", 0));
    zr_z80_load(&cpu, &r);
    CHECK(zr_sr_ok(&r) && cpu.a == 0 && cpu.pc == 1 && cpu.t == 4 && cpu.r == 1);
    zr_sw_free(&w);

    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
