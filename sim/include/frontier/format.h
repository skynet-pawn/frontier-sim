#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>

namespace frontier {

// printf-style formatting into a std::string, used for event-log text.
inline std::string strf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    return buf;
}

// 1234567.8 -> "1,234,568"
inline std::string with_commas(double v) {
    long long n = std::llround(v);
    std::string s = std::to_string(n < 0 ? -n : n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<std::size_t>(i), ",");
    return (n < 0 ? "-" : "") + s;
}

inline std::string money(double v) { return "$" + with_commas(v); }

}  // namespace frontier
