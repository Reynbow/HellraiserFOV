#pragma once

// In-game menu and FOV popup, drawn with the engine's own UCanvas (no D3D hooking, no ReShade).
// Works with the mouse (click, drag, wheel, right-click = default) and the keyboard.

namespace ui {

// Called once per frame on the game thread (from the viewport's PostRender).
void OnFrame(void* viewportClient, void* canvas, double now);

// True while the menu is open and on screen: the mouse and the menu keys belong to the menu.
bool CapturesInput();
// Whether a key press should go to the mod: hotkeys always, menu keys while the menu has input
// and Alt isn't held (Alt+Enter and friends belong to the game).
bool WantsKey(int vk, bool alt);
void OnKeyDown(int vk, bool repeat);

}  // namespace ui
