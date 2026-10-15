# Run with: "C:\Program Files\RenderDoc\qrenderdoc.exe" --python tools/rd_dump_draws.py
# Dumps, for every draw using a target pipeline, the pipeline state + every constant-buffer
# variable + bound render-target sizes + (for the sun) instance vertex data. Uses the UI's
# embedded interpreter because RenderDoc ships no standalone renderdoc.pyd on Windows.
import os, sys, json, struct, traceback
import renderdoc as rd

CAPTURE = os.environ.get('RD_CAPTURE', r'C:\Users\01tee\Downloads\sun.rdc')
OUT = os.environ.get('RD_OUT', r'C:\Users\01tee\Documents\Projects\x4-station-builder\tools\rd_draws.json')
TARGET_PIPES = set(int(x) for x in os.environ.get('RD_PIPES', '18369,18286,18287,18290,18291,18298,14916,13183').split(','))
ALL = os.environ.get('RD_ALL', '') == '1'

def rid(r):
    try:
        return int(str(r).replace('ResourceId::', ''))
    except Exception:
        return str(r)

def var_to_py(v):
    # ShaderVariable -> python
    if len(v.members):
        return {m.name: var_to_py(m) for m in v.members}
    t = v.type
    n = v.rows * v.columns
    if t in (rd.VarType.Float,):
        vals = list(v.value.f32v[:n])
    elif t in (rd.VarType.UInt,):
        vals = list(v.value.u32v[:n])
    elif t in (rd.VarType.SInt,):
        vals = list(v.value.s32v[:n])
    elif t in (rd.VarType.Bool,):
        vals = list(v.value.u32v[:n])
    else:
        vals = list(v.value.f32v[:n])
    return vals[0] if n == 1 else vals

def dump_cbuffers(controller, pipe, stage, out):
    refl = pipe.GetShaderReflection(stage)
    if refl is None:
        return
    entry = pipe.GetShaderEntryPoint(stage)
    shader = pipe.GetShader(stage)
    pipeobj = pipe.GetGraphicsPipelineObject()
    used = pipe.GetConstantBlocks(stage)
    for i, cb in enumerate(refl.constantBlocks):
        # find the descriptor for this slot
        desc = None
        for u in used:
            if u.access.index == i:
                desc = u.descriptor
                break
        if desc is None:
            out.setdefault('cbuffers', {})[f'{stage}:{cb.name}'] = 'unbound'
            continue
        try:
            vars_ = controller.GetCBufferVariableContents(pipeobj, shader, stage, entry, i, desc.resource, desc.byteOffset, desc.byteSize)
            out.setdefault('cbuffers', {})[f'{stage}:{cb.name}'] = {v.name: var_to_py(v) for v in vars_}
        except Exception as e:
            out.setdefault('cbuffers', {})[f'{stage}:{cb.name}'] = f'error {e}'

def dump_textures(controller, pipe, stage, out, names):
    try:
        for u in pipe.GetReadOnlyResources(stage):
            r = rid(u.descriptor.resource)
            if r == 0:
                continue
            out.setdefault('textures', {})[f'{stage}:{u.access.index}[{u.access.arrayElement}]'] = {'id': r, 'name': names.get(r, '')}
    except Exception as e:
        out['textures_err'] = str(e)

def walk(actions, fn):
    for a in actions:
        fn(a)
        if len(a.children):
            walk(a.children, fn)

def main(controller):
    names = {rid(r.resourceId): r.name for r in controller.GetResources()}
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    results = []
    sdfile = controller.GetStructuredFile()

    def visit(a):
        if not (a.flags & rd.ActionFlags.Drawcall):
            return
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vk = controller.GetVulkanPipelineState()
        pid = rid(vk.graphics.pipelineResourceId)
        if not ALL and pid not in TARGET_PIPES:
            return
        rec = {'eid': a.eventId, 'name': a.GetName(sdfile), 'pipeline': pid,
               'numIndices': a.numIndices, 'numInstances': a.numInstances}
        try:
            rec['viewport'] = [[v.x, v.y, v.width, v.height] for v in vk.viewportScissor.viewportScissors and [vs.vp for vs in vk.viewportScissor.viewportScissors]]
        except Exception as e:
            rec['viewport_err'] = str(e)
        try:
            rec['blend'] = [{'enabled': b.enabled, 'src': str(b.colorBlend.source), 'dst': str(b.colorBlend.destination), 'op': str(b.colorBlend.operation),
                             'asrc': str(b.alphaBlend.source), 'adst': str(b.alphaBlend.destination), 'mask': b.writeMask} for b in vk.colorBlend.blends]
        except Exception as e:
            rec['blend_err'] = str(e)
        try:
            rec['depth'] = {'test': vk.depthStencil.depthTestEnable, 'write': vk.depthStencil.depthWriteEnable, 'func': str(vk.depthStencil.depthFunction)}
        except Exception as e:
            rec['depth_err'] = str(e)
        try:
            outs = []
            for o in pipe.GetOutputTargets():
                r = rid(o.resource)
                t = texs.get(r)
                outs.append({'id': r, 'name': names.get(r, ''), 'w': t.width if t else 0, 'h': t.height if t else 0, 'fmt': t.format.Name() if t else ''})
            rec['outputs'] = outs
        except Exception as e:
            rec['outputs_err'] = str(e)
        for stage in (rd.ShaderStage.Vertex, rd.ShaderStage.Fragment):
            try:
                dump_cbuffers(controller, pipe, stage, rec)
            except Exception as e:
                rec.setdefault('cb_err', []).append(f'{stage}: {e}')
            dump_textures(controller, pipe, stage, rec, names)
        if pid == 18369 or ALL:
            try:
                vbs = pipe.GetVBuffers()
                attrs = pipe.GetVertexInputs()
                rec['vertexInputs'] = [{'name': at.name, 'buf': at.vertexBuffer, 'off': at.byteOffset, 'fmt': at.format.Name(), 'perInstance': at.perInstance} for at in attrs]
                rec['vbuffers'] = []
                for i, vb in enumerate(vbs):
                    r = rid(vb.resourceId)
                    if r == 0:
                        continue
                    data = controller.GetBufferData(vb.resourceId, vb.byteOffset, min(vb.byteSize or 4096, 4096))
                    floats = list(struct.unpack('<%df' % (len(data) // 4), data[: (len(data) // 4) * 4]))
                    rec['vbuffers'].append({'slot': i, 'id': r, 'stride': vb.byteStride, 'off': vb.byteOffset, 'floats': floats[:256]})
            except Exception as e:
                rec['vb_err'] = traceback.format_exc()
        results.append(rec)

    walk(controller.GetRootActions(), visit)
    with open(OUT, 'w') as f:
        json.dump(results, f, indent=1, default=str)
    print('WROTE', OUT, len(results))

try:
    ok = pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    with open(OUT + '.err', 'w') as f:
        f.write(traceback.format_exc())
os._exit(0)
