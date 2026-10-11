#include "bob.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "game.h"
#include "log.h"
#include "ue.h"
#include "util.h"

namespace bob {
namespace {

using game::F;
using game::O;

constexpr uint8_t kSpaceWorld = 0;  // ERelativeTransformSpace::RTS_World

// The steady point follows the camera's position on the body and its aim offset through two
// low-pass stages with these time constants (seconds): slow while the player moves, quicker while
// standing still, quick while the game's camera is in use.
constexpr double kTauMoving = 0.45;
constexpr double kTauIdle = 0.25;
constexpr double kTauGame = 0.05;
constexpr double kIdleSpeed = 20.0;  // cm/s
// Looking around moves the head on the body (bending, twisting), so the steady point follows
// quicker while the view turns faster than this (degrees per second): once more as quick for
// every kLookRateScale beyond it.
constexpr double kLookRate = 20.0;
constexpr double kLookRateScale = 40.0;
// Close to looking straight up or down, yaw and roll stop meaning much: the rotation is left alone
// from kVerticalFrom degrees of pitch on, fading in below that.
constexpr double kVerticalFrom = 85.0;
constexpr double kVerticalFade = 15.0;
// The steady camera never strays further than this from the game's camera: larger moves
// (crouching, vaulting...) carry it along instead of being smoothed out. Once one has carried it
// for a moment, the steady point catches up quickly for a little while, so the camera settles
// into the new posture instead of sinking into it slowly.
constexpr double kMaxCm = 8.0;
constexpr double kMaxDeg = 5.0;
constexpr double kCarriedSeconds = 0.08;
constexpr double kCatchUpSeconds = 0.4;
constexpr double kTauCatchUp = 0.1;
// While an animation montage plays (takedowns, interactions...), the camera moving this far from
// the steady point hands it back to the game until the montage ends.
constexpr double kMontageCm = 5.0;
constexpr double kMontageDeg = 3.0;
// So does a jump this large in one frame (a cut in the animation), for a moment.
constexpr double kJumpCm = 25.0;
constexpr double kJumpDeg = 15.0;
constexpr double kSettleSeconds = 0.5;
constexpr double kBlendInSeconds = 0.3;
constexpr double kBlendOutSeconds = 0.15;
// Log limits.
constexpr double kSummarySeconds = 5.0;
constexpr int kMaxSummaryLines = 500;
constexpr int kMaxHandOverLines = 200;
constexpr int kMaxEffectLines = 300;

struct Vec {
  double x, y, z;
};
struct Quat {
  double x, y, z, w;
};
struct Rot {
  double pitch, yaw, roll;
};

Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

// v rotated by the unit quaternion q: v + w*t + u x t, with u = (x, y, z) and t = 2 u x v.
Vec Rotate(const Quat& q, Vec v) {
  const Vec u{q.x, q.y, q.z};
  const Vec c = Cross(u, v);
  const Vec t{2.0 * c.x, 2.0 * c.y, 2.0 * c.z};
  const Vec ut = Cross(u, t);
  return {v.x + q.w * t.x + ut.x, v.y + q.w * t.y + ut.y, v.z + q.w * t.z + ut.z};
}
Vec Unrotate(const Quat& q, Vec v) { return Rotate({-q.x, -q.y, -q.z, q.w}, v); }

double Wrap(double deg) {
  deg = std::fmod(deg + 180.0, 360.0);
  if (deg < 0.0) deg += 360.0;
  return deg - 180.0;
}

bool Ready() {
  return O.Char_FPCameraActor >= 0 && O.Scene_AttachParent >= 0 && O.POV_Location >= 0 && O.POV_Rotation >= 0 &&
         O.Ctrl_ControlRotation >= 0 && O.Xform_Rotation >= 0 && O.Xform_Translation >= 0 &&
         F.GetSocketTransform.Bound() && F.GetSocketTransform.Return() >= 0;
}

// A component's rotation and position in the world: USceneComponent::GetSocketTransform with no
// socket.
bool WorldTransform(void* component, Quat& q, Vec& t) {
  const ue::Function& fn = F.GetSocketTransform;
  static const int32_t pName = fn.Param("InSocketName");
  static const int32_t pSpace = fn.Param("TransformSpace");
  alignas(16) uint8_t params[256] = {};
  if (pName < 0 || pSpace < 0 || fn.ParamsSize() > static_cast<int32_t>(sizeof params)) return false;
  ue::At<ue::FName>(params, pName) = ue::FName{0, 0};  // NAME_None
  ue::At<uint8_t>(params, pSpace) = kSpaceWorld;
  fn.Call(component, params);
  const uint8_t* ret = params + fn.Return();
  memcpy(&q, ret + O.Xform_Rotation, sizeof q);
  memcpy(&t, ret + O.Xform_Translation, sizeof t);
  return std::isfinite(q.w) && std::isfinite(t.x);
}

double PlanarSpeed(void* character) {
  void* movement = O.Char_Movement >= 0 && O.Move_Velocity >= 0 ? ue::Ptr(character, O.Char_Movement) : nullptr;
  if (!movement) return kIdleSpeed + 1.0;  // unknown: treated as moving
  const Vec v = ue::At<Vec>(movement, O.Move_Velocity);
  return std::sqrt(v.x * v.x + v.y * v.y);
}

// Two first-order low-pass stages in a row: they take quick oscillations out much better than one
// stage with the same lag.
struct Filter2 {
  double mid = 0.0, out = 0.0;
  void Reset(double v) { mid = out = v; }
  void Step(double v, double a) {
    mid += (v - mid) * a;
    out += (mid - out) * a;
  }
};

// Keeps both stages of a position filter within |limit| of |v|, carrying them along. True if it
// had to.
bool Leash(Filter2 (&f)[3], const double (&v)[3], double limit) {
  bool moved = false;
  for (double Filter2::*stage : {&Filter2::mid, &Filter2::out}) {
    double d[3], len2 = 0.0;
    for (int i = 0; i < 3; ++i) {
      d[i] = f[i].*stage - v[i];
      len2 += d[i] * d[i];
    }
    if (len2 <= limit * limit) continue;
    const double scale = limit / std::sqrt(len2);
    for (int i = 0; i < 3; ++i) f[i].*stage = v[i] + d[i] * scale;
    moved = true;
  }
  return moved;
}

// The same for angles, each on its own.
bool LeashAngles(Filter2 (&f)[3], const double (&v)[3], double limit) {
  bool moved = false;
  for (int i = 0; i < 3; ++i) {
    for (double* stage : {&f[i].mid, &f[i].out}) {
      const double clamped = util::Clamp(*stage, v[i] - limit, v[i] + limit);
      moved |= clamped != *stage;
      *stage = clamped;
    }
  }
  return moved;
}

struct State {
  void* character = nullptr;
  void* mesh = nullptr;
  bool have = false;  // the filters hold a steady point
  // The camera's position in the mesh's frame (cm) and its rotation relative to where the player
  // aims (pitch, yaw, roll in degrees).
  Filter2 pos[3], aim[3];
  double lastPos[3] = {}, lastAim[3] = {};
  Rot lastControl{};      // the control rotation
  double lookRate = 0.0;  // how fast the view turns, degrees per second, decaying
  double lastStep = -1.0;
  double settleUntil = 0.0;
  double carried = 0.0;  // how long a large move has been carrying the steady point (game seconds)
  double catchUpUntil = 0.0;
  bool montageHold = false;  // a montage moved the camera: the game's camera until it ends
  float weight = 0.f;        // 0 = the game's camera, 1 = steady
};
State g;
bool g_wanted = false;  // for the log
bool g_reportedNotReady = false;

// What the steadying did over the last few seconds, for the log.
struct Window {
  double start = -1.0;
  double moving = 0.0, game = 0.0, leashed = 0.0;
  int catchUps = 0;
  double lo[6] = {}, hi[6] = {};  // camera offset from the steady point: x, y, z (cm), pitch, yaw, roll
  bool any = false;
  double maxCm = 0.0, maxDeg = 0.0, maxSpeed = 0.0;
};
Window g_window;
int g_summaryLines = 0;
int g_handOverLines = 0;

void LogHandOver(const char* why, double cm, double deg, void* anim) {
  if (g_handOverLines >= kMaxHandOverLines) return;
  if (++g_handOverLines == kMaxHandOverLines) {
    LOG("Camera bob: line limit reached, no more hand-over lines this session");
    return;
  }
  void* montage = anim ? game::CallObject(F.GetCurrentActiveMontage, anim) : nullptr;
  LOG("Camera bob: the game's camera for %s (camera %.1f cm / %.1f deg off the steady point%s%s)", why, cm, deg,
      montage ? ", montage " : "", montage ? ue::ObjectName(montage).c_str() : "");
}

void Describe(void* component, void* mesh) {
  const ue::FName socket = O.Scene_AttachSocket >= 0 ? ue::At<ue::FName>(component, O.Scene_AttachSocket) : ue::FName{};
  LOG("Camera bob: the player camera is attached to %s (%s), socket %s", ue::ObjectName(mesh).c_str(),
      ue::ClassName(mesh).c_str(), ue::NameToString(socket).c_str());
}

void Record(const double (&off)[6], bool leashed, bool moving, bool gameCamera, double speed, double dt) {
  Window& w = g_window;
  if (w.start < 0.0) w.start = util::Now();
  if (moving) w.moving += dt;
  if (gameCamera) w.game += dt;
  if (leashed) w.leashed += dt;
  w.maxSpeed = (std::max)(w.maxSpeed, speed);
  for (int i = 0; i < 6; ++i) {
    w.lo[i] = w.any ? (std::min)(w.lo[i], off[i]) : off[i];
    w.hi[i] = w.any ? (std::max)(w.hi[i], off[i]) : off[i];
  }
  w.any = true;
}

// ---- camera effects, for the log --------------------------------------------------------------

// The camera manager's modifiers, and the camera shakes and camera animations they play: a line
// each time they change, so camera motion that isn't the camera moving on the body can be traced
// to its source.
struct EffectArray {
  const char* array;   // the modifier's array property
  const char* object;  // the element's object property
  const char* label;
  bool byClass;  // shakes are known by their class, camera animations by their asset
};
constexpr EffectArray kEffectArrays[] = {
    {"ActiveShakes", "ShakeInstance", "shake", true},
    {"ActiveAnimations", "Sequence", "camera animation", false},
};

struct ArrayCache {
  void* cls;
  const EffectArray* spec;
  ue::ArrayProp array;
  int32_t objectOffset;
};
std::vector<ArrayCache> g_arrays;

const ArrayCache& CachedArray(void* cls, const EffectArray& spec) {
  for (const ArrayCache& c : g_arrays) {
    if (c.cls == cls && c.spec == &spec) return c;
  }
  ArrayCache c{cls, &spec, ue::FindArray(cls, spec.array), -1};
  if (c.array.Valid() && c.array.innerStruct) c.objectOffset = ue::Offset(c.array.innerStruct, spec.object);
  g_arrays.push_back(c);
  return g_arrays.back();
}

void LogCameraEffects(void* cameraManager, double now) {
  static void* describedManager = nullptr;
  static std::string last = "?";
  static double lastTime = -10.0;
  static int lines = 0;
  if (!cameraManager || O.PCM_ModifierList < 0 || lines >= kMaxEffectLines || now - lastTime < 0.25) return;
  const auto& modifiers = ue::At<ue::TArray<void*>>(cameraManager, O.PCM_ModifierList);
  if (!modifiers.data || modifiers.num < 0 || modifiers.num > 32) return;
  std::string names, effects;
  for (int32_t i = 0; i < modifiers.num; ++i) {
    void* modifier = modifiers.data[i];
    if (!modifier || !ue::IsValid(modifier)) continue;
    void* cls = ue::ClassOf(modifier);
    if (!names.empty()) names += ", ";
    names += ue::ClassName(modifier);
    for (const EffectArray& spec : kEffectArrays) {
      const ArrayCache& c = CachedArray(cls, spec);
      if (!c.array.Valid() || c.objectOffset < 0) continue;
      const auto& items = ue::At<ue::TArray<uint8_t>>(modifier, c.array.offset);
      if (!items.data || items.num < 0 || items.num > 64) continue;
      for (int32_t k = 0; k < items.num; ++k) {
        void* object = ue::Ptr(items.data + static_cast<size_t>(k) * c.array.elemSize, c.objectOffset);
        if (!object || !ue::IsValid(object)) continue;
        if (!effects.empty()) effects += ", ";
        effects += std::string(spec.label) + " " + (spec.byClass ? ue::ClassName(object) : ue::ObjectName(object));
      }
    }
  }
  if (cameraManager != describedManager) {
    describedManager = cameraManager;
    LOG("Camera modifiers: %s", names.empty() ? "none" : names.c_str());
    ++lines;
  }
  if (effects == last) return;
  last = effects;
  lastTime = now;
  if (++lines >= kMaxEffectLines) {
    LOG("Camera effects: line limit reached, no more this session");
    return;
  }
  LOG("Camera effects: %s", effects.empty() ? "none" : effects.c_str());
}

}  // namespace

void BeforeModifiers(void* character, void* controller, void* pov, float deltaTime) {
  const bool wanted = !cfg::S.cameraBob;
  if (wanted != g_wanted) {
    g_wanted = wanted;
    LOG(wanted ? "Camera bob: steadying the camera" : "Camera bob: back to the game's camera bob");
  }
  // With the game's camera bob and nothing left to blend out there's nothing to do; the steady
  // point starts afresh the next time.
  if (!wanted && g.weight <= 0.f) {
    g.have = false;
    return;
  }
  if (!Ready()) {
    if (!g_reportedNotReady) {
      g_reportedNotReady = true;
      LOG("Camera bob: can't steady the camera in this game version (something it needs wasn't found at startup)");
    }
    return;
  }
  void* component = ue::Ptr(character, O.Char_FPCameraActor);
  void* mesh = component ? ue::Ptr(component, O.Scene_AttachParent) : nullptr;
  Quat meshRotation;
  Vec meshPosition;
  if (!mesh || !controller || !WorldTransform(mesh, meshRotation, meshPosition)) return;
  if (mesh != g.mesh || character != g.character) {
    if (mesh != g.mesh) Describe(component, mesh);
    g.mesh = mesh;
    g.character = character;
    g.have = false;
  }

  // Where the camera is on the body (in the frame of the mesh it hangs from, which crouching and
  // stepping don't jolt), and how it's turned from where the player aims.
  Vec& location = ue::At<Vec>(pov, O.POV_Location);
  Rot& rotation = ue::At<Rot>(pov, O.POV_Rotation);
  const Rot control = ue::At<Rot>(controller, O.Ctrl_ControlRotation);
  const Vec local =
      Unrotate(meshRotation, {location.x - meshPosition.x, location.y - meshPosition.y, location.z - meshPosition.z});
  const double pos[3] = {local.x, local.y, local.z};
  const double aim[3] = {Wrap(rotation.pitch - control.pitch), Wrap(rotation.yaw - control.yaw),
                         Wrap(rotation.roll - control.roll)};
  if (!std::isfinite(pos[0] + pos[1] + pos[2] + aim[0] + aim[1] + aim[2])) return;

  // ApplyCameraModifiers can run twice in a frame (for the view target and a pending one): the
  // filters step once.
  const double now = util::Now();
  const bool newFrame = now - g.lastStep > 0.001;
  const double dt = newFrame && g.have ? util::Clamp(static_cast<double>(deltaTime), 0.0, 0.1) : 0.0;
  if (!g.have) {
    for (int i = 0; i < 3; ++i) {
      g.pos[i].Reset(pos[i]);
      g.aim[i].Reset(aim[i]);
      g.lastPos[i] = pos[i];
      g.lastAim[i] = aim[i];
    }
    g.lastControl = control;
    g.lookRate = 0.0;
    g.carried = 0.0;
    g.catchUpUntil = 0.0;
    g.montageHold = false;
    g.have = true;
  }
  if (newFrame) g.lastStep = now;

  // Scripted camera moves are left to the game.
  double devCm2 = 0.0, jumpCm2 = 0.0, devDeg = 0.0, jumpDeg = 0.0;
  for (int i = 0; i < 3; ++i) {
    devCm2 += (pos[i] - g.pos[i].out) * (pos[i] - g.pos[i].out);
    jumpCm2 += (pos[i] - g.lastPos[i]) * (pos[i] - g.lastPos[i]);
    devDeg = (std::max)(devDeg, std::fabs(aim[i] - g.aim[i].out));
    jumpDeg = (std::max)(jumpDeg, std::fabs(Wrap(aim[i] - g.lastAim[i])));
  }
  const double devCm = std::sqrt(devCm2), jumpCm = std::sqrt(jumpCm2);
  void* anim = O.Mesh_AnimInstance >= 0 ? ue::Ptr(mesh, O.Mesh_AnimInstance) : nullptr;
  const bool montage = anim && game::CallBool(F.IsAnyMontagePlaying, anim, false);
  if (montage && !g.montageHold && (devCm > kMontageCm || devDeg > kMontageDeg)) {
    g.montageHold = true;
    if (wanted) LogHandOver("a montage", devCm, devDeg, anim);
  } else if (!montage && g.montageHold) {
    g.montageHold = false;
    g.settleUntil = now + kSettleSeconds;
  }
  if (newFrame && (jumpCm > kJumpCm || jumpDeg > kJumpDeg)) {
    if (wanted && now >= g.settleUntil) LogHandOver("a sudden camera move", jumpCm, jumpDeg, montage ? anim : nullptr);
    g.settleUntil = now + kSettleSeconds;
  }
  const bool gameCamera = g.montageHold || now < g.settleUntil;
  const float target = wanted && !gameCamera ? 1.f : 0.f;
  if (dt > 0.0) {
    const float step = static_cast<float>(dt / (target > g.weight ? kBlendInSeconds : kBlendOutSeconds));
    g.weight = target > g.weight ? (std::min)(target, g.weight + step) : (std::max)(target, g.weight - step);
    const double turned =
        (std::max)(std::fabs(Wrap(control.pitch - g.lastControl.pitch)), std::fabs(Wrap(control.yaw - g.lastControl.yaw)));
    g.lookRate = (std::max)(turned / dt, g.lookRate * std::exp(-dt / 0.2));
  }
  if (newFrame) g.lastControl = control;

  // Move the steady point: quickly while the game's camera is in use, slowly while steadying.
  const double speed = PlanarSpeed(character);
  double tau = gameCamera || g.weight <= 0.f ? kTauGame : (speed < kIdleSpeed ? kTauIdle : kTauMoving);
  if (now < g.catchUpUntil) tau = (std::min)(tau, kTauCatchUp);
  if (g.lookRate > kLookRate) tau /= 1.0 + (g.lookRate - kLookRate) / kLookRateScale;
  const double a = dt > 0.0 ? 1.0 - std::exp(-dt / tau) : 0.0;
  for (int i = 0; i < 3; ++i) {
    g.pos[i].Step(pos[i], a);
    g.aim[i].Step(aim[i], a);
  }
  const double off[6] = {pos[0] - g.pos[0].out, pos[1] - g.pos[1].out, pos[2] - g.pos[2].out,
                         aim[0] - g.aim[0].out, aim[1] - g.aim[1].out, aim[2] - g.aim[2].out};
  const bool leashed = Leash(g.pos, pos, kMaxCm) | LeashAngles(g.aim, aim, kMaxDeg);
  if (newFrame) {
    g.carried = leashed ? g.carried + dt : 0.0;
    if (g.carried > kCarriedSeconds) {
      if (now >= g.catchUpUntil && wanted) ++g_window.catchUps;
      g.catchUpUntil = now + kCatchUpSeconds;
    }
    memcpy(g.lastPos, pos, sizeof pos);
    memcpy(g.lastAim, aim, sizeof aim);
  }
  if (wanted && dt > 0.0) Record(off, leashed, speed >= kIdleSpeed, gameCamera, speed, dt);
  if (g.weight <= 0.f) return;

  // The camera, moved back to the steady point by the blend weight.
  const double w = g.weight;
  const Vec shift = Rotate(meshRotation, {(pos[0] - g.pos[0].out) * w, (pos[1] - g.pos[1].out) * w,
                                          (pos[2] - g.pos[2].out) * w});
  location.x -= shift.x;
  location.y -= shift.y;
  location.z -= shift.z;
  const double wr = w * util::Clamp((kVerticalFrom - std::fabs(Wrap(rotation.pitch))) / kVerticalFade, 0.0, 1.0);
  const double turn[3] = {(aim[0] - g.aim[0].out) * wr, (aim[1] - g.aim[1].out) * wr, (aim[2] - g.aim[2].out) * wr};
  rotation.pitch -= turn[0];
  rotation.yaw -= turn[1];
  rotation.roll -= turn[2];
  Window& win = g_window;
  win.maxCm = (std::max)(win.maxCm, std::sqrt(shift.x * shift.x + shift.y * shift.y + shift.z * shift.z));
  win.maxDeg =
      (std::max)(win.maxDeg, (std::max)((std::max)(std::fabs(turn[0]), std::fabs(turn[1])), std::fabs(turn[2])));
}

void OnFrame(void* cameraManager, double now) {
  LogCameraEffects(cameraManager, now);
  Window& w = g_window;
  if (w.start < 0.0 || now - w.start < kSummarySeconds) return;
  // What was taken out while the player was moving (standing still isn't summarized).
  if (w.moving > 1.0 && g_summaryLines < kMaxSummaryLines) {
    if (++g_summaryLines == kMaxSummaryLines) {
      LOG("Camera bob: line limit reached, no more summaries this session");
    } else {
      LOG("Camera bob: %.0f s, moving %.1f s at up to %.0f cm/s | camera around the steady point: x %+.1f..%+.1f "
          "y %+.1f..%+.1f z %+.1f..%+.1f cm, pitch %+.1f..%+.1f yaw %+.1f..%+.1f roll %+.1f..%+.1f deg | removed up "
          "to %.1f cm, %.1f deg | at the limit %.1f s (%d large moves), game's camera %.1f s",
          now - w.start, w.moving, w.maxSpeed, w.lo[0], w.hi[0], w.lo[1], w.hi[1], w.lo[2], w.hi[2], w.lo[3], w.hi[3],
          w.lo[4], w.hi[4], w.lo[5], w.hi[5], w.maxCm, w.maxDeg, w.leashed, w.catchUps, w.game);
    }
  }
  w = Window{};
}

}  // namespace bob
