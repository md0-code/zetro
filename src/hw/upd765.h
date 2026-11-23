/* ZetRo - the NEC uPD765A / Intel 8272A floppy disk controller, written from the data sheet. A chip as hw.h defines it: it talks to the board only
 * through the host callbacks (INT and DRQ out, TC, RESET and READY in) and
 * reaches its drives, plain structs (fdd.h), through pointers the board hands
 * it.
 *
 * Bus cycles: offset bit 0: 0 the main status register (read; a write is
 * ignored), 1 the data register, in whatever space the board maps the chip
 * into. Main status register: bit 7 RQM, 6 DIO (1 = controller to
 * processor), 5 EXM (execution phase in non-DMA mode), 4 CB (command in
 * progress), 3-0 drive n is seeking. RQM comes up a few microseconds (12 us)
 * after each data register access in the command and result phases, as the
 * data sheet's service interval says.
 *
 * Pins (state 1 is the logical assertion). Inputs, ops->input:
 * ZR_UPD765_PIN_TC the terminal count (a level: the chip ends a read or write
 * at the end of the sector that is passing, the way the data sheet has it;
 * also delivered by the ops->dack cycle that reaches it); ZR_UPD765_PIN_RESET
 * holds the chip in reset while 1: every state is cleared, the main status
 * register reads 0, and on the falling edge (release) the chip raises its
 * reset interrupt after 100 us; ZR_UPD765_PIN_READY0 + n the READY input of
 * unit n (used only in ZR_UPD765_READY_PINS mode). Outputs, host->line:
 * ZR_UPD765_PIN_INT, ZR_UPD765_PIN_DRQ. ops->reset is a pulse on RESET: reset
 * and release at once. zr_upd765_int and zr_upd765_drq read the levels (for
 * the board after a state load).
 *
 * With the Specify command's ND bit 0 (the default) every byte of the
 * execution phase is a DMA request: DRQ rises when a byte waits or is wanted
 * and falls on the ops->dack cycle (the DMA controller calls it with the
 * memory-write direction, to_device 0, for a read command; memory-read,
 * to_device 1, for a write, a format or a scan); `tc` of that cycle is the
 * terminal count. The channel number is ignored. During the execution phase
 * the main status register shows only CB, so that a polling program does not
 * take the bytes reserved for DMA. With ND 1 the data register is polled
 * through the main status register (RQM, EXM, DIO) with INT on every byte; a
 * byte the processor has not taken before the next arrives, or not supplied
 * in time on a write, is an overrun (ST1 OR).
 *
 * Commands (the low five bits of the first byte; bit 7 MT, 6 MFM, 5 SK where
 * the data sheet gives them): 02h Read a Track, 03h Specify, 04h Sense Drive
 * Status, 05h Write Data, 06h Read Data, 07h Recalibrate, 08h Sense Interrupt
 * Status, 09h Write Deleted Data, 0Ah Read ID, 0Ch Read Deleted Data, 0Dh
 * Format a Track, 0Fh Seek, 11h Scan Equal, 19h Scan Low or Equal, 1Dh Scan
 * High or Equal. Any other code is invalid: a result phase of one byte,
 * ST0 = 80h (the uPD765A has no Version command).
 *
 * Status registers: ST0 (IC, SE, EC, NR, HD, US), ST1 (EN end of cylinder, DE
 * CRC error, OR overrun, ND no data, NW not writable, MA missing address
 * mark), ST2 (CM control mark, DD CRC error in the data, WC wrong cylinder, BC
 * bad cylinder, SN, SH, MD missing data address mark), ST3 (FT, WP, RY, T0,
 * TS, HD, US). The error bits come from the sector as the disk image records
 * it (core/media/disk.h): a CRC error in the ID field makes the ID unusable
 * (the search goes on and ends with ND and DE); one in the data field is
 * reported when the sector has been transferred (DE, DD, abnormal
 * termination); a missing data address mark ends the command at once with MA
 * and MD; a deleted data mark is CM. FT (the drive never faults) is not raised.
 *
 * Execution. The controller reads and writes at the head's cylinder (the C
 * of a command is only compared with the disk's ID; there is no implied
 * seek) and finds sectors as the disk turns: it looks for an ID whose C, H,
 * R and N match, ID after ID, until the second index pulse after the start of
 * the search, then ends with ND (and MA when no ID field was seen at all; a
 * different C also sets WC). A track recorded in the other encoding than the
 * MFM bit asks for has no address marks to find. A data transfer takes a raw
 * byte time a byte (fdd.h lays the track out), twice that in FM. After each
 * sector: TC, or a skipped or control mark sector with SK 0, ends the command
 * normally; the sector number EOT ends it with EN (abnormal termination)
 * unless TC came, or, with MT on side 0, continues with sector 1 of side 1;
 * any other sector goes on to the next. The result's C, H, R, N are those of
 * the next sector as the data sheet's table has it, except after an error,
 * which leaves them at the sector that failed. A transfer ended by TC in the
 * middle of a sector lets the rest of the sector pass (a write fills the rest
 * of it with zeros). With N = 0, DTL is the number of bytes transferred (the
 * rest of the sector passes). Read a Track starts at the index, reads up to
 * EOT sectors in physical order with no ID comparison and does not stop at a
 * CRC error. Read ID gives the next ID that passes. Format a Track takes four
 * bytes (C, H, R, N) per sector from the host, one revolution after the
 * index it waits for, and writes the track on the disk (any layout the image
 * can hold; the disk module decides). Write commands on a write-protected
 * disk end at once with NW. Scan compares the sector's bytes with the host's
 * (FF in either is equal to anything), steps R by STP between sectors, and
 * ends when a sector satisfies it (SH if equal), at EOT (SN) or on TC.
 *
 * The drive: before a data command the head loads: the Specify HLT time
 * passes unless the head is still loaded from an earlier command (HUT after
 * its end). The times of Specify are those of the data sheet at the chip's
 * clock: SRT (16 - SRT) ms, HUT 16 ms a step (0 is 256), HLT 2 ms a step (0 is
 * 256) at 8 MHz, twice as long at 4 MHz. The clock follows the data rate (16
 * kHz for each kbit/s: 4 MHz at 250 kbit/s, 8 MHz at 500) unless the board sets it
 * (zr_upd765_set_clock). Seek and Recalibrate step the drive pulse by pulse
 * (Recalibrate gives up with EC after 77 pulses) and signal seek end with an
 * interrupt; the controller accepts commands for other drives meanwhile. A
 * drive that is not turning leaves a data command waiting (no index, no ID)
 * until it is. The READY input is wired high by default (the controller
 * sees every drive as ready); zr_upd765_ready_wiring selects the drives'
 * own READY (a disk turning at speed) or the READY pins, which give NR and
 * ready-change interrupts.
 *
 * Interrupts. INT rises at the start of a data command's result phase (and per
 * byte in non-DMA mode), falls on the first result byte read; at the end of a
 * seek or recalibrate (falls on Sense Interrupt Status, which returns ST0 and
 * PCN; one more result is then reported for each further finished drive); and
 * after a reset: one interrupt, then four Sense Interrupt Status results, ST0
 * = C0h, C1h, C2h, C3h (the ready lines of the four drives changing, IC =
 * 11), PCN 0, and 80h after; a program that reads only the first finds the
 * rest gone once any other interrupt is raised (a seek end, a data
 * command's result). With READY following the drives or the pins, only the
 * drives that are ready report, and later changes of a drive's ready state
 * interrupt the same way.
 *
 * Time: guest time (hw.h), the clock's rate given at init; one deadline
 * through host->schedule for the whole chip.
 *
 * Save and load: everything, including a command in progress, its sector
 * buffer and its timing; the drives are the board's to save (zr_fdd_save).
 * load makes no host call except arming the deadline.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_UPD765_H
#define ZR_UPD765_H

#include "hw.h"
#include "fdd.h"

/* Input pins (ops->input) and output pins (host->line). */
enum { ZR_UPD765_PIN_TC, ZR_UPD765_PIN_RESET, ZR_UPD765_PIN_READY0 };      /* READY0 .. READY0 + 3 */
enum { ZR_UPD765_PIN_INT, ZR_UPD765_PIN_DRQ };

/* Where the READY input comes from (zr_upd765_ready_wiring). */
enum { ZR_UPD765_READY_TIED, ZR_UPD765_READY_DRIVE, ZR_UPD765_READY_PINS };

/* Main status register bits. */
enum { ZR_UPD765_RQM = 0x80, ZR_UPD765_DIO = 0x40, ZR_UPD765_EXM = 0x20, ZR_UPD765_CB = 0x10 };

/* Command codes (the low five bits of the first byte). */
enum {
    ZR_UPD765_READ_TRACK = 0x02, ZR_UPD765_SPECIFY = 0x03, ZR_UPD765_SENSE_DRIVE = 0x04,
    ZR_UPD765_WRITE_DATA = 0x05, ZR_UPD765_READ_DATA = 0x06, ZR_UPD765_RECALIBRATE = 0x07,
    ZR_UPD765_SENSE_INT = 0x08, ZR_UPD765_WRITE_DELETED = 0x09, ZR_UPD765_READ_ID = 0x0A,
    ZR_UPD765_READ_DELETED = 0x0C, ZR_UPD765_FORMAT = 0x0D, ZR_UPD765_SEEK = 0x0F,
    ZR_UPD765_SCAN_EQUAL = 0x11, ZR_UPD765_SCAN_LOW = 0x19, ZR_UPD765_SCAN_HIGH = 0x1D
};
/* Command byte flags. */
enum { ZR_UPD765_MT = 0x80, ZR_UPD765_MFM = 0x40, ZR_UPD765_SK = 0x20 };

/* Status register bits. */
enum {
    ZR_ST0_IC_ABNORMAL = 0x40, ZR_ST0_IC_INVALID = 0x80, ZR_ST0_IC_READY = 0xC0,
    ZR_ST0_SE = 0x20, ZR_ST0_EC = 0x10, ZR_ST0_NR = 0x08, ZR_ST0_HD = 0x04,
    ZR_ST1_EN = 0x80, ZR_ST1_DE = 0x20, ZR_ST1_OR = 0x10, ZR_ST1_ND = 0x04, ZR_ST1_NW = 0x02, ZR_ST1_MA = 0x01,
    ZR_ST2_CM = 0x40, ZR_ST2_DD = 0x20, ZR_ST2_WC = 0x10, ZR_ST2_SH = 0x08, ZR_ST2_SN = 0x04, ZR_ST2_BC = 0x02, ZR_ST2_MD = 0x01,
    ZR_ST3_FT = 0x80, ZR_ST3_WP = 0x40, ZR_ST3_RY = 0x20, ZR_ST3_T0 = 0x10, ZR_ST3_TS = 0x08, ZR_ST3_HD = 0x04
};

#define ZR_UPD765_BUFFER 16384u         /* the largest sector (N = 7) */

/* The command being executed (read, write, format, read ID, read track, scan). */
typedef struct zr_upd765_exec {
    uint8_t op, mt, mfm, sk;
    uint8_t unit, head;
    zr_fdd *fdd;                            /* the unit's drive (not saved: the board attaches it) */
    uint8_t c, h, r, n, eot, gpl, dtl, stp; /* the ID registers and the parameters */
    uint8_t st0, st1, st2, fail_st1;
    uint8_t stage, tc, dir, pending, dreg, stop, skip, found, saw_id, crc_seen;
    uint8_t slot_index, crc_err, scan_cmp;
    uint32_t bytepos, size, xfer;           /* position in the sector, its length, bytes to transfer */
    uint32_t unit_units;                    /* units in a raw byte: 1 MFM, 2 FM */
    uint64_t num, den;                      /* guest time of `u` units after data_t: u * num / den */
    uint64_t id_t, data_t, end_t;           /* the end of the ID field, the first data byte, the end of the data field */
    uint8_t buf[ZR_UPD765_BUFFER];
    uint8_t fmt_n, fmt_sc, fmt_gpl, fmt_d;  /* format track parameters */
    unsigned fmt_q;
    uint64_t fmt_t0;
    zr_disk_id fmt_ids[256];
    unsigned rt_slot, rt_count;             /* read track: the physical slot, sectors read */
} zr_upd765_exec;

typedef struct zr_upd765 {
    zr_dev dev;
    zr_fdd *drive[4];           /* the drive each unit number reaches; NULL: none */
    uint32_t clock_hz;          /* the master clock */
    uint32_t rate;              /* the data rate, kbit/s (MFM terms) */
    uint32_t clk_khz;           /* the chip's own clock: sets the Specify times */
    uint8_t ready_mode;
    uint8_t ready_pin[4];
    uint8_t tc_level;
    uint8_t in_reset;
    uint8_t phase;
    uint8_t cmd[9], cmd_len, cmd_got;
    uint8_t res[8], res_len, res_pos;
    uint64_t rqm_at;            /* RQM is up from this time in the command and result phases */
    uint8_t line_int, line_drq;
    uint8_t int_cmd, int_status;
    uint8_t srt, hut, hlt, nd;  /* Specify */
    uint8_t pcn[4];
    uint8_t seeking[4], seek_recal[4], seek_hd[4], seek_target[4], seek_rem[4], seek_pulses[4];
    int8_t seek_dir[4];
    uint8_t se_pending[4], se_st0[4], rc_pending[4], ready_prev[4];
    uint8_t reset_list[4], reset_n, reset_pos;
    uint8_t head_loaded;
    uint64_t head_until;
    uint64_t t_cmd, t_rint, t_poll, t_seek[4], armed;
    zr_upd765_exec x;
} zr_upd765;

/* Fills the chip in its reset state on a master clock of `clock_hz`, 250 kbit/s, no drives; no host call
 * is made. The board then calls dev.ops->reset (or drives the RESET pin) to bring the lines in step. */
void zr_upd765_init(zr_upd765 *f, const zr_hw_host *host, const char *name, uint32_t clock_hz);
/* The drive that unit `unit` (0-3) reaches (it is given the data rate). */
void zr_upd765_attach(zr_upd765 *f, unsigned unit, zr_fdd *drive);
/* The data rate in kbit/s (250, 300, 500: MFM terms; an FM byte takes twice as long), which the board's
 * data separator setting gives: passed on to the drives, and the chip's clock follows it (see above). */
void zr_upd765_set_rate(zr_upd765 *f, unsigned kbit_per_s);
/* The chip's own clock in kHz, when it is not the rate's. */
void zr_upd765_set_clock(zr_upd765 *f, unsigned khz);
/* ZR_UPD765_READY_*: the default is the READY input wired high, as on most boards. */
void zr_upd765_ready_wiring(zr_upd765 *f, int mode);
/* A drive's motor, disk or spindle state changed: wakes a command waiting for the disk and reports
 * ready-line changes. Safe to call at any time. */
void zr_upd765_drive_changed(zr_upd765 *f);
/* The pins' levels now. */
int zr_upd765_int(const zr_upd765 *f);
int zr_upd765_drq(const zr_upd765 *f);

#endif /* ZR_UPD765_H */
