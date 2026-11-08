# Renders the logo SVGs to PNG (headless Edge, transparent background) and makes
# the icons from the 1024 PNGs (Pillow).
# Usage: python render_logo.py <repo root>
import sys, os, subprocess, tempfile, pathlib
from PIL import Image
root = pathlib.Path(sys.argv[1]).resolve()
logo = root / 'assets' / 'logo'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
NAMES = ['stereo_loom_dark', 'stereo_loom_light', 'stereo_loom_mark_on_dark', 'stereo_loom_mark_on_light']
tmp = pathlib.Path(tempfile.mkdtemp())
for n in NAMES:
    svg = (logo / f'{n}.svg').as_uri()
    for px in (1024, 512, 256):
        html = tmp / f'{n}_{px}.html'
        html.write_text(f'<html><body style="margin:0;background:transparent">'
                        f'<img src="{svg}" style="display:block;width:{px}px;height:{px}px"></body></html>')
        out = logo / f'{n}_{px}.png'
        subprocess.run([EDGE, '--headless', '--disable-gpu', '--hide-scrollbars',
                        '--default-background-color=00000000', '--allow-file-access-from-files',
                        f'--user-data-dir={tmp / "edge"}', f'--window-size={px},{px}',
                        f'--screenshot={out}', html.as_uri()], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert Image.open(out).size == (px, px), out
SIZES = [(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)]
for src, ico in (('stereo_loom_dark', 'stereo_loom.ico'), ('stereo_loom_light', 'stereo_loom_light.ico'),
                 ('stereo_loom_mark_on_dark', 'tray_on_dark.ico'), ('stereo_loom_mark_on_light', 'tray_on_light.ico')):
    Image.open(logo / f'{src}_1024.png').convert('RGBA').save(root / 'assets' / ico, sizes=SIZES)
print('done')
