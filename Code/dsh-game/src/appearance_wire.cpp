#include "dsh/game/appearance_wire.h"

#include <cstring>

namespace dsh::game {
namespace {

constexpr std::uint16_t kMaxStringBytes = 2048;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

void put_i32(std::vector<std::uint8_t>& out, std::int32_t v) {
    put_u32(out, static_cast<std::uint32_t>(v));
}

void put_f32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(out, bits);
}

void put_str(std::vector<std::uint8_t>& out, const std::string& s) {
    const std::size_t n = s.size() > kMaxStringBytes ? kMaxStringBytes : s.size();
    put_u16(out, static_cast<std::uint16_t>(n));
    out.insert(out.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> b) : b_(b) {}

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] std::size_t remaining() const { return b_.size() - pos_; }

    std::uint8_t u8() {
        if (pos_ + 1 > b_.size()) { ok_ = false; return 0; }
        return b_[pos_++];
    }
    std::uint16_t u16() {
        if (pos_ + 2 > b_.size()) { ok_ = false; return 0; }
        const std::uint16_t v = static_cast<std::uint16_t>(b_[pos_] | (b_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    std::uint32_t u32() {
        if (pos_ + 4 > b_.size()) { ok_ = false; return 0; }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(b_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() {
        const std::uint32_t bits = u32();
        float v = 0.0f;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }
    std::string str() {
        const std::uint16_t n = u16();
        if (!ok_ || n > kMaxStringBytes || pos_ + n > b_.size()) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(b_.data() + pos_), n);
        pos_ += n;
        return s;
    }

    bool raw(void* dst, std::size_t n) {
        if (pos_ + n > b_.size()) { ok_ = false; return false; }
        std::memcpy(dst, b_.data() + pos_, n);
        pos_ += n;
        return true;
    }

private:
    std::span<const std::uint8_t> b_;
    std::size_t                   pos_ = 0;
    bool                          ok_ = true;
};

}

std::vector<std::uint8_t> encode_loadout_manifest(const dsh::reflect::MachineLoadout* loadout) {
    std::vector<std::uint8_t> out;
    out.reserve(16384);

    out.push_back(kAppearanceVersion);

    const dsh::reflect::MachineLoadout empty;
    const dsh::reflect::MachineLoadout& machine = loadout != nullptr ? *loadout : empty;
    put_str(out, machine.className);
    put_str(out, machine.classPath);
    out.push_back(machine.trigger[0]);
    out.push_back(machine.trigger[1]);
    out.push_back(machine.trigger[2]);

    out.push_back(static_cast<std::uint8_t>(9));
    for (int i = 0; i < 9; ++i) {
        const dsh::reflect::MachineLoadout::Slot& slot = machine.slots[i];
        out.push_back(slot.present ? 1u : 0u);
        if (!slot.present) continue;
        put_str(out, slot.className);
        put_str(out, slot.classPath);
        put_i32(out, static_cast<std::int32_t>(slot.level));
    }

    out.push_back(machine.customizePresent ? 1u : 0u);
    if (machine.customizePresent) {
        for (int k = 0; k < 4; ++k) {
            put_str(out, machine.partsItemName[k]);
            put_str(out, machine.partsItemPath[k]);
        }
        for (int i = 0; i < 13; ++i) {
            for (int c = 0; c < 4; ++c) put_f32(out, machine.customize.coloring[i][c]);
        }
        for (int i = 0; i < 5; ++i) put_f32(out, machine.customize.figure[i]);
        out.push_back(machine.inverseMesh[0]);
        out.push_back(machine.inverseMesh[1]);
        out.push_back(machine.inverseMesh[2]);
    }

    out.push_back(machine.moduleColoringPresent ? 1u : 0u);
    if (machine.moduleColoringPresent) {
        for (int m = 0; m < dsh::reflect::kModuleColorCount; ++m) {
            for (int ch = 0; ch < dsh::reflect::kModuleColorChannels; ++ch) {
                for (int c = 0; c < dsh::reflect::kModuleColorComponents; ++c) {
                    put_f32(out, machine.moduleColoring[m][ch][c]);
                }
            }
        }
    }
    return out;
}

bool decode_loadout_manifest(std::span<const std::uint8_t> bytes, std::string& note,
                       dsh::reflect::MachineLoadout* loadoutOut) {
    Reader r(bytes);
    const std::uint8_t version = r.u8();
    if (!r.ok() || version != kAppearanceVersion) {

        note = "appearance: the manifest says version " + std::to_string(version) +
               " and this build reads version " + std::to_string(kAppearanceVersion) +
               " -- refused whole rather than read as a loadout, because the two layouts differ (version "
               "5 appends FDAAssembleData::ColoringData, the armour shell's 576 colour bytes) and reading "
               "one as the other would produce a real-looking loadout from bytes that never meant it";
        return false;
    }

    dsh::reflect::MachineLoadout machine;
    machine.className = r.str();
    machine.classPath = r.str();
    if (!r.ok()) {
        note = "appearance: truncated at the machine's class name or path";
        return false;
    }
    if (!r.raw(machine.trigger, sizeof(machine.trigger))) {
        note = "appearance: truncated at FDATriggerData";
        return false;
    }
    const std::uint8_t slotCount = r.u8();
    if (!r.ok() || slotCount != 9) {
        note = "appearance: the machine loadout declares " + std::to_string(slotCount) +
               " slot(s) and this build writes 9 -- FDAAssembleData's order is not ours to guess, so "
               "the manifest is refused";
        return false;
    }
    for (int i = 0; i < 9; ++i) {
        const std::uint8_t present = r.u8();
        if (!r.ok()) {
            note = "appearance: truncated at the machine loadout's slot " + std::to_string(i);
            return false;
        }
        if (present == 0) continue;
        dsh::reflect::MachineLoadout::Slot& slot = machine.slots[i];
        slot.present = true;
        slot.className = r.str();
        slot.classPath = r.str();
        slot.level = r.i32();
        if (!r.ok()) {
            note = "appearance: truncated inside the machine loadout's slot " + std::to_string(i);
            return false;
        }
    }
    const std::uint8_t customizePresent = r.u8();
    if (!r.ok()) {
        note = "appearance: truncated at the machine's customize flag";
        return false;
    }
    if (customizePresent != 0) {
        for (int k = 0; k < 4; ++k) {
            machine.partsItemName[k] = r.str();
            machine.partsItemPath[k] = r.str();
        }
        for (int i = 0; i < 13; ++i) {
            for (int c = 0; c < 4; ++c) machine.customize.coloring[i][c] = r.f32();
        }
        for (int i = 0; i < 5; ++i) machine.customize.figure[i] = r.f32();
        if (!r.raw(machine.inverseMesh, sizeof(machine.inverseMesh))) {
            note = "appearance: truncated at the machine's bInverse*Mesh flags";
            return false;
        }
        if (!r.ok()) {
            note = "appearance: truncated inside the machine's FDACustomizeData";
            return false;
        }
        machine.customizePresent = true;
    }

    const std::uint8_t moduleColorPresent = r.u8();
    if (!r.ok()) {
        note = "appearance: truncated at the machine's armour-shell colour flag";
        return false;
    }
    if (moduleColorPresent != 0) {
        for (int m = 0; m < dsh::reflect::kModuleColorCount; ++m) {
            for (int ch = 0; ch < dsh::reflect::kModuleColorChannels; ++ch) {
                for (int c = 0; c < dsh::reflect::kModuleColorComponents; ++c) {
                    machine.moduleColoring[m][ch][c] = r.f32();
                }
            }
        }
        if (!r.ok()) {
            note = "appearance: truncated inside the machine's FDAAssembleData::ColoringData (the "
                   "armour shell, 576 bytes)";
            return false;
        }
        machine.moduleColoringPresent = true;
    }

    if (loadoutOut != nullptr) *loadoutOut = std::move(machine);
    note.clear();
    return true;
}

std::vector<std::vector<std::uint8_t>> chunk_appearance(const std::vector<std::uint8_t>& manifest,
                                                         std::uint32_t manifestId) {
    std::vector<std::vector<std::uint8_t>> out;
    const std::size_t payloadBytes = kAppearanceChunkBytes - kAppearanceChunkHeader;
    const std::size_t total =
        manifest.empty() ? 1 : (manifest.size() + payloadBytes - 1) / payloadBytes;
    if (total > 0xFFFFu) return out;

    for (std::size_t i = 0; i < total; ++i) {
        const std::size_t begin = i * payloadBytes;
        const std::size_t end = (begin + payloadBytes < manifest.size()) ? begin + payloadBytes
                                                                        : manifest.size();
        std::vector<std::uint8_t> chunk;
        chunk.reserve(kAppearanceChunkBytes);
        put_u32(chunk, manifestId);
        put_u16(chunk, static_cast<std::uint16_t>(total));
        put_u16(chunk, static_cast<std::uint16_t>(i));
        chunk.insert(chunk.end(), manifest.begin() + static_cast<std::ptrdiff_t>(begin),
                     manifest.begin() + static_cast<std::ptrdiff_t>(end));
        out.push_back(std::move(chunk));
    }
    return out;
}

void AppearanceAssembler::reset() {
    manifestId_ = 0;
    total_ = 0;
    received_ = 0;
    complete_ = false;
    chunks_.clear();
    have_.clear();
}

bool AppearanceAssembler::add_chunk(std::span<const std::uint8_t> payload, std::string& note) {
    if (payload.size() < kAppearanceChunkHeader) {
        note = "appearance: chunk shorter than its envelope";
        return false;
    }
    Reader r(payload);
    const std::uint32_t id = r.u32();
    const std::uint16_t total = r.u16();
    const std::uint16_t index = r.u16();
    if (!r.ok() || total == 0 || index >= total) {
        note = "appearance: chunk envelope is not usable";
        return false;
    }

    if (total > kAppearanceMaxChunks) {
        note = "appearance: the sender claims " + std::to_string(total) +
               " chunks and this build's ceiling is " + std::to_string(kAppearanceMaxChunks) +
               " -- refused before allocating anything";
        return false;
    }

    if (id != manifestId_) {
        reset();
        manifestId_ = id;
        total_ = total;
        chunks_.resize(total);
        have_.assign(total, false);
    }
    if (total != total_) {
        note = "appearance: chunk count disagrees with the manifest in progress";
        return false;
    }

    if (!have_[index]) {
        have_[index] = true;
        chunks_[index].assign(payload.begin() + static_cast<std::ptrdiff_t>(kAppearanceChunkHeader),
                              payload.end());
        ++received_;
    }
    if (received_ == total_ && !complete_) {
        complete_ = true;
        ++completed_;
        note = "appearance: manifest complete";
        return true;
    }
    note.clear();
    return false;
}

bool AppearanceAssembler::take(std::string& note, dsh::reflect::MachineLoadout* loadoutOut) {
    if (!complete_) {
        note = "appearance: manifest is not complete";
        return false;
    }
    std::vector<std::uint8_t> whole;
    std::size_t totalBytes = 0;
    for (const auto& c : chunks_) totalBytes += c.size();
    whole.reserve(totalBytes);
    for (const auto& c : chunks_) whole.insert(whole.end(), c.begin(), c.end());

    const bool ok = decode_loadout_manifest(whole, note, loadoutOut);
    reset();
    return ok;
}

}
