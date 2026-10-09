#include "config.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cwchar>

#include "log.h"
#include "util.h"

namespace cfg {

Settings S;

namespace {

std::wstring g_path;
bool g_dirty = false;
double g_dirtySince = 0;

bool ReadRaw(const wchar_t* section, const wchar_t* key, wchar_t (&buf)[64]) {
  GetPrivateProfileStringW(section, key, L"", buf, 64, g_path.c_str());
  return buf[0] != 0;
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float def) {
  wchar_t buf[64];
  if (!ReadRaw(section, key, buf)) return def;
  wchar_t* end = nullptr;
  const float v = wcstof(buf, &end);
  return end != buf ? v : def;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int def) {
  wchar_t buf[64];
  if (!ReadRaw(section, key, buf)) return def;
  wchar_t* end = nullptr;
  const long v = wcstol(buf, &end, 0);  // accepts decimal or 0x hex
  return end != buf ? static_cast<int>(v) : def;
}

// Clamps a setting into range; NaN or infinity (e.g. "nan" typed into the ini) falls back to |def|.
float Sanitized(float v, float lo, float hi, float def) { return std::isfinite(v) ? util::Clamp(v, lo, hi) : def; }

void Sanitize(Settings& s) {
  const Settings d;
  s.fov = Sanitized(s.fov, kMinFov, kMaxFov, d.fov);
  if (s.adsFov != 0.f) s.adsFov = Sanitized(s.adsFov, kMinFov, kMaxFov, d.adsFov);
  if (s.viewmodelFov != 0.f) s.viewmodelFov = Sanitized(s.viewmodelFov, kMinFov, kMaxFov, d.viewmodelFov);
  s.transitionSeconds = Sanitized(s.transitionSeconds, 0.f, 2.f, d.transitionSeconds);
  s.menuScale = Sanitized(s.menuScale, kMinMenuScale, kMaxMenuScale, d.menuScale);
}

}  // namespace

Settings Defaults() { return Settings{}; }

void Load(const std::wstring& path) {
  g_path = path;
  S = Defaults();
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    S.enabled = ReadInt(L"General", L"Enabled", S.enabled) != 0;
    S.fov = ReadFloat(L"General", L"GameplayFOV", S.fov);
    S.adsFov = ReadFloat(L"General", L"AimFOV", S.adsFov);
    S.viewmodelFov = ReadFloat(L"General", L"ViewmodelFOV", S.viewmodelFov);
    S.transitionSeconds = ReadFloat(L"General", L"TransitionSeconds", S.transitionSeconds);
    S.keepGameFovEffects = ReadInt(L"General", L"KeepGameFovEffects", S.keepGameFovEffects) != 0;
    S.popup = ReadInt(L"General", L"ShowPopup", S.popup) != 0;
    S.menuScale = ReadFloat(L"General", L"MenuScale", S.menuScale);
    S.keyMenu = ReadInt(L"Keys", L"ToggleMenu", S.keyMenu);
    S.keyFovUp = ReadInt(L"Keys", L"FovUp", S.keyFovUp);
    S.keyFovDown = ReadInt(L"Keys", L"FovDown", S.keyFovDown);
    S.keyToggle = ReadInt(L"Keys", L"ToggleMod", S.keyToggle);
    S.verbose = ReadInt(L"Debug", L"Verbose", S.verbose) != 0;
    Sanitize(S);
  }
  Save();  // creates the file on first run and adds any options introduced by updates
  LOG("Settings: enabled=%d fov=%.1f aim=%.1f viewmodel=%.1f transition=%.2f keepEffects=%d verbose=%d", S.enabled,
      S.fov, S.adsFov, S.viewmodelFov, S.transitionSeconds, S.keepGameFovEffects, S.verbose);
}

void Save() {
  FILE* f = nullptr;
  if (_wfopen_s(&f, g_path.c_str(), L"w") != 0 || !f) {
    LOG("Couldn't write %ls", g_path.c_str());
    return;
  }
  fprintf(f,
          "; TrueFOV settings (Hellraiser: Revival). Change them in game with the menu (F6 by default), or edit this file while\n"
          "; the game is closed.\n\n[General]\n");
  fprintf(f, "Enabled=%d\n", S.enabled ? 1 : 0);
  fprintf(f, "; Gameplay field of view in degrees, on the same scale as the game's own camera FOV.\n");
  fprintf(f, "GameplayFOV=%.1f\n", S.fov);
  fprintf(f, "; FOV while aiming down sights. 0 = the game's own aiming FOV, whatever GameplayFOV is.\n");
  fprintf(f, "AimFOV=%.1f\n", S.adsFov);
  fprintf(f, "; FOV of your first-person arms and held items. 0 = the game's own.\n");
  fprintf(f, "ViewmodelFOV=%.1f\n", S.viewmodelFov);
  fprintf(f, "; Seconds to blend between your FOV and the game's when gameplay starts or stops.\n");
  fprintf(f, "TransitionSeconds=%.2f\n", S.transitionSeconds);
  fprintf(f, "; 1 = keep the game's own FOV changes (sprinting, ladders...), scaled around your FOV.\n");
  fprintf(f, "; 0 = always exactly GameplayFOV (and AimFOV).\n");
  fprintf(f, "KeepGameFovEffects=%d\n", S.keepGameFovEffects ? 1 : 0);
  fprintf(f, "; Show the FOV popup when using the FOV hotkeys.\n");
  fprintf(f, "ShowPopup=%d\n", S.popup ? 1 : 0);
  fprintf(f, "MenuScale=%.2f\n", S.menuScale);
  fprintf(f,
          "\n[Keys]\n; Windows virtual-key codes: "
          "https://learn.microsoft.com/windows/win32/inputdev/virtual-key-codes\n");
  fprintf(f, "ToggleMenu=0x%02X\nFovUp=0x%02X\nFovDown=0x%02X\nToggleMod=0x%02X\n", S.keyMenu, S.keyFovUp,
          S.keyFovDown, S.keyToggle);
  fprintf(f, "\n[Debug]\n; 1 = write extra detail about the game's camera to HellraiserFOV.log.\n");
  fprintf(f, "Verbose=%d\n", S.verbose ? 1 : 0);
  fclose(f);
}

void MarkDirty() {
  g_dirty = true;
  g_dirtySince = util::Now();
}

void SaveIfDirty(double now) {
  if (g_dirty && now - g_dirtySince > 1.0) {
    g_dirty = false;
    Save();
  }
}

void ResetToDefaults() {
  Settings d = Defaults();
  d.keyMenu = S.keyMenu;
  d.keyFovUp = S.keyFovUp;
  d.keyFovDown = S.keyFovDown;
  d.keyToggle = S.keyToggle;
  d.verbose = S.verbose;
  S = d;
  MarkDirty();
}

}  // namespace cfg
