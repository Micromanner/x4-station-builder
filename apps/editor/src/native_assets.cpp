#include "native_assets.hpp"

#include "raygui.h"
#include "x4sb/assetpipe/metadata_cache.hpp"
#include "x4sb/platform/platform.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace x4sb::editor {

std::optional<std::string> chooseGameDirectory(const std::string& initial,
                                               bool previousLoadFailed) {
  std::array<char, 2048> path{};
  const auto setPath = [&path](const std::string& text) {
    path = {};
    std::copy_n(text.begin(), std::min(text.size(), path.size() - 1), path.begin());
  };
  std::error_code ec;
  const std::string steamDefault = "C:/Program Files (x86)/Steam/steamapps/common/X4 Foundations";
  setPath(!initial.empty() || !std::filesystem::exists(steamDefault, ec) ? initial : steamDefault);
  while (!WindowShouldClose()) {
    // raygui's text box has no clipboard support. Read the keys before
    // EndDrawing: its PollInputEvents clears this frame's presses.
    const bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    if ((ctrl && IsKeyPressed(KEY_V)) || (shift && IsKeyPressed(KEY_INSERT))) {
      if (const char* clip = GetClipboardText();
          clip != nullptr && std::string_view(clip).size() > 0) {
        std::string text(clip);
        // Explorer's "Copy as path" wraps the folder in quotes.
        text.erase(std::remove(text.begin(), text.end(), '"'), text.end());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
          text.pop_back();
        setPath(text);
      }
    }
    BeginDrawing();
    ClearBackground(::Color{30, 30, 38, 255});
    DrawText("Choose your X4 installation", 40, 40, 26, RAYWHITE);
    DrawText("Enter or paste (Ctrl+V) the folder containing X4.exe and the game catalogs.", 40, 88,
             18, LIGHTGRAY);
    if (previousLoadFailed)
      DrawText("The previous location could not be loaded. Check the folder and try again.", 40,
               122, 16, ORANGE);
    const float width = static_cast<float>(std::max(200, GetScreenWidth() - 80));
    GuiTextBox({40, 160, width, 36}, path.data(), static_cast<int>(path.size()), true);
    const bool proceed =
        GuiButton({40, 218, 160, 36}, "Load game assets") != 0 || IsKeyPressed(KEY_ENTER);
    EndDrawing();
    if (proceed && path[0] != '\0') return std::string(path.data());
  }
  return std::nullopt;
}

std::optional<NativeAssets> loadNativeAssets(const std::string& gameDirectory) {
  const auto start = std::chrono::steady_clock::now();
  const auto cacheDirectory = platform::appCacheDir();
  auto loaded = loadInstallMetadata(
      gameDirectory, cacheDirectory.empty() ? cacheDirectory : cacheDirectory / "catalog.json");
  if (!loaded) return std::nullopt;
  if (loaded->cacheWriteFailed) TraceLog(LOG_WARNING, "Native assets: metadata cache write failed");
  NativeAssets result;
  result.archive = std::move(loaded->archive);
  result.catalog = std::move(loaded->metadata.catalog);
  result.materials = std::move(loaded->metadata.materials);
  result.metadataCacheHit = loaded->cacheHit;
  result.metadataSeconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  TraceLog(LOG_INFO, "Native assets: %zu modules, %zu skipped, metadata %.3f seconds",
           result.catalog.size(), loaded->skippedModules, result.metadataSeconds);
  return result;
}

std::optional<NativeAssets> loadHarnessAssets(const std::string& gameDirectory) {
  const std::string directory = gameDirectory.empty()
                                    ? platform::loadGameDirectory(platform::settingsFilePath())
                                    : gameDirectory;
  if (directory.empty()) {
    std::fprintf(stderr,
                 "No X4 install configured: pass --x4 <install> or pick it in the editor\n");
    return std::nullopt;
  }
  auto native = loadNativeAssets(directory);
  if (!native) std::fprintf(stderr, "Could not load X4 game data from %s\n", directory.c_str());
  return native;
}

}  // namespace x4sb::editor
