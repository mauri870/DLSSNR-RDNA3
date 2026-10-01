// mkplan <width> <height>: the native planner's frame plan for that extent, on stdout.
#include "nr_native_plan.hpp"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::fputs(nr::make_native_plan(uint32_t(atoi(argv[1])), uint32_t(atoi(argv[2]))).text.c_str(), stdout);
    return 0;
}
