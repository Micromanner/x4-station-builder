"""Per-pixel A/B of sun visibility: X4's (game sun delta / unshadowed prediction with AO,
from tools/lighting_analyze.py's math) vs ours (the --viewshot `shadow` term, r channel),
in camera-distance bands, on sun-facing hull pixels where both frames see the same
surface (depths within 2%: ships and other non-station hulls exist only in the game).

usage: python tools/shadow_ab.py <rd_frame/capture dir> <viewshot prefix>"""
import contextlib, io, os, sys

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import lighting_analyze as la

BANDS = [(0, 0.1), (0.1, 0.4), (0.4, 1), (1, 2), (2, 3.5), (3.5, 6), (6, 9), (9, 11), (11, 14), (14, 40)]


def main():
    d, prefix = sys.argv[1:3]
    # Re-run the analyzer's model, capturing its locals instead of its report.
    captured = {}
    tracer = lambda frame, ev, arg: (captured.update(frame.f_locals) if ev == 'return'
                                     and frame.f_code is la.main.__code__ else None) or tracer
    sys.argv = ['', d]
    sys.setprofile(tracer)
    with contextlib.redirect_stdout(io.StringIO()):
        la.main()
    sys.setprofile(None)
    c = captured
    H, W = c['H'], c['W']
    got, pred = c['got'], (c['sun_ao'] if c['ao'] is not None else c['sun'])

    depth = None
    for line in open(os.path.join(d, 'log.txt')):
        f = line.split()
        if len(f) > 6 and f[1] == 'input' and f[6].startswith('D32'):
            # Sample 0, not the resolve: averaged depths at edges are no surface.
            depth = la.samples(d, f'input_{f[3]}_*.exr')[0]
            depth = depth[..., 0] if depth.ndim == 3 else depth
            break
    P = c['P']
    viewz = P[2, 3] / np.maximum(depth, 1e-12)  # reversed-Z infinite far: z = near / d
    dist_game = viewz / c['ray'][..., 2] / 1000.0

    raw = np.fromfile(prefix + '_shadow_scene.raw', np.float16).astype(np.float32)
    ours = raw.reshape(H, W, 4)[::-1]
    vis_ours, dist_ours, ndl_ours = ours[..., 0], ours[..., 1] * 10.0, ours[..., 2]

    both = c['hull'] & (c['ndl'] > 0.2) & (ndl_ours > 0.2) & (pred > 1e-4) & (dist_ours > 0)
    same = both & (np.abs(dist_game - dist_ours) < 0.02 * dist_game)
    print(f'{os.path.basename(d)}: {both.sum()} sun-facing hull px, {same.sum()} on the same surface')
    both = same
    for lo, hi in BANDS:
        m = both & (dist_game >= lo) & (dist_game < hi)
        if m.sum() < 200:
            continue
        # Both weighted by the predicted sun light: an unweighted mean of ours against the
        # game's weighted ratio read 0.25 vs 0.44 at 1-2 km on pixels that match one to one.
        g = got[m].sum() / pred[m].sum()
        o = (vis_ours[m] * pred[m]).sum() / pred[m].sum()
        print(f'  {lo:4.1f}-{hi:4.1f} km  px {m.sum():7d}  game vis {g:.2f}  ours {o:.2f}')


if __name__ == '__main__':
    main()
