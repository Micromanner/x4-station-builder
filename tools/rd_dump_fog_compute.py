# qrenderdoc --python: cbuffers of every compute dispatch whose shader declares
# M_offset (X4's froxel fog injection, comp_18319: the near fog spheres).
# Set X4SB_RDC / X4SB_RDOUT; writes fog_compute.json.
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log_fog_compute.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


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
        if a.flags & rd.ActionFlags.Dispatch:
            out.append(a)
        walk(a.children, out)


def main(controller):
    dispatches = []
    walk(controller.GetRootActions(), dispatches)
    log('dispatches', len(dispatches))
    out = []
    stage = rd.ShaderStage.Compute
    for a in dispatches:
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            continue
        names = [v.name for cb in refl.constantBlocks for v in cb.variables]
        if not any('M_offset' in n for n in names):
            continue
        cbs = {}
        used = pipe.GetConstantBlocks(stage)
        for i, cb in enumerate(refl.constantBlocks):
            desc = next((u.descriptor for u in used if u.access.index == i), None)
            if desc is None:
                continue
            try:
                vars_ = controller.GetCBufferVariableContents(
                    pipe.GetComputePipelineObject(), pipe.GetShader(stage), stage,
                    pipe.GetShaderEntryPoint(stage), i, desc.resource, desc.byteOffset, desc.byteSize)
                cbs[cb.name] = {v.name: var_to_py(v) for v in vars_}
            except Exception as e:
                cbs[cb.name] = str(e)
        out.append({'eid': a.eventId, 'groups': list(a.dispatchDimension), 'cbuffers': cbs})
        log('dispatch', a.eventId)
    json.dump(out, open(os.path.join(OUTDIR, 'fog_compute.json'), 'w'), indent=1, default=str)
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
