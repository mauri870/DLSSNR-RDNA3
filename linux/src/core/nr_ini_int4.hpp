#pragma once
// [Int4Mixed] in dlssnr-amd.ini: Enabled (the int4 mixed network at start), Hotkey (switch int4 mixed / the default
// network in game), KeepBoth (keep the other network resident after a switch) and Sound (the switch cue). Read by the runtime and by the Linux
// int4 layer when the game creates its device. enabled -1: no file, no section or no readable value - which counts as
// on: these are read only where int4 mixed was installed, and choosing it at install means wanting it on. hotkey
// empty: none.
#include <cctype>
#include <cstdio>
#include <string>

namespace nr {
struct Int4Ini {
    int enabled = -1;
    std::string hotkey;
    bool keep_both = false;   // KeepBoth: both networks stay resident after a switch
    bool sound = true;        // Sound: the switch cue
};
inline Int4Ini ini_int4(const std::string& path) {
    Int4Ini r;
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return r;
    auto trim = [](std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        size_t i = 0; while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        return s.substr(i);
    };
    auto lower = [](std::string s) { for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; };
    std::string section; char line[512];
    while (std::fgets(line, sizeof line, f)) {
        std::string t = line;
        const auto c = t.find_first_of(";#");
        if (c != std::string::npos) t.resize(c);
        t = trim(t);
        if (t.empty()) continue;
        if (t.front() == '[') { const auto e = t.find(']'); section = lower(trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1))); continue; }
        const auto eq = t.find('=');
        if (eq == std::string::npos || section != "int4mixed") continue;
        const std::string key = lower(trim(t.substr(0, eq))), raw = trim(t.substr(eq + 1)), val = lower(raw);
        if (key == "enabled") {
            if (val == "1" || val == "true" || val == "on" || val == "yes") r.enabled = 1;
            else if (val == "0" || val == "false" || val == "off" || val == "no") r.enabled = 0;
        } else if (key == "hotkey") r.hotkey = raw;
        else if (key == "keepboth") r.keep_both = val == "1" || val == "true" || val == "on" || val == "yes";
        else if (key == "sound") r.sound = !(val == "0" || val == "false" || val == "off" || val == "no");
    }
    std::fclose(f);
    return r;
}
}  // namespace nr
