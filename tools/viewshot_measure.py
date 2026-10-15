"""Attribute a view's brightness to hull lighting terms from `--viewshot` dumps.

usage: python tools/viewshot_measure.py <outPrefix> <view.json> [x y radius]

Reads <prefix>_{all,sundiffuse,sunspecular,ibl,emissive}_scene.raw (RGBA16F,
screen size, bottom-up as raylib reads the FBO) and prints linear-luminance
percentiles per term, the share each term contributes to the brightest 1% of
`all`, and, optionally, the same for a disc around a screen pixel (top-left
origin, as in a screenshot)."""
import json, sys, numpy as np

TERMS = ["all", "sundiffuse", "sunspecular", "ibl", "emissive"]

def load(prefix, term, w, h):
    a = np.fromfile(f"{prefix}_{term}_scene.raw", dtype=np.float16).astype(np.float32)
    a = a.reshape(h, w, 4)[::-1, :, :3]  # flip to top-left origin
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]

def report(name, lum, mask):
    v = lum["all"][mask]
    hot = mask & (lum["all"] >= np.percentile(v, 99))
    print(f"[{name}] px={mask.sum()}  all lum p50/p90/p99/max = "
          f"{np.percentile(v, 50):.3f} {np.percentile(v, 90):.3f} {np.percentile(v, 99):.3f} {v.max():.2f}")
    # Every single-term render still carries emissive + fill, so subtract that
    # floor before apportioning the hot pixels between the light terms.
    base = lum["emissive"]
    tot = max((lum["all"] - base)[hot].sum(), 1e-9)
    for t in TERMS[1:]:
        share = (lum[t] - base)[hot].sum() / tot * 100 if t != "emissive" else base[hot].sum() / max(lum["all"][hot].sum(), 1e-9) * 100
        print(f"    {t:12s} p50={np.percentile(lum[t][mask], 50):.3f}  "
              f"{'share of lit energy in top-1%' if t != 'emissive' else 'share of ALL top-1%'}: {share:5.1f}%")

def main():
    prefix, view = sys.argv[1], json.load(open(sys.argv[2]))
    w, h = view["width"], view["height"]
    lum = {t: load(prefix, t, w, h) for t in TERMS}
    report("frame", lum, np.ones((h, w), bool))
    if len(sys.argv) >= 6:
        x, y, r = (int(v) for v in sys.argv[3:6])
        yy, xx = np.mgrid[0:h, 0:w]
        report(f"disc {x},{y} r{r}", lum, (xx - x) ** 2 + (yy - y) ** 2 <= r * r)

main()
