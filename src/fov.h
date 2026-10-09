#pragma once

// Gameplay-only FOV. The player camera manager's ApplyCameraModifiers runs once per view target
// every frame; the hook rescales the FOV only when that view target is the player character (or
// its first-person camera actor). Cutscene cameras are separate view targets and are never
// touched; camera blends between them and the player interpolate naturally.

namespace fov {

enum class Reason { NoPlayer, Disabled, Applying, OtherCamera };

struct Status {
  bool hookAvailable = false;  // ApplyCameraModifiers was found at startup
  bool cameraHooked = false;   // ...and is hooked in the current camera manager
  bool hasPlayer = false;
  Reason reason = Reason::NoPlayer;
  float alpha = 0.f;        // blend weight: 0 = game FOV, 1 = your FOV
  float smoothedFov = 0.f;  // your FOV, eased so hotkey changes zoom smoothly
  float lastIn = 0.f;       // FOV the game wanted for the player camera last frame
  float lastOut = 0.f;      // FOV after the mod
  float lastBase = 0.f;     // the FOV that "keep effects" scaled around
  float gameBase = 0.f;     // the game's gameplay FOV (APlayerCharacter::GetDefaultCameraFov), 0 = unknown
  // Aiming down sights (APlayerCharacter::IsAimingDownSight).
  bool aiming = false;
  float zoomAlpha = 0.f;    // 0 = not aiming, 1 = aiming; eased like the game's own zoom
  float gameAimFov = 0.f;   // where the game's aiming FOV settles, once seen (0 = not yet)
  // First-person meshes (arms, held items), when the game draws them with a FOV of their own.
  float gameViewmodelFov = 0.f;  // the game's FOV for them, last seen (0 = never seen)
  float lastViewmodelOut = 0.f;  // after the mod, last frame (0 = not drawn with their own FOV)
  double lastPovTime = -1.0;
  float lockedFov = 0.f;  // APlayerCameraManager::LockedFOV (the console 'fov' command)
  const char* shown = "nothing";  // what the camera shows, for the menu's status line
  void* character = nullptr;
  void* cameraManager = nullptr;
};

extern Status St;

// Called once per frame on the game thread (from the viewport's PostRender).
void OnFrame(void* viewportClient, double now);
void RequestClearLockedFov();

}  // namespace fov
