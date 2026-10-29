#pragma once
// The menus' [RenoDX] section, drawn from nr_renodx_keys.hpp's table: NR Control (the OptiScaler routes' own menu,
// linux/src/layer/nr_control_layer.cpp) and the ReShade add-on's page (linux/src/pe/nr_reshade_addon.cpp) show the same
// controls in the same groups. ImGui only; the caller owns the context and the values.
//
// `A` gives each key's value by its table index: bool& b(int), uint32_t& u(int), float& f(int), std::string& s(int).
// Sliders and text are committed when the control is let go (one write a drag, not one a frame); a checkbox or a
// combo at once. Returns whether anything was committed this frame.
#include "imgui.h"
#include "nr_renodx_keys.hpp"
#include "pe/nr_engine_panel.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nr::control_ui {

enum class Route { kOptiScaler, kReShade };

inline int key_index(const char* name) {
    for (int i = 0; i < renodx_keys::kCount; ++i)
        if (std::strcmp(renodx_keys::kKeys[i].name, name) == 0) return i;
    return -1;
}

inline void help(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// One key's control. `pending` holds a dragged slider's value until it is let go.
template <class A>
bool draw_key(A& a, int i, float hi_limit = 0.f) {
    const renodx_keys::Key& k = renodx_keys::kKeys[i];
    const float hi = hi_limit > 0.f ? hi_limit : k.hi;
    static std::map<int, float> pending;
    static std::map<int, std::string> text;
    bool changed = false;
    ImGui::PushID(i);
    switch (k.kind) {
        case renodx_keys::kBool: {
            bool v = a.b(i);
            if (ImGui::Checkbox(k.label, &v)) { a.b(i) = v; changed = true; }
            break;
        }
        case renodx_keys::kUint: {
            if (k.items) {
                int v = int(a.u(i));
                if (ImGui::Combo(k.label, &v, k.items)) { a.u(i) = uint32_t(v); changed = true; }
            } else {
                auto it = pending.find(i);
                int v = it != pending.end() ? int(it->second) : int(a.u(i));
                if (ImGui::SliderInt(k.label, &v, int(k.lo), int(hi), "%d", ImGuiSliderFlags_AlwaysClamp)) pending[i] = float(v);
                if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(i)) {
                    a.u(i) = uint32_t(pending[i]); pending.erase(i); changed = true;
                }
            }
            break;
        }
        case renodx_keys::kFloat: {
            auto it = pending.find(i);
            float v = it != pending.end() ? it->second : a.f(i);
            char fmt[8];
            std::snprintf(fmt, sizeof fmt, "%%.%uf", unsigned(k.decimals));
            if (ImGui::SliderFloat(k.label, &v, k.lo, k.hi, fmt, ImGuiSliderFlags_AlwaysClamp)) pending[i] = v;
            if (ImGui::IsItemDeactivatedAfterEdit() && pending.count(i)) {
                a.f(i) = pending[i]; pending.erase(i); changed = true;
            }
            break;
        }
        case renodx_keys::kText: {
            auto it = text.find(i);
            std::string& t = it != text.end() ? it->second : (text[i] = a.s(i));
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s", t.c_str());
            if (ImGui::InputText(k.label, buf, sizeof buf)) t = buf;
            if (ImGui::IsItemDeactivatedAfterEdit()) { a.s(i) = t; text.erase(i); changed = true; }
            else if (!ImGui::IsItemActive()) text.erase(i);
            break;
        }
    }
    if (k.kind == renodx_keys::kFloat || (k.kind == renodx_keys::kUint && !k.items)) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset")) {
            if (k.kind == renodx_keys::kFloat) a.f(i) = k.def;
            else a.u(i) = uint32_t(k.def);
            pending.erase(i);
            changed = true;
        }
    }
    help(k.help);
    ImGui::PopID();
    return changed;
}

// Whether a key belongs on this route's menu (the Present path's on the ReShade route only, the hook point and the
// game-exposure feed on the OptiScaler route only).
inline bool on_route(const char* name, Route route) {
    const bool present = std::strncmp(name, "NRPresent", 9) == 0;
    if (route == Route::kOptiScaler) return !present;
    return std::strcmp(name, "NRHookPoint") != 0 && std::strcmp(name, "NRFeedMode") != 0;
}

// The whole section, group by group in the table's order. `running`: the option is on in this run (its keys apply);
// off, only Enabled is offered (its other settings apply once it is on after a restart). `screenshot`: the menu's own
// capture button and status, drawn at the end of the Screenshot group.
template <class A>
bool draw_renodx(A& a, Route route, bool running, const std::function<void()>& screenshot = {}) {
    using namespace renodx_keys;
    bool changed = false;
    std::vector<const char*> groups;
    for (int i = 0; i < kCount; ++i) {
        bool seen = false;
        for (const char* g : groups) seen |= std::strcmp(g, kKeys[i].group) == 0;
        if (!seen) groups.push_back(kKeys[i].group);
    }
    auto value_of = [&](const char* name) -> float {
        const int i = key_index(name);
        if (i < 0) return 0.f;
        switch (kKeys[i].kind) {
            case kBool: return a.b(i) ? 1.f : 0.f;
            case kUint: return float(a.u(i));
            case kFloat: return a.f(i);
            default: return 0.f;
        }
    };
    for (const char* g : groups) {
        const bool first = std::strcmp(g, "Neural Rendering") == 0;
        if (!running && !first) break;
        int pass = 0;   // "Pass 2", "Pass 3", "Pass 4 and later"
        if (std::strncmp(g, "Pass ", 5) == 0) pass = g[5] - '0';
        if (pass && value_of("NRPasses") < float(pass)) continue;   // a pass the stack does not run
        if (!ImGui::CollapsingHeader(g, first ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
        for (int i = 0; i < kCount; ++i) {
            const Key& k = kKeys[i];
            if (std::strcmp(k.group, g) != 0 || !on_route(k.name, route)) continue;
            if (!running && std::strcmp(k.name, "Enabled") != 0) continue;
            bool off = false;   // greyed out: the value is kept but does nothing now
            if (std::strncmp(k.name, "NRLook", 6) == 0 && std::strcmp(k.name, "NRLookMode") != 0 &&
                std::strcmp(k.name, "NRLookUpsample") != 0)
                off = value_of("NRLookMode") == 0.f;
            if ((std::strcmp(k.name, "NRLookStabilizeMs") == 0 || std::strcmp(k.name, "NRLookStabilizeDetail") == 0) &&
                value_of("NRLookStabilize") == 0.f)
                off = true;
            if (std::strcmp(k.name, "NRSkinStructure") == 0) off = value_of("NRAutoMask") == 0.f;
            if (pass) {
                char follow[32];
                std::snprintf(follow, sizeof follow, "NRPass%dFollowPass1", pass);
                const char* steering[] = {"Style", "LocalTone", "Structure", "Skin", "AutoMask"};
                for (const char* s : steering) {
                    char n[32];
                    std::snprintf(n, sizeof n, "NRPass%d%s", pass, s);
                    if (std::strcmp(k.name, n) == 0) off = value_of(follow) != 0.f;
                }
            }
            // NRPasses: RenoDX's 4 unless NRUnlockPasses
            const float hi = std::strcmp(k.name, "NRPasses") == 0 && value_of("NRUnlockPasses") == 0.f ? 4.f : 0.f;
            ImGui::BeginDisabled(off);
            changed |= draw_key(a, i, hi);
            ImGui::EndDisabled();
        }
        if (screenshot && std::strcmp(g, "Screenshot") == 0) screenshot();
    }
    if (!running)
        ImGui::TextDisabled("RenoDX style is off in this run: check it above and restart the game.");
    return changed;
}

// The engine-data panel: what the colours mean, then one line (nr::pe::panel::split).
inline void engine_legend() {
    float c[4];
    const char* const parts[][2] = {{"+", "from the game"}, {"~", "filled in (not from the game)"}, {"-", "missing (nothing fills it in)"}};
    for (int i = 0; i < 3; ++i) {
        nr::pe::panel::state_colour(parts[i][0][0], c);
        if (i) { ImGui::SameLine(); ImGui::TextDisabled("/"); ImGui::SameLine(); }
        ImGui::TextColored(ImVec4(c[0], c[1], c[2], c[3]), "%s", parts[i][1]);
    }
}
inline void engine_line(const std::string& l) {
    const nr::pe::panel::Drawn d = nr::pe::panel::split(l);
    const ImVec4 c(d.rgba[0], d.rgba[1], d.rgba[2], d.rgba[3]);
    if (d.whole) ImGui::PushStyleColor(ImGuiCol_Text, c);
    if (!d.label.empty()) {
        ImGui::TextColored(c, "%s", d.label.c_str());
        ImGui::SameLine(0.f, 0.f);
    }
    ImGui::TextWrapped("%s", d.rest.c_str());
    if (d.whole) ImGui::PopStyleColor();
}

}  // namespace nr::control_ui
