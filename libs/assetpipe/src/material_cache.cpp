#include "x4sb/assetpipe/material_library.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <map>
#include <string>
#include <type_traits>

namespace x4sb {
namespace {

template <typename Material, typename Visitor>
void fields(Material& d, Visitor&& visit) {
  visit("diffuseMap", d.diffuseMap);
  visit("colorEmissive", d.colorEmissive);
  visit("normalMap", d.normalMap);
  visit("specularMap", d.specularMap);
  visit("metalMap", d.metalMap);
  visit("smoothMap", d.smoothMap);
  visit("emissiveMap", d.emissiveMap);
  visit("diffuseDetailMap", d.diffuseDetailMap);
  visit("normalDetailMap", d.normalDetailMap);
  visit("blendMode", d.blendMode);
  visit("shader", d.shader);
  visit("diffuseStr", d.diffuseStr);
  visit("diffuseDetailTiling", d.diffuseDetailTiling);
  visit("diffuseDetailStr", d.diffuseDetailStr);
  visit("normalDetailTiling", d.normalDetailTiling);
  visit("normalDetailStr", d.normalDetailStr);
  visit("smoothness", d.smoothness);
  visit("metallness", d.metallness);
  visit("glowHue", d.glowHue);
  visit("glowSaturation", d.glowSaturation);
  visit("glowStr", d.glowStr);
  visit("properties", d.properties);
}

nlohmann::json encodeProperties(const MaterialProperties& props) {
  auto json = nlohmann::json::object();
  if (!props.floats.empty()) json["floats"] = props.floats;
  if (!props.colors.empty()) json["colors"] = props.colors;
  if (!props.bitmaps.empty()) json["bitmaps"] = props.bitmaps;
  return json;
}

// Throws json::exception on malformed input (fromCacheJson rejects the cache).
MaterialProperties decodeProperties(const nlohmann::json& json) {
  MaterialProperties props;
  if (json.contains("floats")) {
    // Via double: an out-of-range double -> float conversion is undefined.
    for (const auto& [key, value] : json.at("floats").get<std::map<std::string, double>>()) {
      if (!(std::abs(value) <= 3.0e38)) throw nlohmann::json::other_error::create(501, key, &json);
      props.floats[key] = static_cast<float>(value);
    }
  }
  if (json.contains("colors")) {
    for (const auto& [key, value] :
         json.at("colors").get<std::map<std::string, std::array<double, 4>>>()) {
      std::array<float, 4> rgba{};
      for (std::size_t i = 0; i < rgba.size(); ++i) {
        if (value[i] < 0.0 || value[i] > 255.0)
          throw nlohmann::json::other_error::create(501, key, &json);
        rgba[i] = static_cast<float>(value[i]);
      }
      props.colors[key] = rgba;
    }
  }
  if (json.contains("bitmaps"))
    props.bitmaps = json.at("bitmaps").get<std::map<std::string, std::string>>();
  return props;
}

nlohmann::json encode(const MaterialDef& material) {
  auto json = nlohmann::json::object();
  fields(material, [&](const char* key, const auto& value) {
    using Value = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<Value, std::optional<float>>) {
      if (value) json[key] = *value;
    } else if constexpr (std::is_same_v<Value, MaterialProperties>) {
      json[key] = encodeProperties(value);
    } else {
      json[key] = value;
    }
  });
  return json;
}

}  // namespace

std::string MaterialLibrary::toCacheJson() const {
  auto result = nlohmann::json::object();
  const auto defaults = encode(MaterialDef{});
  for (const auto& [name, material] : materials_) {
    auto json = encode(material);
    for (auto it = json.begin(); it != json.end();) {
      if (defaults.contains(it.key()) && defaults[it.key()] == it.value())
        it = json.erase(it);
      else
        ++it;
    }
    result[name] = std::move(json);
  }
  return result.dump();
}

std::optional<MaterialLibrary> MaterialLibrary::fromCacheJson(const std::string& bytes) {
  try {
    const auto json = nlohmann::json::parse(bytes);
    if (!json.is_object()) return std::nullopt;
    MaterialLibrary result;
    for (const auto& item : json.items()) {
      if (!item.value().is_object()) return std::nullopt;
      MaterialDef material;
      fields(material, [&](const char* key, auto& value) {
        if (!item.value().contains(key)) return;
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::optional<float>>)
          value = item.value().at(key).template get<float>();
        else if constexpr (std::is_same_v<Value, MaterialProperties>)
          value = decodeProperties(item.value().at(key));
        else
          value = item.value().at(key).template get<Value>();
      });
      result.materials_.emplace(item.key(), std::move(material));
    }
    return result;
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

}  // namespace x4sb
