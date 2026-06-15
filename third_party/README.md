# third_party — vendored dependencies (frozen)

These are **committed, frozen copies** of small header-only / single-source
libraries. They are compiled by our own CMake targets (`third_party/CMakeLists.txt`)
and we **never run the upstream build system**. This is deliberate: it removes the
build-system coupling that broke us once already (CMake 4.x rejecting a
dependency's old `cmake_minimum_required`) and makes the build reproducible
offline and immune to upstream / network / toolchain drift.

The only dependency **not** vendored is **raylib** (a large C renderer library);
it stays on pinned FetchContent in `cmake/raylib.cmake`.

## Pinned versions

| Library | Version | CMake target | Files | Source |
|---|---|---|---|---|
| doctest | **v2.5.2** | `doctest::doctest` | `doctest/doctest/doctest.h` | https://github.com/doctest/doctest |
| nlohmann/json | **v3.12.0** | `nlohmann_json::nlohmann_json` | `json/nlohmann/json.hpp` | https://github.com/nlohmann/json |
| pugixml | **v1.15** | `pugixml::pugixml` | `pugixml/{pugixml.hpp,pugixml.cpp,pugiconfig.hpp}` | https://github.com/zeux/pugixml |
| raygui | **4.0** | `RAYGUI_INCLUDE_DIR` (header) | `raygui/raygui.h` | https://github.com/raysan5/raygui |
| sinfl | **(raylib 5.5 bundle)** | `sinfl::sinfl` | `sinfl/sinfl.h` + `sinfl/sinfl_impl.c` | raylib `src/external/sinfl.h` (orig. vurtun/sdefl) |

Recommended (not yet vendored) for the asset pipeline's glTF *writer*:
**cgltf v1.15** (`cgltf.h` + `cgltf_write.h`, https://github.com/jkuhlmann/cgltf) —
preferred over tinygltf, whose v2 is being sunset after mid-2026.

## Updating a dependency

Replace the file(s) from the upstream tag and bump the version in the table
above. Example (run from the repo root):

```sh
curl -fsSL -o third_party/doctest/doctest/doctest.h \
  https://raw.githubusercontent.com/doctest/doctest/<tag>/doctest/doctest.h
curl -fsSL -o third_party/json/nlohmann/json.hpp \
  https://raw.githubusercontent.com/nlohmann/json/<tag>/single_include/nlohmann/json.hpp
curl -fsSL -o third_party/pugixml/pugixml.hpp \
  https://raw.githubusercontent.com/zeux/pugixml/<tag>/src/pugixml.hpp
curl -fsSL -o third_party/pugixml/pugixml.cpp \
  https://raw.githubusercontent.com/zeux/pugixml/<tag>/src/pugixml.cpp
curl -fsSL -o third_party/pugixml/pugiconfig.hpp \
  https://raw.githubusercontent.com/zeux/pugixml/<tag>/src/pugiconfig.hpp
curl -fsSL -o third_party/raygui/raygui.h \
  https://raw.githubusercontent.com/raysan5/raygui/<tag>/src/raygui.h
```

Then rebuild and run the tests. Updates are intentional, reviewed events — not
something that happens silently behind your back.

## Note on git / licensing

These files are third-party code under their own permissive licenses (MIT /
Boost / Unlicense) and are fine to redistribute. This is **library source**, not
X4 game assets — the spec §12 "code only, never commit game assets" rule is
about Egosoft IP, which never lives here.
