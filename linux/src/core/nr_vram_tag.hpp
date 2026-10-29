#pragma once
// What the video memory allocated on this thread is for, for NR Control's VRAM breakdown (the Windows ledger,
// nr_vram_ledger.hpp, reads it at every allocation of ours). Set by scopes around a network's build (its owner) and
// around its weights and activation arena (their kind); everything else of a network is its images and buffers.
#include <cstdint>

namespace nr::vram {
enum Kind : uint8_t {
    kUnset,         // in a network's scope: its images and buffers; outside one: the NR route's own
    kWeights,       // a network's weights
    kActivations,   // a network's activation arena
    kCopies,        // the .asi's copies of the frame and the guides
    kWilsjo2,       // wilsjo2 layers' textures
    kMenu,          // NR Control's own
    kRenoDX,        // the RenoDX processing's textures (per layer and placement)
    kKinds
};
struct Tag {
    Kind kind = kUnset;
    int owner = -1;   // the network (nr::NetworkChoice), -1 none
};
inline thread_local Tag t_tag;
struct Scope {
    Tag previous;
    explicit Scope(Kind kind) : previous(t_tag) { t_tag.kind = kind; }
    Scope(Kind kind, int owner) : previous(t_tag) { t_tag.kind = kind; t_tag.owner = owner; }
    ~Scope() { t_tag = previous; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
}  // namespace nr::vram
