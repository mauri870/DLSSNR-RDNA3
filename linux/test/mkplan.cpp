// mkplan <width> <height> [small]: the native planner's frame plan for that extent, on stdout (small: the 310.9 small model).
#include "nr_native_plan.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const bool small = argc > 3 && std::string(argv[3]) == "small";
    std::fputs(nr::make_native_plan(uint32_t(atoi(argv[1])), uint32_t(atoi(argv[2])),
                                    small ? nr::NetworkModel::Small : nr::NetworkModel::Original).text.c_str(), stdout);
    return 0;
}
