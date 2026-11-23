/* ZetRo - an ATA (IDE) channel with up to two hard disks, written from the ATA-1 standard
 * (ANSI X3.221-1994: the task file, the status and error registers, the PIO protocols, the commands). A chip as hw.h
 * defines it for what the board does with its pins (INTRQ and the activity light out, RESET in); the DivIDE
 * (P8b) reaches it through the byte-wide registers below. The drives are zr_card images (core/media/card.h), used in
 * place: the card's contents are not part of a state.
 *
 * Registers. ops->read and ops->write take the offset in the chip's window (any space), and zr_ata_read and
 * zr_ata_write the same numbers: the eight registers of the task file (CS0 offsets 0 to 7) and the alternate status
 * and device control (CS1 offset 6, here 8). Other offsets are not connected: reads give FFh, writes are ignored.
 *   0 data (16 bits, see below)        4 cylinder low        8 read: alternate status (the status register
 *   1 read: error, write: features     5 cylinder high           without clearing INTRQ), write: device control
 *   2 sector count                     6 drive / head            (bit 1 nIEN, bit 2 SRST)
 *   3 sector number                    7 read: status (clears INTRQ), write: command
 * Both drives see every write to registers 1 to 6 (as on the bus; a drive that is busy ignores it); the drive/head
 * register's bit 4 chooses which drive answers reads from then on and executes a command; EXECUTE DEVICE DIAGNOSTIC
 * runs on both. Drive/head reads back with bits 7 and 5 set. A missing slave: with the master present the master
 * answers for it as ATA says, status 00h, and the other registers read 00h (the host sees no BSY and no DRDY);
 * with no drive at all the bus floats and everything reads FFh.
 *
 * The data register is 16 bits wide: two byte accesses give it, the low byte first and then the high byte, and the
 * word moves at the second. A flip-flop that every command write and every write to another register sets back to
 * the low byte (a read of another register does not) keeps the order, which is what a byte-wide interface such as
 * the DivIDE puts on the bus (it latches the odd byte itself; a board that does that calls zr_ata_read_data16 and
 * zr_ata_write_data16, one call a word). After SET FEATURES 01h (enable 8-bit transfers; 81h disables) the drive
 * moves one byte a transfer, bytes in the order of the sector, so 512 byte accesses or 512 calls of the 16-bit
 * functions (the byte in the low half) make a sector; the byte order of the stream is the same as the 16-bit
 * mode's. A halved HDF image gives zero high bytes. A read of the data register while there is no DRQ gives FFh and
 * a write is ignored.
 *
 * Status bits: BSY 80h, DRDY 40h, DWF 20h, DSC 10h, DRQ 08h, CORR 04h (never), IDX 02h (never: no index pulse is
 * modelled), ERR 01h. With BSY set the status reads 80h and nothing else. After a command that failed: DRDY, DSC and
 * ERR (DWF also for a write the card refused) and the error register: ABRT 04h (a command the drive does not do, a
 * bad parameter, a write to a protected image), IDNF 10h (an address outside the drive or its current geometry, or
 * a sector the transfer ran into the end of), UNC 40h (the image could not be read); the address registers then
 * hold the sector that failed and the sector count the sectors still to do.
 *
 * Commands (a drive that is busy ignores a command written; one written during a transfer abandons it):
 *   20h, 21h READ SECTORS        30h, 31h WRITE SECTORS      40h, 41h READ VERIFY SECTORS (the address range is checked;
 *   70h-7Fh SEEK                 10h-1Fh RECALIBRATE          the sectors are not read)
 *   90h EXECUTE DEVICE DIAGNOSTIC (the signature in the registers of both drives, error 01h)
 *   91h INITIALIZE DEVICE PARAMETERS (sectors per track from the sector count, heads from the head register: the
 *       geometry CHS addresses are translated by, with the cylinders the image holds in it; IDENTIFY words 54 to 58
 *       follow it; a reset or a software reset puts the default back)
 *   ECh IDENTIFY DEVICE (core/media/card.h gives the data: the HDF header's, or built from the size)
 *   EFh SET FEATURES: 01h and 81h 8-bit transfers, 02h and 82h write cache, 03h set transfer mode (PIO modes
 *       only: sector count 00h, 01h or 08h-0Ch), 05h and 85h power management, 55h and AAh read look-ahead, 66h and
 *       CCh reverting to defaults, BBh and 44h ECC bytes; the others abort
 *   E0h-E3h STANDBY and IDLE, E6h SLEEP, E7h FLUSH CACHE and 94h-97h, 99h: succeed and do nothing (E7h flushes the
 *       image's file); E5h and 98h CHECK POWER MODE: the sector count is FFh (active)
 *   Anything else, including READ and WRITE MULTIPLE, the DMA commands and the LBA48 ones: aborted (ABRT).
 * Addressing: bit 6 of drive/head chooses LBA (28 bits: head bits 3-0, cylinder high, low, sector number) or CHS
 * (cylinder = high, low; head; sector 1 to the sectors per track). A sector count of 0 is 256 sectors. After a
 * sector is moved the address registers show the next sector, or the last one at the end of the command, and the sector
 * count the sectors left.
 *
 * Interrupts and the protocol: INTRQ is raised when a block of a data-in command is ready (with DRQ), when a
 * written sector has been taken (with DRQ for the next, or at the end), and at the end of a command that moves no
 * data or fails; the first block of a WRITE has no interrupt (DRQ comes at once). It falls on a status read or a
 * command write. The pin follows the selected drive's interrupt only while nIEN is 0 (the line is released, so
 * off, while nIEN is 1) and is off for an absent drive. After a READ the next sector is busy for a moment, as after
 * a WRITE.
 *
 * Reset. The SRST bit of device control, or RESET input pin (ZR_ATA_PIN_RESET, state 1 asserted), and ops->reset
 * (a pulse, also the power-on state): while it is held both drives show BSY, 80h, and forget what they were
 * doing (the geometry, 8-bit mode and interrupts go back to their defaults); when it is released the master is
 * selected and after ZR_ATA_RESET_US both drives become ready with the diagnostic signature: error 01h, sector count
 * 1, sector number 1, cylinder 0, drive/head A0h or B0h, status 50h; no interrupt.
 *
 * Time is nominal, in guest time (hw.h) at the clock rate given at init, one deadline through host->schedule for
 * the chip: a command is BSY for ZR_ATA_COMMAND_US before it runs (so a program that polls the status sees BSY at
 * least once), the next sector of a multi-sector transfer for ZR_ATA_SECTOR_US, and the reset for
 * ZR_ATA_RESET_US. Seek, rotation and transfer speed of a real disk are not modelled.
 *
 * Pins. Input (ops->input): ZR_ATA_PIN_RESET. Output (host->line): ZR_ATA_PIN_INTRQ (as described), ZR_ATA_PIN_LIGHT
 * (1 while a command is executing or a transfer is under way, for a drive light); zr_ata_intrq and
 * zr_ata_light read the levels (for the board after a state load) and zr_ata_activity counts the sectors a drive has
 * moved (for a light that flashes on an access; not saved).
 *
 * Save and load: everything of the channel and of both drives, a transfer in progress included, its sector buffer
 * and its deadline; the cards are the board's (the same ones must be attached as when it was saved, or load
 * refuses). load makes no host call except arming the deadline.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_ATA_H
#define ZR_ATA_H

#include "hw.h"
#include "media/card.h"

/* Input and output pins. */
enum { ZR_ATA_PIN_RESET };
enum { ZR_ATA_PIN_INTRQ, ZR_ATA_PIN_LIGHT };

/* Register offsets (ops->read, ops->write). */
enum {
    ZR_ATA_DATA = 0, ZR_ATA_ERROR = 1, ZR_ATA_FEATURES = 1, ZR_ATA_COUNT = 2, ZR_ATA_SECTOR = 3, ZR_ATA_CYL_LO = 4,
    ZR_ATA_CYL_HI = 5, ZR_ATA_HEAD = 6, ZR_ATA_STATUS = 7, ZR_ATA_COMMAND = 7, ZR_ATA_ALT_STATUS = 8, ZR_ATA_DEVCTL = 8
};

enum {
    ZR_ATA_BSY = 0x80, ZR_ATA_DRDY = 0x40, ZR_ATA_DWF = 0x20, ZR_ATA_DSC = 0x10, ZR_ATA_DRQ = 0x08, ZR_ATA_CORR = 0x04,
    ZR_ATA_IDX = 0x02, ZR_ATA_ERR = 0x01
};
enum { ZR_ATA_E_AMNF = 0x01, ZR_ATA_E_TK0NF = 0x02, ZR_ATA_E_ABRT = 0x04, ZR_ATA_E_MCR = 0x08, ZR_ATA_E_IDNF = 0x10, ZR_ATA_E_MC = 0x20,
       ZR_ATA_E_UNC = 0x40, ZR_ATA_E_BBK = 0x80 };
/* Device control and drive/head bits. */
enum { ZR_ATA_NIEN = 0x02, ZR_ATA_SRST = 0x04, ZR_ATA_HEAD_LBA = 0x40, ZR_ATA_HEAD_DRV = 0x10 };

/* Commands. */
enum {
    ZR_ATA_CMD_RECALIBRATE = 0x10, ZR_ATA_CMD_READ = 0x20, ZR_ATA_CMD_READ_NR = 0x21, ZR_ATA_CMD_WRITE = 0x30, ZR_ATA_CMD_WRITE_NR = 0x31,
    ZR_ATA_CMD_VERIFY = 0x40, ZR_ATA_CMD_VERIFY_NR = 0x41, ZR_ATA_CMD_SEEK = 0x70, ZR_ATA_CMD_DIAGNOSTIC = 0x90,
    ZR_ATA_CMD_INIT_PARAMS = 0x91, ZR_ATA_CMD_CHECK_POWER_OLD = 0x98, ZR_ATA_CMD_STANDBY_IMM = 0xE0, ZR_ATA_CMD_IDLE_IMM = 0xE1,
    ZR_ATA_CMD_STANDBY = 0xE2, ZR_ATA_CMD_IDLE = 0xE3, ZR_ATA_CMD_CHECK_POWER = 0xE5, ZR_ATA_CMD_SLEEP = 0xE6,
    ZR_ATA_CMD_FLUSH = 0xE7, ZR_ATA_CMD_IDENTIFY = 0xEC, ZR_ATA_CMD_SET_FEATURES = 0xEF
};

/* Nominal times, microseconds of guest time. */
enum { ZR_ATA_COMMAND_US = 20, ZR_ATA_SECTOR_US = 8, ZR_ATA_RESET_US = 50 };

/* What a drive is doing. */
enum { ZR_ATA_IDLE, ZR_ATA_BUSY, ZR_ATA_DATA_IN, ZR_ATA_DATA_OUT, ZR_ATA_RESETTING };

typedef struct zr_ata_drive {
    zr_card *card;                      /* the image; NULL: no drive (not saved: the board attaches it) */
    uint8_t error, features, count, sector, cyl_lo, cyl_hi, head, status;
    uint8_t phase;                      /* ZR_ATA_IDLE ... */
    uint8_t stage;                      /* while busy: what happens when the deadline comes */
    uint8_t cmd;                        /* the command in progress */
    uint8_t intr;                       /* the drive's interrupt request */
    uint8_t eight_bit;                  /* SET FEATURES 01h */
    unsigned cur_cyl, cur_heads, cur_spt;   /* the geometry CHS addresses are translated by */
    uint64_t lba, limit;                /* the sector being moved, and the first address beyond the command's range */
    unsigned left;                      /* sectors still to move, the current one included */
    unsigned pos;                       /* the next byte of the sector buffer */
    uint64_t due;                       /* the deadline of the busy phase */
    uint32_t accesses;                  /* sectors moved (not saved) */
    uint8_t buf[ZR_CARD_SECTOR];
} zr_ata_drive;

typedef struct zr_ata {
    zr_dev dev;
    uint32_t clock_hz;
    uint64_t t_command, t_sector, t_reset;  /* the nominal times, in master clock periods */
    zr_ata_drive drive[2];
    uint8_t selected;                   /* the drive that answers */
    uint8_t devctl;                     /* nIEN and SRST as written */
    uint8_t pin_reset;                  /* the RESET pin */
    uint8_t flip;                       /* the data register's byte flip-flop: 1 the high byte is next */
    uint8_t line_intrq, line_light;
    uint64_t armed;
} zr_ata;

/* Fills the channel with no drives on a master clock of `clock_hz`; no host call is made. The board then
 * calls dev.ops->reset to bring the lines in step. */
void zr_ata_init(zr_ata *a, const zr_hw_host *host, const char *name, uint32_t clock_hz);
/* The image of drive `unit` (0 master, 1 slave); NULL: the drive is absent. The drive starts as after a reset, ready,
 * and the card must outlive its attachment (the board closes it after attaching NULL). */
void zr_ata_attach(zr_ata *a, unsigned unit, zr_card *card);

/* Byte-wide register access (see above); the same as ops->read and ops->write. */
uint8_t zr_ata_read(zr_ata *a, unsigned reg);
void zr_ata_write(zr_ata *a, unsigned reg, uint8_t value);
/* The data register a word at a time, for a board that latches the odd byte itself. */
uint16_t zr_ata_read_data16(zr_ata *a);
void zr_ata_write_data16(zr_ata *a, uint16_t value);

int zr_ata_intrq(const zr_ata *a);
int zr_ata_light(const zr_ata *a);
uint32_t zr_ata_activity(const zr_ata *a, unsigned unit);

#endif /* ZR_ATA_H */
