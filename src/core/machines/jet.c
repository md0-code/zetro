/* The Electromagnetica JET (machine.h, machine_impl.h): the family "jet"
 * with its one model, a ZX Spectrum 48K-compatible as this project's JET
 * core documents it (MiSTerRO/docs/jet/hardware.md, core-jet/rtl/
 * machine_core.sv): the Z80 at 3.5 MHz from a 14 MHz master clock, the
 * 16 KiB JET ROM at 0000h, 48 KiB of RAM at 4000h, port FE decoded on A0
 * low (the keyboard rows by the high address byte, EAR on bit 6, bits 5
 * and 7 high; border, MIC and speaker written), the Kempston joystick on
 * port 1F (the low address byte), no memory contention, no floating bus
 * (unmapped reads FFh; the not original "Spectrum timing" option gives it a
 * Sinclair 48K's, spectrum.h), the frame interrupt from line 248 for 32 T-states,
 * a tape recorder. The board is the Spectrum-compatible core alone
 * (spectrum.h). The other joystick kinds and the AY board are the core's,
 * not original on the JET. ZetRo's own code. */
#include "spectrum.h"
#include "text.h"

static const char *const ROMS[] = {"jet", NULL};
/* LOAD "": in keyword mode J gives LOAD, SYMBOL SHIFT and P a quote. */
static const zr_model_desc MODELS[] = {{"jet", "JET", ROMS, "j\"\"\n"}};

/* The JET's socket is a Kempston port; the other kinds are not original (labels in the order of zr_spectrum_sticks). */
static const int STICK_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_KEMPSTON);
enum { OPTION_JOYSTICK, OPTION_TIMING, OPTION_AY,
       OPTION_INTERFACE, OPTION_COUNT = OPTION_INTERFACE + ZR_SPECTRUM_INTERFACE_OPTIONS };
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK, ZR_SPECTRUM_JOY_KEMPSTON, STICK_LABELS),
    ZR_SPECTRUM_OPTION_TIMING,
    ZR_SPECTRUM_OPTION_AY(ZR_OPTION_NOT_ORIGINAL),          /* the JET core has no AY */
    ZR_SPECTRUM_OPTIONS_INTERFACE(ZR_MODELS_ALL),
};
enum { SLOT_TAPE, SLOT_INTERFACE, SLOT_COUNT = SLOT_INTERFACE + ZR_SPECTRUM_INTERFACE_SLOTS };
static const zr_media_desc MEDIA[SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    ZR_SPECTRUM_MEDIA_INTERFACE(ZR_MODELS_ALL),
};

static void *jet_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.kempston_mask = 0x00FFu;
    cfg.kempston_value = 0x001Fu;
    cfg.nmi = 1;                                /* the core's NMI key (F10) */
    cfg.joystick = (uint8_t)setup->options[OPTION_JOYSTICK];
    cfg.ay = (uint8_t)setup->options[OPTION_AY];
    cfg.sinclair = ZR_ULA_SINCLAIR_48K;         /* the JET is a 48K */
    cfg.spectrum_timing = setup->options[OPTION_TIMING] != 0;
    zr_spectrum_interface_config(&cfg, setup->options + OPTION_INTERFACE, SLOT_INTERFACE);
    return zr_spectrum_create(&cfg, setup, sizeof(zr_spectrum), error, capacity);
}

const zr_family zr_jet_family = {
    .id = "jet", .name = "JET",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .keys = zr_spectrum_keys, .key_count = ZR_SPECTRUM_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = zr_spectrum_combos, .combo_count = ZR_SPECTRUM_COMBOS,
    .joysticks = 1,
    .impl = &zr_spectrum_impl, .create = jet_create,
};
