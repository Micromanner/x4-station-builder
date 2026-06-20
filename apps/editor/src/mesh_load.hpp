#pragma once
// Loading-screen pre-upload (known-issues 1.3). A 3D editor loads its assets up
// front, not by streaming the in-view set per frame: this uploads every mesh the
// station needs (via MeshCache::warm, bypassing the per-frame budget) while drawing
// a progress bar, so the interactive view has no box->mesh pop-in and selecting /
// moving never hitches on a late upload.
#include "mesh_cache.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/document/station.hpp"

namespace x4sb::editor {

// Pre-upload all of `station`'s meshes, drawing a progress screen. Blocks until done
// (or the window is closed). Requires a live GL context (call within the window's
// lifetime). No-op for an empty station.
void loadStationMeshes(const Station& station, const ModuleCatalog& catalog, MeshCache& meshes);

}  // namespace x4sb::editor
