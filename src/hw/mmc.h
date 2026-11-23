/* ZetRo - an SD or MMC card in SPI mode, written from the SD Physical Layer Simplified
 * Specification (the SPI mode chapter, the CSD and CID registers, the commands and responses) and the MMC
 * specification's CMD1 initialisation. The card is a zr_card image (core/media/card.h), used in place: the card's
 * contents are not part of a state. A chip as hw.h defines it for its pins; the DivMMC (P8b) reaches it through the
 * byte exchange below, as the SPI bus does: the master clocks a byte out and a byte in at the same time.
 *
 * The bus. zr_mmc_exchange(m, out) is one byte of the bus: `out` is what the master sends, the return value what the
 * card sends in the same eight clocks (FFh when there is no card, the card is not selected or has nothing to say);
 * ops->write at offset 0 is the same call and ops->read at offset 0 gives the byte the last exchange returned. The
 * chip select is zr_mmc_select (the input pin ZR_MMC_PIN_CS, state 1 selected, is the same): a byte exchanged while
 * the card is not selected does nothing, and deselecting drops what the card was doing, a command received in
 * part, a data block being read or written, a multiple block stream. A card that was busy programming has done it
 * already (a block is written to the image when its last byte arrives), so its busy bytes end at once; the
 * specification has the busy signal resume when the card is selected again.
 *
 * Commands (a frame is 6 bytes: 40h or the command, 4 argument bytes, CRC7 and the stop bit; bytes of other value
 * than 01xxxxxx are not a start and are ignored, and a start byte begins a command at any time the card is not
 * receiving the data of a block, which is how CMD12 reaches a card that has begun to send the next block):
 *   CMD0 GO_IDLE_STATE   CMD1 SEND_OP_COND (MMC only)   CMD8 SEND_IF_COND (SD; echoes the check pattern for the
 *   2.7-3.6 V range, R7)   CMD9 SEND_CSD   CMD10 SEND_CID   CMD12 STOP_TRANSMISSION (R1b)   CMD13 SEND_STATUS (R2)
 *   CMD16 SET_BLOCKLEN (512 only, others are a parameter error)   CMD17 READ_SINGLE_BLOCK   CMD18
 *   READ_MULTIPLE_BLOCK   CMD24 WRITE_BLOCK   CMD25 WRITE_MULTIPLE_BLOCK   CMD55 APP_CMD   CMD58 READ_OCR (R3)
 *   CMD59 CRC_ON_OFF; after CMD55: ACMD13 SD_STATUS (R2 and 64 bytes of zero: no speed class, no protected area),
 *   ACMD22 SEND_NUM_WR_BLOCKS, ACMD23 SET_WR_BLK_ERASE_COUNT (accepted and remembered, it only prepares an erase), ACMD41
 *   SD_SEND_OP_COND (HCS in bit 30), ACMD51 SEND_SCR (SD specification 2.00, 1-bit and 4-bit buses). Anything else,
 *   erase, lock, CMD23 included, is an illegal command; so is a read or write command in the idle state.
 * Responses: R1 (bit 0 idle, 2 illegal command, 3 CRC error, 5 address error, 6 parameter error) after one FFh byte
 * (the card answers in one byte time), R1b with busy bytes (00h), R2 (R1 and the status byte: bit 5 write protect
 * violation, bit 7 out of range, cleared when read), R3 and R7 (R1 and four bytes). A data block is FFh, the token FEh,
 * the bytes and CRC16 (always valid); a read that cannot be done sends an error token (01h error, 08h out of range) instead;
 * a block written is FEh (FCh in a multiple block write; FDh stops it), the bytes and the CRC16, answered by the data
 * response token (05h accepted, 0Bh CRC error, 0Dh write error: a protected image, an address beyond the card or a
 * file that fails) and then busy, ZR_MMC_BUSY_BYTES of 00h after a block and 2 after CMD12 and the stop token.
 * Multiple block reads run to the end of the card, where an out-of-range error token ends the stream.
 * CRC: the CRC7 of a command is checked for CMD0 and CMD8 always (after CMD0 a card is back to its default) and for all commands
 * after CMD59 switches it on, the CRC16 of a written block likewise; a bad command CRC answers R1 with bit 3 and does
 * nothing, a bad block CRC is token 0Bh and is not written.
 *
 * Types. zr_mmc_attach chooses it from the image: ZR_MMC_SD, a standard capacity card (SDSC, CSD version 1.0)
 * up to 2 GiB (4194304 sectors) and a high capacity one (SDHC, CSD 2.0, block addressing: the argument of a
 * read or write is the block number) above; ZR_MMC_MMC, a version 4 MMC card of up to 2 GiB with byte addressing, which
 * answers CMD1 and refuses CMD8, CMD55 and the application commands (as a real MMC card does and an SD card
 * refuses CMD1, which is how a driver tells them apart). A standard capacity card takes byte addresses, which must
 * be multiples of 512 (else R1 bit 5). The capacity the CSD can say is a multiple of 2^(C_SIZE_MULT+2) blocks (SDSC) or
 * of 1024 sectors (SDHC): a tail of the image beyond it is not reachable. A card above 1 GiB says
 * READ_BL_LEN 10 in its CSD, as such cards did, and is still read and written in 512-byte blocks.
 * Initialisation: after CMD0 the card is idle; ACMD41 (CMD1 for MMC) leaves idle at its third call; an SDHC card
 * leaves it only if CMD8 was answered and the host set HCS (as a real one). CMD58 reads 80FF8000h (ready, 2.7-3.6 V)
 * plus bit 30 for an SDHC card, with only the voltage window while idle.
 * The CSD tells the capacity of the image and TMP_WRITE_PROTECT when it is read-only; the CID has the manufacturer
 * 7Ah, the application "ZR", the product "ZETRO", revision 1.0, a serial number from the size and the date 2026-11.
 *
 * Time. There is none: every answer is in byte counts (the busy time of a write is bytes, not microseconds); the chip never
 * schedules anything. The card is as fast as the host clocks it.
 *
 * Pins. Input (ops->input): ZR_MMC_PIN_CS. Output (host->line): ZR_MMC_PIN_LIGHT, 1 while a block is being read
 * or written (a multiple block stream until it is stopped). zr_mmc_light reads it and zr_mmc_activity counts the blocks
 * moved, for a light that flashes on an access (not saved).
 *
 * Save and load: the protocol state, a command or block in progress, with the bytes still to send or those received so
 * far; the card is the board's (the same image must be attached with the same type, or load refuses). No host
 * call is made on load: the board reads zr_mmc_light.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_MMC_H
#define ZR_MMC_H

#include "hw.h"
#include "media/card.h"

enum { ZR_MMC_PIN_CS };                 /* input: 1 the card is selected (the wire is low) */
enum { ZR_MMC_PIN_LIGHT };              /* output */

enum { ZR_MMC_SD, ZR_MMC_MMC };

#define ZR_MMC_BUSY_BYTES 8u            /* busy bytes after a block written */
#define ZR_MMC_TX 520u                  /* the longest answer: FF, R1, FF, token, 512 bytes, CRC */

typedef struct zr_mmc {
    zr_dev dev;
    zr_card *card;                      /* NULL: no card (not saved) */
    uint8_t type;                       /* ZR_MMC_SD or ZR_MMC_MMC */
    uint8_t hc;                         /* block addressing (SDHC) */
    uint64_t blocks;                    /* the sectors the card's registers say it has */
    uint8_t selected, idle, crc_on, v2, app, state, cmd_got, multi, light, last;
    uint8_t status2;                    /* the card status byte R2 carries */
    uint8_t cmd[6];
    uint32_t polls;                     /* initialisation commands since CMD0 */
    uint32_t wr_blocks, erase_count;
    uint64_t addr;                      /* the next block of a multiple block transfer */
    unsigned tx_len, tx_pos, rx_got;
    uint8_t tx[ZR_MMC_TX];
    uint8_t rx[ZR_CARD_SECTOR + 2];
    uint32_t activity;                  /* blocks moved (not saved) */
} zr_mmc;

/* Fills the card slot empty; no host call is made. The board then calls dev.ops->reset. */
void zr_mmc_init(zr_mmc *m, const zr_hw_host *host, const char *name);
/* Puts an image in the slot as an SD or MMC card (see above) and powers it up. 0 with the reason when the image does not
 * suit the type (an MMC card above 2 GiB) or is too small (under 8 sectors). */
int zr_mmc_attach(zr_mmc *m, zr_card *card, int type, char *error, size_t capacity);
/* Takes the card out; the board closes it afterwards. */
void zr_mmc_detach(zr_mmc *m);

void zr_mmc_select(zr_mmc *m, int selected);
uint8_t zr_mmc_exchange(zr_mmc *m, uint8_t out);

int zr_mmc_light(const zr_mmc *m);
uint32_t zr_mmc_activity(const zr_mmc *m);
/* The card's registers as the commands give them (for checks and the setup screen). */
void zr_mmc_csd(const zr_mmc *m, uint8_t out[16]);
void zr_mmc_cid(const zr_mmc *m, uint8_t out[16]);

#endif /* ZR_MMC_H */
