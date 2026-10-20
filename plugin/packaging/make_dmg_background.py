#!/usr/bin/env python3
"""Draws the background of O11 Session's Mac disk image, at 1x and 2x.

The window is 640 x 400 points. Finder draws the two icons on top, centred at the points
dmg_settings.py gives (O11Session.vst3 at 320,180). There is no shortcut to the VST3 folder:
Finder won't drop a bundle on one, and its arrow badge reads as "drag here" (Leo). The text
says how to open the folder and drag the plug-in into its window. The plug-in in the image is
signed with a Developer ID and notarized, so there is no Terminal step.

  python3 make_dmg_background.py SANS.otf SANS-SEMIBOLD.otf OUT_DIR

writes OUT_DIR/dmg-background.png and OUT_DIR/dmg-background@2x.png (needs Pillow). The
PNGs in this folder were drawn with Inter.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H = 640, 400
ICON_X, ICON_Y = 320, 180
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
    centred(64, "1. In Finder, choose Go, Go to Folder, and type /Library/Audio/Plug-Ins/VST3", body, TEXT)
    centred(84, "2. Drag O11 Session from here into that folder's window", body, TEXT)

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
