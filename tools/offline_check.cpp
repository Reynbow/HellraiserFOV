// Offline self-test: maps the game exe into this process (without running it) and checks that the
// code heuristics find the vtable slots the mod hooks and calls, that vtable hooking works, and
// that the winmm proxy forwards to the real DLL.
//
// Usage: offline_check.exe <path to Hellraiser-Win64-Shipping.exe> <path to built winmm.dll>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/game.h"
#include "../src/log.h"
#include "../src/scan.h"
#include "../src/ue.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
  if (!ok) ++g_failures;
}

void ExpectSlot(const char* name, int got, int want) {
  char buf[160];
  snprintf(buf, sizeof buf, "%-40s = %d (expected %d)", name, got, want);
  Check(got == want, buf);
}

void __fastcall DummyHook() {}

// The start of the vtable holding |slotAddr|: the entries before it are code pointers too. (The
// running mod gets its vtables from live objects; this is only for the offline check.)
void** VtableStart(uintptr_t slotAddr) {
  uintptr_t a = slotAddr;
  while (scan::InText(*reinterpret_cast<const uintptr_t*>(a - 8))) a -= 8;
  return reinterpret_cast<void**>(a);
}

// Vtables in the read-only data that contain the function |fn|.
std::vector<void**> VtablesWith(uintptr_t fn) {
  std::vector<void**> out;
  const scan::Range& r = scan::Img().rdata;
  for (uintptr_t p = r.begin; p + 8 <= r.end; p += 8) {
    if (*reinterpret_cast<const uintptr_t*>(p) != fn) continue;
    void** vt = VtableStart(p);
    if (std::find(out.begin(), out.end(), vt) == out.end()) out.push_back(vt);
  }
  return out;
}

unsigned long long Rva(uintptr_t p) { return static_cast<unsigned long long>(p - scan::Img().base); }

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    printf("usage: offline_check <Hellraiser-Win64-Shipping.exe> <winmm.dll>\n");
    return 2;
  }
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  logx::Open((std::wstring(temp) + L"HellraiserFOV_offline_check.log").c_str());

  HMODULE game = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
  if (!game) {
    printf("couldn't map %ls (error %lu)\n", argv[1], GetLastError());
    return 1;
  }
  scan::Image image;
  if (!scan::Describe(game, image)) {
    printf("not a 64-bit PE image\n");
    return 1;
  }
  scan::SetImage(image);
  printf("Mapped game at %p, image size 0x%x, timestamp %08X\n", static_cast<void*>(game), image.size, image.timeStamp);

  printf("\nUGameViewportClient::PostRender:\n");
  const ULONGLONG t0 = GetTickCount64();
  const std::vector<uintptr_t> drawTransition = scan::FunctionsReferencing(L"LoadingMessage");
  printf("  string search took %llu ms\n", GetTickCount64() - t0);
  Check(drawTransition.size() == 1, "one function draws the \"LoadingMessage\" text (DrawTransition)");
  void** viewportVtable = nullptr;
  if (!drawTransition.empty()) {
    printf("  DrawTransition at RVA 0x%llx\n", Rva(drawTransition[0]));
    const std::vector<void**> vtables = VtablesWith(drawTransition[0]);
    Check(!vtables.empty(), "found vtables containing DrawTransition");
    for (void** vt : vtables) {
      char name[64];
      snprintf(name, sizeof name, "PostRender slot (vtable RVA 0x%llx)", Rva(reinterpret_cast<uintptr_t>(vt)));
      ExpectSlot(name, game::FindPostRenderSlot(vt), 111);
      if (!viewportVtable) viewportVtable = vt;
    }
  }

  printf("\nAPlayerCameraManager::ApplyCameraModifiers:\n");
  // Two functions name the style: APlayerCameraManager::UpdateViewTarget and a pawn function that
  // asks whether the camera is in free-cam. Only camera manager vtables have UpdateViewTarget.
  const std::vector<uintptr_t> styleUsers = scan::FunctionsReferencing(L"FreeCam_Default");
  Check(!styleUsers.empty(), "found functions naming the \"FreeCam_Default\" camera style");
  int cameraVtables = 0, otherVtables = 0;
  void** cameraVtable = nullptr;
  for (uintptr_t fn : styleUsers) {
    for (void** vt : VtablesWith(fn)) {
      const int slot = game::FindApplyCameraModifiersSlot(vt, 0x10);
      if (slot < 0) {
        ++otherVtables;
        continue;
      }
      ++cameraVtables;
      char name[80];
      snprintf(name, sizeof name, "ApplyCameraModifiers slot (vtable RVA 0x%llx)", Rva(reinterpret_cast<uintptr_t>(vt)));
      ExpectSlot(name, slot, 253);
      if (!cameraVtable) cameraVtable = vt;
    }
  }
  printf("  (%d other vtables name the style without being camera managers)\n", otherVtables);
  Check(cameraVtables >= 2, "found camera manager vtables (APlayerCameraManager and the game's)");

  printf("\nUObject::ProcessEvent:\n");
  // Found in a plain UObject vtable (the mod uses UObject's own). Actors override the slot with
  // AActor::ProcessEvent, which checks the actor before calling the original.
  ExpectSlot("ProcessEvent slot (viewport vtable)", ue::FindProcessEventSlot(viewportVtable), 76);

  printf("\nName lookups (on a hand-made name table):\n");
  {
    // Entries: a 16-bit header (bit 0 wide, bits 6-15 length) and the characters, 2-byte aligned. The
    // engine keeps the spelling registered first, so a class called Canvas can be stored as "canvas".
    alignas(8) static uint8_t block[64] = {};
    const auto put = [](uint8_t* at, const char* s, bool wide) {
      const auto n = static_cast<uint16_t>(strlen(s));
      const uint16_t header = static_cast<uint16_t>(n << 6 | (wide ? 1 : 0));
      memcpy(at, &header, 2);
      for (uint16_t i = 0; i < n; ++i) {
        if (wide) {
          const wchar_t c = static_cast<unsigned char>(s[i]);
          memcpy(at + 2 + i * 2, &c, 2);
        } else {
          at[2 + i] = static_cast<uint8_t>(s[i]);
        }
      }
    };
    put(block, "canvas", false);       // entry id 0
    put(block + 16, "CANVAS", true);   // entry id 8 (byte offset 16 / 2)
    static uint8_t* blocks[1] = {block};
    ue::UseNameBlocksForTest(blocks);
    Check(ue::NameEquals({0, 0}, "Canvas"), "\"Canvas\" matches the stored spelling \"canvas\"");
    Check(ue::NameEquals({8, 0}, "Canvas"), "\"Canvas\" matches a wide \"CANVAS\" entry");
    Check(!ue::NameEquals({0, 0}, "Canva") && !ue::NameEquals({0, 0}, "Canvass"), "other names don't match");
    Check(ue::NameEquals({0, 1}, "Canvas_0") && !ue::NameEquals({0, 1}, "Canvas"), "numbered names compare with their number");
    ue::UseNameBlocksForTest(nullptr);
  }

  printf("\nVtable hooking (on this process's private copy of the image):\n");
  if (cameraVtable) {
    void* fakeObject = static_cast<void*>(cameraVtable);  // an "object" whose vptr is the vtable
    void* const before = cameraVtable[253];
    void* original = nullptr;
    Check(ue::HookSlot(&fakeObject, 253, reinterpret_cast<void*>(&DummyHook), &original), "HookSlot wrote the slot");
    Check(original == before, "HookSlot returned the previous function");
    Check(cameraVtable[253] == reinterpret_cast<void*>(&DummyHook), "slot 253 now points at the hook");
    void* again = nullptr;
    Check(ue::HookSlot(&fakeObject, 253, reinterpret_cast<void*>(&DummyHook), &again) && again == nullptr,
          "hooking again is a no-op");
  } else {
    Check(false, "no camera manager vtable to hook");
  }

  printf("\nwinmm proxy:\n");
  HMODULE proxy = LoadLibraryW(argv[2]);
  Check(proxy != nullptr, "proxy DLL loads");
  if (proxy) {
    using TimeGetTime = DWORD(WINAPI*)();
    auto fn = reinterpret_cast<TimeGetTime>(GetProcAddress(proxy, "timeGetTime"));
    Check(fn != nullptr, "timeGetTime export resolves");
    HMODULE real = GetModuleHandleW(L"C:\\Windows\\System32\\winmm.dll");
    Check(real != nullptr && real != proxy, "forwarder loaded the real System32 winmm.dll");
    if (fn) {
      const DWORD a = fn();
      Sleep(20);
      const DWORD b = fn();
      Check(b - a >= 15 && b - a < 1000, "timeGetTime works through the proxy");
    }
    Check(GetProcAddress(proxy, "PlaySoundW") != nullptr, "PlaySoundW export resolves");
  }

  printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
