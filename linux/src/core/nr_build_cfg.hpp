#pragma once
// Settings one network build reads - the installed int4 mixed data's choices (dlssnr-amd/int4/settings.txt) and its
// weights file - set by the runtime around its build, on the building thread. Never the process environment: two
// sessions may build at once, and an exact build after an int4 one must see none of them. Unset (development trees, the
// nr_graph tool): the environment, as before.
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>

namespace nr {
inline thread_local const std::map<std::string, std::string>* g_build_cfg = nullptr;
inline const char* build_cfg(const char* key) {
    if (g_build_cfg) {
        const auto it = g_build_cfg->find(key);
        return it == g_build_cfg->end() ? nullptr : it->second.c_str();
    }
    return std::getenv(key);
}
// RAII: `cfg` for the builds on this thread while it lives.
struct BuildCfgScope {
    const std::map<std::string, std::string>* prev;
    explicit BuildCfgScope(const std::map<std::string, std::string>* cfg) : prev(g_build_cfg) { g_build_cfg = cfg; }
    ~BuildCfgScope() { g_build_cfg = prev; }
};
// dlssnr-amd/int4/settings.txt, KEY=VALUE a line ('#' comments): the selective hard-swish - its scales folded into the
// next weights (NR_HS_FOLD wide layers, NR_HS_FOLD32 C=32, NR_HS_FOLDPP the pre and post blocks), the blocks that
// take the fold (NR_HS_FOLD_BLOCKS) and the blocks whose C=32 layers run the hard-swish pipeline (NR_HS_ROUTE).
inline void int4_settings(const std::string& text, std::map<std::string, std::string>& cfg) {
    static const std::set<std::string> keys = {"NR_HS_FOLD", "NR_HS_FOLD32", "NR_HS_FOLDPP", "NR_HS_FOLD_BLOCKS", "NR_HS_ROUTE"};
    std::istringstream lines(text);
    for (std::string l; std::getline(lines, l);) {
        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const auto eq = l.find('=');
        if (l.empty() || l[0] == '#' || eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (!keys.count(k)) throw std::runtime_error("int4 settings.txt: unknown key " + k);
        cfg[k] = v;
    }
}
}  // namespace nr
