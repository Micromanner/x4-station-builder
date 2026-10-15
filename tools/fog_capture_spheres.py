"""Exact X4 fog sphere list from a capture dump (tools/rd_dump_fog.py), as the
editor's per-cluster override (%LOCALAPPDATA%/x4sb/fog/<cluster>.json).

  python -I tools/fog_capture_spheres.py tools/rd_frame/X4_Station_FRONT/fog cluster_14 \
      --zone-offset -57062.328125 0 61362.80859375 \
      --media fog_cluster_14_basefill fog_cluster_14_sector_001

X4 renders relative to the camera's zone: cluster position = render position + the
zone's offset in the cluster (maps/*/sectors.xml). --media names the medium of each
fog draw in draw order (the composite's V_volume_* slots, medium_library.xml).
"""
import argparse
import json
import os
import struct

STRIDE = 96  # INSTANCE0-3 = world matrix columns, INSTANCE4 = (density, 0, 0, radius)

ap = argparse.ArgumentParser()
ap.add_argument('dump')
ap.add_argument('cluster')
ap.add_argument('--zone-offset', type=float, nargs=3, required=True)
ap.add_argument('--media', nargs='+', required=True)
ap.add_argument('--out')
a = ap.parse_args()

fog = json.load(open(os.path.join(a.dump, 'fog.json')))
if len(fog['fog']) != len(a.media):
    raise SystemExit(f"{len(fog['fog'])} fog draws but {len(a.media)} media names")
spheres = []
for draw, medium in zip(fog['fog'], a.media):
    vb = next(v for v in draw['vbs'] if v['slot'] == 1)
    data = open(os.path.join(a.dump, vb['file']), 'rb').read()
    for i in range(draw['instances']):
        f = struct.unpack_from('20f', data, (draw['instanceOffset'] + i) * STRIDE)
        pos = [f[12] + a.zone_offset[0], f[13] + a.zone_offset[1], f[14] + a.zone_offset[2]]
        spheres.append([round(pos[0], 2), round(pos[1], 2), round(pos[2], 2), f[19], f[16], medium])

out = a.out or os.path.join(os.environ['LOCALAPPDATA'], 'x4sb', 'fog', a.cluster + '.json')
os.makedirs(os.path.dirname(out), exist_ok=True)
json.dump({'spheres': spheres}, open(out, 'w'))
print(f'{len(spheres)} spheres -> {out}')
