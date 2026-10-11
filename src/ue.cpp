#include "ue.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"
#include "scan.h"

namespace ue {

Layout L;

namespace {

constexpr uint32_t kNameMaxBlocks = 8192;  // FNameMaxBlocks
constexpr int32_t kChunkElements = 64 * 1024;  // FChunkedFixedUObjectArray::NumElementsPerChunk

uint8_t** g_nameBlocks = nullptr;  // FNameEntryAllocator::Blocks
uintptr_t g_objArray = 0;          // FChunkedFixedUObjectArray: Objects, PreAllocated, Max, Num, ...
int32_t g_cdoOffset = -1;          // UClass::ClassDefaultObject
int g_peSlot = -1;
void* g_classClass = nullptr;      // the UClass "Class"

// Reads only memory that VirtualQuery says is readable; the exception handler is a backstop for
// memory freed between the check and the read.
bool SafeRead(const void* p, void* out, size_t n) {
  if (!scan::Readable(reinterpret_cast<uintptr_t>(p), n)) return false;
  __try {
    memcpy(out, p, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

template <class T>
bool Read(uintptr_t p, T& out) {
  return SafeRead(reinterpret_cast<const void*>(p), &out, sizeof(T));
}

bool Plausible(uintptr_t p) { return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull; }

// ---- name table ----

const uint8_t* NameEntry(uint32_t id) {
  const uint32_t block = id >> 16;
  if (!g_nameBlocks || block >= kNameMaxBlocks) return nullptr;
  const uint8_t* b = g_nameBlocks[block];
  return b ? b + (id & 0xFFFF) * 2 : nullptr;  // entries are 2-byte aligned
}

// Block 0 of the name table starts with the engine's first hard-coded names.
bool LooksLikeFirstNameBlock(uintptr_t block) {
  uint8_t bytes[20];
  if (!Plausible(block) || !SafeRead(reinterpret_cast<const void*>(block), bytes, sizeof bytes)) return false;
  const uint16_t h0 = static_cast<uint16_t>(bytes[0] | bytes[1] << 8);
  const uint16_t h1 = static_cast<uint16_t>(bytes[6] | bytes[7] << 8);
  return !(h0 & 1) && (h0 >> 6) == 4 && memcmp(bytes + 2, "None", 4) == 0 && !(h1 & 1) && (h1 >> 6) == 12 &&
         memcmp(bytes + 8, "ByteProp", 8) == 0;
}

// The name allocator: { FRWLock Lock; uint32 CurrentBlock; uint32 CurrentByteCursor; uint8* Blocks[8192]; }.
// |p| is the candidate address of Blocks.
bool CheckNameBlocks(uintptr_t p) {
  uintptr_t first = 0;
  uint32_t current = 0, cursor = 0;
  if (!Read(p, first) || !LooksLikeFirstNameBlock(first)) return false;
  if (!Read(p - 8, current) || !Read(p - 4, cursor)) return false;
  if (current >= kNameMaxBlocks || cursor > 0x20000) return false;
  uintptr_t last = 0, after = 0;
  if (!Read(p + current * 8ull, last) || !Plausible(last)) return false;
  if (current + 1 < kNameMaxBlocks && (!Read(p + (current + 1) * 8ull, after) || after != 0)) return false;
  return true;
}

bool FindNameBlocks() {
  if (g_nameBlocks) return true;
  const scan::Range& d = scan::Img().data;
  for (uintptr_t p = (d.begin + 0xF) & ~7ull; p + 8 <= d.end; p += 8) {
    uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
    if (!Plausible(v) || (v & 1)) continue;
    if (CheckNameBlocks(p)) {
      g_nameBlocks = reinterpret_cast<uint8_t**>(p);
      LOG("Name table: blocks at %p (exe+0x%llx)", reinterpret_cast<void*>(p),
          static_cast<unsigned long long>(p - scan::Img().base));
      return true;
    }
  }
  return false;
}

// ---- object array ----

int32_t ObjNum() { return g_objArray ? *reinterpret_cast<const int32_t*>(g_objArray + 0x14) : 0; }

void* RawObjectAt(int32_t i, int32_t itemSize) {
  uint8_t* const* chunks = *reinterpret_cast<uint8_t* const* const*>(g_objArray);
  const uint8_t* chunk = chunks ? chunks[i / kChunkElements] : nullptr;
  return chunk ? *reinterpret_cast<void* const*>(chunk + static_cast<size_t>(i % kChunkElements) * itemSize) : nullptr;
}

int32_t InternalIndexOf(const void* object) {
  int32_t index = -1;
  return Read(reinterpret_cast<uintptr_t>(object) + 0x0C, index) ? index : -1;
}

// FChunkedFixedUObjectArray { FUObjectItem** Objects; FUObjectItem* PreAllocatedObjects; int32 MaxElements;
// int32 NumElements; int32 MaxChunks; int32 NumChunks; } with object 0 at index 0.
bool CheckObjectArray(uintptr_t p) {
  uintptr_t objects = 0;
  int32_t v[4] = {};
  if (!Read(p, objects) || !Plausible(objects)) return false;
  if (!SafeRead(reinterpret_cast<const void*>(p + 0x10), v, sizeof v)) return false;
  const int32_t max = v[0], num = v[1], maxChunks = v[2], numChunks = v[3];
  if (num < 0x1000 || num > max || max > 0x10000000) return false;
  if (maxChunks != (max + kChunkElements - 1) / kChunkElements) return false;
  if (numChunks < (num + kChunkElements - 1) / kChunkElements || numChunks > maxChunks) return false;
  uintptr_t chunk0 = 0, obj0 = 0;
  if (!Read(objects, chunk0) || !Plausible(chunk0) || !Read(chunk0, obj0) || !Plausible(obj0)) return false;
  return InternalIndexOf(reinterpret_cast<void*>(obj0)) == 0;
}

// The size of FUObjectItem: the stride at which each object's InternalIndex matches its slot.
int32_t DetectItemSize() {
  uint8_t* const* chunks = *reinterpret_cast<uint8_t* const* const*>(g_objArray);
  const auto chunk0 = reinterpret_cast<uintptr_t>(chunks[0]);
  const int32_t n = (std::min)(ObjNum(), 4096);
  for (int32_t size : {24, 16, 32, 40, 48}) {
    int32_t present = 0;
    bool ok = true;
    for (int32_t i = 1; i < n && ok; ++i) {
      uintptr_t obj = 0;
      if (!Read(chunk0 + static_cast<uintptr_t>(i) * size, obj)) {
        ok = false;
        break;
      }
      if (!obj) continue;
      ++present;
      ok = Plausible(obj) && InternalIndexOf(reinterpret_cast<void*>(obj)) == i;
    }
    if (ok && present > n / 2) return size;
  }
  return 0;
}

bool FindObjectArray() {
  if (g_objArray) return true;
  const scan::Range& d = scan::Img().data;
  for (uintptr_t p = (d.begin + 7) & ~7ull; p + 0x20 <= d.end; p += 8) {
    const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
    if (!Plausible(v) || (v & 7)) continue;
    if (!CheckObjectArray(p)) continue;
    g_objArray = p;
    const int32_t itemSize = DetectItemSize();
    if (!itemSize) {
      g_objArray = 0;
      continue;
    }
    L.itemSize = itemSize;
    LOG("Object array at %p (exe+0x%llx): %d objects, %d-byte items", reinterpret_cast<void*>(p),
        static_cast<unsigned long long>(p - scan::Img().base), ObjNum(), itemSize);
    return true;
  }
  return false;
}

// ---- layout calibration ----

bool SafeNameEquals(uintptr_t nameAddr, const char* text) {
  FName name{};
  if (!Read(nameAddr, name)) return false;
  __try {
    return NameEquals(name, text);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void* FindClassClass() {
  const int32_t n = ObjNum();
  for (int32_t i = 0; i < n; ++i) {
    void* obj = ObjectAt(i);
    if (obj && Ptr(obj, L.objClass) == obj && NameEquals(At<FName>(obj, L.objName), "Class")) return obj;
  }
  return nullptr;
}

int32_t PropertiesSize(void* ustruct) { return At<int32_t>(ustruct, L.structSize); }

// Field layouts, checked against FVector (X, Y, Z: doubles at 0, 8 and 16) and AActor.
bool CalibrateStructs(std::string& error) {
  void* objectClass = FindClass("Object");
  void* actorClass = FindClass("Actor");
  void* structClass = FindClass("Struct");
  void* functionClass = FindClass("Function");
  void* vector = FindStruct("Vector");
  if (!objectClass || !actorClass || !structClass || !functionClass || !vector) {
    error = "core classes not registered yet";
    return false;
  }
  const auto base = [](void* p) { return reinterpret_cast<uintptr_t>(p); };
  // The engine's usual offset if it passes |test|, else the first offset in [lo, hi) that does.
  const auto pick = [](int32_t usual, int32_t lo, int32_t hi, int32_t step, auto test) {
    if (test(usual)) return usual;
    for (int32_t o = lo; o < hi; o += step) {
      if (o != usual && test(o)) return o;
    }
    return -1;
  };

  // UStruct::SuperStruct: AActor derives straight from UObject.
  L.structSuper = pick(0x40, 0x28, 0x90, 8, [&](int32_t o) { return Ptr(actorClass, o) == objectClass; });
  // UStruct::ChildProperties: FVector's first property is "X", named at FField+0x20 (or +0x28 in
  // engine versions with a 16-byte FField owner).
  int32_t nameOff = -1;
  L.structProps = pick(0x50, 0x28, 0x90, 8, [&](int32_t o) {
    const uintptr_t f = base(Ptr(vector, o));
    if (!Plausible(f)) return false;
    for (int32_t n : {0x20, 0x28}) {
      if (SafeNameEquals(f + n, "X")) {
        nameOff = n;
        return true;
      }
    }
    return false;
  });
  if (nameOff > 0) L.ffName = nameOff;
  // UStruct::Children: AActor's functions. The first may be a delegate signature, whose class
  // (DelegateFunction, SparseDelegateFunction) derives from Function.
  const auto isFunction = [&](uintptr_t f) {
    uintptr_t cls = 0;
    if (!Plausible(f) || !Read(f + L.objClass, cls)) return false;
    for (int depth = 0; depth < 4 && Plausible(cls); ++depth) {
      if (cls == base(functionClass)) return true;
      if (L.structSuper < 0 || !Read(cls + L.structSuper, cls)) return false;
    }
    return false;
  };
  L.structChildren = pick(0x48, 0x28, 0x90, 8, [&](int32_t o) {
    return o != L.structSuper && o != L.structProps && isFunction(base(Ptr(actorClass, o)));
  });
  // UStruct::PropertiesSize then MinAlignment (16 bits in Unreal Engine 5.6, sharing its word with
  // other data): 24 and 8 for FVector (three doubles); 16 is accepted too.
  L.structSize = pick(0x58, 0x50, 0x90, 4, [&](int32_t o) {
    const int16_t align = At<int16_t>(vector, o + 4);
    return At<int32_t>(vector, o) == 24 && (align == 8 || align == 16);
  });
  if (L.structSuper < 0 || L.structProps < 0 || L.structChildren < 0 || L.structSize < 0) {
    char buf[160];
    snprintf(buf, sizeof buf, "unrecognised UStruct layout (super %d, children %d, properties %d, size %d)",
             L.structSuper, L.structChildren, L.structProps, L.structSize);
    error = buf;
    return false;
  }

  // FField and FProperty follow from where the name is: { vtable, ClassPrivate, Owner, Next,
  // NamePrivate, FlagsPrivate } then { ArrayDim, ElementSize, PropertyFlags, RepIndex, ...,
  // Offset_Internal, RepNotifyFunc, 4 link pointers }.
  L.ffNext = L.ffName - 8;
  const int32_t fieldSize = L.ffName + 0x10;
  L.propArrayDim = fieldSize;
  L.propElemSize = fieldSize + 4;
  L.propFlags = fieldSize + 8;
  L.propOffset = fieldSize + 0x14;
  L.propSize = fieldSize + 0x40;
  const char* axes[] = {"X", "Y", "Z"};
  uintptr_t f = base(Ptr(vector, L.structProps));
  for (int i = 0; i < 3; ++i) {
    int32_t dim = 0, elem = 0, offset = -1;
    if (!SafeNameEquals(f + L.ffName, axes[i]) || !Read(f + L.propArrayDim, dim) || !Read(f + L.propElemSize, elem) ||
        !Read(f + L.propOffset, offset) || dim != 1 || elem != 8 || offset != 8 * i) {
      error = "unrecognised FProperty layout";
      return false;
    }
    if (i < 2 && !Read(f + L.ffNext, f)) {
      error = "unrecognised FField layout";
      return false;
    }
  }

  // UFunction comes right after UStruct; Func is its last member.
  L.funcFlags = PropertiesSize(structClass);
  L.funcNative = PropertiesSize(functionClass) - 8;
  void* canvas = FindClass("Canvas");
  void* probe = canvas ? FindFunction(canvas, "K2_DrawTexture") : nullptr;
  if (!probe || !(At<uint32_t>(probe, L.funcFlags) & kFuncNative) ||
      !scan::InText(reinterpret_cast<uintptr_t>(Ptr(probe, L.funcNative)))) {
    char buf[160];
    snprintf(buf, sizeof buf, "unrecognised UFunction layout (Canvas %s, K2_DrawTexture %s, flags at 0x%X, func at 0x%X)",
             canvas ? "found" : "missing", probe ? "found" : "missing", L.funcFlags, L.funcNative);
    error = buf;
    return false;
  }

  // UClass::ClassDefaultObject.
  void* cdo = FindObject("Object", "Default__Object");
  for (int32_t o = L.structSize; o < 0x200 && cdo; o += 8) {
    if (Ptr(objectClass, o) == cdo) {
      g_cdoOffset = o;
      break;
    }
  }
  if (g_cdoOffset < 0) {
    error = "class default objects not found";
    return false;
  }
  LOG("Layout: UStruct super 0x%X children 0x%X props 0x%X size 0x%X | FField next 0x%X name 0x%X | FProperty "
      "offset 0x%X size 0x%X | UFunction flags 0x%X func 0x%X | UClass CDO 0x%X",
      L.structSuper, L.structChildren, L.structProps, L.structSize, L.ffNext, L.ffName, L.propOffset, L.propSize,
      L.funcFlags, L.funcNative, g_cdoOffset);
  return true;
}

// UObject::ProcessEvent tests FUNC_Native (0x400) and, further in, FUNC_HasOutParms (0x400000)
// on the function it's given: `test dword ptr [reg + FunctionFlags], imm32`.
bool LooksLikeProcessEvent(uintptr_t fn) {
  uint8_t native[10] = {0xF7, 0x80, 0, 0, 0, 0, 0x00, 0x04, 0x00, 0x00};
  uint8_t outParms[10] = {0xF7, 0x80, 0, 0, 0, 0, 0x00, 0x00, 0x40, 0x00};
  memcpy(native + 2, &L.funcFlags, 4);
  memcpy(outParms + 2, &L.funcFlags, 4);
  return !scan::FindInFunction(fn, native, "x?xxxxxxxx").empty() &&
         !scan::FindInFunction(fn, outParms, "x?xxxxxxxx").empty();
}

bool FindProcessEvent(std::string& error) {
  void* cdo = DefaultObject(FindClass("Object"));
  if (!cdo) {
    error = "UObject default object missing";
    return false;
  }
  g_peSlot = FindProcessEventSlot(*static_cast<void***>(cdo));
  if (g_peSlot < 0) {
    error = "ProcessEvent not found";
    return false;
  }
  return true;
}

}  // namespace

int FindProcessEventSlot(void** vtable) {
  for (int slot = 0; vtable && slot < 160; ++slot) {
    const auto fn = reinterpret_cast<uintptr_t>(vtable[slot]);
    if (!scan::InText(fn)) break;
    if (LooksLikeProcessEvent(fn)) {
      LOG("ProcessEvent: vtable slot %d (exe+0x%llx)", slot, static_cast<unsigned long long>(fn - scan::Img().base));
      return slot;
    }
  }
  return -1;
}

// ---- public ------------------------------------------------------------------------------------

bool Init(std::string& error) {
  scan::ForgetRegions();
  if (!FindNameBlocks()) {
    error = "name table not found";
    return false;
  }
  if (!FindObjectArray()) {
    error = "object array not found";
    return false;
  }
  if (!g_classClass) g_classClass = FindClassClass();
  if (!g_classClass) {
    error = "UClass not registered yet";
    return false;
  }
  if (!CalibrateStructs(error)) return false;
  return FindProcessEvent(error);
}

int ProcessEventSlot() { return g_peSlot; }

void UseNameBlocksForTest(uint8_t** blocks) { g_nameBlocks = blocks; }

std::string NameToString(FName name) {
  const uint8_t* e = NameEntry(name.index);
  if (!e) return "?";
  const uint16_t header = *reinterpret_cast<const uint16_t*>(e);
  const uint32_t length = header >> 6;
  std::string out;
  if (header & 1) {
    const auto* chars = reinterpret_cast<const wchar_t*>(e + 2);
    for (uint32_t i = 0; i < length; ++i) out.push_back(chars[i] < 128 ? static_cast<char>(chars[i]) : '?');
  } else {
    out.assign(reinterpret_cast<const char*>(e + 2), length);
  }
  if (name.number) out += "_" + std::to_string(name.number - 1);
  return out;
}

// FNames compare without regard to case, like the engine's: the name table keeps one entry per
// name, spelled the way it was first registered, so "Canvas" can be stored as "canvas" if
// something registered that spelling first (it differs from PC to PC with the content loaded).
bool NameEquals(FName name, const char* text) {
  if (name.number) return SameText(NameToString(name), text);
  const uint8_t* e = NameEntry(name.index);
  if (!e) return false;
  const uint16_t header = *reinterpret_cast<const uint16_t*>(e);
  const size_t length = header >> 6;
  if (strlen(text) != length) return false;
  const auto lower = [](unsigned c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
  if (!(header & 1)) {
    const auto* chars = e + 2;
    for (size_t i = 0; i < length; ++i) {
      if (lower(chars[i]) != lower(static_cast<unsigned char>(text[i]))) return false;
    }
    return true;
  }
  const auto* chars = reinterpret_cast<const wchar_t*>(e + 2);
  for (size_t i = 0; i < length; ++i) {
    if (lower(chars[i]) != lower(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}

bool SameText(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }

std::string ObjectName(const void* object) {
  return object ? NameToString(At<FName>(const_cast<void*>(object), L.objName)) : "null";
}

std::string ClassName(const void* object) { return object ? ObjectName(ClassOf(object)) : "null"; }

std::string FullName(const void* object) {
  if (!object) return "null";
  std::string path = ObjectName(object);
  void* outer = Ptr(const_cast<void*>(object), L.objOuter);
  for (int depth = 0; outer && depth < 16; ++depth) {
    path = ObjectName(outer) + "." + path;
    outer = Ptr(outer, L.objOuter);
  }
  return ClassName(object) + " " + path;
}

int32_t NumObjects() { return ObjNum(); }

void* ObjectAt(int32_t index) {
  if (index < 0 || index >= ObjNum()) return nullptr;
  return RawObjectAt(index, L.itemSize);
}

void* ClassOf(const void* object) { return object ? Ptr(const_cast<void*>(object), L.objClass) : nullptr; }

void* SuperOf(const void* ustruct) { return ustruct ? Ptr(const_cast<void*>(ustruct), L.structSuper) : nullptr; }

bool IsA(const void* object, const void* cls) {
  if (!object || !cls) return false;
  void* c = ClassOf(object);
  for (int depth = 0; c && depth < 64; ++depth) {
    if (c == cls) return true;
    c = SuperOf(c);
  }
  return false;
}

bool IsDefaultObject(const void* object) {
  return object && (At<uint32_t>(const_cast<void*>(object), L.objFlags) & kFlagClassDefaultObject);
}

bool IsValid(const void* object) {
  if (!object) return false;
  const int32_t index = InternalIndexOf(object);
  return index >= 0 && ObjectAt(index) == object;
}

void* FindObject(const char* className, const char* name) {
  const int32_t n = ObjNum();
  for (int32_t i = 0; i < n; ++i) {
    void* obj = ObjectAt(i);
    if (!obj || !NameEquals(At<FName>(obj, L.objName), name)) continue;
    void* cls = ClassOf(obj);
    if (cls && NameEquals(At<FName>(cls, L.objName), className)) return obj;
  }
  return nullptr;
}

void* FindObjectIn(const char* className, const char* name, const char* outerName) {
  const int32_t n = ObjNum();
  for (int32_t i = 0; i < n; ++i) {
    void* obj = ObjectAt(i);
    if (!obj || !NameEquals(At<FName>(obj, L.objName), name)) continue;
    void* cls = ClassOf(obj);
    void* outer = Ptr(obj, L.objOuter);
    if (cls && outer && NameEquals(At<FName>(cls, L.objName), className) &&
        NameEquals(At<FName>(outer, L.objName), outerName)) {
      return obj;
    }
  }
  return nullptr;
}

void* FindClass(const char* name) { return FindObject("Class", name); }
void* FindStruct(const char* name) { return FindObject("ScriptStruct", name); }

void* DefaultObject(void* cls) { return cls && g_cdoOffset >= 0 ? Ptr(cls, g_cdoOffset) : nullptr; }

void* FindInstance(void* cls) {
  if (!cls) return nullptr;
  const int32_t n = ObjNum();
  for (int32_t i = 0; i < n; ++i) {
    void* obj = ObjectAt(i);
    if (obj && !IsDefaultObject(obj) && IsA(obj, cls)) return obj;
  }
  return nullptr;
}

Prop FindProp(void* ustruct, const char* name) {
  Prop p;
  for (void* s = ustruct; s; s = SuperOf(s)) {
    for (void* f = Ptr(s, L.structProps); f; f = Ptr(f, L.ffNext)) {
      if (!NameEquals(At<FName>(f, L.ffName), name)) continue;
      p.offset = At<int32_t>(f, L.propOffset);
      p.size = At<int32_t>(f, L.propElemSize) * At<int32_t>(f, L.propArrayDim);
      void* fieldClass = Ptr(f, L.ffClass);
      p.type = fieldClass ? NameToString(At<FName>(fieldClass, 0)) : "?";
      if (SameText(p.type, "BoolProperty")) {
        // { uint8 FieldSize, ByteOffset, ByteMask, FieldMask }
        p.offset += At<uint8_t>(f, L.propSize + 1);
        p.boolMask = At<uint8_t>(f, L.propSize + 2);
      } else if (SameText(p.type, "StructProperty") || SameText(p.type, "ObjectProperty") ||
                 SameText(p.type, "ClassProperty") || SameText(p.type, "WeakObjectProperty") ||
                 SameText(p.type, "SoftObjectProperty")) {
        p.inner = Ptr(f, L.propSize);
      }
      return p;
    }
  }
  return p;
}

int32_t Offset(void* ustruct, const char* name) { return ustruct ? FindProp(ustruct, name).offset : -1; }

ArrayProp FindArray(void* ustruct, const char* name) {
  ArrayProp a;
  for (void* s = ustruct; s; s = SuperOf(s)) {
    for (void* f = Ptr(s, L.structProps); f; f = Ptr(f, L.ffNext)) {
      if (!NameEquals(At<FName>(f, L.ffName), name)) continue;
      void* fieldClass = Ptr(f, L.ffClass);
      if (!fieldClass || !NameEquals(At<FName>(fieldClass, 0), "ArrayProperty")) return a;
      // FArrayProperty { EArrayPropertyFlags ArrayFlags; FProperty* Inner; } in Unreal Engine 5
      // (Inner alone in some versions): the element's own property, which has the array's name.
      void* inner = nullptr;
      for (int32_t o : {L.propSize + 8, L.propSize}) {
        uintptr_t p = 0;
        if (Read(reinterpret_cast<uintptr_t>(f) + o, p) && Plausible(p) && SafeNameEquals(p + L.ffName, name)) {
          inner = reinterpret_cast<void*>(p);
          break;
        }
      }
      if (!inner) return a;
      a.offset = At<int32_t>(f, L.propOffset);
      a.elemSize = At<int32_t>(inner, L.propElemSize);
      void* innerClass = Ptr(inner, L.ffClass);
      if (innerClass && NameEquals(At<FName>(innerClass, 0), "StructProperty")) a.innerStruct = Ptr(inner, L.propSize);
      return a;
    }
  }
  return a;
}

bool ReadBool(void* base, const Prop& p) {
  return p.Valid() && (At<uint8_t>(base, p.offset) & (p.boolMask ? p.boolMask : 0xFF)) != 0;
}

void WriteBool(void* base, const Prop& p, bool value) {
  if (!p.Valid()) return;
  const uint8_t mask = p.boolMask ? p.boolMask : 0xFF;
  uint8_t& byte = At<uint8_t>(base, p.offset);
  byte = value ? static_cast<uint8_t>(byte | mask) : static_cast<uint8_t>(byte & ~mask);
}

void* FindFunction(void* cls, const char* name) {
  for (void* s = cls; s; s = SuperOf(s)) {
    for (void* f = Ptr(s, L.structChildren); f; f = Ptr(f, L.fieldNext)) {
      if (NameEquals(At<FName>(f, L.objName), name)) return f;
    }
  }
  return nullptr;
}

bool Function::Bind(void* cls, const char* name) {
  fn_ = cls ? FindFunction(cls, name) : nullptr;
  paramsSize_ = 0;
  returnOffset_ = -1;
  returnSize_ = 0;
  if (!fn_) return false;
  for (void* f = Ptr(fn_, L.structProps); f; f = Ptr(f, L.ffNext)) {
    const int32_t size = At<int32_t>(f, L.propElemSize) * At<int32_t>(f, L.propArrayDim);
    paramsSize_ = (std::max)(paramsSize_, At<int32_t>(f, L.propOffset) + size);
    if (NameEquals(At<FName>(f, L.ffName), "ReturnValue")) {
      returnOffset_ = At<int32_t>(f, L.propOffset);
      returnSize_ = size;
    }
  }
  paramsSize_ = (paramsSize_ + 15) & ~15;
  return true;
}

int32_t Function::Param(const char* name) const {
  if (!fn_) return -1;
  for (void* f = Ptr(fn_, L.structProps); f; f = Ptr(f, L.ffNext)) {
    if (NameEquals(At<FName>(f, L.ffName), name)) return At<int32_t>(f, L.propOffset);
  }
  return -1;
}

void Function::Call(void* object, void* params) const {
  if (fn_ && object) ProcessEvent(object, fn_, params);
}

void ProcessEvent(void* object, void* function, void* params) {
  using Fn = void(__fastcall*)(void*, void*, void*);
  void** vtable = *static_cast<void***>(object);
  reinterpret_cast<Fn>(vtable[g_peSlot])(object, function, params);
}

void DumpStruct(void* ustruct) {
  if (!ustruct) return;
  // Engine base classes are left out: they're the same in every game.
  static void* const stops[] = {FindClass("Actor"), FindClass("Object"), FindClass("ActorComponent"),
                                FindClass("SceneComponent"), FindClass("Pawn")};
  LOG("Reflection: %s", FullName(ustruct).c_str());
  for (void* s = ustruct; s; s = SuperOf(s)) {
    if (s != ustruct && std::find(std::begin(stops), std::end(stops), s) != std::end(stops)) break;
    LOG("  -- %s (%d bytes)", ObjectName(s).c_str(), PropertiesSize(s));
    for (void* f = Ptr(s, L.structProps); f; f = Ptr(f, L.ffNext)) {
      void* fieldClass = Ptr(f, L.ffClass);
      const std::string type = fieldClass ? NameToString(At<FName>(fieldClass, 0)) : "?";
      std::string extra;
      if (SameText(type, "StructProperty") || SameText(type, "ObjectProperty")) extra = " " + ObjectName(Ptr(f, L.propSize));
      LOG("    +0x%04X %-36s %s%s", At<int32_t>(f, L.propOffset), NameToString(At<FName>(f, L.ffName)).c_str(),
          type.c_str(), extra.c_str());
    }
  }
  for (void* f = Ptr(ustruct, L.structChildren); f; f = Ptr(f, L.fieldNext)) {
    const uintptr_t native = reinterpret_cast<uintptr_t>(Ptr(f, L.funcNative));
    LOG("    fn %s%s", ObjectName(f).c_str(),
        (At<uint32_t>(f, L.funcFlags) & kFuncNative) && scan::InText(native) ? " (native)" : "");
  }
}

bool HookSlot(void* object, int slot, void* hook, void** original) {
  if (!object || slot < 0) return false;
  void** entry = *static_cast<void***>(object) + slot;
  if (*entry == hook) return true;
  DWORD old = 0;
  if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) return false;
  *original = *entry;
  InterlockedExchangePointer(entry, hook);
  VirtualProtect(entry, sizeof(void*), old, &old);
  return true;
}

}  // namespace ue
