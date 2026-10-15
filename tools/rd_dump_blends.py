# qrenderdoc --python: census of every blended draw (blend factors, cull, depth, index count,
# bound read-only texture ids/dims/formats/names) to find SCREEN (One, InvSrcColor) draws,
# plus any draw whose index count is in X4SB_IDX.
# Set X4SB_RDC / X4SB_RDOUT.
import os, struct, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
os.makedirs(OUTDIR, exist_ok=True)
# Index counts that are always logged, blended or not (X4SB_IDX, comma list).
WATCH_IDX = {int(i) for i in os.environ.get('X4SB_IDX', '').split(',') if i}
LOG = open(os.path.join(OUTDIR, 'log_blends.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def rid(r):
    try:
        return int(str(r).replace('ResourceId::', ''))
    except Exception:
        return str(r)


def flatten(actions):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            yield a
        yield from flatten(a.children)


def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    names = {rid(r.resourceId): r.name for r in controller.GetResources()}
    for a in flatten(controller.GetRootActions()):
        controller.SetFrameEvent(a.eventId, False)
        pipe = controller.GetPipelineState()
        vk = controller.GetVulkanPipelineState()
        blends = vk.colorBlend.blends
        if a.numIndices not in WATCH_IDX:
            if not blends or not blends[0].enabled:
                continue
        src, dst = 'off', 'off'
        if blends and blends[0].enabled:
            b = blends[0]
            src = str(b.colorBlend.source).split('.')[-1]
            dst = str(b.colorBlend.destination).split('.')[-1]
        if (src, dst) == ('SrcAlpha', 'InvSrcAlpha') and a.numIndices not in WATCH_IDX:
            continue
        rs = vk.rasterizer
        ds = vk.depthStencil
        tex = []
        try:
            for d in pipe.GetReadOnlyResources(rd.ShaderStage.Fragment):
                r = rid(d.descriptor.resource)
                t = texs.get(r)
                if t is None:
                    continue
                tex.append(f'{r}:{t.width}x{t.height}:{str(d.descriptor.format.Name())}:{names.get(r, "")}')
        except Exception:
            tex.append('tex-error')
        fs = pipe.GetShaderReflection(rd.ShaderStage.Fragment)
        if a.numIndices in WATCH_IDX:
            ib = pipe.GetIBuffer()
            raw = controller.GetBufferData(ib.resourceId, ib.byteOffset + a.indexOffset * ib.byteStride,
                                           a.numIndices * ib.byteStride)
            fmt = '<%d%s' % (a.numIndices, 'H' if ib.byteStride == 2 else 'I')
            idx = struct.unpack(fmt, raw)
            tex.append('ibstride %d baseVertex %d unique %d max %d' % (
                ib.byteStride, a.baseVertex, len(set(idx)), max(idx)))
        log(a.eventId, 'idx', a.numIndices, 'inst', a.numInstances,
            'blend', src, dst, 'cull', str(rs.cullMode).split('.')[-1],
            'dtest', ds.depthTestEnable, 'dwrite', ds.depthWriteEnable,
            'fs', rid(fs.resourceId) if fs else None,
            'pipe', rid(vk.graphics.pipelineResourceId), '|', ' '.join(tex))
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
