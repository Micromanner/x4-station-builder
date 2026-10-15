#include "x4sb/data/uv_animation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace x4sb {

using nlohmann::json;

namespace {

// Slope at key k for Smooth segments, approximating the authoring tool's "auto"
// tangent: flat at plateaus and turning points (so blink curves hold their
// levels), the neighbour secant elsewhere. Flat at the end keys too, so a two-key
// curve eases in and out: the energycells poslights scroll one such curve and
// in-game their side lights (crossing the slow ends) flash longer than the tips.
double smoothSlope(const std::vector<UvKey>& keys, std::size_t k) {
  const std::size_t n = keys.size();
  if (k == 0 || k == n - 1) return 0.0;
  const double in = keys[k].value - keys[k - 1].value;
  const double out = keys[k + 1].value - keys[k].value;
  if (in * out <= 0.0) return 0.0;
  return (keys[k + 1].value - keys[k - 1].value) / (keys[k + 1].frame - keys[k - 1].frame);
}

double evalCurve(const std::vector<UvKey>& keys, double frame) {
  if (keys.empty()) return 0.0;
  if (frame <= keys.front().frame) return keys.front().value;
  if (frame >= keys.back().frame) return keys.back().value;
  const auto hi = std::upper_bound(keys.begin(), keys.end(), frame,
                                   [](double f, const UvKey& key) { return f < key.frame; });
  const std::size_t b = static_cast<std::size_t>(hi - keys.begin());
  const std::size_t a = b - 1;
  const UvKey& ka = keys[a];
  const UvKey& kb = keys[b];
  const double h = kb.frame - ka.frame;  // > 0: upper_bound skips equal frames
  const double t = (frame - ka.frame) / h;
  switch (ka.interp) {
    case UvInterp::Step:
      return ka.value;
    case UvInterp::Linear:
      return ka.value + (kb.value - ka.value) * t;
    case UvInterp::Smooth:
      break;
  }
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double v = (2 * t3 - 3 * t2 + 1) * ka.value + (t3 - 2 * t2 + t) * h * smoothSlope(keys, a) +
                   (-2 * t3 + 3 * t2) * kb.value + (t3 - t2) * h * smoothSlope(keys, b);
  // Hermite can overshoot a segment; X4's clamped tangents do not.
  return std::clamp(v, std::min(ka.value, kb.value), std::max(ka.value, kb.value));
}

bool inRange(double v) { return std::abs(v) <= kMaxUvAnimationMagnitude; }

json keysToJson(const std::vector<UvKey>& keys) {
  json out = json::array();
  for (const UvKey& k : keys) out.push_back({k.frame, k.value, static_cast<int>(k.interp)});
  return out;
}

std::vector<UvKey> keysFromJson(const json& j) {
  std::vector<UvKey> keys;
  if (!j.is_array()) return keys;
  for (const json& k : j) {
    if (!k.is_array() || k.size() != 3 || !k[0].is_number() || !k[1].is_number() ||
        !k[2].is_number_integer())
      return {};
    const int interp = k[2].get<int>();
    if (interp < 0 || interp > static_cast<int>(UvInterp::Step)) return {};
    const double frame = k[0].get<double>();
    const double value = k[1].get<double>();
    if (!inRange(frame) || !inRange(value)) return {};
    keys.push_back({frame, value, static_cast<UvInterp>(interp)});
  }
  const bool sorted = std::is_sorted(
      keys.begin(), keys.end(), [](const UvKey& x, const UvKey& y) { return x.frame < y.frame; });
  return sorted ? keys : std::vector<UvKey>{};
}

}  // namespace

UvTransforms evalUvAnimation(const UvAnimation& anim, double seconds) {
  UvTransforms out;
  if (anim.channels.empty()) return out;
  const double span = anim.loopEnd - anim.loopStart;
  const double frame = span > 0.0
                           ? anim.loopStart + std::fmod(std::max(seconds, 0.0) * kAnimFps, span)
                           : anim.loopStart;
  for (const UvChannel& c : anim.channels) {
    if (c.uvSet < 0 || c.uvSet >= kUvSets) continue;
    const auto set = static_cast<std::size_t>(c.uvSet);
    // X4 composes M_texturematrix on the CPU, so the order is inferred (3ds Max's
    // UV transform): uv' = R * S * (uv - pivot) + pivot + offset.
    const double angle = evalCurve(c.rotation, frame) * std::acos(-1.0) / 180.0;
    const double su = c.uTile.empty() ? 1.0 : evalCurve(c.uTile, frame);
    const double sv = c.vTile.empty() ? 1.0 : evalCurve(c.vTile, frame);
    const double cs = std::cos(angle);
    const double sn = std::sin(angle);
    const double l00 = cs * su;
    const double l10 = sn * su;
    const double l01 = -sn * sv;
    const double l11 = cs * sv;
    const double pivot = c.aroundCenter ? 0.5 : 0.0;
    out.linear[set] = {static_cast<float>(l00), static_cast<float>(l10), static_cast<float>(l01),
                       static_cast<float>(l11)};
    out.offset[set].u = static_cast<float>(evalCurve(c.u, frame) + pivot - (l00 + l01) * pivot);
    out.offset[set].v = static_cast<float>(evalCurve(c.v, frame) + pivot - (l10 + l11) * pivot);
  }
  return out;
}

json uvAnimationToJson(const UvAnimation& anim) {
  json channels = json::array();
  for (const UvChannel& c : anim.channels) {
    json jc = {{"set", c.uvSet}, {"u", keysToJson(c.u)}, {"v", keysToJson(c.v)}};
    // Rare (sun coronas, a few station fx): written only when present.
    if (!c.rotation.empty()) jc["rot"] = keysToJson(c.rotation);
    if (!c.uTile.empty()) jc["uTile"] = keysToJson(c.uTile);
    if (!c.vTile.empty()) jc["vTile"] = keysToJson(c.vTile);
    if (c.aroundCenter) jc["center"] = true;
    channels.push_back(std::move(jc));
  }
  return {{"loop", {anim.loopStart, anim.loopEnd}}, {"channels", std::move(channels)}};
}

UvAnimation uvAnimationFromJson(const json& j) {
  UvAnimation anim;
  if (!j.is_object()) return anim;
  const json loop = j.value("loop", json::array());
  if (loop.is_array() && loop.size() == 2 && loop[0].is_number() && loop[1].is_number() &&
      inRange(loop[0].get<double>()) && inRange(loop[1].get<double>())) {
    anim.loopStart = loop[0].get<double>();
    anim.loopEnd = loop[1].get<double>();
  }
  const json channels = j.value("channels", json::array());
  if (!channels.is_array()) return anim;
  for (const json& c : channels) {
    if (!c.is_object()) continue;
    const json set = c.value("set", json());
    if (!set.is_number_integer()) continue;
    const int uvSet = set.get<int>();
    if (uvSet < 0 || uvSet >= kUvSets) continue;
    UvChannel ch;
    ch.uvSet = uvSet;
    ch.u = keysFromJson(c.value("u", json()));
    ch.v = keysFromJson(c.value("v", json()));
    ch.rotation = keysFromJson(c.value("rot", json()));
    ch.uTile = keysFromJson(c.value("uTile", json()));
    ch.vTile = keysFromJson(c.value("vTile", json()));
    const json center = c.value("center", json());
    ch.aroundCenter = center.is_boolean() && center.get<bool>();
    anim.channels.push_back(std::move(ch));
  }
  return anim;
}

}  // namespace x4sb
