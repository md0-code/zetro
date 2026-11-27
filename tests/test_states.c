/* The state files and the rewind ring (app/states.h): a state with its media, thumbnail, time and version written and
 * read back, a slot file on disk, the ring's states put back and cut. On the test machine; when the JET's ROM is in
 * the repository's roms/ (or the main checkout's), the ring's size and cost per state on the JET are printed. No SDL. */
#include "config.h"
#include "machine.h"
#include "../src/app/states.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

static zr_machine *build(const char *yaml, const zr_rom_index *roms) {
    static zr_config config;
    char error[256];
    if (!zr_config_parse(yaml, strlen(yaml), ZR_WORK "/tests/zetro.yaml", &config, error, sizeof error)) return NULL;
    return zr_machine_create(&config, roms, error, sizeof error);
}

/* The ring on the JET: what a state takes and costs, how many seconds the budget holds. */
static void measure_jet(void) {
    static zr_rom_index roms;
    const char *folders[2] = {ZR_WORK "/../roms", ZR_WORK "/../../../../roms"};
    zr_rom_index_init(&roms);
    zr_rom_index_scan(&roms, folders, 2);
    zr_machine *m = build("machine:\n  model: jet\n", &roms);
    if (!m || strcmp(zr_machine_family(m)->models[zr_machine_model(m)].id, "jet")) {
        printf("no JET ROM: the ring is not measured on the JET\n");
        if (m) zr_machine_destroy(m);
        zr_rom_index_free(&roms);
        return;
    }
    zr_rewind ring;
    zr_rewind_init(&ring, ZR_REWIND_BUDGET);
    const uint64_t period = zr_machine_frame_period(m), interval = (uint64_t)zr_machine_clock(m) * ZR_REWIND_INTERVAL_MS / 1000u;
    zr_machine_run(m, period * 150);
    clock_t spent = 0;
    const int takes = 400;
    for (int i = 0; i < takes; ++i) {
        zr_machine_run(m, period);
        ring.next = 0;
        const clock_t t0 = clock();
        CHECK(zr_rewind_take(&ring, m, interval) == 1);
        spent += clock() - t0;
    }
    for (int pass = 0; pass < 2; ++pass) {
        const double per = (double)ring.bytes / (double)ring.count;
        printf("JET, %s: %.0f bytes a state, %.3f ms a state, %.0f s kept in %u MiB\n", pass ? "memory of noise" : "after its start", per,
               1000.0 * (double)spent / CLOCKS_PER_SEC / takes, (double)ZR_REWIND_BUDGET / per * ZR_REWIND_INTERVAL_MS / 1000.0,
               ZR_REWIND_BUDGET >> 20);
        if (pass) break;
        /* The worst case for the packing: every byte of the RAM, the screen included, unlike the next. */
        uint32_t seed = 12345u;
        for (uint32_t a = 0x4000u; a < 0x10000u; ++a) {
            seed = seed * 1103515245u + 12345u;
            zr_machine_poke(m, a, (uint8_t)(seed >> 16));
        }
        zr_rewind_clear(&ring);
        spent = 0;
        for (int i = 0; i < takes; ++i) {
            zr_machine_run(m, period / 8);
            ring.next = 0;
            const clock_t t0 = clock();
            CHECK(zr_rewind_take(&ring, m, interval) == 1);
            spent += clock() - t0;
        }
    }
    zr_rewind_free(&ring);
    zr_machine_destroy(m);
    zr_rom_index_free(&roms);
}

int main(void) {
    char error[256];
    zr_machine *m = build("machine:\n  model: test\n", NULL);
    CHECK(m != NULL);
    if (!m) return 1;
    /* A failed write must leave the sole copy of the edited image in its drive. */
    zr_media *owned = zr_media_from("", (const uint8_t *)"X", 1);
    CHECK(owned && zr_machine_insert(m, 0, owned, error, sizeof error));
    if (!owned) return 1;
    owned->dirty = owned->writable = 1;
    CHECK(!zr_states_replace_media(m, 0, &owned, NULL, 0, error, sizeof error));
    CHECK(owned && zr_machine_media(m, 0) == owned && owned->dirty && owned->data[0] == 'X');
    CHECK(!zr_states_protect_media(m, 0, 1, error, sizeof error) && owned->writable);
    owned->dirty = 0;
    CHECK(zr_states_replace_media(m, 0, &owned, NULL, 0, error, sizeof error));
    const uint64_t period = zr_machine_frame_period(m);
    zr_machine_run(m, period * 3);

    /* A state with every chunk of the shell's, read back. */
    static zr_states_info info, back;
    zr_frame f;
    CHECK(zr_machine_frame(m, &f) && zr_states_thumb_make(&f, &info.thumb));
    CHECK(info.thumb.width == f.width / 4 && info.thumb.height == f.height / 4);
    info.has_thumb = 1;
    info.has_time = 1;
    info.time = (zr_states_time){2026, 11, 13, 14, 5, 9};
    snprintf(info.version, sizeof info.version, "9.8.7");
    info.media[1].present = 1;
    info.media[1].hashed = 1;
    info.media[1].write_protect = 1;
    snprintf(info.media[1].path, sizeof info.media[1].path, "tapes/demo.tap");
    memset(info.media[1].sha256, 0xA5, 32);
    zr_state_writer w;
    zr_sw_init(&w);
    CHECK(zr_states_write(m, &info, &w));
    zr_state_reader r;
    CHECK(zr_states_read(w.data, w.size, &back, &r, error, sizeof error));
    CHECK(zr_machine_load(m, &r, error, sizeof error));
    CHECK(!strcmp(back.version, "9.8.7") && back.has_time && back.time.minute == 5 && back.time.second == 9);
    CHECK(back.has_thumb && back.thumb.width == info.thumb.width && !memcmp(back.thumb.pixels, info.thumb.pixels, info.thumb.width * info.thumb.height));
    CHECK(back.media[1].present && back.media[1].hashed && back.media[1].write_protect && !strcmp(back.media[1].path, "tapes/demo.tap"));
    CHECK(back.media[1].sha256[31] == 0xA5 && !back.media[0].present);
    char when[32];
    zr_states_time_text(&back.time, when, sizeof when);
    CHECK(!strcmp(when, "2026-11-13 14:05"));

    /* A slot file beside the configuration. */
    char path[1024];
    zr_states_slot_path(ZR_WORK "/tests/zetro.yaml", "test", 3, path, sizeof path);
    CHECK(strstr(path, "states") && strstr(path, "slot-3.zrs"));
    CHECK(zr_states_save_file(path, w.data, w.size, error, sizeof error));
    static zr_states_info slot;
    CHECK(zr_states_slot_info(path, &slot) && slot.has_thumb && !strcmp(slot.model, "test"));
    zr_sw_free(&w);

    /* The ring: a state every 2 frames; back to the third, the screen and time of then; the states after it go. */
    zr_rewind ring;
    zr_rewind_init(&ring, ZR_REWIND_BUDGET);
    static char screens[6][4096];
    uint64_t times[6];
    uint8_t key, shift;
    zr_family_typing(zr_machine_family(m), 'a', &key, &shift);
    for (int i = 0; i < 6; ++i) {
        CHECK(zr_rewind_take(&ring, m, period * 2) == 1);
        times[i] = zr_machine_time(m);
        zr_machine_text(m, screens[i], sizeof screens[i]);
        zr_machine_key(m, key, i & 1);
        zr_machine_run(m, period * 2);
    }
    CHECK(ring.count == 6);
    size_t at = 99;
    CHECK(zr_rewind_find(&ring, times[2] + 1, &at) && at == 2);
    CHECK(zr_rewind_restore(&ring, m, at, error, sizeof error));
    static char now[4096];
    zr_machine_text(m, now, sizeof now);
    CHECK(!strcmp(now, screens[2]) && zr_machine_time(m) == times[2]);
    zr_rewind_cut(&ring, at, period * 2);
    CHECK(ring.count == 3 && zr_rewind_take(&ring, m, period * 2) == 0);
    /* A small budget keeps the newest states only. */
    zr_rewind small;
    zr_rewind_init(&small, ring.bytes);
    for (int i = 0; i < 8; ++i) {
        zr_machine_run(m, period);
        CHECK(zr_rewind_take(&small, m, 1) == 1);
    }
    CHECK(small.count >= 1 && small.count < 8 && small.bytes <= ring.bytes && zr_rewind_time(&small, small.count - 1) == zr_machine_time(m));
    zr_rewind_free(&small);
    zr_rewind_free(&ring);
    zr_machine_destroy(m);

    measure_jet();
    printf(failures ? "%d failure(s)\n" : "ok\n", failures);
    return failures != 0;
}
