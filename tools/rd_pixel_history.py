# qrenderdoc --python: pixel history of the albedo G-buffer target at a few pixels, to find
# which game draws wrote them. Set X4SB_RDC / X4SB_RDOUT / X4SB_TEX (resource id) /
# X4SB_PIXELS ("x,y;x,y;...").
import os, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
TEX = int(os.environ['X4SB_TEX'])
PIXELS = [tuple(int(v) for v in p.split(',')) for p in os.environ['X4SB_PIXELS'].split(';')]
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log_pixel_history.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def rid(r):
    try:
        return int(str(r).replace('ResourceId::', ''))
    except Exception:
        return str(r)


def main(controller):
    tex = next(t for t in controller.GetTextures() if rid(t.resourceId) == TEX)
    names = {}

    def walk(actions):
        for a in actions:
            names[a.eventId] = (a.customName or str(a.flags), a.numIndices, a.numInstances)
            walk(a.children)
    walk(controller.GetRootActions())
    for x, y in PIXELS:
        log('pixel', x, y)
        hist = controller.PixelHistory(tex.resourceId, x, y, rd.Subresource(0, 0, 0), rd.CompType.Typeless)
        for m in hist:
            c = m.postMod.col.floatValue
            log('  eid', m.eventId, 'passed', m.Passed(), 'prim', m.primitiveID,
                'post', [round(v, 3) for v in c[:4]], names.get(m.eventId))
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
