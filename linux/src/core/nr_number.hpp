#pragma once
// Numbers in dlssnr-amd.ini, whatever the process's C locale: a game that sets LC_NUMERIC to one with a decimal comma
// (setlocale(LC_ALL, "")) would make strtof stop at "0.5"'s point and snprintf write "0,5" (upstream RenoDX repairs
// both). Read: a point or a comma as the decimal separator, std::from_chars (no locale). Written: decimal_point() on
// what snprintf made.
#include <charconv>
#include <cstddef>
#include <string>
#include <cmath>
#include <cstdint>
#include <limits>
#include <cctype>

namespace nr {
// Hosts may report rejected values; the parsers also serve tools without a logger.
inline void (*number_error)(const std::string&) = nullptr;
inline void bad_number(const std::string& text) { if (number_error) number_error(text); }
inline bool number_tail(const char* p, const char* end) {
    while (p != end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    return p == end;
}
template<class T> inline T parse_integer(const std::string& text, T fallback, bool* ok = nullptr) {
    const char* p = text.data(); const char* end = p + text.size();
    while (p != end && std::isspace(static_cast<unsigned char>(*p))) ++p;
    if (p != end && *p == '+') ++p;
    T v{}; const auto r = std::from_chars(p, end, v);
    const bool valid = r.ec == std::errc{} && number_tail(r.ptr, end);
    if (ok) *ok = valid;
    if (!valid) { bad_number(text); return fallback; }
    return v;
}


// `text` as a float: leading spaces and a '+' allowed, ',' taken as '.'; `fallback` unless the whole value is a finite number (trailing whitespace allowed). `end`
// (optional): set to the characters read (0 when none).
inline float parse_float(const std::string& text, float fallback = 0.0f, size_t* end = nullptr) {
    size_t i = 0;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i < text.size() && text[i] == '+') ++i;
    std::string s = text.substr(i);
    for (char& c : s)
        if (c == ',') { c = '.'; break; }
    float v = fallback;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || !std::isfinite(v) || !number_tail(r.ptr, s.data() + s.size())) {
        bad_number(text);
        if (end) *end = 0;
        return fallback;
    }
    if (end) *end = i + size_t(r.ptr - s.data());
    return v;
}
inline float parse_float(const char* text, float fallback = 0.0f) { return parse_float(std::string(text ? text : ""), fallback); }

// A number snprintf wrote in the process's locale, with a point as its decimal separator.
inline std::string decimal_point(std::string s) {
    for (char& c : s)
        if (c == ',') c = '.';
    return s;
}
inline char* decimal_point(char* s) {
    for (char* p = s; p && *p; ++p)
        if (*p == ',') *p = '.';
    return s;
}

}  // namespace nr
