# qrenderdoc --python: generic lighting dump for any X4 station capture.
# Finds the last G-buffer draw (4 colour outputs) and X4's deferred global-light
# pass after it (fragment reflection has U_pass + F_shadowmaxdistance), then saves
# the G-buffer, the HDR target before/after the global pass, the final backbuffer
# and the pass's constant buffers (camera, sun, CSM). Set CAPTURE/OUTDIR via the
# X4SB_RDC / X4SB_RDOUT environment variables.
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log.txt'), 'w')


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


def cbuffers(controller, pipe):
    rec = {}
    for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment):
        refl = pipe.GetShaderReflection(stage)
        if refl is None:
            continue
        used = pipe.GetConstantBlocks(stage)
        for i, cb in enumerate(refl.constantBlocks):
            desc = next((u.descriptor for u in used if u.access.index == i), None)
            if desc is None:
                continue
            try:
                vars_ = controller.GetCBufferVariableContents(
                    pipe.GetGraphicsPipelineObject(), pipe.GetShader(stage), stage,
                    pipe.GetShaderEntryPoint(stage), i, desc.resource, desc.byteOffset,
                    desc.byteSize)
                rec[f'{int(stage)}:{cb.name}'] = {v.name: var_to_py(v) for v in vars_}
            except Exception as e:
                rec[f'{int(stage)}:{cb.name}'] = str(e)
    return rec


def names(refl):
    out = set()

    def walk(vs):
        for v in vs:
            out.add(v.name)
            walk(v.type.members)
    for cb in refl.constantBlocks:
        walk(cb.variables)
    return out


def flatten(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            outs = [rid(o) for o in a.outputs if rid(o) != 0]
            out.append((a.eventId, outs, a.numIndices))
        flatten(a.children, out)


def save(controller, texs, eid, tid, tag):
    t = texs.get(tid)
    if t is None:
        log('missing tex', tid)
        return
    controller.SetFrameEvent(eid, True)
    for ft, ext in ((rd.FileType.EXR, 'exr'), (rd.FileType.PNG, 'png')):
        sd = rd.TextureSave()
        sd.resourceId = t.resourceId
        sd.destType = ft
        sd.alpha = rd.AlphaMapping.Preserve
        sd.mip = 0
        sd.slice.sliceIndex = 0
        res = controller.SaveTexture(sd, os.path.join(OUTDIR, f'{tag}_{tid}_e{eid}.{ext}'))
    # X4 runs MSAA (config msaa_4x): SaveTexture's default resolves the samples,
    # and shading a resolved G-buffer is wrong at every edge pixel, i.e. almost
    # everywhere on a distant station. Keep each sample too, under samples/.
    if t.msSamp > 1:
        os.makedirs(os.path.join(OUTDIR, 'samples'), exist_ok=True)
        for k in range(t.msSamp):
            sd = rd.TextureSave()
            sd.resourceId = t.resourceId
            sd.destType = rd.FileType.EXR
            sd.alpha = rd.AlphaMapping.Preserve
            sd.mip = 0
            sd.slice.sliceIndex = 0
            sd.sample.mapToArray = False
            sd.sample.sampleIndex = k
            controller.SaveTexture(sd, os.path.join(OUTDIR, 'samples', f'{tag}_{tid}_e{eid}_s{k}.exr'))
    log('save', tag, eid, tid, t.width, t.height, t.format.Name(), res, 'samples', t.msSamp)


def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    draws = []
    flatten(controller.GetRootActions(), draws)
    log('draws', len(draws))
    gbuf = [d for d in draws if len(d[1]) >= 4]
    if not gbuf:
        log('no gbuffer draws')
        return
    g_eid, g_outs, _ = gbuf[-1]
    hdr = g_outs[0]
    log('gbuffer last', g_eid, g_outs, 'count', len(gbuf))

    # The global light is a fullscreen draw (<= 6 indices) into the HDR target
    # whose shader reads the sun + CSM uniforms; X4 issues it once per stencil
    # class, so every such draw is saved and the analysis picks the lit one.
    cands = []
    for eid, outs, nidx in draws:
        if eid <= g_eid or outs != [hdr] or nidx > 6:
            continue
        controller.SetFrameEvent(eid, True)
        refl = controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Fragment)
        if refl is None:
            continue
        n = names(refl)
        if 'V_lightcolor1' in n and 'M_shadowCSM0' in n:
            cands.append(eid)
        if len(cands) >= 8:
            break
    log('global candidates', cands)
    if not cands:
        return
    glob = cands[0]

    controller.SetFrameEvent(glob, True)
    pipe = controller.GetPipelineState()
    rec = {'gbufEid': g_eid, 'gbufOuts': g_outs, 'globalEid': glob,
           'cb': cbuffers(controller, pipe), 'hdrDrawsFromGlobal': cands}
    # Screen-size read-only inputs of the pass (SSAO, depth, CSM): what scales the sun.
    inputs = []
    for u in pipe.GetReadOnlyResources(rd.ShaderStage.Fragment):
        t = texs.get(rid(u.descriptor.resource))
        if t is not None and t.width >= 640 and rid(t.resourceId) not in g_outs:
            inputs.append(rid(t.resourceId))
    rec['screenInputs'] = sorted(set(inputs))
    json.dump(rec, open(os.path.join(OUTDIR, 'lighting.json'), 'w'), indent=1, default=str)

    for t in g_outs[1:]:
        save(controller, texs, g_eid, t, 'gbuf')
    for t in rec['screenInputs']:
        save(controller, texs, glob, t, 'input')
    prev = max(d[0] for d in draws if d[0] < glob)
    save(controller, texs, prev, hdr, 'hdr_preglobal')
    for i, eid in enumerate(cands):
        save(controller, texs, eid, hdr, f'hdr_step{i}')
    last_hdr = max(d[0] for d in draws if hdr in d[1])
    save(controller, texs, last_hdr, hdr, 'hdr_last')
    last = draws[-1]
    for t in last[1]:
        save(controller, texs, last[0], t, 'backbuffer')
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
