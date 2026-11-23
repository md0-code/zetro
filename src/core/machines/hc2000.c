/* The ICE Felix HC-2000 (machine.h, machine_impl.h): the family "hc2000" (named "HC" on the setup screen) with its one
 * model, a Spectrum-compatible with CP/M on the shared core (spectrum.h) as this project's HC core documents it
 * (docs/hc/models.md, interfaces.md "Configuration and CP/M RAM", validation.md "Expansion hardware checks";
 * core-hc/rtl/hc_family_core.sv, model 5):
 *   - the Z80 at 3.5 MHz from the 14 MHz master clock; a 16 KiB BASIC ROM and a 16 KiB CP/M ROM; 64 KiB of RAM as four 16 KiB
 *     pages (physical page 3 at 0000h when the latch says so, 5 at 4000h, 2 at 8000h, 0 at C000h); no memory contention and no
 *     floating bus unless the not original Spectrum timing is on;
 *   - the configuration latch on port 7Eh (bit 0 the CP/M ROM, bit 1 RAM at 0000h with the ROM's upper half at E000h, bit 2
 *     locks the latch until reset, bit 3 the screen in the RAM at C000h), read back; ports C7h and C5h, the alias that makes
 *     C000h-DFFFh reach the RAM at E000h-FFFFh; port FE on A0 low and A7 high; the "Startup" option at CP/M leaves the latch
 *     at 1 at reset;
 *   - the built-in 8272 floppy controller on ports 85h and 87h with its control latch on 05h/07h and its two drives (slots
 *     a and b): the HC's Interface 1 board's controller, drives and sounds (hw/if1.h) without its EPROM, serial port and
 *     network, which this machine has not (the RTL decodes the controller the same way with the interface or built in);
 *   - two joystick sockets on the matrix, as the HC-85/90/91 have them (hc.c); the AY board of the expansion connector;
 *   - a tape recorder.
 * ZetRo's own code. */
#include "spectrum.h"
#include "if1.h"
#include "media/disk.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define PAGE 0x4000u
#define CPM_FONT 0x25ABu                    /* the CP/M ROM's 4 x 8 character set: 95 glyphs of 8 bytes */

static const char *const ROMS[] = {"hc2000-0", "hc2000-1", NULL};
/* LOAD "": in keyword mode J gives LOAD, SYMBOL SHIFT and P a quote. */
static const zr_model_desc MODELS[] = {{"hc2000", "HC-2000", ROMS, "j\"\"\n"}};

static const char *const SWITCH[] = {"off", "on", NULL};
static const char *const STARTUPS[] = {"basic", "cpm", NULL};
static const int STARTUP_LABELS[] = {ZR_T_HC_STARTUP_BASIC, ZR_T_HC_STARTUP_CPM};
/* The sockets are wired to the matrix: the first to the keys 1-5, the second to 6-0 (hc.c). */
static const int STICK1_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR2);
static const int STICK2_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR1);

enum { OPTION_JOYSTICK, OPTION_JOYSTICK2, OPTION_STARTUP, OPTION_DRIVE_SOUND, OPTION_AY,
       OPTION_TIMING,
       OPTION_INTERFACE, OPTION_COUNT = OPTION_INTERFACE + ZR_SPECTRUM_INTERFACE_OPTIONS };
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK1, ZR_SPECTRUM_JOY_KEMPSTON, STICK1_LABELS),
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick2", ZR_T_SPEC_JOYSTICK2, ZR_SPECTRUM_JOY_NONE, STICK2_LABELS),
    {"startup", ZR_T_HC_STARTUP, STARTUPS, STARTUP_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
    {"drive_sound", ZR_T_HC_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET},
    ZR_SPECTRUM_OPTION_AY(0),                   /* the core's expansion-connector AY board */
    ZR_SPECTRUM_OPTION_TIMING,
    ZR_SPECTRUM_OPTIONS_INTERFACE(ZR_MODELS_ALL),
};

enum { SLOT_TAPE, SLOT_A, SLOT_B, SLOT_INTERFACE, SLOT_COUNT = SLOT_INTERFACE + ZR_SPECTRUM_INTERFACE_SLOTS };
static const zr_media_desc MEDIA[SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    {"a", ZR_T_MEDIA_FLOPPY_A, ZR_MEDIA_FLOPPY, "dsk,edsk,img", ZR_MODELS_ALL},
    {"b", ZR_T_MEDIA_FLOPPY_B, ZR_MEDIA_FLOPPY, "dsk,edsk,img", ZR_MODELS_ALL},
    ZR_SPECTRUM_MEDIA_INTERFACE(ZR_MODELS_ALL),
};

typedef struct hc2000 {
    zr_spectrum s;                          /* first: the shared core */
    uint8_t startup_cpm;
    /* the board's latches (saved) */
    uint8_t config;                         /* port 7Eh */
    uint8_t alias;                          /* C7h on, C5h off */
    uint8_t ram0[PAGE];                     /* physical RAM page 3: RAM at 0000h when the latch says so */
    const uint8_t *basic, *cpm;             /* the firmware, in s.rom */
    zr_if1 if1;                             /* the controller, its latch, drives and sounds */
    zr_media *disk_media[2];
    zr_disk *disk[2];
} hc2000;

/* --- the memory map ---------------------------------------------------------------------------------------- */

/* The memory as port 7E and the alias leave it: the ROM at 0000h or RAM page 3 there, with the ROM's upper half at E000h in
 * the second case; the alias moves C000h-DFFFh onto the upper half of page 0; the screen is page 5 or page 0. */
static void remap(hc2000 *h) {
    zr_spectrum *s = &h->s;
    zr_bus *b = &s->bus;
    const uint8_t *rom = h->config & 1u ? h->cpm : h->basic;
    uint8_t *const p5 = s->ram, *const p2 = s->ram + PAGE, *const p0 = s->ram + 2u * PAGE;
    if (h->config & 2u) zr_bus_map_ram(b, 0x0000u, PAGE, h->ram0);
    else zr_bus_map_rom(b, 0x0000u, PAGE, rom);
    zr_bus_map_ram(b, 0x4000u, PAGE, p5);
    zr_bus_map_ram(b, 0x8000u, PAGE, p2);
    zr_bus_map_ram(b, 0xC000u, 0x2000u, p0 + (h->alias ? 0x2000u : 0u));
    zr_bus_map_ram(b, 0xE000u, 0x2000u, p0 + 0x2000u);
    if (h->config & 2u) zr_bus_map_rom(b, 0xE000u, 0x2000u, rom + 0x2000u);
    const uint8_t *screen = h->config & 8u ? p0 : p5;
    if (screen != s->screen) {
        zr_ula_catch_up(&s->ula, zr_spectrum_now(s));   /* the picture so far is of the screen that goes */
        s->screen = screen;
    }
}

/* The DivIDE's and DivMMC's automatic mapping acts only while the BASIC ROM is at 0000h, not the CP/M ROM (port 7E bit 0): the entry
 * points it watches are the 48K ROM's. */
static int interface_rom(zr_spectrum *s) { return !(((const hc2000 *)s)->config & 1u); }

static uint8_t config_read(void *ctx, uint16_t port) {
    (void)port;
    return ((hc2000 *)ctx)->config;
}
static void config_write(void *ctx, uint16_t port, uint8_t value) {
    hc2000 *h = ctx;
    (void)port;
    if (h->config & 4u) return;                         /* locked until reset */
    h->config = value;
    remap(h);
}
static void alias_write(void *ctx, uint16_t port, uint8_t value) {
    hc2000 *h = ctx;
    (void)value;
    h->alias = (port & 0xFFu) == 0xC7u;                 /* C7 on, C5 off */
    remap(h);
}
static uint8_t if1_read(void *ctx, uint16_t port) { return zr_if1_in(&((hc2000 *)ctx)->if1, port); }
static void if1_write(void *ctx, uint16_t port, uint8_t value) { zr_if1_out(&((hc2000 *)ctx)->if1, port, value); }

/* RAM pages by their 128K number, for ZR_PAGE_ADDRESS: the 48K machine's 5, 2 and 0, and the board's 3. */
static uint8_t *page_of(zr_spectrum *s, unsigned page) {
    hc2000 *h = (hc2000 *)s;
    switch (page) {
    case 5: return s->ram;
    case 2: return s->ram + PAGE;
    case 0: return s->ram + 2u * PAGE;
    case 3: return h->ram0;
    default: return NULL;
    }
}

/* --- life cycle ---------------------------------------------------------------------------------------------------- */

static void hc_reset(void *self, int power_on) {
    hc2000 *h = self;
    zr_spectrum_reset(&h->s, power_on);
    if (power_on) memset(h->ram0, 0, sizeof h->ram0);
    h->config = (uint8_t)(h->startup_cpm ? 1u : 0u);     /* the CP/M start: the latch at 1 */
    h->alias = 0;
    zr_if1_reset(&h->if1);                              /* the controller held in reset, the motors stopped */
    remap(h);
}

static void *hc_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.fe_mask = 0x0081u;                              /* A0 low, A7 high */
    cfg.fe_value = 0x0080u;
    cfg.ram_size = 3u * PAGE;                           /* the pages 5, 2 and 0 of the 48K machine */
    cfg.page = page_of;
    cfg.joystick = (uint8_t)o[OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_48K;                 /* a Spectrum 48K in the timing option */
    cfg.spectrum_timing = o[OPTION_TIMING] != 0;
    cfg.ay = (uint8_t)o[OPTION_AY];
    cfg.ay_mask = 0x80FFu;                              /* A15 and the whole low byte: FDh */
    cfg.ay_value = 0x80FDu;
    zr_spectrum_interface_config(&cfg, o + OPTION_INTERFACE, SLOT_INTERFACE);
    cfg.interface_rom = interface_rom;
    hc2000 *h = (hc2000 *)zr_spectrum_create(&cfg, setup, sizeof *h, error, capacity);
    if (!h) return NULL;
    zr_spectrum *s = &h->s;
    h->startup_cpm = o[OPTION_STARTUP] != 0;
    h->basic = s->rom;
    h->cpm = s->rom + PAGE;
    /* The controller and the drives: the Interface 1 board's, with no EPROM (it never pages in) and no serial port. */
    const zr_if1_board board = {.ctx = h};
    zr_if1_init(&h->if1, &s->host, &board, ZR_IF1_HC90, NULL, 0, cfg.clock_hz, 9600, o[OPTION_DRIVE_SOUND] != 0);
    h->if1.entry = 0;
    zr_bus_add_io(&s->bus, 0x00FDu, 0x0005u, if1_read, if1_write, h);      /* 05h, 07h: the control latch */
    zr_bus_add_io(&s->bus, 0x00FDu, 0x0085u, if1_read, if1_write, h);      /* 85h, 87h: the controller */
    zr_bus_add_io(&s->bus, 0x00FFu, 0x00C5u, NULL, alias_write, h);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x00C7u, NULL, alias_write, h);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x007Eu, config_read, config_write, h);
    zr_bus_watch(&s->bus, 0xC000u, 0x1C00u, 1);         /* the screen at C000h */
    if (zr_drivesnd_enabled(&h->if1.sound)) s->drives = &h->if1.sound;
    hc_reset(h, 1);
    return h;
}

/* The disks go back into their media when they have changed. */
static void flush_disk(hc2000 *h, unsigned drive) {
    zr_disk *disk = h->disk[drive];
    if (!disk || !disk->dirty || !h->disk_media[drive]) return;
    char why[128];
    if (!zr_disk_save_media(disk, h->disk_media[drive], why, sizeof why)) snprintf(h->s.error, sizeof h->s.error, "%s", why);
}

static void hc_destroy(void *self) {
    hc2000 *h = self;
    if (!h) return;
    for (unsigned i = 0; i < 2u; ++i) {
        flush_disk(h, i);
        zr_if1_eject(&h->if1, i);
        zr_disk_close(h->disk[i]);
    }
    zr_spectrum_destroy(h);
}

/* After each run: a disk the guest wrote goes back into its media once its drive has stopped. */
static zr_run_status hc_run(void *self, uint64_t ticks) {
    hc2000 *h = self;
    const zr_run_status status = zr_spectrum_run(h, ticks);
    for (unsigned i = 0; i < 2u; ++i)
        if (h->disk[i] && h->disk[i]->dirty && !h->if1.fdd[i].motor) flush_disk(h, i);
    return status;
}

/* --- the screen as text ---------------------------------------------------------------------------------------------- */

/* Spectrum text in 32 columns through the BASIC ROM's character set, the CP/M ROM's in 64 of 4 x 8 pixels while that ROM is
 * the one selected. */
static size_t hc_text(void *self, char *out, size_t capacity) {
    hc2000 *h = self;
    if (!(h->config & 1u)) return zr_spectrum_text(self, out, capacity);
    return zr_spectrum_text_narrow(self, h->cpm + CPM_FONT, out, capacity);
}

/* --- media ------------------------------------------------------------------------------------------------------------- */

static int hc_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    hc2000 *h = self;
    if (slot == SLOT_TAPE) return zr_spectrum_insert(self, slot, media, error, capacity);
    if (slot != SLOT_A && slot != SLOT_B) return zr_spectrum_insert(self, slot, media, error, capacity);
    const unsigned drive = slot - SLOT_A;
    if (h->disk_media[drive]) {
        if (capacity) snprintf(error, capacity, "The slot is not empty");
        return 0;
    }
    zr_disk *disk = zr_disk_open(media, NULL, error, capacity);
    if (!disk) return 0;
    if (!zr_if1_insert(&h->if1, drive, disk, error, capacity)) {
        zr_disk_close(disk);
        return 0;
    }
    h->disk[drive] = disk;
    h->disk_media[drive] = media;
    return 1;
}

static zr_media *hc_media(const void *self, unsigned slot) {
    const hc2000 *h = self;
    if (slot == SLOT_A || slot == SLOT_B) return h->disk_media[slot - SLOT_A];
    return zr_spectrum_media(self, slot);
}

static int hc_eject(void *self, unsigned slot) {
    hc2000 *h = self;
    if (slot != SLOT_A && slot != SLOT_B) return zr_spectrum_eject(self, slot);
    const unsigned drive = slot - SLOT_A;
    if (!h->disk_media[drive]) return 0;
    flush_disk(h, drive);
    zr_if1_eject(&h->if1, drive);
    zr_disk_close(h->disk[drive]);
    h->disk[drive] = NULL;
    h->disk_media[drive] = NULL;
    return 1;
}

static unsigned hc_activity(const void *self) {
    const hc2000 *h = self;
    const unsigned drives = zr_if1_activity(&h->if1);
    return zr_spectrum_activity(self) | (drives & 1u ? 1u << SLOT_A : 0u) | (drives & 2u ? 1u << SLOT_B : 0u);
}

/* --- states ------------------------------------------------------------------------------------------------------------- */

static void hc_save(const void *self, zr_state_writer *w) {
    const hc2000 *h = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "HC20", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, h->config);
    zr_sw_u8(w, h->alias);
    zr_sw_packed(w, h->ram0, sizeof h->ram0);
    zr_sw_end(w);
    zr_if1_save(&h->if1, w);
}

static int hc_load(void *self, zr_state_reader *r) {
    hc2000 *h = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "HC20", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    h->config = zr_sr_u8(r);
    h->alias = zr_sr_u8(r) & 1u;
    zr_sr_packed(r, h->ram0, sizeof h->ram0);
    if (!zr_sr_ok(r) || !zr_if1_load(&h->if1, r)) return 0;
    h->if1.entry = 0;
    remap(h);
    /* The drives' sound starts afresh, in step with the beeper's next frame. */
    const zr_beeper *b = &h->s.beeper;
    zr_drivesnd_reset(&h->if1.sound, b->num ? (b->done * b->den + b->num - 1u) / b->num : 0u);
    return 1;
}

/* --- snapshots --------------------------------------------------------------------------------------------------------- */

/* A snapshot is of the 48K machine: the latch cleared first (the BASIC with RAM at 4000h-FFFFh). */
static int hc_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    hc2000 *h = self;
    const uint8_t config = h->config, alias = h->alias;
    h->config = h->alias = 0;
    remap(h);
    if (zr_spectrum_snapshot_load(self, snap, error, capacity)) {
        memset(h->ram0, 0, sizeof h->ram0);
        return 1;
    }
    h->config = config;
    h->alias = alias;
    remap(h);
    return 0;
}
static int hc_snapshot_save(void *self, struct zr_snap *snap) {
    hc2000 *h = self;
    if ((h->config & 0x0Bu) || h->alias) return 0;
    return zr_spectrum_snapshot_save(self, snap);
}

static const zr_machine_impl HC2000_IMPL = {
    .destroy = hc_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = hc_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = hc_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = zr_spectrum_frame, .text = hc_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = zr_spectrum_key, .joystick = zr_spectrum_joystick,
    .insert = hc_insert, .media = hc_media, .eject = hc_eject, .activity = hc_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .save = hc_save, .load = hc_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = zr_spectrum_peek, .poke = zr_spectrum_poke,
    .snapshot_load = hc_snapshot_load, .snapshot_save = hc_snapshot_save,
};

const zr_family zr_hc2000_family = {
    .id = "hc2000", .name = "HC",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .keys = zr_spectrum_keys, .key_count = ZR_SPECTRUM_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = zr_spectrum_combos, .combo_count = ZR_SPECTRUM_COMBOS,
    .joysticks = 2,
    .impl = &HC2000_IMPL, .create = hc_create,
};
