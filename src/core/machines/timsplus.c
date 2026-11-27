/* The Timisoara TIM-S Plus (machine.h, machine_impl.h): the second model of the family "tims" (tims.c dispatches to this
 * board), an extended ZX Spectrum +3-compatible on the shared core (spectrum.h). From the TIM-S Plus manual (vol. III,
 * chapter 8: the ports and their bits; MiSTerRO/docs/tims/reference/bundled-plus-manual.pdf), this project's TIM-S core
 * (MiSTerRO/docs/tims: README, hardware.md, validation.md, provenance.md; core-tims/rtl/machine_core.sv,
 * core-tims/assets/roms/plus-loader.asm, shared/romanian/rtl/rp_tims_video.sv, rp_i8255.sv, rp_i8253.sv, rp_i8251.sv,
 * rp_spectrum_keyboard.sv, rp_keys.sv; shared/romanian/tools/diagnostics.py) and the +3 system ROM's published
 * hardware description. Where the core and the manual differ the manual is followed:
 *   - the Z80 on the family's 168 MHz master clock (tims.h): 3.5 MHz, or the auxiliary clock of the "CPU switch" while
 *     the first 8255's PC6 is high;
 *   - a 2 KiB bootstrap EPROM (the replacement loader: the original is not archived) over 0000h-3FFFh while PC0 is low,
 *     twelve 16 KiB RAM banks (the blocks BR0 and BR1 of the manual, the +3's eight pages, and BR2, the four banks the
 *     +3 system lives in, written from the system ROM at every reset), 64 KiB of video RAM (BR3) as eight 8 KiB pages;
 *   - the paging of the +3 through 7FFDh and 1FFDh (bit 5 of 7FFDh is not used: no lock) with PC0 (the EPROM), PC1
 *     (BR2 write-protected at 0000h-3FFFh), PC2 (with 7FFDh's bit 2 the block at C000h-FFFFh: BR0, BR1, BR2 or the video
 *     RAM) and PC4 (Spectrum or CP/M mode) of the first 8255; the CPU's writes to 4000h-5FFFh also go into the video
 *     page PA6, PA5 and 7FFDh's bit 3 select while the second 8255's PC2 is high;
 *   - the video (hw/timsvideo.h): 256, 320, 512 or 640 by 192, the page PA4, PA3, 7FFDh's bit 3, the attribute inhibit
 *     PA0, the hardware scroll in the second 8255's port A, FLASH from the 8253's counter 1;
 *   - the ports: low byte FDh with the high byte selecting (the first 8255 at 0CFD-0FFDh, the second at 5CFD-5FFDh, the
 *     8253s at 9CFD-9FFDh and 8CFD-8FFDh, the native 8251 at DEFDh/DFFDh, the serial multiplexer at AFFDh, the expansion
 *     8251 it selects at CFFDh/EFFDh, the 8272 at 2FFDh/3FFDh, the AY at FFFDh/BFFDh), port FE on A0 low (the keyboard
 *     with the two added lines on D5 and D7; the border, MIC, the speaker), MICRONEXT at xx0Fh, xx1Fh, xx2Fh (the
 *     address) and xx3Fh (the data), the network at F7h (data) and F3h (arms its NMI);
 *   - the printer: data in the first 8255's port B (0FFDh), BUSY on its bit 0 read, the strobe 1FFDh's bit 4;
 *   - the 8272 with two 5.25" drives, the motor 1FFDh's bit 3, its interrupt read on the second 8255's PC4;
 *   - five 8251 channels clocked by the 8253s' outputs; the one the "UART channel" option names is the serial port;
 *   - the AY-3-8912 in stereo (A left, C right) or mono; a tape recorder; the NMI key; MICRONEXT's 4 MiB.
 * ZetRo's own code. */
#include "tims.h"
#include "drivesnd.h"
#include "fdd.h"
#include "i8251.h"
#include "i8253.h"
#include "i8255.h"
#include "media/disk.h"
#include "media/snap.h"
#include "text.h"
#include "timsvideo.h"
#include "upd765.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_VERSION 1u
#define PAGE 0x4000u
#define BANKS 12u                               /* 16 KiB RAM banks: BR0, BR1, BR2 */
#define EPROM 0x0800u                           /* the bootstrap */
#define SYSTEM EPROM                            /* the +3 system's 64 KiB in s->rom, after the bootstrap */
#define MICRONEXT (4u << 20)                    /* bytes of MICRONEXT */
#define PIT_TICKS (TIMS_CLOCK_HZ / 2000000u)    /* the 8253s' 2 MHz: 84 ticks */
#define FLASH_TICKS (TIMS_CLOCK_HZ / 109375u)   /* counter 1's 109.375 kHz (X7): 1536 ticks */
#define DOT_TICKS (TIMS_CLOCK_HZ / 14000000u)   /* the video's 14 MHz dot: 12 ticks */

typedef struct timsplus timsplus;
typedef struct uart_ctx {
    timsplus *t;
    unsigned index;
} uart_ctx;

struct timsplus {
    zr_spectrum s;                              /* first: the shared core */
    zr_hw_host chip_host;                       /* the board's chips: guest time, the scheduler, their pins */
    zr_hw_host ula_host;                        /* the shared ULA's, its interrupt not wired (the video's is the machine's) */
    zr_timsvideo video;
    zr_i8255 ppi1, ppi2;
    zr_i8253 pit, mux_pit;
    zr_i8251 uart[TIMS_UARTS];
    uart_ctx uctx[TIMS_UARTS];
    zr_upd765 fdc;
    zr_fdd fdd[2];
    zr_drivesnd drivesnd;
    /* the options */
    uint8_t cpu_switch, channel, physical_printer;
    /* the board's latches (saved) */
    uint8_t page7, page1;                       /* the last bytes written to 7FFDh and 1FFDh */
    uint8_t serial_mux;                         /* AFFDh */
    uint32_t mn_address;                        /* MICRONEXT's address latches (xx0Fh, xx1Fh, xx2Fh) */
    uint8_t network, armed;                     /* the network line written at F7h; F3h armed the NMI */
    uint8_t out1;                               /* the 8253's OUT1 as last reported */
    uint8_t fdc_int;                            /* the 8272's INT */
    /* what the 8255s' pins are now (derived) */
    uint8_t ctl, ctl2, pa1, mirror;
    uint32_t mirror_base;                       /* the video page the CPU's writes to 4000h-5FFFh also go to */
    uint32_t uart_num[TIMS_UARTS];              /* the 8251s' clocks, master ticks per edge (0: stopped) */
    /* memory */
    uint8_t vram[ZR_TIMSVIDEO_VRAM];
    uint8_t *micronext;                         /* allocated at the first write */
    /* the keyboard's added keys held */
    unsigned extra;
    /* the disks */
    zr_media *disk_media[2];
    zr_disk *disk[2];
    /* the host's end of the ports */
    zr_port_host ports;
    int has_ports, loopback;
    uint8_t host_cts, host_dsr, printer_online;
    /* the shared core's decoder of port FE, which the board wraps */
    zr_bus_read_fn keys_read;
    zr_bus_write_fn latch_write;
};

static timsplus *of(void *self) { return self; }

/* --- the 8255s' pins --------------------------------------------------------------------------------------------- */

/* The first 8255's port C as the board sees it: the pins it drives, and where it drives none PC0 low (the manual: R19 to
 * ground, so that the EPROM is in after a reset) and the others high. */
static uint8_t control1(const timsplus *t) {
    const uint8_t drv = zr_i8255_driven(&t->ppi1, ZR_I8255_PORT_C);
    return (uint8_t)((zr_i8255_out(&t->ppi1, ZR_I8255_PORT_C) & drv) | (~drv & 0xFEu));
}
/* The second 8255's port C: undriven pins high. */
static uint8_t control2(const timsplus *t) {
    const uint8_t drv = zr_i8255_driven(&t->ppi2, ZR_I8255_PORT_C);
    return (uint8_t)((zr_i8255_out(&t->ppi2, ZR_I8255_PORT_C) & drv) | (uint8_t)~drv);
}
static int spectrum_mode(const timsplus *t) { return (t->ctl >> 4 & 1u) != 0; }
static uint64_t now_of(const timsplus *t) { return zr_spectrum_now(&t->s); }

/* --- the memory map ---------------------------------------------------------------------------------------------- */

/* The +3's all-RAM configurations (1FFDh bit 0 set): the banks at 0000h, 4000h, 8000h and C000h by bits 1 and 2. */
static const uint8_t ALL_RAM[4][4] = {{0, 1, 2, 3}, {4, 5, 6, 7}, {4, 5, 6, 3}, {4, 7, 6, 3}};

/* The memory as 7FFDh, 1FFDh and the 8255s leave it (machine_core.sv's bank and direct_video, the manual's 8.1). */
static void remap(timsplus *t) {
    zr_spectrum *s = &t->s;
    zr_bus *b = &s->bus;
    const unsigned c0 = t->ctl & 1u, c1 = t->ctl >> 1 & 1u, c2 = t->ctl >> 2 & 1u;
    const unsigned p72 = t->page7 >> 2 & 1u, p74 = t->page7 >> 4 & 1u;
    /* The video page of the CPU's accesses: PA6, PA5, and 7FFDh's bit 3 as A13. */
    const uint32_t cpu_video = (uint32_t)(t->pa1 >> 5 & 3u) << 14 | (uint32_t)(t->page7 >> 3 & 1u) << 13;
    zr_bus_watch(b, 0x0000u, 0x10000u, 0);
    for (unsigned q = 0; q < 4u; ++q) {
        unsigned bank;
        int direct[2] = {0, 0};
        uint32_t vaddr[2] = {cpu_video, cpu_video};
        if (spectrum_mode(t)) {
            if (t->page1 & 1u) bank = ALL_RAM[t->page1 >> 1 & 3u][q];
            else if (q == 0) bank = 8u + ((t->page1 >> 2 & 1u) << 1 | p74);
            else if (q == 1) {
                bank = 5;
                direct[0] = c2 && p72;
            } else if (q == 2) bank = 2;
            else {
                bank = c2 << 3 | (t->page7 & 7u);
                if (c2 && p72) {
                    direct[0] = direct[1] = 1;
                    vaddr[0] = (uint32_t)(t->page7 & 3u) << 14;
                    vaddr[1] = vaddr[0] + 0x2000u;
                }
            }
        } else {
            static const uint8_t NATIVE[4] = {11, 9, 10, 8};
            bank = NATIVE[q];
            if ((q == 1 && p74) || (q == 2 && !p74)) {
                if (c2 && c0 && !p72) bank = 8u + (t->page7 & 3u);
                else if (!c2 && !c1 && !c0) bank = t->page7 & 7u;
                else if (c2 && !c0 && p72) direct[0] = 1;
            }
        }
        for (unsigned h = 0; h < 2u; ++h) {
            const uint16_t at = (uint16_t)(q * PAGE + h * 0x2000u);
            uint8_t *ram = s->ram + bank * PAGE + h * 0x2000u;
            if (direct[h]) {
                zr_bus_map_ram(b, at, 0x2000u, t->vram + vaddr[h]);
                zr_bus_watch(b, at, 0x2000u, 1);
                continue;
            }
            if (q == 0 && !c0)
                for (unsigned e = 0; e < 0x2000u; e += EPROM) zr_bus_map_read(b, (uint16_t)(at + e), EPROM, s->rom);
            else zr_bus_map_read(b, at, 0x2000u, ram);
            zr_bus_map_write(b, at, 0x2000u, q == 0 && !c1 && bank >= 8u ? NULL : ram);
        }
    }
    /* The second 8255's PC2 high: the writes to 4000h-5FFFh go into the video page too. */
    t->mirror = (uint8_t)(t->ctl2 >> 2 & 1u);
    t->mirror_base = cpu_video;
    if (t->mirror) zr_bus_watch(b, 0x4000u, 0x2000u, 1);
    /* The tape's "stop if 48K" blocks stop it while the 48 BASIC ROM is in. */
    zr_tape_deck_set_48k(&s->deck, spectrum_mode(t) && !(t->page1 & 1u) && (t->page1 & 4u) && p74);
}

/* A write to a page the video reads (the video RAM mapped in, or 4000h-5FFFh while the writes are copied): the picture up to
 * the write, and the copy. */
static void video_write(void *ctx, uint16_t address, uint8_t value) {
    timsplus *t = ctx;
    const uint8_t *p = zr_bus_target(&t->s.bus, address);
    const int direct = p && p >= t->vram && p < t->vram + sizeof t->vram;
    const int copy = t->mirror && (address & 0xE000u) == 0x4000u;
    if (!direct && !copy) return;
    zr_timsvideo_catch_up(&t->video, now_of(t));
    if (copy) t->vram[t->mirror_base | (address & 0x1FFFu)] = value;
}

/* The CPU's clock: the auxiliary one while PC6 is high (machine_core.sv's turbo; the manual: low, the 3.5 MHz base). */
static void clock_update(timsplus *t) {
    const int aux = (t->ctl >> 6 & 1u) && t->cpu_switch != TIMS_CPU_3_5;
    tims_set_cpu_clock(&t->s, aux ? tims_cpu_ticks(t->cpu_switch) : TIMS_TICKS_3_5);
}

/* The 8255s' pins moved, or a paging port was written: the video's inputs, the map and the clock follow. */
static void board_update(timsplus *t) {
    zr_timsvideo_catch_up(&t->video, now_of(t));
    t->ctl = control1(t);
    t->ctl2 = control2(t);
    t->pa1 = zr_i8255_out(&t->ppi1, ZR_I8255_PORT_A);
    zr_timsvideo_set_mode(&t->video, !(t->pa1 & 2u), t->pa1 >> 2 & 1u, t->pa1 & 1u);
    zr_timsvideo_set_page(&t->video, (unsigned)(t->pa1 >> 3 & 3u) << 1 | (t->page7 >> 3 & 1u));
    zr_timsvideo_set_scroll(&t->video, zr_i8255_out(&t->ppi2, ZR_I8255_PORT_A));
    remap(t);
    clock_update(t);
}

/* --- the 8251s' clocks, the serial port ------------------------------------------------------------------------------- */

/* The period of a counter's OUT in 2 MHz clocks while it runs as a rate generator or a square wave (modes 2 and 3): its
 * count; 0 when it gives no clock. */
static uint32_t pit_period(const zr_i8253 *p, unsigned i) {
    const zr_i8253_counter *c = &p->c[i];
    if ((c->mode & 3u) != 2u && (c->mode & 3u) != 3u) return 0;
    if (!c->loaded && !c->pending) return 0;
    uint32_t n = c->cr;
    if (c->bcd) n = (n >> 12 & 15u) * 1000u + (n >> 8 & 15u) * 100u + (n >> 4 & 15u) * 10u + (n & 15u);
    if (!n) n = c->bcd ? 10000u : 65536u;
    return n == 1u ? 0u : n;
}

/* Each 8251's TxC and RxC is an 8253's OUT (machine_core.sv's uart_clocks): the native channel counter 0 of the 8253 at
 * 9CFDh, the expansion channels 1-3 the counters of the one at 8CFDh, channel 4 counter 2 of the first. */
static void uart_clocks(timsplus *t) {
    const uint32_t n[TIMS_UARTS] = {pit_period(&t->pit, 0), pit_period(&t->mux_pit, 0), pit_period(&t->mux_pit, 1),
                                    pit_period(&t->mux_pit, 2), pit_period(&t->pit, 2)};
    for (unsigned i = 0; i < TIMS_UARTS; ++i) {
        const uint32_t num = n[i] * PIT_TICKS;
        if (num == t->uart_num[i]) continue;
        t->uart_num[i] = num;
        zr_i8251_set_clock(&t->uart[i], ZR_I8251_TXC, num, 1);
        zr_i8251_set_clock(&t->uart[i], ZR_I8251_RXC, num, 1);
    }
}

/* The modem inputs of the channels: the one on the connector has the host's CTS and DSR (with a loopback plug, its own RTS
 * and DTR, and TxD to RxD); the others are disconnected and report no readiness. */
static void uart_wiring(timsplus *t) {
    for (unsigned i = 0; i < TIMS_UARTS; ++i) {
        zr_i8251 *u = &t->uart[i];
        const int on = t->has_ports && i == t->channel;
        const int plug = on && t->loopback;
        zr_i8251_set_loopback(u, plug);
        const int cts = plug ? zr_i8251_out(u, ZR_I8251_PIN_RTS) : on && t->host_cts;
        const int dsr = plug ? zr_i8251_out(u, ZR_I8251_PIN_DTR) : on && t->host_dsr;
        u->dev.ops->input(u, ZR_I8251_IN_CTS, cts);
        u->dev.ops->input(u, ZR_I8251_IN_DSR, dsr);
    }
}

static void uart_transmit(void *ctx, uint8_t byte) {
    const uart_ctx *c = ctx;
    timsplus *t = c->t;
    if (c->index == t->channel && t->has_ports && !t->loopback && t->ports.transmit) t->ports.transmit(t->ports.ctx, TIMS_PORT_SERIAL, byte);
}

/* --- the chips' pins ---------------------------------------------------------------------------------------------------- */

static uint64_t chip_now(void *ctx) { return zr_spectrum_now(ctx); }
static void chip_schedule(void *ctx, void *dev, uint64_t when) { zr_sched_set(&((zr_spectrum *)ctx)->sched, dev, when); }
static void no_line(void *ctx, void *dev, int pin, int state) { (void)ctx; (void)dev; (void)pin; (void)state; }

static void chip_line(void *ctx, void *dev, int pin, int state) {
    timsplus *t = ctx;
    if (dev == &t->video) {
        if (pin == ZR_TIMSVIDEO_INT) zr_z80_int(&t->s.z80, state);
    } else if (dev == &t->pit) {
        /* OUT1 clocks the FLASH flip-flop on its rising edge. */
        if (pin == ZR_I8253_OUT1) {
            if (state && !t->out1) {
                zr_timsvideo_catch_up(&t->video, now_of(t));
                zr_timsvideo_set_flash(&t->video, !t->video.flash_level);
            }
            t->out1 = (uint8_t)(state != 0);
        }
    } else if (dev == &t->fdc) {
        if (pin == ZR_UPD765_PIN_INT) t->fdc_int = (uint8_t)(state != 0);
    } else if (dev == &t->uart[t->channel] && (pin == ZR_I8251_PIN_DTR || pin == ZR_I8251_PIN_RTS)) {
        zr_i8251 *u = &t->uart[t->channel];
        if (t->has_ports && t->loopback) u->dev.ops->input(u, pin == ZR_I8251_PIN_DTR ? ZR_I8251_IN_DSR : ZR_I8251_IN_CTS, state);
        else if (t->has_ports && t->ports.outputs)
            t->ports.outputs(t->ports.ctx, TIMS_PORT_SERIAL, zr_i8251_out(u, ZR_I8251_PIN_DTR), zr_i8251_out(u, ZR_I8251_PIN_RTS));
    }
}

/* --- the ports ------------------------------------------------------------------------------------------------------ */

/* Port FE read: the keys in bits 0-4 (in Spectrum mode the added keys the core gives the Spectrum's too), the two added
 * lines in bits 5 and 7, EAR in bit 6 (high without a tape, as the standard model's). */
static uint8_t fe_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    zr_spectrum *s = &t->s;
    const uint8_t high = (uint8_t)(port >> 8);
    uint8_t keys = (uint8_t)(t->keys_read(s, port) & 0x1Fu);
    if (spectrum_mode(t)) keys = (uint8_t)(keys & ~tims_extra_matrix(t->extra, high, 1));
    const unsigned ear = s->tape ? (unsigned)(zr_tape_deck_level(&s->deck, zr_spectrum_now(s)) != 0) : 1u;
    return (uint8_t)(keys | ear << 6 | tims_extra_lines(t->extra, high));
}
/* Port FE written (every even port): the border, MIC and the speaker (the shared core's latch), the video's border. */
static void fe_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    zr_timsvideo_catch_up(&t->video, now_of(t));
    t->latch_write(&t->s, port, value);
    zr_timsvideo_set_border(&t->video, value & 7u);
}

/* The first 8255: A at 0EFDh, B at 0FFDh, C at 0CFDh, the control at 0DFDh (machine_core.sv's {!A9, A8}). Port B reads the
 * printer's BUSY on bit 0 (1: busy, off line or none), the others high; port C reads 00h where it is an input (the core's). */
static unsigned ppi1_reg(uint16_t port) { return (unsigned)(!(port >> 9 & 1u)) << 1 | (port >> 8 & 1u); }
static int printer_busy(const timsplus *t) { return t->physical_printer && !(t->has_ports && t->printer_online); }
static uint8_t ppi1_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    zr_i8255 *p = &t->ppi1;
    p->dev.ops->input(p, ZR_I8255_PORT(ZR_I8255_PORT_A), 0xFFu);
    p->dev.ops->input(p, ZR_I8255_PORT(ZR_I8255_PORT_B), (uint8_t)(0xFEu | (printer_busy(t) ? 1u : 0u)));
    p->dev.ops->input(p, ZR_I8255_PORT(ZR_I8255_PORT_C), 0x00u);
    return p->dev.ops->read(p, ZR_SPACE_IO, ppi1_reg(port));
}
static void ppi1_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    t->ppi1.dev.ops->write(&t->ppi1, ZR_SPACE_IO, ppi1_reg(port), value);
    board_update(t);
}

/* The second 8255: A (the scroll register) at 5CFDh, B (the parallel input) at 5DFDh, C at 5EFDh, the control at 5FFDh.
 * Port C's inputs as the manual gives them: PC7 nBUSY2 (high: nothing on the parallel input), PC6 SYNC (high in the
 * vertical sync), PC5 LCLK (the auxiliary clock selected), PC4 the 8272's interrupt (low while it is raised), PC3 nACK
 * (high), PC0-PC2 outputs (high where nothing drives them). */
static uint8_t ppi2_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    zr_i8255 *p = &t->ppi2;
    unsigned line;
    zr_timsvideo_position(&t->video, now_of(t), &line, NULL);
    const int sync = line >= ZR_TIMSVIDEO_SYNC_LINE && line < ZR_TIMSVIDEO_SYNC_LINE + 4u;
    const uint8_t c = (uint8_t)(0x8Fu | (sync ? 0x40u : 0u) | (t->ctl & 0x40u ? 0x20u : 0u) | (t->fdc_int ? 0u : 0x10u));
    p->dev.ops->input(p, ZR_I8255_PORT(ZR_I8255_PORT_A), 0xFFu);
    p->dev.ops->input(p, ZR_I8255_PORT(ZR_I8255_PORT_B), 0xFFu);
    p->dev.ops->input(p, ZR_I8255_PORT(ZR_I8255_PORT_C), c);
    return p->dev.ops->read(p, ZR_SPACE_IO, port >> 8 & 3u);
}
static void ppi2_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    t->ppi2.dev.ops->write(&t->ppi2, ZR_SPACE_IO, port >> 8 & 3u, value);
    board_update(t);
}

/* The 8253s: counters 0-2 and the control word by A9-A8. A control word for counter 1 of the first hands FLASH to it (the
 * core's timer_flash, kept until the power goes). */
static uint8_t pit_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    return t->pit.dev.ops->read(&t->pit, ZR_SPACE_IO, port >> 8 & 3u);
}
static void pit_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    const unsigned reg = port >> 8 & 3u;
    t->pit.dev.ops->write(&t->pit, ZR_SPACE_IO, reg, value);
    if (reg == 3u && (value >> 6) == 1u && !t->video.flash_timer) {
        zr_timsvideo_catch_up(&t->video, now_of(t));
        zr_timsvideo_set_flash_source(&t->video, 1);
    }
    uart_clocks(t);
}
static uint8_t mux_pit_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    return t->mux_pit.dev.ops->read(&t->mux_pit, ZR_SPACE_IO, port >> 8 & 3u);
}
static void mux_pit_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    t->mux_pit.dev.ops->write(&t->mux_pit, ZR_SPACE_IO, port >> 8 & 3u, value);
    uart_clocks(t);
}

/* The native 8251: data at DEFDh, status and command at DFFDh (A8). The expansion 8251 the multiplexer selects: data at
 * CFFDh, status and command at EFFDh (A13). */
static uint8_t uart0_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    return t->uart[0].dev.ops->read(&t->uart[0], ZR_SPACE_IO, port >> 8 & 1u);
}
static void uart0_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    t->uart[0].dev.ops->write(&t->uart[0], ZR_SPACE_IO, port >> 8 & 1u, value);
}
static zr_i8251 *expansion(timsplus *t) { return &t->uart[1u + (t->serial_mux & 3u)]; }
static uint8_t uartx_read(void *ctx, uint16_t port) {
    zr_i8251 *u = expansion(ctx);
    return u->dev.ops->read(u, ZR_SPACE_IO, port >> 13 & 1u);
}
static void uartx_write(void *ctx, uint16_t port, uint8_t value) {
    zr_i8251 *u = expansion(ctx);
    u->dev.ops->write(u, ZR_SPACE_IO, port >> 13 & 1u, value);
}
static void mux_write(void *ctx, uint16_t port, uint8_t value) {
    (void)port;
    of(ctx)->serial_mux = (uint8_t)(value & 15u);
}

/* The 8272: the main status register at 2FFDh, the data register at 3FFDh (A12). */
static uint8_t fdc_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    return t->fdc.dev.ops->read(&t->fdc, ZR_SPACE_IO, port >> 12 & 1u);
}
static void fdc_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    (void)port;
    t->fdc.dev.ops->write(&t->fdc, ZR_SPACE_IO, 1, value);
}

/* 7FFDh: bits 0-2 the bank at C000h (bit 2 with PC2 the block), bit 3 the video page's A13, bit 4 the ROM's A14. */
static void page7_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    (void)port;
    zr_timsvideo_catch_up(&t->video, now_of(t));
    t->page7 = value;
    board_update(t);
}

/* The drives' motor (1FFDh bit 3: both drives). */
static void motor_update(timsplus *t) {
    const uint64_t now = now_of(t);
    for (unsigned u = 0; u < 2u; ++u) zr_fdd_set_motor(&t->fdd[u], (t->page1 >> 3 & 1u) != 0, now);
    zr_upd765_drive_changed(&t->fdc);
}

/* 1FFDh: bits 0-2 the +3's paging, bit 3 the motor, bit 4 the printer's strobe (active low: the byte of 0FFDh is taken as it
 * falls; a physical printer off line takes nothing). */
static void page1_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    (void)port;
    const uint8_t old = t->page1;
    t->page1 = value;
    if ((old ^ value) & 8u) motor_update(t);
    if ((old & 0x10u) && !(value & 0x10u) && !printer_busy(t) && t->has_ports && t->ports.transmit)
        t->ports.transmit(t->ports.ctx, TIMS_PORT_PRINTER, t->ppi1.latch[ZR_I8255_PORT_B]);
    board_update(t);
}

/* MICRONEXT: the address in three bytes (22 bits used: 4 MiB), the data at xx3Fh; its memory exists from the first write
 * (a byte never written reads 00h). */
static void mn_address(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    const unsigned shift = (port >> 4 & 3u) * 8u;
    t->mn_address = (t->mn_address & ~(0xFFu << shift)) | (uint32_t)value << shift;
}
static uint8_t mn_read(void *ctx, uint16_t port) {
    timsplus *t = ctx;
    (void)port;
    return t->micronext ? t->micronext[t->mn_address & (MICRONEXT - 1u)] : 0x00u;
}
static void mn_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    (void)port;
    if (!t->micronext && !(t->micronext = calloc(1, MICRONEXT))) return;
    t->micronext[t->mn_address & (MICRONEXT - 1u)] = value;
}

/* The network: F7h reads the line's receiver on bit 0 (idle: 1; the other bits 1), F7h written drives the line, F3h arms
 * the NMI a SCOUT pulse would give. ZetRo has no network: the line stays idle and the NMI never comes. */
static uint8_t net_read(void *ctx, uint16_t port) { (void)ctx; (void)port; return 0xFFu; }
static void net_write(void *ctx, uint16_t port, uint8_t value) {
    timsplus *t = ctx;
    if ((port & 0xFFu) == 0xF7u) t->network = (uint8_t)(value & 1u);
    else t->armed = 1;
}

/* --- the core's paging hooks (snapshots, ZR_PAGE_ADDRESS) ---------------------------------------------------------------- */

static uint8_t *page_of(zr_spectrum *s, unsigned page) { return page < 8u ? s->ram + page * PAGE : NULL; }
static uint8_t paging_get(zr_spectrum *s) { return ((timsplus *)s)->page7; }
static void paging_set(zr_spectrum *s, uint8_t value) {
    timsplus *t = (timsplus *)s;
    t->page7 = value;
    board_update(t);
}

/* --- life cycle -------------------------------------------------------------------------------------------------------- */

/* The disks go back into their media when they have changed. */
static void flush_disk(timsplus *t, unsigned drive) {
    zr_disk *disk = t->disk[drive];
    if (!disk || !disk->dirty || !t->disk_media[drive]) return;
    char why[128];
    if (!zr_disk_save_media(disk, t->disk_media[drive], why, sizeof why)) snprintf(t->s.error, sizeof t->s.error, "%s", why);
}

static void plus_destroy(void *self) {
    timsplus *t = self;
    if (!t) return;
    for (unsigned i = 0; i < 2u; ++i) {
        flush_disk(t, i);
        zr_fdd_eject(&t->fdd[i]);
        zr_disk_close(t->disk[i]);
    }
    free(t->micronext);
    zr_spectrum_destroy(t);
}

/* The READY input of each drive: a disk in it (machine_core.sv's mounted). */
static void ready_update(timsplus *t) {
    for (unsigned u = 0; u < 2u; ++u) t->fdc.dev.ops->input(&t->fdc, ZR_UPD765_PIN_READY0 + (int)u, t->disk[u] != NULL);
    zr_upd765_drive_changed(&t->fdc);
}

/* The board as RESET leaves it: the bootstrap in (the 8255s' pins float), the +3 system written into BR2 again, the paging
 * ports, the multiplexer, MICRONEXT's address and the network cleared, every chip reset. */
static void board_reset(timsplus *t) {
    zr_spectrum *s = &t->s;
    memcpy(s->ram + 8u * PAGE, s->rom + SYSTEM, 4u * PAGE);
    t->page7 = t->page1 = 0;
    t->serial_mux = 0;
    t->mn_address = 0;
    t->network = 1;
    t->armed = 0;
    t->ppi1.dev.ops->reset(&t->ppi1);
    t->ppi2.dev.ops->reset(&t->ppi2);
    t->pit.dev.ops->reset(&t->pit);
    t->mux_pit.dev.ops->reset(&t->mux_pit);
    t->out1 = (uint8_t)zr_i8253_out(&t->pit, 1);
    for (unsigned i = 0; i < TIMS_UARTS; ++i) t->uart[i].dev.ops->reset(&t->uart[i]);
    uart_clocks(t);
    uart_wiring(t);
    t->fdc.dev.ops->reset(&t->fdc);
    motor_update(t);
    ready_update(t);
    t->video.dev.ops->reset(&t->video);
    zr_timsvideo_catch_up(&t->video, now_of(t));
    zr_timsvideo_set_border(&t->video, 0);
    board_update(t);
}

static void plus_reset(void *self, int power_on) {
    timsplus *t = self;
    zr_spectrum_reset(&t->s, power_on);
    if (power_on) {
        zr_timsvideo_catch_up(&t->video, now_of(t));
        memset(t->vram, 0, sizeof t->vram);
        if (t->micronext) memset(t->micronext, 0, MICRONEXT);
        zr_timsvideo_set_flash_source(&t->video, 0);
        zr_timsvideo_set_flash(&t->video, 0);
    }
    board_reset(t);
}

void *tims_plus_create(const zr_machine_setup *setup, char *error, size_t capacity) {
    const unsigned *o = setup->options;
    zr_spectrum_config cfg;
    zr_spectrum_default_config(&cfg);
    cfg.clock_hz = TIMS_CLOCK_HZ;
    cfg.ticks_per_tstate = TIMS_TICKS_3_5;
    cfg.ula.ticks_per_pixel = TIMS_CLOCK_HZ / 7000000u;
    cfg.ula.ticks_per_tstate = TIMS_TICKS_3_5;
    cfg.ram_size = BANKS * PAGE;
    cfg.traps_rom = SYSTEM + 3u * PAGE;                  /* the 48 BASIC ROM (its copy in BR2 is mapped when it runs) */
    cfg.page = page_of;
    cfg.paging_get = paging_get;
    cfg.paging_set = paging_set;
    cfg.paging_48k = 0x30u;
    cfg.nmi = 1;
    cfg.is_48k = 0;
    cfg.joystick = (uint8_t)o[TIMS_OPTION_JOYSTICK];
    cfg.joystick2 = (uint8_t)o[TIMS_OPTION_JOYSTICK2];
    cfg.sinclair = ZR_ULA_SINCLAIR_128K;
    cfg.ay = o[TIMS_OPTION_PLUS_AY] == TIMS_AY_MONO ? ZR_SPECTRUM_AY_MONO : ZR_SPECTRUM_AY_ABC;
    cfg.ay_mask = 0xFFFFu;                              /* FFFDh and BFFDh exactly */
    cfg.ay_value = 0xFFFDu;
    timsplus *t = (timsplus *)zr_spectrum_create(&cfg, setup, sizeof *t, error, capacity);
    if (!t) return NULL;
    zr_spectrum *s = &t->s;
    /* The text read-back's glyphs: the 48 BASIC ROM's character set, which the +3's menus draw with too (past the 64 KiB the
     * config's offset reaches). */
    for (unsigned g = 0; g < 96u; ++g) {
        uint64_t word = 0;
        for (unsigned y = 0; y < 8u; ++y) word = word << 8 | s->rom[SYSTEM + 3u * PAGE + 0x3D00u + g * 8u + y];
        s->glyph[g] = word;
    }
    t->cpu_switch = (uint8_t)(o[TIMS_OPTION_CPU] < TIMS_CPU_KINDS ? o[TIMS_OPTION_CPU] : TIMS_CPU_3_5);
    t->channel = (uint8_t)(o[TIMS_OPTION_UART] < TIMS_UARTS ? o[TIMS_OPTION_UART] : TIMS_UART_NATIVE);
    t->physical_printer = o[TIMS_OPTION_PRINTER] == TIMS_PRINTER_PHYSICAL;
    /* The AY at 1.75 MHz of the 168 MHz clock (the shared core assumed 14 MHz). */
    zr_ay_init(&s->ay, &s->host, TIMS_CLOCK_HZ, 1750000u, ZR_SPECTRUM_AUDIO_RATE, cfg.ay == ZR_SPECTRUM_AY_MONO ? ZR_AY_MONO : ZR_AY_ABC, NULL);
    /* The chips' host; the shared ULA keeps its raster (the keyboard matrix, the tape's timing) without its interrupt. */
    t->chip_host.ctx = t;
    t->chip_host.now = chip_now;
    t->chip_host.schedule = chip_schedule;
    t->chip_host.line = chip_line;
    t->ula_host = s->host;
    t->ula_host.line = no_line;
    s->ula.dev.host = &t->ula_host;
    zr_timsvideo_init(&t->video, &t->chip_host, DOT_TICKS, t->vram);
    s->screen = t->vram;
    zr_i8255_init(&t->ppi1, &t->chip_host, "tims+ 8255-01");
    zr_i8255_init(&t->ppi2, &t->chip_host, "tims+ 8255-02");
    zr_i8253_init(&t->pit, &t->chip_host, "tims+ 8253", PIT_TICKS, 1);
    zr_i8253_set_clock(&t->pit, 1, FLASH_TICKS, 1);
    zr_i8253_watch(&t->pit, 2u);                        /* OUT1 (FLASH); the others are read as rates */
    zr_i8253_init(&t->mux_pit, &t->chip_host, "tims+ 8253 serial", PIT_TICKS, 1);
    zr_i8253_watch(&t->mux_pit, 0);
    static const char *const UART_NAMES[TIMS_UARTS] = {"tims+ 8251", "tims+ 8251 x1", "tims+ 8251 x2", "tims+ 8251 x3", "tims+ 8251 x4"};
    for (unsigned i = 0; i < TIMS_UARTS; ++i) {
        t->uctx[i].t = t;
        t->uctx[i].index = i;
        zr_i8251_init(&t->uart[i], &t->chip_host, UART_NAMES[i], TIMS_CLOCK_HZ, 0, 0);
        zr_i8251_set_transmit(&t->uart[i], uart_transmit, &t->uctx[i]);
    }
    /* The 8272 and its two 5.25" drives (80 cylinders, two sides, 300 rpm, 250 kbit/s); READY is a disk in the drive. */
    const int drive_sound = o[TIMS_OPTION_DRIVE_SOUND] != 0;
    zr_drivesnd_init(&t->drivesnd, TIMS_CLOCK_HZ, ZR_SPECTRUM_AUDIO_RATE);
    zr_drivesnd_set(&t->drivesnd, drive_sound, 50);
    zr_upd765_init(&t->fdc, &t->chip_host, "tims+ 8272", TIMS_CLOCK_HZ);
    zr_upd765_ready_wiring(&t->fdc, ZR_UPD765_READY_PINS);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_fdd_init(&t->fdd[u], TIMS_CLOCK_HZ, 1, 80, 300);
        if (drive_sound) zr_fdd_set_hook(&t->fdd[u], zr_drivesnd_event, &t->drivesnd, u);
        zr_upd765_attach(&t->fdc, u, &t->fdd[u]);
    }
    zr_upd765_set_rate(&t->fdc, 250);
    if (drive_sound) s->drives = &t->drivesnd;
    /* The memory: the board maps it (remap) and sees the writes to the video's pages. */
    zr_bus_set_hook(&s->bus, video_write, t);
    /* The ports: port FE is the shared core's decoder 0, wrapped; the rest the board's. */
    t->keys_read = s->bus.dec[0].read;
    t->latch_write = s->bus.dec[0].write;
    s->bus.dec[0].read = fe_read;
    s->bus.dec[0].write = fe_write;
    s->bus.dec[0].ctx = t;
    zr_bus_add_io(&s->bus, 0xFCFFu, 0x0CFDu, ppi1_read, ppi1_write, t);
    zr_bus_add_io(&s->bus, 0xFCFFu, 0x5CFDu, ppi2_read, ppi2_write, t);
    zr_bus_add_io(&s->bus, 0xFCFFu, 0x9CFDu, pit_read, pit_write, t);
    zr_bus_add_io(&s->bus, 0xFCFFu, 0x8CFDu, mux_pit_read, mux_pit_write, t);
    zr_bus_add_io(&s->bus, 0xFEFFu, 0xDEFDu, uart0_read, uart0_write, t);
    zr_bus_add_io(&s->bus, 0xFFFFu, 0xAFFDu, NULL, mux_write, t);
    zr_bus_add_io(&s->bus, 0xDFFFu, 0xCFFDu, uartx_read, uartx_write, t);
    zr_bus_add_io(&s->bus, 0xEFFFu, 0x2FFDu, fdc_read, NULL, t);
    zr_bus_add_io(&s->bus, 0xFFFFu, 0x3FFDu, NULL, fdc_write, t);
    zr_bus_add_io(&s->bus, 0xFFFFu, 0x7FFDu, NULL, page7_write, t);
    zr_bus_add_io(&s->bus, 0xFFFFu, 0x1FFDu, NULL, page1_write, t);
    for (uint16_t a = 0x0Fu; a <= 0x2Fu; a = (uint16_t)(a + 0x10u)) zr_bus_add_io(&s->bus, 0x00FFu, a, NULL, mn_address, t);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x003Fu, mn_read, mn_write, t);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x00F7u, net_read, net_write, t);
    zr_bus_add_io(&s->bus, 0x00FFu, 0x00F3u, NULL, net_write, t);
    if (t->cpu_switch != TIMS_CPU_3_5)
        snprintf(s->description, sizeof s->description, "%s, %s MHz", setup->family->models[setup->model].name, tims_cpu_words[t->cpu_switch]);
    plus_reset(t, 1);
    return t;
}

/* After each run: the picture up to the run's end; a disk the guest wrote goes back into its media once its motor is off. */
static zr_run_status plus_run(void *self, uint64_t ticks) {
    timsplus *t = self;
    const zr_run_status status = zr_spectrum_run(t, ticks);
    zr_timsvideo_catch_up(&t->video, now_of(t));
    for (unsigned i = 0; i < 2u; ++i)
        if (t->disk[i] && t->disk[i]->dirty && !t->fdd[i].motor) flush_disk(t, i);
    return status;
}

static int plus_frame(void *self, zr_frame *frame) { return zr_timsvideo_frame(&of(self)->video, frame); }

/* --- the screen as text -------------------------------------------------------------------------------------------- */

/* Each 8 x 8 cell the video shows (32, 40, 64 or 80 a line by the window and the resolution, the scroll applied) matched
 * against the 48 BASIC ROM's character set as it is or inverted; '?' for a cell that is none. */
static size_t plus_text(void *self, char *out, size_t capacity) {
    timsplus *t = self;
    const zr_timsvideo *v = &t->video;
    const unsigned cells = (v->wide ? 40u : 32u) * (v->hires ? 2u : 1u);
    size_t n = 0;
    for (unsigned row = 0; row < 24u; ++row) {
        char line[80];
        unsigned last = 0;
        for (unsigned col = 0; col < cells; ++col) {
            uint64_t cell = 0;
            for (unsigned y = 0; y < 8u; ++y) cell = cell << 8 | t->vram[zr_timsvideo_cell_address(v, row * 8u + y, col)];
            char ch = '?';
            for (unsigned g = 0; g < 96u; ++g)
                if (t->s.glyph[g] == cell || t->s.glyph[g] == ~cell) {
                    ch = (char)(0x20u + g);
                    break;
                }
            line[col] = ch;
            if (ch != ' ') last = col + 1u;
        }
        for (unsigned col = 0; col < last && n + 2 < capacity; ++col) out[n++] = line[col];
        if (n + 1 < capacity) out[n++] = '\n';
    }
    if (capacity) out[n < capacity ? n : capacity - 1] = 0;
    return n;
}

/* --- input ------------------------------------------------------------------------------------------------------------ */

static void plus_key(void *self, unsigned key, int down) {
    timsplus *t = self;
    if (key < ZR_SPECTRUM_KEYS) zr_spectrum_key(self, key, down);
    else if (key < TIMS_KEYS) {
        const unsigned bit = 1u << (key - ZR_SPECTRUM_KEYS);
        t->extra = down ? t->extra | bit : t->extra & ~bit;
    }
}

/* --- media ---------------------------------------------------------------------------------------------------------- */

static int plus_insert(void *self, unsigned slot, zr_media *media, char *error, size_t capacity) {
    timsplus *t = self;
    if (slot == TIMS_SLOT_TAPE) return zr_spectrum_insert(self, slot, media, error, capacity);
    if (slot != TIMS_SLOT_A && slot != TIMS_SLOT_B) {
        if (capacity) snprintf(error, capacity, "There is no such slot");
        return 0;
    }
    const unsigned drive = slot - TIMS_SLOT_A;
    if (t->disk_media[drive]) {
        if (capacity) snprintf(error, capacity, "The slot is not empty");
        return 0;
    }
    zr_disk *disk = zr_disk_open(media, NULL, error, capacity);
    if (!disk) return 0;
    if (!zr_fdd_insert(&t->fdd[drive], disk, error, capacity)) {
        zr_disk_close(disk);
        return 0;
    }
    t->disk[drive] = disk;
    media->disk = disk;
    t->disk_media[drive] = media;
    ready_update(t);
    return 1;
}

static zr_media *plus_media(const void *self, unsigned slot) {
    const timsplus *t = self;
    if (slot == TIMS_SLOT_A || slot == TIMS_SLOT_B) return t->disk_media[slot - TIMS_SLOT_A];
    return zr_spectrum_media(self, slot);
}

static int plus_eject(void *self, unsigned slot) {
    timsplus *t = self;
    if (slot != TIMS_SLOT_A && slot != TIMS_SLOT_B) return zr_spectrum_eject(self, slot);
    const unsigned drive = slot - TIMS_SLOT_A;
    if (!t->disk_media[drive]) return 0;
    flush_disk(t, drive);
    zr_fdd_eject(&t->fdd[drive]);
    zr_disk_close(t->disk[drive]);
    t->disk[drive] = NULL;
    t->disk_media[drive] = NULL;
    ready_update(t);
    return 1;
}

static unsigned plus_activity(const void *self) {
    const timsplus *t = self;
    unsigned bits = zr_spectrum_activity(self);
    for (unsigned u = 0; u < 2u; ++u)
        if (t->disk[u] && t->fdd[u].motor) bits |= 1u << (TIMS_SLOT_A + u);
    return bits;
}

/* --- the ports to the host --------------------------------------------------------------------------------------------- */

static void plus_set_ports(void *self, const zr_port_host *host) {
    timsplus *t = self;
    t->has_ports = host != NULL;
    if (host) t->ports = *host;
    t->loopback = host && host->loopback && host->loopback(host->ctx, TIMS_PORT_SERIAL);
    uart_wiring(t);
    const zr_i8251 *u = &t->uart[t->channel];
    if (host && host->outputs && !t->loopback)
        host->outputs(host->ctx, TIMS_PORT_SERIAL, zr_i8251_out(u, ZR_I8251_PIN_DTR), zr_i8251_out(u, ZR_I8251_PIN_RTS));
}
/* Bytes from the host go to the connector's 8251 as long as its receiver takes them. */
static size_t plus_port_receive(void *self, unsigned port, const uint8_t *data, size_t count) {
    timsplus *t = self;
    if (port != TIMS_PORT_SERIAL) return count;
    size_t n = 0;
    while (n < count && zr_i8251_receive(&t->uart[t->channel], data[n])) ++n;
    return n;
}
static void plus_port_modem(void *self, unsigned port, int dcd, int cts, int dsr, int ri) {
    timsplus *t = self;
    (void)dcd; (void)ri;
    if (port == TIMS_PORT_SERIAL) {
        t->host_cts = (uint8_t)(cts != 0);
        t->host_dsr = (uint8_t)(dsr != 0);
        uart_wiring(t);
    } else if (port == TIMS_PORT_PRINTER) t->printer_online = (uint8_t)(dsr != 0);
}

/* --- peek and poke: BR2 and the video RAM as pages 8-15 of ZR_PAGE_ADDRESS ---------------------------------------------- */

static uint8_t *bank_byte(timsplus *t, uint32_t address) {
    if (address < ZR_PAGE_ADDRESS(8, 0) || address >= ZR_PAGE_ADDRESS(16, 0)) return NULL;
    const uint32_t at = address - ZR_PAGE_ADDRESS(8, 0);
    return at < 4u * PAGE ? t->s.ram + 8u * PAGE + at : t->vram + (at - 4u * PAGE);
}
static int plus_peek(void *self, uint32_t address, uint8_t *value) {
    const uint8_t *p = bank_byte(self, address);
    if (!p) return zr_spectrum_peek(self, address, value);
    *value = *p;
    return 1;
}
static int plus_poke(void *self, uint32_t address, uint8_t value) {
    timsplus *t = self;
    uint8_t *p = bank_byte(t, address);
    if (!p) {
        if (address > 0xFFFFu) return zr_spectrum_poke(self, address, value);
        /* A poke is the CPU's write, the copy into the video page included. */
        zr_bus_write(&t->s.bus, (uint16_t)address, value);
        return 1;
    }
    if (p >= t->vram && p < t->vram + sizeof t->vram) zr_timsvideo_catch_up(&t->video, now_of(t));
    *p = value;
    return 1;
}

/* --- states ------------------------------------------------------------------------------------------------------------- */

static void plus_save(const void *self, zr_state_writer *w) {
    const timsplus *t = self;
    zr_spectrum_save(self, w);
    zr_sw_begin(w, "TIMP", 0);
    zr_sw_u8(w, STATE_VERSION);
    zr_sw_u8(w, t->page7);
    zr_sw_u8(w, t->page1);
    zr_sw_u8(w, t->serial_mux);
    zr_sw_u32(w, t->mn_address);
    zr_sw_u8(w, t->network);
    zr_sw_u8(w, t->armed);
    zr_sw_u8(w, t->out1);
    zr_sw_u8(w, t->fdc_int);
    zr_sw_u8(w, t->host_cts);
    zr_sw_u8(w, t->host_dsr);
    zr_sw_u8(w, t->printer_online);
    zr_sw_packed(w, t->vram, sizeof t->vram);
    zr_sw_u8(w, t->micronext != NULL);
    if (t->micronext) zr_sw_packed(w, t->micronext, MICRONEXT);
    zr_sw_end(w);
    zr_sw_begin(w, "TVID", 0);
    t->video.dev.ops->save(&t->video, w);
    zr_sw_end(w);
    const zr_dev *chips[] = {&t->ppi1.dev, &t->ppi2.dev, &t->pit.dev, &t->mux_pit.dev};
    static const char TAGS[4][5] = {"PPI ", "PPI ", "PIT ", "PIT "};
    for (unsigned i = 0; i < 4u; ++i) {
        zr_sw_begin(w, TAGS[i], i & 1u);
        chips[i]->ops->save(chips[i], w);
        zr_sw_end(w);
    }
    for (unsigned i = 0; i < TIMS_UARTS; ++i) {
        zr_sw_begin(w, "8251", i);
        t->uart[i].dev.ops->save(&t->uart[i], w);
        zr_sw_end(w);
    }
    zr_sw_begin(w, "FDC ", 0);
    t->fdc.dev.ops->save(&t->fdc, w);
    zr_sw_end(w);
    for (unsigned u = 0; u < 2u; ++u) {
        zr_sw_begin(w, "FDD ", u);
        zr_fdd_save(&t->fdd[u], w);
        zr_sw_end(w);
    }
}

static int plus_load(void *self, zr_state_reader *r) {
    timsplus *t = self;
    if (!zr_spectrum_load(self, r)) return 0;
    if (!zr_sr_chunk(r, "TIMP", 0) || zr_sr_u8(r) != STATE_VERSION) return 0;
    t->page7 = zr_sr_u8(r);
    t->page1 = zr_sr_u8(r);
    t->serial_mux = zr_sr_u8(r) & 15u;
    t->mn_address = zr_sr_u32(r);
    t->network = zr_sr_u8(r) & 1u;
    t->armed = zr_sr_u8(r) & 1u;
    t->out1 = zr_sr_u8(r) & 1u;
    t->fdc_int = zr_sr_u8(r) & 1u;
    t->host_cts = zr_sr_u8(r) & 1u;
    t->host_dsr = zr_sr_u8(r) & 1u;
    t->printer_online = zr_sr_u8(r) & 1u;
    zr_sr_packed(r, t->vram, sizeof t->vram);
    if (zr_sr_u8(r)) {
        if (!t->micronext && !(t->micronext = calloc(1, MICRONEXT))) return 0;
        zr_sr_packed(r, t->micronext, MICRONEXT);
    } else if (t->micronext) memset(t->micronext, 0, MICRONEXT);
    if (!zr_sr_ok(r)) return 0;
    if (!zr_sr_chunk(r, "TVID", 0) || !t->video.dev.ops->load(&t->video, r)) return 0;
    zr_dev *chips[] = {&t->ppi1.dev, &t->ppi2.dev, &t->pit.dev, &t->mux_pit.dev};
    static const char TAGS[4][5] = {"PPI ", "PPI ", "PIT ", "PIT "};
    for (unsigned i = 0; i < 4u; ++i)
        if (!zr_sr_chunk(r, TAGS[i], i & 1u) || !chips[i]->ops->load(chips[i], r)) return 0;
    for (unsigned i = 0; i < TIMS_UARTS; ++i)
        if (!zr_sr_chunk(r, "8251", i) || !t->uart[i].dev.ops->load(&t->uart[i], r)) return 0;
    if (!zr_sr_chunk(r, "FDC ", 0) || !t->fdc.dev.ops->load(&t->fdc, r)) return 0;
    for (unsigned u = 0; u < 2u; ++u)
        if (!zr_sr_chunk(r, "FDD ", u) || !zr_fdd_load(&t->fdd[u], r)) return 0;
    /* The drive of the command in progress is the board's to put back (as hc88.c does). */
    t->fdc.x.fdd = t->fdc.drive[t->fdc.x.unit & 3u];
    /* What the loads leave to the board: the pins' consequences, the 8251s' clocks as they have them, the modem lines. */
    for (unsigned i = 0; i < TIMS_UARTS; ++i) t->uart_num[i] = t->uart[i].clk[ZR_I8251_TXC].num;
    t->ctl = control1(t);
    t->ctl2 = control2(t);
    t->pa1 = zr_i8255_out(&t->ppi1, ZR_I8255_PORT_A);
    remap(t);
    clock_update(t);
    uart_wiring(t);
    ready_update(t);
    const zr_beeper *b = &t->s.beeper;
    zr_drivesnd_reset(&t->drivesnd, b->num ? (b->done * b->den + b->num - 1u) / b->num : 0u);
    return 1;
}

/* --- snapshots ------------------------------------------------------------------------------------------------------- */

/* The board as the bootstrap leaves it when the +3 system starts (plus-loader.asm's port writes but its last, which pages
 * the EPROM out). */
static const uint16_t LOADER_OUTS[][2] = {
    {0x0DFD, 0x82}, {0x0CFD, 0x12}, {0x0EFD, 0x02}, {0x5FFD, 0x8A}, {0x5EFD, 0x0C}, {0x9FFD, 0x36}, {0x9CFD, 13}, {0x9CFD, 0},
    {0x9FFD, 0x76}, {0x9DFD, 0xB8}, {0x9DFD, 0x88}, {0xDFFD, 0x4E}, {0xDFFD, 0x37}, {0x0CFD, 0x11},
};

/* A snapshot of a 48K, 128K, +2, +2A or +3 machine, with the board as the bootstrap leaves the +3 system (Spectrum mode,
 * the EPROM out, BR2 protected, the writes to 4000h-5FFFh copied into video page 0): the pages 0-7 and 7FFDh from the
 * file; 1FFDh from a +2A or +3 file, and 04h for the others (the 48 BASIC is the +3's ROM 3: a 128K program that calls
 * the 128 editor's ROM does not run). The video pages 0 and 1 are filled from pages 5 and 7. */
static int plus_snapshot_load(void *self, const struct zr_snap *snap, char *error, size_t capacity) {
    timsplus *t = self;
    zr_spectrum *s = &t->s;
    const int plus3 = snap->machine == ZR_SNAP_M_PLUS2A || snap->machine == ZR_SNAP_M_PLUS3;
    int ok;
    if (plus3) {
        zr_snap *copy = zr_snap_new();
        if (!copy) {
            if (capacity) snprintf(error, capacity, "Out of memory");
            return 0;
        }
        memcpy(copy, snap, sizeof *copy);
        copy->machine = ZR_SNAP_M_128K;
        ok = zr_spectrum_snapshot_load(self, copy, error, capacity);
        zr_snap_free(copy);
    } else ok = zr_spectrum_snapshot_load(self, snap, error, capacity);
    if (!ok) return 0;
    const uint8_t page7 = t->page7;
    memcpy(s->ram + 8u * PAGE, s->rom + SYSTEM, 4u * PAGE);
    t->ppi1.dev.ops->reset(&t->ppi1);
    t->ppi2.dev.ops->reset(&t->ppi2);
    t->pit.dev.ops->reset(&t->pit);
    for (unsigned i = 0; i < sizeof LOADER_OUTS / sizeof LOADER_OUTS[0]; ++i)
        zr_bus_out(&s->bus, LOADER_OUTS[i][0], (uint8_t)LOADER_OUTS[i][1]);
    zr_timsvideo_catch_up(&t->video, now_of(t));
    memcpy(t->vram, s->ram + 5u * PAGE, ZR_TIMSVIDEO_PAGE);
    memcpy(t->vram + ZR_TIMSVIDEO_PAGE, s->ram + 7u * PAGE, ZR_TIMSVIDEO_PAGE);
    t->page7 = page7;
    t->page1 = plus3 ? snap->port_1ffd : 0x04u;
    motor_update(t);
    board_update(t);
    return 1;
}

/* A snapshot is taken as a +3's while the board is as the +3 system runs it (Spectrum mode, the EPROM out, PC2 low: the
 * +3's own memory map); otherwise there is no file format for it. */
static int plus_snapshot_save(void *self, struct zr_snap *snap) {
    timsplus *t = self;
    if ((t->ctl & 0x15u) != 0x11u) return 0;
    if (!zr_spectrum_snapshot_save(self, snap)) return 0;
    zr_snap_set_machine(snap, ZR_SNAP_M_PLUS3, 0, "ZX Spectrum +3");
    snap->port_1ffd = t->page1;
    return 1;
}

const zr_machine_impl tims_plus_impl = {
    .destroy = plus_destroy, .description = zr_spectrum_description, .error = zr_spectrum_error,
    .reset = plus_reset, .nmi = zr_spectrum_nmi,
    .clock = zr_spectrum_clock, .run = plus_run, .time = zr_spectrum_time, .frame_period = zr_spectrum_frame_period,
    .frame = plus_frame, .text = plus_text, .audio_rate = zr_spectrum_audio_rate, .audio_pull = zr_spectrum_audio_pull,
    .key = plus_key, .joystick = zr_spectrum_joystick,
    .insert = plus_insert, .media = plus_media, .eject = plus_eject, .activity = plus_activity,
    .control = zr_spectrum_control, .tape_options = zr_spectrum_tape_options, .tape_deck = zr_spectrum_tape_deck,
    .set_ports = plus_set_ports, .port_receive = plus_port_receive, .port_modem = plus_port_modem,
    .save = plus_save, .load = plus_load, .rom_hashes = zr_spectrum_rom_hashes,
    .set_trace = zr_spectrum_set_trace, .peek = plus_peek, .poke = plus_poke,
    .snapshot_load = plus_snapshot_load, .snapshot_save = plus_snapshot_save,
};
