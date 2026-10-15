#pragma once
// The cluster's sky-sphere meshes, shaded by ports of the X4 shaders their materials
// name: p1_simple_unlit (the vertex-coloured nebula dome, drawn opaque), p1_stars
// (star atlas x a UV1 mask) and p1_starfield (tiled star layers + procedural pixel
// stars), each blended as its material says. sun.rdc: dome blend off, star layers
// ONE/ONE; SCREEN only on the two clusters whose material asks for it.
// Needs a live GL context: construct after InitWindow, destroy before CloseWindow.
#include "raylib.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace x4sb {
struct MaterialDef;
}

namespace x4sb::editor {
struct NativeAssets;

class SkyDomes {
 public:
  SkyDomes();
  ~SkyDomes();
  SkyDomes(const SkyDomes&) = delete;
  SkyDomes& operator=(const SkyDomes&) = delete;
  SkyDomes(SkyDomes&&) = delete;
  SkyDomes& operator=(SkyDomes&&) = delete;

  // The manifest's "domes" array. Throws json errors on malformed entries.
  void load(const NativeAssets& native, const nlohmann::json& domes);
  void unload();

  // Camera-locked, inside BeginMode3D with depth and culling off. `skyRadius` is the
  // distance the meshes are scaled to; `starMotion` is p1_starfield's camera-motion dim.
  void draw(float skyRadius, float starMotion) const;

  [[nodiscard]] std::size_t count() const { return domes_.size(); }
  [[nodiscard]] std::size_t missingAssets() const { return missingAssets_; }

 private:
  enum class Program : std::uint8_t { SimpleUnlit, Stars, Starfield };
  static constexpr std::size_t kProgramCount = 3;
  enum class Blend : std::uint8_t { Opaque, Additive, Screen };

  // One mesh range's material, resolved to the values its X4 shader reads.
  struct Layer {
    bool drawn{true};  // false: material not in the library
    Program program{Program::SimpleUnlit};
    Blend blend{Blend::Opaque};
    ::Vector4 color{1.0F, 1.0F, 1.0F, 1.0F};  // p1_stars: (color_emissive, diffuse_color.a)
    std::array<bool, 3> hasMap{};             // diffuse, diffuse2, diffuse3
    std::array<float, 3> strength{1.0F, 1.0F, 1.0F};
    std::array<float, 3> tiling{1.0F, 1.0F, 1.0F};
    ::Vector4 layer1Boost{0.25F, 0.8F, 10.0F, 60.0F};  // boostmin, boostmax, fovmin, fovmax
    ::Vector4 layer2Boost{0.0F, 1.0F, 3.0F, 10.0F};
  };
  struct Dome {
    ::Model model{};
    std::vector<Layer> layers;  // per mesh
    ::Vector3 pos{};            // display-space, manifest units
    ::Vector4 quat{};           // [x,y,z,w]
    float extent{1.0F};
  };
  struct ProgramGpu {
    ::Shader shader{};
    std::array<int, 3> hasMapLocs{};
    int color{-1};
    int strength{-1};
    int tiling{-1};
    int layer1Boost{-1};
    int layer2Boost{-1};
    int projScale{-1};
    int invProjection{-1};
    int invView{-1};
    int viewport{-1};
    int starMotion{-1};
  };

  [[nodiscard]] static Layer layerFor(const MaterialDef& def);
  static void setBlend(Blend blend);
  [[nodiscard]] ::Texture2D texture(const NativeAssets& native, const std::string& address);
  void drawLayer(const Dome& d, int mesh, float skyRadius, float starMotion) const;

  std::array<ProgramGpu, kProgramCount> programs_{};
  std::vector<Dome> domes_;
  std::map<std::string, ::Texture2D> textures_;  // by archive address, failures included
  std::size_t missingAssets_{};
};

}  // namespace x4sb::editor
