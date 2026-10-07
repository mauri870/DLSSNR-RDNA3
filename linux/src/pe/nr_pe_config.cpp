// dlssnr-amd.ini: the ReShade add-on's settings, and [Preprocess] for every
// route. Re-read while the game runs.
#include "nr_pe_config.hpp"
#if NR_INT4
#include "nr_ini_int4.hpp"
#endif
#include "nr_pe_log.hpp"

#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        if (name == "log") {
            if (key == "enabled") { if (auto b = parse_bool(value)) c.log_enabled = *b; }
            else if (key == "clearonstart") { if (auto b = parse_bool(value)) c.log_clear = *b; }
            continue;
        }
#if NR_INT4
        if (name == "int4mixed") {
            if (key == "enabled") { if (auto b = parse_bool(value)) c.int4 = *b; }
            else if (key == "hotkey") c.int4_hotkey = value;
            else if (key == "keepboth") { if (auto b = parse_bool(value)) c.int4_keep_both = *b; }
            else if (key == "sound") { if (auto b = parse_bool(value)) c.int4_sound = *b; }
            continue;
        }
#endif
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
        else if (key == "whitepoint" || key == "white_point") c.white_point = number(value, 0.01f, 100.0f);
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
    return true;
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

std::string describe(const Preprocess& p) {
    char text[160];
    std::snprintf(text, sizeof text, "exposure %s, ExposureBias %+.2f EV, curve %s, contrast %.2f, saturation %.2f",
                  kExposures[std::clamp(p.exposure, 0, 2)], p.exposure == 0 ? 0.0f : p.bias_ev,
                  kCurves[std::clamp(p.curve, 0, 6)], p.contrast, p.saturation);
    return text;
}

#if NR_INT4
void write_int4mixed(FILE* f, bool enabled, const std::string& hotkey, bool keep_both, bool sound) {
    std::fprintf(f,
        "[Int4Mixed]\n"
        "; int4 mixed: part of the network runs in int4 and other lower precisions; faster, the picture differs somewhat.\n"
        "; Edits take effect when saved, also in game. A switch builds the other network in the background; until it is\n"
        "; ready the current one keeps running.\n"
        "\n"
        "Enabled = %d\n"
        "; 1 = int4 mixed (default)\n"
        "; 0 = the default network\n"
        "; With Enabled = 0 and no Hotkey when the game starts, int4 mixed cannot be switched to in that run (restart the game)\n"
        "\n"
        "Hotkey = %s\n"
        "; Switches between int4 mixed and the default network in game, to compare them on the same picture.\n"
        "; For the current run only, not written back to this file. Empty = no hotkey. Ctrl, Shift, Alt and one key,\n"
        "; e.g. Alt+F9\n"
        "\n"
        "KeepBoth = %d\n"
        "; 0 = after a switch the other network is freed, so only one takes video memory; switching back builds it again\n"
        ";     in the background (default)\n"
        "; 1 = both networks stay in video memory and switching is instant, for comparing back and forth; takes one\n"
        ";     more network's video memory\n"
        "\n"
        "Sound = %d\n"
        "; a short sound when it switches (hotkey or this file): two notes going up = int4 mixed, going down = the\n"
        "; default network. 0 = silent\n"
        "\n", enabled ? 1 : 0, hotkey.c_str(), keep_both ? 1 : 0, sound ? 1 : 0);
}

#endif
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

bool PreprocessFile::poll(const std::string& path, PreprocessConfig& out) {
    const uint64_t stamp = stamp_of(path);
    if (!stamp) {
        if (tried_) return false;
        tried_ = true;
        // No file: write one with the defaults, off, so there is something to edit.
        if (FILE* f = std::fopen(path.c_str(), "w")) {
#if NR_INT4
            write_int4mixed(f, true, "Ctrl+F11", false, true);
#endif
            write_preprocess(f, PreprocessConfig{});
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

#if NR_INT4
int Int4Switch::frame(const std::string& ini) {
    if (off_) return 0;
    const uint64_t now = GetTickCount64();
    // the ini is parsed again only when its write time changed (one attribute query a second, not a read)
    uint64_t stamp = 0;
    if (want_ < 0 || (now - checked_ms_ >= 1000 && (checked_ms_ = now, stamp = stamp_of(ini)) != stamp_)) {
        checked_ms_ = now;
        stamp_ = want_ < 0 ? stamp_of(ini) : stamp;
        const nr::Int4Ini file = nr::ini_int4(ini);
        keep_both_ = file.keep_both; sound_ = file.sound;
        if (want_ < 0) {
            char e[16] = {};
            const unsigned long n = GetEnvironmentVariableA("DLSSNR_INT4", e, sizeof e);
            file_on_ = file.enabled != 0;   // no Enabled: on (nr_ini_int4.hpp)
            want_ = (n && n < sizeof e) ? (std::string(e) == "1" ? 1 : 0) : (file_on_ ? 1 : 0);
            log("[nr] int4 mixed %s at start%s", want_ ? "on" : "off",
                file.hotkey.empty() ? "" : (", Hotkey " + file.hotkey + " switches it").c_str());
            if (!want_ && file.hotkey.empty()) { off_ = true; return 0; }
        } else if ((file.enabled != 0) != file_on_) {
            // Enabled changed in the file while the game runs: switch as the hotkey does
            file_on_ = file.enabled != 0;
            if (want_ != (file_on_ ? 1 : 0)) {
                want_ = file_on_ ? 1 : 0;
                log("[nr] int4 mixed %s (dlssnr-amd.ini)", want_ ? "on" : "off");
                if (sound_) play_cue(want_ != 0);
            }
        }
        if (file.hotkey != key_text_) {
            key_text_ = file.hotkey;
            vk_ = 0;
            if (!key_text_.empty() && !parse_hotkey(key_text_, vk_, mods_)) {
                log("[nr] int4 mixed: Hotkey \"%s\" not understood; no hotkey", key_text_.c_str());
                vk_ = 0;
            }
        }
    }
    if (vk_) {
        const bool down = key_down(vk_) && ((mods_ & MOD_CONTROL) != 0) == key_down(VK_CONTROL) &&
                          ((mods_ & MOD_SHIFT) != 0) == key_down(VK_SHIFT) &&
                          ((mods_ & MOD_ALT) != 0) == key_down(VK_MENU);
        if (down && !down_ && focused()) {
            want_ = want_ ? 0 : 1;
            log("[nr] int4 mixed %s (hotkey)", want_ ? "on" : "off");
            if (sound_) play_cue(want_ != 0);
        }
        down_ = down;
    }
    return want_;
}
#endif

#if NR_INT4
void Int4Switch::set(bool on) {
    if (off_ || want_ < 0) return;
    file_on_ = on;
    if (want_ == (on ? 1 : 0)) return;
    want_ = on ? 1 : 0;
    log("[nr] int4 mixed %s (add-on menu)", on ? "on" : "off");
}

void Int4Switch::unavailable() {
    if (off_) return;
    want_ = 0; off_ = true;
    log("[nr] int4 mixed is not available in this run (see above); the hotkey is off");
}
#endif

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
        "WhitePoint=%.3f\n",
        history, white_point);
    std::fputs("\n", f);
#if NR_INT4
    write_int4mixed(f, int4, int4_hotkey, int4_keep_both, int4_sound);
#endif
    write_preprocess(f, preprocess);
    write_log(f, log_enabled, log_clear);
    std::fclose(f);
    stamp_ = stamp_of(path);
}

}  // namespace nr::pe
