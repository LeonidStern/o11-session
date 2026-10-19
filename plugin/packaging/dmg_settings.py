# dmgbuild settings for O11 Session's Mac disk image: the plug-in, a shortcut to the VST3 folder to
# open, and a background that says to open it and drag the plug-in in (make_dmg_background.py draws it).
#
#   dmgbuild -s dmg_settings.py -D bundle=path/to/O11Session.vst3 -D background=dmg-background.png "O11 Session" out.dmg
#
# dmgbuild finds dmg-background@2x.png next to the background and uses both.
#
# It makes a read-write image: CI then sets the bundle bit inside and compresses it (see the
# macos job in .github/workflows/o11session.yml).

bundle = defines["bundle"]  # noqa: F821 (dmgbuild provides defines)
background = defines["background"]  # noqa: F821

format = "UDRW"
filesystem = "HFS+"
files = [bundle]
symlinks = {"VST3 Plug-Ins": "/Library/Audio/Plug-Ins/VST3"}

# the same points as in make_dmg_background.py; Finder counts the window's y up from the bottom
# of the screen, so 400 puts it in the upper half of most screens
window_rect = ((200, 400), (640, 400))
icon_locations = {"O11Session.vst3": (170, 180), "VST3 Plug-Ins": (470, 180)}
icon_size = 112
text_size = 13
default_view = "icon-view"
show_status_bar = False
show_tab_view = False
show_toolbar = False
show_pathbar = False
show_sidebar = False
show_icon_preview = False
arrange_by = None
