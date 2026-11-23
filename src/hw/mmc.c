/* The SD and MMC card in SPI mode (mmc.h). ZetRo's own code, written from the SD Physical Layer Simplified
 * Specification and the MMC specification. */
#include "mmc.h"
#include "state.h"

#include <stdio.h>
#include <string.h>

#define STATE_VERSION 1u
#define INIT_POLLS 2u                       /* ACMD41 or CMD1 calls that find the card still initialising */
#define SDSC_MAX_SECTORS 4194304u           /* 2 GiB */
#define MIN_SECTORS 8u

/* What the card is doing. */
enum { S_IDLE, S_CMD, S_READ, S_WRITE_TOKEN, S_WRITE_DATA };

/* R1 bits. */
enum { R1_IDLE = 0x01, R1_ILLEGAL = 0x04, R1_CRC = 0x08, R1_ADDRESS = 0x20, R1_PARAM = 0x40 };
/* The status byte of R2. */
enum { ST2_WP_VIOLATION = 0x20, ST2_OUT_OF_RANGE = 0x80 };

/* --- CRCs --------------------------------------------------------------------------------------- */

static uint8_t crc7(const uint8_t *p, unsigned n) {
    uint8_t crc = 0;
    for (unsigned i = 0; i < n; i++) {
        uint8_t d = p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (uint8_t)(crc << 1);
            if ((d & 0x80u) ^ (crc & 0x80u)) crc ^= 0x09u;
            d = (uint8_t)(d << 1);
        }
    }
    return crc & 0x7Fu;
}

static uint16_t crc16_table[256];
static int crc16_ready;

static uint16_t crc16(const uint8_t *p, unsigned n) {
    if (!crc16_ready) {
        for (unsigned i = 0; i < 256; i++) {
            unsigned v = i << 8;
            for (unsigned bit = 0; bit < 8; bit++) v = (v & 0x8000u) ? (v << 1) ^ 0x1021u : v << 1;
            crc16_table[i] = (uint16_t)v;
        }
        crc16_ready = 1;
    }
    unsigned crc = 0;
    for (unsigned i = 0; i < n; i++) crc = ((crc << 8) ^ crc16_table[((crc >> 8) ^ p[i]) & 0xFFu]) & 0xFFFFu;
    return (uint16_t)crc;
}

/* --- the registers --------------------------------------------------------------------------------- */

/* Bits hi..lo of a 128-bit register held MSB first (bit 127 is the top bit of byte 0). */
static void put_bits(uint8_t *reg, unsigned hi, unsigned lo, uint32_t value) {
    for (unsigned i = lo; i <= hi; i++) {
        const uint8_t mask = (uint8_t)(1u << (i & 7u));
        uint8_t *byte = &reg[15u - i / 8u];
        if ((value >> (i - lo)) & 1u) *byte |= mask;
        else *byte = (uint8_t)(*byte & ~mask);
    }
}

static void seal(uint8_t *reg) {
    reg[15] = (uint8_t)(crc7(reg, 15) << 1 | 1u);
}

/* How the CSD says the capacity: READ_BL_LEN, C_SIZE_MULT and C_SIZE of a version 1 CSD, or C_SIZE of version 2, and the
 * sectors that reach. */
typedef struct csd_capacity {
    unsigned bl, mult;
    uint32_t csize;
    uint64_t blocks;
} csd_capacity;

static csd_capacity capacity_of(uint64_t sectors, int hc) {
    csd_capacity c;
    memset(&c, 0, sizeof c);
    if (hc) {
        c.blocks = sectors / 1024u * 1024u;
        c.csize = (uint32_t)(c.blocks / 1024u - 1u);
        c.bl = 9;
        return c;
    }
    c.bl = sectors > 2097152u ? 10u : 9u;
    const uint64_t unit = c.bl == 10u ? 2u : 1u, b = sectors / unit;
    unsigned m = 0;
    while (m < 7 && (b >> (m + 2u)) > 4096u) m++;
    const uint64_t n = b >> (m + 2u);
    c.mult = m;
    c.csize = (uint32_t)(n - 1u);
    c.blocks = (n << (m + 2u)) * unit;
    return c;
}

void zr_mmc_csd(const zr_mmc *m, uint8_t out[16]) {
    memset(out, 0, 16);
    if (!m->card) return;
    const csd_capacity c = capacity_of(m->card->sectors, m->hc);
    const unsigned ro = m->card->writable ? 0u : 1u;
    if (m->hc) {
        put_bits(out, 127, 126, 1);
        put_bits(out, 119, 112, 0x0E);          /* TAAC 1 ms */
        put_bits(out, 103, 96, 0x5A);           /* TRAN_SPEED 50 MHz */
        put_bits(out, 95, 84, 0x5B5);           /* the command classes */
        put_bits(out, 83, 80, 9);
        put_bits(out, 69, 48, c.csize);
        put_bits(out, 46, 46, 1);
        put_bits(out, 45, 39, 0x7F);
        put_bits(out, 28, 26, 2);
        put_bits(out, 25, 22, 9);
        put_bits(out, 12, 12, ro);
    } else {
        if (m->type == ZR_MMC_MMC) {
            put_bits(out, 127, 126, 2);         /* CSD structure 1.2 */
            put_bits(out, 125, 122, 4);         /* specification version 4 */
        }
        put_bits(out, 119, 112, 0x0E);
        put_bits(out, 103, 96, 0x32);           /* 25 MHz */
        put_bits(out, 95, 84, 0x5B5);
        put_bits(out, 83, 80, c.bl);
        put_bits(out, 79, 79, 1);               /* partial blocks may be read */
        put_bits(out, 73, 62, c.csize);
        put_bits(out, 49, 47, c.mult);
        put_bits(out, 46, 46, 1);
        put_bits(out, 45, 39, 0x7F);
        put_bits(out, 28, 26, 2);
        put_bits(out, 25, 22, c.bl);
        put_bits(out, 12, 12, ro);
    }
    seal(out);
}

void zr_mmc_cid(const zr_mmc *m, uint8_t out[16]) {
    memset(out, 0, 16);
    if (!m->card) return;
    put_bits(out, 127, 120, 0x7A);              /* manufacturer */
    put_bits(out, 119, 112, 'Z');               /* application "ZR" */
    put_bits(out, 111, 104, 'R');
    static const char name[5] = {'Z', 'E', 'T', 'R', 'O'};
    for (unsigned i = 0; i < 5; i++) put_bits(out, 103 - 8 * i, 96 - 8 * i, (uint8_t)name[i]);
    put_bits(out, 63, 56, 0x10);                /* revision 1.0 */
    const uint64_t s = m->card->sectors;
    put_bits(out, 55, 24, (uint32_t)(s * 2654435761u ^ (s >> 32) ^ (m->type == ZR_MMC_MMC ? 0x4D4Du : 0u)));
    put_bits(out, 19, 12, 26);                  /* manufactured 2026-11 */
    put_bits(out, 11, 8, 11);
    seal(out);
}

/* --- the answers ---------------------------------------------------------------------------------- */

static void set_light(zr_mmc *m, int on) {
    if ((m->light != 0) == (on != 0)) return;
    m->light = on ? 1u : 0u;
    zr_dev_line(&m->dev, ZR_MMC_PIN_LIGHT, on);
}

static void tx_reset(zr_mmc *m) { m->tx_len = m->tx_pos = 0; }
static void tx_put(zr_mmc *m, uint8_t b) { if (m->tx_len < ZR_MMC_TX) m->tx[m->tx_len++] = b; }
/* R1 after the one byte the card needs to answer. */
static void tx_r1(zr_mmc *m, unsigned r1) {
    tx_reset(m);
    tx_put(m, 0xFF);
    tx_put(m, (uint8_t)r1);
}
/* A data block: a gap byte, the token FEh, the bytes and their CRC16. */
static void tx_block(zr_mmc *m, const uint8_t *data, unsigned n) {
    tx_put(m, 0xFF);
    tx_put(m, 0xFE);
    for (unsigned i = 0; i < n; i++) tx_put(m, data[i]);
    const uint16_t crc = crc16(data, n);
    tx_put(m, (uint8_t)(crc >> 8));
    tx_put(m, (uint8_t)crc);
}
static void tx_r7(zr_mmc *m, unsigned r1, uint32_t value) {
    tx_r1(m, r1);
    tx_put(m, (uint8_t)(value >> 24)); tx_put(m, (uint8_t)(value >> 16)); tx_put(m, (uint8_t)(value >> 8)); tx_put(m, (uint8_t)value);
}

static void protocol_reset(zr_mmc *m) {
    m->idle = 1; m->crc_on = 0; m->v2 = 0; m->app = 0; m->state = S_IDLE; m->cmd_got = 0; m->multi = 0;
    m->status2 = 0; m->polls = 0; m->wr_blocks = 0; m->erase_count = 0; m->addr = 0; m->rx_got = 0;
    tx_reset(m);
    set_light(m, 0);
}

/* The address of a read or write command as a block of the card: 0, or the R1 error bit. */
static unsigned block_of(const zr_mmc *m, uint32_t arg, uint64_t *block) {
    uint64_t b = arg;
    if (!m->hc) {
        if (arg % ZR_CARD_SECTOR) return R1_ADDRESS;
        b = arg / ZR_CARD_SECTOR;
    }
    if (b >= m->blocks) return R1_PARAM;
    *block = b;
    return 0;
}

/* The next block of a multiple read goes on the wire. */
static int read_block(zr_mmc *m, uint64_t block) {
    uint8_t data[ZR_CARD_SECTOR];
    if (!zr_card_read(m->card, block, data)) {
        tx_put(m, 0xFF);
        tx_put(m, 0x01);                        /* data error token: error */
        return 0;
    }
    tx_block(m, data, sizeof data);
    m->addr = block + 1u;
    m->activity++;
    return 1;
}

static void next_block(zr_mmc *m) {
    tx_reset(m);
    if (m->addr >= m->blocks) {
        tx_put(m, 0xFF);
        tx_put(m, 0x08);                        /* data error token: out of range */
        m->status2 |= ST2_OUT_OF_RANGE;
        m->state = S_IDLE;
        return;
    }
    if (!read_block(m, m->addr)) m->state = S_IDLE;
}

static void start_read(zr_mmc *m, uint32_t arg, int multi, unsigned r1) {
    uint64_t block = 0;
    const unsigned err = block_of(m, arg, &block);
    if (err) {
        tx_r1(m, r1 | err);
        if (err == R1_PARAM) m->status2 |= ST2_OUT_OF_RANGE;
        return;
    }
    tx_r1(m, r1);
    m->state = read_block(m, block) && multi ? S_READ : S_IDLE;
    set_light(m, 1);
}

static void start_write(zr_mmc *m, uint32_t arg, int multi, unsigned r1) {
    uint64_t block = 0;
    const unsigned err = block_of(m, arg, &block);
    if (err) {
        tx_r1(m, r1 | err);
        if (err == R1_PARAM) m->status2 |= ST2_OUT_OF_RANGE;
        return;
    }
    tx_r1(m, r1);
    m->addr = block;
    m->multi = multi ? 1u : 0u;
    m->wr_blocks = 0;
    m->state = S_WRITE_TOKEN;
}

/* The data of a written block has arrived: write it and answer with the data response token and the busy bytes. */
static void finish_block(zr_mmc *m) {
    unsigned token;
    const uint16_t crc = (uint16_t)(m->rx[ZR_CARD_SECTOR] << 8 | m->rx[ZR_CARD_SECTOR + 1u]);
    if (m->crc_on && crc16(m->rx, ZR_CARD_SECTOR) != crc) {
        token = 0x0B;
    } else if (!m->card->writable) {
        token = 0x0D;
        m->status2 |= ST2_WP_VIOLATION;
    } else if (m->addr >= m->blocks) {
        token = 0x0D;
        m->status2 |= ST2_OUT_OF_RANGE;
    } else if (!zr_card_write(m->card, m->addr, m->rx)) {
        token = 0x0D;
    } else {
        token = 0x05;
        m->addr++;
        m->wr_blocks++;
        m->activity++;
    }
    tx_reset(m);
    tx_put(m, (uint8_t)token);
    for (unsigned i = 0; i < ZR_MMC_BUSY_BYTES; i++) tx_put(m, 0x00);
    m->rx_got = 0;
    m->state = m->multi ? S_WRITE_TOKEN : S_IDLE;
}

/* --- commands ------------------------------------------------------------------------------------- */

static uint32_t ocr_of(const zr_mmc *m) {
    uint32_t ocr = 0x00FF8000u;
    if (!m->idle) ocr |= 0x80000000u | (m->hc ? 0x40000000u : 0u);
    return ocr;
}

/* One initialisation poll: the card leaves idle at the third. */
static void init_poll(zr_mmc *m, int host_hc) {
    m->polls++;
    if (m->idle && m->polls > INIT_POLLS && (!m->hc || host_hc)) m->idle = 0;
}

static void app_command(zr_mmc *m, unsigned cmd, uint32_t arg, unsigned r1) {
    uint8_t block[64];
    switch (cmd) {
    case 13:                                    /* SD_STATUS */
        if (m->idle) break;
        tx_r1(m, r1);
        tx_put(m, m->status2);
        m->status2 = 0;
        memset(block, 0, sizeof block);
        tx_block(m, block, sizeof block);
        return;
    case 22:                                    /* SEND_NUM_WR_BLOCKS */
        if (m->idle) break;
        tx_r1(m, r1);
        block[0] = (uint8_t)(m->wr_blocks >> 24); block[1] = (uint8_t)(m->wr_blocks >> 16);
        block[2] = (uint8_t)(m->wr_blocks >> 8); block[3] = (uint8_t)m->wr_blocks;
        tx_block(m, block, 4);
        return;
    case 23:                                    /* SET_WR_BLK_ERASE_COUNT */
        if (m->idle) break;
        m->erase_count = arg & 0x7FFFFFu;
        tx_r1(m, r1);
        return;
    case 41:                                    /* SD_SEND_OP_COND */
        init_poll(m, m->v2 && ((arg >> 30) & 1u));
        tx_r1(m, m->idle ? R1_IDLE : 0u);
        return;
    case 51:                                    /* SEND_SCR */
        if (m->idle) break;
        tx_r1(m, r1);
        memset(block, 0, 8);
        block[0] = 0x02;                        /* SCR structure 0, SD specification 2.00 */
        block[1] = 0x05;                        /* no security, 1-bit and 4-bit buses */
        tx_block(m, block, 8);
        return;
    default:
        break;
    }
    tx_r1(m, r1 | R1_ILLEGAL);
}

static void execute(zr_mmc *m) {
    const unsigned cmd = m->cmd[0] & 0x3Fu;
    const uint32_t arg = (uint32_t)m->cmd[1] << 24 | (uint32_t)m->cmd[2] << 16 | (uint32_t)m->cmd[3] << 8 | m->cmd[4];
    const int app = m->app;
    m->app = 0;
    const unsigned idle = m->idle ? R1_IDLE : 0u;
    const int always = !app && (cmd == 0 || cmd == 8);
    const uint8_t expected = (uint8_t)(crc7(m->cmd, 5) << 1);
    if ((m->crc_on || always) && (m->cmd[5] & 0xFEu) != expected) {
        tx_r1(m, idle | R1_CRC);
        return;
    }
    if (app) {
        app_command(m, cmd, arg, idle);
        return;
    }
    const int sd = m->type == ZR_MMC_SD;
    switch (cmd) {
    case 0:
        protocol_reset(m);
        tx_r1(m, R1_IDLE);
        return;
    case 1:                                     /* SEND_OP_COND: MMC cards only */
        if (sd) break;
        init_poll(m, 0);
        tx_r1(m, m->idle ? R1_IDLE : 0u);
        return;
    case 8:
        if (!sd || ((arg >> 8) & 0xFu) != 1u) break;
        m->v2 = 1;
        tx_r7(m, idle, (uint32_t)0x100u | (arg & 0xFFu));
        return;
    case 9: case 10: {
        uint8_t reg[16];
        if (cmd == 9) zr_mmc_csd(m, reg);
        else zr_mmc_cid(m, reg);
        tx_r1(m, idle);
        tx_block(m, reg, sizeof reg);
        return;
    }
    case 12:
        tx_reset(m);
        tx_put(m, 0xFF);                        /* the stuff byte */
        tx_put(m, (uint8_t)idle);
        tx_put(m, 0x00);
        tx_put(m, 0x00);
        m->state = S_IDLE;
        m->multi = 0;
        return;
    case 13:
        tx_r1(m, idle);
        tx_put(m, m->status2);
        m->status2 = 0;
        return;
    case 16:
        tx_r1(m, arg == ZR_CARD_SECTOR ? idle : (idle | R1_PARAM));
        return;
    case 17: case 18:
        if (m->idle) break;
        start_read(m, arg, cmd == 18, idle);
        return;
    case 24: case 25:
        if (m->idle) break;
        start_write(m, arg, cmd == 25, idle);
        return;
    case 55:
        if (!sd) break;
        m->app = 1;
        tx_r1(m, idle);
        return;
    case 58:
        tx_r7(m, idle, ocr_of(m));
        return;
    case 59:
        m->crc_on = (uint8_t)(arg & 1u);
        tx_r1(m, idle);
        return;
    default:
        break;
    }
    tx_r1(m, idle | R1_ILLEGAL);
}

/* --- the bus ------------------------------------------------------------------------------------------ */

static uint8_t pop(zr_mmc *m) { return m->tx[m->tx_pos++]; }

static uint8_t exchange(zr_mmc *m, uint8_t out) {
    if (m->state == S_CMD) {
        m->cmd[m->cmd_got++] = out;
        if (m->cmd_got == 6) {
            m->cmd_got = 0;
            m->state = S_IDLE;
            execute(m);
        }
        return 0xFF;
    }
    if (m->state == S_WRITE_DATA) {
        m->rx[m->rx_got++] = out;
        if (m->rx_got == sizeof m->rx) finish_block(m);
        return 0xFF;
    }
    if ((out & 0xC0u) == 0x40u) {               /* a command starts: whatever the card was sending is dropped */
        tx_reset(m);
        m->multi = 0;
        m->cmd[0] = out;
        m->cmd_got = 1;
        m->state = S_CMD;
        return 0xFF;
    }
    if (m->state == S_WRITE_TOKEN) {
        if (m->tx_pos < m->tx_len) return pop(m);     /* the answer and the busy bytes first */
        if (out == (m->multi ? 0xFCu : 0xFEu)) {
            m->rx_got = 0;
            m->state = S_WRITE_DATA;
            set_light(m, 1);
        } else if (m->multi && out == 0xFDu) {
            tx_reset(m);
            tx_put(m, 0x00);
            tx_put(m, 0x00);
            m->state = S_IDLE;
            m->multi = 0;
        }
        return 0xFF;
    }
    if (m->tx_pos >= m->tx_len && m->state == S_READ) next_block(m);
    return m->tx_pos < m->tx_len ? pop(m) : 0xFF;
}

uint8_t zr_mmc_exchange(zr_mmc *m, uint8_t out) {
    uint8_t in = 0xFF;
    if (m->card && m->selected) {
        in = exchange(m, out);
        if (m->light && m->state != S_READ && m->state != S_WRITE_DATA && m->tx_pos >= m->tx_len) set_light(m, 0);
    }
    m->last = in;
    return in;
}

void zr_mmc_select(zr_mmc *m, int selected) {
    const int on = selected != 0;
    if (!on && m->selected) {
        m->cmd_got = 0;
        m->state = S_IDLE;
        m->multi = 0;
        m->rx_got = 0;
        tx_reset(m);
        set_light(m, 0);
    }
    m->selected = on ? 1u : 0u;
}

/* --- the chip ------------------------------------------------------------------------------------------ */

static uint8_t chip_read(void *dev, unsigned space, uint32_t offset) {
    (void)space;
    return offset == 0 ? ((zr_mmc *)dev)->last : 0xFF;
}
static void chip_write(void *dev, unsigned space, uint32_t offset, uint8_t value) {
    (void)space;
    if (offset == 0) zr_mmc_exchange(dev, value);
}
static void chip_event(void *dev, uint64_t now) { (void)dev; (void)now; }
static void chip_reset(void *dev) {
    zr_mmc *m = dev;
    m->selected = 0;
    m->last = 0xFF;
    protocol_reset(m);
}
static void chip_input(void *dev, int pin, int state) {
    if (pin == ZR_MMC_PIN_CS) zr_mmc_select(dev, state);
}

static void chip_save(const void *dev, zr_state_writer *w) {
    const zr_mmc *m = dev;
    zr_sw_u8(w, STATE_VERSION);
    const uint8_t flags[] = {m->type, m->hc, m->selected, m->idle, m->crc_on, m->v2, m->app, m->state, m->cmd_got, m->multi, m->light, m->last,
                             m->status2};
    zr_sw_bytes(w, flags, sizeof flags);
    zr_sw_u64(w, m->blocks);
    zr_sw_bytes(w, m->cmd, sizeof m->cmd);
    zr_sw_u32(w, m->polls); zr_sw_u32(w, m->wr_blocks); zr_sw_u32(w, m->erase_count);
    zr_sw_u64(w, m->addr);
    zr_sw_u32(w, m->tx_len); zr_sw_u32(w, m->tx_pos); zr_sw_u32(w, m->rx_got);
    zr_sw_bytes(w, m->tx, m->tx_len);
    zr_sw_bytes(w, m->rx, m->rx_got);
}

static int chip_load(void *dev, zr_state_reader *r) {
    zr_mmc *m = dev;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    uint8_t flags[13];
    zr_sr_bytes(r, flags, sizeof flags);
    zr_mmc in;
    memset(&in, 0, sizeof in);
    in.blocks = zr_sr_u64(r);
    zr_sr_bytes(r, in.cmd, sizeof in.cmd);
    in.polls = zr_sr_u32(r); in.wr_blocks = zr_sr_u32(r); in.erase_count = zr_sr_u32(r);
    in.addr = zr_sr_u64(r);
    in.tx_len = zr_sr_u32(r); in.tx_pos = zr_sr_u32(r); in.rx_got = zr_sr_u32(r);
    if (!zr_sr_ok(r) || !m->card || flags[0] != m->type || flags[1] != m->hc || in.blocks != m->blocks) return 0;
    if (flags[7] > S_WRITE_DATA || flags[8] > 5 || in.tx_len > ZR_MMC_TX || in.tx_pos > in.tx_len || in.rx_got > sizeof in.rx ||
        in.addr > m->blocks || (flags[7] == S_CMD && flags[8] < 1) || (flags[7] != S_CMD && flags[8] != 0) ||
        (flags[7] != S_WRITE_DATA && in.rx_got != 0))
        return 0;
    zr_sr_bytes(r, in.tx, in.tx_len);
    zr_sr_bytes(r, in.rx, in.rx_got);
    if (!zr_sr_ok(r)) return 0;
    m->selected = flags[2] ? 1u : 0u; m->idle = flags[3] ? 1u : 0u; m->crc_on = flags[4] ? 1u : 0u; m->v2 = flags[5] ? 1u : 0u;
    m->app = flags[6] ? 1u : 0u; m->state = flags[7]; m->cmd_got = flags[8]; m->multi = flags[9] ? 1u : 0u; m->light = flags[10] ? 1u : 0u;
    m->last = flags[11]; m->status2 = flags[12];
    memcpy(m->cmd, in.cmd, sizeof m->cmd);
    m->polls = in.polls; m->wr_blocks = in.wr_blocks; m->erase_count = in.erase_count; m->addr = in.addr;
    m->tx_len = in.tx_len; m->tx_pos = in.tx_pos; m->rx_got = in.rx_got;
    memcpy(m->tx, in.tx, in.tx_len);
    memcpy(m->rx, in.rx, in.rx_got);
    return 1;
}

static const zr_dev_ops MMC_OPS = {
    .read = chip_read, .write = chip_write, .event = chip_event, .reset = chip_reset, .input = chip_input,
    .save = chip_save, .load = chip_load,
};

void zr_mmc_init(zr_mmc *m, const zr_hw_host *host, const char *name) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &MMC_OPS;
    m->dev.host = host;
    m->dev.name = name ? name : "mmc";
    m->idle = 1;
    m->last = 0xFF;
}

int zr_mmc_attach(zr_mmc *m, zr_card *card, int type, char *error, size_t capacity) {
    const char *why = NULL;
    if (!card) why = "No image";
    else if (card->sectors < MIN_SECTORS) why = "The image is too small for a card";
    else if (type == ZR_MMC_MMC && card->sectors > SDSC_MAX_SECTORS) why = "An MMC card cannot hold more than 2 GiB";
    if (why) {
        if (error && capacity) snprintf(error, capacity, "%s", why);
        return 0;
    }
    m->card = card;
    m->type = (uint8_t)type;
    m->hc = (uint8_t)(type == ZR_MMC_SD && card->sectors > SDSC_MAX_SECTORS);
    m->blocks = capacity_of(card->sectors, m->hc).blocks;
    m->last = 0xFF;
    protocol_reset(m);
    return 1;
}

void zr_mmc_detach(zr_mmc *m) {
    m->card = NULL;
    m->blocks = 0;
    protocol_reset(m);
}

int zr_mmc_light(const zr_mmc *m) { return m->light; }
uint32_t zr_mmc_activity(const zr_mmc *m) { return m->activity; }
