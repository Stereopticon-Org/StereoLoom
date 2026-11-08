# Stereo Loom logo

- `stereo_loom_dark.svg`, `stereo_loom_light.svg`: the masters (1024 x 1024,
  rounded tile, transparent corners). From the Stereopticon logo set
  (Stereopticon Logo.pdf, page 8), redrawn as vectors.
- `stereo_loom_{dark,light}_{1024,512,256}.png`: PNG copies rendered from the SVGs.
- `stereo_loom_mark_on_dark.svg`, `stereo_loom_mark_on_light.svg`: the mark alone
  (no tile, the gaps cut out): light threads for dark backgrounds, dark threads
  for light ones, with PNG copies at the same sizes.
- `../stereo_loom.ico`: the exe and window icon (the dark tile).
  `../tray_on_dark.ico` and `../tray_on_light.ico`: the tray icon, following
  Windows' light or dark taskbar. All made from the 1024 PNGs.

To change it: edit `tools/logo/make_logo_svg.py` and run it with this folder as its
argument, then render the PNGs from the SVGs (any browser or Inkscape) and remake
the icons.
