/* The ATA (IDE) channel (ata.h). ZetRo's own code, written from the ATA-1 standard. */
#include "ata.h"
#include "state.h"

#include <string.h>

#define STATE_VERSION 1u

/* What a busy drive does when its deadline comes. */
enum { ST_COMMAND, ST_NEXT_IN, ST_WRITE };

#define LBA28_LIMIT ((uint64_t)1 << 28)

static zr_ata_drive *selected_drive(zr_ata *a) { return &a->drive[a->selected & 1u]; }
static int present(const zr_ata_drive *d) { return d->card != NULL; }
static int busy(const zr_ata_drive *d) { return d->phase == ZR_ATA_BUSY || d->phase == ZR_ATA_RESETTING; }

/* --- lines and the deadline ------------------------------------------------------------------- */

static void update_lines(zr_ata *a) {
    const zr_ata_drive *s = &a->drive[a->selected & 1u];
    const int intrq = !(a->devctl & ZR_ATA_NIEN) && present(s) && s->intr;
    int light = 0;
    for (unsigned u = 0; u < 2; u++)
        if (present(&a->drive[u]) && (a->drive[u].phase == ZR_ATA_BUSY || a->drive[u].phase == ZR_ATA_DATA_IN || a->drive[u].phase == ZR_ATA_DATA_OUT))
            light = 1;
    if (intrq != a->line_intrq) {
        a->line_intrq = (uint8_t)intrq;
        zr_dev_line(&a->dev, ZR_ATA_PIN_INTRQ, intrq);
    }
    if (light != a->line_light) {
        a->line_light = (uint8_t)light;
        zr_dev_line(&a->dev, ZR_ATA_PIN_LIGHT, light);
    }
}

static void rearm(zr_ata *a) {
    uint64_t m = ZR_NEVER;
    for (unsigned u = 0; u < 2; u++)
        if (present(&a->drive[u]) && a->drive[u].due < m) m = a->drive[u].due;
    if (m != a->armed) {
        a->armed = m;
        zr_dev_schedule(&a->dev, m);
    }
}

static void go_busy(zr_ata *a, zr_ata_drive *d, unsigned stage, uint64_t delay) {
    d->phase = ZR_ATA_BUSY;
    d->stage = (uint8_t)stage;
    d->status = ZR_ATA_BSY;
    d->due = zr_dev_now(&a->dev) + delay;
}

/* --- addresses ------------------------------------------------------------------------------------ */

/* The registers hold an address of the sectors this drive has: the first sector of a command. 0 when it is outside. */
static int resolve(zr_ata_drive *d, uint64_t *lba, int check_sector) {
    const uint64_t sectors = d->card->sectors;
    if (d->head & ZR_ATA_HEAD_LBA) {
        const uint64_t v = (uint64_t)(d->head & 15u) << 24 | (uint64_t)d->cyl_hi << 16 | (uint64_t)d->cyl_lo << 8 | d->sector;
        if (v >= sectors || v >= LBA28_LIMIT) return 0;
        *lba = v;
        return 1;
    }
    const unsigned c = (unsigned)d->cyl_hi << 8 | d->cyl_lo, h = d->head & 15u, s = d->sector;
    if (c >= d->cur_cyl || h >= d->cur_heads || (check_sector && (s == 0 || s > d->cur_spt))) return 0;
    const uint64_t v = ((uint64_t)c * d->cur_heads + h) * d->cur_spt + (s ? s - 1 : 0);
    if (v >= sectors) return 0;
    *lba = v;
    return 1;
}

/* The first address beyond what the command may reach in the addressing mode the registers chose. */
static uint64_t range_limit(const zr_ata_drive *d) {
    uint64_t limit = d->card->sectors;
    if (d->head & ZR_ATA_HEAD_LBA) {
        if (limit > LBA28_LIMIT) limit = LBA28_LIMIT;
    } else {
        const uint64_t chs = (uint64_t)d->cur_cyl * d->cur_heads * d->cur_spt;
        if (limit > chs) limit = chs;
    }
    return limit;
}

static void regs_from_lba(zr_ata_drive *d, uint64_t lba) {
    if (d->head & ZR_ATA_HEAD_LBA) {
        d->sector = (uint8_t)lba;
        d->cyl_lo = (uint8_t)(lba >> 8);
        d->cyl_hi = (uint8_t)(lba >> 16);
        d->head = (uint8_t)((d->head & 0xF0u) | ((lba >> 24) & 15u));
    } else {
        const uint64_t track = (uint64_t)d->cur_heads * d->cur_spt;
        const uint64_t c = lba / track, r = lba % track;
        d->cyl_lo = (uint8_t)c;
        d->cyl_hi = (uint8_t)(c >> 8);
        d->head = (uint8_t)((d->head & 0xF0u) | (unsigned)(r / d->cur_spt));
        d->sector = (uint8_t)(r % d->cur_spt + 1u);
    }
}

/* --- finishing -------------------------------------------------------------------------------------- */

static void finish_ok(zr_ata_drive *d) {
    d->phase = ZR_ATA_IDLE;
    d->status = ZR_ATA_DRDY | ZR_ATA_DSC;
    d->intr = 1;
}

static void finish_error(zr_ata_drive *d, unsigned error, unsigned extra_status) {
    d->phase = ZR_ATA_IDLE;
    d->error = (uint8_t)error;
    d->status = (uint8_t)(ZR_ATA_DRDY | ZR_ATA_DSC | ZR_ATA_ERR | extra_status);
    d->intr = 1;
}

static void signature(zr_ata_drive *d, unsigned unit) {
    d->error = 0x01;
    d->count = 1;
    d->sector = 1;
    d->cyl_lo = d->cyl_hi = 0;
    d->head = (uint8_t)(0xA0u | unit << 4);
}

static void default_geometry(zr_ata_drive *d) {
    d->cur_cyl = d->card->cylinders;
    d->cur_heads = d->card->heads;
    d->cur_spt = d->card->spt;
}

static void power_on(zr_ata_drive *d, unsigned unit) {
    d->phase = ZR_ATA_IDLE;
    d->stage = 0;
    d->cmd = 0;
    d->intr = 0;
    d->eight_bit = 0;
    d->due = ZR_NEVER;
    d->pos = d->left = 0;
    d->features = 0;
    if (d->card) default_geometry(d);
    signature(d, unit);
    d->status = ZR_ATA_DRDY | ZR_ATA_DSC;
}

/* --- reading and writing sectors ------------------------------------------------------------- */

static void read_sector(zr_ata_drive *d) {
    if (d->lba >= d->limit) {
        regs_from_lba(d, d->lba);
        d->count = (uint8_t)d->left;
        finish_error(d, ZR_ATA_E_IDNF, 0);
        return;
    }
    regs_from_lba(d, d->lba);
    if (!zr_card_read(d->card, d->lba, d->buf)) {
        d->count = (uint8_t)d->left;
        finish_error(d, ZR_ATA_E_UNC, 0);
        return;
    }
    d->accesses++;
    d->pos = 0;
    d->phase = ZR_ATA_DATA_IN;
    d->status = ZR_ATA_DRDY | ZR_ATA_DSC | ZR_ATA_DRQ;
    d->intr = 1;
}

static void write_prepare(zr_ata_drive *d, int first) {
    if (d->lba >= d->limit) {
        regs_from_lba(d, d->lba);
        d->count = (uint8_t)d->left;
        finish_error(d, ZR_ATA_E_IDNF, 0);
        return;
    }
    regs_from_lba(d, d->lba);
    d->pos = 0;
    d->phase = ZR_ATA_DATA_OUT;
    d->status = ZR_ATA_DRDY | ZR_ATA_DSC | ZR_ATA_DRQ;
    if (!first) d->intr = 1;
}

/* The host has taken the whole sector buffer. */
static void data_in_done(zr_ata *a, zr_ata_drive *d) {
    a->flip = 0;
    if (d->cmd == ZR_ATA_CMD_IDENTIFY) {
        d->phase = ZR_ATA_IDLE;
        d->status = ZR_ATA_DRDY | ZR_ATA_DSC;
        return;
    }
    d->left--;
    d->count = (uint8_t)d->left;
    d->lba++;
    if (d->left) {
        go_busy(a, d, ST_NEXT_IN, a->t_sector);
    } else {
        d->phase = ZR_ATA_IDLE;
        d->status = ZR_ATA_DRDY | ZR_ATA_DSC;
    }
}

/* The host has supplied the whole sector buffer: the drive writes it. */
static void data_out_done(zr_ata *a, zr_ata_drive *d) {
    a->flip = 0;
    go_busy(a, d, ST_WRITE, a->t_sector);
}

static void commit_sector(zr_ata_drive *d) {
    if (!zr_card_write(d->card, d->lba, d->buf)) {
        d->count = (uint8_t)d->left;
        finish_error(d, ZR_ATA_E_ABRT, ZR_ATA_DWF);
        return;
    }
    d->accesses++;
    d->left--;
    d->count = (uint8_t)d->left;
    d->lba++;
    if (d->left) write_prepare(d, 0);
    else finish_ok(d);
}

/* --- commands ------------------------------------------------------------------------------------ */

static void identify_fill(zr_ata_drive *d) {
    zr_card_identify(d->card, d->buf);
    /* words 54 to 58: the geometry in force now, and the capacity it reaches */
    const uint32_t chs = (uint32_t)((uint64_t)d->cur_cyl * d->cur_heads * d->cur_spt);
    const unsigned w[5] = {d->cur_cyl, d->cur_heads, d->cur_spt, chs & 0xFFFFu, chs >> 16};
    for (unsigned i = 0; i < 5; i++) {
        d->buf[2 * (54 + i)] = (uint8_t)w[i];
        d->buf[2 * (54 + i) + 1] = (uint8_t)(w[i] >> 8);
    }
    d->buf[510] = 0xA5;
    uint8_t sum = 0;
    for (unsigned i = 0; i < 511; i++) sum = (uint8_t)(sum + d->buf[i]);
    d->buf[511] = (uint8_t)(0u - sum);
}

static int set_features(zr_ata_drive *d) {
    switch (d->features) {
    case 0x01: d->eight_bit = 1; return 1;
    case 0x81: d->eight_bit = 0; return 1;
    case 0x02: case 0x82: case 0x05: case 0x85: case 0x55: case 0xAA: case 0x66: case 0xCC: case 0xBB: case 0x44: return 1;
    case 0x03: return d->count == 0x00 || d->count == 0x01 || (d->count >= 0x08 && d->count <= 0x0C);
    default: return 0;
    }
}

/* The command runs: its BSY time has passed. */
static void execute(zr_ata *a, zr_ata_drive *d) {
    uint64_t lba;
    const unsigned cmd = d->cmd;
    d->error = 0;
    switch (cmd) {
    case ZR_ATA_CMD_READ: case ZR_ATA_CMD_READ_NR:
    case ZR_ATA_CMD_WRITE: case ZR_ATA_CMD_WRITE_NR:
    case ZR_ATA_CMD_VERIFY: case ZR_ATA_CMD_VERIFY_NR: {
        const int write = cmd == ZR_ATA_CMD_WRITE || cmd == ZR_ATA_CMD_WRITE_NR;
        const int verify = cmd == ZR_ATA_CMD_VERIFY || cmd == ZR_ATA_CMD_VERIFY_NR;
        d->left = d->count ? d->count : 256u;
        if (write && !d->card->writable) { finish_error(d, ZR_ATA_E_ABRT, 0); break; }
        if (!resolve(d, &lba, 1)) { finish_error(d, ZR_ATA_E_IDNF, 0); break; }
        d->lba = lba;
        d->limit = range_limit(d);
        if (verify) {
            const uint64_t last = lba + d->left - 1u;
            if (last >= d->limit) { d->lba = d->limit; regs_from_lba(d, d->limit); d->count = (uint8_t)(last - d->limit + 1u); finish_error(d, ZR_ATA_E_IDNF, 0); break; }
            regs_from_lba(d, last);
            d->count = 0;
            finish_ok(d);
        } else if (write) {
            write_prepare(d, 1);
        } else {
            read_sector(d);
        }
        break;
    }
    case ZR_ATA_CMD_IDENTIFY:
        identify_fill(d);
        d->pos = 0;
        d->phase = ZR_ATA_DATA_IN;
        d->status = ZR_ATA_DRDY | ZR_ATA_DSC | ZR_ATA_DRQ;
        d->intr = 1;
        break;
    case ZR_ATA_CMD_DIAGNOSTIC:
        for (unsigned u = 0; u < 2; u++)
            if (present(&a->drive[u])) {
                signature(&a->drive[u], u);
                if (&a->drive[u] != d) { a->drive[u].phase = ZR_ATA_IDLE; a->drive[u].status = ZR_ATA_DRDY | ZR_ATA_DSC; }
            }
        finish_ok(d);
        break;
    case ZR_ATA_CMD_INIT_PARAMS: {
        const unsigned spt = d->count, heads = (d->head & 15u) + 1u;
        const uint64_t cyl = d->card->sectors / ((uint64_t)heads * (spt ? spt : 1u));
        if (spt == 0 || spt > 255 || cyl == 0) { finish_error(d, ZR_ATA_E_ABRT, 0); break; }
        d->cur_spt = spt;
        d->cur_heads = heads;
        d->cur_cyl = cyl > 65535u ? 65535u : (unsigned)cyl;
        finish_ok(d);
        break;
    }
    case ZR_ATA_CMD_SET_FEATURES:
        if (set_features(d)) finish_ok(d);
        else finish_error(d, ZR_ATA_E_ABRT, 0);
        break;
    case ZR_ATA_CMD_CHECK_POWER: case ZR_ATA_CMD_CHECK_POWER_OLD:
        d->count = 0xFF;
        finish_ok(d);
        break;
    case ZR_ATA_CMD_FLUSH:
        zr_card_flush(d->card);
        finish_ok(d);
        break;
    case ZR_ATA_CMD_STANDBY_IMM: case ZR_ATA_CMD_IDLE_IMM: case ZR_ATA_CMD_STANDBY: case ZR_ATA_CMD_IDLE: case ZR_ATA_CMD_SLEEP:
    case 0x94: case 0x95: case 0x96: case 0x97: case 0x99:
        finish_ok(d);
        break;
    default:
        if (cmd >= ZR_ATA_CMD_RECALIBRATE && cmd <= 0x1F) {
            finish_ok(d);
        } else if (cmd >= ZR_ATA_CMD_SEEK && cmd <= 0x7F) {
            if (resolve(d, &lba, 0)) finish_ok(d);
            else finish_error(d, ZR_ATA_E_IDNF, 0);
        } else {
            finish_error(d, ZR_ATA_E_ABRT, 0);
        }
        break;
    }
}

static void command(zr_ata *a, zr_ata_drive *d, uint8_t cmd) {
    a->flip = 0;
    d->intr = 0;
    d->cmd = cmd;
    d->pos = 0;
    go_busy(a, d, ST_COMMAND, a->t_command);
}

/* --- reset ---------------------------------------------------------------------------------------- */

static int in_reset(const zr_ata *a) { return (a->devctl & ZR_ATA_SRST) || a->pin_reset; }

static void reset_enter(zr_ata *a) {
    for (unsigned u = 0; u < 2; u++) {
        zr_ata_drive *d = &a->drive[u];
        if (!present(d)) continue;
        d->phase = ZR_ATA_RESETTING;
        d->status = ZR_ATA_BSY;
        d->intr = 0;
        d->eight_bit = 0;
        d->cmd = 0;
        d->due = ZR_NEVER;
        default_geometry(d);
    }
    a->flip = 0;
}

static void reset_leave(zr_ata *a) {
    a->selected = 0;
    for (unsigned u = 0; u < 2; u++)
        if (present(&a->drive[u])) a->drive[u].due = zr_dev_now(&a->dev) + a->t_reset;
}

static void reset_done(zr_ata_drive *d, unsigned unit) {
    signature(d, unit);
    d->phase = ZR_ATA_IDLE;
    d->status = ZR_ATA_DRDY | ZR_ATA_DSC;
}

static void apply_reset_state(zr_ata *a, int was) {
    const int now = in_reset(a);
    if (now && !was) reset_enter(a);
    else if (!now && was) reset_leave(a);
}

/* --- the data register ------------------------------------------------------------------------ */

static uint8_t data_read(zr_ata *a) {
    zr_ata_drive *d = selected_drive(a);
    if (!present(d)) return a->drive[0].card || a->drive[1].card ? 0x00 : 0xFF;
    if (d->phase != ZR_ATA_DATA_IN) return 0xFF;
    uint8_t v;
    if (d->eight_bit) {
        v = d->buf[d->pos++];
    } else if (!a->flip) {
        v = d->buf[d->pos];
        a->flip = 1;
    } else {
        v = d->buf[d->pos + 1u];
        d->pos += 2;
        a->flip = 0;
    }
    if (d->pos >= ZR_CARD_SECTOR) data_in_done(a, d);
    return v;
}

static void data_write(zr_ata *a, uint8_t v) {
    zr_ata_drive *d = selected_drive(a);
    if (!present(d) || d->phase != ZR_ATA_DATA_OUT) return;
    if (d->eight_bit) {
        d->buf[d->pos++] = v;
    } else if (!a->flip) {
        d->buf[d->pos] = v;
        a->flip = 1;
    } else {
        d->buf[d->pos + 1u] = v;
        d->pos += 2;
        a->flip = 0;
    }
    if (d->pos >= ZR_CARD_SECTOR) data_out_done(a, d);
}

uint16_t zr_ata_read_data16(zr_ata *a) {
    zr_ata_drive *d = selected_drive(a);
    uint16_t v;
    if (!present(d)) return a->drive[0].card || a->drive[1].card ? 0x0000u : 0xFFFFu;
    if (d->phase != ZR_ATA_DATA_IN) return 0xFFFFu;
    if (d->eight_bit) {
        v = d->buf[d->pos++];
    } else {
        v = (uint16_t)(d->buf[d->pos] | (uint16_t)d->buf[d->pos + 1u] << 8);
        d->pos += 2;
    }
    a->flip = 0;
    if (d->pos >= ZR_CARD_SECTOR) data_in_done(a, d);
    update_lines(a);
    rearm(a);
    return v;
}

void zr_ata_write_data16(zr_ata *a, uint16_t value) {
    zr_ata_drive *d = selected_drive(a);
    if (!present(d) || d->phase != ZR_ATA_DATA_OUT) return;
    if (d->eight_bit) {
        d->buf[d->pos++] = (uint8_t)value;
    } else {
        d->buf[d->pos] = (uint8_t)value;
        d->buf[d->pos + 1u] = (uint8_t)(value >> 8);
        d->pos += 2;
    }
    a->flip = 0;
    if (d->pos >= ZR_CARD_SECTOR) data_out_done(a, d);
    update_lines(a);
    rearm(a);
}

/* --- the registers --------------------------------------------------------------------------- */

uint8_t zr_ata_read(zr_ata *a, unsigned reg) {
    zr_ata_drive *d = selected_drive(a);
    uint8_t v;
    if (reg > ZR_ATA_ALT_STATUS) return 0xFF;
    if (reg == ZR_ATA_DATA) {
        v = data_read(a);
        update_lines(a);
        rearm(a);
        return v;
    }
    if (!present(d)) return a->drive[0].card || a->drive[1].card ? 0x00 : 0xFF;
    switch (reg) {
    case ZR_ATA_ERROR: v = d->error; break;
    case ZR_ATA_COUNT: v = d->count; break;
    case ZR_ATA_SECTOR: v = d->sector; break;
    case ZR_ATA_CYL_LO: v = d->cyl_lo; break;
    case ZR_ATA_CYL_HI: v = d->cyl_hi; break;
    case ZR_ATA_HEAD: v = (uint8_t)(d->head | 0xA0u); break;
    default:
        v = d->status;
        if (reg == ZR_ATA_STATUS && d->intr) { d->intr = 0; update_lines(a); }
        break;
    }
    return v;
}

void zr_ata_write(zr_ata *a, unsigned reg, uint8_t value) {
    if (reg > ZR_ATA_DEVCTL) return;
    if (reg == ZR_ATA_DATA) {
        data_write(a, value);
    } else if (reg == ZR_ATA_DEVCTL) {
        const int was = in_reset(a);
        a->flip = 0;
        a->devctl = (uint8_t)(value & (ZR_ATA_NIEN | ZR_ATA_SRST));
        apply_reset_state(a, was);
    } else if (reg == ZR_ATA_COMMAND) {
        zr_ata_drive *d = selected_drive(a);
        a->flip = 0;
        if (present(d) && !busy(d)) {
            command(a, d, value);
            if (value == ZR_ATA_CMD_DIAGNOSTIC)
                for (unsigned u = 0; u < 2; u++)
                    if (&a->drive[u] != d && present(&a->drive[u]) && !busy(&a->drive[u])) a->drive[u].intr = 0;
        }
    } else {
        a->flip = 0;
        for (unsigned u = 0; u < 2; u++) {
            zr_ata_drive *d = &a->drive[u];
            if (!present(d) || busy(d)) continue;
            switch (reg) {
            case ZR_ATA_FEATURES: d->features = value; break;
            case ZR_ATA_COUNT: d->count = value; break;
            case ZR_ATA_SECTOR: d->sector = value; break;
            case ZR_ATA_CYL_LO: d->cyl_lo = value; break;
            case ZR_ATA_CYL_HI: d->cyl_hi = value; break;
            default: d->head = value; break;
            }
        }
        if (reg == ZR_ATA_HEAD) a->selected = (value & ZR_ATA_HEAD_DRV) ? 1u : 0u;
    }
    update_lines(a);
    rearm(a);
}

/* --- the chip -------------------------------------------------------------------------------------- */

static uint8_t chip_read(void *dev, unsigned space, uint32_t offset) { (void)space; return zr_ata_read(dev, offset); }
static void chip_write(void *dev, unsigned space, uint32_t offset, uint8_t value) { (void)space; zr_ata_write(dev, offset, value); }

static void chip_event(void *dev, uint64_t now) {
    zr_ata *a = dev;
    a->armed = ZR_NEVER;
    for (unsigned u = 0; u < 2; u++) {
        zr_ata_drive *d = &a->drive[u];
        if (!present(d) || d->due > now) continue;
        d->due = ZR_NEVER;
        if (d->phase == ZR_ATA_RESETTING) {
            reset_done(d, u);
        } else if (d->phase == ZR_ATA_BUSY) {
            switch (d->stage) {
            case ST_NEXT_IN: read_sector(d); break;
            case ST_WRITE: commit_sector(d); break;
            default: execute(a, d); break;
            }
        }
    }
    update_lines(a);
    rearm(a);
}

static void chip_reset(void *dev) {
    zr_ata *a = dev;
    a->devctl = 0;
    a->pin_reset = 0;
    a->selected = 0;
    a->flip = 0;
    for (unsigned u = 0; u < 2; u++)
        if (present(&a->drive[u])) {
            a->drive[u].count = a->drive[u].sector = a->drive[u].cyl_lo = a->drive[u].cyl_hi = a->drive[u].head = 0;
            a->drive[u].features = 0;
        }
    reset_enter(a);
    reset_leave(a);
    update_lines(a);
    rearm(a);
}

static void chip_input(void *dev, int pin, int state) {
    zr_ata *a = dev;
    if (pin != ZR_ATA_PIN_RESET) return;
    const int was = in_reset(a);
    a->pin_reset = state ? 1u : 0u;
    apply_reset_state(a, was);
    update_lines(a);
    rearm(a);
}

/* --- state ------------------------------------------------------------------------------------------ */

static void chip_save(const void *dev, zr_state_writer *w) {
    const zr_ata *a = dev;
    zr_sw_u8(w, STATE_VERSION);
    const uint8_t flags[] = {a->selected, a->devctl, a->pin_reset, a->flip, a->line_intrq, a->line_light,
                             (uint8_t)(present(&a->drive[0]) ? 1 : 0), (uint8_t)(present(&a->drive[1]) ? 1 : 0)};
    zr_sw_bytes(w, flags, sizeof flags);
    for (unsigned u = 0; u < 2; u++) {
        const zr_ata_drive *d = &a->drive[u];
        if (!present(d)) continue;
        const uint8_t regs[] = {d->error, d->features, d->count, d->sector, d->cyl_lo, d->cyl_hi, d->head, d->status,
                                d->phase, d->stage, d->cmd, d->intr, d->eight_bit};
        zr_sw_bytes(w, regs, sizeof regs);
        zr_sw_u32(w, d->cur_cyl); zr_sw_u32(w, d->cur_heads); zr_sw_u32(w, d->cur_spt);
        zr_sw_u64(w, d->lba); zr_sw_u64(w, d->limit);
        zr_sw_u32(w, d->left); zr_sw_u32(w, d->pos);
        zr_sw_u64(w, d->due);
        zr_sw_bytes(w, d->buf, sizeof d->buf);
    }
}

static int chip_load(void *dev, zr_state_reader *r) {
    zr_ata *a = dev;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    uint8_t flags[8];
    zr_sr_bytes(r, flags, sizeof flags);
    if (!zr_sr_ok(r) || flags[6] != (present(&a->drive[0]) ? 1u : 0u) || flags[7] != (present(&a->drive[1]) ? 1u : 0u)) return 0;
    zr_ata_drive in[2];
    memset(in, 0, sizeof in);
    for (unsigned u = 0; u < 2; u++) {
        zr_ata_drive *d = &in[u];
        if (!present(&a->drive[u])) continue;
        uint8_t regs[13];
        zr_sr_bytes(r, regs, sizeof regs);
        d->error = regs[0]; d->features = regs[1]; d->count = regs[2]; d->sector = regs[3]; d->cyl_lo = regs[4]; d->cyl_hi = regs[5];
        d->head = regs[6]; d->status = regs[7]; d->phase = regs[8]; d->stage = regs[9]; d->cmd = regs[10]; d->intr = regs[11] ? 1u : 0u;
        d->eight_bit = regs[12] ? 1u : 0u;
        d->cur_cyl = zr_sr_u32(r); d->cur_heads = zr_sr_u32(r); d->cur_spt = zr_sr_u32(r);
        d->lba = zr_sr_u64(r); d->limit = zr_sr_u64(r);
        d->left = zr_sr_u32(r); d->pos = zr_sr_u32(r);
        d->due = zr_sr_u64(r);
        zr_sr_bytes(r, d->buf, sizeof d->buf);
        if (!zr_sr_ok(r)) return 0;
        if (d->phase > ZR_ATA_RESETTING || d->stage > ST_WRITE || d->pos > ZR_CARD_SECTOR || d->left > 256 ||
            d->cur_heads < 1 || d->cur_heads > 16 || d->cur_spt < 1 || d->cur_spt > 255 || d->cur_cyl < 1 || d->cur_cyl > 65535 ||
            d->limit > a->drive[u].card->sectors || d->lba > d->limit || ((d->pos & 1u) && !d->eight_bit) ||
            ((d->phase == ZR_ATA_BUSY || d->phase == ZR_ATA_RESETTING) && !(d->status & ZR_ATA_BSY)) ||
            (d->phase == ZR_ATA_BUSY && d->due == ZR_NEVER))
            return 0;
    }
    if (flags[0] > 1) return 0;
    /* what the board owns stays: the host, the clock, the cards, the light counters */
    a->selected = flags[0]; a->devctl = flags[1]; a->pin_reset = flags[2] ? 1u : 0u; a->flip = flags[3] ? 1u : 0u;
    a->line_intrq = flags[4] ? 1u : 0u; a->line_light = flags[5] ? 1u : 0u;
    for (unsigned u = 0; u < 2; u++) {
        if (!present(&a->drive[u])) continue;
        zr_card *card = a->drive[u].card;
        const uint32_t accesses = a->drive[u].accesses;
        a->drive[u] = in[u];
        a->drive[u].card = card;
        a->drive[u].accesses = accesses;
    }
    a->armed = ZR_NEVER - 1u;                   /* force the deadline to be armed again */
    rearm(a);
    return 1;
}

static const zr_dev_ops ATA_OPS = {
    .read = chip_read, .write = chip_write, .event = chip_event, .reset = chip_reset, .input = chip_input,
    .save = chip_save, .load = chip_load,
};

static uint64_t micros(uint32_t clock_hz, unsigned us) {
    const uint64_t t = (uint64_t)clock_hz * us / 1000000u;
    return t ? t : 1u;
}

void zr_ata_init(zr_ata *a, const zr_hw_host *host, const char *name, uint32_t clock_hz) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &ATA_OPS;
    a->dev.host = host;
    a->dev.name = name ? name : "ata";
    a->clock_hz = clock_hz ? clock_hz : 1u;
    a->t_command = micros(a->clock_hz, ZR_ATA_COMMAND_US);
    a->t_sector = micros(a->clock_hz, ZR_ATA_SECTOR_US);
    a->t_reset = micros(a->clock_hz, ZR_ATA_RESET_US);
    a->armed = ZR_NEVER;
    for (unsigned u = 0; u < 2; u++) a->drive[u].due = ZR_NEVER;
}

void zr_ata_attach(zr_ata *a, unsigned unit, zr_card *card) {
    if (unit > 1) return;
    zr_ata_drive *d = &a->drive[unit];
    memset(d, 0, sizeof *d);
    d->card = card;
    d->due = ZR_NEVER;
    if (card) power_on(d, unit);
    update_lines(a);
    rearm(a);
}

int zr_ata_intrq(const zr_ata *a) { return a->line_intrq; }
int zr_ata_light(const zr_ata *a) { return a->line_light; }
uint32_t zr_ata_activity(const zr_ata *a, unsigned unit) { return unit < 2 ? a->drive[unit].accesses : 0; }
