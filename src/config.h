#pragma once

#include <string>

namespace cfg {

constexpr float kMinFov = 30.f;
constexpr float kMaxFov = 150.f;
constexpr float kMinMenuScale = 0.75f;
constexpr float kMaxMenuScale = 1.5f;  // the UI further caps it to what fits on screen

struct Settings {
  bool enabled = true;
  float fov = 100.f;          // gameplay FOV
  float adsFov = 0.f;         // FOV while aiming down sights; 0 = the game's own
  float viewmodelFov = 0.f;   // FOV of the first-person arms and held items; 0 = the game's own
  float transitionSeconds = 0.25f;
  bool keepGameFovEffects = true;  // sprinting, climbing ladders... scaled around your FOV
  bool popup = true;
  float menuScale = 1.f;
  int keyMenu = 0x75;     // F6
  int keyFovUp = 0x21;    // Page Up
  int keyFovDown = 0x22;  // Page Down
  int keyToggle = 0x23;   // End
  bool verbose = false;   // [Debug] Verbose: reflection dumps and a longer camera trace in the log
};

extern Settings S;

Settings Defaults();
void Load(const std::wstring& path);
void Save();
void MarkDirty();
void SaveIfDirty(double now);
void ResetToDefaults();  // keeps key bindings and debug options

}  // namespace cfg
