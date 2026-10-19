#!/usr/bin/env python3
"""Draws the background of O11 Session's Mac disk image, at 1x and 2x.

The window is 640 x 400 points. Finder draws the two icons on top, centred at the points
dmg_settings.py gives (O11Session.vst3 at 170,180 and the VST3 folder at 470,180). Finder
won't drop a bundle on a shortcut to a system folder, so there is no arrow: the text says to
open the folder first and drag the plug-in into its window. The plug-in in the image is
signed with a Developer ID and notarized, so there is no Terminal step.

  python3 make_dmg_background.py SANS.otf SANS-SEMIBOLD.otf OUT_DIR

writes OUT_DIR/dmg-background.png and OUT_DIR/dmg-background@2x.png (needs Pillow). The
PNGs in this folder were drawn with Inter.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 640, 400
LEFT, RIGHT, ICONS_Y = 170, 470, 180
BACKGROUND = (246, 246, 244)
TEXT = (29, 29, 31)
DIM = (110, 110, 115)


def draw(scale, sans, semibold):
    s = lambda v: round(v * scale)
    image = Image.new("RGB", (s(W), s(H)), BACKGROUND)
    d = ImageDraw.Draw(image)
    title = ImageFont.truetype(semibold, s(20))
    body = ImageFont.truetype(sans, s(13))

    def centred(y, text, font, fill):
        d.text((s(W / 2), s(y)), text, font=font, fill=fill, anchor="mm")

    centred(36, "Install O11 Session", title, TEXT)
    centred(64, "1. Double-click VST3 Plug-Ins to open the folder", body, TEXT)
    centred(84, "2. Drag O11 Session into that folder's window", body, TEXT)

    # the step after it, in the bottom third (Finder's labels end near y = 255)
    centred(310, "Then rescan plug-ins in your DAW", body, DIM)
    centred(332, "(in Ableton: Settings, Plug-Ins, Rescan)", body, DIM)
    return image


def main():
    sans, semibold, out = sys.argv[1:4]
    draw(1, sans, semibold).save(os.path.join(out, "dmg-background.png"), dpi=(72, 72))
    draw(2, sans, semibold).save(os.path.join(out, "dmg-background@2x.png"), dpi=(144, 144))


if __name__ == "__main__":
    main()
