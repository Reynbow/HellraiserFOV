#include "log.h"

#include <windows.h>

#include <share.h>

#include <cstdarg>
#include <cstdio>

namespace logx {
namespace {

// The log restarts with every launch; this caps a single session in case something logs non-stop.
constexpr long kMaxBytes = 2 * 1024 * 1024;

FILE* g_file = nullptr;
bool g_full = false;
SRWLOCK g_lock = SRWLOCK_INIT;

}  // namespace

void Open(const wchar_t* path) {
  AcquireSRWLockExclusive(&g_lock);
  if (!g_file) g_file = _wfsopen(path, L"w", _SH_DENYWR);
  ReleaseSRWLockExclusive(&g_lock);
}

void Write(const char* fmt, ...) {
  AcquireSRWLockExclusive(&g_lock);
  if (g_file && !g_full) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_file);
    if (ftell(g_file) >= kMaxBytes) {
      fputs("Log size limit reached: nothing more is logged this session.\n", g_file);
      g_full = true;
    }
    fflush(g_file);
  }
  ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace logx
