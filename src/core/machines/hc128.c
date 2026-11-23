/* The ICE Felix HC-91 Plus (HC-128) (machine.h, machine_impl.h): the family "hc128" (named "HC" on the setup screen) with
 * its one model, a ZX Spectrum 128-style computer on the shared core (spectrum.h) as this project's HC core documents it
 * (docs/hc/models.md, interfaces.md "HC91 Plus paging and sound", README.md, validation.md; core-hc/rtl/hc_family_core.sv,
 * model 4):
 *   - the Z80 at 3.5 MHz from the 14 MHz master clock; a 32 KiB ROM of two 16 KiB pages at 0000h, page 0 the 128 editor and
 *     its menu, page 1 the 48 BASIC; 128 KiB of RAM as eight 16 KiB pages, page 5 at 4000h, page 2 at 8000h, the page of
 *     7FFDh's bits 0-2 at C000h; no memory contention and no floating bus unless the not original Spectrum timing is on;
 *   - port 7FFDh, written, decoded by A15 low and A1 low: bits 0-2 the page at C000h, bit 3 the screen (page 5 or 7), bit
 *     4 the ROM page, bit 5 locks the port until reset; port FE on A0 low and A7 high;
 *   - the AY-3-8912 at FFFDh and BFFDh (A15 high, A1 low), the machine's own (the core's chip, here original);
 *   - two joystick sockets on the matrix, as the HC-85/90/91 have them (hc.c);
 *   - Interface 1 (hw/if1.h) as an option, with two diskette drives (slots a and b) and the serial port, and with it, as the
 *     HC core has them only then ("expanded"): the configuration latch on port 7Eh (bit 0 the CP/M firmware, bit 1 RAM at
 *     0000h with the upper half of the ROM at E000h, bit 2 locks it, bit 3 the screen at 4000h or C000h under the CP/M
 *     firmware), ports C7h and C5h (the alias that makes C000h-DFFFh reach the RAM at E000h-FFFFh), the CP/M ROM (the
 *     labelled family bridge "hc-cpm" with the original Rev-E BIOS) and the CP/M start (BREAK, CAPS SHIFT and SPACE, held at
 *     reset, or the "Startup" option at CP/M). Under the CP/M firmware the memory is the 64K machine's (page 0 at C000h,
 *     7FFDh without effect) and the AY's decode is the low byte FDh alone ("CP/M mode releases the AY decoding from disk
 *     ports"). The Interface 1 pages itself in on an opcode fetch at its entries only while the 48 BASIC page or the CP/M
 *     ROM is the one mapped: the 128 editor's startup instruction at 0009h is not intercepted (interfaces.md);
 *   - a tape recorder.
 * ZetRo's own code. */
#include "spectrum.h"
#include "if1.h"
#include "media/disk.h"
#include "media/snap.h"
#include "romdb.h"
#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define PAGE 0x4000u
#define CPM_FONT 0x25ABu                    /* the BIOS's 4 x 8 character set in the bridge ROM, codes 20h-7Eh */

static const char *const ROMS[] = {"hc128", "if1-hc90", "hc-cpm", NULL};
/* The 128 menu's first entry is Tape Loader, and Enter chooses it (LOAD ""): the auto-load text is that one key. */
static const zr_model_desc MODELS[] = {{"hc128", "HC-91 Plus (HC-128)", ROMS, "\n"}};

static const char *const SWITCH[] = {"off", "on", NULL};
static const char *const STARTUPS[] = {"basic", "cpm", NULL};
static const int STARTUP_LABELS[] = {ZR_T_HC_STARTUP_BASIC, ZR_T_HC_STARTUP_CPM};
static const char *const BAUDS[] = {"110", "300", "600", "1200", "2400", "4800", "9600", "19200", NULL};
static const uint32_t BAUD_RATES[] = {110, 300, 600, 1200, 2400, 4800, 9600, 19200};
/* The AY is the machine's own: it cannot be taken out, only heard as mono or stereo. */
static const char *const AYS[] = {"mono", "abc", "acb", NULL};
static const int AY_LABELS[] = {ZR_T_SPEC_AY_MONO, ZR_T_SPEC_AY_ABC, ZR_T_SPEC_AY_ACB};
/* The sockets are wired to the matrix: the first to the keys 1-5, the second to 6-0 (hc.c). */
static const int STICK1_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR2);
static const int STICK2_LABELS[ZR_SPECTRUM_JOY_KINDS] = ZR_SPECTRUM_STICK_LABELS(ZR_SPECTRUM_JOY_SINCLAIR1);

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
    {"startup", ZR_T_HC_STARTUP, STARTUPS, STARTUP_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET, ZR_T_HC_STARTUP_HINT + 1},
    {"drive_sound", ZR_T_HC_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET, 0, "interface1=on"},
    /* The bytes of the serial port to and from the host: ZetRo's UART on the board's bits (not original). */
    {"serial_baud", ZR_T_HC_BAUD, BAUDS, NULL, 6, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_NOT_ORIGINAL | ZR_OPTION_PORT, 0, "interface1=on"},
    /* The HC-91 Plus's own AY (mono: the RTL sums the channels); the stereo modes are the core's. */
    {"ay", ZR_T_HC_AY, AYS, AY_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET},
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

typedef struct hc128 {
    zr_spectrum s;                          /* first: the shared core */
    uint8_t if1_on, startup_cpm;
    /* the board's latches (saved) */
    uint8_t paging;                         /* the last byte written to port 7FFDh */
    uint8_t config;                         /* port 7Eh (with Interface 1) */
    uint8_t alias;                          /* C7h on, C5h off */
    /* the firmware, in s.rom: the two ROM pages, Interface 1's EPROM, the CP/M bridge */
    const uint8_t *basic, *eprom, *cpm;
    /* Interface 1 */
    zr_if1 if1;
    zr_cpu_hooks hooks;
    uint8_t watch[8192];
    zr_media *disk_media[2];
    zr_disk *disk[2];
    zr_port_host ports;
    int has_ports;
} hc128;

/* --- the memory map ---------------------------------------------------------------------------------------- */

/* The CP/M firmware is selected (with Interface 1, latch bit 0): the 64K machine's memory, the bridge ROM at 0000h. */
static int cpm_firmware(const hc128 *h) { return h->if1_on && (h->config & 1u); }

/* The DivIDE's and DivMMC's automatic mapping acts only while the 48 BASIC page (7FFDh bit 4) is the ROM at 0000h, as the real
 * interfaces do through the 128K's ROM-select line; with the 128 editor's page it does not. */
static int interface_rom(zr_spectrum *s) {
    const hc128 *h = (const hc128 *)s;
    return !cpm_firmware(h) && (h->paging & 0x10u) != 0;
}

/* The memory as 7FFDh, the latch and the alias leave it. */
static void remap(hc128 *h) {
    zr_spectrum *s = &h->s;
    zr_bus *b = &s->bus;
    const int cpm = cpm_firmware(h);
    const int ram0 = h->if1_on && (h->config & 2u);
    const uint8_t *rom = cpm ? h->cpm : h->basic + (h->paging >> 4 & 1u) * PAGE;
    const unsigned page = cpm ? 0u : h->paging & 7u;
    if (h->if1_on && zr_if1_paged(&h->if1) && !ram0)
        for (unsigned p = 0; p < 16u; ++p) {
            const uint16_t at = (uint16_t)(p << 10);
            uint8_t *ram = zr_if1_ram_page(&h->if1, p);
            const uint8_t *eprom = zr_if1_rom_page(&h->if1, p);
            if (ram) zr_bus_map_ram(b, at, 1024u, ram);
            else if (eprom) zr_bus_map_rom(b, at, 1024u, eprom);
            else zr_bus_unmap(b, at, 1024u, ZR_BUS_BOTH);
        }
    else if (ram0) zr_bus_map_ram(b, 0x0000u, PAGE, s->ram + 3u * PAGE);
    else zr_bus_map_rom(b, 0x0000u, PAGE, rom);
    zr_bus_map_ram(b, 0x4000u, PAGE, s->ram + 5u * PAGE);
    zr_bus_map_ram(b, 0x8000u, PAGE, s->ram + 2u * PAGE);
    zr_bus_map_ram(b, 0xC000u, 0x2000u, s->ram + page * PAGE + (h->alias ? 0x2000u : 0u));
    if (ram0) zr_bus_map_rom(b, 0xE000u, 0x2000u, rom + 0x2000u);
    else zr_bus_map_ram(b, 0xE000u, 0x2000u, s->ram + page * PAGE + 0x2000u);
    /* The screen: page 5 or 7 by 7FFDh; under the CP/M firmware page 5 or the page at C000h by the latch's bit 3. */
    const uint8_t *screen = cpm ? s->ram + (h->config & 8u ? 0u : 5u * PAGE) : s->ram + (h->paging & 8u ? 7u : 5u) * PAGE;
    if (screen != s->screen) {
        zr_ula_catch_up(&s->ula, zr_spectrum_now(s));
        s->screen = screen;
    }
    zr_spectrum_contend(s, 3, (int)zr_ula_page_contended(page));
    /* The tape's "stop if 48K" blocks stop it in the locked 48K mode and under CP/M (the RTL's tape_48k). */
    zr_tape_deck_set_48k(&s->deck, (h->paging & 0x20u) != 0 || cpm);
    /* The Interface 1's entries page it in only while the 48 BASIC page or the CP/M ROM is mapped (interfaces.md). */
    h->if1.entry = (uint8_t)(cpm || (h->paging & 0x10u));
    /* "CP/M mode releases the AY decoding from disk ports": the low byte FDh alone. */
    if (cpm) zr_spectrum_ay_decode(s, 0x80FFu, 0x80FDu);
    else zr_spectrum_ay_decode(s, s->cfg.ay_mask, s->cfg.ay_value);
}

static void if1_paged(void *ctx) { remap(ctx); }

/* --- the CPU: Interface 1's fetches, the network WAIT -------------------------------------------------------- */

static int hc_before_fetch(void *ctx, uint16_t pc) {
    zr_if1_before_fetch(&((hc128 *)ctx)->if1, pc);
    return 0;
}
static void hc_after_fetch(void *ctx, uint16_t pc, uint8_t opcode) {
    (void)opcode;
    zr_if1_after_fetch(&((hc128 *)ctx)->if1, pc);
}
/* The network WAIT holds every cycle (a frame's worth at a time) while it lasts. */
static unsigned hc_stretch(zr_spectrum *s, int kind, uint16_t address) {
    (void)kind; (void)address;
    hc128 *h = (hc128 *)s;
    return h->if1_on && zr_if1_held(&h->if1) ? 69888u : 0u;
}
static uint64_t hc_quiet(zr_spectrum *s, uint64_t t, uint64_t limit) {
    hc128 *h = (hc128 *)s;
    return h->if1_on && h->if1.net_wait ? t : limit;
}

/* --- ports ------------------------------------------------------------------------------------------------------ */

/* 7FFDh, written, on A15 low and A1 low: any such port, 05h of Interface 1's control latch with a high byte below 80h too. */
static void paging_write(void *ctx, uint16_t port, uint8_t value) {
    hc128 *h = ctx;
    (void)port;
    if (h->paging & 0x20u) return;                      /* locked until reset */
    h->paging = value;
    remap(h);
}
static uint8_t config_read(void *ctx, uint16_t port) {
    (void)port;
    return ((hc128 *)ctx)->config;
}
static void config_write(void *ctx, uint16_t port, uint8_t value) {
    hc128 *h = ctx;
    (void)port;
    if (h->config & 4u) return;
    h->config = value;
    remap(h);
}
static void alias_write(void *ctx, uint16_t port, uint8_t value) {
    hc128 *h = ctx;
    (void)value;
    h->alias = (port & 0xFFu) == 0xC7u;
    remap(h);
}
static uint8_t if1_read(void *ctx, uint16_t port) { return zr_if1_in(&((hc128 *)ctx)->if1, port); }
static void if1_write(void *ctx, uint16_t port, uint8_t value) { zr_if1_out(&((hc128 *)ctx)->if1, port, value); }

/* Interface 1's serial port to the host's end. */
static void serial_out(void *ctx, uint8_t byte) {
    hc128 *h = ctx;
    if (h->has_ports && h->ports.transmit) h->ports.transmit(h->ports.ctx, 0, byte);
}
static void serial_ready(void *ctx, int ready) {
    hc128 *h = ctx;
    if (h->has_ports && h->ports.outputs) h->ports.outputs(h->ports.ctx, 0, ready, ready);
}

/* --- the core's paging hooks (snapshots, ZR_PAGE_ADDRESS) ------------------------------------------------------------- */

static uint8_t *page_of(zr_spectrum *s, unsigned page) { return page < 8u ? s->ram + page * PAGE : NULL; }
static uint8_t paging_get(zr_spectrum *s) { return ((hc128 *)s)->paging; }
static void paging_set(zr_spectrum *s, uint8_t value) {
    hc128 *h = (hc128 *)s;
    h->paging = value;
    remap(h);
}

/* --- life cycle ---------------------------------------------------------------------------------------------------- */

/* BREAK held: CAPS SHIFT (row 0) and SPACE (row 7), column 0 of the matrix. */
static int break_held(const zr_spectrum *s) {
    return !(zr_ula_keys(&s->ula, 0xFEu) & 1u) && !(zr_ula_keys(&s->ula, 0x7Fu) & 1u);
}

static void hc_reset(void *self, int power_on) {
    hc128 *h = self;
    zr_spectrum_reset(&h->s, power_on);
    if (power_on) memset(h->if1.ram, 0, sizeof h->if1.ram);
    h->paging = 0;
    /* The CP/M start: with Interface 1, the Startup option or BREAK held at reset leaves the latch at 1. */
    h->config = (uint8_t)(h->if1_on && (h->startup_cpm || break_held(&h->s)) ? 1u : 0u);
    h->alias = 0;
    zr_if1_reset(&h->if1);
    remap(h);
}

static void *hc_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.fe_mask = 0x0081u;                              /* A0 low, A7 high */
    cfg.fe_value = 0x0080u;
    cfg.ram_size = 8u * PAGE;
    cfg.is_48k = 0;
    cfg.charset = 0x4000u + 0x3D00u;                    /* the 48 BASIC page's character set, which the editor and the menu draw with */
    cfg.traps_rom = 0x4000u;                            /* the 48 BASIC page */
    cfg.page = page_of;
    cfg.paging_get = paging_get;
    cfg.paging_set = paging_set;
    cfg.paging_48k = 0x30u;
    cfg.joystick = (uint8_t)o[OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_128K;                /* the model stands for a Sinclair 128K in the timing option */
    cfg.spectrum_timing = o[OPTION_TIMING] != 0;
    cfg.ay = (uint8_t)(1u + (o[OPTION_AY] < 3u ? o[OPTION_AY] : 0u));   /* the 128K's decode: A15 high, A1 low */
    const int if1_on = o[OPTION_INTERFACE1] != 0;
    if (if1_on) {
        cfg.stretch = hc_stretch;
        cfg.stretch_quiet = hc_quiet;
    }
    zr_spectrum_interface_config(&cfg, o + OPTION_INTERFACE, SLOT_INTERFACE);
    cfg.interface_rom = interface_rom;
    if (zr_spectrum_interface_conflict(&cfg, o[OPTION_INTERFACE1] != 0, error, capacity)) return NULL;
    hc128 *h = (hc128 *)zr_spectrum_create(&cfg, setup, sizeof *h, error, capacity);
    if (!h) return NULL;
    zr_spectrum *s = &h->s;
    h->if1_on = (uint8_t)if1_on;
    h->startup_cpm = o[OPTION_STARTUP] != 0;
    h->basic = s->rom;
    h->eprom = s->rom + 2u * PAGE;
    h->cpm = s->rom + 3u * PAGE;
    /* Interface 1: always built (its drives keep their disks), wired only when fitted. */
    const zr_if1_board board = {.ctx = h, .paged = if1_paged, .serial_out = serial_out, .serial_ready = serial_ready};
    const unsigned baud = o[OPTION_BAUD] < sizeof BAUD_RATES / sizeof BAUD_RATES[0] ? o[OPTION_BAUD] : 6u;
    zr_if1_init(&h->if1, &s->host, &board, ZR_IF1_HC90, h->eprom, PAGE, cfg.clock_hz, BAUD_RATES[baud],
                if1_on && o[OPTION_DRIVE_SOUND] != 0);
    zr_bus_add_io(&s->bus, 0x8002u, 0x0000u, NULL, paging_write, h);
    zr_bus_watch(&s->bus, 0xC000u, 0x1C00u, 1);         /* page 7 shows at C000h too */
    if (if1_on) {
        zr_bus_add_io(&s->bus, 0x00FDu, 0x0005u, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FDu, 0x0085u, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00EFu, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00F7u, if1_read, if1_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00C5u, NULL, alias_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x00C7u, NULL, alias_write, h);
        zr_bus_add_io(&s->bus, 0x00FFu, 0x007Eu, config_read, config_write, h);
        h->hooks.ctx = h;
        h->hooks.watch = h->watch;
        h->hooks.before_fetch = hc_before_fetch;
        h->hooks.after_fetch = hc_after_fetch;
        zr_if1_watch(h->watch);
        zr_spectrum_set_hooks(s, &h->hooks);
        if (zr_drivesnd_enabled(&h->if1.sound)) s->drives = &h->if1.sound;
        snprintf(s->description, sizeof s->description, "%s, %s", setup->family->models[0].name, ZR_TXT(HC_INTERFACE1));
    }
    hc_reset(h, 1);
    return h;
}

/* The disks go back into their media when they have changed. */
static void flush_disk(hc128 *h, unsigned drive) {
    zr_disk *disk = h->disk[drive];
    if (!disk || !disk->dirty || !h->disk_media[drive]) return;
    char why[128];
    if (!zr_disk_save_media(disk, h->disk_media[drive], why, sizeof why)) snprintf(h->s.error, sizeof h->s.error, "%s", why);
}

static void hc_destroy(void *self) {
    hc128 *h = self;
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
    hc128 *h = self;
    const zr_run_status status = zr_spectrum_run(h, ticks);
    for (unsigned i = 0; i < 2u; ++i)
        if (h->disk[i] && h->disk[i]->dirty && !h->if1.fdd[i].motor) flush_disk(h, i);
    return status;
}

/* --- the screen as text ---------------------------------------------------------------------------------------------- */

/* Under the CP/M firmware the BIOS draws 64 columns of 4 x 8 characters (its font is in the bridge ROM); otherwise the
 * shared core's 32 columns through the 48 BASIC ROM's character set. */
static size_t hc_text(void *self, char *out, size_t capacity) {
    hc128 *h = self;
    if (!cpm_firmware(h)) return zr_spectrum_text(self, out, capacity);
    return zr_spectrum_text_narrow(self, h->cpm + CPM_FONT, out, capacity);
}

/* --- media ------------------------------------------------------------------------------------------------------------- */

static int hc_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    hc128 *h = self;
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
    const hc128 *h = self;
    if (slot == SLOT_A || slot == SLOT_B) return h->disk_media[slot - SLOT_A];
    return zr_spectrum_media(self, slot);
}

static int hc_eject(void *self, unsigned slot) {
    hc128 *h = self;
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
    const hc128 *h = self;
    const unsigned drives = h->if1_on ? zr_if1_activity(&h->if1) : 0u;
    return zr_spectrum_activity(self) | (drives & 1u ? 1u << SLOT_A : 0u) | (drives & 2u ? 1u << SLOT_B : 0u);
}

/* --- the serial port ---------------------------------------------------------------------------------------------------- */

static void hc_set_ports(void *self, const zr_port_host *host) {
    hc128 *h = self;
    h->has_ports = host != NULL;
    if (host) h->ports = *host;
    zr_if1_set_loopback(&h->if1, host && host->loopback && host->loopback(host->ctx, 0));
    if (host && host->outputs) {
        const int ready = h->if1_on && zr_if1_serial_cts(&h->if1);
        host->outputs(host->ctx, 0, ready, ready);
    }
}
static size_t hc_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    hc128 *h = self;
    if (port) return 0;
    return h->if1_on ? zr_if1_receive(&h->if1, data, count) : count;
}
static void hc_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    (void)dcd; (void)dsr; (void)ri;
    if (!port) zr_if1_serial_ready_in(&((hc128 *)self)->if1, cts);
}

/* --- states ------------------------------------------------------------------------------------------------------------- */

static void hc_save(const void *self, zr_state_writer *w) {
    const hc128 *h = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "HC28", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, h->paging);
    zr_sw_u8(w, h->config);
    zr_sw_u8(w, h->alias);
    zr_sw_end(w);
    if (h->if1_on) zr_if1_save(&h->if1, w);
}

static int hc_load(void *self, zr_state_reader *r) {
    hc128 *h = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "HC28", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    h->paging = zr_sr_u8(r);
    h->config = zr_sr_u8(r);
    h->alias = zr_sr_u8(r) & 1u;
    if (!zr_sr_ok(r)) return 0;
    if (h->if1_on && !zr_if1_load(&h->if1, r)) return 0;
    if (!h->if1_on) h->config = h->alias = 0;
    remap(h);
    /* The drives' sound starts afresh, in step with the beeper's next frame. */
    const zr_beeper *b = &h->s.beeper;
    zr_drivesnd_reset(&h->if1.sound, b->num ? (b->done * b->den + b->num - 1u) / b->num : 0u);
    return 1;
}

/* --- snapshots --------------------------------------------------------------------------------------------------------- */

/* A snapshot is of a 128K or 48K machine: Interface 1 paged out and the latch cleared first (the core puts the paging back). */
static int hc_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    hc128 *h = self;
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
    hc128 *h = self;
    if (zr_if1_paged(&h->if1) || (h->config & 0x0Bu) || h->alias) return 0;
    return zr_spectrum_snapshot_save(self, snap);
}

static const zr_machine_impl HC128_IMPL = {
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

const zr_family zr_hc128_family = {
    .id = "hc128", .name = "HC",
    .models = MODELS, .model_count = 1,
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOT_COUNT,
    .ports = PORTS, .port_count = 1,
    .keys = zr_spectrum_keys, .key_count = ZR_SPECTRUM_KEYS,
    .typing = zr_spectrum_typing, .typing_count = ZR_SPECTRUM_TYPING,
    .combos = zr_spectrum_combos, .combo_count = ZR_SPECTRUM_COMBOS,
    .joysticks = 2,
    .impl = &HC128_IMPL, .create = hc_create,
};
