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

// Passes, as OptiScaler DLSS-NR (v0.8.91): 1..3, or 1..30 with UnlockPasses.
inline constexpr int kMaxPasses = 30;

// A later pass's own settings. Each one that is absent inherits pass 1, except
// LocalTone, which defaults to 0 (OptiScaler DLSS-NR's PassTuning).
struct PassOverride {
    std::optional<int> style;
    std::optional<float> intensity, local_structure, local_tone, skin_structure;
    std::optional<bool> automatic_mask;
};

// [Preprocess] in dlssnr-amd.ini, read by every route: nr::Preprocess and the
// hotkey that flips Enabled for this run. See linux/shaders/renodx-prep/prep_math.glsl.
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
// [Network], with its comments: which network runs (nr_ini_network.hpp NetworkChoice, by the DLL's codenames), the
// hotkey that cycles through them, the switch's cue, how networks stay in memory (Residency: 0 auto, 1 resident,
// 2 offload to RAM, 3 minimal), where the pre block reads the motion vectors, and the history at a new frame size.
struct NetworkConfig {
    int model = nr::kSmallFp8;
    std::string hotkey = "Ctrl+F11";
    bool sound = true;
    int residency = 0;
    bool motion_without_depth = true;
    bool resize_history = false;
    int osd = 1;   // OSD: 0 off, 1 after a switch, 2 always, 3 always with each layer's cost
    bool style_grade = true;   // Style Grade: Natural / Cinematic graded as NVIDIA's DLL (false: the older picture)
};
void write_network(FILE* f, const NetworkConfig& n);
const char* residency_name(int r);   // auto, resident, offload, minimal
const char* osd_name(int o);         // off, switch, always, cost
// [RenoDX] in dlssnr-amd.ini, every route: the RenoDX-style processing around the network (nr_renodx.hpp).
// Off by default. The keys other than Enabled are RenoDX's own, with its defaults and ranges (a value
// outside its range falls back to the default, as RenoDX does).
struct RenoDXConfig {
    bool enabled = false;
    bool enabled_given = false;   // Enabled was in the file (the .asi route defaults to the RenoDX style without it)
    renodx::Settings values{};
    std::string toggle_key = "F6";   // NRToggleKey: NR on / off in game for this run (NeuralUplift); empty: none
    // RenoDX's NR screenshot (nr_screenshot.hpp)
    std::string screenshot_key = "F5";   // NRScreenshotKey; empty: none
    uint32_t screenshot_source = 0;      // NRScreenshotSource: 0 automatic, 1 the final picture, 2 the model's input / output
    uint32_t screenshot_format = 0;      // NRScreenshotFormat: 0 PNG, 1 JPEG, 2 both
    uint32_t screenshot_layout = 0;      // NRScreenshotLayout: 0 the pair, 1 and a side-by-side file
    uint32_t screenshot_jpeg_quality = 95;
    uint32_t screenshot_max_mb = 10;
    uint32_t output_rect = 1;   // OptiScaler route: Auto, Declared (default, 0.0.7's), Resource
};
bool parse_renodx_key(RenoDXConfig& r, const std::string& key, const std::string& value);
// The field behind nr_renodx_keys.hpp's key `index` (bool, uint32_t, float or std::string by its kind).
void* renodx_field(RenoDXConfig& r, int index);
void write_renodx(FILE* f, const RenoDXConfig& r);
// [wilsjo2]: the OptiScaler route's wilsjo2 style (nr_wilsjo2.hpp Settings), every key at auto.
void write_wilsjo2(FILE* f);
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
    uint64_t stamp_ = 0, checked_ms_ = 0, epoch_ = 0;
};
// The .asi route's NR chain: [Layer1], [Layer2], ... in dlssnr-amd.ini, each an independent NR instance with its
// processing, network, placement and every setting of that processing, read from its own section only (a layer never
// reads another's keys). Without any [LayerN] section the file is one layer, as the .asi ran before the chain: the
// RenoDX processing with [RenoDX] (placed by NRHookPoint), or, with [RenoDX] Enabled = 0 given, the wilsjo2 processing
// with [wilsjo2] (placed by its RunBeforeSR).
enum class Processing { kRenoDX, kWilsjo2 };
enum class Placement { kAfter, kBefore, kPresent };
const char* processing_name(Processing p);
const char* placement_name(Placement p);
struct LayerConfig {
    int number = 0;                     // N of [LayerN]; 0: the one layer of a file without any
    bool enabled = true;
    Processing processing = Processing::kRenoDX;
    Placement placement = Placement::kPresent;   // the default placement
    std::string network;                // a network's codename; empty: [Network] Model
    RenoDXConfig renodx;                // the RenoDX processing's keys
    // every key of the section other than the four above, as written (the wilsjo2 processing parses them)
    std::vector<std::pair<std::string, std::string>> keys;
};
// [Inspect]: NR Control's comparison and debug tools for the whole NR chain, every route. All off by default.
struct InspectConfig {
    uint32_t compare = 0;        // Compare: 0 off, 1 side by side, 2 wipe
    float split = 0.5f;          // Split 0..1: where the wipe cuts
    float zoom = 1.0f;           // Zoom 1..2: side by side, 1 the whole frame (bars), 2 the half filled (cropped)
    bool swap = false;           // Swap: NR on the left
    bool tags = false;           // Tags: "NR off" / "NR on" over the halves (NR Control's overlay)
    float tag_scale = 1.5f;      // TagScale 0.5..4
    int layer = 0;               // Layer: 0 the final picture (the layers of the last placement that runs), N layer N
    uint32_t debug_view = 0;     // DebugView (Layer's): 0 off, 1 the network's input, 2 its answer, 3 its change x20
    bool apply_model = true;     // ApplyModel: false = the network runs, its answer is not put on the frame
    bool hold = false;           // HoldFrame (Layer's): one input frame, given to it every frame
    bool skin_mask = false;      // SkinMask (Layer's, wilsjo2): the skin weight shown instead of the picture
    bool spatial_outlines = false;   // SpatialOutlines (wilsjo2 layers with SpatialCompression): NR Control draws the
                                     // centre band and the working region over the picture
    bool active() const { return compare != 0 || debug_view != 0 || !apply_model || hold || skin_mask; }
};

// [HudFix]: NR's change kept off the game's HUD at Present (the layers placed there), every processing's.
enum class HudSource : uint32_t { kOff = 0, kAuto = 1, kGame = 2, kCaptured = 3, kEstimated = 4 };
struct HudFixConfig {
    uint32_t mode = 1;   // Mode: 0 off, 1 Auto (the default), 2 frame generation's HUD-less frame (DLSS-G,
                         // FSR FG, XeSS FG), 3 captured before the HUD is drawn, 4 estimated (screen-fixed pixels while
                         // the picture moves)
    bool show_mask = false;   // ShowMask: the HUD's alpha shown (white = HUD, kept as the game drew it)
};

struct ChainConfig {
    uint32_t output_rect = 1;
    bool layered = false;               // [LayerN] sections were found
    std::vector<LayerConfig> layers;    // by N
    InspectConfig inspect;              // [Inspect]
    HudFixConfig hud;                   // [HudFix]
};
// False when the file could not be opened (`out` untouched).
bool read_chain(const std::string& path, ChainConfig& out);
// [HudFix] Mode where a file does not say it: Auto (the .asi), Off on the ReShade route (the add-on sets it
// at load).
void set_hudfix_default(uint32_t mode);
class ChainFile {
  public:
    // False when nothing changed since the last call.
    bool poll(const std::string& path, ChainConfig& out);
  private:
    uint64_t stamp_ = 0, checked_ms_ = 0, epoch_ = 0;
};
// NR Control drawn by our DLL (nr_pe_menu.cpp) has just saved dlssnr-amd.ini: the next look of each poll here
// (RenoDXFile, NetworkSwitch, the model's [Preprocess]) is at once rather than up to a second later. config_epoch()
// counts the saves.
void touch_config();
uint64_t config_epoch();
// [Log], with its comments (read by nr_pe_log.cpp; every route's file carries it).
void write_log(FILE* f, bool enabled, bool clear);
// [Reuse], with its comments: the network on every `every`-th frame (0 or 1: every frame), the frames between
// carrying its edit forward by the motion; `gate` in pixels of a 1080p-wide frame.
void write_reuse(FILE* f, int every, float gate);
// [Inspect] (NR Control's comparison and debug tools), with its comments: what the installer writes (every tool off).
void write_inspect(FILE* f, const InspectConfig& i);
void write_hudfix(FILE* f, const HudFixConfig& h);
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
    uint64_t checked_ms_ = 0, stamp_ = 0, epoch_ = 0;
};

// The ReShade add-on's settings, dlssnr-amd.ini next to it. Section [DlssNr]
// with OptiScaler DLSS-NR's key names (ranges as the file's comments say), plus
// ClassicScaler, History and WhitePoint, which are this project's.
struct Config {
    Controls controls{};          // pass 1, Passes, Enabled, ApplyModel, the apply-edit controls
    bool unlock_passes = false;
    bool colour_limit = true;     // ColourLimit (stacking: the edit's colour bounded, RenoDX's +/-1 stop)
    std::array<PassOverride, kMaxPasses - 1> pass{};   // pass[0] is pass 2
    float model_scale = 1.0f;     // WorkingScale, 0.25..2
    uint32_t downscale = 0;       // Downscale below 100 %: 0 area after encoding, 1 bilinear before encoding
    // Passes (fast) on a wilsjo2 layer (PassesMode, FastMethod, Fast<Method>Passes / HaloLimit / Fine, FastEditStrength)
    uint32_t passes_mode = 0;     // 0 legacy, 1 fast
    uint32_t fast_method = 0;     // 0 Extrapolation, 1 Feedback, 2 Hybrid, 3 Edit strength
    // each method's own: [0] Extrapolation, [1] Feedback, [2] Hybrid
    float fast_passes[3] = {3.0f, 3.0f, 3.0f};       // 1..8
    bool fast_halo_limit[3] = {false, false, false};
    float fast_fine[3] = {0.0f, 1.0f, 1.0f};         // 0..2
    float fast_edit_strength = 2.0f;   // 0..4 (Edit strength)
    float history = 1.0f;         // previous-frame blend in the post block, 0..1
    float white_point = 1.0f;     // linear-light input only
    // SpatialCompression and Spatial* (OptiScaler DLSS-NR's peripheral compression, percent), for the layer's wilsjo2
    // processing (nr_wilsjo2_flow.hpp)
    struct Spatial {
        bool on = false;
        float centre[2] = {80.0f, 80.0f}, work[2] = {90.0f, 90.0f}, offset[2] = {0.0f, 0.0f}, shift[2] = {0.0f, 0.0f};
    } spatial;
    // The ReShade routes' motion-vector source: 0 VORT (shipped), 1 LumeniteFX Kernel (install.sh --lumenite installs it
    // from its author's GitHub; it is not in the package). The add-on applies it to ReShade (nr_reshade_addon.cpp).
    int motion_vectors = 0;
    NetworkConfig network{};                 // [Network]
    PreprocessConfig preprocess{};   // [Preprocess]; controls.preprocess is the frame's, see PreprocessSwitch
    RenoDXConfig renodx{};           // [RenoDX]
    bool log_enabled = true;         // [Log] Enabled, read by the log itself at start (nr_pe_log.cpp)
    bool log_clear = true;           // [Log] ClearOnStart

    int pass_limit() const { return unlock_passes ? kMaxPasses : 3; }
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

// The ReShade route's wilsjo2 layer ([LayerN] Processing = wilsjo2): its settings from its own keys, the [DlssNr] ones
// under the original's names and numbers (Transfer as OptiScaler DLSS-NR numbers it, our two enlargements as 5 and 6;
// WhitePointScale), everything else at its default. Only the processing's part of Config is used.
Config layer_config(const LayerConfig& l);

}  // namespace nr::pe
