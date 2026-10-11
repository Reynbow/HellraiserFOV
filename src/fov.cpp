#include "fov.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "bob.h"
#include "config.h"
#include "game.h"
#include "log.h"
#include "ue.h"
#include "util.h"

namespace fov {

Status St;

namespace {

using game::C;
using game::F;
using game::O;
using game::S;
using ApplyCameraModifiersFn = void(__fastcall*)(void* self, float deltaTime, void* pov);

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;
constexpr float kMinOut = 5.f;
constexpr float kMaxOut = 170.f;
// If the player camera hasn't been rendered for this long, it was off screen (cutscene camera,
// loading...) and there's nothing on screen to blend from when it comes back.
constexpr double kOffScreenSeconds = 0.2;
// Camera trace in the log: every frame (at most 60 a second) for this long after each change.
constexpr double kTraceSeconds = 2.5;
constexpr int kMaxTraceLines = 3000;
// Aiming eases in and out like FInterpTo at this speed, close to the game's own aiming zoom.
constexpr float kZoomSpeed = 12.f;

// Camera managers get their vtable slot hooked; each vtable keeps the function it had.
struct Hooked {
  void** vtable;
  ApplyCameraModifiersFn original;
};
Hooked g_hooked[8];
int g_numHooked = 0;

void** g_checkedVtable = nullptr;
double g_lastFrame = 0;
bool g_clearLockedRequested = false;
long g_faults = 0;
uint32_t g_frame = 0;     // PostRender frames so far
uint32_t g_povFrame = 0;  // g_frame when the player camera was last updated
void* g_describedPawn = nullptr;

double TanHalf(double deg) { return std::tan(deg * 0.5 * kDegToRad); }
double FromTanHalf(double t) { return 2.0 * std::atan(t) / kDegToRad; }
double Ease(double a) { return a * a * (3.0 - 2.0 * a); }  // smoothstep
bool Plausible(float fov) { return fov > 1.f && fov < 179.f; }

// The player's own view: the character, or (as the game does it) its first-person camera actor.
bool IsPlayerView(void* target, void* character) {
  return target && character && (target == character || target == game::PlayerCameraActor(character));
}

// What the camera manager is looking through, by class name, for the log (cached per target).
const std::string& TargetName(void* target, void* character) {
  static void* last = reinterpret_cast<void*>(1);
  static std::string name;
  if (target != last) {
    last = target;
    name = !target ? "nothing"
           : target == character ? "the player character"
           : target == game::PlayerCameraActor(character) ? "the player camera (" + ue::ClassName(target) + ")"
                                                         : ue::ClassName(target);
  }
  return name;
}

const char* ReasonName(Reason reason) {
  switch (reason) {
    case Reason::NoPlayer: return "no player";
    case Reason::Disabled: return "mod off";
    case Reason::Applying: return "applying";
    case Reason::OtherCamera: return "another camera is on screen";
  }
  return "?";
}

// One log line whenever the camera situation changes, so "why isn't my FOV applied here?" can be
// answered from HellraiserFOV.log. Returns true if it logged.
bool LogSituation(const std::string& target) {
  static Reason lastReason = Reason::NoPlayer;
  static const char* lastShown = nullptr;
  static std::string lastTarget;
  static bool lastAiming = false, lastHooked = false, logged = false;
  if (logged && lastReason == St.reason && lastShown == St.shown && lastAiming == St.aiming &&
      lastHooked == St.cameraHooked && lastTarget == target) {
    return false;
  }
  logged = true;
  lastReason = St.reason;
  lastShown = St.shown;
  lastAiming = St.aiming;
  lastHooked = St.cameraHooked;
  lastTarget = target;
  LOG("Situation: %s | camera shows: %s (%s) | aiming: %s | camera hook: %s | game base %.1f | FOV %.1f -> %.1f",
      ReasonName(St.reason), St.shown, target.c_str(), St.aiming ? "yes" : "no", St.cameraHooked ? "on" : "off",
      St.gameBase, St.lastIn, St.lastOut);
  return true;
}

// Frame-by-frame camera values for a few seconds after each situation change, so a transition
// (aiming, a cutscene ending...) can be replayed from the log. |screenFov| is the FOV actually
// rendered; the player camera values are stale while another camera is on screen.
void Trace(double now, bool changed, bool povFresh, float screenFov) {
  static double hotUntil = -1.0, start = 0.0, last = -1.0;
  static int lines = 0;
  if (changed) {
    if (now > hotUntil) start = now;
    hotUntil = now + kTraceSeconds * (cfg::S.verbose ? 2.0 : 1.0);
  }
  if (now > hotUntil || lines >= kMaxTraceLines || (!changed && now - last < 1.0 / 60.0)) return;
  last = now;
  if (++lines == kMaxTraceLines) {
    LOG("Camera trace: line limit reached, no more this session");
    return;
  }
  LOG("  %+.3fs %-27s alpha %.2f zoom %.2f | on screen %5.1f | player camera: game %5.1f base %5.1f -> %5.1f%s | "
      "viewmodel %5.1f -> %5.1f",
      now - start, ReasonName(St.reason), St.alpha, St.zoomAlpha, screenFov, St.lastIn, St.lastBase, St.lastOut,
      povFresh ? "" : " (not on screen)", St.lastViewmodelOut > 0.f ? St.gameViewmodelFov : 0.f, St.lastViewmodelOut);
}

// The FOV the hotkeys and menu set, eased in OnFrame.
float UserFov() { return St.smoothedFov > 0.f ? St.smoothedFov : cfg::S.fov; }

// Where the game's aiming FOV settles, to show in the menu: steady for a few frames while aiming.
void LearnAimFov(float in) {
  static float prev = 0.f;
  static int steady = 0;
  if (St.aiming && St.zoomAlpha > 0.99f && std::fabs(in - prev) < 0.02f) {
    if (++steady >= 3) St.gameAimFov = in;
  } else {
    steady = 0;
  }
  prev = in;
}

// While the game draws the first-person meshes (arms, held items) with a FOV of their own, they
// follow the viewmodel FOV instead, blended in and out like the main FOV. Changes the game makes
// to its own value (such as aiming) are kept, scaled around its usual value.
void ApplyViewmodel(void* pov, double a) {
  St.lastViewmodelOut = 0.f;
  if (O.POV_FirstPersonFOV < 0 || !O.POV_UseFirstPerson.Valid() || !ue::ReadBool(pov, O.POV_UseFirstPerson)) return;
  float& fp = ue::At<float>(pov, O.POV_FirstPersonFOV);
  const float game = fp;
  if (!Plausible(game)) return;
  St.gameViewmodelFov = game;
  St.lastViewmodelOut = game;
  static float usual = 0.f;  // the game's value when not aiming
  if (usual <= 0.f || (!St.aiming && St.zoomAlpha <= 0.f)) usual = game;
  if (cfg::S.viewmodelFov <= 0.f || a <= 0.0) return;
  const double tGame = TanHalf(game);
  const double tTarget = TanHalf(cfg::S.viewmodelFov) * tGame / TanHalf(usual);
  const float out = util::Clamp(static_cast<float>(FromTanHalf(tGame + (tTarget - tGame) * a)), kMinOut, kMaxOut);
  fp = out;
  St.lastViewmodelOut = out;
}

// The player character if |pov| is the view of the player's own camera, else nullptr; |pc| gets
// the player controller. Only the current and pending view targets reach the screen.
void* PlayerViewOf(void* cameraManager, void* pov, void*& pc) {
  char* vt = static_cast<char*>(pov) - O.VT_POV;
  char* mgr = static_cast<char*>(cameraManager);
  if (vt != mgr + O.PCM_ViewTarget && vt != mgr + O.PCM_PendingViewTarget) return nullptr;
  pc = ue::Ptr(cameraManager, O.PCM_PCOwner);
  void* pawn = pc ? ue::Ptr(pc, O.Ctrl_Pawn) : nullptr;
  if (!pawn || !ue::IsA(pawn, C.PlayerCharacter) || !IsPlayerView(ue::Ptr(vt, O.VT_Target), pawn)) return nullptr;
  return pawn;
}

void ApplyToPov(void* cameraManager, void* pov) {
  void* pc = nullptr;
  if (!PlayerViewOf(cameraManager, pov, pc)) return;

  float& fov = ue::At<float>(pov, O.POV_FOV);
  const float in = fov;
  St.lastIn = in;
  St.lastOut = in;
  St.lastBase = in;
  g_povFrame = g_frame;
  LearnAimFov(in);

  // Coming back on screen (e.g. a cutscene cut back to the player): start at the right FOV
  // instead of blending from whatever the blend weight decayed to while off screen.
  const double now = util::Now();
  if (now - St.lastPovTime > kOffScreenSeconds) {
    St.alpha = cfg::S.enabled ? 1.f : 0.f;
    St.zoomAlpha = St.aiming ? 1.f : 0.f;
  }
  St.lastPovTime = now;

  const double a = Ease(St.alpha);
  if (a <= 0.0 || !Plausible(in)) {
    ApplyViewmodel(pov, 0.0);
    return;
  }

  // Scaling around the game's gameplay FOV keeps any FOV change the game makes on top of it
  // (sprinting, ladders...).
  const double base = cfg::S.keepGameFovEffects && Plausible(St.gameBase) ? St.gameBase : in;
  St.lastBase = static_cast<float>(base);

  // Work in tan(fov/2) space so the scaling is proportional on screen.
  const double tIn = TanHalf(in);
  const double tUser = TanHalf(UserFov());
  double tTarget = cfg::S.keepGameFovEffects ? tIn * tUser / TanHalf(base) : tUser;
  // Aiming: towards the chosen aim FOV, or by default the game's own aiming FOV (not scaled by
  // your gameplay FOV, so aiming looks the way the game made it).
  if (St.zoomAlpha > 0.f) {
    const double tZoom = cfg::S.adsFov > 0.f ? TanHalf(cfg::S.adsFov) : tIn;
    tTarget += (tZoom - tTarget) * St.zoomAlpha;
  }
  const double tOut = tIn + (tTarget - tIn) * a;
  const float out = util::Clamp(static_cast<float>(FromTanHalf(tOut)), kMinOut, kMaxOut);
  fov = out;
  St.lastOut = out;

  if (O.POV_DesiredFOV >= 0) {
    float& desired = ue::At<float>(pov, O.POV_DesiredFOV);
    if (Plausible(desired)) {
      desired = util::Clamp(static_cast<float>(FromTanHalf(TanHalf(desired) * tOut / tIn)), kMinOut, kMaxOut);
    }
  }
  ApplyViewmodel(pov, a);
}

void SafeApplyToPov(void* cameraManager, void* pov) {
  __try {
    ApplyToPov(cameraManager, pov);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_faults);
  }
}

void SafeSteady(void* cameraManager, float deltaTime, void* pov) {
  __try {
    void* pc = nullptr;
    if (void* character = PlayerViewOf(cameraManager, pov, pc)) bob::BeforeModifiers(character, pc, pov, deltaTime);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    InterlockedIncrement(&g_faults);
  }
}

ApplyCameraModifiersFn OriginalFor(void* self) {
  void** vtable = *static_cast<void***>(self);
  for (int i = 0; i < g_numHooked; ++i) {
    if (g_hooked[i].vtable == vtable) return g_hooked[i].original;
  }
  return nullptr;
}

// The camera bob is taken out of the camera's own view, before the game adds its camera shakes and
// camera animations; the FOV is set on the finished view.
void __fastcall HookApplyCameraModifiers(void* self, float deltaTime, void* pov) {
  SafeSteady(self, deltaTime, pov);
  if (ApplyCameraModifiersFn original = OriginalFor(self)) original(self, deltaTime, pov);
  SafeApplyToPov(self, pov);
}

void EnsureCameraHook(void* cameraManager) {
  void** vtable = *static_cast<void***>(cameraManager);
  if (vtable == g_checkedVtable) return;
  g_checkedVtable = vtable;
  St.cameraHooked = false;
  if (S.ApplyCameraModifiers < 0) return;
  if (OriginalFor(cameraManager)) {
    St.cameraHooked = true;
    return;
  }
  void* hook = reinterpret_cast<void*>(&HookApplyCameraModifiers);
  void* current = vtable[S.ApplyCameraModifiers];
  if (g_numHooked == static_cast<int>(sizeof g_hooked / sizeof g_hooked[0]) || current == hook) return;
  // Recorded before the slot changes: the hook looks its original up by vtable.
  g_hooked[g_numHooked++] = {vtable, reinterpret_cast<ApplyCameraModifiersFn>(current)};
  void* original = nullptr;
  if (!ue::HookSlot(cameraManager, S.ApplyCameraModifiers, hook, &original) || original != current) {
    --g_numHooked;
    LOG("Couldn't hook ApplyCameraModifiers in %s", ue::ClassName(cameraManager).c_str());
    return;
  }
  St.cameraHooked = true;
  LOG("Hooked ApplyCameraModifiers of %s (vtable exe+0x%llx, slot %d)", ue::ClassName(cameraManager).c_str(),
      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vtable) -
                                      reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
      S.ApplyCameraModifiers);
}

// The game's own FOV setting, for the log: UInfernoUserSettings::FOV.
void DescribeSettingsFov(char (&buf)[64]) {
  snprintf(buf, sizeof buf, "?");
  void* engine = game::Engine();
  void* settings = engine && O.Eng_GameUserSettings >= 0 ? ue::Ptr(engine, O.Eng_GameUserSettings) : nullptr;
  if (!settings || O.Settings_FOV < 0 || !ue::IsA(settings, C.InfernoUserSettings)) return;
  snprintf(buf, sizeof buf, "%d / %.2f (int / float)", ue::At<int32_t>(settings, O.Settings_FOV),
           ue::At<float>(settings, O.Settings_FOV));
}

// A camera component's FOV settings, for the log.
std::string DescribeCamera(void* camera) {
  if (!camera) return "none";
  char buf[256];
  snprintf(buf, sizeof buf, "%s: FieldOfView %.2f, first-person FOV %s %.2f, aspect axis %s %d, constrain aspect %s",
           ue::ClassName(camera).c_str(), O.Cam_FieldOfView >= 0 ? ue::At<float>(camera, O.Cam_FieldOfView) : 0.f,
           ue::ReadBool(camera, O.Cam_EnableFirstPersonFOV) ? "on" : "off",
           O.Cam_FirstPersonFOV >= 0 ? ue::At<float>(camera, O.Cam_FirstPersonFOV) : 0.f,
           ue::ReadBool(camera, O.Cam_OverrideAspect) ? "overridden" : "default",
           O.Cam_AspectConstraint >= 0 ? ue::At<uint8_t>(camera, O.Cam_AspectConstraint) : -1,
           ue::ReadBool(camera, O.Cam_ConstrainAspect) ? "yes" : "no");
  return buf;
}

// Once per player character: what the game's camera looks like, for the log.
void DescribePlayer(void* character, void* cameraManager, void* localPlayer) {
  if (character == g_describedPawn) return;
  g_describedPawn = character;
  void* cameraActor = game::PlayerCameraActor(character);
  void* camera = cameraActor && O.CamActor_Camera >= 0 && ue::IsA(cameraActor, C.CameraActor)
                     ? ue::Ptr(cameraActor, O.CamActor_Camera)
                     : nullptr;
  void* target = ue::Ptr(cameraManager, O.PCM_ViewTarget + O.VT_Target);
  char settingsFov[64];
  DescribeSettingsFov(settingsFov);
  LOG("Player: %s | camera manager %s | view target %s | player camera actor %s",
      ue::ClassName(character).c_str(), ue::ClassName(cameraManager).c_str(), TargetName(target, character).c_str(),
      ue::ClassName(cameraActor).c_str());
  LOG("Player camera: %s", DescribeCamera(camera).c_str());
  LOG("Player FOV: GetDefaultCameraFov %.2f, GetCameraFov %.2f, game setting %s, local player aspect axis %d",
      game::CallFloat(game::GetDefaultCameraFov, character, -1.f), game::CallFloat(game::GetCameraFov, character, -1.f),
      settingsFov, localPlayer && O.LP_AspectConstraint >= 0 ? ue::At<uint8_t>(localPlayer, O.LP_AspectConstraint) : -1);
  if (cfg::S.verbose) {
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      if (cameraActor) ue::DumpStruct(ue::ClassOf(cameraActor));
      if (camera) ue::DumpStruct(ue::ClassOf(camera));
    }
  }
}

}  // namespace

void OnFrame(void* viewportClient, double now) {
  const float dt = g_lastFrame > 0 ? util::Clamp(static_cast<float>(now - g_lastFrame), 0.f, 0.1f) : 0.f;
  g_lastFrame = now;
  const bool povFresh = g_povFrame == g_frame;  // the camera hook ran for the player this frame
  ++g_frame;
  St.hookAvailable = S.ApplyCameraModifiers >= 0;

  // The local player, through the viewport's game instance (looked up fresh every frame).
  void* gameInstance = ue::Ptr(viewportClient, O.GVC_GameInstance);
  void* localPlayer = nullptr;
  if (gameInstance) {
    const auto& players = ue::At<ue::TArray<void*>>(gameInstance, O.GI_LocalPlayers);
    if (players.data && players.num > 0 && players.num < 16) localPlayer = players.data[0];
  }
  void* pc = localPlayer ? ue::Ptr(localPlayer, O.Player_PC) : nullptr;
  void* cameraManager = pc ? ue::Ptr(pc, O.PC_CameraManager) : nullptr;
  void* pawn = pc ? ue::Ptr(pc, O.Ctrl_Pawn) : nullptr;
  void* character = pawn && ue::IsA(pawn, C.PlayerCharacter) ? pawn : nullptr;
  if (!cameraManager) character = nullptr;

  St.lockedFov = 0.f;
  if (cameraManager) {
    EnsureCameraHook(cameraManager);
    if (O.PCM_LockedFOV >= 0) {
      float& locked = ue::At<float>(cameraManager, O.PCM_LockedFOV);
      if (g_clearLockedRequested) {
        LOG("Cleared console FOV override (was %.1f)", locked);
        locked = 0.f;
      }
      St.lockedFov = locked;
    }
  }
  g_clearLockedRequested = false;
  St.hasPlayer = character != nullptr;
  St.character = character;
  St.cameraManager = cameraManager;

  St.shown = "nothing";
  bool otherCamera = false;
  void* viewTarget = nullptr;
  if (character) {
    DescribePlayer(character, cameraManager, localPlayer);
    St.gameBase = game::CallFloat(game::GetDefaultCameraFov, character, 0.f);
    if (!Plausible(St.gameBase)) St.gameBase = 0.f;
    St.aiming = game::CallBool(game::IsAimingDownSight, character, false);
    viewTarget = ue::Ptr(cameraManager, O.PCM_ViewTarget + O.VT_Target);
    void* pending = ue::Ptr(cameraManager, O.PCM_PendingViewTarget + O.VT_Target);
    if (IsPlayerView(viewTarget, character)) {
      St.shown = pending && !IsPlayerView(pending, character) ? "player, blending away" : "player";
    } else if (IsPlayerView(pending, character)) {
      St.shown = "other camera, blending to player";
    } else {
      St.shown = "other camera";
      otherCamera = true;
    }
  } else {
    St.aiming = false;
  }

  if (!character) {
    St.reason = Reason::NoPlayer;
  } else if (!cfg::S.enabled) {
    St.reason = Reason::Disabled;
  } else if (otherCamera) {
    St.reason = Reason::OtherCamera;
  } else {
    St.reason = Reason::Applying;
  }
  // Another camera on screen doesn't change the player camera's blend weight: the camera blend
  // between them takes care of the transition.
  const float target = character && cfg::S.enabled ? 1.f : 0.f;
  const float step = cfg::S.transitionSeconds > 0.001f ? dt / cfg::S.transitionSeconds : 1.f;
  St.alpha = target > St.alpha ? (std::min)(target, St.alpha + step) : (std::max)(target, St.alpha - step);
  const float zoomTarget = St.aiming ? 1.f : 0.f;
  St.zoomAlpha += (zoomTarget - St.zoomAlpha) * (std::min)(1.f, dt * kZoomSpeed);
  if (std::fabs(zoomTarget - St.zoomAlpha) < 0.001f) St.zoomAlpha = zoomTarget;

  if (character) {
    float screenFov = 0.f;
    if (O.PCM_CameraCache >= 0 && O.Cache_POV >= 0) {
      screenFov = ue::At<float>(cameraManager, O.PCM_CameraCache + O.Cache_POV + O.POV_FOV);
    }
    Trace(now, LogSituation(TargetName(viewTarget, character)), povFresh, screenFov);
  }

  // Ease hotkey changes over ~0.1s instead of snapping.
  if (St.smoothedFov <= 0.f) St.smoothedFov = cfg::S.fov;
  St.smoothedFov += (cfg::S.fov - St.smoothedFov) * (1.f - std::exp(-dt / 0.05f));

  static long reportedFaults = 0;
  if (g_faults != reportedFaults) {
    reportedFaults = g_faults;
    if (reportedFaults <= 5) LOG("Recovered from a fault in the camera hook (%ld so far)", reportedFaults);
  }
}

void RequestClearLockedFov() { g_clearLockedRequested = true; }

}  // namespace fov
