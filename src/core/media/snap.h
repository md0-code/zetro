/* Snapshots of the Spectrum-compatible machines: SNA, Z80 and SZX files read into, and written
 * from, one neutral structure that belongs to no machine. The formats' own details stop at this file's modules
 * (snap_sna.c, snap_z80.c, snap_szx.c); a board (machines/spectrum_snap.c) applies a snapshot to itself and captures
 * one from itself.
 *
 * What a snapshot holds: which machine it is of (a class the boards decide on, and the format's own identifier and
 * name for it), the Z80's registers with the alternate set, I, R, the interrupt flip-flops and mode, HALT and the
 * delay after EI, the border and the last values written to ports 7FFD and 1FFD, the AY's registers where there is
 * one, the T-states into the frame where the format has them, and the RAM as 16 KiB pages numbered as the 128K
 * machine numbers them (a 48K machine's three areas at 4000h, 8000h and C000h are the pages 5, 2 and 0). A snapshot
 * never holds a ROM.
 *
 * PC is the program counter as the processor holds it: a processor in HALT has already stepped past the HALT. The
 * formats that cannot say HALT (SNA, Z80) get the address of the HALT itself when written, and the SNA's stack word is
 * the writer's business: it pushes PC on a copy of the memory.
 *
 * Every length and offset of a file is checked; a damaged file gives a message and never a crash or an endless loop.
 * ZetRo's own code, written from the SNA, Z80 and SZX format descriptions. */
#ifndef ZR_SNAP_H
#define ZR_SNAP_H

#include <stddef.h>
#include <stdint.h>

#define ZR_SNAP_PAGES 8                     /* RAM pages of the 128K machines */
#define ZR_SNAP_PAGE_SIZE 0x4000u

/* The file formats. */
enum { ZR_SNAP_NONE, ZR_SNAP_SNA, ZR_SNAP_Z80, ZR_SNAP_SZX };

/* The classes of machine, as far as a board needs to tell them apart. OTHER is a machine the formats name but no
 * board of ZetRo is (Pentagon, Scorpion, the Timex machines, SamRam...): its identifier and name are kept. */
enum { ZR_SNAP_M_48K, ZR_SNAP_M_16K, ZR_SNAP_M_128K, ZR_SNAP_M_PLUS2, ZR_SNAP_M_PLUS2A, ZR_SNAP_M_PLUS3, ZR_SNAP_M_OTHER };

/* What else the file says of the machine it came from. */
enum {
    ZR_SNAP_X_INTERFACE1 = 1,               /* an Interface 1 was fitted */
    ZR_SNAP_X_ROM_PAGED = 2                 /* a ROM that is not the machine's own was paged in (Interface 1, MGT, Multiface) */
};

typedef struct zr_snap {
    unsigned format;                        /* ZR_SNAP_*: what it was read from; what a board leaves 0 */
    unsigned machine;                       /* ZR_SNAP_M_* */
    unsigned id;                            /* the format's identifier of the machine (SZX machine id, Z80 hardware mode, 0 for SNA) */
    char name[40];                          /* the machine in words, for messages: "ZX Spectrum 48K" */
    unsigned extras;                        /* ZR_SNAP_X_* */
    /* the processor */
    uint16_t pc, sp;
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2, ix, iy;
    uint8_t i, r;
    uint8_t iff1, iff2, im;                 /* 0 or 1, 0 or 1, 0 to 2 */
    uint8_t halted;                         /* in HALT (PC is past it) */
    uint8_t ei;                             /* no interrupt is accepted before the next instruction (the last was EI) */
    uint16_t wz;                            /* MEMPTR, where the file has it */
    uint8_t has_wz;
    /* the board */
    uint8_t border;                         /* 0 to 7 */
    uint8_t port_fe;                        /* the last value written to port FE, where the file has it */
    uint8_t port_7ffd, port_1ffd;           /* the last values written to the paging ports (0 on a 48K machine) */
    uint8_t ay_present;
    uint8_t ay_current;                     /* the AY register last selected */
    uint8_t ay[16];
    uint8_t has_tstates;
    uint32_t tstates;                       /* T-states since the frame interrupt began */
    /* the RAM: pages by their 128K number; bit n of `pages` says page n is in the snapshot */
    uint8_t pages;
    uint8_t ram[ZR_SNAP_PAGES][ZR_SNAP_PAGE_SIZE];
} zr_snap;

/* An empty snapshot, zeroed (about 128 KiB); NULL when out of memory. Free it with zr_snap_free. */
zr_snap *zr_snap_new(void);
void zr_snap_free(zr_snap *s);

/* Sets the machine of a snapshot: its class (ZR_SNAP_M_*), the format's identifier for it (0: none) and its name. */
void zr_snap_set_machine(zr_snap *s, unsigned machine, unsigned id, const char *name);

/* The format a file name's ending names (sna, z80, szx), ZR_SNAP_NONE for any other; and a format's ending. */
unsigned zr_snap_format_of_name(const char *name);
const char *zr_snap_extension(unsigned format);

/* Reads a snapshot from the bytes of a file. The format is told from the content (an SZX starts with its magic;
 * an SNA has one of the three sizes) and, when that does not settle it, from the file name's ending. `name` may be
 * NULL. Returns 1 with `snap` filled, or 0 with the reason in `error`. */
int zr_snap_read(const uint8_t *bytes, size_t size, const char *name, zr_snap *snap, char *error, size_t capacity);

/* The snapshot as the bytes of a file of `format`: SNA (48K, or the 128K variant for the 128K machines), Z80 version 3
 * with compressed pages, SZX with its RAM pages stored uncompressed. On success returns 1 with malloc'd bytes in
 * *bytes (free() them) and their count in *size; on failure 0 with the reason (a snapshot a format cannot hold:
 * an SNA with its stack in ROM, a machine the format has no number for). */
int zr_snap_write(const zr_snap *snap, unsigned format, uint8_t **bytes, size_t *size, char *error, size_t capacity);

/* The RAM page numbers a machine class has, as a bit mask over the 128K page numbers: 48K 5, 2, 0; 16K 5; the 128K
 * machines 0 to 7. 0 for OTHER. */
unsigned zr_snap_pages_of(unsigned machine);

#endif /* ZR_SNAP_H */
