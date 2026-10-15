#pragma once
#include "raylib.h"
#include "x4sb/data/uv_animation.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace x4sb::editor {
struct NativeAssets;

// X4's V_lightcolor1 / V_ambient1 for one group of planet draws.
struct PlanetLight {
  ::Vector3 color;
  ::Vector3 ambient;
};

// Planets, moons, atmospheres and rings of the current sky, lit with ports of
// X4's own planet shaders (docs/2026-09-23-planet-rendering-investigation.md).
// Drawn inside the sky pass with a projection of their own: real metre
// geometry needs near ~1e5 m / far ~2e9 m, which the station pass cannot share.
class PlanetPass {
 public:
  PlanetPass();  // compiles the shaders: the GL context must be live
  ~PlanetPass();
  PlanetPass(const PlanetPass&) = delete;
  PlanetPass& operator=(const PlanetPass&) = delete;
  PlanetPass(PlanetPass&&) = delete;
  PlanetPass& operator=(PlanetPass&&) = delete;

  // The background manifest's `planets` array (hostile: malformed entries and
  // unsupported shaders are skipped). Replaces whatever was loaded.
  void load(const NativeAssets& native, const nlohmann::json& planets);
  void unload();

  // Call from the sky pass (inside BeginMode3D, after stars and sun glow).
  // `sunDir` points at the sun; bodies and shells/rings take different light
  // encodings (see Sky::draw). Clears depth before and after, so the station
  // pass starts from an empty buffer.
  void draw(const ::Camera3D& camera, ::Vector3 sunDir, const PlanetLight& bodyLight,
            const PlanetLight& shellLight) const;

  [[nodiscard]] std::size_t missingAssets() const { return missingAssets_; }

  // Animation clock (seconds) for F_time and the texture scroll; 0 is a still frame.
  void setTime(double seconds) { time_ = seconds; }

  static constexpr std::size_t kProgramCount = 13;  // Program's enumerators

 private:
  // One per ported X4 shader variant.
  enum class Program : std::uint8_t {
    Earthlike,
    Xenon,
    EarthlikeMv,
    EarthlikeMvUn,
    Moon,
    Gas,
    GasMv,
    GasMvUn,
    Sun,
    Atmosphere,
    Ring,
    Aurora,
    Flare,  // additive p1_complex_unlit on a sun: the ring shader, blended One/One
  };

  struct Gpu {
    ::Shader shader{};
    bool ok{false};
    int mvp{-1};
    int worldRot{-1};
    int worldPos{-1};
    int sunDir{-1};
    int lightColor{-1};
    int ambient{-1};
    int globalLightScale{-1};
    int time{-1};
    int uvLinear{-1};
    int uvOffset{-1};
    int animateUv0{-1};
    int alphaScale{-1};
    int vertexData{-1};
  };

  struct Texture {
    int samplerLoc{-1};
    unsigned int id{0};
  };

  struct Part {
    ::Model model{};
    Program program{Program::Earthlike};
    ::Vector3 pos{};  // metres, display space, relative to the viewing sector
    ::Matrix rotation{};
    float radius{0.0F};
    bool vertexColors{false};
    bool hazeBlend{false};  // material blendmode HAZEBLEND (auroras)
    UvAnimation uvAnimation;
    std::vector<std::pair<int, float>> floats;
    std::vector<std::pair<int, ::Vector4>> colors;
    std::vector<std::pair<int, int>> bools;
    std::vector<Texture> textures;
  };

  [[nodiscard]] static std::optional<Program> programFor(const std::string& kind,
                                                         const std::string& shader);
  // 0 opaque bodies, 1 atmospheres, 2 rings, 3 auroras and flares: draw order and blending.
  [[nodiscard]] static int drawLayer(Program program);
  void drawPart(const Part& part, const ::Matrix& viewProj, ::Vector3 sunDir,
                const PlanetLight& light) const;
  unsigned int texture(const NativeAssets& native, const std::string& address, bool srgb);

  std::array<Gpu, kProgramCount> programs_{};
  std::vector<Part> parts_;
  // Planets share textures (clouds, city lights) across bodies; keyed by
  // (archive address, sRGB decode).
  std::map<std::pair<std::string, bool>, ::Texture2D> textures_;
  unsigned int blankTexture_{0};
  std::size_t missingAssets_{0};
  double time_{0.0};
  float near_{1.0e6F};
  float far_{2.0e9F};
};

}  // namespace x4sb::editor
