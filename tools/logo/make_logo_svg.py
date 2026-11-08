# Stereo Loom's logo as SVG (the master copies): three warp threads, the shuttle
# woven through them (under the outer two, over the middle), on a rounded tile.
# Dark and light. Geometry on a 1024 grid, as Stereopticon Logo.pdf page 8.
import sys, os
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
THEMES = {
    'dark':  dict(bg='#161513', fg='#EDE7DC', accent='#C8894A'),
    'light': dict(bg='#F5EFE6', fg='#1F1C19', accent='#A55D23'),
}
S = 1024.0
bw = S * 0.058; y0, y1 = S * 0.19, S * 0.81
xs = [S * 0.32, S * 0.50, S * 0.68]
cy = S * 0.50; gap = S * 0.026
lx0, lx1, lh = S * 0.23, S * 0.73, S * 0.20

def f(v): return ('%.1f' % v).rstrip('0').rstrip('.')
def vesica(cx, w, h, fill):
    half = w / 2; r = (half * half + (h / 2) ** 2) / h
    a, b = cx - half, cx + half
    return (f'<path d="M{f(a)} {f(cy)} A{f(r)} {f(r)} 0 0 1 {f(b)} {f(cy)} '
            f'A{f(r)} {f(r)} 0 0 1 {f(a)} {f(cy)} Z" fill="{fill}"/>')
def rrect(x0, y0_, x1, y1_, r, fill):
    return f'<rect x="{f(x0)}" y="{f(y0_)}" width="{f(x1 - x0)}" height="{f(y1_ - y0_)}" rx="{f(r)}" fill="{fill}"/>'
def thread(x, fill): return rrect(x - bw / 2, y0, x + bw / 2, y1, bw / 2, fill)

for theme, t in THEMES.items():
    cx = (lx0 + lx1) / 2
    tail = (lx1 - S * 0.02, cy - S * 0.016, S * 0.88, cy + S * 0.016)
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">',
        f'<title>Stereo Loom</title>',
        rrect(0, 0, S, S, S * 0.22, t['bg']),
        '<g>',
        thread(xs[0], t['fg']), thread(xs[2], t['fg']),
        # (the shuttle's gap: the background round it, so the threads under it break)
        vesica(cx, (lx1 - lx0) + gap * 2.4, lh + gap * 2, t['bg']),
        rrect(tail[0], tail[1] - gap, tail[2] + gap, tail[3] + gap, S * 0.03, t['bg']),
        vesica(cx, lx1 - lx0, lh, t['accent']),
        rrect(*tail, S * 0.016, t['accent']),
        # (the middle thread over it, with its own gap)
        rrect(xs[1] - bw / 2 - gap, cy - lh / 2 - gap, xs[1] + bw / 2 + gap, cy + lh / 2 + gap, 0, t['bg']),
        thread(xs[1], t['fg']),
        '</g>', '</svg>', '']
    open(os.path.join(out, f'stereo_loom_{theme}.svg'), 'w', encoding='utf-8', newline='\n').write('\n'.join(parts))
print('ok')
