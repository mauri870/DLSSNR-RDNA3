#pragma once
// [Network] in dlssnr-amd.ini: Model (which model the network runs) and Motion Without Depth (where the pre block
// reads the motion vectors), read by the runtime when it builds the network. ini_network: -1 for no file, no
// section or no readable value (the runtime's default, the small model).
#include <cctype>
#include <cstdio>
#include <string>

namespace nr {
// The networks a user picks, by the DLL's own codenames (dlssnr-amd.ini [Network] Model, NR Control, the log).
enum NetworkChoice : int { kSmallFp8 = 0, kSilverAardwold = 1, kSilverAardwoldInt4 = 2, kNetworkChoices = 3 };
inline const char* network_codename(int n) {
    return n == kSmallFp8 ? "CC_SMALL_FP8" : n == kSilverAardwold ? "CC_SILVER_AARDWOLD"
         : n == kSilverAardwoldInt4 ? "CC_SILVER_AARDWOLD_INT4_MIXED" : "?";
}
// A codename, or the old words (small, original); -1 for anything else (case-insensitive).
inline int network_choice(std::string v) {
    for (auto& c : v) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (v == "CC_SMALL_FP8" || v == "SMALL") return kSmallFp8;
    if (v == "CC_SILVER_AARDWOLD" || v == "ORIGINAL") return kSilverAardwold;
    if (v == "CC_SILVER_AARDWOLD_INT4_MIXED") return kSilverAardwoldInt4;
    return -1;
}
// RuntimeConfig::network's numbering: 1 the small model (CC_SMALL_FP8), 0 the 310.8 model (both CC_SILVER_AARDWOLD
// networks), -1 anything else.
inline int network_value(std::string v) {
    const int n = network_choice(v);
    return n == kSmallFp8 ? 1 : n < 0 ? -1 : 0;
}
inline int ini_network(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return -1;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    int r = -1;
    std::string section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == "model") r = network_value(trim(t.substr(eq + 1)));
    }
    std::fclose(f);
    return r;
}
// Another [Network] key's value, trimmed ("" when there is none): `key` lower case, as the file's keys are compared.
inline std::string ini_network_key(const std::string& path, const std::string& key) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return {};
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::string r, section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "network") continue;
        if (lower(trim(t.substr(0, eq))) == key) r = trim(t.substr(eq + 1));
    }
    std::fclose(f);
    return r;
}
}  // namespace nr
