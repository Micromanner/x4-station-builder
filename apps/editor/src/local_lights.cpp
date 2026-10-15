#include "local_lights.hpp"

#include "external/glad.h"
#include "raymath.h"
#include "rlgl.h"
#include "x4sb/data/math.hpp"
#include "x4sb/editorcore/display_flip.hpp"

#include <algorithm>
#include <cmath>
#include <future>
#include <iterator>
#include <thread>

namespace x4sb::editor {
namespace {

// Must match kFlatFsBody (mesh_cache.cpp).
constexpr int kTilesX = 64;
constexpr int kTilesY = 36;
constexpr int kSlices = 64;
// Below this many lights one thread bins faster than spawning workers.
constexpr std::size_t kParallelMinLights = 2048;
constexpr int kDataUnit = 12;
constexpr int kGridUnit = 13;
constexpr int kIndexUnit = 14;
// Nearest slice boundary: closer than this all shares slice 0.
constexpr double kSliceNear = 2.0;
// X4 fades small lights out (libraries/renderparam_library.xml): point lights
// "fade at 2/3 towards lodradius3" (15), spot/area/box lights have lodradius3 30.
// Read here as the projected radius in pixels: full at lodradius3, gone at 2/3.
// The library's pixels are "for a 1280:720 mainscene", whatever the window size.
constexpr double kPointFullPx = 15.0;
constexpr double kShapeFullPx = 30.0;
constexpr float kLodScreenHeight = 720.0F;

enum class Kind : int { Omni = 0, Spot = 1, Box = 2, Area = 3 };

// X4's LinearFromSRGB_Deprecated: pow 2.2, not the sRGB curve.
Vec3 decode(Vec3 rgb255, double scale) {
  const auto ch = [scale](double c) { return std::pow(std::max(0.0, c / 255.0 * scale), 2.2); };
  return {ch(rgb255.x), ch(rgb255.y), ch(rgb255.z)};
}

std::array<float, 4> texel(Vec3 v, double w) {
  return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z),
          static_cast<float>(w)};
}

// X4 fills the box light's falloff radius on the CPU from an unknown source;
// box lights without an authored range take the box's cross-section.
double boxRadius(const LocalLight& l) {
  return l.range > 0.0 ? l.range : 0.5 * std::max(l.boxSize.y, l.boxSize.z);
}

// Column-major raylib Matrix applied to a point.
std::array<double, 4> transform(const ::Matrix& m, Vec3 p) {
  const auto f = [](float v) { return static_cast<double>(v); };
  return {f(m.m0) * p.x + f(m.m4) * p.y + f(m.m8) * p.z + f(m.m12),
          f(m.m1) * p.x + f(m.m5) * p.y + f(m.m9) * p.z + f(m.m13),
          f(m.m2) * p.x + f(m.m6) * p.y + f(m.m10) * p.z + f(m.m14),
          f(m.m3) * p.x + f(m.m7) * p.y + f(m.m11) * p.z + f(m.m15)};
}

// The authored extent, which the LOD fade measures.
double lodReach(const LocalLight& l) {
  switch (l.kind) {
    case LightKind::Omni:
    case LightKind::Spot:
      return l.range;
    case LightKind::Box:
      return 0.3 * l.boxSize.x + boxRadius(l);
    case LightKind::Area:
      return 0.5 * l.areaRadius + 0.5 * std::sqrt(l.areaX * l.areaX + l.areaY * l.areaY) + l.range;
  }
  return 0.0;
}

// The sphere the light can light anything in, which bins it: the omni and box
// windows in kFlatFsBody reach zero at 0.9 x range.
double reach(const LocalLight& l) {
  switch (l.kind) {
    case LightKind::Omni:
      return 0.9 * l.range;
    case LightKind::Box:
      return 0.3 * l.boxSize.x + 0.9 * boxRadius(l);
    case LightKind::Spot:
    case LightKind::Area:
      break;
  }
  return lodReach(l);
}

}  // namespace

LocalLights::~LocalLights() {
  for (Buffer* b : {&data_, &grid_, &index_}) {
    if (b->texture != 0) glDeleteTextures(1, &b->texture);
    if (b->buffer != 0) glDeleteBuffers(1, &b->buffer);
  }
}

void LocalLights::begin() {
  view_ = rlGetMatrixModelview();
  proj_ = rlGetMatrixProjection();
  std::array<int, 4> vp{};
  glGetIntegerv(GL_VIEWPORT, vp.data());
  viewport_ = {static_cast<float>(std::max(1, vp[2])), static_cast<float>(std::max(1, vp[3]))};
  projF_ = 0.5F * kLodScreenHeight * proj_.m5;
  // Side planes through the eye, (x, y, distance) view coordinates, inside <= 0:
  // ndc = P00 * x / z - P20 within [-1, 1], likewise for y.
  const auto f = [](float v) { return static_cast<double>(v); };
  const auto plane = [](double a, double b, double c) {
    const double n = std::sqrt(a * a + b * b + c * c);
    return std::array<double, 3>{a / n, b / n, c / n};
  };
  sidePlanes_ = {
      plane(f(proj_.m0), 0.0, -(f(proj_.m8) + 1.0)), plane(-f(proj_.m0), 0.0, f(proj_.m8) - 1.0),
      plane(0.0, f(proj_.m5), -(f(proj_.m9) + 1.0)), plane(0.0, -f(proj_.m5), f(proj_.m9) - 1.0)};

  const double zn = std::max(kSliceNear, rlGetCullDistanceNear());
  const double zf = std::max(zn * 2.0, rlGetCullDistanceFar());
  const double scale = kSlices / std::log(zf / zn);
  sliceParams_ = {static_cast<float>(scale), static_cast<float>(-scale * std::log(zn))};

  records_.clear();
  radii_.clear();
  lightCount_ = 0;
}

void LocalLights::addStation(const Station& station, const ModuleCatalog& catalog,
                             std::optional<InstanceId> skip) {
  if (station.revision() != stationRevision_ || moduleLights_.empty()) {
    stationRevision_ = station.revision();
    stationLights_.clear();
    moduleLights_.clear();
    for (const PlacedModule& pm : station.modules()) {
      moduleLights_.emplace_back(pm.instanceId, stationLights_.size());
      if (const ModuleDef* def = catalog.find(pm.defId))
        worldLights(*def, pm.worldTransform, stationLights_);
    }
  }
  for (std::size_t m = 0; m < moduleLights_.size(); ++m) {
    if (skip && moduleLights_[m].first == *skip) continue;
    const std::size_t last =
        m + 1 < moduleLights_.size() ? moduleLights_[m + 1].second : stationLights_.size();
    for (std::size_t i = moduleLights_[m].second; i < last; ++i) push(stationLights_[i]);
  }
}

void LocalLights::add(const ModuleDef& def, const Transform& world) {
  scratch_.clear();
  worldLights(def, world, scratch_);
  for (const WorldLight& l : scratch_) push(l);
}

void LocalLights::push(const WorldLight& light) {
  // Cull on the cheap terms first: most of a big station's lights are too small
  // on screen to survive the LOD fade.
  const std::array<double, 4> vc = transform(view_, light.pos);
  const double z = -vc[2];
  if (z < -light.radius) return;  // wholly behind the eye
  // Up close most of a station's lights sit beside the view; binning never gives
  // them a cluster, but they would still cost the binning walk and the upload.
  for (const auto& pl : sidePlanes_)
    if (pl[0] * vc[0] + pl[1] * vc[1] + pl[2] * z > light.radius) return;
  double fade = 1.0;
  if (z > light.lodRadius) {
    const double full = light.fullPx;
    const double px = light.lodRadius / z * static_cast<double>(projF_);
    fade = std::clamp((px - full * 2.0 / 3.0) / (full / 3.0), 0.0, 1.0);
    if (fade <= 0.0) return;
  }
  const auto f = static_cast<float>(fade);
  for (std::size_t t = 0; t < kTexelsPerLight; ++t) {
    std::array<float, 4> texel = light.texels[t];
    if (t == 1)
      for (std::size_t ch = 0; ch < 3; ++ch) texel[ch] *= f;
    records_.push_back(texel);
  }
  radii_.push_back(light.radius);
}

void LocalLights::worldLights(const ModuleDef& def, const Transform& world,
                              std::vector<WorldLight>& out) {
  for (const LocalLight& l : def.lights) {
    const double radius = reach(l);
    if (radius <= 0.0) continue;
    const Vec3 pos = flipZ(x4sb::apply(world, l.local.position));

    const Quat rot = world.rotation * l.local.rotation;
    const auto axis = [&rot](Vec3 v) { return flipZ(rotate(rot, v)); };
    Kind kind = Kind::Omni;
    Vec3 color{};
    double spec = l.specularIntensity;
    std::array<float, 4> t2{};
    std::array<float, 4> t3{};
    std::array<float, 4> t4{};
    std::array<float, 4> t5{};
    switch (l.kind) {
      case LightKind::Omni:
        // Point/spot/box take intensity premultiplied into the colour before the
        // decode (as V_lightcolor1 does for the sun; inferred for these).
        color = decode(l.color, l.intensity);
        t2 = texel({}, l.range);
        break;
      case LightKind::Spot:
        kind = Kind::Spot;
        color = decode(l.color, l.intensity);
        // The cone shader treats the angle as a half-angle (it scales the
        // proxy by tan): X4 authors the full cone, up to 179 degrees.
        t2 = texel(axis({0, -1, 0}), l.range);
        t3 = texel({}, l.spotAngle * 0.5);
        break;
      case LightKind::Box: {
        kind = Kind::Box;
        // The emitting line spans +-0.3 of size.x (l_pass_boxlight.vert).
        color = decode(l.color, l.intensity);
        t2 = texel(axis({1, 0, 0}), boxRadius(l));
        t3 = texel({}, 0.3 * l.boxSize.x);
        break;
      }
      case LightKind::Area: {
        kind = Kind::Area;
        const double lr = l.areaRadius * 0.5;
        const double hx = l.areaX * 0.5;
        const double hy = l.areaY * 0.5;
        // Area lights decode the bare colour, then scale (l_pass_arealight.vert).
        color = decode(l.color, 1.0) * l.intensity;
        spec = std::clamp(spec, 0.0, 1.0);
        const double sa = std::clamp(l.spotAttenuation, 0.0, 1.0);
        const double spotAtten = -std::cos(sa * 3.14159265358979) * 0.5 + 0.5;
        t2 = texel(axis({0, -1, 0}), l.range);
        t3 = texel(axis({1, 0, 0}), lr);
        t4 = texel(axis({0, 0, 1}), 0.0);
        t5 = texel({hx, hy, spotAtten}, 0.0);
        break;
      }
    }
    if (std::max({color.x, color.y, color.z}) <= 0.0) continue;

    t4[3] = static_cast<float>(kind);
    WorldLight& w = out.emplace_back();
    w.texels = {texel(pos, radius), texel(color, spec), t2, t3, t4, t5};
    w.pos = pos;
    w.radius = radius;
    w.lodRadius = lodReach(l);
    w.fullPx = l.kind == LightKind::Omni ? kPointFullPx : kShapeFullPx;
  }
}

void LocalLights::commit() {
  const std::size_t lights = radii_.size();
  const double nearC = std::max(rlGetCullDistanceNear(), 1e-3);
  const double farC = rlGetCullDistanceFar();
  constexpr std::size_t kClusters = static_cast<std::size_t>(kTilesX) * kTilesY * kSlices;
  const auto f = [](float v) { return static_cast<double>(v); };
  // Symmetric-or-not perspective: ndc = P00 * x / z - P20 with z the view distance.
  const std::array<double, 2> pScale{f(proj_.m0), f(proj_.m5)};
  const std::array<double, 2> pShift{f(proj_.m8), f(proj_.m9)};
  const std::array<int, 2> tiles{kTilesX, kTilesY};
  const double sa = static_cast<double>(sliceParams_[0]);
  const double sb = static_cast<double>(sliceParams_[1]);
  const auto sliceOf = [sa, sb](double z) {
    const double s = std::log(std::max(z, 1e-6)) * sa + sb;
    return std::clamp(static_cast<int>(std::floor(s)), 0, kSlices - 1);
  };
  // The outer slices also hold everything nearer / farther than their log bound.
  const auto sliceNear = [&](int s) { return s == 0 ? nearC : std::exp((s - sb) / sa); };
  const auto sliceFar = [&](int s) {
    return s == kSlices - 1 ? std::max(farC, nearC) : std::exp((s + 1 - sb) / sa);
  };
  const auto tileOf = [](double ndc, int n) {
    return std::clamp(static_cast<int>(std::floor((ndc * 0.5 + 0.5) * n)), 0, n - 1);
  };
  const auto tileNdc = [](int t, int n) { return 2.0 * t / n - 1.0; };

  // (cluster, light) pairs, counting-sorted into per-cluster ranges below. A
  // shipyard close-up bins ~22k lights into ~380k pairs, ~6 ms on one thread
  // (known-issues 1.5), so light ranges bin in parallel; concatenating the
  // workers' lists in order keeps each cluster's lights in index order.
  const std::size_t workers =
      lights < kParallelMinLights
          ? 1
          : std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);
  pairs_.resize(workers);
  const auto binRange = [&](std::size_t w, std::size_t first, std::size_t last) {
    std::vector<std::array<std::uint32_t, 2>>& out = pairs_[w];
    out.clear();
    std::size_t touchedLights = 0;
    for (std::size_t i = first; i < last; ++i) {
      const std::array<float, 4>& t0 = records_[i * kTexelsPerLight];
      const std::array<double, 4> vc = transform(view_, {t0[0], t0[1], t0[2]});
      const std::array<double, 2> c{vc[0], vc[1]};
      const double cz = -vc[2];
      const double r = radii_[i];
      if (cz + r < nearC || cz - r > farC) continue;
      bool touched = false;
      for (int s = sliceOf(cz - r); s <= sliceOf(cz + r); ++s) {
        const double zlo = sliceNear(s);
        const double zhi = sliceFar(s);
        const double za = std::max(zlo, cz - r);
        const double zb = std::min(zhi, cz + r);
        if (za > zb) continue;
        // The sphere's widest cross-section inside this slab bounds it on screen.
        const double dz = cz - std::clamp(cz, za, zb);
        const double rs = std::sqrt(std::max(0.0, r * r - dz * dz));
        std::array<int, 2> t0i{};
        std::array<int, 2> t1i{};
        bool onScreen = true;
        for (std::size_t a = 0; a < 2; ++a) {
          const double lo = c[a] - rs;
          const double hi = c[a] + rs;
          const double n0 = pScale[a] * std::min(lo / za, lo / zb) - pShift[a];
          const double n1 = pScale[a] * std::max(hi / za, hi / zb) - pShift[a];
          if (n1 < -1.0 || n0 > 1.0) onScreen = false;
          t0i[a] = tileOf(n0, tiles[a]);
          t1i[a] = tileOf(n1, tiles[a]);
        }
        if (!onScreen) continue;
        for (int ty = t0i[1]; ty <= t1i[1]; ++ty)
          for (int tx = t0i[0]; tx <= t1i[0]; ++tx) {
            // Exact sphere vs the cell's view-space box: the screen rect alone keeps
            // the rect's corners, which a big sphere's silhouette never reaches.
            double d2 = 0.0;
            const std::array<int, 2> t{tx, ty};
            for (std::size_t a = 0; a < 2; ++a) {
              const double n0 = tileNdc(t[a], tiles[a]) + pShift[a];
              const double n1 = tileNdc(t[a] + 1, tiles[a]) + pShift[a];
              const double lo = std::min(n0 * zlo, n0 * zhi) / pScale[a];
              const double hi = std::max(n1 * zlo, n1 * zhi) / pScale[a];
              const double e = std::max({lo - c[a], 0.0, c[a] - hi});
              d2 += e * e;
            }
            const double ez = std::max({zlo - cz, 0.0, cz - zhi});
            if (d2 + ez * ez > r * r) continue;
            out.push_back({static_cast<std::uint32_t>((s * kTilesY + ty) * kTilesX + tx),
                           static_cast<std::uint32_t>(i)});
            touched = true;
          }
      }
      if (touched) ++touchedLights;
    }
    return touchedLights;
  };
  const auto rangeStart = [&](std::size_t w) { return lights * w / workers; };
  std::vector<std::future<std::size_t>> jobs;
  for (std::size_t w = 1; w < workers; ++w)
    jobs.push_back(std::async(std::launch::async, binRange, w, rangeStart(w), rangeStart(w + 1)));
  lightCount_ = binRange(0, 0, rangeStart(1));
  for (auto& job : jobs) lightCount_ += job.get();

  clusters_.assign(kClusters, {0, 0});
  for (const auto& list : pairs_)
    for (const auto& [cluster, light] : list) ++clusters_[cluster][1];
  std::uint32_t total = 0;
  for (auto& cl : clusters_) {
    cl[0] = total;
    total += cl[1];
    cl[1] = 0;
  }
  indices_.assign(std::max<std::uint32_t>(total, 1), 0);
  for (const auto& list : pairs_)
    for (const auto& [cluster, light] : list) {
      std::array<std::uint32_t, 2>& cl = clusters_[cluster];
      indices_[cl[0] + cl[1]] = light;
      ++cl[1];
    }

  if (records_.empty()) records_.resize(kTexelsPerLight);
  upload(data_, GL_RGBA32F, records_.data(), records_.size() * sizeof(records_[0]));
  upload(grid_, GL_RG32UI, clusters_.data(), clusters_.size() * sizeof(clusters_[0]));
  upload(index_, GL_R32UI, indices_.data(), indices_.size() * sizeof(indices_[0]));
  for (const auto& [unit, b] : {std::pair{kDataUnit, &data_}, std::pair{kGridUnit, &grid_},
                                std::pair{kIndexUnit, &index_}}) {
    glActiveTexture(GL_TEXTURE0 + static_cast<unsigned int>(unit));
    glBindTexture(GL_TEXTURE_BUFFER, b->texture);
  }
  glActiveTexture(GL_TEXTURE0);
}

void LocalLights::upload(Buffer& b, unsigned int internalFormat, const void* data,
                         std::size_t bytes) {
  if (b.buffer == 0) glGenBuffers(1, &b.buffer);
  if (b.texture == 0) glGenTextures(1, &b.texture);
  glBindBuffer(GL_TEXTURE_BUFFER, b.buffer);
  glBufferData(GL_TEXTURE_BUFFER, static_cast<GLsizeiptr>(bytes), data, GL_STREAM_DRAW);
  glBindTexture(GL_TEXTURE_BUFFER, b.texture);
  glTexBuffer(GL_TEXTURE_BUFFER, internalFormat, b.buffer);
  glBindTexture(GL_TEXTURE_BUFFER, 0);
  glBindBuffer(GL_TEXTURE_BUFFER, 0);
}

void LocalLights::apply(FrameData& frame) const {
  const ::float16 view = MatrixToFloatV(view_);
  std::copy(std::begin(view.v), std::end(view.v), frame.lightView.begin());
  frame.clusterViewport = viewport_;
  frame.sliceParams = sliceParams_;
  frame.hasLocalLights = lightCount_ > 0 ? 1 : 0;
}

}  // namespace x4sb::editor
