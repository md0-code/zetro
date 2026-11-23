# Third-party components

Linked into the program:

| Component | Use | License |
|---|---|---|
| [SDL 2.32.10](https://www.libsdl.org/) | Window, OpenGL context, input and sound output; fetched at build time and linked statically | zlib |
| [libyaml 0.2.5](https://github.com/yaml/libyaml/tree/0.2.5) | YAML syntax of `zetro.yaml` (`src/third_party/libyaml`, tag commit `2c891fc7a770e8ba2fec34fc6b545c672beb37e6`); linked statically | MIT (`src/third_party/libyaml/LICENSE`) |
| Microsoft Visual C++ runtime | Linked statically into the released Windows programs | Microsoft's redistribution terms for Visual Studio |

The Linux programs link only the system's C library and math library; SDL loads the system's X11, Wayland, ALSA, PulseAudio and
OpenGL libraries when it runs, and none of them is shipped.

Shipped with the programs in the release folder (`roms/`), not part of ZetRo's source. No software for the machines and
no manuals are shipped. Each file's size,
SHA-256 and origin are recorded in the source repository (`tools/sources.lock.json`); the MiSTer repository these files come from says of them that the original
firmware, documentation scans and cassette recordings retain their original ownership, and that downloading them does not
relicense them as project code. ZetRo ships them as the MiSTer cores' own packages do, and claims no right to them.

| Data | Use | License |
|---|---|---|
| The machines' ROMs (`roms/`) | The firmware of each computer, as the MiSTer cores of this project archive it; `tools/fetch_roms.py` puts them in `roms/` and checks each against its SHA-256 | Their makers': the original dumps are preserved from the z80-romania.ro archive and the CoBra Project, and keep their owners' rights. The replacement bootstraps and loaders the MiSTer cores made (the TIM-S Plus bootstrap, the aMIC's repaired image) are GPL-3.0-or-later; the Spectrum +3's system ROM (`roms/plus3-system.rom`, the unmodified English +3 v4.1 image taken from the pinned MiSTer Spectrum firmware) is Amstrad's and ships with this notice |
| esxDOS 0.8.9 (`roms/esxmmc.bin`, `roms/esxide.bin`; the distribution unpacked in `work/esxdos/0.8.9/`) | The firmware of the DivMMC and the DivIDE (the 8 KiB images ESXMMC.BIN and ESXIDE.BIN), and the SYS and BIN folders of the distribution that a card needs (`tools/make_card.py`). Fetched by `tools/fetch_roms.py` from the distribution's own place, `esxdos.org/files/esxdos089.zip` (SHA-256 `d455888361cd13d455e362a4c841452b6f4568d30f9069443044affdd40de9a4`; the two images' hashes are in `src/core/romdb.c`), never committed | Freeware by Papaya Dezign (2005-2021; main code lordcoxis, NMI browser and more ub880d, boot logos K-0s and bracket; the full credits in the distribution's README.TXT). The distribution carries no licence text: its notice is "esxDOS v0.8.9 (c) 2005-2021 Papaya Dezign ... All rights perversed" (README.TXT and ESX089.NFO), and it is offered for download on esxdos.org. It is used and shipped as it is, unmodified, with that notice; ZetRo's source does not contain it |

## Notices

esxDOS 0.8.9 (`roms/esxmmc.bin`, `roms/esxide.bin`), unmodified: "esxDOS v0.8.9 (c) 2005-2021 Papaya Dezign".

The Spectrum +3 system ROM (`roms/plus3-system.rom`): Amstrad have kindly given their permission for the redistribution of their
copyrighted material but retain that copyright.

libyaml 0.2.5:

```
Copyright (c) 2017-2020 Ingy döt Net
Copyright (c) 2006-2016 Kirill Simonov

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Libretro API

The [Libretro API header](https://github.com/libretro/RetroArch/blob/v1.20.0/libretro-common/include/libretro.h) is from RetroArch v1.20.0.

```
Copyright (C) 2010-2024 The RetroArch team

Permission is hereby granted, free of charge,
to any person obtaining a copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
