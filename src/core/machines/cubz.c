/* The ICE Felix CUB Z (machine_impl.h): the family "cubz" with two models, the two original monitor EPROM sets (1.4 and 1.3),
 * a CP/M machine as this project's CUB Z core documents it (MiSTerRO/docs/cubz: README.md, hardware.md, validation.md, the
 * 1989 technical manual and the main board's schematic; core-cubz/rtl/cubz_core.sv with cubz_video.sv, cubz_keyboard.sv,
 * cubz_ctc.sv, cubz_pio.sv, cubz_sio.sv and cubz_dma.sv as the guide to the behaviour). Wired from the chips.
 *
 *   clock    the 10 MHz dot clock is the master clock; the Z80 runs at 2.5 MHz (4 ticks a T-state)
 *   memory   64 KiB of system RAM; 16 KiB of bitmap RAM at 8000-BFFF while PIO-B2 is 1; the 8 KiB of EPROM (U6.2 at E800,
 *            U7.4 at F000, U7.2 at F800) while PIO-B3 is 1, its writes lost; E000-E7FF stays RAM. After reset U7.2 is seen
 *            over the whole address space (the boot alias) until an OUT to port B0. The DMA reaches the system RAM only
 *   ports    decoded by A7-A4 (IORQ with M1 high): 80-8F the 8257 (channel 2 serves the 8272), 90/91 the 8272's status and
 *            data, A0-A3 the user PIO (GPZ board), B0 the boot alias release (reads FFh: no switches), C0-C3 the main PIO, D0-D3
 *            the CTC, E0-E3 the SIO, F2 the scroll register (Fx with A1-A0 = 2), F3 the floppy option latch (A1-A0 = 3)
 *   PIO-B    B0 display enable, B1 beeper, B2 bitmap RAM, B3 EPROM, B4 -DKMON (the motors turn always: the core's), B5 -KBCLK,
 *            B6 KBDATA, B7 the keyboard interrogation (hw/cubzkbd)
 *   video    hw/cubzvideo: 512 x 256, 10 MHz dots, 640 x 313, column-major, the scroll offset, WAIT on the CPU's video RAM
 *            cycles in the serializer's half of each eight-dot window
 *   disk     the 8257 with bus request (the CPU stands still for its cycles, their T-states added) and the 8272 in DMA mode on
 *            channel 2 with terminal count; two 80-track double-sided drives at 300 rpm, 250 kbit/s; the 8272's INT is not
 *            wired (K1/K2 open: the monitor and the BIOS poll)
 *   CTC      at 2.5 MHz, CLK/TRG0-2 a 583.125 kHz clock (the core's), TRG3 -KBCLK; ZC/TO0 and ZC/TO1 clock SIO A and B
 *   SIO      asynchronous, A the serial port, B the serial printer (the user port, an option); CTC then SIO on the daisy chain
 *   sound    the beeper (B1); drive noises (not original)
 *
 * ZetRo's own code. */
#include "machine_impl.h"
#include "media/disk.h"
#include "beeper.h"
#include "bus.h"
#include "cubzkbd.h"
#include "cubzvideo.h"
#include "drivesnd.h"
#include "fdd.h"
#include "i8257.h"
#include "romdb.h"
#include "sched.h"
#include "text.h"
#include "upd765.h"
#include "z80.h"
#include "z80ctc.h"
#include "z80daisy.h"
#include "z80pio.h"
#include "z80sio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(ZR_BUS_TRACE_IN == ZR_TRACE_IO_READ && ZR_BUS_TRACE_OUT == ZR_TRACE_IO_WRITE &&
               ZR_BUS_TRACE_FETCH == ZR_TRACE_FETCH && ZR_BUS_TRACE_INTERRUPT == ZR_TRACE_INTERRUPT,
               "the bus traces in machine.h's kinds");

#define STATE_VERSION 1u
#define CLOCK_HZ 10000000u
#define TICKS_PER_TSTATE 4u                     /* the 2.5 MHz CPU */
#define AUDIO_RATE 48000u
#define DISK_FLUSH_TICKS (CLOCK_HZ / 2u)
#define TRG_NUM 16000u                          /* CLK/TRG0-2: 583125 Hz, 10 MHz / 583125 = 16000 / 933 */
#define TRG_DEN 933u
#define ROM_SIZE 8192u
#define ROM_U72 0x1800u                         /* where the image has U7.2 (F800-FFFF) */
#define RX_QUEUE 64
#define FONT_GLYPHS 95u

enum { SLOT_A, SLOT_B, SLOTS };
enum { PORT_SERIAL, PORT_USER, PORTS_N };
enum { OPTION_MONITOR, OPTION_USER_PORT, OPTION_DRIVE_SOUND, OPTION_COUNT };
enum { USER_SIO_B, USER_PIO_A };

/* --- the family's tables ---------------------------------------------------------------------------------------------- */

static const char *const ROMS14[] = {"cubz14", NULL};
static const char *const ROMS13[] = {"cubz13", NULL};
static const zr_model_desc MODELS[] = {
    {"cubz14", "CUB Z monitor 1.4", ROMS14, NULL},
    {"cubz13", "CUB Z monitor 1.3", ROMS13, NULL},
};
static const char *const SWITCH[] = {"off", "on", NULL};
static const char *const MONITORS[] = {"white", "green", NULL};
static const int MONITOR_LABELS[] = {ZR_T_CUBZ_MONITOR_WHITE, ZR_T_CUBZ_MONITOR_GREEN};
static const char *const USER_PORTS[] = {"serial_b", "pio_a", NULL};
static const int USER_PORT_LABELS[] = {ZR_T_CUBZ_USER_SIO_B, ZR_T_CUBZ_USER_PIO_A};
static const zr_option_desc OPTIONS[OPTION_COUNT] = {
    {"monitor", ZR_T_CUBZ_MONITOR, MONITORS, MONITOR_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET, 0},
    {"user_port", ZR_T_CUBZ_USER_PORT, USER_PORTS, USER_PORT_LABELS, 0, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_PORT, ZR_T_CUBZ_USER_PORT_HINT + 1},
    {"drive_sound", ZR_T_CUBZ_DRIVE_SOUND, SWITCH, NULL, 1, ZR_MODELS_ALL, ZR_OPTION_RESET | ZR_OPTION_NOT_ORIGINAL, ZR_T_CUBZ_DRIVE_SOUND_HINT + 1},
};
static const zr_media_desc MEDIA[SLOTS] = {
    {"a", ZR_T_CUBZ_FLOPPY_A, ZR_MEDIA_FLOPPY, "dsk,edsk,img", ZR_MODELS_ALL},      /* the monitor boots the disk it finds */
    {"b", ZR_T_CUBZ_FLOPPY_B, ZR_MEDIA_FLOPPY, "dsk,edsk,img", ZR_MODELS_ALL},
};
static const zr_port_desc PORTS[PORTS_N] = {
    {"serial", ZR_T_CUBZ_PORT_SERIAL, ZR_PORT_SERIAL, ZR_MODELS_ALL},
    {"user", ZR_T_CUBZ_PORT_USER, ZR_PORT_SERIAL, ZR_MODELS_ALL},
};

/* --- the keyboard ------------------------------------------------------------------------------------------------------- */

/* The keys of the CUB Z keyboard (manual, "Keyboard layout", drawing 369.108.145): each with the matrix position the
 * keyboard's EPROM gives its code at (cubzkbd.c), or a modifier. The alphanumeric block, the function keys, ESC, TAB, LF,
 * RETURN, BACKSPACE, DEL, SPACE and F1-F16 are found in the EPROM by their codes; the numeric pad by the monitor's table of
 * codes (F7A4h: F3h-FCh are the digits 0-9, FDh the point, FEh ENTER) and the EPROM's ASCII ',' and '-'. The other keys of
 * the cursor and editing blocks send E0h-EFh and F0h-F2h, which position is which key is not documented: they are named by
 * the code they send. SETUP/STATUS sends nothing the processor sees; BREAK is taken as the position
 * that sends 80h. The places are the drawing's, narrowed to 24 keys across (the function keys one key wide): the four keys above the
 * editing block (E0-E3) follow F16 and BREAK follows them, so that none overlaps another; the block's middle row (the drawing's E13-E15,
 * the cursor keys and HOME) is three keys, so EB stands in its middle; the numeric pad's keycaps carry the digits. */
#define NOKEY 0xFFFFu
#define K(name, host, x, y) {name, host, name, x, y, 4, 4}
#define KL(name, host, legend, x, y) {name, host, legend, x, y, 4, 4}
static const zr_key_desc KEY_TABLE[] = {
    /* 0-12: the digit row */
    K("1", "1", 4, 4), K("2", "2", 8, 4), K("3", "3", 12, 4), K("4", "4", 16, 4), K("5", "5", 20, 4), K("6", "6", 24, 4),
    K("7", "7", 28, 4), K("8", "8", 32, 4), K("9", "9", 36, 4), K("0", "0", 40, 4), K("-", "-", 44, 4), K("=", "=", 48, 4),
    K("`", "`", 52, 4),
    /* 13-24: Q to ] */
    K("Q", "Q", 6, 8), K("W", "W", 10, 8), K("E", "E", 14, 8), K("R", "R", 18, 8), K("T", "T", 22, 8), K("Y", "Y", 26, 8),
    K("U", "U", 30, 8), K("I", "I", 34, 8), K("O", "O", 38, 8), K("P", "P", 42, 8), K("[", "[", 46, 8), K("]", "]", 50, 8),
    /* 25-35: A to ' */
    K("A", "A", 8, 12), K("S", "S", 12, 12), K("D", "D", 16, 12), K("F", "F", 20, 12), K("G", "G", 24, 12), K("H", "H", 28, 12),
    K("J", "J", 32, 12), K("K", "K", 36, 12), K("L", "L", 40, 12), K(";", ";", 44, 12), K("'", "'", 48, 12),
    /* 36-45: Z to / */
    K("Z", "Z", 8, 16), K("X", "X", 12, 16), K("C", "C", 16, 16), K("V", "V", 20, 16), K("B", "B", 24, 16), K("N", "N", 28, 16),
    K("M", "M", 32, 16), K(",", ",", 36, 16), K(".", ".", 40, 16), K("/", "/", 44, 16),
    /* 46-55: the function keys of the alphanumeric block and the modifiers */
    {"ESC", "Escape", "ESC", 0, 4, 4, 4}, {"BACKSPACE", "Backspace", "BACK SPACE", 56, 4, 6, 4}, {"TAB", "Tab", "TAB", 0, 8, 6, 4},
    {"RETURN", "Return", "RETURN", 52, 12, 8, 4}, {"CTRL", "Left Ctrl", "CTRL", 0, 12, 4, 4}, {"CAPS LOCK", "CapsLock", "CAPS LOCK", 4, 12, 4, 4},
    {"SHIFT", "Left Shift", "SHIFT", 0, 16, 8, 4}, {"RIGHT SHIFT", "Right Shift", "SHIFT", 48, 16, 6, 4}, K("\\", "\\", 54, 16),
    {"LF", "End", "LF", 0, 20, 6, 4},
    /* 56-59 */
    {"SPACE", "Space", "", 8, 20, 36, 4}, {"REP", "Left Alt", "REP", 44, 20, 6, 4}, {"DEL", "Delete", "DEL", 52, 20, 6, 4},
    {"BREAK", "ScrollLock", "BREAK", 88, 0, 6, 4},
    /* 60-75: F1-F16 */
    K("F1", "F1", 8, 0), K("F2", NULL, 12, 0), K("F3", NULL, 16, 0), K("F4", NULL, 20, 0), K("F5", NULL, 24, 0), K("F6", NULL, 28, 0),
    K("F7", NULL, 32, 0), K("F8", NULL, 36, 0), K("F9", NULL, 40, 0), K("F10", NULL, 44, 0), K("F11", NULL, 48, 0), K("F12", NULL, 52, 0),
    K("F13", NULL, 56, 0), K("F14", NULL, 60, 0), K("F15", NULL, 64, 0), K("F16", NULL, 68, 0),
    /* 76-89: the numeric pad */
    KL("PAD 7", "Keypad 7", "7", 80, 8), KL("PAD 8", "Keypad 8", "8", 84, 8), KL("PAD 9", "Keypad 9", "9", 88, 8), KL("PAD -", "Keypad -", "-", 92, 8),
    KL("PAD 4", "Keypad 4", "4", 80, 12), KL("PAD 5", "Keypad 5", "5", 84, 12), KL("PAD 6", "Keypad 6", "6", 88, 12), KL("PAD ,", "Keypad +", ",", 92, 12),
    KL("PAD 1", "Keypad 1", "1", 80, 16), KL("PAD 2", "Keypad 2", "2", 84, 16), KL("PAD 3", "Keypad 3", "3", 88, 16),
    {"ENTER", "Keypad Enter", "ENTER", 92, 16, 4, 8}, {"PAD 0", "Keypad 0", "0", 80, 20, 8, 4}, KL("PAD .", "Keypad .", ".", 88, 20),
    /* 90-108: the keys of the cursor and editing blocks, by the code each sends */
    K("E0", NULL, 72, 0), K("E1", NULL, 76, 0), K("E2", NULL, 80, 0), K("E3", NULL, 84, 0),
    K("E4", NULL, 92, 4), K("E5", NULL, 62, 4), K("E6", NULL, 66, 4), K("E7", NULL, 70, 4),
    K("E8", NULL, 62, 8), K("E9", NULL, 66, 8), K("EA", NULL, 70, 8), K("EB", NULL, 66, 16),
    K("EC", NULL, 62, 16), K("ED", NULL, 70, 16), K("EE", NULL, 66, 12), K("EF", NULL, 66, 20),
    K("F0", NULL, 80, 4), K("F1 PAD", NULL, 84, 4), K("F2 PAD", NULL, 88, 4),
    {"SETUP", NULL, "SETUP STATUS", 0, 0, 6, 4},
};
#undef K
enum { KEYS = sizeof KEY_TABLE / sizeof KEY_TABLE[0] };
static const uint16_t KEY_ID[KEYS] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x92, 0x9D, 0x90, 0x9C, ZR_CUBZKBD_CTRL, ZR_CUBZKBD_CAPS, ZR_CUBZKBD_SHIFT_LEFT, ZR_CUBZKBD_SHIFT_RIGHT, 0x3D, 0x91,
    0x93, ZR_CUBZKBD_REPEAT, 0x3C, 0x94,
    0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
    0x5B, 0x5C, 0x5D, 0x5A, 0x6B, 0x6C, 0x6D, 0x6A, 0x7B, 0x7C, 0x7D, 0x7A, 0x9B, 0x9A,
    0xA9, 0xAB, 0xAC, 0xAD, 0x4A, 0x4E, 0x4F, 0x49, 0x5E, 0x5F, 0x59, 0x6F, 0x7E, 0x7F, 0x79, 0x9F, 0x4B, 0x4C, 0x4D,
    NOKEY,
};
_Static_assert(sizeof KEY_ID / sizeof KEY_ID[0] == KEYS, "a matrix position for each key");

enum { K1 = 0, K2, K3, K4, K5, K6, K7, K8, K9, K0, KMINUS, KEQUAL, KGRAVE,
       KQ, KW, KE, KR, KT, KY, KU, KI, KO, KP, KLBR, KRBR,
       KA, KS, KD, KF, KG, KH, KJ, KK, KL, KSEMI, KQUOTE,
       KZ, KX, KC, KV, KB, KN, KM, KCOMMA, KDOT, KSLASH,
       KESC, KBS, KTAB, KRET, KCTRL, KCAPS, KSHIFT, KRSHIFT, KBSL, KLF, KSPACE, KREP, KDEL, KBREAK,
       KPAD_COMMA = 83, N_ = ZR_KEY_NONE };

/* What is typed for each character: the EPROM's codes alone, with SHIFT, with CTRL. */
static const zr_typing_desc TYPING[] = {
    {'\n', KRET, N_}, {'\r', KRET, N_}, {'\b', KBS, N_}, {'\t', KTAB, N_}, {'\x1b', KESC, N_}, {'\x7f', KDEL, N_}, {' ', KSPACE, N_},
    {'a', KA, N_}, {'b', KB, N_}, {'c', KC, N_}, {'d', KD, N_}, {'e', KE, N_}, {'f', KF, N_}, {'g', KG, N_}, {'h', KH, N_},
    {'i', KI, N_}, {'j', KJ, N_}, {'k', KK, N_}, {'l', KL, N_}, {'m', KM, N_}, {'n', KN, N_}, {'o', KO, N_}, {'p', KP, N_},
    {'q', KQ, N_}, {'r', KR, N_}, {'s', KS, N_}, {'t', KT, N_}, {'u', KU, N_}, {'v', KV, N_}, {'w', KW, N_}, {'x', KX, N_},
    {'y', KY, N_}, {'z', KZ, N_},
    {'A', KA, KSHIFT}, {'B', KB, KSHIFT}, {'C', KC, KSHIFT}, {'D', KD, KSHIFT}, {'E', KE, KSHIFT}, {'F', KF, KSHIFT},
    {'G', KG, KSHIFT}, {'H', KH, KSHIFT}, {'I', KI, KSHIFT}, {'J', KJ, KSHIFT}, {'K', KK, KSHIFT}, {'L', KL, KSHIFT},
    {'M', KM, KSHIFT}, {'N', KN, KSHIFT}, {'O', KO, KSHIFT}, {'P', KP, KSHIFT}, {'Q', KQ, KSHIFT}, {'R', KR, KSHIFT},
    {'S', KS, KSHIFT}, {'T', KT, KSHIFT}, {'U', KU, KSHIFT}, {'V', KV, KSHIFT}, {'W', KW, KSHIFT}, {'X', KX, KSHIFT},
    {'Y', KY, KSHIFT}, {'Z', KZ, KSHIFT},
    {'1', K1, N_}, {'2', K2, N_}, {'3', K3, N_}, {'4', K4, N_}, {'5', K5, N_}, {'6', K6, N_}, {'7', K7, N_}, {'8', K8, N_},
    {'9', K9, N_}, {'0', K0, N_}, {'-', KMINUS, N_}, {'=', KEQUAL, N_}, {'`', KGRAVE, N_}, {'[', KLBR, N_}, {']', KRBR, N_},
    {';', KSEMI, N_}, {'\'', KQUOTE, N_}, {',', KCOMMA, N_}, {'.', KDOT, N_}, {'/', KSLASH, N_}, {'\\', KBSL, N_},
    {'!', K1, KSHIFT}, {'@', K2, KSHIFT}, {'#', K3, KSHIFT}, {'$', K4, KSHIFT}, {'%', K5, KSHIFT}, {'^', K6, KSHIFT},
    {'&', K7, KSHIFT}, {'*', K8, KSHIFT}, {'(', K9, KSHIFT}, {')', K0, KSHIFT}, {'_', KMINUS, KSHIFT}, {'+', KEQUAL, KSHIFT},
    {'~', KGRAVE, KSHIFT}, {'{', KLBR, KSHIFT}, {'}', KRBR, KSHIFT}, {':', KSEMI, KSHIFT}, {'"', KQUOTE, KSHIFT},
    {'<', KCOMMA, KSHIFT}, {'>', KDOT, KSHIFT}, {'?', KSLASH, KSHIFT}, {'|', KBSL, KSHIFT},
    /* the control characters: CTRL and a key */
    {'\x01', KA, KCTRL}, {'\x02', KB, KCTRL}, {'\x03', KC, KCTRL}, {'\x04', KD, KCTRL}, {'\x05', KE, KCTRL}, {'\x06', KF, KCTRL},
    {'\x07', KG, KCTRL}, {'\x0b', KK, KCTRL}, {'\x0c', KL, KCTRL}, {'\x0e', KN, KCTRL}, {'\x0f', KO, KCTRL}, {'\x10', KP, KCTRL},
    {'\x11', KQ, KCTRL}, {'\x12', KR, KCTRL}, {'\x13', KS, KCTRL}, {'\x14', KT, KCTRL}, {'\x15', KU, KCTRL}, {'\x16', KV, KCTRL},
    {'\x17', KW, KCTRL}, {'\x18', KX, KCTRL}, {'\x19', KY, KCTRL}, {'\x1a', KZ, KCTRL}, {'\x1c', KBSL, KCTRL},
    {'\x1d', KRBR, KCTRL}, {'\x1e', KGRAVE, KCTRL}, {'\x1f', KMINUS, KCTRL},
};

/* Host keys the keyboard has none for: the other Ctrl; the cursor keys and Page Up and Down as WordStar's control keys
 * (CTRL and E, X, S, D, R, C), which CP/M's editors and Turbo Pascal take (not original: what the CUB Z's own cursor and
 * editing keys send is in the table above). */
static const zr_combo_desc COMBOS[] = {
    {"Right Ctrl", KCTRL, N_},
    {"Up", KE, KCTRL}, {"Down", KX, KCTRL}, {"Left", KS, KCTRL}, {"Right", KD, KCTRL},
    {"PageUp", KR, KCTRL}, {"PageDown", KC, KCTRL},
};

/* --- the board ------------------------------------------------------------------------------------------------------------ */

typedef struct cubz {
    const zr_family *family;
    unsigned model;
    char description[160], error[160];
    unsigned user_port, drive_sound;
    zr_hw_host host;
    zr_sched sched;
    zr_tclock clock;
    uint64_t target;
    zr_bus bus;
    zr_z80_bus cpu_bus;
    zr_z80 z80;
    zr_reti_watch reti;
    /* memory */
    uint8_t rom[ROM_SIZE];
    uint8_t rom_hash[32];
    uint8_t ram[65536];
    uint8_t vram[ZR_CUBZVIDEO_VRAM];
    /* the latches */
    uint8_t alias;                              /* the boot alias */
    uint8_t pins_b;                             /* the main PIO's port B as the board sees it (inputs pulled high) */
    uint8_t scroll, option;
    uint8_t vram_on, rom_on;                    /* B2 and B3, as the map was last made */
    /* the chips */
    zr_cubzvideo video;
    zr_cubzkbd kbd;
    zr_pio pio, upio;
    zr_ctc ctc;
    zr_sio sio;
    zr_daisy chain[2];
    zr_i8257 dma;
    zr_upd765 fdc;
    zr_fdd fdd[2];
    zr_beeper beeper;
    zr_drivesnd drivesnd;
    zr_dev dma_dev;
    uint64_t sio_num[2], sio_den[2];            /* the SIO's clocks as last set */
    /* the disks */
    zr_media *media[2];
    zr_disk *disk[2];
    uint64_t flush_at;
    /* the byte ports */
    zr_port_host ports;
    uint8_t has_ports, loopback[PORTS_N];
    uint8_t rx_queue[PORTS_N][RX_QUEUE];
    unsigned rx_count[PORTS_N];
    uint8_t dtr_sent[PORTS_N], rts_sent[PORTS_N];
    /* the text read-back: the monitor's character set, from its EPROM */
    int font_ok;
    uint8_t font[FONT_GLYPHS * 10u];            /* ten rows a glyph, as the monitor keeps it at E3B6h */
} cubz;

static uint64_t board_now(const cubz *s) { return s->sched.in_event ? s->sched.event_time : zr_tclock_time(&s->clock, s->z80.t); }
static uint64_t board_at(const cubz *s, unsigned tstates) {
    return s->sched.in_event ? s->sched.event_time : zr_tclock_time(&s->clock, s->z80.t + tstates);
}
static uint64_t host_now(void *ctx) { return board_now(ctx); }
static void host_schedule(void *ctx, void *dev, uint64_t when) { zr_sched_set(&((cubz *)ctx)->sched, dev, when); }

static void update_int(cubz *s) { zr_z80_int(&s->z80, zr_daisy_int(s->chain, 2)); }

/* --- the memory map -------------------------------------------------------------------------------------------------------- */

/* The map (cubz_core.sv): the boot alias shows U7.2 (2 KiB, mirrored) everywhere and takes no write but the video RAM's;
 * after it, the system RAM, the bitmap RAM over 8000-BFFF with B2 and the EPROM over E800-FFFF with B3. */
static void map_all(cubz *s) {
    zr_bus *b = &s->bus;
    s->vram_on = (uint8_t)(s->pins_b >> 2 & 1u);
    s->rom_on = (uint8_t)(s->pins_b >> 3 & 1u);
    if (s->alias) {
        for (unsigned p = 0; p < ZR_BUS_PAGES; ++p) zr_bus_map_rom(b, (uint16_t)(p * ZR_BUS_PAGE_SIZE), ZR_BUS_PAGE_SIZE, s->rom + ROM_U72 + (p & 1u) * ZR_BUS_PAGE_SIZE);
        if (s->vram_on) zr_bus_map_write(b, 0x8000u, 0x4000u, s->vram);
    } else {
        zr_bus_map_ram(b, 0x0000u, 0x10000u, s->ram);
        if (s->vram_on) zr_bus_map_ram(b, 0x8000u, 0x4000u, s->vram);
        if (s->rom_on) zr_bus_map_rom(b, 0xE800u, 0x1800u, s->rom + 0x0800u);
    }
    zr_bus_watch(b, 0x0000u, 0x10000u, 0);
    zr_bus_watch(b, 0x8000u, 0x4000u, s->vram_on);
}

/* A write to the video RAM: the picture is drawn up to it first (a write lands in the cycle's third T-state). */
static void video_write_hook(void *ctx, uint16_t address, uint8_t value) {
    cubz *s = ctx;
    (void)address;
    (void)value;
    zr_cubzvideo_catch_up(&s->video, board_at(s, 2u));
}

/* --- the main PIO's port B -------------------------------------------------------------------------------------------------- */

/* The pins of port B as the board sees them: the PIO's outputs, the inputs pulled high (the core's reset value FFh). */
static uint8_t port_b_pins(const cubz *s) {
    const uint8_t driven = zr_pio_driven(&s->pio, ZR_PIO_PORT_B);
    return (uint8_t)((zr_pio_out(&s->pio, ZR_PIO_PORT_B) & driven) | (uint8_t)~driven);
}

static void port_b_sync(cubz *s) {
    const uint8_t pins = port_b_pins(s), old = s->pins_b;
    if (pins == old) return;
    s->pins_b = pins;
    const uint64_t now = board_now(s);
    if ((pins ^ old) & 0x01u) {
        zr_cubzvideo_catch_up(&s->video, now);
        zr_cubzvideo_set_enable(&s->video, pins & 1u);
    }
    if ((pins ^ old) & 0x02u) zr_beeper_set(&s->beeper, ZR_BEEPER_SPEAKER, pins >> 1 & 1u, now);
    if ((pins ^ old) & 0x0Cu) map_all(s);
    if ((pins ^ old) & 0x80u) s->kbd.dev.ops->input(&s->kbd, ZR_CUBZKBD_IN_INTERROGATE, pins >> 7 & 1u);
}

/* --- the byte ports ---------------------------------------------------------------------------------------------------------- */

static int sio_port(const cubz *s, unsigned channel) {
    if (channel == ZR_SIO_A) return PORT_SERIAL;
    return s->user_port == USER_SIO_B ? PORT_USER : -1;
}

/* Bytes waiting for a port go to the guest as its receiver can take them. */
static void rx_feed(cubz *s, unsigned port) {
    if (!s->rx_count[port]) return;
    if (port == PORT_SERIAL || s->user_port == USER_SIO_B) {
        const unsigned ch = port == PORT_SERIAL ? ZR_SIO_A : ZR_SIO_B;
        if (!zr_sio_rx_ready(&s->sio, ch)) return;
        zr_sio_frame f;
        zr_sio_make_frame(&s->sio, ch, s->rx_queue[port][0], board_now(s), &f);
        if (!zr_sio_rx_frame(&s->sio, ch, &f)) return;
    } else {
        /* The user PIO's port A, as an input with its strobe: the byte on the pins, a strobe pulse. */
        zr_pio *p = &s->upio;
        if (p->p[0].mode == 0) return;
        p->dev.ops->input(p, ZR_PIO_PORT(ZR_PIO_PORT_A), s->rx_queue[port][0]);
        p->dev.ops->input(p, ZR_PIO_IN_ASTB, 0);
        p->dev.ops->input(p, ZR_PIO_IN_ASTB, 1);
    }
    memmove(s->rx_queue[port], s->rx_queue[port] + 1, --s->rx_count[port]);
}

static void rx_put(cubz *s, unsigned port, uint8_t byte) {
    if (s->rx_count[port] < RX_QUEUE) s->rx_queue[port][s->rx_count[port]++] = byte;
    rx_feed(s, port);
}

static void port_out(cubz *s, unsigned port, uint8_t byte) {
    if (s->loopback[port]) rx_put(s, port, byte);
    else if (s->has_ports && s->ports.transmit) s->ports.transmit(s->ports.ctx, port, byte);
}

static void send_outputs(cubz *s) {
    if (!s->has_ports || !s->ports.outputs) return;
    for (unsigned port = 0; port < PORTS_N; ++port) {
        const int ch = port == PORT_SERIAL ? ZR_SIO_A : s->user_port == USER_SIO_B ? ZR_SIO_B : -1;
        if (ch < 0) continue;
        const uint8_t dtr = (uint8_t)zr_sio_dtr(&s->sio, (unsigned)ch), rts = (uint8_t)zr_sio_rts(&s->sio, (unsigned)ch);
        if (dtr == s->dtr_sent[port] && rts == s->rts_sent[port]) continue;
        s->dtr_sent[port] = dtr;
        s->rts_sent[port] = rts;
        s->ports.outputs(s->ports.ctx, port, dtr, rts);
    }
}

static void sio_tx_frame(void *ctx, unsigned channel, const zr_sio_frame *frame) {
    cubz *s = ctx;
    const int port = sio_port(s, channel);
    if (port >= 0 && !frame->brk) port_out(s, (unsigned)port, frame->data);
}
static void sio_rx_ready(void *ctx, unsigned channel) {
    cubz *s = ctx;
    const int port = sio_port(s, channel);
    if (port >= 0) rx_feed(s, (unsigned)port);
}

/* The SIO's clocks are the ZC/TO pulses of CTC channels 0 (A) and 1 (B), as their periods are after each CTC write. */
static void sio_clocks(cubz *s) {
    for (unsigned ch = 0; ch < 2u; ++ch) {
        uint64_t num = 0, den = 1;
        if (!zr_ctc_zc_period(&s->ctc, ch, &num, &den) || num > 0xFFFFFFFFu || den > 0xFFFFFFFFu) num = 0, den = 1;
        if (num == s->sio_num[ch] && den == s->sio_den[ch]) continue;
        s->sio_num[ch] = num;
        s->sio_den[ch] = den;
        zr_sio_set_clock(&s->sio, ch, ZR_SIO_RXC | ZR_SIO_TXC, (uint32_t)num, (uint32_t)den);
    }
}

/* --- the lines between the chips ----------------------------------------------------------------------------------------------- */

static void host_line(void *ctx, void *dev, int pin, int state) {
    cubz *s = ctx;
    if (dev == &s->pio) {
        if (pin >= ZR_PIO_PIN(ZR_PIO_PORT_B, 0) && pin <= ZR_PIO_PIN(ZR_PIO_PORT_B, 7)) port_b_sync(s);
        /* Port A (the parallel connector J3) and the PIO's interrupt are not wired here. */
    } else if (dev == &s->upio) {
        /* The user port: port A's byte goes out when the PIO raises ARDY after a write in mode 0; the peripheral takes it
         * with a strobe. */
        if (s->user_port == USER_PIO_A && pin == ZR_PIO_OUT_ARDY && state && s->upio.p[0].mode == 0) {
            port_out(s, PORT_USER, s->upio.p[0].out);
            s->upio.dev.ops->input(&s->upio, ZR_PIO_IN_ASTB, 0);
            s->upio.dev.ops->input(&s->upio, ZR_PIO_IN_ASTB, 1);
        }
    } else if (dev == &s->kbd) {
        if (pin == ZR_CUBZKBD_OUT_DATA) s->pio.dev.ops->input(&s->pio, ZR_PIO_PIN(ZR_PIO_PORT_B, 6), state);
        else {
            s->pio.dev.ops->input(&s->pio, ZR_PIO_PIN(ZR_PIO_PORT_B, 5), state);
            s->ctc.dev.ops->input(&s->ctc, ZR_CTC_TRG3, state);
        }
    } else if (dev == &s->ctc) {
        if (pin == ZR_CTC_OUT_INT) update_int(s);
    } else if (dev == &s->sio) {
        if (pin == ZR_SIO_OUT_INT) update_int(s);
        else send_outputs(s);
    } else if (dev == &s->dma) {
        if (pin == ZR_I8257_OUT_HRQ && state) zr_dev_schedule(&s->dma_dev, board_now(s));
    } else if (dev == &s->fdc) {
        if (pin == ZR_UPD765_PIN_DRQ) s->dma.dev.ops->input(&s->dma, ZR_I8257_IN_DRQ2, state);
        /* INT: the K1/K2 jumper to NMI is open. */
    }
}

/* The DMA's bus master access: the physical system RAM, below the EPROM and the bitmap RAM. */
static int host_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    cubz *s = ctx;
    uint8_t *d = data;
    for (uint32_t i = 0; i < length; ++i) {
        const uint16_t a = (uint16_t)(address + i);
        if (write) s->ram[a] = d[i];
        else d[i] = s->ram[a];
    }
    return 1;
}

/* The 8257's DACK: channel 2 is the 8272's. */
static uint8_t dma_device(void *ctx, int channel, int to_device, uint8_t value, int tc) {
    cubz *s = ctx;
    if (channel != 2) return 0xFFu;
    return s->fdc.dev.ops->dack(&s->fdc, channel, to_device, value, tc);
}

/* Bus request: when HRQ rises the CPU gives the bus up at the end of its instruction and stands still while the 8257 makes its
 * cycles; their T-states are added to the CPU's. */
static void dma_event(void *dev, uint64_t now) {
    cubz *s = ((zr_dev *)dev)->host->ctx;
    (void)now;
    for (unsigned guard = 0; guard < 0x4000u && zr_i8257_hrq(&s->dma); ++guard) {
        const unsigned t = zr_i8257_cycle(&s->dma);
        if (!t) break;
        s->z80.t += t;
    }
}

static uint8_t no_read(void *dev, unsigned space, uint32_t offset) { (void)dev; (void)space; (void)offset; return 0xFFu; }
static void no_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)dev; (void)space; (void)offset; (void)value; }
static void no_reset(void *dev) { (void)dev; }
static const zr_dev_ops DMA_OPS = {.read = no_read, .write = no_write, .event = dma_event, .reset = no_reset};

/* --- the CPU's bus ------------------------------------------------------------------------------------------------------------- */

static uint8_t cpu_read(void *ctx, uint16_t address, int m1) {
    cubz *s = ctx;
    const uint8_t v = zr_bus_read(&s->bus, address);
    if (m1 && zr_reti_m1(&s->reti, v) && zr_daisy_reti(s->chain, 2)) update_int(s);
    return v;
}
static void cpu_write(void *ctx, uint16_t address, uint8_t value) { zr_bus_write(&((cubz *)ctx)->bus, address, value); }
static uint8_t cpu_in(void *ctx, uint16_t port) { return zr_bus_in(&((cubz *)ctx)->bus, port); }
static void cpu_out(void *ctx, uint16_t port, uint8_t value) { zr_bus_out(&((cubz *)ctx)->bus, port, value); }
static uint8_t cpu_ack(void *ctx) {
    cubz *s = ctx;
    const uint8_t v = zr_daisy_ack(s->chain, 2);
    zr_bus_trace(&s->bus, ZR_BUS_TRACE_INTERRUPT, v, v);
    update_int(s);
    return v;
}

/* WAIT on the video RAM: a memory cycle on 8000-BFFF while the bitmap RAM is mapped is held one T-state when its second
 * T-state, where the Z80 samples WAIT, falls in the serializer's half of the eight-dot window. */
static unsigned cpu_stretch(void *ctx, int kind, uint16_t address) {
    cubz *s = ctx;
    if (kind > ZR_Z80_WRITE || !s->vram_on || (address >> 14) != 2u) return 0;
    if (s->alias && kind != ZR_Z80_WRITE) return 0;
    return (unsigned)zr_cubzvideo_wait(zr_tclock_time(&s->clock, s->z80.t + 1u));
}

/* --- the ports ----------------------------------------------------------------------------------------------------------------- */

static uint8_t port_read(void *ctx, uint16_t port) {
    cubz *s = ctx;
    switch (port >> 4 & 15u) {
    case 0x8: return s->dma.dev.ops->read(&s->dma, ZR_SPACE_IO, port & 15u);
    case 0x9: return s->fdc.dev.ops->read(&s->fdc, ZR_SPACE_IO, port & 1u);
    case 0xA: return s->upio.dev.ops->read(&s->upio, ZR_SPACE_IO, port & 3u);
    case 0xC: return s->pio.dev.ops->read(&s->pio, ZR_SPACE_IO, port & 3u);
    case 0xD: return s->ctc.dev.ops->read(&s->ctc, ZR_SPACE_IO, port & 3u);
    case 0xE: return s->sio.dev.ops->read(&s->sio, ZR_SPACE_IO, port & 3u);
    default: return 0xFFu;
    }
}

static void port_write(void *ctx, uint16_t port, uint8_t value) {
    cubz *s = ctx;
    switch (port >> 4 & 15u) {
    case 0x8: s->dma.dev.ops->write(&s->dma, ZR_SPACE_IO, port & 15u, value); break;
    case 0x9: if (port & 1u) s->fdc.dev.ops->write(&s->fdc, ZR_SPACE_IO, 1, value); break;
    case 0xA: s->upio.dev.ops->write(&s->upio, ZR_SPACE_IO, port & 3u, value); break;
    case 0xB:
        if (s->alias) {
            s->alias = 0;
            map_all(s);
        }
        break;
    case 0xC: s->pio.dev.ops->write(&s->pio, ZR_SPACE_IO, port & 3u, value); break;
    case 0xD:
        s->ctc.dev.ops->write(&s->ctc, ZR_SPACE_IO, port & 3u, value);
        sio_clocks(s);
        break;
    case 0xE: s->sio.dev.ops->write(&s->sio, ZR_SPACE_IO, port & 3u, value); break;
    case 0xF:
        if ((port & 3u) == 2u) {
            zr_cubzvideo_catch_up(&s->video, board_at(s, 3u));
            s->scroll = value;
            zr_cubzvideo_set_scroll(&s->video, value);
        } else if ((port & 3u) == 3u) s->option = value;
        break;
    default: break;
    }
}

/* --- the disks ------------------------------------------------------------------------------------------------------------------- */

static void flush_disks(cubz *s, uint64_t now, int force) {
    if (!force && (now < s->flush_at || s->fdc.phase != 0)) return;
    for (unsigned u = 0; u < 2u; ++u) {
        if (!s->disk[u] || !s->disk[u]->dirty || !s->media[u]) continue;
        char why[128];
        if (zr_disk_save_media(s->disk[u], s->media[u], why, sizeof why)) s->flush_at = now + DISK_FLUSH_TICKS;
        else snprintf(s->error, sizeof s->error, "%s", why);
    }
}

/* --- the picture and the text ---------------------------------------------------------------------------------------------------- */

static int cubz_frame(void *self, zr_frame *frame) { return zr_cubzvideo_frame(&((cubz *)self)->video, frame); }

/* The monitor's character set: 95 glyphs (20h-7Eh), five dots in bits 7-3, in its EPROM, found by its first glyphs: v1.3 keeps
 * them as ten rows (a blank row above and below, at F3CFh) and copies them to E3B6h, v1.4 as eight rows (at F477h) and adds the
 * blank rows when it copies them there. Either way the monitor draws cells of 6 x 10 dots. */
static void find_font(cubz *s) {
    static const uint8_t TEN[20] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00, 0x00};
    static const uint8_t EIGHT[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00};
    s->font_ok = 0;
    for (uint32_t a = 0; a + FONT_GLYPHS * 10u <= ROM_SIZE; ++a)
        if (!memcmp(s->rom + a, TEN, sizeof TEN)) {
            memcpy(s->font, s->rom + a, sizeof s->font);
            s->font_ok = 1;
            return;
        }
    for (uint32_t a = 0; a + FONT_GLYPHS * 8u <= ROM_SIZE; ++a)
        if (!memcmp(s->rom + a, EIGHT, sizeof EIGHT)) {
            memset(s->font, 0, sizeof s->font);
            for (unsigned g = 0; g < FONT_GLYPHS; ++g) memcpy(s->font + g * 10u + 1u, s->rom + a + g * 8u, 8);
            s->font_ok = 1;
            return;
        }
}

#define CELL_W 6u
#define CELL_H 10u
#define TEXT_COLS 85u
#define TEXT_ROWS 25u

static unsigned screen_dot(const cubz *s, unsigned x, unsigned y) {
    return s->vram[zr_cubzvideo_address(x, y, s->scroll)] >> (7u - (x & 7u)) & 1u;
}

/* Each 6 x 10 cell of the screen (with the scroll offset) matched against the character set, as it is or inverted (the
 * cursor); a blank cell is a space and one that matches nothing '?'. */
static size_t cubz_text(void *self, char *out, size_t capacity) {
    cubz *s = self;
    size_t n = 0;
    if (!s->font_ok) return 0;
    for (unsigned row = 0; row < TEXT_ROWS; ++row) {
        char line[TEXT_COLS];
        unsigned last = 0;
        for (unsigned col = 0; col < TEXT_COLS; ++col) {
            uint8_t cell[CELL_H];
            for (unsigned y = 0; y < CELL_H; ++y) {
                unsigned v = 0;
                for (unsigned x = 0; x < CELL_W; ++x) v = v << 1 | screen_dot(s, col * CELL_W + x, row * CELL_H + y);
                cell[y] = (uint8_t)v;
            }
            /* A glyph as it is or inverted; failing that, with the rows that are all lit left out (an underline cursor). */
            char ch = '?';
            for (unsigned pass = 0; pass < 2u && ch == '?'; ++pass)
                for (unsigned g = 0; g < FONT_GLYPHS && ch == '?'; ++g) {
                    unsigned same = 1, inverted = 1;
                    for (unsigned y = 0; y < CELL_H; ++y) {
                        const uint8_t want = (uint8_t)(s->font[g * CELL_H + y] >> 2);
                        if (pass && cell[y] == 0x3Fu) continue;
                        if (cell[y] != want) same = 0;
                        if (cell[y] != (uint8_t)(~want & 0x3Fu)) inverted = 0;
                    }
                    if (same || (inverted && !pass)) ch = (char)(0x20u + g);
                }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* --- life cycle ------------------------------------------------------------------------------------------------------------------ */

static void cubz_destroy(void *self) {
    cubz *s = self;
    if (!s) return;
    flush_disks(s, board_now(s), 1);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_fdd_eject(&s->fdd[u]);
        zr_disk_close(s->disk[u]);
    }
    free(s);
}

static const char *cubz_description(const void *self) { return ((const cubz *)self)->description; }
static const char *cubz_error(const void *self) { return ((const cubz *)self)->error; }

/* Power on: the memories cleared, the processor as it wakes; RESET: the board's reset line (the processor, the alias, the
 * PIOs, CTC, SIO, 8257, 8272, the scroll and option latches), the memories kept. The keyboard has its own power. */
static void cubz_reset(void *self, int power_on) {
    cubz *s = self;
    const uint64_t now = board_now(s);
    zr_cubzvideo_catch_up(&s->video, now);
    if (power_on) {
        memset(s->ram, 0, sizeof s->ram);
        memset(s->vram, 0, sizeof s->vram);
        zr_z80_power(&s->z80);
        s->kbd.dev.ops->reset(&s->kbd);
    } else zr_z80_reset(&s->z80);
    s->reti.prefix = 0;
    s->alias = 1;
    s->scroll = 0;
    s->option = 0xFFu;
    s->video.dev.ops->reset(&s->video);
    s->pio.dev.ops->reset(&s->pio);
    s->upio.dev.ops->reset(&s->upio);
    s->ctc.dev.ops->reset(&s->ctc);
    s->sio.dev.ops->reset(&s->sio);
    s->dma.dev.ops->reset(&s->dma);
    s->fdc.dev.ops->reset(&s->fdc);
    zr_dev_schedule(&s->dma_dev, ZR_NEVER);
    /* The keyboard's lines on the PIO, the user port's inputs pulled high. */
    s->pio.dev.ops->input(&s->pio, ZR_PIO_PORT(ZR_PIO_PORT_A), 0xFF);
    s->pio.dev.ops->input(&s->pio, ZR_PIO_PORT(ZR_PIO_PORT_B), 0x9Fu | (zr_cubzkbd_clock(&s->kbd) ? 0x20u : 0u) | (zr_cubzkbd_data(&s->kbd) ? 0x40u : 0u));
    s->upio.dev.ops->input(&s->upio, ZR_PIO_PORT(ZR_PIO_PORT_A), 0xFF);
    s->upio.dev.ops->input(&s->upio, ZR_PIO_PORT(ZR_PIO_PORT_B), 0xFF);
    s->pins_b = 0;
    s->pins_b = (uint8_t)~port_b_pins(s);       /* every pin seen as changed once */
    port_b_sync(s);
    for (unsigned ch = 0; ch < 3u; ++ch) zr_ctc_set_trigger_clock(&s->ctc, ch, TRG_NUM, TRG_DEN);
    s->ctc.dev.ops->input(&s->ctc, ZR_CTC_TRG3, zr_cubzkbd_clock(&s->kbd));
    s->sio_num[0] = s->sio_num[1] = 1;          /* set again below */
    sio_clocks(s);
    for (unsigned u = 0; u < 2u; ++u)
        if (!s->fdd[u].motor) zr_fdd_set_motor(&s->fdd[u], 1, now);
    zr_upd765_drive_changed(&s->fdc);
    map_all(s);
    update_int(s);
}

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
static int fail(char *error, size_t capacity, const char *message, const char *what) {
    if (capacity) snprintf(error, capacity, message, what);
    return 0;
}

static void *cubz_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    cubz *s = calloc(1, sizeof *s);
    if (!s) {
        fail(error, capacity, "%s", "Out of memory");
        return NULL;
    }
    s->family = setup->family;
    s->model = setup->model;
    s->user_port = setup->options[OPTION_USER_PORT] ? USER_PIO_A : USER_SIO_B;
    s->drive_sound = setup->options[OPTION_DRIVE_SOUND] != 0;
    snprintf(s->description, sizeof s->description, "%s", setup->family->models[setup->model].name);
    /* The EPROMs, in the core's 8 KiB layout. */
    const char *key = setup->family->models[setup->model].roms[0];
    const zr_known_rom *rom = zr_romdb_find(key);
    const zr_rom_file *file = rom && setup->roms ? zr_rom_index_find(setup->roms, rom) : NULL;
    if (!file || !file->data || file->size != ROM_SIZE) {
        fail(error, capacity, "The ROM %s is missing", rom ? rom->file : key);
        free(s);
        return NULL;
    }
    memcpy(s->rom, file->data, ROM_SIZE);
    zr_sha256(s->rom, ROM_SIZE, s->rom_hash);
    find_font(s);
    /* The board. */
    s->host.ctx = s;
    s->host.now = host_now;
    s->host.schedule = host_schedule;
    s->host.line = host_line;
    s->host.dma = host_dma;
    zr_sched_init(&s->sched);
    zr_tclock_init(&s->clock, TICKS_PER_TSTATE, 0, 0);
    zr_bus_init(&s->bus);
    zr_bus_set_hook(&s->bus, video_write_hook, s);
    zr_bus_add_io(&s->bus, 0x0000u, 0x0000u, port_read, port_write, s);
    s->cpu_bus.ctx = s;
    s->cpu_bus.read = cpu_read;
    s->cpu_bus.write = cpu_write;
    s->cpu_bus.in = cpu_in;
    s->cpu_bus.out = cpu_out;
    s->cpu_bus.ack = cpu_ack;
    s->cpu_bus.stretch = cpu_stretch;
    zr_z80_init(&s->z80, &s->cpu_bus, NULL, 0);
    /* The chips. */
    zr_cubzvideo_init(&s->video, &s->host, s->vram, setup->options[OPTION_MONITOR] ? ZR_CUBZVIDEO_GREEN : ZR_CUBZVIDEO_WHITE);
    zr_cubzkbd_init(&s->kbd, &s->host, "keyboard", CLOCK_HZ);
    zr_pio_init(&s->pio, &s->host, "pio");
    zr_pio_init(&s->upio, &s->host, "user pio");
    zr_ctc_init(&s->ctc, &s->host, "ctc", TICKS_PER_TSTATE, 1u);
    zr_ctc_watch(&s->ctc, 0u);                  /* ZC/TO is wired to the SIO's clocks only, which follow the periods */
    zr_sio_init(&s->sio, &s->host, "sio", CLOCK_HZ);
    const zr_sio_link link = {s, sio_tx_frame, NULL, sio_rx_ready};
    zr_sio_set_link(&s->sio, &link);
    s->chain[0] = s->ctc.daisy;
    s->chain[1] = s->sio.daisy;
    zr_i8257_init(&s->dma, &s->host, "8257", dma_device, s);
    zr_upd765_init(&s->fdc, &s->host, "8272", CLOCK_HZ);
    zr_upd765_set_rate(&s->fdc, 250u);
    zr_upd765_set_clock(&s->fdc, 8000u);
    zr_upd765_ready_wiring(&s->fdc, ZR_UPD765_READY_DRIVE);
    zr_beeper_init(&s->beeper, &s->host, CLOCK_HZ, AUDIO_RATE);
    zr_drivesnd_init(&s->drivesnd, CLOCK_HZ, AUDIO_RATE);
    zr_drivesnd_set(&s->drivesnd, (int)s->drive_sound, 50);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_fdd_init(&s->fdd[u], CLOCK_HZ, 1, 80, 300);
        if (s->drive_sound) zr_fdd_set_hook(&s->fdd[u], zr_drivesnd_event, &s->drivesnd, u);
        zr_upd765_attach(&s->fdc, u, &s->fdd[u]);
    }
    s->dma_dev.ops = &DMA_OPS;
    s->dma_dev.host = &s->host;
    s->dma_dev.name = "bus request";
    s->dtr_sent[0] = s->dtr_sent[1] = s->rts_sent[0] = s->rts_sent[1] = 0xFFu;
    cubz_reset(s, 1);
    return s;
}

/* --- time --------------------------------------------------------------------------------------------------------------------------- */

static uint32_t cubz_clock(const void *self) { (void)self; return CLOCK_HZ; }
static uint64_t cubz_time(const void *self) { return board_now(self); }
static uint64_t cubz_frame_period(const void *self) { return zr_cubzvideo_frame_ticks(&((const cubz *)self)->video); }

static zr_run_status cubz_run(void *self, uint64_t ticks) {
    cubz *s = self;
    s->target += ticks;
    const uint64_t now = zr_sched_run_z80(&s->sched, &s->z80, &s->clock, s->target);
    zr_cubzvideo_catch_up(&s->video, now);
    flush_disks(s, now, 0);
    for (unsigned port = 0; port < PORTS_N; ++port) rx_feed(s, port);
    return ZR_RUN_OK;
}

/* --- sound -------------------------------------------------------------------------------------------------------------------------- */

static uint32_t cubz_audio_rate(const void *self) { (void)self; return AUDIO_RATE; }
static size_t cubz_audio_pull(void *self, int16_t *stereo, size_t frames) {
    cubz *s = self;
    const uint64_t now = board_now(s);
    const size_t n = zr_beeper_render(&s->beeper, now, stereo, frames);
    if (zr_drivesnd_enabled(&s->drivesnd)) zr_drivesnd_render_add(&s->drivesnd, now, stereo, n);
    return n;
}

/* --- input -------------------------------------------------------------------------------------------------------------------------- */

static void cubz_key(void *self, unsigned key, int down) {
    cubz *s = self;
    if (key >= KEYS || KEY_ID[key] == NOKEY) return;
    zr_cubzkbd_key(&s->kbd, KEY_ID[key], down);
}

/* --- media -------------------------------------------------------------------------------------------------------------------------- */

static int cubz_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    cubz *s = self;
    if (slot >= SLOTS) return fail(error, capacity, "%s", "There is no such slot");
    if (s->media[slot]) return fail(error, capacity, "%s", "The slot is not empty");
    static const zr_disk_geom GEOMETRY = {80, 2, 9, 512, ZR_ENC_MFM};
    zr_disk *disk = zr_disk_open(media, &GEOMETRY, error, capacity);
    if (!disk) return 0;
    if (!zr_fdd_insert(&s->fdd[slot], disk, error, capacity)) {
        zr_disk_close(disk);
        return 0;
    }
    s->media[slot] = media;
    s->disk[slot] = disk;
    media->disk = disk;
    zr_upd765_drive_changed(&s->fdc);
    return 1;
}

static zr_media *cubz_media(const void *self, unsigned slot) {
    const cubz *s = self;
    return slot < SLOTS ? s->media[slot] : NULL;
}

static int cubz_eject(void *self, unsigned slot) {
    cubz *s = self;
    if (slot >= SLOTS || !s->media[slot]) return 0;
    flush_disks(s, board_now(s), 1);
    zr_fdd_eject(&s->fdd[slot]);
    zr_disk_close(s->disk[slot]);
    s->disk[slot] = NULL;
    s->media[slot] = NULL;
    zr_upd765_drive_changed(&s->fdc);
    return 1;
}

/* A drive works while the controller has a command on it or its head moves (the motors always turn). */
static unsigned cubz_activity(const void *self) {
    const cubz *s = self;
    unsigned bits = 0;
    for (unsigned u = 0; u < 2u; ++u)
        if (s->disk[u] && (s->fdc.seeking[u] || (s->fdc.phase != 0 && (s->fdc.x.unit & 3u) == u))) bits |= 1u << u;
    return bits;
}

/* --- the byte ports ------------------------------------------------------------------------------------------------------------------ */

static void cubz_set_ports(void *self, const zr_port_host *host) {
    cubz *s = self;
    s->has_ports = host != NULL;
    if (host) s->ports = *host;
    else memset(&s->ports, 0, sizeof s->ports);
    for (unsigned port = 0; port < PORTS_N; ++port) {
        s->loopback[port] = host && host->loopback && host->loopback(host->ctx, port) ? 1u : 0u;
        s->dtr_sent[port] = s->rts_sent[port] = 0xFFu;
    }
    send_outputs(s);
}

static size_t cubz_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    cubz *s = self;
    if (port >= PORTS_N) return count;
    size_t taken = 0;
    while (taken < count && s->rx_count[port] < RX_QUEUE) s->rx_queue[port][s->rx_count[port]++] = data[taken++];
    rx_feed(s, port);
    return taken;
}

static void cubz_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    cubz *s = self;
    (void)dsr;
    (void)ri;
    const int ch = port == PORT_SERIAL ? ZR_SIO_A : (port == PORT_USER && s->user_port == USER_SIO_B) ? ZR_SIO_B : -1;
    if (ch < 0) return;
    s->sio.dev.ops->input(&s->sio, ZR_SIO_IN_DCD(ch), dcd != 0);
    s->sio.dev.ops->input(&s->sio, ZR_SIO_IN_CTS(ch), cts != 0);
}

/* --- states --------------------------------------------------------------------------------------------------------------------------- */

static void chunk(const zr_dev *d, zr_state_writer *w, const char tag[4], uint32_t instance) {
    zr_sw_begin(w, tag, instance);
    d->ops->save(d, w);
    zr_sw_end(w);
}

static void cubz_save(const void *self, zr_state_writer *w) {
    const cubz *s = self;
    zr_sw_begin(w, "CUBZ", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_tclock_save(&s->clock, w);
    zr_sw_u64(w, s->target);
    zr_sw_u8(w, s->alias);
    zr_sw_u8(w, s->scroll);
    zr_sw_u8(w, s->option);
    zr_sw_u8(w, s->reti.prefix);
    zr_sw_u64(w, zr_sched_get(&s->sched, &s->dma_dev));
    for (unsigned port = 0; port < PORTS_N; ++port) {
        zr_sw_u32(w, s->rx_count[port]);
        zr_sw_bytes(w, s->rx_queue[port], RX_QUEUE);
    }
    zr_sw_end(w);
    zr_sw_begin(w, "Z80 ", 0);
    zr_z80_save(&s->z80, w);
    zr_sw_end(w);
    zr_sw_begin(w, "RAM ", 0);
    zr_sw_packed(w, s->ram, sizeof s->ram);
    zr_sw_end(w);
    zr_sw_begin(w, "VRAM", 0);
    zr_sw_packed(w, s->vram, sizeof s->vram);
    zr_sw_end(w);
    chunk(&s->video.dev, w, "VID ", 0);
    chunk(&s->kbd.dev, w, "KBD ", 0);
    chunk(&s->pio.dev, w, "PIO ", 0);
    chunk(&s->upio.dev, w, "PIO ", 1);
    chunk(&s->ctc.dev, w, "CTC ", 0);
    chunk(&s->sio.dev, w, "SIO ", 0);
    chunk(&s->dma.dev, w, "8257", 0);
    chunk(&s->fdc.dev, w, "8272", 0);
    chunk(&s->beeper.dev, w, "BEEP", 0);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_sw_begin(w, "FDD ", u);
        zr_fdd_save(&s->fdd[u], w);
        zr_sw_end(w);
    }
}

static int load_chunk(zr_dev *d, zr_state_reader *r, const char tag[4], uint32_t instance) {
    return zr_sr_chunk(r, tag, instance) && d->ops->load(d, r);
}

static int cubz_load(void *self, zr_state_reader *r) {
    cubz *s = self;
    if (!zr_sr_chunk(r, "CUBZ", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_tclock_load(&s->clock, r);
    s->target = zr_sr_u64(r);
    s->alias = zr_sr_u8(r) & 1u;
    s->scroll = zr_sr_u8(r);
    s->option = zr_sr_u8(r);
    s->reti.prefix = zr_sr_u8(r) & 1u;
    const uint64_t dma_at = zr_sr_u64(r);
    for (unsigned port = 0; port < PORTS_N; ++port) {
        s->rx_count[port] = zr_sr_u32(r);
        if (s->rx_count[port] > RX_QUEUE) return 0;
        zr_sr_bytes(r, s->rx_queue[port], RX_QUEUE);
    }
    if (!zr_sr_ok(r) || !zr_sr_chunk(r, "Z80 ", 0)) return 0;
    zr_z80_load(&s->z80, r);
    if (!zr_sr_chunk(r, "RAM ", 0)) return 0;
    zr_sr_packed(r, s->ram, sizeof s->ram);
    if (!zr_sr_chunk(r, "VRAM", 0)) return 0;
    zr_sr_packed(r, s->vram, sizeof s->vram);
    if (!zr_sr_ok(r)) return 0;
    if (!load_chunk(&s->video.dev, r, "VID ", 0) || !load_chunk(&s->kbd.dev, r, "KBD ", 0) || !load_chunk(&s->pio.dev, r, "PIO ", 0) ||
        !load_chunk(&s->upio.dev, r, "PIO ", 1) || !load_chunk(&s->ctc.dev, r, "CTC ", 0) || !load_chunk(&s->sio.dev, r, "SIO ", 0) ||
        !load_chunk(&s->dma.dev, r, "8257", 0) || !load_chunk(&s->fdc.dev, r, "8272", 0) || !load_chunk(&s->beeper.dev, r, "BEEP", 0))
        return 0;
    for (unsigned u = 0; u < 2u; ++u)
        if (!zr_sr_chunk(r, "FDD ", u) || !zr_fdd_load(&s->fdd[u], r)) return 0;
    s->fdc.x.fdd = s->fdc.drive[s->fdc.x.unit & 3u];
    /* What the loads leave to the board: the pins it reads, the map, its own deadline, the SIO's clocks (saved in the SIO). */
    s->pins_b = port_b_pins(s);
    map_all(s);
    zr_cubzvideo_set_scroll(&s->video, s->scroll);
    zr_dev_schedule(&s->dma_dev, dma_at);
    for (unsigned ch = 0; ch < 2u; ++ch)
        if (!zr_ctc_zc_period(&s->ctc, ch, &s->sio_num[ch], &s->sio_den[ch])) s->sio_num[ch] = 0, s->sio_den[ch] = 1;
    zr_drivesnd_reset(&s->drivesnd, board_now(s));
    for (unsigned port = 0; port < PORTS_N; ++port) s->dtr_sent[port] = s->rts_sent[port] = 0xFFu;
    send_outputs(s);
    s->flush_at = 0;
    update_int(s);
    return zr_sr_ok(r);
}

static uint32_t cubz_rom_hashes(const void *self, uint8_t *hashes, uint32_t max) {
    const cubz *s = self;
    if (!max) return 0;
    memcpy(hashes, s->rom_hash, 32);
    return 1;
}

/* --- checks ------------------------------------------------------------------------------------------------------------------------------ */

static void cubz_set_trace(void *self, zr_trace_fn fn, void *ctx) { zr_bus_set_trace(&((cubz *)self)->bus, fn, ctx); }

/* The memory as the CPU reads it now; from 100000h the RAM pages by number (machine.h's ZR_PAGE_ADDRESS): pages 0-3 the system
 * RAM's quarters whatever is mapped over them, page 4 the bitmap RAM. */
static uint8_t *page_byte(cubz *s, uint32_t address) {
    const uint32_t page = (address - 0x100000u) >> 14, offset = address & 0x3FFFu;
    if (address < 0x100000u) return NULL;
    if (page < 4u) return s->ram + page * 0x4000u + offset;
    if (page == 4u) return s->vram + offset;
    return NULL;
}
static int cubz_peek(void *self, uint32_t address, uint8_t *value) {
    cubz *s = self;
    if (address > 0xFFFFu) {
        const uint8_t *p = page_byte(s, address);
        if (!p) return 0;
        *value = *p;
        return 1;
    }
    *value = zr_bus_peek(&s->bus, (uint16_t)address);
    return 1;
}
static int cubz_poke(void *self, uint32_t address, uint8_t value) {
    cubz *s = self;
    if (address > 0xFFFFu) {
        uint8_t *p = page_byte(s, address);
        if (!p) return 0;
        if (p >= s->vram && p < s->vram + sizeof s->vram) zr_cubzvideo_catch_up(&s->video, board_now(s));
        *p = value;
        return 1;
    }
    zr_bus_write(&s->bus, (uint16_t)address, value);
    return 1;
}

static const zr_machine_impl CUBZ_IMPL = {
    .destroy = cubz_destroy, .description = cubz_description, .error = cubz_error,
    .reset = cubz_reset, .nmi = NULL,
    .clock = cubz_clock, .run = cubz_run, .time = cubz_time, .frame_period = cubz_frame_period,
    .frame = cubz_frame, .text = cubz_text, .audio_rate = cubz_audio_rate, .audio_pull = cubz_audio_pull,
    .key = cubz_key, .joystick = NULL,
    .insert = cubz_insert, .media = cubz_media, .eject = cubz_eject, .activity = cubz_activity, .control = NULL,
    .set_ports = cubz_set_ports, .port_receive = cubz_port_receive, .port_modem = cubz_port_modem,
    .save = cubz_save, .load = cubz_load, .rom_hashes = cubz_rom_hashes,
    .set_trace = cubz_set_trace, .peek = cubz_peek, .poke = cubz_poke, .tape_deck = NULL,
};

const zr_family zr_cubz_family = {
    .id = "cubz", .name = "CUB Z",
    .models = MODELS, .model_count = sizeof MODELS / sizeof MODELS[0],
    .options = OPTIONS, .option_count = OPTION_COUNT,
    .media = MEDIA, .media_count = SLOTS,
    .ports = PORTS, .port_count = PORTS_N,
    .keys = KEY_TABLE, .key_count = KEYS,
    .typing = TYPING, .typing_count = sizeof TYPING / sizeof TYPING[0],
    .combos = COMBOS, .combo_count = sizeof COMBOS / sizeof COMBOS[0],
    .joysticks = 0,
    .impl = &CUBZ_IMPL, .create = cubz_create,
};
