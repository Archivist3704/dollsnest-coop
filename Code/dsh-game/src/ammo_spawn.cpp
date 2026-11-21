#include "dsh/game/ammo_spawn.h"

#include <atomic>
#include <cstring>

namespace dsh::game {
namespace {

struct AmmoRingSlot {
    AmmoSpawnSample sample;
};
AmmoRingSlot               g_ammoRing[kAmmoSpawnCapacity];
std::atomic<std::uint32_t> g_ammoWrite{0};
std::atomic<std::uint32_t> g_ammoRead{0};
std::atomic<std::uint64_t> g_ammoNoted{0};
std::atomic<std::uint64_t> g_ammoDropped{0};
std::atomic<std::uint64_t> g_ammoDrained{0};
std::atomic<std::uint64_t> g_ammoRefused{0};

std::atomic<std::uint64_t> g_ammoForeign{0};

std::atomic<std::uintptr_t> g_ammoParamsFunction{0};

std::atomic<bool>           g_ammoCaptureEnabled{false};

std::atomic<std::uint64_t>  g_traceUntilMs{0};

void put_u16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
    v.push_back(static_cast<std::uint8_t>((x >> 8) & 0xFF));
}

void put_f32(std::vector<std::uint8_t>& v, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFF));
}

void put_f32x3(std::vector<std::uint8_t>& v, const float values[3]) {
    for (int i = 0; i < 3; ++i) put_f32(v, values[i]);
}

bool get_f32x3(std::span<const std::uint8_t> b, std::size_t off, float out[3]) {
    if (off + 12 > b.size()) return false;
    for (int i = 0; i < 3; ++i) {
        const std::size_t at = off + static_cast<std::size_t>(i) * 4;
        const std::uint32_t bits = static_cast<std::uint32_t>(b[at]) |
                                   (static_cast<std::uint32_t>(b[at + 1]) << 8) |
                                   (static_cast<std::uint32_t>(b[at + 2]) << 16) |
                                   (static_cast<std::uint32_t>(b[at + 3]) << 24);
        std::memcpy(&out[i], &bits, sizeof(out[i]));
    }
    return true;
}

void put_str(std::vector<std::uint8_t>& v, const std::string& s) {
    put_u16(v, static_cast<std::uint16_t>(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}

std::uint16_t get_u16(std::span<const std::uint8_t> b, std::size_t off) {
    return static_cast<std::uint16_t>(b[off]) | (static_cast<std::uint16_t>(b[off + 1]) << 8);
}

}
void set_ammo_params_function(std::uintptr_t function) {
    g_ammoParamsFunction.store(function >= 0x10000 ? function : 0, std::memory_order_relaxed);
}

std::uintptr_t ammo_params_function() {
    return g_ammoParamsFunction.load(std::memory_order_relaxed);
}

void set_ammo_capture_enabled(bool enabled) {
    g_ammoCaptureEnabled.store(enabled, std::memory_order_relaxed);
}

bool ammo_capture_enabled() {
    return g_ammoCaptureEnabled.load(std::memory_order_relaxed);
}

void note_ammo_spawn(const AmmoSpawnSample& sample) {
    if (!g_ammoCaptureEnabled.load(std::memory_order_relaxed)) return;
    const std::uint32_t write = g_ammoWrite.load(std::memory_order_relaxed);
    const std::uint32_t read = g_ammoRead.load(std::memory_order_acquire);
    if (write - read >= kAmmoSpawnCapacity) {
        g_ammoDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    AmmoSpawnSample& slot = g_ammoRing[write % kAmmoSpawnCapacity].sample;
    slot = sample;

    for (const std::size_t offset : kAmmoPointerOffsets) {
        if (offset + sizeof(std::uintptr_t) <= kAmmoParamsBytes) {
            std::memset(slot.params + offset, 0, sizeof(std::uintptr_t));
        }
    }
    g_ammoWrite.store(write + 1, std::memory_order_release);
    g_ammoNoted.fetch_add(1, std::memory_order_relaxed);
}

bool take_ammo_spawn(AmmoSpawnSample& out) {
    const std::uint32_t read = g_ammoRead.load(std::memory_order_relaxed);
    const std::uint32_t write = g_ammoWrite.load(std::memory_order_acquire);
    if (read == write) return false;
    out = g_ammoRing[read % kAmmoSpawnCapacity].sample;
    g_ammoRead.store(read + 1, std::memory_order_release);
    g_ammoDrained.fetch_add(1, std::memory_order_relaxed);
    return true;
}

AmmoSpawnStats ammo_spawn_stats() {
    AmmoSpawnStats s;
    s.noted = g_ammoNoted.load(std::memory_order_relaxed);
    s.dropped = g_ammoDropped.load(std::memory_order_relaxed);
    s.drained = g_ammoDrained.load(std::memory_order_relaxed);
    s.refused = g_ammoRefused.load(std::memory_order_relaxed);
    s.foreign = g_ammoForeign.load(std::memory_order_relaxed);
    return s;
}

void note_ammo_foreign() { g_ammoForeign.fetch_add(1, std::memory_order_relaxed); }

void note_ammo_refused() {
    g_ammoRefused.fetch_add(1, std::memory_order_relaxed);
}

void request_process_event_trace(std::uint64_t untilMs) {
    g_traceUntilMs.store(untilMs, std::memory_order_relaxed);
}

std::uint64_t process_event_trace_until_ms() {
    return g_traceUntilMs.load(std::memory_order_relaxed);
}

std::vector<std::uint8_t> encode_ammo_spawn(const dsh::reflect::AmmoSpawnRequest& request,
                                             std::string& note) {
    note.clear();
    if (request.classPath.empty()) {
        note = "the captured projectile has no class path, so the far side would have nothing to spawn";
        return {};
    }
    const auto tooLong = [](const std::string& s) { return s.size() > kMaxAmmoPath; };
    if (tooLong(request.classPath) || tooLong(request.speedCurvePath) ||
        tooLong(request.pointDamageCurvePath) || tooLong(request.damageTypePath)) {
        note = "one of the captured object paths is longer than " + std::to_string(kMaxAmmoPath) +
               " bytes, and a truncated path would name a different object";
        return {};
    }
    if (request.targetKind > kAmmoTargetRefused) {
        note = "the projectile's target kind is " + std::to_string(request.targetKind) +
               ", which this build does not define";
        return {};
    }

    if (request.targetKind == kAmmoTargetEnemy && request.targetName.empty()) {
        note = "the projectile chases an enemy but carries no name, so the far side could not find it";
        return {};
    }
    if (request.targetKind != kAmmoTargetEnemy && !request.targetName.empty()) {
        note = "the projectile carries a target NAME with kind " + std::to_string(request.targetKind) +
               ", which contradicts itself";
        return {};
    }
    if (request.targetName.size() > kMaxAmmoName) {
        note = "the target name is longer than " + std::to_string(kMaxAmmoName) + " bytes";
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(0x180);
    out.push_back(kAmmoWireFormat);
    put_u16(out, static_cast<std::uint16_t>(kAmmoParamsBytes));
    put_str(out, request.classPath);
    put_str(out, request.speedCurvePath);
    put_str(out, request.pointDamageCurvePath);
    put_str(out, request.damageTypePath);
    out.push_back(request.targetKind);
    put_str(out, request.targetName);
    put_f32x3(out, request.locationCm);
    put_f32x3(out, request.rotation);
    out.insert(out.end(), request.params, request.params + kAmmoParamsBytes);
    return out;
}

bool decode_ammo_spawn(std::span<const std::uint8_t> bytes, dsh::reflect::AmmoSpawnRequest& out,
                        std::string& note) {
    note.clear();

    constexpr std::size_t kHead = 3;
    if (bytes.size() < kHead) {
        note = "the projectile payload is " + std::to_string(bytes.size()) + " bytes, shorter than its head";
        return false;
    }
    if (bytes[0] != kAmmoWireFormat) {
        note = "the projectile payload is format " + std::to_string(bytes[0]) +
               ", and this build speaks format " + std::to_string(kAmmoWireFormat);
        return false;
    }
    const std::uint16_t paramsLength = get_u16(bytes, 1);
    if (paramsLength != kAmmoParamsBytes) {
        note = "the projectile payload carries a " + std::to_string(paramsLength) +
               "-byte AmmoParams block, and this build's FDAAmmoParams is " +
               std::to_string(kAmmoParamsBytes) + " bytes -- refusing rather than decoding into the wrong "
               "field";
        return false;
    }

    std::size_t cursor = kHead;
    const auto readStr = [&bytes, &cursor, &note](std::string& target, std::size_t bound, const char* what) {
        if (cursor + 2 > bytes.size()) {
            note = std::string("the projectile payload ends before the ") + what + " length";
            return false;
        }
        const std::size_t length = get_u16(bytes, cursor);
        cursor += 2;
        if (length > bound) {
            note = std::string("the ") + what + " is " + std::to_string(length) + " bytes, past the " +
                   std::to_string(bound) + "-byte bound";
            return false;
        }
        if (cursor + length > bytes.size()) {
            note = std::string("the projectile payload ends inside the ") + what;
            return false;
        }
        target.assign(reinterpret_cast<const char*>(bytes.data() + cursor), length);
        cursor += length;
        return true;
    };

    out = dsh::reflect::AmmoSpawnRequest{};
    if (!readStr(out.classPath, kMaxAmmoPath, "class path")) return false;
    if (!readStr(out.speedCurvePath, kMaxAmmoPath, "speed curve path")) return false;
    if (!readStr(out.pointDamageCurvePath, kMaxAmmoPath, "point damage curve path")) return false;
    if (!readStr(out.damageTypePath, kMaxAmmoPath, "damage type path")) return false;

    if (cursor >= bytes.size()) {
        note = "the projectile payload ends before its target kind";
        return false;
    }
    out.targetKind = bytes[cursor++];
    if (out.targetKind > kAmmoTargetRefused) {
        note = "the projectile names target kind " + std::to_string(out.targetKind) +
               ", which this build does not define";
        return false;
    }
    if (!readStr(out.targetName, kMaxAmmoName, "target name")) return false;
    if (out.targetKind == kAmmoTargetEnemy && out.targetName.empty()) {
        note = "the projectile chases an enemy but carries no name";
        return false;
    }
    if (out.targetKind != kAmmoTargetEnemy && !out.targetName.empty()) {
        note = "the projectile carries a target NAME under kind " + std::to_string(out.targetKind);
        return false;
    }

    if (!get_f32x3(bytes, cursor, out.locationCm)) {
        note = "the projectile payload ends inside the placement (three floats of centimetres)";
        return false;
    }
    cursor += 12;
    if (!get_f32x3(bytes, cursor, out.rotation)) {
        note = "the projectile payload ends inside the rotation (three floats of degrees)";
        return false;
    }
    cursor += 12;

    if (cursor + kAmmoParamsBytes != bytes.size()) {
        note = "the projectile payload is " + std::to_string(bytes.size()) + " bytes and its text ends at " +
               std::to_string(cursor) + ", which leaves " + std::to_string(bytes.size() - cursor) +
               " for a block that is " + std::to_string(kAmmoParamsBytes) +
               " bytes -- trailing bytes are a refusal, not a shrug";
        return false;
    }
    if (out.classPath.empty()) {
        note = "the projectile payload names no class";
        return false;
    }
    std::memcpy(out.params, bytes.data() + cursor, kAmmoParamsBytes);
    return true;
}

AmmoCopyDamage clear_copy_damage(unsigned char* params, std::size_t size) noexcept {
    AmmoCopyDamage out;
    if (params == nullptr || size < kAmmoParamsBytes) return out;
    const auto readFloat = [params](std::size_t at) {
        float value = 0.0f;
        std::memcpy(&value, params + at, sizeof(value));
        return value;
    };
    const auto zeroFloat = [params](std::size_t at) {
        const float zero = 0.0f;
        std::memcpy(params + at, &zero, sizeof(zero));
    };
    for (std::size_t i = 0; i < kAmmoDamageFloats; ++i) {
        const std::size_t at = i * sizeof(float);
        out.pointBefore[i] = readFloat(kAmmoPointDamageOffset + at);
        out.radialBefore[i] = readFloat(kAmmoRadialDamageOffset + at);
        out.radialMinBefore[i] = readFloat(kAmmoRadialMinimumDamageOffset + at);
    }
    out.pointMinBefore = readFloat(kAmmoPointDamageMinOffset);
    for (std::size_t i = 0; i < kAmmoDamageFloats; ++i) {
        if (out.pointBefore[i] != 0.0f) out.sawPointDamage = true;
        if (out.radialBefore[i] != 0.0f) out.sawRadialDamage = true;
        if (out.radialMinBefore[i] != 0.0f) out.sawRadialDamage = true;
    }
    if (out.pointMinBefore != 0.0f) out.sawPointDamage = true;

    for (std::size_t i = 0; i < kAmmoDamageFloats; ++i) {
        const std::size_t at = i * sizeof(float);
        zeroFloat(kAmmoPointDamageOffset + at);
        zeroFloat(kAmmoRadialDamageOffset + at);
        zeroFloat(kAmmoRadialMinimumDamageOffset + at);
    }
    zeroFloat(kAmmoPointDamageMinOffset);
    return out;
}

}
