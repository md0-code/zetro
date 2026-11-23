/* ZetRo - the Z80 CPU (z80.h). ZetRo's own code, written from Zilog's Z80
 * CPU User Manual and the published descriptions of the undocumented
 * instructions, flags and internal registers; fitted to the SingleStepTests
 * Z80 suite. No emulator source consulted.
 *
 * An opcode is taken apart as the processor's own decoder does: x the top
 * two bits, y the middle three, z the low three, with p and q the halves
 * of y. The registers a field names are B C D E H L (HL) A; after a DD or
 * FD prefix H, L and HL are the halves and the whole of IX or IY, except
 * that an instruction with an (IX+d) operand keeps the real H and L for
 * its other operand. */
#include "z80.h"
#include "state.h"

#include <string.h>

enum { FC = ZR_Z80_C, FN = ZR_Z80_N, FP = ZR_Z80_P, FX = ZR_Z80_X, FH = ZR_Z80_H, FY = ZR_Z80_Y, FZ = ZR_Z80_Z, FS = ZR_Z80_S, FXY = FX | FY };

/* S, Z and bits 3 and 5 of a result; the same with the parity of it. */
static uint8_t SZ[256], SZP[256];
static int tables_built;

static void build_tables(void) {
    for (unsigned v = 0; v < 256; ++v) {
        unsigned ones = 0;
        for (unsigned bit = 0; bit < 8; ++bit) ones += v >> bit & 1u;
        SZ[v] = (uint8_t)((v & (FS | FXY)) | (v ? 0 : FZ));
        SZP[v] = (uint8_t)(SZ[v] | (ones & 1u ? 0 : FP));
    }
    tables_built = 1;
}
#define PARITY(v) (SZP[(uint8_t)(v)] & FP)

#define AF(z) zr_z80_af(z)
#define BC(z) zr_z80_bc(z)
#define DE(z) zr_z80_de(z)
#define HL(z) zr_z80_hl(z)
/* The flags as an instruction leaves them: Q remembers that it did. */
#define SETF(z, v) ((z)->f = (z)->q = (uint8_t)(v))

/* --- the bus -------------------------------------------------------------------------- */

static inline void stretch(zr_z80 *z, int kind, uint16_t address) {
    if (z->bus->stretch) z->t += z->bus->stretch(z->bus->ctx, kind, address);
}

/* The refresh address of the last opcode fetch: what stays on the bus in the T-states that follow it. */
static inline uint16_t ir_bus(const zr_z80 *z) { return (uint16_t)(z->i << 8 | (z->r & 0x80) | ((z->r - 1) & 0x7F)); }

/* An opcode fetch: the opcode, then the refresh address while it is decoded. */
static uint8_t m1(zr_z80 *z) {
    stretch(z, ZR_Z80_FETCH, z->pc);
    const uint8_t op = z->bus->read(z->bus->ctx, z->pc, 1);
    ++z->pc;
    if (z->hooks && z->hooks->refresh) z->hooks->refresh(z->hooks->ctx, (uint16_t)(z->i << 8 | z->r));
    z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
    z->t += 4;
    return op;
}

static uint8_t rd(zr_z80 *z, uint16_t address) {
    stretch(z, ZR_Z80_READ, address);
    const uint8_t v = z->bus->read(z->bus->ctx, address, 0);
    z->t += 3;
    return v;
}

static void wr(zr_z80 *z, uint16_t address, uint8_t value) {
    stretch(z, ZR_Z80_WRITE, address);
    z->bus->write(z->bus->ctx, address, value);
    z->t += 3;
}

/* T-states inside the processor, the address left on the bus. */
static void idle(zr_z80 *z, uint16_t address, unsigned count) {
    if (!z->bus->stretch) { z->t += count; return; }
    for (unsigned i = 0; i < count; ++i) {
        z->t += z->bus->stretch(z->bus->ctx, ZR_Z80_IDLE, address);
        ++z->t;
    }
}

static uint8_t port_in(zr_z80 *z, uint16_t port) {
    stretch(z, ZR_Z80_IN, port);
    const uint8_t v = z->bus->in(z->bus->ctx, port);
    z->t += 4;
    return v;
}

static void port_out(zr_z80 *z, uint16_t port, uint8_t value) {
    stretch(z, ZR_Z80_OUT, port);
    z->bus->out(z->bus->ctx, port, value);
    z->t += 4;
}

static uint8_t imm8(zr_z80 *z) { return rd(z, z->pc++); }
static uint16_t imm16(zr_z80 *z) {
    const uint8_t lo = imm8(z);
    return (uint16_t)(imm8(z) << 8 | lo);
}
static void push(zr_z80 *z, uint16_t v) {
    wr(z, --z->sp, (uint8_t)(v >> 8));
    wr(z, --z->sp, (uint8_t)v);
}
static uint16_t pop(zr_z80 *z) {
    const uint8_t lo = rd(z, z->sp++);
    return (uint16_t)(rd(z, z->sp++) << 8 | lo);
}

/* --- the registers a field names ---------------------------------------------------------- */

/* B C D E H L - A, the real ones (index 6 is not a register). */
static uint8_t *plain(zr_z80 *z, unsigned index) {
    switch (index) {
    case 0: return &z->b;
    case 1: return &z->c;
    case 2: return &z->d;
    case 3: return &z->e;
    case 4: return &z->h;
    case 5: return &z->l;
    default: return &z->a;
    }
}
/* The same after a prefix: H and L are the halves of IX or IY. */
static uint8_t *reg(zr_z80 *z, unsigned index) {
    if (z->xy && index == 4) return z->xy == 1 ? &z->ixh : &z->iyh;
    if (z->xy && index == 5) return z->xy == 1 ? &z->ixl : &z->iyl;
    return plain(z, index);
}

/* HL, or IX or IY after a prefix. */
static uint16_t hlx(const zr_z80 *z) { return z->xy == 0 ? HL(z) : z->xy == 1 ? zr_z80_ix(z) : zr_z80_iy(z); }
static void set_hlx(zr_z80 *z, uint16_t v) {
    if (z->xy == 0) { z->h = (uint8_t)(v >> 8); z->l = (uint8_t)v; }
    else if (z->xy == 1) { z->ixh = (uint8_t)(v >> 8); z->ixl = (uint8_t)v; }
    else { z->iyh = (uint8_t)(v >> 8); z->iyl = (uint8_t)v; }
}

/* BC DE HL SP. */
static uint16_t rp(const zr_z80 *z, unsigned p) { return p == 0 ? BC(z) : p == 1 ? DE(z) : p == 2 ? hlx(z) : z->sp; }
static void set_rp(zr_z80 *z, unsigned p, uint16_t v) {
    if (p == 0) { z->b = (uint8_t)(v >> 8); z->c = (uint8_t)v; }
    else if (p == 1) { z->d = (uint8_t)(v >> 8); z->e = (uint8_t)v; }
    else if (p == 2) set_hlx(z, v);
    else z->sp = v;
}

/* The address of the (HL) operand: HL, or IX+d or IY+d with the displacement read and the sum made (5 T-states,
 * the address of the displacement left on the bus), which also goes to WZ. */
static uint16_t operand(zr_z80 *z) {
    if (!z->xy) return HL(z);
    const int8_t d = (int8_t)rd(z, z->pc);
    idle(z, z->pc, 5);
    ++z->pc;
    z->wz = (uint16_t)(hlx(z) + d);
    return z->wz;
}

/* NZ Z NC C PO PE P M. */
static int condition(const zr_z80 *z, unsigned y) {
    static const uint8_t FLAG[4] = {FZ, FC, FP, FS};
    return ((z->f & FLAG[y >> 1]) != 0) == (int)(y & 1u);
}

/* --- arithmetic ----------------------------------------------------------------------------- */

/* ADD ADC SUB SBC AND XOR OR CP on A. */
static void alu(zr_z80 *z, unsigned op, uint8_t v) {
    const unsigned a = z->a;
    unsigned r;
    switch (op) {
    case 0: case 1:
        r = a + v + (op == 1 ? z->f & FC : 0u);
        SETF(z, SZ[r & 0xFF] | ((a ^ v ^ r) & FH) | (((a ^ ~v) & (a ^ r) & 0x80u) >> 5) | (r >> 8 & 1u));
        z->a = (uint8_t)r;
        break;
    case 2: case 3: case 7:
        r = a - v - (op == 3 ? z->f & FC : 0u);
        SETF(z, SZ[r & 0xFF] | FN | ((a ^ v ^ r) & FH) | (((a ^ v) & (a ^ r) & 0x80u) >> 5) | (r >> 8 & 1u));
        if (op == 7) SETF(z, (z->f & ~FXY) | (v & FXY));       /* CP: bits 3 and 5 are the operand's */
        else z->a = (uint8_t)r;
        break;
    case 4: z->a &= v; SETF(z, SZP[z->a] | FH); break;
    case 5: z->a ^= v; SETF(z, SZP[z->a]); break;
    default: z->a |= v; SETF(z, SZP[z->a]); break;
    }
}

static uint8_t inc8(zr_z80 *z, uint8_t v) {
    const uint8_t r = (uint8_t)(v + 1);
    SETF(z, (z->f & FC) | SZ[r] | ((r & 0x0F) ? 0 : FH) | (r == 0x80 ? FP : 0));
    return r;
}
static uint8_t dec8(zr_z80 *z, uint8_t v) {
    const uint8_t r = (uint8_t)(v - 1);
    SETF(z, (z->f & FC) | FN | SZ[r] | ((r & 0x0F) == 0x0F ? FH : 0) | (r == 0x7F ? FP : 0));
    return r;
}

/* RLC RRC RL RR SLA SRA SLL SRL. */
static uint8_t rotate(zr_z80 *z, unsigned op, uint8_t v) {
    const unsigned carry = z->f & FC;
    unsigned r, out;
    switch (op) {
    case 0: out = v >> 7; r = (unsigned)v << 1 | out; break;
    case 1: out = v & 1u; r = v >> 1 | out << 7; break;
    case 2: out = v >> 7; r = (unsigned)v << 1 | carry; break;
    case 3: out = v & 1u; r = v >> 1 | carry << 7; break;
    case 4: out = v >> 7; r = (unsigned)v << 1; break;
    case 5: out = v & 1u; r = v >> 1 | (v & 0x80u); break;
    case 6: out = v >> 7; r = (unsigned)v << 1 | 1u; break;
    default: out = v & 1u; r = v >> 1; break;
    }
    SETF(z, SZP[r & 0xFF] | out);
    return (uint8_t)r;
}

/* ADD HL,rr: the carry out of bit 11 and bit 15; S, Z and P stay. */
static void add16(zr_z80 *z, uint16_t v) {
    const unsigned hl = hlx(z), r = hl + v;
    idle(z, ir_bus(z), 7);
    z->wz = (uint16_t)(hl + 1);
    SETF(z, (z->f & (FS | FZ | FP)) | ((hl ^ v ^ r) >> 8 & FH) | (r >> 16 & 1u) | (r >> 8 & FXY));
    set_hlx(z, (uint16_t)r);
}

/* ADC HL,rr and SBC HL,rr: all the flags, of the 16-bit result. */
static void adc16(zr_z80 *z, uint16_t v, int subtract) {
    const unsigned hl = HL(z), carry = z->f & FC;
    const unsigned r = subtract ? hl - v - carry : hl + v + carry;
    const unsigned overflow = subtract ? (hl ^ v) & (hl ^ r) & 0x8000u : (hl ^ ~v) & (hl ^ r) & 0x8000u;
    idle(z, ir_bus(z), 7);
    z->wz = (uint16_t)(hl + 1);
    SETF(z, (r >> 8 & (FS | FXY)) | ((r & 0xFFFF) ? 0 : FZ) | ((hl ^ v ^ r) >> 8 & FH) | (overflow >> 13) | (r >> 16 & 1u) | (subtract ? FN : 0));
    z->h = (uint8_t)(r >> 8);
    z->l = (uint8_t)r;
}

static void daa(zr_z80 *z) {
    const uint8_t a = z->a;
    unsigned fix = 0, carry = z->f & FC, half;
    if ((z->f & FH) || (a & 0x0F) > 9) fix |= 0x06;
    if (carry || a > 0x99) { fix |= 0x60; carry = FC; }
    if (z->f & FN) {
        half = (z->f & FH) && (a & 0x0F) < 6;
        z->a = (uint8_t)(a - fix);
    } else {
        half = (a & 0x0F) > 9;
        z->a = (uint8_t)(a + fix);
    }
    SETF(z, SZP[z->a] | carry | (z->f & FN) | (half ? FH : 0));
}

/* --- the CB page ---------------------------------------------------------------------------- */

/* Rotates and shifts, BIT, RES and SET. After DD or FD the operand is always (IX+d), read before the opcode, and
 * a rotate, RES or SET also loads its result into the register the opcode names. */
static void exec_cb(zr_z80 *z) {
    uint16_t address = 0;
    uint8_t op, v;
    if (z->xy) {
        z->wz = address = (uint16_t)(hlx(z) + (int8_t)rd(z, z->pc));
        ++z->pc;
        op = rd(z, z->pc);
        idle(z, z->pc, 2);
        ++z->pc;
    } else op = m1(z);
    const unsigned x = op >> 6, y = op >> 3 & 7u, r = op & 7u;
    const int memory = z->xy || r == 6;
    if (memory) {
        if (!z->xy) address = HL(z);
        v = rd(z, address);
        idle(z, address, 1);
    } else v = *plain(z, r);
    if (x == 1) {
        /* BIT: Z and P from the bit, S only for bit 7; bits 3 and 5 from the register, or from WZ's high byte for memory. */
        const unsigned bit = v & 1u << y;
        SETF(z, (z->f & FC) | FH | (bit ? 0 : FZ | FP) | (bit & FS) | ((memory ? z->wz >> 8 : v) & FXY));
        return;
    }
    if (x == 0) v = rotate(z, y, v);
    else if (x == 2) v &= (uint8_t)~(1u << y);
    else v |= (uint8_t)(1u << y);
    if (memory) wr(z, address, v);
    if (r != 6) *plain(z, r) = v;
}

/* --- the ED page ---------------------------------------------------------------------------- */

/* The block instructions: LDI CPI INI OUTI, their D forms and their repeating forms. */
static void exec_block(zr_z80 *z, unsigned y, unsigned kind) {
    const int down = y & 1u, repeat = y >= 6;
    const int step = down ? -1 : 1;
    uint16_t hl = HL(z), bc = BC(z);
    int again = 0;
    uint8_t v;
    switch (kind) {
    case 0: {                                                   /* LDI LDD LDIR LDDR */
        const uint16_t de = DE(z);
        v = rd(z, hl);
        wr(z, de, v);
        idle(z, de, 2);
        --bc;
        const unsigned n = (unsigned)v + z->a;
        SETF(z, (z->f & (FS | FZ | FC)) | (bc ? FP : 0) | (n & FX) | (n << 4 & FY));
        z->d = (uint8_t)((de + step) >> 8);
        z->e = (uint8_t)(de + step);
        again = repeat && bc;
        if (again) idle(z, de, 5);
        break;
    }
    case 1: {                                                   /* CPI CPD CPIR CPDR */
        v = rd(z, hl);
        idle(z, hl, 5);
        --bc;
        const unsigned r = (unsigned)z->a - v, half = (z->a ^ v ^ r) & FH;
        const unsigned n = r - (half ? 1u : 0u);
        SETF(z, (z->f & FC) | FN | (SZ[r & 0xFF] & (FS | FZ)) | half | (bc ? FP : 0) | (n & FX) | (n << 4 & FY));
        z->wz = (uint16_t)(z->wz + step);
        again = repeat && bc && (r & 0xFF);
        if (again) idle(z, hl, 5);
        break;
    }
    default: {                                                  /* INI IND INIR INDR, OUTI OUTD OTIR OTDR */
        unsigned k;
        idle(z, ir_bus(z), 1);
        if (kind == 2) {
            v = port_in(z, bc);
            z->wz = (uint16_t)(bc + step);
            --z->b;
            wr(z, hl, v);
            k = (unsigned)v + ((z->c + step) & 0xFFu);
        } else {
            v = rd(z, hl);
            --z->b;
            bc = BC(z);
            port_out(z, bc, v);
            z->wz = (uint16_t)(bc + step);
            k = (unsigned)v + ((hl + step) & 0xFFu);
        }
        SETF(z, SZ[z->b] | (v & 0x80 ? FN : 0) | (k > 255 ? FH | FC : 0) | PARITY((k & 7u) ^ z->b));
        again = repeat && z->b;
        if (again) {
            /* The repeat steps the program counter back and, on its way, leaves marks in H and P. */
            unsigned f = z->f;
            idle(z, kind == 2 ? hl : bc, 5);
            if (f & FC) {
                if (v & 0x80) { f ^= PARITY((z->b - 1) & 7u) ^ FP; f = (f & ~FH) | ((z->b & 0x0F) == 0x00 ? FH : 0); }
                else { f ^= PARITY((z->b + 1) & 7u) ^ FP; f = (f & ~FH) | ((z->b & 0x0F) == 0x0F ? FH : 0); }
            } else f ^= PARITY(z->b & 7u) ^ FP;
            SETF(z, f);
        }
        bc = BC(z);
        break;
    }
    }
    hl = (uint16_t)(hl + step);
    z->h = (uint8_t)(hl >> 8);
    z->l = (uint8_t)hl;
    z->b = (uint8_t)(bc >> 8);
    z->c = (uint8_t)bc;
    if (again) {
        /* Back to the instruction's first byte; bits 3 and 5 are then the program counter's. */
        z->pc -= 2;
        z->wz = (uint16_t)(z->pc + 1);
        SETF(z, (z->f & ~FXY) | (z->pc >> 8 & FXY));
    }
}

static void exec_ed(zr_z80 *z) {
    const uint8_t op = m1(z);
    const unsigned x = op >> 6, y = op >> 3 & 7u, zz = op & 7u, p = y >> 1, q = y & 1u;
    z->xy = 0;                                                  /* a prefix before ED does nothing */
    if (x == 2 && zz <= 3 && y >= 4) { exec_block(z, y, zz); return; }
    if (x != 1) return;                                         /* the empty codes: two fetches and nothing more */
    switch (zz) {
    case 0: {                                                   /* IN r,(C); IN F,(C) only sets the flags */
        const uint8_t v = port_in(z, BC(z));
        z->wz = (uint16_t)(BC(z) + 1);
        if (y != 6) *plain(z, y) = v;
        SETF(z, (z->f & FC) | SZP[v]);
        break;
    }
    case 1:                                                     /* OUT (C),r; OUT (C),0 (FFh on the CMOS processor) */
        port_out(z, BC(z), y != 6 ? *plain(z, y) : z->cmos ? 0xFF : 0x00);
        z->wz = (uint16_t)(BC(z) + 1);
        break;
    case 2:
        adc16(z, rp(z, p), !q);
        break;
    case 3: {                                                   /* LD (nn),rr and LD rr,(nn) */
        const uint16_t address = imm16(z);
        z->wz = (uint16_t)(address + 1);
        if (!q) {
            const uint16_t v = rp(z, p);
            wr(z, address, (uint8_t)v);
            wr(z, z->wz, (uint8_t)(v >> 8));
        } else {
            const uint8_t lo = rd(z, address);
            set_rp(z, p, (uint16_t)(rd(z, z->wz) << 8 | lo));
        }
        break;
    }
    case 4: {                                                   /* NEG, eight times */
        const uint8_t v = z->a;
        z->a = 0;
        alu(z, 2, v);
        break;
    }
    case 5:                                                     /* RETN, and RETI at 4Dh: both bring IFF1 back */
        z->pc = z->wz = pop(z);
        z->iff1 = z->iff2;
        break;
    case 6: {
        static const uint8_t MODE[8] = {0, 0, 1, 2, 0, 0, 1, 2};
        z->im = MODE[y];
        break;
    }
    default:
        switch (y) {
        case 0: idle(z, ir_bus(z), 1); z->i = z->a; break;
        case 1: idle(z, ir_bus(z), 1); z->r = z->a; break;
        case 2: case 3:                                         /* LD A,I and LD A,R: P is IFF2 */
            idle(z, ir_bus(z), 1);
            z->a = y == 2 ? z->i : z->r;
            SETF(z, (z->f & FC) | SZ[z->a] | (z->iff2 ? FP : 0));
            z->p = 1;
            break;
        case 4: case 5: {                                       /* RRD and RLD */
            const uint16_t hl = HL(z);
            const uint8_t v = rd(z, hl);
            idle(z, hl, 4);
            if (y == 4) { wr(z, hl, (uint8_t)(z->a << 4 | v >> 4)); z->a = (uint8_t)((z->a & 0xF0) | (v & 0x0F)); }
            else { wr(z, hl, (uint8_t)(v << 4 | (z->a & 0x0F))); z->a = (uint8_t)((z->a & 0xF0) | v >> 4); }
            z->wz = (uint16_t)(hl + 1);
            SETF(z, (z->f & FC) | SZP[z->a]);
            break;
        }
        default: break;
        }
        break;
    }
}

/* --- the main page ---------------------------------------------------------------------------- */

static void exec(zr_z80 *z, uint8_t op, uint8_t q_in) {
    const unsigned x = op >> 6, y = op >> 3 & 7u, zz = op & 7u, p = y >> 1, q = y & 1u;
    if (x == 1) {
        if (op == 0x76) { z->halted = 1; return; }
        /* LD r,r': with a memory operand the other one is a real register. */
        if (y == 6) wr(z, operand(z), *plain(z, zz));
        else if (zz == 6) *plain(z, y) = rd(z, operand(z));
        else *reg(z, y) = *reg(z, zz);
        return;
    }
    if (x == 2) {
        alu(z, y, zz == 6 ? rd(z, operand(z)) : *reg(z, zz));
        return;
    }
    if (x == 0) {
        switch (zz) {
        case 0:
            if (y == 0) break;
            if (y == 1) {
                uint8_t s = z->a; z->a = z->a_; z->a_ = s;
                s = z->f; z->f = z->f_; z->f_ = s;
                break;
            }
            /* DJNZ, JR and JR cc: the displacement, then five T-states to add it. */
            if (y == 2) idle(z, ir_bus(z), 1);
            {
                const int8_t d = (int8_t)rd(z, z->pc);
                const int taken = y == 2 ? --z->b != 0 : y == 3 || condition(z, y - 4);
                if (taken) idle(z, z->pc, 5);
                ++z->pc;
                if (taken) z->wz = z->pc = (uint16_t)(z->pc + d);
            }
            break;
        case 1:
            if (!q) set_rp(z, p, imm16(z));
            else add16(z, rp(z, p));
            break;
        case 2:
            switch (y) {
            case 0: case 2: {                                   /* LD (BC),A and LD (DE),A */
                const uint16_t address = rp(z, p);
                wr(z, address, z->a);
                z->wz = (uint16_t)(z->a << 8 | ((address + 1) & 0xFF));
                break;
            }
            case 1: case 3: {                                   /* LD A,(BC) and LD A,(DE) */
                const uint16_t address = rp(z, p);
                z->a = rd(z, address);
                z->wz = (uint16_t)(address + 1);
                break;
            }
            case 4: {                                           /* LD (nn),HL */
                const uint16_t address = imm16(z), v = hlx(z);
                z->wz = (uint16_t)(address + 1);
                wr(z, address, (uint8_t)v);
                wr(z, z->wz, (uint8_t)(v >> 8));
                break;
            }
            case 5: {                                           /* LD HL,(nn) */
                const uint16_t address = imm16(z);
                z->wz = (uint16_t)(address + 1);
                const uint8_t lo = rd(z, address);
                set_hlx(z, (uint16_t)(rd(z, z->wz) << 8 | lo));
                break;
            }
            case 6: {                                           /* LD (nn),A */
                const uint16_t address = imm16(z);
                wr(z, address, z->a);
                z->wz = (uint16_t)(z->a << 8 | ((address + 1) & 0xFF));
                break;
            }
            default: {                                          /* LD A,(nn) */
                const uint16_t address = imm16(z);
                z->a = rd(z, address);
                z->wz = (uint16_t)(address + 1);
                break;
            }
            }
            break;
        case 3:
            idle(z, ir_bus(z), 2);
            set_rp(z, p, (uint16_t)(rp(z, p) + (q ? -1 : 1)));
            break;
        case 4: case 5:                                         /* INC r and DEC r */
            if (y == 6) {
                const uint16_t address = operand(z);
                const uint8_t v = rd(z, address);
                idle(z, address, 1);
                wr(z, address, zz == 4 ? inc8(z, v) : dec8(z, v));
            } else *reg(z, y) = zz == 4 ? inc8(z, *reg(z, y)) : dec8(z, *reg(z, y));
            break;
        case 6:                                                 /* LD r,n */
            if (y != 6) *reg(z, y) = imm8(z);
            else if (!z->xy) { const uint8_t n = imm8(z); wr(z, HL(z), n); }
            else {
                /* LD (IX+d),n: the sum is made while the n is read. */
                z->wz = (uint16_t)(hlx(z) + (int8_t)imm8(z));
                const uint8_t n = rd(z, z->pc);
                idle(z, z->pc, 2);
                ++z->pc;
                wr(z, z->wz, n);
            }
            break;
        default:
            switch (y) {
            case 0: {                                           /* RLCA */
                const unsigned out = z->a >> 7;
                z->a = (uint8_t)(z->a << 1 | out);
                SETF(z, (z->f & (FS | FZ | FP)) | out | (z->a & FXY));
                break;
            }
            case 1: {                                           /* RRCA */
                const unsigned out = z->a & 1u;
                z->a = (uint8_t)(z->a >> 1 | out << 7);
                SETF(z, (z->f & (FS | FZ | FP)) | out | (z->a & FXY));
                break;
            }
            case 2: {                                           /* RLA */
                const unsigned out = z->a >> 7;
                z->a = (uint8_t)(z->a << 1 | (z->f & FC));
                SETF(z, (z->f & (FS | FZ | FP)) | out | (z->a & FXY));
                break;
            }
            case 3: {                                           /* RRA */
                const unsigned out = z->a & 1u;
                z->a = (uint8_t)(z->a >> 1 | (z->f & FC) << 7);
                SETF(z, (z->f & (FS | FZ | FP)) | out | (z->a & FXY));
                break;
            }
            case 4: daa(z); break;
            case 5:                                             /* CPL */
                z->a = (uint8_t)~z->a;
                SETF(z, (z->f & (FS | FZ | FP | FC)) | FH | FN | (z->a & FXY));
                break;
            default: {
                /* SCF and CCF: bits 3 and 5 are A's, and on the NMOS processor also what F held when the
                 * instruction before did not change the flags. */
                const unsigned xy = (z->cmos ? z->a : ((q_in ^ z->f) | z->a)) & FXY;
                if (y == 6) SETF(z, (z->f & (FS | FZ | FP)) | FC | xy);
                else SETF(z, ((z->f & (FS | FZ | FP | FC)) | (z->f & FC ? FH : 0) | xy) ^ FC);
                break;
            }
            }
            break;
        }
        return;
    }
    /* x == 3 */
    switch (zz) {
    case 0:                                                     /* RET cc */
        idle(z, ir_bus(z), 1);
        if (condition(z, y)) z->pc = z->wz = pop(z);
        break;
    case 1:
        if (!q) {                                               /* POP */
            const uint16_t v = pop(z);
            if (p == 3) { z->a = (uint8_t)(v >> 8); z->f = (uint8_t)v; }
            else set_rp(z, p, v);
        } else if (p == 0) z->pc = z->wz = pop(z);              /* RET */
        else if (p == 1) {                                      /* EXX */
            uint8_t s;
            s = z->b; z->b = z->b_; z->b_ = s;
            s = z->c; z->c = z->c_; z->c_ = s;
            s = z->d; z->d = z->d_; z->d_ = s;
            s = z->e; z->e = z->e_; z->e_ = s;
            s = z->h; z->h = z->h_; z->h_ = s;
            s = z->l; z->l = z->l_; z->l_ = s;
        } else if (p == 2) z->pc = hlx(z);                      /* JP (HL) */
        else { idle(z, ir_bus(z), 2); z->sp = hlx(z); }         /* LD SP,HL */
        break;
    case 2:                                                     /* JP cc,nn */
        z->wz = imm16(z);
        if (condition(z, y)) z->pc = z->wz;
        break;
    case 3:
        switch (y) {
        case 0: z->pc = z->wz = imm16(z); break;                /* JP nn */
        case 1: exec_cb(z); break;
        case 2: {                                               /* OUT (n),A */
            const uint8_t n = imm8(z);
            port_out(z, (uint16_t)(z->a << 8 | n), z->a);
            z->wz = (uint16_t)(z->a << 8 | ((n + 1) & 0xFF));
            break;
        }
        case 3: {                                               /* IN A,(n) */
            const uint16_t port = (uint16_t)(z->a << 8 | imm8(z));
            z->a = port_in(z, port);
            z->wz = (uint16_t)(port + 1);
            break;
        }
        case 4: {                                               /* EX (SP),HL */
            const uint8_t lo = rd(z, z->sp), hi = rd(z, (uint16_t)(z->sp + 1));
            const uint16_t v = hlx(z);
            idle(z, (uint16_t)(z->sp + 1), 1);
            wr(z, (uint16_t)(z->sp + 1), (uint8_t)(v >> 8));
            wr(z, z->sp, (uint8_t)v);
            idle(z, z->sp, 2);
            z->wz = (uint16_t)(hi << 8 | lo);
            set_hlx(z, z->wz);
            break;
        }
        case 5: {                                               /* EX DE,HL: never IX or IY */
            uint8_t s = z->d; z->d = z->h; z->h = s;
            s = z->e; z->e = z->l; z->l = s;
            break;
        }
        case 6: z->iff1 = z->iff2 = 0; break;                   /* DI */
        default: z->iff1 = z->iff2 = 1; z->ei = 1; break;       /* EI: no interrupt before the next instruction is over */
        }
        break;
    case 4: {                                                   /* CALL cc,nn */
        z->wz = imm16(z);
        if (condition(z, y)) {
            idle(z, (uint16_t)(z->pc - 1), 1);
            push(z, z->pc);
            z->pc = z->wz;
        }
        break;
    }
    case 5:
        if (!q) {                                               /* PUSH */
            idle(z, ir_bus(z), 1);
            push(z, p == 3 ? AF(z) : rp(z, p));
        } else if (p == 0) {                                    /* CALL nn */
            z->wz = imm16(z);
            idle(z, (uint16_t)(z->pc - 1), 1);
            push(z, z->pc);
            z->pc = z->wz;
        } else if (p == 2) exec_ed(z);
        /* DD and FD are taken by the caller. */
        break;
    case 6:
        alu(z, y, imm8(z));
        break;
    default:                                                    /* RST */
        idle(z, ir_bus(z), 1);
        push(z, z->pc);
        z->pc = z->wz = (uint16_t)(y * 8u);
        break;
    }
}

/* --- interrupts and the instruction loop -------------------------------------------------------- */

unsigned zr_z80_step(zr_z80 *z) {
    const uint64_t start = z->t;
    const uint8_t ei = z->ei, after_ld_a_ir = z->p;
    uint8_t q_in = z->q;
    z->ei = z->p = z->q = 0;
    if (z->nmi) {
        /* An opcode fetch whose byte is dropped, then the return address pushed: IFF2 keeps what IFF1 was. */
        z->nmi = 0;
        z->halted = 0;
        z->iff1 = 0;
        stretch(z, ZR_Z80_FETCH, z->pc);
        (void)z->bus->read(z->bus->ctx, z->pc, 1);
        z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
        z->t += 5;
        push(z, z->pc);
        z->pc = z->wz = 0x0066;
        return (unsigned)(z->t - start);
    }
    if (z->irq && z->iff1 && !ei) {
        /* The acknowledge cycle: an M1 with IORQ, two T-states longer, the device's byte on the bus. */
        z->halted = 0;
        z->iff1 = z->iff2 = 0;
        const uint8_t byte = z->bus->ack ? z->bus->ack(z->bus->ctx) : 0xFF;
        z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
        z->t += 7;
        /* The NMOS processor has not yet settled P when it is interrupted right after LD A,I or LD A,R. */
        if (after_ld_a_ir && !z->cmos) z->f &= (uint8_t)~FP;
        push(z, z->pc);
        if (z->im == 2) {
            const uint16_t vector = (uint16_t)(z->i << 8 | byte);
            const uint8_t lo = rd(z, vector);
            z->pc = (uint16_t)(rd(z, (uint16_t)(vector + 1)) << 8 | lo);
        } else if (z->im == 1) z->pc = 0x0038;
        else z->pc = (byte & 0xC7) == 0xC7 ? (uint16_t)(byte & 0x38) : 0x0038;   /* mode 0: the RST the device gives (ZetRo runs no other opcode from the bus) */
        z->wz = z->pc;
        return (unsigned)(z->t - start);
    }
    if (z->halted) {
        /* HALT: the processor keeps fetching the byte after it and does nothing with it. */
        stretch(z, ZR_Z80_FETCH, z->pc);
        (void)z->bus->read(z->bus->ctx, z->pc, 1);
        z->r = (uint8_t)((z->r & 0x80) | ((z->r + 1) & 0x7F));
        z->t += 4;
        return 4;
    }
    const uint16_t pc = z->pc;
    const int watched = zr_cpu_watched(z->hooks, pc);
    if (watched && z->hooks->before_fetch && z->hooks->before_fetch(z->hooks->ctx, pc)) {
        z->q = q_in;
        return (unsigned)(z->t - start);
    }
    uint8_t op = m1(z);
    if (watched && z->hooks->after_fetch) z->hooks->after_fetch(z->hooks->ctx, pc, op);
    /* DD and FD: the last one before the opcode counts, and no interrupt comes between. A prefix is a step of
     * its own that leaves the flags alone, so Q is clear for the instruction after it. */
    z->xy = 0;
    while (op == 0xDD || op == 0xFD) {
        z->xy = op == 0xDD ? 1 : 2;
        q_in = 0;
        op = m1(z);
    }
    exec(z, op, q_in);
    z->xy = 0;
    return (unsigned)(z->t - start);
}

void zr_z80_nmi(zr_z80 *z) { z->nmi = 1; }
void zr_z80_int(zr_z80 *z, int asserted) { z->irq = asserted != 0; }

/* --- life cycle ------------------------------------------------------------------------------ */

void zr_z80_init(zr_z80 *z, const zr_z80_bus *bus, const zr_cpu_hooks *hooks, int cmos) {
    if (!tables_built) build_tables();
    memset(z, 0, sizeof *z);
    z->bus = bus;
    z->hooks = hooks;
    z->cmos = cmos != 0;
    zr_z80_power(z);
}

void zr_z80_power(zr_z80 *z) {
    const zr_z80_bus *bus = z->bus;
    const zr_cpu_hooks *hooks = z->hooks;
    const uint8_t cmos = z->cmos;
    const uint64_t t = z->t;            /* time goes on: the machine's clock and its scheduled events count from it */
    memset(z, 0, sizeof *z);
    z->bus = bus;
    z->hooks = hooks;
    z->cmos = cmos;
    z->t = t;
    z->a = z->f = 0xFF;
    z->sp = 0xFFFF;
}

void zr_z80_reset(zr_z80 *z) {
    z->pc = 0;
    z->i = z->r = 0;
    z->iff1 = z->iff2 = z->im = 0;
    z->halted = z->ei = z->p = z->q = 0;
    z->nmi = 0;
    z->xy = 0;
}

void zr_z80_save(const zr_z80 *z, zr_state_writer *w) {
    zr_sw_u16(w, z->pc); zr_sw_u16(w, z->sp); zr_sw_u16(w, z->wz);
    const uint8_t bytes[] = {z->a, z->f, z->b, z->c, z->d, z->e, z->h, z->l, z->ixh, z->ixl, z->iyh, z->iyl,
                             z->a_, z->f_, z->b_, z->c_, z->d_, z->e_, z->h_, z->l_, z->i, z->r,
                             z->iff1, z->iff2, z->im, z->halted, z->ei, z->p, z->q, z->nmi, z->irq};
    zr_sw_bytes(w, bytes, sizeof bytes);
    zr_sw_u64(w, z->t);
}

void zr_z80_load(zr_z80 *z, zr_state_reader *r) {
    uint8_t bytes[31];
    z->pc = zr_sr_u16(r); z->sp = zr_sr_u16(r); z->wz = zr_sr_u16(r);
    zr_sr_bytes(r, bytes, sizeof bytes);
    uint8_t *const fields[] = {&z->a, &z->f, &z->b, &z->c, &z->d, &z->e, &z->h, &z->l, &z->ixh, &z->ixl, &z->iyh, &z->iyl,
                               &z->a_, &z->f_, &z->b_, &z->c_, &z->d_, &z->e_, &z->h_, &z->l_, &z->i, &z->r,
                               &z->iff1, &z->iff2, &z->im, &z->halted, &z->ei, &z->p, &z->q, &z->nmi, &z->irq};
    for (size_t i = 0; i < sizeof fields / sizeof *fields; ++i) *fields[i] = bytes[i];
    z->t = zr_sr_u64(r);
    z->xy = 0;
}
