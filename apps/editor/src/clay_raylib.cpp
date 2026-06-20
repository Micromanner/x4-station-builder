#include "clay_raylib.hpp"

#include "raylib.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace x4sb::editor {
namespace {

const ::Font& fontFor(const UiFonts& fonts, std::uint16_t id) {
  return id == static_cast<std::uint16_t>(FontId::Value) ? fonts.value : fonts.display;
}

// Clay measures text through this; userData is the UiFonts* registered in clayInit.
Clay_Dimensions measureText(Clay_StringSlice text, Clay_TextElementConfig* config, void* userData) {
  const auto* fonts = static_cast<const UiFonts*>(userData);
  const ::Font& font = fontFor(*fonts, config->fontId);
  const std::string s(text.chars, static_cast<std::size_t>(text.length));
  const ::Vector2 m = MeasureTextEx(font, s.c_str(), static_cast<float>(config->fontSize),
                                    static_cast<float>(config->letterSpacing));
  return Clay_Dimensions{m.x, m.y};
}

void handleClayError(Clay_ErrorData error) {
  std::fprintf(stderr, "clay: %.*s\n", static_cast<int>(error.errorText.length),
               error.errorText.chars);
}

::Color toRlColor(Clay_Color c) {
  return ::Color{static_cast<unsigned char>(c.r), static_cast<unsigned char>(c.g),
                 static_cast<unsigned char>(c.b), static_cast<unsigned char>(c.a)};
}

// Gunmetal Cherenkov style fills (spec §3 / plan Global Constraints).
constexpr ::Color kGlossTop{42, 51, 61, 255};
constexpr ::Color kGlossBot{26, 32, 38, 255};
constexpr ::Color kCyanEdge{88, 207, 224, 255};
constexpr ::Color kLightEdge{255, 255, 255, 26};
constexpr ::Color kRecessed{10, 11, 14, 153};
constexpr ::Color kShadow{0, 0, 0, 120};

void drawGlossBar(::Rectangle r) {
  DrawRectangleGradientV(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                         static_cast<int>(r.height), kGlossTop, kGlossBot);
  DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width), 1,
                kCyanEdge);  // the signature bright cyan top-edge line
}

}  // namespace

std::vector<std::uint8_t> clayInit(int width, int height, const UiFonts& fonts) {
  const std::uint32_t size = Clay_MinMemorySize();
  std::vector<std::uint8_t> memory(size);
  Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(size, memory.data());
  Clay_Initialize(arena, Clay_Dimensions{static_cast<float>(width), static_cast<float>(height)},
                  Clay_ErrorHandler{handleClayError, nullptr});
  // userData must outlive Clay; the caller keeps UiFonts alive for the app's life.
  Clay_SetMeasureTextFunction(measureText, const_cast<UiFonts*>(&fonts));
  return memory;
}

void renderClayCommands(const Clay_RenderCommandArray& commands, const UiFonts& fonts) {
  for (std::int32_t i = 0; i < commands.length; ++i) {
    const Clay_RenderCommand* cmd = Clay_RenderCommandArray_Get(
        const_cast<Clay_RenderCommandArray*>(&commands), i);  // ⚠ VERIFY-CLAY accessor
    const Clay_BoundingBox b = cmd->boundingBox;
    const ::Rectangle r{b.x, b.y, b.width, b.height};
    switch (cmd->commandType) {
      case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
        const auto tag = static_cast<StyleTag>(reinterpret_cast<std::uintptr_t>(cmd->userData));
        if (tag == StyleTag::GlossBar) {
          DrawRectangle(static_cast<int>(r.x + 3), static_cast<int>(r.y + 3),
                        static_cast<int>(r.width), static_cast<int>(r.height),
                        kShadow);  // drop shadow
          drawGlossBar(r);
        } else if (tag == StyleTag::Recessed) {
          DrawRectangleRec(r, kRecessed);
          DrawRectangleLinesEx(r, 1.0f, kLightEdge);
        } else {
          DrawRectangleRec(r, toRlColor(cmd->renderData.rectangle.backgroundColor));
        }
        break;
      }
      case CLAY_RENDER_COMMAND_TYPE_BORDER: {
        DrawRectangleLinesEx(r, static_cast<float>(cmd->renderData.border.width.top),
                             toRlColor(cmd->renderData.border.color));
        break;
      }
      case CLAY_RENDER_COMMAND_TYPE_TEXT: {
        const Clay_TextRenderData& t = cmd->renderData.text;
        const std::string s(t.stringContents.chars,
                            static_cast<std::size_t>(t.stringContents.length));
        DrawTextEx(fontFor(fonts, t.fontId), s.c_str(), ::Vector2{r.x, r.y},
                   static_cast<float>(t.fontSize), static_cast<float>(t.letterSpacing),
                   toRlColor(t.textColor));
        break;
      }
      case CLAY_RENDER_COMMAND_TYPE_SCISSOR_START:
        BeginScissorMode(static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.width),
                         static_cast<int>(r.height));
        break;
      case CLAY_RENDER_COMMAND_TYPE_SCISSOR_END:
        EndScissorMode();
        break;
      default:
        break;  // CUSTOM / IMAGE not used by the top bar (Approach A: add when a slice needs them)
    }
  }
}

}  // namespace x4sb::editor
