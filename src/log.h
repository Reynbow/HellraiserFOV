#pragma once

namespace logx {

void Open(const wchar_t* path);
void Write(const char* fmt, ...);

}  // namespace logx

#define LOG(...) ::logx::Write(__VA_ARGS__)
