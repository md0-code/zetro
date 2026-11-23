"""The pictures made from the logo, assets/zetro-logo.png (the emblem and the wordmark on a transparent ground).

    python tools/make_logo.py

Writes
  assets/zetro-logo-dark.png  the logo with a white wordmark, which the READMEs show on a dark page
  assets/zetro-icon.png   the emblem alone, 1024 x 1024, transparent ground: the application's icon
  assets/zetro.ico        the same in the sizes Windows asks for (16 to 256), put into zetro.exe by src/app/zetro.rc
  src/app/logo_data.h     the logo as the setup screen's About page draws it: a panel of 256 x 64 pixels, white ground, in
                          the logo's own colours with the shades of its smoothed edges (a palette of its own, which the
                          page's picture carries after the screen's sixteen)
  src/app/icon_data.h     the icon as the window carries it where the program file has none (64 x 64 ARGB)

Run again when the logo changes; the outputs are committed. Needs Pillow.
"""
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parents[1]
LOGO = ROOT / "assets" / "zetro-logo.png"
ICON_SIZES = [16, 24, 32, 48, 64, 128, 256]
PANEL_W, PANEL_H = 256, 64          # the About page's panel: the whole width of the screen, eight rows of cells
LOGO_W = 240                        # the logo inside it
WINDOW_ICON = 64
LOGO_COLORS = 48                    # the panel's palette: the logo's colours and the shades between them and the ground


def rgba(image: Image.Image) -> list[tuple[int, int, int, int]]:
    data = image.tobytes()
    return [tuple(data[i:i + 4]) for i in range(0, len(data), 4)]


def bbox(image: Image.Image, test) -> tuple[int, int, int, int]:
    """The box of the pixels `test` (r, g, b, a) holds for."""
    mask = Image.frombytes("L", image.size, bytes(255 if test(*p) else 0 for p in rgba(image)))
    return mask.getbbox()


def emblem(logo: Image.Image) -> Image.Image:
    """The coloured emblem (the wordmark is black), centred in a square with a margin."""
    small = logo.resize((logo.width // 8, logo.height // 8), Image.LANCZOS)
    left, top, right, bottom = (8 * v for v in bbox(small, lambda r, g, b, a: a > 128 and max(r, g, b) - min(r, g, b) > 60))
    mark = logo.crop((left - 8, top - 8, right + 8, bottom + 8))
    side = int(max(mark.size) * 1.08)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.alpha_composite(mark, ((side - mark.width) // 2, (side - mark.height) // 2))
    return square


def dark(logo: Image.Image) -> Image.Image:
    """The logo for a dark ground: the black wordmark in white, the emblem as it is."""
    r, g, b, a = logo.split()
    high = ImageChops.lighter(ImageChops.lighter(r, g), b)
    low = ImageChops.darker(ImageChops.darker(r, g), b)
    grey = ImageChops.subtract(high, low).point(lambda v: 255 if v <= 60 else 0)       # the wordmark and its edges
    white = Image.new("L", logo.size, 255)
    return Image.merge("RGBA", tuple(Image.composite(white, band, grey) for band in (r, g, b)) + (a,))


def panel(logo: Image.Image) -> Image.Image:
    """The logo on the white panel, smoothed (the full-size logo is laid on white first, so that its edges shade into the
    ground), in a palette of LOGO_COLORS colours."""
    cut = logo.crop(logo.getbbox())
    white = Image.new("RGBA", cut.size, (255, 255, 255, 255))
    white.alpha_composite(cut)
    height = round(cut.height * LOGO_W / cut.width)
    small = white.convert("RGB").resize((LOGO_W, height), Image.LANCZOS)
    ground = Image.new("RGB", (PANEL_W, PANEL_H), (255, 255, 255))
    ground.paste(small, ((PANEL_W - LOGO_W) // 2, (PANEL_H - height) // 2))
    return ground.quantize(colors=LOGO_COLORS, method=Image.MEDIANCUT, dither=Image.NONE)


def c_bytes(values: list[int], per_line: int = 32) -> str:
    lines = []
    for i in range(0, len(values), per_line):
        lines.append("    " + ",".join(f"0x{v:02X}" for v in values[i:i + per_line]) + ",")
    return "\n".join(lines)


def main() -> None:
    logo = Image.open(LOGO).convert("RGBA")
    mark = emblem(logo)
    icon = mark.resize((1024, 1024), Image.LANCZOS)
    icon.save(ROOT / "assets" / "zetro-icon.png", optimize=True)
    icon.save(ROOT / "assets" / "zetro.ico", sizes=[(s, s) for s in ICON_SIZES])

    dark(logo).save(ROOT / "assets" / "zetro-logo-dark.png", optimize=True)
    picture = panel(logo)
    pixels = list(picture.tobytes())
    flat = picture.getpalette()[:3 * LOGO_COLORS]
    colours = [flat[i] << 16 | flat[i + 1] << 8 | flat[i + 2] for i in range(0, len(flat), 3)]
    colours += [0xFFFFFF] * (LOGO_COLORS - len(colours))
    window = mark.resize((WINDOW_ICON, WINDOW_ICON), Image.LANCZOS)
    argb = [a << 24 | r << 16 | g << 8 | b for r, g, b, a in rgba(window)]
    words = "\n".join("    " + ",".join(f"0x{v:08X}u" for v in argb[i:i + 8]) + "," for i in range(0, len(argb), 8))
    text = f"""/* Made by tools/make_logo.py from assets/zetro-logo.png; do not edit. */
#ifndef ZR_LOGO_DATA_H
#define ZR_LOGO_DATA_H

#include <stdint.h>

/* The logo on a white panel for the setup screen's About page: a byte a pixel, each a colour of the panel's own palette
 * (0x00RRGGBB: the logo's colours and the shades of its smoothed edges). */
#define ZR_LOGO_W {PANEL_W}
#define ZR_LOGO_H {PANEL_H}
#define ZR_LOGO_COLORS {LOGO_COLORS}
static const uint32_t zr_logo_palette[ZR_LOGO_COLORS] = {{
{chr(10).join("    " + ", ".join(f"0x{c:06X}" for c in colours[i:i + 8]) + "," for i in range(0, len(colours), 8))}
}};
static const uint8_t zr_logo_pixels[ZR_LOGO_W * ZR_LOGO_H] = {{
{c_bytes(pixels)}
}};

#endif /* ZR_LOGO_DATA_H */
"""
    (ROOT / "src" / "app" / "logo_data.h").write_text(text, encoding="ascii", newline="\n")
    text = f"""/* Made by tools/make_logo.py from assets/zetro-logo.png; do not edit. */
#ifndef ZR_ICON_DATA_H
#define ZR_ICON_DATA_H

#include <stdint.h>

/* The emblem as the window's icon: ARGB, a transparent ground. */
#define ZR_ICON_SIZE {WINDOW_ICON}
static const uint32_t zr_icon_pixels[ZR_ICON_SIZE * ZR_ICON_SIZE] = {{
{words}
}};

#endif /* ZR_ICON_DATA_H */
"""
    (ROOT / "src" / "app" / "icon_data.h").write_text(text, encoding="ascii", newline="\n")
    # for a look at the result
    preview = picture.convert("RGB")
    out = ROOT / "work" / "logo"
    out.mkdir(parents=True, exist_ok=True)
    preview.resize((PANEL_W * 4, PANEL_H * 4), Image.NEAREST).save(out / "panel.png")
    print("assets/zetro-logo-dark.png, assets/zetro-icon.png, assets/zetro.ico, src/app/logo_data.h, src/app/icon_data.h; a look at the panel: work/logo/panel.png")


if __name__ == "__main__":
    main()
