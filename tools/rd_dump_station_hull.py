# qrenderdoc --python: hull-lighting ground truth from station.rdc. Census of every
# draw (outputs, counts), auto-detects the G-buffer pass (MRT draws) and the global
# light pass (fragment shader carrying M_shadowCSM0), dumps their cbuffers, one hull
# draw's vertex stream (for the vertex colour), and saves the G-buffer targets, the
# lit HDR target and the backbuffer so a hull pixel can be followed texture -> albedo
# -> lit -> tonemapped.
import os, json, struct, traceback
import renderdoc as rd
CAPTURE = r'C:\Users\01tee\Downloads\station.rdc'
OUTDIR = r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_frame\station'
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log.txt'), 'w')
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
def res_of(d):
    for k in ('resource', 'resourceId'):
        if hasattr(d, k): return rid(getattr(d, k))
    if hasattr(d, 'descriptor'): return res_of(d.descriptor)
    return 0
def flatten(actions, out):
    for a in actions:
        out.append(a)
        if len(a.children): flatten(a.children, out)
    return out
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
def save_tex(controller, texs, eid, tid, tag):
    t = texs.get(tid)
    if t is None: log('save', tag, tid, 'missing'); return
    controller.SetFrameEvent(eid, True)
    for ext, ft in (('exr', rd.FileType.EXR), ('png', rd.FileType.PNG)):
        sd = rd.TextureSave(); sd.resourceId = t.resourceId; sd.destType = ft
        sd.alpha = rd.AlphaMapping.Preserve; sd.mip = 0; sd.slice.sliceIndex = 0
        ok = controller.SaveTexture(sd, os.path.join(OUTDIR, f'{tag}_{tid}_e{eid}.{ext}'))
        log('save', tag, eid, tid, f'{t.width}x{t.height}', t.format.Name(), ext, ok)
def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    acts = flatten(controller.GetRootActions(), [])
    draws = [a for a in acts if a.flags & rd.ActionFlags.Drawcall]
    log('actions', len(acts), 'draws', len(draws))
    census = []
    shader_has_csm = {}
    gl_eids = []
    mrt = []
    for a in draws:
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        outs = [res_of(o) for o in pipe.GetOutputTargets()]
        outs = [o for o in outs if o]
        fs = rid(pipe.GetShader(rd.ShaderStage.Fragment))
        if fs not in shader_has_csm:
            has = False
            refl = pipe.GetShaderReflection(rd.ShaderStage.Fragment)
            if refl is not None:
                for cb in refl.constantBlocks:
                    if any(v.name == 'M_shadowCSM0' for v in cb.variables): has = True; break
            shader_has_csm[fs] = has
        rec = {'eid': a.eventId, 'idx': a.numIndices, 'inst': a.numInstances, 'fs': fs, 'outs': outs, 'csm': shader_has_csm[fs]}
        census.append(rec)
        if shader_has_csm[fs] and a.numIndices <= 6 and a.numInstances <= 1: gl_eids.append(a.eventId)
        if len(outs) >= 3: mrt.append(rec)
    json.dump(census, open(os.path.join(OUTDIR, 'census.json'), 'w'))
    log('globallight candidates', gl_eids[:10], 'mrt draws', len(mrt))
    if not gl_eids or not mrt:
        log('detection failed'); return
    gl = gl_eids[0]
    gb = [m for m in mrt if m['eid'] < gl]
    gb_last = gb[-1] if gb else mrt[-1]
    hull = max(gb, key=lambda m: m['idx'] * max(1, m['inst'])) if gb else gb_last
    log('gbuffer last', gb_last, 'hull draw', hull)
    dump = {}
    controller.SetFrameEvent(gl, True)
    pipe = controller.GetPipelineState()
    dump['globallight'] = {'eid': gl, 'outs': [res_of(o) for o in pipe.GetOutputTargets()], 'cb': cbuffers(controller, pipe)}
    controller.SetFrameEvent(hull['eid'], True)
    pipe = controller.GetPipelineState()
    h = {'eid': hull['eid'], 'cb': cbuffers(controller, pipe)}
    try:
        h['vertexInputs'] = [{'name': a.name, 'buf': a.vertexBuffer, 'off': a.byteOffset, 'fmt': a.format.Name(), 'inst': a.perInstance} for a in pipe.GetVertexInputs()]
        h['vbuffers'] = []
        for i, vb in enumerate(pipe.GetVBuffers()):
            if rid(vb.resourceId) == 0: continue
            data = controller.GetBufferData(vb.resourceId, vb.byteOffset, min(vb.byteSize or 8192, 8192))
            h['vbuffers'].append({'slot': i, 'stride': vb.byteStride, 'bytes': len(data), 'hex': data[:2048].hex()})
    except Exception: h['vb_err'] = traceback.format_exc()
    dump['hull'] = h
    json.dump(dump, open(os.path.join(OUTDIR, 'draws.json'), 'w'), indent=1, default=str)
    for tid in gb_last['outs']: save_tex(controller, texs, gb_last['eid'], tid, 'gbuf')
    for tid in dump['globallight']['outs']:
        if tid: save_tex(controller, texs, gl, tid, 'lit')
    last = draws[-1].eventId
    swap = [t for t in texs.values() if t.creationFlags & rd.TextureCategory.SwapBuffer]
    for t in swap: save_tex(controller, texs, last, rid(t.resourceId), 'backbuffer')
    log('done')
try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(os.path.join(OUTDIR, 'error.txt'), 'w').write(traceback.format_exc())
os._exit(0)
