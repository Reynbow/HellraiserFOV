#include "game.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "log.h"
#include "scan.h"

namespace game {

Offsets O;
Classes C;
Functions F;
Slots S;
PlayerFunction GetDefaultCameraFov("GetDefaultCameraFov");
PlayerFunction GetCameraFov("GetCameraFov");
PlayerFunction IsAimingDownSight("IsAimingDownSight");

namespace {

// The slots in Unreal Engine 5.6, for the log: a different value means the engine changed.
constexpr int kUsualPostRender = 111;
constexpr int kUsualApplyCameraModifiers = 253;

void* g_engine = nullptr;

bool Contains(const std::vector<uintptr_t>& v, uintptr_t x) { return std::find(v.begin(), v.end(), x) != v.end(); }

void** VtableOfDefault(void* cls) {
  void* cdo = ue::DefaultObject(cls);
  return cdo ? *static_cast<void***>(cdo) : nullptr;
}

}  // namespace

// UGameViewportClient::PostRender only calls DrawTransition (declared right after it), which
// draws the "LOADING" / "PAUSED" messages; DrawTransition is found by those strings.
int FindPostRenderSlot(void** vtable) {
  if (!vtable) return -1;
  const std::vector<uintptr_t> drawTransition = scan::FunctionsReferencing(L"LoadingMessage");
  for (int slot = 1; slot < 512; ++slot) {
    const auto fn = reinterpret_cast<uintptr_t>(vtable[slot]);
    if (!scan::InText(fn)) break;
    if (!Contains(drawTransition, fn)) continue;
    const int thunk = scan::VirtualThunkSlot(reinterpret_cast<uintptr_t>(vtable[slot - 1]));
    LOG("PostRender: vtable slot %d (DrawTransition at exe+0x%llx; slot %d %s)%s", slot - 1,
        static_cast<unsigned long long>(fn - scan::Img().base), slot - 1,
        thunk == slot ? "forwards to it" : "doesn't look like the usual one-line forwarder",
        slot - 1 == kUsualPostRender ? "" : " - not the usual slot");
    return slot - 1;
  }
  LOG("PostRender: DrawTransition not found in the viewport vtable (%zu candidates)", drawTransition.size());
  return -1;
}

// The slot of the last virtual call in |fn| that's passed &OutVT.POV as its third argument
// (`lea r8, [reg + POV]` shortly before `call [reg + slot*8]`), or -1.
static int LastCallPassingPov(uintptr_t fn, int32_t povOffset) {
  static const uint8_t kCall[] = {0xFF, 0x90};  // call qword ptr [reg + disp32]
  int found = -1;
  for (uintptr_t at : scan::FindInFunction(fn, kCall, "x?")) {
    const auto* b = reinterpret_cast<const uint8_t*>(at);
    if ((b[1] & 0xF8) != 0x90 || b[1] == 0x94) continue;
    int32_t disp = 0;
    memcpy(&disp, b + 2, 4);
    if (disp <= 0 || disp % 8 || disp / 8 >= 1024) continue;
    // lea r8, [reg + POV] (REX.WR or REX.WRB, ModRM reg = r8) in the 16 bytes before the call.
    bool passesPov = false;
    for (const uint8_t* p = b - 16; p + 4 <= b && !passesPov; ++p) {
      if ((p[0] != 0x4C && p[0] != 0x4D) || p[1] != 0x8D || (p[2] & 7) == 4) continue;
      if ((p[2] & 0xF8) == 0x40 && p[3] == povOffset) passesPov = true;  // disp8
      int32_t d32 = 0;
      if ((p[2] & 0xF8) == 0x80 && p + 7 <= b && (memcpy(&d32, p + 3, 4), d32 == povOffset)) passesPov = true;
    }
    if (passesPov) found = disp / 8;
  }
  return found;
}

// In UpdateViewTarget (found by its "FreeCam_Default" camera style name), the camera modifiers
// are applied last: the final virtual call on the camera manager that's passed &OutVT.POV.
int FindApplyCameraModifiersSlot(void** vtable, int32_t povOffset) {
  if (!vtable || povOffset < 0) return -1;
  const std::vector<uintptr_t> candidates = scan::FunctionsReferencing(L"FreeCam_Default");
  for (int slot = 0; slot < 1024; ++slot) {
    const auto fn = reinterpret_cast<uintptr_t>(vtable[slot]);
    if (!scan::InText(fn)) break;
    if (!Contains(candidates, fn)) continue;
    const int found = LastCallPassingPov(fn, povOffset);
    if (found < 0) continue;
    LOG("ApplyCameraModifiers: vtable slot %d (UpdateViewTarget: slot %d, exe+0x%llx)%s", found, slot,
        static_cast<unsigned long long>(fn - scan::Img().base),
        found == kUsualApplyCameraModifiers ? "" : " - not the usual slot");
    return found;
  }
  LOG("ApplyCameraModifiers: UpdateViewTarget not found in the camera manager vtable (%zu candidates)",
      candidates.size());
  return -1;
}

bool Resolve(std::string& error) {
  std::vector<std::string> missing;
  const auto need = [&](void* p, const char* what) {
    if (!p) missing.push_back(what);
    return p;
  };
  const auto offset = [&](void* s, const char* sname, const char* prop, int32_t& out, bool optional = false) {
    out = s ? ue::Offset(s, prop) : -1;
    if (out >= 0) return;
    const std::string what = std::string(sname) + "::" + prop;
    if (optional) {
      LOG("Optional %s not found", what.c_str());
    } else {
      missing.push_back(what);
    }
  };
  const auto prop = [&](void* s, const char* sname, const char* name, ue::Prop& out, bool optional = false) {
    out = s ? ue::FindProp(s, name) : ue::Prop{};
    if (out.Valid()) return;
    const std::string what = std::string(sname) + "::" + name;
    if (optional) {
      LOG("Optional %s not found", what.c_str());
    } else {
      missing.push_back(what);
    }
  };

  void* engine = need(ue::FindClass("Engine"), "UEngine");
  void* viewport = need(ue::FindClass("GameViewportClient"), "UGameViewportClient");
  void* gameInstance = need(ue::FindClass("GameInstance"), "UGameInstance");
  void* player = need(ue::FindClass("Player"), "UPlayer");
  void* localPlayer = need(ue::FindClass("LocalPlayer"), "ULocalPlayer");
  void* controller = need(ue::FindClass("Controller"), "AController");
  void* playerController = need(ue::FindClass("PlayerController"), "APlayerController");
  void* cameraManager = need(ue::FindClass("PlayerCameraManager"), "APlayerCameraManager");
  void* font = need(ue::FindClass("Font"), "UFont");
  void* canvas = need(ue::FindClass("Canvas"), "UCanvas");
  void* viewTarget = need(ue::FindStruct("TViewTarget"), "FTViewTarget");
  void* viewInfo = need(ue::FindStruct("MinimalViewInfo"), "FMinimalViewInfo");
  void* cacheEntry = ue::FindStruct("CameraCacheEntry");
  C.CameraComponent = ue::FindClass("CameraComponent");
  C.CameraActor = ue::FindClass("CameraActor");
  void* childActorComponent = ue::FindClass("ChildActorComponent");
  C.PlayerCharacter = ue::FindObjectIn("Class", "PlayerCharacter", "/Script/Project");
  C.InfernoUserSettings = ue::FindObjectIn("Class", "InfernoUserSettings", "/Script/Project");
  if (!C.PlayerCharacter) missing.push_back("APlayerCharacter (/Script/Project)");

  offset(engine, "UEngine", "GameViewport", O.Eng_GameViewport);
  offset(engine, "UEngine", "GameUserSettings", O.Eng_GameUserSettings, true);
  offset(engine, "UEngine", "SmallFont", O.Eng_SmallFont);
  offset(engine, "UEngine", "MediumFont", O.Eng_MediumFont, true);
  offset(engine, "UEngine", "LargeFont", O.Eng_LargeFont, true);
  offset(engine, "UEngine", "SubtitleFont", O.Eng_SubtitleFont, true);
  offset(viewport, "UGameViewportClient", "GameInstance", O.GVC_GameInstance);
  offset(gameInstance, "UGameInstance", "LocalPlayers", O.GI_LocalPlayers);
  offset(player, "UPlayer", "PlayerController", O.Player_PC);
  offset(localPlayer, "ULocalPlayer", "AspectRatioAxisConstraint", O.LP_AspectConstraint, true);
  offset(controller, "AController", "Pawn", O.Ctrl_Pawn);
  offset(playerController, "APlayerController", "PlayerCameraManager", O.PC_CameraManager);
  offset(cameraManager, "APlayerCameraManager", "PCOwner", O.PCM_PCOwner);
  offset(cameraManager, "APlayerCameraManager", "ViewTarget", O.PCM_ViewTarget);
  offset(cameraManager, "APlayerCameraManager", "PendingViewTarget", O.PCM_PendingViewTarget);
  offset(cameraManager, "APlayerCameraManager", "LockedFOV", O.PCM_LockedFOV, true);
  offset(cameraManager, "APlayerCameraManager", "CameraCachePrivate", O.PCM_CameraCache, true);
  offset(viewTarget, "FTViewTarget", "Target", O.VT_Target);
  offset(viewTarget, "FTViewTarget", "POV", O.VT_POV);
  offset(cacheEntry, "FCameraCacheEntry", "POV", O.Cache_POV, true);
  offset(viewInfo, "FMinimalViewInfo", "FOV", O.POV_FOV);
  offset(viewInfo, "FMinimalViewInfo", "DesiredFOV", O.POV_DesiredFOV, true);
  offset(viewInfo, "FMinimalViewInfo", "FirstPersonFOV", O.POV_FirstPersonFOV, true);
  offset(viewInfo, "FMinimalViewInfo", "AspectRatio", O.POV_AspectRatio, true);
  prop(viewInfo, "FMinimalViewInfo", "bUseFirstPersonParameters", O.POV_UseFirstPerson, true);
  prop(viewInfo, "FMinimalViewInfo", "bConstrainAspectRatio", O.POV_ConstrainAspect, true);
  offset(font, "UFont", "FontCacheType", O.Font_CacheType, true);
  offset(font, "UFont", "LegacyFontSize", O.Font_LegacyFontSize, true);
  offset(canvas, "UCanvas", "OrgX", O.Canvas_OrgX, true);
  offset(canvas, "UCanvas", "OrgY", O.Canvas_OrgY, true);
  offset(canvas, "UCanvas", "ClipX", O.Canvas_ClipX, true);
  offset(canvas, "UCanvas", "ClipY", O.Canvas_ClipY, true);
  prop(canvas, "UCanvas", "bCenterX", O.Canvas_CenterX, true);
  prop(canvas, "UCanvas", "bCenterY", O.Canvas_CenterY, true);
  offset(canvas, "UCanvas", "SizeX", O.Canvas_SizeX);
  offset(canvas, "UCanvas", "SizeY", O.Canvas_SizeY);
  offset(canvas, "UCanvas", "DefaultTexture", O.Canvas_DefaultTexture, true);
  offset(C.PlayerCharacter, "APlayerCharacter", "CineCamera", O.Char_CineCamera, true);
  offset(C.PlayerCharacter, "APlayerCharacter", "FPCameraActor", O.Char_FPCameraActor);
  offset(childActorComponent, "UChildActorComponent", "ChildActor", O.ChildActor_Actor);
  offset(C.CameraActor, "ACameraActor", "CameraComponent", O.CamActor_Camera, true);
  offset(C.CameraComponent, "UCameraComponent", "FieldOfView", O.Cam_FieldOfView, true);
  offset(C.CameraComponent, "UCameraComponent", "FirstPersonFieldOfView", O.Cam_FirstPersonFOV, true);
  prop(C.CameraComponent, "UCameraComponent", "bEnableFirstPersonFieldOfView", O.Cam_EnableFirstPersonFOV, true);
  offset(C.CameraComponent, "UCameraComponent", "AspectRatioAxisConstraint", O.Cam_AspectConstraint, true);
  prop(C.CameraComponent, "UCameraComponent", "bOverrideAspectRatioAxisConstraint", O.Cam_OverrideAspect, true);
  prop(C.CameraComponent, "UCameraComponent", "bConstrainAspectRatio", O.Cam_ConstrainAspect, true);
  offset(C.InfernoUserSettings, "UInfernoUserSettings", "FOV", O.Settings_FOV, true);

  if (!F.K2DrawTexture.Bind(canvas, "K2_DrawTexture")) missing.push_back("UCanvas::K2_DrawTexture");
  if (!F.K2DrawText.Bind(canvas, "K2_DrawText")) missing.push_back("UCanvas::K2_DrawText");
  if (!F.K2TextSize.Bind(canvas, "K2_TextSize")) missing.push_back("UCanvas::K2_TextSize");
  for (const char* fn : {"GetDefaultCameraFov", "GetCameraFov", "IsAimingDownSight"}) {
    if (C.PlayerCharacter && !ue::FindFunction(C.PlayerCharacter, fn)) LOG("Optional APlayerCharacter::%s not found", fn);
  }

  if (!missing.empty()) {
    error = "not found: ";
    for (size_t i = 0; i < missing.size(); ++i) error += (i ? ", " : "") + missing[i];
    return false;
  }

  S.PostRender = FindPostRenderSlot(VtableOfDefault(viewport));
  if (S.PostRender < 0) {
    error = "UGameViewportClient::PostRender not found";
    return false;
  }
  // Without it the menu still works; the status line says the camera hook isn't active.
  S.ApplyCameraModifiers = FindApplyCameraModifiersSlot(VtableOfDefault(cameraManager), O.VT_POV);
  return true;
}

void* Engine() {
  if (!g_engine) {
    void* cls = ue::FindClass("GameEngine");
    g_engine = cls ? ue::FindInstance(cls) : nullptr;
  }
  return g_engine;
}

void* GameViewport() {
  void* engine = Engine();
  return engine ? ue::Ptr(engine, O.Eng_GameViewport) : nullptr;
}

float CallFloat(const ue::Function& fn, void* object, float fallback) {
  alignas(16) uint8_t params[64] = {};
  if (!fn.Bound() || !object || fn.Return() < 0 || fn.ParamsSize() > static_cast<int32_t>(sizeof params)) {
    return fallback;
  }
  fn.Call(object, params);
  return fn.ReturnSize() == 8 ? static_cast<float>(ue::At<double>(params, fn.Return()))
                              : ue::At<float>(params, fn.Return());
}

bool CallBool(const ue::Function& fn, void* object, bool fallback) {
  alignas(16) uint8_t params[64] = {};
  if (!fn.Bound() || !object || fn.Return() < 0 || fn.ParamsSize() > static_cast<int32_t>(sizeof params)) {
    return fallback;
  }
  fn.Call(object, params);
  return ue::At<uint8_t>(params, fn.Return()) != 0;
}

bool PlayerFunction::BindTo(void* character) {
  void* cls = character ? ue::ClassOf(character) : nullptr;
  if (cls != cls_) {
    cls_ = cls;
    if (!cls || !fn_.Bind(cls, name_)) fn_ = ue::Function{};
  }
  return fn_.Bound();
}

float CallFloat(PlayerFunction& fn, void* character, float fallback) {
  return fn.BindTo(character) ? CallFloat(fn.Get(), character, fallback) : fallback;
}

bool CallBool(PlayerFunction& fn, void* character, bool fallback) {
  return fn.BindTo(character) ? CallBool(fn.Get(), character, fallback) : fallback;
}

void* PlayerCameraActor(void* character) {
  void* component = character && O.Char_FPCameraActor >= 0 ? ue::Ptr(character, O.Char_FPCameraActor) : nullptr;
  return component && O.ChildActor_Actor >= 0 ? ue::Ptr(component, O.ChildActor_Actor) : nullptr;
}

}  // namespace game
