// TrueFOV for Clive Barker's Hellraiser: Revival: gameplay-only field of view, and the camera bob
// taken out if you like.
//
// Built as winmm.dll: the game imports winmm, so Windows loads this file from the game's
// Binaries\Win64 folder. Every winmm export forwards to the real system DLL (proxy_exports.h).

#include <windows.h>

#include <cwchar>
#include <string>

#include "bob.h"
#include "canvas.h"
#include "config.h"
#include "fov.h"
#include "game.h"
#include "log.h"
#include "proxy_exports.h"
#include "scan.h"
#include "ue.h"
#include "ui.h"
#include "util.h"
#include "version.h"

namespace {

constexpr char kVersion[] = HFOV_VERSION_STRING;
// How long to wait for the engine to start up before giving up (it takes a few seconds).
constexpr double kStartupTimeout = 300.0;

HMODULE g_self = nullptr;
long g_frameFaults = 0;

using PostRenderFn = void(__fastcall*)(void* self, void* canvas);
PostRenderFn g_originalPostRender = nullptr;

void Frame(void* viewportClient, void* canvas) {
  const double now = util::Now();
  fov::OnFrame(viewportClient, now);
  bob::OnFrame(fov::St.cameraManager, now);
  ui::OnFrame(viewportClient, canvas, now);
  cfg::SaveIfDirty(now);
}

void SafeFrame(void* viewportClient, void* canvas) {
  __try {
    Frame(viewportClient, canvas);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    canvas::EmergencyRestore();  // put back a borrowed font size
    if (InterlockedIncrement(&g_frameFaults) <= 5) LOG("Recovered from a fault in the frame hook (0x%08lX)", GetExceptionCode());
  }
}

void __fastcall HookPostRender(void* self, void* canvas) {
  g_originalPostRender(self, canvas);
  if (canvas) SafeFrame(self, canvas);
}

// The engine tables are read while the engine is still filling them in on its own thread, so a
// read can land on an object that is half constructed: retry later instead of crashing.
bool SafeInit(std::string& error) {
  __try {
    return ue::Init(error);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* SafeEngine() {
  __try {
    return game::Engine();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

bool SafeResolve(std::string& error) {
  __try {
    return game::Resolve(error);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* SafeGameViewport() {
  __try {
    return game::GameViewport();
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM param) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != GetCurrentProcessId()) return TRUE;
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
  *reinterpret_cast<bool*>(param) = true;
  return FALSE;
}

// The engine opens its main window once its core has started up (modules loaded, classes
// registered); nothing is read from the game before then.
bool GameWindowExists() {
  bool found = false;
  EnumWindows(FindGameWindowProc, reinterpret_cast<LPARAM>(&found));
  return found;
}

std::wstring ModuleDirectory(HMODULE module) {
  wchar_t path[MAX_PATH * 2] = {};
  GetModuleFileNameW(module, path, MAX_PATH * 2);
  std::wstring dir = path;
  return dir.substr(0, dir.find_last_of(L"\\/") + 1);
}

DWORD WINAPI InitThread(void*) {
  const std::wstring dir = ModuleDirectory(g_self);
  logx::Open((dir + L"HellraiserFOV.log").c_str());  // one log per session: each launch starts it fresh
  LOG("TrueFOV %s for Hellraiser: Revival", kVersion);
  cfg::Load(dir + L"HellraiserFOV.ini");

  scan::Image image;
  if (!scan::Describe(GetModuleHandleW(nullptr), image)) {
    LOG("Can't start: the game executable's layout isn't recognised. The game runs normally without the mod.");
    return 0;
  }
  scan::SetImage(image);
  LOG("Game executable: timestamp %08X, %u MB", image.timeStamp, image.size >> 20);

  const double start = util::Now();
  while (!GameWindowExists()) {
    if (util::Now() - start > kStartupTimeout) {
      LOG("Can't start: the game window never appeared. The game runs normally without the mod.");
      return 0;
    }
    Sleep(250);
  }
  LOG("Game window open after %.1f s", util::Now() - start);
  Sleep(1000);

  // Then the engine's object and name tables, the engine object, and everything the mod uses.
  std::string error, lastError;
  int attempts = 0;
  for (;;) {
    error = "fault while reading the engine tables";
    ++attempts;
    if (SafeInit(error)) break;
    if (error != lastError) {
      LOG("Waiting for the engine (attempt %d): %s", attempts, error.c_str());
      lastError = error;
    }
    if (util::Now() - start > kStartupTimeout) {
      LOG("Can't start: %s. The game runs normally without the mod.", error.c_str());
      return 0;
    }
    Sleep(250);
  }
  while (!SafeEngine()) {
    if (util::Now() - start > kStartupTimeout) {
      LOG("Can't start: the engine object never appeared. The game runs normally without the mod.");
      return 0;
    }
    Sleep(250);
  }
  if (!SafeResolve(error)) {
    LOG("Can't start: %s. The game runs normally without the mod.", error.empty() ? "fault while reading the game" : error.c_str());
    return 0;
  }
  LOG("Engine ready after %.1f s", util::Now() - start);

  // The engine creates its viewport a few seconds into startup; hook its PostRender to get a
  // per-frame callback on the game thread with a canvas to draw on.
  for (;;) {
    if (void* viewport = SafeGameViewport()) {
      // HookSlot stores the original before the slot changes, so the hook can call it at once.
      if (ue::HookSlot(viewport, game::S.PostRender, reinterpret_cast<void*>(&HookPostRender),
                       reinterpret_cast<void**>(&g_originalPostRender)) &&
          g_originalPostRender) {
        LOG("Ready (viewport %s)", ue::ClassName(viewport).c_str());
      } else {
        LOG("Can't start: couldn't hook UGameViewportClient::PostRender");
      }
      return 0;
    }
    Sleep(100);
  }
}

bool IsGameProcess() {
  wchar_t path[MAX_PATH * 2] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
  const wchar_t* name = wcsrchr(path, L'\\');
  return _wcsicmp(name ? name + 1 : path, L"Hellraiser-Win64-Shipping.exe") == 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(module);
    g_self = module;
    if (IsGameProcess()) {
      if (HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(thread);
    }
  }
  return TRUE;
}
