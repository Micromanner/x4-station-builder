#include "x4sb/assetpipe/lensfx.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace x4sb::assetpipe {
namespace {

// Backstops against a hostile/corrupt library, far above real data
// (base game: 115 effects, largest ~30 elements).
constexpr std::size_t kMaxEffects = 4096;
constexpr std::size_t kMaxElements = 256;

bool finiteNonNegative(float v) { return std::isfinite(v) && v >= 0.0F; }

void parseEffectNode(const pugi::xml_node& fxNode, LensEffectMap& out) {
  const std::string name = fxNode.attribute("name").as_string();
  if (name.empty() || out.size() >= kMaxEffects) return;
  LensEffect fx;
  const float scale = fxNode.child("parameters").attribute("scale").as_float(100.0F);
  if (std::isfinite(scale) && scale > 0.0F) fx.scale = scale;
  for (pugi::xml_node el = fxNode.child("elements").child("element"); el;
       el = el.next_sibling("element")) {
    if (fx.elements.size() >= kMaxElements) break;
    if (!el.attribute("active").as_bool(true)) continue;
    LensElement e;
    e.part = el.attribute("part").as_string();
    e.position = el.attribute("position").as_float(1.0F);
    e.minSize = el.attribute("minsize").as_float(0.0F);
    e.maxSize = el.attribute("maxsize").as_float(0.0F);
    e.rotation = el.attribute("rotation").as_float(0.0F);
    e.glow = el.attribute("glow").as_bool(false);
    e.shineBehind = el.attribute("shinebehind").as_bool(false);
    if (e.part.empty()) continue;
    if (!std::isfinite(e.position)) continue;
    if (!finiteNonNegative(e.minSize) || !finiteNonNegative(e.maxSize)) continue;
    if (!std::isfinite(e.rotation)) continue;
    fx.elements.push_back(std::move(e));
  }
  out[name] = std::move(fx);  // last writer wins, both within a doc and across merges
}

}  // namespace

LensEffectMap parseLensEffects(const std::string& xml) {
  LensEffectMap out;
  pugi::xml_document doc;
  if (!doc.load_buffer(xml.data(), xml.size())) return out;
  for (pugi::xml_node fx = doc.child("lenseffects").child("lenseffect"); fx;
       fx = fx.next_sibling("lenseffect"))
    parseEffectNode(fx, out);
  return out;
}

void mergeLensEffectsDiff(LensEffectMap& effects, const std::string& diffXml) {
  pugi::xml_document doc;
  if (!doc.load_buffer(diffXml.data(), diffXml.size())) return;
  for (pugi::xml_node add = doc.child("diff").child("add"); add; add = add.next_sibling("add")) {
    if (std::string(add.attribute("sel").as_string()) != "/lenseffects") continue;
    for (pugi::xml_node fx = add.child("lenseffect"); fx; fx = fx.next_sibling("lenseffect"))
      parseEffectNode(fx, effects);
  }
}

FlareMesh flareMeshFromXmf(const XmfMesh& mesh) {
  FlareMesh out;
  // Backstop against a hostile/corrupt part: real flare parts are 9-150 verts.
  constexpr std::size_t kMaxVerts = 65536;
  const std::size_t n = mesh.positions.size();
  if (n == 0 || n > kMaxVerts || mesh.indices.size() % 3 != 0) return out;
  for (std::size_t i = 0; i < n; ++i) {
    const float x = static_cast<float>(mesh.positions[i].x);
    const float y = static_cast<float>(mesh.positions[i].y);
    if (!std::isfinite(x) || !std::isfinite(y)) return {};
    out.xy.push_back(x);
    out.xy.push_back(y);
    float u = 0.0F;
    float v = 0.0F;
    if (i < mesh.texcoords.size()) {
      u = static_cast<float>(mesh.texcoords[i].x);
      v = static_cast<float>(mesh.texcoords[i].y);
      if (!std::isfinite(u) || !std::isfinite(v)) return {};
    }
    out.uv.push_back(u);
    out.uv.push_back(v);
    // Alpha is a mask channel in X4 vertex colors and ONE/ONE ignores it.
    const std::array<std::uint8_t, 4> c =
        i < mesh.colors.size() ? mesh.colors[i] : std::array<std::uint8_t, 4>{255, 255, 255, 255};
    out.rgb.insert(out.rgb.end(), {c[0], c[1], c[2]});
  }
  for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
    const std::uint32_t i0 = mesh.indices[t];
    const std::uint32_t i1 = mesh.indices[t + 1];
    const std::uint32_t i2 = mesh.indices[t + 2];
    if (i0 >= n || i1 >= n || i2 >= n) continue;
    out.index.insert(out.index.end(), {i0, i1, i2});
  }
  if (out.index.empty()) return {};
  return out;
}

std::optional<SunSprite> makeSunSprite(const LensElement& el, float effectScale, double sunDistance,
                                       const std::string& texFile, float diffuseStr) {
  // Two authored size regimes share one attribute: world metres at the sun's
  // distance (halo/star sprites, >= ~1e8) vs screen-relative units (rays,
  // small glows, <= ~200). 1e6 splits them with orders of magnitude to spare.
  constexpr float kWorldSizeCutoff = 1.0e6F;
  // The captured frame draws every screen element at its MAXSIZE (rays 40-70
  // -> 70, glow 160-190 -> 190); minsize is the far-off-axis end of X4's
  // in-view variation, which a static view never reaches.
  const float size = el.maxSize > 0.0F ? el.maxSize : el.minSize;
  if (!(size > 0.0F)) return std::nullopt;
  SunSprite s;
  s.texFile = texFile;
  s.rotation = el.rotation;
  s.glow = el.glow;
  s.shineBehind = el.shineBehind;
  s.position = el.position;
  // Backstop hostile material data; 100 is far above any shipped diffuseStr.
  if (std::isfinite(diffuseStr) && diffuseStr > 0.0F) s.intensity = std::min(diffuseStr, 100.0F);
  if (size >= kWorldSizeCutoff) {
    if (!(sunDistance > 0.0)) return std::nullopt;
    s.screenMode = false;
    const double halfSize = 0.5 * static_cast<double>(size);
    s.size = static_cast<float>(2.0 * std::atan(halfSize / sunDistance));
  } else {
    s.screenMode = true;
    s.size = size / effectScale;  // parseLensEffects guarantees scale > 0
  }
  return s;
}

}  // namespace x4sb::assetpipe
