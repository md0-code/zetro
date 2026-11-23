/* zetro.yaml: the machine that runs, its options and media, and the
 * settings of the window. libyaml reads the YAML syntax; this module
 * enforces ZetRo's schema (unknown keys, duplicates and bad values refused
 * with their line) and writes settings back through the comment-keeping
 * editor (yamledit.h).
 *
 *   machine:
 *     model: hc85              a model of the registry (machine.h)
 *     options:                 the options of that model's family, by key
 *       interface1: on
 *   media:                     the media slots of the family, by key
 *     tape: { image: demo.tzx, write_protect: false, fast: true, sound: true, autoload: true }
 *   video:   look, aspect, scale, filter, border, fullscreen, vsync
 *   sound:   enabled, volume
 *   controllers: joystick1, joystick2 (device), keys, keyboard_joystick
 *   ports:   the host end of each byte port of the model, by the key of its port table (serial, printer, user...)
 *   ui:      language, state_slot, setup_page
 *
 * The options and the media slots are the family's own (its tables), so
 * the reader checks them against the registry: a key the model does not
 * have and a value an option does not take are refused. */

#ifndef ZR_CONFIG_H
#define ZR_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define ZR_CONFIG_PATH 1024
#define ZR_CONFIG_NAME 32               /* a model id, an option's key or value, a slot's key, with its end */
#define ZR_CONFIG_OPTIONS 32
#define ZR_CONFIG_MEDIA 8
#define ZR_CONFIG_PORTS 8

/* video.look */
enum { ZR_LOOK_PLAIN, ZR_LOOK_CRT };
/* video.aspect, video.scale, video.filter */
enum { ZR_CONFIG_ASPECT_ORIGINAL, ZR_CONFIG_ASPECT_SQUARE };
enum { ZR_CONFIG_SCALE_FIT, ZR_CONFIG_SCALE_INTEGER };
enum { ZR_CONFIG_FILTER_NEAREST, ZR_CONFIG_FILTER_LINEAR, ZR_CONFIG_FILTER_SHARP };
/* video.border: how much of the border around the picture is shown */
enum { ZR_CONFIG_BORDER_FULL, ZR_CONFIG_BORDER_MEDIUM, ZR_CONFIG_BORDER_MINIMAL, ZR_CONFIG_BORDER_NONE };
/* ui.language: the language of the setup screen and of the messages shown
 * in the window (text.h); the command line's help and the diagnostics on
 * stderr stay English. */
enum { ZR_LANG_EN, ZR_LANG_RO, ZR_LANG_COUNT };

/* controllers.joystickN.device: what feeds a joystick socket of the machine
 * (how many it has is the family's, machine.h; a socket the model does not
 * have is ignored): nothing, the keyboard
 * joystick (the keys below) or a host game controller by its number (the
 * order the host reports them). What kind of joystick is in the socket
 * (Kempston, Sinclair, Cursor...) is an option of the machine, not here. */
#define ZR_CONFIG_STICKS 2
#define ZR_CONFIG_PADS 8                /* host game controllers numbered 1 to 8 */
enum { ZR_STICK_NONE, ZR_STICK_KEYBOARD, ZR_STICK_CONTROLLER };
/* controllers.keys: the keyboard joystick's keys, in this order */
enum { ZR_STICK_UP, ZR_STICK_DOWN, ZR_STICK_LEFT, ZR_STICK_RIGHT, ZR_STICK_FIRE, ZR_STICK_FIRE2, ZR_CONFIG_STICK_KEYS };
#define ZR_CONFIG_KEY_NAME 32           /* a host key's name, with its end */

typedef struct zr_config_stick {
    uint32_t device;                    /* ZR_STICK_* */
    uint32_t pad;                       /* ZR_STICK_CONTROLLER: the controller, 1 to ZR_CONFIG_PADS */
} zr_config_stick;

typedef struct zr_config_option {
    char key[ZR_CONFIG_NAME], value[ZR_CONFIG_NAME];
} zr_config_option;

typedef struct zr_config_media {
    char key[ZR_CONFIG_NAME];
    char image[ZR_CONFIG_PATH];         /* empty: an empty slot */
    int write_protect;
    int fast;                           /* not original: the shell runs at full speed while this slot works; a tape is also
                                         * loaded and saved by the fast means below */
    /* A tape slot's own. */
    int sound;                          /* the tape's signal is heard */
    int autoload;                       /* not original: a tape put into the slot is loaded at once (typist.h) */
    /* The parts of a tape's `fast`, each on in a new configuration and of effect only while `fast` is on (the setup screen
     * has the one switch; the file may take a part out): the shell's full speed, the ROM's load and save routines done at
     * once (hw/traps.h), the waits of other loaders skipped (hw/loadaccel.h). */
    int full_speed, traps, save_traps, accelerate;
} zr_config_media;
/* What a tape slot's settings ask of the machine, for zr_machine_tape_options (machine.h). NULL: a slot the configuration
 * does not mention (all on). */
enum { ZR_TAPE_SOUND = 1, ZR_TAPE_TRAPS = 2, ZR_TAPE_SAVE_TRAPS = 4, ZR_TAPE_ACCELERATE = 8 };
#define ZR_TAPE_QUERY 0x80000000u       /* instead of settings: nothing changes, the machine only says which it has */
unsigned zr_config_tape_bits(const zr_config_media *media);

/* ports.KEY: the host end of a guest byte port (a family's port table, machine.h). The file words an end as none (the
 * default), loopback (a serial port's output wired back to its input), a host serial device (COM3 or serial:COM3),
 * pipe:NAME, tcp:HOST:PORT (a client), tcp-listen:[HOST:]PORT (a server) or file:PATH (what the guest sends is appended
 * to the file); a printer takes every end but loopback. A key the running model has no port for is kept and ignored, so
 * switching models loses nothing; a key no model has is refused. `path` is the device, the pipe's name, HOST:PORT, the
 * port or the file (relative to the configuration's folder in the file, as media are). */
enum { ZR_END_NONE, ZR_END_LOOPBACK, ZR_END_SERIAL, ZR_END_PIPE, ZR_END_FILE, ZR_END_TCP, ZR_END_TCP_LISTEN, ZR_END_KINDS };
typedef struct zr_config_port {
    char key[ZR_CONFIG_NAME];
    uint32_t type;                      /* ZR_END_* */
    char path[ZR_CONFIG_PATH];
} zr_config_port;

typedef struct zr_config {
    /* machine */
    char model[ZR_CONFIG_NAME];
    zr_config_option options[ZR_CONFIG_OPTIONS];
    unsigned option_count;
    /* media */
    zr_config_media media[ZR_CONFIG_MEDIA];
    unsigned media_count;
    /* ports: the ends the file names, for any model */
    zr_config_port ports[ZR_CONFIG_PORTS];
    unsigned port_count;
    /* video */
    uint32_t look, aspect, scale, filter, border;
    int fullscreen, vsync;
    /* sound: enabled is the mute switch, volume 0 to 100 percent */
    int sound, volume;
    /* controllers: the sockets' devices; the keyboard joystick's keys by SDL's key names (what the
     * window's keys leave free: zr_config_stick_key_problem); whether those keys feed the joystick
     * from the start (the shell's F11 key switches them at any time) */
    zr_config_stick sticks[ZR_CONFIG_STICKS];
    char stick_keys[ZR_CONFIG_STICK_KEYS][ZR_CONFIG_KEY_NAME];
    int keyboard_joystick;
    /* ui */
    uint32_t language;                  /* ZR_LANG_*; the default is the host's own */
    int state_slot;                     /* the slot F2 saves to and F3 loads from, 1 to 10 */
    char setup_page[ZR_CONFIG_NAME];    /* the page of the setup screen that was open last ("media", "machine"...): F12 opens it again */
    /* the file this was read from (relative paths in it are relative to its folder) */
    char source[ZR_CONFIG_PATH];
} zr_config;

/* The model a new configuration runs. */
#define ZR_CONFIG_DEFAULT_MODEL "test"

void zr_config_defaults(zr_config *config);
/* Reads a file; 0 with a message naming the line when it is not valid. */
int zr_config_load(const char *path, zr_config *config, char *error, size_t capacity);
/* The same from text; source is the file it stands for (relative paths). */
int zr_config_parse(const char *text, size_t length, const char *source, zr_config *config, char *error, size_t capacity);

/* Every setting of config written into doc (yamledit.h) at its key: only
 * values change, comments and order stay. Paths inside the configuration's
 * folder are written relative to it. 0 when out of memory. */
struct zr_ydoc;
int zr_config_store(const zr_config *config, struct zr_ydoc *doc);

/* The value of a machine option as the file words it; NULL when the
 * configuration does not set it (the option is then at its initial value). */
const char *zr_config_option_value(const zr_config *config, const char *key);
/* Sets it (adds the key when missing); 0 when the table is full. */
int zr_config_set_option(zr_config *config, const char *key, const char *value);
/* The settings of a media slot; NULL when the configuration has none.
 * zr_config_media_slot adds an empty one when missing (NULL: table full). */
const zr_config_media *zr_config_media_find(const zr_config *config, const char *key);
zr_config_media *zr_config_media_slot(zr_config *config, const char *key);

/* The end of a port by its key; NULL when the configuration has none (the port is then not connected). The slot form adds
 * an unconnected one when missing (NULL: the table is full). */
const zr_config_port *zr_config_port_find(const zr_config *config, const char *key);
zr_config_port *zr_config_port_slot(zr_config *config, const char *key);
/* An end as the file words it: "none", "loopback", "COM3", "pipe:NAME", "file:out.txt" (a path inside the configuration's
 * folder relative to it), "tcp:HOST:PORT", "tcp-listen:PORT". */
const char *zr_config_port_text(const zr_config *config, const zr_config_port *port, char *buffer, size_t capacity);
/* Whether `target` suits an end of that type: a TCP client needs HOST:PORT, a server [HOST:]PORT, the port 1 to 65535 (0 is
 * allowed for a server: any free port), the others a name that is not empty. ZR_PORT_OK or what is wrong; the second gives it in
 * words. */
enum { ZR_PORT_OK, ZR_PORT_NEEDS_HOST_PORT, ZR_PORT_BAD_TCP_PORT, ZR_PORT_LISTEN_FORM, ZR_PORT_EMPTY };
int zr_config_port_problem(uint32_t type, const char *target);
const char *zr_config_port_problem_text(int problem);

/* The keyboard joystick's keys. The file's name of a function ("up", "fire2") and its default key
 * ("Up", "Left Alt"). */
const char *zr_config_stick_key_name(unsigned function);
const char *zr_config_stick_key_default(unsigned function);
/* What the device of a socket is called in the file: "none", "keyboard" or "controller 2". */
const char *zr_config_stick_device_name(const zr_config_stick *stick, char *buffer, size_t capacity);
/* The shell tells the reader which key names the host knows (SDL's, which the core does not link):
 * without one every printable name is taken. */
void zr_config_set_key_check(int (*known)(const char *name));
/* Whether `name` can be the key of `function`: ZR_KEY_NAME_OK; _BAD when the host has no key of that
 * name; _RESERVED when the window keeps it (F5 to F12, Pause, Print Screen; *reserved is how it is
 * written); _USED when another function of the keyboard joystick has it (*other). */
enum { ZR_KEY_NAME_OK, ZR_KEY_NAME_BAD, ZR_KEY_NAME_RESERVED, ZR_KEY_NAME_USED };
int zr_config_stick_key_problem(const zr_config *config, unsigned function, const char *name, const char **reserved, unsigned *other);

/* The names the file uses for the choice values ("crt", "sharp"). */
const char *zr_config_look_name(uint32_t look);
const char *zr_config_aspect_name(uint32_t aspect);
const char *zr_config_scale_name(uint32_t scale);
const char *zr_config_filter_name(uint32_t filter);
const char *zr_config_border_name(uint32_t border);
const char *zr_config_language_name(uint32_t language); /* "en" */
/* The language a host locale name stands for ("ro-RO", "ro_RO.UTF-8"):
 * Romanian for ro, English for any other. */
uint32_t zr_config_language_for_locale(const char *locale);
/* The language of the host's user locale (host.h); English when it is unknown. */
uint32_t zr_config_default_language(void);

#endif /* ZR_CONFIG_H */
