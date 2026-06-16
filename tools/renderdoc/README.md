# RenderDoc CLI (fetched, not committed)

RenderDoc is a graphics frame debugger: capture a frame, then inspect every draw
call, bound texture/buffer, shader, and pipeline-state value. It is the right tool
for **visual correctness** ("why is this mesh black / this texture wrong / is this
draw even happening?") — distinct from Tracy, which answers CPU **timing**.

The binaries are **not committed** (~250 MB; they match the `*.exe` gitignore rule).
Fetch the portable build once into this folder — **no install / admin needed**.

**Pinned version: RenderDoc 1.44**

```sh
curl -fsSL -o renderdoc.zip https://renderdoc.org/stable/1.44/RenderDoc_1.44_64.zip
unzip -oq renderdoc.zip -d /tmp/rd && cp -r /tmp/rd/RenderDoc_1.44_64/* tools/renderdoc/ && rm -rf renderdoc.zip /tmp/rd
```

Key pieces: `renderdoccmd.exe` (headless capture/inject CLI), `qrenderdoc.exe` (the
GUI inspector), `renderdoc.dll` (injected into the target). The bundled AMD GPU
counter plugins work with this machine's Radeon GPU.

## How capture is wired (programmatic, no key press)

The editor vendors RenderDoc's single in-app API header (`third_party/renderdoc/
renderdoc_app.h`) and uses it from `apps/editor/src/rdc_api.cpp` (isolated from
raylib because it needs `<windows.h>`). The hidden `--rdc <plan>` mode loads a plan,
warms up, then brackets exactly one frame with `StartFrameCapture`/`EndFrameCapture`.
When the editor is launched under RenderDoc the dll is injected and the bracket
records a `.rdc`; in a normal run it is a no-op (no build flag needed).

```powershell
tools/rdc-capture.ps1 -Plan "C:\...\Sphere Shipyard.xml" -Name sphere
# → profiling/sphere_frameNNN.rdc ; open in tools/renderdoc/qrenderdoc.exe
```

## Scripted (agent-readable) analysis — not yet wired

`renderdoccmd` has no `python` subcommand, and the GUI's Python API targets the
bundled Python 3.6 (not the system 3.14). So extracting the draw-call list to text
headlessly is a future add-on; for now frame inspection is visual, via qrenderdoc.
CPU timing (the comparison-free numbers) comes from Tracy — see ../tracy/README.md.
