# qrenderdoc --python: dump every cubemap texture (>= 128 px) of the capture as raw
# per-face mip-0 bytes, so the sky X4 baked into its global env map can be
# measured in HDR (the sky is never drawn as geometry in a live frame).
import os, json, traceback
import renderdoc as rd
CAPTURE = r'C:\Users\01tee\Downloads\sun.rdc'
OUTDIR = r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_frame\envmap'
os.makedirs(OUTDIR, exist_ok=True)
def rid(r):
    try: return int(str(r).replace('ResourceId::', ''))
    except Exception: return str(r)
def main(controller):
    info = {}
    for t in controller.GetTextures():
        if not t.cubemap or t.width < 128: continue
        tid = rid(t.resourceId)
        faces = []
        for face in range(6):
            data = controller.GetTextureData(t.resourceId, rd.Subresource(0, face, 0))
            p = os.path.join(OUTDIR, f'cube_{tid}_face{face}.bin')
            open(p, 'wb').write(bytes(data))
            faces.append(len(data))
        info[tid] = {'w': t.width, 'h': t.height, 'fmt': t.format.Name(), 'mips': t.mips, 'arraysize': t.arraysize, 'faceBytes': faces}
    json.dump(info, open(os.path.join(OUTDIR, 'cubes.json'), 'w'), indent=1)
try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(os.path.join(OUTDIR, 'error.txt'), 'w').write(traceback.format_exc())
os._exit(0)
