# qrenderdoc --python: X4 volumetric-fog inputs of one frame. Finds the instanced
# path-length draws (fog spheres, ONE/ONE into R16 targets) and the composite that
# reads those targets, then dumps per draw: instance vertex buffers (raw), VS/FS
# SPIR-V + cbuffers, bound textures, and the R16 targets after the last fog draw.
# Set X4SB_RDC / X4SB_RDOUT; optional X4SB_FROM / X4SB_TO eid window.
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
EID_FROM = int(os.environ.get('X4SB_FROM', '0'))
EID_TO = int(os.environ.get('X4SB_TO', '1000000000'))
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log_fog.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def rid(r):
    try:
        return int(str(r).replace('ResourceId::', ''))
    except Exception:
        return str(r)


def var_to_py(v):
    if len(v.members):
        return {m.name: var_to_py(m) for m in v.members}
    n = v.rows * v.columns
    if v.type in (rd.VarType.UInt, rd.VarType.Bool):
        vals = list(v.value.u32v[:n])
    elif v.type == rd.VarType.SInt:
        vals = list(v.value.s32v[:n])
    else:
        vals = list(v.value.f32v[:n])
    return vals[0] if n == 1 else vals


def walk(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall and EID_FROM <= a.eventId <= EID_TO:
            out.append(a)
        walk(a.children, out)


def save_tex(controller, t, path):
    sd = rd.TextureSave()
    sd.resourceId = t.resourceId
    sd.destType = rd.FileType.EXR
    sd.mip = 0
    sd.slice.sliceIndex = 0
    controller.SaveTexture(sd, path)


def stage_dump(controller, pipe, stage, tag, texs, saved):
    refl = pipe.GetShaderReflection(stage)
    if refl is None:
        return {}
    open(os.path.join(OUTDIR, f'{tag}.spv'), 'wb').write(bytes(refl.rawBytes))
    cbs = {}
    used = pipe.GetConstantBlocks(stage)
    for i, cb in enumerate(refl.constantBlocks):
        desc = next((u.descriptor for u in used if u.access.index == i), None)
        if desc is None:
            continue
        try:
            vars_ = controller.GetCBufferVariableContents(
                pipe.GetGraphicsPipelineObject(), pipe.GetShader(stage), stage,
                pipe.GetShaderEntryPoint(stage), i, desc.resource, desc.byteOffset, desc.byteSize)
            cbs[cb.name] = {v.name: var_to_py(v) for v in vars_}
        except Exception as e:
            cbs[cb.name] = str(e)
    inputs = []
    for u in pipe.GetReadOnlyResources(stage):
        t = texs.get(rid(u.descriptor.resource))
        if t is None:
            continue
        inputs.append([u.access.index, u.access.arrayElement, rid(t.resourceId), t.width, t.height,
                       t.depth, t.format.Name()])
        if t.width >= 64 and t.depth <= 1 and rid(t.resourceId) not in saved:
            saved.add(rid(t.resourceId))
            save_tex(controller, t, os.path.join(OUTDIR, f'tex_{rid(t.resourceId)}.exr'))
    return {'cbuffers': cbs, 'inputs': inputs}


def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    draws = []
    walk(controller.GetRootActions(), draws)
    log('draws in window', len(draws))

    # Fog spheres: instanced draws whose only colour target is a single-channel R16.
    fog, r16 = [], set()
    for a in draws:
        outs = [rid(o) for o in a.outputs if rid(o) in texs]
        fmts = [texs[o].format.Name() for o in outs]
        if a.numInstances > 1 and outs and all(f.startswith('R16_') for f in fmts):
            fog.append(a)
            r16.update(outs)
            log('fog draw', a.eventId, 'idx', a.numIndices, 'inst', a.numInstances, 'outs', outs, fmts)
    if not fog:
        log('no fog draws found')
        return

    saved = set()
    summary = {'fog': [], 'composite': None, 'targets': sorted(r16)}
    for a in fog:
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vbs = []
        for slot, vb in enumerate(pipe.GetVBuffers()):
            if rid(vb.resourceId) in (0, '0'):
                continue
            size = vb.byteSize if vb.byteSize < (1 << 40) else 0
            data = controller.GetBufferData(vb.resourceId, vb.byteOffset, size)
            path = f'fog_{a.eventId}_vb{slot}.bin'
            open(os.path.join(OUTDIR, path), 'wb').write(bytes(data))
            vbs.append({'slot': slot, 'stride': vb.byteStride, 'bytes': len(data), 'file': path})
        attrs = [[v.name, v.vertexBuffer, v.byteOffset, v.perInstance, v.instanceRate, v.format.Name()]
                 for v in pipe.GetVertexInputs()]
        summary['fog'].append({
            'eid': a.eventId, 'indices': a.numIndices, 'instances': a.numInstances,
            'instanceOffset': a.instanceOffset, 'outputs': [rid(o) for o in a.outputs],
            'vbs': vbs, 'attrs': attrs,
            'vs': stage_dump(controller, pipe, rd.ShaderStage.Vertex, f'fog_{a.eventId}_vs', texs, saved),
            'fs': stage_dump(controller, pipe, rd.ShaderStage.Fragment, f'fog_{a.eventId}_fs', texs, saved),
        })
        log('dumped fog', a.eventId)

    last = fog[-1].eventId
    controller.SetFrameEvent(last, True)
    for t in sorted(r16):
        save_tex(controller, texs[t], os.path.join(OUTDIR, f'pathlen_{t}_e{last}.exr'))

    # Composite: the first later draw whose fragment stage reads a fog target.
    for a in draws:
        if a.eventId <= last:
            continue
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        reads = {rid(u.descriptor.resource) for u in pipe.GetReadOnlyResources(rd.ShaderStage.Fragment)}
        if reads & r16:
            summary['composite'] = {
                'eid': a.eventId,
                'fs': stage_dump(controller, pipe, rd.ShaderStage.Fragment, f'composite_{a.eventId}_fs',
                                 texs, saved),
            }
            log('composite', a.eventId)
            break

    json.dump(summary, open(os.path.join(OUTDIR, 'fog.json'), 'w'), indent=1, default=str)
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
