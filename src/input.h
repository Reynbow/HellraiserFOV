#pragma once

// Keyboard and mouse routing through a subclass of the game window. Everything here runs on the
// game thread: Unreal pumps window messages on it, and the menu is drawn from PostRender on it.
//
// While the menu is on screen it owns the mouse: raw-input packets (camera look), mouse moves,
// clicks and the wheel are swallowed. Each key and mouse button remembers who got its "down", so
// its "up" goes to the same side even if the menu opened or closed in between (no stuck keys).

#include <cstdint>
#include <vector>

namespace input {

struct MouseEvent {
  enum Type { LeftDown, LeftUp, RightDown, RightUp, Wheel, Cancel } type;
  float x, y;     // client pixels
  int wheel;      // WHEEL_DELTA units, positive = away from the user
  uint32_t time;  // GetMessageTime(): when the input happened, unaffected by frame hitches
};

// Subclasses the game window once it exists (retries every second).
void Maintain(double now);
// Mouse events captured for the menu since the last call.
std::vector<MouseEvent> TakeEvents();

// Cursor position in client pixels (valid even while the game hides the cursor) and the client size.
bool CursorClientPos(float& x, float& y, float& clientW, float& clientH);
// True when Windows is showing a cursor, e.g. in the game's own pause menu.
bool OsCursorVisible();
void MoveCursorToClient(float x, float y);

}  // namespace input
