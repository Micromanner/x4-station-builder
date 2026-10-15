#include "profile.hpp"

#include "bloom.hpp"
#include "env_cubemap.hpp"
#include "external/glad.h"
#include "mesh_cache.hpp"
#include "mesh_load.hpp"
#include "native_assets.hpp"
#include "profiling.hpp"
#include "raylib.h"
#include "rdc_api.hpp"
#include "render.hpp"
#include "sky.hpp"
#include "sun_shadow.hpp"
#include "x4sb/data/catalog.hpp"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"
#include "x4sb/planio/plan.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace x4sb::editor {
namespace {

// The user's editor window: fill-rate costs (clustered lights) only show at
// the real size, so 720p under-reported them (known-issues 1.5). PW/PH override.
constexpr int kDefaultW = 2560;
constexpr int kDefaultH = 1369;
constexpr int kWarmup = 5;  // exclude mesh-upload/shader-compile spikes from the stats
constexpr double kSpikeMs = 20.0;

// Nearest-rank percentile of an already-sorted (ascending) sample, p in [0,1].
[[nodiscard]] double percentile(const std::vector<double>& sortedMs, double p) {
  if (sortedMs.empty()) return 0.0;
  const double rank = p * static_cast<double>(sortedMs.size() - 1);
  const std::size_t idx = static_cast<std::size_t>(std::lround(rank));
  return sortedMs[std::min(idx, sortedMs.size() - 1)];
}

[[nodiscard]] int envInt(const char* name, int fallback) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::max(std::atoi(v), 1) : fallback;
}

[[nodiscard]] int profileMaxFps() {
  const char* value = std::getenv("PROFILE_MAX_FPS");
  if (value == nullptr) return 30;
  if (std::string_view(value) == "0") return 0;
  const int parsed = std::atoi(value);
  return parsed > 0 ? std::min(parsed, 240) : 30;
}

// GPU timestamps per frame phase. CPU stamps alone can't split CPU from GPU time:
// GL is async, so all GPU work surfaces as the wait inside EndDrawing's swap.
// Results are read kRing frames late so the readback never stalls the pipeline.
// The marks from kSceneFirst up to kScene are drawScene's own (setScenePhaseHook).
constexpr std::size_t kScenePhases = static_cast<std::size_t>(ScenePhase::Count);
enum GpuMark : std::size_t {
  kStart,
  kShadow,
  kSky,
  kSceneFirst,
  kScene = kSceneFirst + kScenePhases,
  kPost,
  kMarks
};
constexpr std::size_t kRing = 4;

struct Span {
  std::size_t from;
  std::size_t to;
  const char* name;
};
constexpr std::array<Span, 4> kPhases{{
    {kStart, kShadow, "shadow"},
    {kShadow, kSky, "sky"},
    {kSky, kScene, "scene"},
    {kScene, kPost, "post"},
}};
// The pass ending at each scene mark, then what drawScene draws after the last one.
constexpr std::array<Span, kScenePhases + 1> kSceneSpans = [] {
  std::array<Span, kScenePhases + 1> spans{};
  for (std::size_t i = 0; i < kScenePhases; ++i)
    spans[i] = {kSceneFirst + i - 1, kSceneFirst + i, kScenePhaseNames[i]};
  spans[kScenePhases] = {kScene - 1, kScene, "overlays"};
  return spans;
}();

}  // namespace

int runProfile(const NativeAssets& native, const std::string& planPath, int frames) {
  const ModuleCatalog* catalog = &native.catalog;
  std::ifstream in(planPath, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "profile: cannot open plan %s\n", planPath.c_str());
    return 1;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  std::optional<Station> station = importPlanXml(ss.str());
  if (!station) {
    std::fprintf(stderr, "profile: failed to parse plan %s\n", planPath.c_str());
    return 1;
  }

  const int total = std::max(frames, 1);
  const StationBounds bounds = stationBounds(*station, *catalog);
  std::printf("profile: modules=%zu radius=%.0f frames=%d\n", station->modules().size(),
              bounds.radius, total);

  const int screenW = envInt("PW", kDefaultW);
  const int screenH = envInt("PH", kDefaultH);
  const int maxFps = profileMaxFps();
  if (maxFps == 0)
    std::printf("profile: frame pacing=uncapped\n");
  else
    std::printf("profile: frame pacing=%d fps\n", maxFps);
  InitWindow(screenW, screenH, "X4 Station Builder - profile");
  // Pace between frames to limit average workstation load. Timing samples stop at
  // EndDrawing above, before this wait, so the recorded values remain render costs.
  auto nextFrame = std::chrono::steady_clock::now();
  const auto framePeriod =
      maxFps > 0 ? std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                       std::chrono::duration<double>(1.0 / static_cast<double>(maxFps)))
                 : std::chrono::steady_clock::duration::zero();
  const auto paceAfterFrame = [&] {
    if (maxFps == 0) return;
    nextFrame += framePeriod;
    const auto now = std::chrono::steady_clock::now();
    if (nextFrame > now)
      std::this_thread::sleep_until(nextFrame);
    else
      nextFrame = now;
  };
  {
    MeshCache meshes{native};
    const char* pom = std::getenv("POM");  // POM=0: the frame without X4's parallax march
    meshes.setPomEnabled(pom == nullptr || std::string_view(pom) != "0");
    const char* lightMaskEnv = std::getenv("LIGHT_MASK");  // LIGHT_MASK=7: omit local lights
    const int lightMask = lightMaskEnv != nullptr ? std::atoi(lightMaskEnv) : 15;
    meshes.setLightMask(lightMask);
    std::printf("profile: lightMask=%d\n", lightMask);
    // Same frame as the live editor (main.cpp): shadow pass, sky, lit scene, post,
    // lens flares. Default sky so runs are comparable.
    Sky sky{native};
    FramePipeline pipeline;
    EnvCubemap envCube{sky};
    meshes.setEnvMap(envCube.textureId(), envCube.maxLod());
    SunShadow sunShadow;
    // Pre-upload every mesh exactly as the live editor does on plan-open
    // (main.cpp loadStationMeshes); lazy uploads mid-run polluted the numbers.
    loadStationMeshes(*station, *catalog, meshes);

    const Vec3 dt = flipZ(bounds.center);  // display-space target (scene flips Z)
    const ::Vector3 target{static_cast<float>(dt.x), static_cast<float>(dt.y),
                           static_cast<float>(dt.z)};
    const double radius = std::max(bounds.radius, 1.0);
    constexpr double kPitch = 0.45;  // ~26 deg elevation

    // Camera for frame i, orbiting the station twice over the run so each frame
    // re-exercises culling/LOD from a fresh angle. The orbit also dollies between
    // 0.4R and 3R once per run: a cruise through the station's depth is what drives
    // mass per-part LOD switches (a fixed-distance orbit barely exercises them).
    const char* fixedDistEnv = std::getenv("PROFILE_DIST");  // e.g. 0.5: orbit at 0.5R only
    const double fixedDist = fixedDistEnv != nullptr ? std::atof(fixedDistEnv) : 0.0;
    const auto distAt = [&](int i) {
      if (fixedDist > 0.0) return fixedDist;
      const double t = static_cast<double>(i) / static_cast<double>(total);
      return 1.7 + 1.3 * std::cos(t * 2.0 * static_cast<double>(PI));
    };
    const auto cameraAt = [&](int i) {
      const double t = static_cast<double>(i) / static_cast<double>(total);
      const double yaw = t * 4.0 * static_cast<double>(PI);
      const double dist = radius * distAt(i);
      const Vec3 dir{std::cos(kPitch) * std::sin(yaw), std::sin(kPitch),
                     std::cos(kPitch) * std::cos(yaw)};
      return ::Camera3D{::Vector3{target.x + static_cast<float>(dir.x * dist),
                                  target.y + static_cast<float>(dir.y * dist),
                                  target.z + static_cast<float>(dir.z * dist)},
                        target, ::Vector3{0.0f, 1.0f, 0.0f}, 45.0f, CAMERA_PERSPECTIVE};
    };

    std::array<std::array<GLuint, kMarks>, kRing> queries{};
    for (auto& set : queries) glGenQueries(static_cast<GLsizei>(kMarks), set.data());
    // One frame, GPU- and CPU-stamped at the same boundaries; cpu[kMarks] = after
    // EndDrawing (the swap wait).
    std::array<double, kMarks + 1> cpu{};
    std::size_t slot = 0;      // ring slot of the frame being rendered (warmup included)
    std::size_t lastMark = 0;  // the frame's latest mark
    const auto stamp = [&](std::size_t m) {
      // A pass drawScene skipped still gets its mark (a zero-length span): every query in
      // the slot is read back.
      for (std::size_t k = lastMark + 1; k <= m; ++k) {
        glQueryCounter(queries[slot][k], GL_TIMESTAMP);
        cpu[k] = GetTime();
      }
      lastMark = m;
    };
    setScenePhaseHook(
        [&](ScenePhase phase) { stamp(kSceneFirst + static_cast<std::size_t>(phase)); });
    // LOD dissolves last 30 frames whatever the pacing: the camera path is per frame.
    int lodTicks = 0;
    setLodClock([&] { return static_cast<double>(lodTicks) / 30.0; });
    const auto renderFrame = [&](int i, const ::Camera3D& camera) {
      ++lodTicks;
      slot = static_cast<std::size_t>(i) % kRing;
      lastMark = kStart;
      cpu[kStart] = GetTime();
      BeginDrawing();
      glQueryCounter(queries[slot][kStart], GL_TIMESTAMP);
      sunShadow.render(*station, *catalog, meshes, sky.sun().dir, camera);
      stamp(kShadow);
      pipeline.begin();
      ClearBackground(BLACK);
      BeginMode3D(camera);
      sky.draw(camera);
      EndMode3D();
      stamp(kSky);
      drawScene(*station, *catalog, camera, meshes, /*showGizmos=*/true, /*showMeshes=*/true,
                /*allConnectors=*/false, /*lodEnabled=*/false, sky.sun(), &sky.fog());
      stamp(kScene);
      pipeline.end([&] { sky.drawLensFlares(camera, GetScreenWidth(), GetScreenHeight()); });
      stamp(kPost);
      EndDrawing();
      cpu[kMarks] = GetTime();
    };

    for (int i = 0; i < kWarmup && !WindowShouldClose(); ++i) {
      renderFrame(i, cameraAt(0));
      paceAfterFrame();
    }
    glFinish();

    // Per-frame stamps in ms from kStart, so a spike can be split into CPU phase, GPU
    // phase and swap wait, and tied to a mesh-set change (which re-renders the cached
    // shadow map).
    struct FrameDetail {
      std::array<double, kMarks + 1> cpuMs{};
      std::array<double, kMarks> gpuMs{};
      std::size_t resolved = 0;
      MeshCache::OcclusionStats occ{};
    };
    std::vector<FrameDetail> detail(static_cast<std::size_t>(total));
    std::vector<double> frameMs;
    frameMs.reserve(static_cast<std::size_t>(total));
    const auto readGpu = [&](int f) {
      const auto& set = queries[static_cast<std::size_t>(f) % kRing];
      std::array<GLuint64, kMarks> ns{};
      for (std::size_t m = 0; m < kMarks; ++m)
        glGetQueryObjectui64v(set[m], GL_QUERY_RESULT, &ns[m]);
      for (std::size_t m = 0; m < kMarks; ++m)
        detail[static_cast<std::size_t>(f)].gpuMs[m] = static_cast<double>(ns[m] - ns[0]) / 1e6;
    };
    // Under RenderDoc (tools/rdc-capture.ps1 style), RDC_FRAME=<i> captures frame i
    // of this exact workload for per-draw GPU timing.
    const char* rdcFrameEnv = std::getenv("RDC_FRAME");
    const int rdcFrame = rdcFrameEnv != nullptr && rdc::available() ? std::atoi(rdcFrameEnv) : -1;
    int frameCount = 0;
    for (int i = 0; i < total && !WindowShouldClose(); ++i, ++frameCount) {
      if (i == rdcFrame) rdc::startFrameCapture();
      {
        ZoneScopedN("frame");
        renderFrame(i, cameraAt(i));
      }
      if (i == rdcFrame) rdc::endFrameCapture();
      FrameDetail& d = detail[static_cast<std::size_t>(i)];
      for (std::size_t m = 0; m <= kMarks; ++m) d.cpuMs[m] = (cpu[m] - cpu[kStart]) * 1000.0;
      d.resolved = meshes.resolvedCount();
      d.occ = meshes.occlusionStats();
      frameMs.push_back(d.cpuMs[kMarks]);
      if (i + 1 >= static_cast<int>(kRing)) readGpu(i + 1 - static_cast<int>(kRing));
      FrameMark;  // Tracy frame boundary (no-op without the tracy build)
      paceAfterFrame();
    }
    for (int f = std::max(frameCount - static_cast<int>(kRing) + 1, 0); f < frameCount; ++f)
      readGpu(f);
    setScenePhaseHook({});
    setLodClock({});
    for (auto& set : queries) glDeleteQueries(static_cast<GLsizei>(kMarks), set.data());

    // " name cpu/gpu" mean ms of each span over the frames in `set`.
    const auto printSpans = [&](const auto& spans, const std::vector<std::size_t>& set) {
      const double count = std::max(static_cast<double>(set.size()), 1.0);
      for (const Span& s : spans) {
        double cpuSum = 0.0;
        double gpuSum = 0.0;
        for (const std::size_t f : set) {
          cpuSum += detail[f].cpuMs[s.to] - detail[f].cpuMs[s.from];
          gpuSum += detail[f].gpuMs[s.to] - detail[f].gpuMs[s.from];
        }
        std::printf(" %s %.2f/%.2f", s.name, cpuSum / count, gpuSum / count);
      }
    };
    // Phases, the swap wait (= GPU backlog), then the scene split on its own line.
    const auto printBreakdown = [&](const std::vector<std::size_t>& set, const char* scenePrefix) {
      printSpans(kPhases, set);
      double present = 0.0;
      for (const std::size_t f : set) present += detail[f].cpuMs[kMarks] - detail[f].cpuMs[kPost];
      std::printf("  present %.2f\n%s", present / std::max(static_cast<double>(set.size()), 1.0),
                  scenePrefix);
      printSpans(kSceneSpans, set);
      std::printf("\n");
    };

    // Spikes are the point of the dolly run: each with its frame index and camera
    // distance, split by phase (cpu/gpu per phase, then the swap wait).
    std::size_t prevResolved = detail.empty() ? 0 : detail[0].resolved;
    for (int f = 0; f < frameCount; ++f) {
      const FrameDetail& d = detail[static_cast<std::size_t>(f)];
      const double ms = frameMs[static_cast<std::size_t>(f)];
      if (ms > kSpikeMs) {
        std::printf("profile: spike frame %4d %6.1fms dist=%.2fR", f, ms, distAt(f));
        printSpans(kPhases, {static_cast<std::size_t>(f)});
        std::printf("  present %.1f  meshes %+lld\n", d.cpuMs[kMarks] - d.cpuMs[kPost],
                    static_cast<long long>(d.resolved) - static_cast<long long>(prevResolved));
      }
      prevResolved = d.resolved;
    }

    std::vector<std::size_t> all(static_cast<std::size_t>(frameCount));
    std::iota(all.begin(), all.end(), std::size_t{0});
    std::vector<double> gpuTotalMs;
    gpuTotalMs.reserve(all.size());
    double gpuTotalSum = 0.0;
    for (const std::size_t f : all) {
      gpuTotalMs.push_back(detail[f].gpuMs[kPost]);
      gpuTotalSum += gpuTotalMs.back();
    }
    std::sort(gpuTotalMs.begin(), gpuTotalMs.end());
    std::printf("profile: %dx%d  per-phase mean ms (cpu submit / gpu exec):", screenW, screenH);
    printBreakdown(all, "profile: scene split:");
    std::printf("profile: gpu total %.2f ms  p95 %.2f\n",
                gpuTotalSum / std::max(static_cast<double>(all.size()), 1.0),
                percentile(gpuTotalMs, 0.95));

    if (!frameMs.empty()) {
      double sum = 0.0;
      for (const double ms : frameMs) sum += ms;
      const double mean = sum / static_cast<double>(frameMs.size());

      const char* detailEnv = std::getenv("PROFILE_DETAIL");
      if (detailEnv != nullptr && std::string_view(detailEnv) != "0") {
        std::vector<std::size_t> frameOrder = all;
        std::sort(frameOrder.begin(), frameOrder.end(),
                  [&](std::size_t a, std::size_t b) { return frameMs[a] < frameMs[b]; });
        const auto p95Rank = static_cast<std::size_t>(
            std::lround(0.95 * static_cast<double>(frameOrder.size() - 1)));
        const std::size_t p95Frame = frameOrder[p95Rank];
        std::printf("profile: p95 sample frame=%zu dist=%.2fR frame=%.2fms", p95Frame,
                    distAt(static_cast<int>(p95Frame)), frameMs[p95Frame]);
        printBreakdown({p95Frame}, "  scene:");

        // The p95 is set by one distance band; per-band means show whether that band is
        // CPU-submit or GPU bound, and which scene pass carries it.
        constexpr std::array<double, 7> kBandEdges{0.4, 0.6, 0.8, 1.0, 1.5, 2.2, 3.01};
        for (std::size_t b = 0; b + 1 < kBandEdges.size(); ++b) {
          std::vector<std::size_t> band;
          std::vector<double> bandMs;
          for (int f = 0; f < frameCount; ++f) {
            const double dist = distAt(f);
            if (dist < kBandEdges[b] || dist >= kBandEdges[b + 1]) continue;
            band.push_back(static_cast<std::size_t>(f));
            bandMs.push_back(frameMs[static_cast<std::size_t>(f)]);
          }
          if (bandMs.empty()) continue;
          double bandSum = 0.0;
          for (const double ms : bandMs) bandSum += ms;
          std::sort(bandMs.begin(), bandMs.end());
          std::printf("profile: band %.1f-%.1fR n=%3zu mean %5.2f p95 %5.2f |", kBandEdges[b],
                      kBandEdges[b + 1], bandMs.size(),
                      bandSum / static_cast<double>(bandMs.size()), percentile(bandMs, 0.95));
          printBreakdown(band, "  scene:");
          MeshCache::OcclusionStats occ{};
          for (const std::size_t f : band) {
            occ.entries += detail[f].occ.entries;
            occ.visibleEntries += detail[f].occ.visibleEntries;
            occ.indices += detail[f].occ.indices;
            occ.visibleIndices += detail[f].occ.visibleIndices;
            occ.preCullIndices += detail[f].occ.preCullIndices;
          }
          // Hidden = drawn but owning no sample; pre-cull = what the CPU lists held.
          if (occ.indices != 0)
            std::printf(
                "  occlusion: indices/frame pre-cull %.1fM drawn %.1fM visible %.1fM, "
                "hidden of drawn %.1f%%, hidden entries %.1f%% (%llu/frame)\n",
                static_cast<double>(occ.preCullIndices) / 1e6 / static_cast<double>(band.size()),
                static_cast<double>(occ.indices) / 1e6 / static_cast<double>(band.size()),
                static_cast<double>(occ.visibleIndices) / 1e6 / static_cast<double>(band.size()),
                100.0 * static_cast<double>(occ.indices - occ.visibleIndices) /
                    static_cast<double>(occ.indices),
                100.0 * static_cast<double>(occ.entries - occ.visibleEntries) /
                    static_cast<double>(occ.entries),
                static_cast<unsigned long long>(occ.entries / band.size()));
        }
      }

      std::sort(frameMs.begin(), frameMs.end());
      std::printf(
          "profile: frames=%zu  mean=%.2fms (%.0f fps)  p50=%.2f  p95=%.2f  p99=%.2f  "
          "max=%.2fms\n",
          frameMs.size(), mean, mean > 0 ? 1000.0 / mean : 0.0, percentile(frameMs, 0.50),
          percentile(frameMs, 0.95), percentile(frameMs, 0.99), frameMs.back());
    }
  }
  CloseWindow();
  return 0;
}

}  // namespace x4sb::editor
