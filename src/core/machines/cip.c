/* The Electronica CIP-01, CIP-02 and CIP-03 (machine.h, machine_impl.h): the family "cip" with three models, Spectrum-
 * compatible computers built on the shared core (spectrum.h) as this project's CIP core documents them (MiSTerRO/docs/cip/
 * README.md, models.md, hardware.md, validation.md) and builds them (core-cip/rtl/cip01_core.sv):
 *   - the Z80 at 3.5 MHz from the 14 MHz master clock, continuous (no stop while the video reads the RAM), the 448 x 312
 *     raster and the frame interrupt of the JET's values (nominal, as the core has them), no contention, no floating bus;
 *   - 64 KiB of RAM at 0000h-FFFFh; the EPROM overlays 0000h-3FFFh for reads while the boot latch (IC37A) selects it;
 *     writes under the EPROM always reach the RAM;
 *   - the EPROM: on the CIP-01 the 2 KiB copier (a 2716, A0-A10, mirrored eight times across the 16 KiB); on the CIP-03
 *     the 16 KiB BASIC; on the CIP-02 the copier and the BASIC, one of them selected by the "Startup" option (the core's
 *     firmware control: the physical board's selection is unverified and stays marked so);
 *   - port FE on A0 low, A6 high and A4 high (keyboard rows by the high address byte, EAR on bit 6, bits 5 and 7 high;
 *     border, MIC and the speaker written), the boot latch on A0 low, A6 high and A4 low (port EE: D0 = 0 selects the
 *     EPROM, D0 = 1 the RAM), nothing else decoded; a read of EE gives FFh;
 *   - the latch resets at power-up; the "Boot latch on reset" option says whether any reset does too (see below);
 *   - BRIGHT connected (the levels 205 and 255), FLASH;
 *   - two Sinclair joystick sockets wired into the matrix (the first on the keys 1-5, the second on 6-0); the other kinds
 *     of the shared core and the AY board are not original here;
 *   - a tape recorder: the CIP-01 (and the CIP-02 with the copier) loads its BASIC from tape into the RAM at 0000h.
 * No NMI button and no printer interface (the manuals name one; the core has none and gives no circuit).
 *
 * The firmware images of the model are one block, the BASIC first (the shared core maps its first 16 KiB at 0000h, finds
 * the tape routines in it and reads its character set for the text read-back) then the 2 KiB copier: on the CIP-01 the
 * BASIC image is never mapped, it only gives the traps their addresses and the read-back its font for the BASIC the tape
 * brings into the RAM (the same routines at the same places), so the CIP-01 runs without it (cip03.rom): the image is then
 * empty (FFh), the traps look for the routines in the RAM when the boot latch selects it and BASIC's code is there, and
 * the read-back takes the character set from that BASIC, else ZetRo's own. ZetRo's own code. */
#include "spectrum.h"
#include "font8x8.h"
#include "media/snap.h"
#include "text.h"

#include <stdio.h>
#include <string.h>

#define STATE_VERSION 1u
#define BASIC_SIZE 0x4000u
#define COPIER_SIZE 0x0800u
#define LOW 0x4000u                         /* the part of the address space the EPROM overlays */

enum { CIP01, CIP02, CIP03, MODEL_COUNT };
/* BASIC first, then the copier (see the header). The ROMs a model cannot run without: the CIP-01's BASIC image is optional (its
 * image is IMAGE01, the file of bit 0 may be missing). */
static const char *const ROMS01[] = {"cip01", NULL};
static const char *const IMAGE01[] = {"cip03", "cip01", NULL};
#define IMAGE01_OPTIONAL 1u
static const char *const ROMS02[] = {"cip03", "cip01", NULL};
static const char *const ROMS03[] = {"cip03", NULL};
/* The copier takes L, ENTER, ENTER to load from cassette and launch the BASIC the tape brings; the BASIC of the CIP-02/03 waits
 * for ENTER (the K cursor) and takes LOAD "" as on a 48K. What a tape put in by the user gets typed is the board's to say
 * (cip_autoload); the model's is what it starts. */
#define COPIER_LOAD "l\n\n"
#define BASIC_LOAD "\nj\"\"\n"
static const zr_model_desc MODELS[MODEL_COUNT] = {
    {"cip01", "CIP-01", ROMS01, COPIER_LOAD},
    {"cip02", "CIP-02", ROMS02, BASIC_LOAD},
    {"cip03", "CIP-03", ROMS03, BASIC_LOAD},
};
#define STARTUP_MODELS (1u << CIP02)

static const char *const STARTUPS[] = {"basic", "copier", NULL};
static const int STARTUP_LABELS[] = {ZR_T_CIP_STARTUP_BASIC, ZR_T_CIP_STARTUP_COPIER};
static const char *const LATCHES[] = {"kept", "cleared", NULL};
static const int LATCH_LABELS[] = {ZR_T_CIP_RESET_KEPT, ZR_T_CIP_RESET_CLEARED};
/* The sockets are wired to the matrix: the first to the keys 1-5, the second to 6-0 (the core's keyboard). */
#define STICK_LABELS(original)                                                                                         \
    {ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_NONE, 1),                                                                 \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR1, (original) == ZR_SPECTRUM_JOY_SINCLAIR1),                      \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR2, (original) == ZR_SPECTRUM_JOY_SINCLAIR2),                      \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_CURSOR, 0), ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_FULLER, 0),           \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_KEMPSTON, 0)}
static const int STICK1_LABELS[ZR_SPECTRUM_JOY_KINDS] = STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR2);
static const int STICK2_LABELS[ZR_SPECTRUM_JOY_KINDS] = STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR1);

enum {
    OPTION_JOYSTICK, OPTION_JOYSTICK2, OPTION_STARTUP, OPTION_RESET_LATCH, OPTION_AY,
    OPTION_TIMING,
    OPTION_INTERFACE, OPTION_COUNT = OPTION_INTERFACE + ZR_SPECTRUM_INTERFACE_OPTIONS
};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK1, ZR_SPECTRUM_JOY_KEMPSTON, STICK1_LABELS),
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick2", ZR_T_SPEC_JOYSTICK2, ZR_SPECTRUM_JOY_NONE, STICK2_LABELS),
    {"startup", ZR_T_CIP_STARTUP, STARTUPS, STARTUP_LABELS, 0, STARTUP_MODELS, ZR_OPTION_RESET, ZR_T_CIP_STARTUP_HINT + 1},
    /* A bare CPU reset is all the board's button does; the core clears the latch with it (see cip_reset). */
    {"reset_latch", ZR_T_CIP_RESET, LATCHES, LATCH_LABELS, 0, ZR_MODELS_ALL, 0, ZR_T_CIP_RESET_HINT + 1},
    ZR_SPECTRUM_OPTION_AY(ZR_OPTION_NOT_ORIGINAL),      /* the CIP core has no AY */
    ZR_SPECTRUM_OPTION_TIMING,
    ZR_SPECTRUM_OPTIONS_INTERFACE(ZR_MODELS_ALL),
};
enum { SLOT_TAPE, SLOT_INTERFACE, SLOT_COUNT = SLOT_INTERFACE + ZR_SPECTRUM_INTERFACE_SLOTS };
static const zr_media_desc MEDIA[SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    ZR_SPECTRUM_MEDIA_INTERFACE(ZR_MODELS_ALL),
};

/* The host keys the core's keyboard maps: the shared table without Caps Lock (the CIP core has no such key; the 48K's
 * Caps Lock is CAPS SHIFT and 2). */
enum { CS = ZR_SPECTRUM_KEY_CAPS_SHIFT, SS = ZR_SPECTRUM_KEY_SYMBOL_SHIFT, KZ = 1, KC = 3, KV = 4, KR = 13, KT = 14,
       K5 = 19, K0 = 20, K8 = 22, K7 = 23, K6 = 24, KP = 25, KO = 26, KL = 31, KK = 32, KJ = 33, KSPACE = 35, KM = 37, KN = 38 };
static const zr_combo_desc COMBOS[ZR_SPECTRUM_COMBOS - 1] = {
    {"Backspace", K0, CS}, {"Left", K5, CS}, {"Down", K6, CS}, {"Up", K7, CS}, {"Right", K8, CS}, {"Escape", KSPACE, CS},
    {".", KM, SS}, {",", KN, SS}, {"'", K7, SS}, {";", KO, SS}, {"-", KJ, SS}, {"=", KL, SS}, {"/", KV, SS},
    {"Shift+.", KT, SS}, {"Shift+,", KR, SS}, {"Shift+'", KP, SS}, {"Shift+;", KZ, SS}, {"Shift+-", K0, SS},
    {"Shift+=", KK, SS}, {"Shift+/", KC, SS}, {"Left Alt", SS, ZR_KEY_NONE},
};

typedef struct cip {
    zr_spectrum s;                          /* first: the shared core */
    unsigned kind;                          /* CIP01, CIP02, CIP03 */
    uint8_t copier;                         /* the EPROM is the 2 KiB copier (CIP-01, CIP-02 started as the copier) */
    uint8_t reset_latch;                    /* any reset clears the boot latch, as the core does */
    uint8_t latch;                          /* IC37A: 0 selects the EPROM at 0000h, 1 the RAM */
    uint8_t basic_rom;                      /* the BASIC's image (cip03.rom) is in s.rom, not FFh bytes */
    uint8_t ram0[LOW];                      /* the 16 KiB of RAM at 0000h (the rest is the core's `ram`) */
    const uint8_t *basic, *rom_copier;      /* the firmware, in s.rom */
} cip;

/* --- the memory map ------------------------------------------------------------------------------------ */

/* The RAM page of the 128K's numbering a family can peek and poke beyond the core's: the CIP's page 1 is the 16 KiB at
 * 0000h whichever of the EPROM and the RAM the boot latch has selected (machine.h's ZR_PAGE_ADDRESS). */
#define LOW_PAGE 1u

/* 0000h-3FFFh as the boot latch leaves it: the RAM both ways, or the EPROM for reads and the RAM for writes. */
static void remap(cip *h) {
    zr_bus *b = &h->s.bus;
    zr_bus_map_write(b, 0x0000u, LOW, h->ram0);
    if (h->latch) {
        zr_bus_map_read(b, 0x0000u, LOW, h->ram0);
        return;
    }
    if (!h->copier) {
        zr_bus_map_read(b, 0x0000u, LOW, h->basic);
        return;
    }
    for (unsigned p = 0; p < LOW / ZR_BUS_PAGE_SIZE; ++p)           /* A0-A10: the 2 KiB again and again */
        zr_bus_map_read(b, (uint16_t)(p * ZR_BUS_PAGE_SIZE), ZR_BUS_PAGE_SIZE,
                        h->rom_copier + (p * ZR_BUS_PAGE_SIZE) % COPIER_SIZE);
}

/* --- BASIC in the RAM -------------------------------------------------------------------------------------- */

static void own_font(cip *h);

/* BASIC's code is in the 16 KiB at `memory`: the byte loader every Spectrum-compatible BASIC of this project has is found
 * there (hw/traps.h; the BASIC the tape brings has a jump patched into it, so by its entry and exit alone). */
static int basic_in(const uint8_t *memory) {
    static const zr_traps_host none = {0};
    zr_traps probe;
    zr_traps_init_loose(&probe, NULL, &none, memory, LOW);
    return probe.has_load;
}

/* Without the BASIC's image the tape traps have no routines to find until the BASIC is in the RAM and selected: they look
 * then, where the boot latch selects the RAM and none was found yet. */
static void traps_in_ram(cip *h) {
    if (h->basic_rom || !h->latch || h->s.traps.has_load || h->s.traps.has_save) return;
    zr_spectrum_traps_rescan(&h->s, h->ram0, LOW);
}

/* --- ports ----------------------------------------------------------------------------------------------- */

/* IC37A takes D0 of any write to the ports with A0 low, A6 high and A4 low: D0 = 0 selects the EPROM, D0 = 1 the RAM.
 * A read gives nothing (FFh). */
static void latch_write(void *ctx, uint16_t port, uint8_t value) {
    cip *h = ctx;
    (void)port;
    h->latch = value & 1u;
    remap(h);
    traps_in_ram(h);
}

/* --- life cycle -------------------------------------------------------------------------------------------- */

/* Power-up resets the latch (the EPROM selected) and clears the RAM. The board's RESET button is the processor's reset
 * only, and the latch keeps what it was: the CIP-01's BASIC then restarts from the RAM at 0000h (and may crash: it
 * writes over its first bytes). The core resets the latch with every reset (its machine module takes the two controls
 * apart, its top level ties them), which brings the machine back to the EPROM, so BASIC has to be loaded again;
 * "Boot latch on reset" chooses: kept (the board's, in a new configuration) or cleared (the core's, not original). */
static void cip_reset(void *self, int power_on) {
    cip *h = self;
    zr_spectrum_reset(&h->s, power_on);
    if (power_on) memset(h->ram0, 0, sizeof h->ram0);
    if (power_on || h->reset_latch) h->latch = 0;
    remap(h);
}

static void *cip_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned kind = setup->model < MODEL_COUNT ? setup->model : CIP01;
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.ula.bright = 1;                         /* ATTR6 reaches BRIGHT (sheet 3): the levels 205 and 255 */
    cfg.ula.level_normal = 0xCDu;
    cfg.ula.level_bright = 0xFFu;
    cfg.fe_mask = 0x0051u;                      /* A0 low, A6 high, A4 high */
    cfg.fe_value = 0x0050u;
    cfg.nmi = 0;
    cfg.joystick = (uint8_t)o[OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_48K;
    cfg.spectrum_timing = o[OPTION_TIMING] != 0;
    cfg.ay = (uint8_t)o[OPTION_AY];
    if (kind == CIP01) {
        cfg.rom_keys = IMAGE01;                 /* the BASIC image, then the copier: only the copier is needed */
        cfg.rom_optional = IMAGE01_OPTIONAL;
    }
    zr_spectrum_interface_config(&cfg, o + OPTION_INTERFACE, SLOT_INTERFACE);
    cip *h = (cip *)zr_spectrum_create(&cfg, setup, sizeof(cip), error, capacity);
    if (!h) return NULL;
    zr_spectrum *s = &h->s;
    h->kind = kind;
    h->copier = kind == CIP01 || (kind == CIP02 && o[OPTION_STARTUP] != 0);
    h->reset_latch = o[OPTION_RESET_LATCH] != 0;
    h->basic = s->rom;
    h->rom_copier = kind == CIP03 ? NULL : s->rom + BASIC_SIZE;
    const zr_rom_file *basic_file = setup->roms ? zr_rom_index_find(setup->roms, zr_romdb_find("cip03")) : NULL;
    h->basic_rom = kind != CIP01 || (basic_file && basic_file->data);
    if (!h->basic_rom) own_font(h);
    zr_bus_add_io(&s->bus, 0x0051u, 0x0040u, NULL, latch_write, h);
    if (kind == CIP02 && h->copier) snprintf(s->description, sizeof s->description, "%s, %s", MODELS[kind].name, ZR_TXT(CIP_STARTUP_COPIER));
    cip_reset(h, 1);
    return h;
}

/* --- the picture as text ------------------------------------------------------------------------------------ */

/* While the copier is what the machine runs (the EPROM selected, and it is the copier) the screen is the copier's: it
 * draws its own 5-line characters, 8 pixels apart, on the lines 1-5 of each 8-line cell (its routine at 02D7h puts the
 * glyph of code n, the 5 bytes from 03EFh + 5n, down a line at a time from D = 51h): code 0 blank, 1-10 the digits,
 * 11-36 the letters, 37 the marker that follows the keys of its menu. The read-back matches each cell against those; an
 * empty cell is a space, a cell of anything else '?'. Otherwise the shared core reads the screen through BASIC's font. */
#define COPIER_FONT 0x03EFu
#define COPIER_GLYPHS 38u

static char copier_char(const cip *h, const uint8_t rows[8]) {
    if (!(rows[0] | rows[1] | rows[2] | rows[3] | rows[4] | rows[5] | rows[6] | rows[7])) return ' ';
    if (rows[0] || rows[6] || rows[7]) return '?';
    for (unsigned code = 1; code < COPIER_GLYPHS; ++code)
        if (!memcmp(h->rom_copier + COPIER_FONT + code * 5u, rows + 1, 5)) return code < 11u ? (char)('0' + code - 1u) : code < 37u ? (char)('A' + code - 11u) : '#';
    return '?';
}

/* The read-back's character set where there is no BASIC image: the BASIC's own once it is in the RAM at 0000h (its set is at
 * 3D00h there too), else ZetRo's own drawing of the Spectrum's. */
static void set_glyphs(cip *h, const uint8_t *font) {
    for (unsigned g = 0; g < 96u; ++g) {
        uint64_t word = 0;
        for (unsigned y = 0; y < 8u; ++y) word = word << 8 | font[g * 8u + y];
        h->s.glyph[g] = word;
    }
}
static void own_font(cip *h) { set_glyphs(h, &zr_font8x8_ascii[0][0]); }

static size_t cip_text(void *self, char *out, size_t capacity) {
    cip *h = self;
    if (!h->basic_rom) {
        if (h->latch && basic_in(h->ram0)) set_glyphs(h, h->ram0 + h->s.cfg.charset);
        else own_font(h);
    }
    if (!h->copier || h->latch || zr_spectrum_interface_rom(&h->s)) return zr_spectrum_text(self, out, capacity);
    const uint8_t *screen = h->s.screen;
    size_t n = 0;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[32];
        unsigned last = 0;
        for (unsigned col = 0; col < 32u; ++col) {
            uint8_t cell[8];
            for (unsigned y = 0; y < 8u; ++y) {
                const unsigned py = row * 8u + y;
                cell[y] = screen[(py & 0xC0u) << 5 | (py & 7u) << 8 | (py & 0x38u) << 2 | col];
            }
            line[col] = copier_char(h, cell);
            if (line[col] != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* --- memory for the checks -------------------------------------------------------------------------------------- */

static int cip_peek(void *self, uint32_t address, uint8_t *value) {
    cip *h = self;
    if (address >= ZR_PAGE_ADDRESS(LOW_PAGE, 0) && address < ZR_PAGE_ADDRESS(LOW_PAGE, LOW)) {
        *value = h->ram0[address - ZR_PAGE_ADDRESS(LOW_PAGE, 0)];
        return 1;
    }
    return zr_spectrum_peek(self, address, value);
}
static int cip_poke(void *self, uint32_t address, uint8_t value) {
    cip *h = self;
    if (address >= ZR_PAGE_ADDRESS(LOW_PAGE, 0) && address < ZR_PAGE_ADDRESS(LOW_PAGE, LOW)) {
        h->ram0[address - ZR_PAGE_ADDRESS(LOW_PAGE, 0)] = value;
        return 1;
    }
    return zr_spectrum_poke(self, address, value);
}

/* --- states ------------------------------------------------------------------------------------------------------- */

static void cip_save(const void *self, zr_state_writer *w) {
    const cip *h = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "CIP ", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, h->latch);
    zr_sw_packed(w, h->ram0, sizeof h->ram0);
    zr_sw_end(w);
}

static int cip_load(void *self, zr_state_reader *r) {
    cip *h = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "CIP ", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    h->latch = zr_sr_u8(r) & 1u;
    zr_sr_packed(r, h->ram0, sizeof h->ram0);
    if (!zr_sr_ok(r)) return 0;
    remap(h);
    traps_in_ram(h);
    return 1;
}

/* --- snapshots ---------------------------------------------------------------------------------------------------- */

/* A snapshot is of a 48K machine and holds no ROM: where the BASIC is the EPROM it is selected (the 48K's own ROM); where
 * it is not (the CIP-01, the copier start) it has to be in the RAM already, and the snapshot refused while the copier is
 * what the machine runs. The 16 KiB at 0000h are the machine's own either way. */
static int basic_resident(const cip *h) { return !h->copier; }
static int basic_in_reach(const cip *h) { return basic_resident(h) || h->latch; }

static int cip_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    cip *h = self;
    if (!basic_in_reach(h)) {
        if (capacity) snprintf(error, capacity, "No BASIC in the %s: load it from tape first", MODELS[h->kind].name);
        return 0;
    }
    const uint8_t latch = h->latch;
    if (basic_resident(h)) h->latch = 0;
    remap(h);
    if (zr_spectrum_snapshot_load(self, snap, error, capacity)) return 1;
    h->latch = latch;
    remap(h);
    return 0;
}
static int cip_snapshot_save(void *self, struct zr_snap *snap) {
    cip *h = self;
    return basic_in_reach(h) && zr_spectrum_snapshot_save(self, snap);
}

/* What auto-load does for a tape put in now: with BASIC running from the RAM (the latch selects it and BASIC's code is there:
 * the tape brought it) its LOAD "" and no power cycle, which would lose it; otherwise the machine is started again and gets
 * what the EPROM is: the copier's keys for the copier (the CIP-01, the CIP-02 with the Startup option at Copier), BASIC's
 * LOAD "" for a BASIC EPROM. */
static const char *cip_autoload(const void *self, unsigned slot, int *power_cycle) {
    const cip *h = self;
    (void)slot;
    if (h->latch && basic_in(h->ram0)) {
        *power_cycle = 0;
        return BASIC_LOAD;
    }
    *power_cycle = 1;
    return h->copier ? COPIER_LOAD : BASIC_LOAD;
}

static const zr_machine_impl CIP_IMPL = {
    .destroy = zr_spectrum_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = cip_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = zr_spectrum_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = zr_spectrum_frame, .text = cip_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = zr_spectrum_key, .joystick = zr_spectrum_joystick,
    .insert = zr_spectrum_insert, .media = zr_spectrum_media, .eject = zr_spectrum_eject, .activity = zr_spectrum_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .save = cip_save, .load = cip_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = cip_peek, .poke = cip_poke,
    .snapshot_load = cip_snapshot_load, .snapshot_save = cip_snapshot_save,
    .autoload = cip_autoload,
};

const zr_family zr_cip_family = {
    .id = "cip", .name = "CIP",
    .models = MODELS, .model_count = MODEL_COUNT,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .keys = zr_spectrum_keys, .key_count = ZR_SPECTRUM_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = COMBOS, .combo_count = ZR_SPECTRUM_COMBOS - 1,
    .joysticks = 2,
    .impl = &CIP_IMPL, .create = cip_create,
};
