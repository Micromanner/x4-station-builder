"""Compare X4's deferred global-light output against the global_lights math applied to
the capture's own G-buffer (no shadow, no AO), per camera-distance band.

usage: python tools/lighting_analyze.py tools/rd_frame/<capture>

Reads lighting.json + gbuf/hdr EXRs written by tools/rd_dump_lighting.py. The ratio
game / predicted on sun-facing pixels is X4's effective sun visibility (CSM shadow
x the AO it applies to sun diffuse), which our shadow map has to reproduce.

X4 renders MSAA: when the dump has samples/ (per-sample EXRs), every sample is
shaded on its own and the result averaged per pixel, like the game's resolve.
Shading the resolved G-buffer instead mispredicts every edge pixel (~25% low on a
distant station)."""
import glob, json, os, sys
import numpy as np
import OpenEXR

LUM = np.array([0.2126, 0.7152, 0.0722])
LUM601 = np.array([0.299, 0.587, 0.114])


def exr(path):
    ch = OpenEXR.File(path).channels()
    return np.asarray(ch[list(ch)[0]].pixels, np.float32)


def one(d, pattern):
    hits = sorted(glob.glob(os.path.join(d, pattern)))
    return hits[0] if hits else None


def samples(d, pattern):
    """(S, H, W, C): the per-sample EXRs of `pattern` (a resolved-file glob), else
    the resolved image as one sample."""
    stem = pattern[:-len('.exr')]
    hits = sorted(glob.glob(os.path.join(d, 'samples', stem + '_s*.exr')))
    return np.stack([exr(h) for h in hits]) if hits else exr(one(d, pattern))[None]


def flat_cb(cb):
    out = {}
    for v in cb.values():
        if isinstance(v, dict):
            out.update(v)
    return out


def main():
    d = sys.argv[1]
    info = json.load(open(os.path.join(d, 'lighting.json')))
    cb = flat_cb(info['cb'])
    albedo_id, normal_id, mr_id = info['gbufOuts'][1:4]
    alb = samples(d, f'gbuf_{albedo_id}_*.exr')[..., :3]
    nr = samples(d, f'gbuf_{normal_id}_*.exr')
    mr = samples(d, f'gbuf_{mr_id}_*.exr')
    S, H, W = alb.shape[:3]
    hull_s = (np.abs(nr[..., 0]) + np.abs(nr[..., 1])) > 1e-4
    hull = hull_s.any(0)
    # The global pass is one of several consecutive draws (stencil classes): take
    # the draw with the largest hull-luminance jump as the sun/ambient pass.
    steps = [samples(d, 'hdr_preglobal_*.exr')[..., :3]]
    i = 0
    while one(d, f'hdr_step{i}_*.exr'):
        steps.append(samples(d, f'hdr_step{i}_*.exr')[..., :3])
        i += 1
    jumps = [float(((b - a) @ LUM)[hull_s].mean()) for a, b in zip(steps, steps[1:])]
    print('  per-draw hull delta:', ' '.join(f'{j:.4f}' for j in jumps))
    k = int(np.argmax(jumps))
    pre, post = steps[k], steps[k + 1]

    P = np.array(cb['M_projection']).reshape(4, 4)
    L = np.array(cb['V_light_direction_view']['V_light_direction_view[0]'][:3])
    L /= np.linalg.norm(L)
    lc = np.array(cb['V_lightcolor1'][:3]) * cb['F_globallightscale'] * 2.0

    e = np.concatenate([nr[..., :2] * 0.3, np.ones_like(nr[..., :1])], -1)
    g = 2.0 / np.sum(e * e, -1, keepdims=True)
    N = np.empty_like(e)
    N[..., :2] = e[..., :2] * g
    N[..., 2] = -(g[..., 0] - 1.0)
    N /= np.linalg.norm(N, axis=-1, keepdims=True)
    yy, xx = np.mgrid[0:H, 0:W]
    ray = np.stack([((xx + 0.5) / W * 2 - 1) / P[0, 0], ((yy + 0.5) / H * 2 - 1) / P[1, 1],
                    np.ones((H, W))], -1)
    ray /= np.linalg.norm(ray, axis=-1, keepdims=True)
    V = -ray

    rough, metal = mr[..., 0], mr[..., 1]
    cspec = 0.04 * (1 - metal[..., None]) + alb * metal[..., None]
    cdiff = alb * (1 - metal[..., None]) * np.clip(1 - cspec @ LUM601, 0, 1)[..., None]
    ndl = np.clip((N * L).sum(-1), 0, 1)
    R = 2 * (N * V).sum(-1, keepdims=True) * N - V
    C = L * 1000
    ctr = (R @ C)[..., None] * R - C
    ln = np.sqrt(np.maximum(1e-4, (ctr * ctr).sum(-1, keepdims=True)))
    Ls = C + ctr * np.clip(10 / ln, 0, 1)
    Ls /= np.linalg.norm(Ls, axis=-1, keepdims=True)
    m = rough * rough
    spn = 1 / (1 + (1 / (np.pi * np.maximum(0.001, m))) * 0.01)
    Hh = V + Ls
    Hh /= np.linalg.norm(Hh, axis=-1, keepdims=True)
    nl = np.clip((N * Ls).sum(-1), 0, 1)
    nv = np.clip(np.abs((N * V).sum(-1)) + 1e-8, 0, 1)
    nh = np.clip((N * Hh).sum(-1), 0, 1)
    vh = np.clip((V * Hh).sum(-1), 0, 1)
    a2 = m * m
    dd = (nh * a2 - nh) * nh + 1
    D = a2 / (np.pi * dd * dd)
    Vis = 0.5 / np.maximum(1e-8, nl * (nv * (1 - m) + m) + nv * (nl * (1 - m) + m))
    fr = (1 - vh) ** 5
    F = fr[..., None] + (1 - fr[..., None]) * cspec
    spec = (D * Vis * spn * ndl)[..., None] * F * lc
    diff = cdiff * ndl[..., None] * lc
    sun = np.minimum(spec + diff, 10) @ LUM
    got = (post - pre) @ LUM
    print(f'  {S} sample(s) per pixel')

    # The pass's SSAO input (the R8_UNORM screen texture, per log.txt):
    # X4 applies it as diffuse x AO^2, specular x AO.
    ao = None
    for line in open(os.path.join(d, 'log.txt')):
        f = line.split()
        if len(f) > 6 and f[0] == 'save' and f[1] == 'input' and f[6] == 'R8_UNORM':
            a = exr(one(d, f'input_{f[3]}_*.exr'))[..., 0]
            from PIL import Image
            ao = np.asarray(Image.fromarray(a).resize((W, H)))
            print(f'  AO input {f[3]} {f[6]} {f[4]}x{f[5]}: hull mean {ao[hull].mean():.3f}')
            break
    if ao is not None:
        sun_ao = np.minimum(spec * ao[..., None] + diff * (ao * ao)[..., None], 10) @ LUM

    # Per-pixel = mean over samples; non-hull samples carry no global light.
    def resolve(a):
        return np.where(hull_s, a, 0).mean(0)
    cover = np.maximum(hull_s.sum(0), 1)
    got, sun = resolve(got), resolve(sun)
    if ao is not None:
        sun_ao = resolve(sun_ao)
    ndl = np.where(hull_s, ndl, 0).sum(0) / cover
    rough = np.where(hull_s, rough, 0).sum(0) / cover
    metal = np.where(hull_s, metal, 0).sum(0) / cover

    print(f'{os.path.basename(d)}: {W}x{H}, hull px {hull.mean():.2f}, '
          f'shadowmax {cb.get("F_shadowmaxdistance")}, csm thresholds {cb.get("V_csmthresholds")}')
    lit = hull & (ndl > 0.2)
    print(f'  sun-facing hull px: game {got[lit].mean():.4f}  predicted-no-shadow {sun[lit].mean():.4f}  '
          f'ratio {got[lit].mean() / max(sun[lit].mean(), 1e-9):.2f}')
    print(f'  all hull px:        game {got[hull].mean():.4f}  predicted-no-shadow {sun[hull].mean():.4f}')
    if ao is not None:
        print(f'  sun-facing with AO: predicted {sun_ao[lit].mean():.4f}  ratio game/pred '
              f'{got[lit].mean() / max(sun_ao[lit].mean(), 1e-9):.2f}  (remaining = CSM shadow)')
    for name, (y0, y1, x0, x1) in {'top': (0, H // 3, 0, W), 'middle': (H // 3, 2 * H // 3, 0, W),
                                   'bottom': (2 * H // 3, H, 0, W)}.items():
        s = lit[y0:y1, x0:x1]
        if s.sum() < 100:
            continue
        gg, pp = got[y0:y1, x0:x1][s].mean(), sun[y0:y1, x0:x1][s].mean()
        print(f'  {name:6s} third: game {gg:.4f} predicted {pp:.4f} ratio {gg / max(pp, 1e-9):.2f}')


if __name__ == '__main__':
    main()
