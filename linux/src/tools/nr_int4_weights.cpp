// dlssnr-int4-weights: makes dlssnr-amd/dlssnr-int4.bin at install time (package/install.sh, int4_mixed chosen).
// The int4 weights are not shipped: they are rebuilt from the model's own weights (dlssnr.bin) and the package's
// recovery tables (int4/vit, int4/swin: *.fix.bin, *.mu.bin) by the graph builder's own decoder, run here exactly as
// the runtime's int4 build runs it (same plan, same settings), stopping before any GPU work.
//   dlssnr-int4-weights <dlssnr.bin> <int4 data: vit/, swin/, settings.txt, shaders/> <output file>
#define NR_NO_MAIN
#include "nr_graph.cpp"
#include "nr_native_plan.hpp"
#include <sstream>

int main(int argc, char** argv) try {
    if (argc != 4) { std::fprintf(stderr, "usage: %s <dlssnr.bin> <int4 data dir> <output file>\n", argv[0]); return 2; }
    const std::filesystem::path pack = std::filesystem::absolute(argv[1]), i4 = std::filesystem::absolute(argv[2]);
    std::map<std::string, std::string> cfg;
    cfg["NR_I4_DIR"] = (i4 / "vit").string();
    if (std::filesystem::is_directory(i4 / "swin")) cfg["NR_SWI4_DIR"] = (i4 / "swin").string();
    {   // the runtime's reading of int4/settings.txt (nr_runtime.cpp)
        const auto t = slurp((i4 / "settings.txt").string());
        if (t.empty()) throw std::runtime_error("missing " + (i4 / "settings.txt").string());
        std::istringstream lines(std::string(t.begin(), t.end()));
        for (std::string l; std::getline(lines, l);) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            const auto eq = l.find('=');
            if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
            cfg[l.substr(0, eq)] = l.substr(eq + 1);
        }
    }
    nr::set_log_sink([](const char* m) { std::fprintf(stderr, "%s\n", m); });
    const auto native = nr::make_native_plan(1920, 1080);   // the matrices do not depend on the extent
    std::istringstream plan_stream(native.text);
    const auto plan = parse_plan(plan_stream);
    std::vector<std::string> a = {"dlssnr-int4-weights", "--plan", "compiled native descriptor",
        "--unpacked", (pack.parent_path() / "model").string(), "--spv-dir", (i4 / "shaders").string(),
        "--host-boundary", "--no-reuse", "--source-width", "1920", "--source-height", "1080", "--accumulation", "fp32",
        "--model-pack", pack.string(), "--int4-weights-out", argv[3]};
    std::vector<char*> av;
    for (auto& s : a) av.push_back(s.data());
    const nr::BuildCfgScope scope(&cfg);
    NrSession s;
    const int rc = s.build(int(av.size()), av.data(), &plan);
    if (rc != 2) { std::fprintf(stderr, "dlssnr-int4-weights: the build did not reach the int4 weights (%d)\n", rc); return 1; }
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "dlssnr-int4-weights: %s\n", e.what());
    return 1;
}
