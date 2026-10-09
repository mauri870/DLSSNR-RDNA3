// nr_tester: runs the RDNA3 feasibility test on this machine and writes results.txt. Windows only.
// Finds the user's model (or makes it from nvngx_dlssnr.dll / a zip), runs the network on one frame in two
// modes with time limits, compares the picture with the one RADV made, times 1080p and 720p, and reports.
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static const char kModelSha[] = "2b41c888cf4155b8958c665ba64018ab0bd25c85fc71a2b6db86d0d04d1f7fbd";
static std::string g_report;

static void say(const std::string& line) {
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
    g_report += line + "\n";
}

static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

static std::string tail_lines(const std::string& s, size_t count) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        lines.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    std::string out;
    for (size_t i = lines.size() > count ? lines.size() - count : 0; i < lines.size(); ++i) out += "    " + lines[i] + "\n";
    return out;
}

// The lines that say where a failed run stopped: the exception header and the last few trace steps.
static std::string key_lines(const std::string& s) {
    std::vector<std::string> trace, crash;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\n', start);
        if (end == std::string::npos) end = s.size();
        const std::string line = s.substr(start, end - start);
        if (line.rfind("[trace]", 0) == 0) trace.push_back(line);
        if (line.rfind("[crash] exception", 0) == 0 || line.rfind("[crash] reading", 0) == 0 || line.rfind("[crash] writing", 0) == 0) crash.push_back(line);
        start = end + 1;
    }
    std::string out;
    for (size_t i = trace.size() > 4 ? trace.size() - 4 : 0; i < trace.size(); ++i) out += "    " + trace[i] + "\n";
    for (const std::string& line : crash) out += "    " + line + "\n";
    return out;
}

static std::string sha256_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return "";
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "";
    BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    std::vector<char> buffer(1 << 20);
    while (f) {
        f.read(buffer.data(), std::streamsize(buffer.size()));
        const std::streamsize got = f.gcount();
        if (got > 0) BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), ULONG(got), 0);
    }
    unsigned char digest[32] = {};
    BCryptFinishHash(hash, digest, 32, 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return hex;
}

static std::string registry_string(HKEY root, const char* key, const char* value) {
    char buffer[512] = {};
    DWORD size = sizeof buffer;
    if (RegGetValueA(root, key, value, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return "";
    return buffer;
}

// Runs a command line in `dir`, its output into `log`, killed after `timeout_ms`.
static bool run(const std::wstring& command_line, const fs::path& dir, const fs::path& log, DWORD timeout_ms,
                DWORD* exit_code, bool* timed_out) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = out;
    startup.hStdInput = in;
    PROCESS_INFORMATION process{};
    std::wstring mutable_line = command_line;
    const BOOL started = CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        dir.c_str(), &startup, &process);
    *timed_out = false;
    *exit_code = 0xFFFFFFFF;
    if (started) {
        if (WaitForSingleObject(process.hProcess, timeout_ms) == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 10000);
            *timed_out = true;
        }
        GetExitCodeProcess(process.hProcess, exit_code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    CloseHandle(out);
    CloseHandle(in);
    return started != 0;
}

struct Picture {
    bool ok = false, identical = false;
    double psnr = 0, identical_percent = 0;
    int largest = 0;
};

static Picture compare(const fs::path& a_path, const fs::path& b_path) {
    Picture r;
    std::ifstream fa(a_path, std::ios::binary), fb(b_path, std::ios::binary);
    std::vector<unsigned char> a((std::istreambuf_iterator<char>(fa)), {}), b((std::istreambuf_iterator<char>(fb)), {});
    if (a.empty() || a.size() != b.size()) return r;
    double sum = 0;
    size_t same = 0;
    const size_t pixels = a.size() / 4;
    for (size_t i = 0; i < pixels; ++i) {
        bool equal = true;
        for (int c = 0; c < 3; ++c) {
            const int d = int(a[i * 4 + c]) - int(b[i * 4 + c]);
            sum += double(d) * d;
            r.largest = std::max(r.largest, std::abs(d));
            if (d) equal = false;
        }
        same += equal;
    }
    r.ok = true;
    r.identical = sum == 0;
    r.psnr = sum == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / (sum / (double(pixels) * 3)));
    r.identical_percent = 100.0 * double(same) / double(pixels);
    return r;
}

static double frame_ms(const std::string& text) {
    std::smatch m;
    if (std::regex_search(text, m, std::regex("frame_ms ([0-9.]+)"))) return std::stod(m[1]);
    return -1;
}

struct ModeResult {
    std::string name, verdict, detail;
    double ms_1080 = -1, ms_720 = -1;
};

static std::wstring quote(const fs::path& p) { return L"\"" + p.wstring() + L"\""; }

using EnvList = std::vector<std::pair<const char*, const char*>>;

static ModeResult run_mode(const fs::path& base, const char* name, bool barriers, DWORD limit_ms, const EnvList& extra = {}, const std::string& tag_override = "") {
    ModeResult m;
    m.name = name;
    SetEnvironmentVariableA("NR_TCHAIN", barriers ? "0" : nullptr);
    for (const auto& kv : extra) SetEnvironmentVariableA(kv.first, kv.second);
    const fs::path exe = base / "run_frame.exe";
    DWORD code = 0;
    bool timed_out = false;
    const std::string tag = !tag_override.empty() ? tag_override : (barriers ? "barriers" : "default");
    const fs::path out_file = base / ("out_" + tag + ".rgba8"), log = base / ("log_" + tag + ".txt");
    fs::remove(out_file);
    say(std::string("  running the network on one frame (the first run builds the pipelines: a few minutes) ..."));
    run(quote(exe) + L" . in_1080p.rgba8 1920 1080 " + quote(out_file), base, log, limit_ms, &code, &timed_out);
    const std::string text = read_text(log);
    if (timed_out) {
        m.verdict = "TIMED OUT (no result after " + std::to_string(limit_ms / 60000) + " minutes: the GPU may have hung)";
    } else if (code != 0 || !fs::exists(out_file)) {
        m.verdict = "FAILED (exit code " + std::to_string(long(code)) + ")";
        m.detail = tail_lines(text, 120);
    } else {
        const Picture p = compare(out_file, base / "radv_1080p.rgba8");
        char line[200];
        if (!p.ok) {
            m.verdict = "FAILED (cannot compare pictures)";
        } else if (p.identical) {
            m.verdict = "OK, identical to the Linux driver's picture";
        } else {
            std::snprintf(line, sizeof line, "%s: %.2f dB against the Linux driver's picture, largest difference %d, identical pixels %.1f %%",
                          p.psnr >= 60 ? "OK (tiny differences)" : "DIFFERENT PICTURE", p.psnr, p.largest, p.identical_percent);
            m.verdict = line;
        }
        if (!p.ok || p.psnr < 60) m.detail = tail_lines(text, 120);
        say("  timing 1080p ...");
        run(quote(exe) + L" . in_1080p.rgba8 1920 1080 out_timing.rgba8 50", base, base / ("log_" + tag + "_1080p.txt"), limit_ms, &code, &timed_out);
        m.ms_1080 = frame_ms(read_text(base / ("log_" + tag + "_1080p.txt")));
        say("  timing 720p ...");
        run(quote(exe) + L" . in_720p.rgba8 1280 720 out_timing.rgba8 50", base, base / ("log_" + tag + "_720p.txt"), limit_ms, &code, &timed_out);
        m.ms_720 = frame_ms(read_text(base / ("log_" + tag + "_720p.txt")));
    }
    for (const auto& kv : extra) SetEnvironmentVariableA(kv.first, nullptr);
    return m;
}

static void finish(const fs::path& base, int status) {
    std::ofstream f(base / "results.txt", std::ios::binary);
    f << g_report;
    f.close();
    std::printf("\nThe result is in %s\n", (base / "results.txt").u8string().c_str());
    std::printf("Please send that one file (results.txt) back. Thank you!\n");
    std::exit(status);
}

int wmain() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const fs::path base = fs::path(module).parent_path();
    SetCurrentDirectoryW(base.c_str());

    say("RDNA3 network test");
    say("==================");
    say("This only reads the file you were given and runs a short GPU test. It installs nothing.");
    say("");
    say("Windows: " + registry_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "ProductName") + " " +
        registry_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "DisplayVersion") + " (build " +
        registry_string(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", "CurrentBuild") + ")");
    bool found_gpu = false;
    for (int i = 0; i < 16; ++i) {
        char key[200];
        std::snprintf(key, sizeof key, "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", i);
        const std::string name = registry_string(HKEY_LOCAL_MACHINE, key, "DriverDesc");
        if (name.empty()) continue;
        found_gpu = true;
        say("Graphics card: " + name + ", driver " + registry_string(HKEY_LOCAL_MACHINE, key, "DriverVersion") + " (" +
            registry_string(HKEY_LOCAL_MACHINE, key, "DriverDate") + ")");
    }
    if (!found_gpu) say("Graphics card: not found in the registry");
    if (fs::exists(base / "probe.exe")) {
        DWORD code = 0;
        bool timed_out = false;
        run(quote(base / "probe.exe") + L" --report", base, base / "log_report.txt", 120000, &code, &timed_out);
        say("");
        say("What the graphics driver reports:");
        say(read_text(base / "log_report.txt"));
    }
    {
        MEMORYSTATUSEX memory{};
        memory.dwLength = sizeof memory;
        if (GlobalMemoryStatusEx(&memory)) {
            char line[200];
            std::snprintf(line, sizeof line, "Memory: %.1f GB RAM (%.1f GB free), commit limit %.1f GB (%.1f GB free)",
                          double(memory.ullTotalPhys) / 1e9, double(memory.ullAvailPhys) / 1e9,
                          double(memory.ullTotalPageFile) / 1e9, double(memory.ullAvailPageFile) / 1e9);
            say(line);
        }
    }

    wchar_t system_dir[MAX_PATH] = {};
    GetSystemDirectoryW(system_dir, MAX_PATH);
    if (!fs::exists(fs::path(system_dir) / "vulkan-1.dll")) {
        say("");
        say("PROBLEM: this PC has no Vulkan runtime (vulkan-1.dll). Install the latest AMD Adrenalin graphics driver and run the test again.");
        finish(base, 1);
    }
    for (const char* file : {"run_frame.exe", "in_1080p.rgba8", "in_720p.rgba8", "radv_1080p.rgba8"})
        if (!fs::exists(base / file)) {
            say(std::string("PROBLEM: ") + file + " is missing. Unzip the whole folder before running the test.");
            finish(base, 1);
        }

    // ---- the model ----------------------------------------------------------------------------------
    const fs::path model = base / "dlssnr-amd" / "dlssnr.bin";
    say("");
    if (fs::exists(model) && sha256_file(model) == kModelSha) {
        say("Model: found, already made.");
    } else {
        fs::path source;
        std::error_code ec;
        if (fs::exists(base / "nvngx_dlssnr.dll")) source = base / "nvngx_dlssnr.dll";
        std::vector<fs::path> zips;
        for (const auto& entry : fs::directory_iterator(base, ec))
            if (entry.is_regular_file() && entry.path().extension() == L".zip") zips.push_back(entry.path());
        if (source.empty() && zips.empty()) {
            say("PROBLEM: the test needs the file you were sent separately (nvngx_dlssnr.dll, or a zip with it). Put it in this folder, next to Run test.bat, and run the test again.");
            finish(base, 1);
        }
        const fs::path work = base / "_unpacked";
        if (source.empty()) {
            say("Model: looking inside " + zips[0].filename().u8string() + " ...");
            fs::create_directories(work);
            wchar_t windows_dir[MAX_PATH] = {};
            GetWindowsDirectoryW(windows_dir, MAX_PATH);
            DWORD code = 0;
            bool timed_out = false;
            run(quote(fs::path(windows_dir) / "System32" / "tar.exe") + L" -xf " + quote(zips[0]) + L" -C " + quote(work), base,
                base / "log_unzip.txt", 600000, &code, &timed_out);
            for (const auto& entry : fs::recursive_directory_iterator(work, ec))
                if (entry.is_regular_file() && entry.path().filename() == L"nvngx_dlssnr.dll") source = entry.path();
            if (source.empty()) {
                say("PROBLEM: no nvngx_dlssnr.dll was found inside " + zips[0].filename().u8string() + ".");
                finish(base, 1);
            }
        }
        say("Model: making it from " + source.filename().u8string() + " (it is only read, never run) ...");
        fs::create_directories(base / "dlssnr-amd");
        DWORD code = 0;
        bool timed_out = false;
        run(quote(base / "model-tools" / "dlssnr_extract_model.exe") + L" " + quote(source) + L" " + quote(model), base,
            base / "log_model.txt", 900000, &code, &timed_out);
        fs::remove_all(work, ec);
        if (code != 0 || !fs::exists(model)) {
            say("PROBLEM: the model could not be made from that file:");
            say(tail_lines(read_text(base / "log_model.txt"), 6));
            say("The file you were sent may be the wrong version. Please tell the person who sent you this test.");
            finish(base, 1);
        }
        if (sha256_file(model) != kModelSha) {
            say("PROBLEM: the model that was made is not the expected one.");
            fs::remove(model, ec);
            finish(base, 1);
        }
        say("Model: done.");
    }

    // ---- the tests -------------------------------------------------------------------------------------
    const DWORD first_limit = fs::exists(base / "dlssnr-amd" / "pipeline.cache") ? 10 * 60000 : 30 * 60000;
    say("");
    // The first two tests leave out the persistent kernels (NR_NO_PERSIST: every layer is its own dispatch, on
    // pipelines the driver's compiler takes); the last two use them as shipped. The tile-chain (counters one
    // dispatch waits on) is off in the first test of each pair, so a counter that never advances cannot hang the GPU.
    say("Test 1 of 4: layer by layer, barriers between GPU steps");
    const ModeResult flat_barriers = run_mode(base, "flat_barriers", true, first_limit, {{"NR_NO_PERSIST", "1"}}, "nopersist_barriers");
    say("");
    say("Test 2 of 4: layer by layer, tile counters");
    const ModeResult flat_counters = run_mode(base, "flat_counters", false, 10 * 60000, {{"NR_NO_PERSIST", "1"}}, "nopersist_default");
    say("");
    say("Test 3 of 4: persistent kernels, barriers between GPU steps");
    const ModeResult barriers = run_mode(base, "barriers", true, 10 * 60000);
    say("");
    say("Test 4 of 4: persistent kernels, tile counters (the default)");
    const ModeResult normal = run_mode(base, "default", false, 10 * 60000);
    const struct { const char* label; const ModeResult* result; } all_modes[] = {
        {"Layer by layer, barriers:     ", &flat_barriers}, {"Layer by layer, counters:     ", &flat_counters},
        {"Persistent, barriers:         ", &barriers},      {"Persistent, counters (default):", &normal}};

    // When both runs fail, find out which shaders the driver's compiler cannot take (each is tried in its own process).
    bool probed = false;
    bool all_failed = true;
    for (const auto& m : all_modes) all_failed = all_failed && m.result->verdict.rfind("OK", 0) != 0;
    if (all_failed && fs::exists(base / "probe.exe")) {
        say("");
        say("All four tests failed. Now trying each shader on its own to find the cause. This can take up to 45 minutes;");
        say("the window may look idle. Please leave it running.");
        DWORD code = 0;
        bool timed_out = false;
        run(quote(base / "probe.exe") + L" . 45", base, base / "log_probe.txt", 60 * 60000, &code, &timed_out);
        probed = fs::exists(base / "probe_results.txt");
        if (timed_out) say("The probe did not finish; the shaders it did try are listed below.");
    }

    say("");
    say("==================  SUMMARY  ==================");
    for (const auto& entry : all_modes) {
        const ModeResult* m = entry.result;
        char line[300];
        say(std::string(entry.label) + " " + m->verdict);
        if (m->ms_1080 > 0 || m->ms_720 > 0) {
            std::snprintf(line, sizeof line, "    time per frame: 1080p %.2f ms, 720p %.2f ms", m->ms_1080, m->ms_720);
            say(line);
        }
        if (!m->detail.empty()) say("    last lines of the log:\n" + m->detail);
    }
    if (probed) {
        say("");
        say("==================  SHADER PROBE  ==================");
        say(read_text(base / "probe_results.txt"));
    } else if (fs::exists(base / "log_probe.txt")) {
        say("");
        say("==================  SHADER PROBE (unfinished)  ==================");
        say(tail_lines(read_text(base / "log_probe.txt"), 80));
    }
    say("(For reference, Linux with an RX 7900 XTX takes about 15 ms at 1080p and 8.5 ms at 720p.)");
    finish(base, 0);
    return 0;
}
