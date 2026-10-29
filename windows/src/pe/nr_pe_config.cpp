// dlssnr-amd.ini: the ReShade add-on's settings, and [Preprocess] for every
// route. Re-read while the game runs.
#include "nr_pe_config.hpp"
#if NR_INT4
#include "nr_ini_int4.hpp"
#endif
#include "nr_ini_network.hpp"
#include "nr_pe_log.hpp"
#include "nr_pipeline_binary.hpp"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nr::pe {
namespace {

std::string trim(std::string s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::optional<bool> parse_bool(const std::string& v) {
    const std::string s = lower(v);
    if (s == "1" || s == "true" || s == "on" || s == "yes") return true;
    if (s == "0" || s == "false" || s == "off" || s == "no") return false;
    return std::nullopt;
}

float number(const std::string& v, float lo, float hi) {
    return std::clamp(std::strtof(v.c_str(), nullptr), lo, hi);
}

uint64_t stamp_of(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

// One pass field, named as OptiScaler's Pass<N><Field> (Pass2Intensity ...).
bool set_pass_field(PassOverride& o, const std::string& field, const std::string& value) {
    if (field == "style") { const int v = std::atoi(value.c_str()); if (v >= 0 && v <= 2) o.style = v; }
    else if (field == "intensity") o.intensity = number(value, 0, 2);
    else if (field == "localstructure" || field == "local_structure") o.local_structure = number(value, 0, 2);
    else if (field == "localtone" || field == "local_tone") o.local_tone = number(value, 0, 2);
    else if (field == "skinstructure" || field == "skin_structure") o.skin_structure = number(value, -1, 2);
    else if (field == "automask" || field == "automatic_mask") { if (auto b = parse_bool(value)) o.automatic_mask = *b; }
    else return false;
    return true;
}

const char* const kCurves[] = {"none", "neutral", "reinhard", "filmic", "gt", "aces", "agx"};
const char* const kExposures[] = {"off", "auto", "fixed"};

// "Ctrl+Shift+F10" -> a virtual key and MOD_* bits; false when unreadable.
bool parse_hotkey(const std::string& text, int& vk, int& mods) {
    vk = 0; mods = 0;
    std::string rest = lower(text);
    while (!rest.empty()) {
        const auto plus = rest.find('+');
        const std::string part = trim(rest.substr(0, plus));
        rest = plus == std::string::npos ? std::string() : rest.substr(plus + 1);
        if (part == "ctrl" || part == "control") mods |= MOD_CONTROL;
        else if (part == "shift") mods |= MOD_SHIFT;
        else if (part == "alt") mods |= MOD_ALT;
        else if (vk) return false;
        else if (part.size() >= 2 && part[0] == 'f' && std::isdigit(static_cast<unsigned char>(part[1]))) {
            const int n = std::atoi(part.c_str() + 1);
            if (n < 1 || n > 24) return false;
            vk = VK_F1 + n - 1;
        } else if (part.size() == 1 && std::isalnum(static_cast<unsigned char>(part[0])))
            vk = std::toupper(static_cast<unsigned char>(part[0]));
        else if (part == "insert") vk = VK_INSERT;
        else if (part == "delete") vk = VK_DELETE;
        else if (part == "home") vk = VK_HOME;
        else if (part == "end") vk = VK_END;
        else if (part == "pageup") vk = VK_PRIOR;
        else if (part == "pagedown") vk = VK_NEXT;
        else if (part == "pause") vk = VK_PAUSE;
        else if (part == "scrolllock") vk = VK_SCROLL;
        else return false;
    }
    return vk != 0;
}

bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// The switch's cue, a WAV made here: two notes going up for on (660 -> 990 Hz), two going down for
// off (990 -> 495 Hz), 110 ms each, soft. Played on a thread of its own; winmm is loaded on first use, never
// imported, so nothing about loading this module changes.
std::vector<char> make_cue(bool on) {
    const int rate = 44100, note = rate * 11 / 100, gap = rate / 50;
    // -20 dBFS, about a system notification: quiet enough not to startle on a loud system.
    const float kLevel = 0.1f;
    const float hz[2] = {on ? 660.0f : 990.0f, on ? 990.0f : 495.0f};
    std::vector<int16_t> pcm;
    for (int k = 0; k < 2; ++k) {
        for (int i = 0; i < note; ++i) {
            const float env = std::min({1.0f, float(i) / (0.01f * rate), float(note - i) / (0.03f * rate)});
            pcm.push_back(int16_t(kLevel * 32767.0f * env * std::sin(6.2831853f * hz[k] * float(i) / float(rate))));
        }
        if (k == 0) pcm.insert(pcm.end(), size_t(gap), int16_t(0));
    }
    const uint32_t data = uint32_t(pcm.size() * 2);
    std::vector<char> wav(44 + data);
    auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
    auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
    std::memcpy(&wav[0], "RIFF", 4); put32(4, 36 + data); std::memcpy(&wav[8], "WAVEfmt ", 8);
    put32(16, 16); put16(20, 1); put16(22, 1); put32(24, uint32_t(rate)); put32(28, uint32_t(rate * 2));
    put16(32, 2); put16(34, 16); std::memcpy(&wav[36], "data", 4); put32(40, data);
    std::memcpy(&wav[44], pcm.data(), data);
    return wav;
}

// One soft note (CC_SMALL_FP8's switch cue), the same envelope and level as make_cue's.
void play_note(float hz) {
    static std::vector<char> wav;
    if (wav.empty()) {
        const int rate = 44100, note = rate * 11 / 100;
        std::vector<int16_t> pcm;
        for (int i = 0; i < note; ++i) {
            const float env = std::min({1.0f, float(i) / (0.01f * rate), float(note - i) / (0.03f * rate)});
            pcm.push_back(int16_t(0.1f * 32767.0f * env * std::sin(6.2831853f * hz * float(i) / float(rate))));
        }
        const uint32_t data = uint32_t(pcm.size() * 2);
        wav.resize(44 + data);
        auto put32 = [&](size_t at, uint32_t v) { std::memcpy(&wav[at], &v, 4); };
        auto put16 = [&](size_t at, uint16_t v) { std::memcpy(&wav[at], &v, 2); };
        std::memcpy(&wav[0], "RIFF", 4); put32(4, 36 + data); std::memcpy(&wav[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, uint32_t(rate)); put32(28, uint32_t(rate * 2));
        put16(32, 2); put16(34, 16); std::memcpy(&wav[36], "data", 4); put32(40, data);
        std::memcpy(&wav[44], pcm.data(), data);
    }
    std::thread([] {
        using PlaySoundFn = BOOL(WINAPI*)(LPCSTR, HMODULE, DWORD);
        static const PlaySoundFn play = [] {
            const HMODULE winmm = LoadLibraryA("winmm.dll");
            return winmm ? reinterpret_cast<PlaySoundFn>(GetProcAddress(winmm, "PlaySoundA")) : nullptr;
        }();
        if (play) play(wav.data(), nullptr, 0x0004 | 0x0002);   // SND_MEMORY | SND_NODEFAULT, synchronous
    }).detach();
}

void play_cue(bool on) {
    static const std::vector<char> cues[2] = {make_cue(false), make_cue(true)};
    std::thread([on] {
        using PlaySoundFn = BOOL(WINAPI*)(LPCSTR, HMODULE, DWORD);
        static const PlaySoundFn play = [] {
            const HMODULE winmm = LoadLibraryA("winmm.dll");
            return winmm ? reinterpret_cast<PlaySoundFn>(GetProcAddress(winmm, "PlaySoundA")) : nullptr;
        }();
        const DWORD kMemory = 0x0004, kNoDefault = 0x0002;   // SND_MEMORY | SND_NODEFAULT, synchronous
        if (play) play(cues[on ? 1 : 0].data(), nullptr, kMemory | kNoDefault);
    }).detach();
}

// Only while one of this process's windows has the focus.
bool focused() {
    DWORD pid = 0;
    const HWND w = GetForegroundWindow();
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}

// Parse one file into `c`. Returns false if it could not be opened. `legacy`
// is set when the file uses the old lowercase keys (before 2026-09-23).
bool parse(const std::string& path, Config& c, bool& legacy) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    int passes = c.controls.passes;
    bool section = false;
    std::string name;   // the current section, lower case
    // An old file: [Network] Model = small / original and [Int4Mixed] Enabled / Hotkey / Sound (before 0.0.7).
    bool model_old_word = false, have_hotkey = false, have_sound = false;
    int old_int4 = -1;
    std::string old_hotkey; bool old_sound = true;
    char line[512];
    while (std::fgets(line, sizeof line, file)) {
        std::string text = line;
        const auto comment = text.find_first_of(";#");
        if (comment != std::string::npos) text = text.substr(0, comment);
        text = trim(text);
        if (text.empty()) continue;
        if (text.front() == '[') {
            section = true;
            name = lower(trim(text.substr(1, text.find(']') == std::string::npos ? std::string::npos
                                                                                 : text.find(']') - 1)));
            continue;
        }
        const auto equals = text.find('=');
        if (equals == std::string::npos) continue;
        const std::string key = lower(trim(text.substr(0, equals)));
        const std::string value = trim(text.substr(equals + 1));
        if (name == "preprocess") { parse_preprocess_key(c.preprocess, key, value); continue; }
        if (name == "renodx") { parse_renodx_key(c.renodx, key, value); continue; }
        if (name == "log") {
            if (key == "enabled") { if (auto b = parse_bool(value)) c.log_enabled = *b; }
            else if (key == "clearonstart") { if (auto b = parse_bool(value)) c.log_clear = *b; }
            continue;
        }
        if (name == "network") {
            auto& n = c.network;
            if (key == "aco" || key == "aco mode" || key == "acomode") { if (auto b = parse_bool(value)) n.aco = *b; }
            else if (key == "model") {
                const int m = nr::network_choice(value);
                if (m >= 0) { n.model = m; model_old_word = lower(value) == "small" || lower(value) == "original"; }
            }
            else if (key == "hotkey") { n.hotkey = value; have_hotkey = true; }
            else if (key == "sound") { if (auto b = parse_bool(value)) { n.sound = *b; have_sound = true; } }
            else if (key == "residency") {
                const std::string v = lower(value);
                for (int r = 0; r < 4; ++r) if (v == residency_name(r)) n.residency = r;
            }
            else if (key == "motion without depth") { if (auto b = parse_bool(value)) n.motion_without_depth = *b; }
            else if (key == "resize history") { if (auto b = parse_bool(value)) n.resize_history = *b; }
            else if (key == "style grade") { if (auto b = parse_bool(value)) n.style_grade = *b; }
            else if (key == "osd") {
                const std::string v = lower(value);
                for (int o = 0; o < 3; ++o) if (v == osd_name(o)) n.osd = o;
            }
            continue;
        }
        if (name == "int4mixed") {   // the old section
            if (key == "enabled") { if (auto b = parse_bool(value)) old_int4 = *b ? 1 : 0; }
            else if (key == "hotkey") old_hotkey = value;
            else if (key == "sound") { if (auto b = parse_bool(value)) old_sound = *b; }
            continue;
        }
        if (value.empty()) continue;
        auto& k = c.controls;

        if (key == "enabled") { if (auto b = parse_bool(value)) k.enabled = *b; }
        else if (key == "applymodel" || key == "apply") { if (auto b = parse_bool(value)) k.apply_model = *b; }
        else if (key == "passes") passes = std::atoi(value.c_str());
        else if (key == "unlockpasses") { if (auto b = parse_bool(value)) c.unlock_passes = *b; }
        else if (key == "workingscale" || key == "model_scale") c.model_scale = number(value, 0.25f, 1.0f);
        else if (key == "style") { const int v = std::atoi(value.c_str()); if (v >= 0 && v <= 2) k.style = v; }
        else if (key == "intensity") k.intensity = number(value, 0, 2);
        else if (key == "localstructure" || key == "local_structure") k.local_structure = number(value, 0, 2);
        else if (key == "localtone" || key == "local_tone") k.local_tone = number(value, 0, 2);
        else if (key == "skinstructure" || key == "skin_structure") k.skin_structure = number(value, -1, 2);
        else if (key == "automask" || key == "automatic_mask") { if (auto b = parse_bool(value)) k.automatic_mask = *b; }
        else if (key == "transferstrength") k.detail_strength = number(value, 0, 2);
        else if (key == "colourstrength" || key == "colour" || key == "color") k.colour_strength = number(value, 0, 4);
        else if (key == "maxratio") k.max_ratio = number(value, 1, 8);
        else if (key == "transfer") {
            const int v = std::atoi(value.c_str());   // kEnlarge*, as the add-on menu lists them
            k.transfer = v >= 0 && v <= 2 ? v : kEnlargeMatched;
        }
        else if (key == "classicscaler" || key == "classic_scaler") {
            const std::string v = lower(value);
            if (v == "bilinear" || v == "0") k.classic_scaler = 0;
            else if (v == "catmullrom" || v == "catmull-rom" || v == "1") k.classic_scaler = 1;
            else if (v == "lanczos" || v == "lanczos3" || v == "2") k.classic_scaler = 2;
            else if (v == "fsr1" || v == "fsr" || v == "3") k.classic_scaler = 3;
        }
        else if (key == "history") c.history = number(value, 0, 1);
        else if (key == "noiseeveryframe") { if (auto b = parse_bool(value)) k.noise_every_frame = *b; }
        else if (key == "whitepoint" || key == "white_point") c.white_point = number(value, 0.01f, 100.0f);
        else if (key == "motionvectors") {
            const std::string v = lower(value);
            if (v == "vort") c.motion_vectors = 0;
            else if (v == "lumenite") c.motion_vectors = 1;
        }
        else if (key.rfind("pass", 0) == 0 && key.size() > 5 && std::isdigit(static_cast<unsigned char>(key[4]))) {
            // Pass<N><Field>, or the old pass<N>_<field>.
            size_t end = 4;
            while (end < key.size() && std::isdigit(static_cast<unsigned char>(key[end]))) ++end;
            const int n = std::atoi(key.substr(4, end - 4).c_str());
            std::string field = key.substr(end);
            if (!field.empty() && field.front() == '_') field.erase(0, 1);
            if (n >= 2 && n <= kMaxPasses) set_pass_field(c.pass[size_t(n) - 2], field, value);
        }
        // Anything else (the old module's placement/overlay/hooks/intercept/
        // menu_key/toggle_key/spoof_nvidia) is ignored and dropped on save.
    }
    std::fclose(file);
    legacy = !section;
    if (legacy && passes > 2) c.unlock_passes = true;   // the old file allowed up to 16
    c.controls.passes = std::clamp(passes, 1, c.pass_limit());
    c.resolve();
    // An old file: int4 mixed was its own section on top of the original model.
    if (model_old_word && c.network.model == nr::kSilverAardwold && old_int4 == 1) c.network.model = nr::kSilverAardwoldInt4;
    if (!have_hotkey && !old_hotkey.empty()) c.network.hotkey = old_hotkey;
    if (!have_sound) c.network.sound = old_sound;
    return true;
}

// RenoDX's SanitizeFloatKey / SanitizeEnumKey: a value outside [lo, hi] (or not a number) is the default.
float renodx_float(const std::string& v, float def, float lo, float hi) {
    char* end = nullptr;
    const float x = std::strtof(v.c_str(), &end);
    return end != v.c_str() && std::isfinite(x) && x >= lo && x <= hi ? x : def;
}
uint32_t renodx_enum(const std::string& v, uint32_t def, uint32_t lo, uint32_t hi) {
    char* end = nullptr;
    const long x = std::strtol(v.c_str(), &end, 10);
    return end != v.c_str() && x >= long(lo) && x <= long(hi) ? uint32_t(x) : def;
}

// The [Preprocess] section of `path` alone into `p`. False if it could not be opened.
bool parse_preprocess_file(const std::string& path, PreprocessConfig& p) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    std::string name;
    char line[512];
    while (std::fgets(line, sizeof line, file)) {
        std::string text = line;
        const auto comment = text.find_first_of(";#");
        if (comment != std::string::npos) text = text.substr(0, comment);
        text = trim(text);
        if (text.empty()) continue;
        if (text.front() == '[') {
            const auto close = text.find(']');
            name = lower(trim(text.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const auto equals = text.find('=');
        if (equals == std::string::npos || name != "preprocess") continue;
        parse_preprocess_key(p, lower(trim(text.substr(0, equals))), trim(text.substr(equals + 1)));
    }
    std::fclose(file);
    return true;
}

}  // namespace

bool parse_preprocess_key(PreprocessConfig& p, const std::string& key, const std::string& value) {
    auto& v = p.values;
    const std::string low = lower(value);
    if (key == "enabled") { if (auto b = parse_bool(value)) v.enabled = *b; }
    else if (key == "exposure") {
        for (int i = 0; i < 3; ++i) if (low == kExposures[i]) v.exposure = i;
    } else if (key == "exposurebias") v.bias_ev = number(value, -8, 8);
    else if (key == "curve") {
        for (int i = 0; i < 7; ++i) if (low == kCurves[i]) v.curve = i;
    } else if (key == "contrast") v.contrast = number(value, 0.5f, 2);
    else if (key == "saturation") v.saturation = number(value, 0.05f, 2);
    else if (key == "hotkey") p.hotkey = value;
    else if (key == "sound") { if (auto b = parse_bool(value)) p.sound = *b; }
    else return false;
    return true;
}

// The field behind nr_renodx_keys.hpp's key `index` (renodx_keys::Kind says its type).
void* renodx_field(RenoDXConfig& r, int index) {
    auto& v = r.values;
    switch (index) {
        case 0: return &r.enabled;
        case 1: return &v.nr_enabled;
        case 2: return &r.toggle_key;
        case 3: return &v.hook_point;
        case 4: return &v.intensity;
        case 5: return &v.style;
        case 6: return &v.local_tone;
        case 7: return &v.local_structure;
        case 8: return &v.skin_structure;
        case 9: return &v.auto_mask;
        case 10: return &v.skin_independent;
        case 11: return &v.passes;
        case 12: return &v.unlock_passes;
        case 13: return &v.chained_history;
        case 14: return &v.pass_transfer[0];
        case 15: return &v.pass_color[0];
        case 16: return &v.pass_intensity[0];
        case 17: return &v.pass_follow[0];
        case 18: return &v.pass_style[0];
        case 19: return &v.pass_local_tone[0];
        case 20: return &v.pass_local_structure[0];
        case 21: return &v.pass_skin_structure[0];
        case 22: return &v.pass_auto_mask[0];
        case 23: return &v.pass_transfer[1];
        case 24: return &v.pass_color[1];
        case 25: return &v.pass_intensity[1];
        case 26: return &v.pass_follow[1];
        case 27: return &v.pass_style[1];
        case 28: return &v.pass_local_tone[1];
        case 29: return &v.pass_local_structure[1];
        case 30: return &v.pass_skin_structure[1];
        case 31: return &v.pass_auto_mask[1];
        case 32: return &v.pass_transfer[2];
        case 33: return &v.pass_color[2];
        case 34: return &v.pass_intensity[2];
        case 35: return &v.pass_follow[2];
        case 36: return &v.pass_style[2];
        case 37: return &v.pass_local_tone[2];
        case 38: return &v.pass_local_structure[2];
        case 39: return &v.pass_skin_structure[2];
        case 40: return &v.pass_auto_mask[2];
        case 41: return &v.color_strength;
        case 42: return &v.transfer_strength;
        case 43: return &v.chroma_clamp_stops;
        case 44: return &v.transfer_mode;
        case 45: return &v.style_highlight_guard;
        case 46: return &v.detail_stability;
        case 47: return &v.resolution_scale;
        case 48: return &v.follow_input_res;
        case 49: return &v.look_upsample;
        case 50: return &v.look_mode;
        case 51: return &v.look_strength;
        case 52: return &v.look_brighten;
        case 53: return &v.look_darken;
        case 54: return &v.look_colour;
        case 55: return &v.look_hue;
        case 56: return &v.look_shadows;
        case 57: return &v.look_midtones;
        case 58: return &v.look_highlights;
        case 59: return &v.look_max_brighten;
        case 60: return &v.look_max_darken;
        case 61: return &v.look_max_colour;
        case 62: return &v.look_tone;
        case 63: return &v.look_detail;
        case 64: return &v.look_detail_radius;
        case 65: return &v.look_halo;
        case 66: return &v.look_halo_bright;
        case 67: return &v.look_stabilize;
        case 68: return &v.look_stabilize_ms;
        case 69: return &v.look_stabilize_detail;
        case 70: return &v.sdr_black_restore;
        case 71: return &v.display_pedestal;
        case 72: return &v.codec_mode;
        case 73: return &v.neural_floor_guard;
        case 74: return &v.source_encoding;
        case 75: return &v.linear_unit_nits;
        case 76: return &v.proxy_anchor_nits;
        case 77: return &v.paper_white_scale;
        case 78: return &v.diffuse_white_nits;
        case 79: return &v.pq_calibration;
        case 80: return &v.feed_mode;
        case 81: return &v.present_encoding;
        case 82: return &v.present_white_nits;
        case 83: return &v.present_white_source;
        case 84: return &v.norm_governor;
        case 85: return &v.norm_cut_aware;
        case 86: return &v.norm_attack_stops;
        case 87: return &v.norm_release_stops;
        case 88: return &v.norm_slew_stops;
        case 89: return &v.present_guides;
        case 90: return &v.depth_mode;
        case 91: return &v.mvec_scale_x;
        case 92: return &v.mvec_scale_y;
        case 93: return &r.screenshot_key;
        case 94: return &r.screenshot_source;
        case 95: return &r.screenshot_format;
        case 96: return &r.screenshot_layout;
        case 97: return &r.screenshot_jpeg_quality;
        case 98: return &r.screenshot_max_mb;
        default: return nullptr;
    }
}

bool parse_renodx_key(RenoDXConfig& r, const std::string& key, const std::string& value) {
    auto& v = r.values;
    const renodx::Settings d{};
    if (key == "enabled") { if (auto b = parse_bool(value)) r.enabled = *b; }
    else if (key == "neuraluplift") { if (auto b = parse_bool(value)) v.nr_enabled = *b; }
    else if (key == "nrtogglekey") r.toggle_key = value;
    else if (key == "nrhookpoint") v.hook_point = renodx_enum(value, d.hook_point, 0, 2);
    else if (key == "nrintensity") v.intensity = renodx_float(value, d.intensity, 0.0f, 2.0f);
    else if (key == "nrstyle") v.style = renodx_enum(value, d.style, 0, 2);
    else if (key == "nrlocaltone") v.local_tone = renodx_float(value, d.local_tone, 0.0f, 2.0f);
    else if (key == "nrlocalstructure") v.local_structure = renodx_float(value, d.local_structure, 0.0f, 2.0f);
    else if (key == "nrskinstructure") v.skin_structure = renodx_float(value, d.skin_structure, -1.0f, 1.0f);
    else if (key == "nrautomask") { if (auto b = parse_bool(value)) v.auto_mask = *b; }
    else if (key == "nrskinindependent") { if (auto b = parse_bool(value)) v.skin_independent = *b; }
    else if (key == "nrpasses") v.passes = renodx_enum(value, d.passes, 1, 30);
    else if (key == "nrunlockpasses") { if (auto b = parse_bool(value)) v.unlock_passes = *b; }
    else if (key == "nrchainedhistory") { if (auto b = parse_bool(value)) v.chained_history = *b; }
    else if (key == "nrpass2transfer") v.pass_transfer[0] = renodx_float(value, d.pass_transfer[0], 0.0f, 1.0f);
    else if (key == "nrpass2color") v.pass_color[0] = renodx_float(value, d.pass_color[0], 0.0f, 1.0f);
    else if (key == "nrpass2intensity") v.pass_intensity[0] = renodx_float(value, d.pass_intensity[0], 0.0f, 2.0f);
    else if (key == "nrpass2followpass1") { if (auto b = parse_bool(value)) v.pass_follow[0] = *b; }
    else if (key == "nrpass2style") v.pass_style[0] = renodx_enum(value, d.pass_style[0], 0, 2);
    else if (key == "nrpass2localtone") v.pass_local_tone[0] = renodx_float(value, d.pass_local_tone[0], 0.0f, 2.0f);
    else if (key == "nrpass2structure") v.pass_local_structure[0] = renodx_float(value, d.pass_local_structure[0], 0.0f, 2.0f);
    else if (key == "nrpass2skin") v.pass_skin_structure[0] = renodx_float(value, d.pass_skin_structure[0], -1.0f, 1.0f);
    else if (key == "nrpass2automask") { if (auto b = parse_bool(value)) v.pass_auto_mask[0] = *b; }
    else if (key == "nrpass3transfer") v.pass_transfer[1] = renodx_float(value, d.pass_transfer[1], 0.0f, 1.0f);
    else if (key == "nrpass3color") v.pass_color[1] = renodx_float(value, d.pass_color[1], 0.0f, 1.0f);
    else if (key == "nrpass3intensity") v.pass_intensity[1] = renodx_float(value, d.pass_intensity[1], 0.0f, 2.0f);
    else if (key == "nrpass3followpass1") { if (auto b = parse_bool(value)) v.pass_follow[1] = *b; }
    else if (key == "nrpass3style") v.pass_style[1] = renodx_enum(value, d.pass_style[1], 0, 2);
    else if (key == "nrpass3localtone") v.pass_local_tone[1] = renodx_float(value, d.pass_local_tone[1], 0.0f, 2.0f);
    else if (key == "nrpass3structure") v.pass_local_structure[1] = renodx_float(value, d.pass_local_structure[1], 0.0f, 2.0f);
    else if (key == "nrpass3skin") v.pass_skin_structure[1] = renodx_float(value, d.pass_skin_structure[1], -1.0f, 1.0f);
    else if (key == "nrpass3automask") { if (auto b = parse_bool(value)) v.pass_auto_mask[1] = *b; }
    else if (key == "nrpass4transfer") v.pass_transfer[2] = renodx_float(value, d.pass_transfer[2], 0.0f, 1.0f);
    else if (key == "nrpass4color") v.pass_color[2] = renodx_float(value, d.pass_color[2], 0.0f, 1.0f);
    else if (key == "nrpass4intensity") v.pass_intensity[2] = renodx_float(value, d.pass_intensity[2], 0.0f, 2.0f);
    else if (key == "nrpass4followpass1") { if (auto b = parse_bool(value)) v.pass_follow[2] = *b; }
    else if (key == "nrpass4style") v.pass_style[2] = renodx_enum(value, d.pass_style[2], 0, 2);
    else if (key == "nrpass4localtone") v.pass_local_tone[2] = renodx_float(value, d.pass_local_tone[2], 0.0f, 2.0f);
    else if (key == "nrpass4structure") v.pass_local_structure[2] = renodx_float(value, d.pass_local_structure[2], 0.0f, 2.0f);
    else if (key == "nrpass4skin") v.pass_skin_structure[2] = renodx_float(value, d.pass_skin_structure[2], -1.0f, 1.0f);
    else if (key == "nrpass4automask") { if (auto b = parse_bool(value)) v.pass_auto_mask[2] = *b; }
    else if (key == "nrcolorstrength") v.color_strength = renodx_float(value, d.color_strength, 0.0f, 1.0f);
    else if (key == "nrtransferstrength") v.transfer_strength = renodx_float(value, d.transfer_strength, 0.0f, 1.0f);
    else if (key == "nrchromaclamp") v.chroma_clamp_stops = renodx_float(value, d.chroma_clamp_stops, 0.25f, 2.0f);
    else if (key == "nrtransfermode") v.transfer_mode = renodx_enum(value, d.transfer_mode, 0, 1);
    else if (key == "nrstylehighlightguard") v.style_highlight_guard = renodx_float(value, d.style_highlight_guard, 0.0f, 2.0f);
    else if (key == "nrdetailstability") v.detail_stability = renodx_enum(value, d.detail_stability, 0, 2);
    else if (key == "nrresolutionscale") v.resolution_scale = renodx_float(value, d.resolution_scale, 0.33f, 1.0f);
    else if (key == "nrfollowinputres") { if (auto b = parse_bool(value)) v.follow_input_res = *b; }
    else if (key == "nrlookupsample") v.look_upsample = renodx_enum(value, d.look_upsample, 0, 3);
    else if (key == "nrlookmode") { if (auto b = parse_bool(value)) v.look_mode = *b; }
    else if (key == "nrlookstrength") v.look_strength = renodx_float(value, d.look_strength, 0.0f, 2.0f);
    else if (key == "nrlookbrighten") v.look_brighten = renodx_float(value, d.look_brighten, 0.0f, 2.0f);
    else if (key == "nrlookdarken") v.look_darken = renodx_float(value, d.look_darken, 0.0f, 2.0f);
    else if (key == "nrlookcolour") v.look_colour = renodx_float(value, d.look_colour, 0.0f, 2.0f);
    else if (key == "nrlookhue") v.look_hue = renodx_float(value, d.look_hue, 0.0f, 2.0f);
    else if (key == "nrlookshadows") v.look_shadows = renodx_float(value, d.look_shadows, 0.0f, 2.0f);
    else if (key == "nrlookmidtones") v.look_midtones = renodx_float(value, d.look_midtones, 0.0f, 2.0f);
    else if (key == "nrlookhighlights") v.look_highlights = renodx_float(value, d.look_highlights, 0.0f, 2.0f);
    else if (key == "nrlookmaxbrighten") v.look_max_brighten = renodx_float(value, d.look_max_brighten, 0.0f, 4.0f);
    else if (key == "nrlookmaxdarken") v.look_max_darken = renodx_float(value, d.look_max_darken, 0.0f, 4.0f);
    else if (key == "nrlookmaxcolour") v.look_max_colour = renodx_float(value, d.look_max_colour, 0.0f, 2.0f);
    else if (key == "nrlooktone") v.look_tone = renodx_float(value, d.look_tone, 0.0f, 2.0f);
    else if (key == "nrlookdetail") v.look_detail = renodx_float(value, d.look_detail, 0.0f, 2.0f);
    else if (key == "nrlookdetailradius") v.look_detail_radius = renodx_float(value, d.look_detail_radius, 0.25f, 4.0f);
    else if (key == "nrlookhalo") v.look_halo = renodx_float(value, d.look_halo, 0.0f, 1.0f);
    else if (key == "nrlookhalobrightside") v.look_halo_bright = renodx_float(value, d.look_halo_bright, 0.0f, 1.0f);
    else if (key == "nrlookstabilize") v.look_stabilize = renodx_enum(value, d.look_stabilize, 0, 2);
    else if (key == "nrlookstabilizems") v.look_stabilize_ms = renodx_float(value, d.look_stabilize_ms, 10.0f, 250.0f);
    else if (key == "nrlookstabilizedetail") { if (auto b = parse_bool(value)) v.look_stabilize_detail = *b; }
    else if (key == "nrsdrblackrestore") v.sdr_black_restore = renodx_enum(value, d.sdr_black_restore, 0, 2);
    else if (key == "nrdisplaypedestal") v.display_pedestal = renodx_enum(value, d.display_pedestal, 0, 2);
    else if (key == "nrcodecmode") v.codec_mode = renodx_enum(value, d.codec_mode, 0, 3);
    else if (key == "nrneuralfloorguard") { if (auto b = parse_bool(value)) v.neural_floor_guard = *b; }
    else if (key == "nrsourceencoding") v.source_encoding = renodx_enum(value, d.source_encoding, 0, 3);
    else if (key == "nrlinearunitnits") v.linear_unit_nits = renodx_float(value, d.linear_unit_nits, 0.0f, 10000.0f);
    else if (key == "nrproxyanchor") v.proxy_anchor_nits = renodx_float(value, d.proxy_anchor_nits, 0.5f, 32.0f);
    else if (key == "nrpaperwhitescale") v.paper_white_scale = renodx_float(value, d.paper_white_scale, 0.005f, 16.0f);
    else if (key == "nrdiffusewhitenits") v.diffuse_white_nits = renodx_float(value, d.diffuse_white_nits, 80.0f, 1000.0f);
    else if (key == "nrpqcalibration") v.pq_calibration = renodx_float(value, d.pq_calibration, 0.005f, 16.0f);
    else if (key == "nrfeedmode") v.feed_mode = renodx_enum(value, d.feed_mode, 0, 2);
    else if (key == "nrpresentencoding") v.present_encoding = renodx_enum(value, d.present_encoding, 0, 3);
    else if (key == "nrpresentwhitenits") v.present_white_nits = renodx_float(value, d.present_white_nits, 0.0f, 10000.0f);
    else if (key == "nrpresentwhitesource") v.present_white_source = renodx_enum(value, d.present_white_source, 0, 1);
    else if (key == "nrnormgovernor") v.norm_governor = renodx_enum(value, d.norm_governor, 0, 2);
    else if (key == "nrnormcutaware") { if (auto b = parse_bool(value)) v.norm_cut_aware = *b; }
    else if (key == "nrnormattackstops") v.norm_attack_stops = renodx_float(value, d.norm_attack_stops, 0.1f, 64.0f);
    else if (key == "nrnormreleasestops") v.norm_release_stops = renodx_float(value, d.norm_release_stops, 0.05f, 16.0f);
    else if (key == "nrnormslewstops") v.norm_slew_stops = renodx_float(value, d.norm_slew_stops, 0.0f, 0.5f);
    else if (key == "nrpresentguides") v.present_guides = renodx_enum(value, d.present_guides, 0, 2);
    else if (key == "nrdepthmode") v.depth_mode = renodx_enum(value, d.depth_mode, 0, 2);
    else if (key == "nrmvecscalex") v.mvec_scale_x = renodx_float(value, d.mvec_scale_x, -2.0f, 2.0f);
    else if (key == "nrmvecscaley") v.mvec_scale_y = renodx_float(value, d.mvec_scale_y, -2.0f, 2.0f);
    else if (key == "nrscreenshotkey") r.screenshot_key = value;
    else if (key == "nrscreenshotsource") r.screenshot_source = renodx_enum(value, r.screenshot_source, 0, 2);
    else if (key == "nrscreenshotformat") r.screenshot_format = renodx_enum(value, r.screenshot_format, 0, 2);
    else if (key == "nrscreenshotlayout") r.screenshot_layout = renodx_enum(value, r.screenshot_layout, 0, 1);
    else if (key == "nrscreenshotjpegquality") r.screenshot_jpeg_quality = renodx_enum(value, r.screenshot_jpeg_quality, 80, 100);
    else if (key == "nrscreenshotmaxmb") r.screenshot_max_mb = renodx_enum(value, r.screenshot_max_mb, 0, 500);
    else return false;
    return true;
}

void write_renodx(FILE* f, const RenoDXConfig& r) {
    const auto& v = r.values;
    std::fprintf(f,
        "\n[RenoDX]\n"
        "; RenoDX style: the processing around NR (how the picture is handed to NR and how NR's result is put back) follows RenoDX's DLSS5 add-on\n"
        "; v8.5.0-rc10, with its own shaders (MIT). The NR network itself is the same.\n"
        "; With the RenoDX style on, every NR setting is in this section (End in game opens NR Control to change them); the NR settings in the OptiScaler menu\n"
        "; and in [DlssNr] have no effect, OptiScaler only does upscaling and frame generation; off (Enabled = 0), everything is as before.\n"
        "; The keys other than Enabled keep RenoDX's names, defaults and ranges; a value out of range is taken as the default.\n"
        "; Enabled applies after a game restart; the other keys apply when saved, also in game.\n"
        "\n"
        "Enabled = %d\n"
        "; 0 = off, no RenoDX processing (default)\n"
        "; 1 = on\n"
        "\n"
        "NeuralUplift = %d\n"
        "; 1 = NR on (default); 0 = off, the picture passes through unchanged\n"
        "\n"
        "NRToggleKey = %s\n"
        "; hotkey that switches NR on / off in game (for the current run only, not written back), default F6; empty = none\n"
        "\n"
        "NRHookPoint = %u\n"
        "; OptiScaler route only: where NR runs. 0 = after the upscaler (default); 1 = before the upscaler (after it when the game\n"
        "; uses Ray Reconstruction); 2 = Present (where the ReShade routes run; taken as 0 on the OptiScaler route)\n"
        "\n"
        "; the network's own settings (pass 1)\n"
        "\n"
        "NRIntensity = %.2f\n"
        "; 0 .. 2, default 1. Strength of the enhancement\n"
        "\n"
        "NRStyle = %u\n"
        "; 0 = Standard (default); 1 = Natural; 2 = Cinematic\n"
        "\n"
        "NRLocalTone = %.2f\n"
        "; 0 .. 2, default 1. Local tone\n"
        "\n"
        "NRLocalStructure = %.2f\n"
        "; 0 .. 2, default 1. Local structure\n"
        "\n"
        "NRSkinStructure = %.2f\n"
        "; -1 .. 1, default -1 (unchanged). Structure on skin, only with NRAutoMask = 1\n"
        "\n"
        "NRAutoMask = %d\n"
        "; 1 = automatic mask (Character mask): skin is found and treated on its own; 0 = off (default)\n"
        "\n"
        "NRSkinIndependent = %d\n"
        "; 1 = with the automatic mask on, skin follows only NRSkinStructure, not NRLocalStructure; NRLocalStructure then reaches\n"
        "; the network as at most 1 (above 1 the network ignores the skin setting) (default); 0 = both values go to the network as they are\n"
        "\n"
        "; Passes (stacking): how many times the network runs a frame; each pass hands the previous pass's result to the network again and puts it back with this pass's strength.\n"
        "; Every pass costs one more network run; the frame rate drops noticeably.\n"
        "\n"
        "NRPasses = %u\n"
        "; 1 .. 4, default 1; 1 .. 30 with NRUnlockPasses = 1\n"
        "NRUnlockPasses = %d\n"
        "; 1 = NRPasses may go above 4, up to 30 (as OptiScaler's UnlockPasses; RenoDX itself stops at 4); passes 5 and later take pass 4's settings.\n"
        "; Every pass runs the network once more, which lowers the frame rate a lot. 0 = off (default)\n"
        "\n"
        "NRChainedHistory = %d\n"
        "; 1 = from pass 2 on every pass keeps its own history, restarted only on scene cuts and the like (default); 0 = from pass 2 on, every frame starts afresh\n"
        "; (RenoDX's diagnostic mode, may flicker, for comparison only)\n"
        "\n"
        "; settings of passes 2, 3 and 4 (passes after 4 use pass 4's):\n"
        "; Transfer / Color: how strongly this pass is put back, as NRTransferStrength / NRColorStrength, 0 .. 1, default 1;\n"
        "; Intensity: this pass's strength of the enhancement, 0 .. 2, default 1;\n"
        "; FollowPass1: 1 = this pass's network settings are pass 1's (default); 0 = this pass's own Style / LocalTone / Structure /\n"
        "; Skin / AutoMask (meaning as for pass 1 above)\n"
        "\n"
        "NRPass2Transfer = %.2f\n"
        "NRPass2Color = %.2f\n"
        "NRPass2Intensity = %.2f\n"
        "NRPass2FollowPass1 = %d\n"
        "NRPass2Style = %u\n"
        "NRPass2LocalTone = %.2f\n"
        "NRPass2Structure = %.2f\n"
        "NRPass2Skin = %.2f\n"
        "NRPass2AutoMask = %d\n"
        "\n"
        "NRPass3Transfer = %.2f\n"
        "NRPass3Color = %.2f\n"
        "NRPass3Intensity = %.2f\n"
        "NRPass3FollowPass1 = %d\n"
        "NRPass3Style = %u\n"
        "NRPass3LocalTone = %.2f\n"
        "NRPass3Structure = %.2f\n"
        "NRPass3Skin = %.2f\n"
        "NRPass3AutoMask = %d\n"
        "\n"
        "NRPass4Transfer = %.2f\n"
        "NRPass4Color = %.2f\n"
        "NRPass4Intensity = %.2f\n"
        "NRPass4FollowPass1 = %d\n"
        "NRPass4Style = %u\n"
        "NRPass4LocalTone = %.2f\n"
        "NRPass4Structure = %.2f\n"
        "NRPass4Skin = %.2f\n"
        "NRPass4AutoMask = %d\n"
        "\n"
        "; how NR's result is put back into the picture\n"
        "\n"
        "NRColorStrength = %.2f\n"
        "; 0 .. 1, default 1. Float HDR pictures: how much of NR's change is used (0 = the original picture); SDR and HDR10 pictures: how much of NR's colour is used\n"
        "; (0 = only NR's change of brightness)\n"
        "\n"
        "NRTransferStrength = %.2f\n"
        "; 0 .. 1, default 1. Strength of NR's change on HDR10 pictures, and of the HDR shadow restore\n"
        "\n"
        "NRChromaClamp = %.2f\n"
        "; 0.25 .. 2 stops, default 1. How far NR may change a colour on HDR pictures\n"
        "\n"
        "NRTransferMode = %u\n"
        "; HDR pictures: 0 = by the ratio before / after NR only (default); 1 = also compensated by the slope of the display curve, less of the change is lost in bright areas\n"
        "\n"
        "NRStyleHighlightGuard = %.2f\n"
        "; 0 .. 2 stops, default 0 (off). How far NR may darken highlights on HDR pictures\n"
        "\n"
        "NRDetailStability = %u\n"
        "; smooths NR's change over time along the motion vectors, less flickering detail: 0 = auto (on after the upscaler with motion vectors, default); 1 = off; 2 = always on\n"
        "\n"
        "NRResolutionScale = %.2f\n"
        "; the NR network's resolution, 0.33 .. 1 of the picture, default 1. Below 1 the network is faster; the change is enlarged back to the picture by NRLookUpsample below\n"
        "\n"
        "NRFollowInputRes = %d\n"
        "; 1 = the NR network runs at the game's render resolution (the motion vectors' resolution) instead of NRResolutionScale; 0 = off (default)\n"
        "\n"
        "NRLookUpsample = %u\n"
        "; with the network below the picture's resolution, how the change is enlarged back: 0 = auto (edge-aware, default); 1 = edge-aware; 2 = classic (plain enlargement);\n"
        "; 3 = edge-aware, with SDR pictures reduced by area for the network (a change restarts the network's history)\n"
        "\n"
        "NRLookMode = %d\n"
        "; RenoDX's Adjust NR's look: adjusts how much NR changed, directly on its result; changes apply at once, the network does not restart.\n"
        "; 1 = on, the NRLook* keys below apply; 0 = off, NR's result is used as it is (default). Every NRLook* default leaves the result as it is\n"
        "\n"
        "NRLookStrength = %.2f\n"
        "; 0 .. 2, default 1. NR's whole change (in stops): 0 = removed, 1 = kept, 2 = doubled\n"
        "\n"
        "NRLookBrighten = %.2f\n"
        "; 0 .. 2, default 1. How much of NR's brightening is kept: 0 = none, 2 = doubled\n"
        "\n"
        "NRLookDarken = %.2f\n"
        "; 0 .. 2, default 1. How much of NR's darkening is kept (including the dark halos it leaves around bright objects)\n"
        "\n"
        "NRLookColour = %.2f\n"
        "; 0 .. 2, default 1. How far NR moves colours towards or away from grey: 0 = keep the game's saturation, 2 = doubled\n"
        "\n"
        "NRLookHue = %.2f\n"
        "; 0 .. 2, default 1. How far NR turns hues (colour casts): 0 = keep the game's hues\n"
        "\n"
        "NRLookShadows = %.2f\n"
        "; 0 .. 2, default 1. Multiplier of the change in the game picture's dark areas\n"
        "\n"
        "NRLookMidtones = %.2f\n"
        "; 0 .. 2, default 1. Multiplier of the change in midtones\n"
        "\n"
        "NRLookHighlights = %.2f\n"
        "; 0 .. 2, default 1. Multiplier of the change in bright areas\n"
        "\n"
        "NRLookMaxBrighten = %.2f\n"
        "; 0 .. 4 stops, default 0 (no limit). How far NR may brighten a pixel (soft limit)\n"
        "\n"
        "NRLookMaxDarken = %.2f\n"
        "; 0 .. 4 stops, default 0 (no limit). How far NR may darken a pixel (soft limit)\n"
        "\n"
        "NRLookMaxColour = %.2f\n"
        "; 0 .. 2 stops, default 0 (no limit). How far NR may change a colour (soft limit)\n"
        "\n"
        "NRLookTone = %.2f\n"
        "; 0 .. 2, default 1. Multiplier of NR's broad lighting changes (fine ones: NRLookDetail)\n"
        "\n"
        "NRLookDetail = %.2f\n"
        "; 0 .. 2, default 1. Multiplier of NR's fine detail changes\n"
        "\n"
        "NRLookDetailRadius = %.2f\n"
        "; 0.25 .. 4, default 1. Where broad turns into fine, in percent of the network picture's height\n"
        "\n"
        "NRLookHalo = %.2f\n"
        "; 0 .. 1, default 0. Weakens the dark halos NR leaves beside bright objects; even darkening of whole areas and shadows beside dark objects stay.\n"
        "; The larger NRLookDetailRadius, the wider the halos it handles\n"
        "\n"
        "NRLookHaloBrightSide = %.2f\n"
        "; 0 .. 1, default 0. Weakens the soft dark edge NR leaves on the brighter side of dark objects (deeper with every pass)\n"
        "\n"
        "NRLookStabilize = %u\n"
        "; smooths NR's broad changes over time: 0 = off (default); 1 = static (for still pictures); 2 = along the motion vectors.\n"
        "; When on, it replaces NRDetailStability; lighting changes lag slightly\n"
        "\n"
        "NRLookStabilizeMs = %.0f\n"
        "; 10 .. 250 ms, default 60. How long the smoothing remembers: longer is steadier and follows lighting changes more slowly\n"
        "\n"
        "NRLookStabilizeDetail = %d\n"
        "; 1 = fine detail is smoothed too; 0 = broad changes only (default)\n"
        "\n"
        "NRSdrBlackRestore = %u\n"
        "; SDR pictures, taking back NR's lift in dark areas: 0 = near-black areas only (default); 1 = all areas; 2 = as 0, with a smooth transition at the threshold\n"
        "\n"
        "NRDisplayPedestal = %u\n"
        "; HDR pictures, shadow restore: 0 = auto (none on float HDR, default); 1 = always; 2 = as 0, with a smooth transition at the threshold\n"
        "\n"
        "NRCodecMode = %u\n"
        "; HDR encoding: 0 = classic; 1 = anchored; 2 = auto (default); 3 = display\n"
        "\n"
        "NRNeuralFloorGuard = %d\n"
        "; 1 = protects colours where NR's result is near black (default); 0 = off\n"
        "\n"
        "; the picture's encoding and brightness\n"
        "\n"
        "NRSourceEncoding = %u\n"
        "; what the picture's values mean: 0 = from the format (default); 1 = SDR; 2 = linear light; 3 = PQ\n"
        "\n"
        "NRLinearUnitNits = %.1f\n"
        "; 0 .. 10000, default 0. Above 0, float pictures are taken in absolute units: 1.0 = this many nits\n"
        "\n"
        "NRProxyAnchor = %.2f\n"
        "; 0.5 .. 32 nits, default 4. Encoding anchor of absolute-unit HDR\n"
        "\n"
        "NRPaperWhiteScale = %.4f\n"
        "; 0.005 .. 16, default 2.5375 (203/80)\n"
        "\n"
        "NRDiffuseWhiteNits = %.1f\n"
        "; 80 .. 1000, default 203. Diffuse white of PQ pictures\n"
        "\n"
        "NRPQCalibration = %.3f\n"
        "; 0.005 .. 16, default 1\n"
        "\n"
        "NRFeedMode = %u\n"
        "; what float HDR pictures are divided by before NR: 0 = auto (the game's exposure when it hands over a readable exposure texture without AutoExposure,\n"
        "; range-checked, otherwise computed from the picture, default); 1 = always computed from the picture; 2 = always the game's exposure (1 without a texture)\n"
        "; The ReShade routes have no game exposure and always compute it from the picture\n"
        "\n"
        "NRPresentEncoding = %u\n"
        "; ReShade routes only: the picture's encoding. 0 = from the swap chain's colour space (default); 1 = SDR; 2 = HDR10; 3 = scRGB\n"
        "\n"
        "NRPresentWhiteNits = %.1f\n"
        "; HDR pictures on the ReShade routes only: diffuse white in nits. 0 = auto (default; a RenoDX mod's Game Brightness,\n"
        "; Windows' SDR content brightness, NRDiffuseWhiteNits, in this order)\n"
        "\n"
        "NRPresentWhiteSource = %u\n"
        "; ReShade routes only: 0 = auto (default); 1 = do not take a RenoDX mod's Game Brightness\n"
        "\n"
        "NRNormGovernor = %u\n"
        "; how the automatic divisor of relative-unit HDR follows the picture: 0 = taken every frame as it is; 1 = rate-limited every frame; 2 = steady (default)\n"
        "\n"
        "NRNormCutAware = %d\n"
        "; 1 = the automatic divisor restarts at once on the game's scene cuts (Reset) (default); 0 = off\n"
        "\n"
        "NRNormAttackStops = %.2f\n"
        "; 0.1 .. 64, default 4. In steady mode, how many stops a second the divisor may grow (the picture NR sees gets darker)\n"
        "\n"
        "NRNormReleaseStops = %.2f\n"
        "; 0.05 .. 16, default 0.5. In steady mode, how many stops a second the divisor may shrink\n"
        "\n"
        "NRNormSlewStops = %.3f\n"
        "; 0 .. 0.5, default 0.02. In rate-limited mode, how many stops a frame at most\n"
        "\n"
        "; depth and motion vectors\n"
        "\n"
        "NRPresentGuides = %u\n"
        "; ReShade routes only: 0 = motion vectors (DLSS5_Feed) when there are some, neutral ones (no motion, no depth) otherwise, NR runs either way (default);\n"
        "; 1 = no NR without motion vectors; 2 = always neutral ones (games whose motion vectors are not used)\n"
        "\n"
        "NRDepthMode = %u\n"
        "; depth direction: 0 = by the game's flag (default); 1 = taken as normal depth; 2 = taken as reversed depth\n"
        "\n"
        "NRMVecScaleX = %.3f\n"
        "; -2 .. 2, default 1. Multiplier of the game's motion vectors in X (for games that hand over wrong ones)\n"
        "\n"
        "NRMVecScaleY = %.3f\n"
        "; -2 .. 2, default 1. Multiplier of the game's motion vectors in Y\n"
        "\n"
        "; screenshots: the same frame without and with NR (RenoDX's NR screenshot), saved to the NR Screenshots folder in the game folder\n"
        "\n"
        "NRScreenshotKey = %s\n"
        "; screenshot hotkey, default F5; empty = none (NR Control has a screenshot button too)\n"
        "\n"
        "NRScreenshotSource = %u\n"
        "; what is captured: 0 = auto (default, as 1); 1 = the final picture (on the ReShade routes the picture sent to the display; on the OptiScaler route\n"
        "; NR moves to OptiScaler's finished picture for one frame); 2 = the model's input and output (for diagnosis, without the game's tone mapping and UI)\n"
        "\n"
        "NRScreenshotFormat = %u\n"
        "; 0 = PNG (default, lossless; HDR as 16-bit PQ, kept HDR); 1 = JPEG; 2 = both\n"
        "\n"
        "NRScreenshotLayout = %u\n"
        "; 0 = two pictures (default); 1 = also a side-by-side picture (left NR off, right NR on)\n"
        "\n"
        "NRScreenshotJpegQuality = %u\n"
        "; 80 .. 100, default 95. JPEG quality\n"
        "\n"
        "NRScreenshotMaxMB = %u\n"
        "; 0 .. 500, default 10. Above this many MB a JPEG's quality is lowered (to 85 at the least, the picture is not scaled down); 0 = no limit\n",
        r.enabled ? 1 : 0, v.nr_enabled ? 1 : 0, r.toggle_key.c_str(), v.hook_point, v.intensity, v.style,
        v.local_tone, v.local_structure, v.skin_structure, v.auto_mask ? 1 : 0, v.skin_independent ? 1 : 0, v.passes,
        v.unlock_passes ? 1 : 0, v.chained_history ? 1 : 0, v.pass_transfer[0], v.pass_color[0], v.pass_intensity[0],
        v.pass_follow[0] ? 1 : 0, v.pass_style[0], v.pass_local_tone[0], v.pass_local_structure[0],
        v.pass_skin_structure[0], v.pass_auto_mask[0] ? 1 : 0, v.pass_transfer[1], v.pass_color[1],
        v.pass_intensity[1], v.pass_follow[1] ? 1 : 0, v.pass_style[1], v.pass_local_tone[1],
        v.pass_local_structure[1], v.pass_skin_structure[1], v.pass_auto_mask[1] ? 1 : 0, v.pass_transfer[2],
        v.pass_color[2], v.pass_intensity[2], v.pass_follow[2] ? 1 : 0, v.pass_style[2], v.pass_local_tone[2],
        v.pass_local_structure[2], v.pass_skin_structure[2], v.pass_auto_mask[2] ? 1 : 0, v.color_strength,
        v.transfer_strength, v.chroma_clamp_stops, v.transfer_mode, v.style_highlight_guard, v.detail_stability,
        v.resolution_scale, v.follow_input_res ? 1 : 0, v.look_upsample, v.look_mode ? 1 : 0, v.look_strength,
        v.look_brighten, v.look_darken, v.look_colour, v.look_hue, v.look_shadows, v.look_midtones,
        v.look_highlights, v.look_max_brighten, v.look_max_darken, v.look_max_colour, v.look_tone, v.look_detail,
        v.look_detail_radius, v.look_halo, v.look_halo_bright, v.look_stabilize, v.look_stabilize_ms,
        v.look_stabilize_detail ? 1 : 0, v.sdr_black_restore, v.display_pedestal, v.codec_mode,
        v.neural_floor_guard ? 1 : 0, v.source_encoding, v.linear_unit_nits, v.proxy_anchor_nits,
        v.paper_white_scale, v.diffuse_white_nits, v.pq_calibration, v.feed_mode, v.present_encoding,
        v.present_white_nits, v.present_white_source, v.norm_governor, v.norm_cut_aware ? 1 : 0, v.norm_attack_stops,
        v.norm_release_stops, v.norm_slew_stops, v.present_guides, v.depth_mode, v.mvec_scale_x, v.mvec_scale_y,
        r.screenshot_key.c_str(), r.screenshot_source, r.screenshot_format, r.screenshot_layout,
        r.screenshot_jpeg_quality, r.screenshot_max_mb);
}

bool KeyPress::frame(const std::string& key_text) {
    if (key_text != key_text_) {
        key_text_ = key_text;
        vk_ = 0;
        if (!key_text_.empty() && !parse_hotkey(key_text_, vk_, mods_)) {
            log("[nr] RenoDX-style option: NRScreenshotKey \"%s\" not understood; no hotkey", key_text_.c_str());
            vk_ = 0;
        }
    }
    if (!vk_) return false;
    const bool down = key_down(vk_) && ((mods_ & MOD_CONTROL) != 0) == key_down(VK_CONTROL) &&
                      ((mods_ & MOD_SHIFT) != 0) == key_down(VK_SHIFT) && ((mods_ & MOD_ALT) != 0) == key_down(VK_MENU);
    const bool pressed = down && !down_ && focused();
    down_ = down;
    return pressed;
}

bool NrToggle::frame(const RenoDXConfig& file) {
    if (file.toggle_key != key_text_) {
        key_text_ = file.toggle_key;
        vk_ = 0;
        if (!key_text_.empty() && !parse_hotkey(key_text_, vk_, mods_)) {
            log("[nr] RenoDX-style option: NRToggleKey \"%s\" not understood; no hotkey", key_text_.c_str());
            vk_ = 0;
        }
    }
    if (file.values.nr_enabled != file_on_) { file_on_ = file.values.nr_enabled; have_override_ = false; }
    if (vk_) {
        const bool down = key_down(vk_) && ((mods_ & MOD_CONTROL) != 0) == key_down(VK_CONTROL) &&
                          ((mods_ & MOD_SHIFT) != 0) == key_down(VK_SHIFT) &&
                          ((mods_ & MOD_ALT) != 0) == key_down(VK_MENU);
        if (down && !down_ && focused()) {
            override_ = !(have_override_ ? override_ : file_on_);
            have_override_ = true;
        }
        down_ = down;
    }
    const bool on = have_override_ ? override_ : file_on_;
    if (logged_ && on != last_) log("[nr] RenoDX-style option: NR %s", on ? "on" : "off");
    logged_ = true;
    last_ = on;
    return on;
}

bool read_renodx(const std::string& path, RenoDXConfig& out) {
    FILE* file = std::fopen(path.c_str(), "r");
    if (!file) return false;
    RenoDXConfig r{};
    std::string name;
    char line[512];
    while (std::fgets(line, sizeof line, file)) {
        std::string text = line;
        const auto comment = text.find_first_of(";#");
        if (comment != std::string::npos) text = text.substr(0, comment);
        text = trim(text);
        if (text.empty()) continue;
        if (text.front() == '[') {
            const auto close = text.find(']');
            name = lower(trim(text.substr(1, close == std::string::npos ? std::string::npos : close - 1)));
            continue;
        }
        const auto equals = text.find('=');
        if (equals == std::string::npos || name != "renodx") continue;
        parse_renodx_key(r, lower(trim(text.substr(0, equals))), trim(text.substr(equals + 1)));
    }
    std::fclose(file);
    out = r;
    return true;
}

bool RenoDXFile::poll(const std::string& path, RenoDXConfig& out) {
    const uint64_t now = GetTickCount64();
    if (stamp_ && now - checked_ms_ < 1000) return false;
    checked_ms_ = now;
    const uint64_t stamp = stamp_of(path);
    if (!stamp || stamp == stamp_) return false;
    RenoDXConfig next{};
    if (!read_renodx(path, next)) return false;
    stamp_ = stamp;
    out = next;
    return true;
}

std::string describe(const Preprocess& p) {
    char text[160];
    std::snprintf(text, sizeof text, "exposure %s, ExposureBias %+.2f EV, curve %s, contrast %.2f, saturation %.2f",
                  kExposures[std::clamp(p.exposure, 0, 2)], p.exposure == 0 ? 0.0f : p.bias_ev,
                  kCurves[std::clamp(p.curve, 0, 6)], p.contrast, p.saturation);
    return text;
}

const char* osd_name(int o) {
    static const char* const names[] = {"off", "switch", "always"};
    return names[o >= 0 && o < 3 ? o : 1];
}

const char* residency_name(int r) {
    static const char* const names[4] = {"auto", "resident", "offload", "minimal"};
    return names[std::clamp(r, 0, 3)];
}

void write_network(FILE* f, const NetworkConfig& n) {
    std::fprintf(f,
        "[Network]\n"
        "; Which network NR runs. It can be changed in game at any time: the drop-down in NR Control (OptiScaler\n"
        "; routes, opened with End) or on ReShade's Add-ons page, or the Hotkey; a change is written here, so the next\n"
        "; game start uses it.\n"
        "\n"
        "Model = %s\n"
        "; CC_SMALL_FP8                  = the default network of NVIDIA 310.9.16, the fastest\n"
        "; CC_SILVER_AARDWOLD            = the network of 310.8.0 (also preset 1 of 310.9.16)\n"
        "; CC_SILVER_AARDWOLD_INT4_MIXED = CC_SILVER_AARDWOLD with part of the work in int4 and other lower precisions;\n"
        ";                                 faster, the picture differs somewhat; needs ACO Mode = 1 (below)\n"
        "; A network that cannot run (e.g. no 310.9.16 model file) is greyed out in the menus with the reason, and the\n"
        "; hotkey skips it.\n"
        "\n"
        "Hotkey = %s\n"
        "; Cycles through the networks that can run, to compare them on the same picture. Empty = no hotkey. Ctrl,\n"
        "; Shift, Alt and one key, e.g. Alt+F9\n"
        "\n"
        "Sound = %d\n"
        "; A sound when the network changes (hotkey, menu or this file): one note = CC_SMALL_FP8, two notes going down =\n"
        "; CC_SILVER_AARDWOLD, two notes going up = CC_SILVER_AARDWOLD_INT4_MIXED. 0 = silent\n"
        "\n"
        "Residency = %s\n"
        "; What is kept of a network that is no longer running. The three networks share one activation memory, so\n"
        "; what is kept is mainly the weights. When switching back, the current network keeps running until the other\n"
        "; one is ready.\n"
        "; auto     = only the weights stay in video memory, switching back is quick; when video memory runs short\n"
        ";            they move to RAM, and are freed when RAM runs short too (default)\n"
        "; resident = the whole network stays in video memory; switching back is instant, uses the most video memory\n"
        "; offload  = the weights move to RAM and all video memory is freed; switching back copies them back\n"
        "; minimal  = video memory and RAM are freed; switching back builds the network again\n"
        "; NR off for more than 10 s: offload and minimal also treat the running network so; auto only when video\n"
        "; memory runs short\n"
        "\n"
        "Motion Without Depth = %d\n"
        "; Where the network reads a pixel's motion vector to fetch the previous frame. Applied after a game restart.\n"
        "; 1 = the pixel's own motion vector, depth not used, as NVIDIA's DLL does (default)\n"
        "; 0 = with depth, the motion vector of whichever of the pixel and its four diagonal neighbours is nearest\n"
        ";     the camera\n"
        "; Only matters for consecutive frames with motion vectors and depth: the edges of moving objects\n"
        "\n"
        "Resize History = %d\n"
        "; What happens to NR's previous result when the game's render size changes (dynamic resolution). Only\n"
        "; matters with NR before the upscaler.\n"
        "; 0 = dropped, NR starts again at the new size, as NVIDIA's DLL does (default)\n"
        "; 1 = scaled to the new size and kept. Steadier in our tests when the size changes often, but not what\n"
        ";     NVIDIA's DLL does. Most DLSS games jitter their input, which hides most of the restart, so it is off\n"
        ";     by default\n"
        "\n"
        "Style Grade = %d\n"
        "; The grade of the Natural and Cinematic styles:\n"
        "; 1 = as NVIDIA's DLL: the picture is graded for the style after the network (default)\n"
        "; 0 = the picture of 0.0.6 and earlier (without that step after the network). Standard is not affected;\n"
        ";     applied at once\n"
        "\n"
        "OSD = %s\n"
        "; The current network shown in a corner of the screen:\n"
        "; switch = while a network change builds, with its progress, then for 3 s (default)\n"
        "; always = all the time\n"
        "; off    = never\n"
        "\n"
        "ACO Mode = %d\n"
        "; Which compiler made the network's machine code. Applied after a game restart.\n"
        "; 1 = machine code made in advance by the ACO compiler (the one the Linux version uses), loaded by the AMD\n"
        ";     driver (default). When it cannot be used, the driver's compiler is used instead and dlssnr-amd.log says why\n"
        "; 0 = the AMD driver's own compiler. CC_SILVER_AARDWOLD_INT4_MIXED needs ACO Mode = 1\n"
        "\n", nr::network_codename(n.model), n.hotkey.c_str(), n.sound ? 1 : 0, residency_name(n.residency),
        n.motion_without_depth ? 1 : 0, n.resize_history ? 1 : 0, n.style_grade ? 1 : 0, osd_name(n.osd), n.aco ? 1 : 0);
}

void write_preprocess(FILE* f, const PreprocessConfig& p) {
    const auto& v = p.values;
    std::fprintf(f,
        "[Preprocess]\n"
        "; Changes the picture the NR network is shown (exposure, display curve, contrast, saturation),\n"
        "; and so how NR edits the picture.\n"
        "; Two uses:\n"
        ";   - a personal look, in any game: change the settings below. It departs from the original look;\n"
        ";     it may be better or worse.\n"
        ";   - games that do not hand their exposure to the upscaler: NR gets a picture several stops too\n"
        ";     dark and turns it green and grainy (007 First Light); turning it on fixes that.\n"
        "; Values marked [upstream] are the same as off; with every value at [upstream] nothing runs.\n"
        "; Numbers may have decimals.\n"
        "; The first time it is turned on (the hotkey too) NR rebuilds, and a second or two of frames\n"
        "; go without NR.\n"
        "\n"
        "Enabled = %d\n"
        "; 0 = off, nothing runs (default)\n"
        "; 1 = on, starting from auto exposure and the filmic curve. Hotkey also switches it for this\n"
        ";     run, without writing this file\n"
        "\n"
        "Exposure = %s\n"
        "; off   = no exposure change [upstream] (the game's exposure when it gives one, else a fixed white point)\n"
        "; auto  = auto exposure after Unreal Engine's design: adjusts to the picture's brightness, up or\n"
        ";         down, and follows scene changes smoothly. For games that give no exposure, such as 007\n"
        ";         First Light. When a game gives its exposure properly and you want your own colour look,\n"
        ";         use off, not auto\n"
        "; fixed = no automatic change; ExposureBias alone\n"
        "\n"
        "ExposureBias = %.2f\n"
        "; exposure compensation in EV (stops), -8 .. +8. 0 = none [upstream]\n"
        "; every stop is a factor of 2: +1 = the network sees it twice as bright, -1 = half\n"
        "; fixed: the whole gain, the same every frame\n"
        "; auto:  added to what auto works out; the sum is the gain. Auto +4.5 with ExposureBias = 0.5 =\n"
        ";        5 stops up\n"
        "; off:   unused\n"
        "\n"
        "Curve = %s\n"
        "; the display curve of the picture the NR network is shown. Test the effect yourself; the hotkey\n"
        "; compares on the same picture\n"
        "; none = no change [upstream], neutral, reinhard, filmic, gt, aces, agx\n"
        "\n"
        "Contrast = %.2f\n"
        "; contrast of the picture the NR network is shown, about mid grey, 0.5 .. 2. 1.0 = no change\n"
        "; [upstream]\n"
        "\n"
        "Saturation = %.2f\n"
        "; saturation of the picture the NR network is shown, 0.05 .. 2 (not 0). 1.0 = no change [upstream]\n"
        "\n"
        "Hotkey = %s\n"
        "; switches the whole preprocess on and off in the game (flips Enabled for this run), to compare\n"
        "; on the same picture. Never written to this file. Empty = no hotkey. Ctrl, Shift, Alt and one\n"
        "; key, e.g. Alt+F9\n"
        "\n"
        "Sound = %d\n"
        "; a short sound when it switches (hotkey or this file): two notes going up = on, going down = off.\n"
        "; 0 = silent\n",
        v.enabled ? 1 : 0, kExposures[std::clamp(v.exposure, 0, 2)], v.bias_ev,
        kCurves[std::clamp(v.curve, 0, 6)], v.contrast, v.saturation, p.hotkey.c_str(), p.sound ? 1 : 0);
}

void write_log(FILE* f, bool enabled, bool clear) {
    std::fprintf(f,
        "\n[Log]\n"
        "; dlssnr-amd.log in the game folder. Takes effect when the game restarts.\n"
        "\n"
        "Enabled = %d\n"
        "; 1 = write the log (default)\n"
        "; 0 = no log\n"
        "\n"
        "ClearOnStart = %d\n"
        "; 1 = clear the previous log at every game start (default)\n"
        "; 0 = keep appending to the previous log\n",
        enabled ? 1 : 0, clear ? 1 : 0);
}

void configure_network(const std::string& folder) {
    static std::once_flag once;
    std::call_once(once, [&] {
        const std::string ini = folder.empty() ? std::string("dlssnr-amd.ini") : folder + "\\dlssnr-amd.ini";
        bool aco = true;   // no ACO Mode in the file: on (NetworkConfig::aco)
        if (FILE* file = std::fopen(ini.c_str(), "r")) {
            std::string name;
            char line[512];
            while (std::fgets(line, sizeof line, file)) {
                std::string text = line;
                const auto comment = text.find_first_of(";#");
                if (comment != std::string::npos) text = text.substr(0, comment);
                text = trim(text);
                if (text.empty()) continue;
                if (text.front() == '[') { name = lower(trim(text.substr(1, text.find(']') - 1))); continue; }
                const auto equals = text.find('=');
                if (name != "network" || equals == std::string::npos) continue;
                const std::string key = lower(trim(text.substr(0, equals)));
                if (key == "aco" || key == "aco mode" || key == "acomode")
                    if (auto b = parse_bool(trim(text.substr(equals + 1)))) aco = *b;
            }
            std::fclose(file);
        }
        if (!aco) return;
        const std::string root = folder.empty() ? std::string(".") : folder;
        const std::string data = root + "\\dlssnr-amd", bundle = data + "\\aco";
        if (GetFileAttributesA((bundle + "\\records").c_str()) == INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesA((bundle + "\\shaders").c_str()) == INVALID_FILE_ATTRIBUTES) {
            log("[nr] [Network] ACO Mode = 1, but %s has no ACO network (records\\, shaders\\): the driver's compiler is used",
                bundle.c_str());
            return;
        }
        // Templates: the native network shaders, under their own name or the one shell-aliases.txt gives
        // ("<bundle name> <native name>" per line: a kernel the Windows build names otherwise).
        std::map<std::string, std::string> aliases;
        if (FILE* file = std::fopen((bundle + "\\shell-aliases.txt").c_str(), "r")) {
            char a[128], b[128];
            while (std::fscanf(file, "%127s %127s", a, b) == 2) aliases[a] = b;
            std::fclose(file);
        }
        // The imported binaries are kept in aco-cache\ (made again when the driver or the files change).
        const std::string cache = data + "\\aco-cache";
        CreateDirectoryA(cache.c_str(), nullptr);
        nr::binary::configure(bundle, {bundle + "\\shell", data + "\\shaders", data + "\\shaders\\temporal"}, cache,
                              std::move(aliases));
        log("[nr] [Network] ACO Mode = 1: the network runs ACO's machine code from %s", bundle.c_str());
    });
}

bool PreprocessFile::poll(const std::string& path, PreprocessConfig& out) {
    const uint64_t stamp = stamp_of(path);
    if (!stamp) {
        if (tried_) return false;
        tried_ = true;
        // No file: write one with the defaults, off, so there is something to edit.
        if (FILE* f = std::fopen(path.c_str(), "w")) {
            write_network(f, NetworkConfig{});
            write_preprocess(f, PreprocessConfig{});
            write_renodx(f, RenoDXConfig{});
            write_log(f, true, true);
            std::fclose(f);
            stamp_ = stamp_of(path);
        }
        out = PreprocessConfig{};
        return true;
    }
    if (stamp == stamp_) return false;
    PreprocessConfig next{};
    if (!parse_preprocess_file(path, next)) return false;
    stamp_ = stamp;
    tried_ = true;
    out = next;
    return true;
}

Preprocess PreprocessSwitch::frame(const PreprocessConfig& file) {
    if (file.hotkey != key_text_) {
        key_text_ = file.hotkey;
        if (!key_text_.empty() && !parse_hotkey(key_text_, vk_, mods_)) {
            log("[nr] preprocess: Hotkey \"%s\" not understood; no hotkey", key_text_.c_str());
            vk_ = 0;
        } else if (key_text_.empty()) vk_ = 0;
    }
    if (file.values.enabled != file_enabled_) { file_enabled_ = file.values.enabled; have_override_ = false; }
    if (vk_) {
        const bool down = key_down(vk_) && ((mods_ & MOD_CONTROL) != 0) == key_down(VK_CONTROL) &&
                          ((mods_ & MOD_SHIFT) != 0) == key_down(VK_SHIFT) &&
                          ((mods_ & MOD_ALT) != 0) == key_down(VK_MENU);
        if (down && !down_ && focused()) {
            override_ = !(have_override_ ? override_ : file_enabled_);
            have_override_ = true;
        }
        down_ = down;
    }
    Preprocess p = file.values;
    p.enabled = have_override_ ? override_ : file_enabled_;
    const bool on = p.active();
    if (logged_ && on != last_on_) {
        switched_ms_ = GetTickCount64();
        if (file.sound) play_cue(on);
    }
    if (!logged_ || on != last_on_ || (on && p != last_)) {
        if (on) log("[nr] preprocess on: %s", describe(p).c_str());
        else if (logged_) log("[nr] preprocess off");
        logged_ = true;
    }
    last_on_ = on; last_ = p;
    return p;
}

// The network switch's cue: one note for CC_SMALL_FP8, two going down for CC_SILVER_AARDWOLD, two going up for
// CC_SILVER_AARDWOLD_INT4_MIXED (the int4 mixed cue as before).
void play_network_cue(int n) {
    if (n == nr::kSilverAardwoldInt4) play_cue(true);
    else if (n == nr::kSilverAardwold) play_cue(false);
    else play_note(880.0f);
}

// What runs instead of a network that cannot: the same model's other network first (int4 mixed -> CC_SILVER_AARDWOLD),
// else the other model.
int NetworkSwitch::fallback(int n) const {
    static const int order[3][2] = {{nr::kSilverAardwold, nr::kSilverAardwoldInt4},
                                    {nr::kSmallFp8, nr::kSilverAardwoldInt4},
                                    {nr::kSilverAardwold, nr::kSmallFp8}};
    for (int c : order[n]) if (available(c)) return c;
    return n;
}

int NetworkSwitch::next_available(int from) const {
    for (int k = 1; k <= nr::kNetworkChoices; ++k) {
        const int c = (from + k) % nr::kNetworkChoices;
        if (available(c)) return c;
    }
    return from;
}

void NetworkSwitch::unavailable(int n, const std::string& why) {
    if (n < 0 || n >= nr::kNetworkChoices || reason_[n] == why) return;
    reason_[n] = why;
    if (!why.empty()) {
        log("[nr] %s is not available in this run: %s", nr::network_codename(n), why.c_str());
        if (want_ == n) {
            want_ = fallback(n);
            log("[nr] running %s instead", nr::network_codename(want_));
        }
    }
}

// Model in the file, everything else as it was (the line rewritten in place, or added to [Network]).
void NetworkSwitch::write(int n) {
    if (ini_.empty()) return;
    std::string text;
    if (FILE* f = std::fopen(ini_.c_str(), "rb")) {
        char buf[4096]; size_t got;
        while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, got);
        std::fclose(f);
    }
    const std::string value = nr::network_codename(n);
    std::string out, section;
    bool done = false;
    size_t at = 0;
    while (at < text.size()) {
        size_t eol = text.find('\n', at);
        std::string line = text.substr(at, eol == std::string::npos ? std::string::npos : eol - at + 1);
        at = eol == std::string::npos ? text.size() : eol + 1;
        std::string t = trim(line.substr(0, line.find_first_of(";#")));
        if (!t.empty() && t.front() == '[') {
            if (!done && lower(section) == "network") { out += "Model = " + value + "\n"; done = true; }
            const auto e = t.find(']');
            section = trim(t.substr(1, e == std::string::npos ? std::string::npos : e - 1));
        } else if (!done && lower(section) == "network") {
            const auto eq = t.find('=');
            if (eq != std::string::npos && lower(trim(t.substr(0, eq))) == "model") {
                out += "Model = " + value + (line.size() && line.back() == '\n' ? "\n" : "");
                done = true;
                continue;
            }
        }
        out += line;
    }
    if (!done) out += (lower(section) == "network" ? "" : "\n[Network]\n") + std::string("Model = ") + value + "\n";
    if (FILE* f = std::fopen(ini_.c_str(), "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
    }
    stamp_ = stamp_of(ini_);
    file_.model = n;
}

void NetworkSwitch::set(int n) {
    if (n < 0 || n >= nr::kNetworkChoices || !available(n)) return;
    write(n);
    if (want_ == n) return;
    want_ = n;
    log("[nr] network %s (menu)", nr::network_codename(n));
    if (file_.sound) play_network_cue(n);
}

int NetworkSwitch::frame(const std::string& ini) {
    ini_ = ini;
    const uint64_t now = GetTickCount64();
    // the ini is parsed again only when its write time changed (one attribute query a second, not a read)
    uint64_t stamp = 0;
    if (want_ < 0 || (now - checked_ms_ >= 1000 && (checked_ms_ = now, stamp = stamp_of(ini)) != stamp_)) {
        checked_ms_ = now;
        stamp_ = want_ < 0 ? stamp_of(ini) : stamp;
        Config c;
        bool legacy = false;
        parse(ini, c, legacy);
        const bool changed = c.network.model != file_.model;
        file_ = c.network;
        if (want_ < 0) {
            char e[48] = {};
            const unsigned long n = GetEnvironmentVariableA("DLSSNR_MODEL", e, sizeof e);
            const int env = n && n < sizeof e ? nr::network_choice(e) : -1;
            want_ = env >= 0 ? env : file_.model;
            if (!available(want_)) want_ = fallback(want_);
            log("[nr] network %s at start (%s)%s", nr::network_codename(want_), env >= 0 ? "DLSSNR_MODEL" : "dlssnr-amd.ini",
                file_.hotkey.empty() ? "" : (", Hotkey " + file_.hotkey + " cycles through the networks").c_str());
        } else if (changed && file_.model != want_) {
            // Model changed in the file while the game runs (NR Control, or edited): switch as the hotkey does
            if (available(file_.model)) {
                want_ = file_.model;
                log("[nr] network %s (dlssnr-amd.ini)", nr::network_codename(want_));
                if (file_.sound) play_network_cue(want_);
            } else {
                log("[nr] dlssnr-amd.ini asks for %s, not available in this run: %s", nr::network_codename(file_.model),
                    reason_[file_.model].c_str());
            }
        }
        if (file_.hotkey != key_text_) {
            key_text_ = file_.hotkey;
            vk_ = 0;
            if (!key_text_.empty() && !parse_hotkey(key_text_, vk_, mods_)) {
                log("[nr] network Hotkey \"%s\" not understood; no hotkey", key_text_.c_str());
                vk_ = 0;
            }
        }
    }
    if (vk_) {
        const bool down = key_down(vk_) && ((mods_ & MOD_CONTROL) != 0) == key_down(VK_CONTROL) &&
                          ((mods_ & MOD_SHIFT) != 0) == key_down(VK_SHIFT) &&
                          ((mods_ & MOD_ALT) != 0) == key_down(VK_MENU);
        if (down && !down_ && focused()) {
            const int n = next_available(want_);
            if (n != want_) {
                want_ = n;
                write(n);
                log("[nr] network %s (hotkey)", nr::network_codename(n));
                if (file_.sound) play_network_cue(n);
            }
        }
        down_ = down;
    }
    return want_;
}

double PreprocessSwitch::since_switch() const {
    return switched_ms_ ? double(GetTickCount64() - switched_ms_) / 1000.0 : 1e9;
}

void Config::resolve() {
    auto& k = controls;
    k.per_pass.assign(size_t(kMaxPasses) - 1, PassControls{});
    for (size_t i = 0; i < k.per_pass.size(); ++i) {
        const PassOverride& o = pass[i];
        PassControls& p = k.per_pass[i];
        p.used = true;
        p.style = o.style.value_or(k.style);
        p.intensity = o.intensity.value_or(k.intensity);
        p.local_structure = o.local_structure.value_or(k.local_structure);
        p.local_tone = o.local_tone.value_or(0.0f);
        p.skin_structure = o.skin_structure.value_or(k.skin_structure);
        p.automatic_mask = o.automatic_mask.value_or(k.automatic_mask);
    }
}

void Config::load(const std::string& path) {
    Config next{};
    bool legacy = false;
    if (!parse(path, next, legacy)) {
        *this = Config{};
        resolve();
        save(path);
        return;
    }
    *this = next;
    stamp_ = stamp_of(path);
    if (legacy) save(path);
}

bool Config::reload(const std::string& path) {
    const uint64_t stamp = stamp_of(path);
    if (!stamp || stamp == stamp_) return false;
    Config next{};
    bool legacy = false;
    if (!parse(path, next, legacy)) return false;
    *this = next;
    stamp_ = stamp;
    return true;
}

static const char* const kClassicScalers[] = {"bilinear", "catmullrom", "lanczos3", "fsr1"};

void Config::save(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    auto flag = [](bool b) { return b ? "true" : "false"; };
    const auto& k = controls;
    std::fprintf(f,
        "; DLSSNR-AMD-Vulkan ReShade add-on settings; edits take effect when saved, also in game.\n"
        "[DlssNr]\n"
        "Enabled=%s\n"
        "ApplyModel=%s\n"
        "; 1..2; 1..10 with UnlockPasses=true\n"
        "Passes=%d\n"
        "UnlockPasses=%s\n"
        "; Model resolution, 0.25..1\n"
        "WorkingScale=%.3f\n"
        "; 0 Standard, 1 Natural, 2 Cinematic\n"
        "Style=%d\n"
        "Intensity=%.3f\n"
        "LocalStructure=%.3f\n"
        "LocalTone=%.3f\n"
        "; -1 follows LocalStructure\n"
        "SkinStructure=%.3f\n"
        "AutoMask=%s\n"
        "; Apply edit: Detail strength 0..2, Colour strength 0..4 (above 1 adds saturation),\n"
        "; Highlight guard 1..8\n"
        "TransferStrength=%.3f\n"
        "ColourStrength=%.3f\n"
        "MaxRatio=%.3f\n"
        "; Below Model resolution 1, how the model's result is enlarged back to full resolution:\n"
        ";   0 Matched residual (default): the model's change, enlarged, is added to the full-resolution picture\n"
        ";   1 Edge-aware lighting + colour: the change's lighting and colour are enlarged separately (weighted by the\n"
        ";     full-resolution picture's edges), then applied to the full-resolution picture\n"
        ";   2 Classic: the model's output picture is enlarged directly, the way ClassicScaler says\n"
        "Transfer=%d\n"
        "; How Transfer=2 enlarges: bilinear, catmullrom, lanczos3, fsr1\n"
        "ClassicScaler=%s\n"
        "; From the 2nd pass on, each pass can be set on its own: Pass2Style, Pass2Intensity, Pass2LocalStructure,\n"
        "; Pass2LocalTone, Pass2SkinStructure, Pass2AutoMask, and likewise Pass3...\n"
        "; Keys left out inherit the 1st pass, except LocalTone, which defaults to 0.\n",
        flag(k.enabled), flag(k.apply_model), k.passes, flag(unlock_passes), model_scale, k.style,
        k.intensity, k.local_structure, k.local_tone, k.skin_structure, flag(k.automatic_mask),
        k.detail_strength, k.colour_strength, k.max_ratio, std::clamp(k.transfer, 0, 2),
        kClassicScalers[std::clamp(k.classic_scaler, 0, 3)]);
    for (int n = 2; n <= kMaxPasses; ++n) {
        const PassOverride& o = pass[size_t(n) - 2];
        if (o.style) std::fprintf(f, "Pass%dStyle=%d\n", n, *o.style);
        if (o.intensity) std::fprintf(f, "Pass%dIntensity=%.3f\n", n, *o.intensity);
        if (o.local_structure) std::fprintf(f, "Pass%dLocalStructure=%.3f\n", n, *o.local_structure);
        if (o.local_tone) std::fprintf(f, "Pass%dLocalTone=%.3f\n", n, *o.local_tone);
        if (o.skin_structure) std::fprintf(f, "Pass%dSkinStructure=%.3f\n", n, *o.skin_structure);
        if (o.automatic_mask) std::fprintf(f, "Pass%dAutoMask=%s\n", n, flag(*o.automatic_mask));
    }
    std::fprintf(f,
        "\n; This project's own settings\n"
        "; How strongly the previous frame's result is blended into this one, 0..1\n"
        "History=%.3f\n"
        "; White point of linear-light input\n"
        "WhitePoint=%.3f\n"
        "; Frames without motion vectors (DLSS5_MV not bound): the network's noise seed changes every frame, as in\n"
        "; NVIDIA's original. With no previous frame to average the noise, a still picture shimmers slightly. false (default): a fixed seed\n"
        "NoiseEveryFrame=%s\n"
        "; Where the ReShade routes' motion vectors come from: vort (in this package) or lumenite (LumeniteFX Kernel by\n"
        "; Afzaal, installed from its author's GitHub with install.bat (LumeniteFX: y); when it is missing or cannot be used,\n"
        "; vort runs and dlssnr-amd.log says why)\n"
        "MotionVectors=%s\n",
        history, white_point, flag(k.noise_every_frame), motion_vectors == 1 ? "lumenite" : "vort");
    std::fputs("\n", f);
    write_network(f, network);
    write_preprocess(f, preprocess);
    write_renodx(f, renodx);
    write_log(f, log_enabled, log_clear);
    std::fclose(f);
    stamp_ = stamp_of(path);
}

}  // namespace nr::pe
