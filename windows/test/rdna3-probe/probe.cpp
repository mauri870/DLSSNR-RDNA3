// probe: tries to build every compute pipeline of the RDNA3 network on this machine's Vulkan driver, each in
// its own process, and reports which ones the driver's shader compiler accepts and which it crashes on.
//
//   probe <folder with dlssnr-amd and probe-variants> [minutes]   all shaders, results in probe_results.txt
//   probe --one <file.spv>                                          one shader (what the first form runs)
//
// A shader's buffer bindings, images and push-constant size are read from its SPIR-V, so any .spv works.
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include "nrvk.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

// ---- SPIR-V reflection ---------------------------------------------------------------------------------------
struct Shape {
    uint32_t buffers = 0, push = 0;
    std::vector<bool> image_sampled;   // by order of binding; true: combined image sampler, false: storage image
};

static Shape reflect(const std::vector<uint32_t>& code) {
    std::map<uint32_t, uint32_t> binding, pointee, var_type, var_class, member_count;
    std::map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> members;   // struct -> (member, offset)
    std::map<uint32_t, uint32_t> kind, width, element, count, stride, constant;   // type id -> ...
    std::map<uint32_t, std::vector<uint32_t>> struct_types;
    std::map<uint32_t, uint32_t> image_sampled_flag;
    enum { Int = 1, Float, Vector, Array, Struct, Image, SampledImage, Matrix, Other };
    for (size_t i = 5; i < code.size();) {
        const uint32_t op = code[i] & 0xFFFFu, wc = code[i] >> 16;
        if (!wc || i + wc > code.size()) break;
        const uint32_t* w = &code[i];
        switch (op) {
        case 21: kind[w[1]] = Int; width[w[1]] = w[2]; break;                        // OpTypeInt
        case 22: kind[w[1]] = Float; width[w[1]] = w[2]; break;                      // OpTypeFloat
        case 23: kind[w[1]] = Vector; element[w[1]] = w[2]; count[w[1]] = w[3]; break;
        case 24: kind[w[1]] = Matrix; element[w[1]] = w[2]; count[w[1]] = w[3]; break;
        case 25: kind[w[1]] = Image; image_sampled_flag[w[1]] = w[7]; break;          // OpTypeImage ... Sampled
        case 27: kind[w[1]] = SampledImage; break;
        case 28: kind[w[1]] = Array; element[w[1]] = w[2]; count[w[1]] = w[3]; break;   // length is a constant id
        case 30: kind[w[1]] = Struct; struct_types[w[1]].assign(w + 2, w + wc); break;
        case 32: pointee[w[1]] = w[3]; var_class[w[1]] = w[2]; break;                // OpTypePointer
        case 43: constant[w[2]] = w[3]; break;                                       // OpConstant (32-bit)
        case 59: var_type[w[2]] = w[1]; var_class[w[2]] = w[3]; break;               // OpVariable
        case 71:                                                                      // OpDecorate
            if (w[2] == 33) binding[w[1]] = w[3];
            else if (w[2] == 6) stride[w[1]] = w[3];
            break;
        case 72:                                                                      // OpMemberDecorate
            if (w[3] == 35) members[w[1]].push_back({w[2], w[4]});
            break;
        default: break;
        }
        i += wc;
    }
    std::function<uint32_t(uint32_t)> size_of = [&](uint32_t t) -> uint32_t {
        switch (kind.count(t) ? kind[t] : Other) {
        case Int: case Float: return width[t] / 8;
        case Vector: return count[t] * size_of(element[t]);
        case Matrix: return count[t] * size_of(element[t]);
        case Array: { const uint32_t n = constant.count(count[t]) ? constant[count[t]] : 1;
                      return n * (stride.count(t) ? stride[t] : size_of(element[t])); }
        case Struct: { uint32_t end = 0; auto& m = members[t];
                       for (auto& mo : m) end = std::max(end, mo.second + size_of(struct_types[t][mo.first]));
                       return end; }
        default: return 4;
        }
    };
    Shape s;
    std::map<uint32_t, bool> images;   // binding -> sampled
    for (const auto& v : var_type) {
        const uint32_t cls = var_class[v.first];
        const uint32_t target = pointee.count(v.second) ? pointee[v.second] : 0;
        if (cls == 12 && binding.count(v.first) && binding[v.first] < nrvk::Kernel::kAliasBase)
            s.buffers = std::max(s.buffers, binding[v.first] + 1);
        else if (cls == 9) s.push = (size_of(target) + 3u) & ~3u;
        else if (cls == 0 && binding.count(v.first)) {
            // an array of images is not used by this network
            if (kind.count(target) && kind[target] == SampledImage) images[binding[v.first]] = true;
            else if (kind.count(target) && kind[target] == Image) images[binding[v.first]] = image_sampled_flag[target] != 2;
        }
    }
    for (const auto& im : images) s.image_sampled.push_back(im.second);
    return s;
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* e) {
    void* address = e->ExceptionRecord->ExceptionAddress;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)address, &module) && module)
        GetModuleFileNameA(module, name, MAX_PATH);
    const char* slash = std::strrchr(name, '\\');
    std::printf("CRASH exception 0x%08lx at %s+0x%llx\n", e->ExceptionRecord->ExceptionCode, slash ? slash + 1 : name,
                (unsigned long long)((char*)address - (char*)module));
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

static int probe_one(const std::string& path) try {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
    if (std::getenv("PROBE_TEST_CRASH")) *(volatile int*)0 = 1;   // exercises the crash report
    const std::vector<uint32_t> code = nrvk::read_spirv(path);
    Shape shape = reflect(code);
    {   // The real run's bindings and push size for this shader, where the table has one (probe-shapes.txt beside the exe).
        std::ifstream table("probe-shapes.txt");
        std::string stem = fs::path(path).stem().string(), name;
        unsigned buffers, push;
        while (table >> name >> buffers >> push)
            if (name == stem) { shape.buffers = buffers; shape.push = push; }
    }
    nrvk::Context ctx;
    ctx.create();
    std::vector<nrvk::Buffer> buffers;
    std::vector<VkBuffer> handles;
    for (uint32_t i = 0; i < shape.buffers; ++i) { buffers.push_back(ctx.buffer(1 << 20)); handles.push_back(buffers.back().handle); }
    std::vector<nrvk::Context::Image> images;
    images.reserve(shape.image_sampled.size());
    for (bool sampled : shape.image_sampled) images.push_back(ctx.image(64, 64, VK_FORMAT_R32G32B32A32_SFLOAT, sampled));
    std::vector<nrvk::Context::Image*> image_pointers;
    for (auto& im : images) image_pointers.push_back(&im);
    std::printf("shape: %u buffers, %zu images, push %u bytes\n", shape.buffers, images.size(), shape.push);
    std::fflush(stdout);
    nrvk::Kernel kernel;
    kernel.create(ctx, path, handles, shape.push, image_pointers);
    std::printf("OK\n");
    return 0;
} catch (const std::exception& e) {
    std::printf("ERROR %s\n", e.what());
    return 3;
}

// ---- what the driver reports -----------------------------------------------------------------------------
static int report() try {
    nrvk::Context ctx;
    ctx.create();
    VkPhysicalDeviceSubgroupProperties sub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties sgc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv; drv.pNext = &sub; sub.pNext = &sgc;
    vkGetPhysicalDeviceProperties2(ctx.physical, &p2);
    const VkPhysicalDeviceLimits& l = p2.properties.limits;
    std::printf("device '%s' vendor 0x%x id 0x%x api %u.%u.%u driver 0x%x\n", p2.properties.deviceName, p2.properties.vendorID, p2.properties.deviceID,
                VK_VERSION_MAJOR(p2.properties.apiVersion), VK_VERSION_MINOR(p2.properties.apiVersion), VK_VERSION_PATCH(p2.properties.apiVersion), p2.properties.driverVersion);
    std::printf("driver '%s' '%s'\n", drv.driverName, drv.driverInfo);
    std::printf("maxComputeSharedMemorySize %u, maxComputeWorkGroupInvocations %u, maxComputeWorkGroupSize %u x %u x %u\n",
                l.maxComputeSharedMemorySize, l.maxComputeWorkGroupInvocations, l.maxComputeWorkGroupSize[0], l.maxComputeWorkGroupSize[1], l.maxComputeWorkGroupSize[2]);
    std::printf("maxStorageBufferRange %u, maxPushConstantsSize %u, maxPerStageDescriptorStorageBuffers %u, maxBoundDescriptorSets %u\n",
                l.maxStorageBufferRange, l.maxPushConstantsSize, l.maxPerStageDescriptorStorageBuffers, l.maxBoundDescriptorSets);
    std::printf("subgroupSize %u, subgroup size range %u..%u (compute stage may require: %d), maxComputeWorkgroupSubgroups %u\n", sub.subgroupSize,
                sgc.minSubgroupSize, sgc.maxSubgroupSize, int((sgc.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) != 0), sgc.maxComputeWorkgroupSubgroups);
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(ctx.physical, nullptr, &count, exts.data());
    std::string names;
    for (const auto& e : exts) { std::string n = e.extensionName; if (n.find("cooperative") != std::string::npos || n.find("workgroup_memory") != std::string::npos || n.find("subgroup") != std::string::npos || n.find("shader_") != std::string::npos || n.find("memory_model") != std::string::npos || n.find("vulkan_memory") != std::string::npos) names += n + " "; }
    std::printf("%u device extensions; relevant: %s\n", count, names.c_str());
    auto fn = reinterpret_cast<PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR>(vkGetInstanceProcAddr(ctx.instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR"));
    if (fn) {
        uint32_t n = 0;
        fn(ctx.physical, &n, nullptr);
        std::vector<VkCooperativeMatrixPropertiesKHR> props(n, VkCooperativeMatrixPropertiesKHR{VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR});
        fn(ctx.physical, &n, props.data());
        std::printf("%u cooperative matrix configurations (M N K, A B C Result component types, scope, saturating):\n", n);
        for (const auto& p : props)
            std::printf("  %2u %2u %2u   %d %d %d %d   scope %d  sat %d\n", p.MSize, p.NSize, p.KSize, int(p.AType), int(p.BType), int(p.CType), int(p.ResultType), int(p.scope), int(p.saturatingAccumulation));
        std::printf("  (component types: 0 float16, 1 float32, 2 float64, 3 sint8, 4 sint16, 5 sint32, 6 sint64, 7 uint8, 8 uint16, 9 uint32, 10 uint64; scope 3 = subgroup)\n");
    }
    return 0;
} catch (const std::exception& e) {
    std::printf("report failed: %s\n", e.what());
    return 3;
}

// ---- the parent ----------------------------------------------------------------------------------------------
#ifdef _WIN32
static bool run_child(const fs::path& self, const fs::path& spv, const fs::path& log, DWORD timeout_ms, DWORD* code, bool* timed_out) {
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = out;
    startup.hStdInput = in;
    PROCESS_INFORMATION process{};
    std::wstring line = L"\"" + self.wstring() + L"\" --one \"" + spv.wstring() + L"\"";
    const BOOL started = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    *timed_out = false;
    *code = 0xFFFFFFFF;
    if (started) {
        if (WaitForSingleObject(process.hProcess, timeout_ms) == WAIT_TIMEOUT) {
            TerminateProcess(process.hProcess, 1);
            WaitForSingleObject(process.hProcess, 10000);
            *timed_out = true;
        }
        GetExitCodeProcess(process.hProcess, code);
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
    CloseHandle(out);
    CloseHandle(in);
    return started != 0;
}
#endif

static std::string read_text(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc >= 3 && std::wstring(argv[1]) == L"--one") return probe_one(fs::path(argv[2]).string());
    if (argc >= 2 && std::wstring(argv[1]) == L"--report") return report();
    if (argc < 2) { std::printf("usage: probe <folder> [minutes]\n"); return 2; }
    const fs::path root = argv[1];
    const double budget_minutes = argc >= 3 ? _wtof(argv[2]) : 45.0;
    wchar_t module[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const fs::path self = module;
    const fs::path logs = root / "probe_logs";
    fs::create_directories(logs);

    std::vector<fs::path> files;
    std::error_code ec;
    for (const char* dir : {"probe-micro", "probe-micro2", "probe-unrolled", "probe-variants"}) {
        std::vector<fs::path> group;
        for (const auto& e : fs::directory_iterator(root / dir, ec))
            if (e.path().extension() == ".spv") group.push_back(e.path());
        std::sort(group.begin(), group.end());
        files.insert(files.end(), group.begin(), group.end());
    }
    std::vector<fs::path> network;
    for (const char* sub : {"dlssnr-amd/shaders", "dlssnr-amd/shaders/runtime", "dlssnr-amd/shaders/temporal"})
        for (const auto& e : fs::directory_iterator(root / sub, ec))
            if (e.path().extension() == ".spv") network.push_back(e.path());
    std::sort(network.begin(), network.end(), [](const fs::path& a, const fs::path& b) { return fs::file_size(a) < fs::file_size(b); });
    files.insert(files.end(), network.begin(), network.end());

    std::ostringstream report;
    std::map<std::string, int> crash_sites;
    int ok = 0, crashed = 0, other = 0, skipped = 0;
    const auto begin = std::chrono::steady_clock::now();
    report << "Pipeline probe: " << files.size() << " shaders, budget " << budget_minutes << " minutes\n\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() / 60.0;
        const std::string label = files[i].parent_path().filename().string() + "/" + files[i].filename().string();
        if (elapsed > budget_minutes) { report << "NOT TRIED   " << label << "\n"; ++skipped; continue; }
        const fs::path log = logs / (std::to_string(i) + "_" + files[i].stem().string() + ".txt");
        DWORD code = 0;
        bool timed_out = false;
        const auto t0 = std::chrono::steady_clock::now();
        run_child(self, files[i], log, 5 * 60000, &code, &timed_out);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const std::string text = read_text(log);
        std::string verdict, site;
        std::smatch m;
        if (timed_out) { verdict = "TIMEOUT"; ++other; }
        else if (code == 0 && std::regex_search(text, std::regex("(^|\n)OK\r?(\n|$)"))) { verdict = "OK"; ++ok; }
        else if (std::regex_search(text, m, std::regex("CRASH exception (0x[0-9a-f]+) at ([^\\r\\n]+)"))) { verdict = "CRASH"; site = m[2]; ++crashed; ++crash_sites[site]; }
        else if (code == 0xC0000005u || code == 0xC0000409u) { verdict = "CRASH(no report)"; ++crashed; }
        else { verdict = "ERROR"; ++other; std::smatch e; if (std::regex_search(text, e, std::regex("ERROR ([^\\r\\n]+)"))) site = e[1]; }
        char line[400];
        std::snprintf(line, sizeof line, "%-16s %6.1f s  %-48s %s", verdict.c_str(), seconds, label.c_str(), site.c_str());
        std::printf("%s\n", line);
        std::fflush(stdout);
        report << line << "\n";
    }
    report << "\nOK " << ok << ", crashed " << crashed << ", other failures " << other << ", not tried " << skipped << "\n";
    for (const auto& s : crash_sites) report << "  " << s.second << " crash(es) at " << s.first << "\n";
    std::ofstream(root / "probe_results.txt", std::ios::binary) << report.str();
    std::printf("\n%s", report.str().substr(report.str().find("\nOK ")).c_str());
    return 0;
}
#else
int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--one") return probe_one(argv[2]);
    std::printf("probe is built for Windows\n");
    return 2;
}
#endif
