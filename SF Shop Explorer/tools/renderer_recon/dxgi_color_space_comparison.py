"""Build a visual UNORM-vs-sRGB sampling comparison from one shipped BC1 texture.

This is an analytical primary-mip probe, not a Starfield capture. It decodes
one BA2 texture identically for both columns, applies either UNORM sampling or
the DXGI sRGB transfer function, then writes those values to the same UNORM
output target under a fixed lighting factor. That models the format distinction
behind the recorded D3D12 color regression while holding texture and lighting
constant.

Example:
    python tools/renderer_recon/dxgi_color_space_comparison.py
    python tools/renderer_recon/dxgi_color_space_comparison.py \
        --ba2 "B:/SteamLibrary/steamapps/common/Starfield/Data/Starfield - Textures11.ba2" \
        --texture textures/weapons/regulator/regulator_decals_color.dds
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageStat

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DATA = Path("B:/SteamLibrary/steamapps/common/Starfield/Data")
DEFAULT_TEXTURE = "textures/weapons/breach/stock_tactical_color.dds"
LIGHTING = 0.85

sys.path.insert(0, str(ROOT / "tools" / "ba2_audit"))
from ba2lib import Ba2Archive  # noqa: E402


def srgb_to_linear(value: float) -> float:
    """Apply the DXGI sRGB SRV decode curve to a normalized channel."""
    if value <= 0.04045:
        return value / 12.92
    return ((value + 0.055) / 1.055) ** 2.4


def srgb_sample_to_unorm_byte(channel: int, lighting: float = LIGHTING) -> int:
    """sRGB SRV sample, common linear shader lighting, then UNORM RTV store."""
    linear_sample = srgb_to_linear(channel / 255.0)
    return max(0, min(255, round(linear_sample * lighting * 255.0)))


def unorm_sample_to_unorm_byte(channel: int, lighting: float = LIGHTING) -> int:
    """UNORM SRV sample, common linear shader lighting, then UNORM RTV store."""
    return max(0, min(255, round((channel / 255.0) * lighting * 255.0)))


def decode_bc1_rgb(data: bytes, width: int, height: int) -> Image.Image:
    """Decode the top BC1 mip to RGBA using the BC1 RGB565 palette rules."""
    blocks_x = (width + 3) // 4
    blocks_y = (height + 3) // 4
    expected = blocks_x * blocks_y * 8
    if len(data) < expected:
        raise ValueError(f"short BC1 primary mip: {len(data)} bytes, expected {expected}")
    pixels = bytearray(width * height * 4)

    def rgb565(value: int) -> tuple[int, int, int]:
        r = (value >> 11) & 31
        g = (value >> 5) & 63
        b = value & 31
        return (r * 255 + 15) // 31, (g * 255 + 31) // 63, (b * 255 + 15) // 31

    for by in range(blocks_y):
        for bx in range(blocks_x):
            offset = (by * blocks_x + bx) * 8
            c0, c1, selectors = struct.unpack_from("<HHI", data, offset)
            p0, p1 = rgb565(c0), rgb565(c1)
            if c0 > c1:
                palette = (p0, p1,
                           tuple((2 * p0[i] + p1[i] + 1) // 3 for i in range(3)),
                           tuple((p0[i] + 2 * p1[i] + 1) // 3 for i in range(3)))
                alphas = (255, 255, 255, 255)
            else:
                palette = (p0, p1,
                           tuple((p0[i] + p1[i]) // 2 for i in range(3)),
                           (0, 0, 0))
                alphas = (255, 255, 255, 0)
            for py in range(4):
                y = by * 4 + py
                if y >= height:
                    continue
                for px in range(4):
                    x = bx * 4 + px
                    if x >= width:
                        continue
                    index = (selectors >> (2 * (py * 4 + px))) & 3
                    at = (y * width + x) * 4
                    pixels[at:at + 4] = bytes((*palette[index], alphas[index]))
    return Image.frombytes("RGBA", (width, height), bytes(pixels))


def find_texture(archive: Path, key: str, data_dir: Path) -> tuple[Ba2Archive, str]:
    """Open the requested BA2 or search texture archives under --data."""
    if archive:
        candidate = archive if archive.is_file() else data_dir / archive
        ba2 = Ba2Archive(candidate)
        if not ba2.valid:
            raise ValueError(f"cannot parse BA2: {candidate}")
        actual = next((name for name in ba2.tex_files if name.endswith(key.lower())), None)
        if actual is None:
            raise ValueError(f"{key!r} not found in {candidate.name}")
        return ba2, actual

    for candidate in sorted(data_dir.glob("*.ba2")):
        if "texture" not in candidate.name.lower():
            continue
        ba2 = Ba2Archive(candidate)
        if not ba2.valid:
            continue
        actual = next((name for name in ba2.tex_files if name.endswith(key.lower())), None)
        if actual is not None:
            return ba2, actual
    raise ValueError(f"{key!r} not found in texture BA2s under {data_dir}")


def _font(size: int, bold: bool = False):
    candidates = (
        ["C:/Windows/Fonts/seguisb.ttf", "C:/Windows/Fonts/arialbd.ttf"] if bold else
        ["C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"]
    )
    for path in candidates:
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            pass
    return ImageFont.load_default()


def _fit(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    result = image.copy()
    result.thumbnail(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", size, (38, 43, 52, 255))
    canvas.alpha_composite(result, ((size[0] - result.width) // 2,
                                    (size[1] - result.height) // 2))
    return canvas


def _mean_luma(image: Image.Image) -> float:
    rgb = image.convert("RGB")
    stat = ImageStat.Stat(rgb)
    # Display-referred Rec.709 luma in the stored output bytes.
    return 0.2126 * stat.mean[0] + 0.7152 * stat.mean[1] + 0.0722 * stat.mean[2]


def render_comparison(source: Image.Image, out_path: Path, texture: str, archive: str,
                      dxgi: int) -> tuple[float, float]:
    if dxgi not in (71, 72):
        raise ValueError(f"expected BC1 DXGI 71/72; got DXGI {dxgi}")
    rgb = source.convert("RGB")
    unorm = rgb.point(lambda channel: unorm_sample_to_unorm_byte(channel))
    srgb = rgb.point(lambda channel: srgb_sample_to_unorm_byte(channel))

    width, height = 1520, 1120
    canvas = Image.new("RGB", (width, height), (18, 22, 30))
    draw = ImageDraw.Draw(canvas)
    title = _font(38, True)
    header = _font(27, True)
    body = _font(20)
    small = _font(17)
    draw.text((48, 30), "DXGI TEXTURE SAMPLING: UNORM vs sRGB", font=title, fill=(236, 241, 248))
    draw.text((50, 84), "Same BC1 weapon map · same mip · same shader light · same UNORM output target",
              font=body, fill=(162, 178, 198))
    draw.text((50, 122), f"{texture}    |    {archive}    |    DXGI {dxgi} (BC1)    |    {source.width}×{source.height}",
              font=small, fill=(127, 155, 184))

    panel_y, panel_w, panel_h, gap = 178, 704, 772, 28
    panels = [
        (48, "A · CURRENT PREVIEW: UNORM SRV", "Stored byte / 255 is used directly", unorm, (65, 192, 255)),
        (48 + panel_w + gap, "B · sRGB SRV COUNTERFACTUAL", "Hardware sRGB-decodes before shader", srgb, (255, 171, 88)),
    ]
    image_size = (620, 620)
    for x, label, detail, image, accent in panels:
        draw.rounded_rectangle((x, panel_y, x + panel_w, panel_y + panel_h), radius=18,
                               fill=(31, 37, 47), outline=(61, 72, 88), width=2)
        draw.rounded_rectangle((x + 1, panel_y + 1, x + panel_w - 1, panel_y + 60), radius=17,
                               fill=(42, 51, 65))
        draw.rectangle((x + 1, panel_y + 42, x + panel_w - 1, panel_y + 60), fill=(42, 51, 65))
        draw.text((x + 22, panel_y + 15), label, font=header, fill=accent)
        draw.text((x + 23, panel_y + 72), detail, font=body, fill=(207, 216, 228))
        preview = _fit(image.convert("RGBA"), image_size)
        canvas.paste(preview.convert("RGB"), (x + (panel_w - image_size[0]) // 2, panel_y + 112))
        draw.rectangle((x + 40, panel_y + 111, x + panel_w - 40,
                        panel_y + 113 + image_size[1]), outline=(87, 99, 116), width=1)
        mean = _mean_luma(image)
        draw.text((x + 24, panel_y + 750), f"Mean stored-output luma: {mean:.1f} / 255",
                  font=body, fill=(232, 237, 245))

    unorm_luma = _mean_luma(unorm)
    srgb_luma = _mean_luma(srgb)
    delta = unorm_luma - srgb_luma
    percent = (delta / unorm_luma * 100.0) if unorm_luma else 0.0
    draw.rounded_rectangle((48, 972, width - 48, 1088), radius=16, fill=(36, 48, 62))
    draw.text((72, 988), f"Measured in this fixed-light probe: sRGB output is {delta:.1f} luma levels lower ({percent:.1f}%).",
              font=header, fill=(255, 210, 142))
    draw.text((72, 1032),
              f"Both panels apply the same ×{LIGHTING:.2f} shader factor; sRGB sample = IEC 61966-2-1 decode; output values are stored directly to UNORM.",
              font=small, fill=(205, 215, 228))
    draw.text((72, 1060),
              "Analytical primary-mip probe, not a game screenshot: geometry, mip filtering, masks, tone mapping, and game lighting are excluded.",
              font=small, fill=(155, 172, 194))
    out_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(out_path)
    return unorm_luma, srgb_luma


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA,
                        help="Starfield Data directory used to search texture BA2 archives")
    parser.add_argument("--ba2", type=Path, default=None,
                        help="specific BA2 to inspect; when omitted, search --data texture archives")
    parser.add_argument("--texture", default=DEFAULT_TEXTURE, help="game-relative texture path")
    parser.add_argument("--out", type=Path, default=ROOT / "tools/renderer_recon/extracted/unorm_srgb_sample_comparison.png")
    args = parser.parse_args()
    try:
        archive, key = find_texture(args.ba2, args.texture, args.data)
        tf = archive.tex_files[key]
        if tf.dxgi not in (71, 72):
            raise ValueError(f"{key} is DXGI {tf.dxgi}; this focused probe requires BC1 DXGI 71/72")
        raw, error = archive.read_texture(key)
        if raw is None:
            raise ValueError(f"cannot decode primary mip: {error}")
        image = decode_bc1_rgb(raw, tf.width, tf.height)
        unorm_luma, srgb_luma = render_comparison(
            image, args.out, key, archive.path.name, tf.dxgi)
        print(f"source={archive.path}")
        print(f"texture={key} dimensions={tf.width}x{tf.height} DXGI={tf.dxgi} mipBytes={len(raw)}")
        print(f"output={args.out}")
        print(f"fixed-light mean luma: UNORM={unorm_luma:.2f} sRGB={srgb_luma:.2f} delta={unorm_luma - srgb_luma:.2f}")
        return 0
    except (OSError, ValueError, struct.error) as exc:
        print(f"[FAIL] {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())