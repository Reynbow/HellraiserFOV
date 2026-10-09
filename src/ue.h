#pragma once

// Minimal access to Unreal Engine objects through the engine's own reflection data: the global
// object array, the name table, and every class's property and function lists. Property offsets
// and functions are looked up by name at startup, so they follow game updates. The handful of
// core engine layouts that reflection can't describe are checked against well-known objects
// (FVector, AActor, ...) before anything relies on them.

#include <cstdint>
#include <string>

namespace ue {

// Engine value types passed to and from engine functions; layouts must match the engine's.
struct FName {
  uint32_t index;
  uint32_t number;
};
struct FVector2D {
  double X, Y;
};
struct FLinearColor {
  float R, G, B, A;
};
struct FColor {
  uint8_t B, G, R, A;  // FColor is stored BGRA on little-endian platforms
};
struct FString {  // TArray<TCHAR>; Num counts the terminator
  const wchar_t* data;
  int32_t num;
  int32_t max;
};
template <class T>
struct TArray {
  T* data;
  int32_t num;
  int32_t max;
};

// Core layouts. The defaults are Unreal Engine 5.6's; Init() confirms or corrects each one.
struct Layout {
  int32_t objFlags = 0x08;        // UObjectBase::ObjectFlags
  int32_t objIndex = 0x0C;        // UObjectBase::InternalIndex
  int32_t objClass = 0x10;        // UObjectBase::ClassPrivate
  int32_t objName = 0x18;         // UObjectBase::NamePrivate
  int32_t objOuter = 0x20;        // UObjectBase::OuterPrivate
  int32_t fieldNext = 0x28;       // UField::Next
  int32_t structSuper = 0x40;     // UStruct::SuperStruct
  int32_t structChildren = 0x48;  // UStruct::Children (functions)
  int32_t structProps = 0x50;     // UStruct::ChildProperties (FField list)
  int32_t structSize = 0x58;      // UStruct::PropertiesSize
  int32_t funcFlags = 0xB0;       // UFunction::FunctionFlags
  int32_t funcNative = 0xD8;      // UFunction::Func
  int32_t ffClass = 0x08;         // FField::ClassPrivate (an FFieldClass, whose FName comes first)
  int32_t ffNext = 0x18;          // FField::Next
  int32_t ffName = 0x20;          // FField::NamePrivate
  int32_t propArrayDim = 0x30;    // FProperty::ArrayDim
  int32_t propElemSize = 0x34;    // FProperty::ElementSize
  int32_t propFlags = 0x38;       // FProperty::PropertyFlags
  int32_t propOffset = 0x44;      // FProperty::Offset_Internal
  int32_t propSize = 0x70;        // sizeof(FProperty): where FBoolProperty / FStructProperty data starts
  int32_t itemSize = 24;          // sizeof(FUObjectItem)
};
extern Layout L;

constexpr uint32_t kFlagClassDefaultObject = 0x10;  // RF_ClassDefaultObject
constexpr uint32_t kFuncNative = 0x400;             // FUNC_Native

// Finds the name table and the object array and checks the core layouts. Call from a worker
// thread: it returns false until the engine has started up far enough (the caller retries).
bool Init(std::string& error);
// The vtable slot of UObject::ProcessEvent, found from the code (Init must have succeeded).
int ProcessEventSlot();
// The slot of ProcessEvent in a UObject vtable (uses L.funcFlags); -1 if not found.
int FindProcessEventSlot(void** vtable);
// For the offline check: points the name lookups at a hand-made name table (FNameEntryAllocator::Blocks).
void UseNameBlocksForTest(uint8_t** blocks);

template <class T>
inline T& At(void* base, int64_t offset) {
  return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}
inline void* Ptr(void* base, int64_t offset) { return At<void*>(base, offset); }

// ---- names and objects ------------------------------------------------------------------------

std::string NameToString(FName name);
// Whether |name| is |text|, ignoring case as the engine does, without building a string.
bool NameEquals(FName name, const char* text);
bool SameText(const std::string& a, const char* b);  // ASCII, ignoring case
std::string ObjectName(const void* object);     // e.g. "PlayerCharacter_C_0"
std::string ClassName(const void* object);      // name of the object's class
std::string FullName(const void* object);       // "Class Package.Outer.Name", for the log

int32_t NumObjects();
void* ObjectAt(int32_t index);
void* ClassOf(const void* object);
void* SuperOf(const void* ustruct);
bool IsA(const void* object, const void* cls);
bool IsDefaultObject(const void* object);
// A live object that refers back to itself through its slot in the object array.
bool IsValid(const void* object);

// The UClass, UScriptStruct or other object called |name| whose own class is called |className|
// (e.g. "Class", "ScriptStruct", "BlueprintGeneratedClass"). Scans the object array.
void* FindObject(const char* className, const char* name);
// The same, limited to objects whose outer (package) is called |outerName|, e.g. "/Script/Project".
void* FindObjectIn(const char* className, const char* name, const char* outerName);
void* FindClass(const char* name);   // native class, e.g. "Canvas"
void* FindStruct(const char* name);  // native struct, e.g. "MinimalViewInfo"
void* DefaultObject(void* cls);      // the class default object ("Default__X")
// The first instance of |cls| (or a subclass) that isn't a default object, or nullptr.
void* FindInstance(void* cls);

// ---- properties and functions ----------------------------------------------------------------

struct Prop {
  int32_t offset = -1;    // from the start of the object or struct
  int32_t size = 0;       // ElementSize * ArrayDim
  uint8_t boolMask = 0;   // bool properties: the bit in the byte at |offset| (0xFF for a plain bool)
  void* inner = nullptr;  // struct properties: the UScriptStruct; object properties: the class
  std::string type;       // "FloatProperty", "StructProperty", ...
  bool Valid() const { return offset >= 0; }
};

// A property of |ustruct| (a class or a struct) or of one of its super structs.
Prop FindProp(void* ustruct, const char* name);
int32_t Offset(void* ustruct, const char* name);  // -1 if there's no such property

// Reads a bool property described by |p| from |base| (handles bitfields).
bool ReadBool(void* base, const Prop& p);
void WriteBool(void* base, const Prop& p, bool value);

// A UFunction of |cls| or one of its super classes.
void* FindFunction(void* cls, const char* name);

// A UFunction bound for calling through ProcessEvent, with its parameter offsets.
class Function {
 public:
  bool Bind(void* cls, const char* name);
  bool Bound() const { return fn_ != nullptr; }
  void* Get() const { return fn_; }
  int32_t ParamsSize() const { return paramsSize_; }
  // Offset of a parameter in the parameter block, or -1.
  int32_t Param(const char* name) const;
  int32_t Return() const { return returnOffset_; }  // offset of ReturnValue, or -1
  int32_t ReturnSize() const { return returnSize_; }
  // Calls the function on |object| with the parameter block |params| (ParamsSize() bytes).
  void Call(void* object, void* params) const;

 private:
  void* fn_ = nullptr;
  int32_t paramsSize_ = 0;
  int32_t returnOffset_ = -1;
  int32_t returnSize_ = 0;
};

// Calls UObject::ProcessEvent.
void ProcessEvent(void* object, void* function, void* params);

// Every property of |ustruct| and its supers (name, type, offset) and its functions, to the log.
void DumpStruct(void* ustruct);

// ---- hooking ---------------------------------------------------------------------------------

// Points slot |slot| of |object|'s vtable at |hook|. |original| receives the previous entry
// (left untouched if the slot already holds |hook|). Returns false if the slot can't be written.
bool HookSlot(void* object, int slot, void* hook, void** original);

}  // namespace ue
