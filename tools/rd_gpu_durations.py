# qrenderdoc --python: per-draw GPU duration census of an editor capture (profile.cpp
# RDC_FRAME). Writes the top draws by EventGPUDuration with their shader, index count,
# instance count and render-target size, plus a per-program total, to RD_OUT.
import os, traceback
import renderdoc as rd

CAPTURE = os.environ.get('RD_CAPTURE', r'C:\Users\01tee\Documents\Projects\x4-station-builder\profiling\fps60_capture.rdc')
OUT = os.environ.get('RD_OUT', r'C:\Users\01tee\Documents\Projects\x4-station-builder\profiling\gpu_durations.txt')


def main(controller):
    counters = controller.EnumerateCounters()
    if rd.GPUCounter.EventGPUDuration not in counters:
        open(OUT, 'w').write('EventGPUDuration unsupported\n')
        return
    results = controller.FetchCounters([rd.GPUCounter.EventGPUDuration])
    dur = {r.eventId: r.value.d for r in results if r.value.d == r.value.d}
    extra = [c for c in (rd.GPUCounter.PSInvocations, rd.GPUCounter.VSInvocations,
                         rd.GPUCounter.SamplesPassed) if c in counters]
    stats = {}
    for r in controller.FetchCounters(extra):
        stats.setdefault(r.eventId, {})[str(r.counter).split('.')[-1]] = r.value.u64

    actions = {}
    def walk(list_):
        for a in list_:
            actions[a.eventId] = a
            walk(a.children)
    walk(controller.GetRootActions())

    rows = []
    by_prog = {}
    total = 0.0
    for eid, d in dur.items():
        a = actions.get(eid)
        if a is None:
            continue
        total += d
        controller.SetFrameEvent(eid, False)
        st = controller.GetGLPipelineState()
        prog = str(st.fragmentShader.programResourceId)
        by_prog.setdefault(prog, [0.0, 0])
        by_prog[prog][0] += d
        by_prog[prog][1] += 1
        rows.append((d, eid, a.GetName(controller.GetStructuredFile()), a.numIndices, a.numInstances, prog))

    rows.sort(reverse=True)
    lines = ['total GPU %.3f ms over %d events' % (total * 1000, len(rows)), '', 'per program:']
    for prog, (d, n) in sorted(by_prog.items(), key=lambda kv: -kv[1][0]):
        lines.append('  %-28s %8.3f ms  %5d draws' % (prog, d * 1000, n))
    ps = {}
    for d, eid, name, idx, inst, prog in rows:
        ps[prog] = ps.get(prog, 0) + stats.get(eid, {}).get('12', 0)
    lines += ['', 'PS invocations per program:'] + ['  %-28s %12d' % kv for kv in sorted(ps.items(), key=lambda kv: -kv[1])[:6]]
    lines += ['', 'top draws:']
    for d, eid, name, idx, inst, prog in rows[:60]:
        lines.append('  eid %6d %8.3f ms  idx %8d inst %6d  %s  %s  %s' % (eid, d * 1000, idx, inst, prog, name[:40], stats.get(eid, {})))
    watch = set(int(x) for x in os.environ.get('RD_EIDS', '').split(',') if x)
    if watch:
        lines += ['', 'watched:']
        for d, eid, name, idx, inst, prog in sorted(rows, key=lambda r: r[1]):
            if eid in watch:
                lines.append('  eid %6d %8.3f ms  idx %8d inst %6d  %s  %s' % (eid, d * 1000, idx, inst, prog, stats.get(eid, {})))
    open(OUT, 'w').write('\n'.join(lines) + '\n')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(OUT, 'w').write(traceback.format_exc())
os._exit(0)
