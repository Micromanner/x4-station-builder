# qrenderdoc --python: raster/depth/blend state and vertex-stage world matrix of draws.
# Set X4SB_RDC / X4SB_RDOUT / X4SB_EID (comma list).
import os, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
EIDS = [int(e) for e in os.environ['X4SB_EID'].split(',')]
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log_state.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def main(controller):
    for eid in EIDS:
        controller.SetFrameEvent(eid, True)
        vk = controller.GetVulkanPipelineState()
        r = vk.rasterizer
        ds = vk.depthStencil
        log(eid, 'cull', r.cullMode, 'frontCCW', r.frontCCW, 'depthTest', ds.depthTestEnable,
            'depthWrite', ds.depthWriteEnable, 'func', ds.depthFunction)
        for i, b in enumerate(vk.colorBlend.blends[:1]):
            log('  blend', b.enabled, b.colorBlend.source, b.colorBlend.destination,
                b.colorBlend.operation, b.alphaBlend.source, b.alphaBlend.destination)
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
