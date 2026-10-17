#!/usr/bin/env python3
"""Draws the background of O11 Session's Mac disk image, at 1x and 2x.

The window is 640 x 400 points. Finder draws the two icons on top, centred at the points
dmg_settings.py gives (O11Session.vst3 at 170,180 and the VST3 folder at 470,180), so this
draws an arrow between them and the next step under them. The plug-in in the image is
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
ORANGE = (255, 125, 59)


def draw(scale, sans, semibold):
    s = lambda v: round(v * scale)
    image = Image.new("RGB", (s(W), s(H)), BACKGROUND)
    d = ImageDraw.Draw(image)
    title = ImageFont.truetype(semibold, s(20))
    body = ImageFont.truetype(sans, s(13))

    def centred(y, text, font, fill):
        d.text((s(W / 2), s(y)), text, font=font, fill=fill, anchor="mm")

    centred(42, "Drag O11 Session onto the VST3 folder", title, TEXT)

    # the arrow, between where the two icons end
    x0, x1, y, head = LEFT + 78, RIGHT - 78, ICONS_Y, 12
    d.line((s(x0), s(y), s(x1 - head), s(y)), fill=ORANGE, width=s(4))
    d.polygon([(s(x1), s(y)), (s(x1 - head - 2), s(y - 9)), (s(x1 - head - 2), s(y + 9))], fill=ORANGE)

    # the step after it, in the bottom third (Finder's labels end near y = 255)
    centred(310, "Then rescan plug-ins in your DAW", body, DIM)
    centred(332, "(in Live: Settings, Plug-Ins, Rescan)", body, DIM)
    return image


def main():
    sans, semibold, out = sys.argv[1:4]
    draw(1, sans, semibold).save(os.path.join(out, "dmg-background.png"), dpi=(72, 72))
    draw(2, sans, semibold).save(os.path.join(out, "dmg-background@2x.png"), dpi=(144, 144))


if __name__ == "__main__":
    main()
