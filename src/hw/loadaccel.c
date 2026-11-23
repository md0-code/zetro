/* ZetRo - loader acceleration (loadaccel.h). ZetRo's own code: the rule and its proof are ZetRo's own; the instruction set is Zilog's Z80 CPU User Manual's, the
 * flags as src/hw/z80.c makes them. */
#include "loadaccel.h"

#include <string.h>

/* The registers the analysis follows, one bit each in a mask. */
enum { RA, RB, RC, RD, RE, RH, RL, RXH, RXL, RYH, RYL, NREG };
#define M(r) ((uint16_t)(1u << (r)))
#define NONE 0xFFu

enum { FC = ZR_Z80_C, FN = ZR_Z80_N, FP = ZR_Z80_P, FX = ZR_Z80_X, FH = ZR_Z80_H, FY = ZR_Z80_Y, FZ = ZR_Z80_Z, FS = ZR_Z80_S };
#define NOT_C ((uint8_t)(0xFFu & ~(unsigned)FC))

/* What an instruction is to the analysis. K_OTHER is anything that may run only on values that do not depend
 * on the counter; the others the slice can evaluate. */
enum { K_OTHER, K_INC8, K_DEC8, K_INC16, K_DEC16, K_DJNZ, K_LD8, K_ALU, K_BIT, K_BRANCH, K_IN };
/* How it goes on: to the next instruction, to `target` always, to `target` on a condition, or (RET cc,
 * CALL cc) out of the loop on a condition. */
enum { BR_NONE, BR_ALWAYS, BR_COND, BR_LEAVE };
/* The memory it reads: none, (HL), (IX+d), (IY+d), (BC), (DE), (nn) one byte, (nn) two bytes. */
enum { MEM_NONE, MEM_HL, MEM_IX, MEM_IY, MEM_BC, MEM_DE, MEM_NN, MEM_NN2 };

/* --- the CPU's registers by index ------------------------------------------------------------------ */

static uint8_t *reg_ptr(zr_z80 *z, unsigned r) {
    switch (r) {
    case RA: return &z->a;
    case RB: return &z->b;
    case RC: return &z->c;
    case RD: return &z->d;
    case RE: return &z->e;
    case RH: return &z->h;
    case RL: return &z->l;
    case RXH: return &z->ixh;
    case RXL: return &z->ixl;
    case RYH: return &z->iyh;
    default: return &z->iyl;
    }
}
static void regs_of(const zr_z80 *z, uint8_t v[NREG]) {
    v[RA] = z->a; v[RB] = z->b; v[RC] = z->c; v[RD] = z->d; v[RE] = z->e; v[RH] = z->h; v[RL] = z->l;
    v[RXH] = z->ixh; v[RXL] = z->ixl; v[RYH] = z->iyh; v[RYL] = z->iyl;
}

/* --- decoding ------------------------------------------------------------------------------------------ */

static const uint8_t PLAIN8[8] = {RB, RC, RD, RE, RH, RL, NONE, RA};
static const uint8_t CC_FLAG[4] = {FZ, FC, FP, FS};

/* The register a 3-bit field names; after DD or FD, H and L are the halves of IX or IY unless the
 * instruction also has an (IX+d) operand. */
static unsigned reg8(unsigned r, unsigned xy, int mem) {
    if (xy && !mem && (r == 4u || r == 5u)) return xy == 1u ? (r == 4u ? RXH : RXL) : (r == 4u ? RYH : RYL);
    return PLAIN8[r];
}
static uint16_t hl_mask(unsigned xy) { return xy == 0u ? (uint16_t)(M(RH) | M(RL)) : xy == 1u ? (uint16_t)(M(RXH) | M(RXL)) : (uint16_t)(M(RYH) | M(RYL)); }
/* BC DE HL (IX IY); 0 for SP, which the analysis does not take. */
static int pair_regs(unsigned p, unsigned xy, uint8_t *hi, uint8_t *lo) {
    static const uint8_t HI[3] = {RB, RD, RH}, LO[3] = {RC, RE, RL};
    if (p > 2u) return 0;
    *hi = HI[p];
    *lo = LO[p];
    if (p == 2u && xy) { *hi = xy == 1u ? RXH : RYH; *lo = xy == 1u ? RXL : RYL; }
    return 1;
}

/* The instruction at `pc`, when it is one the analysis takes: 1 and *in; 0 for any other (or bytes that are
 * not plain memory). */
static int decode(const zr_loadaccel *a, uint16_t pc, zr_loadaccel_insn *in) {
    uint8_t op, b;
    unsigned xy = 0, len = 0;
    memset(in, 0, sizeof *in);
    in->pc = pc;
    in->dst = in->src = NONE;
#define BYTE(v) do { if (!a->host.peek(a->host.ctx, (uint16_t)(pc + len), &(v))) return 0; ++len; } while (0)
#define COND(cc) do { in->kind = K_BRANCH; in->cflag = CC_FLAG[(cc) >> 1]; in->csense = (uint8_t)((cc) & 1u); in->frd = in->cflag; } while (0)
    for (;;) {
        BYTE(op);
        if (op != 0xDD && op != 0xFD) break;
        xy = op == 0xDD ? 1u : 2u;
        if (len >= 4u) return 0;
    }
    if (op == 0xCB) {
        /* BIT on a register or memory; the rotates, RES and SET on a register. */
        uint8_t op2;
        if (xy) {
            BYTE(b);
            in->disp = (int8_t)b;
        }
        BYTE(op2);
        const unsigned x = op2 >> 6, y = op2 >> 3 & 7u, r = op2 & 7u;
        const int mem = xy || r == 6u;
        if (x == 1u) {
            in->fwr = NOT_C;
            if (mem) {
                in->mem = (uint8_t)(xy ? MEM_HL + xy : MEM_HL);
                in->rd = hl_mask(xy);
            } else {
                in->kind = K_BIT;
                in->op = (uint8_t)y;
                in->src = PLAIN8[r];
                in->rd = M(in->src);
            }
        } else {
            if (mem) return 0;                                   /* writes memory */
            in->rd = in->wr = M(PLAIN8[r]);
            if (x == 0u) {
                in->fwr = 0xFF;
                if (y == 2u || y == 3u) in->frd = FC;
            }
        }
        in->len = (uint8_t)len;
        return 1;
    }
    if (op == 0xED) {
        /* IN r,(C) (IN F,(C)) and NEG; a prefix before ED does nothing. */
        uint8_t op2;
        BYTE(op2);
        const unsigned x = op2 >> 6, y = op2 >> 3 & 7u, zz = op2 & 7u;
        if (x != 1u) return 0;
        if (zz == 0u) {
            in->kind = K_IN;
            in->rd = (uint16_t)(M(RB) | M(RC));
            if (y != 6u) in->wr = M(PLAIN8[y]);
            in->fwr = NOT_C;
        } else if (zz == 4u) {
            in->rd = in->wr = M(RA);
            in->fwr = 0xFF;
        } else return 0;
        in->len = (uint8_t)len;
        return 1;
    }
    const unsigned x = op >> 6, y = op >> 3 & 7u, zz = op & 7u, p = y >> 1, q = y & 1u;
    switch (x) {
    case 0:
        switch (zz) {
        case 0:
            if (y == 0u) break;                                 /* NOP */
            if (y == 1u) return 0;                              /* EX AF,AF' */
            BYTE(b);
            in->target = (uint16_t)(pc + len + (int8_t)b);
            if (y == 2u) {                                      /* DJNZ */
                in->kind = K_DJNZ;
                in->branch = BR_COND;
                in->rd = in->wr = M(RB);
            } else if (y == 3u) in->branch = BR_ALWAYS;         /* JR */
            else {                                              /* JR cc */
                COND(y - 4u);
                in->branch = BR_COND;
            }
            break;
        case 1:
            if (q) return 0;                                    /* ADD HL,rr */
            if (!pair_regs(p, xy, &in->dst, &in->src)) return 0;
            BYTE(b);
            BYTE(b);
            in->wr = (uint16_t)(M(in->dst) | M(in->src));       /* LD rr,nn */
            break;
        case 2:
            if (y == 1u || y == 3u) {                           /* LD A,(BC) LD A,(DE) */
                in->mem = (uint8_t)(y == 1u ? MEM_BC : MEM_DE);
                in->rd = y == 1u ? (uint16_t)(M(RB) | M(RC)) : (uint16_t)(M(RD) | M(RE));
                in->wr = M(RA);
            } else if (y == 5u || y == 7u) {                    /* LD HL,(nn) LD A,(nn) */
                uint8_t lo, hi;
                BYTE(lo);
                BYTE(hi);
                in->target = (uint16_t)(hi << 8 | lo);
                in->mem = (uint8_t)(y == 5u ? MEM_NN2 : MEM_NN);
                in->wr = y == 5u ? hl_mask(xy) : M(RA);
            } else return 0;                                    /* the writes */
            break;
        case 3: {                                               /* INC rr DEC rr */
            if (!pair_regs(p, xy, &in->dst, &in->src)) return 0;
            in->kind = (uint8_t)(q ? K_DEC16 : K_INC16);
            in->rd = in->wr = (uint16_t)(M(in->dst) | M(in->src));
            break;
        }
        case 4: case 5:                                         /* INC r DEC r */
            if (y == 6u) return 0;                              /* (HL): writes memory */
            in->kind = (uint8_t)(zz == 4u ? K_INC8 : K_DEC8);
            in->dst = (uint8_t)reg8(y, xy, 0);
            in->rd = in->wr = M(in->dst);
            in->fwr = NOT_C;
            break;
        case 6:                                                 /* LD r,n */
            if (y == 6u) return 0;
            BYTE(b);
            in->kind = K_LD8;
            in->dst = (uint8_t)reg8(y, xy, 0);
            in->imm = b;
            in->wr = M(in->dst);
            break;
        default:
            if (y <= 3u) {                                      /* RLCA RRCA RLA RRA */
                in->rd = in->wr = M(RA);
                in->fwr = FC | FH | FN | FX | FY;
                if (y >= 2u) in->frd = FC;
            } else if (y == 5u) {                               /* CPL */
                in->rd = in->wr = M(RA);
                in->fwr = FH | FN | FX | FY;
            } else return 0;                                    /* DAA SCF CCF */
            break;
        }
        break;
    case 1:
        if (op == 0x76 || y == 6u) return 0;                    /* HALT; LD (HL),r writes */
        if (zz == 6u) {                                         /* LD r,(HL) LD r,(IX+d) */
            if (xy) {
                BYTE(b);
                in->disp = (int8_t)b;
            }
            in->mem = (uint8_t)(MEM_HL + xy);
            in->rd = hl_mask(xy);
            in->wr = M(reg8(y, xy, 1));
        } else {                                                /* LD r,r' */
            in->kind = K_LD8;
            in->dst = (uint8_t)reg8(y, xy, 0);
            in->src = (uint8_t)reg8(zz, xy, 0);
            in->rd = M(in->src);
            in->wr = M(in->dst);
        }
        break;
    case 2:                                                     /* ADD ADC SUB SBC AND XOR OR CP */
        in->op = (uint8_t)y;
        in->fwr = 0xFF;
        if (y == 1u || y == 3u) in->frd = FC;
        if (y != 7u) in->wr = M(RA);
        if (zz == 6u) {
            if (xy) {
                BYTE(b);
                in->disp = (int8_t)b;
            }
            in->mem = (uint8_t)(MEM_HL + xy);
            in->rd = (uint16_t)(M(RA) | hl_mask(xy));
        } else {
            in->kind = K_ALU;
            in->src = (uint8_t)reg8(zz, xy, 0);
            in->rd = (uint16_t)(M(RA) | M(in->src));
        }
        break;
    default:
        switch (zz) {
        case 0:                                                 /* RET cc: only when it does not return */
            COND(y);
            in->branch = BR_LEAVE;
            break;
        case 2: {                                               /* JP cc,nn */
            uint8_t lo, hi;
            BYTE(lo);
            BYTE(hi);
            in->target = (uint16_t)(hi << 8 | lo);
            COND(y);
            in->branch = BR_COND;
            break;
        }
        case 3:
            if (y == 0u) {                                      /* JP nn */
                uint8_t lo, hi;
                BYTE(lo);
                BYTE(hi);
                in->target = (uint16_t)(hi << 8 | lo);
                in->branch = BR_ALWAYS;
            } else if (y == 3u) {                               /* IN A,(n) */
                BYTE(b);
                in->kind = K_IN;
                in->imm = b;
                in->rd = in->wr = M(RA);
            } else return 0;
            break;
        case 4: {                                               /* CALL cc,nn: only when it does not call */
            uint8_t lo, hi;
            BYTE(lo);
            BYTE(hi);
            in->target = (uint16_t)(hi << 8 | lo);
            COND(y);
            in->branch = BR_LEAVE;
            break;
        }
        case 6:                                                 /* ALU A,n */
            BYTE(b);
            in->kind = K_ALU;
            in->op = (uint8_t)y;
            in->imm = b;
            in->rd = M(RA);
            in->fwr = 0xFF;
            if (y == 1u || y == 3u) in->frd = FC;
            if (y != 7u) in->wr = M(RA);
            break;
        default: return 0;
        }
        break;
    }
#undef BYTE
#undef COND
    in->len = (uint8_t)len;
    return 1;
}

/* --- the slice: the instructions that depend on the counter, evaluated -------------------------------- */

static uint8_t sz(unsigned v) { v &= 0xFFu; return (uint8_t)((v & (FS | FX | FY)) | (v ? 0u : FZ)); }
static uint8_t szp(unsigned v) {
    unsigned ones = 0;
    for (unsigned bit = 0; bit < 8u; ++bit) ones += v >> bit & 1u;
    return (uint8_t)(sz(v) | (ones & 1u ? 0u : FP));
}

/* As z80.c's alu(). */
static void alu(uint8_t *a, uint8_t *f, unsigned op, uint8_t v) {
    const unsigned x = *a;
    unsigned r;
    switch (op) {
    case 0: case 1:
        r = x + v + (op == 1u ? *f & FC : 0u);
        *f = (uint8_t)(sz(r) | ((x ^ v ^ r) & FH) | (((x ^ ~(unsigned)v) & (x ^ r) & 0x80u) >> 5) | (r >> 8 & 1u));
        *a = (uint8_t)r;
        break;
    case 2: case 3: case 7:
        r = x - v - (op == 3u ? *f & FC : 0u);
        *f = (uint8_t)(sz(r) | FN | ((x ^ v ^ r) & FH) | (((x ^ v) & (x ^ r) & 0x80u) >> 5) | (r >> 8 & 1u));
        if (op == 7u) *f = (uint8_t)((*f & ~(unsigned)(FX | FY)) | (v & (FX | FY)));
        else *a = (uint8_t)r;
        break;
    case 4: *a &= v; *f = (uint8_t)(szp(*a) | FH); break;
    case 5: *a ^= v; *f = szp(*a); break;
    default: *a |= v; *f = szp(*a); break;
    }
}

static uint32_t counter_of(const zr_loadaccel_loop *L, const uint8_t v[NREG]) {
    if (L->wide) return (uint32_t)v[L->hi] << 8 | v[L->lo];
    return L->lo == NONE ? 0u : v[L->lo];
}
static uint32_t counter_mask(const zr_loadaccel_loop *L) { return L->wide ? 0xFFFFu : L->lo == NONE ? 0u : 0xFFu; }

/* One turn of the slice from the CPU's registers with the counter at `c`: 1 when every branch goes as in the
 * observed turn, with the counter at the turn's end in *end. */
static int slice_run(const zr_loadaccel_loop *L, const zr_z80 *z, uint32_t c, uint32_t *end) {
    uint8_t v[NREG];
    regs_of(z, v);
    uint8_t f = z->f;
    if (L->wide) { v[L->hi] = (uint8_t)(c >> 8); v[L->lo] = (uint8_t)c; }
    else if (L->lo != NONE) v[L->lo] = (uint8_t)c;
    for (unsigned i = 0; i < L->slice_n; ++i) {
        const zr_loadaccel_insn *in = &L->slice[i];
        switch (in->kind) {
        case K_INC8: {
            const uint8_t r = (uint8_t)(v[in->dst] + 1u);
            f = (uint8_t)((f & FC) | sz(r) | ((r & 0x0Fu) ? 0u : FH) | (r == 0x80u ? FP : 0u));
            v[in->dst] = r;
            break;
        }
        case K_DEC8: {
            const uint8_t r = (uint8_t)(v[in->dst] - 1u);
            f = (uint8_t)((f & FC) | FN | sz(r) | ((r & 0x0Fu) == 0x0Fu ? FH : 0u) | (r == 0x7Fu ? FP : 0u));
            v[in->dst] = r;
            break;
        }
        case K_INC16: case K_DEC16: {
            const uint16_t w = (uint16_t)(((unsigned)v[in->dst] << 8 | v[in->src]) + (in->kind == K_INC16 ? 1u : 0xFFFFu));
            v[in->dst] = (uint8_t)(w >> 8);
            v[in->src] = (uint8_t)w;
            break;
        }
        case K_DJNZ:
            --v[RB];
            if ((v[RB] != 0) != in->expect) return 0;
            break;
        case K_LD8:
            v[in->dst] = in->src == NONE ? in->imm : v[in->src];
            break;
        case K_ALU:
            alu(&v[RA], &f, in->op, in->src == NONE ? in->imm : v[in->src]);
            break;
        case K_BIT: {
            const unsigned value = v[in->src], bit = value & 1u << in->op;
            f = (uint8_t)((f & FC) | FH | (bit ? 0u : FZ | FP) | (bit & FS) | (value & (FX | FY)));
            break;
        }
        default:                                                /* K_BRANCH */
            if ((((f & in->cflag) != 0) == in->csense) != in->expect) return 0;
            break;
        }
    }
    *end = counter_of(L, v);
    return 1;
}

/* --- the analysis of a turn ---------------------------------------------------------------------------- */

/* The registers that depend on the counter at the end of a turn when `seed` does at its start, and the flags
 * (any instruction with an input that depends on it makes all its outputs depend on it). */
static void propagate(const zr_loadaccel_insn *ins, unsigned n, uint16_t seed, uint16_t *regs, uint8_t *flags) {
    uint16_t tr = seed;
    uint8_t tf = 0;
    for (unsigned i = 0; i < n; ++i) {
        const zr_loadaccel_insn *in = &ins[i];
        if ((in->rd & tr) || (in->frd & tf)) {
            tr |= in->wr;
            tf |= in->fwr;
        } else {
            tr &= (uint16_t)~in->wr;
            tf &= (uint8_t)~in->fwr;
        }
    }
    *regs = tr;
    *flags = tf;
}

/* The address a memory operand reads, from the registers (they hold the turn's values: the analysis made sure). */
static uint16_t address_of(const zr_loadaccel_insn *in, const uint8_t v[NREG]) {
    switch (in->mem) {
    case MEM_HL: return (uint16_t)(v[RH] << 8 | v[RL]);
    case MEM_IX: return (uint16_t)((v[RXH] << 8 | v[RXL]) + in->disp);
    case MEM_IY: return (uint16_t)((v[RYH] << 8 | v[RYL]) + in->disp);
    case MEM_BC: return (uint16_t)(v[RB] << 8 | v[RC]);
    case MEM_DE: return (uint16_t)(v[RD] << 8 | v[RE]);
    default: return in->target;
    }
}

/* Whether what a memory operand reads is plain memory. */
static int mem_plain(const zr_loadaccel *a, const zr_loadaccel_insn *in, const uint8_t v[NREG]) {
    const uint16_t at = address_of(in, v);
    uint8_t byte;
    return a->host.peek(a->host.ctx, at, &byte) && (in->mem != MEM_NN2 || a->host.peek(a->host.ctx, (uint16_t)(at + 1u), &byte));
}

/* The turn a->path[0..n) with the CPU now at its end: the loop's description in a->loop (ok 0 when it cannot
 * be jumped through). `diff` the registers and `fdiff` the flags that differ between the two reads. */
static void analyse(zr_loadaccel *a, const zr_z80 *z, uint16_t diff, uint8_t fdiff) {
    zr_loadaccel_loop *L = &a->loop;
    const unsigned n = a->n;
    zr_loadaccel_insn ins[ZR_LOADACCEL_PATH];
    L->n = n;
    memcpy(L->pc, a->path, n * sizeof *L->pc);
    L->ok = 0;
    L->carried = 0;
    L->fcarried = 0;
    L->seed = 0;
    L->fseed = 0;
    L->slice_n = 0;
    L->mem_n = 0;
    L->code_first = 0;
    L->code_last = 0xFFFFu;
    if (z->hooks && z->hooks->refresh) return;
    /* Decoded, and the path as the instructions go: each to the next, the last (the read) back to the first. */
    for (unsigned i = 0; i < n; ++i) {
        zr_loadaccel_insn *in = &ins[i];
        if (zr_cpu_watched(z->hooks, a->path[i]) || !decode(a, a->path[i], in)) return;
        if ((in->kind == K_IN) != (i == n - 1u)) return;
        const uint16_t next = i + 1u < n ? a->path[i + 1u] : a->path[0];
        const uint16_t fall = (uint16_t)(in->pc + in->len);
        switch (in->branch) {
        case BR_ALWAYS: if (next != in->target) return; break;
        case BR_COND:
            if (in->target == fall || (next != in->target && next != fall)) return;
            in->expect = next == in->target;
            break;
        default: if (next != fall) return; break;      /* BR_LEAVE: did not leave */
        }
    }
    /* The bytes the code spans: a write there makes the analysis void. */
    uint32_t first = 0xFFFFu, last = 0;
    for (unsigned i = 0; i < n; ++i) {
        const uint32_t end = (uint32_t)ins[i].pc + ins[i].len - 1u;
        if (end > 0xFFFFu) { first = 0; last = 0xFFFFu; break; }
        if (ins[i].pc < first) first = ins[i].pc;
        if (end > last) last = end;
    }
    L->code_first = (uint16_t)first;
    L->code_last = (uint16_t)last;
    /* Read before written (live at the turn's start) and written, registers and flags. */
    uint16_t live = 0, written = 0;
    uint8_t flive = 0, fwritten = 0;
    for (unsigned i = 0; i < n; ++i) {
        live |= (uint16_t)(ins[i].rd & ~written);
        written |= ins[i].wr;
        flive |= (uint8_t)(ins[i].frd & ~fwritten);
        fwritten |= ins[i].fwr;
    }
    L->carried = (uint16_t)(live & written);
    L->fcarried = (uint8_t)(flive & fwritten);
    L->seed = (uint16_t)(diff & L->carried);
    L->fseed = (uint8_t)(fdiff & L->fcarried);
    /* A flag carried from turn to turn that changed: not a loop of this kind. */
    if (L->fseed) return;
    /* The counter: the carried registers that changed, and every carried register that comes to depend on them. */
    uint16_t counter = L->seed, tr;
    uint8_t tf;
    for (;;) {
        propagate(ins, n, counter, &tr, &tf);
        const uint16_t more = (uint16_t)(live & tr & ~counter);
        if (!more) break;
        counter |= more;
    }
    if (tf & flive) return;                                 /* a flag that depends on it is read before it is set */
    /* One register or a pair. */
    L->hi = L->lo = NONE;
    L->wide = 0;
    if (counter) {
        static const uint8_t PAIRS[5][2] = {{RB, RC}, {RD, RE}, {RH, RL}, {RXH, RXL}, {RYH, RYL}};
        for (unsigned r = 0; r < NREG; ++r)
            if (counter == M(r)) L->lo = (uint8_t)r;
        for (unsigned k = 0; k < 5u; ++k)
            if (counter == (uint16_t)(M(PAIRS[k][0]) | M(PAIRS[k][1]))) { L->hi = PAIRS[k][0]; L->lo = PAIRS[k][1]; L->wide = 1; }
        if (L->lo == NONE) return;
    }
    L->counter = counter;
    L->tainted = tr;
    L->tflags = tf;
    /* The slice, with what each of its instructions reads known: depending on the counter (evaluated), or as the
     * turn starts (the CPU's registers now; not written before in the turn by anything else). */
    uint8_t v[NREG];
    regs_of(z, v);
    uint16_t taint = counter, known = (uint16_t)((1u << NREG) - 1u);
    uint8_t ftaint = 0, fknown = 0xFF;
    unsigned exit_at = n;
    uint16_t seen_w = 0;
    uint8_t fseen_w = 0;
    for (unsigned i = 0; i < n; ++i) {
        const zr_loadaccel_insn *in = &ins[i];
        if (in->mem) {
            /* Plain memory at an address that is the same every turn: from registers as the turn starts. */
            static const uint16_t ADDRESS_REGS[8] = {0, M(RH) | M(RL), M(RXH) | M(RXL), M(RYH) | M(RYL), M(RB) | M(RC), M(RD) | M(RE), 0, 0};
            const uint16_t address_regs = ADDRESS_REGS[in->mem];
            if ((address_regs & taint) || (address_regs & ~known) || !mem_plain(a, in, v)) return;
            L->mem[L->mem_n++] = *in;
        }
        const int depends = (in->rd & taint) || (in->frd & ftaint);
        if (depends) {
            switch (in->kind) {
            case K_INC8: case K_DEC8: case K_INC16: case K_DEC16: case K_DJNZ: case K_LD8: case K_ALU: case K_BIT: case K_BRANCH: break;
            default: return;
            }
            if ((in->rd & ~taint & ~known) || (in->frd & ~ftaint & ~fknown)) return;
            if (L->slice_n >= ZR_LOADACCEL_PATH) return;
            L->slice[L->slice_n++] = *in;
            if (in->branch != BR_NONE && exit_at == n) exit_at = i;
            taint |= in->wr;
            ftaint |= in->fwr;
        } else {
            taint &= (uint16_t)~in->wr;
            ftaint &= (uint8_t)~in->fwr;
            known &= (uint16_t)~in->wr;
            fknown &= (uint8_t)~in->fwr;
        }
        /* What is written before the first branch the counter decides (that branch writes nothing but DJNZ's B,
         * the counter itself). */
        if (exit_at == n) {
            seen_w |= in->wr;
            fseen_w |= in->fwr;
        }
    }
    /* Every value that depends on the counter at the turn's end, other than the counter, must be set before that
     * branch: the turn after a jump starts with them as the turn before it left them, and a turn that leaves the
     * loop there must leave none of them behind. */
    if (taint != tr || ftaint != tf) return;
    if ((tr & ~counter & ~seen_w) || (tf & ~fseen_w)) return;
    L->ok = 1;
}

/* --- observing and jumping ------------------------------------------------------------------------------ */

static uint64_t min64(uint64_t x, uint64_t y) { return x < y ? x : y; }

/* The CPU's fields other than the registers the analysis follows, F and R and t: all must be the same. */
static int same_rest(const zr_z80 *p, const zr_z80 *z) {
    return p->pc == z->pc && p->sp == z->sp && p->wz == z->wz && p->i == z->i && ((p->r ^ z->r) & 0x80u) == 0 &&
           p->a_ == z->a_ && p->f_ == z->f_ && p->b_ == z->b_ && p->c_ == z->c_ && p->d_ == z->d_ && p->e_ == z->e_ &&
           p->h_ == z->h_ && p->l_ == z->l_ && p->iff1 == z->iff1 && p->iff2 == z->iff2 && p->im == z->im &&
           p->halted == z->halted && p->ei == z->ei && p->p == z->p && p->q == z->q && p->nmi == z->nmi &&
           p->irq == z->irq && p->xy == z->xy && p->cmos == z->cmos;
}

/* A read of the input just ended the turn a->path: if it is a turn of a loop that can be jumped through, the
 * jump. */
static void try_jump(zr_loadaccel *a, zr_sched *s, zr_z80 *z, const zr_tclock *c, uint64_t until) {
    const zr_z80 *p = &a->prev;
    const unsigned n = a->n;
    if (z->pc != a->path[0] || p->pc != a->path[0] || z->t <= p->t) return;
    if ((z->irq && z->iff1) || z->nmi || z->halted || z->ei || !same_rest(p, z)) return;
    uint8_t pv[NREG], nv[NREG];
    regs_of(p, pv);
    regs_of(z, nv);
    uint16_t diff = 0;
    for (unsigned r = 0; r < NREG; ++r)
        if (pv[r] != nv[r]) diff |= M(r);
    const uint8_t fdiff = (uint8_t)(p->f ^ z->f);
    zr_loadaccel_loop *L = &a->loop;
    if (!a->cached || L->n != n || memcmp(L->pc, a->path, n * sizeof *L->pc) || (diff & L->carried) != L->seed ||
        (fdiff & L->fcarried) != L->fseed) {
        analyse(a, z, diff, fdiff);
        a->cached = 1;
    } else if (L->ok)
        for (unsigned i = 0; i < L->mem_n; ++i)
            if (!mem_plain(a, &L->mem[i], nv)) return;
    if (!L->ok) return;
    /* Everything but the counter, what depends on it and R is as it was a turn ago. */
    if ((diff & ~L->tainted) || (fdiff & ~L->tflags)) return;
    const uint32_t mask = counter_mask(L);
    const uint32_t c_now = counter_of(L, nv), d = (c_now - counter_of(L, pv)) & mask;
    const uint64_t turn = z->t - p->t;
    const unsigned rstep = (unsigned)(z->r - p->r) & 0x7Fu;
    /* The bound: no deadline, end of the run or tape edge before the end of the turn after the jump. */
    const uint64_t at = zr_tclock_time(c, z->t);
    const uint64_t bound = min64(min64(until, s->next), a->host.level_until(a->host.ctx));
    if (bound <= at) return;
    const uint64_t k = c->ticks_per_tstate;
    uint64_t limit = c->t0 + (bound - c->ticks0 + k - 1u) / k;   /* the first T-state at or past the bound */
    if (limit <= z->t) return;
    uint64_t room = (limit - 1u - z->t) / turn;                  /* turns that end before it */
    if (a->host.quiet) {
        if (a->host.quiet(a->host.ctx, p->t, z->t) < z->t) return;          /* the turn seen was lengthened */
        const uint64_t q = a->host.quiet(a->host.ctx, z->t, z->t + room * turn);
        room = min64(room, (q - z->t) / turn);
    }
    if (room < 2u) return;
    /* Turns 0..j-1 jumped: each must go on in the loop with the counter it starts with. */
    uint64_t j = 0;
    const uint64_t most = min64(room - 1u, (uint64_t)1 << 24);
    if (!L->slice_n) j = most;
    else
        for (uint32_t cv = c_now; j < most; ++j, cv = (cv + d) & mask) {
            uint32_t end;
            if (!slice_run(L, z, cv, &end) || end != ((cv + d) & mask)) break;
        }
    if (!j) return;
    /* The jump. */
    const uint32_t cj = (uint32_t)((c_now + (uint64_t)d * j) & mask);
    if (L->wide) { *reg_ptr(z, L->hi) = (uint8_t)(cj >> 8); *reg_ptr(z, L->lo) = (uint8_t)cj; }
    else if (L->lo != NONE) *reg_ptr(z, L->lo) = (uint8_t)cj;
    z->r = (uint8_t)((z->r & 0x80u) | ((z->r + (uint64_t)rstep * j) & 0x7Fu));
    z->t += turn * j;
    ++a->jumps;
    a->tstates += turn * j;
}

static void observe(zr_loadaccel *a, zr_sched *s, zr_z80 *z, const zr_tclock *c, uint64_t until) {
    if (!a->disturbed && a->sampled == 1u && a->have_prev && a->n && a->n <= ZR_LOADACCEL_PATH) try_jump(a, s, z, c, until);
    a->prev = *z;
    a->have_prev = 1;
    a->n = 0;
    a->sampled = 0;
    a->disturbed = 0;
}

/* --- the run ------------------------------------------------------------------------------------------------ */

void zr_loadaccel_init(zr_loadaccel *a, const zr_loadaccel_host *host) {
    memset(a, 0, sizeof *a);
    a->host = *host;
}

void zr_loadaccel_restart(zr_loadaccel *a) {
    a->have_prev = 0;
    a->cached = 0;
    a->n = 0;
    a->sampled = 0;
    a->disturbed = 0;
}

uint64_t zr_loadaccel_run_z80(zr_loadaccel *a, zr_sched *s, zr_z80 *z, const zr_tclock *c, uint64_t until) {
    uint64_t now = zr_tclock_time(c, z->t);
    if (s->next <= now) a->disturbed = 1;
    zr_sched_run(s, now);
    while (now < until) {
        do {
            if (a->n < ZR_LOADACCEL_PATH) a->path[a->n] = z->pc;
            if (a->n <= ZR_LOADACCEL_PATH) ++a->n;
            zr_z80_step(z);
            if (a->sampled) observe(a, s, z, c, until);
            now = zr_tclock_time(c, z->t);
        } while (now < until && now < s->next);
        if (s->next <= now) a->disturbed = 1;
        zr_sched_run(s, now);
    }
    return now;
}
