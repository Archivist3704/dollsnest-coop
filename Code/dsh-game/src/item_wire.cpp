#include "dsh/game/item_wire.h"

#include <array>
#include <atomic>
#include <utility>

namespace dsh::game {
namespace {

std::array<ItemUseSample, kItemUseCapacity> g_ring{};
std::atomic<std::uint32_t> g_write{0};
std::atomic<std::uint32_t> g_read{0};

std::atomic<std::uint64_t> g_noted{0};
std::atomic<std::uint64_t> g_dropped{0};
std::atomic<std::uint64_t> g_drained{0};
std::atomic<std::uint64_t> g_sent{0};
std::atomic<std::uint64_t> g_refused{0};
std::atomic<std::uint64_t> g_foreign{0};

bool printable_ascii(const std::string& text) {
    for (const char ch : text) {
        const auto u = static_cast<unsigned char>(ch);
        if (u < 0x20 || u > 0x7E) return false;
    }
    return true;
}

}

void note_item_use(std::uintptr_t itemClass, std::uint64_t ms) {

    if (itemClass < 0x10000) {
        g_refused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t write = g_write.load(std::memory_order_relaxed);
    const std::uint32_t read = g_read.load(std::memory_order_acquire);
    if (write - read >= kItemUseCapacity) {

        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ItemUseSample& slot = g_ring[write % kItemUseCapacity];
    slot.itemClass = itemClass;
    slot.ms = ms;
    g_write.store(write + 1, std::memory_order_release);
    g_noted.fetch_add(1, std::memory_order_relaxed);
}

bool take_item_use(ItemUseSample& out) {
    const std::uint32_t read = g_read.load(std::memory_order_relaxed);
    const std::uint32_t write = g_write.load(std::memory_order_acquire);
    if (read == write) return false;
    out = g_ring[read % kItemUseCapacity];
    g_read.store(read + 1, std::memory_order_release);
    g_drained.fetch_add(1, std::memory_order_relaxed);
    return true;
}

ItemUseStats item_use_stats() {
    ItemUseStats s;
    s.noted = g_noted.load(std::memory_order_relaxed);
    s.dropped = g_dropped.load(std::memory_order_relaxed);
    s.drained = g_drained.load(std::memory_order_relaxed);
    s.sent = g_sent.load(std::memory_order_relaxed);
    s.refused = g_refused.load(std::memory_order_relaxed);
    s.refusedForeign = g_foreign.load(std::memory_order_relaxed);
    return s;
}

void note_item_use_refused() { g_refused.fetch_add(1, std::memory_order_relaxed); }
void note_item_use_foreign() { g_foreign.fetch_add(1, std::memory_order_relaxed); }
void note_item_use_sent() { g_sent.fetch_add(1, std::memory_order_relaxed); }

std::vector<std::uint8_t> encode_item_use(const std::string& classPath, std::string& note) {
    note.clear();
    if (classPath.empty()) {
        note = "the item's class path is empty, so there is nothing to load on the far side";
        return {};
    }
    if (classPath.size() > kMaxItemPath) {
        note = "the item's class path is " + std::to_string(classPath.size()) + " byte(s), past the " +
               std::to_string(kMaxItemPath) + " this wire allows";
        return {};
    }
    if (!printable_ascii(classPath)) {

        note = "the item's class path has a byte that is not printable ASCII: '" + classPath + "'";
        return {};
    }
    std::vector<std::uint8_t> out;
    out.reserve(3 + classPath.size());
    out.push_back(kItemWireFormat);
    const auto length = static_cast<std::uint16_t>(classPath.size());
    out.push_back(static_cast<std::uint8_t>(length & 0xFF));
    out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
    out.insert(out.end(), classPath.begin(), classPath.end());
    return out;
}

bool decode_item_use(std::span<const std::uint8_t> bytes, std::string& classPath, std::string& note) {
    note.clear();
    if (bytes.size() < 3) {
        note = "the payload is " + std::to_string(bytes.size()) + " byte(s), shorter than its 3-byte header";
        return false;
    }
    if (bytes[0] != kItemWireFormat) {
        note = "format " + std::to_string(static_cast<unsigned>(bytes[0])) + " is not this build's (" +
               std::to_string(static_cast<unsigned>(kItemWireFormat)) + ")";
        return false;
    }
    const std::size_t length =
        static_cast<std::size_t>(bytes[1]) | (static_cast<std::size_t>(bytes[2]) << 8);
    if (length == 0) {
        note = "the payload names no item path (its length field is 0)";
        return false;
    }
    if (length > kMaxItemPath) {
        note = "the length field says " + std::to_string(length) + " byte(s), past the " +
               std::to_string(kMaxItemPath) + " this wire allows";
        return false;
    }
    if (bytes.size() != 3 + length) {
        note = "the payload is " + std::to_string(bytes.size()) + " byte(s) but its length field says " +
               std::to_string(3 + length) + " -- trailing or missing bytes are refused rather than ignored";
        return false;
    }
    std::string path(reinterpret_cast<const char*>(bytes.data() + 3), length);
    if (!printable_ascii(path)) {
        note = "the item's class path has a byte that is not printable ASCII";
        return false;
    }
    classPath = std::move(path);
    return true;
}

}
