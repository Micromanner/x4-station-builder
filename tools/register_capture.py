"""Register an X4 station capture to an editor plan and write the capture's camera as
a `--viewshot` view, so the editor renders the exact game view.

usage: python tools/register_capture.py <rd_frame/capture dir> <plan.xml> <cluster> <sector>

Inputs: worlds.json (tools/rd_dump_worlds.py: every G-buffer draw's M_world) and
lighting.json (tools/rd_dump_lighting.py: camera block). Every draw votes for a
station translation = its world position - each plan module position; the true
placement collects the votes of every part that sits at its module origin. Yaw
is tried in 90 degree steps. Writes <dir>/view.json (display space = X4 with z
flipped, as the editor's livedump_view.json)."""
import collections, json, math, os, re, sys

import numpy as np


def plan_positions(path):
    text = open(path, encoding='utf-8').read()
    out = []
    for m in re.finditer(r'<position([^/]*)/>', text):
        attrs = dict(re.findall(r'(\w)="([-\d.eE]+)"', m.group(1)))
        out.append([float(attrs.get(k, 0.0)) for k in 'xyz'])
    return np.array(out)


def yaw_matrix(deg):
    c, s = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])


def main():
    d, plan_path, cluster, sector = sys.argv[1:5]
    worlds = [np.array(w['world']).reshape(4, 4) for w in json.load(open(os.path.join(d, 'worlds.json')))
              if w['world']]
    gpos = np.array([w[:3, 3] for w in worlds])
    ppos = plan_positions(plan_path)
    print(f'{len(gpos)} draws with M_world, {len(ppos)} plan modules')

    best = None
    for yaw in (0, 90, 180, 270):
        R = yaw_matrix(yaw)
        rp = ppos @ R.T
        votes = collections.Counter()
        for g in gpos:
            for key in map(tuple, np.round((g - rp) / 2.0).astype(np.int64)):
                votes[key] += 1
        key, n = votes.most_common(1)[0]
        print(f'  yaw {yaw:3d}: best translation {np.array(key) * 2.0} with {n} votes')
        if best is None or n > best[2]:
            best = (yaw, np.array(key, float) * 2.0, n)
    yaw, T, n = best
    R = yaw_matrix(yaw)
    # Refine on the voters: mean residual of draws within 4 m of a module origin.
    rp = ppos @ R.T + T
    res = []
    for g in gpos:
        dists = np.linalg.norm(rp - g, axis=1)
        i = int(np.argmin(dists))
        if dists[i] < 4.0:
            res.append(g - rp[i])
    if res:
        T = T + np.mean(res, axis=0)
    print(f'station placement: yaw {yaw}, translation {np.round(T, 2)} ({len(res)} draws within 4 m)')

    info = json.load(open(os.path.join(d, 'lighting.json')))
    cb = {}
    for v in info['cb'].values():
        if isinstance(v, dict):
            cb.update(v)
    VI = np.array(cb['M_viewinverse']).reshape(4, 4)
    P = np.array(cb['M_projection']).reshape(4, 4)
    cam, fwd, up = VI[:3, 3], VI[:3, 2], VI[:3, 1]
    Rinv = R.T
    flip = np.array([1.0, 1.0, -1.0])
    pos = (Rinv @ (cam - T)) * flip
    fwd_d = (Rinv @ fwd) * flip
    up_d = (Rinv @ up) * flip
    fovy = math.degrees(2 * math.atan(1 / abs(P[1, 1])))
    view = {'position': pos.tolist(), 'target': (pos + fwd_d * 1000.0).tolist(), 'up': up_d.tolist(),
            'fovy': fovy, 'width': 2560, 'height': 1440, 'cluster': cluster, 'sector': sector,
            'exposure': 1.0}
    json.dump(view, open(os.path.join(d, 'view.json'), 'w'), indent=1)
    print(f'camera (display space) {np.round(pos, 1)}, fovy {fovy:.2f}, '
          f'distance to plan origin {np.linalg.norm(pos):.0f} m -> {os.path.join(d, "view.json")}')


if __name__ == '__main__':
    main()
