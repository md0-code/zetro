/* ZetRo - the Intel 8257 programmable DMA controller.
 *
 * Four channels, each with a 16-bit address register and a terminal count
 * register: bits 13-0 are the number of bytes to transfer minus one, bits
 * 15-14 the transfer type (00 verify, 01 write: the device's byte goes to
 * memory, 10 read: memory's byte goes to the device, 11 illegal). Written
 * from Intel's 8257 data sheet, and checked against this project's
 * cubz_dma.sv (the CUB Z's channel 2 serves the floppy controller).
 *
 * Bus: offset bits 3-0 (the address pins A3-A0) in whatever space the board
 * maps the chip into: 0 channel 0 address, 1 channel 0 count, 2-3 channel 1,
 * 4-5 channel 2, 6-7 channel 3, 8 the mode set register (write) and the
 * status register (read). The registers are 16 bits wide on an 8-bit bus: one
 * first/last flip-flop, shared by every register, says low byte (0, after
 * reset) or high byte (1) and toggles at every access of an address or count
 * register; it is cleared by reset and by writing the mode set register. A
 * read of the status register clears its flags. Other offsets read as FFh
 * and are ignored on a write.
 *
 * Mode set: bits 0-3 enable channels 0-3, bit 4 rotating priority (after a
 * cycle the channel served becomes the lowest; fixed: channel 0 highest),
 * bit 5 extended write (only the timing of the write strobes: no effect
 * here), bit 6 TC stop (a channel is disabled, its enable bit cleared, after
 * its terminal count), bit 7 auto load (channel 2's address and count are
 * reloaded from channel 3's at channel 2's terminal count and the channel goes
 * on, TC stop does not stop it; channel 3 then holds the reload values and
 * has no DRQ of its own).
 * Status: bits 0-3 the terminal count flags of channels 0-3 (set at a
 * channel's terminal count), bit 4 the update flag (set when channel 2 is
 * reloaded in auto load mode); a read clears all of them.
 *
 * Pins (hw.h). DRQ0-DRQ3 and HLDA come through ops->input (state 1 =
 * asserted = high). The chip reports HRQ (any enabled channel has its DRQ
 * up), TC (during the last cycle of a channel) and MARK (during the cycle
 * after which a multiple of 128 cycles remains: the low 7 bits of the count
 * register are all ones) through host->line (state 1 = high); the DACK
 * outputs are not pins: the device callback says which channel has its DACK.
 *
 * The transfers. The chip owns no bus: a cycle moves one byte between memory
 * (host->dma, in the memory space) and the device, through the callback the
 * board gives at init,
 *   uint8_t device(void *ctx, int channel, int to_device, uint8_t value, int tc)
 * which is how the board reaches the floppy controller's ops->dack (to_device
 * 1: `value`, read from memory, goes to the device; 0: the device returns the
 * byte, which goes to memory; `tc` is 1 in the channel's last cycle, the
 * 8257's TC output at the time of the strobe). Verify (type 00) and the
 * illegal type 11 count the cycle and move nothing: the controller makes no
 * memory or device strobe. The callback may lower the DRQ (the device is
 * served) by calling the chip's input through the board's wiring while the
 * cycle runs.
 *
 * Stealing the bus. The chip does not touch the CPU. The board watches HRQ
 * (the line pin, or zr_i8257_hrq): when it is up the board lets the CPU
 * finish its instruction (the Z80 gives the bus up between instructions in
 * this emulator), may report the grant with HLDA (the chip records it; a
 * cycle does not require it), and calls zr_i8257_cycle once for every DMA
 * cycle it grants while HRQ stays up. The call performs one cycle for the
 * highest-priority enabled channel with its DRQ up and returns the T-states
 * the cycle took (4 unless the board set another count with
 * zr_i8257_set_cycle; 0 when no channel is ready): the board adds them to
 * the CPU's clock (the CPU stands still) and to the guest time it moves the
 * other chips by. A device whose DRQ stays up gets a cycle for each call, so
 * the board loops until the HRQ drops or until it has given the CPU its share
 * of the time.
 *
 * Save and load: the whole state, so a load continues exactly; it makes no
 * host call, the board reads HRQ with zr_i8257_hrq. The callback is the
 * board's and is not saved.
 *
 * Portable C11. ZetRo's own code. */
#ifndef ZR_I8257_H
#define ZR_I8257_H

#include "hw.h"

/* Input pins (ops->input) and output pins (host->line). */
enum { ZR_I8257_IN_DRQ0, ZR_I8257_IN_DRQ1, ZR_I8257_IN_DRQ2, ZR_I8257_IN_DRQ3, ZR_I8257_IN_HLDA };
enum { ZR_I8257_OUT_HRQ, ZR_I8257_OUT_TC, ZR_I8257_OUT_MARK };

/* Mode set register bits, status register bits, transfer types. */
enum { ZR_I8257_ROTATE = 0x10, ZR_I8257_EXT_WRITE = 0x20, ZR_I8257_TC_STOP = 0x40, ZR_I8257_AUTOLOAD = 0x80 };
enum { ZR_I8257_ST_UPDATE = 0x10 };
enum { ZR_I8257_VERIFY = 0, ZR_I8257_WRITE = 1, ZR_I8257_READ = 2 };

typedef uint8_t (*zr_i8257_device_fn)(void *ctx, int channel, int to_device, uint8_t value, int tc);

typedef struct zr_i8257 {
    zr_dev dev;
    zr_i8257_device_fn device;
    void *device_ctx;
    uint16_t addr[4], count[4]; /* address registers; count registers with the transfer type in bits 15-14 */
    uint8_t mode, status;
    uint8_t ff;                 /* the first/last flip-flop: 1 the next access is the high byte */
    uint8_t drq, hlda;          /* DRQ0-3 (bits) and HLDA, 1 asserted */
    uint8_t last;               /* the channel served last (rotating priority) */
    uint8_t hrq_reported;
    uint8_t cycle_t;            /* T-states a DMA cycle takes */
} zr_i8257;

/* Sets the chip up on `host` with the device callback, as after reset; no
 * host call is made. The board then calls dev.ops->reset. */
void zr_i8257_init(zr_i8257 *d, const zr_hw_host *host, const char *name, zr_i8257_device_fn device, void *device_ctx);
/* T-states one DMA cycle takes (default 4; at least 1). */
void zr_i8257_set_cycle(zr_i8257 *d, unsigned tstates);
/* HRQ: some enabled channel has its DRQ up. */
int zr_i8257_hrq(const zr_i8257 *d);
/* One DMA cycle for the highest-priority channel that is enabled and has its
 * DRQ up. Returns the T-states it took, 0 when there is none. */
unsigned zr_i8257_cycle(zr_i8257 *d);

#endif /* ZR_I8257_H */
