// The RenoDX-style option's NR screenshot (nr_screenshot.hpp). The encoding and the files are RenoDX v8.5.0-rc10's
// screenshot.hpp (MIT, Copyright (C) 2026, PEQHUB/RenoDX-DLSS5-Generic), ported with their logic unchanged: the
// readback decoding takes Vulkan formats instead of DXGI ones, WIC is created here instead of RenoDX's utils/png.hpp,
// and the log is ours. The arming, the GPU proof and the readbacks are our session's (nr_pe_session.cpp).
#include "nr_screenshot.hpp"

#include "nr_pe_log.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

namespace nr::pe::screenshot {
namespace {

// ---- arming (RenoDX's RequestCapture / FailRequest / StatusText) ----
struct OutputOptions {
    uint32_t format = 0;
    bool side_by_side = false;
    uint32_t jpeg_quality = 95;
    uint32_t max_megabytes = 10;
};
std::mutex g_options_mutex;
Options g_options;
std::atomic_bool g_requested{false};
std::atomic_bool g_model{false};        // the armed request's source
std::atomic_bool g_pending{false};      // copies recorded, waiting for the GPU and the worker
std::atomic_uint32_t g_frames_armed{0};
std::atomic_uint64_t g_serial{0};
OutputOptions g_requested_output;       // frozen when a request arms
// Frames an armed request may wait for a capture frame (RenoDX's kArmTimeoutPresents, ~10 s at 60 fps).
constexpr uint32_t kArmTimeoutFrames = 600;

std::mutex g_finished_mutex;
std::string g_outcome = "none";
bool g_finished_model = false;

std::mutex g_folder_mutex;
std::wstring g_folder;

void publish(const char* outcome, bool model) {
    std::lock_guard<std::mutex> lock(g_finished_mutex);
    g_outcome = outcome;
    g_finished_model = model;
}

// ---- PNG byte plumbing (screenshot.hpp) ----
void AppendPngBe32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xff));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xff));
    bytes.push_back(static_cast<uint8_t>(value & 0xff));
}

uint32_t ReadBe32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

uint32_t PngCrc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320u & (-(crc & 1u)));
    }
    return ~crc;
}

uint32_t PngAdler32(const uint8_t* data, size_t size) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < size; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void AppendPngChunk(std::vector<uint8_t>& png, const char type[4], const std::vector<uint8_t>& payload) {
    AppendPngBe32(png, static_cast<uint32_t>(payload.size()));
    const size_t type_offset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), payload.begin(), payload.end());
    AppendPngBe32(png, PngCrc32(png.data() + type_offset, 4 + payload.size()));
}

std::vector<uint8_t> ZlibStored(const uint8_t* data, size_t size) {
    std::vector<uint8_t> zlib = {0x78, 0x01};
    zlib.reserve(size + size / 65535 * 5 + 16);
    size_t offset = 0;
    do {
        const size_t block_size = std::min<size_t>(65535, size - offset);
        zlib.push_back(offset + block_size == size ? 1u : 0u);
        const uint16_t length = static_cast<uint16_t>(block_size);
        const uint16_t inverse = static_cast<uint16_t>(~length);
        zlib.push_back(static_cast<uint8_t>(length & 0xff));
        zlib.push_back(static_cast<uint8_t>(length >> 8));
        zlib.push_back(static_cast<uint8_t>(inverse & 0xff));
        zlib.push_back(static_cast<uint8_t>(inverse >> 8));
        zlib.insert(zlib.end(), data + offset, data + offset + block_size);
        offset += block_size;
    } while (offset < size);
    AppendPngBe32(zlib, PngAdler32(data, size));
    return zlib;
}

// ---- readback decoding ----
constexpr uint32_t kUnwrittenStamp[4] = {0x7FC0DEADu, 0x7FC0BEEFu, 0x7FC0CAFEu, 0x7FC0F00Du};

bool Unwritten(const void* mapped, uint64_t bytes) {
    return std::memcmp(mapped, kUnwrittenStamp, static_cast<size_t>(std::min<uint64_t>(sizeof(kUnwrittenStamp), bytes))) == 0;
}

float HalfToFloat(uint16_t bits) {
    const uint32_t sign = (bits & 0x8000u) << 16;
    const uint32_t exponent = (bits >> 10) & 0x1fu;
    const uint32_t mantissa = bits & 0x3ffu;
    uint32_t value = 0;
    if (exponent == 0) {
        if (mantissa != 0) {
            float result = std::ldexp(static_cast<float>(mantissa), -24);
            return (sign != 0 ? -result : result);
        }
        value = sign;
    } else if (exponent == 31) {
        value = sign | 0x7f800000u | (mantissa << 13);
    } else {
        value = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
    }
    float result = 0.f;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

float DecodeUnsignedFloat(uint32_t bits, uint32_t mantissa_bits) {
    const uint32_t mantissa_mask = (1u << mantissa_bits) - 1u;
    const uint32_t mantissa = bits & mantissa_mask;
    const uint32_t exponent = (bits >> mantissa_bits) & 0x1fu;
    if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), -14 - static_cast<int>(mantissa_bits));
    if (exponent == 31) return std::numeric_limits<float>::infinity();
    return std::ldexp(1.f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits),
                      static_cast<int>(exponent) - 15);
}

// SDR is already sRGB-coded, independent of storage type. HDR10 is already PQ/BT.2020 and is preserved. scRGB is
// BT.709 linear at 80 nits per unit; convert primaries in linear light, then encode absolute nits to PQ. A capture
// is never normalised by its maximum: the two halves would get different exposure.
float LinearNitsToPq(float nits) {
    const float p = std::pow(std::clamp(nits / 10000.f, 0.f, 1.f), 2610.f / 16384.f);
    return std::pow((3424.f / 4096.f + (2413.f / 128.f) * p) / (1.f + (2392.f / 128.f) * p), 2523.f / 32.f);
}

float PqToNits(float code) {
    const float e = std::pow(std::clamp(code, 0.f, 1.f), 32.f / 2523.f);
    return 10000.f * std::pow(std::max(e - 3424.f / 4096.f, 0.f) / (2413.f / 128.f - (2392.f / 128.f) * e),
                              16384.f / 2610.f);
}

// Display-coded RGB, 16 bits a channel, in the swap chain's own signal: sRGB codes for SDR, PQ BT.2020 codes for HDR.
struct CodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint16_t> rgb;   // row-major R, G, B
};

// Formats whose codes are exact at 8 bits: their 8-bit files need no dither.
bool EightBitFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default: return false;
    }
}

bool ReadbackToCoded(const Pair& capture, const void* mapped, CodedImage* image, bool* any_nonzero, bool* unwritten) {
    if (mapped == nullptr || capture.width == 0 || capture.height == 0 || capture.row_pitch == 0) return false;
    const uint32_t bpp = bytes_per_pixel(capture.format);
    if (!bpp) return false;
    *unwritten = Unwritten(mapped, capture.bytes);
    if (*unwritten) return false;
    bool saw_nonzero = false;
    image->width = capture.width;
    image->height = capture.height;
    image->rgb.assign(static_cast<size_t>(capture.width) * capture.height * 3u, 0);
    const uint8_t* base = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < capture.height; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * capture.row_pitch;
        for (uint32_t x = 0; x < capture.width; ++x) {
            float r = 0.f, g = 0.f, b = 0.f;
            switch (capture.format) {
                case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
                case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
                    r = row[x * 4u] / 255.f; g = row[x * 4u + 1] / 255.f; b = row[x * 4u + 2] / 255.f;
                    break;
                case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB:
                    b = row[x * 4u] / 255.f; g = row[x * 4u + 1] / 255.f; r = row[x * 4u + 2] / 255.f;
                    break;
                case VK_FORMAT_A2B10G10R10_UNORM_PACK32: {   // DXGI R10G10B10A2
                    uint32_t v; std::memcpy(&v, row + x * 4u, 4);
                    r = (v & 0x3ffu) / 1023.f; g = ((v >> 10) & 0x3ffu) / 1023.f; b = ((v >> 20) & 0x3ffu) / 1023.f;
                    break;
                }
                case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                    uint32_t v; std::memcpy(&v, row + x * 4u, 4);
                    b = (v & 0x3ffu) / 1023.f; g = ((v >> 10) & 0x3ffu) / 1023.f; r = ((v >> 20) & 0x3ffu) / 1023.f;
                    break;
                }
                case VK_FORMAT_B10G11R11_UFLOAT_PACK32: {    // DXGI R11G11B10
                    uint32_t v; std::memcpy(&v, row + x * 4u, 4);
                    r = DecodeUnsignedFloat(v & 0x7ffu, 6);
                    g = DecodeUnsignedFloat((v >> 11) & 0x7ffu, 6);
                    b = DecodeUnsignedFloat((v >> 22) & 0x3ffu, 5);
                    break;
                }
                case VK_FORMAT_R16G16B16A16_SFLOAT: {
                    uint16_t q[3]; std::memcpy(q, row + x * 8u, sizeof q);
                    r = HalfToFloat(q[0]); g = HalfToFloat(q[1]); b = HalfToFloat(q[2]);
                    break;
                }
                case VK_FORMAT_R32G32B32A32_SFLOAT: {
                    float q[3]; std::memcpy(q, row + x * 16u, sizeof q);
                    r = q[0]; g = q[1]; b = q[2];
                    break;
                }
                default:
                    image->rgb.clear();
                    return false;
            }
            if (!std::isfinite(r)) r = 0.f;
            if (!std::isfinite(g)) g = 0.f;
            if (!std::isfinite(b)) b = 0.f;
            if (capture.hdr_mode == 1) {
                // Preserve scRGB's negative wide-gamut channels until after the BT.709 -> BT.2020 matrix. Clamp only
                // at the PQ output boundary.
                const float r2020 = 0.627404f * r + 0.329283f * g + 0.043313f * b;
                const float g2020 = 0.069097f * r + 0.919540f * g + 0.011362f * b;
                const float b2020 = 0.016391f * r + 0.088013f * g + 0.895595f * b;
                r = LinearNitsToPq(r2020 * capture.linear_unit_nits);
                g = LinearNitsToPq(g2020 * capture.linear_unit_nits);
                b = LinearNitsToPq(b2020 * capture.linear_unit_nits);
            }
            // Game alpha does not describe screenshot transparency: no alpha.
            uint16_t* dst = image->rgb.data() + (static_cast<size_t>(y) * capture.width + x) * 3u;
            dst[0] = static_cast<uint16_t>(std::lround(std::clamp(r, 0.f, 1.f) * 65535.f));
            dst[1] = static_cast<uint16_t>(std::lround(std::clamp(g, 0.f, 1.f) * 65535.f));
            dst[2] = static_cast<uint16_t>(std::lround(std::clamp(b, 0.f, 1.f) * 65535.f));
            // Nonzero at 8 bits, the precision an SDR file keeps.
            if (dst[0] >= 129 || dst[1] >= 129 || dst[2] >= 129) saw_nonzero = true;
        }
    }
    if (any_nonzero != nullptr) *any_nonzero = saw_nonzero;
    return true;
}

// ---- HDR signalling for sharing (screenshot.hpp): an ICC v4.4 display profile, cicp 9/16/0/1 (BT.2020, ST 2084,
// RGB, full range), the same tuple as the PNG cICP chunk; its matrix/TRC fallback reads PQ against a 203-nit white
// with a soft roll-off above 75 %. The pixels themselves are never tone mapped.
const std::vector<uint8_t>& Bt2100PqIcc() {
    static const std::vector<uint8_t> kProfile = [] {
        const auto s15 = [](std::vector<uint8_t>& out, double value) {
            AppendPngBe32(out, static_cast<uint32_t>(static_cast<int32_t>(std::lround(value * 65536.0))));
        };
        const auto signature = [](const char* name) { return ReadBe32(reinterpret_cast<const uint8_t*>(name)); };
        const auto element = [&](const char* type) {
            std::vector<uint8_t> out;
            AppendPngBe32(out, signature(type));
            AppendPngBe32(out, 0);
            return out;
        };
        const auto text = [&](const char* ascii) {
            std::vector<uint8_t> out = element("mluc");
            const size_t length = std::strlen(ascii);
            AppendPngBe32(out, 1);    // one record
            AppendPngBe32(out, 12);   // record size
            out.insert(out.end(), {'e', 'n', 'U', 'S'});
            AppendPngBe32(out, static_cast<uint32_t>(length * 2));
            AppendPngBe32(out, 28);   // string offset from the element start
            for (size_t i = 0; i < length; ++i) {
                out.push_back(0);
                out.push_back(static_cast<uint8_t>(ascii[i]));
            }
            return out;
        };
        const auto xyz = [&](double x, double y, double z) {
            std::vector<uint8_t> out = element("XYZ ");
            s15(out, x);
            s15(out, y);
            s15(out, z);
            return out;
        };
        // Bradford D65 -> D50, and BT.2020 RGB -> XYZ (D65).
        constexpr double kBradford[3][3] = {{1.0478112, 0.0228866, -0.0501270},
                                            {0.0295424, 0.9904844, -0.0170491},
                                            {-0.0092345, 0.0150436, 0.7521316}};
        constexpr double kBt2020ToXyz[3][3] = {{0.6369580, 0.1446169, 0.1688810},
                                               {0.2627002, 0.6779981, 0.0593017},
                                               {0.0000000, 0.0280727, 1.0609851}};
        double adapted[3][3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) adapted[i][j] += kBradford[i][k] * kBt2020ToXyz[k][j];
        std::vector<uint8_t> chad = element("sf32");
        for (const auto& row : kBradford)
            for (const double value : row) s15(chad, value);
        std::vector<uint8_t> curve = element("curv");
        constexpr uint32_t kCurvePoints = 1024;
        AppendPngBe32(curve, kCurvePoints);
        for (uint32_t i = 0; i < kCurvePoints; ++i) {
            const double relative = PqToNits(static_cast<float>(i) / (kCurvePoints - 1)) / 203.0;
            const double shown = relative <= 0.75 ? relative : 0.75 + 0.25 * (1.0 - std::exp(-(relative - 0.75) / 0.25));
            const auto code = static_cast<uint16_t>(std::lround(std::clamp(shown, 0.0, 1.0) * 65535.0));
            curve.push_back(static_cast<uint8_t>(code >> 8));
            curve.push_back(static_cast<uint8_t>(code & 0xff));
        }
        std::vector<uint8_t> cicp = element("cicp");
        cicp.insert(cicp.end(), {9, 16, 0, 1});
        const std::vector<uint8_t> desc = text("BT.2100 PQ (RenoDX DLSS5 screenshot)");
        const std::vector<uint8_t> cprt = text("No copyright, use freely");
        const std::vector<uint8_t> wtpt = xyz(0.9642, 1.0, 0.8249);
        const std::vector<uint8_t> red = xyz(adapted[0][0], adapted[1][0], adapted[2][0]);
        const std::vector<uint8_t> green = xyz(adapted[0][1], adapted[1][1], adapted[2][1]);
        const std::vector<uint8_t> blue = xyz(adapted[0][2], adapted[1][2], adapted[2][2]);
        const std::pair<const char*, const std::vector<uint8_t>*> tags[] = {
            {"desc", &desc}, {"cprt", &cprt}, {"wtpt", &wtpt}, {"chad", &chad}, {"rXYZ", &red}, {"gXYZ", &green},
            {"bXYZ", &blue}, {"rTRC", &curve}, {"gTRC", &curve}, {"bTRC", &curve}, {"cicp", &cicp}};
        std::vector<uint8_t> profile(128, 0);
        profile[8] = 0x04;   // version 4.4
        profile[9] = 0x40;
        std::memcpy(&profile[12], "mntr", 4);
        std::memcpy(&profile[16], "RGB ", 4);
        std::memcpy(&profile[20], "XYZ ", 4);
        const uint8_t created[12] = {0x07, 0xEA, 0, 9, 0, 26, 0, 0, 0, 0, 0, 0};
        std::memcpy(&profile[24], created, sizeof(created));
        std::memcpy(&profile[36], "acsp", 4);
        const uint8_t d50[12] = {0, 0, 0xF6, 0xD6, 0, 1, 0, 0, 0, 0, 0xD3, 0x2D};
        std::memcpy(&profile[68], d50, sizeof(d50));
        AppendPngBe32(profile, static_cast<uint32_t>(std::size(tags)));
        const size_t data_start = profile.size() + 12 * std::size(tags);
        std::vector<uint8_t> data;
        const std::vector<uint8_t>* previous = nullptr;
        uint32_t offset = 0;
        for (const auto& [name, element_data] : tags) {
            if (element_data != previous) {   // the three TRCs share one element
                while (data.size() % 4 != 0) data.push_back(0);
                offset = static_cast<uint32_t>(data_start + data.size());
                data.insert(data.end(), element_data->begin(), element_data->end());
                previous = element_data;
            }
            AppendPngBe32(profile, signature(name));
            AppendPngBe32(profile, offset);
            AppendPngBe32(profile, static_cast<uint32_t>(element_data->size()));
        }
        while (data.size() % 4 != 0) data.push_back(0);
        profile.insert(profile.end(), data.begin(), data.end());
        const auto size = static_cast<uint32_t>(profile.size());
        for (int i = 0; i < 4; ++i) profile[i] = static_cast<uint8_t>(size >> (24 - 8 * i));
        return profile;
    }();
    return kProfile;
}

// CTA-861.3 MaxCLL and MaxFALL of one PQ image, in nits (PNG cLLi).
struct ContentLight {
    float max_cll = 0.f;
    float max_fall = 0.f;
};

ContentLight MeasureContentLight(const CodedImage& image) {
    static const std::vector<float> kNits = [] {
        std::vector<float> nits(65536);
        for (uint32_t code = 0; code < nits.size(); ++code) nits[code] = PqToNits(code / 65535.f);
        return nits;
    }();
    ContentLight light;
    double sum = 0.0;
    for (size_t i = 0; i + 2 < image.rgb.size(); i += 3) {
        const float nits = kNits[std::max({image.rgb[i], image.rgb[i + 1], image.rgb[i + 2]})];
        light.max_cll = std::max(light.max_cll, nits);
        sum += nits;
    }
    if (!image.rgb.empty()) light.max_fall = static_cast<float>(sum / (image.rgb.size() / 3));
    return light;
}

// A PNG with this capture's colour chunks right after IHDR, and without any colour chunk the encoder wrote. Empty when
// the stream is malformed. SDR: sRGB. HDR: cICP, iCCP with the same signal, cLLi.
std::vector<uint8_t> WithColorChunks(const std::vector<uint8_t>& png, bool hdr, const ContentLight& light) {
    static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (png.size() < 8 || std::memcmp(png.data(), kSignature, 8) != 0) return {};
    std::vector<uint8_t> out(png.begin(), png.begin() + 8);
    out.reserve(png.size() + Bt2100PqIcc().size() + 256);
    bool placed = false;
    size_t at = 8;
    while (at + 12 <= png.size()) {
        const uint32_t length = ReadBe32(png.data() + at);
        if (length > png.size() - at - 12) return {};
        const char* type = reinterpret_cast<const char*>(png.data() + at + 4);
        const size_t end = at + 12 + length;
        bool colour = false;
        for (const char* owned : {"sRGB", "gAMA", "cHRM", "iCCP", "cICP", "cLLi", "mDCv"})
            colour = colour || std::memcmp(type, owned, 4) == 0;
        if (!colour) out.insert(out.end(), png.begin() + at, png.begin() + end);
        at = end;
        if (std::memcmp(type, "IHDR", 4) == 0 && !placed) {
            placed = true;
            if (!hdr) {
                AppendPngChunk(out, "sRGB", {0});
                continue;
            }
            AppendPngChunk(out, "cICP", {9, 16, 0, 1});
            static constexpr char kName[] = "BT.2100 PQ";
            std::vector<uint8_t> iccp(kName, kName + sizeof(kName));   // name + NUL
            iccp.push_back(0);   // deflate
            const std::vector<uint8_t> zlib = ZlibStored(Bt2100PqIcc().data(), Bt2100PqIcc().size());
            iccp.insert(iccp.end(), zlib.begin(), zlib.end());
            AppendPngChunk(out, "iCCP", iccp);
            std::vector<uint8_t> clli;   // units of 0.0001 cd/m2
            AppendPngBe32(clli, static_cast<uint32_t>(std::lround(light.max_cll * 10000.0)));
            AppendPngBe32(clli, static_cast<uint32_t>(std::lround(light.max_fall * 10000.0)));
            AppendPngChunk(out, "cLLi", clli);
        }
        if (std::memcmp(type, "IEND", 4) == 0) return placed ? out : std::vector<uint8_t>{};
    }
    return {};
}

// The profile as an APP2 ICC_PROFILE segment after SOI and APP0 (JFIF). Empty when the stream is not a JPEG.
std::vector<uint8_t> WithIccSegment(const std::vector<uint8_t>& jpeg) {
    const std::vector<uint8_t>& icc = Bt2100PqIcc();
    if (jpeg.size() < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return {};
    size_t at = 2;
    while (at + 4 <= jpeg.size() && jpeg[at] == 0xFF && jpeg[at + 1] == 0xE0)
        at += 2 + ((static_cast<size_t>(jpeg[at + 2]) << 8) | jpeg[at + 3]);
    static constexpr char kMarker[] = "ICC_PROFILE";   // with its NUL
    const size_t length = 2 + sizeof(kMarker) + 2 + icc.size();
    if (at > jpeg.size() || length > 0xFFFF) return {};
    std::vector<uint8_t> out(jpeg.begin(), jpeg.begin() + at);
    out.reserve(jpeg.size() + length + 2);
    out.insert(out.end(), {0xFF, 0xE2, static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length & 0xff)});
    out.insert(out.end(), kMarker, kMarker + sizeof(kMarker));
    out.insert(out.end(), {1, 1});   // chunk 1 of 1
    out.insert(out.end(), icc.begin(), icc.end());
    out.insert(out.end(), jpeg.begin() + at, jpeg.end());
    return out;
}

// Triangular dither in (-1, 1) LSB from a hash of the position.
float DitherOffset(uint32_t x, uint32_t y, uint32_t channel) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ (channel + 1u) * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return static_cast<float>((h & 0xffffu) + (h >> 16)) / 65535.f - 1.f;
}

// 8-bit BGR for WIC (every JPEG, the SDR PNG). `dither` for sources finer than 8 bits; the position hash repeats every
// `period` columns, so both halves of a side-by-side file carry identical noise.
std::vector<uint8_t> ToBgr8(const CodedImage& image, bool dither, uint32_t period) {
    std::vector<uint8_t> bgr(image.rgb.size());
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t at = (static_cast<size_t>(y) * image.width + x) * 3u;
            for (uint32_t channel = 0; channel < 3; ++channel) {
                float value = image.rgb[at + channel] / 257.f;
                if (dither) value += DitherOffset(x % period, y, channel);
                bgr[at + 2u - channel] = static_cast<uint8_t>(std::clamp(std::lround(value), 0l, 255l));
            }
        }
    }
    return bgr;
}

CodedImage SideBySide(const CodedImage& left, const CodedImage& right) {
    CodedImage both;
    both.width = left.width + right.width;
    both.height = std::min(left.height, right.height);
    both.rgb.reserve(static_cast<size_t>(both.width) * both.height * 3u);
    for (uint32_t y = 0; y < both.height; ++y) {
        for (const CodedImage* half : {&left, &right}) {
            const auto row = half->rgb.begin() + static_cast<ptrdiff_t>(y) * half->width * 3;
            both.rgb.insert(both.rgb.end(), row, row + static_cast<ptrdiff_t>(half->width) * 3);
        }
    }
    return both;
}

// COM on the worker and WIC's factory (RenoDX's utils/png.hpp does the same).
struct ScopedCom {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
    ~ScopedCom() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
Microsoft::WRL::ComPtr<IWICImagingFactory> CreateWicFactory() {
    static constexpr CLSID kFactory = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
    Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(kFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return nullptr;
    return factory;
}

// One WIC encode into memory: JPEG from 24bpp BGR at `quality` (0-1, 4:4:4 chroma where offered - *chroma_444 says),
// or PNG from 24bpp BGR or 48bpp RGB with adaptive filtering. False on any failure; the caller falls back.
bool WicEncode(IWICImagingFactory* factory, bool jpeg, uint32_t width, uint32_t height, const void* pixels,
               bool sixteen, float quality, std::vector<uint8_t>* out, bool* chroma_444 = nullptr) {
    using Microsoft::WRL::ComPtr;
    static constexpr GUID kJpegContainer = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
    static constexpr GUID kPngContainer = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
    static constexpr GUID kBgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
    static constexpr GUID kRgb48 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x15}};
    if (factory == nullptr || (jpeg && sixteen)) return false;
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(jpeg ? kJpegContainer : kPngContainer, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)))
        return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(&frame, &options))) return false;
    PROPBAG2 option{};
    VARIANT value{};
    if (jpeg) {
        option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        value.vt = VT_R4;
        value.fltVal = quality;
        if (FAILED(options->Write(1, &option, &value))) return false;
        option.pstrName = const_cast<LPOLESTR>(L"JpegYCrCbSubsampling");
        value.vt = VT_UI1;
        value.bVal = 3;   // WICJpegYCrCbSubsampling444 (Windows 8.1+)
        const bool full_chroma = SUCCEEDED(options->Write(1, &option, &value));
        if (chroma_444 != nullptr) *chroma_444 = full_chroma;
    } else {
        option.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        value.vt = VT_UI1;
        value.bVal = 6;   // WICPngFilterAdaptive; the encoder's default otherwise
        options->Write(1, &option, &value);
    }
    const GUID wanted = sixteen ? kRgb48 : kBgr24;
    WICPixelFormatGUID format = wanted;
    const UINT stride = width * (sixteen ? 6u : 3u);
    if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(width, height)) ||
        FAILED(frame->SetPixelFormat(&format)) || !InlineIsEqualGUID(format, wanted) ||
        FAILED(frame->WritePixels(height, stride, stride * height, static_cast<BYTE*>(const_cast<void*>(pixels)))) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    STATSTG stat{};
    const LARGE_INTEGER start{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || stat.cbSize.QuadPart == 0 || stat.cbSize.QuadPart > 0x7fffffffull ||
        FAILED(stream->Seek(start, STREAM_SEEK_SET, nullptr)))
        return false;
    out->resize(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    return SUCCEEDED(stream->Read(out->data(), static_cast<ULONG>(out->size()), &read)) && read == out->size();
}

// The PNG without WIC: stored deflate blocks, larger but never lost.
std::vector<uint8_t> StoredPng(const CodedImage& image, const std::vector<uint8_t>* bgr8) {
    const bool sixteen = bgr8 == nullptr;
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(image.height) * (1u + image.width * (sixteen ? 6u : 3u)));
    for (uint32_t y = 0; y < image.height; ++y) {
        raw.push_back(0);   // PNG filter: none
        for (uint32_t x = 0; x < image.width; ++x) {
            const size_t at = (static_cast<size_t>(y) * image.width + x) * 3u;
            for (uint32_t channel = 0; channel < 3; ++channel) {
                if (sixteen) {
                    raw.push_back(static_cast<uint8_t>(image.rgb[at + channel] >> 8));
                    raw.push_back(static_cast<uint8_t>(image.rgb[at + channel] & 0xff));
                } else {
                    raw.push_back((*bgr8)[at + 2u - channel]);
                }
            }
        }
    }
    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    AppendPngBe32(ihdr, image.width);
    AppendPngBe32(ihdr, image.height);
    ihdr.insert(ihdr.end(), {static_cast<uint8_t>(sixteen ? 16 : 8), 2, 0, 0, 0});
    AppendPngChunk(png, "IHDR", ihdr);
    AppendPngChunk(png, "IDAT", ZlibStored(raw.data(), raw.size()));
    AppendPngChunk(png, "IEND", {});
    return png;
}

// A finished PNG: RGB16 PQ for HDR, RGB8 for SDR (from `bgr8`), WIC deflate when WIC is available (*compressed),
// stored blocks otherwise, and this capture's colour chunks either way.
std::vector<uint8_t> EncodePng(IWICImagingFactory* factory, const CodedImage& image, bool hdr,
                               const std::vector<uint8_t>& bgr8, const ContentLight& light, bool* compressed) {
    std::vector<uint8_t> png;
    *compressed = WicEncode(factory, false, image.width, image.height,
                            hdr ? static_cast<const void*>(image.rgb.data()) : bgr8.data(), hdr, 0.f, &png);
    if (*compressed) {
        std::vector<uint8_t> tagged = WithColorChunks(png, hdr, light);
        if (!tagged.empty()) return tagged;
        *compressed = false;
    }
    return WithColorChunks(StoredPng(image, hdr ? nullptr : &bgr8), hdr, light);
}

bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return file.good();
}

std::string narrow(const std::filesystem::path& p) {
    const std::wstring w = p.wstring();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

struct Written { std::wstring pre_image, post_image; };

// Every file of one capture, encoded and written; true when the pair was. The pair is all or nothing: a lone half is
// removed. The side-by-side file is extra, and its failure is logged, never a lost pair. JPEGs share one quality - the
// requested one, lowered in steps of 3 (to 85, or the request when lower) until each file fits the size cap; the image
// is never downscaled. Appends what it did to *meta.
bool WriteCaptureFiles(const Pair& work, const OutputOptions& output, const CodedImage& pre, const CodedImage& post,
                       const std::filesystem::path& base, Written* record, std::string* meta) {
    const bool hdr = work.hdr_mode != 0;
    const bool dither = hdr || !EightBitFormat(work.format);
    ScopedCom com;
    const Microsoft::WRL::ComPtr<IWICImagingFactory> factory = com.usable() ? CreateWicFactory() : nullptr;
    // JPEG needs WIC; without it the capture is still kept, as PNG.
    const bool want_jpeg = output.format != 0 && factory != nullptr;
    const bool want_png = output.format != 1 || !want_jpeg;
    if (output.format != 0 && !want_jpeg)
        log("[nr] NR screenshot: JPEG needs the Windows Imaging Component, which did not load; writing PNG instead");

    struct Picture {
        const wchar_t* suffix;
        const CodedImage* image;
        bool pair;
        std::vector<uint8_t> bgr8;
        ContentLight light;
    };
    CodedImage both;
    std::vector<Picture> pictures;
    pictures.push_back({work.model_diagnostic ? L"_MODEL_INPUT" : L"_NR_OFF", &pre, true, {}, {}});
    pictures.push_back({work.model_diagnostic ? L"_MODEL_OUTPUT" : L"", &post, true, {}, {}});
    if (output.side_by_side) {
        both = SideBySide(pre, post);
        pictures.push_back({work.model_diagnostic ? L"_MODEL_COMPARE" : L"_COMPARE", &both, false, {}, {}});
    }
    for (Picture& picture : pictures) {
        if (want_jpeg || !hdr) picture.bgr8 = ToBgr8(*picture.image, dither, pre.width);
        if (hdr) picture.light = MeasureContentLight(*picture.image);
    }

    struct File {
        std::filesystem::path path;
        std::vector<uint8_t> bytes;
        bool pair;
    };
    std::vector<File> files;
    bool compressed = true;
    if (want_png) {
        for (const Picture& picture : pictures) {
            bool wic = false;
            files.push_back({base.wstring() + picture.suffix + L".png",
                             EncodePng(factory.Get(), *picture.image, hdr, picture.bgr8, picture.light, &wic),
                             picture.pair});
            compressed = compressed && wic;
        }
    }
    const uint32_t requested = std::clamp(output.jpeg_quality, 80u, 100u);
    const uint32_t lowest = std::min(85u, requested);
    const uint64_t budget = static_cast<uint64_t>(output.max_megabytes) * 1000000ull;
    uint32_t pair_quality = 0;
    uint32_t compare_quality = 0;
    bool chroma_444 = false;
    bool over_budget = false;
    if (want_jpeg) {
        for (const bool pair : {true, false}) {
            std::vector<File> encoded;
            for (uint32_t quality = requested;; quality = std::max(lowest, quality - 3u)) {
                encoded.clear();
                bool fits = true;
                for (const Picture& picture : pictures) {
                    if (picture.pair != pair) continue;
                    std::vector<uint8_t> jpeg;
                    if (WicEncode(factory.Get(), true, picture.image->width, picture.image->height, picture.bgr8.data(),
                                  false, quality / 100.f, &jpeg, &chroma_444) &&
                        hdr)
                        jpeg = WithIccSegment(jpeg);
                    if (jpeg.empty()) {
                        log("[nr] NR screenshot JPEG encode failed");
                        encoded.clear();
                        break;
                    }
                    fits = fits && (budget == 0 || jpeg.size() <= budget);
                    encoded.push_back({base.wstring() + picture.suffix + L".jpg", std::move(jpeg), pair});
                }
                if (encoded.empty() || fits || quality == lowest) {
                    uint32_t& used = pair ? pair_quality : compare_quality;
                    used = encoded.empty() ? 0 : quality;
                    over_budget = over_budget || (!encoded.empty() && !fits);
                    break;
                }
            }
            if (pair && encoded.empty() && !want_png) return false;   // nothing to write
            for (File& file : encoded) files.push_back(std::move(file));
        }
    }

    bool pair_ok = true;
    bool extra_ok = true;
    std::string names;
    std::string sizes;
    for (const File& file : files) {
        bool& ok = file.pair ? pair_ok : extra_ok;
        ok = WriteBytes(file.path, file.bytes) && ok;
        const std::string name = narrow(file.path.filename());
        names += (names.empty() ? "" : ",") + name;
        sizes += (sizes.empty() ? "" : ",") + name + ":" + std::to_string(file.bytes.size());
    }
    if (!pair_ok) {   // never leave a lone A image without its B counterpart
        for (const File& file : files) {
            std::error_code ignored;
            if (file.pair) std::filesystem::remove(file.path, ignored);
        }
        return false;
    }
    if (!extra_ok) log("[nr] NR screenshot side-by-side file could not be written");
    const std::wstring primary = want_png ? L".png" : L".jpg";
    record->pre_image = base.wstring() + pictures[0].suffix + primary;
    record->post_image = base.wstring() + pictures[1].suffix + primary;
    if (over_budget)
        log("[nr] NR screenshot JPEG is larger than NRScreenshotMaxMB=%u even at quality %u; written anyway (the image "
            "is never downscaled)", output.max_megabytes, lowest);
    std::string jpeg_note;
    if (want_jpeg)
        jpeg_note = ", JPEG quality " + std::to_string(pair_quality) + (chroma_444 ? " 4:4:4" : " (encoder-default chroma)");
    log("[nr] NR screenshot files: %s%s%s", sizes.c_str(), jpeg_note.c_str(),
        hdr ? ", HDR PQ BT.2020 signalled by cICP and ICC cicp 9/16/0/1" : ", SDR sRGB");

    std::string lines = std::string("file_format=") + (want_png && want_jpeg ? "png+jpeg" : want_png ? "png" : "jpeg") +
                        "\nlayout=" + (output.side_by_side ? "pair+side-by-side" : "pair") + "\nfiles=" + names +
                        "\nbytes=" + sizes + "\npng_bits=" + (hdr ? "16" : "8") +
                        "\npng_encoder=" + (!want_png ? "none" : compressed ? "wic-deflate" : "stored") +
                        "\ndither_8bit=" + (dither ? "1" : "0");
    if (want_jpeg) {
        lines += "\njpeg_quality=" + std::to_string(pair_quality) + "\njpeg_requested_quality=" + std::to_string(requested) +
                 "\njpeg_budget_bytes=" + std::to_string(budget) + "\njpeg_over_budget=" + (over_budget ? "1" : "0") +
                 "\njpeg_chroma=" + (chroma_444 ? "444" : "encoder-default") +
                 "\njpeg_icc=" + (hdr ? "bt2100-pq-cicp-9-16-0-1" : "none-srgb");
        if (output.side_by_side) lines += "\njpeg_compare_quality=" + std::to_string(compare_quality);
    }
    if (hdr) {
        char light[160];
        std::snprintf(light, sizeof(light), "\nmax_cll_nits=%.1f,%.1f\nmax_fall_nits=%.1f,%.1f", pictures[0].light.max_cll,
                      pictures[1].light.max_cll, pictures[0].light.max_fall, pictures[1].light.max_fall);
        lines += light;
    }
    *meta += lines + "\n";
    return true;
}

// ---- the worker: owns everything after the hand-off; never throws across its boundary ----
struct Job { Pair pair; OutputOptions output; uint64_t serial; uint64_t timestamp_ms; };
std::mutex g_job_mutex;
std::condition_variable g_job_signal;
std::optional<Job> g_job;
std::atomic_bool g_worker_started{false};

void ProcessJob(Job work) noexcept {
    const Pair& pair = work.pair;
    const char* outcome = "exception";
    try {
        std::filesystem::path directory;
        {
            std::lock_guard<std::mutex> lock(g_folder_mutex);
            directory = std::filesystem::path(g_folder);
        }
        if (directory.empty()) {   // <game>\NR Screenshots: beside this module (the game's folder on every route)
            HMODULE self = nullptr;
            wchar_t name[MAX_PATH * 4] = {};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(&ProcessJob), &self) &&
                GetModuleFileNameW(self, name, DWORD(std::size(name))))
                directory = std::filesystem::path(name).parent_path() / L"NR Screenshots";
        }
        if (!directory.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
        }
        const std::wstring stem = std::wstring(pair.model_diagnostic ? L"NR_DIAGNOSTIC_" : L"NR_CAPTURE_") +
                                  std::to_wstring(work.timestamp_ms) + L"_" + std::to_wstring(work.serial);
        const std::filesystem::path base = directory.empty() ? std::filesystem::path(stem) : directory / stem;
        CodedImage pre_image, post_image;
        bool pre_nonzero = false, post_nonzero = false, pre_unwritten = false, post_unwritten = false;
        Pair pre_pair = pair;
        if (pair.pre_format) { pre_pair.format = pair.pre_format; pre_pair.row_pitch = pair.pre_row_pitch; }
        const bool read_ok = ReadbackToCoded(pre_pair, pair.pre, &pre_image, &pre_nonzero, &pre_unwritten) &&
                             ReadbackToCoded(pair, pair.post, &post_image, &post_nonzero, &post_unwritten);
        if (pair.release) pair.release(pair.owner);
        if (pre_unwritten || post_unwritten) {
            log("[nr] NR screenshot refused: the GPU had not run its copies when they were read; no files written - "
                "take the screenshot again");
            outcome = "unexecuted";
        } else {
            if (read_ok && (!pre_nonzero || !post_nonzero))
                log("[nr] NR screenshot pair contains an entirely black image%s%s; the GPU ran the copies, so the "
                    "captured frame itself was black", !pre_nonzero ? " [pre-NR]" : "", !post_nonzero ? " [NR on]" : "");
            std::string meta = pair.meta;
            Written record;
            const bool wrote = read_ok && WriteCaptureFiles(pair, work.output, pre_image, post_image, base, &record, &meta);
            outcome = wrote ? "written" : "write_failed";
            if (wrote) {
                log("[nr] NR screenshot pair written: %s and %s", narrow(record.pre_image).c_str(),
                    narrow(record.post_image).c_str());
                if (!meta.empty()) {
                    std::filesystem::path meta_path = base;
                    meta_path += L".meta.txt";
                    std::ofstream meta_out(meta_path, std::ios::trunc);
                    if (meta_out.good()) meta_out << meta;
                }
            } else {
                log("[nr] NR screenshot readback or image write failed; no pair written (%s). Check the folder exists "
                    "and is writable, and that disk space is available", narrow(directory).c_str());
            }
        }
    } catch (const std::exception& ex) {
        log("[nr] NR screenshot exception: %s", ex.what());
    } catch (...) {
        log("[nr] NR screenshot unknown exception");
    }
    publish(outcome, pair.model_diagnostic);
    g_pending.store(false, std::memory_order_release);
}

void WorkerLoop() noexcept {
    // The module stays loaded while this thread runs (it is never joined).
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&WorkerLoop), &pinned);
    for (;;) {
        std::optional<Job> work;
        {
            std::unique_lock<std::mutex> lock(g_job_mutex);
            g_job_signal.wait(lock, [] { return g_job.has_value(); });
            work = std::move(g_job);
            g_job.reset();
        }
        ProcessJob(std::move(*work));
    }
}

}  // namespace

void configure(const Options& o) {
    std::lock_guard<std::mutex> lock(g_options_mutex);
    g_options = o;
}

bool request() {
    if (g_requested.load(std::memory_order_acquire) || g_pending.load(std::memory_order_acquire)) return false;
    Options o;
    {
        std::lock_guard<std::mutex> lock(g_options_mutex);
        o = g_options;
    }
    g_model.store(o.source == 2, std::memory_order_relaxed);
    g_requested_output = {o.format <= 2u ? o.format : 0u, o.layout == 1u, o.jpeg_quality, o.max_megabytes};
    g_frames_armed.store(0, std::memory_order_relaxed);
    g_requested.store(true, std::memory_order_release);
    log(o.source == 2 ? "[nr] NR diagnostic armed: the model's input / output from one pass; excludes look, transfer, "
                        "game tone mapping and UI"
                      : "[nr] NR screenshot armed: one final picture, before and after NR (HDR stays HDR)");
    return true;
}

bool armed() { return g_requested.load(std::memory_order_acquire); }
bool wants_final() { return armed() && !g_model.load(std::memory_order_relaxed); }
bool wants_model() { return armed() && g_model.load(std::memory_order_relaxed); }

void fail(const char* outcome, const char* explanation) {
    if (!g_requested.exchange(false, std::memory_order_acq_rel)) return;
    publish(outcome, g_model.load(std::memory_order_relaxed));
    log("[nr] NR screenshot unavailable: %s", explanation);
}

void abandon(const char* outcome, const char* explanation) {
    g_requested.store(false, std::memory_order_release);
    g_pending.store(false, std::memory_order_release);
    publish(outcome, g_model.load(std::memory_order_relaxed));
    log("[nr] NR screenshot: %s; no files written", explanation);
}

void tick() {
    // NR_SCREENSHOT_AT=<n>: one request at the n-th frame (a test without a keyboard).
    static const long at = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("NR_SCREENSHOT_AT", v, sizeof v) ? std::atol(v) : 0L;
    }();
    static long frames = 0;
    if (at > 0 && ++frames == at) request();
    if (!armed()) {
        g_frames_armed.store(0, std::memory_order_relaxed);
        return;
    }
    if (g_frames_armed.fetch_add(1, std::memory_order_relaxed) + 1 > kArmTimeoutFrames) {
        g_frames_armed.store(0, std::memory_order_relaxed);
        fail("no_nr_frame", "no eligible capture frame arrived within 600 frames");
    }
}

const char* status() {
    if (g_pending.load(std::memory_order_acquire)) return "waiting for GPU completion";
    if (wants_final()) return "armed, waiting for one final picture";
    if (armed()) return "armed, waiting for an evaluated model buffer pair";
    std::lock_guard<std::mutex> lock(g_finished_mutex);
    const std::string& o = g_outcome;
    if (o == "written")
        return g_finished_model ? "saved: model input/output diagnostic; not the displayed game frame"
                                : "saved: same frame before and after NR";
    if (o == "unsupported_present") return "unavailable on this presentation path";
    if (o == "unsupported_diagnostic") return "evaluated-buffer capture is unavailable here";
    if (o == "nr_disabled") return "enable NR before taking a comparison";
    if (o == "none") return "idle";
    return "capture failed; see the log for its cause";
}

void stamp(void* mapped, uint64_t bytes) {
    std::memcpy(mapped, kUnwrittenStamp, static_cast<size_t>(std::min<uint64_t>(sizeof(kUnwrittenStamp), bytes)));
}

void recorded() {
    g_pending.store(true, std::memory_order_release);
    g_requested.store(false, std::memory_order_release);
}

void deliver(Pair pair) {
    g_requested.store(false, std::memory_order_release);
    g_pending.store(true, std::memory_order_release);
    Job job{std::move(pair), g_requested_output, g_serial.fetch_add(1) + 1,
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::system_clock::now().time_since_epoch()).count())};
    {
        std::lock_guard<std::mutex> lock(g_job_mutex);
        if (g_job && g_job->pair.release) g_job->pair.release(g_job->pair.owner);   // superseded before the write
        g_job = std::move(job);
    }
    if (!g_worker_started.exchange(true, std::memory_order_acq_rel)) {
        try {
            std::thread(WorkerLoop).detach();
        } catch (...) {
            // No worker: one synchronous write on this thread rather than a wedged feature.
            g_worker_started.store(false, std::memory_order_release);
            std::optional<Job> fallback;
            {
                std::lock_guard<std::mutex> lock(g_job_mutex);
                fallback = std::move(g_job);
                g_job.reset();
            }
            if (fallback) ProcessJob(std::move(*fallback));
            return;
        }
    }
    g_job_signal.notify_one();
}

void set_folder(const std::wstring& folder) {
    std::lock_guard<std::mutex> lock(g_folder_mutex);
    g_folder = folder;
}

uint32_t bytes_per_pixel(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
            return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
        default: return 0;
    }
}
bool readable(VkFormat format) { return bytes_per_pixel(format) != 0; }

}  // namespace nr::pe::screenshot
