#include "dsh/game/doll_stance.h"

#include "dsh/reflect/spawn.h"

namespace dsh::game {

bool stance_read_process_bytes(std::uintptr_t address, void* dst, std::size_t bytes) {
    return dsh::reflect::read_process_bytes(address, dst, bytes);
}

StanceApplyResult doll_stance_apply(std::uintptr_t actor, std::uintptr_t characterClass,
                                    const DollStance& incoming) {
    StanceApplyResult result;

    if (actor < 0x10000) {
        result.note = "no actor to apply the stance to";
        return result;
    }
    if (characterClass == 0) {
        result.note = "the " + std::string(kStanceApiClass) +
                      " class could not be resolved, so the class chain cannot be checked and nothing "
                      "is written at";
        return result;
    }

    if (!actor_class_chain_reaches(actor, characterClass, stance_read_process_bytes)) {
        result.note = "the puppet's class chain does not reach " + std::string(kStanceApiClass) +
                      ", so is not CurrentStance and nothing is written";
        return result;
    }

    std::uint8_t current = 0;
    if (!dsh::reflect::read_process_bytes(actor + kCharacterCurrentStanceOffset, &current,
                                          sizeof(current))) {
        result.note = "CurrentStance at could not be read, so there is no old value for the "
                      "RepNotify and nothing is written";
        return result;
    }
    result.written = current;
    if (current == incoming.currentStance) {

        result.applied = true;
        return result;
    }

    if (incoming.currentStance > kStanceHighestReal) {
        result.note = "the host sent stance value " + std::to_string(incoming.currentStance) +
                      ", which is not one of the three values EDACharacterStanceType has (Neutral=0, "
                      "Fight=1, Beware=2) -- refused, so the byte and its neighbour are untouched";
        return result;
    }

    if (!dsh::reflect::write_process_bytes(actor + kCharacterCurrentStanceOffset,
                                           &incoming.currentStance, sizeof(incoming.currentStance))) {
        result.note = "write_process_bytes refused (uncommitted, read-only, or a range that "
                      "straddles two regions) -- nothing was applied";
        return result;
    }
    std::uint8_t readBack = 0;
    if (!dsh::reflect::read_process_bytes(actor + kCharacterCurrentStanceOffset, &readBack,
                                          sizeof(readBack)) ||
        readBack != incoming.currentStance) {
        result.note = "the byte was written but does not read back -- is not holding the "
                      "stance this call wrote";
        return result;
    }
    result.written = readBack;
    result.changed = true;

    std::string callNote;
    if (!dsh::reflect::stance_call_rep_notify(actor, current, callNote)) {

        result.note = "CurrentStance was written, but " + callNote;
        return result;
    }
    result.applied = true;
    return result;
}

}
