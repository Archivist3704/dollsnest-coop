#include "dsh/game/pawn.h"

#include "dsh/platform/log.h"
#include "dsh/reflect/scanner.h"
#include "dsh/reflect/offsets.h"
#include "dsh/reflect/spawn.h"
#include "dsh/reflect/world.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace dsh::game {
namespace {

constexpr std::uint32_t kActorRootComponent    = 0x0130;
constexpr std::uint32_t kSceneAttachParent     = 0x00C0;
constexpr std::uint32_t kSceneRelativeLocation = 0x011C;
constexpr std::uint32_t kSceneRelativeRotation = 0x0128;

std::string object_name_of(std::uintptr_t obj) {
    std::uint64_t nameField = 0;
    std::string name;
    if (dsh::reflect::try_read_u64(obj + dsh::reflect::offsets::kUObjectNameOffset,
                                   nameField)) {
        std::string note;
        dsh::reflect::resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), name,
                                    note);
    }
    return name;
}

std::string class_name_of(std::uintptr_t obj) {
    std::uint64_t cls = 0;
    if (!dsh::reflect::try_read_u64(obj + dsh::reflect::offsets::kUObjectClassOffset, cls) ||
        cls < 0x10000) {
        return {};
    }
    return object_name_of(static_cast<std::uintptr_t>(cls));
}

bool read_floats(std::uintptr_t addr, float* out, int count) {
    return dsh::reflect::try_read_bytes(addr, out, sizeof(float) * static_cast<std::size_t>(count));
}

}

PawnHandle locate_local_pawn() {
    PawnHandle h;
    dsh::reflect::LiveObjects live = dsh::reflect::find_live_objects();
    if (live.world == 0) {
        h.note = "no LIVE world (are we at the main menu?)";
        return h;
    }
    if (live.playerController == 0) {
        h.note = "no live PlayerController in " + std::to_string(live.worldObjectCount) + " worlds";
        return h;
    }

    if (live.pawn == 0) {
        static std::uint64_t s_lastForcedRescanMs = 0;
        const std::uint64_t now = GetTickCount64();
        if (now - s_lastForcedRescanMs >= 5000) {
            s_lastForcedRescanMs = now;
            std::uint64_t rawPawn = 0;
            std::uint64_t rawAck = 0;
            const bool pawnRead = dsh::reflect::try_read_u64(
                live.playerController + dsh::reflect::actor_offsets::kControllerPawn, rawPawn);
            const bool ackRead = dsh::reflect::try_read_u64(
                live.playerController + dsh::reflect::actor_offsets::kAcknowledgedPawn, rawAck);
            const dsh::reflect::LiveObjects again = dsh::reflect::rescan_live_objects();
            DSH_WARN("pawn: the remembered controller 0x%llX '%s' has NO pawn (Pawn %s 0x%llX, "
                     "AcknowledgedPawn %s 0x%llX), so the object table was RE-SCANNED -- it now says "
                     "controller 0x%llX '%s' with pawn 0x%llX (%s)",
                     static_cast<unsigned long long>(live.playerController),
                     dsh::reflect::object_name_of_actor(live.playerController).c_str(),
                     pawnRead ? "reads" : "is UNREADABLE, not", static_cast<unsigned long long>(rawPawn),
                     ackRead ? "reads" : "is UNREADABLE, not", static_cast<unsigned long long>(rawAck),
                     static_cast<unsigned long long>(again.playerController),
                     dsh::reflect::object_name_of_actor(again.playerController).c_str(),
                     static_cast<unsigned long long>(again.pawn),
                     again.pawn != 0 ? "the menu can be built" : "STILL nothing to build a menu on");
            live = again;
        }
    }
    if (live.pawn == 0) {
        h.note = "PlayerController has no Pawn yet";
        h.controller = live.playerController;
        return h;
    }
    h.pawn = live.pawn;
    h.pawnClass = class_name_of(h.pawn);
    h.pawnName = object_name_of(h.pawn);

    std::string whyNot;
    if (!dsh::reflect::object_is_still_registered(h.pawn, whyNot)) {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "0x%llX", static_cast<unsigned long long>(h.pawn));
        h.note = std::string("the controller's Pawn ") + hex +
                 " is not a registered object any more (" + whyNot +
                 ") -- the game has replaced or destroyed it, so it must not be handed to the engine";
        h.pawn = 0;
        h.valid = false;
        return h;
    }

    std::uint64_t root = 0;
    if (!dsh::reflect::try_read_u64(h.pawn + kActorRootComponent, root) || root < 0x10000) {

        h.note = "pawn has no RootComponent (offset 0x130 read as 0x" +
                 std::to_string(root) + ")";
        return h;
    }
    h.rootComponent = static_cast<std::uintptr_t>(root);
    h.valid = true;
    h.note = "ok";
    return h;
}

bool read_pawn_pose(const PawnHandle& handle, dsh::sync::PoseSample& out, std::string& note) {
    if (!handle.valid) {
        note = "invalid pawn handle: " + handle.note;
        return false;
    }

    std::uint64_t parent = 0;
    if (dsh::reflect::try_read_u64(handle.rootComponent + kSceneAttachParent, parent) &&
        parent >= 0x10000) {
        note = "root component HAS an AttachParent (0x" + std::to_string(parent) +
               ") -- RelativeLocation is not world space; refusing to report a pose";
        return false;
    }

    float loc[3] = {0.0f, 0.0f, 0.0f};
    float rot[3] = {0.0f, 0.0f, 0.0f};
    if (!read_floats(handle.rootComponent + kSceneRelativeLocation, loc, 3)) {
        note = "RelativeLocation unreadable";
        return false;
    }
    if (!read_floats(handle.rootComponent + kSceneRelativeRotation, rot, 3)) {
        note = "RelativeRotation unreadable";
        return false;
    }

    constexpr float kMaxCm = 5.0e6f;
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(loc[i]) || std::abs(loc[i]) > kMaxCm) {
            note = "RelativeLocation looks wrong (component " + std::to_string(i) + " = " +
                   std::to_string(loc[i]) + "); refusing to send it";
            return false;
        }
    }

    out.x = loc[0] / 100.0f;
    out.y = loc[1] / 100.0f;
    out.z = loc[2] / 100.0f;
    constexpr float kDegToRad = 3.14159265358979f / 180.0f;
    out.pitch = rot[0] * kDegToRad;
    out.yaw = rot[1] * kDegToRad;
    out.samplerTimeMs = GetTickCount64();
    note = "ok";
    return true;
}

}
