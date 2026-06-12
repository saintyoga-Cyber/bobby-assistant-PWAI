#!/usr/bin/env python3
# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Generates the Claude-starburst branding assets for the watchapp.

Replaces the upstream Bobby pony artwork (PDC vector images/sequences and
PNG bitmaps) with a Claude-style starburst ("spark"), keeping every resource
name, viewbox size and memory format identical so no C code changes are
needed for the art swap.

Run from the repo root:  python3 tools/claude-brand/generate.py
Requires: pillow (PNGs only).
"""

import math
import os
import struct

APP = os.path.join(os.path.dirname(__file__), "..", "..", "app", "resources")

# Pebble GColor8 values (2 bits per channel: aarrggbb).
BLACK = 0xC0
WHITE = 0xFF
CLEAR = 0x00

# Claude brand orange (#D97757) quantised to the Pebble 64-colour palette:
# GColorSunsetOrange (#FF5555).
ORANGE_RGB = (255, 85, 85)

# Subtle per-ray length variation for an organic spark.
RAY_SCALES = [1.0, 0.90, 0.97, 0.88, 1.0, 0.92, 0.96, 0.88, 1.0, 0.90, 0.97, 0.90]
NUM_RAYS = 12
VALLEY_RATIO = 0.27


def spark_points(cx, cy, radius, rotation_deg=0.0):
    """A 12-ray starburst as one closed polygon (tip, valley, tip, ...)."""
    pts = []
    step = 360.0 / NUM_RAYS
    for k in range(NUM_RAYS):
        tip_angle = math.radians(k * step - 90 + rotation_deg)
        valley_angle = math.radians(k * step - 90 + step / 2 + rotation_deg)
        tip_r = radius * RAY_SCALES[k % len(RAY_SCALES)]
        pts.append((cx + tip_r * math.cos(tip_angle), cy + tip_r * math.sin(tip_angle)))
        pts.append((cx + radius * VALLEY_RATIO * math.cos(valley_angle),
                    cy + radius * VALLEY_RATIO * math.sin(valley_angle)))
    return pts


# --------------------------------------------------------------------------
# PDC writing (format verified against the original Bobby assets)
# --------------------------------------------------------------------------

def cmd_precise_path(points, stroke, stroke_width, fill, open_path=False):
    """A type-3 (precise path) draw command; coordinates are in 1/8 px."""
    out = struct.pack("<BBBBB", 3, 0, stroke, stroke_width, fill)
    out += struct.pack("<H", 1 if open_path else 0)
    out += struct.pack("<H", len(points))
    for x, y in points:
        out += struct.pack("<hh", round(x * 8), round(y * 8))
    return out


def cmd_list(cmds):
    return struct.pack("<H", len(cmds)) + b"".join(cmds)


def write_pdc_image(path, w, h, cmds):
    payload = struct.pack("<BBhh", 1, 0, w, h) + cmd_list(cmds)
    with open(path, "wb") as f:
        f.write(b"PDCI" + struct.pack("<I", len(payload)) + payload)
    print(f"wrote {path}")


def write_pdc_sequence(path, w, h, frames, play_count=0xFFFF):
    payload = struct.pack("<BBhhHH", 1, 0, w, h, play_count, len(frames))
    for duration_ms, cmds in frames:
        payload += struct.pack("<H", duration_ms) + cmd_list(cmds)
    with open(path, "wb") as f:
        f.write(b"PDCS" + struct.pack("<I", len(payload)) + payload)
    print(f"wrote {path}")


def spark_cmd(cx, cy, radius, rotation=0.0, stroke=BLACK, width=3, fill=WHITE):
    return cmd_precise_path(spark_points(cx, cy, radius, rotation), stroke, width, fill)


# --------------------------------------------------------------------------
# Vector assets
# --------------------------------------------------------------------------

def root_spark(path, w, h, width):
    r = min(w, h) / 2 - width - 1
    write_pdc_image(path, w, h, [spark_cmd(w / 2, h / 2, r, width=width)])


def sleeping_spark(path):
    # A small spark resting at the bottom left, with z z z drifting up.
    cmds = [spark_cmd(16, 34, 13, rotation=9)]
    for i, (zx, zy, zs) in enumerate([(28, 22, 4), (35, 14, 5), (43, 5, 6)]):
        z = [(zx - zs, zy - zs), (zx + zs, zy - zs), (zx - zs, zy + zs), (zx + zs, zy + zs)]
        cmds.append(cmd_precise_path(z, BLACK, 2, CLEAR, open_path=True))
    write_pdc_image(path, 50, 50, cmds)


def failed_spark(path):
    # A spark with a bold X struck across it (the X arms sit between rays).
    cmds = [spark_cmd(25, 25, 21)]
    arm = 18
    for a1, a2 in [(135, 315), (45, 225)]:
        p1 = (25 + arm * math.cos(math.radians(a1)), 25 + arm * math.sin(math.radians(a1)))
        p2 = (25 + arm * math.cos(math.radians(a2)), 25 + arm * math.sin(math.radians(a2)))
        cmds.append(cmd_precise_path([p1, p2], BLACK, 5, CLEAR, open_path=True))
    write_pdc_image(path, 50, 50, cmds)


def running_spark(path):
    # Loading spinner: the spark rotates one ray-period over 8 frames.
    frames = []
    for i in range(8):
        rot = (360.0 / NUM_RAYS) * i / 8
        frames.append((50, [spark_cmd(22.5, 12.5, 10.5, rotation=rot, width=2)]))
    write_pdc_sequence(path, 45, 25, frames)


def tired_spark(path):
    # Alarm ringing: the spark pulses.
    scales = [1.0, 0.94, 0.88, 0.94, 1.0, 1.06, 1.12, 1.06]
    frames = [(120, [spark_cmd(25, 25, 19 * s)]) for s in scales]
    write_pdc_sequence(path, 50, 50, frames)


# --------------------------------------------------------------------------
# PNG assets
# --------------------------------------------------------------------------

def render_spark_png(path, size, radius, fill_rgb, outline_rgb=None, outline_w=0,
                     bg_rgb=None, center=None):
    """Renders the spark supersampled, then snaps to hard-edged palette
    colours (Pebble bitmaps shouldn't contain semi-transparent pixels)."""
    from PIL import Image, ImageDraw

    ss = 8
    w, h = size
    cx, cy = center or (w / 2, h / 2)
    im = Image.new("RGBA", (w * ss, h * ss), (0, 0, 0, 0) if bg_rgb is None else (*bg_rgb, 255))
    draw = ImageDraw.Draw(im)
    pts = [(x * ss, y * ss) for x, y in spark_points(cx, cy, radius)]
    if outline_rgb and outline_w:
        draw.polygon(pts, fill=(*fill_rgb, 255), outline=(*outline_rgb, 255), width=outline_w * ss)
    else:
        draw.polygon(pts, fill=(*fill_rgb, 255))
    im = im.resize((w, h), Image.LANCZOS)

    # Snap every pixel to the nearest of the involved solid colours.
    palette = [fill_rgb] + ([outline_rgb] if outline_rgb else []) + ([bg_rgb] if bg_rgb else [])
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    src = im.load()
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            if a < 128 and bg_rgb is None:
                continue
            best = min(palette, key=lambda c: (c[0] - r) ** 2 + (c[1] - g) ** 2 + (c[2] - b) ** 2)
            dst[x, y] = (*best, 255)
    out.save(path)
    print(f"wrote {path}")


def main():
    img = lambda *p: os.path.join(APP, *p)

    # Launcher menu icon: the Claude spark in brand orange on transparency.
    render_spark_png(img("icons", "menu_icon_default.png"), (25, 25), 11.5, ORANGE_RGB)

    # Root screen mascot (drawn on the orange-accent background).
    root_spark(img("images", "root_screen", "pony.pdc"), 57, 59, width=4)
    root_spark(img("images", "root_screen", "pony~emery.pdc"), 84, 84, width=4)

    # Result-pane images and animations.
    sleeping_spark(img("images", "sleeping_pony.pdc"))
    failed_spark(img("images", "failed_pony.pdc"))
    running_spark(img("animations", "running_pony.pdcs"))
    tired_spark(img("animations", "tired_pony.pdcs"))

    # About-window picture. Colour: white spark w/ black outline on the brand
    # orange. B&W (diorite, 1BitPalette: exactly two colours, no alpha):
    # black spark on white.
    render_spark_png(img("images", "fence_pony_color.png"), (144, 104), 46,
                     (255, 255, 255), outline_rgb=(0, 0, 0), outline_w=1, bg_rgb=ORANGE_RGB)
    render_spark_png(img("images", "fence_pony_color~emery.png"), (200, 104), 46,
                     (255, 255, 255), outline_rgb=(0, 0, 0), outline_w=1, bg_rgb=ORANGE_RGB)
    render_spark_png(img("images", "fence_pony_bw.png"), (144, 104), 46,
                     (0, 0, 0), bg_rgb=(255, 255, 255))


if __name__ == "__main__":
    main()
