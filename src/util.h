#pragma once

#include <windows.h>

namespace util {

// Seconds since an arbitrary point, from the high-resolution performance counter.
inline double Now() {
  static const double inv = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1.0 / static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return static_cast<double>(c.QuadPart) * inv;
}

template <class T>
inline T Clamp(T v, T lo, T hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace util
