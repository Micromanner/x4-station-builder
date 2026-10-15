# qrenderdoc --python: rasterizer/depth state of the draws that write each CSM depth
# map (the D16 textures, by size), to match our shadow pass's cull mode and bias.
# Set X4SB_RDC / X4SB_RDOUT.
import os, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log_shadow_raster.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def main(controller):
    for t in controller.GetTextures():
        if t.format.Name() != 'D16' or t.width < 2048:
            continue
        evs = [u.eventId for u in controller.GetUsage(t.resourceId)
               if u.usage == rd.ResourceUsage.DepthStencilTarget]
        log(f'texture {t.resourceId} {t.width}x{t.height}: {len(evs)} depth-target events')
        seen = set()
        for eid in evs[::max(1, len(evs) // 12)]:
            controller.SetFrameEvent(eid, True)
            vk = controller.GetVulkanPipelineState()
            r, ds = vk.rasterizer, vk.depthStencil
            fields = ('cullMode', 'frontCCW', 'depthBiasEnable', 'depthBias', 'slopeScaledDepthBias',
                      'depthBiasClamp', 'depthClampEnable')
            state = tuple(str(getattr(r, f, '?')) for f in fields) + (
                str(ds.depthFunction), str(ds.depthWriteEnable))
            log(f'  eid {eid}: ' + ' '.join(f'{k}={v}' for k, v in
                                            zip(fields + ('depthFunc', 'depthWrite'), state))
                + (' (same)' if state in seen else ''))
            seen.add(state)


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
