#include "x4sb/assetpipe/nebula_fog.hpp"

#include "strutil.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>

namespace x4sb::assetpipe {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Spheres per size^3 of boundary volume: 496 basefill spheres in cluster_14's
// 500 km sphere of size 100 km. Only the sphere boundary is calibrated; the
// cylinder, box and unbounded rules reuse it.
constexpr double kVolumeFill = 496.0 / (4.0 / 3.0 * kPi * 125.0);
// Spheres per size of spline length x density x mean lateral falloff: 197 along
// cluster_14's 667 km tube (density 1.5, mean lateral 2.305, size 23 km).
constexpr double kTubeFill = 1.9635;
// Captured tube centres sit up to ~1.5 r off the centreline, median ~0.67 r.
constexpr double kTubeRadialScale = 1.2;
constexpr std::size_t kMaxSplinePoints = 256;
constexpr std::size_t kMaxFalloffSteps = 64;
constexpr std::size_t kMaxFogFields = 8;
constexpr std::size_t kMaxPlacements = 256;
constexpr int kArcSamples = 64;
// Rejection sampling gives up after this many tries per sphere, and a whole field
// build after this many in total: near-zero acceptance must not hang a sky load.
constexpr std::size_t kAttemptsPerSphere = 64;
constexpr std::size_t kMaxAttempts = kMaxFogSpheres * kAttemptsPerSphere;
// Hostile-input bounds: far past any shipped value, small enough that no
// generated quantity overflows a float or an integer cast.
constexpr double kMaxExtent = 1e8;
constexpr double kMinFogSize = 1.0;
constexpr double kMaxFogSize = 1e6;
constexpr double kMaxFactor = 1e3;

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

// False for NaN too.
bool inRange(double v, double lo, double hi) { return v >= lo && v <= hi; }

bool bounded(Vec3 v) {
  return inRange(v.x, -kMaxExtent, kMaxExtent) && inRange(v.y, -kMaxExtent, kMaxExtent) &&
         inRange(v.z, -kMaxExtent, kMaxExtent);
}

Vec3 vec3Attr(const pugi::xml_node& n, const char* x, const char* y, const char* z) {
  return {n.attribute(x).as_double(), n.attribute(y).as_double(), n.attribute(z).as_double()};
}

std::vector<FalloffStep> parseSteps(const pugi::xml_node& node) {
  std::vector<FalloffStep> steps;
  for (const pugi::xml_node s : node.children("step")) {
    if (steps.size() == kMaxFalloffSteps) break;
    const FalloffStep step{s.attribute("position").as_double(), s.attribute("value").as_double()};
    if (inRange(step.position, -kMaxFactor, kMaxFactor) &&
        inRange(step.value, -kMaxFactor, kMaxFactor))
      steps.push_back(step);
  }
  std::sort(steps.begin(), steps.end(),
            [](const FalloffStep& a, const FalloffStep& b) { return a.position < b.position; });
  return steps;
}

// Piecewise linear, clamped at the ends; no steps = no falloff.
double falloff(const std::vector<FalloffStep>& steps, double x) {
  if (steps.empty()) return 1.0;
  if (x <= steps.front().position) return steps.front().value;
  for (std::size_t i = 1; i < steps.size(); ++i) {
    if (x > steps[i].position) continue;
    const double span = steps[i].position - steps[i - 1].position;
    const double t = span > 0.0 ? (x - steps[i - 1].position) / span : 1.0;
    return steps[i - 1].value + (steps[i].value - steps[i - 1].value) * t;
  }
  return steps.back().value;
}

double falloffMax(const std::vector<FalloffStep>& steps) {
  double m = steps.empty() ? 1.0 : 0.0;
  for (const FalloffStep& s : steps) m = std::max(m, s.value);
  return m;
}

double falloffMean(const std::vector<FalloffStep>& steps) {
  if (steps.empty()) return 1.0;
  double area = 0.0;
  for (std::size_t i = 1; i < steps.size(); ++i)
    area += (steps[i].position - steps[i - 1].position) * (steps[i].value + steps[i - 1].value) / 2;
  return area;
}

std::uint32_t fnv1a(std::string_view s) {
  std::uint32_t h = 2166136261U;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 16777619U;
  }
  return h;
}

// mt19937's output sequence is fixed by the standard; the std distributions are
// not, so uniforms are mapped by hand to keep layouts identical across toolchains.
class Rng {
 public:
  explicit Rng(std::uint32_t seed) : engine_(seed) {}
  double uniform() { return static_cast<double>(engine_() >> 8U) * 0x1p-24; }
  double uniform(double lo, double hi) { return lo + (hi - lo) * uniform(); }

 private:
  std::mt19937 engine_;
};

// 1D value noise in [0, 1): our stand-in for X4's region noise, which leaves
// whole stretches of a tube empty.
double valueNoise(double x, std::uint32_t seed) {
  const auto lattice = [seed](double i) {
    // Wrapped first: the integer cast of an out-of-range double is undefined.
    const double wrapped = std::fmod(i, 4294967296.0);
    std::uint32_t h = seed ^ static_cast<std::uint32_t>(static_cast<std::int64_t>(wrapped));
    h ^= h >> 16U;
    h *= 0x7feb352dU;
    h ^= h >> 15U;
    h *= 0x846ca68bU;
    h ^= h >> 16U;
    return static_cast<double>(h >> 8U) * 0x1p-24;
  };
  const double i = std::floor(x);
  const double f = x - i;
  const double s = f * f * (3.0 - 2.0 * f);
  return lattice(i) + (lattice(i + 1.0) - lattice(i)) * s;
}

double sphereRadius(const VolumetricFogDef& fog, Rng& rng) {
  return fog.size * kFogRadiusScale * rng.uniform(1.0 - fog.sizeVariation, 1.0 + fog.sizeVariation);
}

std::size_t sphereCount(double n) {
  if (!(n > 0.0)) return 0;
  return static_cast<std::size_t>(std::min(std::round(n), static_cast<double>(kMaxFogSpheres)));
}

// Uniform in the boundary, kept with the falloff weight.
std::vector<FogSphere> generateVolume(const RegionDef& region, const VolumetricFogDef& fog,
                                      Rng& rng, std::size_t& budget) {
  const double r = region.boundary == RegionBoundary::Unbounded ? kFogRange : region.radius;
  const Vec3& box = region.box;
  double volume = 0.0;
  switch (region.boundary) {
    case RegionBoundary::Sphere:
    case RegionBoundary::Unbounded:
      volume = 4.0 / 3.0 * kPi * r * r * r;
      break;
    case RegionBoundary::Cylinder:
      volume = kPi * r * r * region.linear;
      break;
    case RegionBoundary::Box:
      volume = box.x * box.y * box.z;
      break;
    case RegionBoundary::SplineTube:
      break;
  }
  const std::size_t n =
      sphereCount(kVolumeFill * region.density * volume / (fog.size * fog.size * fog.size));
  const double radialMax = falloffMax(region.radial);
  const double lateralMax = falloffMax(region.lateral);
  // An all-zero falloff authors an empty region (and would divide 0 by 0).
  if (!(radialMax > 0.0) || !(lateralMax > 0.0)) return {};
  std::vector<FogSphere> out;
  for (std::size_t attempt = 0; out.size() < n && attempt < n * kAttemptsPerSphere && budget > 0;
       ++attempt, --budget) {
    Vec3 p{};
    double weight = 1.0;
    if (region.boundary == RegionBoundary::Box) {
      p = {rng.uniform(-0.5, 0.5) * box.x, rng.uniform(-0.5, 0.5) * box.y,
           rng.uniform(-0.5, 0.5) * box.z};
    } else if (region.boundary == RegionBoundary::Cylinder) {
      p = {rng.uniform(-r, r), rng.uniform(-0.5, 0.5) * region.linear, rng.uniform(-r, r)};
      const double rho = std::hypot(p.x, p.z) / r;
      if (rho > 1.0) continue;
      weight = falloff(region.radial, rho) / radialMax *
               falloff(region.lateral, p.y / region.linear + 0.5) / lateralMax;
    } else {
      p = {rng.uniform(-r, r), rng.uniform(-r, r), rng.uniform(-r, r)};
      const double rho = length(p) / r;
      if (rho > 1.0) continue;
      weight = falloff(region.radial, rho) / radialMax;
    }
    if (rng.uniform() >= weight) continue;
    out.push_back({p, sphereRadius(fog, rng), static_cast<float>(fog.multiplier), 0});
  }
  return out;
}

struct ArcSample {
  Vec3 pos;
  double length;  // from the spline start
};

std::vector<FogSphere> generateTube(const RegionDef& region, const VolumetricFogDef& fog, Rng& rng,
                                    std::uint32_t noiseSeed, std::size_t& budget) {
  std::vector<ArcSample> arc;
  for (std::size_t k = 0; k + 1 < region.spline.size(); ++k) {
    for (int i = arc.empty() ? 0 : 1; i <= kArcSamples; ++i) {
      const Vec3 p =
          bezierPoint(region.spline[k], region.spline[k + 1], static_cast<double>(i) / kArcSamples);
      arc.push_back({p, arc.empty() ? 0.0 : arc.back().length + length(p - arc.back().pos)});
    }
  }
  if (arc.size() < 2 || !(arc.back().length > 0.0)) return {};
  const double total = arc.back().length;
  const std::size_t n =
      sphereCount(kTubeFill * region.density * falloffMean(region.lateral) * total / fog.size);
  const double lateralMax = falloffMax(region.lateral);
  const double radialMax = falloffMax(region.radial);
  if (!(radialMax > 0.0) || !(lateralMax > 0.0)) return {};
  const double noiseRange = std::max(region.maxNoise - region.minNoise, 1e-6);
  std::vector<FogSphere> out;
  for (std::size_t attempt = 0; out.size() < n && attempt < n * kAttemptsPerSphere && budget > 0;
       ++attempt, --budget) {
    const double s = rng.uniform() * total;
    const double noise = std::clamp(
        (valueNoise(s / (2.0 * fog.size), noiseSeed) - region.minNoise) / noiseRange, 0.0, 1.0);
    if (rng.uniform() >= falloff(region.lateral, s / total) / lateralMax * noise) continue;
    // pdf proportional to t (uniform over the disc area), shaped by the radial falloff.
    const double t = std::sqrt(rng.uniform());
    if (rng.uniform() >= falloff(region.radial, t) / radialMax) continue;

    const auto hi = std::lower_bound(arc.begin() + 1, arc.end() - 1, s,
                                     [](const ArcSample& a, double v) { return a.length < v; });
    const ArcSample& a = *(hi - 1);
    const double span = hi->length - a.length;
    const Vec3 centre = a.pos + (hi->pos - a.pos) * (span > 0.0 ? (s - a.length) / span : 0.0);
    const Vec3 dir = hi->pos - a.pos;
    const double dl = length(dir);
    if (!(dl > 0.0)) continue;
    const Vec3 tangent = dir * (1.0 / dl);
    const Vec3 helper = std::abs(tangent.y) < 0.9 ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
    const Vec3 u = cross(tangent, helper) * (1.0 / length(cross(tangent, helper)));
    const Vec3 v = cross(tangent, u);
    const double angle = rng.uniform() * 2.0 * kPi;
    const double off = t * region.radius * kTubeRadialScale;
    const Vec3 p = centre + u * (off * std::cos(angle)) + v * (off * std::sin(angle));
    out.push_back({p, sphereRadius(fog, rng), static_cast<float>(fog.multiplier), 0});
  }
  return out;
}

std::optional<PhaseFunction> phaseFunctionOf(std::string_view name) {
  if (name == "MEDIUM_PHASEFUNC_ISOTROPIC") return PhaseFunction::Isotropic;
  if (name == "MEDIUM_PHASEFUNC_RAYLEIGH") return PhaseFunction::Rayleigh;
  if (name == "MEDIUM_PHASEFUNC_HG") return PhaseFunction::HenyeyGreenstein;
  if (name == "MEDIUM_PHASEFUNC_MIEHAZY") return PhaseFunction::MieHazy;
  if (name == "MEDIUM_PHASEFUNC_MIEMURKY") return PhaseFunction::MieMurky;
  return std::nullopt;
}

// Coefficients are per metre, around 1e-4 at most: anything beyond kMaxFactor
// is hostile, and checking in double keeps the float narrowing defined.
std::optional<std::array<float, 3>> narrow3(double r, double g, double b) {
  if (!inRange(r, -kMaxFactor, kMaxFactor) || !inRange(g, -kMaxFactor, kMaxFactor) ||
      !inRange(b, -kMaxFactor, kMaxFactor))
    return std::nullopt;
  return std::array<float, 3>{static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
}

std::optional<std::array<float, 3>> rgbScaled(const pugi::xml_node& n, double scale) {
  return narrow3(n.attribute("r").as_double() * scale, n.attribute("g").as_double() * scale,
                 n.attribute("b").as_double() * scale);
}

bool numbers(const nlohmann::json& a, std::size_t n) {
  if (!a.is_array() || a.size() < n) return false;
  for (std::size_t i = 0; i < n; ++i)
    if (!a[i].is_number()) return false;
  return true;
}

// Hostile manifest/override input: shape-checked, never thrown through.
std::optional<std::array<float, 3>> floats3(const nlohmann::json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || !numbers(*it, 3)) return std::nullopt;
  return narrow3((*it)[0].get<double>(), (*it)[1].get<double>(), (*it)[2].get<double>());
}

std::optional<std::string> stringOf(const nlohmann::json& obj, const char* key) {
  const auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

}  // namespace

const RegionDef* RegionLibrary::find(std::string_view name) const {
  std::string key(name);
  for (int hop = 0; hop < 8; ++hop) {
    if (const auto it = regions.find(key); it != regions.end()) return &it->second;
    const auto alias = aliases.find(key);
    if (alias == aliases.end()) return nullptr;
    key = alias->second;
  }
  return nullptr;
}

std::vector<RegionPlacement> parseClusterRegions(std::string_view clustersXml,
                                                 std::string_view clusterMacro) {
  pugi::xml_document doc;
  if (!doc.load_buffer(clustersXml.data(), clustersXml.size())) return {};
  const std::string cluster = detail::toLower(std::string(clusterMacro));
  std::vector<RegionPlacement> out;
  for (const pugi::xml_node macro : doc.child("macros").children("macro")) {
    if (detail::toLower(macro.attribute("name").as_string()) != cluster) continue;
    for (const pugi::xml_node conn : macro.child("connections").children("connection")) {
      if (out.size() == kMaxPlacements) return out;
      if (std::string_view(conn.attribute("ref").as_string()) != "regions") continue;
      RegionPlacement p;
      p.region =
          conn.child("macro").child("properties").child("region").attribute("ref").as_string();
      const pugi::xml_node offset = conn.child("offset");
      p.offset = vec3Attr(offset.child("position"), "x", "y", "z");
      if (const pugi::xml_node q = offset.child("quaternion")) {
        p.rotation = {q.attribute("qw").as_double(1.0), q.attribute("qx").as_double(),
                      q.attribute("qy").as_double(), q.attribute("qz").as_double()};
      }
      const double qn = std::sqrt(p.rotation.w * p.rotation.w + p.rotation.x * p.rotation.x +
                                  p.rotation.y * p.rotation.y + p.rotation.z * p.rotation.z);
      if (p.region.empty() || !bounded(p.offset) || !(qn > 1e-9) || !std::isfinite(qn)) continue;
      p.rotation = {p.rotation.w / qn, p.rotation.x / qn, p.rotation.y / qn, p.rotation.z / qn};
      out.push_back(std::move(p));
    }
  }
  return out;
}

void parseRegionDefinitions(std::string_view xml, RegionLibrary& into) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return;
  const pugi::xml_node root = doc.child("regions");
  for (const pugi::xml_node a : root.children("alias")) {
    const std::string name = a.attribute("name").as_string();
    const std::string ref = a.attribute("ref").as_string();
    if (!name.empty() && !ref.empty()) into.aliases[name] = ref;
  }
  for (const pugi::xml_node r : root.children("region")) {
    RegionDef def;
    def.name = r.attribute("name").as_string();
    def.density = r.attribute("density").as_double(1.0);
    def.minNoise = r.attribute("minnoisevalue").as_double(0.0);
    def.maxNoise = r.attribute("maxnoisevalue").as_double(1.0);
    if (def.name.empty() || !inRange(def.density, 0.0, kMaxFactor) ||
        !inRange(def.minNoise, -kMaxFactor, kMaxFactor) ||
        !inRange(def.maxNoise, -kMaxFactor, kMaxFactor))
      continue;

    const pugi::xml_node boundary = r.child("boundary");
    const std::string_view cls = boundary.attribute("class").as_string();
    const pugi::xml_node size = boundary.child("size");
    def.radius = size.attribute("r").as_double();
    if (!boundary) {
      def.boundary = RegionBoundary::Unbounded;
    } else if (cls == "sphere") {
      def.boundary = RegionBoundary::Sphere;
    } else if (cls == "cylinder") {
      def.boundary = RegionBoundary::Cylinder;
      def.linear = size.attribute("linear").as_double();
      if (!inRange(def.linear, kMinFogSize, kMaxExtent)) continue;
    } else if (cls == "box") {
      def.boundary = RegionBoundary::Box;
      def.box = vec3Attr(size, "x", "y", "z");
      if (!inRange(def.box.x, kMinFogSize, kMaxExtent) ||
          !inRange(def.box.y, kMinFogSize, kMaxExtent) ||
          !inRange(def.box.z, kMinFogSize, kMaxExtent))
        continue;
    } else if (cls == "splinetube") {
      def.boundary = RegionBoundary::SplineTube;
      for (const pugi::xml_node s : boundary.children("splineposition")) {
        if (def.spline.size() == kMaxSplinePoints) break;
        SplinePoint p{vec3Attr(s, "x", "y", "z"), vec3Attr(s, "tx", "ty", "tz"),
                      s.attribute("inlength").as_double(), s.attribute("outlength").as_double()};
        const double tl = length(p.tangent);
        if (!bounded(p.pos) || !inRange(tl, 1e-6, kMaxFactor) ||
            !inRange(p.inLength, 0.0, kMaxExtent) || !inRange(p.outLength, 0.0, kMaxExtent))
          continue;
        p.tangent = p.tangent * (1.0 / tl);
        def.spline.push_back(p);
      }
      if (def.spline.size() < 2) continue;
    } else {
      continue;
    }
    const bool needsRadius = def.boundary == RegionBoundary::Sphere ||
                             def.boundary == RegionBoundary::Cylinder ||
                             def.boundary == RegionBoundary::SplineTube;
    if (needsRadius && !inRange(def.radius, kMinFogSize, kMaxExtent)) continue;

    def.lateral = parseSteps(r.child("falloff").child("lateral"));
    def.radial = parseSteps(r.child("falloff").child("radial"));
    for (const pugi::xml_node f : r.child("fields").children("volumetricfog")) {
      if (def.fogs.size() == kMaxFogFields) break;
      VolumetricFogDef fog;
      fog.medium = f.attribute("medium").as_string();
      fog.texture = f.attribute("texture").as_string();
      fog.multiplier = f.attribute("multiplier").as_double(1.0);
      fog.size = f.attribute("size").as_double();
      fog.sizeVariation = f.attribute("sizevariation").as_double(0.0);
      fog.seed = f.attribute("seed").as_uint();
      if (fog.medium.empty() || !inRange(fog.size, kMinFogSize, kMaxFogSize) ||
          !inRange(fog.multiplier, 0.0, kMaxFactor) || !std::isfinite(fog.sizeVariation))
        continue;
      fog.sizeVariation = std::clamp(fog.sizeVariation, 0.0, 1.0);
      def.fogs.push_back(std::move(fog));
    }
    into.regions[def.name] = std::move(def);
  }
}

void parseMediumLibrary(std::string_view xml, MediumLibrary& into) {
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return;
  for (const pugi::xml_node m : doc.child("library").children("medium")) {
    FogMedium medium;
    medium.name = m.attribute("name").as_string();
    const double scale = m.attribute("scale").as_double();
    if (medium.name.empty() || !inRange(scale, 0.0, kMaxFactor)) continue;
    std::array<double, 3> ext{};
    for (const pugi::xml_node p : m.children("phase")) {
      if (medium.phases.size() == kMaxFogPhases) break;
      const auto function = phaseFunctionOf(p.attribute("function").as_string());
      const auto sigmaS = rgbScaled(p, p.attribute("scale").as_double() * scale);
      const double shape = p.attribute("shape").as_double();
      if (!function || !sigmaS || !inRange(shape, -1.0, 1.0)) continue;
      for (std::size_t c = 0; c < 3; ++c) ext[c] += static_cast<double>((*sigmaS)[c]);
      medium.phases.push_back({*function, *sigmaS, static_cast<float>(shape)});
    }
    // Absorption may be negative (sector nebulae cancel their own extinction).
    const pugi::xml_node absorption = m.child("absorption");
    const auto a = rgbScaled(absorption, absorption.attribute("scale").as_double() * scale);
    const auto e =
        a ? narrow3(ext[0] + static_cast<double>((*a)[0]), ext[1] + static_cast<double>((*a)[1]),
                    ext[2] + static_cast<double>((*a)[2]))
          : std::nullopt;
    if (!e) continue;
    medium.sigmaExt = *e;
    into[medium.name] = std::move(medium);
  }
}

Vec3 bezierPoint(const SplinePoint& a, const SplinePoint& b, double t) {
  const Vec3 c1 = a.pos + a.tangent * a.outLength;
  const Vec3 c2 = b.pos - b.tangent * b.inLength;
  const double u = 1.0 - t;
  return a.pos * (u * u * u) + c1 * (3.0 * u * u * t) + c2 * (3.0 * u * t * t) +
         b.pos * (t * t * t);
}

namespace {
std::vector<FogSphere> generate(const RegionDef& region, const VolumetricFogDef& fog,
                                std::size_t& budget) {
  const std::uint32_t seed = fog.seed ^ fnv1a(region.name);
  Rng rng(seed);
  return region.boundary == RegionBoundary::SplineTube
             ? generateTube(region, fog, rng, seed, budget)
             : generateVolume(region, fog, rng, budget);
}
}  // namespace

std::vector<FogSphere> generateFogSpheres(const RegionDef& region, const VolumetricFogDef& fog) {
  std::size_t budget = kMaxAttempts;
  return generate(region, fog, budget);
}

FogField buildFogField(const std::vector<RegionPlacement>& placements, const RegionLibrary& regions,
                       const MediumLibrary& media, Vec3 origin) {
  FogField field;
  field.origin = origin;
  std::size_t budget = kMaxAttempts;
  for (const RegionPlacement& placement : placements) {
    const RegionDef* region = regions.find(placement.region);
    if (region == nullptr) continue;
    for (const VolumetricFogDef& fog : region->fogs) {
      const auto medium = media.find(fog.medium);
      if (medium == media.end()) continue;
      const auto known = std::find_if(field.media.begin(), field.media.end(),
                                      [&](const FogMedium& m) { return m.name == fog.medium; });
      auto index = static_cast<std::size_t>(known - field.media.begin());
      if (known == field.media.end()) {
        if (field.media.size() == kMaxFogMedia) continue;
        field.media.push_back(medium->second);
        field.media.back().pattern = fog.texture;
      }
      for (FogSphere s : generate(*region, fog, budget)) {
        if (field.spheres.size() == kMaxFogSpheres) return field;
        s.pos = placement.offset + rotate(placement.rotation, s.pos);
        s.medium = static_cast<int>(index);
        field.spheres.push_back(s);
      }
    }
  }
  return field;
}

nlohmann::json fogFieldToJson(const FogField& field) {
  nlohmann::json media = nlohmann::json::array();
  for (const FogMedium& m : field.media) {
    nlohmann::json phases = nlohmann::json::array();
    for (const FogPhase& p : m.phases)
      phases.push_back(
          {{"fn", static_cast<int>(p.function)}, {"sigmaS", p.sigmaS}, {"shape", p.shape}});
    media.push_back({{"name", m.name},
                     {"pattern", m.pattern},
                     {"sigmaExt", m.sigmaExt},
                     {"phases", std::move(phases)}});
  }
  nlohmann::json spheres = nlohmann::json::array();
  for (const FogSphere& s : field.spheres) {
    if (s.medium < 0 || static_cast<std::size_t>(s.medium) >= field.media.size()) continue;
    spheres.push_back({s.pos.x, s.pos.y, s.pos.z, s.radius, s.density,
                       field.media[static_cast<std::size_t>(s.medium)].name});
  }
  return {{"origin", {field.origin.x, field.origin.y, field.origin.z}},
          {"media", std::move(media)},
          {"spheres", std::move(spheres)}};
}

FogField fogFieldFromJson(const nlohmann::json& j) {
  FogField field;
  if (!j.is_object()) return field;
  if (const auto o = j.find("origin"); o != j.end() && numbers(*o, 3)) {
    const Vec3 origin{(*o)[0].get<double>(), (*o)[1].get<double>(), (*o)[2].get<double>()};
    if (finite(origin)) field.origin = origin;
  }
  const auto media = j.find("media");
  if (media == j.end() || !media->is_array()) return field;
  for (const auto& m : *media) {
    if (field.media.size() == kMaxFogMedia) break;
    if (!m.is_object()) continue;
    const auto name = stringOf(m, "name");
    const auto pattern = stringOf(m, "pattern");
    const auto sigmaExt = floats3(m, "sigmaExt");
    const auto phases = m.find("phases");
    if (!name || !pattern || !sigmaExt || phases == m.end() || !phases->is_array()) continue;
    FogMedium medium{*name, *pattern, *sigmaExt, {}};
    for (const auto& p : *phases) {
      if (medium.phases.size() == kMaxFogPhases) break;
      if (!p.is_object()) continue;
      const auto fn = p.find("fn");
      const auto shape = p.find("shape");
      const auto sigmaS = floats3(p, "sigmaS");
      if (fn == p.end() || !fn->is_number_integer() || shape == p.end() || !shape->is_number() ||
          !sigmaS)
        continue;
      const int function = fn->get<int>();
      const double g = shape->get<double>();
      if (function < 0 || function > static_cast<int>(PhaseFunction::MieMurky) ||
          !inRange(g, -1.0, 1.0))
        continue;
      medium.phases.push_back(
          {static_cast<PhaseFunction>(function), *sigmaS, static_cast<float>(g)});
    }
    field.media.push_back(std::move(medium));
  }
  field.spheres = fogSpheresFromJson(j, field.media);
  return field;
}

std::vector<FogSphere> fogSpheresFromJson(const nlohmann::json& j,
                                          const std::vector<FogMedium>& media) {
  std::vector<FogSphere> out;
  if (!j.is_object()) return out;
  const auto spheres = j.find("spheres");
  if (spheres == j.end() || !spheres->is_array()) return out;
  for (const auto& s : *spheres) {
    if (out.size() == kMaxFogSpheres) break;
    if (!numbers(s, 5) || s.size() < 6 || !s[5].is_string()) continue;
    const std::string name = s[5].get<std::string>();
    const auto medium = std::find_if(media.begin(), media.end(),
                                     [&](const FogMedium& m) { return m.name == name; });
    const Vec3 pos{s[0].get<double>(), s[1].get<double>(), s[2].get<double>()};
    const double radius = s[3].get<double>();
    const double density = s[4].get<double>();
    if (medium == media.end() || !bounded(pos) || !inRange(radius, kMinFogSize, kMaxExtent) ||
        !inRange(density, 0.0, kMaxFactor))
      continue;
    out.push_back(
        {pos, radius, static_cast<float>(density), static_cast<int>(medium - media.begin())});
  }
  return out;
}

}  // namespace x4sb::assetpipe
