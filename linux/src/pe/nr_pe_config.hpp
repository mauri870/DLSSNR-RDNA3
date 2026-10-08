#pragma once
#include "nr_runtime.hpp"
#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace nr::pe {

// Passes, as OptiScaler DLSS-NR: 1..2, or 1..10 with UnlockPasses.
inline constexpr int kMaxPasses = 10;

// A later pass's own settings. Each one that is absent inherits pass 1, except
// LocalTone, which defaults to 0 (OptiScaler DLSS-NR's PassTuning).
struct PassOverride {
    std::optional<int> style;
    std::optional<float> intensity, local_structure, local_tone, skin_structure;
    std::optional<bool> automatic_mask;
};

// [Preprocess] in dlssnr-amd.ini, read by every route: nr::Preprocess and the
// hotkey that flips Enabled for this run. See runtime_prep.comp.
struct PreprocessConfig {
    Preprocess values{};
    std::string hotkey = "Ctrl+F10";   // empty: none
    bool sound = true;                 // a cue when it switches on or off
    // [Reuse], the same file: temporal reuse of the network's edit (Controls::reuse_every / reuse_gate).
    // The OptiScaler route builds its controls from NGX parameters, which have no key for it.
    int reuse_every = 0;
    float reuse_gate = 40.0f;
};
// One key of the section into `p`; false when it is not one of its keys.
bool parse_preprocess_key(PreprocessConfig& p, const std::string& key, const std::string& value);
// The section, with its comments, as the file holds it.
void write_preprocess(FILE* f, const PreprocessConfig& p);
#if NR_INT4
// [Int4Mixed], with its comments.
void write_int4mixed(FILE* f, bool enabled, const std::string& hotkey, bool keep_both, bool sound);
#endif
// [Log], with its comments (read by nr_pe_log.cpp; every route's file carries it).
void write_log(FILE* f, bool enabled, bool clear);
// The [Reuse] section the OptiScaler route adds to the file it creates.
void write_reuse(FILE* f, const PreprocessConfig& p);
// "exposure auto, ExposureBias +0.00 EV, curve none, contrast 1.00, saturation 1.00", for the log.
std::string describe(const Preprocess& p);

// The OptiScaler route's file: the [Preprocess] section alone (a ReShade
// add-on's [DlssNr] in the same file is left to the add-on). Written with the
// defaults, Enabled = 0, when there is no file; re-read when it changes.
class PreprocessFile {
  public:
    // False when the file did not change since the last call.
    bool poll(const std::string& path, PreprocessConfig& out);
  private:
    uint64_t stamp_ = 0;
    bool tried_ = false;
};

// Enabled as a frame sees it: the file's, until the hotkey flips it for this
// run (never written back); a change of Enabled in the file wins again.
class PreprocessSwitch {
  public:
    Preprocess frame(const PreprocessConfig& file);
    bool on() const { return last_on_; }   // what the last frame ran with
    // Seconds since it last switched on or off (hotkey or file); large before the first switch.
    double since_switch() const;
  private:
    std::string key_text_;
    int vk_ = 0, mods_ = 0;
    bool down_ = false, have_override_ = false, override_ = false, file_enabled_ = false;
    bool logged_ = false, last_on_ = false;
    uint64_t switched_ms_ = 0;   // GetTickCount64 at the last switch, 0 before one
    Preprocess last_{};
};

#if NR_INT4
// [Int4Mixed] in game, every route: which network the next build should be. Starts from DLSSNR_INT4 when set, else
// from the file's Enabled; the Hotkey flips it for this run (never written back), and so does a change of Enabled in
// the file (its write time is checked about once a second, the file re-read when it changed: Enabled, Hotkey,
// KeepBoth). With Enabled = 0 and no Hotkey at start the int4 layer left the game's device without the extensions
// int4 mixed needs, so nothing switches in that run.
class Int4Switch {
  public:
    // 0 the default network, 1 int4 mixed. `ini`: the game's dlssnr-amd.ini.
    int frame(const std::string& ini);
    // KeepBoth: keep the other network resident after a switch (else it is freed once the new one is in).
    bool keep_both() const { return keep_both_; }
    // The int4 mixed network could not be built in this run (the runtime logged why): the default network from now on, no hotkey.
    void unavailable();
    // From a host's UI: as the hotkey, and the caller writes Enabled = on to the file (so re-reading it is no switch).
    void set(bool on);
    bool is_off() const { return off_; }
  private:
    int want_ = -1;
    bool keep_both_ = false, sound_ = true;
    std::string key_text_;
    int vk_ = 0, mods_ = 0;
    bool down_ = false;
    bool file_on_ = true;   // the file's Enabled as last read
    uint64_t checked_ms_ = 0, stamp_ = 0;
    // Enabled = 0 and no Hotkey at start: the int4 layer left the game's device as it was, so int4 mixed cannot
    // run in this launch - the switch stops looking at anything for the rest of it.
    bool off_ = false;
};

#endif
// The ReShade add-on's settings, dlssnr-amd.ini next to it. Section [DlssNr]
// with OptiScaler DLSS-NR's key names (ranges as the file's comments say), plus
// ClassicScaler, History and WhitePoint, which are this project's.
struct Config {
    Controls controls{};          // pass 1, Passes, Enabled, ApplyModel, the apply-edit controls
    bool unlock_passes = false;
    std::array<PassOverride, kMaxPasses - 1> pass{};   // pass[0] is pass 2
    float model_scale = 1.0f;     // WorkingScale, 0.25..1
    float history = 1.0f;         // previous-frame blend in the post block, 0..1
    float white_point = 1.0f;     // linear-light input only
#if NR_INT4
    bool int4 = true;             // [Int4Mixed] Enabled: the int4 mixed network at start (no Enabled: on, nr_ini_int4.hpp)
    std::string int4_hotkey = "Ctrl+F11";   // [Int4Mixed] Hotkey: switch int4 mixed / the default network in game; empty: none
    bool int4_keep_both = false;            // [Int4Mixed] KeepBoth
    bool int4_sound = true;                 // [Int4Mixed] Sound
#endif
    PreprocessConfig preprocess{};   // [Preprocess]; controls.preprocess is the frame's, see PreprocessSwitch
    bool log_enabled = true;         // [Log] Enabled, read by the log itself at start (nr_pe_log.cpp)
    bool log_clear = true;           // [Log] ClearOnStart

    int pass_limit() const { return unlock_passes ? kMaxPasses : 2; }
    // Fill controls.per_pass from `pass`; call after any change.
    void resolve();

    // Read the file, creating it with defaults when absent and rewriting it in
    // the current format when it still holds the old keys.
    void load(const std::string& path);
    void save(const std::string& path);
    // Re-read if the file changed since the last load/save/reload.
    bool reload(const std::string& path);

  private:
    bool legacy_ = false;
    uint64_t stamp_ = 0;
};

}  // namespace nr::pe
