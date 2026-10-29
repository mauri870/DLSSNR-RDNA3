#pragma once
#include "nr_renodx_keys.hpp"
namespace nr::wilsjo2_keys {
inline constexpr renodx_keys::Key kHistory = {
    "History", renodx_keys::kFloat, 2, 0.0f, 1.0f, 1.0f, "Network", "History (previous frame blend)",
    "History. The post block's previous-frame blend.", nullptr};
}
