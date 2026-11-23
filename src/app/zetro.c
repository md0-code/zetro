/* zetro: the command line, the configuration, the ROMs, then the window
 * (desktop.c). */
#include "../core/config.h"
#include "../core/machine.h"
#include "../core/romdb.h"
#include "../host/host.h"
#include "desktop.h"
#include "pads.h"
#include "setup.h"
#include "zetro_default_yaml.h"
#include "zr_version.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIG_NAME "zetro.yaml"

/* The last message of a failure, also shown in a message box when the
 * program was started without a console. */
static char last_error[2048];
static void complain(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof last_error, format, args);
    va_end(args);
    fprintf(stderr, "%s\n", last_error);
}

/* Without --config, zetro.yaml in the working directory; a missing one is
 * written from the template so every setting can be edited. */
static const char *default_configuration(char *error, size_t capacity) {
    FILE *f = fopen(CONFIG_NAME, "rb");
    if (f) { fclose(f); return CONFIG_NAME; }
    /* The template, with the language of the host's user locale (the template says en). */
    static unsigned char text[sizeof zr_default_yaml];
    memcpy(text, zr_default_yaml, sizeof text);
    static const char KEY[] = "language: en";
    if (zr_config_default_language() == ZR_LANG_RO)
        for (size_t i = 0; i + sizeof KEY - 1 <= sizeof text; ++i)
            if (!memcmp(text + i, KEY, sizeof KEY - 1)) { text[i + sizeof KEY - 3] = 'r'; text[i + sizeof KEY - 2] = 'o'; break; }
    f = fopen(CONFIG_NAME, "wb");
    int ok = f && fwrite(text, 1, sizeof text, f) == sizeof text;
    if (f && fclose(f)) ok = 0;
    if (!ok) { snprintf(error, capacity, "Cannot create %s", CONFIG_NAME); return NULL; }
    printf("Created %s with the default settings\n", CONFIG_NAME);
    return CONFIG_NAME;
}

/* The roms folders: in the working directory, beside the configuration,
 * beside the program (the same folder is read once). */
static int scan_roms(zr_rom_index *roms, const char *yaml) {
    char beside[ZR_CONFIG_PATH + 8], program[ZR_CONFIG_PATH + 8] = "", folder[ZR_CONFIG_PATH];
    snprintf(folder, sizeof folder, "%s", yaml ? yaml : "");
    char *slash = strrchr(folder, '/'), *backslash = strrchr(folder, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
    if (slash) slash[1] = 0; else folder[0] = 0;
    snprintf(beside, sizeof beside, "%sroms", folder);
    if (zr_host_program_folder(folder, sizeof folder)) snprintf(program, sizeof program, "%sroms", folder);
    const char *folders[3] = {"roms", beside, program[0] ? program : NULL};
    zr_rom_index_init(roms);
    return zr_rom_index_scan(roms, folders, 3);
}

/* The known ROMs and where each was found, then every file of the roms
 * folders and what it was recognised as. */
static void list_roms(const zr_rom_index *ix) {
    char what[256];
    unsigned missing = 0;
    printf("Known ROMs (recognised by the SHA-256 of their content, whatever the file name):\n");
    printf("  %-18s %-40s %6s  %-22s %s\n", "key", "name", "bytes", "standard name", "found");
    for (size_t i = 0; i < zr_romdb_count(); ++i) {
        const zr_known_rom *k = zr_romdb_entry(i);
        const zr_rom_file *f = zr_rom_index_find(ix, k);
        missing += !f;
        printf("  %-18s %-40s %6u  %-22s %s\n", k->key, k->name, (unsigned)k->size, k->file, f ? f->path : "missing");
    }
    for (size_t k = 0; k < ix->folder_count; ++k) {
        const size_t length = strlen(ix->folders[k]);
        size_t n = 0;
        for (size_t i = 0; i < ix->count; ++i) n += !strncmp(ix->files[i].path, ix->folders[k], length);
        printf("\nROM folder %s: %zu file%s\n", ix->folders[k], n, n == 1 ? "" : "s");
        for (size_t i = 0; i < ix->count; ++i)
            if (!strncmp(ix->files[i].path, ix->folders[k], length))
                printf("  %s: %s\n", ix->files[i].name, zr_rom_file_describe(ix, &ix->files[i], what, sizeof what));
    }
    if (missing) printf("\npython tools/fetch_roms.py fetches the ROMs into roms/\n");
}

/* Every model of the registry, a line for each family name (the HC's boards are several families of one name): the models in the
 * registry's order. */
static void list_models(void) {
    for (unsigned i = 0; i < zr_family_count(); ++i) {
        const zr_family *f = zr_family_at(i);
        int shown = 0;
        for (unsigned e = 0; e < i && !shown; ++e) shown = !strcmp(zr_family_at(e)->name, f->name);
        if (shown) continue;
        printf("%s:", f->name);
        const char *separator = "";
        for (unsigned g = i; g < zr_family_count(); ++g) {
            const zr_family *family = zr_family_at(g);
            if (strcmp(family->name, f->name)) continue;
            for (unsigned k = 0; k < family->model_count; ++k) {
                printf("%s %s (%s)", separator, family->models[k].id, family->models[k].name);
                separator = ",";
            }
        }
        printf("\n");
    }
}

static int usage(void) {
    printf("ZetRo %s, an emulator of the Z80-based Romanian computers\n\n"
           "zetro [--config FILE.yaml] [--model ID] [--hidden [--realtime]] [--frames N] [--screenshot FILE.png|FILE.bmp]\n"
           "      [--wav FILE.wav] [--script FILE] [--control stdin|tcp:PORT] [--drop FILE]\n"
           "      [--setup-page machine|media|display|sound|controllers|ports|states|cheats|about] [--hostkey KEY@N]...\n"
           "zetro --list-roms [--config FILE.yaml] | --list-models | --version | --help\n\n"
           "Without --config, zetro.yaml in the working directory is used, written with the defaults when missing.\n"
           "ROMs are read from roms/ in the working directory, beside the configuration and beside the program.\n"
           "--model runs that model instead of the configuration's machine.model (its options are then the defaults);\n"
           "--hidden runs without a visible window or sound device, the same on any host; --frames N stops after N\n"
           "frames; --realtime paces a hidden window to the clock as a visible one is; --screenshot saves the last\n"
           "frame (a PNG when the name ends in .png); --wav records the sound to a file instead of playing it;\n"
           "--drop puts a file in as if it had been dropped on the window (a tape is then auto-loaded).\n"
           "--script runs a file of harness commands, one a line, and ends the run after the last; --control takes\n"
           "the same commands on standard input or on a local TCP port, answering each with a line of JSON:\n"
           "  wait SECONDS, frames N, type TEXT, enter, key NAME, release NAME, joy N [up|down|left|right|fire|fire2]...,\n"
           "  insert SLOT FILE, drop FILE, eject SLOT, play FILE, tape play|stop|rewind|next|previous|record, nmi, reset [power|keys],\n"
           "  osk on|off, osk click KEY, osk pad up|down|left|right|a|b, osk keys (the on-screen keyboard, F8 in the window),\n"
           "  shot FILE.png, text, expect TEXT, until SECONDS TEXT, state save|load FILE, rewind on|off|SECONDS,\n"
           "  speed full|normal, peek ADDR [COUNT], poke ADDR BYTE..., trace FILE|off, snapshot load|save FILE (sna, z80,\n"
           "  szx), cheat load FILE.pok, cheat on N [VALUE] (N from 1; VALUE 0 to 255 for a trainer that asks), cheat off N,\n"
           "  quit [STATUS]\n"
           "  (text, expect and until read the setup screen while it is open; a headless run keeps no rewind ring\n"
           "  until rewind on)\n"
           "--setup-page opens the setup screen on a page at the start; --hostkey KEY@N presses a host key after N frames\n"
           "(an SDL key name such as F12, Down, Return, PageUp or A, with ctrl+, shift+ or alt+ in front, and + or - before\n"
           "all for only the press or only the release, so a key can be held: +F4@100 -F4@130; at most %d),\n"
           "so a hidden run can drive the setup screen and take --screenshot of it.\n"
           "In the window: F12 opens and closes the setup screen, F8 the on-screen keyboard (click a key, or move with a game\n"
           "controller's d-pad and press with A; shift keys stay held until the next key; B or F8 closes it), Alt+Enter\n"
           "toggles full screen, Pause stops and starts\n"
           "the machine, Print Screen saves zetro-NNNN.png, F10 is the NMI button, F11 switches the keyboard joystick on\n"
           "and off, F1 chooses the tape's file, F5 plays and stops the tape, Shift+F5 records on it (a new media/tape-NNNN.tzx when the slot is empty)\n"
           "and stops, F6 and F7 go to its previous and next block, F2 saves the state to the\n"
           "current slot and F3 loads it back (ui.state_slot; the files are states/MODEL/slot-N.zrs beside the\n"
           "configuration), F4 held goes back in time, F9 switches between 100%% and full speed, Ctrl+F12 switches the\n"
           "machine off and on, Alt+F12 resets it, Shift+F12 exits. Other keys are the machine's keys in the same place. A file dropped on\n"
           "the window goes into the slot that takes it, and a tape is then loaded at once (its slot's autoload); a .sna, .z80 or\n"
           ".szx file is loaded as a snapshot (the States page loads and saves them too, in snapshots/ beside the\n"
           "configuration) and a .pok file as the cheats (a tape's NAME.pok beside it or in its POKES folder comes by\n"
           "itself; the Cheats page switches the trainers on and off).\n"
           "The machine's byte ports (a serial line, a printer) are connected to the host in zetro.yaml's ports section or on\n"
           "the Ports page: each by the key of its port (serial, printer, user) to none, loopback, a host serial port (COM3),\n"
           "pipe:NAME, tcp:HOST:PORT, tcp-listen:[HOST:]PORT or file:FILE (what the guest sends is appended to the file).\n"
           "ui.language in zetro.yaml sets the language of the messages in the window: en or ro (Romanian); the default\n"
           "is the host's language.\n", ZR_VERSION, ZR_DESKTOP_HOSTKEYS);
    return 0;
}

/* 1 when `text` starts with `prefix`, without regard to case. */
static int starts_with(const char *text, const char *prefix) {
    for (; *prefix; ++text, ++prefix) if (tolower((unsigned char)*text) != *prefix) return 0;
    return 1;
}

/* --hostkey KEY@N: the key's SDL name, with ctrl+, shift+ or alt+ in front, and the frame after which it is pressed; the keys
 * are kept in the order of their frames. 0 when the argument is not one. */
static int add_hostkey(zr_desktop_options *o, const char *spec) {
    const char *at = strrchr(spec, '@');
    unsigned long long frame = 0;
    char tail = 0;
    if (o->hostkey_count == ZR_DESKTOP_HOSTKEYS || !at || sscanf(at + 1, "%llu%c", &frame, &tail) != 1 || !frame) return 0;
    const char *key = spec;
    size_t n = (size_t)(at - spec);
    unsigned mods = 0;
    if (n > 1 && (*key == '+' || *key == '-')) {         /* + only the press, - only the release: a key held over frames */
        mods |= *key == '+' ? ZR_HOSTKEY_PRESS : ZR_HOSTKEY_RELEASE;
        ++key;
        --n;
    }
    for (;;) {                                          /* ctrl+, shift+ and alt+ in front */
        if (n > 5 && starts_with(key, "ctrl+")) { mods |= ZR_HOSTKEY_CTRL; key += 5; n -= 5; }
        else if (n > 6 && starts_with(key, "shift+")) { mods |= ZR_HOSTKEY_SHIFT; key += 6; n -= 6; }
        else if (n > 4 && starts_with(key, "alt+")) { mods |= ZR_HOSTKEY_ALT; key += 4; n -= 4; }
        else break;
    }
    zr_desktop_hostkey k;
    memset(&k, 0, sizeof k);
    if (!n || n >= sizeof k.name) return 0;
    memcpy(k.name, key, n);
    if (!zr_desktop_key_known(k.name)) return 0;
    k.at = frame;
    k.mods = mods;
    unsigned i = o->hostkey_count++;
    for (; i && o->hostkeys[i - 1].at > frame; --i) o->hostkeys[i] = o->hostkeys[i - 1];       /* after the keys of its frame or before */
    o->hostkeys[i] = k;
    return 1;
}

static int run(int argc, char **argv) {
    const char *yaml = NULL, *model = NULL;
    int roms_list = 0;
    zr_desktop_options options;
    memset(&options, 0, sizeof options);
    options.setup_page = -1;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--version")) { printf("ZetRo %s\n", ZR_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help")) return usage();
        else if (!strcmp(argv[i], "--list-models")) { list_models(); return 0; }
        else if (!strcmp(argv[i], "--list-roms")) roms_list = 1;
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) yaml = argv[++i];
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model = argv[++i];
        else if (!strcmp(argv[i], "--hidden")) options.hidden = 1;
        else if (!strcmp(argv[i], "--realtime")) options.realtime = 1;
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) options.screenshot = argv[++i];
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) options.wav = argv[++i];
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) options.script = argv[++i];
        else if (!strcmp(argv[i], "--control") && i + 1 < argc) options.control = argv[++i];
        else if (!strcmp(argv[i], "--drop") && i + 1 < argc) options.drop = argv[++i];
        else if (!strcmp(argv[i], "--setup-page") && i + 1 < argc) {
            options.setup_page = zr_setup_page_by_name(argv[++i]);
            if (options.setup_page < 0) { complain("--setup-page needs machine, media, display, sound, controllers, ports, states, cheats or about"); return 2; }
        } else if (!strcmp(argv[i], "--hostkey") && i + 1 < argc) {
            if (!add_hostkey(&options, argv[++i])) {
                complain("--hostkey needs KEY@N: an SDL key name (F12, Down, Return, PageUp, A), optionally with ctrl+, shift+ or alt+, and a "
                         "frame number; at most %d", ZR_DESKTOP_HOSTKEYS);
                return 2;
            }
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            char *end = NULL;
            options.frames = strtoull(argv[++i], &end, 10);
            if (!options.frames || !end || *end) { complain("--frames needs a positive whole number"); return 2; }
        } else { complain("Unknown or incomplete argument: %s (zetro --help)", argv[i]); return 2; }
    }
    if (!zr_machine_host_ok()) { complain("ZetRo needs a little-endian host"); return 1; }
    char error[1024] = "";
    static zr_config config;
    static zr_rom_index roms;
    if (roms_list) {
        if (!scan_roms(&roms, yaml)) { complain("Out of memory"); return 1; }
        list_roms(&roms);
        zr_rom_index_free(&roms);
        return 0;
    }
    zr_config_set_key_check(zr_host_key_known);         /* controllers.keys names keys the host has */
    if (!yaml && !(yaml = default_configuration(error, sizeof error))) { complain("%s", error); return 1; }
    if (!zr_config_load(yaml, &config, error, sizeof error)) { complain("%s: %s", yaml, error); return 2; }
    if (model) {
        if (!zr_family_of_model(model, NULL)) { complain("--model: there is no model %s (zetro --list-models)", model); return 2; }
        snprintf(config.model, sizeof config.model, "%s", model);
        config.option_count = 0;
        config.media_count = 0;
    }
    if (!scan_roms(&roms, yaml)) { complain("Out of memory"); return 1; }
    zr_machine *machine = zr_machine_create(&config, &roms, error, sizeof error);
    if (!machine) { complain("%s", error); zr_rom_index_free(&roms); return 1; }
    options.roms = &roms;
    /* The control channel on standard output carries only the answers. */
    if (!options.control || strcmp(options.control, "stdin")) {
        printf("ZetRo %s: %s\n", ZR_VERSION, zr_machine_description(machine));
        fflush(stdout);
    }
    error[0] = 0;
    const int status = zr_desktop_run(&machine, &config, &options, error, sizeof error);
    if (status && error[0]) complain("%s", error);
    zr_machine_destroy(machine);
    zr_rom_index_free(&roms);
    return status;
}

/* A program without a console window of its own: messages go to the
 * console it was started from or to the pipes a script gave it; started
 * from the desktop, a failure is shown in a message box. */
int main(int argc, char **argv) {
    const int unseen = zr_host_console_init();
    const int status = run(argc, argv);
    fflush(stdout);
    if (unseen && status && last_error[0]) zr_host_alert("ZetRo", last_error);
    return status;
}
