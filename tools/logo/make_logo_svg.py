# Stereo Loom's logo as SVG (the master copies): three warp threads with the
# shuttle woven through them -- under the outer two, over the middle one.
#   stereo_loom_{dark,light}.svg        the app icon: the mark on a rounded tile
#   stereo_loom_mark_on_{dark,light}.svg the mark alone, gaps cut out, cropped to
#                                        it with the threads in the middle
# Geometry on a 1024 grid, after Stereopticon Logo.pdf, page 8.
# Usage: python make_logo_svg.py <output folder>
import sys, os
out = sys.argv[1]
os.makedirs(out, exist_ok=True)
THEMES = {
    'dark':  dict(bg='#161513', fg='#EDE7DC', accent='#C8894A'),
    'light': dict(bg='#F5EFE6', fg='#1F1C19', accent='#A55D23'),
}
S = 1024.0
bw = S * 0.058                     # thread width
y0, y1 = S * 0.19, S * 0.81        # thread ends
xs = [S * 0.32, S * 0.50, S * 0.68]
cy = S * 0.50
gap = S * 0.026                    # the gap where one passes over another
lx0 = S * 0.23                     # the shuttle's point
apx = S * 0.48                     # where it is deepest
lh = S * 0.20                      # its depth
th = S * 0.016                     # half the tail's thickness
tj = S * 0.74                      # where the body has narrowed into the tail
tx1 = S * 0.88                     # the tail's end (round)

def f(v): return ('%.1f' % v).rstrip('0').rstrip('.')

# The shuttle: a pointed body that narrows smoothly into its tail -- one outline,
# each curve meeting the next along the same tangent.
def shuttle_d():
    top, bot = cy - lh / 2, cy + lh / 2
    return ' '.join([
        f'M{f(lx0)} {f(cy)}',
        f'C{f(lx0 + S*0.06)} {f(cy - lh*0.30)} {f(apx - S*0.12)} {f(top)} {f(apx)} {f(top)}',
        f'C{f(apx + S*0.13)} {f(top)} {f(tj - S*0.10)} {f(cy - th)} {f(tj)} {f(cy - th)}',
        f'L{f(tx1)} {f(cy - th)}',
        f'A{f(th)} {f(th)} 0 0 1 {f(tx1)} {f(cy + th)}',
        f'L{f(tj)} {f(cy + th)}',
        f'C{f(tj - S*0.10)} {f(cy + th)} {f(apx + S*0.13)} {f(bot)} {f(apx)} {f(bot)}',
        f'C{f(apx - S*0.12)} {f(bot)} {f(lx0 + S*0.06)} {f(cy + lh*0.30)} {f(lx0)} {f(cy)} Z'])
SH = shuttle_d()

def rrect(x0, y0_, x1, y1_, r, fill):
    return f'<rect x="{f(x0)}" y="{f(y0_)}" width="{f(x1 - x0)}" height="{f(y1_ - y0_)}" rx="{f(r)}" fill="{fill}"/>'
def thread(x, fill): return rrect(x - bw / 2, y0, x + bw / 2, y1, bw / 2, fill)
def shuttle(fill, halo=None):
    # (halo: the shuttle grown by the gap all round, in that colour -- the gap)
    if halo:
        return f'<path d="{SH}" fill="{halo}" stroke="{halo}" stroke-width="{f(gap * 2)}" stroke-linejoin="round"/>'
    return f'<path d="{SH}" fill="{fill}"/>'
def mid_gap(fill):
    return rrect(xs[1] - bw / 2 - gap, cy - lh / 2 - gap, xs[1] + bw / 2 + gap, cy + lh / 2 + gap, 0, fill)

# The app icon, on its tile.
for theme, t in THEMES.items():
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">',
        '<title>Stereo Loom</title>',
        rrect(0, 0, S, S, S * 0.22, t['bg']),
        thread(xs[0], t['fg']), thread(xs[2], t['fg']),
        shuttle(None, halo=t['bg']),
        shuttle(t['accent']),
        mid_gap(t['bg']),
        thread(xs[1], t['fg']),
        '</svg>', '']
    open(os.path.join(out, f'stereo_loom_{theme}.svg'), 'w', encoding='utf-8', newline='\n').write('\n'.join(parts))

# The mark alone: cropped to it, the middle thread in the middle (so as far to the
# left of it as the tail reaches to the right), top and bottom at the threads' ends.
reach = tx1 + th - xs[1]
vx0, vx1 = xs[1] - reach, xs[1] + reach
vy0, vy1 = y0, y1
VW, VH = vx1 - vx0, vy1 - vy0
for name, theme in (('on_dark', 'dark'), ('on_light', 'light')):
    t = THEMES[theme]
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{f(vx0)} {f(vy0)} {f(VW)} {f(VH)}" width="{f(VW)}" height="{f(VH)}">',
        '<title>Stereo Loom</title>',
        '<defs>',
        '<mask id="under" maskUnits="userSpaceOnUse" x="0" y="0" width="1024" height="1024">'
        '<rect width="1024" height="1024" fill="#fff"/>' + shuttle(None, halo='#000') + '</mask>',
        '<mask id="over" maskUnits="userSpaceOnUse" x="0" y="0" width="1024" height="1024">'
        '<rect width="1024" height="1024" fill="#fff"/>' + mid_gap('#000') + '</mask>',
        '</defs>',
        '<g mask="url(#under)">' + thread(xs[0], t['fg']) + thread(xs[2], t['fg']) + '</g>',
        '<g mask="url(#over)">' + shuttle(t['accent']) + '</g>',
        thread(xs[1], t['fg']),
        '</svg>', '']
    open(os.path.join(out, f'stereo_loom_mark_{name}.svg'), 'w', encoding='utf-8', newline='\n').write('\n'.join(parts))
print('mark', f(VW), 'x', f(VH))
