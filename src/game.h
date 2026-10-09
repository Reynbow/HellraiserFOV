#pragma once

// Everything the mod uses from the game, looked up by name through reflection at startup:
// property offsets, functions it calls, and the vtable slots it hooks. -1 / nullptr means
// "not found" for optional entries; Resolve() fails if a required one is missing.

#include <cstdint>
#include <string>

#include "ue.h"

namespace game {

struct Offsets {
  int32_t Eng_GameViewport = -1;       // UEngine::GameViewport
  int32_t Eng_GameUserSettings = -1;   // UEngine::GameUserSettings
  int32_t Eng_SmallFont = -1;          // UEngine::SmallFont
  int32_t Eng_MediumFont = -1;         // UEngine::MediumFont
  int32_t Eng_LargeFont = -1;          // UEngine::LargeFont
  int32_t Eng_SubtitleFont = -1;       // UEngine::SubtitleFont
  int32_t GVC_GameInstance = -1;       // UGameViewportClient::GameInstance
  int32_t GI_LocalPlayers = -1;        // UGameInstance::LocalPlayers
  int32_t Player_PC = -1;              // UPlayer::PlayerController
  int32_t LP_AspectConstraint = -1;    // ULocalPlayer::AspectRatioAxisConstraint (log only)
  int32_t Ctrl_Pawn = -1;              // AController::Pawn
  int32_t PC_CameraManager = -1;       // APlayerController::PlayerCameraManager
  int32_t PCM_PCOwner = -1;            // APlayerCameraManager::PCOwner
  int32_t PCM_ViewTarget = -1;         // APlayerCameraManager::ViewTarget
  int32_t PCM_PendingViewTarget = -1;  // APlayerCameraManager::PendingViewTarget
  int32_t PCM_LockedFOV = -1;          // APlayerCameraManager::LockedFOV (set by the 'fov' console command)
  int32_t PCM_CameraCache = -1;        // APlayerCameraManager::CameraCachePrivate (log only)
  int32_t VT_Target = -1;              // FTViewTarget::Target
  int32_t VT_POV = -1;                 // FTViewTarget::POV
  int32_t Cache_POV = -1;              // FCameraCacheEntry::POV
  int32_t POV_FOV = -1;                // FMinimalViewInfo::FOV
  int32_t POV_DesiredFOV = -1;         // FMinimalViewInfo::DesiredFOV
  int32_t POV_FirstPersonFOV = -1;     // FMinimalViewInfo::FirstPersonFOV
  int32_t POV_AspectRatio = -1;        // FMinimalViewInfo::AspectRatio (log only)
  ue::Prop POV_UseFirstPerson;         // FMinimalViewInfo::bUseFirstPersonParameters
  ue::Prop POV_ConstrainAspect;        // FMinimalViewInfo::bConstrainAspectRatio (log only)
  int32_t Font_CacheType = -1;         // UFont::FontCacheType (uint8: 0 offline bitmap, 1 runtime)
  int32_t Font_LegacyFontSize = -1;    // UFont::LegacyFontSize (int32 points; runtime fonts rasterize at this)
  int32_t Canvas_OrgX = -1;            // UCanvas::OrgX
  int32_t Canvas_OrgY = -1;            // UCanvas::OrgY
  int32_t Canvas_ClipX = -1;           // UCanvas::ClipX
  int32_t Canvas_ClipY = -1;           // UCanvas::ClipY
  ue::Prop Canvas_CenterX;             // UCanvas::bCenterX
  ue::Prop Canvas_CenterY;             // UCanvas::bCenterY
  int32_t Canvas_SizeX = -1;           // UCanvas::SizeX
  int32_t Canvas_SizeY = -1;           // UCanvas::SizeY
  int32_t Canvas_DefaultTexture = -1;  // UCanvas::DefaultTexture
  // Not reflected: UCanvas::Canvas (FCanvas*) and the FCanvas fields GetDPIScale() reads. The
  // values are Unreal Engine 5.6's; each frame they're used only if what they point at is sane.
  int32_t Canvas_Canvas = 0x2E0;
  int32_t FCanvas_Stereo = 0xA0;
  int32_t FCanvas_DPIScale = 0xB0;
  // The game's player character (APlayerCharacter) and its cameras. The game renders through
  // BP_PlayerCamera, the child actor of APlayerCharacter::FPCameraActor.
  int32_t Char_CineCamera = -1;        // APlayerCharacter::CineCamera (log only)
  int32_t Char_FPCameraActor = -1;     // APlayerCharacter::FPCameraActor (UChildActorComponent)
  int32_t ChildActor_Actor = -1;       // UChildActorComponent::ChildActor
  int32_t CamActor_Camera = -1;        // ACameraActor::CameraComponent (log only)
  int32_t Cam_FieldOfView = -1;        // UCameraComponent::FieldOfView (log only)
  int32_t Cam_FirstPersonFOV = -1;     // UCameraComponent::FirstPersonFieldOfView (log only)
  ue::Prop Cam_EnableFirstPersonFOV;   // UCameraComponent::bEnableFirstPersonFieldOfView (log only)
  int32_t Cam_AspectConstraint = -1;   // UCameraComponent::AspectRatioAxisConstraint (log only)
  ue::Prop Cam_OverrideAspect;         // UCameraComponent::bOverrideAspectRatioAxisConstraint (log only)
  ue::Prop Cam_ConstrainAspect;        // UCameraComponent::bConstrainAspectRatio (log only)
  int32_t Settings_FOV = -1;           // UInfernoUserSettings::FOV (log only)
};

struct Classes {
  void* PlayerCharacter = nullptr;  // APlayerCharacter (/Script/Project)
  void* CameraComponent = nullptr;
  void* CameraActor = nullptr;
  void* InfernoUserSettings = nullptr;
};

struct Functions {
  ue::Function K2DrawTexture;  // UCanvas::K2_DrawTexture
  ue::Function K2DrawText;     // UCanvas::K2_DrawText
  ue::Function K2TextSize;     // UCanvas::K2_TextSize
};

// A function of the player character, bound to the character's actual class: the game's
// Blueprint player class overrides some of them (GetDefaultCameraFov...), and only calling the
// override gives the game's real value.
class PlayerFunction {
 public:
  explicit PlayerFunction(const char* name) : name_(name) {}
  // Binds to |character|'s class if it changed; false if the function doesn't exist there.
  bool BindTo(void* character);
  const ue::Function& Get() const { return fn_; }

 private:
  const char* name_;
  void* cls_ = nullptr;
  ue::Function fn_;
};

extern PlayerFunction GetDefaultCameraFov;  // APlayerCharacter::GetDefaultCameraFov
extern PlayerFunction GetCameraFov;         // APlayerCharacter::GetCameraFov
extern PlayerFunction IsAimingDownSight;    // APlayerCharacter::IsAimingDownSight

struct Slots {
  int PostRender = -1;            // UGameViewportClient::PostRender(UCanvas*)
  int ApplyCameraModifiers = -1;  // APlayerCameraManager::ApplyCameraModifiers(float, FMinimalViewInfo&)
};

extern Offsets O;
extern Classes C;
extern Functions F;
extern Slots S;

// Looks everything up (after ue::Init). Returns false with |error| if something required is missing.
bool Resolve(std::string& error);

// The vtable slot finders Resolve uses, given the vtable of UGameViewportClient / of
// APlayerCameraManager and FTViewTarget::POV's offset (also run by the offline check). -1 if not found.
int FindPostRenderSlot(void** vtable);
int FindApplyCameraModifiersSlot(void** vtable, int32_t povOffset);

// The engine (UGameEngine), or nullptr before it exists.
void* Engine();
void* GameViewport();

// Calls a function with no parameters that returns a float / bool. Returns |fallback| if the
// function isn't bound.
float CallFloat(const ue::Function& fn, void* object, float fallback);
bool CallBool(const ue::Function& fn, void* object, bool fallback);
// The same for a player character function (binding it to the character's class first).
float CallFloat(PlayerFunction& fn, void* character, float fallback);
bool CallBool(PlayerFunction& fn, void* character, bool fallback);

// The actor the player's view comes from: BP_PlayerCamera, the child actor of the character's
// FPCameraActor component (nullptr if there's none).
void* PlayerCameraActor(void* character);

}  // namespace game
