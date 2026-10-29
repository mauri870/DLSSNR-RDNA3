#pragma once
// NR_TEST_FAULT (tests only: the chaos gates): named calls made to fail on purpose, to drive the failure and recovery
// paths a game rarely reaches. A comma list of <site>:<when>, <when> one of
//   N    every N-th time the site is reached        ~N   at random, one time in N (NR_TEST_FAULT_SEED, else 1)
//   @K   the K-th time only                         *    every time
// e.g. NR_TEST_FAULT=present_submit:~20,d11_wait:@300. Not set: one load of a static per site, nothing else.
#include "nr_pe_log.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace nr::pe {

namespace fault_detail {
struct Rule { std::string site; char kind = 0; uint64_t n = 0; std::atomic<uint64_t> calls{0}, fired{0}; };
struct Rules {
    Rule rule[16];
    int count = 0;
    uint64_t seed = 1;
    std::mutex lock;   // the random generator
    Rules() {
        const char* e = std::getenv("NR_TEST_FAULT");
        if (const char* s = std::getenv("NR_TEST_FAULT_SEED")) seed = std::strtoull(s, nullptr, 10) | 1u;
        for (std::string text = e ? e : ""; !text.empty() && count < 16;) {
            const size_t comma = text.find(',');
            const std::string item = text.substr(0, comma);
            text = comma == std::string::npos ? std::string() : text.substr(comma + 1);
            const size_t colon = item.find(':');
            if (colon == std::string::npos || colon + 1 >= item.size()) continue;
            Rule& r = rule[count];
            r.site = item.substr(0, colon);
            const char k = item[colon + 1];
            r.kind = k == '~' || k == '@' || k == '*' ? k : 'n';
            r.n = std::strtoull(item.c_str() + colon + 1 + (r.kind == 'n' ? 0 : 1), nullptr, 10);
            if (r.kind != '*' && !r.n) continue;
            log("[nr] test: fault site %s armed (%s)", r.site.c_str(), item.c_str() + colon + 1);
            ++count;
        }
    }
    uint64_t next() {   // xorshift64*
        std::lock_guard<std::mutex> g(lock);
        seed ^= seed >> 12; seed ^= seed << 25; seed ^= seed >> 27;
        return seed * 2685821657736338717ull;
    }
};
inline Rules& rules() { static Rules r; return r; }
}  // namespace fault_detail

// True when this call of `site` is to fail (logged: the first 5 of each site, then every 100th).
inline bool test_fault(const char* site) {
    static const bool any = std::getenv("NR_TEST_FAULT") != nullptr;
    if (!any) return false;
    fault_detail::Rules& rs = fault_detail::rules();
    for (int i = 0; i < rs.count; ++i) {
        fault_detail::Rule& r = rs.rule[i];
        if (r.site != site) continue;
        const uint64_t call = ++r.calls;
        const bool fail = r.kind == '*' || (r.kind == 'n' && call % r.n == 0) || (r.kind == '@' && call == r.n) ||
                          (r.kind == '~' && rs.next() % r.n == 0);
        if (!fail) return false;
        const uint64_t fired = ++r.fired;
        if (fired <= 5 || fired % 100 == 0)
            log("[nr] test: fault %s injected (call %llu, fault %llu)", site, static_cast<unsigned long long>(call),
                static_cast<unsigned long long>(fired));
        return true;
    }
    return false;
}

}  // namespace nr::pe
