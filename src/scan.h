#pragma once

// Reading the game's own code: section bounds, string references and function boundaries. Used
// to find the few engine functions that reflection doesn't describe (vtable slots), so nothing
// is hard-coded to one build of the game.

#include <windows.h>

#include <cstdint>
#include <vector>

namespace scan {

struct Range {
  uintptr_t begin = 0, end = 0;
  bool Contains(uintptr_t p) const { return p >= begin && p < end; }
};

struct Image {
  uintptr_t base = 0;
  uint32_t size = 0;
  uint32_t timeStamp = 0;
  Range text, rdata, data;  // data includes the zero-initialised tail (its virtual size)
};

// Describes a mapped PE image (the game exe, or a copy mapped by the offline check).
bool Describe(HMODULE module, Image& out);
// The image every lookup below works on.
void SetImage(const Image& image);
const Image& Img();

inline bool InText(uintptr_t p) { return Img().text.Contains(p); }

// True if [p, p + n) is committed, readable memory that isn't a guard page (VirtualQuery, with a
// small per-thread cache). Lets the startup scans check a candidate pointer before reading it,
// instead of reading and catching the fault.
bool Readable(uintptr_t p, size_t n);
// Forgets the cached memory regions (call before a new round of scanning).
void ForgetRegions();

// Addresses of a UTF-16 string (with its terminator) in the read-only data.
std::vector<uintptr_t> FindWide(const wchar_t* s);
// Every `lea r64, [rip + target]` in the code, for any of |targets|.
std::vector<uintptr_t> LeaXrefs(const std::vector<uintptr_t>& targets);
// The functions (primary entry points) that reference the UTF-16 string |s|.
std::vector<uintptr_t> FunctionsReferencing(const wchar_t* s);

// The primary entry point of the function containing |addr|: follows chained unwind info, so a
// separated chunk of a function (a switch case moved out of line) leads back to its start.
uintptr_t FunctionStart(uintptr_t addr);
// The code ranges of a function: its primary entry and the chunks chained to it.
std::vector<Range> FunctionChunks(uintptr_t fn);

// Byte pattern search in a function's chunks. |mask| has 'x' for bytes that must match and '?'
// for wildcards. Returns the addresses of the matches, in address order.
std::vector<uintptr_t> FindInFunction(uintptr_t fn, const uint8_t* pattern, const char* mask);

// If |fn| is a thunk that jumps through slot N of its first argument's vtable
// (`mov rax, [rcx]; jmp [rax + N*8]`), returns N; otherwise -1.
int VirtualThunkSlot(uintptr_t fn);

}  // namespace scan
