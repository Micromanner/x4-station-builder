# qrenderdoc --python: whole-frame draw census + texture saves. Per pipeline the first
# 3 draws also get their DYNAMIC/MATERIAL cbuffers, so every shader's real uniform
# values are on record without replaying cbuffers for all ~thousand draws.
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ.get('RD_CAPTURE', r'C:\Users\01tee\Downloads\sun.rdc')
OUTDIR = r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_frame'
SAVE_TEX = [int(x) for x in os.environ.get('RD_TEX', '10610,10620,11849,11763,11835').split(',') if x]
os.makedirs(OUTDIR, exist_ok=True)

def rid(r):
    try: return int(str(r).replace('ResourceId::', ''))
    except Exception: return str(r)

def var_to_py(v):
    if len(v.members): return {m.name: var_to_py(m) for m in v.members}
    n = v.rows * v.columns
    if v.type == rd.VarType.UInt or v.type == rd.VarType.Bool: vals = list(v.value.u32v[:n])
    elif v.type == rd.VarType.SInt: vals = list(v.value.s32v[:n])
    else: vals = list(v.value.f32v[:n])
    return vals[0] if n == 1 else vals

def cbuffers(controller, pipe, stage):
    out = {}
    refl = pipe.GetShaderReflection(stage)
    if refl is None: return out
    entry, shader, pipeobj = pipe.GetShaderEntryPoint(stage), pipe.GetShader(stage), pipe.GetGraphicsPipelineObject()
    used = pipe.GetConstantBlocks(stage)
    for i, cb in enumerate(refl.constantBlocks):
        if 'CAMERA' in cb.name: continue
        desc = next((u.descriptor for u in used if u.access.index == i), None)
        if desc is None: continue
        try:
            vars_ = controller.GetCBufferVariableContents(pipeobj, shader, stage, entry, i, desc.resource, desc.byteOffset, desc.byteSize)
            out[f'{int(stage)}:{cb.name}'] = {v.name: var_to_py(v) for v in vars_ if not v.name.startswith('M_')}
        except Exception as e:
            out[f'{int(stage)}:{cb.name}'] = f'error {e}'
    return out

def walk(actions, fn):
    for a in actions:
        fn(a)
        if len(a.children): walk(a.children, fn)

def main(controller):
    names = {rid(r.resourceId): r.name for r in controller.GetResources()}
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    sdfile = controller.GetStructuredFile()
    results, seen = [], {}
    def visit(a):
        if not (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)): return
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vk = controller.GetVulkanPipelineState()
        is_draw = bool(a.flags & rd.ActionFlags.Drawcall)
        pid = rid(vk.graphics.pipelineResourceId if is_draw else vk.compute.pipelineResourceId)
        rec = {'eid': a.eventId, 'name': a.GetName(sdfile), 'pipeline': pid, 'draw': is_draw,
               'numIndices': a.numIndices, 'numInstances': a.numInstances}
        try:
            rec['modules'] = {str(int(s)): rid(pipe.GetShader(s)) for s in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment, rd.ShaderStage.Compute) if rid(pipe.GetShader(s)) != 0}
        except Exception as e: rec['modules_err'] = str(e)
        if is_draw:
            try:
                rec['outputs'] = [{'id': rid(o.resource), 'w': texs[rid(o.resource)].width, 'h': texs[rid(o.resource)].height, 'fmt': texs[rid(o.resource)].format.Name()} for o in pipe.GetOutputTargets() if rid(o.resource) in texs]
                rec['viewport'] = [[vs.vp.x, vs.vp.y, vs.vp.width, vs.vp.height] for vs in vk.viewportScissor.viewportScissors]
                rec['blend'] = [{'on': b.enabled, 'src': str(b.colorBlend.source).split('.')[-1], 'dst': str(b.colorBlend.destination).split('.')[-1]} for b in vk.colorBlend.blends]
                rec['depth'] = {'test': vk.depthStencil.depthTestEnable, 'write': vk.depthStencil.depthWriteEnable}
            except Exception as e: rec['state_err'] = str(e)
        seen[pid] = seen.get(pid, 0) + 1
        if seen[pid] <= 3:
            try:
                for stage in ((rd.ShaderStage.Vertex, rd.ShaderStage.Fragment) if is_draw else (rd.ShaderStage.Compute,)):
                    rec.setdefault('cbuffers', {}).update(cbuffers(controller, pipe, stage))
                    rec.setdefault('textures', {}).update({f'{int(stage)}:{u.access.index}[{u.access.arrayElement}]': {'id': rid(u.descriptor.resource), 'name': names.get(rid(u.descriptor.resource), ''), 'w': texs[rid(u.descriptor.resource)].width if rid(u.descriptor.resource) in texs else 0, 'h': texs[rid(u.descriptor.resource)].height if rid(u.descriptor.resource) in texs else 0} for u in pipe.GetReadOnlyResources(stage) if rid(u.descriptor.resource) != 0})
            except Exception as e: rec['cb_err'] = traceback.format_exc()
        results.append(rec)
    walk(controller.GetRootActions(), visit)
    with open(os.path.join(OUTDIR, 'frame.json'), 'w') as f:
        json.dump(results, f, indent=0, default=str)
    log = []
    for tid in SAVE_TEX:
        t = texs.get(tid)
        if t is None: log.append(f'{tid}: not a texture'); continue
        for fmt, ext in ((rd.FileType.PNG, 'png'), (rd.FileType.EXR, 'exr')):
            sd = rd.TextureSave(); sd.resourceId = t.resourceId; sd.destType = fmt; sd.mip = 0; sd.slice.sliceIndex = 0
            sd.alpha = rd.AlphaMapping.Preserve
            ok = controller.SaveTexture(sd, os.path.join(OUTDIR, f'tex_{tid}.{ext}'))
            log.append(f'{tid} {t.width}x{t.height} {t.format.Name()} mips={t.mips} -> {ext} {ok}')
    open(os.path.join(OUTDIR, 'textures.log'), 'w').write('\n'.join(log))

try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(os.path.join(OUTDIR, 'error.txt'), 'w').write(traceback.format_exc())
os._exit(0)
