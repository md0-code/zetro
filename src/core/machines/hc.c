/* The ICE Felix HC-85, HC-90 and HC-91 (machine.h, machine_impl.h): the family "hc", Spectrum-compatible machines
 * built on the shared core (spectrum.h), as this project's HC core documents them (MiSTerRO/docs/hc/models.md,
 * hardware.md, interfaces.md) and builds them (core-hc/rtl/hc_family_core.sv):
 *   - the Z80 at 3.5 MHz from the 14 MHz master clock, 16 KiB of BASIC ROM at 0000h, 48 KiB of RAM at 4000h, the
 *     448 x 312 raster and the frame interrupt of the JET's core values (nominal, as the HC core has them);
 *   - port FE on A0 low and A6 high (HC-85) or A7 high (HC-90/91);
 *   - BRIGHT on the HC-90/91 only (the HC-85's attribute bit 6 is not connected);
 *   - the HC-85's video RAM arbitration: the processor's clock is stopped for the second half of each four-T-state
 *     slot while it has memory in 4000h-7FFFh on the bus (the core's half-slot model, nominal: the counters' gates
 *     are not transcribed);
 *   - the HC-90/91's configuration latch on port 7Eh (bit 0 the CP/M firmware, which needs Interface 1; bit 1 RAM at
 *     0000h with the upper half of the ROM at E000h; bit 2 locks the latch until reset; bit 3 the screen at C000h),
 *     read back on 7Eh, and, with Interface 1, ports C7h and C5h that switch on and off the alias that makes
 *     C000h-DFFFh reach the RAM at E000h-FFFFh;
 *   - the CP/M start: with Interface 1 and the "Startup" option at CP/M, a reset leaves the latch at 1, so the
 *     labelled family bridge ROM ("hc-cpm") starts the original HC-2000 Rev-E BIOS from it;
 *   - Interface 1 (hw/if1.h) as an option, with two diskette drives (slots a and b) and the serial port;
 *   - two joystick sockets on the matrix: the first on the keys 1-5 (Sinclair II), the second on 6-0 (Sinclair I);
 *     the other kinds of the shared core are not original here;
 *   - the AY board on the expansion connector (FFFDh/BFFDh, the whole low byte FDh decoded, A15 high);
 *   - a tape recorder.
 * ZetRo's own code. */
#include "spectrum.h"
#include "if1.h"
#include "media/disk.h"
#include "romdb.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define CPM_FONT 0x25ABu                    /* the BIOS's 4 x 8 character set in the bridge ROM, codes 20h-7Eh */
#define CPM_GLYPHS 95u

enum { HC85, HC90, HC91, MODEL_COUNT };
static const char *const ROMS85[] = {"hc85", "if1-hc85", NULL};
static const char *const ROMS90[] = {"hc90", "if1-hc90", "hc-cpm", NULL};
static const char *const ROMS91[] = {"hc91", "if1-hc90", "hc-cpm", NULL};
/* LOAD "": in keyword mode J gives LOAD, SYMBOL SHIFT and P a quote. */
static const zr_model_desc MODELS[MODEL_COUNT] = {
    {"hc85", "HC-85", ROMS85, "j\"\"\n"},
    {"hc90", "HC-90", ROMS90, "j\"\"\n"},
    {"hc91", "HC-91", ROMS91, "j\"\"\n"},
};
#define CONFIGURABLE ((1u << HC90) | (1u << HC91))

static const char *const SWITCH[] = {"off", "on", NULL};
static const char *const STARTUPS[] = {"basic", "cpm", NULL};
static const int STARTUP_LABELS[] = {ZR_T_HC_STARTUP_BASIC, ZR_T_HC_STARTUP_CPM};
static const char *const BAUDS[] = {"110", "300", "600", "1200", "2400", "4800", "9600", "19200", NULL};
static const uint32_t BAUD_RATES[] = {110, 300, 600, 1200, 2400, 4800, 9600, 19200};
/* The sockets are wired to the matrix: the first to the keys 1-5, the second to 6-0. */
#define STICK_LABELS(original)                                                                                         \
    {ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_NONE, 1),                                                                 \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR1, (original) == ZR_SPECTRUM_JOY_SINCLAIR1),                      \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_SINCLAIR2, (original) == ZR_SPECTRUM_JOY_SINCLAIR2),                      \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_CURSOR, 0), ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_FULLER, 0),           \
     ZR_SPECTRUM_STICK_LABEL(ZR_SPECTRUM_JOY_KEMPSTON, 0)}
static const int STICK1_LABELS[ZR_SPECTRUM_JOY_KINDS] = STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR2);
static const int STICK2_LABELS[ZR_SPECTRUM_JOY_KINDS] = STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR1);

enum {
    OPTION_JOYSTICK, OPTION_JOYSTICK2, OPTION_INTERFACE1, OPTION_STARTUP, OPTION_DRIVE_SOUND,
    OPTION_BAUD, OPTION_AY, OPTION_TIMING,
    OPTION_INTERFACE, OPTION_COUNT = OPTION_INTERFACE + ZR_SPECTRUM_INTERFACE_OPTIONS
};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick", ZR_T_SPEC_JOYSTICK1, ZR_SPECTRUM_JOY_KEMPSTON, STICK1_LABELS),
    ZR_SPECTRUM_OPTION_JOYSTICK("joystick2", ZR_T_SPEC_JOYSTICK2, ZR_SPECTRUM_JOY_NONE, STICK2_LABELS),
    /* CP/M is on the Interface 1's disks: starting in it fits the interface. */
    {"interface1", ZR_T_HC_INTERFACE1, SWITCH, NULL, 0, ZR_MODELS_ALL, ZR_OPTION_RESET, 0, NULL, "startup=cpm:on"},
    {"startup", ZR_T_HC_STARTUP, STARTUPS, STARTUP_LABELS, 0, CONFIGURABLE, ZR_OPTION_RESET, ZR_T_HC_STARTUP_HINT + 1},
    {"drive_sound", ZR_T_HC_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET, 0, "interface1=on"},
    /* The bytes of the serial port to and from the host: ZetRo's UART on the board's bits (not original). */
    {"serial_baud", ZR_T_HC_BAUD, BAUDS, NULL, 6, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_PORT, 0, "interface1=on"},
    ZR_SPECTRUM_OPTION_AY(0),                   /* the core's expansion-connector AY board */
    ZR_SPECTRUM_OPTION_TIMING,
    /* Interface 1 and an added interface both take the ROM at 0000h: with Interface 1 on there is no other. */
    ZR_SPECTRUM_OPTIONS_INTERFACE_RULES(ZR_MODELS_ALL, "interface1=off", "interface1=on:none"),
};

enum { SLOT_TAPE, SLOT_A, SLOT_B, SLOT_INTERFACE, SLOT_COUNT = SLOT_INTERFACE + ZR_SPECTRUM_INTERFACE_SLOTS };
static const zr_media_desc MEDIA[SLOT_COUNT] = {
    {"tape", ZR_T_MEDIA_TAPE, ZR_MEDIA_TAPE, "tap,tzx,pzx,csw,wav", ZR_MODELS_ALL},
    /* the Interface 1's drives; CP/M starts from its system disk in drive A */
    {"a", ZR_T_MEDIA_FLOPPY_A, ZR_MEDIA_FLOPPY, "dsk", ZR_MODELS_ALL, "interface1=on"},
    {"b", ZR_T_MEDIA_FLOPPY_B, ZR_MEDIA_FLOPPY, "dsk", ZR_MODELS_ALL, "interface1=on"},
    ZR_SPECTRUM_MEDIA_INTERFACE(ZR_MODELS_ALL),
};
static const zr_port_desc PORTS[] = {{"serial", ZR_T_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL, "interface1=on"}};   /* the Interface 1's */

typedef struct hc {
    zr_spectrum s;                          /* first: the shared core */
    unsigned kind;                          /* HC85, HC90, HC91 */
    uint8_t configurable;                   /* port 7Eh (HC-90/91) */
    uint8_t if1_on, startup_cpm;
    /* the board's latches (saved) */
    uint8_t config;                         /* port 7Eh */
    uint8_t alias;                          /* C7h on, C5h off */
    uint8_t ram0[0x4000];                   /* the 16 KiB of RAM under the ROM (HC-90/91) */
    /* the firmware, in s.rom */
    const uint8_t *basic, *eprom, *cpm;
    uint32_t eprom_size;
    /* Interface 1 */
    zr_if1 if1;
    zr_cpu_hooks hooks;
    uint8_t watch[8192];
    zr_media *disk_media[2];
    zr_disk *disk[2];
    zr_port_host ports;
    int has_ports;
    /* the CP/M BIOS's character set, for the text read-back: 8 rows of 4 bits in a word */
    uint32_t cpm_glyph[CPM_GLYPHS];
} hc;

/* --- the memory map ---------------------------------------------------------------------------------------- */

/* The CP/M firmware is selected (HC-90/91 with Interface 1, latch bit 0). */
static int cpm_firmware(const hc *h) { return h->configurable && h->if1_on && (h->config & 1u); }

/* 0000h-3FFFh, and on the HC-90/91 C000h-FFFFh and the screen, as the latches leave them. */
static void remap(hc *h) {
    zr_spectrum *s = &h->s;
    zr_bus *b = &s->bus;
    const int ram0 = h->configurable && (h->config & 2u);
    const uint8_t *rom = cpm_firmware(h) ? h->cpm : h->basic;
    if (h->if1_on && zr_if1_paged(&h->if1) && !ram0)
        for (unsigned p = 0; p < 16u; ++p) {
            const uint16_t at = (uint16_t)(p << 10);
            uint8_t *ram = zr_if1_ram_page(&h->if1, p);
            const uint8_t *eprom = zr_if1_rom_page(&h->if1, p);
            if (ram) zr_bus_map_ram(b, at, 1024u, ram);
            else if (eprom) zr_bus_map_rom(b, at, 1024u, eprom);
            else zr_bus_unmap(b, at, 1024u, ZR_BUS_BOTH);
        }
    else if (ram0) zr_bus_map_ram(b, 0x0000u, 0x4000u, h->ram0);
    else zr_bus_map_rom(b, 0x0000u, 0x4000u, rom);
    if (!h->configurable) return;
    zr_bus_map_ram(b, 0xC000u, 0x2000u, s->ram + (h->alias ? 0xA000u : 0x8000u));
    if (ram0) zr_bus_map_rom(b, 0xE000u, 0x2000u, rom + 0x2000u);
    else zr_bus_map_ram(b, 0xE000u, 0x2000u, s->ram + 0xA000u);
    const uint8_t *screen = s->ram + (h->config & 8u ? 0x8000u : 0u);
    if (screen != s->screen) {
        zr_ula_catch_up(&s->ula, zr_spectrum_now(s));
        s->screen = screen;
    }
}

static void if1_paged(void *ctx) { remap(ctx); }

/* --- the CPU: Interface 1's fetches, the HC-85's video arbitration, the network WAIT ---------------------------- */

static int hc_before_fetch(void *ctx, uint16_t pc) {
    zr_if1_before_fetch(&((hc *)ctx)->if1, pc);
    return 0;
}
static void hc_after_fetch(void *ctx, uint16_t pc, uint8_t opcode) {
    (void)opcode;
    zr_if1_after_fetch(&((hc *)ctx)->if1, pc);
}

/* The HC-85 stops the processor's clock in the second half of each slot of four T-states while a memory cycle has an
 * address in 4000h-7FFFh: the T-states of the cycle with MREQ low (two of a fetch, three of a read or write) run only
 * in the first half. The network WAIT holds every cycle (a frame's worth at a time) while it lasts. */
static unsigned hc_stretch(zr_spectrum *s, int kind, uint16_t address) {
    hc *h = (hc *)s;
    unsigned added = 0;
    if (h->if1_on && zr_if1_held(&h->if1)) added += 69888u;
    if (h->kind == HC85 && !s->cfg.spectrum_timing && (address & 0xC000u) == 0x4000u &&
        (kind == ZR_Z80_FETCH || kind == ZR_Z80_READ || kind == ZR_Z80_WRITE)) {
        uint64_t t = s->z80.t + added;
        for (unsigned n = kind == ZR_Z80_FETCH ? 2u : 3u; n; --n, ++t)
            for (; (t & 3u) >= 2u; ++t) ++added;
    }
    return added;
}
/* Loader acceleration: a loop whose code lies outside 4000h-7FFFh and that reads no memory is never stopped. */
static uint64_t hc_quiet(zr_spectrum *s, uint64_t t, uint64_t limit) {
    hc *h = (hc *)s;
    if (h->if1_on && h->if1.net_wait) return t;
    if (h->kind != HC85 || s->cfg.spectrum_timing) return limit;
    const zr_loadaccel_loop *l = &s->accel.loop;
    if (!l->mem_n && l->code_first <= l->code_last && (l->code_last < 0x4000u || l->code_first >= 0x8000u)) return limit;
    return t;
}

/* --- ports ------------------------------------------------------------------------------------------------------ */

static uint8_t config_read(void *ctx, uint16_t port) {
    (void)port;
    return ((hc *)ctx)->config;
}
static void config_write(void *ctx, uint16_t port, uint8_t value) {
    hc *h = ctx;
    (void)port;
    if (h->config & 4u) return;
    h->config = value;
    remap(h);
}
static void alias_write(void *ctx, uint16_t port, uint8_t value) {
    hc *h = ctx;
    (void)value;
    h->alias = (port & 0xFFu) == 0xC7u;
    remap(h);
}
static uint8_t if1_read(void *ctx, uint16_t port) { return zr_if1_in(&((hc *)ctx)->if1, port); }
static void if1_write(void *ctx, uint16_t port, uint8_t value) { zr_if1_out(&((hc *)ctx)->if1, port, value); }

/* Interface 1's serial port to the host's end. */
static void serial_out(void *ctx, uint8_t byte) {
    hc *h = ctx;
    if (h->has_ports && h->ports.transmit) h->ports.transmit(h->ports.ctx, 0, byte);
}
static void serial_ready(void *ctx, int ready) {
    hc *h = ctx;
    if (h->has_ports && h->ports.outputs) h->ports.outputs(h->ports.ctx, 0, ready, ready);
}

/* --- life cycle ---------------------------------------------------------------------------------------------------- */

static void hc_reset(void *self, int power_on) {
    hc *h = self;
    zr_spectrum_reset(&h->s, power_on);
    if (power_on) {
        memset(h->ram0, 0, sizeof h->ram0);
        memset(h->if1.ram, 0, sizeof h->if1.ram);
    }
    h->config = (uint8_t)(h->configurable && h->if1_on && h->startup_cpm ? 1u : 0u);
    h->alias = 0;
    zr_if1_reset(&h->if1);
    remap(h);
}

static void *hc_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned kind = setup->model < MODEL_COUNT ? setup->model : HC85;
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.ula.bright = kind != HC85;
    cfg.ula.level_normal = kind == HC85 ? 0xFFu : 0xC0u;
    cfg.fe_mask = kind == HC85 ? 0x0041u : 0x0081u;
    cfg.fe_value = kind == HC85 ? 0x0040u : 0x0080u;
    cfg.joystick = (uint8_t)o[OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_48K;
    cfg.spectrum_timing = o[OPTION_TIMING] != 0;
    cfg.ay = (uint8_t)o[OPTION_AY];
    cfg.ay_mask = 0x80FFu;                      /* A15 and the whole low byte: FDh */
    cfg.ay_value = 0x80FDu;
    const int if1_on = o[OPTION_INTERFACE1] != 0;
    if (kind == HC85 || if1_on) {
        cfg.stretch = hc_stretch;
        cfg.stretch_quiet = hc_quiet;
    }
    zr_spectrum_interface_config(&cfg, o + OPTION_INTERFACE, SLOT_INTERFACE);
    if (zr_spectrum_interface_conflict(&cfg, if1_on, error, capacity)) return NULL;
    hc *h = (hc *)zr_spectrum_create(&cfg, setup, sizeof(hc), error, capacity);
    if (!h) return NULL;
    zr_spectrum *s = &h->s;
    h->kind = kind;
    h->configurable = (CONFIGURABLE >> kind & 1u) != 0;
    h->if1_on = (uint8_t)if1_on;
    h->startup_cpm = h->configurable && o[OPTION_STARTUP] != 0;
    /* The firmware, as the model's ROM keys put it into s.rom: BASIC, Interface 1, the CP/M bridge. */
    h->basic = s->rom;
    h->eprom_size = kind == HC85 ? 10240u : 16384u;
    h->eprom = s->rom + 0x4000u;
    h->cpm = h->configurable ? s->rom + 0x4000u + h->eprom_size : NULL;
    if (h->cpm)
        for (unsigned g = 0; g < CPM_GLYPHS; ++g) {
            uint32_t word = 0;
            for (unsigned y = 0; y < 8u; ++y) word = word << 4 | (h->cpm[CPM_FONT + g * 8u + y] & 15u);
            h->cpm_glyph[g] = word;
        }
    /* Interface 1: always built (its drives keep their disks), wired only when fitted. */
    const zr_if1_board board = {.ctx = h, .paged = if1_paged, .serial_out = serial_out, .serial_ready = serial_ready};
    const unsigned baud = o[OPTION_BAUD] < sizeof BAUD_RATES / sizeof BAUD_RATES[0] ? o[OPTION_BAUD] : 6u;
    zr_if1_init(&h->if1, &s->host, &board, kind == HC85 ? ZR_IF1_HC85 : ZR_IF1_HC90, h->eprom, h->eprom_size, cfg.clock_hz,
                BAUD_RATES[baud], if1_on && o[OPTION_DRIVE_SOUND] != 0);
    if (if1_on) {
        zr_bus_add_io(&s->bus, 0x00FDu, 0x0005u, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FDu, 0x0085u, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00EFu, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00F7u, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00C5u, NULL, alias_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00C7u, NULL, alias_write, h);
        h->hooks.ctx = h;
        h->hooks.watch = h->watch;
        h->hooks.before_fetch = hc_before_fetch;
        h->hooks.after_fetch = hc_after_fetch;
        zr_if1_watch(h->watch);
        zr_spectrum_set_hooks(s, &h->hooks);
        if (zr_drivesnd_enabled(&h->if1.sound)) s->drives = &h->if1.sound;
        snprintf(s->description, sizeof s->description, "%s, %s", setup->family->models[kind].name, ZR_TXT(HC_INTERFACE1));
    }
    if (h->configurable) {
        zr_bus_add_io(&s->bus, 0x00FFu, 0x007Eu, config_read, config_write, h);
        zr_bus_watch(&s->bus, 0xC000u, 0x1C00u, 1);    /* the screen at C000h */
    }
    hc_reset(h, 1);
    return h;
}

/* The disks go back into their media when they have changed. */
static void flush_disk(hc *h, unsigned drive) {
    zr_disk *disk = h->disk[drive];
    if (!disk || !disk->dirty || !h->disk_media[drive]) return;
    char why[128];
    if (!zr_disk_save_media(disk, h->disk_media[drive], why, sizeof why)) snprintf(h->s.error, sizeof h->s.error, "%s", why);
}

static void hc_destroy(void *self) {
    hc *h = self;
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
    hc *h = self;
    const zr_run_status status = zr_spectrum_run(h, ticks);
    for (unsigned i = 0; i < 2u; ++i)
        if (h->disk[i] && h->disk[i]->dirty && !h->if1.fdd[i].motor) flush_disk(h, i);
    return status;
}

/* --- the screen as text ---------------------------------------------------------------------------------------------- */

/* Under the CP/M firmware the BIOS draws 64 columns of 4 x 8 characters; each cell is matched against its character set,
 * as it is or inverted. Otherwise the shared core's 32 columns through the BASIC ROM's. */
static size_t hc_text(void *self, char *out, size_t capacity) {
    hc *h = self;
    if (!cpm_firmware(h)) return zr_spectrum_text(self, out, capacity);
    const uint8_t *screen = h->s.screen;
    size_t n = 0;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[64];
        unsigned last = 0;
        for (unsigned col = 0; col < 64u; ++col) {
            uint32_t cell = 0;
            for (unsigned y = 0; y < 8u; ++y) {
                const unsigned py = row * 8u + y;
                const uint8_t byte = screen[(py & 0xC0u) << 5 | (py & 7u) << 8 | (py & 0x38u) << 2 | col >> 1];
                cell = cell << 4 | (col & 1u ? byte & 15u : byte >> 4);
            }
            char ch = '?';
            for (unsigned g = 0; g < CPM_GLYPHS; ++g)
                if (h->cpm_glyph[g] == cell || h->cpm_glyph[g] == ~cell) { ch = (char)(0x20u + g); break; }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* --- media ------------------------------------------------------------------------------------------------------------- */

static int hc_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    hc *h = self;
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
    const hc *h = self;
    if (slot == SLOT_A || slot == SLOT_B) return h->disk_media[slot - SLOT_A];
    return zr_spectrum_media(self, slot);
}

static int hc_eject(void *self, unsigned slot) {
    hc *h = self;
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
    const hc *h = self;
    const unsigned drives = h->if1_on ? zr_if1_activity(&h->if1) : 0u;
    return zr_spectrum_activity(self) | (drives & 1u ? 1u << SLOT_A : 0u) | (drives & 2u ? 1u << SLOT_B : 0u);
}

/* --- the serial port ---------------------------------------------------------------------------------------------------- */

static void hc_set_ports(void *self, const zr_port_host *host) {
    hc *h = self;
    h->has_ports = host != NULL;
    if (host) h->ports = *host;
    zr_if1_set_loopback(&h->if1, host && host->loopback && host->loopback(host->ctx, 0));
    if (host && host->outputs) {
        const int ready = h->if1_on && zr_if1_serial_cts(&h->if1);
        host->outputs(host->ctx, 0, ready, ready);
    }
}
static size_t hc_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    hc *h = self;
    if (port) return 0;
    return h->if1_on ? zr_if1_receive(&h->if1, data, count) : count;
}
static void hc_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    (void)dcd; (void)dsr; (void)ri;
    if (!port) zr_if1_serial_ready_in(&((hc *)self)->if1, cts);
}

/* --- states ------------------------------------------------------------------------------------------------------------- */

static void hc_save(const void *self, zr_state_writer *w) {
    const hc *h = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "HC  ", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, h->config);
    zr_sw_u8(w, h->alias);
    zr_sw_packed(w, h->ram0, sizeof h->ram0);
    zr_sw_end(w);
    if (h->if1_on) zr_if1_save(&h->if1, w);
}

static int hc_load(void *self, zr_state_reader *r) {
    hc *h = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "HC  ", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    h->config = zr_sr_u8(r);
    h->alias = zr_sr_u8(r) & 1u;
    zr_sr_packed(r, h->ram0, sizeof h->ram0);
    if (!zr_sr_ok(r)) return 0;
    if (h->if1_on && !zr_if1_load(&h->if1, r)) return 0;
    if (!h->configurable) h->config = h->alias = 0;
    remap(h);
    /* The drives' sound starts afresh, in step with the beeper's next frame. */
    const zr_beeper *b = &h->s.beeper;
    zr_drivesnd_reset(&h->if1.sound, b->num ? (b->done * b->den + b->num - 1u) / b->num : 0u);
    return 1;
}

/* --- snapshots --------------------------------------------------------------------------------------------------------- */

/* A snapshot is of a 48K machine: Interface 1 paged out and the HC-90/91's latch cleared first. */
static int hc_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    hc *h = self;
    const uint8_t phantom = h->if1.phantom, config = h->config, alias = h->alias;
    h->if1.phantom = 0;
    h->config = h->alias = 0;
    remap(h);
    if (zr_spectrum_snapshot_load(self, snap, error, capacity)) return 1;
    h->if1.phantom = phantom;
    h->config = config;
    h->alias = alias;
    remap(h);
    return 0;
}
static int hc_snapshot_save(void *self, struct zr_snap *snap) {
    hc *h = self;
    if (zr_if1_paged(&h->if1) || (h->config & 0x0Bu) || h->alias) return 0;
    return zr_spectrum_snapshot_save(self, snap);
}

static const zr_machine_impl HC_IMPL = {
    .destroy = hc_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = hc_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = hc_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = zr_spectrum_frame, .text = hc_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = zr_spectrum_key, .joystick = zr_spectrum_joystick,
    .insert = hc_insert, .media = hc_media, .eject = hc_eject, .activity = hc_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .set_ports = hc_set_ports, .port_receive = hc_port_receive, .port_modem = hc_port_modem,
    .save = hc_save, .load = hc_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = zr_spectrum_peek, .poke = zr_spectrum_poke,
    .snapshot_load = hc_snapshot_load, .snapshot_save = hc_snapshot_save,
};

const zr_family zr_hc_family = {
    .id = "hc", .name = "HC",
    .models = MODELS, .model_count = MODEL_COUNT,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .ports = PORTS, .port_count = 1,
    .keys = zr_spectrum_keys, .key_count = ZR_SPECTRUM_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = zr_spectrum_combos, .combo_count = ZR_SPECTRUM_COMBOS,
    .joysticks = 2,
    .impl = &HC_IMPL, .create = hc_create,
};
