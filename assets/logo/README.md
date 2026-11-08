# Stereo Loom logo

The masters are the SVGs from the Stereopticon logo set (`lib/Logos`):

- `stereo_loom_dark.svg`, `stereo_loom_light.svg`: the app icon on its rounded tile.
- `stereo_loom_mark_on_dark.svg`, `stereo_loom_mark_on_light.svg`: the mark alone,
  light threads for dark backgrounds and dark threads for light ones.
- `*_{1024,512,256}.png`: PNG copies rendered from them (the number is the width).
- `../stereo_loom.ico`: the exe and window icon (the dark tile); `../stereo_loom_light.ico`,
  the light tile, is in the exe too.
- `../tray_on_dark.ico`, `../tray_on_light.ico`: the tray icon, the mark trimmed to
  its edges, following Windows' light or dark taskbar.

To remake the PNGs and icons after changing an SVG, run `tools/logo/render_logo.py`
with the repo root as its argument (it renders with Microsoft Edge; needs Pillow).
