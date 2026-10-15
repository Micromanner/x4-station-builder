# qrenderdoc --python: the 4 frame-start additive quad draws (pipe 29376) in full —
# vertex/instance buffers, all cbuffers, textures — plus raw 3D tonemap LUTs.
import os, json, struct, traceback
import renderdoc as rd
CAPTURE = r'C:\Users\01tee\Downloads\sun.rdc'
OUTDIR = r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_frame'
EIDS = [8533, 8542, 9805, 9814, 9817, 9826, 9835, 9838, 9841, 9850, 9860, 9872, 9884, 9887, 9890, 9893, 9896, 9899, 9902, 9905]
SAVE_TEX = []
RAW_TEX = []
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
def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    out = {}
    for eid in EIDS:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
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
                    rec[f'{int(stage)}:{cb.name}'] = {v.name: var_to_py(v) for v in vars_ if not v.name.startswith(('M_shadow','M_jitter','M_prevjitter','M_texturematrix','M_envmap','V_volume','V_evsm','I_volume','F_texturefactor'))}
                except Exception as e: rec[f'{int(stage)}:{cb.name}'] = str(e)
            rec[f'tex{int(stage)}'] = {f'{u.access.index}[{u.access.arrayElement}]': rid(u.descriptor.resource) for u in pipe.GetReadOnlyResources(stage) if rid(u.descriptor.resource)}
        try:
            rec['vertexInputs'] = [{'name': a.name, 'buf': a.vertexBuffer, 'off': a.byteOffset, 'fmt': a.format.Name(), 'inst': a.perInstance} for a in pipe.GetVertexInputs()]
            rec['vbuffers'] = []
            for i, vb in enumerate(pipe.GetVBuffers()):
                if rid(vb.resourceId) == 0: continue
                data = controller.GetBufferData(vb.resourceId, vb.byteOffset, min(vb.byteSize or 2048, 2048))
                rec['vbuffers'].append({'slot': i, 'stride': vb.byteStride, 'floats': list(struct.unpack('<%df' % (len(data)//4), data[:(len(data)//4)*4]))[:200]})
            ib = pipe.GetIBuffer()
            data = controller.GetBufferData(ib.resourceId, ib.byteOffset, 24)
            rec['indices'] = list(struct.unpack('<%dH' % (len(data)//2), data))
        except Exception: rec['vb_err'] = traceback.format_exc()
        out[eid] = rec
    json.dump(out, open(os.path.join(OUTDIR, 'p1lens_draws.json'), 'w'), indent=1, default=str)
    log = []
    SAVE_TEX.extend(sorted({t for r in out.values() for t in r.get('tex4', {}).values() if t in texs and texs[t].width <= 2048 and texs[t].width > 16}))
    for tid in SAVE_TEX:
        t = texs.get(tid)
        if t is None: log.append(f'{tid} missing'); continue
        sd = rd.TextureSave(); sd.resourceId = t.resourceId; sd.destType = rd.FileType.PNG; sd.alpha = rd.AlphaMapping.Preserve
        log.append(f'{tid} {t.width}x{t.height} {t.format.Name()} -> {controller.SaveTexture(sd, os.path.join(OUTDIR, f"tex_{tid}.png"))}')
    for tid in RAW_TEX:
        t = texs.get(tid)
        if t is None: log.append(f'{tid} missing'); continue
        data = controller.GetTextureData(t.resourceId, rd.Subresource(0, 0, 0))
        open(os.path.join(OUTDIR, f'lut_{tid}.raw'), 'wb').write(bytes(data))
        log.append(f'{tid} {t.width}x{t.height}x{t.depth} {t.format.Name()} raw {len(data)}B')
    open(os.path.join(OUTDIR, 'p1lens_textures.log'), 'w').write('\n'.join(log))
try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(os.path.join(OUTDIR, 'p1lens_error.txt'), 'w').write(traceback.format_exc())
os._exit(0)
