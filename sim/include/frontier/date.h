#pragma once
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace frontier {

// Dates are stored as "days since 1970-01-01" so the sim can do plain integer
// arithmetic on them. Algorithms are Howard Hinnant's public-domain civil-date
// conversions; they handle leap years without any library dependency.

inline long days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

inline void civil_from_days(long z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = static_cast<int>(yoe) + static_cast<int>(era) * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += (m <= 2);
}

// Parses "YYYY-MM-DD".
inline long parse_date(const std::string& s) {
    const bool shape_ok = s.size() == 10 && s[4] == '-' && s[7] == '-';
    const int y = shape_ok ? std::atoi(s.substr(0, 4).c_str()) : 0;
    const int m = shape_ok ? std::atoi(s.substr(5, 2).c_str()) : 0;
    const int d = shape_ok ? std::atoi(s.substr(8, 2).c_str()) : 0;
    if (!shape_ok || m < 1 || m > 12 || d < 1 || d > 31)
        throw std::runtime_error("bad date '" + s + "' (expected YYYY-MM-DD)");
    return days_from_civil(y, static_cast<unsigned>(m), static_cast<unsigned>(d));
}

inline std::string format_date(long days) {
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    char buf[16];
    std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", y, m, d);
    return buf;
}

inline int year_of(long days) {
    int y;
    unsigned m, d;
    civil_from_days(days, y, m, d);
    return y;
}

}  // namespace frontier
