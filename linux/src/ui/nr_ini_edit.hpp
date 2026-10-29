#pragma once
// dlssnr-amd.ini edited in place (a value's line keeps its key and spacing, comments stay), a key table's values read
// from and written to one section, and which [RenoDX] keys belong to the whole NR chain: NR Control's layer pages on
// the .asi route (nr_pe_menu.cpp) and the ReShade add-on's (nr_reshade_addon.cpp).
#include "nr_number.hpp"
#include "nr_pe_config.hpp"
#include "nr_pe_log.hpp"
#include "nr_renodx_keys.hpp"

#include <windows.h>

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace nr::ini_edit {

// ---- dlssnr-amd.ini, edited in place: a value's line keeps its key and spacing, comments stay ----
class Ini {
  public:
    explicit Ini(std::filesystem::path p) : path_(std::move(p)) {}
    // Re-read when the file changed (the user's editor, the DLL writing defaults). True when it did.
    bool refresh() {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path_, ec);
        if (ec) return false;
        const int64_t stamp = int64_t(t.time_since_epoch().count());
        if (stamp == stamp_ && !lines_.empty()) return false;
        std::ifstream f(path_);
        if (!f) return false;
        lines_.clear();
        std::string l;
        while (std::getline(f, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines_.push_back(l);
        }
        stamp_ = stamp;
        return true;
    }
    bool has_section(const std::string& sec) const { return find_section(sec) >= 0; }
    std::string get(const std::string& sec, const std::string& key, const std::string& def) const {
        const int at = find_key(sec, key);
        if (at < 0) return def;
        const std::string& l = lines_[size_t(at)];
        std::string v = l.substr(l.find('=') + 1);
        const auto c = v.find_first_of(";#");
        if (c != std::string::npos) v = v.substr(0, c);
        return trim(v);
    }
    void set(const std::string& sec, const std::string& key, const std::string& value_in) {
        // a number snprintf wrote under a decimal-comma locale ("0,50"): with its point (nr_number.hpp)
        std::string value = value_in;
        if (value.find(',') != std::string::npos &&
            value.find_first_not_of("+-0123456789,eE") == std::string::npos)
            value = nr::decimal_point(value);
        const int at = find_key(sec, key);
        if (at >= 0) {
            std::string& l = lines_[size_t(at)];
            const auto eq = l.find('=');
            const bool spaced = eq + 1 < l.size() && l[eq + 1] == ' ';
            l = l.substr(0, eq + 1) + (spaced ? " " : "") + value;
        } else {
            int s = find_section(sec);
            if (s < 0) {
                lines_.push_back("");
                lines_.push_back("[" + sec + "]");
                s = int(lines_.size()) - 1;
            }
            size_t end = size_t(s) + 1;
            while (end < lines_.size() && !(trim(lines_[end]).rfind("[", 0) == 0)) ++end;
            lines_.insert(lines_.begin() + std::ptrdiff_t(end), key + " = " + value);
        }
        dirty_ = true;
    }
    // The sections' names as written, in file order.
    std::vector<std::string> sections() const {
        std::vector<std::string> out;
        for (const auto& l : lines_) {
            const std::string t = trim(l);
            if (t.size() > 2 && t.front() == '[' && t.find(']') != std::string::npos) out.push_back(t.substr(1, t.find(']') - 1));
        }
        return out;
    }
    // A section's keys and values as written (comments left out), in file order.
    std::vector<std::pair<std::string, std::string>> keys(const std::string& sec) const {
        std::vector<std::pair<std::string, std::string>> out;
        const int s = find_section(sec);
        if (s < 0) return out;
        for (size_t i = size_t(s) + 1; i < lines_.size(); ++i) {
            const std::string t = trim(lines_[i]);
            if (t.rfind("[", 0) == 0) break;
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            std::string v = t.substr(eq + 1);
            const auto c = v.find_first_of(";#");
            if (c != std::string::npos) v = v.substr(0, c);
            out.push_back({trim(t.substr(0, eq)), trim(v)});
        }
        return out;
    }
    void rename_section(const std::string& from, const std::string& to) {
        const int s = find_section(from);
        if (s < 0) return;
        lines_[size_t(s)] = "[" + to + "]";
        dirty_ = true;
    }
    // The section's header and lines up to the next one (a blank line before it goes too).
    void remove_section(const std::string& sec) {
        const int s = find_section(sec);
        if (s < 0) return;
        size_t end = size_t(s) + 1;
        while (end < lines_.size() && !(trim(lines_[end]).rfind("[", 0) == 0)) ++end;
        size_t begin = size_t(s);
        if (begin > 0 && trim(lines_[begin - 1]).empty()) --begin;
        lines_.erase(lines_.begin() + std::ptrdiff_t(begin), lines_.begin() + std::ptrdiff_t(end));
        dirty_ = true;
    }
    // Written whole through a temporary and a replacing move: the DLL never reads half a file.
    void save() {
        if (!dirty_) return;
        const auto tmp = path_.wstring() + L".nrcontrol";
        {
            std::ofstream f(std::filesystem::path(tmp), std::ios::trunc | std::ios::binary);
            for (const auto& l : lines_) f << l << "\r\n";
        }
        if (!MoveFileExW(tmp.c_str(), path_.wstring().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            nr::pe::log("[nr] NR Control: could not save dlssnr-amd.ini (error %lu)", GetLastError());
        dirty_ = false;
        nr::pe::touch_config();   // the DLL takes it at its next look, now
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(path_, ec);
        if (!ec) stamp_ = int64_t(t.time_since_epoch().count());
    }

  private:
    static std::string trim(std::string s) {
        const auto a = s.find_first_not_of(" \t\r\n");
        const auto b = s.find_last_not_of(" \t\r\n");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    int find_section(const std::string& sec) const {
        for (size_t i = 0; i < lines_.size(); ++i)
            if (lower(trim(lines_[i])) == "[" + lower(sec) + "]") return int(i);
        return -1;
    }
    int find_key(const std::string& sec, const std::string& key) const {
        const int s = find_section(sec);
        if (s < 0) return -1;
        for (size_t i = size_t(s) + 1; i < lines_.size(); ++i) {
            const std::string t = trim(lines_[i]);
            if (t.rfind("[", 0) == 0) break;
            if (t.empty() || t[0] == ';' || t[0] == '#') continue;
            const auto eq = t.find('=');
            if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == lower(key)) return int(i);
        }
        return -1;
    }
    std::filesystem::path path_;
    std::vector<std::string> lines_;
    int64_t stamp_ = -1;
    bool dirty_ = false;
};

// Keys of [RenoDX] that are NR Control's for every layer, or a layer's own (Enabled, Placement), so never a layer's page
// or a [LayerN] key of its settings.
inline bool chain_global(const std::string& key) {
    std::string k = key;
    for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
    return k == "enabled" || k == "neuraluplift" || k == "nrtogglekey" || k == "nrhookpoint" || k == "nroutputrect" || k.rfind("nrscreenshot", 0) == 0;
}

// A key table's values for nr_control_ui.hpp (nr_renodx_keys.hpp's shape), read from and written to one section of the
// ini by the table's names: [RenoDX], or a [LayerN] with its processing's table.
struct KeyValues {
    const nr::renodx_keys::Key* table = nr::renodx_keys::kKeys;
    int count = nr::renodx_keys::kCount;
    std::string section = "RenoDX";
    std::vector<uint8_t> bools;
    std::vector<uint32_t> uints;
    std::vector<float> floats;
    std::vector<std::string> texts;
    bool& b(int i) { return reinterpret_cast<bool&>(bools[size_t(i)]); }
    uint32_t& u(int i) { return uints[size_t(i)]; }
    float& f(int i) { return floats[size_t(i)]; }
    std::string& s(int i) { return texts[size_t(i)]; }
    void load(const Ini& ini) {
        using namespace nr::renodx_keys;
        bools.assign(size_t(count), 0); uints.assign(size_t(count), 0); floats.assign(size_t(count), 0.f);
        texts.assign(size_t(count), {});
        for (int i = 0; i < count; ++i) {
            const Key& k = table[i];
            std::string v = ini.get(section, k.name, "");
            if (v == "auto") v.clear();
            switch (k.kind) {
                case kBool: bools[size_t(i)] = v.empty() ? k.def != 0.f : (v == "1" || v == "true"); break;
                case kUint:
                    uints[size_t(i)] = std::string(k.name) == "NRPassesMode" ? (v == "fast" ? 1u : 0u)
                        : v.empty() ? uint32_t(k.def) : nr::parse_integer<uint32_t>(v, uint32_t(k.def)); break;
                case kFloat: floats[size_t(i)] = v.empty() ? k.def : nr::parse_float(v, k.def); break;
                case kText: texts[size_t(i)] = std::string(v); break;
            }
        }
    }
    void store(Ini& ini) const {
        using namespace nr::renodx_keys;
        for (int i = 0; i < count; ++i) {
            const Key& k = table[i];
            char v[64];
            switch (k.kind) {
                case kBool: std::snprintf(v, sizeof v, "%d", bools[size_t(i)] ? 1 : 0); break;
                case kUint:
                    if (std::string(k.name) == "NRPassesMode") std::snprintf(v, sizeof v, "%s", uints[size_t(i)] ? "fast" : "legacy");
                    else std::snprintf(v, sizeof v, "%u", uints[size_t(i)]);
                    break;
                case kFloat: std::snprintf(v, sizeof v, "%.*f", int(k.decimals), double(floats[size_t(i)])); break;
                case kText: std::snprintf(v, sizeof v, "%s", texts[size_t(i)].c_str()); break;
            }
            if (section != "RenoDX" && chain_global(k.name)) continue;   // the layer's own keys say these
            const std::string had = ini.get(section, k.name, "\x01");
            // a key the section does not have and the value its default: left out (a layer section stays short)
            const bool is_default = (k.kind == kBool && bools[size_t(i)] == (k.def != 0.f)) ||
                                    (k.kind == kUint && uints[size_t(i)] == uint32_t(k.def)) ||
                                    (k.kind == kFloat && floats[size_t(i)] == k.def) || (k.kind == kText && texts[size_t(i)].empty());
            if (had == "\x01" && is_default && section != "RenoDX") continue;
            if (had != v) ini.set(section, k.name, v);
        }
    }
};

}  // namespace nr::ini_edit
