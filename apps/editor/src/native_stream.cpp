#include "native_stream.hpp"

#include "x4sb/assetpipe/native_background.hpp"
#include "x4sb/assetpipe/xmf.hpp"

#include <algorithm>
#include <cctype>

namespace x4sb::editor {

std::string normalizeTexturePath(const std::string& path) {
  std::string normalized = path;
  for (auto& ch : normalized)
    ch = ch == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return normalized;
}

bool isComplexUnlit(const MaterialDef& material) {
  return material.shader == "p1_complex_unlit" || isForcefield(material);
}

bool isForcefield(const MaterialDef& material) {
  return material.shader == "p1_complex_unlit_forcefield";
}

bool isSimpleUnlit(const MaterialDef& material) {
  return material.shader == "p1_simple_unlit" || material.shader == "xu_simple_hdr_out";
}

bool isLightcone(const MaterialDef& material) {
  return material.shader == "xu_lightcone" || material.shader == "xu_lightcone_env" ||
         material.shader == "xu_lightcone_alpha";
}

bool usesHullLayers(const MaterialDef& material) {
  return material.shader.rfind("p1_complex_surface", 0) == 0 || isMultimat(material) ||
         usesHeightMask(material) || material.shader == "p1_complex_projection" ||
         material.shader == "p2_complex_projection";
}

bool isPortedFx(const MaterialDef& material) {
  return usesHullLayers(material) || isComplexUnlit(material) || isSimpleUnlit(material) ||
         isLightcone(material);
}

bool isBoron(const MaterialDef& material) {
  return material.shader.rfind("p2_complex_boron", 0) == 0;
}

bool isBoronAnimglow(const MaterialDef& material) {
  return material.shader == "p2_complex_boron_animglow";
}

bool usesHeightMask(const MaterialDef& material) {
  return material.shader.rfind("p2_complex_surface", 0) == 0 || isBoron(material);
}

bool isMultimat(const MaterialDef& material) {
  return isP1Multimat(material) || isP2Multimat(material);
}

bool isP1Multimat(const MaterialDef& material) { return material.shader == "p1_multimat"; }

bool isP2Multimat(const MaterialDef& material) { return material.shader == "p2_multimat"; }

std::string textureKey(const TextureRef& ref) {
  return std::string(ref.array ? "array:" : "") + (ref.srgb ? "srgb:" : "") + ref.path;
}

std::vector<TextureRef> materialTextures(const MaterialDef& material) {
  std::vector<TextureRef> refs;
  const std::array<std::pair<const std::string*, bool>, 7> maps{
      {{&material.diffuseMap, true},
       {&material.normalMap, false},
       {&material.emissiveMap, false},
       {&material.metalMap, false},
       {&material.smoothMap, false},
       {&material.diffuseDetailMap, true},
       {&material.normalDetailMap, false}}};
  for (const auto& [path, srgb] : maps)
    if (!path->empty()) refs.push_back({normalizeTexturePath(*path), srgb});
  if (usesHullLayers(material)) {
    for (const char* layer : kHullLayerMaps) {
      const auto it = material.properties.bitmaps.find(layer);
      if (it != material.properties.bitmaps.end() && !it->second.empty())
        refs.push_back({normalizeTexturePath(it->second), true});
    }
  }
  if (usesHeightMask(material)) {
    const auto it = material.properties.bitmaps.find(kHeightMap);
    if (it != material.properties.bitmaps.end() && !it->second.empty())
      refs.push_back({normalizeTexturePath(it->second), false});
  }
  if (!material.emissiveMap.empty()) {
    for (std::size_t k = 0; k < kGlowDetailMaps.size(); ++k) {
      const auto it = material.properties.bitmaps.find(kGlowDetailMaps[k]);
      if (it != material.properties.bitmaps.end() && !it->second.empty())
        refs.push_back({normalizeTexturePath(it->second), glowDetailIsColor(k)});
    }
  }
  if (isMultimat(material)) {
    for (std::size_t k = 0; k < kMultimatArrayMaps.size(); ++k) {
      const auto it = material.properties.bitmaps.find(kMultimatArrayMaps[k]);
      if (it != material.properties.bitmaps.end() && !it->second.empty())
        refs.push_back({normalizeTexturePath(it->second), multimatArrayIsColor(k), true});
    }
  }
  if (isComplexUnlit(material)) {
    for (std::size_t k = 0; k < kComplexUnlitLayers.size(); ++k) {
      const auto it = material.properties.bitmaps.find(kComplexUnlitLayers[k]);
      if (it != material.properties.bitmaps.end() && !it->second.empty())
        refs.push_back({normalizeTexturePath(it->second), complexUnlitLayerIsColor(k)});
    }
  }
  return refs;
}

NativeStreamer::NativeStreamer(ExtractFn extract, MaterialLibrary materials,
                               std::unordered_map<std::string, NativePart> parts)
    : extract_(std::move(extract)), materials_(std::move(materials)), parts_(std::move(parts)) {
  // Leave a core for the render thread; beyond ~6 the shared DAT reads dominate.
  const unsigned hardware = std::max(2U, std::thread::hardware_concurrency());
  const unsigned count = std::min(6U, hardware - 1);
  for (unsigned i = 0; i < count; ++i) workers_.emplace_back([this] { run(); });
}

NativeStreamer::~NativeStreamer() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  jobReady_.notify_all();
  for (auto& worker : workers_) worker.join();
}

void NativeStreamer::requestModel(const std::string& key, bool urgent) {
  {
    const std::lock_guard lock(mutex_);
    if (urgent)
      jobs_.push_front({false, key});
    else
      jobs_.push_back({false, key});
  }
  jobReady_.notify_one();
}

NativeStreamer::Completed NativeStreamer::takeCompleted(std::chrono::milliseconds wait) {
  std::unique_lock lock(mutex_);
  if (wait.count() > 0)
    resultReady_.wait_for(
        lock, wait, [this] { return !completed_.models.empty() || !completed_.textures.empty(); });
  Completed result;
  std::swap(result, completed_);
  return result;
}

StreamedModel NativeStreamer::loadModel(const std::string& key) {
  StreamedModel result;
  result.key = key;
  const auto source = parts_.find(key);
  if (source == parts_.end()) return result;
  const auto& xmfPath = source->second.xmfPath;
  auto bytes = extract_(xmfPath);
  // Some parts ship only a coarser LOD. Never substitute one for an explicitly
  // requested sibling: the renderer already handles absent levels itself.
  const auto lod0 = xmfPath.rfind("-lod0.xmf");
  if (!bytes && lod0 != std::string::npos) {
    for (int lod = 1; lod < 4 && !bytes; ++lod)
      bytes = extract_(xmfPath.substr(0, lod0) + "-lod" + std::to_string(lod) + ".xmf");
  }
  if (!bytes) return result;
  const auto parsed = parseXmf(*bytes);
  if (!parsed) return result;
  const std::size_t ranges = std::max<std::size_t>(parsed->submeshes.size(), 1);
  for (std::size_t i = 0; i < ranges; ++i) {
    const auto& materialRef =
        parsed->submeshes.empty() ? source->second.materialRef : parsed->submeshes[i].materialRef;
    result.materials.push_back(materials_.getMaterial(materialRef).value_or(MaterialDef{}));
  }
  result.effect = source->second.effect;
  if (result.effect && std::none_of(result.materials.begin(), result.materials.end(), isPortedFx)) {
    result.materials.clear();
    return result;
  }
  std::vector<bool> extendedVertexData(result.materials.size());
  std::transform(result.materials.begin(), result.materials.end(), extendedVertexData.begin(),
                 [](const MaterialDef& m) { return isP2Multimat(m); });
  result.part = prepareStationPart(*parsed, extendedVertexData);
  if (!result.part) result.materials.clear();
  result.vertexColors = !parsed->colors.empty();
  result.paintUv = !parsed->texcoords2.empty();
  return result;
}

void NativeStreamer::run() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      jobReady_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
      if (stopping_) return;
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    // A worker exception would std::terminate the editor; report it as missing.
    if (job.texture) {
      StreamedTexture texture{{job.name, job.srgb, job.array}, std::nullopt};
      try {
        texture.dds = readTextureDds(extract_, job.name);
      } catch (const std::exception&) {
        texture.dds.reset();
      }
      const std::lock_guard lock(mutex_);
      completed_.textures.push_back(std::move(texture));
    } else {
      StreamedModel model;
      try {
        model = loadModel(job.name);
      } catch (const std::exception&) {
        model = StreamedModel{job.name, std::nullopt, {}};
      }
      const std::lock_guard lock(mutex_);
      // Textures jump the queue: the model can't be drawn until they land.
      for (const auto& material : model.materials)
        for (auto& ref : materialTextures(material))
          if (claimedTextures_.insert(textureKey(ref)).second)
            jobs_.push_front({true, std::move(ref.path), ref.srgb, ref.array});
      completed_.models.push_back(std::move(model));
      jobReady_.notify_all();
    }
    resultReady_.notify_all();
  }
}

}  // namespace x4sb::editor
