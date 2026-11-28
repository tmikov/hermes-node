/*
 * Copyright (c) Tzvetan Mikov.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// The binding's call scope, readers, writers and error throwing
// (bind_util.h).

#include "bind_util.h"

#include "bind_guards.h"
#include "imgui_addon.h"

#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>

namespace hnimgui {
namespace bind {

namespace {

/// Set for the whole of each binding function, newFrame() and render()
/// included; lowered only by a CallbackScope.
bool gInsideCall = false;
/// How many CallbackScopes are alive.
int gCallbackDepth = 0;
/// name -> times it called ImGui.
std::map<std::string, int> gCallCounts;

std::string vformat(const char *fmt, va_list args) {
  va_list measure;
  va_copy(measure, args);
  const int n = std::vsnprintf(nullptr, 0, fmt, measure);
  va_end(measure);
  if (n < 0)
    return fmt;
  std::string s(static_cast<size_t>(n) + 1, '\0');
  std::vsnprintf(&s[0], s.size(), fmt, args);
  s.resize(static_cast<size_t>(n));
  return s;
}

/// The longest prefix of \p s of at most \p max bytes that does not end
/// inside a UTF-8 sequence.
size_t utf8Prefix(const std::string &s, size_t max) {
  if (s.size() <= max)
    return s.size();
  size_t cut = max;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80)
    --cut;
  return cut;
}

bool isFloatConversion(char c) {
  return std::strchr("fFeEgGaA", c) != nullptr;
}

bool isIntConversion(char c) {
  return std::strchr("diuxXo", c) != nullptr;
}

/// The longest conversion specification, from its `%` to its conversion
/// character inclusive, that ImGui can copy. ImParseFormatSanitizeForPrinting
/// and ImParseFormatSanitizeForScanning copy the specification into a
/// `char[32]` and check only with a plain assertion that its length plus
/// the terminator is less than 32, that is at most 30 bytes
/// (imgui_widgets.cpp:3713-3714, 3728-3729). The copies: fmt_sanitized[32]
/// in RoundScalarWithFormatT (imgui_widgets.cpp:2516-2517), reached from
/// DragBehaviorT and SliderBehaviorT when the value changes (2636, 3250,
/// 3272); format_sanitized[32] in DataTypeApplyFromText (2402-2409), on the
/// text entry of an int slider, drag or input; and fmt_buf[32] in
/// TempInputScalar, which ImParseFormatTrimDecorations fills with a bounded
/// copy that would silently truncate (3823-3825, 3704). Everything else that
/// formats with the whole format string writes a fixed buffer through
/// ImFormatString, which is vsnprintf with the buffer's size
/// (imgui.cpp:2466-2478): value_buf[64] in DragScalar and SliderScalar
/// (2838-2839, 3450), buf[64] in InputScalar (3887-3891), v_str[64] in
/// RoundScalarWithFormatT (2521-2522), data_buf[32] in TempInputScalar
/// (3826-3828). Those truncate the text and cannot overflow.
constexpr size_t kMaxFormatSpec = 30;

/// The most digits a width or a precision may have, so at most 99. Both
/// reach vsnprintf every frame the widget is drawn, which produces the
/// whole padded or zero-extended text before truncating it to the buffer,
/// so a width or precision of 999999999 costs seconds a call (measured on
/// glibc by the final review). ImGui's own reading of a precision,
/// ImParseFormatPrecision, ignores one over 99 (imgui_widgets.cpp:3772),
/// and reads it with ImAtoi<int> (3746-3755, 3771), which overflows from
/// ten digits; it does not read the width (3766-3767 skip it).
constexpr size_t kMaxFormatFieldDigits = 2;

enum class FormatProblem { None, Wrong, TooLong, FieldTooWide };

/// Why \p fmt is not accepted for \p kind (None when it is): exactly
/// one printf conversion that fits \p kind, with flags, a width and a
/// precision of at most kMaxFormatFieldDigits digits each allowed, no `*`,
/// no `$`, no length modifier, at most kMaxFormatSpec bytes long. `%%` is a
/// literal and does not count, as in ImGui's own ImParseFormatFindStart
/// (imgui_widgets.cpp:3659-3669). A `0` after `%` is a flag, so leading
/// zeros are not width digits.
FormatProblem formatProblem(const std::string &fmt, FormatKind kind) {
  int conversions = 0;
  bool fieldTooWide = false;
  for (size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%')
      continue;
    const size_t start = i;
    ++i;
    if (i < fmt.size() && fmt[i] == '%')
      continue;
    while (i < fmt.size() && std::strchr("-+ #0", fmt[i]) != nullptr)
      ++i;
    size_t digits = i;
    while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
      ++i;
    if (i - digits > kMaxFormatFieldDigits)
      fieldTooWide = true;
    if (i < fmt.size() && fmt[i] == '.') {
      ++i;
      digits = i;
      while (i < fmt.size() && fmt[i] >= '0' && fmt[i] <= '9')
        ++i;
      if (i - digits > kMaxFormatFieldDigits)
        fieldTooWide = true;
    }
    if (i >= fmt.size())
      return FormatProblem::Wrong;
    const char c = fmt[i];
    if (kind == FormatKind::Float ? !isFloatConversion(c) : !isIntConversion(c))
      return FormatProblem::Wrong;
    if (i - start + 1 > kMaxFormatSpec)
      return FormatProblem::TooLong;
    ++conversions;
  }
  if (conversions != 1)
    return FormatProblem::Wrong;
  return fieldTooWide ? FormatProblem::FieldTooWide : FormatProblem::None;
}

} // namespace

// --- Call ------------------------------------------------------------------

Call::Call(napi_env env, napi_callback_info info, const char *name)
    : env_(env), name_(name) {
  napi_get_undefined(env, &undefined_);
  size_t argc = kMaxArgs;
  if (!napiOk(
          napi_get_cb_info(env, info, &argc, argv_, nullptr, nullptr),
          "reading",
          "the arguments"))
    return;
  argc_ = argc;
  if (gInsideCall) {
    std::string message = std::string("imgui.") + name +
        "(): called while another imgui call is running (from an "
        "accessor of one of its arguments)";
    napi_throw_error(env, "ERR_IMGUI_REENTRANT", message.c_str());
    ok_ = false;
    return;
  }
  gInsideCall = true;
  raisedFlag_ = true;
}

Call::~Call() {
  if (raisedFlag_)
    gInsideCall = false;
}

bool Call::ok() const {
  return ok_;
}

napi_value Call::arg(size_t i) const {
  return i < argc_ && i < kMaxArgs ? argv_[i] : undefined_;
}

napi_valuetype Call::typeOf(size_t i) const {
  napi_valuetype t = napi_undefined;
  if (i < argc_ && i < kMaxArgs)
    napi_typeof(env_, argv_[i], &t);
  return t;
}

bool Call::isMissing(size_t i) const {
  return typeOf(i) == napi_undefined;
}

bool Call::isNull(size_t i) const {
  return typeOf(i) == napi_null;
}

bool Call::isArray(size_t i) const {
  bool is = false;
  return napi_is_array(env_, arg(i), &is) == napi_ok && is;
}

bool Call::napiOk(napi_status status, const char *action, const char *what) {
  if (status == napi_ok)
    return true;
  ok_ = false;
  bool isPending = false;
  napi_is_exception_pending(env_, &isPending);
  if (!isPending) {
    const std::string message = std::string("imgui.") + name_ +
        "(): internal error " + action + " " + what;
    napi_throw_error(env_, nullptr, message.c_str());
  }
  return false;
}

bool Call::pending() {
  bool isPending = false;
  napi_is_exception_pending(env_, &isPending);
  if (isPending)
    ok_ = false;
  return isPending;
}

bool Call::needFrame() {
  if (frameOpen())
    return true;
  ok_ = false;
  throwNoFrame(env_, name_);
  return false;
}

bool Call::needContext() {
  if (ensureContext(env_))
    return true;
  std::string message = std::string("imgui.") + name_ +
      "(): there is no window; create one with ui.createWindow() first";
  napi_throw_error(env_, "ERR_IMGUI_NO_WINDOW", message.c_str());
  ok_ = false;
  return false;
}

bool Call::needNoFrame() {
  if (!frameOpen())
    return true;
  // Traced as a guard's refusal is: the binding, not ImGui, refuses it.
  static const char kMsg[] =
      "must be called outside a frame (before newFrame())";
  trace("usage error: %s", kMsg);
  throwUsage("%s", kMsg);
  return false;
}

bool Call::needLabelId(const char *what, const std::string &label) {
  if (!guard::labelIdIsWindowId(label.c_str()))
    return true;
  throwUsage(
      "%s gives the item the window's own ID (an empty label, or one ending "
      "in '###', at the root of a window); use '##name' or pushID()",
      what);
  return false;
}

// --- Throwing ----------------------------------------------------------------

napi_value Call::throwUsage(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string body = vformat(fmt, args);
  va_end(args);
  const std::string message = std::string("imgui.") + name_ + "(): " + body;
  napi_throw_error(env_, "ERR_IMGUI_USAGE", message.c_str());
  ok_ = false;
  return nullptr;
}

napi_value Call::throwType(const char *code, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string body = vformat(fmt, args);
  va_end(args);
  const std::string message = std::string("imgui.") + name_ + "(): " + body;
  napi_throw_type_error(env_, code, message.c_str());
  ok_ = false;
  return nullptr;
}

napi_value Call::throwRange(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const std::string body = vformat(fmt, args);
  va_end(args);
  const std::string message = std::string("imgui.") + name_ + "(): " + body;
  napi_throw_range_error(env_, "ERR_OUT_OF_RANGE", message.c_str());
  ok_ = false;
  return nullptr;
}

// --- Scalar readers ----------------------------------------------------------

bool Call::boolean(size_t i, const char *what, bool *out) {
  if (typeOf(i) != napi_boolean) {
    throwType("ERR_INVALID_ARG_TYPE", "%s must be a boolean", what);
    return false;
  }
  return napiOk(napi_get_value_bool(env_, arg(i), out), "reading", what);
}

bool Call::int32(size_t i, const char *what, int *out) {
  double v = 0;
  if (!f64(i, what, &v))
    return false;
  if (!(std::trunc(v) == v && v >= INT32_MIN && v <= INT32_MAX)) {
    throwRange(
        "%s must be an integer from -2147483648 to 2147483647; got %g",
        what,
        v);
    return false;
  }
  *out = static_cast<int>(v);
  return true;
}

bool Call::intIn(size_t i, const char *what, int lo, int hi, int *out) {
  int v = 0;
  if (!int32(i, what, &v))
    return false;
  if (v < lo || v > hi) {
    throwRange("%s must be an integer from %d to %d; got %d", what, lo, hi, v);
    return false;
  }
  *out = v;
  return true;
}

bool Call::u32(size_t i, const char *what, ImU32 *out) {
  double v = 0;
  if (!f64(i, what, &v))
    return false;
  if (!(std::trunc(v) == v && v >= -2147483648.0 && v <= 4294967295.0)) {
    throwRange(
        "%s must be an integer from -2147483648 to 4294967295; got %g",
        what,
        v);
    return false;
  }
  *out = static_cast<ImU32>(
      v < 0 ? static_cast<int64_t>(v) + 4294967296LL : static_cast<int64_t>(v));
  return true;
}

bool Call::f32Value(napi_value value, const char *what, float *out) {
  napi_valuetype t = napi_undefined;
  if (!napiOk(napi_typeof(env_, value, &t), "reading", what))
    return false;
  if (t != napi_number) {
    throwType("ERR_INVALID_ARG_TYPE", "%s must be a number", what);
    return false;
  }
  double v = 0;
  if (!napiOk(napi_get_value_double(env_, value, &v), "reading", what))
    return false;
  if (!(std::isfinite(v) && std::fabs(v) <= FLT_MAX)) {
    throwRange(
        "%s must be a finite number within the range of a float; got %g",
        what,
        v);
    return false;
  }
  *out = static_cast<float>(v);
  return true;
}

bool Call::f32(size_t i, const char *what, float *out) {
  return f32Value(arg(i), what, out);
}

bool Call::belowIntLimit(float v, const char *what) {
  if (v < 2147483648.0f)
    return true;
  throwRange("%s must be less than 2147483648; got %g", what, v);
  return false;
}

bool Call::inIntRange(float v, const char *what) {
  if (v >= -2147483648.0f && v < 2147483648.0f)
    return true;
  throwRange(
      "%s must be at least -2147483648 and less than 2147483648; got %g",
      what,
      v);
  return false;
}

bool Call::f64(size_t i, const char *what, double *out) {
  if (typeOf(i) != napi_number) {
    throwType("ERR_INVALID_ARG_TYPE", "%s must be a number", what);
    return false;
  }
  return napiOk(napi_get_value_double(env_, arg(i), out), "reading", what);
}

/// Reads string \p value into \p out; throws for a non-string or one
/// containing U+0000. \p what names it in the message.
static bool
readString(Call &c, napi_value value, const char *what, std::string *out) {
  napi_env env = c.env();
  napi_valuetype t = napi_undefined;
  if (!c.napiOk(napi_typeof(env, value, &t), "reading", what))
    return false;
  if (t != napi_string) {
    c.throwType("ERR_INVALID_ARG_TYPE", "%s must be a string", what);
    return false;
  }
  size_t length = 0;
  if (!c.napiOk(
          napi_get_value_string_utf8(env, value, nullptr, 0, &length),
          "reading",
          what))
    return false;
  std::vector<char> buf(length + 1);
  size_t copied = 0;
  if (!c.napiOk(
          napi_get_value_string_utf8(
              env, value, buf.data(), buf.size(), &copied),
          "reading",
          what))
    return false;
  if (std::memchr(buf.data(), '\0', copied) != nullptr) {
    c.throwType("ERR_INVALID_ARG_VALUE", "%s must not contain U+0000", what);
    return false;
  }
  out->assign(buf.data(), copied);
  return true;
}

bool Call::string(size_t i, const char *what, std::string *out) {
  return readString(*this, arg(i), what, out);
}

bool Call::stringOrNull(
    size_t i,
    const char *what,
    std::string *out,
    bool *isNullOut) {
  if (isMissing(i) || isNull(i)) {
    out->clear();
    *isNullOut = true;
    return true;
  }
  *isNullOut = false;
  return string(i, what, out);
}

bool Call::format(
    size_t i,
    const char *what,
    FormatKind kind,
    std::string *out) {
  if (!string(i, what, out))
    return false;
  const FormatProblem problem = formatProblem(*out, kind);
  if (problem == FormatProblem::TooLong) {
    throwType(
        "ERR_INVALID_ARG_VALUE",
        "%s: the conversion must be at most %zu characters long, from its "
        "'%%' to its conversion character; got '%s'",
        what,
        kMaxFormatSpec,
        out->c_str());
    return false;
  }
  if (problem == FormatProblem::FieldTooWide) {
    throwType(
        "ERR_INVALID_ARG_VALUE",
        "%s: a width or a precision must be at most %zu digits (at most "
        "99); got '%s'",
        what,
        kMaxFormatFieldDigits,
        out->c_str());
    return false;
  }
  if (problem != FormatProblem::None) {
    throwType(
        "ERR_INVALID_ARG_VALUE",
        "%s must hold exactly one %s conversion (such as %s); got '%s'",
        what,
        kind == FormatKind::Float ? "float" : "integer",
        kind == FormatKind::Float ? "%.3f" : "%d",
        out->c_str());
    return false;
  }
  return true;
}

bool Call::flags(size_t i, const char *what, const EnumInfo &type, int *out) {
  int v = 0;
  if (!int32(i, what, &v))
    return false;
  const unsigned bad =
      static_cast<unsigned>(v) & ~static_cast<unsigned>(type.validMask);
  if (bad != 0) {
    throwRange(
        "%s has bits set that no %s constant has: 0x%x", what, type.name, bad);
    return false;
  }
  *out = v;
  return true;
}

bool Call::enumValue(
    size_t i,
    const char *what,
    const EnumInfo &type,
    int *out) {
  int v = 0;
  if (!int32(i, what, &v))
    return false;
  for (int k = 0; k < type.valueCount; ++k) {
    if (type.values[k] == v) {
      *out = v;
      return true;
    }
  }
  throwRange(
      "%s must equal one of the %s constants; got %d", what, type.name, v);
  return false;
}

// --- Arrays ------------------------------------------------------------------

bool Call::element(napi_value array, uint32_t k, napi_value *out) {
  // Reading an element can run JavaScript (an accessor on the index).
  return napiOk(
             napi_get_element(env_, array, k, out), "reading", "an element") &&
      !pending();
}

bool Call::arrayOf(
    size_t i,
    const char *what,
    uint32_t n,
    bool nullable,
    napi_value *array) {
  if (nullable && (isMissing(i) || isNull(i))) {
    *array = nullptr;
    return true;
  }
  if (!isArray(i)) {
    throwType("ERR_INVALID_ARG_TYPE", "%s must be an Array", what);
    return false;
  }
  uint32_t length = 0;
  if (!napiOk(napi_get_array_length(env_, arg(i), &length), "reading", what))
    return false;
  if (length < n) {
    throwRange(
        "%s must have at least %u element%s; got %u",
        what,
        n,
        n == 1 ? "" : "s",
        length);
    return false;
  }
  *array = arg(i);
  return true;
}

bool Call::vec2(size_t i, const char *what, ImVec2 *out) {
  napi_value array = nullptr;
  if (!arrayOf(i, what, 2, false, &array))
    return false;
  float v[2];
  for (uint32_t k = 0; k < 2; ++k) {
    napi_value e = nullptr;
    const std::string name = std::string(what) + "[" + std::to_string(k) + "]";
    if (!element(array, k, &e) || !f32Value(e, name.c_str(), &v[k]))
      return false;
  }
  *out = ImVec2(v[0], v[1]);
  return true;
}

bool Call::vec4(size_t i, const char *what, ImVec4 *out) {
  napi_value array = nullptr;
  if (!arrayOf(i, what, 4, false, &array))
    return false;
  float v[4];
  for (uint32_t k = 0; k < 4; ++k) {
    napi_value e = nullptr;
    const std::string name = std::string(what) + "[" + std::to_string(k) + "]";
    if (!element(array, k, &e) || !f32Value(e, name.c_str(), &v[k]))
      return false;
  }
  *out = ImVec4(v[0], v[1], v[2], v[3]);
  return true;
}

bool Call::refBool(size_t i, const char *what, bool nullable, RefBool *out) {
  napi_value array = nullptr;
  if (!arrayOf(i, what, 1, nullable, &array))
    return false;
  out->array = array;
  if (array == nullptr)
    return true;
  napi_value e = nullptr;
  if (!element(array, 0, &e))
    return false;
  napi_valuetype t = napi_undefined;
  if (!napiOk(napi_typeof(env_, e, &t), "reading", what))
    return false;
  if (t != napi_boolean) {
    throwType("ERR_INVALID_ARG_TYPE", "%s[0] must be a boolean", what);
    return false;
  }
  return napiOk(napi_get_value_bool(env_, e, &out->value), "reading", what);
}

bool Call::refInt(
    size_t i,
    const char *what,
    int n,
    bool nullable,
    RefInt *out) {
  napi_value array = nullptr;
  if (!arrayOf(i, what, static_cast<uint32_t>(n), nullable, &array))
    return false;
  out->array = array;
  out->n = n;
  if (array == nullptr)
    return true;
  for (int k = 0; k < n; ++k) {
    napi_value e = nullptr;
    if (!element(array, static_cast<uint32_t>(k), &e))
      return false;
    napi_valuetype t = napi_undefined;
    if (!napiOk(napi_typeof(env_, e, &t), "reading", what))
      return false;
    if (t != napi_number) {
      throwType("ERR_INVALID_ARG_TYPE", "%s[%d] must be a number", what, k);
      return false;
    }
    double v = 0;
    if (!napiOk(napi_get_value_double(env_, e, &v), "reading", what))
      return false;
    if (!(std::trunc(v) == v && v >= INT32_MIN && v <= INT32_MAX)) {
      throwRange(
          "%s[%d] must be an integer from -2147483648 to 2147483647; got %g",
          what,
          k,
          v);
      return false;
    }
    out->values[k] = static_cast<int>(v);
  }
  return true;
}

bool Call::refFloat(
    size_t i,
    const char *what,
    int n,
    bool nullable,
    RefFloat *out) {
  napi_value array = nullptr;
  if (!arrayOf(i, what, static_cast<uint32_t>(n), nullable, &array))
    return false;
  out->array = array;
  out->n = n;
  if (array == nullptr)
    return true;
  for (int k = 0; k < n; ++k) {
    napi_value e = nullptr;
    const std::string name = std::string(what) + "[" + std::to_string(k) + "]";
    if (!element(array, static_cast<uint32_t>(k), &e) ||
        !f32Value(e, name.c_str(), &out->values[k]))
      return false;
  }
  return true;
}

bool Call::refString(
    size_t i,
    const char *what,
    size_t capacity,
    RefString *out) {
  napi_value array = nullptr;
  if (!arrayOf(i, what, 1, false, &array))
    return false;
  napi_value e = nullptr;
  if (!element(array, 0, &e))
    return false;
  const std::string name = std::string(what) + "[0]";
  if (!readString(*this, e, name.c_str(), &out->original))
    return false;
  out->array = array;
  // capacity >= 1 is the caller's check (the capacity argument's range).
  out->buffer.assign(capacity, '\0');
  const size_t n = utf8Prefix(out->original, capacity - 1);
  std::memcpy(out->buffer.data(), out->original.data(), n);
  out->buffer[n] = '\0';
  return true;
}

bool Call::floats(size_t i, const char *what, size_t maxCount, FloatSpan *out) {
  if (isArray(i)) {
    uint32_t length = 0;
    if (!napiOk(napi_get_array_length(env_, arg(i), &length), "reading", what))
      return false;
    // Before anything is sized by the length or any element is read: an
    // Array's length can be 2^32 - 1 with no element present.
    if (length > maxCount) {
      throwRange(
          "%s must hold at most %zu numbers; got %u", what, maxCount, length);
      return false;
    }
    out->copy.resize(length);
    for (uint32_t k = 0; k < length; ++k) {
      napi_value e = nullptr;
      const std::string name =
          std::string(what) + "[" + std::to_string(k) + "]";
      if (!element(arg(i), k, &e) || !f32Value(e, name.c_str(), &out->copy[k]))
        return false;
    }
    out->data = out->copy.data();
    out->count = length;
    return true;
  }
  bool isTyped = false;
  if (!napiOk(napi_is_typedarray(env_, arg(i), &isTyped), "reading", what))
    return false;
  napi_typedarray_type type = napi_int8_array;
  size_t length = 0;
  void *data = nullptr;
  if (isTyped &&
      !napiOk(
          napi_get_typedarray_info(
              env_, arg(i), &type, &length, &data, nullptr, nullptr),
          "reading",
          what))
    return false;
  if (!isTyped || type != napi_float32_array) {
    throwType(
        "ERR_INVALID_ARG_TYPE", "%s must be an Array or a Float32Array", what);
    return false;
  }
  // Hermes keeps a typed array's length when its buffer is detached and
  // answers a null data pointer for it
  // (hermes/API/napi/hermes_napi_typedarray.cpp:221-232).
  if (data == nullptr && length > 0) {
    throwType("ERR_INVALID_ARG_VALUE", "%s is a detached Float32Array", what);
    return false;
  }
  if (length > maxCount) {
    throwRange(
        "%s must hold at most %zu numbers; got %zu", what, maxCount, length);
    return false;
  }
  // Borrowed: nothing that can run JavaScript may happen between here and
  // the ImGui call.
  const float *values = static_cast<const float *>(data);
  for (size_t k = 0; k < length; ++k) {
    if (!std::isfinite(values[k])) {
      throwRange("%s[%zu] must be a finite number; got %g", what, k, values[k]);
      return false;
    }
  }
  out->data = values;
  out->count = length;
  return true;
}

bool Call::outArray(size_t i, const char *what) {
  if (isMissing(i) || isArray(i))
    return true;
  throwType("ERR_INVALID_ARG_TYPE", "%s must be an Array", what);
  return false;
}

// --- The ImGui call ----------------------------------------------------------

void Call::enterImGui() {
  // An error reported outside any binding call belongs to none: drop it,
  // so it is not blamed on this one. (It was traced when it happened.)
  guard::takeUsageError();
  // The last ImGui call may have turned asserts or ImGui's own error output
  // back on (bind_guards.h says which does).
  guard::restoreRecoveryFlags();
  ++gCallCounts[name_];
}

bool Call::leaveImGui() {
  if (const char *msg = guard::takeUsageError()) {
    throwUsage("%s", msg);
    return false;
  }
  return true;
}

// --- Writers -----------------------------------------------------------------

bool Call::write(RefBool &r) {
  if (r.array == nullptr)
    return true;
  napi_value v = nullptr;
  // Writing an element can run JavaScript (a setter on the index).
  return napiOk(napi_get_boolean(env_, r.value, &v), "writing", "a result") &&
      napiOk(napi_set_element(env_, r.array, 0, v), "writing", "an element") &&
      !pending();
}

bool Call::write(RefInt &r) {
  if (r.array == nullptr)
    return true;
  for (int k = 0; k < r.n; ++k) {
    napi_value v = nullptr;
    if (!napiOk(
            napi_create_int32(env_, r.values[k], &v), "writing", "a result") ||
        !napiOk(
            napi_set_element(env_, r.array, static_cast<uint32_t>(k), v),
            "writing",
            "an element") ||
        pending())
      return false;
  }
  return true;
}

bool Call::write(RefFloat &r) {
  if (r.array == nullptr)
    return true;
  for (int k = 0; k < r.n; ++k) {
    napi_value v = nullptr;
    if (!napiOk(
            napi_create_double(env_, r.values[k], &v), "writing", "a result") ||
        !napiOk(
            napi_set_element(env_, r.array, static_cast<uint32_t>(k), v),
            "writing",
            "an element") ||
        pending())
      return false;
  }
  return true;
}

bool Call::write(RefString &r) {
  // Compared with the original string's encoding, not the truncated copy,
  // so that a truncation is written back too. Bounded by the capacity,
  // although the buffer stays terminated: ImGui never writes its last byte
  // with anything but the terminator. ImGui inserts only whole code points
  // when the buffer is full (STB_TEXTEDIT_INSERTCHARS,
  // imgui_widgets.cpp:4311-4313), and napi_create_string_utf8 would turn an
  // invalid sequence into U+FFFD rather than fail.
  const size_t n = strnlen(r.buffer.data(), r.buffer.size());
  if (n == r.original.size() &&
      std::memcmp(r.buffer.data(), r.original.data(), n) == 0)
    return true;
  napi_value v = nullptr;
  return napiOk(
             napi_create_string_utf8(env_, r.buffer.data(), n, &v),
             "writing",
             "a result") &&
      napiOk(napi_set_element(env_, r.array, 0, v), "writing", "an element") &&
      !pending();
}

napi_value Call::result(bool v) {
  napi_value out = nullptr;
  return napiOk(napi_get_boolean(env_, v, &out), "returning", "the result")
      ? out
      : nullptr;
}

napi_value Call::result(double v) {
  napi_value out = nullptr;
  return napiOk(napi_create_double(env_, v, &out), "returning", "the result")
      ? out
      : nullptr;
}

napi_value Call::resultU32(ImU32 v) {
  napi_value out = nullptr;
  return napiOk(napi_create_uint32(env_, v, &out), "returning", "the result")
      ? out
      : nullptr;
}

napi_value Call::fillVec(size_t outArg, const float *v, uint32_t n) {
  napi_value target = nullptr;
  if (!isMissing(outArg)) {
    target = arg(outArg); // checked by outArray() before the ImGui call
  } else if (!napiOk(
                 napi_create_array_with_length(env_, n, &target),
                 "returning",
                 "the result")) {
    return nullptr;
  }
  for (uint32_t k = 0; k < n; ++k) {
    napi_value e = nullptr;
    if (!napiOk(
            napi_create_double(env_, v[k], &e), "returning", "the result") ||
        !napiOk(napi_set_element(env_, target, k, e), "writing", "out") ||
        pending())
      return nullptr;
  }
  return target;
}

napi_value Call::resultVec2(size_t outArg, ImVec2 v) {
  const float values[2] = {v.x, v.y};
  return fillVec(outArg, values, 2);
}

napi_value Call::resultVec4(size_t outArg, ImVec4 v) {
  const float values[4] = {v.x, v.y, v.z, v.w};
  return fillVec(outArg, values, 4);
}

napi_value Call::undefined() {
  return undefined_;
}

// --- CallbackScope -----------------------------------------------------------

CallbackScope::CallbackScope() : savedFlag_(gInsideCall) {
  gInsideCall = false;
  ++gCallbackDepth;
}

CallbackScope::~CallbackScope() {
  --gCallbackDepth;
  gInsideCall = savedFlag_;
  // The binding call that ran the callback goes on calling ImGui without
  // another enterImGui(), and the callback may have run anything.
  guard::restoreRecoveryFlags();
}

bool insideCallback() {
  return gCallbackDepth > 0;
}

std::vector<std::pair<std::string, int>> bindCalls() {
  return std::vector<std::pair<std::string, int>>(
      gCallCounts.begin(), gCallCounts.end());
}

bool registerFunction(
    napi_env env,
    napi_value target,
    const char *name,
    napi_callback cb) {
  napi_value fn = nullptr;
  return napi_create_function(env, name, NAPI_AUTO_LENGTH, cb, nullptr, &fn) ==
      napi_ok &&
      napi_set_named_property(env, target, name, fn) == napi_ok;
}

} // namespace bind
} // namespace hnimgui
