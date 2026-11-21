#include "dsh/game/health_wire.h"

#include <cmath>
#include <cstring>

namespace dsh::game {
namespace {

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

void put_f32(std::vector<std::uint8_t>& out, float v) {
    std::uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(v), "float must be 32 bits");
    std::memcpy(&bits, &v, sizeof(bits));
    put_u32(out, bits);
}

std::uint16_t get_u16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | (static_cast<std::uint16_t>(b[at + 1]) << 8));
}

std::uint32_t get_u32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) |
           (static_cast<std::uint32_t>(b[at + 2]) << 16) | (static_cast<std::uint32_t>(b[at + 3]) << 24);
}

float get_f32(std::span<const std::uint8_t> b, std::size_t at) {
    const std::uint32_t bits = get_u32(b, at);
    float v = 0.0f;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
}

std::vector<std::uint8_t> encode_health(const std::vector<HealthEntry>& entries, bool requirePositive,
                                        std::string& note) {
    std::vector<std::uint8_t> out;
    note.clear();
    std::uint32_t skippedEmpty = 0;
    std::uint32_t skippedValue = 0;
    std::uint32_t tooMany = 0;

    std::vector<const HealthEntry*> kept;
    for (const HealthEntry& e : entries) {
        if (e.enemyName.empty() || e.enemyName.size() > kMaxHealthName) {
            ++skippedEmpty;
            continue;
        }
        if (!std::isfinite(e.value)) {
            ++skippedValue;
            continue;
        }
        if (requirePositive && !(e.value > 0.0f)) {
            ++skippedValue;
            continue;
        }
        if (!requirePositive && e.value < 0.0f) {
            ++skippedValue;
            continue;
        }
        kept.push_back(&e);
    }
    if (kept.size() > kMaxHealthEntries) {
        tooMany = static_cast<std::uint32_t>(kept.size() - kMaxHealthEntries);
        kept.resize(kMaxHealthEntries);
    }

    put_u16(out, static_cast<std::uint16_t>(kept.size()));
    for (const HealthEntry* e : kept) {
        out.push_back(static_cast<std::uint8_t>(e->enemyName.size()));
        out.insert(out.end(), e->enemyName.begin(), e->enemyName.end());
        put_u32(out, e->sequence);
        put_f32(out, e->value);
    }

    if (skippedEmpty != 0 || skippedValue != 0 || tooMany != 0) {
        note = "skipped " + std::to_string(skippedEmpty) + " entr(y/ies) with no usable enemy name and " +
               std::to_string(skippedValue) + " with an unusable value; " + std::to_string(tooMany) +
               " over the " + std::to_string(kMaxHealthEntries) + "-entry cap were not encoded";
    }
    return out;
}

}

std::vector<std::uint8_t> encode_health_report(const std::vector<HealthEntry>& entries, std::string& note) {
    return encode_health(entries, true, note);
}

std::vector<std::uint8_t> encode_health_broadcast(const std::vector<HealthEntry>& entries, std::string& note) {
    return encode_health(entries, false, note);
}

bool decode_health(std::span<const std::uint8_t> bytes, bool requirePositiveValue,
                   std::vector<HealthEntry>& out, std::string& note) {
    out.clear();
    note.clear();
    if (bytes.size() < 2) {
        note = "the payload is shorter than its two-byte count";
        return false;
    }
    const std::uint16_t count = get_u16(bytes, 0);
    if (count > kMaxHealthEntries) {
        note = "the count is " + std::to_string(count) + ", above the " + std::to_string(kMaxHealthEntries) +
               "-entry cap -- refused rather than truncated";
        return false;
    }
    std::size_t at = 2;
    std::vector<HealthEntry> decoded;
    decoded.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        if (at >= bytes.size()) {
            note = "the payload ends inside entry " + std::to_string(i + 1) + " of " + std::to_string(count);
            return false;
        }
        const std::uint8_t nameLen = bytes[at++];
        if (nameLen == 0) {
            note = "entry " + std::to_string(i + 1) + " names no enemy -- an entry with no name cannot be "
                   "matched on this side, so the whole payload is refused";
            return false;
        }
        if (nameLen > kMaxHealthName) {
            note = "entry " + std::to_string(i + 1) + " has a " + std::to_string(nameLen) +
                   "-byte name, above the " + std::to_string(kMaxHealthName) + "-byte cap";
            return false;
        }
        if (at + nameLen + 8 > bytes.size()) {
            note = "the payload ends inside entry " + std::to_string(i + 1) + " of " + std::to_string(count);
            return false;
        }
        HealthEntry e;
        e.enemyName.assign(reinterpret_cast<const char*>(bytes.data() + at), nameLen);
        at += nameLen;
        e.sequence = get_u32(bytes, at);
        at += 4;
        e.value = get_f32(bytes, at);
        at += 4;
        if (!std::isfinite(e.value)) {
            note = "entry " + std::to_string(i + 1) + " ('" + e.enemyName + "') carries a non-finite value; "
                   "refused rather than applied";
            return false;
        }
        if (requirePositiveValue && !(e.value > 0.0f)) {
            note = "entry " + std::to_string(i + 1) + " ('" + e.enemyName + "') reports " +
                   std::to_string(e.value) + " damage; a report must be positive, and a zero or negative "
                   "one is refused rather than treated as healing";
            return false;
        }
        if (!requirePositiveValue && e.value < 0.0f) {
            note = "entry " + std::to_string(i + 1) + " ('" + e.enemyName + "') broadcasts a negative health";
            return false;
        }
        decoded.push_back(std::move(e));
    }
    if (at != bytes.size()) {
        note = "there are " + std::to_string(bytes.size() - at) + " trailing byte(s) after the last entry -- "
               "the sender and this decoder disagree about the format";
        return false;
    }
    out = std::move(decoded);
    return true;
}

std::vector<std::uint8_t> encode_player_health(const PlayerHealth& value, std::string& note) {
    note.clear();
    if (!std::isfinite(value.health) || !std::isfinite(value.maxHealth)) {
        note = "the local player's own health is not a finite number (" + std::to_string(value.health) + " of " +
               std::to_string(value.maxHealth) + "), so nothing is sent -- a NaN written into the peer's "
               "puppet would be worse than a stale number";
        return {};
    }
    if (value.health < 0.0f || value.maxHealth < 0.0f) {
        note = "the local player's own health is negative (" + std::to_string(value.health) + " of " +
               std::to_string(value.maxHealth) + ") -- refused rather than sent";
        return {};
    }
    std::vector<std::uint8_t> out;
    out.reserve(kPlayerHealthBytes);
    out.push_back(kPlayerHealthFormat);
    put_f32(out, value.health);
    put_f32(out, value.maxHealth);
    return out;
}

bool decode_player_health(std::span<const std::uint8_t> bytes, PlayerHealth& out, std::string& note) {
    out = PlayerHealth{};
    note.clear();
    if (bytes.size() != kPlayerHealthBytes) {
        note = "the player-health payload is " + std::to_string(bytes.size()) + " bytes, and this build's is " +
               std::to_string(kPlayerHealthBytes) + " -- refused rather than decoded into the wrong field";
        return false;
    }
    if (bytes[0] != kPlayerHealthFormat) {
        note = "the player-health payload is format " + std::to_string(bytes[0]) + ", and this build speaks " +
               std::to_string(kPlayerHealthFormat);
        return false;
    }

    const float health = get_f32(bytes, 1);
    const float maxHealth = get_f32(bytes, 5);
    if (!std::isfinite(health) || !std::isfinite(maxHealth)) {
        note = "the player-health payload carries a non-finite number";
        return false;
    }
    if (health < 0.0f || maxHealth < 0.0f) {
        note = "the player-health payload carries a negative number";
        return false;
    }
    out.health = health;
    out.maxHealth = maxHealth;
    return true;
}

}
