/* ZetRo - the uPD765A / 8272A floppy disk controller (upd765.h). Portable C11.
 * ZetRo's own code, written from the data sheet. */
#include "upd765.h"

#include "state.h"

#include <string.h>

#define STATE_VERSION 1u

enum { PH_IDLE, PH_CMD, PH_EXEC, PH_RESULT };
/* The stage of the command being executed: what the chip's deadline (t_cmd) is for. */
enum {
    X_NONE,
    X_START,        /* the command has been accepted: check the drive, load the head */
    X_HEAD,         /* the head load time has passed */
    X_WAKE,         /* waiting for a turning disk (t_cmd: when it will turn, or never) */
    X_FIND,         /* the outcome of the ID search is due: a sector found (x.found) or ND */
    X_RT_IDX,       /* read track: the index pulse */
    X_BYTE,         /* a byte of the data field is due */
    X_END,          /* the end of the sector: commit a write, decide what comes next */
    X_FMT_IDX,      /* format: the index pulse */
    X_FMT_TICK,     /* format: a byte of a sector's ID is due */
    X_FMT_END,      /* format: the end of the revolution: write the track */
    X_STAGES
};

#define CMD_LATENCY_US 24u      /* last command byte to first action */
#define RQM_DELAY_US   12u      /* the data register service interval */
#define RESET_INT_US   100u     /* RESET released to the first interrupt */
#define RECAL_PULSES   77u

static uint64_t now_of(const zr_upd765 *f) { return zr_dev_now(&f->dev); }

/* Guest time of `us` microseconds, rounded up. */
static uint64_t us_ticks(const zr_upd765 *f, uint64_t us) { return (us * f->clock_hz + 999999u) / 1000000u; }

static int is_scan(unsigned op) { return op == ZR_UPD765_SCAN_EQUAL || op == ZR_UPD765_SCAN_LOW || op == ZR_UPD765_SCAN_HIGH; }
static int is_write(unsigned op) { return op == ZR_UPD765_WRITE_DATA || op == ZR_UPD765_WRITE_DELETED; }

/* --- the pins ------------------------------------------------------------------------------ */

static void set_pin(zr_upd765 *f, int pin, uint8_t *cache, int level) {
    level = level != 0;
    if (*cache == level) return;
    *cache = (uint8_t)level;
    zr_dev_line(&f->dev, pin, level);
}
static void update_int(zr_upd765 *f) {
    set_pin(f, ZR_UPD765_PIN_INT, &f->line_int, !f->in_reset && (f->int_cmd || f->int_status));
}
static void set_drq(zr_upd765 *f, int level) { set_pin(f, ZR_UPD765_PIN_DRQ, &f->line_drq, level); }

int zr_upd765_int(const zr_upd765 *f) { return f->line_int; }
int zr_upd765_drq(const zr_upd765 *f) { return f->line_drq; }

static void rearm(zr_upd765 *f) {
    uint64_t m = f->t_cmd < f->t_rint ? f->t_cmd : f->t_rint;
    if (f->t_poll < m) m = f->t_poll;
    for (unsigned u = 0; u < 4; u++)
        if (f->t_seek[u] < m) m = f->t_seek[u];
    if (m != f->armed) {
        f->armed = m;
        zr_dev_schedule(&f->dev, m);
    }
}

/* --- times from Specify --------------------------------------------------------------------- */

/* A time of the data sheet (given at the 8 MHz clock) at the chip's clock, in microseconds. */
static uint64_t at_clock(const zr_upd765 *f, uint64_t us) { return us * 8000u / (f->clk_khz ? f->clk_khz : 4000u); }
static uint64_t step_time(const zr_upd765 *f) { return us_ticks(f, at_clock(f, (16u - f->srt) * 1000u)); }
static uint64_t hut_time(const zr_upd765 *f) { return us_ticks(f, at_clock(f, (f->hut ? f->hut : 16u) * 16000u)); }
static uint64_t hlt_time(const zr_upd765 *f) { return us_ticks(f, at_clock(f, (f->hlt ? f->hlt : 128u) * 2000u)); }

static int ready_input(const zr_upd765 *f, unsigned unit, uint64_t now) {
    switch (f->ready_mode) {
    case ZR_UPD765_READY_TIED: return 1;
    case ZR_UPD765_READY_PINS: return f->ready_pin[unit];
    default: return f->drive[unit] && zr_fdd_rotating(f->drive[unit], now);
    }
}

/* --- reset ---------------------------------------------------------------------------------- */

static void assert_reset(zr_upd765 *f) {
    f->in_reset = 1;
    f->t_cmd = f->t_rint = f->t_poll = ZR_NEVER;
    for (unsigned u = 0; u < 4; u++) f->t_seek[u] = ZR_NEVER;
    f->phase = PH_IDLE;
    f->cmd_got = f->cmd_len = f->res_len = f->res_pos = 0;
    memset(&f->x, 0, sizeof f->x);
    f->int_cmd = f->int_status = 0;
    memset(f->pcn, 0, sizeof f->pcn);
    memset(f->seeking, 0, sizeof f->seeking);
    memset(f->se_pending, 0, sizeof f->se_pending);
    memset(f->rc_pending, 0, sizeof f->rc_pending);
    f->reset_n = f->reset_pos = 0;
    f->srt = f->hut = f->hlt = f->nd = 0;
    f->head_loaded = 0;
    f->rqm_at = 0;
    set_drq(f, 0);
    update_int(f);
    rearm(f);
}

static void release_reset(zr_upd765 *f) {
    if (!f->in_reset) return;
    const uint64_t now = now_of(f);
    f->in_reset = 0;
    f->phase = PH_IDLE;
    f->rqm_at = now;
    f->t_rint = now + us_ticks(f, RESET_INT_US);
    rearm(f);
}

/* --- results -------------------------------------------------------------------------------- */

/* The reset results the program did not ask for are gone once anything else
 * interrupts: a BIOS that reads one must see the seek end of its next
 * Recalibrate, not the second drive's ready change. */
static void drop_reset_results(zr_upd765 *f) {
    f->reset_pos = f->reset_n;
    f->int_status = 0;                          /* the one interrupt flip-flop: what is pending now is only a seek's or a ready change */
    for (unsigned u = 0; u < 4; u++)
        if (f->rc_pending[u] || f->se_pending[u]) f->int_status = 1;
}

static void enter_result(zr_upd765 *f, unsigned n, uint64_t t, int raise_int) {
    if (raise_int) drop_reset_results(f);
    f->phase = PH_RESULT;
    f->res_len = (uint8_t)n;
    f->res_pos = 0;
    f->rqm_at = t;
    f->x.stage = X_NONE;
    f->x.pending = 0;
    f->t_cmd = ZR_NEVER;
    set_drq(f, 0);
    if (raise_int) f->int_cmd = 1;
    update_int(f);
}

/* The seven result bytes of a data command; ic 1 is abnormal termination. */
static void finish(zr_upd765 *f, uint64_t t, unsigned ic) {
    zr_upd765_exec *x = &f->x;
    f->res[0] = (uint8_t)(ic << 6 | x->st0 | x->head << 2 | x->unit);
    f->res[1] = x->st1;
    f->res[2] = x->st2;
    f->res[3] = x->c;
    f->res[4] = x->h;
    f->res[5] = x->r;
    f->res[6] = x->n;
    f->head_loaded = 1;
    f->head_until = t + hut_time(f);
    if (x->fdd) zr_fdd_notify(x->fdd, ZR_DRIVE_HEAD_HOLD, t, (uint32_t)hut_time(f), 0);
    enter_result(f, 7, t, 1);
}
static void fail(zr_upd765 *f, uint64_t t, unsigned st1, unsigned st2) {
    f->x.st1 |= (uint8_t)st1;
    f->x.st2 |= (uint8_t)st2;
    finish(f, t, 1);
}

/* --- the data transfer ----------------------------------------------------------------------- */

/* A byte is waiting for the host (read) or wanted from it (write, scan). */
static void request(zr_upd765 *f) {
    f->x.pending = 1;
    if (f->nd) {
        f->int_cmd = 1;
        update_int(f);
    } else set_drq(f, 1);
}
static void unrequest(zr_upd765 *f) {
    f->x.pending = 0;
    if (f->nd) {
        f->int_cmd = 0;
        update_int(f);
    } else set_drq(f, 0);
}

/* The guest time of the data field's byte `i` (its start) of the sector in x. */
static uint64_t byte_time(const zr_upd765_exec *x, uint32_t i) {
    return x->data_t + (uint64_t)i * x->unit_units * x->num / x->den;
}

static void take_slot(zr_upd765_exec *x, const zr_fdd_slot *s) {
    x->slot_index = (uint8_t)s->index;
    x->id_t = zr_fdd_slot_time(s, s->id_end);
    x->data_t = zr_fdd_slot_time(s, s->data);
    x->end_t = zr_fdd_slot_time(s, s->data_end);
    x->unit_units = s->unit;
    x->num = s->num;
    x->den = s->den;
}

/* A CRC error in the ID field: the ID cannot be used. */
static int id_crc_error(const zr_sector *s) { return (s->st1 & ZR_DISK_ST1_DE) && !(s->st2 & ZR_DISK_ST2_DD); }

/* Which of the sectors' ID the controller sees next as the disk turns, and
 * when: sets the deadline of the outcome (a sector found, or the search
 * ended by the second index pulse). */
static void begin_find(zr_upd765 *f, uint64_t from) {
    zr_upd765_exec *x = &f->x;
    zr_fdd *d = x->fdd;
    const uint64_t rotating_at = d ? zr_fdd_rotating_at(d) : ZR_NEVER;
    x->found = 0;
    if (rotating_at == ZR_NEVER || from < rotating_at) {
        x->stage = X_WAKE;
        f->t_cmd = rotating_at;
        return;
    }
    const uint64_t deadline = zr_fdd_next_index(d, from) + zr_fdd_rev(d);
    uint64_t cursor = from;
    x->saw_id = 0;
    x->crc_seen = 0;
    x->st2 &= (uint8_t)~(ZR_ST2_WC | ZR_ST2_BC);
    for (;;) {
        zr_fdd_slot slot;
        if (!zr_fdd_find_id(d, x->head, !x->mfm, cursor, &slot)) break;
        const uint64_t end = zr_fdd_slot_time(&slot, slot.id_end);
        if (end >= deadline) break;
        x->saw_id = 1;
        const zr_sector *s = slot.sector;
        if (id_crc_error(s)) {
            x->crc_seen = 1;
            cursor = end;
            continue;
        }
        if (x->op == ZR_UPD765_READ_ID) {
            x->c = s->c; x->h = s->h; x->r = s->r; x->n = s->n;
        }
        if (x->op == ZR_UPD765_READ_ID || (s->c == x->c && s->h == x->h && s->r == x->r && s->n == x->n)) {
            x->found = 1;
            take_slot(x, &slot);
            x->stage = X_FIND;
            f->t_cmd = end;
            return;
        }
        if (s->c != x->c) {
            x->st2 |= ZR_ST2_WC;
            if (s->c == 0xFFu) x->st2 |= ZR_ST2_BC;
        }
        cursor = end;
    }
    x->fail_st1 = (uint8_t)((x->op == ZR_UPD765_READ_ID ? 0 : ZR_ST1_ND) | (x->saw_id ? 0 : ZR_ST1_MA) | (x->crc_seen ? ZR_ST1_DE : 0));
    if (x->op == ZR_UPD765_READ_ID) x->fail_st1 |= ZR_ST1_MA;
    x->stage = X_FIND;
    f->t_cmd = deadline;
}

/* The wait for the next index pulse that format and read track start at. */
static void begin_index(zr_upd765 *f, uint64_t from, unsigned stage) {
    zr_upd765_exec *x = &f->x;
    zr_fdd *d = x->fdd;
    const uint64_t rotating_at = d ? zr_fdd_rotating_at(d) : ZR_NEVER;
    if (rotating_at == ZR_NEVER || from < rotating_at) {
        x->stage = X_WAKE;
        f->t_cmd = rotating_at;
        return;
    }
    x->stage = (uint8_t)stage;
    f->t_cmd = zr_fdd_next_index(d, from);
}

/* How many bytes of the sector of `size` bytes the host takes: all, or DTL of the 128 when N is 0. */
static uint32_t transfer_length(const zr_upd765_exec *x, uint32_t size) {
    if (x->n == 0 && x->op != ZR_UPD765_READ_TRACK && !is_scan(x->op)) return x->dtl < size ? x->dtl : size;
    return size;
}

/* The sector at x.slot_index is read from the disk and its data field starts coming (check_mark: a data
 * mark of the wrong kind is a control mark; read track takes the sector as it is). */
static void begin_read_transfer(zr_upd765 *f, uint64_t t, int check_mark) {
    zr_upd765_exec *x = &f->x;
    const zr_sector *s = zr_fdd_sector(x->fdd, x->head, x->slot_index);
    if (!s) { fail(f, t, ZR_ST1_ND, 0); return; }
    const uint32_t size = zr_disk_nominal(s->n);
    if (!zr_fdd_read_sector(x->fdd, x->head, x->slot_index, x->buf, size, &s)) { fail(f, t, ZR_ST1_ND, 0); return; }
    x->skip = 0;
    x->crc_err = 0;
    x->scan_cmp = 0;
    if (s->st2 & ZR_DISK_ST2_MD) {                  /* no data address mark: nothing to transfer */
        if (x->op != ZR_UPD765_READ_TRACK) { fail(f, t, ZR_ST1_MA, ZR_ST2_MD); return; }
        x->st1 |= ZR_ST1_MA;
        x->st2 |= ZR_ST2_MD;
    }
    const int deleted = (s->st2 & ZR_DISK_ST2_CM) != 0;
    if (check_mark && deleted != (x->op == ZR_UPD765_READ_DELETED)) {
        x->st2 |= ZR_ST2_CM;
        if (x->sk) x->skip = 1;
        else x->stop = 1;
    }
    if (check_mark && is_scan(x->op) && deleted) {  /* a scan treats a deleted sector as the last one */
        x->st2 |= ZR_ST2_CM;
        if (x->sk) { x->skip = 1; x->stop = 0; }
        else x->stop = 1;
    }
    if (!x->skip && (s->st2 & ZR_DISK_ST2_DD)) x->crc_err = 1;
    if (!x->skip && x->op == ZR_UPD765_READ_TRACK) x->st2 |= (uint8_t)(s->st2 & ZR_DISK_ST2_CM);
    x->size = size;
    x->xfer = transfer_length(x, size);
    x->bytepos = 0;
    x->dir = !is_scan(x->op);
    if (x->skip) {
        x->stage = X_END;
        f->t_cmd = x->end_t;
        return;
    }
    if (!x->xfer) {
        x->stage = X_END;
        f->t_cmd = x->end_t;
        return;
    }
    x->stage = X_BYTE;
    if (x->dir) f->t_cmd = byte_time(x, 1);
    else {                                          /* a scan wants the host's bytes, as a write does */
        f->t_cmd = byte_time(x, 0);
        request(f);
    }
}

/* An ID matched (or, for Read ID, passed): `t` is the end of the ID field. */
static void sector_start(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    switch (x->op) {
    case ZR_UPD765_READ_ID:
        finish(f, t, 0);
        break;
    case ZR_UPD765_WRITE_DATA:
    case ZR_UPD765_WRITE_DELETED: {
        const zr_sector *s = zr_fdd_sector(x->fdd, x->head, x->slot_index);
        if (!s) { fail(f, t, ZR_ST1_ND, 0); return; }
        x->size = zr_disk_nominal(s->n);
        x->xfer = transfer_length(x, x->size);
        memset(x->buf, 0, x->size);
        x->dir = 0;
        x->bytepos = 0;
        if (!x->xfer) {
            x->stage = X_END;
            f->t_cmd = x->end_t;
            break;
        }
        x->stage = X_BYTE;
        f->t_cmd = x->data_t;
        request(f);
        break;
    }
    default:
        begin_read_transfer(f, t, x->op != ZR_UPD765_READ_TRACK);
        break;
    }
}

/* The ID registers after a sector, as the data sheet's table has them. */
static void advance(zr_upd765_exec *x) {
    if (x->r != x->eot) {
        x->r++;
        return;
    }
    x->r = 1;
    if (x->mt && x->head == 0) {
        x->head = 1;
        x->h = 1;
    } else {
        x->c++;
        if (x->mt) x->h = 0;
    }
}

static void rt_sector(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    zr_fdd_slot slot;
    if (!zr_fdd_find_id(x->fdd, x->head, !x->mfm, x->end_t, &slot)) {
        fail(f, t, ZR_ST1_ND | ZR_ST1_MA, 0);
        return;
    }
    if (slot.index != x->rt_slot) {                 /* the track has no more sectors */
        x->r++;
        finish(f, t, 0);
        return;
    }
    x->c = slot.sector->c; x->h = slot.sector->h; x->r = slot.sector->r; x->n = slot.sector->n;
    take_slot(x, &slot);
    begin_read_transfer(f, t, 0);
}

/* A scan's sector is over: it was satisfied or the scan goes on. */
static void scan_evaluate(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    const int equal = x->scan_cmp == 0;
    int satisfied = equal;
    if (x->op == ZR_UPD765_SCAN_LOW && x->scan_cmp == 1) satisfied = 1;
    if (x->op == ZR_UPD765_SCAN_HIGH && x->scan_cmp == 2) satisfied = 1;
    x->st2 &= (uint8_t)~(ZR_ST2_SH | ZR_ST2_SN);
    if (equal) x->st2 |= ZR_ST2_SH;
    if (!satisfied) x->st2 |= ZR_ST2_SN;
    if (satisfied || x->tc || x->stop || x->r >= x->eot) {
        advance(x);
        finish(f, t, 0);
        return;
    }
    const unsigned next = (unsigned)x->r + (x->stp ? x->stp : 1u);
    if (next > x->eot) {                            /* the sector after the last one asked for is not there */
        x->r = (uint8_t)next;
        fail(f, t, ZR_ST1_ND, 0);
        return;
    }
    x->r = (uint8_t)next;
    begin_find(f, t);
}

/* The sector is over: what next. */
static void evaluate(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    if (x->op == ZR_UPD765_READ_TRACK) {
        x->rt_count++;
        if (x->tc || x->rt_count >= x->eot || !zr_fdd_sector(x->fdd, x->head, x->rt_slot + 1u)) {
            x->r++;
            finish(f, t, (x->st1 & (ZR_ST1_DE | ZR_ST1_MA | ZR_ST1_ND | ZR_ST1_OR)) ? 1u : 0u);
        } else {
            x->rt_slot++;
            rt_sector(f, t);
        }
        return;
    }
    if (is_scan(x->op)) {
        scan_evaluate(f, t);
        return;
    }
    if (x->tc || x->stop) {
        advance(x);
        finish(f, t, 0);
        return;
    }
    if (x->r == x->eot && !(x->mt && x->head == 0)) {
        advance(x);
        x->st1 |= ZR_ST1_EN;
        finish(f, t, 1);
        return;
    }
    advance(x);
    begin_find(f, t);
}

static void on_byte(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    if (x->dir) {                               /* controller to host */
        if (x->pending) { fail(f, t, ZR_ST1_OR, 0); return; }
        if (x->tc) {                            /* the host is done: the rest of the sector passes */
            x->stage = X_END;
            f->t_cmd = x->end_t;
            return;
        }
        x->dreg = x->buf[x->bytepos++];
        request(f);
        if (x->bytepos >= x->xfer) {
            x->stage = X_END;
            f->t_cmd = x->end_t;
        } else f->t_cmd = byte_time(x, x->bytepos + 1u);
        return;
    }
    /* host to controller: byte `bytepos` is consumed now */
    if (x->pending) { unrequest(f); fail(f, t, ZR_ST1_OR, 0); return; }
    if (is_scan(x->op)) {
        const uint8_t disk = x->buf[x->bytepos], host = x->dreg;
        if (x->scan_cmp == 0 && disk != host && disk != 0xFFu && host != 0xFFu) x->scan_cmp = disk < host ? 1 : 2;
        x->bytepos++;
    } else {
        x->buf[x->bytepos++] = x->dreg;
    }
    if (x->tc || x->bytepos >= x->xfer) {
        x->stage = X_END;
        f->t_cmd = x->end_t;
        return;
    }
    request(f);
    f->t_cmd = byte_time(x, x->bytepos);
}

static void on_end(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    if (x->dir) {
        if (x->pending) { fail(f, t, ZR_ST1_OR, 0); return; }
        if (x->crc_err && x->op != ZR_UPD765_READ_TRACK) { fail(f, t, ZR_ST1_DE, ZR_ST2_DD); return; }
        if (x->crc_err) { x->st1 |= ZR_ST1_DE; x->st2 |= ZR_ST2_DD; }
    } else if (is_write(x->op)) {
        const int rc = zr_fdd_write_sector(x->fdd, x->head, x->slot_index, x->buf, x->size, x->op == ZR_UPD765_WRITE_DELETED);
        if (rc == ZR_DISK_PROTECTED) { fail(f, t, ZR_ST1_NW, 0); return; }
        if (rc != ZR_DISK_OK) { fail(f, t, ZR_ST1_ND, 0); return; }
    } else if (x->pending) {                    /* a scan that the host did not feed in time */
        unrequest(f);
        fail(f, t, ZR_ST1_OR, 0);
        return;
    }
    evaluate(f, t);
}

/* --- format track ---------------------------------------------------------------------------- */

static uint64_t fmt_tick_time(const zr_upd765_exec *x, unsigned q) {
    const int fm = !x->mfm;
    const uint32_t unit = fm ? 2u : 1u;
    const uint64_t units = zr_fdd_format_pre(fm) + (uint64_t)(q / 4u) * zr_fdd_format_pitch(fm, x->fmt_n, x->fmt_gpl) +
                           (fm ? 7u : 16u) * unit + (q % 4u) * unit;
    return x->fmt_t0 + zr_fdd_ticks(x->fdd, units);
}

static void on_fmt_index(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    x->fmt_t0 = t;
    x->fmt_q = 0;
    x->dir = 0;
    if (x->fmt_sc == 0) {
        x->stage = X_FMT_END;
        f->t_cmd = t + zr_fdd_rev(x->fdd);
        return;
    }
    request(f);
    x->stage = X_FMT_TICK;
    f->t_cmd = fmt_tick_time(x, 0);
}

static void on_fmt_tick(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    if (x->pending) { unrequest(f); fail(f, t, ZR_ST1_OR, 0); return; }
    zr_disk_id *id = &x->fmt_ids[x->fmt_q / 4u];
    switch (x->fmt_q % 4u) {
    case 0: id->c = x->dreg; break;
    case 1: id->h = x->dreg; break;
    case 2: id->r = x->dreg; break;
    default: id->n = x->dreg; break;
    }
    x->fmt_q++;
    if (x->fmt_q == 4u * x->fmt_sc) {
        const uint64_t rev_end = x->fmt_t0 + zr_fdd_rev(x->fdd);
        x->stage = X_FMT_END;
        f->t_cmd = rev_end > t ? rev_end : t;
        return;
    }
    request(f);
    f->t_cmd = fmt_tick_time(x, x->fmt_q);
}

static void on_fmt_end(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    if (x->fmt_n > 7) { fail(f, t, ZR_ST1_ND, 0); return; }
    zr_disk_format format;
    memset(&format, 0, sizeof format);
    format.encoding = x->mfm ? ZR_ENC_MFM : ZR_ENC_FM;
    format.rate = f->rate >= 500 ? ZR_RATE_HD : ZR_RATE_DD;
    format.n = x->fmt_n;
    format.gap3 = x->fmt_gpl;
    format.filler = x->fmt_d;
    format.count = x->fmt_sc;
    format.id = x->fmt_ids;
    const int rc = zr_fdd_format_track(x->fdd, x->head, &format);
    if (rc == ZR_DISK_PROTECTED) { fail(f, t, ZR_ST1_NW, 0); return; }
    if (rc != ZR_DISK_OK) { fail(f, t, ZR_ST1_ND, 0); return; }
    if (x->fmt_sc) {
        const zr_disk_id *last = &x->fmt_ids[x->fmt_sc - 1u];
        x->c = last->c; x->h = last->h; x->r = last->r;
    }
    x->n = x->fmt_n;
    finish(f, t, 0);
}

/* --- starting a data command ----------------------------------------------------------------- */

static void begin_op(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    switch (x->op) {
    case ZR_UPD765_FORMAT: begin_index(f, t, X_FMT_IDX); break;
    case ZR_UPD765_READ_TRACK: begin_index(f, t, X_RT_IDX); break;
    default: begin_find(f, t); break;
    }
}

static void on_start(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    if (!ready_input(f, x->unit, t)) {
        x->st0 |= ZR_ST0_NR;
        finish(f, t, 1);
        return;
    }
    if ((is_write(x->op) || x->op == ZR_UPD765_FORMAT) && x->fdd && zr_fdd_wprot(x->fdd)) {
        fail(f, t, ZR_ST1_NW, 0);
        return;
    }
    if (!f->head_loaded || t >= f->head_until) {
        x->stage = X_HEAD;
        f->t_cmd = t + hlt_time(f);
        if (x->fdd) zr_fdd_notify(x->fdd, ZR_DRIVE_HEAD_LOAD, t, 0, 0);
        return;
    }
    begin_op(f, t);
}

static void on_cmd_timer(zr_upd765 *f, uint64_t t) {
    zr_upd765_exec *x = &f->x;
    f->t_cmd = ZR_NEVER;
    switch (x->stage) {
    case X_START: on_start(f, t); break;
    case X_HEAD: f->head_loaded = 1; begin_op(f, t); break;
    case X_WAKE: begin_op(f, t); break;
    case X_FIND:
        if (x->found) sector_start(f, t);
        else fail(f, t, x->fail_st1, 0);
        break;
    case X_RT_IDX:
        x->fmt_t0 = t;
        x->end_t = t;
        x->rt_slot = x->rt_count = 0;
        rt_sector(f, t);
        break;
    case X_BYTE: on_byte(f, t); break;
    case X_END: on_end(f, t); break;
    case X_FMT_IDX: on_fmt_index(f, t); break;
    case X_FMT_TICK: on_fmt_tick(f, t); break;
    case X_FMT_END: on_fmt_end(f, t); break;
    default: break;
    }
}

/* --- seek and recalibrate -------------------------------------------------------------------- */

static void seek_done(zr_upd765 *f, unsigned u, unsigned st0) {
    f->seeking[u] = 0;
    f->t_seek[u] = ZR_NEVER;
    drop_reset_results(f);
    f->se_pending[u] = 1;
    f->se_st0[u] = (uint8_t)(st0 | f->seek_hd[u] << 2 | u);
    f->int_status = 1;
    update_int(f);
}

static void seek_event(zr_upd765 *f, unsigned u, uint64_t t) {
    zr_fdd *d = f->drive[u];
    if (!f->seek_recal[u]) {
        if (f->seek_rem[u] == 0) {
            seek_done(f, u, ZR_ST0_SE);
            return;
        }
        if (d) zr_fdd_step(d, f->seek_dir[u], t);
        f->pcn[u] = (uint8_t)(f->pcn[u] + f->seek_dir[u]);
        f->seek_rem[u]--;
        f->t_seek[u] = t + step_time(f);
        return;
    }
    if (d && zr_fdd_track0(d)) {
        f->pcn[u] = 0;
        seek_done(f, u, ZR_ST0_SE);
        return;
    }
    if (f->seek_pulses[u] >= RECAL_PULSES) {
        seek_done(f, u, ZR_ST0_SE | ZR_ST0_EC | ZR_ST0_IC_ABNORMAL);
        return;
    }
    if (d) zr_fdd_step(d, -1, t);
    if (f->pcn[u]) f->pcn[u]--;
    f->seek_pulses[u]++;
    f->t_seek[u] = t + step_time(f);
}

/* --- the command phase ----------------------------------------------------------------------- */

static unsigned command_length(unsigned code) {
    switch (code) {
    case ZR_UPD765_READ_TRACK: case ZR_UPD765_WRITE_DATA: case ZR_UPD765_READ_DATA:
    case ZR_UPD765_WRITE_DELETED: case ZR_UPD765_READ_DELETED:
    case ZR_UPD765_SCAN_EQUAL: case ZR_UPD765_SCAN_LOW: case ZR_UPD765_SCAN_HIGH: return 9;
    case ZR_UPD765_FORMAT: return 6;
    case ZR_UPD765_SPECIFY: case ZR_UPD765_SEEK: return 3;
    case ZR_UPD765_SENSE_DRIVE: case ZR_UPD765_RECALIBRATE: case ZR_UPD765_READ_ID: return 2;
    default: return 1;                          /* Sense Interrupt Status, and the invalid codes */
    }
}

static void start_exec(zr_upd765 *f, uint64_t now) {
    const uint8_t *c = f->cmd;
    zr_upd765_exec *x = &f->x;
    memset(x, 0, sizeof *x);
    x->op = c[0] & 0x1Fu;
    x->mt = (c[0] >> 7) & 1u;
    x->mfm = (c[0] >> 6) & 1u;
    x->sk = (c[0] >> 5) & 1u;
    x->unit = c[1] & 3u;
    x->head = (c[1] >> 2) & 1u;
    x->fdd = f->drive[x->unit];
    x->tc = f->tc_level;
    if (x->op == ZR_UPD765_FORMAT) {
        x->fmt_n = c[2]; x->fmt_sc = c[3]; x->fmt_gpl = c[4]; x->fmt_d = c[5];
        x->n = c[2];
    } else if (x->op != ZR_UPD765_READ_ID) {
        x->c = c[2]; x->h = c[3]; x->r = c[4]; x->n = c[5]; x->eot = c[6]; x->gpl = c[7]; x->dtl = c[8]; x->stp = c[8];
    }
    x->dir = x->op == ZR_UPD765_READ_DATA || x->op == ZR_UPD765_READ_DELETED || x->op == ZR_UPD765_READ_TRACK;
    x->stage = X_START;
    f->phase = PH_EXEC;
    f->t_cmd = now + us_ticks(f, CMD_LATENCY_US);
}

static void start_command(zr_upd765 *f, uint64_t now) {
    const uint8_t *c = f->cmd;
    const unsigned code = c[0] & 0x1Fu;
    const uint64_t ready = now + us_ticks(f, RQM_DELAY_US);
    switch (code) {
    case ZR_UPD765_SPECIFY:
        f->srt = c[1] >> 4;
        f->hut = c[1] & 0x0Fu;
        f->hlt = c[2] >> 1;
        f->nd = c[2] & 1u;
        f->phase = PH_IDLE;
        f->rqm_at = ready;
        break;
    case ZR_UPD765_SENSE_DRIVE: {
        const unsigned u = c[1] & 3u;
        const zr_fdd *d = f->drive[u];
        unsigned st3 = u | (c[1] & 4u);
        if (d && d->installed) st3 |= ZR_ST3_TS;
        if (d && zr_fdd_track0(d)) st3 |= ZR_ST3_T0;
        if (d && zr_fdd_wprot(d)) st3 |= ZR_ST3_WP;
        if (ready_input(f, u, now)) st3 |= ZR_ST3_RY;
        f->res[0] = (uint8_t)st3;
        enter_result(f, 1, ready, 0);
        break;
    }
    case ZR_UPD765_SENSE_INT: {
        unsigned n = 1, u;
        f->int_status = 0;
        if (f->reset_pos < f->reset_n) {
            u = f->reset_list[f->reset_pos++];
            f->res[0] = (uint8_t)(ZR_ST0_IC_READY | u);
            f->res[1] = 0;
            n = 2;
        } else {
            for (u = 0; u < 4 && !f->rc_pending[u]; u++) {}
            if (u < 4) {
                f->rc_pending[u] = 0;
                f->res[0] = (uint8_t)(ZR_ST0_IC_READY | u);
                f->res[1] = f->pcn[u];
                n = 2;
            } else {
                for (u = 0; u < 4 && !f->se_pending[u]; u++) {}
                if (u < 4) {
                    f->se_pending[u] = 0;
                    f->res[0] = f->se_st0[u];
                    f->res[1] = f->pcn[u];
                    n = 2;
                } else f->res[0] = ZR_ST0_IC_INVALID;
            }
        }
        for (u = 0; u < 4; u++)
            if (f->rc_pending[u] || f->se_pending[u]) f->int_status = 1;
        update_int(f);
        enter_result(f, n, ready, 0);
        break;
    }
    case ZR_UPD765_RECALIBRATE:
    case ZR_UPD765_SEEK: {
        const unsigned u = c[1] & 3u;
        f->seeking[u] = 1;
        f->seek_recal[u] = code == ZR_UPD765_RECALIBRATE;
        f->seek_hd[u] = (c[1] >> 2) & 1u;
        f->seek_pulses[u] = 0;
        if (code == ZR_UPD765_SEEK) {
            f->seek_target[u] = c[2];
            f->seek_dir[u] = c[2] >= f->pcn[u] ? 1 : -1;
            f->seek_rem[u] = (uint8_t)(c[2] >= f->pcn[u] ? c[2] - f->pcn[u] : f->pcn[u] - c[2]);
        }
        f->t_seek[u] = now + us_ticks(f, CMD_LATENCY_US);
        f->phase = PH_IDLE;
        f->rqm_at = ready;
        break;
    }
    case ZR_UPD765_READ_TRACK: case ZR_UPD765_WRITE_DATA: case ZR_UPD765_READ_DATA:
    case ZR_UPD765_WRITE_DELETED: case ZR_UPD765_READ_ID: case ZR_UPD765_READ_DELETED:
    case ZR_UPD765_FORMAT: case ZR_UPD765_SCAN_EQUAL: case ZR_UPD765_SCAN_LOW: case ZR_UPD765_SCAN_HIGH:
        start_exec(f, now);
        break;
    default:                                    /* invalid */
        f->res[0] = ZR_ST0_IC_INVALID;
        enter_result(f, 1, ready, 0);
        break;
    }
}

/* --- the READY input -------------------------------------------------------------------------- */

/* With READY following the drives or the pins: a change of a drive's ready state is an interrupt
 * (IC = 11); the next change that is due by itself is a spindle reaching speed, polled at that time. */
static void poll_ready(zr_upd765 *f, uint64_t t) {
    uint64_t next = ZR_NEVER;
    if (f->ready_mode != ZR_UPD765_READY_TIED && !f->in_reset && f->t_rint == ZR_NEVER) {
        for (unsigned u = 0; u < 4; u++) {
            const uint8_t r = (uint8_t)ready_input(f, u, t);
            if (r != f->ready_prev[u]) {
                f->ready_prev[u] = r;
                f->rc_pending[u] = 1;
                f->int_status = 1;
            }
            const uint64_t at = (f->ready_mode == ZR_UPD765_READY_DRIVE && f->drive[u]) ? zr_fdd_rotating_at(f->drive[u]) : ZR_NEVER;
            if (at != ZR_NEVER && at > t && at < next) next = at;
        }
        update_int(f);
    }
    f->t_poll = next;
}

/* --- the bus interface ----------------------------------------------------------------------- */

static uint8_t read_msr(const zr_upd765 *f, uint64_t now) {
    if (f->in_reset) return 0;
    unsigned v = 0;
    for (unsigned u = 0; u < 4; u++)
        if (f->seeking[u]) v |= 1u << u;
    switch (f->phase) {
    case PH_IDLE:
        if (now >= f->rqm_at) v |= ZR_UPD765_RQM;
        break;
    case PH_CMD:
        v |= ZR_UPD765_CB;
        if (now >= f->rqm_at) v |= ZR_UPD765_RQM;
        break;
    case PH_EXEC:
        v |= ZR_UPD765_CB;
        if (f->nd) {
            v |= ZR_UPD765_EXM;
            if (f->x.pending) v |= ZR_UPD765_RQM;
            if (f->x.dir) v |= ZR_UPD765_DIO;
        }
        break;
    default:
        v |= ZR_UPD765_CB | ZR_UPD765_DIO;
        if (now >= f->rqm_at) v |= ZR_UPD765_RQM;
        break;
    }
    return (uint8_t)v;
}

static uint8_t read_data(zr_upd765 *f, uint64_t now) {
    if (f->in_reset) return 0xFF;
    if (f->phase == PH_RESULT) {
        const uint8_t v = f->res[f->res_pos];
        if (f->res_pos == 0 && f->int_cmd) {
            f->int_cmd = 0;
            update_int(f);
        }
        f->res_pos++;
        if (f->res_pos >= f->res_len) f->phase = PH_IDLE;
        f->rqm_at = now + us_ticks(f, RQM_DELAY_US);
        return v;
    }
    if (f->phase == PH_EXEC && f->nd && f->x.dir && f->x.pending) {
        const uint8_t v = f->x.dreg;
        unrequest(f);
        return v;
    }
    return 0xFF;
}

static void write_data(zr_upd765 *f, uint64_t now, uint8_t v) {
    if (f->in_reset) return;
    if (f->phase == PH_IDLE) {
        f->cmd[0] = v;
        f->cmd_len = (uint8_t)command_length(v & 0x1Fu);
        f->cmd_got = 1;
        f->phase = PH_CMD;
        f->rqm_at = now + us_ticks(f, RQM_DELAY_US);
    } else if (f->phase == PH_CMD) {
        f->cmd[f->cmd_got++] = v;
        f->rqm_at = now + us_ticks(f, RQM_DELAY_US);
    } else if (f->phase == PH_EXEC && f->nd && !f->x.dir && f->x.pending) {
        f->x.dreg = v;
        unrequest(f);
        return;
    } else return;
    if (f->cmd_got >= f->cmd_len) start_command(f, now);
}

static uint8_t chip_read(void *dev, unsigned space, uint32_t offset) {
    zr_upd765 *f = dev;
    const uint64_t now = now_of(f);
    (void)space;
    const uint8_t v = (offset & 1u) ? read_data(f, now) : read_msr(f, now);
    rearm(f);
    return v;
}

static void chip_write(void *dev, unsigned space, uint32_t offset, uint8_t value) {
    zr_upd765 *f = dev;
    (void)space;
    if (offset & 1u) write_data(f, now_of(f), value);
    rearm(f);
}

static void chip_event(void *dev, uint64_t now) {
    zr_upd765 *f = dev;
    f->armed = ZR_NEVER;                        /* the host has cleared the deadline that fired */
    for (;;) {
        uint64_t best = f->t_cmd;
        int which = 0;
        if (f->t_rint < best) { best = f->t_rint; which = 1; }
        if (f->t_poll < best) { best = f->t_poll; which = 2; }
        for (unsigned u = 0; u < 4; u++)
            if (f->t_seek[u] < best) { best = f->t_seek[u]; which = 3 + (int)u; }
        if (best == ZR_NEVER || best > now) break;
        if (which == 0) on_cmd_timer(f, best);
        else if (which == 1) {
            f->t_rint = ZR_NEVER;
            f->reset_n = f->reset_pos = 0;
            for (unsigned u = 0; u < 4; u++) {
                f->ready_prev[u] = (uint8_t)ready_input(f, u, best);
                if (f->ready_prev[u]) f->reset_list[f->reset_n++] = (uint8_t)u;
            }
            if (f->reset_n) {
                f->int_status = 1;
                update_int(f);
            }
            poll_ready(f, best);
        } else if (which == 2) poll_ready(f, best);
        else seek_event(f, (unsigned)(which - 3), best);
    }
    rearm(f);
}

static void chip_reset(void *dev) {
    zr_upd765 *f = dev;
    assert_reset(f);
    release_reset(f);
}

static void chip_input(void *dev, int pin, int state) {
    zr_upd765 *f = dev;
    if (pin == ZR_UPD765_PIN_TC) {
        f->tc_level = state != 0;
        if (state && f->phase == PH_EXEC) f->x.tc = 1;
    } else if (pin == ZR_UPD765_PIN_RESET) {
        if (state) assert_reset(f);
        else release_reset(f);
    } else if (pin >= ZR_UPD765_PIN_READY0 && pin < ZR_UPD765_PIN_READY0 + 4) {
        f->ready_pin[pin - ZR_UPD765_PIN_READY0] = state != 0;
        if (f->ready_mode == ZR_UPD765_READY_PINS) {
            poll_ready(f, now_of(f));
            zr_upd765_drive_changed(f);
        }
    }
}

static uint8_t chip_dack(void *dev, int channel, int to_device, uint8_t value, int tc) {
    zr_upd765 *f = dev;
    uint8_t r = 0xFF;
    (void)channel;
    if (f->phase == PH_EXEC && !f->nd) {
        zr_upd765_exec *x = &f->x;
        if (x->pending) {
            if (!to_device && x->dir) {
                r = x->dreg;
                unrequest(f);
            } else if (to_device && !x->dir) {
                x->dreg = value;
                unrequest(f);
            }
        }
        if (tc) x->tc = 1;
    }
    return r;
}

/* --- state -------------------------------------------------------------------------------------- */

static void chip_save(const void *dev, zr_state_writer *w) {
    const zr_upd765 *f = dev;
    const zr_upd765_exec *x = &f->x;
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u32(w, f->rate);
    zr_sw_u32(w, f->clk_khz);
    const uint8_t flags[] = {f->ready_mode, f->ready_pin[0], f->ready_pin[1], f->ready_pin[2], f->ready_pin[3], f->tc_level,
                             f->in_reset, f->phase, f->cmd_len, f->cmd_got, f->res_len, f->res_pos, f->line_int, f->line_drq,
                             f->int_cmd, f->int_status, f->srt, f->hut, f->hlt, f->nd, f->reset_n, f->reset_pos, f->head_loaded};
    zr_sw_bytes(w, flags, sizeof flags);
    zr_sw_bytes(w, f->cmd, sizeof f->cmd);
    zr_sw_bytes(w, f->res, sizeof f->res);
    zr_sw_u64(w, f->rqm_at);
    zr_sw_bytes(w, f->pcn, sizeof f->pcn);
    zr_sw_bytes(w, f->seeking, sizeof f->seeking);
    zr_sw_bytes(w, f->seek_recal, sizeof f->seek_recal);
    zr_sw_bytes(w, f->seek_hd, sizeof f->seek_hd);
    zr_sw_bytes(w, f->seek_target, sizeof f->seek_target);
    zr_sw_bytes(w, f->seek_rem, sizeof f->seek_rem);
    zr_sw_bytes(w, f->seek_pulses, sizeof f->seek_pulses);
    for (unsigned u = 0; u < 4; u++) zr_sw_u8(w, (uint8_t)f->seek_dir[u]);
    zr_sw_bytes(w, f->se_pending, sizeof f->se_pending);
    zr_sw_bytes(w, f->se_st0, sizeof f->se_st0);
    zr_sw_bytes(w, f->rc_pending, sizeof f->rc_pending);
    zr_sw_bytes(w, f->ready_prev, sizeof f->ready_prev);
    zr_sw_bytes(w, f->reset_list, sizeof f->reset_list);
    zr_sw_u64(w, f->head_until);
    zr_sw_u64(w, f->t_cmd);
    zr_sw_u64(w, f->t_rint);
    zr_sw_u64(w, f->t_poll);
    for (unsigned u = 0; u < 4; u++) zr_sw_u64(w, f->t_seek[u]);
    /* the command in progress */
    const uint8_t xf[] = {x->op, x->mt, x->mfm, x->sk, x->unit, x->head, x->c, x->h, x->r, x->n, x->eot, x->gpl, x->dtl, x->stp,
                          x->st0, x->st1, x->st2, x->fail_st1, x->stage, x->tc, x->dir, x->pending, x->dreg, x->stop, x->skip,
                          x->found, x->saw_id, x->crc_seen, x->slot_index, x->crc_err, x->scan_cmp, x->fmt_n, x->fmt_sc,
                          x->fmt_gpl, x->fmt_d};
    zr_sw_bytes(w, xf, sizeof xf);
    zr_sw_u32(w, x->bytepos); zr_sw_u32(w, x->size); zr_sw_u32(w, x->xfer); zr_sw_u32(w, x->unit_units);
    zr_sw_u64(w, x->num); zr_sw_u64(w, x->den);
    zr_sw_u64(w, x->id_t); zr_sw_u64(w, x->data_t); zr_sw_u64(w, x->end_t);
    zr_sw_u32(w, x->fmt_q); zr_sw_u64(w, x->fmt_t0);
    zr_sw_u32(w, x->rt_slot); zr_sw_u32(w, x->rt_count);
    zr_sw_bytes(w, x->buf, x->size > ZR_UPD765_BUFFER ? 0 : x->size);
    for (unsigned i = 0; i < x->fmt_sc; i++) {
        const uint8_t id[4] = {x->fmt_ids[i].c, x->fmt_ids[i].h, x->fmt_ids[i].r, x->fmt_ids[i].n};
        zr_sw_bytes(w, id, 4);
    }
}

static int chip_load(void *dev, zr_state_reader *r) {
    zr_upd765 *f = dev;
    if (zr_sr_u8(r) != STATE_VERSION) return 0;
    zr_upd765 in;
    memset(&in, 0, sizeof in);
    in.rate = zr_sr_u32(r);
    in.clk_khz = zr_sr_u32(r);
    uint8_t fl[23];
    zr_sr_bytes(r, fl, sizeof fl);
    in.ready_mode = fl[0];
    for (unsigned u = 0; u < 4; u++) in.ready_pin[u] = fl[1 + u];
    in.tc_level = fl[5]; in.in_reset = fl[6]; in.phase = fl[7]; in.cmd_len = fl[8]; in.cmd_got = fl[9]; in.res_len = fl[10];
    in.res_pos = fl[11]; in.line_int = fl[12]; in.line_drq = fl[13]; in.int_cmd = fl[14]; in.int_status = fl[15];
    in.srt = fl[16]; in.hut = fl[17]; in.hlt = fl[18]; in.nd = fl[19]; in.reset_n = fl[20]; in.reset_pos = fl[21]; in.head_loaded = fl[22];
    zr_sr_bytes(r, in.cmd, sizeof in.cmd);
    zr_sr_bytes(r, in.res, sizeof in.res);
    in.rqm_at = zr_sr_u64(r);
    zr_sr_bytes(r, in.pcn, sizeof in.pcn);
    zr_sr_bytes(r, in.seeking, sizeof in.seeking);
    zr_sr_bytes(r, in.seek_recal, sizeof in.seek_recal);
    zr_sr_bytes(r, in.seek_hd, sizeof in.seek_hd);
    zr_sr_bytes(r, in.seek_target, sizeof in.seek_target);
    zr_sr_bytes(r, in.seek_rem, sizeof in.seek_rem);
    zr_sr_bytes(r, in.seek_pulses, sizeof in.seek_pulses);
    for (unsigned u = 0; u < 4; u++) in.seek_dir[u] = (int8_t)zr_sr_u8(r);
    zr_sr_bytes(r, in.se_pending, sizeof in.se_pending);
    zr_sr_bytes(r, in.se_st0, sizeof in.se_st0);
    zr_sr_bytes(r, in.rc_pending, sizeof in.rc_pending);
    zr_sr_bytes(r, in.ready_prev, sizeof in.ready_prev);
    zr_sr_bytes(r, in.reset_list, sizeof in.reset_list);
    in.head_until = zr_sr_u64(r);
    in.t_cmd = zr_sr_u64(r);
    in.t_rint = zr_sr_u64(r);
    in.t_poll = zr_sr_u64(r);
    for (unsigned u = 0; u < 4; u++) in.t_seek[u] = zr_sr_u64(r);
    zr_upd765_exec *x = &in.x;
    uint8_t xf[35];
    zr_sr_bytes(r, xf, sizeof xf);
    x->op = xf[0]; x->mt = xf[1]; x->mfm = xf[2]; x->sk = xf[3]; x->unit = xf[4]; x->head = xf[5]; x->c = xf[6]; x->h = xf[7];
    x->r = xf[8]; x->n = xf[9]; x->eot = xf[10]; x->gpl = xf[11]; x->dtl = xf[12]; x->stp = xf[13]; x->st0 = xf[14];
    x->st1 = xf[15]; x->st2 = xf[16]; x->fail_st1 = xf[17]; x->stage = xf[18]; x->tc = xf[19]; x->dir = xf[20];
    x->pending = xf[21]; x->dreg = xf[22]; x->stop = xf[23]; x->skip = xf[24]; x->found = xf[25]; x->saw_id = xf[26];
    x->crc_seen = xf[27]; x->slot_index = xf[28]; x->crc_err = xf[29]; x->scan_cmp = xf[30]; x->fmt_n = xf[31];
    x->fmt_sc = xf[32]; x->fmt_gpl = xf[33]; x->fmt_d = xf[34];
    x->bytepos = zr_sr_u32(r); x->size = zr_sr_u32(r); x->xfer = zr_sr_u32(r); x->unit_units = zr_sr_u32(r);
    x->num = zr_sr_u64(r); x->den = zr_sr_u64(r);
    x->id_t = zr_sr_u64(r); x->data_t = zr_sr_u64(r); x->end_t = zr_sr_u64(r);
    x->fmt_q = zr_sr_u32(r); x->fmt_t0 = zr_sr_u64(r);
    x->rt_slot = zr_sr_u32(r); x->rt_count = zr_sr_u32(r);
    if (!zr_sr_ok(r) || x->size > ZR_UPD765_BUFFER || x->xfer > x->size || x->bytepos > x->size) return 0;
    zr_sr_bytes(r, x->buf, x->size);
    for (unsigned i = 0; i < x->fmt_sc; i++) {
        uint8_t id[4];
        zr_sr_bytes(r, id, 4);
        x->fmt_ids[i].c = id[0]; x->fmt_ids[i].h = id[1]; x->fmt_ids[i].r = id[2]; x->fmt_ids[i].n = id[3];
    }
    if (!zr_sr_ok(r)) return 0;
    if (in.ready_mode > ZR_UPD765_READY_PINS || in.phase > PH_RESULT || in.cmd_len > sizeof in.cmd || in.cmd_got > in.cmd_len ||
        in.res_len > sizeof in.res || in.res_pos > in.res_len || in.reset_n > 4 || in.reset_pos > in.reset_n || x->stage >= X_STAGES ||
        x->unit > 3 || x->head > 1 || (in.phase == PH_CMD && (in.cmd_got < 1 || in.cmd_got >= in.cmd_len)) ||
        x->fmt_q > 4u * x->fmt_sc || in.rate == 0)
        return 0;
    for (unsigned u = 0; u < 4; u++)
        if (in.seek_dir[u] != 1 && in.seek_dir[u] != -1 && in.seek_dir[u] != 0) return 0;
    if (x->den == 0) { x->den = 1; x->unit_units = 1; }
    /* what the board owns stays: the host, the clock, the drives */
    in.dev = f->dev;
    in.clock_hz = f->clock_hz;
    memcpy(in.drive, f->drive, sizeof in.drive);
    in.armed = f->armed;
    *f = in;
    f->x.fdd = f->drive[f->x.unit];            /* the drive of the command in progress (x pointed into `in`, which is gone) */
    for (unsigned u = 0; u < 4; u++)
        if (f->drive[u]) zr_fdd_set_rate(f->drive[u], f->rate);
    f->armed = ZR_NEVER - 1u;                   /* force the deadline to be armed again */
    rearm(f);
    return 1;
}

static const zr_dev_ops UPD765_OPS = {
    .read = chip_read, .write = chip_write, .event = chip_event, .reset = chip_reset, .input = chip_input,
    .dack = chip_dack, .save = chip_save, .load = chip_load,
};

void zr_upd765_init(zr_upd765 *f, const zr_hw_host *host, const char *name, uint32_t clock_hz) {
    memset(f, 0, sizeof *f);
    f->dev.ops = &UPD765_OPS;
    f->dev.host = host;
    f->dev.name = name ? name : "upd765";
    f->clock_hz = clock_hz ? clock_hz : 1u;
    f->rate = 250;
    f->clk_khz = 4000;
    f->ready_mode = ZR_UPD765_READY_TIED;
    f->t_cmd = f->t_rint = f->t_poll = f->armed = ZR_NEVER;
    for (unsigned u = 0; u < 4; u++) f->t_seek[u] = ZR_NEVER;
}

void zr_upd765_attach(zr_upd765 *f, unsigned unit, zr_fdd *drive) {
    if (unit >= 4) return;
    f->drive[unit] = drive;
    if (drive) zr_fdd_set_rate(drive, f->rate);
}

void zr_upd765_set_rate(zr_upd765 *f, unsigned kbit_per_s) {
    f->rate = kbit_per_s ? kbit_per_s : 250u;
    f->clk_khz = f->rate * 16u;
    for (unsigned u = 0; u < 4; u++)
        if (f->drive[u]) zr_fdd_set_rate(f->drive[u], f->rate);
}

void zr_upd765_set_clock(zr_upd765 *f, unsigned khz) { f->clk_khz = khz ? khz : 4000u; }

void zr_upd765_ready_wiring(zr_upd765 *f, int mode) {
    const uint64_t now = now_of(f);
    f->ready_mode = (uint8_t)(mode >= ZR_UPD765_READY_TIED && mode <= ZR_UPD765_READY_PINS ? mode : ZR_UPD765_READY_TIED);
    for (unsigned u = 0; u < 4; u++) f->ready_prev[u] = (uint8_t)ready_input(f, u, now);
    poll_ready(f, now);
    rearm(f);
}

void zr_upd765_drive_changed(zr_upd765 *f) {
    const uint64_t now = now_of(f);
    poll_ready(f, now);
    if (f->phase == PH_EXEC) {
        zr_upd765_exec *x = &f->x;
        const int idle = !(x->fdd && zr_fdd_rotating(x->fdd, now));
        if (x->stage == X_WAKE || (idle && (x->stage == X_FIND || x->stage == X_RT_IDX || x->stage == X_FMT_IDX))) begin_op(f, now);
    }
    rearm(f);
}
