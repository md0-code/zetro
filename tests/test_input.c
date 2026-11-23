/* The input's smoke test, without SDL: the keyboard joystick's mapping and
 * switch, a stick's directions, the configuration's controllers section
 * (read, refused, written back), and the typist and auto-load on the test
 * machine. */
#include "config.h"
#include "machine.h"
#include "stick.h"
#include "yamledit.h"
#include "../src/app/joykeys.h"
#include "../src/app/typist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

/* --- the keyboard joystick ----------------------------------------------------------------- */

static void test_joykeys(void) {
    enum { UP = 82, DOWN = 81, LEFT = 80, RIGHT = 79, FIRE = 226, FIRE2 = 224, OTHER = 4 };
    const int keys[ZR_CONFIG_STICK_KEYS] = {UP, DOWN, LEFT, RIGHT, FIRE, FIRE2};
    zr_joykeys j;
    zr_joykeys_init(&j, keys, 0);
    /* Off: no key is taken. */
    CHECK(!zr_joykeys_key(&j, UP, 1) && !zr_joykeys_key(&j, UP, 0) && zr_joykeys_held(&j) == 0);
    CHECK(zr_joykeys_switch(&j) == 1 && zr_joykeys_on(&j));
    /* On: the six keys are taken and give their bits; any other key is the machine's. */
    CHECK(zr_joykeys_key(&j, UP, 1) && zr_joykeys_held(&j) == ZR_JOY_UP);
    CHECK(zr_joykeys_key(&j, FIRE, 1) && zr_joykeys_held(&j) == (ZR_JOY_UP | ZR_JOY_FIRE));
    CHECK(!zr_joykeys_key(&j, OTHER, 1) && !zr_joykeys_key(&j, OTHER, 0) && zr_joykeys_held(&j) == (ZR_JOY_UP | ZR_JOY_FIRE));
    /* Opposite directions cancel. */
    CHECK(zr_joykeys_key(&j, DOWN, 1) && zr_joykeys_held(&j) == ZR_JOY_FIRE);
    CHECK(zr_joykeys_key(&j, DOWN, 0) && zr_joykeys_held(&j) == (ZR_JOY_UP | ZR_JOY_FIRE));
    CHECK(zr_joykeys_key(&j, UP, 0) && zr_joykeys_key(&j, FIRE, 0) && zr_joykeys_held(&j) == 0);
    CHECK(zr_joykeys_key(&j, LEFT, 1) && zr_joykeys_key(&j, RIGHT, 1) && zr_joykeys_key(&j, FIRE2, 1) && zr_joykeys_held(&j) == ZR_JOY_FIRE2);
    /* Off again: everything is let go, and a key still down is the machine's when it comes up. */
    CHECK(zr_joykeys_switch(&j) == 0 && zr_joykeys_held(&j) == 0);
    CHECK(!zr_joykeys_key(&j, LEFT, 0) && !zr_joykeys_key(&j, FIRE2, 0));
    /* A key pressed while off and still down when it is switched on is the machine's until let go. */
    CHECK(!zr_joykeys_key(&j, RIGHT, 1));
    CHECK(zr_joykeys_switch(&j) == 1 && !zr_joykeys_key(&j, RIGHT, 0) && zr_joykeys_held(&j) == 0);
    /* A key that is unassigned (-1) is never taken. */
    const int none[ZR_CONFIG_STICK_KEYS] = {-1, -1, -1, -1, -1, -1};
    zr_joykeys_set_keys(&j, none);
    CHECK(!zr_joykeys_key(&j, -1, 1) && !zr_joykeys_key(&j, UP, 1));
    /* A stick's directions, beyond the dead zone only. */
    CHECK(zr_stick_directions(0, 0) == 0 && zr_stick_directions(ZR_STICK_DEAD_ZONE - 1, -(ZR_STICK_DEAD_ZONE - 1)) == 0);
    CHECK(zr_stick_directions(-32768, 0) == ZR_JOY_LEFT && zr_stick_directions(0, 32767) == ZR_JOY_DOWN);
    CHECK(zr_stick_directions(20000, -20000) == (ZR_JOY_RIGHT | ZR_JOY_UP));
    CHECK(zr_stick_resolve(ZR_JOY_LEFT | ZR_JOY_RIGHT | ZR_JOY_FIRE | ZR_JOY_UP) == (ZR_JOY_FIRE | ZR_JOY_UP));
}

/* --- the configuration's controllers ---------------------------------------------------------- */

static int key_known(const char *name) {
    static const char *const NAMES[] = {"Up", "Down", "Left", "Right", "Left Alt", "Left Ctrl", "Space", "A", "S", "Z", "F11", "Pause", "PrintScreen", "F5", NULL};
    for (const char *const *n = NAMES; *n; ++n) if (!strcmp(*n, name)) return 1;
    return 0;
}

static int parse(const char *text, zr_config *c, char *error, size_t capacity) {
    return zr_config_parse(text, strlen(text), "zetro.yaml", c, error, capacity);
}

static int same_controllers(const zr_config *a, const zr_config *b) {
    if (a->keyboard_joystick != b->keyboard_joystick) return 0;
    for (unsigned i = 0; i < ZR_CONFIG_STICKS; ++i)
        if (a->sticks[i].device != b->sticks[i].device || a->sticks[i].pad != b->sticks[i].pad) return 0;
    for (unsigned i = 0; i < ZR_CONFIG_STICK_KEYS; ++i) if (strcmp(a->stick_keys[i], b->stick_keys[i])) return 0;
    return 1;
}

/* The configuration written into the text and read again. */
static int round_trip(const zr_config *c, const char *text, zr_config *back, char **out) {
    zr_ydoc doc;
    char error[256];
    memset(&doc, 0, sizeof doc);
    if (!zr_ydoc_parse(&doc, text, strlen(text)) || !zr_config_store(c, &doc)) return 0;
    size_t length = 0;
    *out = zr_ydoc_text(&doc, &length);
    zr_ydoc_free(&doc);
    return *out && zr_config_parse(*out, length, "zetro.yaml", back, error, sizeof error);
}

static void test_config(void) {
    static zr_config c, back, bad;
    char error[256], *text = NULL;
    zr_config_set_key_check(key_known);
    /* Defaults: the keyboard on socket 1, off until F11, the cursor keys and Left Alt, Left Ctrl for fire. */
    CHECK(parse("machine:\n  model: test\n", &c, error, sizeof error));
    CHECK(c.sticks[0].device == ZR_STICK_KEYBOARD && c.sticks[1].device == ZR_STICK_NONE && !c.keyboard_joystick);
    CHECK(!strcmp(c.stick_keys[ZR_STICK_UP], "Up") && !strcmp(c.stick_keys[ZR_STICK_FIRE], "Left Alt") && !strcmp(c.stick_keys[ZR_STICK_FIRE2], "Left Ctrl"));
    /* Read. */
    static const char FILE_TEXT[] =
        "controllers:\n  joystick1:\n    device: controller 2\n  joystick2:\n    device: keyboard\n  keys:\n    up: Z\n    fire: Space\n"
        "  keyboard_joystick: true\nui:\n  autoload: false\n";
    CHECK(parse(FILE_TEXT, &c, error, sizeof error));
    CHECK(c.sticks[0].device == ZR_STICK_CONTROLLER && c.sticks[0].pad == 2 && c.sticks[1].device == ZR_STICK_KEYBOARD);
    CHECK(!strcmp(c.stick_keys[ZR_STICK_UP], "Z") && !strcmp(c.stick_keys[ZR_STICK_FIRE], "Space") && !strcmp(c.stick_keys[ZR_STICK_DOWN], "Down"));
    CHECK(c.keyboard_joystick && !zr_config_media_find(&c, "tape")->autoload);      /* an older file's ui.autoload: the tape slot's now */
    /* Written back and read again, in a file that has the section and in one that has not. */
    CHECK(round_trip(&c, FILE_TEXT, &back, &text) && same_controllers(&c, &back));
    free(text);
    c.sticks[0].device = ZR_STICK_NONE;
    c.sticks[0].pad = 0;
    snprintf(c.stick_keys[ZR_STICK_LEFT], sizeof c.stick_keys[0], "%s", "S");
    c.keyboard_joystick = 0;
    CHECK(round_trip(&c, FILE_TEXT, &back, &text) && same_controllers(&c, &back));
    free(text);
    CHECK(parse("machine:\n  model: test\n", &c, error, sizeof error));
    CHECK(round_trip(&c, "machine:\n  model: test\n", &back, &text) && same_controllers(&c, &back) && !strstr(text, "joystick") && !strstr(text, "autoload"));
    free(text);
    /* Refused, with the line. */
    CHECK(!parse("controllers:\n  keys:\n    up: F11\n", &bad, error, sizeof error) && strstr(error, "line 3") && strstr(error, "belongs to the window"));
    CHECK(!parse("controllers:\n  keys:\n    up: Pause\n", &bad, error, sizeof error) && strstr(error, "Pause"));
    CHECK(!parse("controllers:\n  keys:\n    left: F5\n", &bad, error, sizeof error) && strstr(error, "F5"));
    CHECK(!parse("controllers:\n  keys:\n    up: Banana\n", &bad, error, sizeof error) && strstr(error, "name of a host key"));
    CHECK(!parse("controllers:\n  keys:\n    up: Left Alt\n", &bad, error, sizeof error) && strstr(error, "controllers.keys.up and controllers.keys.fire"));
    CHECK(!parse("controllers:\n  joystick1:\n    device: controller 9\n", &bad, error, sizeof error) && strstr(error, "line 3"));
    CHECK(!parse("controllers:\n  joystick1:\n    device: pad\n", &bad, error, sizeof error));
    CHECK(!parse("controllers:\n  joystick3:\n    device: none\n", &bad, error, sizeof error) && strstr(error, "line 2"));
    CHECK(!parse("controllers:\n  keys:\n    jump: A\n", &bad, error, sizeof error));
    CHECK(!parse("ui:\n  autoload: maybe\n", &bad, error, sizeof error) && strstr(error, "line 2"));
    /* What the setup screen asks before it takes a key. */
    CHECK(parse("machine:\n  model: test\n", &c, error, sizeof error));
    const char *reserved = NULL;
    unsigned other = 0;
    CHECK(zr_config_stick_key_problem(&c, ZR_STICK_UP, "Left Ctrl", &reserved, &other) == ZR_KEY_NAME_USED && other == ZR_STICK_FIRE2);
    CHECK(zr_config_stick_key_problem(&c, ZR_STICK_UP, "Up", &reserved, &other) == ZR_KEY_NAME_OK);
    CHECK(zr_config_stick_key_problem(&c, ZR_STICK_UP, "F11", &reserved, &other) == ZR_KEY_NAME_RESERVED && !strcmp(reserved, "F11"));
    CHECK(zr_config_stick_key_problem(&c, ZR_STICK_UP, "PrintScreen", &reserved, &other) == ZR_KEY_NAME_RESERVED && !strcmp(reserved, "Print Screen"));
    CHECK(zr_config_stick_key_problem(&c, ZR_STICK_UP, "Banana", &reserved, &other) == ZR_KEY_NAME_BAD);
    zr_config_set_key_check(NULL);
}

/* --- the typist -------------------------------------------------------------------------------- */

static unsigned step_frame(zr_typist *t, zr_machine *m) {
    zr_typist_step(t, m);
    zr_machine_run(m, zr_machine_frame_period(m));
    return 1;
}

/* Steps until the typist is idle (at most `limit` frames); the frames it took. */
static unsigned run_idle(zr_typist *t, zr_machine *m, unsigned limit) {
    unsigned frames = 0;
    while (zr_typist_busy(t) && frames < limit) frames += step_frame(t, m);
    return frames;
}

static int on_screen(zr_machine *m, const char *text) {
    static char screen[4096];
    zr_machine_text(m, screen, sizeof screen);
    return strstr(screen, text) != NULL;
}

/* A tape of two blocks (a header and 100 bytes of data) as a TAP file. */
static zr_media *make_tape(void) {
    uint8_t tap[2 + 19 + 2 + 100];
    memset(tap, 0, sizeof tap);
    tap[0] = 19;
    tap[2 + 1] = 3;
    memcpy(tap + 2 + 2, "ZETRO     ", 10);
    tap[2 + 12] = 98;
    uint8_t sum = 0;
    for (int i = 0; i < 18; ++i) sum ^= tap[2 + i];
    tap[2 + 18] = sum;
    tap[21] = 100;
    tap[23] = 0xFF;
    sum = 0xFF;
    for (int i = 0; i < 98; ++i) { tap[24 + i] = (uint8_t)(i * 7); sum ^= tap[24 + i]; }
    tap[24 + 98] = sum;
    return zr_media_from("demo.tap", tap, sizeof tap);
}

static void test_typist(void) {
    static zr_config c;
    char error[256];
    CHECK(parse("machine:\n  model: test\n", &c, error, sizeof error));
    zr_machine *m = zr_machine_create(&c, NULL, error, sizeof error);
    CHECK(m != NULL);
    if (!m) return;
    zr_typist t;
    zr_typist_init(&t);
    /* A text: four frames a character, as the harness's `type` does. */
    CHECK(!zr_typist_start(&t, m, "a~") && !zr_typist_busy(&t));
    CHECK(zr_typist_start(&t, m, "hello 42") && zr_typist_busy(&t));
    CHECK(run_idle(&t, m, 100) == 8 * 4 + 6 && on_screen(m, "Keys: HELLO 42"));
    /* Escapes as the harness has them: \n is Enter, \b delete. */
    CHECK(zr_typist_start(&t, m, "ab\\bc") && run_idle(&t, m, 100) == 4 * 4 && on_screen(m, "Keys: HELLO 42AC"));
    CHECK(zr_typist_start(&t, m, "\n") && run_idle(&t, m, 100) == 4 && !on_screen(m, "HELLO"));
    /* Cancelled: no more keys. */
    CHECK(zr_typist_start(&t, m, "abcd"));
    for (int i = 0; i < 5; ++i) step_frame(&t, m);
    zr_typist_cancel(&t, m);
    CHECK(!zr_typist_busy(&t));
    for (int i = 0; i < 20; ++i) step_frame(&t, m);
    CHECK(on_screen(m, "Keys: AB") && !on_screen(m, "Keys: ABC"));

    /* Auto-load: nothing without a tape slot, with the setting off or without the model's text. */
    zr_media *tape = make_tape();
    CHECK(tape && zr_machine_insert(m, 0, tape, error, sizeof error));
    zr_config_media_slot(&c, "tape")->autoload = 0;
    CHECK(!zr_typist_autoload(&t, m, &c, 0) && !zr_typist_busy(&t));
    zr_config_media_slot(&c, "tape")->autoload = 1;
    CHECK(!zr_typist_autoload(&t, m, &c, 5) && !zr_typist_busy(&t));
    /* The machine is power cycled (the echo of the keys typed is gone), then it waits 50 frames, types LOAD and Enter (5 keys)
     * and presses PLAY: the recorder shows it runs. */
    CHECK(zr_typist_start(&t, m, "zz") && run_idle(&t, m, 100) == 8 + 6 && on_screen(m, "Keys: ABZZ"));
    CHECK(on_screen(m, "blocks") && !on_screen(m, "blocks >"));
    CHECK(zr_typist_autoload(&t, m, &c, 0) && zr_typist_busy(&t) && !on_screen(m, "Keys: ABZZ"));
    CHECK(run_idle(&t, m, 200) == ZR_TYPIST_READY_MIN + 5 * 4 - 1);
    CHECK(on_screen(m, "blocks >"));
    /* A key of the user's cancels it, and the tape does not start. */
    zr_machine_control(m, 0, ZR_MEDIA_STOP, 0);
    zr_machine_control(m, 0, ZR_MEDIA_REWIND, 0);
    CHECK(zr_typist_autoload(&t, m, &c, 0));
    for (int i = 0; i < 60; ++i) step_frame(&t, m);
    zr_typist_cancel(&t, m);
    CHECK(!zr_typist_busy(&t));
    for (int i = 0; i < 100; ++i) step_frame(&t, m);
    CHECK(!on_screen(m, "blocks >"));
    /* A screen that keeps changing is waited for until the limit, then typed on. */
    CHECK(zr_typist_autoload(&t, m, &c, 0));
    unsigned frames = 0;
    while (zr_typist_busy(&t) && frames < 700) {
        if (frames < ZR_TYPIST_READY_LIMIT - 1) zr_machine_key(m, frames & 1u ? 0 : 1, 1);   /* the echo alternates: '1' '2' '1' ... */
        frames += step_frame(&t, m);
    }
    CHECK(frames == ZR_TYPIST_READY_LIMIT + 5 * 4 - 1);
    zr_machine_destroy(m);
    zr_media_close(tape);
}

int main(void) {
    test_joykeys();
    test_config();
    test_typist();
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
