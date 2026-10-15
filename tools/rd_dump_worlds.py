# qrenderdoc --python: every G-buffer draw's M_world (X4 sector space) plus the
# camera, so tools/register_capture.py can solve the station's placement and
# `--viewshot` can render the capture's exact camera. Set X4SB_RDC / X4SB_RDOUT.
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
os.makedirs(OUTDIR, exist_ok=True)
LOG = open(os.path.join(OUTDIR, 'log_worlds.txt'), 'w')


def log(*a):
    LOG.write(' '.join(str(x) for x in a) + '\n')
    LOG.flush()


def rid(r):
    try:
        return int(str(r).replace('ResourceId::', ''))
    except Exception:
        return str(r)


def flatten(actions, out):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            outs = [rid(o) for o in a.outputs if rid(o) != 0]
            out.append((a.eventId, outs, a.numIndices, a.numInstances))
        flatten(a.children, out)


def find_var(vars_, name):
    for v in vars_:
        if v.name == name:
            return list(v.value.f32v[:16])
        if len(v.members):
            r = find_var(v.members, name)
            if r is not None:
                return r
    return None


def main(controller):
    draws = []
    flatten(controller.GetRootActions(), draws)
    gbuf = [d for d in draws if len(d[1]) >= 4]
    out = []
    for eid, outs, nidx, ninst in gbuf:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if refl is None:
            continue
        used = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
        world = None
        for i, cb in enumerate(refl.constantBlocks):
            if 'WORLD' not in cb.name:
                continue
            desc = next((u.descriptor for u in used if u.access.index == i), None)
            if desc is None:
                continue
            vars_ = controller.GetCBufferVariableContents(
                pipe.GetGraphicsPipelineObject(), pipe.GetShader(rd.ShaderStage.Vertex),
                rd.ShaderStage.Vertex, pipe.GetShaderEntryPoint(rd.ShaderStage.Vertex), i,
                desc.resource, desc.byteOffset, desc.byteSize)
            world = find_var(vars_, 'M_world')
            break
        out.append({'eid': eid, 'idx': nidx, 'inst': ninst, 'world': world})
    json.dump(out, open(os.path.join(OUTDIR, 'worlds.json'), 'w'))
    log('draws', len(out), 'with world', sum(1 for o in out if o['world']))


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
