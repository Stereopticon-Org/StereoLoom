# Renders the logo SVGs (assets/logo, the masters) to PNG with headless Edge on a
# transparent background, and makes the icons from the 1024 renders with Pillow.
# The tray icons are the bare mark trimmed to its edges and set in a square.
# Usage: python render_logo.py <repo root>
import sys, re, subprocess, tempfile, pathlib
from PIL import Image
root = pathlib.Path(sys.argv[1]).resolve()
logo = root / 'assets' / 'logo'
EDGE = r'C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe'
NAMES = ['stereo_loom_dark', 'stereo_loom_light', 'stereo_loom_mark_on_dark', 'stereo_loom_mark_on_light']
tmp = pathlib.Path(tempfile.mkdtemp())
for n in NAMES:
    src = logo / f'{n}.svg'
    vb = [float(v) for v in re.search(r'viewBox="([^"]+)"', src.read_text()).group(1).split()]
    for px in (1024, 512, 256):
        w, h = px, round(px * vb[3] / vb[2])
        html = tmp / f'{n}_{px}.html'
        html.write_text(f'<html><body style="margin:0;background:transparent">'
                        f'<img src="{src.as_uri()}" style="display:block;width:{w}px;height:{h}px"></body></html>')
        out = logo / f'{n}_{px}.png'
        subprocess.run([EDGE, '--headless', '--disable-gpu', '--hide-scrollbars',
                        '--default-background-color=00000000', '--allow-file-access-from-files',
                        f'--user-data-dir={tmp / "edge"}', f'--window-size={w},{h}',
                        f'--screenshot={out}', html.as_uri()], check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        assert Image.open(out).size == (w, h), out
SIZES = [(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)]
def square(im):
    im = im.crop(im.getbbox())
    side = max(im.size)
    sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
    sq.alpha_composite(im, ((side - im.width) // 2, (side - im.height) // 2))
    return sq
for src, ico, trim in (('stereo_loom_dark', 'stereo_loom.ico', False), ('stereo_loom_light', 'stereo_loom_light.ico', False),
                       ('stereo_loom_mark_on_dark', 'tray_on_dark.ico', True), ('stereo_loom_mark_on_light', 'tray_on_light.ico', True)):
    im = Image.open(logo / f'{src}_1024.png').convert('RGBA')
    (square(im) if trim else im).save(root / 'assets' / ico, sizes=SIZES)
print('done')
