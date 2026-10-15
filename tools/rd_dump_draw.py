# qrenderdoc --python: one draw's fragment SPIR-V, constant buffers and read-only
# textures (EXR), for decompiling a pass found by tools/rd_dump_hdr_groups.py.
# Set X4SB_RDC / X4SB_RDOUT / X4SB_EID (one eid or a comma list).
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
EIDS = [int(e) for e in os.environ['X4SB_EID'].split(',')]
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, f'log_draw_{EIDS[0]}.txt'), 'w')


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


def main(controller):
    for eid in EIDS:
        dump(controller, eid)
    log('done')


def dump(controller, EID):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    controller.SetFrameEvent(EID, True)
    pipe = controller.GetPipelineState()
    stage = rd.ShaderStage.Fragment
    refl = pipe.GetShaderReflection(stage)
    open(os.path.join(OUTDIR, f'draw_{EID}.spv'), 'wb').write(bytes(refl.rawBytes))
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
    json.dump(cbs, open(os.path.join(OUTDIR, f'draw_{EID}_cb.json'), 'w'), indent=1, default=str)
    for u in pipe.GetReadOnlyResources(stage):
        t = texs.get(rid(u.descriptor.resource))
        if t is None:
            continue
        log(EID, 'input', u.access.index, u.access.arrayElement, rid(t.resourceId), t.width, t.height,
            t.format.Name())
        if t.width >= 64 and t.depth <= 1:
            sd = rd.TextureSave()
            sd.resourceId = t.resourceId
            sd.destType = rd.FileType.EXR
            sd.mip = 0
            sd.slice.sliceIndex = 0
            controller.SaveTexture(sd, os.path.join(OUTDIR, f'draw_{EID}_in_{rid(t.resourceId)}.exr'))


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
