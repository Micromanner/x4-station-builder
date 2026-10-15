# qrenderdoc --python: attribute HDR-target light to pass groups. Walks the draws
# into the HDR target in (X4SB_FROM, X4SB_TO], groups consecutive draws by fragment
# shader, saves the target after each group and logs each shader's telltale
# uniform names. Set X4SB_RDC / X4SB_RDOUT / X4SB_HDR / X4SB_FROM / X4SB_TO.
import os, json, traceback
import renderdoc as rd

CAPTURE = os.environ['X4SB_RDC']
OUTDIR = os.environ['X4SB_RDOUT']
HDR = int(os.environ['X4SB_HDR'])
FROM = int(os.environ['X4SB_FROM'])
TO = int(os.environ['X4SB_TO'])
LOG = open(os.path.join(OUTDIR, 'log_groups.txt'), 'w')


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
            out.append((a.eventId, [rid(o) for o in a.outputs if rid(o) != 0], a.numIndices,
                        a.numInstances))
        flatten(a.children, out)


def names(refl):
    out = []

    def walk(vs):
        for v in vs:
            out.append(v.name)
            walk(v.type.members)
    for cb in refl.constantBlocks:
        walk(cb.variables)
    return out


def main(controller):
    texs = {rid(t.resourceId): t for t in controller.GetTextures()}
    draws = []
    flatten(controller.GetRootActions(), draws)
    sel = [d for d in draws if FROM < d[0] <= TO and HDR in d[1]]
    groups = []
    for eid, outs, nidx, ninst in sel:
        controller.SetFrameEvent(eid, True)
        fs = rid(controller.GetPipelineState().GetShader(rd.ShaderStage.Fragment))
        if groups and groups[-1]['fs'] == fs:
            groups[-1]['last'] = eid
            groups[-1]['count'] += 1
        else:
            groups.append({'fs': fs, 'first': eid, 'last': eid, 'count': 1, 'idx': nidx,
                           'inst': ninst, 'outs': outs})
    log('groups', len(groups))
    seen = {}
    for g in groups:
        controller.SetFrameEvent(g['first'], True)
        refl = controller.GetPipelineState().GetShaderReflection(rd.ShaderStage.Fragment)
        if g['fs'] not in seen:
            n = names(refl) if refl else []
            seen[g['fs']] = [x for x in n if x.startswith(('U_', 'B_', 'F_', 'S_')) and
                             not x.startswith(('S_sampler', 'F_alpha'))][:40]
        g['uniforms'] = seen[g['fs']]
    json.dump(groups, open(os.path.join(OUTDIR, 'groups.json'), 'w'), indent=1)
    t = texs[HDR]
    for i, g in enumerate(groups[:40]):
        controller.SetFrameEvent(g['last'], True)
        sd = rd.TextureSave()
        sd.resourceId = t.resourceId
        sd.destType = rd.FileType.EXR
        sd.mip = 0
        sd.slice.sliceIndex = 0
        controller.SaveTexture(sd, os.path.join(OUTDIR, f'grp{i:02d}_e{g["last"]}.exr'))
        log('group', i, g['first'], g['last'], 'n', g['count'], 'fs', g['fs'], 'idx', g['idx'],
            'inst', g['inst'])
    log('done')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    log(traceback.format_exc())
LOG.close()
os._exit(0)
