/* The ROM tape traps (traps.h). ZetRo's own code, written from the disassembly of the JET ROM's tape
 * routines; the addresses in the comments are the JET ROM's (the routines are byte for byte the same
 * in every ROM that has them, and the trap is given none of the addresses: it finds them).
 *
 * The loader, 0556h, as the code reads:
 *   0556 INC D / EX AF,AF' / DEC D / DI: the entry A and the carry are put in the alternate pair, with Z set
 *        when D was FFh (the loader then takes its first byte for data, not for the flag: no trap for it); the
 *        interrupts are off. Then the border, `LD HL,053Fh / PUSH HL` (every RET of the routine returns to the
 *        exit at 053Fh, which writes the border from BORDCR, tests SPACE, EI, and returns to the caller), and
 *        the search for the leader tone and the sync pulses.
 *   05A3 H := 0, L := the byte read, H := H xor L for every byte read, the flag byte and the parity byte
 *        included. A byte is read by 05C8..05DD with B left at B0h whatever the bit lengths were.
 *   05A9 LD-LOOP: swaps the alternate pair in. The first time Z is clear: the byte just read is the flag
 *        byte, tested against A (xor; RET NZ: A = A xor L, the flags of the xor, carry clear: failure), C is
 *        rotated through the carry (RL C, then RRA: C loses its bit 7) and the carry of LOAD/VERIFY
 *        travels in the alternate F from then on, with Z set. Each later byte: LOAD stores it at (IX),
 *        VERIFY compares it with (IX) (xor L; RET NZ: A = (IX) xor L, failure); IX is incremented and DE
 *        decremented after the store, the next byte is read, and the loop goes on while DE is not 0.
 *        DE counts the data bytes: the flag byte leaves it alone (INC DE ... DEC DE), the byte read with
 *        DE already 0 is the parity byte and is not stored.
 *   05DF LD A,H / CP 01h / RET: success when H (the xor of every byte) is 0, as the carry of CP says; A = H.
 *   When the next edge does not come (the block is over: 05E7's counter wraps, AND A, INC B, RET Z) the
 *        routine returns with A = 0, F = Z|H (INC B from FFh), B = 0, carry clear: failure; the byte that
 *        was being read has L = 01h (LD L,01h at 05C8).
 * The saver, 04C2h: flag byte, then the DE bytes at IX, then H (the parity), 0 or 1 bits as 855 or 1710 T-state
 *   pulses; INC DE / DEC IX at the entry and DEC DE / INC IX after every byte (flag included) make the flag
 *   leave both alone; after the parity byte (sent with DE = 0) DE is FFFFh and the routine ends with LD A,D /
 *   INC A (Z set, A = 0, H set, the carry left by the BREAK test, which is set), LD B,3Bh / DJNZ (B = 0) and
 *   RET; C is 0Eh from 04F6; H and L are 0 (H := H xor L with L = H, then L is shifted out); the alternate pair
 *   holds A = 0Dh (04EE) and the flags of the last DEC H of the leader (04E4: FFh, so S, X, Y, H and N set).
 * The tape routines test BREAK as the carry of a read of port FEh with A = 7Fh (SPACE alone), in the loader's
 *   edge routine and at the exit and the saver after every byte: with SPACE down the trap leaves the ROM's own
 *   routine to run, which fails at once and reports BREAK at its exit. */
#include "traps.h"

#include <stdlib.h>
#include <string.h>

/* --- what the routines are -------------------------------------------------------------------- */

/* The first bytes of each routine (the loader's entry; the saver's entry), the length of the whole routine and a hash of
 * it (FNV-1a, 64 bits) and the exit. The loader's whole routine runs to the end of its edge routine (0604h). */
static const uint8_t LOAD_SIGNATURE[32] = {
    0x14, 0x08, 0x15, 0xF3, 0x3E, 0x0F, 0xD3, 0xFE, 0x21, 0x3F, 0x05, 0xE5,
    0xDB, 0xFE, 0x1F, 0xE6, 0x20, 0xF6, 0x02, 0x4F, 0xBF, 0xC0, 0xCD, 0xE7,
    0x05, 0x30, 0xFA, 0x21, 0x15, 0x04, 0x10, 0xFE,
};
static const uint8_t SAVE_SIGNATURE[32] = {
    0x21, 0x3F, 0x05, 0xE5, 0x21, 0x80, 0x1F, 0xCB, 0x7F, 0x28, 0x03, 0x21,
    0x98, 0x0C, 0x08, 0x13, 0xDD, 0x2B, 0xF3, 0x3E, 0x02, 0x47, 0x10, 0xFE,
    0xD3, 0xFE, 0xEE, 0x0F, 0x06, 0xA4, 0x2D, 0x20,
};
static const uint8_t EXIT_ROUTINE[23] = {
    0xF5, 0x3A, 0x48, 0x5C, 0xE6, 0x38, 0x0F, 0x0F, 0x0F, 0xD3, 0xFE, 0x3E,
    0x7F, 0xDB, 0xFE, 0x1F, 0xFB, 0x38, 0x02, 0xCF, 0x0C, 0xF1, 0xC9,
};
enum { LOAD_LENGTH = 95, SAVE_LENGTH = 125 };
#define LOAD_HASH 0x31A69103514C0E38ull
#define SAVE_HASH 0x2FCBA87A0E99AA80ull
/* Where each routine says it returns to: the operand of its `LD HL,nn / PUSH HL`. */
enum { LOAD_EXIT_OPERAND = 9, SAVE_EXIT_OPERAND = 1 };

/* What the loader leaves in B and C (the border colours and the edge state, which the program cannot tell from the tape),
 * as a load leaves them when the tape plays from the first pulse of the block with its signal low: B is B0h after any byte
 * (05D3); C is 7Eh once the flag byte is tested (bit 7 gone), and its complement when the block ends before the byte
 * wanted (the last pulse's end is one more edge). Measured against the ROM's own routine, played from the recorder. */
enum { LOADER_B = 0xB0, LOADER_C = 0x7E };
/* What the saver leaves in C (LD BC,3B0Eh) and in the alternate A (LD A,0Dh), and the flags of its last DEC H. */
enum { SAVER_C = 0x0E, SAVER_ALT_A = 0x0D, SAVER_ALT_F = 0xBA };

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xCBF29CE484222325ull;
    while (n--) {
        h ^= *p++;
        h *= 0x100000001B3ull;
    }
    return h;
}

static int find(const uint8_t *rom, size_t limit, const uint8_t *signature, size_t signature_length, size_t length,
                uint64_t hash, int loose, uint16_t *at) {
    if (length > limit) return 0;
    for (size_t a = 0; a + length <= limit; ++a)
        if (rom[a] == signature[0] && !memcmp(rom + a, signature, signature_length) && (loose || fnv1a(rom + a, length) == hash)) {
            *at = (uint16_t)a;
            return 1;
        }
    return 0;
}

static int exit_in_rom(const uint8_t *rom, size_t limit, uint16_t leave) {
    return (size_t)leave + sizeof EXIT_ROUTINE <= limit && !memcmp(rom + leave, EXIT_ROUTINE, sizeof EXIT_ROUTINE);
}

/* The bytes at the entry and at the exit are what the signatures say, in the memory as the CPU sees it now. */
static int live(const zr_traps *t, uint16_t entry, const uint8_t *signature, size_t length, uint16_t leave) {
    for (size_t i = 0; i < length; ++i)
        if (t->host.read(t->host.ctx, (uint16_t)(entry + i)) != signature[i]) return 0;
    for (size_t i = 0; i < sizeof EXIT_ROUTINE; ++i)
        if (t->host.read(t->host.ctx, (uint16_t)(leave + i)) != EXIT_ROUTINE[i]) return 0;
    return 1;
}

/* --- flags ------------------------------------------------------------------------------------ */

static uint8_t parity(uint8_t v) {
    v = (uint8_t)(v ^ (v >> 4));
    v = (uint8_t)(v ^ (v >> 2));
    v = (uint8_t)(v ^ (v >> 1));
    return (v & 1u) ? 0 : (uint8_t)ZR_Z80_P;
}

/* XOR and OR: S, X, Y and Z from the result, P its parity, H, N and C clear. */
static uint8_t logic_flags(uint8_t v) {
    return (uint8_t)((v & (ZR_Z80_S | ZR_Z80_Y | ZR_Z80_X)) | (v ? 0 : ZR_Z80_Z) | parity(v));
}

/* CP b with A = a: the flags of a - b, X and Y from b. */
static uint8_t cp_flags(uint8_t a, uint8_t b) {
    const uint8_t r = (uint8_t)(a - b);
    return (uint8_t)((r & ZR_Z80_S) | (b & (ZR_Z80_Y | ZR_Z80_X)) | (r ? 0 : ZR_Z80_Z) | ((a ^ b ^ r) & ZR_Z80_H) |
                     (((a ^ b) & (a ^ r) & 0x80) ? ZR_Z80_P : 0) | ZR_Z80_N | (a < b ? ZR_Z80_C : 0));
}

/* --- the loader's trap ---------------------------------------------------------------------------- */

/* The registers the loader leaves, on the path it took. */
typedef struct outcome {
    uint8_t a, f, b, c, h, l, a_, f_;
    uint16_t de, ix;
} outcome;

/* What the loader does with the bytes of the block `tape` (m of them, the flag byte first), for the entry registers of `z`. */
static void load_run(const zr_traps *t, const uint8_t *tape, size_t m, outcome *o) {
    const zr_z80 *z = t->cpu;
    const int load = (z->f & ZR_Z80_C) != 0;
    uint16_t ix = zr_z80_ix(z), left = zr_z80_de(z);
    uint8_t h = tape[0];                                /* the flag byte is read first: H := 0 xor L */
    memset(o, 0, sizeof *o);
    o->b = LOADER_B;
    o->h = h;
    o->l = tape[0];
    o->de = left;
    o->ix = ix;
    const uint8_t x = (uint8_t)(z->a ^ tape[0]);
    if (x) {
        /* The flag byte is not the one asked for: RET NZ at 05B6 with the xor in A. C was rotated left through the
         * carry (RL C) just before; the alternate pair holds the D|E of the byte-read loop. */
        o->a = x;
        o->f = logic_flags(x);
        o->a_ = (uint8_t)(z->d | z->e);
        o->f_ = logic_flags(o->a_);
        o->c = (uint8_t)(LOADER_C << 1 | (load ? 1u : 0u));
        return;
    }
    /* The flag byte matched: C := C with bit 7 clear, and the alternate pair is the loop's "mode" register: A = C, Z set
     * (the xor was 0), P (its parity), the carry of LOAD or VERIFY, X and Y from C. VERIFY's own xor leaves (0, Z|P). */
    const uint8_t c = (uint8_t)(LOADER_C & 0x7F);
    uint8_t ta = c, tf = (uint8_t)(ZR_Z80_Z | ZR_Z80_P | (load ? ZR_Z80_C : 0) | (c & (ZR_Z80_Y | ZR_Z80_X)));
    o->c = c;
    for (size_t k = 1;; ++k) {
        if (k >= m) {
            /* The block ends before the byte the loader wants: the edge never comes, the counter wraps (A = 0, Z|H, B = 0,
             * carry clear), the byte being read has only its marker, L = 01h; DE and IX stand as the stored bytes left them. C has
             * seen the edge that ends the last pulse. */
            o->a = 0;
            o->f = (uint8_t)(ZR_Z80_Z | ZR_Z80_H);
            o->b = 0;
            o->c = (uint8_t)~c;
            o->l = 0x01;
            o->h = h;
            o->a_ = ta;
            o->f_ = tf;
            o->de = left;
            o->ix = ix;
            return;
        }
        const uint8_t byte = tape[k];
        h = (uint8_t)(h ^ byte);
        o->h = h;
        o->l = byte;
        if (left == 0) {
            /* The parity byte: LD A,H / CP 01h / RET at 05DF; the carry is set when H is 0. */
            o->a = h;
            o->f = cp_flags(h, 1);
            o->a_ = ta;
            o->f_ = tf;
            o->de = 0;
            o->ix = ix;
            return;
        }
        if (load) {
            t->host.write(t->host.ctx, ix, byte);
        } else {
            const uint8_t d = (uint8_t)(t->host.read(t->host.ctx, ix) ^ byte);
            if (d) {
                /* VERIFY: RET NZ at 05C1 with A = (IX) xor L; IX and DE stay at the byte that differs. */
                o->a = d;
                o->f = logic_flags(d);
                o->a_ = (uint8_t)(left >> 8 | left);
                o->f_ = logic_flags(o->a_);
                o->de = left;
                o->ix = ix;
                return;
            }
            ta = 0;
            tf = (uint8_t)(ZR_Z80_Z | ZR_Z80_P);
        }
        ++ix;
        --left;
    }
}

static int load_trap(zr_traps *t) {
    zr_z80 *z = t->cpu;
    /* D = FFh: the loader's INC D sets Z and it takes the flag byte for data; DE = 0: it reads the flag byte, compares
     * nothing and ends. Neither is a load a program makes: the ROM does them itself. */
    if (z->d == 0xFF || (z->d | z->e) == 0) return 0;
    if (!live(t, t->load, LOAD_SIGNATURE, sizeof LOAD_SIGNATURE, t->load_exit)) return 0;
    if (t->host.break_held && t->host.break_held(t->host.ctx)) return 0;
    const uint8_t *tape;
    size_t m;
    if (!t->host.block_next(t->host.ctx, &tape, &m) || m < 1) return 0;
    outcome o;
    load_run(t, tape, m, &o);
    t->host.block_done(t->host.ctx);
    z->a = o.a;
    z->f = o.f;
    z->b = o.b;
    z->c = o.c;
    z->h = o.h;
    z->l = o.l;
    z->d = (uint8_t)(o.de >> 8);
    z->e = (uint8_t)o.de;
    z->ixh = (uint8_t)(o.ix >> 8);
    z->ixl = (uint8_t)o.ix;
    z->a_ = o.a_;
    z->f_ = o.f_;
    z->iff1 = z->iff2 = 0;                              /* the loader's DI: its exit enables them */
    z->pc = t->load_exit;
    z->t += ZR_TRAPS_NOMINAL_TSTATES;
    return 1;
}

/* --- the saver's trap ---------------------------------------------------------------------------- */

static int save_trap(zr_traps *t) {
    zr_z80 *z = t->cpu;
    /* A block of more than 65535 bytes fits no tape block, and from D = FFh the saver itself stops after the flag byte. */
    if (z->d == 0xFF) return 0;
    if (!live(t, t->save, SAVE_SIGNATURE, sizeof SAVE_SIGNATURE, t->save_exit)) return 0;
    if (t->host.break_held && t->host.break_held(t->host.ctx)) return 0;
    const uint16_t n = zr_z80_de(z), ix = zr_z80_ix(z);
    uint8_t *block = malloc((size_t)n + 2u);
    if (!block) return 0;
    uint8_t parity_byte = z->a;
    block[0] = z->a;
    for (size_t i = 0; i < n; ++i) {
        block[1 + i] = t->host.read(t->host.ctx, (uint16_t)(ix + i));
        parity_byte = (uint8_t)(parity_byte ^ block[1 + i]);
    }
    block[(size_t)n + 1u] = parity_byte;
    const int taken = t->host.block_put(t->host.ctx, block, (size_t)n + 2u);
    free(block);
    if (!taken) return 0;
    /* The saver's success exit (see the header comment). */
    z->a = 0;
    z->f = (uint8_t)(ZR_Z80_Z | ZR_Z80_H | ZR_Z80_C);
    z->b = 0;
    z->c = SAVER_C;
    z->d = z->e = 0xFF;
    z->h = z->l = 0;
    const uint16_t after = (uint16_t)(ix + n + 1u);
    z->ixh = (uint8_t)(after >> 8);
    z->ixl = (uint8_t)after;
    z->a_ = SAVER_ALT_A;
    z->f_ = SAVER_ALT_F;
    z->iff1 = z->iff2 = 0;
    z->pc = t->save_exit;
    z->t += ZR_TRAPS_NOMINAL_TSTATES;
    return 1;
}

/* --- the CPU hook and the board's calls -------------------------------------------------------------- */

static int before_fetch(void *ctx, uint16_t pc) {
    zr_traps *t = ctx;
    if (t->load_on && pc == t->load) return load_trap(t);
    if (t->save_on && pc == t->save) return save_trap(t);
    return 0;
}

static void scan(zr_traps *t, zr_z80 *cpu, const zr_traps_host *host, const uint8_t *rom, size_t size, int loose) {
    memset(t, 0, sizeof *t);
    t->host = *host;
    t->cpu = cpu;
    t->hooks.ctx = t;
    t->hooks.watch = t->watch;
    t->hooks.before_fetch = before_fetch;
    const size_t limit = rom && size < 0x4000u ? size : rom ? 0x4000u : 0;
    uint16_t at;
    if (find(rom, limit, LOAD_SIGNATURE, sizeof LOAD_SIGNATURE, LOAD_LENGTH, LOAD_HASH, loose, &at)) {
        const uint16_t leave = zr_get16(rom + at + LOAD_EXIT_OPERAND);
        if (exit_in_rom(rom, limit, leave)) {
            t->load = at;
            t->load_exit = leave;
            t->has_load = 1;
        }
    }
    if (find(rom, limit, SAVE_SIGNATURE, sizeof SAVE_SIGNATURE, SAVE_LENGTH, SAVE_HASH, loose, &at)) {
        const uint16_t leave = zr_get16(rom + at + SAVE_EXIT_OPERAND);
        if (exit_in_rom(rom, limit, leave)) {
            t->save = at;
            t->save_exit = leave;
            t->has_save = 1;
        }
    }
}

void zr_traps_init(zr_traps *t, zr_z80 *cpu, const zr_traps_host *host, const uint8_t *rom, size_t size) {
    scan(t, cpu, host, rom, size, 0);
}

void zr_traps_init_loose(zr_traps *t, zr_z80 *cpu, const zr_traps_host *host, const uint8_t *memory, size_t size) {
    scan(t, cpu, host, memory, size, 1);
}

void zr_traps_enable(zr_traps *t, int load, int save) {
    t->load_on = (uint8_t)(load && t->has_load);
    t->save_on = (uint8_t)(save && t->has_save);
    memset(t->watch, 0, sizeof t->watch);
    if (t->load_on) t->watch[t->load >> 3] |= (uint8_t)(1u << (t->load & 7u));
    if (t->save_on) t->watch[t->save >> 3] |= (uint8_t)(1u << (t->save & 7u));
}

const zr_cpu_hooks *zr_traps_hooks(const zr_traps *t) { return t->load_on || t->save_on ? &t->hooks : NULL; }
