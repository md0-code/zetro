/* The core's smoke test: a configuration is read and checked against the
 * registry, the machine it names is built, and its state survives a save
 * and a load. No SDL. */
#include "config.h"
#include "machine.h"
#include "text.h"
#include "zr_version.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

int main(void) {
    static zr_config config;
    char error[256];
    static const char GOOD[] = "machine:\n  model: test\n  options:\n    beep: off\nmedia:\n  tape:\n    image: demo.tzx\nvideo:\n  look: plain\n";
    CHECK(zr_config_parse(GOOD, sizeof GOOD - 1, "work/zetro.yaml", &config, error, sizeof error));
    CHECK(!strcmp(zr_config_option_value(&config, "beep"), "off") && config.look == ZR_LOOK_PLAIN);
    CHECK(!strcmp(zr_config_media_find(&config, "tape")->image, "work/demo.tzx"));
    static zr_config bad;
    static const char BAD_VALUE[] = "machine:\n  model: test\n  options:\n    beep: loud\n";
    CHECK(!zr_config_parse(BAD_VALUE, sizeof BAD_VALUE - 1, "zetro.yaml", &bad, error, sizeof error) && strstr(error, "line 4"));
    static const char BAD_MODEL[] = "machine:\n  model: nothing\n";
    CHECK(!zr_config_parse(BAD_MODEL, sizeof BAD_MODEL - 1, "zetro.yaml", &bad, error, sizeof error) && strstr(error, "line 2"));

    /* The About page's list of changes is the current version's (text_table.h): written anew with every version. */
    CHECK(!strcmp(ZR_CHANGES_VERSION, ZR_VERSION));

    zr_machine *m = zr_machine_create(&config, NULL, error, sizeof error);
    CHECK(m != NULL);
    if (!m) return 1;
    const zr_family *f = zr_machine_family(m);
    uint8_t key, shift;
    CHECK(zr_family_typing(f, 'h', &key, &shift));
    zr_machine_key(m, key, 1);
    zr_machine_key(m, key, 0);
    zr_machine_run(m, zr_machine_frame_period(m) * 3);
    static char before[4096], after[4096];
    zr_machine_text(m, before, sizeof before);
    CHECK(strstr(before, "Keys: H"));

    /* video.border: the test screen's 256 x 192 picture with 5 lines around it, the shape kept per pixel. */
    zr_frame whole, part;
    CHECK(zr_machine_frame(m, &whole));
    part = whole;
    zr_frame_crop(&part, ZR_CONFIG_BORDER_MINIMAL);
    CHECK(part.width == 266 && part.height == 202 && part.pixels == whole.pixels + 19 * whole.pitch + 27 &&
          (uint64_t)part.aspect_w * part.height * whole.width * whole.aspect_h == (uint64_t)part.aspect_h * part.width * whole.height * whole.aspect_w);

    /* The state: saved, the machine changed, loaded, the same screen and time again. */
    zr_state_writer w;
    zr_sw_init(&w);
    zr_machine_save(m, &w);
    CHECK(zr_sw_finish(&w));
    const uint64_t time = zr_machine_time(m);
    zr_machine_key(m, key, 1);
    zr_machine_run(m, zr_machine_frame_period(m));
    zr_state_reader r;
    CHECK(zr_sr_open(&r, w.data, w.size, error, sizeof error));
    CHECK(zr_machine_load(m, &r, error, sizeof error));
    zr_machine_text(m, after, sizeof after);
    CHECK(!strcmp(before, after) && zr_machine_time(m) == time);
    /* A state cut short is refused. */
    CHECK(!zr_sr_open(&r, w.data, w.size - 13, error, sizeof error));
    zr_sw_free(&w);
    zr_machine_destroy(m);
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
