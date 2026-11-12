#!/usr/bin/env python3
"""Generate a vkBasalt .cube using FrameColor's gain/gamma/contrast/saturation.

Usage: generate_vulkan_lut.py current.txt output.cube [size]
"""

import sys
import math
from pathlib import Path


def transform(value: float, contrast: float, gamma: float, gain: float = 1.0) -> float:
    contrast = min(1.0, max(0.0, contrast))
    gamma = min(2.50, max(0.50, gamma))
    gain = min(1.50, max(0.50, gain))
    color = min(1.0, max(0.0, value * gain)) ** (1.0 / gamma)
    smooth = color * color * (3.0 - 2.0 * color)
    return min(1.0, max(0.0, color * (1.0 - contrast) + smooth * contrast))


def apply_saturation(
    rgb: tuple[float, float, float], saturation: float | tuple[float, float, float]
) -> tuple[float, float, float]:
    levels = (saturation,) * 3 if isinstance(saturation, (int, float)) else saturation
    levels = tuple(min(2.0, max(0.0, level)) for level in levels)
    if levels == (1.0, 1.0, 1.0):
        return rgb
    luminance = rgb[0] * 0.2126 + rgb[1] * 0.7152 + rgb[2] * 0.0722
    return tuple(min(1.0, max(0.0, luminance + (channel - luminance) * levels[index]))
                 for index, channel in enumerate(rgb))


def warn(message: str) -> None:
    print(f"generate_vulkan_lut.py: {message}", file=sys.stderr)


def clamped(values: list[float], start: int, end: int, lo: float, hi: float, default: float, name: str) -> None:
    """Brings values[start:end] into lo..hi in place (a non-number becomes default), warning once."""
    changed = False
    for i in range(start, min(end, len(values))):
        value = values[i]
        fixed = default if not math.isfinite(value) else min(hi, max(lo, value))
        if fixed != value:
            values[i] = fixed
            changed = True
    if changed:
        warn(f"{name} outside {lo}..{hi}: clamped")


def read_values(path: Path) -> list[float]:
    """current.txt's numbers, made usable: the installer must not stop over a damaged or hand-edited file."""
    values: list[float] = []
    try:
        text = path.read_text()
    except (OSError, UnicodeDecodeError) as error:
        warn(f"can't read {path} ({error}): using the neutral look")
        text = ""
    for token in text.split():
        try:
            values.append(float(token))
        except ValueError:
            warn(f"not a number in {path}: {token!r}: using the neutral look")
            values = []
            break
    if len(values) < 6:
        if text.strip():
            warn("current.txt needs three contrast and three gamma values: using the neutral look")
        values = [0.0, 0.0, 0.0, 1.0, 1.0, 1.0]
    if len(values) == 11:
        warn("saturation has two values: using the first for all three")
        values = values[:10]
    clamped(values, 0, 3, 0.0, 1.0, 0.0, "contrast")
    clamped(values, 3, 6, 0.50, 2.50, 1.0, "gamma")
    clamped(values, 6, 9, 0.50, 1.50, 1.0, "gain")
    clamped(values, 9, 12, 0.0, 2.0, 1.0, "saturation")
    # The 13th value is the sharpen amount, the 14th the compare bypass and the
    # 15th the sharpen area (0: centre of each eye, 1: whole image). The Vulkan
    # layer applies them in the shader, so they do not change the LUT.
    clamped(values, 12, 13, 0.0, 3.0, 0.0, "sharpen")  # up to 300% (framecolor::kSharpenMax)
    for index, name in ((13, "bypass"), (14, "sharpen area")):
        if index < len(values) and values[index] not in (0.0, 1.0):
            warn(f"{name} must be 0 or 1: using {1.0 if values[index] > 0.5 else 0.0:g}")
            values[index] = 1.0 if math.isfinite(values[index]) and values[index] > 0.5 else 0.0
    return values


def main() -> None:
    if len(sys.argv) not in (3, 4):
        raise SystemExit(__doc__)
    values = read_values(Path(sys.argv[1]))
    size = int(sys.argv[3]) if len(sys.argv) == 4 else 33
    if not 2 <= size <= 65:
        raise SystemExit("LUT size must be between 2 and 65")
    contrast = values[:3]
    gamma = values[3:6]
    gain = values[6:9] if len(values) >= 9 else [1.0] * 3
    saturation = tuple(values[9:12]) if len(values) >= 12 else (values[9],) * 3 if len(values) == 10 else (1.0,) * 3
    lines = ["# FrameColor Vulkan test LUT", f"LUT_3D_SIZE {size}", "DOMAIN_MIN 0 0 0", "DOMAIN_MAX 1 1 1"]
    for blue in range(size):
        for green in range(size):
            for red in range(size):
                rgb = (red, green, blue)
                corrected = tuple(transform(rgb[i] / (size - 1), contrast[i], gamma[i], gain[i]) for i in range(3))
                transformed = apply_saturation(corrected, saturation)
                lines.append(" ".join(f"{channel:.7f}" for channel in transformed))
    Path(sys.argv[2]).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
