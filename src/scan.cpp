#include "scan.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

namespace scan {
namespace {

Image g_img;
const RUNTIME_FUNCTION* g_funcs = nullptr;
size_t g_numFuncs = 0;

constexpr uint8_t kChainInfo = 4;  // UNW_FLAG_CHAININFO

const RUNTIME_FUNCTION* Lookup(uint32_t rva) {
  if (!g_funcs || !g_numFuncs) return nullptr;
  size_t lo = 0, hi = g_numFuncs;
  while (lo < hi) {  // first entry starting after rva
    const size_t mid = (lo + hi) / 2;
    if (g_funcs[mid].BeginAddress <= rva) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return nullptr;
  const RUNTIME_FUNCTION* rf = &g_funcs[lo - 1];
  return rva < rf->EndAddress ? rf : nullptr;
}

// Follows indirect entries and chained unwind info up to the function's primary entry.
const RUNTIME_FUNCTION* Primary(const RUNTIME_FUNCTION* rf) {
  for (int guard = 0; rf && guard < 32; ++guard) {
    if (rf->UnwindData & 1) {  // RUNTIME_FUNCTION_INDIRECT: points at another entry
      rf = reinterpret_cast<const RUNTIME_FUNCTION*>(g_img.base + (rf->UnwindData & ~1u));
      continue;
    }
    const auto* info = reinterpret_cast<const uint8_t*>(g_img.base + rf->UnwindData);
    if (!((info[0] >> 3) & kChainInfo)) return rf;
    const uint8_t codes = info[2];
    rf = reinterpret_cast<const RUNTIME_FUNCTION*>(info + 4 + ((codes + 1) & ~1) * 2);
  }
  return nullptr;
}

bool Matches(const uint8_t* at, const uint8_t* pattern, const char* mask, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (mask[i] == 'x' && at[i] != pattern[i]) return false;
  }
  return true;
}

struct Region {
  uintptr_t begin = 0, end = 0;
  bool readable = false;
};
thread_local Region t_regions[16];
thread_local unsigned t_nextRegion = 0;

bool Query(uintptr_t p, Region& out) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof mbi)) return false;
  out.begin = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
  out.end = out.begin + mbi.RegionSize;
  const DWORD access = mbi.Protect & 0xFF;
  out.readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && access != 0 && access != PAGE_NOACCESS &&
                 access != PAGE_EXECUTE;
  return true;
}

}  // namespace

bool Readable(uintptr_t p, size_t n) {
  if (p < 0x10000 || p + n < p || p + n > 0x00007FFFFFFFFFFFull) return false;
  const uintptr_t end = p + n;
  while (p < end) {
    const Region* hit = nullptr;
    for (const Region& r : t_regions) {
      if (p >= r.begin && p < r.end) {
        hit = &r;
        break;
      }
    }
    if (!hit) {
      Region fresh;
      if (!Query(p, fresh)) return false;
      Region& slot = t_regions[t_nextRegion++ % (sizeof t_regions / sizeof t_regions[0])];
      slot = fresh;
      hit = &slot;
    }
    if (!hit->readable) return false;
    p = hit->end;
  }
  return true;
}

void ForgetRegions() {
  for (Region& r : t_regions) r = {};
}

bool Describe(HMODULE module, Image& out) {
  const auto base = reinterpret_cast<uintptr_t>(module);
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
  out = {};
  out.base = base;
  out.size = nt->OptionalHeader.SizeOfImage;
  out.timeStamp = nt->FileHeader.TimeDateStamp;
  const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
    char name[9] = {};
    memcpy(name, sec->Name, 8);
    const Range r{base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
    if (!strcmp(name, ".text")) out.text = r;
    if (!strcmp(name, ".rdata")) out.rdata = r;
    if (!strcmp(name, ".data")) out.data = r;
  }
  return out.text.begin && out.rdata.begin && out.data.begin;
}

void SetImage(const Image& image) {
  g_img = image;
  const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.base);
  const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
  g_funcs = dir.VirtualAddress ? reinterpret_cast<const RUNTIME_FUNCTION*>(image.base + dir.VirtualAddress) : nullptr;
  g_numFuncs = dir.Size / sizeof(RUNTIME_FUNCTION);
}

const Image& Img() { return g_img; }

std::vector<uintptr_t> FindWide(const wchar_t* s) {
  std::vector<uintptr_t> out;
  const size_t bytes = (wcslen(s) + 1) * sizeof(wchar_t);
  const auto* p = reinterpret_cast<const uint8_t*>(g_img.rdata.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(g_img.rdata.end) - bytes;
  const auto* needle = reinterpret_cast<const uint8_t*>(s);
  for (; p <= end; p += 2) {  // UTF-16 literals are 2-byte aligned
    if (p[0] == needle[0] && p[1] == needle[1] && memcmp(p, needle, bytes) == 0) {
      out.push_back(reinterpret_cast<uintptr_t>(p));
    }
  }
  return out;
}

std::vector<uintptr_t> LeaXrefs(const std::vector<uintptr_t>& targets) {
  std::vector<uintptr_t> out;
  if (targets.empty()) return out;
  const auto lo = *std::min_element(targets.begin(), targets.end());
  const auto hi = *std::max_element(targets.begin(), targets.end());
  const auto* p = reinterpret_cast<const uint8_t*>(g_img.text.begin);
  const auto* end = reinterpret_cast<const uint8_t*>(g_img.text.end) - 7;
  for (; p < end; ++p) {
    // REX.W (48) or REX.WR (4C), opcode 8D, ModRM mod=00 rm=101 (RIP-relative).
    if (p[1] != 0x8D || (p[0] != 0x48 && p[0] != 0x4C) || (p[2] & 0xC7) != 0x05) continue;
    int32_t disp;
    memcpy(&disp, p + 3, 4);
    const uintptr_t target = reinterpret_cast<uintptr_t>(p) + 7 + static_cast<intptr_t>(disp);
    if (target < lo || target > hi) continue;
    if (std::find(targets.begin(), targets.end(), target) != targets.end()) {
      out.push_back(reinterpret_cast<uintptr_t>(p));
    }
  }
  return out;
}

std::vector<uintptr_t> FunctionsReferencing(const wchar_t* s) {
  std::vector<uintptr_t> fns;
  for (uintptr_t ref : LeaXrefs(FindWide(s))) {
    const uintptr_t fn = FunctionStart(ref);
    if (fn && std::find(fns.begin(), fns.end(), fn) == fns.end()) fns.push_back(fn);
  }
  return fns;
}

uintptr_t FunctionStart(uintptr_t addr) {
  if (!g_img.text.Contains(addr)) return 0;
  const RUNTIME_FUNCTION* rf = Primary(Lookup(static_cast<uint32_t>(addr - g_img.base)));
  return rf ? g_img.base + rf->BeginAddress : 0;
}

std::vector<Range> FunctionChunks(uintptr_t fn) {
  std::vector<Range> out;
  if (!g_img.text.Contains(fn)) return out;
  const RUNTIME_FUNCTION* first = Lookup(static_cast<uint32_t>(fn - g_img.base));
  if (!first || g_img.base + first->BeginAddress != fn) return out;
  out.push_back({fn, g_img.base + first->EndAddress});
  // Out-of-line chunks follow their function in the table; stop at the first unrelated entry.
  for (const RUNTIME_FUNCTION* rf = first + 1; rf < g_funcs + g_numFuncs && rf - first <= 64; ++rf) {
    const RUNTIME_FUNCTION* primary = Primary(rf);
    if (!primary || g_img.base + primary->BeginAddress != fn) break;
    out.push_back({g_img.base + rf->BeginAddress, g_img.base + rf->EndAddress});
  }
  return out;
}

std::vector<uintptr_t> FindInFunction(uintptr_t fn, const uint8_t* pattern, const char* mask) {
  std::vector<uintptr_t> out;
  const size_t n = strlen(mask);
  for (const Range& r : FunctionChunks(fn)) {
    if (r.end - r.begin < n) continue;
    for (uintptr_t p = r.begin; p + n <= r.end; ++p) {
      if (Matches(reinterpret_cast<const uint8_t*>(p), pattern, mask, n)) out.push_back(p);
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

int VirtualThunkSlot(uintptr_t fn) {
  if (!g_img.text.Contains(fn)) return -1;
  const auto* p = reinterpret_cast<const uint8_t*>(fn);
  if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x01) return -1;  // mov rax, [rcx]
  p += 3;
  if (p[0] == 0x48) ++p;  // optional REX.W on the jump
  if (p[0] != 0xFF) return -1;
  int32_t disp = -1;
  if (p[1] == 0xA0) {  // jmp [rax + disp32]
    memcpy(&disp, p + 2, 4);
  } else if (p[1] == 0x60) {  // jmp [rax + disp8]
    disp = static_cast<int8_t>(p[2]);
  } else if (p[1] == 0x20) {  // jmp [rax]
    disp = 0;
  }
  return disp >= 0 && disp % 8 == 0 ? disp / 8 : -1;
}

}  // namespace scan
