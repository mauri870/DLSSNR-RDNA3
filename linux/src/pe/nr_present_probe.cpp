// nr_present_probe.exe (dlssnr-amd\): where DXGI's swap chain methods are, for NR Control (nr_pe_menu.cpp).
//
// The menu hooks the system's IDXGISwapChain::Present, Present1 and SetColorSpace1. Their addresses come from a swap
// chain's vtable, and a swap chain made inside the game is seen by everything hooked there - OptiScaler takes a D3D12
// device made beside the game's for the game's own, and under Proton a probe swap chain's Vulkan surface for the
// game's window - and then uses it after it is gone. So the swap chains are made here, in a process of their own,
// and each method is written as its module's path and its offset in that module; system DLLs sit at the same base
// in every process of a session, so the game's process finds the same code at the same offset.
//
//   nr_present_probe.exe <output file>
// One line per method found: "<method> <api> <offset, hex> <first 16 bytes, hex> <module path>". Exit 0 when anything
// was found.
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

template <class T> void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

void write(FILE* f, IDXGISwapChain1* sc, const char* api) {
    void** vt = *reinterpret_cast<void***>(sc);
    const struct { const char* name; int slot; } methods[] = {{"Present", 8}, {"Present1", 22}, {"SetColorSpace1", 38}};
    for (const auto& m : methods) {
        HMODULE owner = nullptr;
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                static_cast<LPCWSTR>(vt[m.slot]), &owner) ||
            !GetModuleFileNameW(owner, path, MAX_PATH))
            continue;
        char utf8[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, nullptr, nullptr);
        const unsigned long long offset =
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(vt[m.slot]) - reinterpret_cast<uintptr_t>(owner));
        char bytes[33] = {};
        for (int i = 0; i < 16; ++i)
            std::snprintf(bytes + 2 * i, 3, "%02x", static_cast<const unsigned char*>(vt[m.slot])[i]);
        std::fprintf(f, "%s %s %llx %s %s\n", m.name, api, offset, bytes, utf8);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    FILE* f = _wfopen(argv[1], L"w");
    if (!f) return 2;
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"nr_present_probe";
    RegisterClassExW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr,
                                  wc.hInstance, nullptr);
    IDXGIFactory2* factory = nullptr;
    if (!window || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory) { std::fclose(f); return 1; }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 64; sd.Height = 64; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    bool any = false;
    // D3D12 first, each written out at once: whatever one API does to the process, the other's lines are kept.
    {
        ID3D12Device* device = nullptr;
        ID3D12CommandQueue* queue = nullptr;
        IDXGISwapChain1* sc = nullptr;
        D3D12_COMMAND_QUEUE_DESC qd{};
        HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
        auto create = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
                                  reinterpret_cast<void*>(GetProcAddress(d3d12, "D3D12CreateDevice"))) : nullptr;
        if (create && SUCCEEDED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))) && device &&
            SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))) && queue &&
            SUCCEEDED(factory->CreateSwapChainForHwnd(queue, window, &sd, nullptr, nullptr, &sc)) && sc) {
            write(f, sc, "D3D12");
            std::fflush(f);
            any = true;
        }
        release(sc);
        release(queue);
        release(device);
    }
    {
        ID3D11Device* device = nullptr;
        IDXGISwapChain1* sc = nullptr;
        HMODULE d3d11 = LoadLibraryW(L"d3d11.dll");
        auto create = d3d11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
                                  reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"))) : nullptr;
        if (create && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, nullptr, nullptr)) && device &&
            SUCCEEDED(factory->CreateSwapChainForHwnd(device, window, &sd, nullptr, nullptr, &sc)) && sc) {
            write(f, sc, "D3D11");
            std::fflush(f);
            any = true;
        }
        release(sc);
        release(device);
    }
    release(factory);
    DestroyWindow(window);
    std::fclose(f);
    return any ? 0 : 1;
}
