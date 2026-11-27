/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The shared machinery of the binding functions: the call scope (re-entry,
// the frame requirement, call counting, the usage-error check), argument
// readers and writers, error throwing, and registration.
//
// Every binding function has one fixed shape, the one part C generates:
//
//   Call c(env, info, "name");     // enter: the re-entrancy flag
//   if (!c.ok()) return nullptr;
//   ... read and check every argument with the readers ...
//   if (!c.needFrame()) return nullptr;    // or needContext()
//   ... preconditions (guard::can<Function>()) ...
//   c.enterImGui();                // counted here and nowhere else
//   ImGui::Something(...);
//   if (!c.leaveImGui()) return nullptr;   // a usage error: nothing written
//   ... write back, in parameter order ...
//   return c.result(...);
//
// A reader that fails has thrown a JavaScript exception and returns false;
// so does everything else returning bool here.

#ifndef HERMES_NODE_IMGUI_BIND_UTIL_H
#define HERMES_NODE_IMGUI_BIND_UTIL_H

#include <imgui.h>
#include <node_api.h>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace hnimgui {
namespace bind {

enum class FormatKind { Float, Int };

/// A flags or enum type: what may be passed for it. For a flags type
/// (`isFlags`) no bit outside `validMask`; for an enum type one of
/// `values[0 .. valueCount)`. `name` is the JavaScript prefix of its
/// constants (`"WindowFlags_"`), used in messages. Instances are defined
/// with the constants, in bind_constants.cpp, from the same lists that are
/// exported.
struct EnumInfo {
  const char *name;
  bool isFlags;
  int validMask;
  const int *values;
  int valueCount;
};

// One per enum type of the surface table's "Constants" section, named
// after the type minus `ImGui` / `Im` and the trailing underscore.
// ImGuiMouseButton has one too, for completeness: a mouse-button argument
// is read with intIn(0, 4) instead (G1).
extern const EnumInfo kWindowFlags;
extern const EnumInfo kChildFlags;
extern const EnumInfo kInputTextFlags;
extern const EnumInfo kTreeNodeFlags;
extern const EnumInfo kPopupFlags;
extern const EnumInfo kSelectableFlags;
extern const EnumInfo kComboFlags;
extern const EnumInfo kTabBarFlags;
extern const EnumInfo kTabItemFlags;
extern const EnumInfo kFocusedFlags;
extern const EnumInfo kHoveredFlags;
extern const EnumInfo kDockNodeFlags;
extern const EnumInfo kDir;
extern const EnumInfo kKey;
extern const EnumInfo kCol;
extern const EnumInfo kStyleVar;
extern const EnumInfo kButtonFlags;
extern const EnumInfo kColorEditFlags;
extern const EnumInfo kSliderFlags;
extern const EnumInfo kMouseButton;
extern const EnumInfo kCond;
extern const EnumInfo kTableFlags;
extern const EnumInfo kTableColumnFlags;
extern const EnumInfo kTableRowFlags;
extern const EnumInfo kDrawFlags;

/// A one-element reference array of booleans (`bool*`).
struct RefBool {
  napi_value array = nullptr; // null when the nullable argument was null
  bool value = false;
  bool isNull() const {
    return array == nullptr;
  }
  bool *ptr() {
    return array == nullptr ? nullptr : &value;
  }
};

/// A reference array of `n` ints (`int*`, `int v[N]`), n <= 4.
struct RefInt {
  napi_value array = nullptr;
  int n = 0;
  int values[4] = {0, 0, 0, 0};
  bool isNull() const {
    return array == nullptr;
  }
  int *ptr() {
    return array == nullptr ? nullptr : values;
  }
};

/// A reference array of `n` floats (`float*`, `float v[N]`), n <= 4.
struct RefFloat {
  napi_value array = nullptr;
  int n = 0;
  float values[4] = {0, 0, 0, 0};
  bool isNull() const {
    return array == nullptr;
  }
  float *ptr() {
    return array == nullptr ? nullptr : values;
  }
};

/// A one-element reference array holding a string, with the buffer ImGui
/// edits (`char* buf, size_t buf_size`). `buffer` has `capacity` bytes and
/// is always terminated; `original` is the incoming string's encoding,
/// untruncated, which write() compares the buffer with.
struct RefString {
  napi_value array = nullptr;
  std::string original;
  std::vector<char> buffer;
  char *data() {
    return buffer.data();
  }
  size_t capacity() const {
    return buffer.size();
  }
};

/// A sequence of floats: an Array, copied into `copy`, or a Float32Array,
/// whose data is borrowed and valid only until the next call into the
/// engine.
struct FloatSpan {
  const float *data = nullptr;
  size_t count = 0;
  std::vector<float> copy;
};

/// One per binding function, constructed first. Throws ERR_IMGUI_REENTRANT
/// (ok() false) if another binding function is on the stack.
class Call {
 public:
  Call(napi_env env, napi_callback_info info, const char *name);
  ~Call(); // lowers the inside-a-call flag if this call raised it
  Call(const Call &) = delete;
  Call &operator=(const Call &) = delete;

  bool ok() const; // false once anything has thrown
  napi_env env() const {
    return env_;
  }
  const char *name() const {
    return name_;
  }
  size_t argc() const {
    return argc_;
  }
  napi_value arg(size_t i) const; // undefined when i >= argc
  bool isMissing(size_t i) const; // undefined, or beyond argc
  bool isNull(size_t i) const; // exactly null
  napi_valuetype typeOf(size_t i) const; // napi_undefined beyond argc
  bool isArray(size_t i) const; // a real Array

  // Frame requirement; each throws and returns false when unmet.
  bool needFrame(); // ERR_IMGUI_NO_FRAME
  bool needContext(); // creates it; ERR_IMGUI_NO_WINDOW with no window
  bool needNoFrame(); // ERR_IMGUI_USAGE inside a frame
  // The label-id check (guard::labelIdIsWindowId), after needFrame():
  // ERR_IMGUI_USAGE when \p label, the argument named \p what, would give
  // the item the current window's own ID.
  bool needLabelId(const char *what, const std::string &label);

  // Readers: return false after throwing. `what` is the parameter's name.
  bool boolean(size_t i, const char *what, bool *out);
  bool int32(size_t i, const char *what, int *out); // signed 32-bit
  bool intIn(size_t i, const char *what, int lo, int hi, int *out);
  bool u32(size_t i, const char *what, ImU32 *out); // -2^31..2^32-1, mod 2^32
  bool f32(size_t i, const char *what, float *out); // finite, float range
  bool f64(size_t i, const char *what, double *out);
  bool string(size_t i, const char *what, std::string *out); // no U+0000
  bool stringOrNull(size_t i, const char *what, std::string *out, bool *isNull);
  bool vec2(size_t i, const char *what, ImVec2 *out);
  bool vec4(size_t i, const char *what, ImVec4 *out);
  bool flags(size_t i, const char *what, const EnumInfo &type, int *out);
  bool enumValue(size_t i, const char *what, const EnumInfo &type, int *out);
  bool format(size_t i, const char *what, FormatKind kind, std::string *out);
  // Reference arrays: read n elements now, remember the array for
  // write-back. A nullable one accepts undefined or null (isNull() then).
  bool refBool(size_t i, const char *what, bool nullable, RefBool *out);
  bool refInt(size_t i, const char *what, int n, bool nullable, RefInt *out);
  bool
  refFloat(size_t i, const char *what, int n, bool nullable, RefFloat *out);
  bool refString(size_t i, const char *what, size_t capacity, RefString *out);
  // Array (copied) or Float32Array (borrowed); call LAST. At most
  // \p maxCount numbers, checked before anything is allocated or read
  // (ERR_OUT_OF_RANGE); a detached Float32Array is ERR_INVALID_ARG_VALUE.
  bool floats(size_t i, const char *what, size_t maxCount, FloatSpan *out);
  // The optional output array of a vector getter: absent, or an Array.
  bool outArray(size_t i, const char *what);
  // A width or size ImGui converts to int with `(float)(int)v` (IM_TRUNC
  // and ImTrunc, imgui_internal.h:298, 556), undefined from 2^31 up:
  // ERR_OUT_OF_RANGE when \p v, the value named \p what, is 2^31 or more.
  // A negative value is not converted as it is, so it is not bounded here;
  // each caller says why.
  bool belowIntLimit(float v, const char *what);

  // After the arguments are read: counts the call, then the caller calls
  // ImGui.
  void enterImGui();
  // After the ImGui call: throws ERR_IMGUI_USAGE if ImGui reported an
  // error during it (returns false then, and nothing is written back).
  bool leaveImGui();
  // Writers, used only after leaveImGui() returned true. A null reference
  // is skipped.
  bool write(RefBool &r);
  bool write(RefInt &r);
  bool write(RefFloat &r);
  bool write(RefString &r);
  napi_value result(bool v);
  napi_value result(double v);
  napi_value resultU32(ImU32 v);
  // Fills arg(outArg) if given (checked with outArray() first), else a new
  // Array.
  napi_value resultVec2(size_t outArg, ImVec2 v);
  napi_value resultVec4(size_t outArg, ImVec4 v);
  napi_value undefined();
  // Throwing helpers; each returns nullptr, prefixes `imgui.<name>(): `.
  napi_value throwUsage(const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
  napi_value throwType(const char *code, const char *fmt, ...)
      __attribute__((format(printf, 3, 4)));
  napi_value throwRange(const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));
  // True when \p status is napi_ok. Otherwise the call is no longer ok,
  // and unless the engine already has an exception pending this throws an
  // Error, `imgui.<name>(): internal error <action> <what>`, so that no
  // failure path returns without an exception.
  bool napiOk(napi_status status, const char *action, const char *what);

 private:
  static constexpr size_t kMaxArgs = 8;
  /// Whether an exception is pending after a call into the engine; if so,
  /// the call is no longer ok.
  bool pending();
  bool element(napi_value array, uint32_t k, napi_value *out);
  bool arrayOf(
      size_t i,
      const char *what,
      uint32_t n,
      bool nullable,
      napi_value *array);
  bool f32Value(napi_value v, const char *what, float *out);
  napi_value fillVec(size_t outArg, const float *v, uint32_t n);

  napi_env env_;
  const char *name_;
  bool ok_ = true;
  bool raisedFlag_ = false;
  size_t argc_ = 0;
  napi_value argv_[kMaxArgs] = {};
  napi_value undefined_ = nullptr;
};

/// Around a call from the binding into a JavaScript callback (the list
/// clipper's): lowers the inside-a-call flag so the callback may call
/// binding functions, raises the callback depth, and restores both in its
/// destructor, also when a JavaScript exception is pending. Nests.
class CallbackScope {
 public:
  CallbackScope();
  ~CallbackScope();
  CallbackScope(const CallbackScope &) = delete;
  CallbackScope &operator=(const CallbackScope &) = delete;

 private:
  bool savedFlag_;
};

/// True while any CallbackScope is alive; newFrame() and render() throw
/// ERR_IMGUI_USAGE then.
bool insideCallback();

/// name -> times that binding function called ImGui, for __test.bindCalls().
std::vector<std::pair<std::string, int>> bindCalls();

/// Defines `name` on `target` as a function calling `cb`.
bool registerFunction(
    napi_env env,
    napi_value target,
    const char *name,
    napi_callback cb);

/// Defines every exported enumerator, and FLT_MIN / FLT_MAX, on `exports`.
bool registerConstants(napi_env env, napi_value exports);

// One per group, each in its bind_<group>.cpp.
bool registerFrame(napi_env env, napi_value exports);
bool registerWindows(napi_env env, napi_value exports);
bool registerLayout(napi_env env, napi_value exports); // Layout and IDs
bool registerText(napi_env env, napi_value exports);
bool registerWidgets(napi_env env, napi_value exports);
bool registerTreesTabs(napi_env env, napi_value exports);
bool registerMenus(napi_env env, napi_value exports);
bool registerTables(napi_env env, napi_value exports);
bool registerPopups(napi_env env, napi_value exports);
bool registerQueries(napi_env env, napi_value exports);
bool registerStyle(napi_env env, napi_value exports);
bool registerPlots(napi_env env, napi_value exports);
// `imgui.drawList` and its functions.
bool registerDrawList(napi_env env, napi_value exports);
bool registerClipper(napi_env env, napi_value exports);

} // namespace bind
} // namespace hnimgui

#endif // HERMES_NODE_IMGUI_BIND_UTIL_H
