#pragma once

#include <cstdint>
#include <string>

#include "dsh/sync/interpolator.h"

namespace dsh::game {

struct PawnHandle {
    bool           valid = false;
    std::uintptr_t controller = 0;
    std::uintptr_t pawn = 0;
    std::uintptr_t rootComponent = 0;
    std::string    pawnClass;
    std::string    pawnName;
    std::string    note;
};

[[nodiscard]] PawnHandle locate_local_pawn();

[[nodiscard]] bool read_pawn_pose(const PawnHandle& handle, dsh::sync::PoseSample& out,
                                 std::string& note);

}
