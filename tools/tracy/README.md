# Tracy profiler CLI tools (fetched, not committed)

The Tracy server-side binaries are **not committed** (they are ~80 MB and match the
`*.exe` gitignore rule). Fetch them once into this folder. Pinned to the same
version as the vendored client in `third_party/tracy` (see that tree's source).

**Pinned version: Tracy v0.13.1**

```sh
curl -fsSL -o tracy-win.zip \
  https://github.com/wolfpld/tracy/releases/download/v0.13.1/windows-0.13.1.zip
unzip -o tracy-win.zip -d tools/tracy && rm tracy-win.zip
```

This yields:

| Tool | Use |
|---|---|
| `tracy-capture.exe`   | Headless: connect to a running client and record a `.tracy` trace. |
| `tracy-csvexport.exe` | Export per-zone CPU stats from a `.tracy` to CSV (no GUI needed). |
| `tracy-profiler.exe`  | The GUI: open a `.tracy` for the live flame graph / timeline. |
| `tracy-import-*`, `tracy-update` | Format converters / trace upgrader (unused here). |

## How profiling is wired

- The editor links the vendored Tracy **client** only under the `full-tracy` preset
  (`cmake --preset full-tracy && cmake --build --preset full-tracy`), which defines
  `TRACY_ENABLE`. The default `full` build has zero profiling cost (the zone macros
  in `apps/editor/src/profiling.hpp` no-op).
- `apps/editor/src/profile.cpp` adds a `--profile <plan.xml> <frames>` mode: an
  orbiting, input-free render of a real plan. It prints frame-time percentiles on
  its own and, in a `full-tracy` build, emits a Tracy zone per frame.
- `tools/profile-capture.ps1` orchestrates the whole capture → CSV flow.

```powershell
tools/profile-capture.ps1 -Plan "C:\...\Sphere Shipyard.xml" -Frames 400 -Name sphere
# → profiling/sphere.tracy (open in tracy-profiler.exe) and profiling/sphere.csv
```

Zones currently instrumented: `frame`, `input+update`, `drawScene`, and the three
`drawPlacedModules` passes (`modules: cull+lod+mesh`, `modules: boxes`,
`modules: markers`). GPU zones (`TracyOpenGL.hpp`) are available but not yet wired.
