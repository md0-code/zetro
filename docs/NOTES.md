# Notes

How to use the machines, every setting, the command line, and what ZetRo does not do. The keys and the setup screen are in the [README](../README.md).

## Libretro

Choose the `ZetRo-Libretro-<version>-<os>-<arch>` archive that matches
RetroArch's operating system and architecture. Copy the library from `cores/`
and its metadata from `info/` into RetroArch's Cores and Core Info directories.
Copy the supplied files from `roms/` into `zetro/` inside the frontend's
System/BIOS directory.

Load the ZetRo core and start it on its own to reach the JET's BASIC prompt.
Use Core Options to choose another machine, then close and reopen the core or
content. Load Content opens a tape, disk, card, ROM or snapshot accepted by
the chosen machine. Images opened through Libretro are write-protected.
Save and load states through the frontend. Enable RetroArch's Game Focus to
send keyboard shortcuts to the emulated keyboard. RetroPad B is fire, and
L and R play and stop a mounted tape.

An optional `zetro.yaml` beside the firmware supplies the model, board and
tape settings. Use it to enable hardware such as DivMMC or Interface 1.
Select **configured** in the core's Machine model option to use the model
from that file.

## Using ZetRo

### Tapes, disks and ROMs

**Tapes** are TAP, TZX, PZX, CSW or WAV files; TAP, TZX and PZX can also be written. Put one in the tape slot (Media page, or
drop it on the window; with the tape's Auto-load on, which is the default, the machine restarts and loads it by itself). To
save a program to tape, press Shift+F5 before the SAVE starts writing and again when it has ended; a new
`media/tape-NNNN.tzx` is started when the slot is empty. With Fast loading on (the tape's switch on the Media page, marked
`*`, on by default) a tape loads faster than the original machine loads it: at full speed while it runs, and, on the
Spectrum-compatible machines, a block at once instead of in seconds, SAVE straight onto the tape, and tape waits skipped by
loaders of their own. Switched off, the tape loads as on the machine itself.

**Disks** are DSK or EDSK images (FM and MFM, weak sectors and status bytes kept) or raw images by size. Name them in the
`media` section of `zetro.yaml`, choose them on the Media page or drop them on the window. What the machine writes goes back to
the image file when its drive stops, so keep a copy of a disk you care about; the slot's write protection stops it. The drives
are heard as the real ones were (motor, stepper clicks), made up from what the drives do; `drive_sound` switches them off.

**ROMs** are in `roms/` in the working folder, beside the configuration and beside the program. ZetRo recognises them by the
SHA-256 of their content, whatever the file names are, so renamed copies work; `zetro --list-roms` shows what it found and what
is missing. A machine whose ROMs are missing shows a test screen that says so; the test screen itself (`zetro --model test`) needs none.

### Fast loading and the Spectrum features

On the Spectrum-compatible machines (the JET, the HC family except the HC-88, the CIP, the TIM-S and the CoBra):

- **Snapshots**: drop a `.sna`, `.z80` or `.szx` file on the window, or use the States page. A snapshot holds the processor, the
  memory and the border; the ROM is always the machine's own.
- **Cheats**: a `.pok` file is a list of trainers, each a few memory locations and the values to put in them. Drop one on the
  window, or load it on the Cheats page; a tape's `NAME.pok` beside it or in a `POKES` folder comes by itself. Enter (or `+` and
  `-`) switches a trainer on and off; one that wants a value (a number of lives) asks for it. A trainer that is on writes its
  values again every frame; switching it off puts back what was there.
- **Spectrum timing**: the option gives the machine the contended memory, floating bus and frame timing of Sinclair's own
  Spectrum, for programs timed to it; the clones have none.
- **AY board and joysticks**: an AY sound board (mono, stereo ABC or ACB) and joysticks of the kinds Kempston, Sinclair I,
  Sinclair II, Cursor and Fuller on every model, marked `*` where the original machine had none.

**Joysticks** are driven by the keyboard (the cursor keys, Left Alt for fire and Left Ctrl for the second button, switched on and
off with F11; `controllers.keys` changes them) or by a game controller (the d-pad or left stick, A and B).

### States and rewind

F2 saves everything the machine is doing to the current slot and F3 puts it back, so a game can be taken up again exactly where
it was. Each model has ten slots, kept as files in a `states` folder beside `zetro.yaml` (`states/hc91/slot-1.zrs`);
`ui.state_slot` says which is current. A state remembers the tapes and disks that were in the machine, by file name and content;
if a file is missing or has changed, the message line says so. Holding F4 runs the machine backwards through the last minutes at
about ten times the speed, and when F4 is let go the machine carries on from there. Only the machine goes back, not the files it
uses: what a program wrote to a disk stays written.

### On-screen keyboard

F8 (or the Select/Back button of a game controller) opens the machine's own keyboard over the lower part of the picture, drawn
in the frame like the rest of the picture, with the keywords and the machine's native keys. Click a key with the mouse or a
finger to press it for as long as the button is held; the shift keys stay held until the next key has been typed, so a mouse
alone types shifted characters and keywords. With a game controller the d-pad moves from key to key, A presses the selected key
and B closes the keyboard. The machine keeps running and the keys of the host's keyboard work as usual.

### Interfaces

The Spectrum-compatible machines can take an added interface (not original to any of them), option Interface on the Machine page
(`machine.options.interface`), none in a new configuration: the DivMMC (an SD card) and the DivIDE (two IDE disks), which add
a card or hard disk and esxDOS to the machine, and the ZX Dandanator Mini cartridge. The JET, HC-85/90/91, HC-91 Plus, HC-2000,
CIP-01/02/03 and CIP-04 take them; an HC with Interface 1 on cannot have one.

**DivMMC and DivIDE.** esxDOS 0.8.9 is in `roms/`. The card (an SD image, `.img`) or the disks (`.img`, `.vhd` or `.hdf`) go in
the slots "DivMMC card / IDE disk 1" and "IDE disk 2" on the Media page (`media.card.image`, `media.card2.image`), and the
interface is chosen with Interface; the machine restarts and esxDOS shows its start-up lines. A card needs a FAT16 or FAT32
partition with the SYS and BIN folders of the esxDOS distribution on it (esxdos.org); the Media page's "New blank card" makes an
empty image (64 MiB, 256 MiB or 1 GiB) to fill from a computer, and in a source checkout `python tools/make_card.py card.img`
makes a card with esxDOS already on it. esxDOS loads and saves from the card (`LOAD *"name"`, `SAVE *"name"`), has dot commands
(`.ls` lists the card) and a menu on the NMI key (F10).

**Dandanator.** Put a 512 KiB ROM set (`.rom` or `.bin`) in the slot "Dandanator ROM set" (`media.romset.image`) and set Interface
to Dandanator: the machine starts into the cartridge's menu, a program starts by its key, and F10 is the cartridge's button. With
"Dandanator programming" on, the machine starts in its own BASIC and a writer program loaded from tape writes the set's file. An
empty file in the slot becomes a blank set.

### Printers and serial ports

A machine's printer and serial lines are connected to something on the computer ZetRo runs on; until you say what, nothing is.
The Ports page lists the ports of the machine that runs, each with its connection; they are written to the `ports` section of
`zetro.yaml` and take effect at once:

```
ports:
  printer: file:printer.out       # what is printed is appended to the file
  serial: tcp-listen:2323         # waits for a program (a terminal, telnet 127.0.0.1 2323) to connect
```

An end is `none`, `loopback` (a serial port wired back to itself), a serial port of the computer (`COM3`, `/dev/ttyUSB0`),
`pipe:NAME`, `tcp:HOST:PORT`, `tcp-listen:[HOST:]PORT` or `file:FILE`; [ports](#ports) below has the details.

### Starting each machine

| Machine | At the start |
|---|---|
| JET, HC-85/90/91, HC-2000, TIM-S, CoBra (in BASIC) | The machine's BASIC; `LOAD ""` loads the tape |
| HC with Interface 1 | `FORMAT "d";1` formats a blank disk image in drive A on the HC-85; `CAT 1`, `SAVE *"d";1;"name"`, `LOAD *"d";1;"name"`. On the HC-90 and HC-91 set Startup to CP/M (Interface 1 comes on with it) and put a CP/M system disk in drive A, or type `PRINT USR 14446` on the HC-91; `DIR` lists its files |
| HC-91 Plus | A menu in the manner of the 128K; with Interface 1 on, CP/M starts as on the HC-91 |
| HC-2000 | Startup CP/M starts from the system disk in drive A |
| HC-88 | Starts CP/M from the system disk in drive A; `BASIC` starts the cassette interpreter |
| CIP-03, CIP-02 | The `Basic-S` prompt: press Enter, then type as on a 48K Spectrum |
| CIP-01 | A copier with the menu L (launch), I (load), S (save), V (verify): press L, Enter, Enter and play the BASIC cassette; then `LOAD ""`. With Auto-load on, a tape put in while the copier is on the screen has the copier's keys typed, and once BASIC runs a tape has `LOAD ""` typed without a restart |
| CIP-04 | The +3's menu: Enter loads from tape or disk; slot `a` takes a DSK image |
| TIM-S | Starts from its ROM, copies it into RAM and runs from there; F10 is the NMI key |
| TIM-S Plus | A menu with Loader, +3 BASIC, Calculator and 48 BASIC; put a blank disk image in drive A and use `SAVE "a:name"`, `LOAD "a:name"` and `CAT` |
| CoBra | The bootstrap's menu: B for BASIC, C for the cassette loader, D for CP/M from the disk in drive A; the Startup option presses the key for you |
| CUB Z | Its monitor (`cubZ V1.4`, prompt `.`) boots CP/M from the disk in drive A; with a disk put in later, reset, or type `GF800` and Enter |
| aMIC | A monitor with a `*` prompt: `G0800` and Enter start BASIC; `LOA` loads from the tape, `SAV` saves, `RUN` runs |
| PRAE-M | Enter for the local console, Enter past the welcome page and Enter at HIGHEST MEMORY?; then `ALOAD "NAME"` loads a program from the tape, `LOAD "NAME" &6000` a memory image and `ASAVE "NAME"` saves. With Auto-load on, a tape put in has the command typed with the name read from its header |

### Scripting

`zetro --hidden --script FILE` runs a file of commands without opening a window, and `zetro --control stdin` (or `tcp:PORT`)
takes the same commands a line at a time and answers each with a line of JSON: typing, keys, media, screenshots, the text on the
screen, saved states, snapshots, cheats, rewind, speed, memory. A hidden run can drive the setup screen too (`--setup-page`,
`--hostkey`) and save what the window shows with `--screenshot`. `zetro --help` lists every switch and command, and the
[command line](#command-line) and [harness commands](#harness-commands) below describe them.

## Configuration

ZetRo reads its settings from `zetro.yaml` in the working folder (or the file named by `--config FILE.yaml`). A
folder without one gets a file with the default settings and a comment on the main ones, written on the first start. The
setup screen (F12) writes its changes back to the file and keeps every comment. A setting that is not in the file has its
default, so a file may list only what it changes; a key the program does not know, or a value it cannot use, stops the
start with the line of the file and what was expected.

Paths (tapes, disks, card images, output files) may be absolute or relative; a relative one is relative to the folder of the
configuration file, not to the working folder. The `states`, `snapshots` and `media` folders ZetRo writes are beside the
configuration file too.

The tables list every key with its values and its default. A setting marked "not original" is something the real machine
did not have: it is marked with `*` on the setup screen and can be switched off. Options marked "restarts" build the machine
again when the setup screen is left (after asking); the others act at once.

### machine

```yaml
machine:
  model: hc91
  options:
    joystick: kempston
    ay: mono
```

| Key | Values | Default | Meaning |
|---|---|---|---|
| `machine.model` | a model's id | `test` | The computer that runs. `zetro --list-models` names the ids; the table below lists them. A change restarts the machine, with the options of the new model at their defaults |
| `machine.options.KEY` | the option's values | the option's default | An option of the model, by its key. The options belong to the model: a key the model does not have, or a value the option does not take, is refused |

| Model | Id | Family of options |
|---|---|---|
| JET | `jet` | JET |
| HC-85, HC-90, HC-91 | `hc85`, `hc90`, `hc91` | HC |
| HC-91 Plus (HC-128) | `hc128` | HC-91 Plus |
| HC-2000 | `hc2000` | HC-2000 |
| HC-88 | `hc88` | HC-88 |
| CIP-01, CIP-02, CIP-03 | `cip01`, `cip02`, `cip03` | CIP |
| CIP-04 | `cip04` | CIP-04 |
| TIM-S, TIM-S Plus | `tims`, `timsplus` | TIM-S |
| CoBra 80K, CoBra 64K | `cobra80`, `cobra64` | CoBra |
| CUB Z monitor 1.4, 1.3 | `cubz14`, `cubz13` | CUB Z |
| aMIC | `amic` | aMIC |
| PRAE-M BASIC 3.6, 3.5 | `prae36`, `prae35` | PRAE |
| Test screen | `test` | Test (needs no ROM) |

#### What applies and what is required

An option, a media slot or a port that belongs to something the machine does not have is not there: the Interface 1's
drives, serial port and serial speed without Interface 1; a card or a ROM set without the interface that uses it. The setup
screen does not list such things; the file may still name them (they are kept and ignored). An option another setting needs
is set with it, whatever the file says, and the setup screen shows it and does not let it be changed: `startup: cpm` on an
HC fits `interface1: on`, and Interface 1 excludes an added interface (`interface` is then `none`). The settings of a byte
port (`serial_baud`, `serial_bits`, `uart_channel`, `printer`, `user_port`, `parallel`) are options of the machine in the
file and rows of the Ports page on the setup screen.

A machine set to start from a disk (an HC with `startup: cpm`, the HC-88, the CUB Z, a CoBra with `startup: cpm`) needs its
system disk named for drive A (`media.a.image`): none comes with ZetRo.

#### Options of the Spectrum-compatible machines

These are the options the JET, the HC family, the CIP family, the TIM-S and the CoBra share. The column Models says which
machines have each; the other columns are the same on all of them unless the Default column says otherwise.

| Key | Values | Default | Models | Meaning |
|---|---|---|---|---|
| `joystick` | `none`, `sinclair1`, `sinclair2`, `cursor`, `fuller`, `kempston` | `kempston` | JET, HC, HC-91 Plus, HC-2000, CIP-01/02/03, TIM-S, CoBra | What kind of joystick the first socket holds: Sinclair I (keys 6-0), Sinclair II (1-5), Cursor (5-8 and 0), Fuller or Kempston. The setup screen marks with `*` the kinds the original socket was not wired for. The JET's socket is a Kempston port; the HC, CIP and TIM-S sockets are wired to the keys 1-5 and 6-0. What drives it is `controllers.joystick1` |
| `joystick2` | the same | `none` | HC, HC-91 Plus, HC-2000, CIP-01/02/03, TIM-S | The second socket |
| `timing` | `original`, `spectrum` | `original` | JET, HC, HC-91 Plus, HC-2000, CIP, CIP-04, TIM-S | "Spectrum timing", not original, restarts: `spectrum` gives the machine the contended memory, floating bus and frame timing of Sinclair's own Spectrum (48K, or 128K on the 128K-style machines), for programs timed to it; the clones have none |
| `ay` | `off`, `mono`, `abc`, `acb` | `off` | JET, HC, HC-2000, CIP-01/02/03, TIM-S (not the Plus), CoBra | The AY-3-8912 sound board, mono or stereo ABC or ACB. Not original. Restarts. The HC-91 Plus and the CIP-04 have an AY of their own, which cannot be taken out: their `ay` is `mono` (the default), `abc` or `acb` |
| `interface` | `none`, `dandanator`, `divide`, `divmmc` | `none` | JET, HC, HC-91 Plus, HC-2000, CIP, CIP-04 | An added interface, not original, restarts: the ZX Dandanator Mini cartridge, the DivIDE (two IDE disks) or the DivMMC (an SD card), the last two with esxDOS. They take the ROM at 0000h, so they exclude the HC's Interface 1. Their files are the media slots `romset`, `card` and `card2` |
| `dandanator_program` | `off`, `on` | `off` | the same | Not original, restarts: with the Dandanator, `on` starts the machine in its own BASIC with the cartridge's flash writable, so that a writer program loaded from tape can program it |

#### Options of the HC family

| Key | Values | Default | Models | Meaning |
|---|---|---|---|---|
| `interface1` | `off`, `on` | `off` | HC-85, HC-90, HC-91, HC-91 Plus | Interface 1: two floppy drives (media `a`, `b`), the serial port and the network, with its own EPROM. Restarts. `on` whatever the file says while `startup` is `cpm`. The HC-2000 has its drives built in |
| `startup` | `basic`, `cpm` | `basic` | HC-90, HC-91, HC-91 Plus, HC-2000 | What the machine starts: its BASIC, or CP/M from drive A (Interface 1 comes on with it, except on the HC-2000, which has its drives built in). Restarts |
| `drive_sound` | `off`, `on` | `on` | HC, HC-91 Plus, HC-2000 | The drives are heard: motor, steps, the click of the mechanism. Synthesised from what the drives do, to the measure of recordings of a real drive; no recording is played. Restarts |
| `serial_baud` | `110`, `300`, `600`, `1200`, `2400`, `4800`, `9600`, `19200` | `9600` | HC, HC-91 Plus | Not original, restarts: the rate at which the Interface 1's serial port turns the firmware's bit-banged line into bytes for the host (`ports.serial`) |

#### Options of the HC-88

| Key | Values | Default | Meaning |
|---|---|---|---|
| `drive_sound` | `off`, `on` | `on` | Not original: the drives are heard |
| `floppy` | `original`, `core` | `core` | Not original, restarts: the floppy controller as the schematic has it (`original`), or as the MiSTer core's replacement BIOS expects it (`core`) |
| `serial_baud` | `300`, `600`, `1200`, `2400`, `4800`, `9600`, `19200`, `38400` | `9600` | The bit rate of the serial line for the host |

#### Options of the CIP family

| Key | Values | Default | Models | Meaning |
|---|---|---|---|---|
| `startup` | `basic`, `copier` | `basic` | CIP-02 | Restarts: the BASIC ROM, or the copier (loads BASIC from tape like the CIP-01); which ROM the real CIP-02 selects is unverified |
| `reset_latch` | `kept`, `cleared` | `kept` | CIP-01, CIP-02, CIP-03 | `kept`: the reset button leaves BASIC where it is, as the real board does; `cleared` (not original): a reset returns to the ROM, as the MiSTer core does |

The CIP-04 has the options `drive_sound` (`on`, restarts), `serial_baud` (as the HC's, `9600`, not original,
restarts), `ay` (`mono`, `abc` or `acb`, default `mono`; it has one), `timing`, `interface` and `dandanator_program` as
above, and `joystick` and `joystick2` as the other CIPs have them. The media slots are `tape`, `a` (one floppy),
`romset`, `card` and `card2`; the ports `serial` and `printer`.

#### Options of the TIM-S

| Key | Values | Default | Models | Meaning |
|---|---|---|---|---|
| `cpu_switch` | `3.5`, `6`, `7`, `8` | `3.5` | TIM-S, TIM-S Plus | Restarts: the processor speed in MHz the machine runs at after its start; the picture, the tape and the sound keep their rates. The ROM's own tape loader needs 3.5 MHz unless `tape_traps` is on |
| `plus_ay` | `stereo`, `mono` | `stereo` | TIM-S Plus | The AY's output |
| `uart_channel` | `native`, `exp1`, `exp2`, `exp3`, `exp4` | `native` | TIM-S Plus | Which of the five serial channels is the serial port (`ports.serial`). Restarts |
| `printer` | `virtual`, `physical` | `virtual` | TIM-S Plus | `virtual`: the printer is always ready; `physical`: it is busy while the host's end is off line (`ports.printer`). Restarts |
| `drive_sound` | `off`, `on` | `on` | TIM-S Plus | Not original, restarts: the drives are heard |

The TIM-S also has `joystick`, `joystick2`, `timing` and `ay` as in the table above; the TIM-S Plus has `joystick` and
`joystick2` (its AY is `plus_ay`).

#### Options of the CoBra

| Key | Values | Default | Meaning |
|---|---|---|---|
| `col80` | `off`, `on` | `on` | Restarts: the 80-column display modification (the PC5 line) is fitted; `off` shows the machine without it |
| `parallel` | `off`, `on` | `off` | Restarts: the external parallel input lines on port B |
| `startup` | `menu`, `basic`, `cassette`, `cpm` | `menu` | Restarts: the bootstrap's menu, or the key held for it at power on (B for BASIC, C for the cassette loader, D for CP/M from drive A) |
| `drive_a` to `drive_d` | `5in`, `8in` | `5in` | Restarts: the size of each of the four drives (5.25 inch or 8 inch) |
| `drive_sound` | `off`, `on` | `on` | Not original, restarts: the drives are heard |

The CoBra also has `joystick` and `ay` as above (no `joystick2`, `timing` or `interface`).

#### Options of the CUB Z

| Key | Values | Default | Meaning |
|---|---|---|---|
| `monitor` | `white`, `green` | `white` | Restarts: the colour of the display |
| `user_port` | `serial_b`, `pio_a` | `serial_b` | Restarts: what the user connector carries: the second serial line or the user PIO's port A |
| `drive_sound` | `off`, `on` | `on` | Not original, restarts: the drives are heard |

#### Options of the aMIC and the PRAE-M

| Key | Values | Default | Models | Meaning |
|---|---|---|---|---|
| `memory` | `32`, `16`, `48` | `32` | aMIC | Restarts: RAM in KiB |
| `serial_baud` | `1200`, `600`, `300` on the aMIC; `300`, `600`, `1200`, `2400`, `4800`, `9600`, `19200` on the PRAE-M | `1200` on the aMIC, `300` on the PRAE-M | aMIC, PRAE-M | The aMIC's baud switch (restarts); on the PRAE-M, not original: the rate at which the program's software serial line is turned into bytes |
| `serial_bits` | `7`, `8` | `7` | PRAE-M | Not original: the bits of a character on that line |
| `monitor` | `white`, `green` | `white` | aMIC, PRAE-M | The colour of the lit dots |

The test screen (`machine.model: test`) has one option, `beep` (`off`, `on`, default `on`): a beep on a key.

### media

What is in the model's slots. Which slots a model has depends on it; a slot the model does not have is refused, and the setup
screen's Media page lists the slots of the running model. An empty `image` is an empty slot.

```yaml
media:
  tape:
    image: media/game.tzx
    write_protect: false
    fast: true
    sound: true
    autoload: true
```

| Slot | Models | Takes | Meaning |
|---|---|---|---|
| `tape` | all but CUB Z | `.tap`, `.tzx`, `.pzx`, `.csw`, `.wav` | The tape recorder. TAP, TZX, PZX, CSW and WAV are read; TAP, TZX and PZX are written. A SAVE records to the file; recording (Shift+F5) into an empty slot starts a new `media/tape-NNNN.tzx` |
| `a`, `b` | HC-85/90/91 with Interface 1, HC-91 Plus, HC-2000, HC-88, TIM-S Plus, CoBra, CUB Z | `.dsk`; also `.edsk` on the HC-2000, CoBra and CUB Z, and `.img` on the HC-2000, HC-88 and CUB Z | Floppy drives A and B. The image's geometry gives the drive (40 or 80 tracks) |
| `c`, `d` | CoBra | `.dsk`, `.edsk` | The CoBra's third and fourth drives |
| `a` | CIP-04 | `.dsk` | The CIP-04's one floppy drive |
| `card` | JET, HC, HC-91 Plus, HC-2000, CIP, CIP-04 | `.img`, `.vhd`, `.hdf`, `.mmc` | The DivMMC's SD card, or the DivIDE's first IDE disk (`interface: divmmc` or `divide`) |
| `card2` | the same | the same | The DivIDE's second IDE disk |
| `romset` | the same | `.rom`, `.bin` | The Dandanator's 512 KiB ROM set (`interface: dandanator`); an empty file becomes a blank set |

| Key | Values | Default | Meaning |
|---|---|---|---|
| `media.SLOT.image` | a file, or empty | empty | The file in the slot |
| `media.SLOT.write_protect` | `true`, `false` | `false` | The disk, card or set is write protected: the guest's writes are refused. A disk or card that is not protected is written back as the guest writes it, so keep a copy of what you care about |
| `media.SLOT.fast` | `true`, `false` | `true` | Fast access, not original: the machine runs at full host speed while this slot works (the tape runs, the disk's motor turns). A tape is also loaded and saved by the fast means its machine has (the setup screen's "Fast loading"): on the Spectrum-compatible machines a block is loaded at once when the ROM's own loader is called, as far as the ROM has the standard routine, and SAVE writes its block to the tape file at once; there and on the aMIC and the PRAE-M, when a program waits on the tape edge by edge, guest time jumps to the next edge (no table of known loaders). `false`: the tape loads as on the machine itself |
| `media.tape.sound` | `true`, `false` | `true` | The tape's signal is heard while it loads or records |
| `media.tape.autoload` | `true`, `false` | `true` | Not original: a tape put into the machine (dropped on the window, or put in on the Media page) is loaded at once: the machine restarts, types its own load command and starts the tape |
| `media.tape.full_speed`, `traps`, `save_traps`, `accelerate` | `true`, `false` | `true` | The parts of a tape's `fast`, for a program one of them does not suit: full host speed while the tape runs, the ROM's loader done at once, the ROM's SAVE done at once, the waits of other loaders skipped. Each acts only while `fast` is on; the setup screen has the one switch |

A file written before 0.13.0 has these as `machine.options.tape_sound`, `tape_traps`, `tape_save_traps`, `tape_accelerate`
and `ui.autoload`: they are still read, and moved here when the setup screen next writes the file.

### video

| Key | Values | Default | Meaning |
|---|---|---|---|
| `video.look` | `plain`, `crt` | `crt` | The picture as it is, or through the M-CRT shader, which imitates a cathode-ray tube monitor (without OpenGL the picture is plain whatever this says) |
| `video.aspect` | `original`, `square` | `original` | The picture's shape: 4:3 as on the monitor, or square pixels |
| `video.scale` | `fit`, `integer` | `fit` | Fit the window, or whole multiples of the picture only |
| `video.border` | `full`, `medium`, `minimal`, `none` | `full` | How much of the border around the machine's screen is shown: all the frame the monitor showed, 16 lines of it (`medium`), 5 lines (`minimal`) or none; at the sides as wide as that. The setup screen follows it |
| `video.filter` | `nearest`, `linear`, `sharp` | `sharp` | How the plain look's picture is scaled to the window: `nearest` repeats pixels (uneven at scales that are not whole), `linear` is smooth and blurred, `sharp` is crisp with soft edges only where a pixel straddles two screen pixels. The `crt` look does its own smoothing and ignores it |
| `video.fullscreen` | `true`, `false` | `false` | Start in full screen. Alt+Enter switches while running |
| `video.vsync` | `true`, `false` | `true` | Wait for the display's vertical sync |

### sound

| Key | Values | Default | Meaning |
|---|---|---|---|
| `sound.enabled` | `true`, `false` | `true` | `false` mutes the machine and the drives |
| `sound.volume` | `0` to `100` | `100` | The overall volume in percent |

The sound is dropped at full speed (F9).

### controllers

What drives each joystick socket of the machine, and the keyboard joystick's keys. A socket the model does not have is
ignored. What kind of joystick is in the socket is the machine's option `joystick`.

| Key | Values | Default | Meaning |
|---|---|---|---|
| `controllers.joystick1.device` | `none`, `keyboard`, `controller N` (1 to 8) | `keyboard` | What feeds the first socket: nothing, the keyboard joystick (the keys below) or the Nth host game controller (its d-pad or left stick, A for fire, B for a second button) |
| `controllers.joystick2.device` | the same | `none` | What feeds the second socket |
| `controllers.keys.up` | an SDL key name | `Up` | The keyboard joystick's key for up. The six keys are named as `--hostkey` takes them (`Up`, `A`, `Space`, `Left Ctrl`, `Keypad 8`); a name no key has, and the window's own keys (F5 to F12, Pause, Print Screen), are refused, and each function needs a key of its own |
| `controllers.keys.down` | an SDL key name | `Down` | The key for down |
| `controllers.keys.left` | an SDL key name | `Left` | The key for left |
| `controllers.keys.right` | an SDL key name | `Right` | The key for right |
| `controllers.keys.fire` | an SDL key name | `Left Alt` | The key for the fire button |
| `controllers.keys.fire2` | an SDL key name | `Left Ctrl` | The key for the second button |
| `controllers.keyboard_joystick` | `true`, `false` | `false` | The keys are the joystick from the start. F11 switches them on and off at any time; while they are the joystick they no longer reach the machine |

### ports

The host end of the byte ports of the machine (serial lines, printers), by the key of the port: `serial` (the HC's Interface
1, the HC-88, the CIP-04, the TIM-S, the CoBra, the CUB Z's first line, the aMIC and the PRAE-M), `printer` (the TIM-S and the
CIP-04) and `user` (the CUB Z's second line). Until you say what a port is connected to, nothing is: a TIM-S waits on
`LPRINT` for a printer that is not there. A key the running model has no port for is kept and ignored, so one file serves
every model. Changes on the Ports page of the setup screen take effect at once; the page also has the ports' own settings (the
serial speed and the like), which are options of the machine (`machine.options`) and restart it where their rows say so.

```yaml
ports:
  printer: file:printer.out
  serial: tcp-listen:2323
```

| End | Meaning |
|---|---|
| `none` | Not connected (the default) |
| `loopback` | A serial port's transmitter wired to its receiver, for testing; not for a printer |
| `COM3`, `serial:COM3`, `/dev/ttyUSB0` | A serial port of the host; its handshake lines are the machine's |
| `pipe:NAME` | A local pipe: `\\.\pipe\NAME` on Windows, a socket in `/tmp` on Linux. ZetRo creates it unless another program already has |
| `tcp:HOST:PORT` | A TCP connection ZetRo makes, again every second while nobody answers |
| `tcp-listen:PORT`, `tcp-listen:HOST:PORT` | A TCP server, one client at a time, on 127.0.0.1 unless a HOST such as `0.0.0.0` is given |
| `file:FILE` | What the machine sends is appended to the file (beside the configuration unless the name is a full path) |

A printer is on line while its end is open (a file always, a connection while it is up); a serial port whose end is a connection
is ready while the connection is. An end that cannot be opened (a port another program holds, a missing device) leaves the
port unconnected, and the message line says why. A value with a backslash or a colon is written in single quotes, as the setup
screen does (`'pipe:\\.\pipe\zetro'`).

| Key | Values | Default | Meaning |
|---|---|---|---|
| `ports.serial` | an end | `none` | The serial line |
| `ports.printer` | an end except `loopback` | `none` | The printer port |
| `ports.user` | an end | `none` | The CUB Z's user serial line |

### ui

| Key | Values | Default | Meaning |
|---|---|---|---|
| `ui.language` | `en`, `ro` | the host's language | The language of the setup screen and of the messages in the window: English or Romanian. The file a first start writes has `ro` on a system whose user language is Romanian, else `en`; the command line's help stays English |
| `ui.state_slot` | `1` to `10` | `1` | The slot F2 saves to and F3 loads from. The states are the files `states/MODEL/slot-N.zrs` beside the configuration |
| `ui.setup_page` | `machine`, `media`, `display`, `sound`, `controllers`, `ports`, `states`, `cheats`, `about` | `machine` | The page of the setup screen that was open last: F12 opens the screen on it. Written by the setup screen as the pages are turned |

### Command line

```
zetro [--config FILE.yaml] [--model ID] [--hidden [--realtime]] [--frames N] [--screenshot FILE.png]
      [--wav FILE.wav] [--script FILE] [--control stdin|tcp:PORT] [--drop FILE]
      [--setup-page machine|media|display|sound|controllers|ports|states|cheats|about] [--hostkey KEY@N]...
zetro --list-roms [--config FILE.yaml] | --list-models | --version | --help
```

| Switch | Meaning |
|---|---|
| `--config FILE.yaml` | The configuration file; without it `zetro.yaml` in the working folder, written with the defaults when missing |
| `--model ID` | Run that model instead of `machine.model`, with its options and media at their defaults |
| `--hidden` | Run with no window and no sound device, the same on any host; it never opens a window on the desktop |
| `--realtime` | With `--hidden`: pace the run to the clock as a visible window is |
| `--frames N` | Stop after N frames |
| `--screenshot FILE` | Save the last frame, as the window shows it, when the run ends (a `.png` file) |
| `--wav FILE.wav` | Record the sound to a file instead of playing it |
| `--script FILE` | Run a file of harness commands, one a line, and end the run after the last; the exit status is 1 at the first command that fails |
| `--control stdin`, `--control tcp:PORT` | Take the harness commands on standard input or on a local TCP port, answering each with a line of JSON; with `stdin` standard output carries only the answers |
| `--drop FILE` | Put a file in as if it had been dropped on the window (a tape is then auto-loaded) |
| `--setup-page PAGE` | Open the setup screen on a page at the start |
| `--hostkey KEY@N` | Press a host key after N frames: an SDL key name (`F12`, `Down`, `Return`, `PageUp`, `A`), with `ctrl+`, `shift+` or `alt+` in front, and `+` or `-` before all of it for only the press or only the release (`+F4@100 -F4@130` holds F4). At most 256 |
| `--list-roms` | List the ROMs ZetRo knows (recognised by the SHA-256 of their content, whatever the file name) and where each was found: in `roms/` in the working folder, beside the configuration and beside the program |
| `--list-models` | List the models by family |
| `--version`, `--help` | The version; the usage |

A hidden run can drive the setup screen: `zetro --hidden --setup-page display --hostkey Down@2 --hostkey PageDown@3 --frames 5
--screenshot display.png`.

### Harness commands

The commands of `--script` (one a line; empty lines and lines starting with `#` are skipped) and of `--control`. A script stops
at the first command that fails, with its line number, and the run ends with status 1. On the control channel a command is
answered when it is done with `{"ok":true,"frame":N}`, plus `"text"` or `"data"` where the command has a result, or
`{"ok":false,"error":"..."}`. The machine stands still between commands when the run is headless, so a run is the same every
time.

| Command | |
|---|---|
| `wait SECONDS` | Run guest time (a decimal number of seconds) |
| `frames N` | Run N frames |
| `type TEXT` | Type TEXT on the machine's keyboard (`\n` is Enter, `\b` delete, `\\` a backslash) |
| `enter` | The same as `type \n` |
| `key NAME`, `release NAME` | Press and release a key of the machine's keyboard (`key A`, `key CAPS SHIFT`) |
| `joy N [up] [down] [left] [right] [fire] [fire2]` | Joystick N (from 0) holds these directions and buttons; none releases it |
| `insert SLOT FILE`, `eject SLOT` | Put a file into a media slot, or take it out |
| `drop FILE` | Put a file in as a drop on the window does (a tape is then auto-loaded) |
| `play FILE` | A tape into the first tape slot, and its PLAY key |
| `tape play\|stop\|rewind\|next\|previous\|record` | The tape recorder's keys |
| `osk on\|off` | The on-screen keyboard opened or closed (it is drawn into `shot`) |
| `osk click KEY` | A click on a key's keycap, by the path a mouse click takes |
| `osk pad up\|down\|left\|right\|a\|b` | A game controller's d-pad, A or B on the on-screen keyboard; the answer is the key the selection is on |
| `osk keys` | One line for each keycap: x, y, width and height in the frame's pixels, then the key's name |
| `nmi` | The NMI button |
| `reset [power\|keys]` | The reset button; `power` a power cycle; `keys` the reset button with the keys as they are held |
| `shot FILE.png` | The frame as it is now, as a PNG |
| `text` | The characters on the screen, read back through the machine's ROM font (the setup screen's while it is open) |
| `expect TEXT` | Fails unless TEXT is on the screen now |
| `until SECONDS TEXT` | Runs until TEXT is on the screen; fails after SECONDS |
| `state save\|load FILE` | A saved state |
| `rewind on\|off` | Keep the rewind ring (a headless run does not until this) |
| `rewind SECONDS` | Back through the ring by at least SECONDS of guest time, then on from there |
| `speed full\|normal` | As fast as the host allows, or 100% |
| `peek ADDR [COUNT]` | Bytes of memory (hex address and count) |
| `poke ADDR BYTE...` | Write bytes (hex) |
| `trace FILE`, `trace off` | The I/O cycles and interrupts to a file |
| `snapshot load\|save FILE` | A snapshot file (`.sna`, `.z80` or `.szx`) put into the machine, or written from it |
| `cheat load FILE.pok` | A file of trainers, numbered from 1 |
| `cheat on N [VALUE]`, `cheat off N` | Trainer N on (VALUE, 0 to 255, for a trainer that asks), or off |
| `quit [STATUS]` | End the run with an exit status (0) |

## Limitations

What ZetRo does not do for a machine because the documents do not say, with where the machine's behaviour is described. Nothing
here is guessed: a part that is not known is not emulated.

### CIP-04

The CIP-04's ROM, the +3's hardware
(taken from published documents) and the ports its firmware drives are emulated; nothing
of it has been compared with a board, and the core's own validation is the ROM and its ports ("full CPU/AY/disk operation needs
physical testing").

- **The extra 128 KiB of RAM.** The CIP-04 installs 256 KiB (`MiSTerRO/docs/cip/models.md`); the +3 has 128 KiB. How the other 128 KiB
  is addressed is not documented (no schematic, no traced board), so the machine has the +3's eight pages only, and software that
  pages the rest sees the +3's.
- **Port 5FFDh.** The firmware writes 0 to it at startup and nothing else of it is known. It is a write latch with no effect of
  its own; the +3's decoding of 7FFDh also answers it, so the write also sets 7FFDh to 0 (the value RESET leaves it at).
- **Drive B.** The +3 has a connector for a second, external drive; the CIP-04's documents and the MiSTer core have drive A only,
  and so has ZetRo.
- **The serial port's hardware.** The CIP-04's firmware receives through the AY's register 14 (bit 7 RXD, bit 2 CTS) as the +3's
  does, but its transmit routine writes its bits to bit 6 of port FE and waits on bit 7 of a read of it (the +3's sends through the
  AY). The MiSTer core's RTL decodes only the AY's lines. ZetRo serves both as one line; which the real board has is not known.
- **The printer's strobe edge.** The port sends its byte when 1FFDh's bit 4 rises; the MiSTer RTL starts it on the falling edge. The
  firmware's printing writes the bit once per character, so only the rising edge is in every sequence; a printer
  that latches on the other edge is not told apart.
- **The lock bit.** The documents do not say whether 7FFDh's bit 5 holds 1FFDh's paging bits; here it does, and the disk motor and
  printer strobe stay free.
- **The interrupt's length** with the +3's Spectrum timing is the 128K's 36 T-states (the documents do not give the +3's), and the
  Sinclair Wiki's and the FAQ's contention start (14361 and 14365) differ by four T-states; the FAQ's is used. The option is not
  original.
- **Hardware verification.** The startup, keyboard, tape, AY and disk of the real CIP-04 are unverified; so are the board's timing
  and sound levels.

### The TIM-S Plus

- The original 2 KiB bootstrap EPROM is not archived. ZetRo runs the replacement of this project's TIM-S core
  (`core-tims/assets/roms/plus-loader.asm`), which programs the board as the core's documents say and starts the unmodified
  English +3 system 4.1, not the Plus's own system: the Plus's CP/M and its own BASIC extensions are not there.
- The +3 system's `FORMAT "A:"` (and `"B:"`) ends with "Unsuitable media" on the Plus's 80-track drives: having read a disk of
  the Plus's 80-track format (the core's `blank.dsk`), the +3 system takes the drive for a double-track one, and it cannot put
  its own 40-track format on such a drive (it does not double-step; its code returns error 9 there). The +3 system reads and
  writes the 80-track disks: SAVE, LOAD and CAT work on a copy of `blank.dsk`. The Plus's own system, which formatted its disks,
  is the missing bootstrap's.
- The core's native diagnostic for the Plus is a replacement bootstrap; ZetRo takes its firmware by content hash, so the checks
  run the diagnostic's parts as programs from +3 BASIC instead.
- The network has no other end: F7h and F3h are there, the line stays idle and its NMI never comes.

### CUB Z

The CUB Z runs both original monitors and the original CP/M disk of this project's core.
What it does not have:

- **The IEEE-488 controller** of the GPZ board (8291, 8292, 8293). The manual's port table gives it no addresses and nothing of
  the firmware or the disk drives it; it is not emulated, as in the core.
- **The SIO's synchronous modes** (monosync, bisync, SDLC, the CRC, the hunt phase): the SIO runs its two lines asynchronously only,
  as the core's; the registers take the synchronous settings and keep them.
- **Which key of the cursor and editing blocks is which.** The keyboard's EPROM gives the codes of every matrix position, but which
  position is under which key is documented only for the keys whose codes say it (the alphanumeric block, the function keys, the
  numeric pad through the monitor's code table). The arrows, HOME and the 15 extra keys send E0h-EFh and F0h-F2h and are named by
  the code they send; BREAK (a line of its own on the keyboard's connector) is taken as the position that sends 80h.
- **The "Offset-Info" block** that the core's CP/M disk carries at its end (approximate sector positions written by an imaging
  tool) is not read and is dropped when the disk is written back; the tracks, the sectors, their statuses, the missing tracks, the
  oversized track and the weak sector's copies are kept.
- **The main PIO's port A** (the parallel connector J3) and **the floppy option latch** (port F3h): the documents do not say what
  is connected or what the latch's bits do; port A has no host end and the latch is kept without effect, as in the core.

### aMIC and PRAE-M

Both are emulated from this project's cores, the manuals and the ROMs; what
is not emulated, or not known:

- **The aMIC's external 8255 outputs.** The core routes the chip's 24 lines to the MiSTer user port. The machine interface has byte
  ports (serial, parallel), not a 24-line port, so a joystick on PA0-PA4 is all of it that is carried; the other lines read high and
  what the program writes to the chip goes nowhere.
- **The aMIC's firmware is a repaired image.** The archived dump's RAM probe stops BASIC from starting with 16 KiB and its BASIC's
  LOA and SAV call the monitor's cassette entries with other arguments, so LOA does not load a tape. ZetRo runs the dump with those
  36 bytes repaired; the unchanged dump is not shipped.
- **The PRAE-M's interrupt pulse.** The documents say only "50 Hz". The RTL makes the interrupt a pulse of 16 pixels (8 T-states) at
  raster line 256; a Z80 takes it only when an instruction ends inside it, so a program that runs long instructions can miss ticks.
  The pulse's width is nominal.
- **The PRAE-M's serial settings.** The firmware keeps the rate and the data bits in RAM (the start-up values are 300 baud, 7 bits,
  2 stop bits, no parity), so the board's options say which to read the line at; a program that changes them needs the options changed.
  Parity and the stop bits' count are not looked at when reading the line.
- **The PRAE-M's floppy.** The manual discusses a floppy interface as a future version and gives no circuit or boot software; none is
  emulated.
- **Hardware verification.** Both are checked against the documents, the ROMs and the core's runtime cases, not against a real machine:
  the monitor's frame (320 x 288), the sound levels, the phosphor green, the keyboard's pace and the idle level of the tape input are
  nominal.

### DivIDE and DivMMC (added interfaces, not original)

The two interfaces are emulated from the DivIDE's published memory and programming-model documents and run the real esxDOS 0.8.9
firmware. What is not there or not known:

- **The DivMMC's own document.** It was not available: its ports (E3h, E7h, EBh), the 16 banks of RAM, the reset behaviour and the
  SPI port's read-gives-the-byte-before rule are nominal, from the interface's known behaviour and the firmware's use of them.
- **The "allram" mode and the MAPRAM reset by a write to port DFh** of one CPLD version of the DivMMC, and the DivIDE's "RETURN",
  "WRITEENABLE" and extra bank bit of its newer GAL. Not implemented; esxDOS 0.8.9's firmware does not use them.
- **The EEPROM jumper and writing the firmware.** The EEPROM is the file in `roms/` and is never written (the real interface's
  EEPROM-write jumper is not offered); the automatic mapping always has an EEPROM to show.
- **The NMI menu on the JET.** esxDOS's NMI entry assumes the 48K ROM's one-byte `PUSH AF` at 0066h; the JET's ROM has `JP 386Eh`
  there, so the NMI key does not open the menu on the JET (the real interface would fail the same way). It opens on the HC-91
  (checked); the HC-85, HC-90, HC-2000, CIP-03 and CIP-04 have the same bytes at 0066h (not tried).
- **A card changed while esxDOS runs.** A file put in or taken out of a card slot while the machine runs does not reset it: esxDOS
  keeps what it found at start-up until the machine is reset.
- **Cards above 2 GiB** (SDHC) are supported by the storage model but not checked against the firmware; the second IDE drive is
  found by it and not used further.

### The release builds

- **ARM64.** The Windows ARM64 program is compiled but has not been run; the Linux ARM64 program's tests and a start of the test
  screen have run only under qemu user-mode emulation. No ARM64 machine has run either.
- **Real displays and sound.** The checks run headless: on Linux on SDL's offscreen video driver and dummy audio driver. The Linux
  programs have not been run in a window on a real X11 or Wayland display by the project's checks, and the Linux sound output
  (ALSA, PulseAudio) has been exercised only through SDL's dummy driver.
