// dlssnr_extract_model: make the model files from NVIDIA's nvngx_dlssnr.dll, written into <output folder>:
//   310.9.16: dlssnr.bin (the old model, the DLL's preset 1) and dlssnr-small.bin (the new model, the DLL's default)
//   310.8.0:  dlssnr.bin only
//
//   dlssnr_extract_model <nvngx_dlssnr.dll> <output folder>
//
// One-program port of the Python model tools (extract_model.sh, inspect_nr.py,
// unpack_swin_family.py --max-c 256, unpack_splitswin.py, unpack_vit.py,
// unpack_preblock.py, unpack_postblock.py, pack_model.py --verify). Only reads the
// weight data in the DLL; never loads or runs it. Every entry is checked against the
// embedded model-files.sha256 / model-files-small.sha256 and a file is written only if all
// of them match the tested model byte for byte (to <file>.tmp, renamed at the end).
//
// The layer tables, entry lists and checksums are generated into dlssnr_extract_tables.hpp and
// dlssnr_extract_small_tables.hpp by gen_extract_tables.py. C++17, standard library only.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dlssnr_extract_tables.hpp"
#include "dlssnr_extract_small_tables.hpp"

namespace fs = std::filesystem;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i64 = std::int64_t;

static const char kSmallDllSha256[] = "2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f";
static const char kDllSha256[] = "e16bcf15e16e13f527491cdf7845b2fe6521a738d8f7c9c721866a8496e1fc8e";

struct FormatError : std::runtime_error {  // inspect_nr.py's FormatError (and its crashes)
    using std::runtime_error::runtime_error;
};
struct Failure : std::runtime_error {  // any other reason to stop
    using std::runtime_error::runtime_error;
};

template <class... A>
static std::string fmt(const char* f, A... a) {
    char buf[512];
    std::snprintf(buf, sizeof buf, f, a...);
    return buf;
}

// ---- SHA-256 ------------------------------------------------------------------------

class Sha256 {
public:
    Sha256() { reset(); }
    void reset() {
        static const u32 init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h_, init, sizeof h_);
        total_ = 0;
        fill_ = 0;
    }
    void update(const u8* p, size_t n) {
        total_ += n;
        if (fill_) {
            size_t k = std::min(n, size_t(64) - fill_);
            std::memcpy(buf_ + fill_, p, k);
            fill_ += k, p += k, n -= k;
            if (fill_ < 64) return;
            block(buf_);
            fill_ = 0;
        }
        for (; n >= 64; p += 64, n -= 64) block(p);
        std::memcpy(buf_, p, n);
        fill_ = n;
    }
    std::string hex() {
        u64 bits = total_ * 8;
        u8 pad[72] = {0x80};
        size_t padn = (fill_ < 56 ? 56 : 120) - fill_;
        for (int i = 0; i < 8; ++i) pad[padn + i] = u8(bits >> (56 - 8 * i));
        update(pad, padn + 8);
        std::string s;
        for (u32 v : h_) s += fmt("%08x", v);
        reset();
        return s;
    }
    static std::string of(const u8* p, size_t n) {
        Sha256 s;
        s.update(p, n);
        return s.hex();
    }

private:
    static u32 ror(u32 x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const u8* p) {
        static const u32 k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        u32 w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = u32(p[4 * i]) << 24 | u32(p[4 * i + 1]) << 16 | u32(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            u32 s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            u32 s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        u32 a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
        for (int i = 0; i < 64; ++i) {
            u32 t1 = h + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            u32 t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h_[0] += a, h_[1] += b, h_[2] += c, h_[3] += d, h_[4] += e, h_[5] += f, h_[6] += g, h_[7] += h;
    }
    u32 h_[8];
    u8 buf_[64];
    u64 total_;
    size_t fill_;
};

// ---- bounds-checked little-endian reads (inspect_nr.py: checked / unpack) -----------

struct Span {
    const u8* p;
    u64 n;
};

static Span checked(Span d, u64 off, u64 size) {
    if (off > d.n || size > d.n - off)
        throw FormatError(fmt("out of bounds: offset=%#llx, size=%llu, buffer=%llu",
                              (unsigned long long)off, (unsigned long long)size,
                              (unsigned long long)d.n));
    return {d.p + off, size};
}
static u64 le(Span d, u64 off, int bytes) {
    Span s = checked(d, off, u64(bytes));
    u64 v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = v << 8 | s.p[i];
    return v;
}
static u16 rd16(Span d, u64 off) { return u16(le(d, off, 2)); }
static u32 rd32(Span d, u64 off) { return u32(le(d, off, 4)); }
static u64 rd64(Span d, u64 off) { return le(d, off, 8); }
static u64 add(u64 a, u64 b) {  // Python ints do not wrap; past 2^64 is out of bounds anyway
    if (a > ~u64(0) - b) throw FormatError("out of bounds: offset overflow");
    return a + b;
}

// ---- PE32+ and the resource tree (inspect_nr.py: PE) --------------------------------

struct Section {
    u32 rva, file_offset, file_size;
};

struct Pe {
    Span data;
    std::vector<Section> sections;
    u32 resource_rva = 0, resource_size = 0;

    const char* resource_name;
    explicit Pe(Span d, const char* resource = "WEIGHTS_HT") : data(d), resource_name(resource) {
        Span mz = checked(d, 0, 2);
        if (mz.p[0] != 'M' || mz.p[1] != 'Z') throw FormatError("not a PE file");
        u32 pe = rd32(d, 0x3c);
        if (std::memcmp(checked(d, pe, 4).p, "PE\0\0", 4) != 0) throw FormatError("missing PE signature");
        rd16(d, u64(pe) + 4);  // machine
        u16 count = rd16(d, u64(pe) + 6);
        u64 opt = u64(pe) + 24;
        u16 opt_size = rd16(d, u64(pe) + 20);
        u16 magic = rd16(d, opt);
        if (magic != 0x20b || opt_size < 144) throw FormatError("expected PE32+ optional header");
        rd64(d, opt + 24);  // image base
        resource_rva = rd32(d, opt + 128);
        resource_size = rd32(d, opt + 132);
        for (u32 i = 0; i < count; ++i) {
            u64 o = opt + opt_size + 40ull * i;
            Span name = checked(d, o, 8);
            size_t len = 8;
            while (len && name.p[len - 1] == 0) --len;  // rstrip(b"\0")
            for (size_t j = 0; j < len; ++j)
                if (name.p[j] >= 0x80) throw FormatError("section name is not ASCII");
            rd32(d, o + 8);  // virtual size
            u32 va = rd32(d, o + 12), size = rd32(d, o + 16), raw = rd32(d, o + 20);
            checked(d, raw, size);
            sections.push_back({va, raw, size});
        }
    }

    u64 offset(u64 rva, u64 size) const {
        for (const Section& s : sections) {
            i64 delta = i64(rva) - i64(s.rva);
            if (delta >= 0 && u64(delta) + size <= s.file_size) return s.file_offset + u64(delta);
        }
        throw FormatError(fmt("RVA not backed by file: %#llx, size=%llu", (unsigned long long)rva,
                              (unsigned long long)size));
    }

    // Every leaf of the resource tree whose path contains a component equal to "WEIGHTS_HT".
    std::vector<Span> weights_ht() const {
        std::vector<Span> out;
        if (!resource_rva) return out;
        u64 root = offset(resource_rva, resource_size);
        Span tree = checked(data, root, resource_size);
        std::set<u64> active;
        walk(tree, 0, 0, false, active, out);
        return out;
    }

private:
    void walk(Span tree, u64 rel, int depth, bool weights, std::set<u64>& active,
              std::vector<Span>& out) const {
        if (active.count(rel) || depth > 8) throw FormatError("cyclic or excessively deep resource tree");
        active.insert(rel);
        u32 n = u32(rd16(tree, rel + 12)) + rd16(tree, rel + 14);
        for (u32 i = 0; i < n; ++i) {
            u32 name = rd32(tree, rel + 16 + 8ull * i), child = rd32(tree, rel + 20 + 8ull * i);
            bool is_weights = false;
            if (name & 0x80000000u) {
                u64 o = name & 0x7fffffffu;
                u16 len = rd16(tree, o);
                Span label = checked(tree, o + 2, u64(len) * 2);
                // Python's strict utf-16-le decode: no unpaired surrogates.
                for (u32 j = 0; j < len; ++j) {
                    u16 c = u16(label.p[2 * j] | label.p[2 * j + 1] << 8);
                    if (c >= 0xd800 && c < 0xdc00) {
                        u16 c2 = j + 1 < len ? u16(label.p[2 * j + 2] | label.p[2 * j + 3] << 8) : 0;
                        if (c2 < 0xdc00 || c2 >= 0xe000) throw FormatError("invalid UTF-16 resource name");
                        ++j;
                    } else if (c >= 0xdc00 && c < 0xe000) {
                        throw FormatError("invalid UTF-16 resource name");
                    }
                }
                const char* want = resource_name;
                is_weights = len == std::strlen(want);
                for (u32 j = 0; is_weights && j < len; ++j)
                    is_weights = label.p[2 * j] == u8(want[j]) && label.p[2 * j + 1] == 0;
            }  // numeric IDs are str(id): never "WEIGHTS_HT"
            if (child & 0x80000000u) {
                walk(tree, child & 0x7fffffffu, depth + 1, weights || is_weights, active, out);
            } else {
                u32 rva = rd32(tree, child), size = rd32(tree, u64(child) + 4);
                rd32(tree, u64(child) + 12);  // codepage, reserved
                u64 off = offset(rva, size);
                if (weights || is_weights) out.push_back({data.p + off, size});
            }
        }
        active.erase(rel);
    }
};

// ---- weight-record envelope (inspect_nr.py: parse_weights) --------------------------

struct Record {
    std::string name;
    Span payload;
};

static bool valid_utf8(const u8* s, u64 n) {  // Python's strict decoder
    for (u64 i = 0; i < n;) {
        u8 c = s[i];
        if (c < 0x80) { ++i; continue; }
        int len = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
        if (!len || i + len > n) return false;
        u8 lo = 0x80, hi = 0xbf;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f;
        if (s[i + 1] < lo || s[i + 1] > hi) return false;
        for (int j = 2; j < len; ++j)
            if (s[i + j] < 0x80 || s[i + j] > 0xbf) return false;
        i += len;
    }
    return true;
}

static std::vector<Record> parse_weights(Span data) {
    u64 total = rd64(data, 0);
    if (total != data.n)
        throw FormatError(fmt("weight map size mismatch: %llu != %llu", (unsigned long long)total,
                              (unsigned long long)data.n));
    u64 p = 8;
    std::vector<Record> records;
    std::set<std::string> names;
    while (p < total) {
        u64 name_size = rd64(data, p);
        p += 8;
        if (!(0 < name_size && name_size <= 4096)) throw FormatError("invalid weight name length");
        Span nb = checked(data, p, name_size);
        if (!valid_utf8(nb.p, nb.n)) throw FormatError("invalid weight name");
        std::string name(reinterpret_cast<const char*>(nb.p), size_t(nb.n));
        if (!names.insert(name).second) throw FormatError("duplicate weight name: " + name);
        p += name_size;
        u64 outer_size = rd64(data, p);
        p += 8;
        Span body = checked(data, p, outer_size);
        u64 inner_size = rd64(body, 0), raw_size = rd64(body, 8);
        u32 device = rd32(body, 16);
        if (inner_size != outer_size) throw FormatError(name + ": inner/outer sizes differ");
        if (!raw_size || device > 1)
            throw FormatError(fmt("%s: empty data or non-host device %u", name.c_str(), device));
        Span payload = checked(body, 20, raw_size);
        rd32(body, add(20, raw_size));      // type id
        rd32(body, add(24, raw_size));      // layout id
        u64 ndim = rd64(body, add(28, raw_size));
        if (ndim > 32) throw FormatError(fmt("%s: implausible dimension count %llu", name.c_str(),
                                             (unsigned long long)ndim));
        checked(body, add(36, raw_size), 4 * ndim);  // storage shape
        if (36 + raw_size + 4 * ndim != inner_size) throw FormatError(name + ": trailing or missing metadata");
        records.push_back({name, payload});
        p += outer_size;
    }
    if (p != total) throw FormatError("weight map did not end exactly at resource boundary");
    return records;
}

// ---- CUDA ELF validation (inspect_nr.py: cuda_elfs) ---------------------------------
// inspect_nr.py inventories the embedded cubins and fails on a malformed section table;
// only the validation is kept here.

static void check_cuda_elfs(Span data) {
    static const u8 magic[4] = {0x7f, 'E', 'L', 'F'};
    for (u64 start = 0; start + 4 <= data.n; ++start) {
        const void* hit = std::memchr(data.p + start, 0x7f, size_t(data.n - start));
        if (!hit) break;
        start = u64(static_cast<const u8*>(hit) - data.p);
        if (start + 4 > data.n) break;
        if (std::memcmp(data.p + start, magic, 4) != 0) continue;
        Span cls = checked(data, start + 4, 2);
        if (cls.p[0] != 2 || cls.p[1] != 1) continue;
        if (rd16(data, start + 18) != 190) continue;
        u64 shoff = rd64(data, start + 40);
        rd32(data, start + 48);  // flags
        u16 entsize = rd16(data, start + 58), count = rd16(data, start + 60), strindex = rd16(data, start + 62);
        if (entsize != 64 || strindex >= count) throw FormatError("invalid CUDA ELF section table");
        struct Sh { u32 name, type; u64 offset, size; };
        std::vector<Sh> table;
        for (u32 i = 0; i < count; ++i) {
            u64 o = add(add(start, shoff), 64ull * i);
            checked(data, o, 64);
            table.push_back({rd32(data, o), rd32(data, o + 4), rd64(data, o + 24), rd64(data, o + 32)});
        }
        Span strings = checked(data, add(start, table[strindex].offset), table[strindex].size);
        u64 extent = shoff + 64ull * count;
        for (const Sh& s : table) {
            if (s.name >= strings.n) throw FormatError("invalid ELF section name offset");
            const void* end = std::memchr(strings.p + s.name, 0, size_t(strings.n - s.name));
            if (!end) throw FormatError("unterminated ELF section name");
            for (const u8* c = strings.p + s.name; c != end; ++c)
                if (*c >= 0x80) throw FormatError("ELF section name is not ASCII");
            if (s.type != 8) {  // SHT_NOBITS has no bytes in the file.
                checked(data, add(start, s.offset), s.size);
                extent = std::max(extent, add(s.offset, s.size));
            }
        }
        checked(data, start, extent);
    }
}

// ---- unpacked entries: what each unpacker would write, produced on demand -----------

enum class Op { Copy, SwinDeswizzle, SliceDeswizzle, VitQkv, PostOutProject };

struct Producer {
    Op op;
    Span p;          // the record payload
    i64 a = 0, b = 0;  // Copy / PostOutProject: p[a:b]; deswizzles: base = a
    int N = 0, K = 0, inner = 0, which = 0;

    u64 size() const {
        switch (op) {
        case Op::Copy:
        case Op::PostOutProject: {
            i64 n = i64(p.n), lo = std::min(a, n), hi = std::min(b, n);
            return hi > lo ? u64(hi - lo) : 0;
        }
        case Op::VitQkv: return 1024ull * 1024;
        default: return u64(N) * u64(K);
        }
    }

    void produce(std::vector<u8>& out) const {
        out.assign(size_t(size()), 0);
        const u8* blob = p.p;
        if (op == Op::Copy) {
            if (!out.empty()) std::memcpy(out.data(), blob + a, out.size());
        } else if (op == Op::SwinDeswizzle) {
            // unpack_swin_family.py: deswizzle(..., row_order="t-major")
            int kg = K / 32;
            u64 st_k = 512ull * inner, st_o = st_k * kg;
            for (int t = 0; t < N / 16; ++t)
                for (int g = 0; g < kg; ++g) {
                    u64 off = u64(a) + u64(t % inner) * 512 + g * st_k + u64(t / inner) * st_o;
                    for (int half = 0; half < 2; ++half)
                        for (int n = 0; n < 8; ++n)
                            for (int k = 0; k < 32; ++k) {
                                int lane = 4 * n + (k % 8) / 2, byte = 2 * (k / 8) + k % 2;
                                int row = t * 16 + half * 8 + n;
                                out[size_t(row) * K + g * 32 + k] = blob[off + lane * 16 + half * 8 + byte];
                            }
                }
        } else if (op == Op::SliceDeswizzle) {
            // unpack_splitswin.py / unpack_vit.py: deswizzle(blob, base, N, K)
            int ngrp = N / 16, kgrp = K / 32;
            for (int s = 0; s < ngrp * kgrp; ++s) {
                int ng = s % ngrp, kgi = s / ngrp;
                u64 off = u64(a) + u64(s) * 512;
                for (int half = 0; half < 2; ++half)
                    for (int lane = 0; lane < 32; ++lane) {
                        int n = ng * 16 + half * 8 + lane / 4;
                        u64 src = off + lane * 16 + half * 8;
                        for (int byte = 0; byte < 8; ++byte) {
                            int k = 8 * (byte >> 1) + 2 * (lane % 4) + (byte & 1);
                            out[size_t(n) * K + kgi * 32 + k] = blob[src + byte];
                        }
                    }
            }
        } else if (op == Op::VitQkv) {
            // unpack_vit.py: deswizzle_qkv(blob)[which*1024*1024 : (which+1)*1024*1024]
            for (int row = 0; row < 1024; ++row)
                for (int k = 0; k < 1024; ++k) {
                    int b2 = (k & 1) | ((k >> 1 & 1) << 4) | ((k >> 2 & 1) << 5) | ((k >> 3 & 1) << 1) |
                             ((k >> 4 & 1) << 2) | ((row & 1) << 6) | ((row >> 1 & 1) << 7) |
                             ((row >> 2 & 1) << 8) | ((row >> 3 & 1) << 3) | ((row >> 4 & 1) << 9);
                    int tile = (row >> 5) | ((k >> 5) << 5);
                    out[size_t(row) * 1024 + k] = blob[128 + u64(3 * tile + which) * 1024 + b2];
                }
        } else {
            // unpack_postblock.py: out_project, FP16[16][32] from two m16n8k16 B blocks.
            const u8* raw = blob + a;
            for (int n = 0; n < 16; ++n)
                for (int k = 0; k < 32; ++k) {
                    int i = (k / 16) * 256 + 32 * (n % 8) + 8 * ((k % 8) / 2) + 4 * (n / 8) + k % 2 +
                            2 * ((k % 16) / 8);
                    out[2 * (n * 32 + k)] = raw[2 * i];
                    out[2 * (n * 32 + k) + 1] = raw[2 * i + 1];
                }
        }
    }
};

using Plan = std::map<std::string, Producer>;

static Producer copy_of(Span p, i64 a, i64 b) {
    if (a < 0 || b < 0) throw Failure("negative region bound");
    Producer r{Op::Copy, p};
    r.a = a, r.b = b;
    return r;
}

// Python raises IndexError on any read past the payload; check before producing.
static Producer swin_deswizzle(Span p, i64 base, int N, int K, int inner, const std::string& what) {
    Producer r{Op::SwinDeswizzle, p};
    r.a = base, r.N = N, r.K = K, r.inner = inner;
    int kg = K / 32;
    if (N / 16 > 0 && kg > 0) {
        if (inner <= 0) throw Failure(what + ": zero inner tile count");
        u64 st_k = 512ull * inner, st_o = st_k * kg, last = 0;
        for (int t = 0; t < N / 16; ++t)
            last = std::max(last, u64(base) + u64(t % inner) * 512 + (kg - 1) * st_k + u64(t / inner) * st_o);
        if (last + 512 > p.n) throw Failure(what + ": matrix reads past the record");
    }
    return r;
}

static Producer slice_deswizzle(Span p, i64 base, int N, int K, const std::string& what) {
    Producer r{Op::SliceDeswizzle, p};
    r.a = base, r.N = N, r.K = K;
    u64 slices = u64(N / 16) * u64(K / 32);
    if (slices && u64(base) + slices * 512 > p.n) throw Failure(what + ": matrix reads past the record");
    return r;
}

static bool parse_block_layer(const std::string& s, int& block, int& layer) {
    // re.fullmatch(r"block(\d+)\.layer(\d+)\.layer", s); int() of each group
    size_t i = 0;
    auto lit = [&](const char* w) {
        size_t n = std::strlen(w);
        if (s.compare(i, n, w) != 0) return false;
        i += n;
        return true;
    };
    auto num = [&](int& v) {
        size_t j = i;
        u64 x = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            x = std::min<u64>(x * 10 + u64(s[i] - '0'), u64(1) << 40);
            ++i;
        }
        v = x >= (u64(1) << 31) ? -1 : int(x);  // -1: no such descriptor key
        return i > j;
    };
    return lit("block") && num(block) && lit(".layer") && num(layer) && lit(".layer") && i == s.size();
}

struct ModelTables {
    const tables::Layer* layers;
    unsigned layer_count;
    const char* const* types;
    const tables::Entry* entries;
    unsigned entry_count;
    const char* const* verify_extra;
};
static ModelTables model_tables(bool small) {
    if (small) return {small_tables::kLayers, small_tables::kLayerCount, small_tables::kTypes,
        small_tables::kEntries, small_tables::kEntryCount, small_tables::kVerifyExtra};
    return {tables::kLayers, tables::kLayerCount, tables::kTypes,
        tables::kEntries, tables::kEntryCount, tables::kVerifyExtra};
}
struct Layers {
    std::map<std::pair<int, int>, const tables::Layer*> by_key;
    const char* const* types;
    explicit Layers(const ModelTables& t) : types(t.types) {
        for (unsigned i = 0; i < t.layer_count; ++i)
            by_key[{t.layers[i].block, t.layers[i].layer}] = &t.layers[i];
    }
    const tables::Layer& at(int b, int l) const {  // the unpackers' layers[(b, l)]: KeyError if absent
        auto it = by_key.find({b, l});
        if (b < 0 || l < 0 || it == by_key.end())
            throw Failure(fmt("descriptor has no layer block%d.layer%d", b, l));
        return *it->second;
    }
};

// unpack_swin_family.py --max-c 256
static void plan_swin_family(const std::vector<Record>& records, const Layers& layers, Plan& plan) {
    for (const Record& r : records) {
        int bi, li;
        if (!parse_block_layer(r.name, bi, li)) continue;
        const tables::Layer& l = layers.at(bi, li);
        int ci = l.in, co = l.out, C = std::min(ci, co);
        std::string type = layers.types[l.type];
        static const std::string pre = "CCTinlayoutFusedSwin";
        bool family = type.size() == pre.size() + 2 && type.compare(0, pre.size(), pre) == 0 &&
                      type[pre.size()] >= '0' && type[pre.size()] <= '9' && type[pre.size() + 1] == 'H';
        if (!family || C > 256 || (ci != co && std::max(ci, co) != 2 * C)) continue;
        int heads = C / 32;
        if (heads == 0) throw Failure(r.name + ": region law divides by zero heads");

        struct Reg { const char* name; i64 a, b; int N, K, inner; bool matrix; };
        std::vector<Reg> regs;
        i64 off = 0;
        auto addr = [&](const char* name, i64 size, int N = 0, int K = 0, int inner = 0, bool matrix = false) {
            regs.push_back({name, off, off + size, N, K, inner, matrix});
            off += size;
        };
        int H = 4 * C;
        if (heads == 1) {
            addr("mlp_expand", i64(C) * H, H, C, H / 16, true);
            addr("mlp_contract", i64(C) * H, C, H, C / 16, true);
        } else {
            addr("mlp_expand", i64(C) * H, H, C, (H / heads) / 16, true);
            addr("mlp_mid", i64(C) * H / heads, C, H / heads, (C / heads) / 16, true);
            addr("mlp_contract", i64(C) * C, C, C, C / 16, true);
        }
        i64 resample = (ci && ci != co) ? i64(ci) * co : 0;
        bool up = resample && co < ci;
        if (up) addr("resample", resample, co, ci, co / 16, true);
        addr("residual_scale", 2 * C + 32);
        if (up) addr("upsample_gain", C == 32 ? 2 * C : 2 * C - 32);
        addr("qkv", 3 * i64(C) * C, 3 * C, C, (3 * C) / 16, true);
        addr("attn_pos_bias", i64(heads) * 8192);
        addr("scalars_b", std::max(16, 4 * heads));
        addr("attn_out_proj", i64(C) * C, C, C, C / 16, true);
        if (resample && !up) {
            addr("attn_residual_scale", 2 * C);
            addr("resample", resample, co, ci, co / 16, true);
            if (C == 32) addr("tail", 16);
        } else {
            addr("attn_residual_scale", 2 * C + 16);
        }
        if (u64(regs.back().b) != r.payload.n) continue;  // "SKIP: region law gives ..."
        for (const Reg& g : regs) {
            std::string fn = "unpacked/" + r.name + "." + g.name + ".bin";
            plan[fn] = g.matrix ? swin_deswizzle(r.payload, g.a, g.N, g.K, g.inner, fn)
                                : copy_of(r.payload, g.a, g.b);
        }
    }
}

// unpack_splitswin.py
static void plan_splitswin(const std::vector<Record>& records, const Layers& layers, Plan& plan) {
    enum Kind { M, F16, RAW };
    struct Part { const char* name; Kind kind; i64 off; int a, b; };
    static const std::map<std::string, std::vector<Part>> layout = {
        {"CCSplitSwin16HFfwd", {{"ffwd_a", M, 0, 512, 512}, {"ffwd_b", M, 262144, 512, 512}}},
        {"CCSplitSwin16HFfwdProj", {{"weight", M, 0, 512, 512}, {"skip_weight", F16, 262144, 512, 0}}},
        {"CCSplitSwin16HProj", {{"weight", M, 0, 512, 512}, {"skip_weight", F16, 262144, 512, 0}}},
        {"CCSplitSwin16HProjPool", {{"weight", M, 0, 512, 512}, {"skip_weight", F16, 262144, 512, 0}}},
        {"CCSplitSwin16HQKVAttn",
         {{"qkv", M, 0, 1536, 512}, {"attn_pos_bias", F16, 786432, 65536, 0}, {"tail", RAW, 917504, 64, 0}}},
        {"CCSplitSwin16HFinalHead", {{"weight", M, 0, 1024, 512}, {"tail", RAW, 524288, 16, 0}}},
        {"CCDecInputUpsample", {{"weight", M, 0, 512, 1024}, {"skip_weight", F16, 524288, 512, 0}}},
    };
    for (const Record& r : records) {
        int bi, li;
        if (!parse_block_layer(r.name, bi, li)) continue;
        auto it = layout.find(layers.types[layers.at(bi, li).type]);
        if (it == layout.end()) continue;
        for (const Part& t : it->second) {
            std::string fn = "unpacked-splitswin/" + r.name + "." + t.name + ".bin";
            if (t.kind == M) {
                plan[fn] = slice_deswizzle(r.payload, t.off, t.a, t.b, fn);
            } else if (t.kind == F16) {
                // struct.unpack("<%de" % a, raw) needs all 2a bytes
                if (u64(t.off) + 2ull * t.a > r.payload.n) throw Failure(fn + ": FP16 region past the record");
                plan[fn] = copy_of(r.payload, t.off, t.off + 2 * i64(t.a));
            } else {
                plan[fn] = copy_of(r.payload, t.off, t.off + t.a);
            }
        }
    }
}

// unpack_vit.py
static void plan_vit(const std::vector<Record>& records, const Layers& layers, Plan& plan) {
    struct L { int N, K, head, tail, split; };
    static const std::map<std::string, L> layout = {
        {"CCVit1DQKV", {3072, 1024, 128, 0, 3}},
        {"CCVit1DFfnExpand", {4096, 1024, 0, 0, 1}},
        {"CCVit1DFfnContract", {1024, 4096, 0, 1024, 1}},
        {"CCVit1DProjection", {1024, 1024, 0, 1024, 1}},
    };
    for (const Record& r : records) {
        int bi, li;
        if (!parse_block_layer(r.name, bi, li)) continue;
        auto it = layout.find(layers.types[layers.at(bi, li).type]);
        if (it == layout.end()) continue;
        const L& v = it->second;
        std::string stem = "unpacked-vit/" + r.name + ".";
        if (v.head) plan[stem + "header.bin"] = copy_of(r.payload, 0, v.head);
        if (v.split == 3) {
            if (128 + 3ull * 1024 * 1024 > r.payload.n) throw Failure(stem + "qkv: matrix reads past the record");
            static const char* const qkv[3] = {"q", "k", "v"};
            for (int i = 0; i < 3; ++i) {
                Producer p{Op::VitQkv, r.payload};
                p.which = i;
                plan[stem + qkv[i] + ".bin"] = p;
            }
        } else {
            plan[stem + "weight.bin"] = slice_deswizzle(r.payload, v.head, v.N, v.K, stem + "weight");
        }
        if (v.tail) {
            i64 a = v.head + i64(v.N) * v.K, b = a + 2 * i64(v.tail);
            if (u64(b) > r.payload.n) throw Failure(stem + "skip_weight: FP16 region past the record");
            plan[stem + "skip_weight.bin"] = copy_of(r.payload, a, b);
        }
    }
}

// unpack_preblock.py / unpack_postblock.py: fixed region maps on one record each.
struct FixedReg { const char* name; i64 a, b; int N, K, inner; char kind; };  // 'm' e4m3 matrix, 'o' out_project

static void plan_fixed(const std::vector<Record>& records, const char* record, const char* dir,
                       const std::vector<FixedReg>& regs, Plan& plan) {
    const Record* r = nullptr;
    for (const Record& x : records)
        if (x.name == record) r = &x;
    if (!r) throw Failure(std::string("no weight record ") + record);
    if (u64(regs.back().b) != r->payload.n)
        throw Failure(fmt("%s: region map gives %lld, record is %llu", record, (long long)regs.back().b,
                          (unsigned long long)r->payload.n));
    int bad = 0;
    for (const FixedReg& g : regs) {
        std::string fn = std::string(dir) + "/" + record + "." + g.name + ".bin";
        if (g.kind == 'm') {
            Producer p = swin_deswizzle(r->payload, g.a, g.N, g.K, g.inner, fn);
            std::vector<u8> m;
            p.produce(m);
            for (u8 x : m)
                if (x == 0x7f || x == 0xff) { ++bad; break; }  // e4m3 NaN: the map is wrong
            plan[fn] = p;
        } else {
            Producer p = copy_of(r->payload, g.a, g.b);
            if (g.kind == 'o') p.op = Op::PostOutProject;
            plan[fn] = p;
        }
    }
    if (bad) throw Failure(fmt("%s: %d e4m3 matrix regions contain NaN: the region map is wrong", record, bad));
}

static void plan_preblock(const std::vector<Record>& records, Plan& plan) {
    const int C = 32, H = 4 * C;
    plan_fixed(records, "block0.layer0.layer", "unpacked-preblock",
               {{"mlp_expand", 0, 4096, H, C, H / 16, 'm'},
                {"mlp_contract", 4096, 8192, C, H, C / 16, 'm'},
                {"input_lift", 8208, 9232, 32, 16, 0, 'r'},
                {"residual_scale", 9232, 9312, 0, 0, 0, 'r'},
                {"qkv", 9312, 12384, 3 * C, C, 3 * C / 16, 'm'},
                {"attn_pos_bias", 12384, 20576, 0, 0, 0, 'r'},
                {"scalars_b", 20576, 20592, 0, 0, 0, 'r'},
                {"attn_out_proj", 20592, 21616, C, C, C / 16, 'm'},
                {"attn_residual_scale", 21616, 21696, 0, 0, 0, 'r'}},
               plan);
}

static void plan_postblock(const std::vector<Record>& records, Plan& plan, bool small) {
    const int C = 32, H = 4 * C;
    plan_fixed(records, small ? "block30.layer0.layer" : "block70.layer0.layer", "unpacked-postblock",
               {{"mlp_expand", 0, 4096, H, C, H / 16, 'm'},
                {"mlp_contract", 4096, 8192, C, H, C / 16, 'm'},
                {"residual_scale", 8208, 8272, 0, 0, 0, 'r'},
                {"main_gain", 8272, 8336, 0, 0, 0, 'r'},
                {"skip_gain", 8336, 8400, 0, 0, 0, 'r'},
                {"qkv", 8400, 11472, 3 * C, C, 3 * C / 16, 'm'},
                {"attn_pos_bias", 11472, 19664, 0, 0, 0, 'r'},
                {"scalars_b", 19664, 19680, 0, 0, 0, 'r'},
                {"attn_out_proj", 19680, 20704, C, C, C / 16, 'm'},
                {"attn_residual_scale", 20704, 20784, 0, 0, 0, 'r'},
                {"out_project", 20784, 21808, 16, 32, 0, 'o'}},
               plan);
}

// ---- main ---------------------------------------------------------------------------

static std::vector<u8> read_file(const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) throw Failure("no such file: " + path.u8string());
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw Failure("cannot open " + path.u8string());
    std::streamoff n = f.tellg();
    if (n < 0) throw Failure("cannot read " + path.u8string());
    // Not in inspect_nr.py for plain files (it caps ZIP members at 1 GiB); keeps a wrong
    // input from exhausting memory. nvngx_dlssnr.dll 310.8.0 is ~110 MB.
    if (n > (std::streamoff(1) << 30)) throw Failure(path.u8string() + " is over 1 GiB: not nvngx_dlssnr.dll");
    std::vector<u8> data(static_cast<size_t>(n));
    f.seekg(0);
    if (n && !f.read(reinterpret_cast<char*>(data.data()), n)) throw Failure("cannot read " + path.u8string());
    return data;
}

// One model's pack from the DLL's bytes: the 310.8.0 model (resource WEIGHTS_HT, in both DLLs) or the new model of
// 310.9.16 (WEIGHTS_CC_SMALL_FP8).
static void extract(const std::vector<u8>& dll, const fs::path& src, bool small, const fs::path& out) {
    const Span data{dll.data(), dll.size()};
    const auto selected = model_tables(small);

    // inspect_nr.py --extract
    std::vector<Record> records;
    try {
        Pe pe(data, small ? "WEIGHTS_CC_SMALL_FP8" : "WEIGHTS_HT");
        std::vector<Span> matches = pe.weights_ht();
        if (matches.size() != 1) throw FormatError(std::string("expected exactly one ") + pe.resource_name + " resource");
        records = parse_weights(matches[0]);
        check_cuda_elfs(data);
        for (const Record& r : records) {  // never let serialized names become paths
            bool safe = !r.name.empty() && r.name != "." && r.name != "..";
            for (char c : r.name)
                safe = safe && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                                c == '_' || c == '.' || c == '-');
            if (!safe) throw FormatError("unsafe extraction name");
        }
    } catch (const FormatError& e) {
        std::string hint = dll.size() >= 4 && std::memcmp(dll.data(), "PK\3\4", 4) == 0
                               ? " This is a zip: extract nvngx_dlssnr.dll from it first."
                               : "";
        throw Failure("Cannot read nvngx_dlssnr weights from " + src.u8string() +
                      " (needs nvngx_dlssnr.dll 310.9.16 or 310.8.0): " + e.what() + "." + hint);
    }

    // inventory/weights/ and the five unpackers, in extract_model.sh's order.
    Plan plan;
    for (const Record& r : records) plan["inventory/weights/" + r.name + ".bin"] = copy_of(r.payload, 0, i64(r.payload.n));
    Layers layers(selected);
    plan_swin_family(records, layers, plan);
    plan_splitswin(records, layers, plan);
    plan_vit(records, layers, plan);
    plan_preblock(records, plan);
    plan_postblock(records, plan, small);

    // pack_model.py --verify
    const unsigned count = selected.entry_count;
    std::vector<const Producer*> entries;
    for (unsigned i = 0; i < count; ++i) {
        auto it = plan.find(selected.entries[i].name);
        if (it == plan.end()) throw Failure(std::string("missing ") + selected.entries[i].name);
        entries.push_back(&it->second);
    }
    std::string index = "NRMODEL1";
    auto put = [&](u64 v, int bytes) {
        for (int i = 0; i < bytes; ++i) index += char(u8(v >> (8 * i)));
    };
    put(count, 4);
    put(0, 4);
    u64 header = index.size();
    for (unsigned i = 0; i < count; ++i) header += 4 + std::strlen(selected.entries[i].name) + 16;
    std::vector<u64> offsets;
    u64 off = (header + 255) / 256 * 256;
    for (unsigned i = 0; i < count; ++i) {
        const char* name = selected.entries[i].name;
        u64 size = entries[i]->size();
        offsets.push_back(off);
        put(std::strlen(name), 4);
        index += name;
        put(off, 8);
        put(size, 8);
        off = (off + size + 255) / 256 * 256;
    }

    if (out.has_parent_path()) fs::create_directories(out.parent_path());
    fs::path tmp = out;
    tmp += ".tmp";
    unsigned bad = 0;
    std::string first_bad;
    u64 file_size = 0;
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw Failure("cannot write " + tmp.u8string());
    try {
        f.write(index.data(), std::streamsize(index.size()));
        u64 pos = index.size();
        std::vector<u8> blob;
        static const u8 zeros[256] = {};
        for (unsigned i = 0; i < count; ++i) {
            entries[i]->produce(blob);
            if (Sha256::of(blob.data(), blob.size()) != selected.entries[i].sha256) {
                if (!bad++) first_bad = selected.entries[i].name;
            }
            if (bad || blob.empty()) continue;  // f.seek(o); f.write(b"") does not extend the file
            for (; pos < offsets[i]; pos += std::min<u64>(256, offsets[i] - pos))
                f.write(reinterpret_cast<const char*>(zeros), std::streamsize(std::min<u64>(256, offsets[i] - pos)));
            f.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
            pos += blob.size();
        }
        for (const char* const* x = selected.verify_extra; *x; ++x)
            if (!bad++) first_bad = *x;
        if (bad)
            throw Failure(fmt("%u weight files differ from the tested model, first: %s", bad, first_bad.c_str()));
        f.close();
        if (!f) throw Failure("cannot write " + tmp.u8string());
        file_size = pos;
    } catch (...) {  // write nothing: the partial file goes
        f.close();
        std::error_code ec;
        fs::remove(tmp, ec);
        throw;
    }
    std::error_code ec;
    fs::rename(tmp, out, ec);
    if (ec) {
        fs::remove(tmp, ec);
        throw Failure("cannot rename " + tmp.u8string() + " to " + out.u8string());
    }
    std::printf("%s: %u entries, %.1f MiB\n", out.u8string().c_str(), count, double(file_size) / 1048576.0);
}

// 310.9.16: dlssnr.bin (the old model, the DLL's preset 1) and dlssnr-small.bin (the new model, its default);
// 310.8.0: dlssnr.bin only. As extract_model.sh.
static int run(const fs::path& src, const fs::path& out) {
    std::error_code ec;
    if (!fs::is_directory(out, ec)) throw Failure("no such output folder: " + out.u8string());
    const std::vector<u8> dll = read_file(src);
    const std::string got = Sha256::of(dll.data(), dll.size());
    if (got != kDllSha256 && got != kSmallDllSha256) {
        const bool zip = dll.size() >= 4 && std::memcmp(dll.data(), "PK\3\4", 4) == 0;
        throw Failure(zip ? "This is a zip: extract nvngx_dlssnr.dll from it first."
                          : "This is not nvngx_dlssnr 310.9.16 or 310.8.0 (SHA256 " + got +
                                "). One of these two DLL versions is required.");
    }
    extract(dll, src, false, out / "dlssnr.bin");
    if (got == kSmallDllSha256) extract(dll, src, true, out / "dlssnr-small.bin");
    return 0;
}

static int main_paths(int argc, const fs::path* args) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: dlssnr_extract_model <nvngx_dlssnr.dll> <output folder>\n");
        return 2;
    }
    try {
        return run(args[1], args[2]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

#ifdef _WIN32
// Wide arguments so non-ASCII paths work (built with -municode).
int wmain(int argc, wchar_t** argv) {
    std::vector<fs::path> a(argv, argv + argc);
    return main_paths(argc, a.data());
}
#else
int main(int argc, char** argv) {
    std::vector<fs::path> a(argv, argv + argc);
    return main_paths(argc, a.data());
}
#endif
