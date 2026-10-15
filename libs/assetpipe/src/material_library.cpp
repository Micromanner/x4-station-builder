#include "x4sb/assetpipe/material_library.hpp"

#include "strutil.hpp"
#include "x4sb/archive/archive.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>

namespace x4sb {
namespace {

// Untrusted XML: a non-numeric or non-finite value drops the property so the
// shader default applies, as if it were never authored.
void addProperty(MaterialProperties& props, const std::string& type, const std::string& key,
                 const pugi::xml_node& prop) {
  if (key.empty()) return;
  if (type == "Float" || type == "Int" || type == "Integer") {
    const pugi::xml_attribute value = prop.attribute("value");
    const double v = value.as_double(std::nan(""));
    if (value && std::isfinite(v) && std::abs(v) <= 3.0e38)
      props.floats[key] = static_cast<float>(v);
  } else if (type == "Color") {
    std::array<float, 4> rgba{};
    const std::array<const char*, 4> channels{"r", "g", "b", "a"};
    for (std::size_t i = 0; i < channels.size(); ++i) {
      const double v = prop.attribute(channels[i]).as_double(255.0);
      rgba[i] = std::isfinite(v) ? static_cast<float>(std::clamp(v, 0.0, 255.0)) : 255.0F;
    }
    props.colors[key] = rgba;
  } else if (type == "BitMap") {
    props.bitmaps[key] = prop.attribute("value").as_string();
  }
}

}  // namespace

MaterialLibrary::MaterialLibrary(const std::string& xmlBytes) { addXml(xmlBytes); }

void MaterialLibrary::addXml(const std::string& xmlBytes) {
  pugi::xml_document doc;
  if (!doc.load_string(xmlBytes.c_str())) return;

  const auto addMaterial = [this](const std::string& collName, const pugi::xml_node& mat) {
    const std::string matName = mat.attribute("name").as_string();
    if (matName.empty()) return;

    MaterialDef def;
    def.blendMode = mat.attribute("blendmode").as_string();
    def.shader = mat.attribute("shader").as_string();
    for (const pugi::xml_node prop : mat.child("properties").children("property")) {
      const std::string type = prop.attribute("type").as_string();
      const std::string name = prop.attribute("name").as_string();
      addProperty(def.properties, type, detail::toLower(name), prop);
      if (type == "Float") {
        if (name == "diffuseStr") {
          def.diffuseStr = prop.attribute("value").as_float(1.0F);
        } else if (name == "diffuse_detail_tiling") {
          def.diffuseDetailTiling = prop.attribute("value").as_float(1.0F);
        } else if (name == "diffuse_detailStr" || name == "diffuse_detailstr") {
          def.diffuseDetailStr = prop.attribute("value").as_float(1.0F);
        } else if (name == "normal_detail_tiling") {
          def.normalDetailTiling = prop.attribute("value").as_float(1.0F);
        } else if (name == "normal_detailStr" || name == "normal_detailstr") {
          def.normalDetailStr = prop.attribute("value").as_float(1.0F);
        } else if (name == "glow_hue") {
          def.glowHue = prop.attribute("value").as_float(0.5F);
        } else if (name == "glow_saturation") {
          def.glowSaturation = prop.attribute("value").as_float(1.0F);
        } else if (name == "color_glowStr" || name == "color_glowstr") {
          def.glowStr = prop.attribute("value").as_float(1.0F);
        } else if (name == "Smoothness" || name == "smoothness") {
          def.smoothness = prop.attribute("value").as_float(1.0F);
        } else if (name == "Metallness" || name == "metallness" || name == "metalness") {
          def.metallness = prop.attribute("value").as_float(1.0F);
        }
        continue;
      }
      if (type == "Color" && name == "color_emissive") {
        const auto channel = [&prop](const char* attr) {
          const int v = prop.attribute(attr).as_int(255);
          return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
        };
        def.colorEmissive = {channel("r"), channel("g"), channel("b")};
        continue;
      }
      if (type != "BitMap") continue;
      const std::string value = prop.attribute("value").as_string();

      if (name == "diffuse_map") {
        def.diffuseMap = value;
      } else if (name == "normal_map") {
        def.normalMap = value;
      } else if (name == "specular_map") {
        def.specularMap = value;
      } else if (name == "metal_map") {
        def.metalMap = value;
      } else if (name == "smooth_map") {
        def.smoothMap = value;
      } else if (name == "diffuse_detail_map") {
        def.diffuseDetailMap = value;
      } else if (name == "normal_detail_map" || name == "normal_detailmap") {
        // normal_detailmap (no underscore): a library typo shipped in 11 materials.
        def.normalDetailMap = value;
      } else if (name == "emission_map" || name == "emissive_map" || name == "color_glow_map") {
        // color_glow_map is what the real X4 library ships (incl. the station
        // window materials); emission/emissive never occur there.
        def.emissiveMap = value;
      }
    }
    materials_[collName + "." + matName] = std::move(def);
  };

  const auto addCollection = [&addMaterial](const pugi::xml_node& coll) {
    const std::string collName = coll.attribute("name").as_string();
    if (collName.empty()) return;
    for (const pugi::xml_node mat : coll.children("material")) addMaterial(collName, mat);
  };

  // Base-game library: <materiallibrary><collection>...
  for (const pugi::xml_node coll : doc.child("materiallibrary").children("collection"))
    addCollection(coll);

  // Extension libraries are <diff> patch documents: whole collections (or bare
  // materials aimed at an existing collection via the sel xpath) sit under <add>.
  for (const pugi::xml_node add : doc.child("diff").children("add")) {
    for (const pugi::xml_node coll : add.children("collection")) addCollection(coll);

    const std::string sel = add.attribute("sel").as_string();
    const std::size_t namePos = sel.find("[@name='");
    if (namePos == std::string::npos) continue;
    const std::size_t nameStart = namePos + 8;
    const std::size_t nameEnd = sel.find('\'', nameStart);
    if (nameEnd == std::string::npos) continue;
    const std::string collName = sel.substr(nameStart, nameEnd - nameStart);
    for (const pugi::xml_node mat : add.children("material")) addMaterial(collName, mat);
  }
}

std::optional<MaterialDef> MaterialLibrary::getMaterial(const std::string& ref) const {
  const auto it = materials_.find(ref);
  if (it != materials_.end()) return it->second;
  return std::nullopt;
}

MaterialLibrary mergedMaterialLibrary(const Archive& archive) {
  MaterialLibrary lib;
  for (const std::string& prefix : archive.sources()) {
    for (const char* file :
         {"libraries/material_library.xml", "libraries/material_library_1.xml"}) {
      if (const auto bytes = archive.extract(prefix + file)) lib.addXml(*bytes);
    }
  }
  return lib;
}

}  // namespace x4sb
