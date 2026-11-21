#include "dsh/game/doll_stance.h"

namespace dsh::game {

const char* stance_name(std::uint8_t stance) noexcept {
    switch (stance) {
        case kStanceNeutral: return "Neutral";
        case kStanceFight:   return "Fight";
        case kStanceBeware:  return "Beware";
        default:             return "?";
    }
}

std::size_t encode_stance_state(dsh::net::ByteWriter& w, const StanceWireState& state) {
    const std::size_t before = w.size();
    w.f32(state.posM[0]);
    w.f32(state.posM[1]);
    w.f32(state.posM[2]);
    w.f32(state.yawRad);
    w.f32(state.pitchRad);

    w.u8(state.stance.defaultStance);
    w.u8(state.stance.currentStance);

    w.f32(state.velocity[0]);
    w.f32(state.velocity[1]);
    w.f32(state.velocity[2]);
    w.u8(state.movementMode);

    w.u8(state.movementInput.ignoreFriction);
    w.u8(state.movementInput.hover);
    w.u8(state.movementInput.sprint);
    w.u8(state.movementInput.boostJump);
    w.u8(state.movementInput.boostDash);
    w.f32(state.movementInput.recentInput[0]);
    w.f32(state.movementInput.recentInput[1]);
    w.f32(state.movementInput.recentInput[2]);

    w.f32(state.energyTimers.intermissionTime);
    w.f32(state.energyTimers.fastChargeStartTime);

    w.u8(state.frontWeaponIndex);

    w.f32(state.acceleration[0]);
    w.f32(state.acceleration[1]);
    w.f32(state.acceleration[2]);

    w.u8(state.exclusiveTypes);
    w.u8(state.exclusiveOwnerKind);
    return w.size() - before;
}

bool decode_stance_state(std::span<const std::uint8_t> bytes, StanceWireState& out,
                         std::string& note) {

    if (bytes.size() < kStatePayloadBytes) {
        note = "the state payload is " + std::to_string(bytes.size()) + " byte(s) and this build needs " +
               std::to_string(kStatePayloadBytes) +
               " (five pose float32, the two stance bytes, three velocity float32, the "
               "MovementMode byte, five movement-input flag bytes, the twelve-byte control-input "
               "vector, the host's two energy timer float32, the host's front-weapon byte, the "
               "host's three acceleration float32 and the host's exclusive-mode pair) -- dropped";
        return false;
    }

    dsh::net::ByteReader reader(bytes);
    if (!reader.f32(out.posM[0]) || !reader.f32(out.posM[1]) || !reader.f32(out.posM[2]) ||
        !reader.f32(out.yawRad) || !reader.f32(out.pitchRad)) {
        note = "the state payload is long enough but its five pose float32 could not be read -- dropped";
        return false;
    }
    if (!reader.u8(out.stance.defaultStance) || !reader.u8(out.stance.currentStance)) {
        note = "the state payload carries no readable stance bytes -- dropped";
        return false;
    }
    if (!reader.f32(out.velocity[0]) || !reader.f32(out.velocity[1]) || !reader.f32(out.velocity[2])) {
        note = "the state payload carries no readable velocity float32 -- dropped";
        return false;
    }
    if (!reader.u8(out.movementMode)) {
        note = "the state payload carries no readable MovementMode byte -- dropped";
        return false;
    }

    if (!reader.u8(out.movementInput.ignoreFriction) || !reader.u8(out.movementInput.hover) ||
        !reader.u8(out.movementInput.sprint) || !reader.u8(out.movementInput.boostJump) ||
        !reader.u8(out.movementInput.boostDash)) {
        note = "the state payload carries no readable movement-input flag bytes -- dropped";
        return false;
    }
    if (!reader.f32(out.movementInput.recentInput[0]) ||
        !reader.f32(out.movementInput.recentInput[1]) ||
        !reader.f32(out.movementInput.recentInput[2])) {
        note = "the state payload carries no readable control-input vector -- dropped";
        return false;
    }

    if (!reader.f32(out.energyTimers.intermissionTime) ||
        !reader.f32(out.energyTimers.fastChargeStartTime)) {
        note = "the state payload carries no readable energy timer float32 -- dropped";
        return false;
    }

    if (!reader.u8(out.frontWeaponIndex)) {
        note = "the state payload carries no readable front-weapon byte -- dropped";
        return false;
    }

    if (!reader.f32(out.acceleration[0]) || !reader.f32(out.acceleration[1]) ||
        !reader.f32(out.acceleration[2])) {
        note = "the state payload carries no readable acceleration float32 -- dropped";
        return false;
    }

    if (!reader.u8(out.exclusiveTypes) || !reader.u8(out.exclusiveOwnerKind)) {
        note = "the state payload carries no readable exclusive-mode pair -- dropped";
        return false;
    }
    note.clear();
    return true;
}

bool actor_class_chain_reaches(std::uintptr_t actor, std::uintptr_t wantedClass,
                              StanceByteReader read) noexcept {
    if (actor < 0x10000 || wantedClass == 0 || read == nullptr) return false;

    std::uint64_t classValue = 0;
    if (!read(actor + 0x10, &classValue, sizeof(classValue))) return false;
    std::uintptr_t cls = static_cast<std::uintptr_t>(classValue);

    for (int depth = 0; cls >= 0x10000 && depth < 32; ++depth) {
        if (cls == wantedClass) return true;
        std::uint64_t super = 0;
        if (!read(cls + 0x40, &super, sizeof(super))) return false;
        cls = static_cast<std::uintptr_t>(super);
    }
    return false;
}

bool read_doll_stance(std::uintptr_t actor, std::uintptr_t characterClass, DollStance& out,
                      std::string& note, StanceByteReader read) {
    out = DollStance{};
    note.clear();

    if (actor < 0x10000 || read == nullptr) {
        note = "no actor to read the stance from";
        return false;
    }
    if (characterClass == 0) {

        note = "the " + std::string(kStanceApiClass) +
               " class could not be resolved, so the class chain cannot be checked and the stance is "
               "NOT read";
        return false;
    }
    if (!actor_class_chain_reaches(actor, characterClass, read)) {
        note = "the actor's class chain does not reach " + std::string(kStanceApiClass) +
               ", so its is not CurrentStance and NOTHING is read from it";
        return false;
    }

    if (!read(actor + kCharacterDefaultStanceOffset, &out.defaultStance, sizeof(out.defaultStance))) {
        note = "DefaultStance at could not be read";
        return false;
    }
    if (!read(actor + kCharacterCurrentStanceOffset, &out.currentStance, sizeof(out.currentStance))) {
        note = "CurrentStance at could not be read";
        return false;
    }
    return true;
}

}
