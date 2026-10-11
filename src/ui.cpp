#include "ui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <vector>

#include "canvas.h"
#include "config.h"
#include "fov.h"
#include "game.h"
#include "input.h"
#include "ue.h"
#include "util.h"

namespace ui {
namespace {

using canvas::Painter;
using canvas::Rect;
using canvas::Role;
using ue::FColor;
using ue::FLinearColor;

// ---- palette: rectangles are linear colors, text is 8-bit sRGB ---------------------------

constexpr FLinearColor kPanel{0.008f, 0.007f, 0.007f, 0.95f};
constexpr FLinearColor kBorder{1.f, 1.f, 1.f, 0.14f};
constexpr FLinearColor kTitleBar{0.20f, 0.010f, 0.010f, 1.f};
constexpr FLinearColor kAccent{0.80f, 0.07f, 0.06f, 1.f};
constexpr FLinearColor kSelectedRow{1.f, 1.f, 1.f, 0.09f};
constexpr FLinearColor kPressedRow{1.f, 1.f, 1.f, 0.16f};
constexpr FLinearColor kButtonBorder{1.f, 1.f, 1.f, 0.30f};
constexpr FLinearColor kButtonDisabledBorder{1.f, 1.f, 1.f, 0.10f};
constexpr FLinearColor kButtonHover{1.f, 1.f, 1.f, 0.12f};
constexpr FLinearColor kButtonDown{1.f, 1.f, 1.f, 0.24f};
constexpr FLinearColor kCloseHover{0.80f, 0.07f, 0.06f, 0.95f};
constexpr FLinearColor kCloseDown{0.50f, 0.03f, 0.03f, 1.f};
constexpr FLinearColor kTrack{1.f, 1.f, 1.f, 0.18f};
constexpr FLinearColor kTrackHover{1.f, 1.f, 1.f, 0.26f};
constexpr FLinearColor kThumb{1.f, 1.f, 1.f, 1.f};
constexpr FLinearColor kThumbActive{0.95f, 0.30f, 0.26f, 1.f};
constexpr FLinearColor kThumbGame{0.45f, 0.45f, 0.45f, 1.f};
constexpr FLinearColor kThumbOutline{0.f, 0.f, 0.f, 1.f};
constexpr FLinearColor kTick{1.f, 1.f, 1.f, 0.50f};
constexpr FLinearColor kSwitchOn{0.10f, 0.45f, 0.14f, 1.f};
constexpr FLinearColor kSwitchOffBorder{1.f, 1.f, 1.f, 0.35f};
constexpr FLinearColor kArmed{0.55f, 0.36f, 0.04f, 1.f};
constexpr FLinearColor kRule{1.f, 1.f, 1.f, 0.12f};

constexpr FColor Rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return FColor{b, g, r, a}; }
constexpr FColor kPrimary = Rgb(238, 238, 238);
constexpr FColor kSecondary = Rgb(185, 185, 185);
constexpr FColor kDisabledText = Rgb(110, 110, 110);
constexpr FColor kStatusOn = Rgb(150, 215, 150);
constexpr FColor kWarn = Rgb(240, 190, 90);
constexpr FColor kSwitchOnText = Rgb(18, 18, 18);

// ---- layout, in 1080p design pixels -------------------------------------------------------

constexpr float kPanelW = 660.f;
constexpr float kTitleH = 52.f;
constexpr float kPad = 24.f;
constexpr float kFooterReserve = 135.f;  // footer height including the optional warning line

// ---- menu model -----------------------------------------------------------------------------

enum class Row : uint8_t {
  Fov,
  AdsFov,
  ViewmodelFov,
  Enabled,
  Transition,
  KeepEffects,
  CameraBob,
  Popup,
  MenuScale,
  ResetAll
};

constexpr Row kRows[] = {Row::Fov,         Row::AdsFov,    Row::ViewmodelFov, Row::Enabled,   Row::Transition,
                         Row::KeepEffects, Row::CameraBob, Row::Popup,        Row::MenuScale, Row::ResetAll};
constexpr int kNumRows = static_cast<int>(sizeof kRows / sizeof kRows[0]);

bool IsSliderRow(Row kind) { return kind == Row::Fov || kind == Row::AdsFov || kind == Row::ViewmodelFov; }

float RowDp(Row k) { return IsSliderRow(k) ? 72.f : 38.f; }

// Height of the rows area. |d| rounds design pixels to screen pixels the way the rows are laid
// out (identity for design pixels).
template <class RoundFn>
float RowsArea(RoundFn d) {
  float h = 0.f;
  for (Row k : kRows) h += d(RowDp(k));
  return h;
}

float ReservedDp() { return kTitleH + 8.f + kFooterReserve + RowsArea([](float dp) { return dp; }); }

const wchar_t* Label(Row k) {
  switch (k) {
    case Row::Fov: return L"Gameplay FOV";
    case Row::AdsFov: return L"Aim FOV (aiming down sights)";
    case Row::ViewmodelFov: return L"Viewmodel FOV (arms and held items)";
    case Row::Enabled: return L"FOV mod";
    case Row::Transition: return L"Blend time";
    case Row::KeepEffects: return L"Keep the game's FOV effects";
    case Row::CameraBob: return L"Camera bobbing";
    case Row::Popup: return L"FOV popup on hotkeys";
    case Row::MenuScale: return L"Menu size";
    case Row::ResetAll: return L"Reset all settings";
  }
  return L"";
}

// Largest menu scale whose panel still fits the screen (updated every frame the menu is open).
float g_menuScaleFit = cfg::kMaxMenuScale;

struct NumberSpec {
  float* value;
  float small, big, lo, hi;
  bool integer;
};

NumberSpec Number(Row kind) {
  cfg::Settings& s = cfg::S;
  switch (kind) {
    case Row::Fov: return {&s.fov, 1.f, 5.f, cfg::kMinFov, cfg::kMaxFov, true};
    case Row::AdsFov: return {&s.adsFov, 1.f, 5.f, cfg::kMinFov, cfg::kMaxFov, true};  // 0 = the game's
    case Row::ViewmodelFov: return {&s.viewmodelFov, 1.f, 5.f, cfg::kMinFov, cfg::kMaxFov, true};  // 0 = the game's
    case Row::Transition: return {&s.transitionSeconds, 0.05f, 0.25f, 0.f, 2.f, false};
    case Row::MenuScale: {
      const float fit = std::floor(g_menuScaleFit * 20.f + 1e-3f) / 20.f;  // whole 0.05 steps that fit
      return {&s.menuScale, 0.05f, 0.25f, cfg::kMinMenuScale,
              util::Clamp(fit, cfg::kMinMenuScale, cfg::kMaxMenuScale), false};
    }
    default: return {nullptr, 0.f, 0.f, 0.f, 0.f, false};
  }
}

// What the row shows and steps from: a stored value beyond the range (e.g. a menu size that no
// longer fits) counts as the nearest limit.
float Effective(const NumberSpec& n) { return util::Clamp(*n.value, n.lo, n.hi); }

void SetNumber(Row kind, float v) {
  const NumberSpec n = Number(kind);
  if (!n.value) return;
  v = n.integer ? std::round(v) : std::round(v * 100.f) / 100.f;
  v = util::Clamp(v, n.lo, n.hi);
  if (v == *n.value) return;
  *n.value = v;
  cfg::MarkDirty();
}

// The aim and viewmodel FOVs are 0 while they follow the game's own.
bool FollowsGame(Row kind) {
  return (kind == Row::AdsFov && cfg::S.adsFov == 0.f) || (kind == Row::ViewmodelFov && cfg::S.viewmodelFov == 0.f);
}

// The game's own value for a FOV row: its gameplay FOV, where its aiming FOV settles, or the FOV
// it draws the arms and held items with. 0 until seen.
float GameValue(Row kind) {
  if (kind == Row::AdsFov) return fov::St.gameAimFov;
  if (kind == Row::ViewmodelFov) return fov::St.gameViewmodelFov;
  return fov::St.gameBase > 1.f ? fov::St.gameBase : 0.f;
}

void StepNumber(Row kind, int steps, bool big) {
  const NumberSpec n = Number(kind);
  if (!n.value) return;
  if (FollowsGame(kind)) {
    // First step away from the game's value: from the game's value once seen, else your FOV.
    const float game = GameValue(kind);
    const float from = game > 0.f ? std::round(game) : cfg::S.fov;
    SetNumber(kind, from + steps * (big ? n.big : n.small));
    return;
  }
  // Already at the limit: leave a stored value beyond it alone (e.g. a larger saved menu size
  // that fits again on a bigger screen) instead of pulling it back.
  if ((steps > 0 && *n.value >= n.hi) || (steps < 0 && *n.value <= n.lo)) return;
  SetNumber(kind, Effective(n) + steps * (big ? n.big : n.small));
}

// Menu size actually used for drawing: the setting, capped to what fits (same value the row shows).
float EffectiveMenuScale() { return Effective(Number(Row::MenuScale)); }

bool* Flag(Row k) {
  cfg::Settings& s = cfg::S;
  switch (k) {
    case Row::Enabled: return &s.enabled;
    case Row::KeepEffects: return &s.keepGameFovEffects;
    case Row::CameraBob: return &s.cameraBob;
    case Row::Popup: return &s.popup;
    default: return nullptr;
  }
}

void Toggle(Row k) {
  if (bool* flag = Flag(k)) {
    *flag = !*flag;
    cfg::MarkDirty();
  }
}

void ResetRow(Row k) {
  const cfg::Settings d = cfg::Defaults();
  switch (k) {
    case Row::Fov: SetNumber(k, d.fov); break;
    case Row::AdsFov:
      if (cfg::S.adsFov != d.adsFov) {
        cfg::S.adsFov = d.adsFov;  // back to the game's (outside the slider's range)
        cfg::MarkDirty();
      }
      break;
    case Row::ViewmodelFov:
      if (cfg::S.viewmodelFov != d.viewmodelFov) {
        cfg::S.viewmodelFov = d.viewmodelFov;  // back to the game's (outside the slider's range)
        cfg::MarkDirty();
      }
      break;
    case Row::Transition: SetNumber(k, d.transitionSeconds); break;
    case Row::MenuScale: SetNumber(k, d.menuScale); break;
    case Row::Enabled: cfg::S.enabled = d.enabled; cfg::MarkDirty(); break;
    case Row::KeepEffects: cfg::S.keepGameFovEffects = d.keepGameFovEffects; cfg::MarkDirty(); break;
    case Row::CameraBob: cfg::S.cameraBob = d.cameraBob; cfg::MarkDirty(); break;
    case Row::Popup: cfg::S.popup = d.popup; cfg::MarkDirty(); break;
    case Row::ResetAll: break;
  }
}

// ---- state (all touched only on the game thread) --------------------------------------------

enum Part : uint32_t { kPartRow, kPartMinus, kPartPlus, kPartSlider, kPartButton };
uint32_t IdOf(Row k, Part part) { return (static_cast<uint32_t>(k) + 1) << 16 | part; }
constexpr uint32_t kCloseId = 0xFFFF0001u;
constexpr uint32_t kClearId = 0xFFFF0002u;
const uint32_t kResetButtonId = IdOf(Row::ResetAll, kPartButton);

bool g_open = false;
// Input capture follows frames: the menu owns input while the latest PostRender drew it. The
// wall-clock limit only covers PostRender stopping altogether (loading screens, minimized).
double g_lastPostRender = -1.0;
bool g_drawnLastFrame = false;
constexpr double kCaptureTimeout = 1.5;

Row g_selected = Row::Fov;
uint32_t g_active = 0;     // element held with the left button
bool g_activeSeen = false;
int g_releasedFrames = 0;  // frames the physical button has been up while g_active is set
double g_nextRepeat = 0;
// Layout scale is frozen while an element is held, and after a mouse change to Menu size until
// the pointer leaves that row, so repeated clicks keep landing on the same button.
float g_frozenUnit = 0.f;
float g_frozenForW = 0.f, g_frozenForH = 0.f;
bool g_holdScale = false;
bool g_pointerOnScaleRow = false;
double g_resetArmedUntil = 0;
uint32_t g_resetArmedAtMsg = 0;  // GetMessageTime() of the input that armed Reset
bool g_keyboardMode = false;  // cursor and hover are hidden after keyboard use until the mouse moves
bool g_centerCursor = false;
bool g_hadPointer = false;
float g_lastPx = 0.f, g_lastPy = 0.f;
int g_wheelRemainder = 0;
// Mouse events held back to the next frame: a press after a release in the same frame waits, so
// every release is seen while the element it belongs to is still the active one.
std::vector<input::MouseEvent> g_pendingEvents;

double g_toastUntil = 0;
wchar_t g_toastTitle[64] = L"";
bool g_toastIsFov = false;

void SetOpen(bool open) {
  if (open == g_open) return;
  g_open = open;
  g_active = 0;
  g_releasedFrames = 0;
  g_holdScale = false;
  g_resetArmedUntil = 0;
  g_wheelRemainder = 0;
  g_pendingEvents.clear();
  g_toastUntil = 0;  // no popup carried across the menu opening or closing
  if (open) {
    g_centerCursor = true;
    g_keyboardMode = false;
  }
}

// The menu itself is the feedback while it's open.
void ShowToast(const wchar_t* title, bool isFov) {
  if (g_open) return;
  wcsncpy_s(g_toastTitle, title, _TRUNCATE);
  g_toastIsFov = isFov;
  g_toastUntil = util::Now() + 1.8;
}

void ShowFovToast() {
  wchar_t buf[64];
  swprintf_s(buf, L"FOV  %.0f°", cfg::S.fov);
  ShowToast(buf, true);
}

const wchar_t* KeyName(int vk, wchar_t (&buf)[16]) {
  switch (vk) {
    case VK_PRIOR: return L"PgUp";
    case VK_NEXT: return L"PgDn";
    case VK_END: return L"End";
    case VK_HOME: return L"Home";
    case VK_INSERT: return L"Ins";
    case VK_DELETE: return L"Del";
    case VK_ADD: return L"Num+";
    case VK_SUBTRACT: return L"Num-";
    case VK_MULTIPLY: return L"Num*";
    case VK_DIVIDE: return L"Num/";
    case VK_OEM_PLUS: return L"=";
    case VK_OEM_MINUS: return L"-";
    case 0: return L"(unbound)";
  }
  if (vk >= VK_F1 && vk <= VK_F24) {
    swprintf_s(buf, L"F%d", vk - VK_F1 + 1);
  } else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
    swprintf_s(buf, L"Num%d", vk - VK_NUMPAD0);
  } else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
    swprintf_s(buf, L"%c", static_cast<wchar_t>(vk));
  } else {
    swprintf_s(buf, L"0x%02X", vk);
  }
  return buf;
}

bool IsMenuNavKey(int vk) {
  return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT || vk == VK_RETURN || vk == VK_SPACE ||
         vk == VK_BACK || vk == VK_ESCAPE;
}

bool IsHotkey(int vk) {
  const cfg::Settings& s = cfg::S;
  return vk != 0 && (vk == s.keyMenu || vk == s.keyFovUp || vk == s.keyFovDown || vk == s.keyToggle);
}

void MoveSelection(int dir) {
  int i = 0;
  while (i < kNumRows && kRows[i] != g_selected) ++i;
  if (i >= kNumRows) i = 0;
  g_selected = kRows[(i + dir + kNumRows) % kNumRows];
}

// |msgTime| is the input's GetMessageTime(), so frame hitches don't stretch the gap between clicks.
void Activate(Row k, double now, uint32_t msgTime) {
  switch (k) {
    case Row::ResetAll: {
      // A confirm has to be its own decision, not the second half of a double-click.
      const uint32_t guardMs = (std::max)(400u, static_cast<uint32_t>(GetDoubleClickTime()));
      if (now < g_resetArmedUntil) {
        if (msgTime - g_resetArmedAtMsg < guardMs) break;
        cfg::ResetToDefaults();
        g_resetArmedUntil = 0;
      } else {
        g_resetArmedAtMsg = msgTime;
        g_resetArmedUntil = now + 3.0;
      }
      break;
    }
    case Row::Fov:
    case Row::AdsFov:
    case Row::ViewmodelFov:
    case Row::Transition:
    case Row::MenuScale: break;
    default: Toggle(k); break;
  }
}

// ---- immediate-mode widgets -----------------------------------------------------------------

struct Event {
  input::MouseEvent::Type type;
  float x, y;     // canvas pixels
  int steps;      // wheel notches
  uint32_t time;  // GetMessageTime()
  bool consumed;
};

struct Ctx {
  const Painter& p;
  float u;              // physical pixels per design pixel
  float sx = 1, sy = 1;  // canvas pixels per client pixel
  float px = 0, py = 0;  // pointer, canvas pixels
  bool pointer = false;  // pointer is known and shown
  bool moved = false;    // pointer moved since last frame
  bool shift = false;
  double now = 0;
  std::vector<Event> events;

  float D(float dp) const { return std::round(dp * u); }
};

struct Hit {
  bool hovered = false, pressed = false, held = false, clicked = false, rightClicked = false;
  int wheel = 0;
  float pressX = 0, pressY = 0;
  uint32_t clickTime = 0;
};

// Feeds this frame's mouse events to one element. Declare inner elements (buttons) before the row
// that contains them: a press or wheel notch is consumed by the first element that takes it.
Hit Behave(Ctx& c, uint32_t id, const Rect& r) {
  Hit h;
  for (Event& e : c.events) {
    const bool inside = r.Contains(e.x, e.y);
    switch (e.type) {
      case input::MouseEvent::LeftDown:
        if (inside && !e.consumed) {
          e.consumed = true;
          g_active = id;
          g_nextRepeat = c.now + 0.40;
          if (id != kResetButtonId) g_resetArmedUntil = 0;
          h.pressed = true;
          h.pressX = e.x;
          h.pressY = e.y;
        }
        break;
      case input::MouseEvent::LeftUp:
        if (g_active == id) {
          h.clicked = inside;
          h.clickTime = e.time;
          g_active = 0;
        }
        break;
      case input::MouseEvent::RightDown:
        if (inside && !e.consumed) {
          e.consumed = true;
          h.rightClicked = true;
        }
        break;
      case input::MouseEvent::Wheel:
        if (inside && !e.consumed) {
          e.consumed = true;
          h.wheel += e.steps;
        }
        break;
      default: break;
    }
  }
  if (g_active == id) {
    g_activeSeen = true;
    h.held = true;
  }
  h.hovered = c.pointer && r.Contains(c.px, c.py) && (g_active == 0 || g_active == id);
  return h;
}

// Auto-repeat for a held +/- button: after 0.4 s, every 60 ms while the pointer stays on it.
bool RepeatDue(const Ctx& c, const Hit& h) {
  if (!h.held || !h.hovered || c.now < g_nextRepeat) return false;
  g_nextRepeat = c.now + 0.06;
  return true;
}

void TrackSelection(const Ctx& c, Row k, bool hovered, bool pressed) {
  if (pressed || (hovered && c.moved)) g_selected = k;
}

void RowBackground(const Ctx& c, const Rect& row, bool selected, bool pressed) {
  if (pressed) {
    c.p.Fill(row, kPressedRow);
  } else if (selected) {
    c.p.Fill(row, kSelectedRow);
  }
  if (selected) c.p.Fill({row.x, row.y, c.D(4.f), row.h}, kAccent);
}

void DrawButton(const Ctx& c, const Rect& r, const wchar_t* label, const Hit& h, bool enabled, Role role) {
  const bool down = enabled && h.held && h.hovered;
  if (down) {
    c.p.Fill(r, kButtonDown);
  } else if (enabled && h.hovered) {
    c.p.Fill(r, kButtonHover);
  }
  c.p.Outline(r, enabled ? kButtonBorder : kButtonDisabledBorder);
  Rect text = r;
  if (down) text.y += 1.f;
  c.p.TextCentered(text, label, role, enabled ? kPrimary : kDisabledText);
}

void DrawLabel(const Ctx& c, Row k, const Rect& row, float bandH) {
  c.p.Text(row.x + c.D(kPad), row.y + (bandH - c.p.LineHeight(Role::Body)) * 0.5f, Label(k), Role::Body, kPrimary);
}

// [-] value [+] against the right edge of |row|, vertically centered in the top |bandH| pixels.
struct Stepper {
  Rect minus, value, plus, minusHit, plusHit;
};

Stepper LayoutStepper(const Ctx& c, const Rect& row, float bandH, float valueW) {
  Stepper s;
  const float bw = c.D(30.f), gap = c.D(6.f), y = row.y + std::round((bandH - bw) * 0.5f);
  s.plus = {row.x + row.w - c.D(kPad) - bw, y, bw, bw};
  s.value = {s.plus.x - gap - c.D(valueW), y, c.D(valueW), bw};
  s.minus = {s.value.x - gap - bw, y, bw, bw};
  s.minusHit = {s.minus.x - c.D(3.f), row.y, bw + c.D(6.f), bandH};
  s.plusHit = {s.plus.x - c.D(3.f), row.y, bw + c.D(6.f), bandH};
  return s;
}

// A FOV with -/+ and a slider. The tick marks the game's own value. An aim or viewmodel FOV that
// follows the game's shows "Game" and a grey thumb at the game's value.
void DrawFovRow(Ctx& c, Row k, const Rect& row) {
  const float band = c.D(40.f);
  const Stepper st = LayoutStepper(c, row, band, 96.f);
  const Rect track{row.x + c.D(kPad), row.y + c.D(49.f), row.w - 2.f * c.D(kPad), (std::max)(2.f, c.D(6.f))};
  const Rect sliderHit{row.x + c.D(16.f), row.y + c.D(38.f), row.w - c.D(32.f), c.D(32.f)};

  const Hit hm = Behave(c, IdOf(k, kPartMinus), st.minusHit);
  const Hit hp = Behave(c, IdOf(k, kPartPlus), st.plusHit);
  const Hit hs = Behave(c, IdOf(k, kPartSlider), sliderHit);
  const Hit hr = Behave(c, IdOf(k, kPartRow), row);

  if (hm.pressed || RepeatDue(c, hm)) StepNumber(k, -1, c.shift);
  if (hp.pressed || RepeatDue(c, hp)) StepNumber(k, +1, c.shift);
  if (hs.pressed || (hs.held && c.pointer)) {  // follow the pointer only while its position is known
    const float at = hs.pressed ? hs.pressX : c.px;
    const float t = util::Clamp((at - track.x) / track.w, 0.f, 1.f);
    float v = cfg::kMinFov + t * (cfg::kMaxFov - cfg::kMinFov);
    if (c.shift) v = std::round(v / 5.f) * 5.f;
    SetNumber(k, v);
  }
  const int wheel = hm.wheel + hp.wheel + hs.wheel + hr.wheel;
  if (wheel) StepNumber(k, wheel, c.shift);
  if (hm.rightClicked || hp.rightClicked || hs.rightClicked || hr.rightClicked) ResetRow(k);
  TrackSelection(c, k, hm.hovered || hp.hovered || hs.hovered || hr.hovered,
                 hm.pressed || hp.pressed || hs.pressed || hr.pressed || wheel != 0);

  const bool followsGame = FollowsGame(k);
  const float value = *Number(k).value;
  const float gameValue = GameValue(k);
  RowBackground(c, row, g_selected == k, false);
  DrawLabel(c, k, row, band);
  wchar_t text[24];
  if (!followsGame) {
    swprintf_s(text, L"%.0f°", value);
  } else if (gameValue > 0.f) {
    swprintf_s(text, L"Game %.0f°", gameValue);
  } else {
    swprintf_s(text, L"Game");
  }
  DrawButton(c, st.minus, L"-", hm, followsGame || value > cfg::kMinFov, Role::Body);
  c.p.TextCentered(st.value, text, Role::Body, followsGame ? kSecondary : kPrimary);
  DrawButton(c, st.plus, L"+", hp, followsGame || value < cfg::kMaxFov, Role::Body);

  const bool sliderActive = hs.held || hs.hovered;
  const auto xOf = [&](float v) {
    return track.x + util::Clamp((v - cfg::kMinFov) / (cfg::kMaxFov - cfg::kMinFov), 0.f, 1.f) * track.w;
  };
  c.p.Fill(track, sliderActive ? kTrackHover : kTrack);
  if (gameValue > 0.f) {
    const float tickX = xOf(gameValue);
    c.p.Fill({tickX - c.D(1.f), row.y + c.D(45.f), (std::max)(1.f, c.D(2.f)), c.D(14.f)}, kTick);
  }
  const float shown = followsGame ? gameValue : value;
  if (shown <= 0.f) return;  // following the game's value, which hasn't been seen yet
  const float thumbX = xOf(shown);
  if (!followsGame) c.p.Fill({track.x, track.y, thumbX - track.x, track.h}, kAccent);
  const Rect thumb{thumbX - c.D(6.f), row.y + c.D(40.f), c.D(12.f), c.D(24.f)};
  c.p.Fill(thumb, kThumbOutline);
  c.p.Fill({thumb.x + 1.f, thumb.y + 1.f, thumb.w - 2.f, thumb.h - 2.f},
           sliderActive ? kThumbActive : (followsGame ? kThumbGame : kThumb));
}

void DrawNumberRow(Ctx& c, Row k, const Rect& row) {
  const Stepper st = LayoutStepper(c, row, row.h, 88.f);
  const Hit hm = Behave(c, IdOf(k, kPartMinus), st.minusHit);
  const Hit hp = Behave(c, IdOf(k, kPartPlus), st.plusHit);
  const Hit hr = Behave(c, IdOf(k, kPartRow), row);

  const bool stepped = hm.pressed || hp.pressed;
  if (hm.pressed || RepeatDue(c, hm)) StepNumber(k, -1, c.shift);
  if (hp.pressed || RepeatDue(c, hp)) StepNumber(k, +1, c.shift);
  const int wheel = hm.wheel + hp.wheel + hr.wheel;
  if (wheel) StepNumber(k, wheel, c.shift);
  if (hm.rightClicked || hp.rightClicked || hr.rightClicked) ResetRow(k);
  TrackSelection(c, k, hm.hovered || hp.hovered || hr.hovered, hm.pressed || hp.pressed || hr.pressed || wheel != 0);
  if (k == Row::MenuScale) {
    if (stepped || wheel || hm.rightClicked || hp.rightClicked || hr.rightClicked) g_holdScale = true;
    g_pointerOnScaleRow = hm.hovered || hp.hovered || hr.hovered;
  }

  const NumberSpec n = Number(k);
  const float v = Effective(n);
  RowBackground(c, row, g_selected == k, false);
  DrawLabel(c, k, row, row.h);
  wchar_t value[16];
  swprintf_s(value, k == Row::Transition ? L"%.2f s" : L"%.2fx", v);
  DrawButton(c, st.minus, L"-", hm, v > n.lo + 1e-4f, Role::Body);
  c.p.TextCentered(st.value, value, Role::Body, kPrimary);
  DrawButton(c, st.plus, L"+", hp, v < n.hi - 1e-4f, Role::Body);
}

void DrawSwitchRow(Ctx& c, Row k, const Rect& row) {
  const Hit h = Behave(c, IdOf(k, kPartRow), row);
  if (h.clicked) Toggle(k);
  if (h.rightClicked) ResetRow(k);
  TrackSelection(c, k, h.hovered, h.pressed || h.wheel != 0);

  RowBackground(c, row, g_selected == k, h.held && h.hovered);
  DrawLabel(c, k, row, row.h);
  const Rect box{row.x + row.w - c.D(96.f), row.y + std::round((row.h - c.D(26.f)) * 0.5f), c.D(72.f), c.D(26.f)};
  if (*Flag(k)) {
    c.p.Fill(box, kSwitchOn);
    c.p.TextCentered(box, L"ON", Role::Small, kSwitchOnText);
  } else {
    c.p.Outline(box, kSwitchOffBorder);
    c.p.TextCentered(box, L"OFF", Role::Small, kSecondary);
  }
}

void DrawResetRow(Ctx& c, Row k, const Rect& row) {
  const Rect button{row.x + row.w - c.D(kPad) - c.D(132.f), row.y + std::round((row.h - c.D(28.f)) * 0.5f), c.D(132.f),
                    c.D(28.f)};
  const Hit hb = Behave(c, kResetButtonId, button);
  const Hit hr = Behave(c, IdOf(k, kPartRow), row);
  if (hb.clicked) Activate(k, c.now, hb.clickTime);
  TrackSelection(c, k, hb.hovered || hr.hovered, hb.pressed || hr.pressed);

  RowBackground(c, row, g_selected == k, false);
  DrawLabel(c, k, row, row.h);
  const bool armed = c.now < g_resetArmedUntil;
  if (armed) c.p.Fill(button, kArmed);
  DrawButton(c, button, armed ? L"Confirm" : L"Reset...", hb, true, Role::Small);
}

void StatusLine(wchar_t (&buf)[160], FColor& color) {
  const fov::Status& st = fov::St;
  color = kSecondary;
  if (!st.hookAvailable) {
    swprintf_s(buf, L"Camera hook not found in this game version - see HellraiserFOV.log");
    color = kWarn;
    return;
  }
  if (st.hasPlayer && !st.cameraHooked) {
    swprintf_s(buf, L"Camera hook not active - see HellraiserFOV.log");
    color = kWarn;
    return;
  }
  switch (st.reason) {
    case fov::Reason::NoPlayer: swprintf_s(buf, L"Waiting for gameplay (menu or loading)"); break;
    case fov::Reason::Disabled: swprintf_s(buf, L"Mod is off: the game uses its own FOV (%.1f°)", st.lastIn); break;
    case fov::Reason::Applying:
      swprintf_s(buf, L"%ls: FOV %.1f° (game wants %.1f°)", st.aiming ? L"Aiming" : L"Gameplay", st.lastOut,
                 st.lastIn);
      color = kStatusOn;
      break;
    case fov::Reason::OtherCamera: swprintf_s(buf, L"Cutscene or scripted camera: game FOV untouched"); break;
  }
}

float FooterHeight(const Ctx& c, bool warning) {
  const float ls = c.p.LineHeight(Role::Small);
  float h = c.D(10.f) + 1.f + c.D(9.f) + ls + c.D(6.f) + ls + c.D(4.f) + ls + c.D(14.f);
  if (warning) h += c.D(28.f) + c.D(6.f);
  return h;
}

void DrawFooter(Ctx& c, float x, float y, float w, bool warning) {
  const Painter& p = c.p;
  const float pad = c.D(kPad), ls = p.LineHeight(Role::Small);
  float fy = y + c.D(10.f);
  p.Fill({x + pad, fy, w - 2.f * pad, 1.f}, kRule);
  fy += 1.f + c.D(9.f);

  wchar_t line[160];
  FColor color;
  StatusLine(line, color);
  p.Text(x + pad, fy, line, Role::Small, color);
  fy += ls + c.D(6.f);

  if (warning) {
    const float lineH = c.D(28.f);
    const Rect clear{x + w - pad - c.D(72.f), fy + std::round((lineH - c.D(24.f)) * 0.5f), c.D(72.f), c.D(24.f)};
    const Hit h = Behave(c, kClearId, clear);
    if (h.clicked) fov::RequestClearLockedFov();
    swprintf_s(line, L"Console 'fov %.0f' overrides every camera", fov::St.lockedFov);
    p.Text(x + pad, fy + (lineH - ls) * 0.5f, line, Role::Small, kWarn);
    DrawButton(c, clear, L"Clear", h, true, Role::Small);
    fy += lineH + c.D(6.f);
  }

  const wchar_t* hint = L"Mouse or arrow keys  ·  Right-click or Backspace = default";
  if (p.TextWidth(hint, Role::Small) > w - 2.f * pad) hint = L"Mouse or arrow keys  ·  Right-click = default";
  p.Text(x + pad, fy, hint, Role::Small, kSecondary);
  fy += ls + c.D(4.f);

  wchar_t k1[16], k2[16], k3[16];
  swprintf_s(line, L"Anytime: %ls / %ls = FOV  ·  %ls = mod on/off", KeyName(cfg::S.keyFovUp, k1),
             KeyName(cfg::S.keyFovDown, k2), KeyName(cfg::S.keyToggle, k3));
  p.Text(x + pad, fy, line, Role::Small, kSecondary);
}

void DrawMenu(Ctx& c) {
  const Painter& p = c.p;
  const bool warning = fov::St.lockedFov > 0.f;

  const float cw = p.Width(), ch = p.Height();
  const float w = c.D(kPanelW), titleH = c.D(kTitleH);
  const float rowsH = RowsArea([&c](float dp) { return c.D(dp); });
  const float h = titleH + c.D(8.f) + rowsH + FooterHeight(c, warning);
  // Inside the centred 16:9 area on ultrawide screens; the top edge doesn't move when the
  // optional warning line appears.
  const float safeLeft = (std::max)(0.f, (cw - ch * 16.f / 9.f) * 0.5f);
  const float x = std::round((std::min)(safeLeft + 48.f * c.u, cw - w - 8.f * c.u));
  const float y = std::round((std::max)(0.f, (ch - ReservedDp() * c.u) * 0.5f));

  if (g_centerCursor) {
    g_centerCursor = false;
    // The game hides the cursor during gameplay but it still moves (confined to the window), so
    // put it on the menu. When the game shows its own cursor (pause menu), leave it alone.
    if (!input::OsCursorVisible()) input::MoveCursorToClient((x + w * 0.5f) / c.sx, (y + titleH + c.D(28.f)) / c.sy);
  }

  const Rect panel{x, y, w, h};
  p.Fill(panel, kPanel);
  p.Outline(panel, kBorder);
  p.Fill({x, y, w, titleH}, kTitleBar);
  p.Text(x + c.D(kPad), y + (titleH - p.LineHeight(Role::Title)) * 0.5f, L"HELLRAISER FOV", Role::Title, kPrimary);

  const Rect closeHit{x + w - c.D(52.f), y, c.D(52.f), titleH};
  const Rect closeBox{x + w - c.D(46.f), y + c.D(6.f), c.D(40.f), c.D(40.f)};
  const Hit hc = Behave(c, kCloseId, closeHit);
  if (hc.held && hc.hovered) {
    p.Fill(closeBox, kCloseDown);
  } else if (hc.hovered) {
    p.Fill(closeBox, kCloseHover);
  }
  p.TextCentered(closeBox, L"×", Role::Title, kPrimary);
  wchar_t key[16], hint[48];
  swprintf_s(hint, L"%ls / Esc", KeyName(cfg::S.keyMenu, key));
  p.Text(closeHit.x - c.D(10.f) - p.TextWidth(hint, Role::Small), y + (titleH - p.LineHeight(Role::Small)) * 0.5f, hint,
         Role::Small, kSecondary);

  const float top = y + titleH + c.D(8.f);
  float ry = top;
  for (Row k : kRows) {
    const Rect row{x, ry, w, c.D(RowDp(k))};
    switch (k) {
      case Row::Fov:
      case Row::AdsFov:
      case Row::ViewmodelFov: DrawFovRow(c, k, row); break;
      case Row::Transition:
      case Row::MenuScale: DrawNumberRow(c, k, row); break;
      case Row::ResetAll: DrawResetRow(c, k, row); break;
      default: DrawSwitchRow(c, k, row); break;
    }
    ry += row.h;
  }
  DrawFooter(c, x, top + rowsH, w, warning);

  if (hc.clicked) SetOpen(false);
}

FLinearColor Faded(FLinearColor color, float a) {
  color.A *= a;
  return color;
}

FColor Faded(FColor color, float a) {
  color.A = static_cast<uint8_t>(util::Clamp(a, 0.f, 1.f) * color.A);
  return color;
}

void DrawToast(const Painter& p, float u, double now) {
  const float fade = util::Clamp(static_cast<float>((g_toastUntil - now) / 0.35), 0.f, 1.f);
  const wchar_t* sub;
  if (g_toastIsFov) {
    sub = fov::St.hasPlayer && !fov::St.cameraHooked        ? L"camera hook not active"
          : fov::St.reason == fov::Reason::Applying ? L"gameplay camera only"
          : fov::St.reason == fov::Reason::Disabled ? L"mod is off"
                                                    : L"applies when gameplay resumes";
  } else {
    sub = cfg::S.enabled ? L"gameplay FOV active" : L"game's own FOV everywhere";
  }
  const auto D = [u](float dp) { return std::round(dp * u); };
  const float lt = p.LineHeight(Role::ToastTitle), ls = p.LineHeight(Role::ToastSub);
  const float titleW = p.TextWidth(g_toastTitle, Role::ToastTitle), subW = p.TextWidth(sub, Role::ToastSub);
  const float w = std::round((std::max)((std::max)(titleW, subW) + 2.f * D(24.f), D(240.f)));
  const float h = D(12.f) + lt + D(2.f) + ls + D(14.f);
  const float x = std::round((p.Width() - w) * 0.5f), y = std::round(p.Height() * 0.09f);
  p.Fill({x, y, w, h}, Faded(kPanel, fade));
  p.Fill({x, y, w, (std::max)(1.f, D(3.f))}, Faded(kAccent, fade));
  p.Text(x + (w - titleW) * 0.5f, y + D(12.f), g_toastTitle, Role::ToastTitle, Faded(kPrimary, fade));
  p.Text(x + (w - subW) * 0.5f, y + D(12.f) + lt + D(2.f), sub, Role::ToastSub, Faded(kSecondary, fade));
}

// The largest menu scale at which the whole panel fits on screen.
float FitScale(float cw, float ch) {
  const float base = ch / 1080.f, margin = 24.f * base;
  return (std::min)((ch - 2.f * margin) / ReservedDp(), (cw - 2.f * margin) / (kPanelW + 48.f)) / base;
}

// Physical pixels per design pixel. Call after g_menuScaleFit is updated for this canvas.
float MenuUnit(float ch) { return (std::max)(ch / 1080.f * EffectiveMenuScale(), 0.25f); }

// Ends a hold whose release never arrived (button let go outside the window, on a secure desktop,
// ...). The physical button has to read "up" for two frames, so a release still in the message
// queue is always delivered normally first.
void ReleaseWatchdog(const std::vector<input::MouseEvent>& events) {
  if (!g_active) {
    g_releasedFrames = 0;
    return;
  }
  for (const input::MouseEvent& e : events) {
    if (e.type == input::MouseEvent::LeftUp) {
      g_releasedFrames = 0;
      return;
    }
  }
  const int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;  // physical primary button
  if (GetAsyncKeyState(vk) & 0x8000) {
    g_releasedFrames = 0;
  } else if (++g_releasedFrames >= 2) {
    g_active = 0;  // a cancel, not a click
    g_releasedFrames = 0;
  }
}

// This frame's mouse events, in order: a press that follows a release waits for the next frame, and
// a focus loss (Cancel) ends any hold and drops the presses queued before it.
std::vector<input::MouseEvent> CollectEvents() {
  std::vector<input::MouseEvent> events = std::move(g_pendingEvents);
  g_pendingEvents.clear();
  const std::vector<input::MouseEvent> fresh = input::TakeEvents();
  events.insert(events.end(), fresh.begin(), fresh.end());
  if (!g_open) return {};
  bool sawUp = false;
  for (size_t i = 0; i < events.size(); ++i) {
    if (events[i].type == input::MouseEvent::LeftUp) {
      sawUp = true;
    } else if (sawUp && events[i].type == input::MouseEvent::LeftDown) {
      g_pendingEvents.assign(events.begin() + static_cast<ptrdiff_t>(i), events.end());
      events.resize(i);
      break;
    }
  }
  for (size_t i = events.size(); i-- > 0;) {
    if (events[i].type == input::MouseEvent::Cancel) {
      g_active = 0;
      events.erase(events.begin(), events.begin() + static_cast<ptrdiff_t>(i) + 1);
      break;
    }
  }
  return events;
}

}  // namespace

bool CapturesInput() {
  return g_open && g_drawnLastFrame && util::Now() - g_lastPostRender < kCaptureTimeout;
}

bool WantsKey(int vk, bool alt) { return IsHotkey(vk) || (!alt && CapturesInput() && IsMenuNavKey(vk)); }

void OnKeyDown(int vk, bool repeat) {
  cfg::Settings& s = cfg::S;
  const bool shift = GetKeyState(VK_SHIFT) < 0;
  const double now = util::Now();
  if (vk == s.keyMenu) {
    if (!repeat) SetOpen(!g_open);
    return;
  }
  if (vk == s.keyFovUp || vk == s.keyFovDown) {
    StepNumber(Row::Fov, vk == s.keyFovUp ? 1 : -1, shift);
    if (s.popup && !g_open) ShowFovToast();
    return;
  }
  if (vk == s.keyToggle) {
    if (!repeat) {
      s.enabled = !s.enabled;
      cfg::MarkDirty();
      ShowToast(s.enabled ? L"FOV mod ON" : L"FOV mod OFF", false);
    }
    return;
  }
  if (!g_open) return;
  if (g_active) {  // the mouse is holding something: only Esc (cancel) applies
    if (vk == VK_ESCAPE && !repeat) g_active = 0;
    return;
  }

  g_keyboardMode = true;
  const Row k = g_selected;
  if (vk != VK_RETURN && vk != VK_SPACE && vk != VK_ESCAPE) g_resetArmedUntil = 0;
  switch (vk) {
    case VK_UP: MoveSelection(-1); break;
    case VK_DOWN: MoveSelection(+1); break;
    case VK_LEFT:
    case VK_RIGHT:
      if (Number(k).value) {
        StepNumber(k, vk == VK_RIGHT ? 1 : -1, shift);
      } else if (Flag(k) && !repeat) {  // one flip per press, not one per auto-repeat
        Toggle(k);
      }
      break;
    case VK_RETURN:
    case VK_SPACE:
      if (!repeat) Activate(k, now, static_cast<uint32_t>(GetMessageTime()));
      break;
    case VK_BACK:
      if (!repeat) ResetRow(k);
      break;
    case VK_ESCAPE:
      if (repeat) break;
      if (now < g_resetArmedUntil) {
        g_resetArmedUntil = 0;
      } else {
        SetOpen(false);
      }
      break;
  }
}

void OnFrame(void* /*viewportClient*/, void* canvasObject, double now) {
  g_lastPostRender = now;
  g_drawnLastFrame = false;  // every early return below means "the menu wasn't drawn this frame"
  input::Maintain(now);
  const std::vector<input::MouseEvent> raw = CollectEvents();
  ReleaseWatchdog(raw);
  const bool toast = !g_open && now < g_toastUntil;
  if (!g_open && !toast) return;

  // If the menu can't be drawn this frame, still honour releases so nothing stays held.
  const auto dropReleased = [&raw] {
    for (const input::MouseEvent& e : raw) {
      if (e.type == input::MouseEvent::LeftUp) g_active = 0;
    }
  };
  const float cw = static_cast<float>(ue::At<int32_t>(canvasObject, game::O.Canvas_SizeX));
  const float ch = static_cast<float>(ue::At<int32_t>(canvasObject, game::O.Canvas_SizeY));
  if (cw < 64.f || ch < 64.f) {
    dropReleased();
    return;
  }
  g_menuScaleFit = FitScale(cw, ch);
  float unit = MenuUnit(ch);  // the popup uses the same size as the menu
  if (g_open) {
    const bool sameCanvas = cw == g_frozenForW && ch == g_frozenForH;
    if ((g_active || g_holdScale) && g_frozenUnit > 0.f && sameCanvas) {
      unit = g_frozenUnit;
    } else {
      g_frozenUnit = unit;
      g_frozenForW = cw;
      g_frozenForH = ch;
      g_holdScale = false;
    }
  }

  Painter p;
  if (!p.Begin(canvasObject, unit)) {
    dropReleased();
    return;
  }
  if (toast) {
    DrawToast(p, unit, now);
    return;
  }

  Ctx c{p, unit};
  c.now = now;
  c.shift = GetKeyState(VK_SHIFT) < 0;
  float cx = 0, cy = 0, clientW = 0, clientH = 0;
  const bool havePointer = input::CursorClientPos(cx, cy, clientW, clientH);
  if (havePointer) {
    c.sx = cw / clientW;
    c.sy = ch / clientH;
    c.px = util::Clamp(cx * c.sx, 0.f, cw - 1.f);
    c.py = util::Clamp(cy * c.sy, 0.f, ch - 1.f);
  }
  const bool moved =
      havePointer && g_hadPointer && (std::fabs(c.px - g_lastPx) >= 1.f || std::fabs(c.py - g_lastPy) >= 1.f);
  g_hadPointer = havePointer;
  g_lastPx = c.px;
  g_lastPy = c.py;

  if (moved) g_keyboardMode = false;
  for (const input::MouseEvent& e : raw) {
    Event ev{e.type, e.x * c.sx, e.y * c.sy, 0, e.time, false};
    if (e.type == input::MouseEvent::Wheel) {
      g_wheelRemainder += e.wheel;
      ev.steps = g_wheelRemainder / WHEEL_DELTA;
      g_wheelRemainder -= ev.steps * WHEEL_DELTA;
      if (!ev.steps) continue;
      // With the cursor hidden after keyboard use, the wheel acts on the highlighted row.
      if (g_keyboardMode) {
        if (!g_active && Number(g_selected).value) StepNumber(g_selected, ev.steps, c.shift);
        continue;
      }
    }
    if (e.type == input::MouseEvent::LeftDown || e.type == input::MouseEvent::RightDown) g_keyboardMode = false;
    c.events.push_back(ev);
  }
  c.pointer = havePointer && !g_keyboardMode;
  c.moved = moved && c.pointer;

  g_activeSeen = false;
  g_pointerOnScaleRow = false;
  DrawMenu(c);
  if (g_active && !g_activeSeen) g_active = 0;  // the held element went away
  if (g_holdScale && !g_active && (!c.pointer || !g_pointerOnScaleRow)) g_holdScale = false;
  g_drawnLastFrame = g_open;
  if (g_open && c.pointer && !input::OsCursorVisible()) p.Cursor(c.px, c.py, g_active != 0);
}

}  // namespace ui
