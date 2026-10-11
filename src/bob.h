#pragma once

// Camera bob. The game's player camera hangs from the camera_jnt bone of the character mesh, so
// the walk and sprint animations move it with every step; the game's own Camera Bobbing setting
// only reduces that. With the mod's Camera bobbing setting off, the camera is held at a steady
// point on the body instead: its position in the mesh's frame and its rotation relative to where
// the player aims are followed slowly (crouching, posture and looking around still move it, and it
// never strays more than a few centimetres or degrees from the game's camera) while the quick
// step-by-step motion is taken out. Camera moves made by animation montages (takedowns,
// interactions...) are left to the game.

namespace bob {

// Called in the camera manager's ApplyCameraModifiers before the game's camera shakes and camera
// animations are added, with the view of the player's own camera (|pov|, an FMinimalViewInfo).
void BeforeModifiers(void* character, void* controller, void* pov, float deltaTime);
// Called once per frame (from the viewport's PostRender): the log lines about what it does.
void OnFrame(void* cameraManager, double now);

}  // namespace bob
