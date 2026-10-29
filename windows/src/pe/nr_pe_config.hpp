#pragma once
#include "nr_runtime.hpp"
#include "nr_renodx.hpp"
#include "nr_ini_network.hpp"
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
};
// One key of the section into `p`; false when it is not one of its keys.
bool parse_preprocess_key(PreprocessConfig& p, const std::string& key, const std::string& value);
// The section, with its comments, as the file holds it.
void write_preprocess(FILE* f, const PreprocessConfig& p);
// [Network], with its comments: which network runs (nr_ini_network.hpp NetworkChoice, by the DLL's codenames), the
// hotkey that cycles through them, the switch's cue, how networks stay in memory (Residency: 0 auto, 1 resident,
// 2 offload to RAM, 3 minimal), where the pre block reads the motion vectors, the history at a new frame size, the
// Style grade, the OSD and which compiler made the network's machine code (ACO Mode).
struct NetworkConfig {
    int model = nr::kSmallFp8;
    std::string hotkey = "Ctrl+F11";
    bool sound = true;
    int residency = 0;
    bool motion_without_depth = true;
    bool resize_history = false;
    int osd = 1;   // OSD: 0 off, 1 after a switch, 2 always
    bool style_grade = true;   // Style Grade: Natural / Cinematic graded as NVIDIA's DLL (false: the older picture)
    // ACO Mode: the network's machine code made by ACO (nr_pipeline_binary.hpp; false: the driver's compiler), read once
    // per process by configure_network
    bool aco = true;
};
void write_network(FILE* f, const NetworkConfig& n);
const char* residency_name(int r);   // auto, resident, offload, minimal
const char* osd_name(int o);         // off, switch, always
// [RenoDX] in dlssnr-amd.ini, every route: the RenoDX-style processing around the network (nr_renodx.hpp).
// Off by default. The keys other than Enabled are RenoDX's own, with its defaults and ranges (a value
// outside its range falls back to the default, as RenoDX does).
struct RenoDXConfig {
    bool enabled = false;
    renodx::Settings values{};
    std::string toggle_key = "F6";   // NRToggleKey: NR on / off in game for this run (NeuralUplift); empty: none
    // RenoDX's NR screenshot (nr_screenshot.hpp)
    std::string screenshot_key = "F5";   // NRScreenshotKey; empty: none
    uint32_t screenshot_source = 0;      // NRScreenshotSource: 0 automatic, 1 the final picture, 2 the model's input / output
    uint32_t screenshot_format = 0;      // NRScreenshotFormat: 0 PNG, 1 JPEG, 2 both
    uint32_t screenshot_layout = 0;      // NRScreenshotLayout: 0 the pair, 1 and a side-by-side file
    uint32_t screenshot_jpeg_quality = 95;
    uint32_t screenshot_max_mb = 10;
};
bool parse_renodx_key(RenoDXConfig& r, const std::string& key, const std::string& value);
// The field behind nr_renodx_keys.hpp's key `index` (bool, uint32_t, float or std::string by its kind).
void* renodx_field(RenoDXConfig& r, int index);
void write_renodx(FILE* f, const RenoDXConfig& r);
// The [RenoDX] section of `path` alone. False when the file could not be opened (`out` untouched).
bool read_renodx(const std::string& path, RenoDXConfig& out);
// [RenoDX] NeuralUplift as a frame sees it: the file's, until NRToggleKey flips it for this run (never written
// back); a change of NeuralUplift in the file wins again.
class NrToggle {
  public:
    bool frame(const RenoDXConfig& file);
  private:
    std::string key_text_;
    int vk_ = 0, mods_ = 0;
    bool down_ = false, have_override_ = false, override_ = false, file_on_ = true, logged_ = false, last_ = true;
};
// NRScreenshotKey: true on the frame the key goes down (the game's window in front).
class KeyPress {
  public:
    bool frame(const std::string& key_text);
  private:
    std::string key_text_;
    int vk_ = 0, mods_ = 0;
    bool down_ = false;
};
// The section as a frame sees it: re-read when the file changed, looked at about once a second.
class RenoDXFile {
  public:
    // False when nothing changed since the last call.
    bool poll(const std::string& path, RenoDXConfig& out);
  private:
    uint64_t stamp_ = 0, checked_ms_ = 0;
};
// [Log], with its comments (read by nr_pe_log.cpp; every route's file carries it).
void write_log(FILE* f, bool enabled, bool clear);
// [Network] ACO Mode of dlssnr-amd.ini in `folder`, handed to nr::binary before the first device is made: on
// (and the bundle dlssnr-amd/aco present) the network runs ACO's machine code. Logs what it decided.
void configure_network(const std::string& folder);
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

// [Network] Model in game, every route: which network the next build should be (nr::NetworkChoice). Starts from
// DLSSNR_MODEL when set, else the file's Model; the Hotkey cycles through the networks available in this run and
// writes the choice back to the file, as a host's UI does (set); a change of Model in the file switches too (its
// write time is checked about once a second). A network that cannot run in this run is skipped, with the reason
// a UI shows beside it (unavailable).
class NetworkSwitch {
  public:
    // The network wanted now. `ini`: the game's dlssnr-amd.ini.
    int frame(const std::string& ini);
    // From a host's UI (or the hotkey): switch to `n`, and write Model to the file.
    void set(int n);
    // `n` cannot run in this run, and why (in a UI's words); an empty reason makes it available again.
    void unavailable(int n, const std::string& why);
    const std::string& reason(int n) const { return reason_[n]; }
    bool available(int n) const { return reason_[n].empty(); }
    const NetworkConfig& file() const { return file_; }   // [Network] as last read
  private:
    void write(int n);
    int next_available(int from) const;
    int fallback(int n) const;
    std::string ini_;
    int want_ = -1;
    NetworkConfig file_{};
    std::string reason_[nr::kNetworkChoices];
    std::string key_text_;
    int vk_ = 0, mods_ = 0;
    bool down_ = false;
    uint64_t checked_ms_ = 0, stamp_ = 0;
};

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
    // The ReShade routes' motion-vector source: 0 VORT (shipped), 1 LumeniteFX Kernel (install.bat (LumeniteFX: y) installs it
    // from its author's GitHub; it is not in the package). The add-on applies it to ReShade (nr_reshade_addon.cpp).
    int motion_vectors = 0;
    NetworkConfig network{};                 // [Network]
    PreprocessConfig preprocess{};   // [Preprocess]; controls.preprocess is the frame's, see PreprocessSwitch
    RenoDXConfig renodx{};           // [RenoDX]
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
