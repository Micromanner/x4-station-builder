# qrenderdoc --python: list the API calls (with key params) in an event range, to
# find what a stalling draw waits on. RD_FROM/RD_TO bound the range.
import os, traceback
import renderdoc as rd

CAPTURE = os.environ.get('RD_CAPTURE', r'C:\Users\01tee\Documents\Projects\x4-station-builder\profiling\fps60_capture.rdc')
OUT = os.environ.get('RD_OUT', r'C:\Users\01tee\Documents\Projects\x4-station-builder\profiling\events_around.txt')
LO = int(os.environ.get('RD_FROM', '8900'))
HI = int(os.environ.get('RD_TO', '9150'))


def main(controller):
    sf = controller.GetStructuredFile()
    lines = []
    def walk(list_):
        for a in list_:
            for ev in a.events:
                if LO <= ev.eventId <= HI:
                    chunk = sf.chunks[ev.chunkIndex]
                    args = []
                    for i in range(chunk.NumChildren()):
                        c = chunk.GetChild(i)
                        try:
                            v = c.AsString() if c.type.basetype == rd.SDBasic.String else str(c.data.basic.u) if c.type.basetype in (rd.SDBasic.UnsignedInteger, rd.SDBasic.Enum, rd.SDBasic.Resource) else str(c.data.basic.i) if c.type.basetype == rd.SDBasic.SignedInteger else ''
                        except Exception:
                            v = '?'
                        args.append('%s=%s' % (c.name, v))
                    lines.append('%6d %s(%s)' % (ev.eventId, chunk.name, ', '.join(args)[:160]))
            walk(a.children)
    walk(controller.GetRootActions())
    open(OUT, 'w').write('\n'.join(lines) + '\n')


try:
    pyrenderdoc.LoadCapture(CAPTURE, rd.ReplayOptions(), CAPTURE, False, True)
    pyrenderdoc.Replay().BlockInvoke(main)
except Exception:
    open(OUT, 'w').write(traceback.format_exc())
os._exit(0)
