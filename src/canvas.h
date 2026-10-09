#pragma once

// Drawing on the engine's UCanvas: rectangles, crisp text at any size, and a mouse cursor, through
// the canvas's own Blueprint drawing functions (K2_DrawTexture, K2_DrawText, K2_TextSize).
//
// Scaling text would only stretch the glyph bitmaps. Runtime (composite) fonts are rasterized at
// UFont::LegacyFontSize, read on every draw and measure, so each text call briefly sets that field
// to the size it needs and restores it right after. Glyphs are generated synchronously on the game
// thread, so nothing else observes the temporary size.

#include "ue.h"

namespace canvas {

// Text roles; line heights are in 1080p design pixels, scaled by the frame's unit.
enum class Role { Title, Body, Small, ToastTitle, ToastSub, Count };

struct Rect {
  float x = 0, y = 0, w = 0, h = 0;
  bool Contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Painter {
 public:
  Painter() = default;
  ~Painter() { End(); }
  Painter(const Painter&) = delete;
  Painter& operator=(const Painter&) = delete;

  // Prepares drawing for this frame at |unit| physical pixels per design pixel. Returns false if
  // the canvas or font isn't usable.
  bool Begin(void* canvas, float unit);
  void End();

  float Width() const { return width_; }
  float Height() const { return height_; }
  float LineHeight(Role role) const { return lineH_[static_cast<int>(role)]; }

  void Fill(const Rect& r, ue::FLinearColor color) const;
  void Outline(const Rect& r, ue::FLinearColor color) const;  // 1 px
  void Text(float x, float y, const wchar_t* s, Role role, ue::FColor color) const;
  void TextCentered(const Rect& r, const wchar_t* s, Role role, ue::FColor color) const;
  float TextWidth(const wchar_t* s, Role role) const;
  // Arrow cursor with its hotspot at (x, y).
  void Cursor(float x, float y, bool pressed) const;

 private:
  float TextCoord(float physical) const;

  void* canvas_ = nullptr;
  void* font_ = nullptr;
  void* white_ = nullptr;
  bool runtimeFont_ = false;
  bool begun_ = false;
  float dpi_ = 1.f;      // FCanvas DPI scale: canvas units -> physical pixels
  float sizeDpi_ = 1.f;  // converts text measurements to physical pixels (dpi_ for runtime fonts)
  float width_ = 0.f, height_ = 0.f;
  int32_t size_[static_cast<int>(Role::Count)] = {};
  float lineH_[static_cast<int>(Role::Count)] = {};
};

// Undoes a temporary font size after a structured exception mid-draw.
void EmergencyRestore();

}  // namespace canvas
