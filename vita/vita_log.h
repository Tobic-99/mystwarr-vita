#pragma once
// Vita build: printf goes to stderr (the TTY) and to ux0:data/mystwarr/log.txt.
#include <cstdio>
#include <cstdarg>
namespace std {   // host code writes std::printf, so the logger must be reachable as std::vita_logf too
inline void vita_logf(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt); std::vfprintf(stderr, fmt, a); va_end(a);
    static FILE* f = std::fopen("ux0:data/mystwarr/log.txt", "w");
    if (f) { va_start(a, fmt); std::vfprintf(f, fmt, a); va_end(a); std::fflush(f); }
}
}
using std::vita_logf;
#define printf(...) vita_logf(__VA_ARGS__)
