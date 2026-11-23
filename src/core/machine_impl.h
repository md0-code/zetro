/* Inside the machine interface (machine.h): each board supplies this table
 * and its family's tables, and machine.c dispatches the public calls to it.
 * Not for the shell. */
#ifndef ZR_MACHINE_IMPL_H
#define ZR_MACHINE_IMPL_H

#include "machine.h"

/* What a board is built from: the model, a value for each option of the
 * family (indices into the option's values; an option the model does not
 * have is at its initial value) and the ROMs. `wanted` is for the test
 * machine only: the model that was asked for and could not be built. */
typedef struct zr_machine_setup {
    const zr_family *family;
    unsigned model;
    const unsigned *options;
    const zr_rom_index *roms;
    const zr_family *wanted_family;
    unsigned wanted_model;
} zr_machine_setup;

/* An entry a board does not have is NULL where the comment says so. */
typedef struct zr_machine_impl {
    void (*destroy)(void *self);
    const char *(*description)(const void *self);
    const char *(*error)(const void *self);
    void (*reset)(void *self, int power_on);
    void (*nmi)(void *self);                                    /* NULL: no NMI button */
    uint32_t (*clock)(const void *self);
    zr_run_status (*run)(void *self, uint64_t ticks);
    uint64_t (*time)(const void *self);
    uint64_t (*frame_period)(const void *self);
    int (*frame)(void *self, zr_frame *frame);
    size_t (*text)(void *self, char *out, size_t capacity);     /* NULL: cannot say */
    uint32_t (*audio_rate)(const void *self);
    size_t (*audio_pull)(void *self, int16_t *stereo, size_t frames);
    void (*key)(void *self, unsigned key, int down);
    void (*joystick)(void *self, unsigned stick, unsigned held); /* NULL: no sockets */
    int (*insert)(void *self, unsigned slot, zr_media *media, char *error, size_t capacity);
    zr_media *(*media)(const void *self, unsigned slot);
    int (*eject)(void *self, unsigned slot);
    unsigned (*activity)(const void *self);
    int (*control)(void *self, unsigned slot, unsigned command, uint32_t value);   /* NULL: none */
    void (*set_ports)(void *self, const zr_port_host *host);    /* NULL: no ports (the three of them) */
    size_t (*port_receive)(void *self, unsigned port, const uint8_t *data, size_t count);
    void (*port_modem)(void *self, unsigned port, int dcd, int cts, int dsr, int ri);
    /* The board's chunks (the header is machine.c's), and back: 0 when the state does not fit. */
    void (*save)(const void *self, zr_state_writer *w);
    int (*load)(void *self, zr_state_reader *r);
    /* The SHA-256 of each ROM in use, up to `max` of them, 32 bytes each; returns how many. */
    uint32_t (*rom_hashes)(const void *self, uint8_t *hashes, uint32_t max);
    void (*set_trace)(void *self, zr_trace_fn fn, void *ctx);   /* NULL: no bus */
    int (*peek)(void *self, uint32_t address, uint8_t *value);  /* NULL: no memory (and poke) */
    int (*poke)(void *self, uint32_t address, uint8_t value);
    struct zr_tape_deck *(*tape_deck)(void *self, unsigned slot);   /* NULL: no tape recorder */
    /* Snapshots (media/snap.h); both NULL: the machine takes none. */
    int (*snapshot_load)(void *self, const struct zr_snap *snap, char *error, size_t capacity);
    int (*snapshot_save)(void *self, struct zr_snap *snap);
    /* What auto-load does for a tape put into tape slot `slot` now (zr_machine_autoload): the text to type, NULL for none,
     * and whether to power cycle first. NULL entry: the model's `autoload` text, with a power cycle. */
    const char *(*autoload)(const void *self, unsigned slot, int *power_cycle);
    /* The tape slot's settings (zr_machine_tape_options); NULL: no tape recorder with any. */
    unsigned (*tape_options)(void *self, unsigned slot, unsigned bits);
} zr_machine_impl;

/* The families built in (machine.c lists them). */
extern const zr_family zr_stub_family;
extern const zr_family zr_jet_family;
extern const zr_family zr_hc_family;
extern const zr_family zr_hc128_family;
extern const zr_family zr_hc2000_family;
extern const zr_family zr_hc88_family;
extern const zr_family zr_cip_family;
extern const zr_family zr_tims_family;
extern const zr_family zr_cobra_family;
extern const zr_family zr_cip04_family;
extern const zr_family zr_cubz_family;
extern const zr_family zr_amic_family;
extern const zr_family zr_prae_family;

#endif /* ZR_MACHINE_IMPL_H */
