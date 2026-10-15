# qrenderdoc --python: station.rdc second pass (after rd_dump_station_hull.py's census).
# Saves the HDR scene target 161150 before/after the fullscreen global-light draw
# (26092) and at the end of the light range (40172), dumps the global-light cbuffers
# and the biggest hull draw's material + vertex colours.
import os, json, struct, traceback
import renderdoc as rd
CAPTURE = r'C:\Users\01tee\Downloads\station.rdc'
OUTDIR = r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_frame\station'
GL = 26092
HULL = 15926
SNAPS = [(GL - 1, 161150), (GL, 161150), (40172, 161150)]
LOG = open(os.path.join(OUTDIR, 'log2.txt'), 'w')
def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n'); LOG.flush()
def rid(r):
    try: return int(str(r).replace('ResourceId::', ''))
    except Exception: return str(r)
def var_to_py(v):
    if len(v.members): return {m.name: var_to_py(m) for m in v.members}
    n = v.rows * v.columns
    if v.type in (rd.VarType.UInt, rd.VarType.Bool): vals = list(v.value.u32v[:n])
    elif v.type == rd.VarType.SInt: vals = list(v.value.s32v[:n])
    else: vals = list(v.value.f32v[:n])
    return vals[0] if n == 1 else vals
def cbuffers(controller, pipe):
    rec = {}
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment):
        refl = pipe.GetShaderReflection(stage)
        if refl is None: continue
        used = pipe.GetConstantBlocks(stage)
        for i, cb in enumerate(refl.constantBlocks):
            desc = next((u.descriptor for u in used if u.access.index == i), None)
            if desc is None: continue
            try:
                vars_ = controller.GetCBufferVariableContents(pipe.GetGraphicsPipelineObject(), pipe.GetShader(stage), stage, pipe.GetShaderEntryPoint(stage), i, desc.resource, desc.byteOffset, desc.byteSize)
                rec[f'{int(stage)}:{cb.name}'] = {v.name: var_to_py(v) for v in vars_}
            except Exception as e: rec[f'{int(stage)}:{cb.name}'] = str(e)
        rec[f'tex{int(stage)}'] = {f'{u.access.index}[{u.access.arrayElement}]': rid(u.descriptor.resource) for u in pipe.GetReadOnlyResources(stage) if rid(u.descriptor.resource)}
    return rec
def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    dump = {}
    controller.SetFrameEvent(GL, True)
    pipe = controller.GetPipelineState()
    dump['globallight'] = {'eid': GL, 'cb': cbuffers(controller, pipe)}
    controller.SetFrameEvent(HULL, True)
    pipe = controller.GetPipelineState()
    h = {'eid': HULL, 'cb': cbuffers(controller, pipe)}
    try:
        h['vertexInputs'] = [{'name': a.name, 'buf': a.vertexBuffer, 'off': a.byteOffset, 'fmt': a.format.Name()} for a in pipe.GetVertexInputs()]
        h['vbuffers'] = []
        for i, vb in enumerate(pipe.GetVBuffers()):
            if rid(vb.resourceId) == 0: continue
            data = controller.GetBufferData(vb.resourceId, vb.byteOffset, min(vb.byteSize or 65536, 65536))
            h['vbuffers'].append({'slot': i, 'stride': vb.byteStride, 'bytes': len(data), 'hex': data.hex()})
    except Exception: h['vb_err'] = traceback.format_exc()
    dump['hull'] = h
    json.dump(dump, open(os.path.join(OUTDIR, 'draws2.json'), 'w'), indent=1, default=str)
    for eid, tid in SNAPS:
        t = texs[tid]
        controller.SetFrameEvent(eid, True)
        sd = rd.TextureSave(); sd.resourceId = t.resourceId; sd.destType = rd.FileType.EXR
        sd.alpha = rd.AlphaMapping.Preserve; sd.mip = 0; sd.slice.sliceIndex = 0
        log('save', eid, tid, controller.SaveTexture(sd, os.path.join(OUTDIR, f'scene_{tid}_e{eid}.exr')))
    log('done')
try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(os.path.join(OUTDIR, 'error2.txt'), 'w').write(traceback.format_exc())
os._exit(0)
