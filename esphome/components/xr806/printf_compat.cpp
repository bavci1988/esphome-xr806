#ifdef USE_XR806

#include <cstdarg>
#include <cstddef>
#include <cstdio>

// snprintf() and friends come from the XR806 ROM, a newlib-nano build without the C99 'z' and 't'
// length modifiers: "%zu" prints "zu" and shifts every following argument. That broke HTTP chunk
// sizes and Content-Length headers. size_t and ptrdiff_t have the width of int on this target,
// so the wrappers drop those modifiers before the ROM formats the string. The link wraps these
// symbols (see xr806_esphome.mk), so calls from every object in the image land here.

extern "C" {
int __real_vsnprintf(char *buf, size_t size, const char *format, va_list args);
int __wrap_vsnprintf(char *buf, size_t size, const char *format, va_list args);
int __wrap_snprintf(char *buf, size_t size, const char *format, ...);
int __wrap_vsprintf(char *buf, const char *format, va_list args);
int __wrap_sprintf(char *buf, const char *format, ...);
}

namespace {

// Format strings longer than this are passed through unchanged; none in the image comes close.
constexpr size_t MAX_FORMAT_LEN = 256;

bool is_flag_or_width(char c) {
  return c == '-' || c == '+' || c == ' ' || c == '#' || c == '.' || c == '*' || (c >= '0' && c <= '9');
}

// Copies format to out without 'z'/'t' length modifiers. Returns nullptr when nothing changed or
// the format does not fit, so the caller can use the original string.
const char *strip_size_modifiers(const char *format, char (&out)[MAX_FORMAT_LEN]) {
  bool changed = false;
  size_t o = 0;
  for (const char *p = format; *p != '\0'; ++p) {
    if (o + 1 >= MAX_FORMAT_LEN)
      return nullptr;
    out[o++] = *p;
    if (*p != '%')
      continue;
    ++p;
    while (*p != '\0' && is_flag_or_width(*p)) {
      if (o + 1 >= MAX_FORMAT_LEN)
        return nullptr;
      out[o++] = *p++;
    }
    if (*p == 'z' || *p == 't') {
      changed = true;
      ++p;
    }
    if (*p == '\0')
      break;
    if (o + 1 >= MAX_FORMAT_LEN)
      return nullptr;
    out[o++] = *p;
  }
  if (!changed)
    return nullptr;
  out[o] = '\0';
  return out;
}

}  // namespace

extern "C" int __wrap_vsnprintf(char *buf, size_t size, const char *format, va_list args) {
  char fixed[MAX_FORMAT_LEN];
  const char *use = format == nullptr ? nullptr : strip_size_modifiers(format, fixed);
  return __real_vsnprintf(buf, size, use != nullptr ? use : format, args);
}

extern "C" int __wrap_snprintf(char *buf, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int ret = __wrap_vsnprintf(buf, size, format, args);
  va_end(args);
  return ret;
}

// The ROM's sprintf has no bound; keep that, as callers size their buffers for the result.
extern "C" int __wrap_vsprintf(char *buf, const char *format, va_list args) {
  return __wrap_vsnprintf(buf, static_cast<size_t>(-1) >> 1, format, args);
}

extern "C" int __wrap_sprintf(char *buf, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int ret = __wrap_vsprintf(buf, format, args);
  va_end(args);
  return ret;
}

#endif  // USE_XR806
