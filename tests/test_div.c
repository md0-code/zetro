/* The DivIDE's and DivMMC's paging and ports against a fake board: the control register (CONMEM, MAPRAM that only a power on
 * clears, the bank), what 0000h-3FFFh shows and where a write lands in each mode, the automatic mapping through the CPU's hooks
 * (the entry points map in after their fetch, 3D00h at once, 1FF8h out), the NMI button, the DivIDE's odd and even data accesses
 * (low byte first, any other register resets the order) on an image, and the DivMMC's SPI port (a read gives the byte kept and
 * exchanges the next). The firmware here is a pattern, not ESXDOS: tests/verify.py runs the real one. No SDL. */
#include "divide.h"
#include "divmmc.h"
#include "state.h"
#include "../src/host/host.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(what) do { if (!(what)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #what); ++failures; } } while (0)

#define CLOCK 14000000u
#define DIR ZR_WORK "/tests/div/"

/* --- a fake board ---------------------------------------------------------------------------------------- */

typedef struct fake {
    zr_hw_host host;
    uint64_t now, when;
    zr_dev *dev;
    unsigned maps, nmis;
    int rom_ok;
} fake;

static uint64_t h_now(void *ctx) { return ((fake *)ctx)->now; }
static void h_schedule(void *ctx, void *dev, uint64_t when) { (void)dev; ((fake *)ctx)->when = when; }
static void h_line(void *ctx, void *dev, int pin, int state) { (void)ctx; (void)dev; (void)pin; (void)state; }
static void b_map(void *ctx) { ++((fake *)ctx)->maps; }
static int b_ok(void *ctx) { return ((fake *)ctx)->rom_ok; }
static void b_nmi(void *ctx) { ++((fake *)ctx)->nmis; }

static void fake_init(fake *f, zr_dev *dev, zr_divmem_board *board) {
    memset(f, 0, sizeof *f);
    f->host.ctx = f;
    f->host.now = h_now;
    f->host.schedule = h_schedule;
    f->host.line = h_line;
    f->when = ZR_NEVER;
    f->dev = dev;
    f->rom_ok = 1;
    board->ctx = f;
    board->map = b_map;
    board->automap_ok = b_ok;
    board->nmi = b_nmi;
}

static void advance(fake *f, unsigned us) {
    const uint64_t t = f->now + (uint64_t)us * CLOCK / 1000000u;
    for (unsigned guard = 0; f->when <= t && guard < 100000; guard++) {
        f->now = f->when;
        f->when = ZR_NEVER;
        f->dev->ops->event(f->dev, f->now);
    }
    f->now = t;
}

/* A fetch as the CPU makes it: the hooks, then the byte the page shows. */
static uint8_t fetch(zr_divmem *d, uint16_t pc, const uint8_t *rom) {
    const zr_cpu_hooks *h = zr_divmem_hooks(d);
    const int watched = zr_cpu_watched(h, pc);
    if (watched) h->before_fetch(h->ctx, pc);
    const uint8_t *bytes;
    const uint8_t op = zr_divmem_page(d, pc >> 10, &bytes) ? bytes[pc & 1023u] : rom[pc];
    if (watched) h->after_fetch(h->ctx, pc, op);
    return op;
}

static uint8_t eeprom[ZR_DIVMEM_EEPROM], rom[0x4000];

static int shows(zr_divmem *d, uint16_t address, uint8_t *value) {
    const uint8_t *bytes;
    if (!zr_divmem_page(d, address >> 10, &bytes)) return 0;
    *value = bytes[address & 1023u];
    return 1;
}

/* --- the paging -------------------------------------------------------------------------------------------- */

static void test_paging(void) {
    fake f;
    zr_divmem_board board;
    zr_divmem d;
    uint8_t v;
    fake_init(&f, NULL, &board);
    zr_divmem_init(&d, &board, eeprom, 4);
    zr_divmem_reset(&d, 1);
    CHECK(!zr_divmem_mapped(&d) && !shows(&d, 0x0000, &v));

    /* The entry points map in after their own fetch (that opcode is the ROM's), 3Dxx at once (the opcode is the interface's),
     * and the off area out after its fetch (the opcode is still the interface's). */
    static const uint16_t entries[] = {0x0000, 0x0008, 0x0038, 0x0066, 0x04C6, 0x0562};
    for (unsigned i = 0; i < 6; i++) {
        zr_divmem_reset(&d, 0);
        CHECK(fetch(&d, entries[i], rom) == rom[entries[i]] && zr_divmem_mapped(&d));
        CHECK(fetch(&d, (uint16_t)(entries[i] + 1), rom) == eeprom[entries[i] + 1]);
    }
    zr_divmem_reset(&d, 0);
    CHECK(fetch(&d, 0x0100, rom) == rom[0x0100] && !zr_divmem_mapped(&d));
    CHECK(fetch(&d, 0x3D13, rom) != 0xFF && zr_divmem_mapped(&d));
    CHECK(shows(&d, 0x3D13, &v) && v == d.ram[0x1D13]);                     /* bank 0 of the RAM at 2000h */
    CHECK(fetch(&d, 0x1FF9, rom) == eeprom[0x1FF9] && !zr_divmem_mapped(&d));
    f.rom_ok = 0;                                                           /* the board's own ROM is not at 0000h */
    fetch(&d, 0x0008, rom);
    CHECK(!zr_divmem_mapped(&d));
    f.rom_ok = 1;

    /* CONMEM: the EEPROM and the bank at once; the bank is writable; the EEPROM is not. */
    zr_divmem_reset(&d, 0);
    zr_divmem_control(&d, 0x80 | 2);
    CHECK(zr_divmem_mapped(&d) && shows(&d, 0x0010, &v) && v == eeprom[0x10]);
    zr_divmem_write(&d, 0x2005, 0x5A);
    zr_divmem_write(&d, 0x0005, 0x5A);
    CHECK(shows(&d, 0x2005, &v) && v == 0x5A && d.ram[2 * 0x2000 + 5] == 0x5A && eeprom[5] != 0x5A);
    zr_divmem_control(&d, 0x80 | 3);                                        /* fill bank 3 for MAPRAM */
    zr_divmem_write(&d, 0x0000, 0x11);                                      /* still the EEPROM: not written */
    for (unsigned i = 0; i < 0x2000; i++) zr_divmem_write(&d, (uint16_t)(0x2000 + i), (uint8_t)(i * 3));
    CHECK(d.ram[3 * 0x2000 + 7] == 21);

    /* MAPRAM: bank 3 stands in for the EEPROM when the automatic mapping has the memory in, read-only; with CONMEM the EEPROM
     * has the priority; the bank at 2000h is writable unless it is bank 3. A reset does not clear it, a power on does. */
    zr_divmem_control(&d, 0x40 | 1);
    CHECK(!zr_divmem_mapped(&d) && d.mapram);
    fetch(&d, 0x0066, rom);
    CHECK(zr_divmem_mapped(&d) && shows(&d, 0x0007, &v) && v == 21);
    zr_divmem_write(&d, 0x0007, 0);
    CHECK(shows(&d, 0x0007, &v) && v == 21);
    zr_divmem_write(&d, 0x2000, 0x77);
    CHECK(shows(&d, 0x2000, &v) && v == 0x77);                              /* bank 1 */
    zr_divmem_control(&d, 3);
    zr_divmem_write(&d, 0x2000, 0x99);
    CHECK(shows(&d, 0x2000, &v) && v == 0 && d.ram[3 * 0x2000] == 0);       /* bank 3 under MAPRAM: protected */
    zr_divmem_control(&d, 0x80 | 3);
    CHECK(shows(&d, 0x0007, &v) && v == eeprom[7]);
    zr_divmem_control(&d, 0);
    zr_divmem_reset(&d, 0);
    CHECK(d.mapram && !zr_divmem_mapped(&d) && d.ram[3 * 0x2000 + 7] == 21);
    zr_divmem_reset(&d, 1);
    CHECK(!d.mapram && d.ram[3 * 0x2000 + 7] == 0);

    /* The NMI button goes unless the memory is mapped. */
    CHECK(zr_divmem_button(&d) && f.nmis == 1);
    fetch(&d, 0x0066, rom);
    CHECK(!zr_divmem_button(&d) && f.nmis == 1);

    /* A state: the paging and the RAM. */
    zr_divmem_control(&d, 0x40 | 2);
    d.ram[5] = 0xA5;
    zr_state_writer w;
    zr_sw_init(&w);
    zr_sw_header(&w, "test", "test", NULL, 0);
    zr_sw_begin(&w, "DIVM", 0);
    zr_divmem_save(&d, &w);
    zr_sw_end(&w);
    CHECK(zr_sw_finish(&w));
    zr_divmem_reset(&d, 1);
    zr_state_reader r;
    char error[64];
    CHECK(zr_sr_open(&r, w.data, w.size, error, sizeof error) && zr_sr_chunk(&r, "DIVM", 0) && zr_divmem_load(&d, &r));
    CHECK(d.mapram && d.bank == 2 && d.automap && d.ram[5] == 0xA5);
    zr_sw_free(&w);
}

/* --- the DivIDE's data register ------------------------------------------------------------------------------ */

static void test_divide(void) {
    char error[160];
    uint8_t sector[512];
    CHECK(zr_card_create(DIR "ide.img", ZR_CARD_RAW, 1u << 20, 1, error, sizeof error));
    zr_card *card = zr_card_open(DIR "ide.img", 0, error, sizeof error);
    CHECK(card != NULL);
    if (!card) return;
    for (unsigned i = 0; i < 512; i++) sector[i] = (uint8_t)(i * 5 + 1);
    CHECK(zr_card_write(card, 3, sector));
    static zr_divide d;
    fake f;
    zr_divmem_board board;
    fake_init(&f, &d.ata.dev, &board);
    zr_divide_init(&d, &f.host, &board, eeprom, CLOCK);
    zr_divide_attach(&d, 0, card);
    zr_divide_reset(&d, 1);
    advance(&f, 200);
    CHECK(zr_divide_in(&d, 0xBF) == 0x50 && zr_divide_in(&d, 0xBB) == 0xA0);          /* ready, the master */
    /* READ SECTORS, LBA 3: the registers at AB, AF, B3, B7, BB and BF; the data a byte at a time at A3, the low byte first. */
    zr_divide_out(&d, 0xAB, 1); zr_divide_out(&d, 0xAF, 3); zr_divide_out(&d, 0xB3, 0); zr_divide_out(&d, 0xB7, 0);
    zr_divide_out(&d, 0xBB, 0xE0); zr_divide_out(&d, 0xBF, 0x20);
    advance(&f, 100);
    CHECK((zr_divide_in(&d, 0xBF) & 0x88) == 0x08);
    uint8_t got[512];
    for (unsigned i = 0; i < 3; i++) got[i] = zr_divide_in(&d, 0xA3);
    CHECK(!memcmp(got, sector, 3));
    zr_divide_in(&d, 0xA7);                                                 /* another register: the next data access is odd again */
    CHECK(zr_divide_in(&d, 0xA3) == sector[4] && zr_divide_in(&d, 0xA3) == sector[5]);     /* the third word, low byte first */
    for (unsigned i = 6; i < 512; i++) got[i] = zr_divide_in(&d, 0xA3);
    CHECK(!memcmp(got + 6, sector + 6, 506) && (zr_divide_in(&d, 0xBF) & 0x08) == 0);
    /* WRITE SECTORS, LBA 4: the word goes at the even access, the low byte latched by the odd one. */
    zr_divide_out(&d, 0xAB, 1); zr_divide_out(&d, 0xAF, 4); zr_divide_out(&d, 0xB3, 0); zr_divide_out(&d, 0xB7, 0);
    zr_divide_out(&d, 0xBB, 0xE0); zr_divide_out(&d, 0xBF, 0x30);
    advance(&f, 100);
    for (unsigned i = 0; i < 512; i++) zr_divide_out(&d, 0xA3, (uint8_t)(255 - i));
    advance(&f, 100);
    uint8_t back[512];
    CHECK(zr_card_read(card, 4, back) && back[0] == 255 && back[1] == 254 && back[511] == (uint8_t)(255u - (511u & 255u)));
    /* the control register through port E3 maps the memory. */
    zr_divide_out(&d, 0xE3, 0x80);
    CHECK(zr_divmem_mapped(&d.mem) && f.maps > 0);
    zr_divide_attach(&d, 0, NULL);
    zr_card_close(card);
}

/* --- the DivMMC's SPI port ------------------------------------------------------------------------------------- */

static void test_divmmc(void) {
    char error[160];
    CHECK(zr_card_create(DIR "mmc.img", ZR_CARD_RAW, 4u << 20, 1, error, sizeof error));
    zr_card *card = zr_card_open(DIR "mmc.img", 0, error, sizeof error);
    CHECK(card != NULL);
    if (!card) return;
    static zr_divmmc d;
    fake f;
    zr_divmem_board board;
    fake_init(&f, &d.mmc.dev, &board);
    zr_divmmc_init(&d, &f.host, &board, eeprom);
    CHECK(zr_divmmc_attach(&d, card, error, sizeof error));
    zr_divmmc_reset(&d, 1);
    CHECK(zr_divmmc_in(&d, 0xEB) == 0xFF);                                  /* the card is not selected */
    zr_divmmc_out(&d, 0xE7, 0xFE);                                          /* selected: bit 0 low */
    static const uint8_t cmd0[] = {0x40, 0, 0, 0, 0, 0x95};
    for (unsigned i = 0; i < 6; i++) zr_divmmc_out(&d, 0xEB, cmd0[i]);
    unsigned r1 = 0xFF;
    for (unsigned i = 0; i < 8 && r1 == 0xFF; i++) r1 = zr_divmmc_in(&d, 0xEB);
    CHECK(r1 == 0x01);                                                      /* idle */
    zr_divmmc_out(&d, 0xE7, 0xFF);
    CHECK(zr_divmmc_in(&d, 0xEB) == 0xFF && zr_divmmc_in(&d, 0xEB) == 0xFF);
    zr_divmmc_out(&d, 0xE3, 0x40 | 5);                                      /* 16 banks: bank 5 */
    CHECK(d.mem.bank == 5 && d.mem.mapram);
    zr_divmmc_reset(&d, 0);
    CHECK(d.mem.bank == 0 && d.mem.mapram);
    zr_divmmc_attach(&d, NULL, NULL, 0);
    zr_card_close(card);
}

int main(void) {
    for (unsigned i = 0; i < sizeof eeprom; i++) eeprom[i] = (uint8_t)(i * 13 + 7);
    for (unsigned i = 0; i < sizeof rom; i++) rom[i] = (uint8_t)(i * 29 + 3);
    zr_host_make_folders(DIR);
    test_paging();
    test_divide();
    test_divmmc();
    if (failures) printf("%d failures\n", failures);
    else printf("test_div: ok\n");
    return failures ? 1 : 0;
}
