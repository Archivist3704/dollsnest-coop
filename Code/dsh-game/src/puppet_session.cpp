#include "dsh/game/puppet_session.h"

#include "dsh/game/appearance_wire.h"
#include "dsh/game/ammo_spawn.h"
#include "dsh/game/item_wire.h"
#include "dsh/game/mission_wire.h"
#include "dsh/reflect/mission.h"
#include "dsh/game/health_sync.h"
#include "dsh/game/doll_stance.h"
#include "dsh/game/exclusive_mode.h"
#include "dsh/game/front_weapon.h"
#include "dsh/game/module_component.h"
#include "dsh/game/movement_input.h"
#include "dsh/game/movement_state.h"
#include "dsh/game/observed_watch.h"
#include "dsh/game/pawn.h"
#include "dsh/game/puppet_dolls.h"
#include "dsh/net/packet.h"
#include "dsh/net/session.h"
#include "dsh/net/transport.h"
#include "dsh/platform/log.h"
#include "dsh/reflect/gamethread.h"
#include "dsh/reflect/enemies.h"
#include "dsh/reflect/target_model.h"
#include "dsh/reflect/gates.h"
#include "dsh/reflect/reflection.h"
#include "dsh/reflect/spawn.h"
#include "dsh/reflect/world.h"
#include "dsh/sync/cadence.h"
#include "dsh/sync/interpolator.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <algorithm>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace dsh::game {
namespace {

std::function<void()> g_sessionTickHook;

std::uint64_t g_recvRead = 0;
std::uint64_t g_recvDispatched = 0;
std::uint64_t g_recvApplied = 0;

std::uint64_t g_poseTicks = 0;
std::uint64_t g_poseSampled = 0;
std::uint64_t g_poseRepeated = 0;
std::uint64_t g_poseNoPose = 0;
std::uint64_t g_poseNoPeer = 0;
std::string   g_poseLastRefusal;

std::atomic<std::uint64_t> g_ammoCreated{0};
std::atomic<std::uint64_t> g_ammoRefused{0};
std::atomic<std::uint64_t> g_ammoRefusalLines{0};

std::atomic<std::uint64_t> g_ammoSpawnLines{0};

std::atomic<std::uint64_t> g_ammoHomingCleared{0};

std::atomic<std::uint64_t> g_ammoDispatchPushed{0};
std::atomic<std::uint64_t> g_ammoDispatchRan{0};

constexpr int kAmmoProbeSlots = 4;
struct AmmoCreatedSlot {
    std::uintptr_t actor = 0;
    std::uint64_t  ms = 0;
    float          locationCm[3] = {0.0f, 0.0f, 0.0f};
};
AmmoCreatedSlot       g_ammoCreatedSlot[kAmmoProbeSlots];
std::atomic<int>      g_ammoCreatedPublished{0};

constexpr std::uint64_t kAmmoProbeDelayMs = 400;

bool        g_sessionStopRequested = false;
std::string g_sessionStopReason;

std::atomic<bool> g_sessionLive{false};

}
void set_session_tick_hook(std::function<void()> hook) { g_sessionTickHook = std::move(hook); }

bool session_running() { return g_sessionLive.load(std::memory_order_acquire); }

void request_session_stop(const std::string& reason) {
    g_sessionStopRequested = true;
    g_sessionStopReason = reason;
}

bool session_stop_requested() { return g_sessionStopRequested; }

std::string session_stop_reason() { return g_sessionStopReason; }

namespace {

dsh::net::Endpoint make_endpoint(const sockaddr_in& a) {
    dsh::net::Endpoint ep;
    ep.bytes.resize(sizeof(a));
    std::memcpy(ep.bytes.data(), &a, sizeof(a));
    return ep;
}

std::uint32_t manifest_id_of(const std::vector<std::uint8_t>& manifest) {
    std::uint32_t id = 2166136261u;
    for (std::uint8_t b : manifest) {
        id ^= b;
        id *= 16777619u;
    }
    return id;
}

std::uint64_t steady_ms() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point epoch = clock::now();
    const auto since = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - epoch).count();
    return static_cast<std::uint64_t>(since) / 1000;
}

inline constexpr std::uint64_t kStateIntervalMs = 50;

inline constexpr std::uint64_t kPoseApplyIntervalMs = 16;

inline constexpr std::uint64_t kInterpDelayMs = 100;

inline constexpr std::uint32_t kPoseRepeatMaxStreak =
    static_cast<std::uint32_t>(1000 / kStateIntervalMs);

inline constexpr int kPlacementTimeoutMs = 250;

inline constexpr int kMovementTicksPerSample = 13;

inline constexpr float kIntegrationMoveThresholdCm = 5.0f;

bool same_velocity3(const float a[3], const float b[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        if (!(std::fabs(a[i] - b[i]) < 1.0f)) return false;
    }
    return true;
}

bool sample_local_pawn(dsh::game::PawnHandle& pawn, dsh::sync::PoseSample& pose, std::string& note,
                       std::uint64_t nowMs, std::uint64_t& nextLocateMs) {
    if (pawn.valid && dsh::game::read_pawn_pose(pawn, pose, note)) return true;

    if (pawn.valid) {
        DSH_WARN("puppetsession: the cached pawn handle stopped reading (%s) -- re-locating it",
                 note.c_str());
        pawn = dsh::game::PawnHandle{};
    }
    if (nowMs < nextLocateMs) return false;
    nextLocateMs = nowMs + 500;

    pawn = dsh::game::locate_local_pawn();
    if (!pawn.valid) {
        note = pawn.note;
        return false;
    }
    if (!dsh::game::read_pawn_pose(pawn, pose, note)) {
        pawn = dsh::game::PawnHandle{};
        return false;
    }
    return true;
}

struct SprintCapture {
    bool                            called = false;
    bool                            refused = false;
    bool                            asked = false;
    bool                            byteBeforeSetSprint = false;
    bool                            byteBeforeSetSprintKnown = false;
    bool                            byteAfter = false;
    bool                            byteAfterKnown = false;
    dsh::reflect::CharacterDerivedFlags flags;
    bool                            flagsSampled = false;
    std::string                     note;
};

struct MovementFlagEdges {
    std::uint64_t sprintUp = 0;
    std::uint64_t sprintDown = 0;
    std::uint64_t hoverUp = 0;
    std::uint64_t hoverDown = 0;
    std::uint64_t boostJumpUp = 0;
    std::uint64_t boostJumpDown = 0;
    std::uint64_t boostDashUp = 0;
    std::uint64_t boostDashDown = 0;
    bool          havePrevious = false;
};

std::string sprint_call_line(const SprintCapture& sprint, std::uint64_t sessionCalls) {
    if (!sprint.called) {
        if (sprint.refused) {
            return "sprintCall=SetSprint REFUSED -- " + sprint.note;
        }

        return "sprintCall=SetSprint NOT made on this dispatch (the write on it left the puppet's "
               "sprint byte as it found it), and this dispatch made call 0 of the " +
               std::to_string(sessionCalls) +
               " call(s) the SESSION totals line account for -- if that number is 0 then no dispatch "
               "this session ever reached the gate either";
    }
    const std::string getterText =
        sprint.flagsSampled
            ? ("getters IsSprint=" + std::to_string(sprint.flags.sprint ? 1 : 0) + " IsSprinting=" +
               std::to_string(sprint.flags.sprinting ? 1 : 0) + " IsHover=" +
               std::to_string(sprint.flags.hover ? 1 : 0) + " IsHovering=" +
               std::to_string(sprint.flags.hovering ? 1 : 0))
            : ("getters=NOT sampled" +
               (sprint.note.empty() ? std::string() : (" (" + sprint.note + ")")));
    std::string verdict;
    if (sprint.flagsSampled && sprint.flags.sprinting == sprint.asked) {
        verdict = " -- THE DERIVED GETTER AGREED WITH THE CALL";
    } else if (sprint.byteAfterKnown && sprint.byteAfter == sprint.asked) {
        verdict = " -- the byte holds the value asked for and IsSprinting did NOT follow it";
    } else if (sprint.byteAfterKnown) {
        verdict = " -- and the byte did not hold the value asked for either";
    }

    return "sprintCall=SetSprint called on the last sprint-byte change(asked=" +
           std::to_string(sprint.asked ? 1 : 0) +
           ", byte before=" +
           (sprint.byteBeforeSetSprintKnown ? std::to_string(sprint.byteBeforeSetSprint ? 1 : 0)
                                            : std::string("NOT read")) +
           ", byte after=" + (sprint.byteAfterKnown ? std::to_string(sprint.byteAfter ? 1 : 0)
                                                    : std::string("NOT read")) +
           "), " + getterText + verdict;
}

std::string sprint_call_counts_line(std::uint64_t calls, std::uint64_t refused, std::uint64_t matched,
                                    std::uint64_t reverted, std::uint64_t rawAgreed,
                                    std::uint64_t derivedAgreed) {
    if (calls == 0 && refused == 0) {
        return "sprintCall SESSION totals (NOT this window): NO ATTEMPT YET -- no dispatch's write has "
               "ever changed the puppet's sprint byte, so none of these counters is a reading of the "
               "game's own SetSprint (0 calls, 0 refused)";
    }
    const std::string counters =
        "sprintCall SESSION totals (NOT this window): calls=" + std::to_string(calls) + " refused=" +
        std::to_string(refused) + " byteMatched=" + std::to_string(matched) + "/" +
        std::to_string(calls) + " byteReverted=" + std::to_string(reverted) + "/" +
        std::to_string(calls) + " IsSprint-agreed=" + std::to_string(rawAgreed) + "/" +
        std::to_string(calls) + " IsSprinting-agreed=" + std::to_string(derivedAgreed) + "/" +
        std::to_string(calls);

    if (calls == 0) {
        return counters +
               " -- VERDICT: NO CALL WAS EVER MADE (every attempt was refused), so the refused= count "
               "above is the whole result";
    }
    if (matched == 0) {
        return counters +
               " -- VERDICT: the byte did NOT hold what was asked (byteReverted) -- the game's own "
               "setter is not a plain write of. (`byteReverted` also counts a call whose read "
               "back failed; that case names itself on the two-second line above)";
    }
    if (derivedAgreed == 0) {
        return counters +
               " -- VERDICT: the byte held what was asked and IsSprinting did NOT follow it -- a mere "
               "byte writer, so the derived state is driven elsewhere";
    }
    if (derivedAgreed == calls && rawAgreed == calls) {
        return counters +
               " -- VERDICT: the byte held what was asked AND IsSprinting followed it -- the game's own "
               "verb reaches the derived state: this is the door";
    }
    return counters + " -- VERDICT: of the " + std::to_string(matched) +
           " call(s) the byte held for, IsSprinting followed it on " + std::to_string(derivedAgreed) +
           " -- the verb reaches the derived state on some calls and not on others";
}

constexpr std::uintptr_t kMachineHumanoidOffset = 0x15E0;

struct ModuleSlotRef {
    std::uintptr_t offset;
    const char*    name;
};
constexpr ModuleSlotRef kModuleSlots[] = {
    {0x15D8, "Mobility"},       {0x15E0, "Humanoid"},
    {0x15E8, "Headset"},        {0x15F0, "Repeater"},
    {0x15F8, "Thruster"},       {0x1600, "Utility"},
    {0x1608, "Hanger"},         {0x1610, "LeftRear"},
    {0x1618, "RightRear"},      {0x1620, "PrimaryFront"},
    {0x1628, "SecondaryFront"}, {0x1630, "CurrentFront"},
};
constexpr std::size_t kModuleSlotCount = sizeof(kModuleSlots) / sizeof(kModuleSlots[0]);

constexpr std::uintptr_t kActorOwnerOffset          = 0x00E0;
constexpr std::uintptr_t kActorChildrenOffset       = 0x0120;
constexpr std::uintptr_t kActorRootOffset           = 0x0130;
constexpr std::uintptr_t kSceneAttachParentOffset   = 0x00C0;
constexpr std::uintptr_t kSceneAttachChildrenOffset = 0x00D0;
constexpr std::uintptr_t kSceneFlagsOffset          = 0x014C;
constexpr std::uintptr_t kSceneMobilityOffset       = 0x014F;

constexpr std::uintptr_t kSceneRelativeLocationOffset = 0x011C;
constexpr unsigned char  kSceneAbsoluteLocationBit  = 0x04;
constexpr unsigned char  kSceneAbsoluteRotationBit  = 0x08;
constexpr unsigned char  kSceneAbsoluteScaleBit     = 0x10;

constexpr std::uintptr_t kSceneHiddenFlagsOffset      = 0x014D;
constexpr std::uintptr_t kSkinnedRenderFlagsOffset    = 0x0607;
constexpr unsigned char  kSceneComponentToWorldBit    = 0x01;
constexpr unsigned char  kSceneHiddenInGameBit        = 0x04;
constexpr unsigned char  kSkinnedRecentlyRenderedBit  = 0x40;

constexpr double kStuckPartThresholdCm = 300.0;

constexpr double kStuckPartDriftCm = 300.0;
constexpr double kStuckPartMovedCm = 300.0;

std::size_t read_pointer_array(std::uintptr_t arrayAddress, std::size_t cap,
                               std::vector<std::uintptr_t>& out) {
    out.clear();
    if (arrayAddress < 0x10000) return 0;
    unsigned char header[0x10] = {};
    if (!dsh::reflect::read_process_bytes(arrayAddress, header, sizeof(header))) return 0;
    std::uintptr_t data = 0;
    std::int32_t   number = 0;
    std::memcpy(&data, header, sizeof(data));
    std::memcpy(&number, header + 0x08, sizeof(number));
    if (data < 0x10000 || number <= 0) return 0;
    std::size_t count = static_cast<std::size_t>(number);
    if (count > cap) count = cap;
    out.resize(count);
    if (!dsh::reflect::read_process_bytes(data, out.data(), count * sizeof(std::uintptr_t))) {
        out.clear();
        return 0;
    }
    return count;
}

double distance_cm(const float a[3], const float b[3]) {
    const double dx = static_cast<double>(a[0]) - static_cast<double>(b[0]);
    const double dy = static_cast<double>(a[1]) - static_cast<double>(b[1]);
    const double dz = static_cast<double>(a[2]) - static_cast<double>(b[2]);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct AttachChain {
    bool           rootRead = false;
    bool           reachesMachine = false;
    int            hops = 0;
    std::uintptr_t attachParent = 0;
    std::uintptr_t top = 0;
    bool           absoluteLocation = false;
    bool           absoluteRotation = false;
    bool           absoluteScale = false;
    int            mobility = -1;
};

AttachChain walk_attach_chain(std::uintptr_t root, std::uintptr_t machineRoot) {
    AttachChain chain;
    if (root < 0x10000) return chain;
    chain.rootRead = true;
    chain.top = root;
    std::uintptr_t cursor = root;
    for (int hop = 0; hop < 16; ++hop) {
        std::uint64_t parent = 0;
        if (!dsh::reflect::read_process_bytes(cursor + kSceneAttachParentOffset, &parent, sizeof(parent))) {
            break;
        }
        const auto next = static_cast<std::uintptr_t>(parent);
        if (next < 0x10000) {
            chain.top = 0;
            break;
        }
        ++chain.hops;
        chain.top = next;
        if (hop == 0) chain.attachParent = next;
        cursor = next;
        if (machineRoot >= 0x10000 && cursor == machineRoot) {
            chain.reachesMachine = true;
            break;
        }
    }
    unsigned char flags = 0;
    if (dsh::reflect::read_process_bytes(root + kSceneFlagsOffset, &flags, sizeof(flags))) {
        chain.absoluteLocation = (flags & kSceneAbsoluteLocationBit) != 0;
        chain.absoluteRotation = (flags & kSceneAbsoluteRotationBit) != 0;
        chain.absoluteScale    = (flags & kSceneAbsoluteScaleBit) != 0;
    }
    unsigned char mobility = 0;
    if (dsh::reflect::read_process_bytes(root + kSceneMobilityOffset, &mobility, sizeof(mobility))) {
        chain.mobility = static_cast<int>(mobility);
    }
    return chain;
}

const char* mobility_word(int mobility) {
    switch (mobility) {
        case 0: return "Static";
        case 1: return "Stationary";
        case 2: return "Movable";
        default: return "NOT read";
    }
}

bool looks_transient_actor(const std::string& className) {
    static const char* const kWords[] = {"Ammo", "GC_", "Effect", "Niagara", "Trail", "Decal", "Sound", "Cue"};
    for (const char* word : kWords) {
        if (className.find(word) != std::string::npos) return true;
    }
    return false;
}

bool looks_like_a_part_class(const std::string& className) {
    static const char* const kWords[] = {"Module", "Sheath", "FWM", "RWM"};
    for (const char* word : kWords) {
        if (className.find(word) != std::string::npos) return true;
    }
    return false;
}

bool looks_like_a_capture_component(const std::string& className) {
    return className.find("Camera") != std::string::npos ||
           className.find("Capture") != std::string::npos;
}

struct CensusEntry {
    std::uintptr_t component = 0;
    std::uintptr_t owner = 0;
    std::size_t    doll = 0;
    std::string    ownerSlot;
    std::string    className;
    std::string    ownName;
    bool           scene = false;

    double         lastDistanceCm = -1.0;
    float          lastMachineCm[3] = {0.0f, 0.0f, 0.0f};
    bool           haveLast = false;
};
struct ComponentCensus {
    std::vector<CensusEntry>    entry;
    std::vector<std::uintptr_t> machine;
    std::uint64_t               builds = 0;
};
struct CensusContext {
    std::vector<std::uintptr_t> machines;
    std::vector<std::uintptr_t> partners;
    std::vector<std::size_t>    partnerDoll;
    std::vector<std::string>    partnerSlot;
    ComponentCensus*            out = nullptr;
};

void census_on_object(void* user, std::uintptr_t object, std::uintptr_t outer) {
    auto* ctx = static_cast<CensusContext*>(user);
    if (ctx == nullptr || ctx->out == nullptr || object < 0x10000 || outer < 0x10000) return;
    CensusEntry entry;
    entry.component = object;
    entry.owner = outer;
    for (std::size_t i = 0; i < ctx->machines.size(); ++i) {
        if (ctx->machines[i] == outer) {
            entry.doll = i;
            entry.ownerSlot = "the machine";
            break;
        }
    }
    if (entry.ownerSlot.empty()) {
        for (std::size_t i = 0; i < ctx->partners.size(); ++i) {
            if (ctx->partners[i] == outer) {
                entry.doll = ctx->partnerDoll[i];
                entry.ownerSlot = ctx->partnerSlot[i];
                break;
            }
        }
    }
    if (entry.ownerSlot.empty()) return;
    entry.className = dsh::reflect::object_class_name(object);
    entry.ownName = dsh::reflect::object_own_name(object);
    ctx->out->entry.push_back(std::move(entry));
}

struct MeshCopyContext {
    std::uintptr_t              actorClass = 0;
    std::vector<std::uintptr_t> assets;

    std::vector<std::uintptr_t> known;

    std::vector<std::uintptr_t> dollActor;
    std::vector<std::size_t>    dollActorDoll;
    std::vector<float>          dollCm;
    std::vector<std::string>    dollLabel;
    std::uint64_t*              lines = nullptr;
    std::size_t                 found = 0;
    std::size_t                 unseenOnADoll = 0;
    std::size_t                 skippedDefaultObjects = 0;
    std::string                 label;
    std::size_t                 thisDoll = 0;
};

void mesh_copy_on_object(void* user, std::uintptr_t object, std::uintptr_t outer) {
    auto* ctx = static_cast<MeshCopyContext*>(user);
    if (ctx == nullptr || object < 0x10000 || outer < 0x10000) return;
    if (dsh::reflect::object_class_name(object).find("SkeletalMeshComponent") == std::string::npos) return;
    std::uint64_t assetRaw = 0;
    if (!dsh::reflect::read_process_bytes(object + 0x0480, &assetRaw, sizeof(assetRaw)) ||
        assetRaw < 0x10000) {
        return;
    }
    const auto asset = static_cast<std::uintptr_t>(assetRaw);
    bool wanted = false;
    for (const auto candidate : ctx->assets) {
        if (candidate == asset) wanted = true;
    }
    if (!wanted) return;
    if (std::find(ctx->known.begin(), ctx->known.end(), object) != ctx->known.end()) return;
    if (!dsh::game::actor_class_chain_reaches(outer, ctx->actorClass, &dsh::reflect::read_process_bytes)) return;

    if (dsh::reflect::object_own_name(outer).rfind("Default__", 0) == 0) {
        ++ctx->skippedDefaultObjects;
        return;
    }

    std::size_t ownerDoll = ctx->dollActor.size();
    for (std::size_t i = 0; i < ctx->dollActor.size(); ++i) {
        if (ctx->dollActor[i] == outer) {
            ownerDoll = ctx->dollActorDoll[i];
            break;
        }
    }

    dsh::reflect::ComponentPlacement comp;
    std::string                    compNote;
    const bool compRead = dsh::reflect::read_component_location(object, comp, compNote);
    dsh::reflect::ActorPlacement   ownerPlace;
    std::string                    ownerNote;
    const bool ownerRead = dsh::reflect::read_actor_placement(outer, ownerPlace, ownerNote);
    ++ctx->found;
    if (ownerDoll < ctx->dollActor.size()) ++ctx->unseenOnADoll;

    char where[192] = {};
    if (compRead) {
        std::size_t nearest = ctx->thisDoll;
        double nearestCm = -1.0;
        for (std::size_t d = 0; d * 3 + 2 < ctx->dollCm.size(); ++d) {
            const double cm = distance_cm(comp.locationCm, &ctx->dollCm[d * 3]);
            if (nearestCm < 0.0 || cm < nearestCm) {
                nearestCm = cm;
                nearest = d;
            }
        }
        std::snprintf(where, sizeof(where), "at (%.1f, %.1f, %.1f) cm, %.1f cm from %s",
                      static_cast<double>(comp.locationCm[0]), static_cast<double>(comp.locationCm[1]),
                      static_cast<double>(comp.locationCm[2]), nearestCm,
                      nearest < ctx->dollLabel.size() ? ctx->dollLabel[nearest].c_str() : "the doll");
    } else {
        std::snprintf(where, sizeof(where), "NOT locatable (%s)", compNote.c_str());
    }
    char heldBy[160] = {};
    if (ownerRead) {
        std::snprintf(heldBy, sizeof(heldBy), "its actor '%s' [%s] is at (%.1f, %.1f, %.1f) cm",
                      dsh::reflect::object_own_name(outer).c_str(),
                      dsh::reflect::object_class_name(outer).c_str(),
                      static_cast<double>(ownerPlace.locationCm[0]),
                      static_cast<double>(ownerPlace.locationCm[1]),
                      static_cast<double>(ownerPlace.locationCm[2]));
    } else {
        std::snprintf(heldBy, sizeof(heldBy), "its actor '%s' [%s] could not be located (%s)",
                      dsh::reflect::object_own_name(outer).c_str(),
                      dsh::reflect::object_class_name(outer).c_str(), ownerNote.c_str());
    }

    if (ctx->lines == nullptr || *ctx->lines >= 120) return;
    ++(*ctx->lines);
    DSH_INFO("puppetsession: MESH COPY[%s]: asset '%s' is rendered by the component '%s' [%s], which is %s; "
             "the component is %s; %s",
             ctx->label.c_str(), dsh::reflect::object_own_name(asset).c_str(),
             dsh::reflect::object_own_name(object).c_str(),
             dsh::reflect::object_class_name(object).c_str(),
             ownerDoll < ctx->dollActor.size()
                 ? "ON ONE OF THE DOLLS' OWN ACTORS AND NOT IN THE CENSUS -- a component built after the census"
                 : "on an actor that is NOT any watched doll",
             where, heldBy);
}

struct ScreenRow {
    std::uintptr_t component = 0;
    std::uintptr_t owner = 0;
    std::size_t    doll = 0;
    bool           onADollActor = false;
    bool           unseenOnADoll = false;
    bool           hiddenInGame = false;
    bool           hiddenRead = false;
    bool           recentlyRendered = false;
    bool           recentRead = false;
    bool           onScreen = false;
    float          locationCm[3] = {0.0f, 0.0f, 0.0f};
    float          screenX = 0.0f;
    float          screenY = 0.0f;
    double         distanceCm = 0.0;
    std::string    className;
    std::string    ownName;
    std::string    ownerClass;
    std::string    assetName;
};

struct ScreenContext {
    std::uintptr_t              dollActorClass = 0;
    std::vector<std::uintptr_t> dollActor;
    std::vector<std::size_t>    dollActorDoll;
    std::vector<std::uintptr_t> known;
    std::vector<float>          dollCm;
    std::vector<std::string>    dollLabel;
    std::uintptr_t              playerController = 0;
    std::vector<ScreenRow>      rows;
    std::size_t                 meshComponents = 0;
    std::size_t                 kept = 0;
};

void screen_on_object(void* user, std::uintptr_t object, std::uintptr_t outer) {
    auto* ctx = static_cast<ScreenContext*>(user);
    if (ctx == nullptr || object < 0x10000 || outer < 0x10000) return;
    const std::string className = dsh::reflect::object_class_name(object);
    const bool skeletal = className.find("SkeletalMeshComponent") != std::string::npos;
    const bool statik = className.find("StaticMeshComponent") != std::string::npos;
    if (!skeletal && !statik) return;
    if (!dsh::game::actor_class_chain_reaches(outer, ctx->dollActorClass,
                                              &dsh::reflect::read_process_bytes)) {
        return;
    }
    ++ctx->meshComponents;
    dsh::reflect::ComponentPlacement place;
    std::string                    note;
    if (!dsh::reflect::read_component_location(object, place, note)) return;

    std::size_t onDoll = ctx->dollActor.size();
    for (std::size_t i = 0; i < ctx->dollActor.size(); ++i) {
        if (ctx->dollActor[i] == outer) {
            onDoll = ctx->dollActorDoll[i];
            break;
        }
    }
    std::size_t nearest = 0;
    double      nearestCm = -1.0;
    for (std::size_t d = 0; d * 3 + 2 < ctx->dollCm.size(); ++d) {
        const double cm = distance_cm(place.locationCm, &ctx->dollCm[d * 3]);
        if (nearestCm < 0.0 || cm < nearestCm) {
            nearestCm = cm;
            nearest = d;
        }
    }
    const bool onADollActor = onDoll < ctx->dollActor.size();
    if (!onADollActor && nearestCm > 5000.0) return;
    ++ctx->kept;

    ScreenRow row;
    row.component = object;
    row.owner = outer;
    row.doll = onADollActor ? onDoll : nearest;
    row.onADollActor = onADollActor;
    row.unseenOnADoll =
        onADollActor && std::find(ctx->known.begin(), ctx->known.end(), object) == ctx->known.end();
    row.distanceCm = onADollActor ? distance_cm(place.locationCm, &ctx->dollCm[onDoll * 3]) : nearestCm;
    for (int i = 0; i < 3; ++i) row.locationCm[i] = place.locationCm[i];
    row.className = className;
    row.ownName = dsh::reflect::object_own_name(object);
    row.ownerClass = dsh::reflect::object_class_name(outer);
    if (skeletal) {
        std::uint64_t assetRaw = 0;
        if (dsh::reflect::read_process_bytes(object + 0x0480, &assetRaw, sizeof(assetRaw)) &&
            assetRaw >= 0x10000) {
            row.assetName = dsh::reflect::object_own_name(static_cast<std::uintptr_t>(assetRaw));
        }
    }

    unsigned char hidden = 0;
    if (dsh::reflect::read_process_bytes(object + kSceneHiddenFlagsOffset, &hidden, sizeof(hidden))) {
        row.hiddenRead = true;
        row.hiddenInGame = (hidden & kSceneHiddenInGameBit) != 0;
    }
    if (skeletal) {
        unsigned char render = 0;
        if (dsh::reflect::read_process_bytes(object + kSkinnedRenderFlagsOffset, &render, sizeof(render))) {
            row.recentRead = true;
            row.recentlyRendered = (render & kSkinnedRecentlyRenderedBit) != 0;
        }
    }
    ctx->rows.push_back(std::move(row));
}

std::string screen_row_text(const ScreenRow& row, const std::vector<std::string>& dollLabel) {
    const std::string dollName =
        row.doll < dollLabel.size() ? dollLabel[row.doll] : std::string("a doll");
    const std::string ownerName = dsh::reflect::object_own_name(row.owner);
    const std::string componentName = row.ownName.empty() ? std::string("<unnamed>") : row.ownName;
    std::string assetWord;
    if (!row.assetName.empty()) assetWord = " asset=" + row.assetName;
    char text[512] = {};
    char screen[96] = {};
    if (row.onScreen) {
        std::snprintf(screen, sizeof(screen), "screen (%.0f, %.0f)", static_cast<double>(row.screenX),
                      static_cast<double>(row.screenY));
    } else {
        std::snprintf(screen, sizeof(screen), "OFF the picture");
    }
    std::snprintf(text, sizeof(text),
                  "'%s' [%s]%s on '%s' [%s] at (%.1f, %.1f, %.1f) cm, %.1f cm from %s, %s, hid=%d recent=%d",
                  componentName.c_str(), row.className.c_str(), assetWord.c_str(),
                  ownerName.empty() ? "<unnamed>" : ownerName.c_str(), row.ownerClass.c_str(),
                  static_cast<double>(row.locationCm[0]), static_cast<double>(row.locationCm[1]),
                  static_cast<double>(row.locationCm[2]), row.distanceCm, dollName.c_str(), screen,
                  row.hiddenRead ? (row.hiddenInGame ? 1 : 0) : -1,
                  row.recentRead ? (row.recentlyRendered ? 1 : 0) : -1);
    return std::string(text);
}

void print_screen_table(ScreenContext& ctx, const std::string& label, std::uint64_t& lineBudget) {
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::string  viewportNote;
    const bool   haveViewport =
        dsh::reflect::read_viewport_size(ctx.playerController, width, height, viewportNote);
    for (auto& row : ctx.rows) {
        std::string note;
        row.onScreen = dsh::reflect::project_world_to_screen(ctx.playerController, row.locationCm, row.screenX,
                                                             row.screenY, note);
    }
    std::sort(ctx.rows.begin(), ctx.rows.end(), [](const ScreenRow& a, const ScreenRow& b) {
        return a.distanceCm < b.distanceCm;
    });
    std::size_t onScreen = 0;
    std::size_t onDoll = 0;
    std::size_t unseen = 0;
    std::size_t offDollOnScreen = 0;
    for (const auto& row : ctx.rows) {
        if (row.onScreen) ++onScreen;
        if (row.onADollActor) ++onDoll;
        if (row.unseenOnADoll) ++unseen;
        if (!row.onADollActor && row.onScreen) ++offDollOnScreen;
    }
    std::string viewportText;
    if (haveViewport) {
        viewportText = std::to_string(width) + "x" + std::to_string(height) + " pixels";
    } else {
        viewportText = "viewport NOT read: " + viewportNote;
    }
    DSH_INFO("puppetsession: parts[%s] SCREEN: %s; %zu mesh component(s) in the process on an actor, %zu kept "
             "(on a doll, or within 50 m of one), %zu of them project into the picture, %zu of those are NOT on "
             "a doll; %zu component(s) live on a doll's actor that the census never saw",
             label.c_str(), viewportText.c_str(), ctx.meshComponents, ctx.kept, onScreen, offDollOnScreen,
             unseen);

    std::size_t printedDoll = 0;
    for (const auto& row : ctx.rows) {
        if (!row.onADollActor) continue;
        if (lineBudget >= 160) break;
        if (printedDoll >= 24) break;
        ++printedDoll;
        ++lineBudget;
        DSH_INFO("puppetsession: parts[%s] screen ON A DOLL%s: %s", label.c_str(),
                 row.unseenOnADoll ? " (NOT IN THE CENSUS)" : "", screen_row_text(row, ctx.dollLabel).c_str());
    }

    std::size_t printedOther = 0;
    for (const auto& row : ctx.rows) {
        if (row.onADollActor || !row.onScreen) continue;
        if (lineBudget >= 160) break;
        if (printedOther >= 40) break;
        ++printedOther;
        ++lineBudget;
        DSH_INFO("puppetsession: parts[%s] screen OFF A DOLL: %s", label.c_str(),
                 screen_row_text(row, ctx.dollLabel).c_str());
    }
    DSH_INFO("puppetsession: parts[%s] screen: %zu of the doll's own primitive(s) printed, %zu other "
             "primitive(s) printed (%zu were on the picture and are not shown if this number is bigger)",
             label.c_str(), printedDoll, printedOther, offDollOnScreen);
}

constexpr std::uintptr_t kActorMeshOffset                = 0x0280;
constexpr std::uintptr_t kHumanoidLegMeshOffset          = 0x12D0;
constexpr std::uintptr_t kHumanoidFaceMeshOffset         = 0x12D8;
constexpr std::uintptr_t kHumanoidFrontHairMeshOffset    = 0x12E0;
constexpr std::uintptr_t kHumanoidBackHairMeshOffset     = 0x12E8;

constexpr std::uintptr_t kPrimitiveSimulatePhysicsOffset = 0x02C8 + 0x0010;

struct ComponentState {
    bool           registered = false;
    bool           locationRead = false;
    float          locationCm[3] = {0.0f, 0.0f, 0.0f};
    bool           attachRead = false;
    std::uintptr_t attachParent = 0;
    std::string    attachClass;
    std::string    attachName;
    bool           flagsRead = false;
    bool           absoluteLocation = false;
    bool           absoluteRotation = false;
    bool           absoluteScale = false;
    bool           visible = false;

    bool           componentToWorldUpdated = false;
    bool           hiddenInGameRead = false;
    bool           hiddenInGame = false;
    bool           recentlyRenderedRead = false;
    bool           recentlyRendered = false;
    bool           renderFlagsRead = false;
    bool           renderStatic = false;
    bool           animFlagsRead = false;
    bool           noSkeletonUpdate = false;
    bool           pauseAnims = false;

    bool           skinnedStateRead = false;
    int            animTickOption = -1;
    bool           boundsFromMasterPose = false;
    bool           fixedSkelBounds = false;
    bool           syncAttachParentLOD = false;
    bool           cpuSkinning = false;
    std::uint32_t  masterPoseIndex = 0;

    std::uint32_t  masterPoseSerial = 0;
    bool           masterPoseResolvedRead = false;
    std::uintptr_t masterPose = 0;
    std::string    masterPoseClass;
    std::string    masterPoseName;
    std::string    masterPoseNote;
    int            mobility = -1;
    bool           simulateRead = false;
    bool           simulatePhysics = false;
    std::string    className;
    std::string    ownName;
};

ComponentState component_state(std::uintptr_t component, std::uintptr_t primitiveClass) {
    ComponentState state;
    if (component < 0x10000) return state;
    std::string why;
    if (!dsh::reflect::object_is_still_registered(component, why)) return state;
    state.registered = true;
    state.className = dsh::reflect::object_class_name(component);
    state.ownName = dsh::reflect::object_own_name(component);
    dsh::reflect::ComponentPlacement place;
    std::string placeNote;
    if (dsh::reflect::read_component_location(component, place, placeNote)) {
        state.locationRead = true;
        for (int i = 0; i < 3; ++i) state.locationCm[i] = place.locationCm[i];
    }
    std::uint64_t parent = 0;
    if (dsh::reflect::read_process_bytes(component + kSceneAttachParentOffset, &parent, sizeof(parent))) {
        state.attachRead = true;
        state.attachParent = static_cast<std::uintptr_t>(parent);
        if (state.attachParent >= 0x10000) {
            state.attachClass = dsh::reflect::object_class_name(state.attachParent);
            state.attachName = dsh::reflect::object_own_name(state.attachParent);
        }
    }
    unsigned char flags = 0;
    if (dsh::reflect::read_process_bytes(component + kSceneFlagsOffset, &flags, sizeof(flags))) {
        state.flagsRead = true;
        state.absoluteLocation = (flags & kSceneAbsoluteLocationBit) != 0;
        state.absoluteRotation = (flags & kSceneAbsoluteRotationBit) != 0;
        state.absoluteScale = (flags & kSceneAbsoluteScaleBit) != 0;

        state.visible = (flags & 0x20) != 0;
        state.componentToWorldUpdated = (flags & kSceneComponentToWorldBit) != 0;
    }

    unsigned char hiddenFlags = 0;
    if (dsh::reflect::read_process_bytes(component + kSceneHiddenFlagsOffset, &hiddenFlags,
                                        sizeof(hiddenFlags))) {
        state.hiddenInGameRead = true;
        state.hiddenInGame = (hiddenFlags & kSceneHiddenInGameBit) != 0;
    }

    static std::uintptr_t skinnedClass = 0;
    if (skinnedClass == 0) {
        skinnedClass = dsh::reflect::resolve_uclass_address("SkinnedMeshComponent");
    }
    if (skinnedClass >= 0x10000 &&
        dsh::game::actor_class_chain_reaches(component, skinnedClass, &dsh::reflect::read_process_bytes)) {
        unsigned char render = 0;
        if (dsh::reflect::read_process_bytes(component + kSkinnedRenderFlagsOffset, &render,
                                             sizeof(render))) {
            state.recentlyRenderedRead = true;
            state.recentlyRendered = (render & kSkinnedRecentlyRenderedBit) != 0;
        }

        unsigned char renderFlags = 0;
        if (dsh::reflect::read_process_bytes(component + 0x0608, &renderFlags, sizeof(renderFlags))) {
            state.renderFlagsRead = true;
            state.renderStatic = (renderFlags & 0x10) != 0;
        }
        unsigned char animFlags = 0;
        if (dsh::reflect::read_process_bytes(component + 0x08C1, &animFlags, sizeof(animFlags))) {
            state.animFlagsRead = true;
            state.noSkeletonUpdate = (animFlags & 0x20) != 0;
            state.pauseAnims = (animFlags & 0x40) != 0;
        }

        unsigned char skinned604 = 0;
        unsigned char skinned606 = 0;
        unsigned char skinned607 = 0;
        unsigned char skinned608 = 0;
        if (dsh::reflect::read_process_bytes(component + 0x0604, &skinned604, sizeof(skinned604)) &&
            dsh::reflect::read_process_bytes(component + 0x0606, &skinned606, sizeof(skinned606)) &&
            dsh::reflect::read_process_bytes(component + 0x0607, &skinned607, sizeof(skinned607)) &&
            dsh::reflect::read_process_bytes(component + 0x0608, &skinned608, sizeof(skinned608))) {
            state.skinnedStateRead = true;
            state.animTickOption = static_cast<int>(skinned604);
            state.boundsFromMasterPose = (skinned606 & 0x10) != 0;
            state.fixedSkelBounds = (skinned607 & 0x04) != 0;
            state.syncAttachParentLOD = (skinned607 & 0x10) != 0;
            state.cpuSkinning = (skinned608 & 0x02) != 0;
        }

        std::uint32_t masterPair[2] = {0, 0};
        if (dsh::reflect::read_process_bytes(component + 0x0488, masterPair, sizeof(masterPair))) {
            state.masterPoseIndex = masterPair[0];
            state.masterPoseSerial = masterPair[1];
            const bool none = masterPair[0] == 0 || masterPair[0] == 0xFFFFFFFFu;
            if (!none) {
                std::string weakNote;
                if (dsh::reflect::object_from_weak_index(masterPair[0], masterPair[1], state.masterPose,
                                                         weakNote)) {
                    state.masterPoseResolvedRead = true;
                    state.masterPoseClass = dsh::reflect::object_class_name(state.masterPose);
                    state.masterPoseName = dsh::reflect::object_own_name(state.masterPose);
                } else {
                    state.masterPoseNote = weakNote;
                }
            }
        }
    }
    unsigned char mobility = 0;
    if (dsh::reflect::read_process_bytes(component + kSceneMobilityOffset, &mobility, sizeof(mobility))) {
        state.mobility = static_cast<int>(mobility);
    }
    if (primitiveClass >= 0x10000 &&
        dsh::game::actor_class_chain_reaches(component, primitiveClass, &dsh::reflect::read_process_bytes)) {
        unsigned char body = 0;
        if (dsh::reflect::read_process_bytes(component + kPrimitiveSimulatePhysicsOffset, &body, sizeof(body))) {
            state.simulateRead = true;
            state.simulatePhysics = (body & 0x01) != 0;
        }
    }
    return state;
}

std::string component_words(const ComponentState& state, double distanceCm) {
    std::string out;
    if (!state.registered) return "NOT a live object any more";
    out = "'" + (state.ownName.empty() ? state.className : state.ownName) + "' [" + state.className + "]";
    char number[64];
    if (state.locationRead) {
        std::snprintf(number, sizeof(number), " at (%.1f, %.1f, %.1f) cm", static_cast<double>(state.locationCm[0]),
                      static_cast<double>(state.locationCm[1]), static_cast<double>(state.locationCm[2]));
        out += number;
        if (distanceCm >= 0.0) {
            std::snprintf(number, sizeof(number), ", %.1f cm from the doll", distanceCm);
            out += number;
        }
    } else {
        out += ", location NOT read";
    }
    if (state.attachRead) {
        if (state.attachParent >= 0x10000) {
            out += ", attach '" + (state.attachName.empty() ? state.attachClass : state.attachName) + "' [" +
                   state.attachClass + "]";
        } else {
            out += ", attach NONE";
        }
    } else {
        out += ", attach NOT read";
    }
    if (state.flagsRead) {
        std::snprintf(number, sizeof(number), ", abs(%.0f/%.0f/%.0f)", state.absoluteLocation ? 1.0 : 0.0,
                      state.absoluteRotation ? 1.0 : 0.0, state.absoluteScale ? 1.0 : 0.0);
        out += number;
    }

    out += state.visible ? ", vis=1" : ", vis=0";
    if (state.hiddenInGameRead) {
        out += state.hiddenInGame ? ", hid=1" : ", hid=0";
    } else {
        out += ", hid=?";
    }
    if (state.flagsRead) {
        out += state.componentToWorldUpdated ? ", rto=1" : ", rto=0";
    }
    if (state.recentlyRenderedRead) {
        out += state.recentlyRendered ? ", recent=1" : ", recent=0";
    }
    if (state.renderFlagsRead && state.renderStatic) out += ", RENDER-STATIC";
    if (state.animFlagsRead && state.noSkeletonUpdate) out += ", NO-SKELETON-UPDATE";
    if (state.animFlagsRead && state.pauseAnims) out += ", PAUSE-ANIMS";

    if (state.skinnedStateRead) {
        char skinned[64] = {};
        std::snprintf(skinned, sizeof(skinned), ", tick=%d%s%s%s%s", state.animTickOption,
                      state.cpuSkinning ? ", CPU-SKINNING" : "", state.fixedSkelBounds ? ", FIXED-SKEL-BOUNDS" : "",
                      state.syncAttachParentLOD ? ", SYNC-PARENT-LOD" : "",
                      state.boundsFromMasterPose ? ", BOUNDS-FROM-MASTER" : "");
        out += skinned;
    }

    if (state.masterPoseIndex == 0 || state.masterPoseIndex == 0xFFFFFFFFu) {
        out += ", master pose NONE";
    } else if (state.masterPoseResolvedRead) {
        out += ", master pose '" + (state.masterPoseName.empty() ? state.masterPoseClass : state.masterPoseName) +
               "' [" + state.masterPoseClass + "]";
    } else {
        char master[96] = {};
        std::snprintf(master, sizeof(master), ", master pose idx%u: NOT resolvable (%s)",
                      static_cast<unsigned>(state.masterPoseIndex),
                      state.masterPoseNote.empty() ? "no reason recorded" : state.masterPoseNote.c_str());
        out += master;
    }
    out += ", mobility ";
    out += mobility_word(state.mobility);
    if (state.simulateRead) out += state.simulatePhysics ? ", SIMULATING PHYSICS" : ", not simulating";
    return out;
}

std::atomic<bool> g_partsDumpRequested{false};

std::atomic<bool> g_partsHideRequested{false};
std::atomic<bool> g_partsFixRequested{false};

constexpr double kRenderRunawayCm = 500.0;
constexpr double kRenderMovedCm = 2.0;
constexpr double kComponentMovedCm = 10.0;

constexpr std::uint32_t kComponentQueueIndexOffset = 0x0080;

constexpr std::uint32_t kPrimitiveSceneProxyOffset = 0x0430;
constexpr std::uint32_t kQueueNotQueued = 0xFFFFFFFFu;
constexpr std::uint64_t kQueueStuckMs = 5000;

constexpr double kRenderAgreedCm = 100.0;
enum QueueWhere { kQueueWhereUnknown = 0, kQueueWhereNotQueued, kQueueWhereTransform, kQueueWhereGameThread,
                  kQueueWhereLost };

struct QueueReading {
    std::uint32_t index = kQueueNotQueued;
    QueueWhere    where = kQueueWhereUnknown;
    bool          proven = false;

    int           numTransform = -1;
    int           numGameThread = -1;
};

bool g_queueOffsetsProven = false;

QueueReading queue_reading_of(std::uintptr_t component, std::uint32_t index) {
    QueueReading reading;
    reading.index = index;
    if (index == kQueueNotQueued) {
        reading.where = kQueueWhereNotQueued;
        reading.proven = g_queueOffsetsProven;
        return reading;
    }
    const std::uintptr_t world = dsh::reflect::live_world_address();
    if (world < 0x10000) return reading;
    const std::uint32_t offsets[2] = {
        dsh::reflect::world_offsets::kComponentsNeedEndOfFrameUpdate,
        dsh::reflect::world_offsets::kComponentsNeedEndOfFrameUpdateOnGameThread};
    for (int queue = 0; queue < 2; ++queue) {
        struct Table {
            std::uintptr_t data;
            std::int32_t   num;
            std::int32_t   max;
        } table = {};
        if (!dsh::reflect::read_process_bytes(world + offsets[queue], &table, sizeof(table))) continue;
        if (table.num < 0 || table.num > 200000) continue;
        if (queue == 0) {
            reading.numTransform = table.num;
        } else {
            reading.numGameThread = table.num;
        }
        if (table.data < 0x10000 || table.num == 0) continue;
        if (index >= static_cast<std::uint32_t>(table.num)) continue;
        std::uint64_t slot = 0;
        if (!dsh::reflect::read_process_bytes(table.data + static_cast<std::uint64_t>(index) * 8, &slot,
                                              sizeof(slot))) {
            continue;
        }
        if (slot != component) continue;
        g_queueOffsetsProven = true;
        reading.proven = true;
        reading.where = (queue == 0) ? kQueueWhereTransform : kQueueWhereGameThread;
        return reading;
    }
    reading.proven = g_queueOffsetsProven;
    if (g_queueOffsetsProven) reading.where = kQueueWhereLost;
    return reading;
}

const char* queue_where_word(QueueWhere where) {
    switch (where) {
        case kQueueWhereNotQueued:   return "not queued";
        case kQueueWhereTransform:   return "in the transform queue";
        case kQueueWhereGameThread:  return "in the GAME-THREAD queue";
        case kQueueWhereLost:        return "in NEITHER queue";
        default:                     return "queue UNVERIFIED";
    }
}

std::string queue_short_word(const QueueReading& reading) {
    if (reading.index == kQueueNotQueued) return "-1";
    char buf[48] = {};
    const char* tag = reading.where == kQueueWhereTransform     ? "transform"
                      : reading.where == kQueueWhereGameThread  ? "gamethread"
                      : reading.where == kQueueWhereLost        ? "neither"
                                                                : "?";
    std::snprintf(buf, sizeof(buf), "0x%X/%s", static_cast<unsigned>(reading.index), tag);
    return buf;
}

void queue_stuck_line(const std::string& label, const char* what, const std::string& name,
                      std::uintptr_t component, const QueueReading& reading, std::uint64_t heldSeconds,
                      const std::string& origin) {
    DSH_INFO("puppetsession: MARK STUCK[%s] %s '%s' @0x%llX: the queue index at component%03X has held "
             "0x%X for %llu s and the component is %s -- so every MarkRenderTransformDirty since then, and "
             "every `fix` recreate request, has been dropped silently (UWorld::"
             "MarkActorComponentForNeededEndOfFrameUpdate, LevelTick.cpp:899-946); the renderer keeps the "
             "last transform it was given; %s",
             label.c_str(), what, name.c_str(), static_cast<unsigned long long>(component),
             static_cast<unsigned>(kComponentQueueIndexOffset), static_cast<unsigned>(reading.index),
             static_cast<unsigned long long>(heldSeconds), queue_where_word(reading.where), origin.c_str());
}

std::string write_ring_note(std::uintptr_t component, std::uint32_t index, std::uint64_t nowMs);

constexpr std::uint64_t kMarkWatchPeriodMs = 250;
constexpr std::size_t   kMarkWatchMax = 48;

extern bool          g_frameGuardHigh;
extern std::uint32_t g_markEnterCount;
extern std::uint32_t g_markEnterWithGuard;

std::mutex g_writeRingMutex;

struct MarkWatchEntry {
    std::uintptr_t component = 0;
    std::string    what;
    std::uint32_t  index = kQueueNotQueued;
    std::uint64_t  sinceMs = 0;
    bool           sampled = false;
    bool           entered = false;

    std::uint32_t  pendingIndex = kQueueNotQueued;
    std::uint64_t  pendingMs = 0;

    bool           pendingGuard = false;
};

constexpr std::uint64_t kMarkWatchConfirmMs = 200;

std::vector<MarkWatchEntry> g_markWatch;
std::uint64_t g_markWatchNextMs = 0;

std::vector<std::uintptr_t> g_fixMidProbe;
std::uint64_t g_fixMidMs = 0;

void mark_watch_note(const char* what, std::uintptr_t component) {
    if (component < 0x10000 || what == nullptr) return;
    const std::lock_guard<std::mutex> lock(g_writeRingMutex);
    for (const auto& entry : g_markWatch) {
        if (entry.component == component) return;
    }
    if (g_markWatch.size() >= kMarkWatchMax) return;
    MarkWatchEntry entry;
    entry.component = component;
    entry.what = what;
    g_markWatch.push_back(std::move(entry));
}

void mark_watch_pump(std::uint64_t nowMs) {
    if (nowMs < g_markWatchNextMs) return;
    g_markWatchNextMs = nowMs + kMarkWatchPeriodMs;
    for (auto& entry : g_markWatch) {
        std::uint32_t index = kQueueNotQueued;
        if (!dsh::reflect::read_process_bytes(entry.component + kComponentQueueIndexOffset, &index,
                                              sizeof(index))) {
            continue;
        }
        if (!entry.sampled) {
            entry.sampled = true;
            entry.index = index;
            entry.sinceMs = nowMs;
            continue;
        }
        if (index != entry.index) {
            entry.index = index;
            entry.sinceMs = nowMs;
            entry.pendingIndex = kQueueNotQueued;
        }
        if (index == kQueueNotQueued) {
            entry.entered = false;
            entry.pendingIndex = kQueueNotQueued;
            continue;
        }
        if (entry.entered) continue;

        if (entry.pendingIndex != index) {
            entry.pendingIndex = index;
            entry.pendingMs = nowMs;
            entry.pendingGuard = g_frameGuardHigh;
            continue;
        }
        if (nowMs - entry.pendingMs < kMarkWatchConfirmMs) continue;
        const QueueReading reading = queue_reading_of(entry.component, index);
        if (reading.where != kQueueWhereLost) {
            entry.pendingIndex = kQueueNotQueued;
            continue;
        }

        if (reading.numTransform == 0 && reading.numGameThread == 0) {
            entry.pendingIndex = kQueueNotQueued;
            continue;
        }
        entry.entered = true;
        ++g_markEnterCount;
        if (entry.pendingGuard) ++g_markEnterWithGuard;
        DSH_INFO("puppetsession: MARK ENTER %s @0x%llX: the queue index has held 0x%X for %llu ms and the "
                 "component is in NEITHER queue (transform queue holds %d, game-thread queue holds %d) -- a "
                 "mark made in the world's drain window is cleared long before this line, so this one is the "
                 "fault: its transform updates stop at the moment the index first appeared. The frame-pass "
                 "guard was %s at that first sight; entries made with it high: %u of %u; %s",
                 entry.what.c_str(), static_cast<unsigned long long>(entry.component),
                 static_cast<unsigned>(index), static_cast<unsigned long long>(nowMs - entry.pendingMs),
                 reading.numTransform, reading.numGameThread, entry.pendingGuard ? "HIGH" : "low",
                 static_cast<unsigned>(g_markEnterWithGuard), static_cast<unsigned>(g_markEnterCount),
                 write_ring_note(entry.component, index, nowMs).c_str());
    }
}

constexpr std::uint32_t kFrameGuardScanBytes = 0x400;
constexpr std::uint64_t kFrameGuardScanMs = 20000;
constexpr int           kFrameGuardCandidates = 3;

std::uint8_t  g_frameGuardPrev[kFrameGuardScanBytes] = {};
std::uint8_t  g_frameGuardPrev2[kFrameGuardScanBytes] = {};
std::uint32_t g_frameGuardPulses[kFrameGuardScanBytes] = {};
std::uint32_t g_frameGuardToggles[kFrameGuardScanBytes] = {};
std::uint32_t g_frameGuardSamples = 0;
std::uint64_t g_frameGuardStartMs = 0;
int           g_frameGuardOffset = -1;
std::uint8_t  g_frameGuardBaseline = 0;
bool          g_frameGuardHigh = false;
std::uint32_t g_markEnterCount = 0;
std::uint32_t g_markEnterWithGuard = 0;

void frame_guard_sample(std::uint64_t nowMs) {
    const std::uintptr_t world = dsh::reflect::live_world_address();
    if (world < 0x10000) return;
    std::uint8_t bytes[kFrameGuardScanBytes] = {};
    if (!dsh::reflect::read_process_bytes(world, bytes, sizeof(bytes))) return;
    if (g_frameGuardStartMs == 0) {
        g_frameGuardStartMs = nowMs;
        std::memcpy(g_frameGuardPrev, bytes, sizeof(bytes));
        std::memcpy(g_frameGuardPrev2, bytes, sizeof(bytes));
        return;
    }
    for (std::uint32_t i = 0; i < kFrameGuardScanBytes; ++i) {
        if (bytes[i] != g_frameGuardPrev[i]) ++g_frameGuardToggles[i];

        if (bytes[i] == g_frameGuardPrev2[i] && g_frameGuardPrev[i] != g_frameGuardPrev2[i]) {
            ++g_frameGuardPulses[i];
        }
    }
    std::memcpy(g_frameGuardPrev2, g_frameGuardPrev, sizeof(bytes));
    std::memcpy(g_frameGuardPrev, bytes, sizeof(bytes));
    ++g_frameGuardSamples;
    const double seconds = static_cast<double>(nowMs - g_frameGuardStartMs) / 1000.0;
    if (g_frameGuardOffset < 0 && seconds >= static_cast<double>(kFrameGuardScanMs) / 1000.0 &&
        g_frameGuardSamples > 100) {
        int best[kFrameGuardCandidates] = {-1, -1, -1};
        for (std::uint32_t i = 0; i < kFrameGuardScanBytes; ++i) {
            const double rate = static_cast<double>(g_frameGuardToggles[i]) / seconds;
            if (rate < 20.0) continue;
            if (g_frameGuardPulses[i] * 4 < static_cast<std::uint32_t>(rate * seconds)) continue;
            for (int slot = 0; slot < kFrameGuardCandidates; ++slot) {
                if (best[slot] < 0) {
                    best[slot] = static_cast<int>(i);
                    break;
                }
            }
        }
        std::string list;
        for (int slot = 0; slot < kFrameGuardCandidates; ++slot) {
            if (best[slot] < 0) continue;
            const double rate = static_cast<double>(g_frameGuardToggles[best[slot]]) / seconds;
            char one[112] = {};
            std::snprintf(one, sizeof(one), "%sUWorld%03X (%.0f change(s)/s, %u single-sample pulse(s))",
                          list.empty() ? "" : ", ", static_cast<unsigned>(best[slot]), rate,
                          static_cast<unsigned>(g_frameGuardPulses[best[slot]]));
            list += one;
        }
        if (best[0] >= 0) {
            g_frameGuardOffset = best[0];
            DSH_INFO("puppetsession: frame guard: candidate byte(s) for the per-frame pass guard -- %s -- from %u "
                     "samples over %.0f s; MARK ENTER will now say whether the guard was HIGH when a component's "
                     "index was first seen lost",
                     list.c_str(), static_cast<unsigned>(g_frameGuardSamples), seconds);
        } else {
            DSH_INFO("puppetsession: frame guard: NOT ONE of the world's first 0x%X bytes both changes at frame "
                     "rate and comes back to its own value (%u samples over %.0f s) -- so either the guard is "
                     "outside that range or there is no per-frame guard bit to see",
                     static_cast<unsigned>(kFrameGuardScanBytes), static_cast<unsigned>(g_frameGuardSamples), seconds);
            g_frameGuardOffset = -2;
        }
        std::memset(g_frameGuardPulses, 0, sizeof(g_frameGuardPulses));
        std::memset(g_frameGuardToggles, 0, sizeof(g_frameGuardToggles));
        g_frameGuardSamples = 0;
        g_frameGuardStartMs = nowMs;
    }
    if (g_frameGuardOffset >= 0) {
        const std::uint8_t value = bytes[static_cast<std::uint32_t>(g_frameGuardOffset)];
        if (g_frameGuardSamples == 0) g_frameGuardBaseline = value;
        g_frameGuardHigh = value != g_frameGuardBaseline;
    }
}

constexpr std::size_t   kWriteRingMax = 4096;
constexpr std::size_t   kResetRingMax = 8;
constexpr std::size_t   kSeenIndexMax = 320;
constexpr std::uint64_t kWritePumpPeriodMs = 2;
constexpr std::uint64_t kWriteSiteReportPeriodMs = 10000;

enum WriteSite {
    kWriteSiteFixHide = 0,
    kWriteSiteFixShow,
    kWriteSiteHide,
    kWriteSiteHideRestore,
    kWriteSitePose,
    kWriteSiteCount
};

const char* write_site_name(int site) {
    switch (site) {
        case kWriteSiteFixHide:     return "fix-hide";
        case kWriteSiteFixShow:     return "fix-show";
        case kWriteSiteHide:        return "hide";
        case kWriteSiteHideRestore: return "hide-restore";
        case kWriteSitePose:        return "actor-pose";
        default:                    return "?";
    }
}

struct WriteRecord {
    std::uint64_t  ms = 0;
    int            site = -1;
    std::uintptr_t component = 0;
    std::uint32_t  index = kQueueNotQueued;
    QueueWhere     where = kQueueWhereUnknown;
    int            numTransform = -1;
    int            numGameThread = -1;
    std::uint32_t  thread = 0;
};
WriteRecord  g_writeRing[kWriteRingMax] = {};
std::size_t  g_writeRingCount = 0;
std::size_t  g_writeRingNext = 0;

std::uint64_t g_resetRing[kResetRingMax] = {};
std::size_t   g_resetRingCount = 0;
std::size_t   g_resetRingNext = 0;

struct SeenIndex {
    std::uintptr_t component = 0;
    std::uint32_t  index = kQueueNotQueued;
};
SeenIndex    g_seenIndex[kSeenIndexMax] = {};
std::size_t  g_seenIndexCount = 0;
std::size_t  g_seenIndexNext = 0;

std::uint32_t g_gtThreadId = 0;
std::uint32_t g_writeSiteWrites[kWriteSiteCount] = {};
std::uint32_t g_writeSiteOffGt[kWriteSiteCount] = {};
bool          g_writeSiteWarned[kWriteSiteCount] = {};
std::uint32_t g_writeRecordsFromMod = 0;
std::uint32_t g_writeRecordsFromEngine = 0;
std::uint64_t g_writePumpNextMs = 0;
std::uint64_t g_writeSiteReportNextMs = 0;
int           g_lastNumTransform = -1;
int           g_lastNumGameThread = -1;
bool          g_queueSampleTaken = false;

void queue_nums_now(int& numTransform, int& numGameThread) {
    numTransform = -1;
    numGameThread = -1;
    const std::uintptr_t world = dsh::reflect::live_world_address();
    if (world < 0x10000) return;
    const std::uint32_t offsets[2] = {
        dsh::reflect::world_offsets::kComponentsNeedEndOfFrameUpdate,
        dsh::reflect::world_offsets::kComponentsNeedEndOfFrameUpdateOnGameThread};
    int* const out[2] = {&numTransform, &numGameThread};
    for (int queue = 0; queue < 2; ++queue) {
        struct Table {
            std::uintptr_t data;
            std::int32_t   num;
            std::int32_t   max;
        } table = {};
        if (!dsh::reflect::read_process_bytes(world + offsets[queue], &table, sizeof(table))) continue;
        if (table.num < 0 || table.num > 200000) continue;
        *out[queue] = table.num;
    }
}

SeenIndex* seen_index_for(std::uintptr_t component) {
    for (std::size_t i = 0; i < g_seenIndexCount; ++i) {
        if (g_seenIndex[i].component == component) return &g_seenIndex[i];
    }
    return nullptr;
}

void observe_index_locked(int site, std::uintptr_t component, std::uint64_t nowMs, std::uint32_t thread,
                          int numTransform, int numGameThread) {
    if (component < 0x10000) return;
    std::uint32_t index = kQueueNotQueued;
    if (!dsh::reflect::read_process_bytes(component + kComponentQueueIndexOffset, &index, sizeof(index))) return;
    SeenIndex* seen = seen_index_for(component);
    if (seen == nullptr) {
        if (g_seenIndexCount < kSeenIndexMax) {
            seen = &g_seenIndex[g_seenIndexCount++];
        } else {
            seen = &g_seenIndex[g_seenIndexNext];
            g_seenIndexNext = (g_seenIndexNext + 1) % kSeenIndexMax;
        }
        seen->component = component;
        seen->index = index;
        return;
    }
    if (index == seen->index) return;
    seen->index = index;
    if (index == kQueueNotQueued) return;

    const QueueReading placed = queue_reading_of(component, index);
    WriteRecord& record = g_writeRing[g_writeRingNext];
    record.ms = nowMs;
    record.site = site;
    record.component = component;
    record.index = index;
    record.where = placed.where;
    record.numTransform = numTransform;
    record.numGameThread = numGameThread;
    record.thread = thread;
    g_writeRingNext = (g_writeRingNext + 1) % kWriteRingMax;
    if (g_writeRingCount < kWriteRingMax) ++g_writeRingCount;
    if (site >= 0) {
        ++g_writeRecordsFromMod;
    } else {
        ++g_writeRecordsFromEngine;
    }
}

void account_write_locked(int site, std::uint32_t thread) {

    const std::uint32_t owner = dsh::reflect::game_thread_owner_id();
    if (owner != 0) {
        g_gtThreadId = owner;
    } else if (g_gtThreadId == 0) {
        g_gtThreadId = thread;
    }
    if (site < 0 || site >= static_cast<int>(kWriteSiteCount)) return;
    ++g_writeSiteWrites[site];
    if (thread == g_gtThreadId) return;
    ++g_writeSiteOffGt[site];
    if (g_writeSiteWarned[site]) return;
    g_writeSiteWarned[site] = true;
    DSH_WARN("puppetsession: WRITE OFF GT %s: this write ran on thread %u while the game thread is %u -- an engine "
             "write from here can land inside the world's end-of-frame pass from a thread that is not allowed in it, "
             "and that is one of the two ways a mark is lost for good",
             write_site_name(site), static_cast<unsigned>(thread), static_cast<unsigned>(g_gtThreadId));
}

void note_write_target(int site, std::uintptr_t component, std::uint64_t nowMs) {
    const std::uint32_t thread = dsh::reflect::current_thread_id();
    int numTransform = -1;
    int numGameThread = -1;
    queue_nums_now(numTransform, numGameThread);
    const std::lock_guard<std::mutex> lock(g_writeRingMutex);
    account_write_locked(site, thread);
    observe_index_locked(site, component, nowMs, thread, numTransform, numGameThread);
}

void note_write_census(int site, std::uint64_t nowMs) {
    const std::uint32_t thread = dsh::reflect::current_thread_id();
    int numTransform = -1;
    int numGameThread = -1;
    queue_nums_now(numTransform, numGameThread);
    const std::lock_guard<std::mutex> lock(g_writeRingMutex);
    account_write_locked(site, thread);
    for (const auto& entry : g_markWatch) {
        observe_index_locked(site, entry.component, nowMs, thread, numTransform, numGameThread);
    }
}

void write_pump(std::uint64_t nowMs) {
    if (nowMs >= g_writePumpNextMs) {
        g_writePumpNextMs = nowMs + kWritePumpPeriodMs;
        int numTransform = -1;
        int numGameThread = -1;
        queue_nums_now(numTransform, numGameThread);
        if (numTransform >= 0 || numGameThread >= 0) {
            const std::lock_guard<std::mutex> lock(g_writeRingMutex);
            if (g_queueSampleTaken && numTransform == 0 && numGameThread == 0 &&
                (g_lastNumTransform > 0 || g_lastNumGameThread > 0)) {
                g_resetRing[g_resetRingNext] = nowMs;
                g_resetRingNext = (g_resetRingNext + 1) % kResetRingMax;
                if (g_resetRingCount < kResetRingMax) ++g_resetRingCount;
            }
            g_lastNumTransform = numTransform;
            g_lastNumGameThread = numGameThread;
            g_queueSampleTaken = true;

            for (const auto& entry : g_markWatch) {
                observe_index_locked(-1, entry.component, nowMs, 0, numTransform, numGameThread);
            }
        }
    }
    if (g_writeSiteReportNextMs == 0) g_writeSiteReportNextMs = nowMs + kWriteSiteReportPeriodMs;
    if (nowMs < g_writeSiteReportNextMs) return;
    std::string line;
    {
        const std::lock_guard<std::mutex> lock(g_writeRingMutex);
        g_writeSiteReportNextMs = nowMs + kWriteSiteReportPeriodMs;
        for (int site = 0; site < static_cast<int>(kWriteSiteCount); ++site) {
            if (g_writeSiteWrites[site] == 0) continue;
            char one[96] = {};
            std::snprintf(one, sizeof(one), "%s%s=%u (%u off the game thread)", line.empty() ? "" : " ",
                          write_site_name(site), static_cast<unsigned>(g_writeSiteWrites[site]),
                          static_cast<unsigned>(g_writeSiteOffGt[site]));
            line += one;
        }
        if (!line.empty()) {
            char tail[176] = {};
            std::snprintf(tail, sizeof(tail),
                          " ; index records: %u created by one of these writes, %u created with no mod write in "
                          "flight (ring holds %u)",
                          static_cast<unsigned>(g_writeRecordsFromMod),
                          static_cast<unsigned>(g_writeRecordsFromEngine),
                          static_cast<unsigned>(g_writeRingCount));
            line += tail;
        }
    }
    if (!line.empty()) DSH_INFO("puppetsession: WRITE SITES %s", line.c_str());
}

std::string write_ring_note(std::uintptr_t component, std::uint32_t index, std::uint64_t nowMs) {
    char buf[600] = {};
    {
        const std::lock_guard<std::mutex> lock(g_writeRingMutex);
        const WriteRecord* match = nullptr;
        for (std::size_t i = 0; i < g_writeRingCount; ++i) {
            const std::size_t slot = (g_writeRingNext + kWriteRingMax - 1 - i) % kWriteRingMax;
            const WriteRecord& record = g_writeRing[slot];
            if (record.component == component && record.index == index) {
                match = &record;
                break;
            }
        }
        if (match == nullptr) {
            std::snprintf(buf, sizeof(buf),
                          "no mod write created this index: the ring holds %u record(s) (%u from a mod write, %u "
                          "with no mod write in flight) and none of them is @0x%llX with index 0x%X -- so whatever "
                          "granted it did so while no mod write was running",
                          static_cast<unsigned>(g_writeRingCount),
                          static_cast<unsigned>(g_writeRecordsFromMod),
                          static_cast<unsigned>(g_writeRecordsFromEngine),
                          static_cast<unsigned long long>(component), static_cast<unsigned>(index));
            return buf;
        }
        std::uint64_t resetMs = 0;
        for (std::size_t i = 0; i < g_resetRingCount; ++i) {
            const std::size_t slot = (g_resetRingNext + kResetRingMax - 1 - i) % kResetRingMax;
            if (g_resetRing[slot] >= match->ms) {
                resetMs = g_resetRing[slot];
                break;
            }
        }
        const char* who = (match->site < 0) ? "the ENGINE (no mod write had claimed it)"
                                           : write_site_name(match->site);

        const char* where = (match->site < 0)
                                ? "a thread this instrument did not record"
                                : ((g_gtThreadId == 0 || match->thread == g_gtThreadId) ? "the game thread"
                                                                                       : "ANOTHER THREAD");

        const char* placed = match->where == kQueueWhereTransform    ? "its slot was in the TRANSFORM queue"
                             : match->where == kQueueWhereGameThread ? "its slot was in the GAME-THREAD queue"
                             : match->where == kQueueWhereLost       ? "its slot was in NEITHER queue"
                                                                     : "the queue it went into was not readable";
        if (resetMs == 0) {

            const bool alreadyEmpty = (match->numTransform == 0 && match->numGameThread == 0);
            std::snprintf(buf, sizeof(buf),
                          "the index 0x%X was created %llu ms ago by %s on %s, when the queues read %d / %d, and %s; %s",
                          static_cast<unsigned>(index), static_cast<unsigned long long>(nowMs - match->ms), who,
                          where, match->numTransform, match->numGameThread, placed,
                          alreadyEmpty ? "both queues were ALREADY empty when this was first seen, so the emptying "
                                         "happened between the mark and that sample -- the pass, seen one sample late"
                                       : "no queue emptying is in the ring after that");
            return buf;
        }
        const std::uint64_t gap = resetMs - match->ms;
        std::snprintf(buf, sizeof(buf),
                      "the index 0x%X was created %llu ms ago by %s on %s, when the queues read %d / %d, and %s; both "
                      "queues were EMPTIED %llu ms after that%s",
                      static_cast<unsigned>(index), static_cast<unsigned long long>(nowMs - match->ms), who, where,
                      match->numTransform, match->numGameThread, placed, static_cast<unsigned long long>(gap),
                      gap <= 2 ? " -- SO THIS MARK WAS MADE INSIDE THE WORLD'S END-OF-FRAME PASS, and the pass "
                                 "emptied the queue it was appended to"
                               : "");
    }
    return buf;
}

struct RenderWatch {
    std::uintptr_t component = 0;
    bool           located = false;
    std::uintptr_t proxySlot = 0;
    std::uintptr_t matrixOffset = 0;
    std::uintptr_t sceneInfoSlot = 0;
    std::uintptr_t sceneInfoInner = 0;
    int            failures = 0;
    bool           stale = false;
    std::uint64_t  lastLineMs = 0;
    bool           haveLast = false;
    float          lastRenderer[3] = {0.0f, 0.0f, 0.0f};
    float          lastComponent[3] = {0.0f, 0.0f, 0.0f};
    std::uintptr_t lastProxy = 0;

    bool           stateKnown = false;
    int            lastMobility = -1;
    bool           lastVisible = false;
    bool           lastHiddenInGame = false;
    bool           lastFixedSkelBounds = false;
    bool           lastSyncAttachParentLOD = false;
    bool           lastCpuSkinning = false;
    int            lastAnimTickOption = -1;
    std::uint32_t  lastMasterIndex = 0;
    bool           lastRenderStatic = false;

    bool           queueSampled = false;
    std::uint32_t  queueIndex = kQueueNotQueued;
    std::uint64_t  queueIndexSinceMs = 0;
    bool           queueStuck = false;
};
std::vector<RenderWatch> g_renderWatch;

RenderWatch& render_watch_for(std::uintptr_t component) {
    for (auto& watch : g_renderWatch) {
        if (watch.component == component) return watch;
    }
    g_renderWatch.push_back(RenderWatch{});
    g_renderWatch.back().component = component;
    return g_renderWatch.back();
}

constexpr int kDollTrackSamples = 64;
struct DollTrackSample {
    std::uint64_t ms = 0;
    float         locationCm[3] = {0.0f, 0.0f, 0.0f};
};
struct DollTrack {
    std::uintptr_t  machine = 0;
    DollTrackSample sample[kDollTrackSamples];
    int             next = 0;
    int             count = 0;
};
std::vector<DollTrack> g_dollTrack;

void doll_track_push(std::uintptr_t machine, const float locationCm[3]) {
    if (machine < 0x10000) return;
    DollTrack* track = nullptr;
    for (auto& candidate : g_dollTrack) {
        if (candidate.machine == machine) {
            track = &candidate;
            break;
        }
    }
    if (track == nullptr) {
        g_dollTrack.push_back(DollTrack{});
        track = &g_dollTrack.back();
        track->machine = machine;
    }
    DollTrackSample& sample = track->sample[track->next];
    sample.ms = GetTickCount64();
    for (int i = 0; i < 3; ++i) sample.locationCm[i] = locationCm[i];
    track->next = (track->next + 1) % kDollTrackSamples;
    if (track->count < kDollTrackSamples) ++track->count;
}

bool doll_track_nearest(std::uintptr_t machine, const float locationCm[3], double& distanceOut,
                        double& ageSecondsOut) {
    distanceOut = -1.0;
    ageSecondsOut = -1.0;
    if (machine < 0x10000) return false;
    const DollTrack* track = nullptr;
    for (const auto& candidate : g_dollTrack) {
        if (candidate.machine == machine) {
            track = &candidate;
            break;
        }
    }
    if (track == nullptr || track->count == 0) return false;
    const std::uint64_t nowMs = GetTickCount64();
    for (int i = 0; i < track->count; ++i) {
        const double cm = distance_cm(track->sample[i].locationCm, locationCm);
        if (distanceOut < 0.0 || cm < distanceOut) {
            distanceOut = cm;
            ageSecondsOut = static_cast<double>(nowMs - track->sample[i].ms) / 1000.0;
        }
    }
    return true;
}

std::uintptr_t g_renderSceneInfoSlot = 0;

std::string component_bytes_word(std::uintptr_t component) {
    unsigned char bytes[16] = {};
    if (!dsh::reflect::read_process_bytes(component + 0x0080, bytes, sizeof(bytes))) {
        return ", component bytes 0x80..0x8F NOT readable";
    }
    std::string out = ", component bytes 0x80..0x8F = ";
    char hex[4] = {};
    for (unsigned char byte : bytes) {
        std::snprintf(hex, sizeof(hex), "%02X", byte);
        out += hex;
    }
    return out;
}

std::string render_facts_word(const dsh::reflect::RenderPrimitive& proxy, std::uintptr_t component,
                              int componentMobility, const std::string& queueWord) {
    if (!proxy.factsRead) {
        return "; the proxy's own fields were NOT readable, end-of-frame queue " + queueWord +
               component_bytes_word(component);
    }
    const char* owner = proxy.proxyComponent == 0
                            ? "none recorded"
                            : (proxy.proxyComponent == component ? "THIS component"
                                                                 : "A DIFFERENT component");
    const char* sceneInfo = proxy.proxySceneInfo == 0
                                ? "NONE, so this proxy is in NO scene"
                                : (proxy.proxySceneInfoPointsBack ? "OK, it points back at the proxy"
                                                                  : "PRESENT, but it does not point back");
    char buf[400] = {};
    std::snprintf(buf, sizeof(buf),
                  "; the proxy itself says: mobility-when-built=%s (the component reads %s NOW), static-lighting=%d, "
                  "vf-needs-primitive-UB=%d, scene-info=%s, owner-component=%s",
                  mobility_word(proxy.proxyMobility), mobility_word(componentMobility),
                  proxy.proxyStaticLighting ? 1 : 0, proxy.proxyVfNeedsPrimitiveUniformBuffer ? 1 : 0, sceneInfo,
                  owner);
    return std::string(buf) + ", end-of-frame queue " + queueWord + component_bytes_word(component);
}
void probe_render_transform(const std::string& labelIn, const char* what, std::uintptr_t component,
                            const ComponentState& state, std::uintptr_t machine, bool dump) {
    if (component < 0x10000 || !state.locationRead) return;
    RenderWatch& watch = render_watch_for(component);

    std::string label(labelIn);
    const std::uint64_t nowMs = GetTickCount64();

    if (watch.stateKnown) {
        if (state.mobility != watch.lastMobility || state.visible != watch.lastVisible ||
            state.hiddenInGame != watch.lastHiddenInGame || state.animTickOption != watch.lastAnimTickOption ||
            state.masterPoseIndex != watch.lastMasterIndex ||
            state.fixedSkelBounds != watch.lastFixedSkelBounds ||
            state.syncAttachParentLOD != watch.lastSyncAttachParentLOD ||
            state.cpuSkinning != watch.lastCpuSkinning || state.renderStatic != watch.lastRenderStatic) {
            DSH_INFO("puppetsession: render[%s] %s '%s': the COMPONENT'S OWN STATE changed -- mobility %s -> %s, "
                     "vis %d -> %d, hid %d -> %d, tick %d -> %d, master idx %u -> %u, fixed-bounds %d -> %d, "
                     "sync-lod %d -> %d, cpu-skin %d -> %d, render-static %d -> %d (a proxy built while mobility "
                     "was Static is never updated again, which is why this is watched)",
                     label.c_str(), what, state.ownName.c_str(), mobility_word(watch.lastMobility),
                     mobility_word(state.mobility), watch.lastVisible ? 1 : 0, state.visible ? 1 : 0,
                     watch.lastHiddenInGame ? 1 : 0, state.hiddenInGame ? 1 : 0, watch.lastAnimTickOption,
                     state.animTickOption, static_cast<unsigned>(watch.lastMasterIndex),
                     static_cast<unsigned>(state.masterPoseIndex), watch.lastFixedSkelBounds ? 1 : 0,
                     state.fixedSkelBounds ? 1 : 0, watch.lastSyncAttachParentLOD ? 1 : 0,
                     state.syncAttachParentLOD ? 1 : 0, watch.lastCpuSkinning ? 1 : 0, state.cpuSkinning ? 1 : 0,
                     watch.lastRenderStatic ? 1 : 0, state.renderStatic ? 1 : 0);
            watch.lastLineMs = nowMs;
        }
        watch.lastMobility = state.mobility;
        watch.lastVisible = state.visible;
        watch.lastHiddenInGame = state.hiddenInGame;
        watch.lastAnimTickOption = state.animTickOption;
        watch.lastMasterIndex = state.masterPoseIndex;
        watch.lastFixedSkelBounds = state.fixedSkelBounds;
        watch.lastSyncAttachParentLOD = state.syncAttachParentLOD;
        watch.lastCpuSkinning = state.cpuSkinning;
        watch.lastRenderStatic = state.renderStatic;
    } else {
        watch.stateKnown = true;
        watch.lastMobility = state.mobility;
        watch.lastVisible = state.visible;
        watch.lastHiddenInGame = state.hiddenInGame;
        watch.lastAnimTickOption = state.animTickOption;
        watch.lastMasterIndex = state.masterPoseIndex;
        watch.lastFixedSkelBounds = state.fixedSkelBounds;
        watch.lastSyncAttachParentLOD = state.syncAttachParentLOD;
        watch.lastCpuSkinning = state.cpuSkinning;
        watch.lastRenderStatic = state.renderStatic;
    }

    {
        char labelBuf[96] = {};
        std::snprintf(labelBuf, sizeof(labelBuf), "%s@0x%llX", labelIn.c_str(),
                      static_cast<unsigned long long>(component));
        label = labelBuf;
    }

    std::uint32_t queueIndex = kQueueNotQueued;
    if (!dsh::reflect::read_process_bytes(component + kComponentQueueIndexOffset, &queueIndex,
                                          sizeof(queueIndex))) {
        queueIndex = kQueueNotQueued;
    }
    const QueueReading queue = queue_reading_of(component, queueIndex);
    if (!watch.queueSampled || queueIndex != watch.queueIndex) {
        const bool wasStuck = watch.queueStuck;
        watch.queueSampled = true;
        watch.queueIndex = queueIndex;
        watch.queueIndexSinceMs = nowMs;
        if (wasStuck) {
            watch.queueStuck = false;
            DSH_INFO("puppetsession: render[%s] %s '%s': the queue index MOVED AGAIN (%s) -- the component "
                     "is back on the end-of-frame path",
                     label.c_str(), what, state.ownName.c_str(), queue_short_word(queue).c_str());
        }
    } else if (!watch.queueStuck && queueIndex != kQueueNotQueued &&
               nowMs - watch.queueIndexSinceMs >= kQueueStuckMs) {

        if (queue.where == kQueueWhereLost) {
            watch.queueStuck = true;
            queue_stuck_line(label, what, state.ownName, component, queue,
                             (nowMs - watch.queueIndexSinceMs) / 1000,
                             write_ring_note(component, queue.index, nowMs));
        }
    }

    if (dump) {
        const std::uintptr_t holes[5] = {0x0248, 0x027C, 0x0288, 0x02B8, 0x0430};
        std::string slots;
        char slot[32] = {};
        for (std::uintptr_t hole : holes) {
            std::uint64_t value = 0;
            if (dsh::reflect::read_process_bytes(component + hole, &value, sizeof(value))) {
                std::snprintf(slot, sizeof(slot), " %04llX=0x%llX",
                              static_cast<unsigned long long>(hole), static_cast<unsigned long long>(value));
            } else {
                std::snprintf(slot, sizeof(slot), " %04llX=NOT-READ",
                              static_cast<unsigned long long>(hole));
            }
            slots += slot;
        }
        DSH_INFO("puppetsession: render[%s] %s '%s': candidate proxy slots%s%s", label.c_str(), what,
                 state.ownName.empty() ? state.className.c_str() : state.ownName.c_str(), slots.c_str(),
                 state.visible ? "" : " <-- bVisible=0: the engine creates NO proxy for this one, so which slot "
                                      "is null here IS the answer");
    }

    if (!state.visible || state.hiddenInGame) return;

    if (!watch.located) {

        dsh::reflect::RenderPrimitive located;
        if (!dsh::reflect::locate_render_primitive(component, state.locationCm, located)) {
            if (nowMs - watch.lastLineMs > 60000) {
                watch.lastLineMs = nowMs;
                DSH_INFO("puppetsession: render[%s] %s '%s': no renderer proxy could be found for this component "
                         "yet, so this mesh is not covered by the instrument -- %s",
                         label.c_str(), what, state.ownName.c_str(), located.note.c_str());
            }
            return;
        }
        watch.located = true;
        watch.proxySlot = located.proxySlot;
        watch.matrixOffset = located.matrixOffset;
        watch.sceneInfoSlot = located.sceneInfoSlot;

        if (located.sceneInfoSlot != 0) {
            g_renderSceneInfoSlot = located.sceneInfoSlot;
            watch.sceneInfoInner = located.sceneInfoInner;
        } else if (g_renderSceneInfoSlot != 0) {
            watch.sceneInfoSlot = g_renderSceneInfoSlot;
        }
        watch.lastLineMs = nowMs;
        char scene[112] = {};
        if (located.sceneInfoSlot == 0) {
            std::snprintf(scene, sizeof(scene), "no scene-info word was found inside it");
        } else {
            std::snprintf(scene, sizeof(scene), "it names %s, at proxy%llX (back-pointer %llX)",
                          located.sceneInfo != 0 ? "a scene" : "NO scene",
                          static_cast<unsigned long long>(located.sceneInfoSlot),
                          static_cast<unsigned long long>(located.sceneInfoInner));
        }
        DSH_INFO("puppetsession: render[%s] %s '%s': proxy 0x%llX at component%llX, transform at proxy%llX, "
                 "%s, %d candidate pointer(s); it reads (%.1f, %.1f, %.1f) cm against the component's "
                 "(%.1f, %.1f, %.1f) cm -- %.1f cm apart%s",
                 label.c_str(), what, state.ownName.c_str(),
                 static_cast<unsigned long long>(located.proxy),
                 static_cast<unsigned long long>(located.proxySlot),
                 static_cast<unsigned long long>(located.matrixOffset), scene, located.candidates,
                 static_cast<double>(located.translationCm[0]), static_cast<double>(located.translationCm[1]),
                 static_cast<double>(located.translationCm[2]), static_cast<double>(state.locationCm[0]),
                 static_cast<double>(state.locationCm[1]), static_cast<double>(state.locationCm[2]),
                 distance_cm(located.translationCm, state.locationCm),
                 located.matrixMatchesComponent ? ", and it IS this component's own transform"
                                                : ", and it is NOT this component's transform");
        DSH_INFO("puppetsession: render[%s] %s '%s':%s", label.c_str(), what, state.ownName.c_str(),
                 render_facts_word(located, component, state.mobility, queue_short_word(queue)).c_str());
        return;
    }

    dsh::reflect::RenderPrimitive reading;
    if (!dsh::reflect::read_render_primitive(component, watch.proxySlot, watch.matrixOffset,
                                             watch.sceneInfoSlot, reading)) {

        if (++watch.failures >= 3) {
            watch = RenderWatch{};
            watch.component = component;
        }
        if (dump || nowMs - watch.lastLineMs > 30000) {
            watch.lastLineMs = nowMs;
            DSH_INFO("puppetsession: render[%s] %s '%s': NOT readable this tick -- %s",
                     label.c_str(), what, state.ownName.c_str(), reading.note.c_str());
        }
        return;
    }
    watch.failures = 0;
    const double apart = distance_cm(reading.translationCm, state.locationCm);
    const bool   sceneMissing = reading.sceneInfoSlot != 0 && reading.sceneInfo == 0;
    const char*  scene = reading.sceneInfoSlot == 0 ? "scene UNKNOWN"
                          : (reading.sceneInfo != 0 ? "in a scene" : "in NO scene");

    const float prevComponent[3] = {watch.lastComponent[0], watch.lastComponent[1], watch.lastComponent[2]};
    const std::uintptr_t prevProxy = watch.lastProxy;
    const bool  rendererMoved =
        watch.haveLast && distance_cm(reading.translationCm, watch.lastRenderer) > kRenderMovedCm;
    const bool componentMoved =
        watch.haveLast && distance_cm(state.locationCm, watch.lastComponent) > kComponentMovedCm;
    const bool frozen = watch.haveLast && !rendererMoved && componentMoved;
    bool       stale = frozen || apart > kRenderRunawayCm || sceneMissing;

    if (!stale && watch.stale && apart > kRenderAgreedCm) stale = true;

    for (int i = 0; i < 3; ++i) {
        watch.lastRenderer[i] = reading.translationCm[i];
        watch.lastComponent[i] = state.locationCm[i];
    }
    watch.haveLast = true;
    watch.lastProxy = reading.proxy;

    if (prevProxy != 0 && prevProxy != reading.proxy) {
        DSH_INFO("puppetsession: render[%s] %s '%s': the proxy CHANGED 0x%llX -> 0x%llX -- the render state was "
                 "destroyed and rebuilt since the previous tick", label.c_str(), what, state.ownName.c_str(),
                 static_cast<unsigned long long>(prevProxy), static_cast<unsigned long long>(reading.proxy));
    }

    if (stale) {
        const bool announce = !watch.stale || dump || nowMs - watch.lastLineMs > 10000;
        watch.stale = true;
        if (!announce) return;
        watch.lastLineMs = nowMs;
        double      nearestCm = -1.0;
        double      ageSeconds = -1.0;
        std::string where = "this doll has no recorded position to compare it with";
        if (doll_track_nearest(machine, reading.translationCm, nearestCm, ageSeconds)) {
            char buf[192] = {};
            std::snprintf(buf, sizeof(buf),
                          "the nearest place this doll has stood is %.1f cm away from it, %.1f s ago", nearestCm,
                          ageSeconds);
            where = buf;
        }

        DSH_INFO("puppetsession: RENDER STALE[%s] %s '%s': the renderer has it FROZEN at (%.1f, %.1f, %.1f) cm -- "
                 "it did not move since the previous tick -- while the component went from (%.1f, %.1f, %.1f) to "
                 "(%.1f, %.1f, %.1f) cm, %.1f cm apart now; proxy 0x%llX, %s; %s",
                 label.c_str(), what, state.ownName.c_str(), static_cast<double>(reading.translationCm[0]),
                 static_cast<double>(reading.translationCm[1]), static_cast<double>(reading.translationCm[2]),
                 static_cast<double>(prevComponent[0]), static_cast<double>(prevComponent[1]),
                 static_cast<double>(prevComponent[2]), static_cast<double>(state.locationCm[0]),
                 static_cast<double>(state.locationCm[1]), static_cast<double>(state.locationCm[2]), apart,
                 static_cast<unsigned long long>(reading.proxy), scene, where.c_str());
        DSH_INFO("puppetsession: RENDER STALE[%s] %s '%s':%s", label.c_str(), what, state.ownName.c_str(),
                 render_facts_word(reading, component, state.mobility, queue_short_word(queue)).c_str());
        return;
    }
    if (watch.stale) {

        watch.stale = false;
        watch.lastLineMs = nowMs;
        DSH_INFO("puppetsession: render[%s] %s '%s': the renderer is back in agreement with the component at "
                 "(%.1f, %.1f, %.1f) cm -- %.1f cm apart, %s; proxy 0x%llX%s",
                 label.c_str(), what, state.ownName.c_str(), static_cast<double>(reading.translationCm[0]),
                 static_cast<double>(reading.translationCm[1]), static_cast<double>(reading.translationCm[2]),
                 apart, scene, static_cast<unsigned long long>(reading.proxy),
                 prevProxy != 0 && prevProxy != reading.proxy
                     ? " (a DIFFERENT proxy than the frozen one: the render state was rebuilt)"
                     : "");
        return;
    }
    if (dump) {

        DSH_INFO("puppetsession: render[%s] %s '%s': the renderer has it at (%.1f, %.1f, %.1f) cm, the component "
                 "at (%.1f, %.1f, %.1f) cm -- %.1f cm apart (one frame of travel is normal), %s; proxy 0x%llX",
                 label.c_str(), what, state.ownName.c_str(), static_cast<double>(reading.translationCm[0]),
                 static_cast<double>(reading.translationCm[1]), static_cast<double>(reading.translationCm[2]),
                 static_cast<double>(state.locationCm[0]), static_cast<double>(state.locationCm[1]),
                 static_cast<double>(state.locationCm[2]), apart, scene,
                 static_cast<unsigned long long>(reading.proxy));
        DSH_INFO("puppetsession: render[%s] %s '%s':%s", label.c_str(), what, state.ownName.c_str(),
                 render_facts_word(reading, component, state.mobility, queue_short_word(queue)).c_str());
    }
}

std::string check_body_meshes(const std::string& label, std::uintptr_t machine,
                              const float machineLocationCm[3], std::uintptr_t primitiveClass,
                              std::uint64_t& lineBudget, bool dump) {
    if (machine < 0x10000) return " no machine";
    std::uint64_t humanoidRaw = 0;
    if (!dsh::reflect::read_process_bytes(machine + kMachineHumanoidOffset, &humanoidRaw,
                                          sizeof(humanoidRaw)) ||
        humanoidRaw < 0x10000) {
        return " no humanoid module";
    }
    const auto humanoidActor = static_cast<std::uintptr_t>(humanoidRaw);
    struct NamedPart {
        const char*    what;
        std::uintptr_t base;
        std::uintptr_t offset;

        bool           direct = false;
    };

    std::uintptr_t thrusterMesh = 0;
    {
        std::uint64_t thrusterModule = 0;
        if (dsh::reflect::read_process_bytes(machine + dsh::game::kMachineThrusterOffset, &thrusterModule,
                                             sizeof(thrusterModule)) &&
            thrusterModule >= 0x10000) {
            std::uint64_t mesh = 0;
            if (dsh::reflect::read_process_bytes(static_cast<std::uintptr_t>(thrusterModule) +
                                                     dsh::game::kThrusterModuleMeshOffset,
                                                 &mesh, sizeof(mesh)) &&
                mesh >= 0x10000) {
                thrusterMesh = static_cast<std::uintptr_t>(mesh);
            }
        }
    }
    const NamedPart parts[7] = {{"body", humanoidActor, kActorMeshOffset},
                                {"legs", humanoidActor, kHumanoidLegMeshOffset},
                                {"face", humanoidActor, kHumanoidFaceMeshOffset},
                                {"front hair", humanoidActor, kHumanoidFrontHairMeshOffset},
                                {"back hair", humanoidActor, kHumanoidBackHairMeshOffset},
                                {"body(machine)", machine, kActorMeshOffset},
                                {"thruster", thrusterMesh, 0, true}};
    std::string text;
    for (const auto& part : parts) {
        std::uint64_t raw = 0;
        if (part.direct) {
            raw = static_cast<std::uint64_t>(part.base);
        } else if (!dsh::reflect::read_process_bytes(part.base + part.offset, &raw, sizeof(raw))) {
            text += std::string(" | ") + part.what + " NOT read";
            continue;
        }
        const auto component = static_cast<std::uintptr_t>(raw);
        if (component < 0x10000) {
            text += std::string(" | ") + part.what + " NONE";
            continue;
        }
        const ComponentState state = component_state(component, primitiveClass);
        const double distance =
            state.locationRead ? distance_cm(state.locationCm, machineLocationCm) : -1.0;
        const std::string name = state.ownName.empty() ? state.className : state.ownName;

        probe_render_transform(label, part.what, component, state, machine, dump);

        mark_watch_note(part.what, component);
        char one[288] = {};

        std::uint32_t queueIndex = kQueueNotQueued;
        if (!dsh::reflect::read_process_bytes(component + kComponentQueueIndexOffset, &queueIndex,
                                              sizeof(queueIndex))) {
            queueIndex = kQueueNotQueued;
        }
        const std::string queueWord = queue_short_word(queue_reading_of(component, queueIndex));

        unsigned char renderFlags = 0;
        int hiddenInGame = -1;
        if (dsh::reflect::read_process_bytes(component + kSceneFlagsOffset + 1, &renderFlags, sizeof(renderFlags))) {
            hiddenInGame = (renderFlags & 0x04) != 0 ? 1 : 0;
        }
        float relative[3] = {0.0f, 0.0f, 0.0f};
        const bool relativeRead =
            dsh::reflect::read_process_bytes(component + kSceneRelativeLocationOffset, relative, sizeof(relative));
        if (state.locationRead) {
            std::snprintf(one, sizeof(one), " | %s '%s' %.1f cm hid=%d rel(%.1f, %.1f, %.1f)%s q=%s", part.what,
                          name.c_str(), distance, hiddenInGame,
                          relativeRead ? static_cast<double>(relative[0]) : -99999.0,
                          relativeRead ? static_cast<double>(relative[1]) : -99999.0,
                          relativeRead ? static_cast<double>(relative[2]) : -99999.0,
                          relativeRead ? "" : " NO-REL", queueWord.c_str());
        } else {
            std::snprintf(one, sizeof(one), " | %s '%s' NOT located hid=%d q=%s", part.what, name.c_str(),
                          hiddenInGame, queueWord.c_str());
        }
        text += one;
        if (distance > kStuckPartThresholdCm && lineBudget < 96) {
            ++lineBudget;
            DSH_INFO("puppetsession: stuck part[%s]: the doll's %s mesh %s -- it is not where the doll is",
                     label.c_str(), part.what, component_words(state, distance).c_str());
        }
    }
    return text;
}

std::string mesh_owner_sprint_word(std::uintptr_t machine) {
    if (machine < 0x10000) return "no machine";
    std::uint64_t humanoid = 0;
    if (!dsh::reflect::read_process_bytes(machine + kMachineHumanoidOffset, &humanoid, sizeof(humanoid)) ||
        humanoid < 0x10000) {
        return "no humanoid";
    }
    std::uint8_t byte = 0;
    if (!dsh::reflect::read_process_bytes(
            static_cast<std::uintptr_t>(humanoid) + dsh::game::kCharacterSprintOffset, &byte,
            sizeof(byte))) {
        return "NOT read";
    }
    return (byte != 0) ? "1" : "0";
}

std::string sprint_effect_text(std::uint64_t applied, std::uint64_t refused, std::uint64_t removed,
                               std::uint64_t removalFailures, std::uint64_t handle) {
    std::string out = " | the game's sprint effect: applied=" + std::to_string(applied) + " refused=" +
                      std::to_string(refused) + " removed=" + std::to_string(removed) +
                      " removal-failed=" + std::to_string(removalFailures);
    if (handle == 0) {
        out += ", and this side holds NO handle right now";
        return out;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), ", holding handle 0x%llX", static_cast<unsigned long long>(handle));
    out += buf;
    return out;
}

struct PositionUpdate {
    float                       posM[3] = {0.0f, 0.0f, 0.0f};

    float                       yawRad = 0.0f;
    float                       pitchRad = 0.0f;

    DollStance                  stance;

    float                       velocity[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t                hostMovementMode = 0;

    float                       acceleration[3] = {0.0f, 0.0f, 0.0f};

    std::uint8_t                exclusiveTypes = 0;
    std::uint8_t                exclusiveOwnerKind = 0;

    dsh::game::EnergyTimers      energyTimers;
    bool                        energyTimersReadable = false;

    bool                        energyStepKnown = false;
    float                       energyPreviousIntermission = 0.0f;

    std::uint8_t                frontWeaponIndex = kFrontWeaponNotRead;
    dsh::game::FrontWeaponApplyResult appliedFrontWeapon;
    std::uintptr_t              weaponClass = 0;

    dsh::game::EnergyTimerApplyResult appliedEnergyTimers;

    dsh::game::MovementInputs   movementInput;

    float                       appliedVelocity[3] = {0.0f, 0.0f, 0.0f};
    bool                        appliedVelocityOk = false;

    dsh::game::VelocityApplyResult appliedVelocityResult;

    dsh::game::MovementInputs   appliedMovementInput;
    bool                        appliedMovementFlagsOk = false;
    bool                        appliedMovementVectorOk = false;
    bool                        appliedMovementFlagsChanged = false;
    bool                        appliedMovementVectorChanged = false;

    bool                        derivedSampled = false;
    std::string                 derivedNote;

    SprintCapture               sprintResult;

    std::uintptr_t              stanceClass = 0;
    std::vector<std::uintptr_t> actors;
};

struct GetterCapture {
    bool                            sampled = false;
    dsh::reflect::CharacterDerivedFlags flags;
    dsh::game::MovementInputs       bytes;
    std::string                     note;
};

std::string hex_preview(std::uintptr_t address, std::size_t bytes) {
    std::vector<std::uint8_t> raw(bytes, 0);
    if (!dsh::reflect::read_process_bytes(address, raw.data(), raw.size())) return "<unreadable>";
    std::string out;
    char buf[8];
    for (std::size_t i = 0; i < raw.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%02X ", raw[i]);
        out += buf;
    }
    return out;
}

void log_report_lines(const char* prefix, const std::string& report) {
    std::size_t begin = 0;
    for (;;) {
        const std::size_t end = report.find('\n', begin);
        const std::string line =
            report.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!line.empty()) DSH_INFO("%s%s", prefix, line.c_str());
        if (end == std::string::npos) break;
        begin = end + 1;
    }
}

constexpr const char* kWalkThrusterClass = "DAThrusterModule";

constexpr const char* kWalkNiagaraClass  = "NiagaraComponent";

void module_object_iterator(std::uint32_t maxObjects,
                            void (*onObject)(void* user, std::uintptr_t object, std::uintptr_t outer),
                            void* user, std::uint32_t& visited);
bool module_walk_name(std::uintptr_t nameAddress, std::string& out);

struct MachineBuildStats {
    bool        ranOnGameThread = false;
    bool        spawned = false;
    int         modulesMerged = 0;

    std::string report;

    std::string note;

    bool                              arrivalValid = false;
    dsh::reflect::ModuleArrivalResult arrival;

    dsh::game::ModuleComponentReport  moduleBefore;
    dsh::game::ModuleComponentReport  moduleAfter;
    bool                              moduleWalked = false;
    std::string                       moduleWalkNote;
    std::uint64_t                     thrusterSlotBefore = 0;
    std::uint64_t                     thrusterSlotAfter = 0;
    bool                              thrusterSlotReadBefore = false;
    bool                              thrusterSlotReadAfter = false;
};

std::string hex_pointer(std::uintptr_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

std::vector<std::string> module_arrival_report(const MachineBuildStats& stats) {
    std::vector<std::string> out;

    if (!stats.arrivalValid) {

        out.push_back(std::string("module arrival: NOT attempted, and NOTHING was measured -- ") +
                      (stats.spawned
                           ? ("the machine spawned but the game filled none of its ten module pointers "
                              "(or no 'DAThrusterModule' walk could be made of it), so there is no "
                              "module the Blueprint side could be told about and the game's own OnRep "
                              "was NOT called" +
                              (stats.moduleWalkNote.empty() ? std::string()
                                                            : ("; the walk refused: " + stats.moduleWalkNote)))
                           : "the machine did not spawn, so there is no module to announce") +
                      " (see the build's own lines above)");
        return out;
    }

    const char* const* const slots = dsh::reflect::module_names_rep_notify_slots();
    int called = 0;
    std::string notCalled;
    for (int i = 0; i < 9; ++i) {
        if (stats.arrival.callMade[i]) {
            ++called;
            continue;
        }
        if (!notCalled.empty()) notCalled += ", ";
        notCalled += slots[i];
    }

    char head[704];
    std::snprintf(head, sizeof(head),
                  "module arrival: %d of the 9 module OnRep notification(s) WERE CALLED on the puppet "
                  "-- a count BELOW 9 means that many could not be resolved and were NOT called (each "
                  "one is named on the next line); resolved by owner ADDRESS: "
                  "resolve_uclass_address(\"DAMachine\") then find_function_owned_by per slot. The nine "
                  "are Mobility Humanoid Headset Repeater Thruster Utility Hanger LeftRearWeapon "
                  "RightRearWeapon, in that order. CurrentFrontWeapon has NO call here: its "
                  "OnRep takes a weapon parameter and belongs to the weapon path",
                  called);
    out.push_back(head);
    if (!notCalled.empty()) {

        out.push_back("module arrival: the slot(s) NOT called, from the call's own per-slot flags: " +
                      notCalled);
    }
    if (!stats.arrival.unresolved.empty()) {
        out.push_back("module arrival: NOT called -- " + stats.arrival.unresolved);
    }
    if (!stats.arrival.note.empty()) {
        out.push_back("module arrival: the call refused -- " + stats.arrival.note);
    }
    if (called >= 9) {
        out.push_back("module arrival: all nine resolved and called and NOTHING was refused, so the "
                      "count, the two Thruster-slot reads below and the two walks are a complete "
                      "account of this step");
    }

    char slotLine[288];
    std::snprintf(slotLine, sizeof(slotLine),
                  "module arrival: the Thruster slot at read %s before the calls and %s after "
                  "them%s",
                  stats.thrusterSlotReadBefore
                      ? hex_pointer(stats.thrusterSlotBefore).c_str()
                      : "NOT read",
                  stats.thrusterSlotReadAfter
                      ? hex_pointer(stats.thrusterSlotAfter).c_str()
                      : "NOT read",
                  (stats.thrusterSlotReadBefore && stats.thrusterSlotReadAfter &&
                   stats.thrusterSlotBefore != stats.thrusterSlotAfter)
                      ? " -- THE SLOT ITSELF CHANGED ACROSS THE CALLS"
                      : "");
    out.push_back(slotLine);

    if (!stats.moduleWalked) {
        out.push_back("module arrival: the puppet's thruster module could NOT be walked on either side "
                      "of the calls -- " +
                      (stats.moduleWalkNote.empty()
                           ? std::string("the walk refused without a named reason")
                           : stats.moduleWalkNote) +
                      " -- so whether the calls changed the module is NOT measured by this run");
        return out;
    }

    const std::vector<std::string> a = stats.moduleBefore.lines();
    const std::vector<std::string> b = stats.moduleAfter.lines();
    out.push_back("module arrival: the puppet's thruster module, walked immediately BEFORE the calls "
                  "-- the same fields the two-second line prints:");
    for (const std::string& line : a) out.push_back(" before | " + line);
    out.push_back("module arrival: the puppet's thruster module, walked immediately AFTER the calls "
                  "returned:");
    for (const std::string& line : b) out.push_back(" after | " + line);

    std::string changed;
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < common; ++i) {
        if (a[i] == b[i]) continue;
        if (!changed.empty()) changed += "; ";
        changed += "line " + std::to_string(i) + " was '" + a[i] + "' and is now '" + b[i] + "'";
    }
    if (a.size() != b.size()) {
        if (!changed.empty()) changed += "; ";
        changed += "the before walk printed " + std::to_string(a.size()) + " line(s) and the after walk "
                   "printed " + std::to_string(b.size());
    }
    if (changed.empty()) {
        out.push_back("module arrival: before-and-after verdict -- IDENTICAL: all " +
                      std::to_string(a.size()) +
                      " line(s) of the module walk, including every component's class, its activation "
                      "bytes and the ModuleStatus block, read exactly the same before the nine calls "
                      "and after them -- THE CALL CHANGED NOTHING THIS WALK MEASURES");
    } else {
        out.push_back("module arrival: before-and-after verdict -- CHANGED: " + changed);
    }
    return out;
}

MachineBuildStats build_machine(const dsh::reflect::MachineLoadout& loadout,
                                std::vector<std::uintptr_t>& actorsOut) {

    DSH_TIME_WALK("loop:build");

    {
        static std::atomic<std::uint64_t> builds{0};
        const auto count = builds.fetch_add(1) + 1;
        DSH_INFO("build: machine build #%llu in this session (each one loads assets and spawns actors; it should "
                 "be #1 only)", static_cast<unsigned long long>(count));
    }
    MachineBuildStats stats;
    stats.ranOnGameThread = dsh::reflect::run_on_game_thread(
        [&]() {
            std::uintptr_t actor = 0;
            stats.spawned =
                dsh::reflect::spawn_machine_with_loadout(loadout, actor, stats.report, stats.note);
            if (!stats.spawned || actor < 0x10000) return;
            actorsOut.push_back(actor);

            stats.modulesMerged = dsh::reflect::read_machine_modules(actor).nonNull;

            if (stats.modulesMerged > 0) {

                static std::uintptr_t s_thrusterClass = 0;
                static std::uintptr_t s_niagaraClass = 0;
                static bool           s_checked = false;
                if (!s_checked) {
                    s_checked = true;
                    s_thrusterClass = dsh::reflect::resolve_uclass_address(kWalkThrusterClass);
                    s_niagaraClass = dsh::reflect::resolve_uclass_address(kWalkNiagaraClass);
                }

                std::uint64_t slotValue = 0;
                stats.thrusterSlotReadBefore =
                    dsh::reflect::read_process_bytes(actor + dsh::game::kMachineThrusterOffset,
                                                     &slotValue, sizeof(slotValue));
                stats.thrusterSlotBefore = static_cast<std::uint64_t>(slotValue);

                bool walked = false;
                if (s_thrusterClass != 0) {
                    walked = dsh::game::module_component_dump(
                        actor, "theirs", s_thrusterClass, s_niagaraClass, 0, &module_object_iterator,
                        &dsh::reflect::read_process_bytes, &module_walk_name,
                        &dsh::game::actor_class_chain_reaches, stats.moduleBefore);
                    if (!walked) stats.moduleWalkNote = stats.moduleBefore.note;
                } else {
                    stats.moduleWalkNote =
                        std::string("the '") + kWalkThrusterClass +
                        "' UClass could not be resolved, so the module's own offsets cannot be checked "
                        "and nothing was read from it on either side of the calls";
                }

                if (walked) {

                    stats.arrivalValid = true;

                    dsh::reflect::module_call_arrival_notify(actor, stats.arrival);

                    stats.moduleWalked = dsh::game::module_component_dump(
                        actor, "theirs", s_thrusterClass, s_niagaraClass, 0, &module_object_iterator,
                        &dsh::reflect::read_process_bytes, &module_walk_name,
                        &dsh::game::actor_class_chain_reaches, stats.moduleAfter);
                    if (!stats.moduleWalked && stats.moduleWalkNote.empty()) {
                        stats.moduleWalkNote = stats.moduleAfter.note;
                    }
                    stats.thrusterSlotReadAfter =
                        dsh::reflect::read_process_bytes(actor + dsh::game::kMachineThrusterOffset,
                                                         &slotValue, sizeof(slotValue));
                    stats.thrusterSlotAfter = static_cast<std::uint64_t>(slotValue);
                }
            }

                    },
        30000);
    return stats;
}

constexpr std::uintptr_t kObjectNameOffset = 0x18;
constexpr std::uintptr_t kObjectOuterOffset = 0x20;

constexpr std::int32_t  kGameGObjects           = 0x04A73450;
constexpr std::uint32_t kObjectChunkSize        = 0x10000;
constexpr std::uint32_t kObjectItemsOffset      = 0x00;
constexpr std::uint32_t kObjectCountOffset      = 0x14;
constexpr std::uint32_t kObjectItemStride       = 0x18;

inline constexpr const char* kWalkLabelMine   = "mine";
inline constexpr const char* kWalkLabelTheirs = "theirs";

bool module_walk_name(std::uintptr_t nameAddress, std::string& out) {
    out.clear();
    std::uint64_t nameField = 0;
    if (!dsh::reflect::read_process_bytes(nameAddress, &nameField, sizeof(nameField))) return false;
    const std::uint32_t index = static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu);
    if (index == 0) return false;
    std::string note;
    return dsh::reflect::resolve_fname(index, out, note) && !out.empty();
}

void module_object_iterator(std::uint32_t maxObjects,
                            void (*onObject)(void* user, std::uintptr_t object, std::uintptr_t outer),
                            void* user, std::uint32_t& visited) {
    visited = 0;

    const std::uintptr_t base = dsh::reflect::main_module_base();
    if (base == 0) return;
    const std::uintptr_t array = base + static_cast<std::uintptr_t>(kGameGObjects);
    std::uint64_t chunkTable = 0;
    std::uint32_t numElements = 0;
    if (!dsh::reflect::read_process_bytes(array + kObjectItemsOffset, &chunkTable, sizeof(chunkTable)) ||
        chunkTable < 0x10000) {
        return;
    }
    if (!dsh::reflect::read_process_bytes(array + kObjectCountOffset, &numElements,
                                          sizeof(numElements))) {
        return;
    }
    if (maxObjects != 0 && numElements > maxObjects) numElements = maxObjects;
    for (std::uint32_t i = 0; i < numElements; ++i) {
        std::uint64_t chunk = 0;
        if (!dsh::reflect::read_process_bytes(
                static_cast<std::uintptr_t>(chunkTable) +
                    static_cast<std::uintptr_t>(i / kObjectChunkSize) * 8,
                &chunk, sizeof(chunk)) ||
            chunk < 0x10000) {
            continue;
        }
        std::uint64_t obj = 0;
        if (!dsh::reflect::read_process_bytes(
                static_cast<std::uintptr_t>(chunk) +
                    static_cast<std::uintptr_t>(i % kObjectChunkSize) * kObjectItemStride,
                &obj, sizeof(obj))) {
            continue;
        }
        if (obj < 0x10000) continue;
        const auto address = static_cast<std::uintptr_t>(obj);
        std::uint64_t outer = 0;
        if (!dsh::reflect::read_process_bytes(address + kObjectOuterOffset, &outer, sizeof(outer))) {
            continue;
        }
        ++visited;
        if (onObject != nullptr) onObject(user, address, static_cast<std::uintptr_t>(outer));
    }
}

struct ModuleWalkCache {
    std::uintptr_t                   machine = 0;
    bool                             haveList = false;
    dsh::game::ModuleComponentReport report;
};

ModuleWalkCache& module_walk_cache(const std::string& label) {
    static ModuleWalkCache mineCache;
    static ModuleWalkCache theirsCache;
    return (label == kWalkLabelMine) ? mineCache : theirsCache;
}

const dsh::game::ModuleComponentReport* cached_module_walk(const std::string& label) {
    const ModuleWalkCache& cache = module_walk_cache(label);
    return cache.haveList ? &cache.report : nullptr;
}

std::vector<std::string> thruster_module_walk(std::uintptr_t machine, const std::string& label,
                                              std::uintptr_t thrusterClass, std::uintptr_t niagaraClass,
                                              bool& classRefusalLogged, const std::string& notMade = {}) {
    std::vector<std::string> out;
    if (machine < 0x10000) {
        out.push_back("thruster module walk[" + label + "]: not made -- " +
                      (notMade.empty() ? std::string("there is no machine actor on this side right now")
                                       : notMade));
        return out;
    }
    if (thrusterClass == 0 || niagaraClass == 0) {

        if (!classRefusalLogged) {
            classRefusalLogged = true;
            DSH_WARN("puppetsession: %s is NOT in the object array (or %s is not), so the thruster "
                     "module's class chain cannot be checked and its own offsets will NOT be read -- the "
                     "walk below lists the module's components and says so on every line",
                     kWalkThrusterClass, kWalkNiagaraClass);
        }
    }

    ModuleWalkCache& cache = module_walk_cache(label);

    std::string change;
    if (!cache.haveList || cache.machine != machine) {

        if (cache.haveList) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "the machine actor CHANGED from 0x%llX to 0x%llX, so the component list was "
                          "re-walked rather than reused",
                          static_cast<unsigned long long>(cache.machine),
                          static_cast<unsigned long long>(machine));
            change = buf;
        }

        const bool walked =
            dsh::game::module_component_dump(machine, label, thrusterClass, niagaraClass, 0,
                                             &module_object_iterator, &dsh::reflect::read_process_bytes,
                                             &module_walk_name, &dsh::game::actor_class_chain_reaches,
                                             cache.report);
        cache.machine = machine;
        if (!walked) {

            cache.haveList = false;
            out.push_back("thruster module walk[" + label + "]: not made -- " +
                          (cache.report.note.empty()
                               ? std::string("the walk refused without a named reason")
                               : cache.report.note));
            return out;
        }
        cache.haveList = true;

        {
            static bool fieldsLogged[2] = {false, false};
            const int side = (label == kWalkLabelMine) ? 0 : 1;
            if (!fieldsLogged[side] && cache.report.module != 0) {
                fieldsLogged[side] = true;
                std::uintptr_t moduleClass = 0;
                if (dsh::reflect::read_process_bytes(cache.report.module + 0x0010, &moduleClass,
                                                     sizeof(moduleClass)) &&
                    moduleClass != 0) {
                    std::uint64_t fieldBytes = 0;
                    const std::vector<std::string> fieldLines = dsh::game::class_field_lines(
                        moduleClass, &dsh::reflect::read_process_bytes, &module_walk_name, &fieldBytes);
                    for (const std::string& fieldLine : fieldLines) out.push_back(fieldLine);
                    char cost[256];
                    std::snprintf(cost, sizeof(cost),
                                  "class fields: the listing above cost %llu guarded byte(s) of reads, "
                                  "against roughly 200,000 reads for one walk of the object table",
                                  static_cast<unsigned long long>(fieldBytes));
                    out.push_back(cost);
                } else {
                    out.push_back(
                        "class fields: NOT listed -- the module's own UObject::Class at could not be "
                        "read, and a listing of nothing would read as a class that has no fields");
                }
            }
        }
    } else {

        dsh::game::refresh_module_component_state(cache.report, thrusterClass,
                                                  &dsh::reflect::read_process_bytes,
                                                  &dsh::game::actor_class_chain_reaches);
    }

    out = cache.report.lines();
    if (!change.empty()) {

        const std::size_t at = out.empty() ? 0 : 1;
        out.insert(out.begin() + static_cast<std::ptrdiff_t>(at),
                   "thruster module walk[" + label + "]: " + change);
    }
    return out;
}

std::uintptr_t local_pawn_machine(std::uintptr_t& cached, std::string& refusal) {
    if (cached >= 0x10000) {
        refusal.clear();
        return cached;
    }
    const dsh::game::PawnHandle handle = dsh::game::locate_local_pawn();
    if (handle.pawn < 0x10000) {
        refusal = handle.note.empty() ? std::string("the local pawn could not be located")
                                      : handle.note;
        return 0;
    }
    cached = handle.pawn;
    refusal = handle.note.empty() ? std::string("located") : handle.note;
    return cached;
}

float local_aim_pitch_radians(std::uintptr_t machine, float capsulePitchRad, bool& readable,
                             std::string& why) {
    constexpr float kDegToRad = 3.14159265358979f / 180.0f;
    readable = false;
    why.clear();
    if (machine < 0x10000) {
        why = "the pawn handle is not a live address";
        return capsulePitchRad;
    }
    std::uint64_t controller = 0;
    if (!dsh::reflect::read_process_bytes(machine + dsh::game::kPawnControllerOffset, &controller,
                                          sizeof(controller))) {
        why = "APawn::Controller could not be READ -- the guarded read refused";
        return capsulePitchRad;
    }
    if (controller < 0x10000) {
        why = "this pawn has no controller (APawn::Controller reads 0), which is what a pawn looks "
              "like before it is possessed and after it is unpossessed";
        return capsulePitchRad;
    }
    float pitchDeg = 0.0f;
    if (!dsh::reflect::read_process_bytes(
            static_cast<std::uintptr_t>(controller) + dsh::game::kControllerControlRotationOffset,
            &pitchDeg, sizeof(pitchDeg))) {
        char hex[24];
        std::snprintf(hex, sizeof(hex), "%llX", static_cast<unsigned long long>(controller));
        why = std::string("the controller 0x") + hex +
              " is there, but its ControlRotation could not be READ";
        return capsulePitchRad;
    }
    readable = true;
    return pitchDeg * kDegToRad;
}

bool puppet_write_aim_pitch(std::uintptr_t machine, float pitchRad) {
    constexpr float kRadToDeg = 180.0f / 3.14159265358979f;
    const std::uint8_t byte = dsh::game::remote_view_pitch_byte(pitchRad * kRadToDeg);
    bool wrote = dsh::reflect::write_process_bytes(machine + dsh::game::kPawnRemoteViewPitchOffset, &byte,
                                                   sizeof(byte));
    std::uint64_t humanoid = 0;
    if (dsh::reflect::read_process_bytes(machine + kMachineHumanoidOffset, &humanoid, sizeof(humanoid)) &&
        humanoid >= 0x10000) {
        wrote = dsh::reflect::write_process_bytes(
                    static_cast<std::uintptr_t>(humanoid) + dsh::game::kPawnRemoteViewPitchOffset, &byte,
                    sizeof(byte)) ||
                wrote;
    }
    return wrote;
}

constexpr std::uintptr_t kProbeHeadsetOffset = 0x15E8;
constexpr std::uintptr_t kProbeLeftRearWeaponOffset = 0x1610;
constexpr std::uintptr_t kProbeRightRearWeaponOffset = 0x1618;
constexpr std::uintptr_t kProbeCurrentFrontWeaponOffset = 0x1630;
constexpr std::uintptr_t kWeaponLookAtOffset = 0x0228;
constexpr std::uintptr_t kLookAtTargetOffset = 0x01F8;

struct LookAtTargetView {
    bool  component = false;
    int   type = -1;
    float location[3] = {0.0f, 0.0f, 0.0f};
    bool  actor = false;

    bool  rotOk = false;
    float relativeRotation[3] = {0.0f, 0.0f, 0.0f};
    float disableOffsetRangeWeight = 0.0f;
    int   waitingType = -1;
    float yawSpeed = 0.0f;
    int   yawClamp = -1;
    float yawMin = 0.0f;
    float yawMax = 0.0f;
    float pitchSpeed = 0.0f;
};

LookAtTargetView read_lookat_target(std::uintptr_t weapon) {
    LookAtTargetView out;
    if (weapon < 0x10000) return out;
    std::uint64_t component = 0;
    if (!dsh::reflect::read_process_bytes(weapon + kWeaponLookAtOffset, &component, sizeof(component)) ||
        component < 0x10000) {
        return out;
    }
    out.component = true;
    const auto lookAt = static_cast<std::uintptr_t>(component);
    {
        float rotation[3] = {0.0f, 0.0f, 0.0f};
        if (dsh::reflect::read_process_bytes(lookAt + 0x0128, rotation, sizeof(rotation))) {
            std::memcpy(out.relativeRotation, rotation, sizeof(rotation));
            out.rotOk = true;
        }
    }
    (void)dsh::reflect::read_process_bytes(lookAt + 0x0248, &out.disableOffsetRangeWeight,
                                           sizeof(out.disableOffsetRangeWeight));
    std::uint8_t waiting = 0;
    if (dsh::reflect::read_process_bytes(lookAt + 0x0274, &waiting, sizeof(waiting))) {
        out.waitingType = static_cast<int>(waiting);
    }

    {
        float value = 0.0f;
        if (dsh::reflect::read_process_bytes(lookAt + 0x0264, &value, sizeof(value))) out.yawSpeed = value;
        std::uint8_t clampFlag = 0;
        if (dsh::reflect::read_process_bytes(lookAt + 0x0268, &clampFlag, sizeof(clampFlag))) {
            out.yawClamp = static_cast<int>(clampFlag);
        }
        if (dsh::reflect::read_process_bytes(lookAt + 0x026C, &value, sizeof(value))) out.yawMin = value;
        if (dsh::reflect::read_process_bytes(lookAt + 0x0270, &value, sizeof(value))) out.yawMax = value;
        if (dsh::reflect::read_process_bytes(lookAt + 0x0250, &value, sizeof(value))) out.pitchSpeed = value;
    }
    unsigned char block[0x28] = {};
    if (!dsh::reflect::read_process_bytes(lookAt + kLookAtTargetOffset, block, sizeof(block))) {
        return out;
    }
    out.type = static_cast<int>(block[0]);
    std::memcpy(out.location, block + 0x04, sizeof(out.location));
    std::uint64_t actor = 0;
    std::memcpy(&actor, block + 0x10, sizeof(actor));
    out.actor = actor >= 0x10000;
    return out;
}

float part_relative_yaw(std::uintptr_t part, float bodyYawDeg) {
    if (part < 0x10000) return 999.0f;
    dsh::reflect::ActorPlacement placement;
    std::string note;
    if (!dsh::reflect::read_actor_placement(part, placement, note)) return 999.0f;
    float relative = placement.yaw - bodyYawDeg;
    while (relative > 180.0f) relative -= 360.0f;
    while (relative < -180.0f) relative += 360.0f;
    return relative;
}

std::string relative_yaw_text(float value) {
    if (value > 900.0f) return "n/a";
    char text[24];
    std::snprintf(text, sizeof(text), "%+.1f", static_cast<double>(value));
    return text;
}

std::string lookat_text(const LookAtTargetView& view) {
    if (!view.component) return "no component";
    if (view.type < 0) return "component, block unreadable";
    char text[320];
    std::snprintf(text, sizeof(text),
                  "type %d at (%.0f, %.0f, %.0f)%s; comp rot %s (%.1f, %.1f, %.1f), weight %.2f, wait %d, "
                  "yaw(speed %.1f clamp %d %.1f..%.1f) pitch speed %.1f",
                  view.type, static_cast<double>(view.location[0]), static_cast<double>(view.location[1]),
                  static_cast<double>(view.location[2]), view.actor ? " on an ACTOR" : ", no actor",
                  view.rotOk ? "read" : "n/a", static_cast<double>(view.relativeRotation[0]),
                  static_cast<double>(view.relativeRotation[1]),
                  static_cast<double>(view.relativeRotation[2]),
                  static_cast<double>(view.disableOffsetRangeWeight), view.waitingType,
                  static_cast<double>(view.yawSpeed), view.yawClamp, static_cast<double>(view.yawMin),
                  static_cast<double>(view.yawMax), static_cast<double>(view.pitchSpeed));
    return text;
}

void log_aim_geometry(std::uintptr_t machine, const char* label, const char* rolePrefix) {
    if (machine < 0x10000) return;
    dsh::reflect::ActorPlacement body;
    std::string note;
    if (!dsh::reflect::read_actor_placement(machine, body, note)) {
        DSH_INFO("aim%s: %s: the machine's own rotation could not be read (%s)", rolePrefix, label,
                 note.c_str());
        return;
    }
    float controlYaw = 999.0f;
    std::uint64_t controller = 0;
    if (dsh::reflect::read_process_bytes(machine + dsh::game::kPawnControllerOffset, &controller,
                                         sizeof(controller)) &&
        controller >= 0x10000) {
        float yawDeg = 0.0f;
        if (dsh::reflect::read_process_bytes(
                static_cast<std::uintptr_t>(controller) + dsh::game::kControllerControlRotationOffset + 4,
                &yawDeg, sizeof(yawDeg))) {
            controlYaw = yawDeg;
        }
    }
    std::uint64_t humanoid = 0, headset = 0, front = 0, leftRear = 0, rightRear = 0;
    (void)dsh::reflect::read_process_bytes(machine + kMachineHumanoidOffset, &humanoid, sizeof(humanoid));
    (void)dsh::reflect::read_process_bytes(machine + kProbeHeadsetOffset, &headset, sizeof(headset));
    (void)dsh::reflect::read_process_bytes(machine + kProbeCurrentFrontWeaponOffset, &front, sizeof(front));
    (void)dsh::reflect::read_process_bytes(machine + kProbeLeftRearWeaponOffset, &leftRear,
                                           sizeof(leftRear));
    (void)dsh::reflect::read_process_bytes(machine + kProbeRightRearWeaponOffset, &rightRear,
                                           sizeof(rightRear));
    char controlText[32];
    if (controlYaw > 900.0f) {
        std::snprintf(controlText, sizeof(controlText), "n/a (no controller)");
    } else {
        std::snprintf(controlText, sizeof(controlText), "%.1f", static_cast<double>(controlYaw));
    }
    DSH_INFO("%saim: %s: control yaw %s, body yaw %.1f; relative yaw -- humanoid %s, headset %s, front weapon "
             "%s, left rear %s, right rear %s; shoulder LookAt -- left %s, right %s",
             rolePrefix, label, controlText, static_cast<double>(body.yaw),
             relative_yaw_text(part_relative_yaw(static_cast<std::uintptr_t>(humanoid), body.yaw)).c_str(),
             relative_yaw_text(part_relative_yaw(static_cast<std::uintptr_t>(headset), body.yaw)).c_str(),
             relative_yaw_text(part_relative_yaw(static_cast<std::uintptr_t>(front), body.yaw)).c_str(),
             relative_yaw_text(part_relative_yaw(static_cast<std::uintptr_t>(leftRear), body.yaw)).c_str(),
             relative_yaw_text(part_relative_yaw(static_cast<std::uintptr_t>(rightRear), body.yaw)).c_str(),
             lookat_text(read_lookat_target(static_cast<std::uintptr_t>(leftRear))).c_str(),
             lookat_text(read_lookat_target(static_cast<std::uintptr_t>(rightRear))).c_str());
}

constexpr const char* kHumanoidWatchVars[] = {"Yaw", "NewYaw", "Pitch", "NewPitch"};
constexpr const char* kLegWatchVars[] = {"TurnAngle", "TrunDirection", "CurrentAnimState",
                                         "StoredAnimationState", "Direction", "LastControllerRotationZ"};

using AnimWatchState = std::unordered_map<std::uintptr_t, std::unordered_map<std::string, std::string>>;

void watch_one_anim(std::uintptr_t instance, const char* label, const char* which, const char* rolePrefix,
                    const char* const* names, std::size_t nameCount, AnimWatchState& state,
                    std::uint32_t& linesLeft) {
    if (instance < 0x10000 || linesLeft == 0) return;
    std::uint64_t classRaw = 0;
    if (!dsh::reflect::read_process_bytes(instance + 0x0010, &classRaw, sizeof(classRaw)) ||
        classRaw < 0x10000) {
        return;
    }
    std::uintptr_t klass = static_cast<std::uintptr_t>(classRaw);
    std::unordered_map<std::string, std::string>& previous = state[instance];
    for (int depth = 0; klass >= 0x10000 && depth < 4; ++depth) {
        std::string note;

        const auto rows = dsh::reflect::class_property_values(klass, instance, 400, note);
        for (const std::string& row : rows) {
            for (std::size_t i = 0; i < nameCount; ++i) {
                const std::string quoted = std::string("'") + names[i] + "'";
                if (row.rfind(quoted, 0) != 0) continue;
                const auto it = previous.find(names[i]);
                if (it != previous.end() && it->second == row) break;
                previous[names[i]] = row;
                if (linesLeft == 0) return;
                --linesLeft;
                DSH_INFO("%s anim watch: %s: %s %s", rolePrefix, label, which, row.c_str());
                break;
            }
        }
        std::uint64_t super = 0;
        if (!dsh::reflect::read_process_bytes(klass + 0x0040, &super, sizeof(super))) return;
        klass = static_cast<std::uintptr_t>(super);
    }
}

void watch_anim_graph(std::uintptr_t owner, const char* label, const char* which, const char* rolePrefix,
                      const char* const* names, std::size_t nameCount, AnimWatchState& state,
                      std::uint32_t& linesLeft) {
    if (owner < 0x10000 || linesLeft == 0) return;
    std::uint64_t meshRaw = 0;
    if (!dsh::reflect::read_process_bytes(owner + 0x0280, &meshRaw, sizeof(meshRaw)) || meshRaw < 0x10000) {
        return;
    }
    const auto mesh = static_cast<std::uintptr_t>(meshRaw);
    std::uint64_t animRaw = 0;
    if (!dsh::reflect::read_process_bytes(mesh + 0x06B0, &animRaw, sizeof(animRaw)) || animRaw < 0x10000) {
        return;
    }
    watch_one_anim(static_cast<std::uintptr_t>(animRaw), label, which, rolePrefix, names, nameCount, state,
                   linesLeft);
    std::uint64_t linkedData = 0;
    std::uint32_t linkedNum = 0;
    if (!dsh::reflect::read_process_bytes(mesh + 0x0730, &linkedData, sizeof(linkedData)) ||
        !dsh::reflect::read_process_bytes(mesh + 0x0738, &linkedNum, sizeof(linkedNum))) {
        return;
    }
    if (linkedNum > 3) linkedNum = 3;
    for (std::uint32_t i = 0; i < linkedNum; ++i) {
        std::uint64_t instRaw = 0;
        const auto slot = static_cast<std::uintptr_t>(linkedData) + static_cast<std::uintptr_t>(i) * 8u;
        if (!dsh::reflect::read_process_bytes(slot, &instRaw, sizeof(instRaw)) || instRaw < 0x10000) continue;
        watch_one_anim(static_cast<std::uintptr_t>(instRaw), label, which, rolePrefix, names, nameCount, state,
                       linesLeft);
    }
}

void dump_anim_instance(std::uintptr_t inst, const char* label, const char* rolePrefix, const char* which,
                        std::uint32_t rowBudget) {
    std::uint64_t classRaw = 0;
    if (!dsh::reflect::read_process_bytes(inst + 0x0010, &classRaw, sizeof(classRaw)) ||
        classRaw < 0x10000) {
        DSH_INFO("animports%s: %s: %s 0x%llX has no readable class -- nothing asked", rolePrefix, label,
                 which, static_cast<unsigned long long>(inst));
        return;
    }
    DSH_INFO("animports%s: %s: %s '%s' [%s]", rolePrefix, label, which,
             dsh::reflect::object_own_name(inst).c_str(),
             dsh::reflect::object_class_name(inst).c_str());
    std::uintptr_t klass = static_cast<std::uintptr_t>(classRaw);
    for (int depth = 0; klass >= 0x10000 && depth < 4; ++depth) {
        std::string note;

        const auto lines = dsh::reflect::class_property_values(klass, inst, 400, note);
        std::vector<const std::string*> kept;
        for (const std::string& line : lines) {
            if (line.rfind("'AnimGraphNode_", 0) == 0) continue;
            if (line.rfind("'UberGraphFrame'", 0) == 0) continue;
            kept.push_back(&line);
        }
        const std::string headerNote = note.empty() ? std::string() : ("; " + note);
        DSH_INFO("animports%s: %s: %s class '%s' [%s] -- %zu row(s), %zu variable(s)%s", rolePrefix,
                 label, which, dsh::reflect::object_own_name(klass).c_str(),
                 dsh::reflect::object_class_name(klass).c_str(), lines.size(), kept.size(),
                 headerNote.c_str());
        for (const std::string* line : kept) {
            if (rowBudget == 0) break;
            DSH_INFO("animports%s: %s: %s %s", rolePrefix, label, which, line->c_str());
            --rowBudget;
        }
        if (rowBudget == 0) return;
        std::uint64_t super = 0;
        if (!dsh::reflect::read_process_bytes(klass + 0x0040, &super, sizeof(super))) return;
        klass = static_cast<std::uintptr_t>(super);
    }
}

void dump_anim_table_from(std::uintptr_t owner, const char* ownerRole, const char* label,
                          const char* rolePrefix, std::uint32_t rowBudget) {
    if (owner < 0x10000) {
        DSH_INFO("animports%s: %s: no %s actor, so there is no mesh to ask", rolePrefix, label, ownerRole);
        return;
    }
    std::uint64_t meshRaw = 0;
    if (!dsh::reflect::read_process_bytes(owner + 0x0280, &meshRaw, sizeof(meshRaw)) ||
        meshRaw < 0x10000) {
        DSH_INFO("animports%s: %s: %s '%s' [%s] has no Mesh at (read 0x%llX)", rolePrefix, label,
                 ownerRole, dsh::reflect::object_own_name(owner).c_str(),
                 dsh::reflect::object_class_name(owner).c_str(),
                 static_cast<unsigned long long>(meshRaw));
        return;
    }
    const auto mesh = static_cast<std::uintptr_t>(meshRaw);
    std::uint64_t animRaw = 0;
    if (!dsh::reflect::read_process_bytes(mesh + 0x06B0, &animRaw, sizeof(animRaw)) || animRaw < 0x10000) {
        DSH_INFO("animports%s: %s: mesh '%s' has no AnimScriptInstance at (read 0x%llX) -- this "
                 "%s is NOT running an animation graph, which is a different answer from a graph "
                 "that runs and holds zeros",
                 rolePrefix, label, dsh::reflect::object_own_name(mesh).c_str(),
                 static_cast<unsigned long long>(animRaw), ownerRole);
        return;
    }
    const auto anim = static_cast<std::uintptr_t>(animRaw);

    std::uint8_t remoteViewPitch = 0;
    dsh::reflect::read_process_bytes(owner + dsh::game::kPawnRemoteViewPitchOffset, &remoteViewPitch,
                                     sizeof(remoteViewPitch));
    DSH_INFO("animports%s: %s: %s '%s' -> mesh '%s' -> anim '%s' [%s]; RemoteViewPitch = %u",
             rolePrefix, label, ownerRole, dsh::reflect::object_own_name(owner).c_str(),
             dsh::reflect::object_own_name(mesh).c_str(), dsh::reflect::object_own_name(anim).c_str(),
             dsh::reflect::object_class_name(anim).c_str(), static_cast<unsigned>(remoteViewPitch));

    dump_anim_instance(anim, label, rolePrefix, "anim", rowBudget);

    std::uint64_t linkedData = 0;
    std::uint32_t linkedNum = 0;
    std::uint32_t linkedMax = 0;
    if (!dsh::reflect::read_process_bytes(mesh + 0x0730, &linkedData, sizeof(linkedData)) ||
        !dsh::reflect::read_process_bytes(mesh + 0x0738, &linkedNum, sizeof(linkedNum)) ||
        !dsh::reflect::read_process_bytes(mesh + 0x073C, &linkedMax, sizeof(linkedMax))) {
        DSH_INFO("animports%s: %s: LinkedInstances at is NOT READABLE", rolePrefix, label);
        return;
    }
    DSH_INFO("animports%s: %s: linked instances: Num=%u Max=%u (at)", rolePrefix, label,
             static_cast<unsigned>(linkedNum), static_cast<unsigned>(linkedMax));
    if (linkedNum > 3) linkedNum = 3;
    for (std::uint32_t i = 0; i < linkedNum; ++i) {
        std::uint64_t instRaw = 0;
        const auto slot = static_cast<std::uintptr_t>(linkedData) + static_cast<std::uintptr_t>(i) * 8u;
        if (!dsh::reflect::read_process_bytes(slot, &instRaw, sizeof(instRaw)) || instRaw < 0x10000) {
            DSH_INFO("animports%s: %s: linked[%u]: null or not readable", rolePrefix, label,
                     static_cast<unsigned>(i));
            continue;
        }
        dump_anim_instance(static_cast<std::uintptr_t>(instRaw), label, rolePrefix, "linked",
                           rowBudget / 2);
    }
}

void dump_anim_table(std::uintptr_t machine, const char* label, const char* rolePrefix) {
    if (machine < 0x10000) {
        DSH_INFO("animports%s: %s: no machine actor, so there is no humanoid to ask", rolePrefix, label);
        return;
    }
    std::uint64_t humanoidRaw = 0;
    if (!dsh::reflect::read_process_bytes(machine + kMachineHumanoidOffset, &humanoidRaw,
                                          sizeof(humanoidRaw)) ||
        humanoidRaw < 0x10000) {
        DSH_INFO("animports%s: %s: machine 0x%llX has no Humanoid at %llX (read 0x%llX)", rolePrefix,
                 label, static_cast<unsigned long long>(machine),
                 static_cast<unsigned long long>(kMachineHumanoidOffset),
                 static_cast<unsigned long long>(humanoidRaw));
        return;
    }

    dump_anim_table_from(static_cast<std::uintptr_t>(humanoidRaw), "humanoid", label, rolePrefix, 80);
    dump_anim_table_from(machine, "pawn", label, rolePrefix, 160);
}

enum class ActionModuleKind : int {
    kNone = 0,
    kGun = 1,
    kMelee = 2,
    kUtility = 3,
};

bool class_chain_reaches_any(std::uintptr_t cls, const char* const* names, std::size_t count) {
    for (int depth = 0; cls >= 0x10000 && depth < 32; ++depth) {
        const std::string own = dsh::reflect::object_own_name(cls);
        for (std::size_t i = 0; i < count; ++i) {
            if (own == names[i]) return true;
        }
        std::uint64_t super = 0;
        if (!dsh::reflect::read_process_bytes(cls + 0x0040, &super, sizeof(super))) return false;
        cls = static_cast<std::uintptr_t>(super);
    }
    return false;
}

struct ActionReplay {
    bool          ok = false;
    ActionModuleKind kind = ActionModuleKind::kNone;
    std::uintptr_t module = 0;
    std::string   className;
    dsh::reflect::FunctionRef call;
    std::string   note;
};

ActionReplay plan_action_replay(std::uintptr_t doll, std::uint32_t action, bool pressed) {
    static const char* const kMeleeClasses[] = {"BP_SingleMeleeActionFrontWeaponModuleBase_C",
                                                "BP_ChargeMeleeActionFrontWeaponModuleBase_C"};
    static const char* const kUtilityClasses[] = {"DAUtilityModule"};
    static const char* const kGunClasses[] = {"DAFrontWeaponModule", "DARearWeaponModule"};

    ActionReplay out;
    if (doll < 0x10000) {
        out.note = "this side holds no doll for that peer, so there is nothing to act with";
        return out;
    }
    std::uintptr_t slotOffset = 0;
    switch (action) {
        case 0: slotOffset = 0x1630; break;
        case 1: slotOffset = 0x1610; break;
        case 2: slotOffset = 0x1618; break;
        case 3: slotOffset = 0x1600; break;
        default:
            out.note = "action id " + std::to_string(action) + " is not one of the four the game declares";
            return out;
    }
    std::uint64_t moduleRaw = 0;
    if (!dsh::reflect::read_process_bytes(doll + slotOffset, &moduleRaw, sizeof(moduleRaw)) ||
        moduleRaw < 0x10000) {
        out.note = "the doll has NO module at 0x" + std::to_string(slotOffset) + " for that action";
        return out;
    }
    out.module = static_cast<std::uintptr_t>(moduleRaw);
    out.className = dsh::reflect::object_class_name(out.module);
    std::uint64_t classRaw = 0;
    if (!dsh::reflect::read_process_bytes(out.module + 0x0010, &classRaw, sizeof(classRaw)) ||
        classRaw < 0x10000) {
        out.note = "the module's class could not be read, so it cannot be classified";
        return out;
    }
    const auto moduleClass = static_cast<std::uintptr_t>(classRaw);
    if (action == 3) {
        out.kind = class_chain_reaches_any(moduleClass, kUtilityClasses, 1) ? ActionModuleKind::kUtility
                                                                          : ActionModuleKind::kNone;
    } else if (class_chain_reaches_any(moduleClass, kMeleeClasses, 2)) {
        out.kind = ActionModuleKind::kMelee;
    } else if (class_chain_reaches_any(moduleClass, kGunClasses, 2)) {
        out.kind = ActionModuleKind::kGun;
    }
    if (out.kind == ActionModuleKind::kNone) {
        out.note = "the module here is not an action module this build knows ('" + out.className + "')";
        return out;
    }
    if (out.kind == ActionModuleKind::kGun) {
        out.note = "this is a GUN ('" + out.className +
                   "'), and its shot already travels as the projectile copy -- replaying the trigger here "
                   "would fire it twice (v412)";
        return out;
    }

    static const char* const kPressNames[4] = {"OnPressedActionFront", "OnPressedActionLeft",
                                               "OnPressedActionRight", "OnPressedActionUtility"};
    static const char* const kReleaseNames[4] = {"OnReleasedActionFront", "OnReleasedActionLeft",
                                                 "OnReleasedActionRight", "OnReleasedActionUtility"};
    static std::uintptr_t s_machineClass = 0;
    if (s_machineClass == 0) {
        s_machineClass = dsh::reflect::resolve_uclass_address("DAMachine");
    }
    if (s_machineClass == 0) {
        out.note = "the DAMachine UClass is not in the object array, so the verb cannot be resolved";
        return out;
    }
    const char* verb = (pressed ? kPressNames : kReleaseNames)[action];
    out.call = dsh::reflect::reflect_resolve_function(s_machineClass, verb);
    if (!out.call.usable) {
        out.note = std::string("no UFunction named '") + verb + "' is reachable from DAMachine";
        return out;
    }
    out.ok = true;
    return out;
}

}

void request_parts_dump() { g_partsDumpRequested.store(true, std::memory_order_relaxed); }
void request_parts_hide() { g_partsHideRequested.store(true, std::memory_order_relaxed); }
void request_parts_fix() { g_partsFixRequested.store(true, std::memory_order_relaxed); }

PuppetSessionResult run_puppet_session(bool asHost, const std::string& address, std::uint16_t port,
                                       int seconds) {
    PuppetSessionResult r;

    if (seconds < 0) seconds = 0;

    g_sessionStopRequested = false;
    g_sessionStopReason.clear();

    g_ammoCreatedPublished.store(0, std::memory_order_release);

    constexpr std::uint8_t kMaxLabel = 8;
    static const char* const kHostLabel = "puppetsession[host]:";
    static const char* const kLabels[kMaxLabel + 1] = {
        "puppetsession[client]:",  "puppetsession[client1]:", "puppetsession[client2]:",
        "puppetsession[client3]:", "puppetsession[client4]:", "puppetsession[client5]:",
        "puppetsession[client6]:", "puppetsession[client7]:", "puppetsession[client8]:",
    };
    const char* rolePrefix = asHost ? kHostLabel : kLabels[0];
    bool        roleLabelled = asHost;

    const std::string announcedName =
        asHost ? std::string("puppet-host") : ("client-" + std::to_string(GetCurrentProcessId()));

    auto transport = dsh::net::make_udp_transport();
    if (!transport) {
        r.detail = "no UDP transport";
        DSH_ERROR("puppetsession: %s", r.detail.c_str());
        return r;
    }

    if (!transport->listen("", asHost ? port : 0)) {
        r.detail = "could not bind the UDP port";
        DSH_ERROR("puppetsession: %s", r.detail.c_str());
        return r;
    }

    transport->start_background();

    dsh::net::SessionConfig cfg;
    cfg.playerName = announcedName;
    dsh::net::Session session(cfg);
    session.set_send_function([&](const dsh::net::Endpoint& to, std::span<const std::uint8_t> bytes) {
        transport->send(to, bytes);
    });

    if (asHost) {
        session.become_host(dsh::PeerId{0});
        std::uint16_t boundPort = port;
        const auto local = transport->local_endpoint();
        if (local.bytes.size() == sizeof(sockaddr_in)) {
            sockaddr_in in{};
            std::memcpy(&in, local.bytes.data(), sizeof(in));
            boundPort = ntohs(in.sin_port);
        }
        DSH_INFO("puppetsession: HOST listening on UDP port %u -- the other machine joins that port",
                 boundPort);
    } else {
        if (address.empty() || port == 0) {
            r.detail = "expected an address and a port to join";
            DSH_ERROR("puppetsession: %s", r.detail.c_str());
            transport->stop_background();
        transport->stop_background();
    transport->close();
            return r;
        }
        sockaddr_in hostAddr{};
        hostAddr.sin_family = AF_INET;
        hostAddr.sin_port = htons(port);

        if (inet_pton(AF_INET, address.c_str(), &hostAddr.sin_addr) != 1) {
            r.detail = "could not parse the address '" + address + "'";
            DSH_ERROR("puppetsession: %s", r.detail.c_str());
            transport->close();
            return r;
        }
        session.become_client(dsh::PeerId{1}, make_endpoint(hostAddr));
        DSH_INFO("puppetsession: CLIENT joining %s:%u -- announcing itself as %s; the host will answer "
                 "with the number this client is, and every line below will then say client<N>",
                 address.c_str(), port, announcedName.c_str());
    }

    std::vector<std::vector<std::uint8_t>> appearanceChunks;

    dsh::reflect::DollCustomize dollCustomize;
    std::string customizeNote;

    dsh::reflect::MachineLoadout loadout;
    bool haveLoadout = false;
    std::string loadoutNote;
    dsh::reflect::MachineLoadout puppetLoadout;
    bool havePuppetLoadout = false;

    dsh::game::PawnHandle pawn;

    std::uintptr_t stanceCharacterClass = 0;
    bool stanceClassResolved = false;
    bool stanceClassCheckLogged = false;
    std::uintptr_t stanceClassCheckedPawn = 0;

    DollStance stanceSent;
    std::uint64_t stanceDatagrams = 0;

    bool stateSizeMismatchLogged = false;

    bool                      haveInput = false;
    dsh::game::MovementInputs hostInput;
    dsh::game::MovementInputs hostInputSent;
    bool                      inputReadRefusalLogged = false;

    std::uint64_t inputDatagramsNonZero = 0;
    std::uint64_t inputDatagramsNonZeroThisWindow = 0;
    std::uint64_t boostDatagramsNonZero = 0;
    std::uint64_t boostDatagramsNonZeroThisWindow = 0;

    bool                  haveEnergy = false;
    dsh::game::EnergyTimers hostEnergy;
    dsh::game::EnergyTimers hostEnergySent;

    dsh::game::EnergyTimers energyLastReceived;
    bool                  energyLastReceivedReadable = false;

    bool                  energyPairEverNonZero = false;

    bool                  energyLastStepKnown = false;
    float                 energyLastStep = 0.0f;
    bool                  energyReadRefusalLogged = false;
    std::uint64_t         energyDatagramsNonZero = 0;
    std::uint64_t         energyDatagramsNonZeroThisWindow = 0;
    std::uint64_t         energyApplied = 0;
    std::uint64_t         energyChanged = 0;
    std::uint64_t         energyFailed = 0;
    dsh::game::EnergyTimerApplyResult energyLastApplied;
    int                   energyNoteCount = 0;

    bool                  energyHavePrevious = false;
    float                 energyPreviousIntermission = 0.0f;

    bool                  energySentHavePrevious = false;
    float                 energySentPreviousIntermission = 0.0f;

    bool                  clientEnergyHavePrevious = false;
    float                 clientEnergyPreviousIntermission = 0.0f;

    bool                  clientAppliedHavePrevious = false;
    float                 clientAppliedPreviousIntermission = 0.0f;

    std::string           energyLastNote;

    bool                        haveWeapon = false;
    dsh::game::FrontWeaponState hostFrontWeapon;
    std::uint8_t                hostFrontWeaponIndex = kFrontWeaponNotRead;
    bool                        weaponReadRefusalLogged = false;

    bool                        aimPitchRefusalLogged = false;
    std::uint64_t               aimPitchWrites = 0;
    std::uint64_t               weaponDatagramsDrawn = 0;
    std::uint64_t               weaponDatagramsNotRead = 0;

    std::uintptr_t              weaponMachineClass = 0;
    bool                        weaponClassResolved = false;
    std::uintptr_t              weaponClassCheckedPawn = 0;
    std::uintptr_t              puppetWeaponClass = 0;
    {

        constexpr std::uintptr_t kMachineAssembleDataOffset = 0x1308;

        dsh::sync::PoseSample customizePose;
        std::string locateNote;
        std::uint64_t nextLocateForCustomize = 0;
        if (!sample_local_pawn(pawn, customizePose, locateNote, GetTickCount64(),
                               nextLocateForCustomize)) {
            customizeNote = "no local pawn to read the customisation from: " + locateNote;
            DSH_WARN("puppetsession: %s -- the puppet keeps the colours this build had before",
                     customizeNote.c_str());
        } else {
            const std::string machineClassName = dsh::reflect::object_class_name(pawn.pawn);
            std::uint64_t humanoidValue = 0;
            const bool humanoidRead =
                dsh::reflect::read_process_bytes(pawn.pawn + kMachineHumanoidOffset, &humanoidValue,
                                                 sizeof(humanoidValue));
            const auto humanoid = static_cast<std::uintptr_t>(humanoidValue);
            const std::string humanoidClassName = humanoid >= 0x10000
                                                      ? dsh::reflect::object_class_name(humanoid)
                                                      : std::string("<null>");
            DSH_INFO("puppetsession: the local machine is '%s' (0x%llX); its Humanoid module is "
                     "'%s' (0x%llX)", machineClassName.c_str(),
                     static_cast<unsigned long long>(pawn.pawn), humanoidClassName.c_str(),
                     static_cast<unsigned long long>(humanoid));

            if (!humanoidRead || humanoid < 0x10000 ||
                !dsh::reflect::actor_is_doll(humanoid)) {
                customizeNote = "the machine '" + machineClassName + "' reports its Humanoid module as '" +
                                humanoidClassName + "'";
                DSH_WARN("puppetsession: %s (0x%llX), which is not an ADAHumanoid -- the "
                         "customisation cannot be read; the puppet keeps the colours this build had "
                         "before", customizeNote.c_str(),
                         static_cast<unsigned long long>(humanoid));
            } else if (dsh::reflect::capture_doll_customize(humanoid, dollCustomize,
                                                            customizeNote)) {
                DSH_INFO("puppetsession: the doll's customisation was captured from the Humanoid "
                         "module -- 13 colours and 5 body-shape values (%zu bytes on the wire)",
                         static_cast<std::size_t>(13 * 4 * 4 + 5 * 4));
            } else {
                DSH_WARN("puppetsession: the doll's customisation could not be read (%s) -- the "
                         "puppet keeps the colours this build had before", customizeNote.c_str());
            }

            DSH_INFO("puppetsession: the machine's AssembleData begins: %s",
                     hex_preview(pawn.pawn + kMachineAssembleDataOffset, 64).c_str());

            if (!dsh::reflect::capture_machine_loadout(pawn.pawn, loadout, loadoutNote)) {
                haveLoadout = false;
                DSH_WARN("puppetsession: the machine's loadout could NOT be read (%s) -- the other "
                         "side would have nothing to spawn, and this session can only report that",
                         loadoutNote.c_str());
            } else {
                haveLoadout = true;
                std::string slotList;
                for (int i = 0; i < 9; ++i) {
                    if (i != 0) slotList += ", ";
                    slotList += dsh::reflect::machine_slot_name(i);
                    slotList += '=';
                    const dsh::reflect::MachineLoadout::Slot& slot = loadout.slots[i];
                    if (!slot.present) {
                        slotList += '-';
                        continue;
                    }
                    slotList += slot.className + " (level " + std::to_string(slot.level) + ")";
                }
                DSH_INFO("puppetsession: the machine's loadout -- class '%s', FDATriggerData (%u, %u, "
                         "%u), FDAAssembleData: %s; FDACustomizeData %s; FDAAssembleData::ColoringData "
                         "(the ARMOUR SHELL) %s",
                         loadout.className.c_str(), static_cast<unsigned>(loadout.trigger[0]),
                         static_cast<unsigned>(loadout.trigger[1]),
                         static_cast<unsigned>(loadout.trigger[2]), slotList.c_str(),
                         loadout.customizePresent
                             ? "read (13 colours, 5 body-shape values, 4 part class(es), 3 flags)"
                             : "NOT readable -- the game will merge the class's default body",
                         loadout.moduleColoringPresent
                             ? "read (9 modules x Main/Sub/Inner/Glow = 576 bytes)"
                             : "NOT readable -- the shell will keep the class's default colours");
                if (!loadoutNote.empty()) {

                    DSH_WARN("puppetsession: the loadout was read with a caveat: %s",
                             loadoutNote.c_str());
                }
            }
        }

        if (!haveLoadout) loadout = dsh::reflect::MachineLoadout{};
        const std::vector<std::uint8_t> manifest =
            dsh::game::encode_loadout_manifest(&loadout);
        appearanceChunks = dsh::game::chunk_appearance(manifest, manifest_id_of(manifest));
        DSH_INFO("puppetsession: appearance manifest is %zu byte(s) in %zu chunk(s), carrying this "
                 "side's own machine loadout '%s' (%d of 9 slot(s) filled, FDACustomizeData %s, the "
                 "armour shell's 576 colour bytes %s)",
                 manifest.size(), appearanceChunks.size(),
                 haveLoadout ? loadout.className.c_str() : "<none>", loadout.slot_count(),
                 loadout.customizePresent ? "present" : "ABSENT",
                 loadout.moduleColoringPresent ? "present" : "ABSENT");
    }

    dsh::game::PuppetDolls dolls;
    bool puppetBuilt = false;

    bool enteredLoop = false;

    std::vector<std::uintptr_t> puppetActors;

    bool ammoProbeDone[kAmmoProbeSlots] = {};

    struct DollDiag {

        float         integBase[3] = {0.0f, 0.0f, 0.0f};
        bool          integHave = false;
        std::uint64_t integComparisons = 0;
        std::uint64_t integComparisonsWindow = 0;
        std::uint64_t integMoves = 0;
        std::uint64_t integMovesWindow = 0;
        float         integMaxCm = 0.0f;
        float         integMaxWindowCm = 0.0f;
        float         integLastCm = -1.0f;

        std::uint64_t modeWrittenWindow = 0;
        std::uint64_t modeAppliedWindow = 0;
        std::uint64_t modeRevertedWindow = 0;
        std::uint64_t modeRefusedWindow = 0;
        std::uint64_t modeWritten = 0;
        std::uint64_t modeApplied = 0;
        std::uint64_t modeReverted = 0;

        float         velocityApplied[3] = {0.0f, 0.0f, 0.0f};
        std::uint64_t velocityAppliedWindow = 0;
        std::uint64_t survivorHitsWindow = 0;
        std::uint64_t survivorMissesWindow = 0;

        std::uint8_t  peerModeAtLastApply = 0;

        std::uint8_t                  weaponLastReceivedIndex = kFrontWeaponNotRead;
        bool                          weaponLastReceivedValid = false;
        std::uint64_t                 weaponStated = 0;
        std::uint64_t                 weaponCalls = 0;
        std::uint64_t                 weaponApplied = 0;
        std::uint64_t                 weaponHolding = 0;
        std::uint64_t                 weaponFailed = 0;
        std::uint64_t                 weaponNotRead = 0;
        dsh::game::FrontWeaponApplyResult weaponLastApplied;
        int                           weaponNoteCount = 0;
        std::string                   weaponLastNote;

        std::uint64_t exclusiveStarted = 0;
        std::uint64_t exclusiveEnded = 0;
        std::uint64_t exclusiveRefused = 0;
        std::uint64_t exclusiveFailed = 0;
        int           exclusiveNoteCount = 0;

        dsh::game::MachineMovementState probe;
        float                          windowMaxSpeed = 0.0f;

        bool         initSampled = false;
        bool         initValue = false;
        std::string  initNote;
    };
    std::array<DollDiag, dsh::game::PuppetDolls::kSlots> dollDiag{};

    const auto doll_diag = [&dollDiag](dsh::PeerId peer) -> DollDiag& {
        const std::size_t slot = peer.value < dollDiag.size() ? peer.value : 0;
        return dollDiag[slot];
    };
    bool placedOnce = false;
    bool warnedNoGameThread = false;

    std::uint64_t consecutivePlacementDrops = 0;
    std::vector<dsh::net::Datagram> inbox;
    const std::uint64_t runStart = GetTickCount64();

    const std::uintptr_t sessionWorld = dsh::reflect::live_world_address();
    DSH_INFO("puppetsession: session start -- the live world is at 0x%llX. This value is printed so that a "
             "map change can be TOLD APART from a map change that reuses the same UWorld: the crash this "
             "guard was written for happened on a transition, and whether the world pointer moves is the "
             "measurement that decides what the guard should watch.",
             static_cast<unsigned long long>(sessionWorld));
    std::uint64_t nextManifest = runStart;
    std::uint64_t lastPeerLog = runStart;

    int animProbeDumps = 0;
    bool animProbeSawPuppet = false;

    AnimWatchState animWatch;
    std::uint32_t animWatchLinesLeft = 300;

    bool          missionListAsked   = false;
    bool          missionListArrived = false;
    std::uint64_t nextMissionListAsk = GetTickCount64() + 1500;
    std::uint32_t missionListSequence = 0;
    std::uint32_t missionListRequestAttempts = 0;

    std::vector<std::uint8_t> manifestAckedPeers;
    std::uint64_t             nextManifestAckMs = 0;
    std::uint32_t             manifestAckSequence = 1;
    std::uint64_t             manifestRepeats = 0;

    const std::uintptr_t                 sessionPawn = pawn.valid ? pawn.pawn : 0;

    std::uint32_t healthBroadcastNothingSaid = 0;
    std::uint32_t healthBroadcastsReceived = 0;
    std::uint32_t healthReportsReceived = 0;
    std::uint32_t healthReportsRefused = 0;
    std::uint32_t healthEntriesAppliedOnHost = 0;

    std::uint64_t nextTargetModelMs = runStart;
    std::uint64_t nextTargetModelReportMs = runStart;
    std::uint32_t targetModelForced = 0;
    std::uint32_t targetModelVerified = 0;
    std::uint32_t targetModelRefused = 0;
    std::uint32_t targetModelReports = 0;
    std::string   targetModelPuppetName;
    std::uint32_t healthBroadcastsRefused = 0;
    std::uint32_t healthEntriesApplied = 0;
    std::uint32_t healthEntriesFailedToWrite = 0;
    std::uint32_t healthEntriesNotInMyLevels = 0;    constexpr std::uint64_t kHealthBroadcastFullMs = 1000;
    std::uint64_t nextHealthBroadcast = runStart;
    std::uint64_t nextHealthBroadcastFull = runStart;
    constexpr std::uint64_t kHealthReportMs = 100;
    std::uint64_t nextHealthReportSend = runStart;
    std::uint32_t healthReportSequence = 1;
    std::uint32_t healthReportsSent = 0;
    std::uint32_t healthEntriesReported = 0;
    std::uint32_t healthBroadcastFulls = 0;
    std::uint32_t healthBroadcastSequence = 1;
    std::uint32_t healthBroadcastsSent = 0;
    std::uint32_t healthEntriesSent = 0;
    bool          healthBroadcastForceAll = true;

    constexpr std::uint64_t kPlayerHealthSendMs = 5000;
    std::uint64_t nextPlayerHealthSend = runStart;
    std::uint32_t playerHealthSequence = 1;
    std::uint32_t playerHealthSent = 0;
    std::uint32_t playerHealthSendRefusals = 0;
    std::uint32_t playerHealthReceived = 0;
    std::uint32_t playerHealthRefusedAtWire = 0;
    std::uint32_t playerHealthWritten = 0;
    std::uint32_t playerHealthAlreadyRight = 0;
    std::uint32_t playerHealthWriteFailures = 0;
    std::uint32_t playerHealthNoPuppet = 0;
    std::uint32_t playerHealthWriteLines = 0;

    DSH_INFO("puppetsession: %s", dsh::reflect::liveness_selfcheck_report().c_str());

    std::uint64_t stateEpoch = steady_ms();
    std::uint64_t stateTick = 0;
    std::uint64_t stateSkipped = 0;
    std::uint64_t nextPawnLocate = runStart;
    std::uint64_t nextNoPoseLog = runStart;
    std::uint64_t stateDatagrams = 0;

    std::vector<std::uint8_t> lastPosePayload;
    std::uint32_t poseRepeatStreak = 0;

    std::uint64_t manifestDatagrams = 0;

    std::uint64_t dollRefused = 0;

    std::uint64_t ackNoteCount = 0;

    std::uint64_t relayCandidates = 0;

    std::uint64_t manifestReplayed = 0;
    std::uint64_t manifestReplayLines = 0;

    std::uint64_t nextModulePlacementMs = 0;
    std::uint64_t nextModuleTableMs = 0;
    std::uint64_t moduleSlotLines = 0;
    std::uint64_t moduleAttachLines = 0;
    std::uint64_t moduleOrphanLines = 0;
    std::uint64_t moduleWatchScans = 0;
    std::uint64_t moduleOrphansSeen = 0;
    std::uint64_t moduleOwnedCandidates = 0;
    std::uint64_t moduleTransientSkipped = 0;
    struct ModuleSlotMemory {
        std::uintptr_t machine = 0;
        std::uintptr_t slot[kModuleSlotCount] = {};
        std::string    slotClass[kModuleSlotCount];
        bool           valid = false;
    };
    ModuleSlotMemory moduleSlotMemory[4];

    std::vector<std::uintptr_t> moduleLevelActors;
    std::vector<std::uintptr_t> moduleLevelScratch;

    ComponentCensus moduleCensus;
    std::uint64_t   moduleCensusLines = 0;
    std::uint64_t   modulePartsLines = 0;

    std::uint64_t   moduleStrayBodies = 0;

    struct HideTarget {
        std::uintptr_t component = 0;
        bool           wasHidden = false;
        std::string    what;
    };
    std::vector<HideTarget> hideCycle;
    std::size_t             hideStep = 0;
    std::uintptr_t          hideHeld = 0;
    std::uint64_t           hideUntilMs = 0;
    std::vector<HideTarget> fixTargets;
    std::uint64_t           fixUntilMs = 0;

    std::uint64_t nextFallWatchMs = 0;
    bool          fallWatchArmed = false;

    int deathLines = 0;
    bool          buildAttempted = false;
    int           modulesMerged = 0;

    std::uint64_t placementsApplied = 0;
    std::uint64_t placementsDropped = 0;

    std::uint64_t nextInterpApplyMs = 0;
    std::uint64_t interpApplied = 0;
    std::uint64_t interpDropped = 0;

    std::uint32_t actionSequence = 1;
    std::uint64_t actionEdgesSent = 0;
    std::uint64_t actionEdgesReceived = 0;
    std::uint64_t actionApplied = 0;
    std::uint64_t actionRefused = 0;
    std::uint64_t actionNoteCount = 0;

    std::uint64_t itemUsesSent = 0;
    std::uint64_t itemUsesReceived = 0;
    std::uint64_t itemUsesReplayed = 0;
    std::uint32_t itemUseSequence = 1;
    std::uint64_t itemNoteCount = 0;

    std::uint64_t supplyNoteCount = 0;
    std::uint64_t shotEventsReceived = 0;
    std::uint64_t actionDuplicates = 0;
    std::uint64_t actionIgnored = 0;
    bool modulePropsDumped = false;

    std::uint32_t ammoSequence = 1;
    std::uint64_t ammoSent = 0;
    std::uint64_t ammoReceived = 0;
    std::uint64_t ammoDecodeFailed = 0;
    std::uint64_t ammoNoteCount = 0;
    bool          firstAmmoLinePrinted = false;

    std::uint32_t ammoHomingCaptured = 0;
    std::uint32_t ammoHomingReproduced = 0;
    std::uint32_t ammoHomingSenderRefused = 0;
    std::uint32_t ammoHomingNameMissing = 0;
    std::uint32_t ammoHomingNoComponent = 0;
    std::uint32_t ammoHomingRefusedNpc = 0;
    std::uint32_t ammoHomingLines = 0;
    std::uint32_t ammoHomingProbeLines = 0;
    std::uint32_t ammoHomingNpcLines = 0;

    std::uint64_t ammoCopiesBornHarmless = 0;
    std::uint32_t ammoDamageLines = 0;

    std::unordered_map<std::uintptr_t, std::string> ammoPathCache;

    bool ammoFunctionResolved = false;

    std::uint64_t stanceApplied = 0;
    std::uint64_t stanceFailed = 0;
    std::uint64_t stanceChanged = 0;

    int           stanceNoteCount = 0;

    std::uintptr_t stanceClass = 0;

    int         shortStatePayloads = 0;
    std::string lastStateDecodeNote;

    DollStance  stanceLastApplied;

    GetterCapture hostDerivedGetters;
    GetterCapture puppetDerivedGetters;

    std::uint64_t sprintEffectHandle = 0;
    std::uintptr_t sprintEffectActor = 0;
    std::uint64_t sprintEffectApplies = 0;
    std::uint64_t sprintEffectRefusals = 0;
    std::uint64_t sprintEffectRemovals = 0;
    std::uint64_t sprintEffectRemovalFailures = 0;
    std::uint64_t sprintEffectRetryWarnings = 0;

    std::atomic<bool> sprintEdgeForAnimation{false};

    bool sprintDelegatesPrinted = false;

    bool exclusiveDelegatesPrinted = false;

    bool              animProbeRearmed = false;

    dsh::game::MachineMovementState hostMovement;
    float hostWindowMaxSpeed = 0.0f;

    int movementTicksSinceSample = 0;

    float       hostVelocitySent[3] = {0.0f, 0.0f, 0.0f};
    std::uint8_t hostModeSent = 0;
    std::uint64_t velocityDatagramsNonZero = 0;
    std::uint64_t velocityDatagramsNonZeroThisWindow = 0;
    float       velocityLastApplied[3] = {0.0f, 0.0f, 0.0f};

    std::uint64_t velocityApplied = 0;
    std::uint64_t velocitySurvivorHits = 0;
    std::uint64_t velocitySurvivorMisses = 0;
    std::uint64_t velocityFailed = 0;
    std::uint64_t velocityChanged = 0;

    std::uint64_t accelerationApplied = 0;
    std::uint64_t accelerationChanged = 0;
    std::uint64_t accelerationFailed = 0;
    float         accelerationLastApplied[3] = {0.0f, 0.0f, 0.0f};
    int           accelerationNoteCount = 0;

    int velocityNoteCount = 0;

    std::uint64_t inputFlagsApplied = 0;

    std::uint64_t puppetFlagMoves = 0;
    std::uint64_t inputFlagsChanged = 0;
    std::uint64_t inputFlagsFailed = 0;
    std::uint64_t inputFlagsChangedBoost = 0;
    std::uint64_t inputVectorApplied = 0;
    std::uint64_t inputVectorChanged = 0;
    std::uint64_t inputVectorFailed = 0;

    dsh::game::MovementInputs inputLastReceived;
    dsh::game::MovementInputs inputLastWritten;

    int inputNoteCount = 0;

    std::uint64_t sprintCallsThisWindow = 0;
    std::uint64_t sprintRefusedThisWindow = 0;
    std::uint64_t sprintByteMatchedThisWindow = 0;
    std::uint64_t sprintByteRevertedThisWindow = 0;
    std::uint64_t sprintRawAgreedThisWindow = 0;
    std::uint64_t sprintDerivedAgreedThisWindow = 0;
    std::uint64_t sprintCalls = 0;
    std::uint64_t sprintRefused = 0;
    std::uint64_t sprintByteMatched = 0;
    std::uint64_t sprintByteReverted = 0;
    std::uint64_t sprintRawAgreed = 0;
    std::uint64_t sprintDerivedAgreed = 0;

    SprintCapture sprintLast;

    MovementFlagEdges flagEdges;
    MovementFlagEdges flagEdgesThisWindow;
    dsh::game::MovementInputs flagEdgesPrevious;

    movementTicksSinceSample = kMovementTicksPerSample;

    const std::uintptr_t movementStateClass = dsh::reflect::resolve_uclass_address(kMovementApiClass);
    if (movementStateClass == 0) {
        DSH_WARN("puppetsession: the '%s' class is not in the object array, so no machine's "
                 "class chain can be checked and NO movement state is read -- the movement line will "
                 "say so rather than print a movement mode", kMovementApiClass);
    }

    const std::uintptr_t thrusterModuleClass = dsh::reflect::resolve_uclass_address(kWalkThrusterClass);
    const std::uintptr_t niagaraComponentClass = dsh::reflect::resolve_uclass_address(kWalkNiagaraClass);

    const std::uintptr_t derivedMovementClass =
        dsh::reflect::resolve_uclass_address(kDerivedMovementApiClass);
    if (derivedMovementClass == 0) {
        DSH_WARN("puppetsession: the '%s' class is not in the object array, so the block will NOT "
                 "be read as that class's fields by the differential watch -- the watch's own summary says "
                 "so on every line (the other class names this session needs are unaffected)",
                 kDerivedMovementApiClass);
    }

    bool moduleWalkClassRefusalLogged = false;

    std::uintptr_t localPawnMachine = 0;
    std::string    localPawnRefusal = "the local pawn has not been looked for yet";

    dsh::game::ObservedWatch watch;
    dsh::game::WatchModuleKey watchModuleKey;
    bool          watchStarted = false;
    std::uint64_t watchBeginMillis = 0;
    std::uint64_t watchBeginDatagram = 0;
    std::vector<dsh::game::WatchChange> watchTrace;

    {
        const auto resolveOn = [](std::uintptr_t cls, const char* name) -> std::uintptr_t {
            if (cls < 0x10000) return 0;
            const dsh::reflect::FunctionRef ref = dsh::reflect::reflect_resolve_function(cls, name);
            return ref.usable ? ref.function : 0;
        };
        const std::uintptr_t checkpointDialogueFn = resolveOn(
            dsh::reflect::resolve_uclass_address("DADialogueTask_Checkpoint"), "DialogueCheckpoint");
        const std::uintptr_t regressionFn = resolveOn(
            dsh::reflect::resolve_uclass_address("DAPersistentSaveGame"),
            "SetRegressionPointFromCheckpointTag");
        const std::uintptr_t saveFn =
            resolveOn(dsh::reflect::resolve_uclass_address("DAGameMode"), "Save");
        dsh::game::set_checkpoint_functions(checkpointDialogueFn, regressionFn, saveFn);
        if (checkpointDialogueFn != 0 || regressionFn != 0 || saveFn != 0) {
            DSH_INFO("puppetsession: the checkpoint watch is ARMED -- dialogue 0x%llX, regression point 0x%llX, "
                     "save 0x%llX. A rest point used during this session ends it BEFORE the world is reset, "
                     "and the log names which of the three saw it",
                     static_cast<unsigned long long>(checkpointDialogueFn),
                     static_cast<unsigned long long>(regressionFn),
                     static_cast<unsigned long long>(saveFn));
        } else {
            DSH_WARN("puppetsession: the checkpoint watch could NOT be armed -- none of 'DialogueCheckpoint', "
                     "'SetRegressionPointFromCheckpointTag' or 'DAGameMode::Save' resolved, so a rest point "
                     "will NOT end this session by itself. The world guard still ends it when the supply "
                     "reloads the level");
        }
    }

    const std::uintptr_t dialogueSystem = dsh::reflect::find_object_of_class("DADialogueSystem");
    if (dialogueSystem >= 0x10000) {
        DSH_INFO("puppetsession: the dialogue subsystem is at 0x%llX -- the rest-point detector polls its "
                 "CurrentCheckpoint, which is the only place this game keeps 'a checkpoint menu is "
                 "open'", static_cast<unsigned long long>(dialogueSystem));
    } else {
        DSH_WARN("puppetsession: 'DADialogueSystem' was not found in the object table, so a rest point will "
                 "NOT end this session by itself -- the world guard is the only thing left");
    }
    std::uint64_t lastDialoguePointer = 0;

    g_sessionLive.store(true, std::memory_order_release);

    std::uint64_t goneSinceMs[dsh::game::PuppetDolls::kSlots] = {};

    while (seconds <= 0 ||
           GetTickCount64() - runStart < static_cast<std::uint64_t>(seconds) * 1000) {
        const std::uint64_t now = GetTickCount64();

        const std::uint64_t nowSteady = steady_ms();

        if (!roleLabelled) {
            const auto& peersNow = session.peers();

            if (!peersNow.empty() && peersNow.front().state == dsh::net::PeerState::connected) {
                const std::uint8_t assigned = session.local_id().value;
                if (assigned >= 1 && assigned <= kMaxLabel) {
                    rolePrefix = kLabels[assigned];
                    roleLabelled = true;
                    DSH_INFO("%s this side is client%u -- the host assigned it that number, and this session's "
                             "lines say so from here on (it announced itself as '%s')",
                             rolePrefix, static_cast<unsigned>(assigned), announcedName.c_str());
                }
            }
        }

        {
            static std::uint64_t s_lastIterMs = 0;
            static std::uint64_t s_iterCount = 0;
            static std::uint64_t s_iterMax = 0;
            static std::uint64_t s_reportMs = 0;
            if (s_lastIterMs != 0) {
                const std::uint64_t delta = now - s_lastIterMs;
                ++s_iterCount;
                if (delta > s_iterMax) s_iterMax = delta;

                if (delta >= 800) {
                    DSH_WARN("STALL: an iteration took %llu ms and ended at %llu ms of the session (the pose gate "
                             "asks for %llu ms)",
                             static_cast<unsigned long long>(delta), static_cast<unsigned long long>(now - runStart),
                             static_cast<unsigned long long>(kStateIntervalMs));
                }
            }
            s_lastIterMs = now;
            if (s_reportMs == 0) s_reportMs = now;
            if (now - s_reportMs >= 10000) {
                const std::uint64_t span = now - s_reportMs;
                const double perIter = s_iterCount == 0
                                           ? 0.0
                                           : static_cast<double>(span) / static_cast<double>(s_iterCount);
                DSH_INFO("loop: %llu iteration(s) in the last %llu ms (~%.1f ms each), longest %llu ms; the pose "
                         "gate asks for %llu ms, so this loop can send at most %.1f state datagram(s)/s",
                         static_cast<unsigned long long>(s_iterCount), static_cast<unsigned long long>(span),
                         perIter, static_cast<unsigned long long>(s_iterMax),
                         static_cast<unsigned long long>(kStateIntervalMs),
                         perIter <= 0.0 ? 0.0 : 1000.0 / perIter);

                DSH_INFO("dispatch: %s", dsh::reflect::game_thread_dispatch_report().c_str());

                DSH_INFO("recv: socket read=%llu, dispatched=%llu, pose applied=%llu in the last 10 s",
                         static_cast<unsigned long long>(g_recvRead),
                         static_cast<unsigned long long>(g_recvDispatched),
                         static_cast<unsigned long long>(g_recvApplied));
                g_recvRead = 0;
                g_recvDispatched = 0;
                g_recvApplied = 0;

                DSH_INFO("pose: %llu tick(s), %llu sampled, %llu repeated the last pose, %llu with no pose, "
                         "%llu built for no connected peer, %llu grid point(s) skipped in the last 10 s%s%s",
                         static_cast<unsigned long long>(g_poseTicks),
                         static_cast<unsigned long long>(g_poseSampled),
                         static_cast<unsigned long long>(g_poseRepeated),
                         static_cast<unsigned long long>(g_poseNoPose),
                         static_cast<unsigned long long>(g_poseNoPeer),
                         static_cast<unsigned long long>(stateSkipped),
                         g_poseLastRefusal.empty() ? "" : " -- last refusal: ",
                         g_poseLastRefusal.c_str());

                std::size_t interpBuffered = 0;
                dsh::sync::PoseInterpolator::Stats interpTotals;
                for (const auto& slot : dolls.slots()) {
                    if (!slot) continue;
                    interpBuffered += slot->interp.buffered();
                    const auto one = slot->interp.stats();
                    interpTotals.interpolatedFrames += one.interpolatedFrames;
                    interpTotals.extrapolatedFrames += one.extrapolatedFrames;
                    interpTotals.frozenFrames += one.frozenFrames;
                    interpTotals.noDataFrames += one.noDataFrames;
                    interpTotals.staleFrames += one.staleFrames;
                    interpTotals.pushed += one.pushed;
                    interpTotals.discardedTooOld += one.discardedTooOld;
                    interpTotals.discardedOverflow += one.discardedOverflow;
                    interpTotals.replacedDuplicate += one.replacedDuplicate;
                }
                DSH_INFO("interp: %llu applied (%llu dropped by the game thread), %zu buffered; since the "
                         "session started: %llu interpolated, %llu extrapolated, %llu FROZEN, %llu with no "
                         "data, %llu stale, %llu sample(s) pushed, %llu too old, %llu overflow, %llu "
                         "duplicate stamp(s)",
                         static_cast<unsigned long long>(interpApplied),
                         static_cast<unsigned long long>(interpDropped), interpBuffered,
                         static_cast<unsigned long long>(interpTotals.interpolatedFrames),
                         static_cast<unsigned long long>(interpTotals.extrapolatedFrames),
                         static_cast<unsigned long long>(interpTotals.frozenFrames),
                         static_cast<unsigned long long>(interpTotals.noDataFrames),
                         static_cast<unsigned long long>(interpTotals.staleFrames),
                         static_cast<unsigned long long>(interpTotals.pushed),
                         static_cast<unsigned long long>(interpTotals.discardedTooOld),
                         static_cast<unsigned long long>(interpTotals.discardedOverflow),
                         static_cast<unsigned long long>(interpTotals.replacedDuplicate));

                {
                    dsh::sync::PlaybackClock::Stats clockTotals;
                    std::int64_t worstRequest = 0;
                    std::int64_t worstStepBack = 0;
                    double rateSum = 0.0;
                    std::size_t rateCount = 0;
                    for (const auto& slot : dolls.slots()) {
                        if (!slot) continue;
                        const auto one = slot->clock.stats();
                        clockTotals.arrivals += one.arrivals;
                        clockTotals.rateEstimates += one.rateEstimates;
                        clockTotals.advances += one.advances;
                        clockTotals.backwardRequests += one.backwardRequests;
                        clockTotals.backwardTotalMs += one.backwardTotalMs;
                        if (one.worstBackwardMs > worstRequest) worstRequest = one.worstBackwardMs;
                        clockTotals.backSteps += one.backSteps;
                        if (one.worstStepBackMs > worstStepBack) worstStepBack = one.worstStepBackMs;
                        clockTotals.resyncs += one.resyncs;
                        if (slot->clock.running()) {
                            rateSum += slot->clock.rate();
                            ++rateCount;
                        }
                    }
                    DSH_INFO("render clock: the old formula would have pulled the render time BACKWARD %llu "
                             "time(s) -- %lld ms in total, worst %lld ms; this clock stepped back %llu time(s), "
                             "worst %lld ms, and resynced %llu time(s); the sender's rate measures %.3f stamp(s) "
                             "per ms of real time, over %llu estimate(s) from %llu arrival(s)",
                             static_cast<unsigned long long>(clockTotals.backwardRequests),
                             static_cast<long long>(clockTotals.backwardTotalMs),
                             static_cast<long long>(worstRequest),
                             static_cast<unsigned long long>(clockTotals.backSteps),
                             static_cast<long long>(worstStepBack),
                             static_cast<unsigned long long>(clockTotals.resyncs),
                             rateCount == 0 ? 0.0 : rateSum / static_cast<double>(rateCount),
                             static_cast<unsigned long long>(clockTotals.rateEstimates),
                             static_cast<unsigned long long>(clockTotals.arrivals));
                }

                {
                    const dsh::game::ActionEdgeStats edges = dsh::game::action_edge_stats();

                    std::string puppetName;
                    for (const auto& actionSlot : dolls.slots()) {
                        if (!actionSlot) continue;
                        puppetName +=
                            std::string(puppetName.empty() ? "" : ", ") + "peer " +
                            std::to_string(static_cast<unsigned>(actionSlot->peer.value)) + "'s '" +
                            (actionSlot->actors.empty()
                                 ? std::string("<not built>")
                                 : dsh::reflect::object_class_name(actionSlot->actors.front())) +
                            "'";
                    }
                    if (puppetName.empty()) puppetName = "<no puppet>";

                    DSH_INFO("action: this side SENT %llu of its own command(s); it RECEIVED %llu from the "
                             "peer and replayed %llu of them onto '%s' (this side's own doll is never the "
                             "target), %llu refused; the capture ring has taken %llu and dropped %llu; "
                             "%llu hit(s) whose causer was a puppet COPY were zeroed in the caller's frame "
                             "(the detour's fallback -- 0 is the normal reading now), and %llu projectile(s) "
                             "arrived and had their DAMAGE NUMBERS zeroed BEFORE the copy was built, which is "
                             "what makes a copy harmless by construction: that damage is the other machine's "
                             "health report to make, not this one's",
                             static_cast<unsigned long long>(actionEdgesSent),
                             static_cast<unsigned long long>(actionEdgesReceived),
                             static_cast<unsigned long long>(actionApplied), puppetName.c_str(),
                             static_cast<unsigned long long>(actionRefused),
                             static_cast<unsigned long long>(edges.noted),
                             static_cast<unsigned long long>(edges.dropped),
                             static_cast<unsigned long long>(dsh::game::puppet_damage_dropped()),
                             static_cast<unsigned long long>(ammoCopiesBornHarmless));
                }

                {
                    const dsh::game::AmmoSpawnStats ammo = dsh::game::ammo_spawn_stats();
                    DSH_INFO("ammo: this side CAPTURED %llu projectile(s) from SetAmmoParams and SENT %llu "
                             "datagram(s) (%llu could not be encoded, ring dropped %llu, and %llu was NOT "
                             "captured because the shooter was not this machine's player -- which is the "
                             "enemies' rounds AND the copies of the other player's projectiles, since both "
                             "go through the same function); it RECEIVED %llu, "
                             "REFUSED %llu at the wire, and CREATED %llu of its own (%llu could not be "
                             "created). HOMING: this side CAPTURED a target for %llu projectile(s) and "
                             "REFUSED %llu (they are NPCs); it REPRODUCED %llu in its copies and cleared the "
                             "flag on %llu -- %llu because the sender refused to name a target, %llu because "
                             "the name is not in this world, %llu because the actor has no lock-on component "
                             "or the interface would not answer. SPAWN QUEUE: %llu dispatch(es) were pushed and "
                             "%llu RAN -- a push that never ran is a copy the game thread cancelled, which is "
                             "the case run 579-2 could not see",
                             static_cast<unsigned long long>(ammo.noted),
                             static_cast<unsigned long long>(ammoSent),
                             static_cast<unsigned long long>(ammo.refused),
                             static_cast<unsigned long long>(ammo.dropped),
                             static_cast<unsigned long long>(ammo.foreign),
                             static_cast<unsigned long long>(ammoReceived),
                             static_cast<unsigned long long>(ammoDecodeFailed),
                             static_cast<unsigned long long>(g_ammoCreated.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_ammoRefused.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(ammoHomingCaptured),
                             static_cast<unsigned long long>(ammoHomingRefusedNpc),
                             static_cast<unsigned long long>(ammoHomingReproduced),
                             static_cast<unsigned long long>(g_ammoHomingCleared.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(ammoHomingSenderRefused),
                             static_cast<unsigned long long>(ammoHomingNameMissing),
                             static_cast<unsigned long long>(ammoHomingNoComponent),
                             static_cast<unsigned long long>(
                                 g_ammoDispatchPushed.load(std::memory_order_relaxed)),
                             static_cast<unsigned long long>(g_ammoDispatchRan.load(std::memory_order_relaxed)));
                }

                {
                    const std::uintptr_t mine = pawn.valid ? pawn.pawn : 0;
                    const dsh::reflect::CharacterVitals mineVitals = dsh::reflect::character_vitals(mine);
                    char buf[240];
                    if (mineVitals.read) {
                        std::snprintf(buf, sizeof(buf),
                                      "health %.1f of %.1f, shield %.1f of %.1f (attribute set 0x%llX)",
                                      static_cast<double>(mineVitals.health),
                                      static_cast<double>(mineVitals.maxHealth),
                                      static_cast<double>(mineVitals.shield),
                                      static_cast<double>(mineVitals.maxShield),
                                      static_cast<unsigned long long>(mineVitals.set));
                    } else {
                        std::snprintf(buf, sizeof(buf), "NOT READ (%s)", mineVitals.note.c_str());
                    }
                    const std::string mineText = buf;

                    std::string puppetText;
                    {
                        std::uint32_t vitalsDolls = 0;
                        for (const auto& vitalsSlot : dolls.slots()) {
                            if (!vitalsSlot) continue;
                            const std::uintptr_t theirs =
                                vitalsSlot->actors.empty() ? 0 : vitalsSlot->actors.front();
                            const dsh::reflect::CharacterVitals vitals =
                                dsh::reflect::character_vitals(theirs);
                            ++vitalsDolls;
                            puppetText += std::string(vitalsDolls == 1 ? "" : "; ") + "peer " +
                                          std::to_string(static_cast<unsigned>(vitalsSlot->peer.value));
                            if (vitals.read) {
                                std::snprintf(buf, sizeof(buf),
                                              " health %.1f of %.1f, shield %.1f of %.1f",
                                              static_cast<double>(vitals.health),
                                              static_cast<double>(vitals.maxHealth),
                                              static_cast<double>(vitals.shield),
                                              static_cast<double>(vitals.maxShield));
                            } else if (theirs < 0x10000) {
                                std::snprintf(buf, sizeof(buf), " has no doll built yet");
                            } else {
                                std::snprintf(buf, sizeof(buf), " NOT READ (%s)", vitals.note.c_str());
                            }
                            puppetText += buf;
                        }
                        if (vitalsDolls == 0) {
                            puppetText = "there is no other player's doll on this side yet";
                        }
                    }
                    DSH_INFO("puppetsession: MY player: %s; the other doll(s)' own: %s; the player-health mirror: "
                             "SENT %u (refused to build %u), RECEIVED %u (refused at the wire %u), written "
                             "%u, already right %u, could not write %u, no puppet %u",
                             mineText.c_str(), puppetText.c_str(), playerHealthSent, playerHealthSendRefusals,
                             playerHealthReceived, playerHealthRefusedAtWire, playerHealthWritten,
                             playerHealthAlreadyRight, playerHealthWriteFailures, playerHealthNoPuppet);
                }
                g_poseTicks = 0;
                g_poseSampled = 0;
                g_poseRepeated = 0;
                g_poseNoPose = 0;
                g_poseNoPeer = 0;
                g_poseLastRefusal.clear();
                stateSkipped = 0;
                s_reportMs = now;
                s_iterCount = 0;
                s_iterMax = 0;
            }
        }

        if (g_sessionStopRequested) {
            DSH_WARN("puppetsession: ENDING EARLY on request -- %s", g_sessionStopReason.c_str());
            break;
        }

        if (g_sessionTickHook) {
            try {
                g_sessionTickHook();
            } catch (...) {
                DSH_WARN("puppetsession: the caller's own tick hook threw and was skipped for this "
                         "iteration -- the session carries on, because the hook is not the session");
            }
        }

        if (!puppetActors.empty()) {
            std::string puppetWhy;
            if (!dsh::reflect::object_is_still_registered(puppetActors.front(), puppetWhy)) {
                DSH_WARN("puppetsession: the puppet is no longer a live object (%s) -- this session is "
                         "ending instead of writing poses into it", puppetWhy.c_str());
                r.detail = "the puppet was removed from the world (" + puppetWhy + ")";
                break;
            }
        }
        const std::uintptr_t worldNow = dsh::reflect::live_world_address();
        if (worldNow != sessionWorld) {
            DSH_WARN("puppetsession: the live world MOVED, 0x%llX -> 0x%llX -- this session is ending",
                     static_cast<unsigned long long>(sessionWorld),
                     static_cast<unsigned long long>(worldNow));
            r.detail = "the world was REPLACED (a map change through a rest point, a portal or a load) -- "
                       "this session is ENDING rather than writing poses into actors that no longer exist. "
                       "Start a new session in the new area.";
            DSH_ERROR("puppetsession: %s", r.detail.c_str());
            break;
        }

        if (sessionPawn >= 0x10000 && pawn.valid && pawn.pawn >= 0x10000 && pawn.pawn != sessionPawn) {
            DSH_WARN("puppetsession: the LOCAL PAWN CHANGED, 0x%llX -> 0x%llX -- the session is running in a "
                     "different place from the one it started in, so it is ENDING rather than writing into "
                     "what it built there",
                     static_cast<unsigned long long>(sessionPawn),
                     static_cast<unsigned long long>(pawn.pawn));
            r.detail = "the local pawn was REPLACED (a map change, a respawn or a level transition) -- this "
                       "session is ENDING rather than writing poses into actors that no longer exist. Start "
                       "a new session in the new area.";
            DSH_ERROR("puppetsession: %s", r.detail.c_str());
            break;
        }

        {
            std::uint8_t checkpointSignal = 0;
            if (dsh::game::take_checkpoint(checkpointSignal)) {
                const char* what = "an unnamed signal";
                switch (static_cast<dsh::game::CheckpointSignal>(checkpointSignal)) {
                    case dsh::game::CheckpointSignal::dialogue:
                        what = "the checkpoint dialogue opened";
                        break;
                    case dsh::game::CheckpointSignal::regressionPoint:
                        what = "a checkpoint set the spawn point";
                        break;
                    case dsh::game::CheckpointSignal::save:
                        what = "the game saved from the regression point";
                        break;
                    default:
                        break;
                }
                DSH_WARN("%s A REST POINT WAS USED (%s) -- ending the session NOW, before the world is reset. "
                         "Nothing of ours is left standing when the supply reloads the level; press F7 on both "
                         "machines to host and join again after it", rolePrefix, what);
                dsh::game::request_session_stop(std::string("a rest point was used (") + what + ")");
            }
        }

        if (dialogueSystem >= 0x10000) {
            std::uint64_t currentCheckpoint = 0;
            std::uint64_t currentDialogue = 0;
            dsh::reflect::read_process_bytes(dialogueSystem + 0x00D8, &currentCheckpoint,
                                             sizeof(currentCheckpoint));
            dsh::reflect::read_process_bytes(dialogueSystem + 0x00F0, &currentDialogue,
                                             sizeof(currentDialogue));
            if (currentDialogue != lastDialoguePointer) {
                lastDialoguePointer = currentDialogue;
                DSH_INFO("%s a dialogue is open: 0x%llX -- CurrentCheckpoint reads %s", rolePrefix,
                         static_cast<unsigned long long>(currentDialogue),
                         currentCheckpoint >= 0x10000 ? "a task" : "0");
            }
            if (currentCheckpoint >= 0x10000) {
                DSH_WARN("%s A REST POINT WAS USED (the game's dialogue subsystem says a checkpoint task is "
                         "running) -- ending the session NOW, before the player picks a row. The supply resets "
                         "the world; press F7 on both machines to host and join again after it", rolePrefix);
                dsh::game::request_session_stop("a rest point was used (checkpoint task)");
            }
        }

        if (!appearanceChunks.empty() && now >= nextManifest) {
            nextManifest = now + 2000;
            std::size_t passes = 0;
            for (const auto& peer : session.peers()) {
                if (peer.state != dsh::net::PeerState::connected) continue;

                bool acked = false;
                for (std::uint8_t slot : manifestAckedPeers) {
                    if (slot == peer.id.value) acked = true;
                }
                if (acked) continue;
                std::uint32_t chunkSeq = 1;
                for (const auto& chunk : appearanceChunks) {
                    session.send_appearance_to(peer.id, chunkSeq++, chunk);
                    ++r.chunksSent;
                }
                ++passes;
            }
            if (passes != 0) ++manifestRepeats;
        }

        if (now >= nextManifestAckMs) {
            nextManifestAckMs = now + 200;
            for (auto& ackSlot : dolls.slots()) {
                if (!ackSlot || ackSlot->ackPending <= 0) continue;
                --ackSlot->ackPending;
                const std::uint8_t owner = ackSlot->peer.value;
                const dsh::PeerId to = asHost ? ackSlot->peer : dsh::kHostPeer;
                session.send_appearance_ack_to(
                    to, manifestAckSequence++,
                    std::span<const std::uint8_t>(&owner, 1));
                DSH_INFO("puppetsession: peer %u's manifest ASSEMBLED here -- acknowledging it to %s so that "
                         "the sender stops repeating %zu chunk(s) every two seconds (%d more "
                         "acknowledgement(s) will follow)",
                         static_cast<unsigned>(owner),
                         asHost ? "that peer directly" : "the host, which routes it",
                         appearanceChunks.empty() ? 0 : appearanceChunks.size(), ackSlot->ackPending);
            }
        }

        if (now >= nextTargetModelMs) {
            nextTargetModelMs = now + 1000;
            DSH_TIME_WALK("loop:targets");
            dsh::reflect::TargetModelReport model;

            if (dsh::reflect::run_on_game_thread(
                    [&]() { model = dsh::reflect::ai_enforce_targets(GetTickCount64(), asHost); }, 2000)) {
                targetModelForced += model.forced;
                targetModelVerified += model.verified;
                targetModelRefused += model.refused;
                const bool sayNow = !model.decisions.empty() && model.forced != 0;
                if (sayNow || now >= nextTargetModelReportMs) {
                    nextTargetModelReportMs = now + 10000;
                    ++targetModelReports;
                    for (const std::string& line : model.report_lines()) DSH_INFO("puppetsession: %s", line.c_str());
                }
            } else {
                DSH_WARN("puppetsession: the target model did not run on the game thread this second");
            }
        }

        if (!asHost && now >= nextHealthReportSend) {
            DSH_TIME_WALK("loop:health-report");
            nextHealthReportSend = now + kHealthReportMs;
            std::string reportNote;
            const std::vector<dsh::game::HealthEntry> reports =
                dsh::game::client_collect_damage_reports(healthReportSequence, reportNote);
            if (!reports.empty()) {
                healthReportSequence += static_cast<std::uint32_t>(reports.size());
                std::string encodeNote;
                const std::vector<std::uint8_t> payload = dsh::game::encode_health_report(reports, encodeNote);
                if (!payload.empty()) {
                    for (const auto& peer : session.peers()) {
                        if (peer.state != dsh::net::PeerState::connected) continue;

                    if (!asHost && !missionListArrived && missionListRequestAttempts < 3 &&
                        GetTickCount64() >= nextMissionListAsk) {
                        ++missionListRequestAttempts;
                        session.send_mission_list_request_to(peer.id, missionListSequence++);
                        nextMissionListAsk = GetTickCount64() + 2000;
                        missionListAsked = true;
                        DSH_INFO("puppetsession[client]: asked the host for its boss records -- attempt %u of "
                                 "3 (a request, not a broadcast; it stops when the answer arrives)",
                                 missionListRequestAttempts);
                    } else if (!asHost && !missionListArrived && missionListRequestAttempts >= 3) {
                        missionListRequestAttempts = 3;
                        missionListArrived = true;
                        DSH_WARN("puppetsession[client]: sent 3 mission-list requests and the host never "
                                 "answered -- nothing more will be asked this session. The exchange is "
                                 "request-driven; reconnect to try again");
                    }
                        session.send_health_report_to(peer.id, healthReportSequence, payload);
                        ++healthReportsSent;
                    }
                    healthEntriesReported += static_cast<std::uint32_t>(reports.size());
                    std::string named;
                    for (std::size_t i = 0; i < reports.size() && i < 4; ++i) {
                        char one[128];

                        std::snprintf(one, sizeof(one), "%s'%s' = %.1f", named.empty() ? "" : "; ",
                                      reports[i].enemyName.c_str(), static_cast<double>(reports[i].value));
                        named += one;
                    }
                    const std::string tail = reportNote.empty() ? std::string() : (" -- " + reportNote);
                    DSH_INFO("puppetsession: health report: %zu state(s) reported to the host (%u payload(s), %u entr(y/ies) this session): %s%s",
                             reports.size(), healthReportsSent, healthEntriesReported, named.c_str(), tail.c_str());
                }
            }
        }
        if (asHost && now >= nextHealthBroadcastFull) {
            nextHealthBroadcastFull = now + kHealthBroadcastFullMs;
            healthBroadcastForceAll = true;
            ++healthBroadcastFulls;
        }
        if (asHost && now >= nextHealthBroadcast) {
            DSH_TIME_WALK("loop:health-broadcast");
            nextHealthBroadcast = now + dsh::game::kHealthBroadcastMs;
            std::string healthNote;
            const std::vector<dsh::game::HealthEntry> changed =
                dsh::game::host_collect_health_changes(healthBroadcastForceAll, healthNote);
            healthBroadcastForceAll = false;

            if (changed.empty() && !healthNote.empty() && healthBroadcastNothingSaid < 3) {
                ++healthBroadcastNothingSaid;
                DSH_WARN("puppetsession: health broadcast: NOTHING was broadcast: %s", healthNote.c_str());
            }
            if (!changed.empty()) {
                std::string encodeNote;
                const std::vector<std::uint8_t> payload =
                    dsh::game::encode_health_broadcast(changed, encodeNote);
                if (!payload.empty()) {
                    for (const auto& peer : session.peers()) {
                        if (peer.state != dsh::net::PeerState::connected) continue;
                        session.send_health_broadcast_to(peer.id, healthBroadcastSequence++, payload);
                        ++healthBroadcastsSent;
                    }
                    healthEntriesSent += static_cast<std::uint32_t>(changed.size());
                    std::string named;
                    for (std::size_t i = 0; i < changed.size() && i < 4; ++i) {
                        char one[96];
                        std::snprintf(one, sizeof(one), "%s'%s' = %.1f", named.empty() ? "" : "; ",
                                      changed[i].enemyName.c_str(),
                                      static_cast<double>(changed[i].value));
                        named += one;
                    }
                    if (changed.size() > 4) named += "; ...";
                    const std::string tail = healthNote.empty() ? std::string() : (" -- " + healthNote);
                    DSH_INFO("puppetsession: health broadcast: %zu entr(y/ies) CHANGED since the last one "
                             "(%u broadcast(s), %u entr(y/ies) this session): %s%s",
                             changed.size(), healthBroadcastsSent, healthEntriesSent, named.c_str(),
                             tail.c_str());
                }
            }
        }

        if (now >= nextPlayerHealthSend) {
            DSH_TIME_WALK("loop:player-health");
            nextPlayerHealthSend = now + kPlayerHealthSendMs;
            const std::uintptr_t mine = pawn.valid ? pawn.pawn : 0;
            const dsh::reflect::CharacterVitals vitals = dsh::reflect::character_vitals(mine);
            if (!vitals.read) {

                if (++playerHealthSendRefusals <= 3) {
                    DSH_WARN("puppetsession: this side's own player health could NOT be read, so nothing was "
                             "sent this tick: %s", vitals.note.c_str());
                }
            } else {
                dsh::game::PlayerHealth value;
                value.health = vitals.health;
                value.maxHealth = vitals.maxHealth;
                std::string encodeNote;
                const std::vector<std::uint8_t> payload = dsh::game::encode_player_health(value, encodeNote);
                if (payload.empty()) {
                    if (++playerHealthSendRefusals <= 3) {
                        DSH_WARN("puppetsession: this side's own player health was NOT sent: %s",
                                 encodeNote.c_str());
                    }
                } else {
                    for (const auto& peer : session.peers()) {
                        if (peer.state != dsh::net::PeerState::connected) continue;
                        session.send_player_health_to(peer.id, playerHealthSequence++, payload);
                        ++playerHealthSent;
                    }
                }
            }
        }

        const std::uint64_t stateDue = dsh::sync::tick_index(stateEpoch, nowSteady, kStateIntervalMs);
        if (stateDue > stateTick) {
            DSH_TIME_WALK("loop:pose");
            stateSkipped += dsh::sync::ticks_skipped(stateTick, stateDue);
            stateTick = stateDue;
            ++g_poseTicks;

            const std::uint32_t wireTick = static_cast<std::uint32_t>(stateTick);

            dsh::sync::PoseSample pose;
            std::string note;
            const bool sampled = sample_local_pawn(pawn, pose, note, now, nextPawnLocate);
            if (sampled) {
                ++g_poseSampled;
                poseRepeatStreak = 0;
            } else if (!note.empty()) {

                g_poseLastRefusal = note;
            }
            if (sampled) {

                std::string stanceNote;
                if (!stanceClassResolved || stanceClassCheckedPawn != pawn.pawn) {

                    stanceCharacterClass = dsh::reflect::resolve_uclass_address(kStanceApiClass);
                    stanceClassResolved = stanceCharacterClass != 0;
                    stanceClassCheckedPawn = pawn.pawn;
                    stanceClassCheckLogged = false;
                }
                const bool haveStance =
                    stanceClassResolved &&
                    dsh::game::read_doll_stance(pawn.pawn, stanceCharacterClass, stanceSent,
                                                stanceNote, dsh::game::stance_read_process_bytes);
                if (!haveStance && !stanceClassCheckLogged) {

                    stanceClassCheckLogged = true;
                    DSH_WARN("puppetsession: the doll's stance is NOT being sent -- %s (the local pawn "
                             "is a '%s')", stanceNote.c_str(),
                             dsh::reflect::object_class_name(pawn.pawn).c_str());
                }

                dsh::net::ByteWriter payload;
                StanceWireState wire;
                wire.posM[0] = pose.x;
                wire.posM[1] = pose.y;
                wire.posM[2] = pose.z;
                wire.yawRad = pose.yaw;

                {
                    bool aimPitchReadable = false;
                    std::string aimPitchWhy;
                    wire.pitchRad =
                        local_aim_pitch_radians(pawn.pawn, pose.pitch, aimPitchReadable, aimPitchWhy);
                    if (!aimPitchReadable && !aimPitchRefusalLogged) {
                        aimPitchRefusalLogged = true;
                        DSH_WARN("puppetsession: this side's AIM PITCH is NOT being sent -- %s, so the "
                                 "pose's pitch stays the capsule's (the local pawn is a '%s')",
                                 aimPitchWhy.c_str(), dsh::reflect::object_class_name(pawn.pawn).c_str());
                    }
                }

                wire.stance = haveStance ? stanceSent : DollStance{};

                wire.velocity[0] = hostMovement.readable ? hostMovement.velocity[0] : 0.0f;
                wire.velocity[1] = hostMovement.readable ? hostMovement.velocity[1] : 0.0f;
                wire.velocity[2] = hostMovement.readable ? hostMovement.velocity[2] : 0.0f;

                wire.movementMode = hostMovement.readable ? hostMovement.movementMode : 0;

                wire.acceleration[0] = hostMovement.readable ? hostMovement.acceleration[0] : 0.0f;
                wire.acceleration[1] = hostMovement.readable ? hostMovement.acceleration[1] : 0.0f;
                wire.acceleration[2] = hostMovement.readable ? hostMovement.acceleration[2] : 0.0f;

                {
                    std::string sendRefusal;
                    const std::uintptr_t ownMachineForMode = local_pawn_machine(localPawnMachine, sendRefusal);
                    const dsh::game::ExclusiveState ownMode = dsh::game::exclusive_state_read(
                        ownMachineForMode, movementStateClass, dsh::reflect::read_process_bytes);
                    const dsh::game::ExclusiveOwnerKindRead ownKind = dsh::game::exclusive_owner_kind(
                        ownMode.read ? ownMode.owner : 0, ownMachineForMode,
                        dsh::reflect::read_process_bytes);

                    wire.exclusiveTypes = ownMode.read ? ownMode.types : 0;
                    wire.exclusiveOwnerKind = static_cast<std::uint8_t>(ownKind.kind);
                }

                hostVelocitySent[0] = wire.velocity[0];
                hostVelocitySent[1] = wire.velocity[1];
                hostVelocitySent[2] = wire.velocity[2];
                hostModeSent = wire.movementMode;

                if (movementStateClass != 0) {
                    std::string inputNote;
                    haveInput = dsh::game::read_movement_inputs(pawn.pawn, movementStateClass, hostInput,
                                                               inputNote,
                                                               dsh::game::stance_read_process_bytes);
                    if (!haveInput && !inputReadRefusalLogged) {

                        inputReadRefusalLogged = true;
                        DSH_WARN("puppetsession: the machine's movement input is NOT being sent -- %s "
                                 "(the local pawn is a '%s')", inputNote.c_str(),
                                 dsh::reflect::object_class_name(pawn.pawn).c_str());
                    }

                    if (haveInput && flagEdges.havePrevious) {
                        if (flagEdgesPrevious.sprint == 0 && hostInput.sprint != 0) {
                            ++flagEdges.sprintUp;
                            ++flagEdgesThisWindow.sprintUp;
                        } else if (flagEdgesPrevious.sprint != 0 && hostInput.sprint == 0) {
                            ++flagEdges.sprintDown;
                            ++flagEdgesThisWindow.sprintDown;
                        }
                        if (flagEdgesPrevious.hover == 0 && hostInput.hover != 0) {
                            ++flagEdges.hoverUp;
                            ++flagEdgesThisWindow.hoverUp;
                        } else if (flagEdgesPrevious.hover != 0 && hostInput.hover == 0) {
                            ++flagEdges.hoverDown;
                            ++flagEdgesThisWindow.hoverDown;
                        }
                        if (flagEdgesPrevious.boostJump == 0 && hostInput.boostJump != 0) {
                            ++flagEdges.boostJumpUp;
                            ++flagEdgesThisWindow.boostJumpUp;
                        } else if (flagEdgesPrevious.boostJump != 0 && hostInput.boostJump == 0) {
                            ++flagEdges.boostJumpDown;
                            ++flagEdgesThisWindow.boostJumpDown;
                        }
                        if (flagEdgesPrevious.boostDash == 0 && hostInput.boostDash != 0) {
                            ++flagEdges.boostDashUp;
                            ++flagEdgesThisWindow.boostDashUp;
                        } else if (flagEdgesPrevious.boostDash != 0 && hostInput.boostDash == 0) {
                            ++flagEdges.boostDashDown;
                            ++flagEdgesThisWindow.boostDashDown;
                        }
                    }
                    if (haveInput) {
                        flagEdgesPrevious = hostInput;
                        flagEdges.havePrevious = true;
                    }
                }
                wire.movementInput = haveInput ? hostInput : dsh::game::MovementInputs{};
                hostInputSent = wire.movementInput;

                if (movementStateClass != 0) {
                    std::string energyNote;
                    haveEnergy = dsh::game::read_energy_timers(pawn.pawn, movementStateClass, hostEnergy,
                                                              energyNote,
                                                              dsh::game::stance_read_process_bytes);
                    if (!haveEnergy && !energyReadRefusalLogged) {
                        energyReadRefusalLogged = true;
                        DSH_WARN("puppetsession: the machine's two energy timers are NOT being sent -- %s "
                                 "(the local pawn is a '%s')", energyNote.c_str(),
                                 dsh::reflect::object_class_name(pawn.pawn).c_str());
                    }

                    if (haveEnergy) {

                        energySentHavePrevious = energyHavePrevious;
                        energySentPreviousIntermission = energyPreviousIntermission;
                        energyHavePrevious = true;
                        energyPreviousIntermission = hostEnergy.intermissionTime;
                    }
                }
                wire.energyTimers = haveEnergy ? hostEnergy : dsh::game::EnergyTimers{};
                hostEnergySent = wire.energyTimers;

                if (!weaponClassResolved || weaponClassCheckedPawn != pawn.pawn) {

                    weaponMachineClass = dsh::reflect::resolve_uclass_address(kFrontWeaponApiClass);
                    weaponClassResolved = weaponMachineClass != 0;
                    weaponClassCheckedPawn = pawn.pawn;
                }
                std::string weaponNote;
                haveWeapon = dsh::game::read_front_weapon(pawn.pawn, weaponMachineClass, hostFrontWeapon,
                                                          weaponNote,
                                                          dsh::game::stance_read_process_bytes);
                if (!haveWeapon) {
                    if (!weaponReadRefusalLogged) {
                        weaponReadRefusalLogged = true;
                        DSH_WARN("puppetsession: this side's choice of front weapon is NOT being sent -- %s "
                                 "(the local pawn is a '%s')", weaponNote.c_str(),
                                 dsh::reflect::object_class_name(pawn.pawn).c_str());
                    }
                    hostFrontWeaponIndex = kFrontWeaponNotRead;
                } else {
                    hostFrontWeaponIndex = dsh::game::front_weapon_index(hostFrontWeapon);
                }
                wire.frontWeaponIndex = hostFrontWeaponIndex;

                const std::size_t payloadBytes = dsh::game::encode_stance_state(payload, wire);
                if (payloadBytes != kStatePayloadBytes && !stateSizeMismatchLogged) {
                    stateSizeMismatchLogged = true;
                    DSH_ERROR("puppetsession: this build encoded a %zu-byte state payload where %zu was "
                              "expected -- the receiver drops anything that is not %zu, so the puppet "
                              "will not move and this is a defect in the sender",
                              payloadBytes, kStatePayloadBytes, kStatePayloadBytes);
                }

                std::size_t peersSent = 0;
                for (const auto& peer : session.peers()) {
                    if (peer.state != dsh::net::PeerState::connected) continue;
                    session.send_state_to(peer.id, wireTick, payload.buffer());
                    ++peersSent;
                    ++stateDatagrams;
                    ++stanceDatagrams;

                    if (wire.velocity[0] != 0.0f || wire.velocity[1] != 0.0f || wire.velocity[2] != 0.0f) {
                        ++velocityDatagramsNonZero;
                        ++velocityDatagramsNonZeroThisWindow;
                    }

                    if (wire.movementInput.boostJump != 0 || wire.movementInput.boostDash != 0 ||
                        wire.movementInput.hover != 0 || wire.movementInput.sprint != 0 ||
                        wire.movementInput.ignoreFriction != 0 ||
                        wire.movementInput.recentInput[0] != 0.0f ||
                        wire.movementInput.recentInput[1] != 0.0f ||
                        wire.movementInput.recentInput[2] != 0.0f) {
                        ++inputDatagramsNonZero;
                        ++inputDatagramsNonZeroThisWindow;
                    }

                    if (wire.movementInput.boostJump != 0 || wire.movementInput.boostDash != 0) {
                        ++boostDatagramsNonZero;
                        ++boostDatagramsNonZeroThisWindow;
                    }

                    if (wire.energyTimers.intermissionTime != 0.0f ||
                        wire.energyTimers.fastChargeStartTime != 0.0f) {
                        ++energyDatagramsNonZero;
                        ++energyDatagramsNonZeroThisWindow;
                    }

                    if (wire.frontWeaponIndex == kFrontWeaponSlot0 ||
                        wire.frontWeaponIndex == kFrontWeaponSlot1) {
                        ++weaponDatagramsDrawn;
                    } else if (wire.frontWeaponIndex == kFrontWeaponNotRead) {
                        ++weaponDatagramsNotRead;
                    }
                }

                if (peersSent == 0) {
                    ++g_poseNoPeer;
                } else {
                    lastPosePayload.assign(payload.buffer().begin(), payload.buffer().end());
                }
            } else if (!lastPosePayload.empty() && poseRepeatStreak < kPoseRepeatMaxStreak) {

                ++poseRepeatStreak;
                std::size_t peersSent = 0;
                for (const auto& peer : session.peers()) {
                    if (peer.state != dsh::net::PeerState::connected) continue;
                    session.send_state_to(peer.id, wireTick, lastPosePayload);
                    ++peersSent;

                    ++stateDatagrams;
                }
                if (peersSent == 0) {
                    ++g_poseNoPeer;
                } else {
                    ++g_poseRepeated;
                }
            } else {

                ++g_poseNoPose;
                if (now >= nextNoPoseLog) {

                    nextNoPoseLog = now + 5000;
                    DSH_WARN("puppetsession: no local position to stream (%s) -- looking for the local "
                             "pawn again", note.c_str());
                }
            }

            {
                const std::uintptr_t watchMachine = local_pawn_machine(localPawnMachine, localPawnRefusal);
                if (watchMachine >= 0x10000 && thrusterModuleClass != 0) {
                    const dsh::game::ModuleComponentReport* walk = cached_module_walk(kWalkLabelMine);
                    if (walk != nullptr && walk->machine == watchMachine && walk->module >= 0x10000) {

                        dsh::game::refresh_module_component_state(
                            module_walk_cache(kWalkLabelMine).report, thrusterModuleClass,
                            &dsh::reflect::read_process_bytes, &dsh::game::actor_class_chain_reaches);
                        const dsh::game::ModuleComponentReport& fresh =
                            module_walk_cache(kWalkLabelMine).report;

                        dsh::game::WatchModuleKey key;
                        key.module = fresh.module;
                        key.componentCount = static_cast<int>(fresh.components.size());
                        key.firstComponent =
                            fresh.components.empty() ? 0 : fresh.components.front().address;
                        key.usable = fresh.moduleUsable;
                        for (const auto& component : fresh.components) {
                            key.componentNames.push_back(component.objectName.empty()
                                                            ? component.className
                                                            : component.objectName);
                        }

                        const bool firstWalk = !watchStarted;
                        if (!watchStarted || key != watchModuleKey) {
                            if (!watchStarted) {
                                watchStarted = true;
                                watchBeginMillis = now;
                                watchBeginDatagram = stateDatagrams;
                            }
                            const int rowsBuilt =
                                dsh::game::watch_begin(watch, watchMachine, movementStateClass,
                                                       derivedMovementClass, key,
                                                       &dsh::reflect::read_process_bytes,
                                                       &dsh::game::actor_class_chain_reaches,
                                                       &dsh::game::movement_component_address);
                            watchModuleKey = key;

                            DSH_INFO("%s:watch: the differential watch is running -- %d "
                                     "row(s) over this side's own machine 0x%llX, swept once per datagram "
                                     "on the send path starting at datagram %llu, %llu ms into the run%s",
                                     rolePrefix, rowsBuilt,
                                     static_cast<unsigned long long>(watchMachine),
                                     static_cast<unsigned long long>(stateDatagrams),
                                     static_cast<unsigned long long>(now - runStart),
                                     firstWalk ? "" : " (rebuilt: the machine, the module or the "
                                                      "component list changed)");

                            for (const std::string& tableLine :
                                 dsh::game::watch_table_lines(watch)) {
                                DSH_INFO("%s:%s", rolePrefix, tableLine.c_str());
                            }
                        }

                        dsh::game::WatchModuleSample sample;
                        sample.module = fresh.module;
                        sample.usable = fresh.moduleUsable;
                        sample.purgedRead = fresh.purgedRead;
                        sample.purged = fresh.purged ? 1 : 0;
                        sample.levelRead = fresh.levelRead;
                        sample.level = fresh.level;
                        sample.statusRead = fresh.statusRead;
                        if (fresh.statusRead) std::memcpy(sample.status, fresh.status, kWatchStatusBytes);
                        for (const auto& component : fresh.components) {
                            dsh::game::WatchModuleSample::Component out;
                            out.address = component.address;
                            out.name = component.objectName.empty() ? component.className
                                                                    : component.objectName;
                            out.activationRead = component.activationRead;
                            out.autoActivate = component.autoActivate ? 1 : 0;
                            out.isActive = component.isActive ? 1 : 0;
                            out.niagara = component.niagara;
                            out.assetRead = component.assetRead;
                            out.asset = component.asset;
                            out.niagaraFlagsRead = component.niagaraFlagsRead;
                            out.niagaraFlags = static_cast<std::uint8_t>(
                                (component.autoDestroy ? 0x01u : 0x00u) |
                                (component.renderingEnabled ? 0x02u : 0x00u));
                            sample.components.push_back(std::move(out));
                        }

                        watchTrace.clear();
                        const std::size_t moves =
                            dsh::game::watch_step(watch, sample, stateDatagrams, now - runStart, watchTrace);

                        (void)moves;
                        for (const dsh::game::WatchChange& change : watchTrace) {
                            DSH_INFO("%s:%s", rolePrefix,
                                     dsh::game::watch_change_line(change).c_str());
                        }

                        {
                            constexpr std::uintptr_t kBlindBegin = 0x04C0;
                            constexpr std::uintptr_t kBlindEnd   = 0x1260;
                            constexpr std::size_t    kBlindChunk = 256;
                            constexpr std::size_t    kBlindSpan  = kBlindEnd - kBlindBegin;
                            constexpr int            kBlindMovesShown = 400;
                            static std::uint8_t   blindPrev[kBlindSpan] = {};
                            static std::uint8_t   blindNow[kBlindChunk]  = {};
                            static std::uintptr_t blindCursor = 0;
                            static std::uint64_t  blindSweeps = 0;
                            static int            blindMoves  = 0;
                            static bool           blindMovesCapped = false;

                            const std::uintptr_t blindBase = watchMachine + kBlindBegin + blindCursor;
                            if (dsh::reflect::read_process_bytes(blindBase, blindNow, kBlindChunk)) {

                                if (blindSweeps >= kBlindSpan / kBlindChunk) {
                                    for (std::size_t w = 0; w + 4 <= kBlindChunk; w += 4) {
                                        std::uint32_t was = 0;
                                        std::uint32_t is  = 0;
                                        std::memcpy(&was, blindPrev + blindCursor + w, sizeof(was));
                                        std::memcpy(&is,  blindNow  + w, sizeof(is));
                                        if (was == is) continue;
                                        ++blindMoves;
                                        if (blindMoves > kBlindMovesShown) {
                                            blindMovesCapped = true;
                                            continue;
                                        }
                                        float wasF = 0.0f;
                                        float isF  = 0.0f;
                                        std::memcpy(&wasF, &was, sizeof(wasF));
                                        std::memcpy(&isF,  &is,  sizeof(isF));
                                        DSH_INFO("%s:BLIND MOVE: %04llX "
                                                 "word 0x%08X -> 0x%08X (float %.4f -> %.4f) "
                                                 "at datagram %llu (+%llu ms)", rolePrefix,
                                                 static_cast<unsigned long long>(
                                                     kBlindBegin + blindCursor + w),
                                                 static_cast<unsigned>(was),
                                                 static_cast<unsigned>(is),
                                                 static_cast<double>(wasF), static_cast<double>(isF),
                                                 static_cast<unsigned long long>(stateDatagrams),
                                                 static_cast<unsigned long long>(now - runStart));
                                    }
                                }
                                std::memcpy(blindPrev + blindCursor, blindNow, kBlindChunk);
                                ++blindSweeps;
                                blindCursor += kBlindChunk;
                                if (blindCursor + kBlindChunk > kBlindSpan) blindCursor = 0;
                            }

                            if (blindSweeps != 0 && (blindSweeps % (kBlindSpan / kBlindChunk)) == 0) {
                                DSH_INFO("%s:blind scan: %llu chunk(s) swept over "
                                         "%04llX..%04llX (%llu byte(s) each, cursor now %04llX), "
                                         "%d moved word(s)%s", rolePrefix,
                                         static_cast<unsigned long long>(blindSweeps),
                                         static_cast<unsigned long long>(kBlindBegin),
                                         static_cast<unsigned long long>(kBlindEnd),
                                         static_cast<unsigned long long>(kBlindChunk),
                                         static_cast<unsigned long long>(kBlindBegin + blindCursor),
                                         blindMoves,
                                         blindMovesCapped
                                             ? " -- MORE THAN THE CAP, so later ones were not printed"
                                             : "");
                            }
                        }
                    }
                }
            }
        }

        if (++movementTicksSinceSample >= kMovementTicksPerSample) {
            movementTicksSinceSample = 0;
            if (pawn.valid) {
                hostMovement = movement_state_probe(pawn.pawn, movementStateClass,
                                                    dsh::reflect::read_process_bytes);
                if (hostMovement.readable && hostMovement.speed() > hostWindowMaxSpeed) {
                    hostWindowMaxSpeed = hostMovement.speed();
                }
            }

            for (const auto& slot : dolls.slots()) {
                if (!slot || slot->actors.empty()) continue;
                DollDiag& diag = doll_diag(slot->peer);

                diag.probe = movement_state_probe(slot->actors.front(), movementStateClass,
                                                  dsh::reflect::read_process_bytes);
                if (diag.probe.readable && diag.probe.speed() > diag.windowMaxSpeed) {
                    diag.windowMaxSpeed = diag.probe.speed();
                }

                if (diag.probe.readable && diag.velocityAppliedWindow > 0) {
                    if (same_velocity3(diag.probe.lastUpdateVelocity, diag.velocityApplied)) {
                        ++velocitySurvivorHits;
                        ++diag.survivorHitsWindow;
                    } else {
                        ++velocitySurvivorMisses;
                        ++diag.survivorMissesWindow;
                    }
                }
            }
        }

        {
            DSH_TIME_WALK("loop:recv");

            {
                DSH_TIME_WALK("loop:recv:poll");
                inbox.clear();
                transport->poll(inbox);
            }
            g_recvRead += inbox.size();
            for (const auto& d : inbox) {
            ++g_recvDispatched;
            session.on_datagram(now, d.from, d.payload);

            dsh::net::ByteReader reader(d.payload);
            dsh::net::PacketHeader hdr;
            if (!dsh::net::read_header(reader, hdr)) continue;

            if (asHost && dsh::net::message_is_relayed(hdr.type)) {
                ++relayCandidates;
                session.relay_from_endpoint(d.from, d.payload, now);
            }

            const dsh::PeerId origin{hdr.senderPeer};
            dsh::game::PuppetDoll* source = dolls.ensure(origin);
            if (source == nullptr) {
                ++dollRefused;
                continue;
            }

            if (hdr.type == dsh::net::MessageType::actionEdge) {
                DSH_TIME_WALK("recv:action");
                ++actionEdgesReceived;

                if (source->lastActionSequence != 0 &&
                    static_cast<std::int32_t>(hdr.sequence - source->lastActionSequence) <= 0) {
                    ++actionDuplicates;
                    continue;
                }
                source->lastActionSequence = hdr.sequence;
                const std::span<const std::uint8_t> body =
                    std::span<const std::uint8_t>(d.payload).subspan(reader.cursor());
                std::string why;
                if (body.size() < 3) {

                    why = "the action payload was " + std::to_string(body.size()) +
                          " byte(s) and this build speaks three (kind, pressed, weapon)";
                } else {
                    const std::uint32_t action = body[0];
                    const bool pressed = body[1] != 0;

                    if (action == dsh::game::kSupplyActionBase) {
                        const std::uint32_t supplySlot = body[2];
                        if (supplySlot > 2 || source->actors.empty()) {
                            ++actionRefused;
                            if (supplyNoteCount++ < 8) {
                                DSH_WARN("puppetsession: a reload edge arrived (slot %u) and %s -- nothing was "
                                         "replayed", supplySlot,
                                         supplySlot > 2 ? "this build has three weapons (0 front, 1 left, 2 "
                                                          "right)"
                                                        : "this side holds no doll for that peer");
                            }
                            continue;
                        }
                        const std::uintptr_t supplyDoll = source->actors.front();
                        const std::uintptr_t supplyOffset =
                            supplySlot == 0 ? 0x1630 : (supplySlot == 1 ? 0x1610 : 0x1618);
                        std::uint64_t supplyModuleRaw = 0;
                        dsh::reflect::read_process_bytes(supplyDoll + supplyOffset, &supplyModuleRaw,
                                                         sizeof(supplyModuleRaw));
                        const std::uintptr_t supplyModule = static_cast<std::uintptr_t>(supplyModuleRaw);
                        if (supplyModule < 0x10000) {
                            ++actionRefused;
                            if (supplyNoteCount++ < 8) {
                                DSH_WARN("puppetsession: a reload edge arrived for the %s weapon but the puppet "
                                         "has NO module at %04llX -- nothing was replayed",
                                         supplySlot == 0 ? "front" : (supplySlot == 1 ? "left" : "right"),
                                         static_cast<unsigned long long>(supplyOffset));
                            }
                            continue;
                        }
                        {

                            constexpr int kSupplyClassSlots = 4;
                            static std::uintptr_t s_supplyClass[kSupplyClassSlots] = {0, 0, 0, 0};
                            static dsh::reflect::FunctionRef s_supplyStarted[kSupplyClassSlots];
                            static dsh::reflect::FunctionRef s_supplyEnded[kSupplyClassSlots];
                            static int s_supplyNext = 0;
                            std::uint64_t supplyClassRaw = 0;
                            dsh::reflect::read_process_bytes(supplyModule + 0x0010, &supplyClassRaw,
                                                             sizeof(supplyClassRaw));
                            const auto supplyClass = static_cast<std::uintptr_t>(supplyClassRaw);
                            int cacheSlot = -1;
                            for (int i = 0; i < kSupplyClassSlots; ++i) {
                                if (s_supplyClass[i] == supplyClass) {
                                    cacheSlot = i;
                                    break;
                                }
                            }
                            if (cacheSlot < 0) {
                                cacheSlot = s_supplyNext;
                                s_supplyNext = (s_supplyNext + 1) % kSupplyClassSlots;
                                s_supplyClass[cacheSlot] = supplyClass;
                                s_supplyStarted[cacheSlot] = dsh::reflect::FunctionRef{};
                                s_supplyEnded[cacheSlot] = dsh::reflect::FunctionRef{};
                                std::uintptr_t cls = supplyClass;
                                for (int depth = 0; cls >= 0x10000 && depth < 32; ++depth) {
                                    if (!s_supplyStarted[cacheSlot].usable) {
                                        s_supplyStarted[cacheSlot] =
                                            dsh::reflect::reflect_resolve_function(cls, "OnSupplyStarted");
                                    }
                                    if (!s_supplyEnded[cacheSlot].usable) {
                                        s_supplyEnded[cacheSlot] =
                                            dsh::reflect::reflect_resolve_function(cls, "OnSupplyEnded");
                                    }
                                    if (s_supplyStarted[cacheSlot].usable &&
                                        s_supplyEnded[cacheSlot].usable) {
                                        break;
                                    }
                                    std::uint64_t super = 0;
                                    if (!dsh::reflect::read_process_bytes(cls + 0x40, &super, sizeof(super))) {
                                        break;
                                    }
                                    cls = static_cast<std::uintptr_t>(super);
                                }
                            }
                            const dsh::reflect::FunctionRef call =
                                pressed ? s_supplyStarted[cacheSlot] : s_supplyEnded[cacheSlot];
                            if (!call.usable) {
                                ++actionRefused;
                                if (supplyNoteCount++ < 8) {
                                    DSH_WARN("puppetsession: no UFunction named '%s' is reachable from the %s "
                                             "module's class '%s', so this weapon has no magazine to reload -- "
                                             "nothing was replayed",
                                             pressed ? "OnSupplyStarted" : "OnSupplyEnded",
                                             supplySlot == 0 ? "front" : (supplySlot == 1 ? "left" : "right"),
                                             dsh::reflect::object_class_name(supplyModule).c_str());
                                }
                                continue;
                            }
                            const std::uintptr_t target = supplyModule;

                            dsh::reflect::game_thread_push([target, call]() {
                                std::string why;
                                if (!dsh::reflect::object_is_still_registered(target, why)) return;
                                dsh::game::set_action_replay_in_flight(true);
                                alignas(8) unsigned char frame[0x10] = {};
                                std::string note;
                                (void)dsh::reflect::reflect_invoke_function(target, call, frame, note);
                                dsh::game::set_action_replay_in_flight(false);
                            });
                            ++actionApplied;
                            if (supplyNoteCount++ < 8) {
                                DSH_INFO("puppetsession: reload %s from peer %u REPLAYED on its doll's %s "
                                         "module -- the game's own '%s' drives the montage",
                                         pressed ? "STARTED" : "ENDED",
                                         static_cast<unsigned>(hdr.senderPeer),
                                         supplySlot == 0 ? "front" : (supplySlot == 1 ? "left" : "right"),
                                         pressed ? "OnSupplyStarted" : "OnSupplyEnded");
                            }
                        }
                        continue;
                    }

                    if (action != dsh::game::kShotActionBase) {

                        ++actionIgnored;
                        if (source->actors.empty()) {
                            ++actionRefused;
                            if (actionNoteCount++ < 8) {
                                DSH_WARN("puppetsession: action kind %u arrived, but this side holds NO DOLL "
                                         "for peer %u -- nothing was replayed",
                                         action, static_cast<unsigned>(hdr.senderPeer));
                            }
                            continue;
                        }
                        const std::uintptr_t actionDoll = source->actors.front();
                        const ActionReplay plan = plan_action_replay(actionDoll, action, pressed);
                        if (!plan.ok) {
                            ++actionRefused;
                            if (actionNoteCount++ < 8) {
                                DSH_WARN("puppetsession: action kind %u (%s) from peer %u was NOT replayed: %s",
                                         action, pressed ? "press" : "release",
                                         static_cast<unsigned>(hdr.senderPeer), plan.note.c_str());
                            }
                            continue;
                        }

                        const dsh::reflect::FunctionRef call = plan.call;
                        dsh::reflect::game_thread_push([doll = actionDoll, call]() {

                            std::string why;
                            if (!dsh::reflect::object_is_still_registered(doll, why)) return;
                            dsh::game::set_action_replay_in_flight(true);
                            alignas(8) unsigned char frame[0x10] = {};
                            std::string note;
                            (void)dsh::reflect::reflect_invoke_function(doll, call, frame, note);
                            dsh::game::set_action_replay_in_flight(false);
                        });
                        ++actionApplied;
                        if (actionNoteCount++ < 8) {
                            static const char* const kKindNames[4] = {"nothing", "a GUN", "melee", "the back "
                                                                     "module"};
                            DSH_INFO("puppetsession: action kind %u (%s) from peer %u REPLAYED on its doll -- "
                                     "module '%s' is %s, so the game's own %s verb drives the rest",
                                     action, pressed ? "press" : "release",
                                     static_cast<unsigned>(hdr.senderPeer), plan.className.c_str(),
                                     kKindNames[static_cast<int>(plan.kind)],
                                     pressed ? "OnPressedAction*" : "OnReleasedAction*");
                        }
                        continue;
                    }
                    const std::uint32_t shotSlot = body[2];
                    if (shotSlot > 2) {
                        ++actionRefused;
                        if (actionNoteCount++ < 8) {
                            DSH_WARN("puppetsession: a shot arrived naming weapon %u, and this build has "
                                     "three weapons (0 front, 1 left, 2 right) -- nothing was replayed",
                                     shotSlot);
                        }
                        continue;
                    }

                    ++shotEventsReceived;

                    if (!source->actors.empty()) {
                        const std::uintptr_t machine = source->actors.front();

                        const std::uintptr_t moduleOffset =
                            shotSlot == 0 ? 0x1630 : (shotSlot == 1 ? 0x1610 : 0x1618);
                        std::uint64_t moduleRaw = 0;
                        dsh::reflect::read_process_bytes(machine + moduleOffset, &moduleRaw,
                                                         sizeof(moduleRaw));
                        const std::uintptr_t shotModule = static_cast<std::uintptr_t>(moduleRaw);
                        if (shotModule < 0x10000) {

                            ++actionRefused;
                            if (actionNoteCount++ < 8) {
                                DSH_WARN("puppetsession: a %s-weapon shot arrived but the puppet has NO "
                                         "module at %04llX -- nothing was replayed",
                                         shotSlot == 0 ? "front" : (shotSlot == 1 ? "left" : "right"),
                                         static_cast<unsigned long long>(moduleOffset));
                            }
                            continue;
                        }
                        {

                            std::uint64_t classRaw = 0;
                            dsh::reflect::read_process_bytes(shotModule + 0x0010, &classRaw,
                                                             sizeof(classRaw));
                            constexpr int kShotClassSlots = 4;
                            static std::uintptr_t s_shotClass[kShotClassSlots] = {0, 0, 0, 0};
                            static dsh::reflect::FunctionRef s_shotFn[kShotClassSlots];
                            static int s_shotNext = 0;
                            const auto shotClass = static_cast<std::uintptr_t>(classRaw);
                            int cacheSlot = -1;
                            for (int i = 0; i < kShotClassSlots; ++i) {
                                if (s_shotClass[i] == shotClass) {
                                    cacheSlot = i;
                                    break;
                                }
                            }
                            if (cacheSlot < 0) {
                                cacheSlot = s_shotNext;
                                s_shotNext = (s_shotNext + 1) % kShotClassSlots;
                                s_shotClass[cacheSlot] = shotClass;
                                s_shotFn[cacheSlot] = dsh::reflect::FunctionRef{};
                                std::uintptr_t cls = shotClass;
                                for (int depth = 0; cls >= 0x10000 && depth < 32; ++depth) {
                                    s_shotFn[cacheSlot] = dsh::reflect::reflect_resolve_function(cls, "OnShot");
                                    if (s_shotFn[cacheSlot].usable) break;
                                    std::uint64_t super = 0;
                                    if (!dsh::reflect::read_process_bytes(cls + 0x40, &super,
                                                                          sizeof(super))) {
                                        break;
                                    }
                                    cls = static_cast<std::uintptr_t>(super);
                                }
                            }
                            if (s_shotFn[cacheSlot].usable) {
                                const std::uintptr_t target = shotModule;
                                const dsh::reflect::FunctionRef call = s_shotFn[cacheSlot];

                                dsh::reflect::game_thread_push([target, call]() {
                                    std::string why;
                                    if (!dsh::reflect::object_is_still_registered(target, why)) {
                                        return;
                                    }
                                    unsigned char frame[0x20] = {};
                                    std::string note;
                                    dsh::reflect::reflect_invoke_function(target, call, frame, note);
                                });
                                ++actionApplied;
                            } else {
                                ++actionRefused;
                                if (actionNoteCount++ < 8) {
                                    DSH_WARN("puppetsession: no UFunction named 'OnShot' is reachable from "
                                             "the %s module's class '%s' -- nothing was replayed",
                                             shotSlot == 0 ? "front" : (shotSlot == 1 ? "left" : "right"),
                                             dsh::reflect::object_class_name(shotModule).c_str());
                                }
                            }
                        }
                    }
                    continue;
                }
            }

            if (hdr.type == dsh::net::MessageType::ammoSpawn) {
                DSH_TIME_WALK("recv:ammo");
                ++ammoReceived;
                dsh::reflect::AmmoSpawnRequest request;
                std::string decodeNote;
                if (!dsh::game::decode_ammo_spawn(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), request,
                        decodeNote)) {
                    ++ammoDecodeFailed;
                    if (ammoNoteCount++ < 8) {
                        DSH_WARN("puppetsession: the peer's projectile was REFUSED at the wire: %s",
                                 decodeNote.c_str());
                    }
                    continue;
                }

                if (source->actors.empty()) {
                    g_ammoRefused.fetch_add(1, std::memory_order_relaxed);
                    if (ammoNoteCount++ < 8) {
                        DSH_WARN("puppetsession: peer %u fired a projectile, but this side holds NO DOLL for "
                                 "it, so there is nothing to own it or place it -- nothing was created",
                                 static_cast<unsigned>(origin.value));
                    }
                    continue;
                }
                const std::uintptr_t owner = source->actors.front();

                dsh::reflect::LockonTarget lockon;
                std::uintptr_t homingActor = 0;
                std::string homingWhy;
                if (request.targetKind == dsh::game::kAmmoTargetOwnDoll) {

                    homingActor = source->actors.front();
                } else if (request.targetKind == dsh::game::kAmmoTargetEnemy) {
                    if (!dsh::game::health_resolve_actor(request.targetName, homingActor, homingWhy)) {
                        ++ammoHomingNameMissing;
                    }
                } else if (request.targetKind == dsh::game::kAmmoTargetRefused) {
                    ++ammoHomingSenderRefused;
                    homingWhy = "the sending machine refused to name this projectile's target";
                }
                if (homingActor >= 0x10000) {
                    std::string lockonNote;
                    if (!dsh::reflect::read_lockon_target(homingActor, lockon, lockonNote)) {
                        ++ammoHomingNoComponent;
                        homingWhy = "this side's DALockonTargetInterface could not answer: " + lockonNote;
                    } else if (lockon.component == 0) {
                        ++ammoHomingNoComponent;
                        homingWhy = "'" + dsh::reflect::object_own_name(homingActor) +
                                    "' reports no lock-on component";
                    }
                }
                constexpr std::size_t kAmmoHomingOffset = 0x94;
                if (request.params[kAmmoHomingOffset] != 0 && lockon.component == 0) {

                    request.params[kAmmoHomingOffset] = 0;
                    g_ammoHomingCleared.fetch_add(1, std::memory_order_relaxed);
                    if (ammoNoteCount++ < 8) {
                        DSH_WARN("puppetsession: this projectile asks for HOMING and its target could NOT be "
                                 "reproduced here (%s) -- bEnableHoming is cleared in the copy so it flies "
                                 "straight instead of reading a target that is not there",
                                 homingWhy.empty() ? "the sender named no target" : homingWhy.c_str());
                    }
                } else if (request.params[kAmmoHomingOffset] != 0) {
                    ++ammoHomingReproduced;
                    if (ammoHomingLines++ < 6) {
                        DSH_INFO("ammo: homing reproduced -- this copy chases '%s' via component '%s', socket "
                                 "'%s' (the sender said kind %u)",
                                 dsh::reflect::object_own_name(homingActor).c_str(),
                                 lockon.componentName.c_str(),
                                 lockon.socketName.empty() ? "<none>" : lockon.socketName.c_str(),
                                 static_cast<unsigned>(request.targetKind));
                    }
                }

                {
                    const dsh::game::AmmoCopyDamage copyDamage =
                        dsh::game::clear_copy_damage(request.params, sizeof(request.params));
                    ++ammoCopiesBornHarmless;
                    if (ammoDamageLines++ < 6) {
                        DSH_INFO("ammo: the copy is born HARMLESS -- the sender's point group was [kinetic %.1f, "
                                 "thermal %.1f, effective kinetic %.1f, effective thermal %.1f, penetration "
                                 "%.1f] over a floor of %.1f, and its radial group was [%.1f, %.1f, %.1f, %.1f, "
                                 "%.1f] over floors [%.1f, %.1f, %.1f, %.1f, %.1f]; those sixteen floats are now "
                                 "zero and both enable flags are untouched, so this copy still collides and still "
                                 "shows and cannot move a health bar -- the shooter's report is what moves it",
                                 static_cast<double>(copyDamage.pointBefore[0]),
                                 static_cast<double>(copyDamage.pointBefore[1]),
                                 static_cast<double>(copyDamage.pointBefore[2]),
                                 static_cast<double>(copyDamage.pointBefore[3]),
                                 static_cast<double>(copyDamage.pointBefore[4]),
                                 static_cast<double>(copyDamage.pointMinBefore),
                                 static_cast<double>(copyDamage.radialBefore[0]),
                                 static_cast<double>(copyDamage.radialBefore[1]),
                                 static_cast<double>(copyDamage.radialBefore[2]),
                                 static_cast<double>(copyDamage.radialBefore[3]),
                                 static_cast<double>(copyDamage.radialBefore[4]),
                                 static_cast<double>(copyDamage.radialMinBefore[0]),
                                 static_cast<double>(copyDamage.radialMinBefore[1]),
                                 static_cast<double>(copyDamage.radialMinBefore[2]),
                                 static_cast<double>(copyDamage.radialMinBefore[3]),
                                 static_cast<double>(copyDamage.radialMinBefore[4]));
                    }
                }
                dsh::reflect::AmmoSpawnPrepared prepared;
                std::string prepareNote;
                if (!dsh::reflect::prepare_ammo_spawn(request, lockon, prepared, prepareNote)) {
                    g_ammoRefused.fetch_add(1, std::memory_order_relaxed);
                    if (g_ammoRefusalLines.fetch_add(1, std::memory_order_relaxed) < 8) {
                        DSH_WARN("puppetsession: the projectile could not be prepared: %s",
                                 prepareNote.c_str());
                    }
                    continue;
                }
                if (!prepareNote.empty() && g_ammoRefusalLines.fetch_add(1, std::memory_order_relaxed) < 8) {
                    DSH_WARN("puppetsession: projectile prepared with a caveat: %s", prepareNote.c_str());
                }

                dsh::reflect::ActorPlacement placement;
                placement.locationCm[0] = request.locationCm[0];
                placement.locationCm[1] = request.locationCm[1];
                placement.locationCm[2] = request.locationCm[2];
                placement.pitch = request.rotation[0];
                placement.yaw = request.rotation[1];
                placement.roll = request.rotation[2];
                placement.valid = true;
                dsh::reflect::game_thread_push([prepared, placement, owner]() {

                    g_ammoDispatchRan.fetch_add(1, std::memory_order_relaxed);

                    if (owner >= 0x10000) {
                        std::string ownerWhy;
                        if (!dsh::reflect::object_is_still_registered(owner, ownerWhy)) {
                            dsh::game::note_ammo_refused();
                            return;
                        }
                    }
                    std::uintptr_t actor = 0;
                    std::string    spawnNote;
                    if (dsh::reflect::spawn_ammo_from_params(prepared, placement, owner, owner, actor,
                                                              spawnNote)) {

                        g_ammoCreated.fetch_add(1, std::memory_order_relaxed);

                        {
                            const int index = g_ammoCreatedPublished.load(std::memory_order_relaxed);
                            if (index < kAmmoProbeSlots) {
                                AmmoCreatedSlot& slot = g_ammoCreatedSlot[index];
                                slot.actor = actor;
                                slot.ms = GetTickCount64();
                                slot.locationCm[0] = placement.locationCm[0];
                                slot.locationCm[1] = placement.locationCm[1];
                                slot.locationCm[2] = placement.locationCm[2];
                                g_ammoCreatedPublished.store(index + 1, std::memory_order_release);
                            }
                        }
                        if (g_ammoSpawnLines.fetch_add(1, std::memory_order_relaxed) < 12) {
                            DSH_INFO("ammo: %s", spawnNote.c_str());
                        }

                        if (g_ammoSpawnLines.load(std::memory_order_relaxed) <= 1) {
                            dsh::game::request_process_event_trace(GetTickCount64() + 600);
                        }
                    } else {
                        g_ammoRefused.fetch_add(1, std::memory_order_relaxed);
                        if (g_ammoRefusalLines.fetch_add(1, std::memory_order_relaxed) < 8) {
                            DSH_WARN("puppetsession: the projectile was NOT CREATED: %s",
                                     spawnNote.c_str());
                        }
                    }
                });
                g_ammoDispatchPushed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (hdr.type == dsh::net::MessageType::appearanceAck) {
                const std::span<const std::uint8_t> ackBody =
                    std::span<const std::uint8_t>(d.payload).subspan(reader.cursor());
                if (ackBody.size() != dsh::game::kAppearanceAckBytes) {
                    if (ackNoteCount++ < 4) {
                        DSH_WARN("puppetsession: an appearance acknowledgement was %zu byte(s) and this "
                                 "build speaks %zu (the peer whose manifest it is) -- nothing was recorded, "
                                 "and the peer it came from will keep repeating its manifest",
                                 ackBody.size(), dsh::game::kAppearanceAckBytes);
                    }
                    continue;
                }
                const dsh::PeerId owner{ackBody[0]};
                if (owner != session.local_id()) {
                    if (asHost) {
                        session.relay_to_peer(d.from, owner, d.payload, now);
                    }
                    continue;
                }
                bool known = false;
                for (std::uint8_t slot : manifestAckedPeers) {
                    if (slot == hdr.senderPeer) known = true;
                }
                if (!known) {
                    manifestAckedPeers.push_back(hdr.senderPeer);
                    DSH_INFO("puppetsession: the peer ACKNOWLEDGED this side's manifest (peer %u) -- the "
                             "two-second repeat of %zu chunk(s) stops now, and this side has spent %llu "
                             "full pass(es) of it",
                             static_cast<unsigned>(hdr.senderPeer), appearanceChunks.size(),
                             static_cast<unsigned long long>(manifestRepeats));
                }
                continue;
            }

            if (!asHost && hdr.type == dsh::net::MessageType::missionList) {
                DSH_TIME_WALK("recv:missionList");
                std::vector<dsh::game::MissionListEntry> hostEntries;
                bool hostHoldsNoRecord = false;
                std::string decodeNote;
                if (!dsh::game::decode_mission_list(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), hostEntries,
                        hostHoldsNoRecord, decodeNote)) {
                    DSH_WARN("puppetsession[client]: the host's mission list was REFUSED and nothing was "
                             "compared: %s", decodeNote.c_str());
                    continue;
                }
                missionListArrived = true;
                if (hostHoldsNoRecord) {
                    DSH_INFO("puppetsession[client]: the host explicitly reports that its save holds NO boss "
                             "record at all (%s) -- so every boss cleared HERE is un-cleared on the host", decodeNote.c_str());
                }

                std::string clearedNote;
                const std::vector<dsh::reflect::BossStateRecord> mine =
                    dsh::reflect::all_boss_states(clearedNote);
                auto hostState = [&](const std::string& id) -> std::uint8_t {
                    for (const dsh::game::MissionListEntry& e : hostEntries) {
                        if (e.id == id) return e.state;
                    }
                    return 0;
                };

                std::string commandLine = "mission uncomplete";
                std::uint32_t clearedHere = 0;
                std::uint32_t worthIt = 0;
                for (const dsh::reflect::BossStateRecord& record : mine) {
                    if (record.state != 2) continue;
                    ++clearedHere;
                    const std::uint8_t host = hostState(record.id);
                    if (host == 2) {
                        DSH_INFO("puppetsession[client]: mission '%s' -- the HOST has already cleared it "
                                 "(Cleared(2)), so it is left alone here", record.id.c_str());
                        continue;
                    }
                    ++worthIt;
                    commandLine += " " + record.id;
                    DSH_INFO("puppetsession[client]: mission '%s' -- cleared HERE, and the HOST has NOT "
                             "cleared it (%s): PUT IT BACK and reload", record.id.c_str(),
                             host == 1 ? "Attempted(1)" : "not cleared (no record)");
                }
                if (clearedHere == 0) {
                    DSH_INFO("puppetsession[client]: this machine has cleared no boss (%s), so there is "
                             "nothing to put back", clearedNote.c_str());
                } else if (worthIt == 0) {
                    DSH_INFO("puppetsession[client]: all %u boss(es) cleared here are also cleared on the "
                             "host -- NOTHING to put back", clearedHere);
                } else {

                    std::vector<std::string> toPutBack;
                    {
                        std::string rest = commandLine;
                        std::size_t space = rest.find(' ');
                        while (space != std::string::npos) {
                            rest = rest.substr(space + 1);
                            space = rest.find(' ');
                            if (space == std::string::npos) { if (!rest.empty()) toPutBack.push_back(rest); break; }
                            const std::string token = rest.substr(0, space);
                            if (!token.empty()) toPutBack.push_back(token);
                        }
                    }
                    std::uint32_t written = 0;
                    for (const std::string& id : toPutBack) {
                        const dsh::reflect::BossStateWriteResult write = dsh::reflect::set_boss_state(id, 1);
                        if (write.written || (write.beforeMeasured && write.before == 1)) ++written;
                        for (const std::string& one : write.report_lines()) DSH_INFO("%s", one.c_str());
                    }
                    if (written != 0) {
                        for (const std::string& one : dsh::reflect::mission_save_game()) DSH_INFO("%s", one.c_str());
                    }
                    DSH_INFO("puppetsession[client]: %u of %u boss(es) cleared here were NOT cleared on the "
                             "host, and %u of them are now Attempted(1) in this save. RELOAD THE LEVEL BY HAND "
                             "now (the reload ends this session) -- the arena will come up as an unfinished "
                             "mission, staged by the game itself. The line was: %s",
                             worthIt, clearedHere, written, commandLine.c_str());
                }
                continue;
            }

            if (hdr.type == dsh::net::MessageType::itemUse) {
                DSH_TIME_WALK("recv:item");
                ++itemUsesReceived;

                const std::uint64_t itemArrivedMs = GetTickCount64();
                std::string itemPath;
                std::string itemDecodeNote;
                if (!dsh::game::decode_item_use(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), itemPath,
                        itemDecodeNote)) {
                    if (itemNoteCount++ < 8) {
                        DSH_WARN("puppetsession: the peer's item use was REFUSED at the wire: %s",
                                 itemDecodeNote.c_str());
                    }
                    continue;
                }
                if (source->actors.empty()) {
                    if (itemNoteCount++ < 8) {
                        DSH_WARN("puppetsession: peer %u used '%s' but this side holds NO DOLL for it -- "
                                 "nothing was replayed", static_cast<unsigned>(hdr.senderPeer), itemPath.c_str());
                    }
                    continue;
                }
                const std::uintptr_t itemDoll = source->actors.front();
                const std::uintptr_t itemClass = dsh::reflect::load_asset_by_path(itemPath);
                if (itemClass < 0x10000) {
                    if (itemNoteCount++ < 8) {
                        DSH_WARN("puppetsession: the item '%s' could not be loaded on this machine, so its use "
                                 "was NOT replayed", itemPath.c_str());
                    }
                    continue;
                }
                std::uint64_t itemCdoRaw = 0;
                dsh::reflect::read_process_bytes(itemClass + 0x0118, &itemCdoRaw, sizeof(itemCdoRaw));
                const std::uintptr_t itemCdo = static_cast<std::uintptr_t>(itemCdoRaw);
                if (itemCdo < 0x10000) {
                    if (itemNoteCount++ < 8) {
                        DSH_WARN("puppetsession: the item class '%s' has no default object at, so its "
                                 "use was NOT replayed", itemPath.c_str());
                    }
                    continue;
                }
                dsh::reflect::game_thread_push([itemDoll, itemCdo, itemPath, itemArrivedMs]() {
                    std::string why;
                    if (!dsh::reflect::object_is_still_registered(itemDoll, why)) return;
                    if (!dsh::reflect::object_is_still_registered(itemCdo, why)) return;

                    std::uint64_t classRaw = 0;
                    if (!dsh::reflect::read_process_bytes(itemCdo + 0x0010, &classRaw, sizeof(classRaw))) return;
                    std::uintptr_t cls = static_cast<std::uintptr_t>(classRaw);
                    dsh::reflect::FunctionRef canUse;
                    dsh::reflect::FunctionRef useFn;
                    for (int depth = 0; cls >= 0x10000 && depth < 32; ++depth) {
                        if (!canUse.usable) canUse = dsh::reflect::reflect_resolve_function(cls, "CanUse");
                        if (!useFn.usable) useFn = dsh::reflect::reflect_resolve_function(cls, "Use");
                        if (canUse.usable && useFn.usable) break;
                        std::uint64_t super = 0;
                        if (!dsh::reflect::read_process_bytes(cls + 0x40, &super, sizeof(super))) break;
                        cls = static_cast<std::uintptr_t>(super);
                    }
                    if (!canUse.usable || !useFn.usable) {
                        DSH_WARN("puppetsession: '%s' declares neither 'CanUse' nor 'Use' on its class chain, so "
                                 "nothing was replayed", itemPath.c_str());
                        return;
                    }
                    const dsh::reflect::CharacterVitals before = dsh::reflect::character_vitals(itemDoll);
                    const std::uint64_t sourceActor = itemDoll;
                    const std::uint64_t targetActor = itemDoll;
                    alignas(8) unsigned char canFrame[0x20] = {};
                    std::memcpy(canFrame + 0x00, &sourceActor, sizeof(sourceActor));
                    std::memcpy(canFrame + 0x08, &targetActor, sizeof(targetActor));
                    std::string canNote;
                    dsh::game::set_action_replay_in_flight(true);
                    (void)dsh::reflect::reflect_invoke_function(itemCdo, canUse, canFrame, canNote);
                    unsigned char accepted = 0;
                    std::memcpy(&accepted, canFrame + 0x10, sizeof(accepted));
                    bool used = false;
                    if (accepted != 0) {
                        alignas(8) unsigned char useFrame[0x20] = {};
                        std::memcpy(useFrame + 0x00, &sourceActor, sizeof(sourceActor));
                        std::memcpy(useFrame + 0x08, &targetActor, sizeof(targetActor));
                        std::string useNote;
                        (void)dsh::reflect::reflect_invoke_function(itemCdo, useFn, useFrame, useNote);
                        unsigned char returned = 0;
                        std::memcpy(&returned, useFrame + 0x10, sizeof(returned));
                        used = returned != 0;
                    }
                    dsh::game::set_action_replay_in_flight(false);
                    const dsh::reflect::CharacterVitals after = dsh::reflect::character_vitals(itemDoll);
                    const std::uint64_t replayAtMs = GetTickCount64();
                    DSH_INFO("puppetsession: item use REPLAYED on its doll -- '%s': CanUse=%s%s; the doll's "
                             "health %s -> %s (of %.1f); the call ran %llu ms after the datagram was read",
                             itemPath.c_str(),
                             accepted != 0 ? "true" : "false (the game said no, so Use was NOT called)",
                             used ? ", Use returned true" : "",
                             before.read ? std::to_string(before.health).c_str() : "NOT read",
                             after.read ? std::to_string(after.health).c_str() : "NOT read", after.maxHealth,
                             static_cast<unsigned long long>(
                                 replayAtMs >= itemArrivedMs ? replayAtMs - itemArrivedMs : 0));
                });
                ++itemUsesReplayed;
                continue;
            }

            if (asHost && hdr.type == dsh::net::MessageType::missionListRequest) {
                DSH_TIME_WALK("recv:missionListRequest");
                std::string recordsNote;
                const std::vector<dsh::reflect::BossStateRecord> records = dsh::reflect::all_boss_states(recordsNote);
                std::vector<dsh::game::MissionListEntry> entries;
                for (const dsh::reflect::BossStateRecord& record : records) {
                    dsh::game::MissionListEntry e;
                    e.id = record.id;
                    e.state = record.state;
                    entries.push_back(std::move(e));
                }
                std::string encodeNote;
                const std::vector<std::uint8_t> payload =
                    dsh::game::encode_mission_list(entries, encodeNote);
                session.send_mission_list_to(dsh::PeerId{hdr.senderPeer}, missionListSequence++, payload);
                DSH_INFO("puppetsession[host]: answered the client's mission-list request with %zu entr(ies) "
                         "(%s). The records come from the SAVE-WIDE BossStates map, confirmed one by one by "
                         "the game's own GetBossState (%s) -- not from this level's volumes, which was the "
                         "mistake v452 made", entries.size(), encodeNote.c_str(), recordsNote.c_str());
                continue;
            }

            if (asHost && hdr.type == dsh::net::MessageType::healthReport) {
                DSH_TIME_WALK("recv:healthReport");
                dsh::game::HealthReportResult applied;
                if (!dsh::game::host_apply_health_report(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), applied)) {
                    ++healthReportsRefused;
                    if (healthReportsRefused <= 5) {
                        DSH_WARN("puppetsession: health report REFUSED: %s", applied.note.c_str());
                    }
                } else {
                    ++healthReportsReceived;
                    healthEntriesAppliedOnHost += applied.applied;
                    if (applied.applied != 0 || healthReportsReceived <= 5) {
                        for (const std::string& line : applied.report_lines()) {
                            DSH_INFO("puppetsession: %s", line.c_str());
                        }
                    }
                }
            }            if (!asHost && hdr.type == dsh::net::MessageType::healthBroadcast) {
                DSH_TIME_WALK("recv:healthBroadcast");
                dsh::game::HealthApplyResult applied;
                if (!dsh::game::client_apply_health_broadcast(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), applied)) {
                    ++healthBroadcastsRefused;
                    if (healthBroadcastsRefused <= 5) {
                        for (const std::string& line : applied.report_lines()) {
                            DSH_WARN("puppetsession: %s", line.c_str());
                        }
                    }
                } else {
                    ++healthBroadcastsReceived;
                    healthEntriesApplied += applied.written;
                    healthEntriesFailedToWrite += applied.failed;
                    healthEntriesNotInMyLevels += applied.nameNotFound;
                    if (applied.written != 0 || applied.failed != 0 || healthBroadcastsReceived <= 5) {
                        for (const std::string& line : applied.report_lines()) {
                            DSH_INFO("puppetsession: %s", line.c_str());
                        }
                    }
                }
            }

            if (hdr.type == dsh::net::MessageType::playerHealth) {
                DSH_TIME_WALK("recv:playerHealth");
                dsh::game::PlayerHealth value;
                std::string decodeNote;
                if (!dsh::game::decode_player_health(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), value,
                        decodeNote)) {
                    ++playerHealthRefusedAtWire;
                    if (playerHealthRefusedAtWire <= 5) {
                        DSH_WARN("puppetsession: the peer's player-health payload was REFUSED: %s",
                                 decodeNote.c_str());
                    }
                    continue;
                }
                ++playerHealthReceived;

                if (source->actors.empty()) {
                    if (++playerHealthNoPuppet <= 3) {
                        DSH_WARN("puppetsession: peer %u stated its own player's health (%.1f of %.1f) but this "
                                 "side holds NO DOLL for it yet, so there is nothing to write it into",
                                 static_cast<unsigned>(origin.value),
                                 static_cast<double>(value.health), static_cast<double>(value.maxHealth));
                    }
                    continue;
                }
                const std::uintptr_t puppet = source->actors.front();
                std::uintptr_t set = 0;
                std::string setNote;
                if (!dsh::reflect::character_attribute_set(puppet, set, setNote)) {
                    if (++playerHealthWriteFailures <= 3) {
                        DSH_WARN("puppetsession: the puppet's health could NOT be written -- %s",
                                 setNote.c_str());
                    }
                    continue;
                }
                float have = 0.0f;
                if (!dsh::reflect::read_process_bytes(set + 0x48, &have, sizeof(have))) {
                    if (++playerHealthWriteFailures <= 3) {
                        DSH_WARN("puppetsession: the puppet's health at 0x%llX could not be read back before "
                                 "the write",
                                 static_cast<unsigned long long>(set + 0x48));
                    }
                    continue;
                }
                if (std::fabs(have - value.health) < 0.5f) {
                    ++playerHealthAlreadyRight;
                    continue;
                }
                float back = 0.0f;
                std::string writeNote;
                if (!dsh::reflect::hp_write_health(set, value.health, &back, writeNote)) {
                    ++playerHealthWriteFailures;
                    if (playerHealthWriteFailures <= 5) {
                        DSH_WARN("puppetsession: the puppet's health was NOT written: %s", writeNote.c_str());
                    }
                    continue;
                }
                ++playerHealthWritten;
                if (playerHealthWriteLines++ < 8) {
                    DSH_INFO("puppetsession: puppet health <- the peer's own player: %.1f of %.1f (it was "
                             "%.1f, and the write reads back %.1f)",
                             static_cast<double>(value.health), static_cast<double>(value.maxHealth),
                             static_cast<double>(have), static_cast<double>(back));
                }
            }

            if (hdr.type == dsh::net::MessageType::playerState) {
                DSH_TIME_WALK("recv:pose");
                StanceWireState wire;
                std::string decodeNote;
                if (!dsh::game::decode_stance_state(
                        std::span<const std::uint8_t>(d.payload).subspan(reader.cursor()), wire,
                        decodeNote)) {

                    if (shortStatePayloads < 2 && decodeNote != lastStateDecodeNote) {
                        ++shortStatePayloads;
                        lastStateDecodeNote = decodeNote;
                        DSH_WARN("puppetsession: %s -- no position, no stance and no velocity is applied "
                                 "from this datagram; the two ends ship together, so a peer sending a "
                                 "different payload size is a peer from another build",
                                 decodeNote.c_str());
                    }
                    continue;
                }
                const float* const posM = wire.posM;
                const float yawRad = wire.yawRad;
                const float pitchRad = wire.pitchRad;

                {
                    dsh::sync::PoseSample sample;
                    sample.samplerTimeMs =
                        static_cast<std::uint64_t>(hdr.sequence) * kStateIntervalMs;
                    sample.x = posM[0];
                    sample.y = posM[1];
                    sample.z = posM[2];
                    sample.yaw = yawRad;
                    sample.pitch = pitchRad;
                    sample.flags = 0;

                    if (source->latestSampleHave) {
                        const float jumpX = sample.x - source->latestSampleM[0];
                        const float jumpY = sample.y - source->latestSampleM[1];
                        const float jumpZ = sample.z - source->latestSampleM[2];
                        const float jumpM = std::sqrt(jumpX * jumpX + jumpY * jumpY + jumpZ * jumpZ);
                        if (jumpM > 20.0f) {
                            source->interp.clear();
                            ++source->teleportsSeen;
                            if (now - source->lastTeleportLogMs > 4000) {
                                source->lastTeleportLogMs = now;
                                DSH_INFO("puppetsession: peer %u's doll was TELEPORTED %.1f m in one sample "
                                         "(--> now at %.1f, %.1f, %.1f m): the interpolation buffer is thrown "
                                         "away and the doll is placed at the destination on the next tick, "
                                         "instead of walking there through a hundred milliseconds it does "
                                         "not have",
                                         static_cast<unsigned>(source->peer.value),
                                         static_cast<double>(jumpM), static_cast<double>(sample.x),
                                         static_cast<double>(sample.y), static_cast<double>(sample.z));
                            }
                        }
                    }
                    source->interp.push(sample);
                    source->interpHave = true;

                    source->clock.note_arrival(sample.samplerTimeMs, steady_ms());

                    source->latestSampleM[0] = sample.x;
                    source->latestSampleM[1] = sample.y;
                    source->latestSampleM[2] = sample.z;
                    source->latestSampleHave = true;
                }

                if (source->actors.empty()) continue;

                auto update = std::make_shared<PositionUpdate>();
                update->posM[0] = posM[0];
                update->posM[1] = posM[1];
                update->posM[2] = posM[2];
                update->yawRad = yawRad;
                update->pitchRad = pitchRad;
                update->stance = wire.stance;
                update->velocity[0] = wire.velocity[0];
                update->velocity[1] = wire.velocity[1];
                update->velocity[2] = wire.velocity[2];
                update->hostMovementMode = wire.movementMode;
                update->acceleration[0] = wire.acceleration[0];
                update->acceleration[1] = wire.acceleration[1];
                update->acceleration[2] = wire.acceleration[2];
                update->exclusiveTypes = wire.exclusiveTypes;
                update->exclusiveOwnerKind = wire.exclusiveOwnerKind;
                update->movementInput = wire.movementInput;
                inputLastReceived = wire.movementInput;

                const bool energyPairNonZero =
                    wire.energyTimers.intermissionTime != 0.0f ||
                    wire.energyTimers.fastChargeStartTime != 0.0f;
                if (energyPairNonZero) energyPairEverNonZero = true;
                const bool peerEnergyReadable = energyPairNonZero || energyPairEverNonZero;
                update->energyTimers = wire.energyTimers;
                update->energyTimersReadable = peerEnergyReadable;
                energyLastReceived = wire.energyTimers;
                energyLastReceivedReadable = peerEnergyReadable;

                update->frontWeaponIndex = wire.frontWeaponIndex;
                update->weaponClass = puppetWeaponClass;

                doll_diag(source->peer).weaponLastReceivedIndex = wire.frontWeaponIndex;
                doll_diag(source->peer).weaponLastReceivedValid = true;

                update->energyStepKnown = clientEnergyHavePrevious;
                update->energyPreviousIntermission = clientEnergyPreviousIntermission;
                clientEnergyHavePrevious = true;
                clientEnergyPreviousIntermission = wire.energyTimers.intermissionTime;

                energyLastStepKnown = update->energyStepKnown;
                energyLastStep = update->energyPreviousIntermission;
                update->stanceClass = stanceClass;
                update->actors = source->actors;

                DSH_TIME_WALK("loop:apply");

                DollDiag& dg = doll_diag(source->peer);
                if (dsh::reflect::run_on_game_thread(
                        [update, &stanceApplied, &stanceChanged, &stanceFailed, &stanceNoteCount,
                         &stanceLastApplied, &velocityApplied, &velocityChanged, &velocityFailed,
                         &velocityNoteCount, &velocityLastApplied, &hostModeAtLastApply = dg.peerModeAtLastApply,
                         &accelerationApplied, &accelerationChanged, &accelerationFailed,
                         &accelerationLastApplied, &accelerationNoteCount,
                         &velocityAppliedThisWindow = dg.velocityApplied,
                         &velocityAppliedThisWindowCount = dg.velocityAppliedWindow,
                         &inputFlagsApplied, &inputFlagsChanged, &inputFlagsFailed,
                         &inputFlagsChangedBoost, &inputVectorApplied, &inputVectorChanged,
                         &inputVectorFailed, &inputLastWritten, &inputNoteCount,
                         &puppetFlagMoves,
                         &puppetDerivedGetters,
                         &diagInitSampled = dg.initSampled, &diagInitValue = dg.initValue,
                         &diagInitNote = dg.initNote,
                         &modeWrittenThisWindow = dg.modeWrittenWindow,
                         &modeAppliedThisWindow = dg.modeAppliedWindow,
                         &modeRevertedThisWindow = dg.modeRevertedWindow,
                         &modeRefusedThisWindow = dg.modeRefusedWindow,
                         &modeWrittenSession = dg.modeWritten,
                         &modeAppliedSession = dg.modeApplied,
                         &modeRevertedSession = dg.modeReverted,
                         &integrationBase = dg.integBase,
                         &haveIntegrationBase = dg.integHave,
                         &integrationComparisons = dg.integComparisons,
                         &integrationComparisonsThisWindow = dg.integComparisonsWindow,
                         &integrationMoves = dg.integMoves,
                         &integrationMovesThisWindow = dg.integMovesWindow,
                         &integrationMaxMoveCm = dg.integMaxCm,
                         &integrationMaxMoveThisWindowCm = dg.integMaxWindowCm,
                         &integrationLastMoveCm = dg.integLastCm,
                         &sprintCallsThisWindow, &sprintRefusedThisWindow,
                         &sprintByteMatchedThisWindow, &sprintByteRevertedThisWindow,
                         &sprintDerivedAgreedThisWindow, &sprintRawAgreedThisWindow, &sprintCalls,
                         &sprintRefused, &sprintByteMatched, &sprintByteReverted, &sprintRawAgreed,
                         &sprintDerivedAgreed, &sprintLast,
                         &energyApplied, &energyChanged, &energyFailed, &energyLastApplied,
                         &energyNoteCount, &clientAppliedHavePrevious,
                         &clientAppliedPreviousIntermission, &energyLastNote,
                         &weaponStated = dg.weaponStated, &weaponCalls = dg.weaponCalls,
                         &weaponApplied = dg.weaponApplied, &weaponHolding = dg.weaponHolding,
                         &weaponFailed = dg.weaponFailed, &weaponNotRead = dg.weaponNotRead,
                         &weaponLastApplied = dg.weaponLastApplied,
                         &weaponNoteCount = dg.weaponNoteCount, &weaponLastNote = dg.weaponLastNote,
                         &sprintEffectHandle, &sprintEffectActor, &sprintEffectApplies,
                         &sprintEffectRefusals, &sprintEffectRemovals, &sprintEffectRemovalFailures,
                         &sprintEffectRetryWarnings, &sprintEdgeForAnimation, &sprintDelegatesPrinted,
                         &exclusiveStarted = dg.exclusiveStarted, &exclusiveEnded = dg.exclusiveEnded,
                         &exclusiveRefused = dg.exclusiveRefused, &exclusiveFailed = dg.exclusiveFailed,
                         &exclusiveNoteCount = dg.exclusiveNoteCount,

                         &lastExclusiveTypes = source->lastExclusiveTypes,
                         &lastExclusiveOwnerKind = source->lastExclusiveOwnerKind,
                         &exclusiveDelegatesPrinted, &localPawnMachine]() {

                            {
                                static float         lastPos[3] = {0.0f, 0.0f, 0.0f};
                                static std::uint64_t lastMs = 0;
                                static std::uint64_t updates = 0;
                                static std::uint64_t jumps = 0;
                                static std::uint64_t maxGapMs = 0;
                                static float         maxStepM = 0.0f;
                                static std::uint64_t reportMs = 0;
                                const std::uint64_t nowMs = GetTickCount64();
                                const float dx = update->posM[0] - lastPos[0];
                                const float dy = update->posM[1] - lastPos[1];
                                const float dz = update->posM[2] - lastPos[2];
                                const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
                                if (lastMs != 0) {
                                    ++updates;
                                    ++g_recvApplied;
                                    const std::uint64_t gap = nowMs - lastMs;
                                    if (gap > maxGapMs) maxGapMs = gap;
                                    if (step > maxStepM) maxStepM = step;
                                    if (step > 10.0f) ++jumps;
                                }
                                lastPos[0] = update->posM[0];
                                lastPos[1] = update->posM[1];
                                lastPos[2] = update->posM[2];
                                lastMs = nowMs;
                                if (reportMs == 0) reportMs = nowMs;
                                if (nowMs - reportMs >= 10000) {
                                    DSH_INFO("puppetsession: pose stream: %llu update(s) in the last %llu ms, "
                                             "largest step %.2f m, longest gap %llu ms, step over 10 m on %llu",
                                             static_cast<unsigned long long>(updates),
                                             static_cast<unsigned long long>(nowMs - reportMs),
                                             static_cast<double>(maxStepM),
                                             static_cast<unsigned long long>(maxGapMs),
                                             static_cast<unsigned long long>(jumps));
                                    updates = 0;
                                    jumps = 0;
                                    maxGapMs = 0;
                                    maxStepM = 0.0f;
                                    reportMs = nowMs;
                                }
                            }

                            for (const std::uintptr_t actor : update->actors) {

                                const dsh::game::StanceApplyResult stance = dsh::game::doll_stance_apply(
                                    actor, update->stanceClass, update->stance);
                                if (stance.applied) {
                                    ++stanceApplied;
                                    if (stance.changed) ++stanceChanged;
                                    stanceLastApplied = update->stance;
                                } else {
                                    ++stanceFailed;
                                    if (stanceNoteCount < 3) {
                                        ++stanceNoteCount;
                                        DSH_WARN("puppetsession: the stance was NOT applied to the "
                                                 "puppet: %s", stance.note.c_str());
                                    }
                                }

                                const dsh::game::VelocityApplyResult velocity =
                                    dsh::game::puppet_velocity_apply(actor, update->stanceClass,
                                                                     update->velocity,
                                                                     update->hostMovementMode,
                                                                     update->acceleration);

                                if (velocity.applied) {
                                    ++velocityApplied;
                                    ++velocityAppliedThisWindowCount;
                                    velocityAppliedThisWindow[0] = velocity.written[0];
                                    velocityAppliedThisWindow[1] = velocity.written[1];
                                    velocityAppliedThisWindow[2] = velocity.written[2];
                                    update->appliedVelocity[0] = velocity.written[0];
                                    update->appliedVelocity[1] = velocity.written[1];
                                    update->appliedVelocity[2] = velocity.written[2];
                                    update->appliedVelocityOk = true;
                                    if (velocity.changed) ++velocityChanged;
                                    velocityLastApplied[0] = velocity.written[0];
                                    velocityLastApplied[1] = velocity.written[1];
                                    velocityLastApplied[2] = velocity.written[2];
                                    hostModeAtLastApply = update->hostMovementMode;
                                } else {
                                    ++velocityFailed;
                                    if (velocityNoteCount < 3) {
                                        ++velocityNoteCount;
                                        DSH_WARN("puppetsession: the velocity was NOT applied to the "
                                                 "puppet: %s", velocity.note.c_str());
                                    }
                                }

                                if (velocity.accelerationRequested) {
                                    if (velocity.accelerationApplied) {
                                        ++accelerationApplied;
                                        if (velocity.accelerationChanged) ++accelerationChanged;
                                        accelerationLastApplied[0] = velocity.accelerationWritten[0];
                                        accelerationLastApplied[1] = velocity.accelerationWritten[1];
                                        accelerationLastApplied[2] = velocity.accelerationWritten[2];
                                    } else {
                                        ++accelerationFailed;
                                        if (accelerationNoteCount < 3) {
                                            ++accelerationNoteCount;
                                            DSH_WARN("puppetsession: the acceleration was NOT applied to "
                                                     "the puppet: %s",
                                                     velocity.accelerationNote.c_str());
                                        }
                                    }
                                }

                                if (update->exclusiveTypes != lastExclusiveTypes ||
                                    update->exclusiveOwnerKind != lastExclusiveOwnerKind) {
                                    const auto kind = static_cast<dsh::game::ExclusiveOwnerKind>(
                                        update->exclusiveOwnerKind);

                                    if (kind == dsh::game::ExclusiveOwnerKind::none) {
                                        std::string endNote;
                                        if (dsh::reflect::character_end_exclusive_mode(actor, endNote)) {
                                            ++exclusiveEnded;
                                            DSH_INFO("puppetsession: %s", endNote.c_str());
                                        } else {
                                            ++exclusiveFailed;
                                            if (exclusiveNoteCount < 3) {
                                                ++exclusiveNoteCount;
                                                DSH_WARN("puppetsession: the puppet's exclusive mode was NOT "
                                                         "ended: %s", endNote.c_str());
                                            }
                                        }
                                    } else if (kind == dsh::game::ExclusiveOwnerKind::other) {
                                        ++exclusiveRefused;
                                        if (exclusiveNoteCount < 3) {
                                            ++exclusiveNoteCount;
                                            DSH_WARN("puppetsession: the host's exclusive mode is owned by an "
                                                     "object that is not one of the four weapon slots (types "
                                                     "%u, kind %u) -- NOT applied, because this side cannot "
                                                     "name the puppet's own owner for it",
                                                     static_cast<unsigned>(update->exclusiveTypes),
                                                     static_cast<unsigned>(update->exclusiveOwnerKind));
                                        }
                                    } else {
                                        const std::uintptr_t slotOffset =
                                            dsh::game::exclusive_owner_slot_offset(kind);
                                        std::uint64_t ownerSlot = 0;
                                        if (slotOffset == 0 ||
                                            !dsh::reflect::read_process_bytes(actor + slotOffset, &ownerSlot,
                                                                              sizeof(ownerSlot)) ||
                                            ownerSlot < 0x10000) {
                                            ++exclusiveRefused;
                                            if (exclusiveNoteCount < 3) {
                                                ++exclusiveNoteCount;
                                                DSH_WARN("puppetsession: the puppet's own '%s' slot is empty or "
                                                         "unreadable, so the host's exclusive mode (%s, types "
                                                         "%u) was NOT applied",
                                                         dsh::game::exclusive_owner_kind_text(kind),
                                                         dsh::game::exclusive_types_text(update->exclusiveTypes).c_str(),
                                                         static_cast<unsigned>(update->exclusiveTypes));
                                            }
                                        } else {
                                            std::string startNote;
                                            if (dsh::reflect::character_start_exclusive_mode(
                                                    actor, static_cast<std::uintptr_t>(ownerSlot),
                                                    update->exclusiveTypes, startNote)) {
                                                ++exclusiveStarted;
                                                DSH_INFO("puppetsession: %s ('%s')", startNote.c_str(),
                                                         dsh::game::exclusive_owner_kind_text(kind));
                                            } else {
                                                ++exclusiveFailed;
                                                if (exclusiveNoteCount < 3) {
                                                    ++exclusiveNoteCount;
                                                    DSH_WARN("puppetsession: the puppet's exclusive mode was NOT "
                                                             "started: %s", startNote.c_str());
                                                }
                                            }
                                        }
                                    }
                                    lastExclusiveTypes = update->exclusiveTypes;
                                    lastExclusiveOwnerKind = update->exclusiveOwnerKind;
                                }

                                if (!exclusiveDelegatesPrinted) {
                                    exclusiveDelegatesPrinted = true;
                                    std::string ownRefusalForDelegates;
                                    const std::uintptr_t ownForDelegates =
                                        local_pawn_machine(localPawnMachine, ownRefusalForDelegates);
                                    std::string mineDelegates;
                                    std::string theirsDelegates;
                                    dsh::reflect::character_exclusive_delegate_text(ownForDelegates,
                                                                                    mineDelegates);
                                    dsh::reflect::character_exclusive_delegate_text(actor, theirsDelegates);
                                    DSH_INFO("puppetsession: the exclusive-mode DELEGATES -- mine: %s | "
                                             "theirs(puppet): %s",
                                             mineDelegates.c_str(), theirsDelegates.c_str());
                                }

                                if (velocity.modeRequested) {
                                    if (velocity.modeWritten) {
                                        ++modeWrittenThisWindow;
                                        ++modeWrittenSession;
                                    }
                                    if (velocity.modeApplied) {
                                        ++modeAppliedThisWindow;
                                        ++modeAppliedSession;
                                    } else if (velocity.modeReadBack) {
                                        ++modeRevertedThisWindow;
                                        ++modeRevertedSession;
                                    }
                                } else {
                                    ++modeRefusedThisWindow;
                                }

                                if (velocity.locationRead) {
                                    if (haveIntegrationBase) {
                                        const float dx = velocity.location[0] - integrationBase[0];
                                        const float dy = velocity.location[1] - integrationBase[1];
                                        const float dz = velocity.location[2] - integrationBase[2];
                                        const float moved =
                                            std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
                                        ++integrationComparisons;
                                        ++integrationComparisonsThisWindow;
                                        integrationLastMoveCm = moved;
                                        if (moved > integrationMaxMoveCm) integrationMaxMoveCm = moved;
                                        if (moved > integrationMaxMoveThisWindowCm) {
                                            integrationMaxMoveThisWindowCm = moved;
                                        }

                                        if (moved > kIntegrationMoveThresholdCm) {
                                            ++integrationMoves;
                                            ++integrationMovesThisWindow;
                                        }
                                    }
                                    integrationBase[0] = velocity.location[0];
                                    integrationBase[1] = velocity.location[1];
                                    integrationBase[2] = velocity.location[2];
                                    haveIntegrationBase = true;
                                }

                                update->appliedVelocityResult = velocity;

                                const dsh::game::MovementInputApplyResult input =
                                    dsh::game::puppet_movement_input_apply(actor, update->stanceClass,
                                                                           update->movementInput);
                                update->appliedMovementFlagsOk = input.flagsApplied;
                                update->appliedMovementVectorOk = input.vectorApplied;
                                update->appliedMovementFlagsChanged = input.flagsChanged;
                                update->appliedMovementVectorChanged = input.vectorChanged;
                                update->appliedMovementInput = input.written;

                                {
                                    static int lastSprintSeen = -1;
                                    static int lastHoverSeen  = -1;
                                    std::uint8_t sprintByte = 0;
                                    std::uint8_t hoverByte  = 0;
                                    if (dsh::reflect::read_process_bytes(
                                            actor + dsh::game::kCharacterSprintOffset, &sprintByte, 1) &&
                                        dsh::reflect::read_process_bytes(
                                            actor + dsh::game::kCharacterHoverOffset, &hoverByte, 1)) {
                                        const int sprintNow = (sprintByte & 0x01u) ? 1 : 0;
                                        const int hoverNow  = (hoverByte & 0x01u) ? 1 : 0;
                                        if (sprintNow != lastSprintSeen || hoverNow != lastHoverSeen) {
                                            if (lastSprintSeen >= 0) ++puppetFlagMoves;
                                            DSH_INFO("puppetsession[client]:the puppet's OWN bSprint/bHover "
                                                     "now read sprint=%d hover=%d (the host's datagram said "
                                                     "sprint=%d hover=%d) -- this line is printed only when "
                                                     "the pair CHANGED, so a change here that this side did "
                                                     "not write is the game's own hand",
                                                     sprintNow, hoverNow,
                                                     update->movementInput.sprint ? 1 : 0,
                                                     update->movementInput.hover ? 1 : 0);
                                            lastSprintSeen = sprintNow;
                                            lastHoverSeen  = hoverNow;
                                        }
                                    }
                                }
                                if (input.flagsApplied || input.vectorApplied) {
                                    inputLastWritten = input.written;
                                }
                                if (input.flagsApplied) {
                                    ++inputFlagsApplied;
                                    if (input.flagsChanged) ++inputFlagsChanged;

                                    if (input.before.boostJump != input.written.boostJump ||
                                        input.before.boostDash != input.written.boostDash) {
                                        ++inputFlagsChangedBoost;
                                    }
                                } else if (input.flagsChanged) {

                                    ++inputFlagsFailed;
                                }
                                if (input.vectorApplied) {
                                    ++inputVectorApplied;
                                    if (input.vectorChanged) ++inputVectorChanged;
                                } else if (input.vectorChanged) {
                                    ++inputVectorFailed;
                                }
                                if (!input.note.empty() && inputNoteCount < 3) {
                                    ++inputNoteCount;
                                    DSH_WARN("puppetsession: the movement input was NOT fully applied to "
                                             "the puppet: %s (before: %s; host sent: %s)",
                                             input.note.c_str(),
                                             dsh::game::movement_input_line(input.before).c_str(),
                                             dsh::game::movement_input_line(update->movementInput)
                                                 .c_str());
                                }

                                bool sprintCalledThisDispatch = false;
                                {
                                    SprintCapture capture;
                                    capture.asked = input.written.sprint != 0;
                                    capture.byteBeforeSetSprint = input.written.sprint != 0;
                                    capture.byteBeforeSetSprintKnown = input.flagsApplied;
                                    const bool sprintGateOpen =
                                        input.before.sprint != input.written.sprint;
                                    if (sprintGateOpen) {
                                        std::string sprintNote;
                                        if (dsh::reflect::character_call_set_sprint(
                                                actor, capture.asked, sprintNote)) {
                                            capture.called = true;
                                            sprintCalledThisDispatch = true;
                                            ++sprintCalls;
                                            ++sprintCallsThisWindow;
                                        } else {
                                            capture.refused = true;
                                            capture.note = sprintNote;
                                            ++sprintRefused;
                                            ++sprintRefusedThisWindow;
                                        }
                                        if (capture.called) {

                                            std::uint8_t after = 0;
                                            if (dsh::reflect::read_process_bytes(
                                                    actor + dsh::game::kCharacterSprintOffset, &after,
                                                    sizeof(after))) {
                                                capture.byteAfterKnown = true;
                                                capture.byteAfter = after != 0;
                                                if (capture.byteAfter == capture.asked) {
                                                    ++sprintByteMatched;
                                                    ++sprintByteMatchedThisWindow;
                                                } else {
                                                    ++sprintByteReverted;
                                                    ++sprintByteRevertedThisWindow;
                                                }
                                            } else {
                                                capture.note =
                                                    "the byte at does not read back after the "
                                                    "SetSprint call";
                                            }
                                        }
                                    }

                                    if (sprintGateOpen) sprintLast = capture;
                                }

                                {
                                    const bool sprintEdgeThisDispatch =
                                        input.before.sprint != input.written.sprint;
                                    if (sprintEdgeThisDispatch) {

                                        if (!sprintDelegatesPrinted) {
                                            sprintDelegatesPrinted = true;
                                            std::string ownDelegateRefusal;
                                            const std::uintptr_t ownDelegateMachine =
                                                local_pawn_machine(localPawnMachine, ownDelegateRefusal);
                                            std::string mineDelegates;
                                            std::string theirsDelegates;
                                            dsh::reflect::character_sprint_delegate_text(ownDelegateMachine,
                                                                                         mineDelegates);
                                            dsh::reflect::character_sprint_delegate_text(actor,
                                                                                         theirsDelegates);
                                            DSH_INFO("puppetsession: the sprint DELEGATES on the first "
                                                     "sprint edge -- mine: %s | theirs(puppet): %s",
                                                     mineDelegates.c_str(), theirsDelegates.c_str());
                                        }
                                        if (sprintEffectActor != actor && sprintEffectHandle != 0) {
                                            DSH_WARN("puppetsession: the puppet was rebuilt while a "
                                                     "sprint effect (handle 0x%llX) was still applied to "
                                                     "the old actor -- that handle is dropped, never "
                                                     "removed on the new one",
                                                     static_cast<unsigned long long>(
                                                         sprintEffectHandle));
                                            sprintEffectHandle = 0;
                                        }
                                        sprintEffectActor = actor;
                                        if (input.written.sprint != 0) {

                                            {
                                                std::string abilityNote;
                                                if (dsh::reflect::character_call_sprint_ability(
                                                        actor, abilityNote)) {
                                                    DSH_INFO("puppetsession: %s", abilityNote.c_str());
                                                } else {
                                                    DSH_WARN("puppetsession: the puppet's sprint ability was "
                                                             "NOT asked for: %s", abilityNote.c_str());
                                                }
                                            }
                                            std::uint64_t handle = 0;
                                            std::string effectNote;
                                            if (dsh::reflect::character_apply_sprint_effect(
                                                    actor, handle, effectNote)) {
                                                sprintEffectHandle = handle;
                                                ++sprintEffectApplies;
                                                DSH_INFO("puppetsession: %s", effectNote.c_str());
                                            } else {
                                                ++sprintEffectRefusals;
                                                DSH_WARN("puppetsession: the game's sprint effect was NOT "
                                                         "applied to the puppet: %s",
                                                         effectNote.c_str());
                                            }
                                        } else if (sprintEffectHandle != 0) {
                                            std::string removeNote;
                                            if (dsh::reflect::character_remove_sprint_effect(
                                                    actor, sprintEffectHandle, removeNote)) {
                                                ++sprintEffectRemovals;
                                                sprintEffectHandle = 0;
                                                DSH_INFO("puppetsession: %s", removeNote.c_str());
                                            } else {
                                                ++sprintEffectRemovalFailures;
                                                DSH_WARN("puppetsession: the sprint effect could NOT be "
                                                         "removed: %s -- it will be retried on every "
                                                         "dispatch while the byte stays 0",
                                                         removeNote.c_str());
                                            }
                                        }
                                        sprintEdgeForAnimation = true;
                                    } else if (sprintEffectHandle != 0 && input.written.sprint == 0) {
                                        std::string removeNote;
                                        if (dsh::reflect::character_remove_sprint_effect(
                                                actor, sprintEffectHandle, removeNote)) {
                                            ++sprintEffectRemovals;
                                            sprintEffectHandle = 0;
                                            DSH_INFO("puppetsession: %s (retried after the first attempt "
                                                     "failed)", removeNote.c_str());
                                        } else if (++sprintEffectRetryWarnings <= 3) {
                                            DSH_WARN("puppetsession: the sprint effect is still applied "
                                                     "and removal still fails: %s", removeNote.c_str());
                                        }
                                    }
                                }

                                {
                                    GetterCapture capture;
                                    capture.bytes = input.written;
                                    if (dsh::reflect::character_call_derived_flags(actor, capture.flags,
                                                                                  capture.note)) {
                                        capture.sampled = true;
                                    }
                                    puppetDerivedGetters = capture;

                                    diagInitSampled = capture.sampled;
                                    diagInitValue = capture.flags.initialized;
                                    diagInitNote = capture.note;
                                    update->derivedSampled = capture.sampled;
                                    update->derivedNote = capture.note;

                                    if (sprintCalledThisDispatch && capture.sampled &&
                                        sprintLast.byteAfterKnown) {
                                        sprintLast.flags = capture.flags;
                                        sprintLast.flagsSampled = true;
                                        const bool asked = sprintLast.asked;
                                        if (capture.flags.sprint == asked) {
                                            ++sprintRawAgreed;
                                            ++sprintRawAgreedThisWindow;
                                        }
                                        if (capture.flags.sprinting == asked) {
                                            ++sprintDerivedAgreed;
                                            ++sprintDerivedAgreedThisWindow;
                                        }
                                    }

                                    update->sprintResult = sprintLast;
                                }

                                {
                                    const dsh::game::EnergyTimerApplyResult energy =
                                        dsh::game::puppet_energy_timers_apply(actor, update->stanceClass,
                                                                             update->energyTimers);
                                    update->appliedEnergyTimers = energy;
                                    if (energy.applied) {
                                        ++energyApplied;
                                        if (energy.changed) {
                                            ++energyChanged;

                                            clientAppliedHavePrevious = energyLastApplied.writtenRead;
                                            clientAppliedPreviousIntermission =
                                                energyLastApplied.written.intermissionTime;
                                            energyLastApplied = energy;
                                        }
                                    } else if (energy.changed) {

                                        ++energyFailed;
                                    }
                                    if (!energy.note.empty() && energyNoteCount < 3) {
                                        ++energyNoteCount;
                                        energyLastNote = energy.note;
                                        DSH_WARN("puppetsession: the two energy timers were NOT applied to "
                                                 "the puppet: %s (before: %.3f/%.3f; host sent: "
                                                 "%.3f/%.3f)",
                                                 energy.note.c_str(),
                                                 static_cast<double>(energy.before.intermissionTime),
                                                 static_cast<double>(energy.before.fastChargeStartTime),
                                                 static_cast<double>(update->energyTimers.intermissionTime),
                                                 static_cast<double>(
                                                     update->energyTimers.fastChargeStartTime));
                                    }
                                }

                                {
                                    const dsh::game::FrontWeaponApplyResult weapon =
                                        dsh::game::puppet_front_weapon_apply(actor, update->weaponClass,
                                                                            update->frontWeaponIndex);
                                    update->appliedFrontWeapon = weapon;
                                    if (weapon.beforeRead) weaponLastApplied = weapon;
                                    if (weapon.stated) ++weaponStated;
                                    if (weapon.called) ++weaponCalls;
                                    if (weapon.applied) {
                                        ++weaponApplied;
                                    } else if (weapon.holding) {
                                        ++weaponHolding;
                                    } else if (weapon.stated && weapon.requested != kFrontWeaponNone) {

                                        ++weaponFailed;
                                    }
                                    if (weapon.requested == kFrontWeaponNotRead) ++weaponNotRead;
                                    if (!weapon.note.empty() && weaponNoteCount < 3) {
                                        ++weaponNoteCount;
                                        weaponLastNote = weapon.note;
                                        DSH_WARN("puppetsession: the host's front weapon was NOT put into "
                                                 "the puppet's hand: %s (the puppet held 0x%llX at, "
                                                 "the wire carried %u)",
                                                 weapon.note.c_str(),
                                                 static_cast<unsigned long long>(weapon.before.current),
                                                 static_cast<unsigned>(weapon.requested));
                                    }
                                }
                            }
                        },
                        kPlacementTimeoutMs)) {
                    ++placementsApplied;
                    consecutivePlacementDrops = 0;
                    if (!placedOnce) {
                        placedOnce = true;

                        const std::string firstDerivedText =
                            update->derivedSampled ? ("derived: " + update->derivedNote)
                                                   : ("derived: NOT sampled");
                        constexpr float kRadToDeg = 180.0f / 3.14159265358979f;
                        const std::string appliedVelocityText =
                            update->appliedVelocityOk
                                ? ("(" + std::to_string(update->appliedVelocity[0]) + ", " +
                                   std::to_string(update->appliedVelocity[1]) + ", " +
                                   std::to_string(update->appliedVelocity[2]) + ")")
                                : std::string("NOT applied");
                        const std::string hostVelocityText =
                            "(" + std::to_string(wire.velocity[0]) + ", " +
                            std::to_string(wire.velocity[1]) + ", " +
                            std::to_string(wire.velocity[2]) + ")";

                        const dsh::game::MachineMovementState& posedProbe = doll_diag(source->peer).probe;
                        const char* const puppetModeName =
                            posedProbe.readable
                                ? dsh::game::movement_mode_name(posedProbe.movementMode)
                                : "?";
                        const unsigned puppetModeNumber =
                            posedProbe.readable
                                ? static_cast<unsigned>(posedProbe.movementMode)
                                : 0u;

                        std::string modeWriteText;
                        if (!update->appliedVelocityResult.modeReadBack &&
                            !update->appliedVelocityResult.modeRequested) {

                            modeWriteText = "hostModeWrite=NOT attempted (the apply did not reach the mode "
                                            "block -- see the warning above)";
                        } else if (!update->appliedVelocityResult.modeRequested) {
                            modeWriteText = "hostModeWrite=NOT requested (" +
                                            update->appliedVelocityResult.modeNote + ")";
                        } else {
                            modeWriteText = std::string("hostModeWrite=") +
                                            (update->appliedVelocityResult.modeApplied ? "APPLIED, reads back as "
                                                                                        : "NOT applied, reads back as ") +
                                            dsh::game::movement_mode_name(
                                                update->appliedVelocityResult.movementModeAfter) +
                                            "(" +
                                            std::to_string(static_cast<unsigned>(
                                                update->appliedVelocityResult.movementModeAfter)) +
                                            ")" +
                                            (update->appliedVelocityResult.modeNote.empty()
                                                 ? std::string()
                                                 : (" -- " + update->appliedVelocityResult.modeNote));
                        }
                        DSH_INFO("puppetsession: peer %u's doll was placed at (%.2f, %.2f, %.2f) m facing "
                                 "yaw %.1f deg, pitch %.1f deg with stance %u (%s) on the host's first "
                                 "position update; doll MovementMode=%s(%u) GravityScale=%.1f "
                                 "velocityApplied=%s hostVelocity=%s hostMovementMode=%s(%u) "
                                 "appliedAtMovementMode=%s(%u) %s; hostModeAsked=%s(%u) "
                                 "hostModeWriteNote=%s",
                                 static_cast<unsigned>(source->peer.value),
                                 posM[0], posM[1], posM[2], yawRad * kRadToDeg, pitchRad * kRadToDeg,
                                 static_cast<unsigned>(wire.stance.currentStance),
                                 dsh::game::stance_name(wire.stance.currentStance), puppetModeName,
                                 puppetModeNumber,
                                 posedProbe.readable
                                     ? static_cast<double>(posedProbe.gravityScale)
                                     : 0.0,
                                 appliedVelocityText.c_str(), hostVelocityText.c_str(),
                                 dsh::game::movement_mode_name(wire.movementMode),
                                 static_cast<unsigned>(wire.movementMode), puppetModeName,
                                 puppetModeNumber, firstDerivedText.c_str(),
                                 dsh::game::movement_mode_name(
                                     update->appliedVelocityResult.modeAsked),
                                 static_cast<unsigned>(update->appliedVelocityResult.modeAsked),
                                 modeWriteText.c_str());

                        const std::string appliedFlagsText =
                            update->appliedMovementFlagsOk
                                ? (dsh::game::movement_input_line(update->appliedMovementInput) +
                                   (update->appliedMovementFlagsChanged ? " [WROTE a new value]"
                                                                        : " [already held it]"))
                                : std::string("NOT applied (the flags did not change, or the write was "
                                              "refused -- see the warning above if there was one)");
                        const std::string appliedVectorText =
                            update->appliedMovementVectorOk
                                ? (update->appliedMovementVectorChanged ? "WROTE a new value"
                                                                        : "already held it")
                                : std::string("NOT applied");

                        const std::string firstSprintText =
                            sprint_call_line(update->sprintResult, sprintCalls);
                        DSH_INFO("puppetsession: the movement input on the host's first position update "
                                 "-- host sent %s; puppet flags %s; puppet recentInput %s; %s",
                                 dsh::game::movement_input_line(wire.movementInput).c_str(),
                                 appliedFlagsText.c_str(), appliedVectorText.c_str(),
                                 firstSprintText.c_str());
                    }
                } else {
                    ++placementsDropped;
                    ++consecutivePlacementDrops;

                    if (!warnedNoGameThread) {
                        warnedNoGameThread = true;
                        DSH_ERROR("puppetsession: %llu consecutive position update(s) were CANCELLED "
                                  "because the game thread did not take them within %d ms -- the puppet "
                                  "will not move while this lasts. That means the game thread is not "
                                  "calling ProcessEvent, which is most often a loading screen or the "
                                  "game shutting down; if 'gthook' was never run, the trampoline being "
                                  "absent is the other cause. The queue reports: %s",
                                  static_cast<unsigned long long>(consecutivePlacementDrops),
                                  kPlacementTimeoutMs,
                                  dsh::reflect::game_thread_report().c_str());
                    }
                }

                continue;
            }

            if (hdr.type != dsh::net::MessageType::playerStateBatch) continue;
            DSH_TIME_WALK("recv:manifest");
            ++manifestDatagrams;

            std::string note;
            const std::span<const std::uint8_t> whole(d.payload);

            if (asHost && !source->haveLoadout &&
                source->manifestDatagrams.size() < dsh::game::kAppearanceMaxChunks) {
                bool stored = false;
                for (const auto& kept : source->manifestDatagrams) {
                    if (kept.size() == d.payload.size() &&
                        std::equal(kept.begin(), kept.end(), d.payload.begin())) {
                        stored = true;
                        break;
                    }
                }
                if (!stored) {
                    source->manifestDatagrams.emplace_back(d.payload.begin(), d.payload.end());
                }
            }

            if (!source->assembler.add_chunk(whole.subspan(reader.cursor()), note)) {
                if (!note.empty()) DSH_WARN("puppetsession: %s", note.c_str());
                continue;
            }

            if (asHost) {
                const std::size_t accepted = source->assembler.chunksReceived();
                if (source->manifestDatagrams.size() == accepted) {
                    DSH_INFO("puppetsession: peer %u's manifest is complete and this host kept ALL %zu of "
                             "the %zu datagram(s) its assembler accepted -- a player who joins later will be "
                             "handed the whole manifest, not a prefix",
                             static_cast<unsigned>(source->peer.value),
                             source->manifestDatagrams.size(), accepted);
                } else {
                    DSH_WARN("puppetsession: peer %u's manifest is complete but this host kept only %zu of "
                             "the %zu datagram(s) its assembler accepted -- a player who joins later would "
                             "be handed a PREFIX and would never build this doll",
                             static_cast<unsigned>(source->peer.value),
                             source->manifestDatagrams.size(), accepted);
                }
            }

            if (!source->ackScheduled && source->assembler.complete()) {
                source->ackScheduled = true;
                source->ackPending = 3;
                nextManifestAckMs = now;
            }
            }
        }

        {
            dsh::game::ActionEdge edge;

            std::uint32_t drained = 0;
            while (drained < 16 && dsh::game::take_action_edge(edge)) {
                ++drained;

                if (!asHost && edge.action == dsh::game::kShotActionBase) continue;

                const std::uint8_t body[3] = {static_cast<std::uint8_t>(edge.action),
                                              static_cast<std::uint8_t>(edge.pressed ? 1 : 0),
                                              static_cast<std::uint8_t>(edge.module)};
                for (const auto& peer : session.peers()) {
                    if (peer.state != dsh::net::PeerState::connected) continue;
                    session.send_action_to(peer.id, actionSequence++,
                                           std::span<const std::uint8_t>(body, 3));
                }
                ++actionEdgesSent;
            }
        }

        {
            dsh::game::ItemUseSample use;
            std::uint32_t drainedUses = 0;
            while (drainedUses < 8 && dsh::game::take_item_use(use)) {
                ++drainedUses;
                const std::string classPath = dsh::reflect::object_path_name(use.itemClass);
                std::string encodeNote;
                const std::vector<std::uint8_t> payload = dsh::game::encode_item_use(classPath, encodeNote);
                if (payload.empty()) {
                    dsh::game::note_item_use_refused();
                    if (itemNoteCount++ < 8) {
                        DSH_WARN("puppetsession: a quick-slot use could NOT be encoded: %s", encodeNote.c_str());
                    }
                    continue;
                }
                for (const auto& peer : session.peers()) {
                    if (peer.state != dsh::net::PeerState::connected) continue;
                    session.send_item_use_to(peer.id, itemUseSequence++,
                                             std::span<const std::uint8_t>(payload));
                }
                dsh::game::note_item_use_sent();
                ++itemUsesSent;
                if (itemNoteCount++ < 8) {

                    const std::uint64_t sentAtMs = GetTickCount64();
                    DSH_INFO("puppetsession: a quick-slot use was SENT -- '%s' (%zu byte(s)), %llu ms after the "
                             "game reported the consumption", classPath.c_str(), payload.size(),
                             static_cast<unsigned long long>(sentAtMs >= use.ms ? sentAtMs - use.ms : 0));
                }
            }
        }

        {
            dsh::game::AmmoSpawnSample sample;
            std::uint32_t drained = 0;
            while (drained < 16 && dsh::game::take_ammo_spawn(sample)) {
                ++drained;
                const auto cachedPath = [&ammoPathCache](std::uintptr_t object) -> std::string {
                    if (object < 0x10000) return {};
                    const auto it = ammoPathCache.find(object);
                    if (it != ammoPathCache.end()) return it->second;
                    std::string path = dsh::reflect::object_path_name(object);
                    if (ammoPathCache.size() < 256) ammoPathCache.emplace(object, path);
                    return path;
                };
                dsh::reflect::AmmoSpawnRequest request;
                request.classPath = cachedPath(sample.actorClass);
                request.speedCurvePath = cachedPath(sample.speedCurve);
                request.pointDamageCurvePath = cachedPath(sample.pointDamageCurve);
                request.damageTypePath = cachedPath(sample.damageType);

                {
                    dsh::reflect::ActorPlacement placement;
                    std::string placementNote;
                    if (dsh::reflect::read_actor_placement(sample.actor, placement, placementNote)) {
                        std::memcpy(request.locationCm, placement.locationCm, sizeof(request.locationCm));
                        request.rotation[0] = placement.pitch;
                        request.rotation[1] = placement.yaw;
                        request.rotation[2] = placement.roll;
                    } else {
                        dsh::game::note_ammo_refused();
                        if (ammoNoteCount++ < 8) {
                            DSH_WARN("puppetsession: the captured projectile's PLACEMENT could not be read "
                                     "(%s), so nothing was sent for it -- a bullet at an invented position is "
                                     "worse than a bullet that did not appear",
                                     placementNote.c_str());
                        }
                        continue;
                    }
                }
                std::memcpy(request.params, sample.params, sizeof(request.params));

                {
                    dsh::reflect::AmmoHomingTarget target;
                    std::string targetNote;
                    if (!dsh::reflect::read_ammo_homing_target(sample.actor, target, targetNote)) {
                        dsh::game::note_ammo_refused();
                        if (ammoNoteCount++ < 8) {
                            DSH_WARN("puppetsession: the captured projectile's HOMING TARGET could not be read "
                                     "(%s), so nothing was sent for it", targetNote.c_str());
                        }
                        continue;
                    }
                    if (target.component == 0) {
                        request.targetKind = dsh::game::kAmmoTargetNone;
                    } else if (pawn.valid && pawn.pawn >= 0x10000 && target.actor == pawn.pawn) {
                        request.targetKind = dsh::game::kAmmoTargetOwnDoll;
                        ++ammoHomingCaptured;
                    } else {
                        std::uint64_t controller = 0;
                        std::string controllerClass;
                        if (dsh::reflect::read_process_bytes(target.actor + dsh::game::kPawnControllerOffset,
                                                             &controller, sizeof(controller)) &&
                            controller >= 0x10000) {
                            controllerClass = dsh::reflect::object_class_name(
                                static_cast<std::uintptr_t>(controller));
                        }
                        const std::string npcWhy =
                            dsh::reflect::npc_exclusion_reason(target.actorClass, controllerClass);
                        if (!npcWhy.empty()) {
                            request.targetKind = dsh::game::kAmmoTargetRefused;
                            ++ammoHomingRefusedNpc;
                            if (ammoHomingNpcLines++ < 4) {
                                DSH_INFO("ammo: the projectile's target is an NPC ('%s'), so its homing is NOT "
                                         "reproduced -- %s. An NPC is never named, never called and nothing "
                                         "branches on it",
                                         target.actorName.c_str(), npcWhy.c_str());
                            }
                        } else {
                            request.targetKind = dsh::game::kAmmoTargetEnemy;
                            request.targetName = target.actorName;
                            ++ammoHomingCaptured;
                        }
                    }

                    if (target.component != 0 && ammoHomingProbeLines < 4) {
                        ++ammoHomingProbeLines;
                        dsh::reflect::LockonTarget lockon;
                        std::string lockonNote;
                        const bool asked = dsh::reflect::read_lockon_target(target.actor, lockon, lockonNote);
                        DSH_INFO("ammo: homing -- this projectile chases '%s' [%s] via component '%s', socket "
                                 "'%s'; the target's own GetLockonTargetComponent says '%s' (%s), socket '%s'",
                                 target.actorName.c_str(), target.actorClass.c_str(),
                                 target.componentName.c_str(),
                                 target.socketName.empty() ? "<none>" : target.socketName.c_str(),
                                 asked ? (lockon.component == 0 ? "<null>" : lockon.componentName.c_str())
                                       : "<not asked>",
                                 asked ? (lockon.component == target.component ? "SAME as the missile's"
                                                                              : "DIFFERS from the missile's")
                                       : lockonNote.c_str(),
                                 lockon.socketName.empty() ? "<none>" : lockon.socketName.c_str());
                    }
                }
                std::string encodeNote;
                const std::vector<std::uint8_t> payload =
                    dsh::game::encode_ammo_spawn(request, encodeNote);
                if (payload.empty()) {
                    dsh::game::note_ammo_refused();
                    if (ammoNoteCount++ < 8) {
                        DSH_WARN("puppetsession: a captured projectile was NOT SENT: %s",
                                 encodeNote.c_str());
                    }
                    continue;
                }

                if (!firstAmmoLinePrinted) {
                    firstAmmoLinePrinted = true;
                    float speed = 0.0f, lifespan = 0.0f, operating = 0.0f, homing = 0.0f;
                    std::memcpy(&operating, sample.params + 0x00, sizeof(operating));
                    std::memcpy(&lifespan, sample.params + 0x04, sizeof(lifespan));
                    std::memcpy(&speed, sample.params + 0x08, sizeof(speed));
                    std::memcpy(&homing, sample.params + 0x9C, sizeof(homing));
                    DSH_INFO("ammo: the FIRST projectile of this session is class '%s' -- operating %.1f, "
                             "lifespan %.2f, speed %.1f, homing %.2f, payload %zu byte(s)",
                             request.classPath.c_str(), static_cast<double>(operating),
                             static_cast<double>(lifespan), static_cast<double>(speed),
                             static_cast<double>(homing), payload.size());
                }
                for (const auto& peer : session.peers()) {
                    if (peer.state != dsh::net::PeerState::connected) continue;
                    session.send_ammo_spawn_to(peer.id, ammoSequence++, payload);
                    ++ammoSent;
                }
            }
        }

        {
            const int published = g_ammoCreatedPublished.load(std::memory_order_acquire);
            for (int i = 0; i < published && i < kAmmoProbeSlots; ++i) {
                if (ammoProbeDone[i]) continue;
                const AmmoCreatedSlot& slot = g_ammoCreatedSlot[i];
                if (now < slot.ms + kAmmoProbeDelayMs) continue;
                ammoProbeDone[i] = true;
                const std::uint64_t ageMs = now - slot.ms;
                std::string goneWhy;
                if (!dsh::reflect::object_is_still_registered(slot.actor, goneWhy)) {
                    DSH_INFO("ammo: the copy 0x%llX is GONE %llu ms after it was created (%s) -- it did not "
                             "live long enough to be asked whether it moved",
                             static_cast<unsigned long long>(slot.actor),
                             static_cast<unsigned long long>(ageMs),
                             goneWhy.empty() ? "no reason given" : goneWhy.c_str());
                    continue;
                }
                dsh::reflect::ActorPlacement at;
                std::string atNote;
                if (!dsh::reflect::read_actor_placement(slot.actor, at, atNote)) {
                    DSH_WARN("ammo: the copy 0x%llX was still registered %llu ms after it was created, but its "
                             "position could NOT be read (%s) -- so 'it moved' and 'it did not' are both "
                             "unanswered for this one",
                             static_cast<unsigned long long>(slot.actor),
                             static_cast<unsigned long long>(ageMs), atNote.c_str());
                    continue;
                }

                const bool readAsOrigin =
                    at.locationCm[0] == 0.0f && at.locationCm[1] == 0.0f && at.locationCm[2] == 0.0f;
                const bool spawnedAtOrigin = slot.locationCm[0] == 0.0f && slot.locationCm[1] == 0.0f &&
                                             slot.locationCm[2] == 0.0f;
                if (readAsOrigin && !spawnedAtOrigin) {
                    DSH_WARN("ammo: the copy 0x%llX answered (0.0, 0.0, 0.0) %llu ms after it was created -- "
                             "that is a REFUSED read and not a place: the actor was collected between the "
                             "liveness check and the call, so whether it moved is NOT measured for this one",
                             static_cast<unsigned long long>(slot.actor),
                             static_cast<unsigned long long>(ageMs));
                    continue;
                }
                const float dx = at.locationCm[0] - slot.locationCm[0];
                const float dy = at.locationCm[1] - slot.locationCm[1];
                const float dz = at.locationCm[2] - slot.locationCm[2];
                const float movedCm = std::sqrt(dx * dx + dy * dy + dz * dz);

                const char* verdict =
                    movedCm >= 25.0f
                        ? "so the engine IS flying it"
                        : (movedCm <= 1.0f
                               ? "so it has NOT moved at all -- a copy at its spawn point is a copy that never "
                                 "started flying"
                               : "so it moved a little -- neither parked nor flying");
                DSH_INFO("ammo: the copy 0x%llX %s: %.1f cm in %llu ms, from (%.1f, %.1f, %.1f) to "
                         "(%.1f, %.1f, %.1f); its params now -- %s",
                         static_cast<unsigned long long>(slot.actor), verdict,
                         static_cast<double>(movedCm), static_cast<unsigned long long>(ageMs),
                         static_cast<double>(slot.locationCm[0]), static_cast<double>(slot.locationCm[1]),
                         static_cast<double>(slot.locationCm[2]), static_cast<double>(at.locationCm[0]),
                         static_cast<double>(at.locationCm[1]), static_cast<double>(at.locationCm[2]),
                         dsh::reflect::ammo_params_text(slot.actor).c_str());
            }
        }

        const std::uint64_t applyNowMs = steady_ms();
        if (applyNowMs >= nextInterpApplyMs) {
            DSH_TIME_WALK("loop:interp");
            nextInterpApplyMs = applyNowMs + kPoseApplyIntervalMs;
            for (auto& slot : dolls.slots()) {
                if (!slot || !slot->interpHave || slot->actors.empty()) continue;
                dsh::game::PuppetDoll& doll = *slot;

                const dsh::sync::PlaybackClock::Frame frame = doll.clock.frame(applyNowMs, kInterpDelayMs);
                const dsh::sync::PoseOutput out = doll.interp.sample(frame.renderMs, frame.samplerNowMs);
                if (!out.renderable()) continue;
                const std::vector<std::uintptr_t> actors = doll.actors;
                const float px = out.x, py = out.y, pz = out.z;
                const float yaw = out.yaw, pitch = out.pitch;
                if (dsh::reflect::run_on_game_thread(
                        [actors, px, py, pz, yaw, pitch, &aimPitchWrites]() {
                            const float posM[3] = {px, py, pz};
                            for (const std::uintptr_t actor : actors) {

                                dsh::reflect::puppet_apply_actor_pose(actor, posM, yaw, 0.0f);
                                if (puppet_write_aim_pitch(actor, pitch)) ++aimPitchWrites;
                            }

                            note_write_census(kWriteSitePose, static_cast<std::uint64_t>(::GetTickCount64()));
                        },
                        kPlacementTimeoutMs)) {
                    ++interpApplied;
                } else {
                    ++interpDropped;
                }
            }
        }

        {
            DSH_TIME_WALK("loop:tail");

        for (auto& slotOpt : dolls.slots()) {
            if (!slotOpt) continue;
            dsh::game::PuppetDoll* buildDoll = &*slotOpt;
            if (buildDoll->buildAttempted || !buildDoll->assembler.complete()) continue;
            dsh::reflect::MachineLoadout incoming;
            std::string decodeNote;
            if (!buildDoll->assembler.take(decodeNote, &incoming)) {
                DSH_WARN("puppetsession: the manifest from peer %u assembled but did not decode: %s",
                         static_cast<unsigned>(buildDoll->peer.value), decodeNote.c_str());
                buildDoll->buildAttempted = true;
                continue;
            }
            buildDoll->buildAttempted = true;

            buildDoll->loadout = incoming;
            buildDoll->haveLoadout = !incoming.classPath.empty() && !incoming.className.empty();
            const bool firstDoll = puppetActors.empty();
            if (firstDoll) {
                puppetLoadout = incoming;
                havePuppetLoadout = buildDoll->haveLoadout;
            }
            DSH_INFO("puppetsession: manifest received from peer %u -- its machine loadout '%s' with %d "
                     "of 9 slot(s) filled, FDACustomizeData %s, the armour shell's 576 colour bytes "
                     "%s",
                     static_cast<unsigned>(buildDoll->peer.value), incoming.className.c_str(),
                     incoming.slot_count(),
                     incoming.customizePresent ? "present" : "ABSENT",
                     incoming.moduleColoringPresent ? "present" : "ABSENT");
            if (!buildDoll->haveLoadout) {
                DSH_ERROR("puppetsession: the manifest carried no machine class, so there is "
                          "nothing to spawn -- the peer captured no loadout, and it said so in its "
                          "own log");
            } else {

                const MachineBuildStats stats = build_machine(incoming, buildDoll->actors);
                buildDoll->built = stats.spawned;
                if (firstDoll) puppetActors = buildDoll->actors;

                {
                    std::vector<std::uintptr_t> owners;
                    for (const auto& ownerSlot : dolls.slots()) {
                        if (ownerSlot && !ownerSlot->actors.empty()) {
                            owners.push_back(ownerSlot->actors.front());
                        }
                    }
                    dsh::game::set_puppet_owners(owners.data(), owners.size());
                }
                if (!stats.ranOnGameThread) {
                    DSH_ERROR("puppetsession: the machine build did NOT run -- the game thread was "
                              "not reached within 30000 ms. Engine work is deliberately not "
                              "attempted off that thread, because it corrupts engine state instead "
                              "of failing");
                } else {

                    log_report_lines("puppetsession: ", stats.report);
                    if (!stats.note.empty()) {
                        log_report_lines("puppetsession: build caveat: ", stats.note);
                    }

                    for (const std::string& line : module_arrival_report(stats)) {
                        DSH_INFO("puppetsession: %s", line.c_str());
                    }

                    puppetBuilt = puppetBuilt || stats.spawned;

                    buildAttempted = true;
                    modulesMerged = stats.modulesMerged;
                    if (stats.spawned) {

                        stanceClass = dsh::reflect::resolve_uclass_address(kStanceApiClass);
                        if (stanceClass == 0) {
                            DSH_ERROR("puppetsession: the '%s' class is not in the object array, so "
                                      "the puppet's class chain cannot be checked and the peer's "
                                      "stance will NOT be applied to it", kStanceApiClass);
                        }

                        {
                            const dsh::reflect::FunctionRef deathMulticast =
                                stanceClass >= 0x10000
                                    ? dsh::reflect::reflect_resolve_function(stanceClass,
                                                                             "MulticastOnDeath")
                                    : dsh::reflect::FunctionRef{};
                            const dsh::reflect::FunctionRef deathReceive =
                                stanceClass >= 0x10000
                                    ? dsh::reflect::reflect_resolve_function(stanceClass,
                                                                             "ReceiveOnDeath")
                                    : dsh::reflect::FunctionRef{};
                            dsh::game::set_death_functions(
                                deathMulticast.usable ? deathMulticast.function : 0,
                                deathReceive.usable ? deathReceive.function : 0);
                            if (deathMulticast.usable || deathReceive.usable) {
                                DSH_INFO("puppetsession: the death watch is ARMED -- '%s' declares "
                                         "MulticastOnDeath at 0x%llX and ReceiveOnDeath at 0x%llX; the "
                                         "ProcessEvent detour compares those two addresses on every call, so "
                                         "a doll's death is now an EVENT in this log and not an inference "
                                         "from fields this build writes (549-1: health, movement mode and "
                                         "velocity are all ours, and they said 'alive')",
                                         kStanceApiClass,
                                         static_cast<unsigned long long>(
                                             deathMulticast.usable ? deathMulticast.function : 0),
                                         static_cast<unsigned long long>(
                                             deathReceive.usable ? deathReceive.function : 0));
                            } else {
                                DSH_WARN("puppetsession: the death watch could NOT be armed -- neither "
                                         "MulticastOnDeath nor ReceiveOnDeath resolved on '%s' (%s / %s), so "
                                         "a doll's death will NOT appear in this log at all",
                                         kStanceApiClass, deathMulticast.note.c_str(),
                                         deathReceive.note.c_str());
                            }
                        }

                        puppetWeaponClass = dsh::reflect::resolve_uclass_address(kFrontWeaponApiClass);
                        if (puppetWeaponClass == 0) {
                            DSH_ERROR("puppetsession: the '%s' class is not in the object array, so "
                                      "the puppet's class chain cannot be checked and the peer's choice "
                                      "of front weapon will NOT reach it", kFrontWeaponApiClass);
                        }

                        movementTicksSinceSample = kMovementTicksPerSample;
                    }
                    if (!stats.spawned) {
                        DSH_ERROR("puppetsession: the machine was NOT spawned -- see the build's "
                                  "own line above for which step refused");
                    } else if (stats.modulesMerged == 0) {

                        DSH_ERROR("puppetsession: the machine spawned but the game filled NONE of "
                                  "its ten module pointers -- the loadout was accepted as data and "
                                  "IGNORED by the game. Nothing about the doll can be judged until "
                                  "this line says otherwise");
                    }
                }
            }
        }

        if (asHost) {
            std::uint8_t connectedPeers = 0;
            for (const auto& p : session.peers()) {
                if (p.state != dsh::net::PeerState::connected) continue;
                if (p.id.value >= dsh::game::PuppetDolls::kSlots) continue;
                connectedPeers = static_cast<std::uint8_t>(connectedPeers | (1u << p.id.value));
            }
            for (auto& replaySlot : dolls.slots()) {
                if (!replaySlot) continue;
                dsh::game::PuppetDoll& doll = *replaySlot;
                doll.manifestSentMask =
                    static_cast<std::uint8_t>(doll.manifestSentMask & connectedPeers);
                if (doll.manifestDatagrams.empty() || !doll.haveLoadout) continue;
                const std::uint8_t need = dsh::game::peers_needing_manifest(
                    connectedPeers, doll.manifestSentMask, doll.peer);
                if (need == 0) continue;
                const dsh::net::Peer* owner = session.find_peer(doll.peer);

                if (owner == nullptr || owner->state != dsh::net::PeerState::connected) continue;
                for (std::uint8_t targetIndex = 0; targetIndex < dsh::game::PuppetDolls::kSlots;
                     ++targetIndex) {
                    const std::uint8_t targetBit = static_cast<std::uint8_t>(1u << targetIndex);
                    if ((need & targetBit) == 0) continue;
                    const dsh::net::Peer* target = session.find_peer(dsh::PeerId{targetIndex});
                    if (target == nullptr) continue;
                    for (const auto& storedDatagram : doll.manifestDatagrams) {
                        session.relay_to_peer(owner->endpoint, target->id, storedDatagram, now);
                        ++manifestReplayed;
                    }
                    doll.manifestSentMask = static_cast<std::uint8_t>(doll.manifestSentMask | targetBit);
                    if (manifestReplayLines++ < 8) {
                        DSH_INFO("puppetsession: peer %u's stored manifest was sent to peer %u (%zu "
                                 "datagram(s)), once: the host's own copy -- which is what a player who "
                                 "joined after the owner stopped repeating it relies on, and also what "
                                 "repairs a relayed chunk the wire dropped",
                                 static_cast<unsigned>(doll.peer.value),
                                 static_cast<unsigned>(targetIndex), doll.manifestDatagrams.size());
                    }
                }
            }
        }

        const auto primitive_class_name = [](const std::string& name) {
            return name.find("SkeletalMeshComponent") != std::string::npos ||
                   name.find("StaticMeshComponent") != std::string::npos ||
                   name.find("NiagaraComponent") != std::string::npos ||
                   name.find("ParticleSystemComponent") != std::string::npos;
        };

        struct CensusSkip {
            std::size_t notScene = 0;
            std::size_t notPrimitive = 0;
            std::size_t unregistered = 0;
            std::size_t unreadable = 0;
            std::size_t alreadyHidden = 0;
        };
        CensusSkip censusSkip;
        const auto collect_primitives = [&](std::vector<HideTarget>& out) {
            out.clear();
            censusSkip = CensusSkip{};
            for (const auto& entry : moduleCensus.entry) {
                if (!entry.scene) {
                    ++censusSkip.notScene;
                    continue;
                }
                if (!primitive_class_name(entry.className)) {
                    ++censusSkip.notPrimitive;
                    continue;
                }
                std::string why;
                if (!dsh::reflect::object_is_still_registered(entry.component, why)) {
                    ++censusSkip.unregistered;
                    continue;
                }
                unsigned char hidden = 0;
                if (!dsh::reflect::read_process_bytes(entry.component + kSceneHiddenFlagsOffset, &hidden,
                                                      sizeof(hidden))) {
                    ++censusSkip.unreadable;
                    continue;
                }
                if ((hidden & kSceneHiddenInGameBit) != 0) {
                    ++censusSkip.alreadyHidden;
                    continue;
                }
                HideTarget target;
                target.component = entry.component;
                target.wasHidden = false;
                const bool effect = entry.className.find("Niagara") != std::string::npos ||
                                    entry.className.find("ParticleSystem") != std::string::npos;
                target.what = std::string(effect ? "0:" : "1:") + entry.ownerSlot + " / " +
                              (entry.ownName.empty() ? entry.className : entry.ownName) + " ('" +
                              dsh::reflect::object_own_name(entry.owner) + "')";
                out.push_back(std::move(target));
            }
            std::sort(out.begin(), out.end(),
                      [](const HideTarget& a, const HideTarget& b) { return a.what < b.what; });
        };

        frame_guard_sample(now);
        mark_watch_pump(now);
        write_pump(now);

        if (g_partsFixRequested.exchange(false)) {
            std::vector<HideTarget> all;
            collect_primitives(all);
            if (all.empty()) {
                DSH_INFO("puppetsession: fix: there is nothing to rebuild -- the census holds no visible primitive "
                         "for this side's dolls yet. Type `parts` once first. NOTHING was written");
            } else {
                DSH_INFO("puppetsession: fix: the census holds %zu entries; %zu primitive(s) are in the scene and "
                         "visible and will be written; skipped %zu not-a-scene-component, %zu not-a-mesh-or-"
                         "effect-class, %zu no-longer-registered, %zu unreadable-flags, %zu already-hidden",
                         moduleCensus.entry.size(), all.size(), censusSkip.notScene, censusSkip.notPrimitive,
                         censusSkip.unregistered, censusSkip.unreadable, censusSkip.alreadyHidden);
                for (const auto& target : all) fixTargets.push_back(target);
                fixUntilMs = now + 400;

                g_fixMidProbe.clear();
                g_fixMidProbe.reserve(all.size());
                for (const auto& target : all) g_fixMidProbe.push_back(target.component);
                g_fixMidMs = now + 150;
                const std::size_t count = fixTargets.size();
                const bool ran = dsh::reflect::run_on_game_thread([all]() {
                    std::size_t done = 0;
                    std::size_t refused = 0;
                    std::size_t verified = 0;
                    std::size_t index = 0;
                    for (const auto& target : all) {
                        ++index;
                        std::string note;
                        if (!dsh::reflect::set_hidden_in_game(target.component, true, note)) {
                            ++refused;
                            DSH_WARN("puppetsession: fix write[%zu/%zu] @0x%llX REFUSED (%s) -- %s", index,
                                     all.size(), static_cast<unsigned long long>(target.component),
                                     note.c_str(), target.what.c_str());
                            continue;
                        }
                        ++done;

                        note_write_target(kWriteSiteFixHide, target.component,
                                          static_cast<std::uint64_t>(::GetTickCount64()));
                        unsigned char flags = 0;
                        const bool on = dsh::reflect::read_process_bytes(
                                            target.component + kSceneHiddenFlagsOffset, &flags, sizeof(flags)) &&
                                        (flags & kSceneHiddenInGameBit) != 0;
                        if (on) ++verified;
                        DSH_INFO("puppetsession: fix write[%zu/%zu] @0x%llX %s hid=%s", index, all.size(),
                                 static_cast<unsigned long long>(target.component), target.what.c_str(),
                                 on ? "ON" : "DID-NOT-TAKE");
                    }
                    DSH_INFO("puppetsession: fix: %zu primitive(s) hidden for one frame (%zu refused) -- their "
                             "render states are destroyed and will be rebuilt from the CURRENT transforms; the "
                             "bit was read back ON for %zu of them", done, refused, verified);
                }, 400);
                if (!ran) {
                    fixTargets.clear();
                    fixUntilMs = 0;
                    DSH_WARN("puppetsession: fix: the game-thread queue did not run the hide; NOTHING was written");
                } else {
                    DSH_INFO("puppetsession: fix: rebuilding the render state of %zu primitive(s) -- the doll "
                             "vanishes for about 0.4 s and comes back with every stale transform dropped", count);
                }
            }
        }
        if (g_fixMidMs != 0 && now >= g_fixMidMs) {

            std::size_t gone = 0;
            std::size_t present = 0;
            std::string stillPresent;
            for (const auto component : g_fixMidProbe) {
                std::uint64_t proxy = 0;
                if (!dsh::reflect::read_process_bytes(component + kPrimitiveSceneProxyOffset, &proxy,
                                                      sizeof(proxy))) {
                    continue;
                }
                if (proxy == 0) {
                    ++gone;
                    continue;
                }
                ++present;
                if (stillPresent.size() < 620) {
                    char one[40] = {};
                    std::snprintf(one, sizeof(one), " 0x%llX", static_cast<unsigned long long>(component));
                    stillPresent += one;
                }
            }
            DSH_INFO("puppetsession: fix window: %zu of %zu primitive(s) had NO proxy 150 ms after the hide "
                     "(their render states WERE destroyed); %zu still had one and therefore kept their stale "
                     "transform --%s",
                     gone, g_fixMidProbe.size(), present,
                     stillPresent.empty() ? " none" : stillPresent.c_str());
            g_fixMidMs = 0;
            g_fixMidProbe.clear();
        }
        if (fixUntilMs != 0 && now >= fixUntilMs) {
            std::vector<HideTarget> restore;
            restore.swap(fixTargets);
            fixUntilMs = 0;
            const bool ran = dsh::reflect::run_on_game_thread([restore]() {
                std::size_t done = 0;
                std::size_t leftHidden = 0;
                for (const auto& target : restore) {
                    std::string note;
                    if (!dsh::reflect::set_hidden_in_game(target.component, target.wasHidden, note)) continue;
                    ++done;
                    note_write_target(kWriteSiteFixShow, target.component,
                                      static_cast<std::uint64_t>(::GetTickCount64()));
                    unsigned char flags = 0;
                    if (dsh::reflect::read_process_bytes(target.component + kSceneHiddenFlagsOffset, &flags,
                                                         sizeof(flags)) &&
                        (flags & kSceneHiddenInGameBit) != 0) {
                        ++leftHidden;
                    }
                }

                std::size_t restored = 0;
                for (const auto& target : restore) {
                    std::uint64_t proxy = 0;
                    if (!dsh::reflect::read_process_bytes(target.component + kPrimitiveSceneProxyOffset, &proxy,
                                                          sizeof(proxy))) {
                        continue;
                    }
                    if (proxy != 0) ++restored;
                }
                DSH_INFO("puppetsession: fix: %zu primitive(s) shown again; %zu of them still read hidden "
                         "afterwards; proxies present at this instant: %zu of %zu (the new ones are built at the "
                         "end of the frame this runs in, so a low number here is the order of events and not a "
                         "failure)",
                         done, leftHidden, restored, restore.size());
            }, 400);
            if (!ran) {
                DSH_WARN("puppetsession: fix: the game-thread queue did not run the restore -- %zu primitive(s) "
                         "may stay hidden", restore.size());
            }
        }
        if (g_partsHideRequested.exchange(false)) {
            if (hideCycle.empty() || hideStep >= hideCycle.size()) {
                collect_primitives(hideCycle);
                hideStep = 0;
                if (!hideCycle.empty()) {
                    DSH_INFO("puppetsession: hide: a new pass over %zu primitive(s), one per press, effects first. "
                             "Press `hide` again and again until the stuck piece vanishes, then STOP -- the last "
                             "line below names the component that draws it", hideCycle.size());
                }
            }
            if (hideCycle.empty()) {
                DSH_INFO("puppetsession: hide: nothing to blink -- the census holds no visible primitive for this "
                         "side's dolls yet. Type `parts` once first (that builds it). NOTHING was written");
            } else {
                const HideTarget target = hideCycle[hideStep];
                ++hideStep;
                if (hideHeld >= 0x10000) {
                    const std::uintptr_t previous = hideHeld;
                    hideHeld = 0;
                    (void)dsh::reflect::run_on_game_thread([previous]() {
                        std::string note;
                        (void)dsh::reflect::set_hidden_in_game(previous, false, note);
                        note_write_target(kWriteSiteHideRestore, previous,
                                          static_cast<std::uint64_t>(::GetTickCount64()));
                    }, 250);
                }
                hideHeld = target.component;
                hideUntilMs = now + 1500;
                const bool ran = dsh::reflect::run_on_game_thread([target]() {
                    std::string note;
                    if (!dsh::reflect::set_hidden_in_game(target.component, true, note)) {
                        DSH_WARN("puppetsession: hide: hiding 0x%llX was refused (%s)",
                                 static_cast<unsigned long long>(target.component), note.c_str());
                    } else {
                        note_write_target(kWriteSiteHide, target.component,
                                          static_cast<std::uint64_t>(::GetTickCount64()));
                    }
                }, 400);
                DSH_INFO("puppetsession: hide: %zu/%zu -- %s is HIDDEN NOW for 1.5 s%s", hideStep, hideCycle.size(),
                         target.what.c_str() + 2, ran ? "" : " (the game-thread queue did not run it)");
            }
        }
        if (hideHeld >= 0x10000 && hideUntilMs != 0 && now >= hideUntilMs) {
            const std::uintptr_t restoreOne = hideHeld;
            hideHeld = 0;
            hideUntilMs = 0;
            const bool ran = dsh::reflect::run_on_game_thread([restoreOne]() {
                std::string note;
                if (!dsh::reflect::set_hidden_in_game(restoreOne, false, note)) {
                    DSH_WARN("puppetsession: hide: RESTORING 0x%llX was refused (%s) -- it may stay hidden",
                             static_cast<unsigned long long>(restoreOne), note.c_str());
                } else {
                    note_write_target(kWriteSiteHideRestore, restoreOne,
                                      static_cast<std::uint64_t>(::GetTickCount64()));
                }
            }, 400);
            if (!ran) {
                DSH_WARN("puppetsession: hide: the game-thread queue did not run the restore of 0x%llX",
                         static_cast<unsigned long long>(restoreOne));
            }
        }
        const bool partsDump = g_partsDumpRequested.exchange(false);
        if (now >= nextModulePlacementMs || partsDump) {
            nextModulePlacementMs = now + 30000;
            const bool tableDue = (nextModuleTableMs == 0) || (now >= nextModuleTableMs);
            if (tableDue) nextModuleTableMs = now + 120000;
            ++moduleWatchScans;

            struct WatchedMachine {
                std::string    label;
                std::uintptr_t machine = 0;
            };
            WatchedMachine watched[4];
            std::size_t watchedCount = 0;
            std::string ownRefusal;
            const std::uintptr_t own = local_pawn_machine(localPawnMachine, ownRefusal);
            if (own >= 0x10000) {
                watched[watchedCount].label = "mine";
                watched[watchedCount].machine = own;
                ++watchedCount;
            }
            for (const auto& probeSlot : dolls.slots()) {
                if (watchedCount >= 4) break;
                if (!probeSlot || !probeSlot->built || probeSlot->actors.empty()) continue;
                if (probeSlot->actors.front() < 0x10000) continue;
                watched[watchedCount].label =
                    "peer " + std::to_string(static_cast<unsigned>(probeSlot->peer.value));
                watched[watchedCount].machine = probeSlot->actors.front();
                ++watchedCount;
            }

            std::vector<std::uintptr_t>  slotActors;

            std::vector<std::uintptr_t>  screenActors[4];
            std::vector<std::uintptr_t>  pointerScratch;
            std::vector<std::uintptr_t>  moduleActorsHere;
            dsh::reflect::ActorPlacement machinePlacements[4];
            bool                         machinePlacementValid[4] = {false, false, false, false};
            std::size_t                  censusChecked[4] = {0, 0, 0, 0};
            std::size_t                  censusBeyond[4] = {0, 0, 0, 0};

            const std::uintptr_t primitiveClass = dsh::reflect::resolve_uclass_address("PrimitiveComponent");

            {
                bool needCensus = false;
                for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                    bool known = false;
                    for (const auto knownMachine : moduleCensus.machine) {
                        if (knownMachine == watched[wi].machine) known = true;
                    }
                    if (!known) needCensus = true;
                }
                if (needCensus && watchedCount > 0) {
                    moduleCensus.entry.clear();
                    moduleCensus.machine.clear();
                    CensusContext context;
                    context.out = &moduleCensus;
                    for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                        context.machines.push_back(watched[wi].machine);
                        moduleCensus.machine.push_back(watched[wi].machine);
                        for (const auto& slotRef : kModuleSlots) {
                            std::uint64_t raw = 0;
                            if (!dsh::reflect::read_process_bytes(watched[wi].machine + slotRef.offset, &raw,
                                                                  sizeof(raw))) {
                                continue;
                            }
                            const auto moduleActor = static_cast<std::uintptr_t>(raw);
                            if (moduleActor < 0x10000) continue;
                            context.partners.push_back(moduleActor);
                            context.partnerDoll.push_back(wi);
                            context.partnerSlot.push_back(slotRef.name);
                        }
                    }
                    std::uint32_t visited = 0;
                    module_object_iterator(0, &census_on_object, &context, visited);

                    const std::uintptr_t sceneClass = dsh::reflect::resolve_uclass_address("SceneComponent");
                    std::size_t sceneCount = 0;
                    for (auto& entry : moduleCensus.entry) {
                        entry.scene = sceneClass >= 0x10000 &&
                                      dsh::game::actor_class_chain_reaches(
                                          entry.component, sceneClass, &dsh::reflect::read_process_bytes);
                        if (entry.scene) ++sceneCount;
                    }
                    ++moduleCensus.builds;
                    DSH_INFO("puppetsession: the component census: %u object(s) walked, %zu of them owned by "
                             "one of %zu doll(s) and their modules, %zu with a world position (the rest are "
                             "audio, movement, input and the like) -- built %llu time(s) this session, and it "
                             "is only rebuilt when a doll it has never seen appears",
                             static_cast<unsigned>(visited), moduleCensus.entry.size(), watchedCount,
                             sceneCount, static_cast<unsigned long long>(moduleCensus.builds));

                    for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                        std::string names;
                        std::size_t count = 0;
                        for (const auto& entry : moduleCensus.entry) {
                            if (entry.doll != wi) continue;
                            if (entry.className.find("SkeletalMesh") == std::string::npos &&
                                entry.className.find("StaticMesh") == std::string::npos) {
                                continue;
                            }
                            if (count >= 12) break;
                            if (names.size() > 420) break;
                            names += " '" + (entry.ownName.empty() ? entry.className : entry.ownName) + "'";
                            names += "[" + entry.ownerSlot + "]";
                            ++count;
                        }
                        DSH_INFO("puppetsession: the component census's mesh component(s)[%s]:%s",
                                 watched[wi].label.c_str(),
                                 names.empty() ? " (none found)" : names.c_str());
                    }
                }
            }

            for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                const std::uintptr_t machine = watched[wi].machine;
                const std::string&   label = watched[wi].label;

                dsh::reflect::ActorPlacement machinePlace;
                std::string placeNote;
                if (!dsh::reflect::read_actor_placement(machine, machinePlace, placeNote)) {
                    DSH_INFO("puppetsession: stuck part[%s]: the doll's own location could not be read (%s) -- "
                             "nothing is claimed about its parts", label.c_str(), placeNote.c_str());
                    continue;
                }
                machinePlacements[wi] = machinePlace;
                machinePlacementValid[wi] = true;
                std::uintptr_t machineRoot = 0;
                (void)dsh::reflect::read_process_bytes(machine + kActorRootOffset, &machineRoot,
                                                       sizeof(machineRoot));

                std::uintptr_t slotPtrs[kModuleSlotCount] = {};
                std::string    slotClasses[kModuleSlotCount];
                std::string    slotsFirst;
                std::string    slotsSecond;
                std::size_t    present = 0;
                std::size_t    emptySlots = 0;
                std::size_t    goneSlots = 0;
                std::size_t    unreadable = 0;
                moduleActorsHere.clear();

                for (std::size_t slotIndex = 0; slotIndex < kModuleSlotCount; ++slotIndex) {
                    const ModuleSlotRef& slotRef = kModuleSlots[slotIndex];
                    std::string& line = (slotIndex < kModuleSlotCount / 2) ? slotsFirst : slotsSecond;
                    std::uint64_t raw = 0;
                    const bool slotRead =
                        dsh::reflect::read_process_bytes(machine + slotRef.offset, &raw, sizeof(raw));
                    const auto module = static_cast<std::uintptr_t>(slotRead ? raw : 0);
                    if (module < 0x10000) {
                        ++emptySlots;
                        line += std::string(" | ") + slotRef.name + " EMPTY";
                        continue;
                    }
                    std::string goneWhy;
                    if (!dsh::reflect::object_is_still_registered(module, goneWhy)) {
                        ++goneSlots;
                        line += std::string(" | ") + slotRef.name + " GONE";
                        continue;
                    }
                    ++present;
                    slotPtrs[slotIndex] = module;
                    slotClasses[slotIndex] = dsh::reflect::object_class_name(module);
                    slotActors.push_back(module);
                    moduleActorsHere.push_back(module);
                    line += std::string(" | ") + slotRef.name + " '" + slotClasses[slotIndex] + "'";

                    std::uintptr_t moduleRoot = 0;
                    const bool rootRead =
                        dsh::reflect::read_process_bytes(module + kActorRootOffset, &moduleRoot,
                                                         sizeof(moduleRoot)) &&
                        moduleRoot >= 0x10000;
                    const AttachChain chain =
                        rootRead ? walk_attach_chain(moduleRoot, machineRoot) : AttachChain{};
                    if (rootRead && module != machine && chain.attachParent < 0x10000 && moduleAttachLines < 64) {
                        ++moduleAttachLines;
                        DSH_INFO("puppetsession: stuck part[%s]: the root of %s ('%s') 0x%llX has NO "
                                 "AttachParent at all (mobility %s) -- it is not attached to any component, "
                                 "whatever its position says", label.c_str(), slotRef.name,
                                 slotClasses[slotIndex].c_str(),
                                 static_cast<unsigned long long>(moduleRoot), mobility_word(chain.mobility));
                    }

                    dsh::reflect::ActorPlacement modulePlace;
                    std::string moduleNote;
                    if (dsh::reflect::read_actor_placement(module, modulePlace, moduleNote)) {
                        const double moduleDistance = distance_cm(modulePlace.locationCm, machinePlace.locationCm);
                        char number[40] = {};
                        std::snprintf(number, sizeof(number), " %.1f cm", moduleDistance);
                        line += number;
                        if (moduleDistance > kStuckPartThresholdCm && moduleAttachLines < 64) {
                            ++moduleAttachLines;
                            if (!rootRead) {
                                DSH_INFO("puppetsession: stuck part[%s]: the slot actor %s ('%s') is %.1f cm "
                                         "from the doll at (%.1f, %.1f, %.1f) -- its root component could not be "
                                         "read, so NO claim is made about what it is attached to",
                                         label.c_str(), slotRef.name, slotClasses[slotIndex].c_str(),
                                         moduleDistance, static_cast<double>(machinePlace.locationCm[0]),
                                         static_cast<double>(machinePlace.locationCm[1]),
                                         static_cast<double>(machinePlace.locationCm[2]));
                            } else {
                                DSH_INFO("puppetsession: stuck part[%s]: the slot actor %s ('%s') is %.1f cm "
                                         "from the doll at (%.1f, %.1f, %.1f) -- %d hop(s) up its root %s the "
                                         "doll's own root, its absolute transform bits are location=%d "
                                         "rotation=%d scale=%d, and its mobility is %s",
                                         label.c_str(), slotRef.name, slotClasses[slotIndex].c_str(),
                                         moduleDistance, static_cast<double>(machinePlace.locationCm[0]),
                                         static_cast<double>(machinePlace.locationCm[1]),
                                         static_cast<double>(machinePlace.locationCm[2]),
                                         chain.hops, chain.reachesMachine ? "reaches" : "does NOT reach",
                                         chain.absoluteLocation ? 1 : 0,
                                         chain.absoluteRotation ? 1 : 0,
                                         chain.absoluteScale ? 1 : 0,
                                         mobility_word(chain.mobility));
                            }
                        }
                    } else {
                        ++unreadable;
                        line += " NOT readable";
                    }
                    if (!rootRead) continue;

                    if (!read_pointer_array(moduleRoot + kSceneAttachChildrenOffset, 4, pointerScratch)) continue;
                    for (const auto child : pointerScratch) {
                        if (child < 0x10000) continue;
                        std::string childWhy;
                        if (!dsh::reflect::object_is_still_registered(child, childWhy)) continue;
                        const std::string childClass = dsh::reflect::object_class_name(child);
                        if (looks_like_a_capture_component(childClass)) continue;
                        if (looks_transient_actor(childClass)) continue;
                        dsh::reflect::ComponentPlacement childPlace;
                        std::string childNote;
                        if (!dsh::reflect::read_component_location(child, childPlace, childNote)) continue;
                        const double childDistance = distance_cm(childPlace.locationCm, machinePlace.locationCm);
                        if (childDistance > kStuckPartThresholdCm && moduleAttachLines < 64) {
                            ++moduleAttachLines;
                            DSH_INFO("puppetsession: stuck part[%s]: a '%s' COMPONENT under %s is %.1f cm from "
                                     "the doll -- a component that is no longer riding the module that owns "
                                     "it", label.c_str(), childClass.c_str(), slotRef.name, childDistance);
                        }
                    }
                }

                screenActors[wi].clear();
                screenActors[wi].push_back(machine);
                {
                    std::uint64_t humanoidRaw = 0;
                    if (dsh::reflect::read_process_bytes(machine + kMachineHumanoidOffset, &humanoidRaw,
                                                         sizeof(humanoidRaw)) &&
                        humanoidRaw >= 0x10000) {
                        screenActors[wi].push_back(static_cast<std::uintptr_t>(humanoidRaw));
                    }
                    for (const auto slot : slotPtrs) {
                        if (slot >= 0x10000) screenActors[wi].push_back(slot);
                    }
                }

                {
                    std::size_t checked = 0;
                    std::size_t beyond = 0;
                    for (auto& entry : moduleCensus.entry) {
                        if (entry.doll != wi || !entry.scene) continue;
                        if (looks_like_a_capture_component(entry.className)) continue;
                        if (looks_transient_actor(entry.className)) continue;
                        std::string componentWhy;
                        if (!dsh::reflect::object_is_still_registered(entry.component, componentWhy)) continue;
                        dsh::reflect::ComponentPlacement componentPlace;
                        std::string componentNote;
                        if (!dsh::reflect::read_component_location(entry.component, componentPlace,
                                                                   componentNote)) {
                            continue;
                        }
                        ++checked;
                        const double componentDistance =
                            distance_cm(componentPlace.locationCm, machinePlace.locationCm);
                        const double machineMoved =
                            entry.haveLast ? distance_cm(machinePlace.locationCm, entry.lastMachineCm) : -1.0;
                        const double drift = entry.lastDistanceCm >= 0.0
                                                 ? std::fabs(componentDistance - entry.lastDistanceCm)
                                                 : -1.0;
                        if (entry.haveLast && machineMoved > kStuckPartMovedCm && drift > kStuckPartDriftCm) {
                            ++beyond;
                            if (moduleCensusLines < 96) {
                                ++moduleCensusLines;
                                DSH_INFO("puppetsession: stuck part[%s]: the COMPONENT '%s %s' owned by %s "
                                         "stayed put while the doll moved -- it was %.1f cm away last tick and "
                                         "is %.1f cm away now (drift %.1f cm) after the doll itself moved "
                                         "%.1f cm; it is at (%.1f, %.1f, %.1f) cm",
                                         label.c_str(), entry.className.c_str(),
                                         entry.ownName.empty() ? "<unnamed>" : entry.ownName.c_str(),
                                         entry.ownerSlot.c_str(), entry.lastDistanceCm, componentDistance,
                                         drift, machineMoved,
                                         static_cast<double>(componentPlace.locationCm[0]),
                                         static_cast<double>(componentPlace.locationCm[1]),
                                         static_cast<double>(componentPlace.locationCm[2]));
                            }
                        }
                        entry.lastDistanceCm = componentDistance;
                        for (int axis = 0; axis < 3; ++axis) {
                            entry.lastMachineCm[axis] = machinePlace.locationCm[axis];
                        }
                        entry.haveLast = true;
                    }
                    censusChecked[wi] = checked;
                    censusBeyond[wi] = beyond;
                }

                std::string humanoidText = check_body_meshes(label, machine, machinePlace.locationCm,
                                                              primitiveClass, modulePartsLines, true);

                if (partsDump) {
                    DSH_INFO("puppetsession: ===== PARTS DUMP ON REQUEST -- %s =====", label.c_str());
                    DSH_INFO("puppetsession: parts[%s] machine 0x%llX '%s' at (%.1f, %.1f, %.1f) cm", label.c_str(),
                             static_cast<unsigned long long>(machine), dsh::reflect::object_class_name(machine).c_str(),
                             static_cast<double>(machinePlace.locationCm[0]),
                             static_cast<double>(machinePlace.locationCm[1]),
                             static_cast<double>(machinePlace.locationCm[2]));
                    for (std::size_t slotIndex = 0; slotIndex < kModuleSlotCount; ++slotIndex) {
                        const ModuleSlotRef& slotRef = kModuleSlots[slotIndex];
                        std::uint64_t raw = 0;
                        if (!dsh::reflect::read_process_bytes(machine + slotRef.offset, &raw, sizeof(raw)) ||
                            raw < 0x10000) {
                            DSH_INFO("puppetsession: parts[%s] %s -- EMPTY", label.c_str(), slotRef.name);
                            continue;
                        }
                        const auto slotActor = static_cast<std::uintptr_t>(raw);
                        std::uintptr_t slotRoot = 0;
                        (void)dsh::reflect::read_process_bytes(slotActor + kActorRootOffset, &slotRoot,
                                                               sizeof(slotRoot));
                        const ComponentState slotState = component_state(slotRoot, primitiveClass);
                        const double slotDistance =
                            slotState.locationRead ? distance_cm(slotState.locationCm, machinePlace.locationCm)
                                                   : -1.0;
                        DSH_INFO("puppetsession: parts[%s] %s actor 0x%llX '%s'; root %s", label.c_str(),
                                 slotRef.name, static_cast<unsigned long long>(slotActor),
                                 dsh::reflect::object_class_name(slotActor).c_str(),
                                 component_words(slotState, slotDistance).c_str());
                    }
                    DSH_INFO("puppetsession: parts[%s] the body's own meshes:%s", label.c_str(),
                             humanoidText.c_str());
                    for (const auto& entry : moduleCensus.entry) {
                        if (entry.doll != wi) continue;
                        const ComponentState state = component_state(entry.component, primitiveClass);

                        if (entry.scene) {
                            probe_render_transform(label, entry.ownName.empty() ? entry.className.c_str()
                                                                               : entry.ownName.c_str(),
                                                   entry.component, state, machine, true);
                            std::string chain;
                            std::uintptr_t walk = entry.component;
                            for (int hop = 0; hop < 8 && walk >= 0x10000; ++hop) {
                                std::uint64_t parent = 0;
                                if (!dsh::reflect::read_process_bytes(walk + kSceneAttachParentOffset, &parent,
                                                                     sizeof(parent)) ||
                                    parent < 0x10000) {
                                    break;
                                }
                                walk = static_cast<std::uintptr_t>(parent);
                                const std::string parentName = dsh::reflect::object_own_name(walk);
                                const std::string parentClass = dsh::reflect::object_class_name(walk);
                                chain += " <- '" + (parentName.empty() ? parentClass : parentName) + "' [" +
                                         parentClass + "]";
                            }
                            if (!chain.empty()) {
                                DSH_INFO("puppetsession: parts[%s] chain: '%s'%s", label.c_str(),
                                         entry.ownName.empty() ? entry.className.c_str() : entry.ownName.c_str(),
                                         chain.c_str());
                            }
                        }
                        const double distance =
                            state.locationRead ? distance_cm(state.locationCm, machinePlace.locationCm) : -1.0;
                        DSH_INFO("puppetsession: parts[%s] owned by %s: %s", label.c_str(),
                                 entry.ownerSlot.c_str(), component_words(state, distance).c_str());
                    }

                    {
                        std::size_t listed = 0;
                        std::size_t candidates = 0;
                        const std::uintptr_t worldNow = dsh::reflect::live_world_address();
                        if (worldNow >= 0x10000) {
                            std::vector<std::uintptr_t> levelsHere;
                            read_pointer_array(worldNow + dsh::reflect::world_offsets::kLevels, 32, levelsHere);
                            for (const auto levelPtr : levelsHere) {
                                if (levelPtr < 0x10000) continue;
                                std::vector<std::uintptr_t> actorsHere;
                                read_pointer_array(levelPtr + dsh::reflect::level_offsets::kActors, 60000,
                                                   actorsHere);
                                for (const auto candidate : actorsHere) {
                                    if (candidate < 0x10000) continue;
                                    if (std::find(slotActors.begin(), slotActors.end(), candidate) !=
                                        slotActors.end()) {
                                        continue;
                                    }
                                    std::string registeredWhy;
                                    if (!dsh::reflect::object_is_still_registered(candidate, registeredWhy)) continue;
                                    const std::string candidateClass =
                                        dsh::reflect::object_class_name(candidate);
                                    if (!looks_like_a_part_class(candidateClass)) continue;
                                    dsh::reflect::ActorPlacement candidatePlace;
                                    std::string candidateNote;
                                    if (!dsh::reflect::read_actor_placement(candidate, candidatePlace,
                                                                           candidateNote)) {
                                        continue;
                                    }
                                    const double candidateDistance =
                                        distance_cm(candidatePlace.locationCm, machinePlace.locationCm);
                                    if (candidateDistance > 3000.0) continue;
                                    std::uint64_t candidateOwnerRaw = 0;
                                    (void)dsh::reflect::read_process_bytes(candidate + kActorOwnerOffset,
                                                                           &candidateOwnerRaw,
                                                                           sizeof(candidateOwnerRaw));
                                    bool candidateOwnedByADoll = false;
                                    for (std::size_t ow = 0; ow < watchedCount; ++ow) {
                                        if (watched[ow].machine ==
                                            static_cast<std::uintptr_t>(candidateOwnerRaw)) {
                                            candidateOwnedByADoll = true;
                                        }
                                    }
                                    if (candidateOwnedByADoll) continue;
                                    ++candidates;
                                    if (listed >= 20) continue;
                                    ++listed;
                                    std::uint64_t ownerRaw = 0;
                                    std::string ownerText = "NONE";
                                    if (dsh::reflect::read_process_bytes(candidate + kActorOwnerOffset, &ownerRaw,
                                                                         sizeof(ownerRaw)) &&
                                        ownerRaw >= 0x10000) {
                                        ownerText = "'" +
                                                    dsh::reflect::object_class_name(
                                                        static_cast<std::uintptr_t>(ownerRaw)) +
                                                    "'";
                                    }
                                    DSH_INFO("puppetsession: parts[%s] NOT IN A SLOT and %.1f cm away: '%s' [%s] "
                                             "at (%.1f, %.1f, %.1f) cm, owner %s",
                                             label.c_str(), candidateDistance,
                                             dsh::reflect::object_own_name(candidate).c_str(),
                                             candidateClass.c_str(),
                                             static_cast<double>(candidatePlace.locationCm[0]),
                                             static_cast<double>(candidatePlace.locationCm[1]),
                                             static_cast<double>(candidatePlace.locationCm[2]),
                                             ownerText.c_str());
                                }
                            }
                        }
                        DSH_INFO("puppetsession: parts[%s] doll-part actors within 30 m of this doll that are in "
                                 "NO slot: %zu (of which %zu named above)",
                                 label.c_str(), candidates, listed);
                    }

                    {
                        std::vector<std::uintptr_t> levelsHere;
                        const std::uintptr_t worldNow = dsh::reflect::live_world_address();
                        if (worldNow >= 0x10000) {
                            read_pointer_array(worldNow + dsh::reflect::world_offsets::kLevels, 32, levelsHere);
                        }
                        struct PartActor {
                            std::uintptr_t actor = 0;
                            std::uintptr_t owner = 0;
                            float          locationCm[3] = {0.0f, 0.0f, 0.0f};
                            double         distanceCm = 0.0;
                            std::string    className;
                            std::string    ownerClass;
                            std::string    ownName;
                        };
                        std::vector<PartActor> found;
                        std::size_t seen = 0;
                        for (const auto levelPtr : levelsHere) {
                            if (levelPtr < 0x10000) continue;
                            std::vector<std::uintptr_t> actorsHere;
                            read_pointer_array(levelPtr + dsh::reflect::level_offsets::kActors, 60000, actorsHere);
                            for (const auto actor : actorsHere) {
                                if (actor < 0x10000) continue;
                                if (actor == machine) continue;
                                if (std::find(slotActors.begin(), slotActors.end(), actor) != slotActors.end()) {
                                    continue;
                                }
                                std::string registeredWhy;
                                if (!dsh::reflect::object_is_still_registered(actor, registeredWhy)) continue;
                                const std::string actorClass = dsh::reflect::object_class_name(actor);
                                if (!looks_like_a_part_class(actorClass)) continue;
                                ++seen;
                                dsh::reflect::ActorPlacement partPlace;
                                std::string partNote;
                                if (!dsh::reflect::read_actor_placement(actor, partPlace, partNote)) continue;
                                PartActor part;
                                part.actor = actor;
                                part.className = actorClass;
                                part.ownName = dsh::reflect::object_own_name(actor);
                                for (int axis = 0; axis < 3; ++axis) part.locationCm[axis] = partPlace.locationCm[axis];
                                part.distanceCm = distance_cm(partPlace.locationCm, machinePlace.locationCm);
                                std::uint64_t ownerRaw = 0;
                                if (dsh::reflect::read_process_bytes(actor + kActorOwnerOffset, &ownerRaw,
                                                                      sizeof(ownerRaw)) &&
                                    ownerRaw >= 0x10000) {
                                    part.owner = static_cast<std::uintptr_t>(ownerRaw);
                                    part.ownerClass = dsh::reflect::object_class_name(part.owner);
                                }
                                found.push_back(std::move(part));
                            }
                        }
                        std::sort(found.begin(), found.end(), [](const PartActor& a, const PartActor& b) {
                            return a.distanceCm < b.distanceCm;
                        });
                        std::size_t printedOwnerless = 0;
                        std::size_t ownerless = 0;
                        for (const auto& part : found) {
                            if (part.owner >= 0x10000) continue;
                            ++ownerless;
                            if (moduleCensusLines >= 200) continue;
                            ++moduleCensusLines;
                            DSH_INFO("puppetsession: parts[%s] PART ACTOR WITH NO OWNER, %.1f cm from this doll: "
                                     "'%s' [%s] at (%.1f, %.1f, %.1f) cm -- nothing owns it, so nothing moves it",
                                     label.c_str(), part.distanceCm,
                                     part.ownName.empty() ? "<unnamed>" : part.ownName.c_str(),
                                     part.className.c_str(), static_cast<double>(part.locationCm[0]),
                                     static_cast<double>(part.locationCm[1]),
                                     static_cast<double>(part.locationCm[2]));
                        }
                        std::size_t printedNearest = 0;
                        for (const auto& part : found) {
                            if (printedNearest >= 40 || moduleCensusLines >= 200) break;
                            ++printedNearest;
                            ++moduleCensusLines;
                            DSH_INFO("puppetsession: parts[%s] part actor #%zu: '%s' [%s] at (%.1f, %.1f, %.1f) "
                                     "cm, %.1f cm from this doll, owner %s",
                                     label.c_str(), printedNearest,
                                     part.ownName.empty() ? "<unnamed>" : part.ownName.c_str(),
                                     part.className.c_str(), static_cast<double>(part.locationCm[0]),
                                     static_cast<double>(part.locationCm[1]),
                                     static_cast<double>(part.locationCm[2]), part.distanceCm,
                                     part.ownerClass.empty() ? "NONE" : part.ownerClass.c_str());
                        }
                        DSH_INFO("puppetsession: parts[%s] EVERY doll-part actor in the live level: %zu found (%zu "
                                 "with NO owner, %zu named above), %zu of them nearest-first printed (cap 40) -- "
                                 "this list has no distance limit on purpose",
                                 label.c_str(), found.size(), ownerless, printedOwnerless, printedNearest);
                    }

                    {
                        std::vector<std::uintptr_t> assetsHere;
                        for (const auto& entry : moduleCensus.entry) {
                            if (entry.doll != wi) continue;
                            if (entry.className.find("SkeletalMeshComponent") == std::string::npos) continue;
                            std::uint64_t asset = 0;
                            if (dsh::reflect::read_process_bytes(entry.component + 0x0480, &asset, sizeof(asset)) &&
                                asset >= 0x10000) {
                                assetsHere.push_back(static_cast<std::uintptr_t>(asset));
                            }
                        }
                        MeshCopyContext context;
                        context.actorClass = dsh::reflect::resolve_uclass_address("Actor");
                        context.assets = assetsHere;
                        for (const auto& entry : moduleCensus.entry) context.known.push_back(entry.component);
                        for (std::size_t dw = 0; dw < watchedCount; ++dw) {
                            for (const auto actor : screenActors[dw]) {
                                context.dollActor.push_back(actor);
                                context.dollActorDoll.push_back(dw);
                            }
                            if (machinePlacementValid[dw]) {
                                for (int axis = 0; axis < 3; ++axis) {
                                    context.dollCm.push_back(machinePlacements[dw].locationCm[axis]);
                                }
                                context.dollLabel.push_back(watched[dw].label);
                            }
                        }
                        context.lines = &moduleCensusLines;
                        context.label = label;
                        context.thisDoll = wi;
                        std::uint32_t visited = 0;
                        module_object_iterator(0, &mesh_copy_on_object, &context, visited);
                        DSH_INFO("puppetsession: MESH COPY[%s]: %zu mesh asset(s) of this doll's own parts, %zu "
                                 "component(s) in the process render one of them and are not components the "
                                 "census can name (%zu of those sit on a doll's own actor, %zu were class "
                                 "default objects and are not in the world at all)",
                                 label.c_str(), assetsHere.size(), context.found, context.unseenOnADoll,
                                 context.skippedDefaultObjects);
                    }

                    DSH_INFO("puppetsession: ===== END OF PARTS DUMP -- %s =====", label.c_str());
                }

                std::string attachedText;
                auto scan_actor_children = [&](std::uintptr_t parentActor, const char* ownerWord,
                                               std::size_t cap) {
                    if (!read_pointer_array(parentActor + kActorChildrenOffset, cap, pointerScratch)) return;
                    for (const auto child : pointerScratch) {
                        if (child < 0x10000) continue;
                        if (std::find(slotActors.begin(), slotActors.end(), child) != slotActors.end()) {
                            continue;
                        }
                        std::string childWhy;
                        if (!dsh::reflect::object_is_still_registered(child, childWhy)) continue;
                        const std::string childClass = dsh::reflect::object_class_name(child);
                        if (looks_like_a_capture_component(childClass)) continue;
                        if (looks_transient_actor(childClass)) {
                            ++moduleTransientSkipped;
                            continue;
                        }
                        dsh::reflect::ActorPlacement childPlace;
                        std::string childNote;
                        if (!dsh::reflect::read_actor_placement(child, childPlace, childNote)) continue;
                        const double childDistance = distance_cm(childPlace.locationCm, machinePlace.locationCm);
                        if (attachedText.size() < 200) {
                            char one[192] = {};
                            std::snprintf(one, sizeof(one), " '%s' %.1f cm", childClass.c_str(), childDistance);
                            attachedText += one;
                        }
                        if (childDistance > kStuckPartThresholdCm && moduleAttachLines < 64) {
                            ++moduleAttachLines;
                            DSH_INFO("puppetsession: stuck part[%s]: the ACTOR '%s' that the engine lists as "
                                     "attached to %s is %.1f cm from the doll at (%.1f, %.1f, %.1f) -- an "
                                     "actor still attached on paper and no longer on the doll",
                                     label.c_str(), childClass.c_str(), ownerWord, childDistance,
                                     static_cast<double>(machinePlace.locationCm[0]),
                                     static_cast<double>(machinePlace.locationCm[1]),
                                     static_cast<double>(machinePlace.locationCm[2]));
                        }
                    }
                };
                scan_actor_children(machine, "the doll itself", 32);
                for (const auto moduleActor : moduleActorsHere) {
                    scan_actor_children(moduleActor, "one of its modules", 24);
                }

                ModuleSlotMemory* memory = nullptr;
                bool              freshMemory = false;
                for (auto& entry : moduleSlotMemory) {
                    if (entry.valid && entry.machine == machine) {
                        memory = &entry;
                        break;
                    }
                }
                if (memory == nullptr) {
                    for (auto& entry : moduleSlotMemory) {
                        if (entry.valid) continue;
                        entry.valid = true;
                        entry.machine = machine;
                        memory = &entry;
                        freshMemory = true;
                        break;
                    }
                }

                if (memory == nullptr) {
                    for (auto& entry : moduleSlotMemory) {
                        bool stillWatched = false;
                        for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                            if (watched[wi].machine == entry.machine) stillWatched = true;
                        }
                        if (stillWatched) continue;
                        entry.machine = machine;
                        memory = &entry;
                        freshMemory = true;
                        break;
                    }
                }
                if (memory != nullptr && !freshMemory) {
                    for (std::size_t slotIndex = 0; slotIndex < kModuleSlotCount; ++slotIndex) {
                        if (memory->slot[slotIndex] == slotPtrs[slotIndex]) continue;
                        const std::string before =
                            memory->slot[slotIndex] < 0x10000
                                ? std::string("EMPTY")
                                : ("'" + memory->slotClass[slotIndex] + "' 0x" +
                                   [&] {
                                       char hex[24] = {};
                                       std::snprintf(hex, sizeof(hex), "%llX",
                                                     static_cast<unsigned long long>(memory->slot[slotIndex]));
                                       return std::string(hex);
                                   }());
                        const std::string after =
                            slotPtrs[slotIndex] < 0x10000
                                ? std::string("EMPTY")
                                : ("'" + slotClasses[slotIndex] + "' 0x" +
                                   [&] {
                                       char hex[24] = {};
                                       std::snprintf(hex, sizeof(hex), "%llX",
                                                     static_cast<unsigned long long>(slotPtrs[slotIndex]));
                                       return std::string(hex);
                                   }());
                        if (moduleSlotLines < 64) {
                            ++moduleSlotLines;
                            DSH_INFO("puppetsession: module slot[%s] %s CHANGED: %s -> %s (the doll is at "
                                     "(%.1f, %.1f, %.1f) cm) -- a slot actor that moved, appeared or went away",
                                     label.c_str(), kModuleSlots[slotIndex].name, before.c_str(), after.c_str(),
                                     static_cast<double>(machinePlace.locationCm[0]),
                                     static_cast<double>(machinePlace.locationCm[1]),
                                     static_cast<double>(machinePlace.locationCm[2]));
                        }
                        memory->slot[slotIndex] = slotPtrs[slotIndex];
                        memory->slotClass[slotIndex] = slotClasses[slotIndex];
                    }
                }
                if (memory != nullptr && freshMemory) {
                    for (std::size_t slotIndex = 0; slotIndex < kModuleSlotCount; ++slotIndex) {
                        memory->slot[slotIndex] = slotPtrs[slotIndex];
                        memory->slotClass[slotIndex] = slotClasses[slotIndex];
                    }
                }

                if (tableDue) {

                    if (slotsFirst.rfind(" | ", 0) == 0) slotsFirst.erase(0, 3);
                    if (slotsSecond.rfind(" | ", 0) == 0) slotsSecond.erase(0, 3);
                    DSH_INFO("puppetsession: module slots[%s] at (%.1f, %.1f, %.1f) cm -- %zu present, %zu "
                             "EMPTY, %zu GONE, %zu unreadable:%s",
                             label.c_str(), static_cast<double>(machinePlace.locationCm[0]),
                             static_cast<double>(machinePlace.locationCm[1]),
                             static_cast<double>(machinePlace.locationCm[2]), present, emptySlots, goneSlots,
                             unreadable, slotsFirst.c_str());
                    DSH_INFO("puppetsession: module slots[%s] (2 of 2):%s | actors the engine attaches to the "
                             "doll or to one of its modules, beyond the twelve slots:%s",
                             label.c_str(), slotsSecond.c_str(),
                             attachedText.empty() ? " (none)" : attachedText.c_str());
                    std::size_t censusEntries = 0;
                    std::size_t censusScene = 0;
                    for (const auto& entry : moduleCensus.entry) {
                        if (entry.doll != wi) continue;
                        ++censusEntries;
                        if (entry.scene) ++censusScene;
                    }
                    DSH_INFO("puppetsession: component census[%s]: %zu object(s) owned by this doll and its "
                             "modules, %zu of them with a world position, %zu checked this tick, %zu stayed "
                             "put while the doll moved",
                             label.c_str(), censusEntries, censusScene, censusChecked[wi], censusBeyond[wi]);
                    DSH_INFO("puppetsession: the body's own meshes[%s]:%s", label.c_str(), humanoidText.c_str());
                }
            }

            if (partsDump) {
                ScreenContext screen;
                screen.dollActorClass = dsh::reflect::resolve_uclass_address("Actor");
                bool anyDoll = false;
                for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                    const std::size_t compact = screen.dollCm.size() / 3;
                    const bool        placed = machinePlacementValid[wi];
                    for (int axis = 0; axis < 3; ++axis) {
                        screen.dollCm.push_back(placed ? machinePlacements[wi].locationCm[axis] : 0.0f);
                    }
                    screen.dollLabel.push_back(watched[wi].label);
                    if (placed) anyDoll = true;
                    for (const auto actor : screenActors[wi]) {
                        screen.dollActor.push_back(actor);
                        screen.dollActorDoll.push_back(compact);
                    }
                    for (const auto& entry : moduleCensus.entry) {
                        if (entry.doll == wi) screen.known.push_back(entry.component);
                    }
                }
                screen.playerController = dsh::reflect::find_live_objects().playerController;
                if (!anyDoll) {
                    DSH_INFO("puppetsession: parts screen: NOT ONE doll's location could be read, so no point can "
                             "be projected -- this is a refusal and not an empty picture");
                } else if (screen.playerController < 0x10000) {
                    DSH_INFO("puppetsession: parts screen: there is no live PlayerController, so the engine "
                             "cannot be asked where anything lands on this machine's picture");
                } else {
                    std::uint32_t screenVisited = 0;
                    module_object_iterator(0, &screen_on_object, &screen, screenVisited);
                    print_screen_table(screen, "every doll", modulePartsLines);
                }
            }

            std::size_t scanActors = 0;
            std::size_t scanOrphans = 0;
            {
                const std::uintptr_t world = dsh::reflect::live_world_address();

                moduleLevelActors.clear();
                if (world >= 0x10000 && watchedCount > 0) {
                    read_pointer_array(world + dsh::reflect::world_offsets::kLevels, 32, moduleLevelScratch);
                    for (const auto levelPtr : moduleLevelScratch) {
                        if (levelPtr < 0x10000) continue;
                        const std::size_t room = 60000 - moduleLevelActors.size();
                        if (room == 0) break;
                        std::vector<std::uintptr_t> actorsHere;
                        read_pointer_array(levelPtr + dsh::reflect::level_offsets::kActors, room, actorsHere);
                        moduleLevelActors.insert(moduleLevelActors.end(), actorsHere.begin(), actorsHere.end());
                    }
                }
                {
                    for (const auto actor : moduleLevelActors) {
                        if (actor < 0x10000) continue;
                        ++scanActors;
                        std::uint64_t ownerRaw = 0;
                        if (!dsh::reflect::read_process_bytes(actor + kActorOwnerOffset, &ownerRaw,
                                                              sizeof(ownerRaw))) {
                            continue;
                        }

                           if (std::find(slotActors.begin(), slotActors.end(), actor) == slotActors.end()) {
                               const std::string strayClass = dsh::reflect::object_class_name(actor);
                               bool ownedByADoll = false;
                               for (std::size_t ow = 0; ow < watchedCount; ++ow) {
                                   if (watched[ow].machine == static_cast<std::uintptr_t>(ownerRaw)) {
                                       ownedByADoll = true;
                                   }
                               }
                               if (!ownedByADoll && looks_like_a_part_class(strayClass)) {
                                   dsh::reflect::ActorPlacement strayPlace;
                                   std::string strayNote;
                                   if (dsh::reflect::read_actor_placement(actor, strayPlace, strayNote)) {
                                       for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                                           if (!machinePlacementValid[wi]) continue;
                                           const double strayDistance =
                                               distance_cm(strayPlace.locationCm, machinePlacements[wi].locationCm);
                                           if (strayDistance > 2000.0) continue;
                                           ++moduleStrayBodies;
                                           if (moduleStrayBodies > 40) break;
                                           std::string strayOwner = "NONE";
                                           if (ownerRaw >= 0x10000) {
                                               strayOwner = "\x27" + dsh::reflect::object_class_name(
                                                               static_cast<std::uintptr_t>(ownerRaw)) +
                                                            "\x27";
                                           }
                                           DSH_INFO("puppetsession: SECOND BODY[%s]: \x27%s\x27 [%s] at (%.1f, %.1f, %.1f) cm, "
                                                    "%.1f cm from this doll and in NO slot -- owner %s",
                                                    watched[wi].label.c_str(),
                                                    dsh::reflect::object_own_name(actor).c_str(), strayClass.c_str(),
                                                    static_cast<double>(strayPlace.locationCm[0]),
                                                    static_cast<double>(strayPlace.locationCm[1]),
                                                    static_cast<double>(strayPlace.locationCm[2]), strayDistance,
                                                    strayOwner.c_str());
                                           break;
                                       }
                                   }
                               }
                           }
                        std::uintptr_t owner = static_cast<std::uintptr_t>(ownerRaw);
                        int            hops = 0;
                        int            doll = -1;
                        std::uintptr_t ownerAtMatch = 0;
                        while (owner >= 0x10000 && hops < 3) {
                            ++hops;
                            for (std::size_t wi = 0; wi < watchedCount; ++wi) {
                                if (watched[wi].machine == owner) {
                                    doll = static_cast<int>(wi);
                                    break;
                                }
                            }
                            if (doll >= 0) {
                                ownerAtMatch = owner;
                                break;
                            }
                            std::uint64_t up = 0;
                            if (!dsh::reflect::read_process_bytes(owner + kActorOwnerOffset, &up,
                                                                  sizeof(up))) {
                                break;
                            }
                            owner = static_cast<std::uintptr_t>(up);
                        }
                        if (doll < 0) continue;
                        if (!machinePlacementValid[doll]) continue;
                        if (std::find(slotActors.begin(), slotActors.end(), actor) != slotActors.end()) continue;
                        ++moduleOwnedCandidates;
                        const std::string actorClass = dsh::reflect::object_class_name(actor);
                        if (actorClass.empty()) continue;
                        if (hops == 1) {

                            if (!looks_like_a_part_class(actorClass)) {
                                continue;
                            }
                        } else if (looks_transient_actor(actorClass)) {
                            ++moduleTransientSkipped;
                            continue;
                        }
                        dsh::reflect::ActorPlacement actorPlace;
                        std::string actorNote;
                        if (!dsh::reflect::read_actor_placement(actor, actorPlace, actorNote)) continue;
                        const double actorDistance =
                            distance_cm(actorPlace.locationCm, machinePlacements[doll].locationCm);
                        if (actorDistance <= kStuckPartThresholdCm) continue;
                        ++moduleOrphansSeen;
                        if (scanOrphans >= 6 || moduleOrphanLines >= 64) continue;
                        ++scanOrphans;
                        ++moduleOrphanLines;
                        const std::string ownerClass = dsh::reflect::object_class_name(ownerAtMatch);
                        DSH_INFO("puppetsession: ORPHAN[%s]: '%s %s' at (%.1f, %.1f, %.1f) cm is %.1f cm from "
                                 "the doll and %d level(s) up its Owner chain ('%s'); it is NOT in a module "
                                 "slot -- the game still owns it as this player's and the doll no longer has "
                                 "it attached",
                                 watched[doll].label.c_str(), actorClass.c_str(),
                                 dsh::reflect::object_own_name(actor).c_str(),
                                 static_cast<double>(actorPlace.locationCm[0]),
                                 static_cast<double>(actorPlace.locationCm[1]),
                                 static_cast<double>(actorPlace.locationCm[2]), actorDistance, hops,
                                 ownerClass.c_str());
                    }
                }
            }
            if (tableDue) {
                DSH_INFO("puppetsession: the stuck-part watch: %llu scan(s), %zu actor(s) in the live level this "
                         "scan, %llu actor(s) owned by a doll so far, %llu orphan(s) printed so far (%zu this "
                         "scan), %llu filtered as effects or projectiles, %llu slot change line(s), %llu attach "
                         "line(s)",
                         static_cast<unsigned long long>(moduleWatchScans), scanActors,
                         static_cast<unsigned long long>(moduleOwnedCandidates),
                         static_cast<unsigned long long>(moduleOrphansSeen), scanOrphans,
                         static_cast<unsigned long long>(moduleTransientSkipped),
                         static_cast<unsigned long long>(moduleSlotLines),
                         static_cast<unsigned long long>(moduleAttachLines));
            }
        }

        {
            dsh::game::DeathNote deathNote;
            int printed = 0;
            while (printed < 4 && dsh::game::take_death(deathNote)) {
                ++printed;

                if (pawn.valid && pawn.pawn >= 0x10000 && deathNote.actor == pawn.pawn) {
                    DSH_WARN("%s THE LOCAL PLAYER DIED -- ending the session. The three triggers are the same "
                             "teardown: the F7 row, a rest point, and this", rolePrefix);
                    dsh::game::request_session_stop("the local player died");
                }
                if (deathLines >= 64) continue;
                ++deathLines;
                if (deathLines == 64) {
                    DSH_WARN("puppetsession: DEATH -- 64 death event(s) printed; further ones in this session "
                             "are still drained from the ring but no longer printed (the enemies' deaths are "
                             "the usual source)");
                }
                const char* which = deathNote.multicast ? "MulticastOnDeath" : "ReceiveOnDeath";
                dsh::game::PuppetDoll* ownerDoll = nullptr;
                for (auto& deathSlot : dolls.slots()) {
                    if (!deathSlot) continue;
                    for (const auto dollActor : deathSlot->actors) {
                        if (dollActor == deathNote.actor) ownerDoll = &*deathSlot;
                    }
                }
                if (ownerDoll == nullptr) {
                    DSH_INFO("puppetsession: DEATH -- the game called %s on 0x%llX '%s', which is NOT one of "
                             "this side's dolls (the local player, an NPC or an enemy)",
                             which, static_cast<unsigned long long>(deathNote.actor),
                             dsh::reflect::object_class_name(deathNote.actor).c_str());
                    continue;
                }
                std::size_t filled = 0;
                for (const auto& slotRef : kModuleSlots) {
                    std::uint64_t raw = 0;
                    if (!dsh::reflect::read_process_bytes(ownerDoll->actors.front() + slotRef.offset, &raw,
                                                          sizeof(raw))) {
                        continue;
                    }
                    if (raw >= 0x10000) ++filled;
                }
                dsh::reflect::ActorPlacement deathPlace;
                std::string deathPlaceNote;
                const bool placed = dsh::reflect::read_actor_placement(
                    ownerDoll->actors.front(), deathPlace, deathPlaceNote);
                DSH_WARN("puppetsession: DEATH -- the game called %s on THIS side's doll for peer %u (0x%llX): "
                         "it stands at (%.1f, %.1f, %.1f) cm, %zu of %zu module slot(s) still filled; the "
                         "owner's newest sample is (%.1f, %.1f, %.1f) m",
                         which, static_cast<unsigned>(ownerDoll->peer.value),
                         static_cast<unsigned long long>(ownerDoll->actors.front()),
                         placed ? static_cast<double>(deathPlace.locationCm[0]) : 0.0,
                         placed ? static_cast<double>(deathPlace.locationCm[1]) : 0.0,
                         placed ? static_cast<double>(deathPlace.locationCm[2]) : 0.0, filled,
                         kModuleSlotCount, static_cast<double>(ownerDoll->latestSampleM[0]),
                         static_cast<double>(ownerDoll->latestSampleM[1]),
                         static_cast<double>(ownerDoll->latestSampleM[2]));
            }
        }

        if (now >= nextFallWatchMs) {
            nextFallWatchMs = now + 2000;
            if (!fallWatchArmed) {
                fallWatchArmed = true;
                DSH_INFO("puppetsession: fall watch (v549) armed: it prints when a doll is more than 300 cm "
                         "BELOW its owner's newest sample (the shape of a teleport it did not take) or when a "
                         "doll's health reaches 0; anything else is silence, and silence means neither "
                         "happened");
                const dsh::reflect::WorldBounds bounds = dsh::reflect::read_world_bounds();
                if (bounds.read) {
                    DSH_INFO("puppetsession: fall watch: the world's death boundary is KillZ %.1f cm and its "
                             "world-bounds check is %s -- a doll that dies far ABOVE this Z was killed by a "
                             "volume (this game ships ADAKillZVolume), not by the bound check",
                             static_cast<double>(bounds.killZ), bounds.boundsChecks ? "ON" : "OFF");
                } else {
                    DSH_WARN("puppetsession: fall watch: the world's death boundary could NOT be read (%s) -- "
                             "a death below the map can then be attributed to neither the bound check nor a "
                             "volume", bounds.note.c_str());
                }
            }
            for (auto& watchSlot : dolls.slots()) {
                if (!watchSlot || watchSlot->actors.empty() || !watchSlot->latestSampleHave) continue;
                dsh::game::PuppetDoll& doll = *watchSlot;
                const std::uintptr_t actor = doll.actors.front();

                {
                    unsigned char damageBit = 0;
                    if (dsh::reflect::read_process_bytes(actor + 0x005A, &damageBit, 1) &&
                        (damageBit & 0x01u) != 0) {
                        const unsigned char cleared = static_cast<unsigned char>(damageBit & ~0x01u);
                        if (dsh::reflect::write_process_bytes(actor + 0x005A, &cleared, 1)) {
                            ++doll.damageBlockedWrites;
                            if (doll.damageBlockedWrites <= 3) {
                                DSH_INFO("puppetsession: peer %u's doll: bCanBeDamaged was 1 and is now 0 "
                                         "( bit 0) -- a fall and the level's kill volumes can no longer "
                                         "end it, which is the one rule this game applies to an actor and not "
                                         "to the player standing beside it",
                                         static_cast<unsigned>(doll.peer.value));
                            }
                        }
                    }
                }
                std::string goneWhy;
                if (!dsh::reflect::object_is_still_registered(actor, goneWhy)) {
                    if (doll.watchLines < 12) {
                        ++doll.watchLines;
                        DSH_WARN("puppetsession: fall watch[peer %u]: the doll is GONE from the world (%s) -- "
                                 "the largest dip this session measured was %+.0f cm against its owner's "
                                 "newest sample",
                                 static_cast<unsigned>(doll.peer.value), goneWhy.c_str(),
                                 static_cast<double>(doll.watchMaxDipCm));
                    }
                    continue;
                }
                dsh::reflect::ActorPlacement place;
                std::string placeNote;
                if (!dsh::reflect::read_actor_placement(actor, place, placeNote)) continue;
                const double sampleZcm = static_cast<double>(doll.latestSampleM[2]) * 100.0;
                const double dipCm = sampleZcm - static_cast<double>(place.locationCm[2]);
                const dsh::reflect::CharacterVitals vitals = dsh::reflect::character_vitals(actor);
                const float health = vitals.read ? vitals.health : -1.0f;
                const bool hadRead = doll.watchHealthLast >= 0.0f;
                const bool wasAlive = doll.watchHealthLast > 0.0f;
                if (vitals.read && health <= 0.0f && (wasAlive || !hadRead)) {
                    if (doll.watchLines < 12) {
                        ++doll.watchLines;
                        if (wasAlive) {
                            DSH_WARN("puppetsession: fall watch[peer %u]: the doll's health reached %.1f (it "
                                     "was %.1f) while the doll stood %+.0f cm from its owner's newest sample "
                                     "(doll z %.0f cm, owner z %.0f cm) -- this is the tick the doll died",
                                     static_cast<unsigned>(doll.peer.value), static_cast<double>(health),
                                     static_cast<double>(doll.watchHealthLast), dipCm,
                                     static_cast<double>(place.locationCm[2]), sampleZcm);
                        } else {
                            DSH_WARN("puppetsession: fall watch[peer %u]: the doll's health reads %.1f at the "
                                     "FIRST reading of this session, %+.0f cm from its owner's newest sample "
                                     "(doll z %.0f cm, owner z %.0f cm) -- it was dead, or killed, before "
                                     "this probe ever looked",
                                     static_cast<unsigned>(doll.peer.value), static_cast<double>(health), dipCm,
                                     static_cast<double>(place.locationCm[2]), sampleZcm);
                        }
                    }
                }
                doll.watchHealthLast = health;
                if (dipCm > static_cast<double>(doll.watchMaxDipCm)) {
                    doll.watchMaxDipCm = static_cast<float>(dipCm);
                }
                if (dipCm > 300.0 && doll.watchLines < 12) {
                    ++doll.watchLines;
                    char healthText[32] = {};
                    if (vitals.read) {
                        std::snprintf(healthText, sizeof(healthText), "%.1f",
                                      static_cast<double>(health));
                    } else {
                        std::snprintf(healthText, sizeof(healthText), "unreadable");
                    }
                    DSH_INFO("puppetsession: fall watch[peer %u]: the doll is %.0f cm BELOW its owner's newest "
                             "sample (doll z %.0f cm, owner z %.0f cm, health %s, %zu sample(s) buffered) -- "
                             "H1: the teleport in the stream is not something this doll took",
                             static_cast<unsigned>(doll.peer.value), dipCm,
                             static_cast<double>(place.locationCm[2]), sampleZcm, healthText,
                             doll.interp.buffered());
                }
            }
        }

        {
            DSH_TIME_WALK("loop:tick2s");
        if ((now - lastPeerLog) > 2000) {
            lastPeerLog = now;
            const std::string sent =
                "chunk(s) sent=" + std::to_string(r.chunksSent) +
                " of an appearance manifest, position datagram(s) sent=" +
                std::to_string(stateDatagrams);

            const std::string received =
                "manifest datagram(s) received=" + std::to_string(manifestDatagrams) +
                (buildAttempted ? (", machine module(s) merged=" + std::to_string(modulesMerged) +
                                   " of 10")
                                : std::string(", machine not built yet"));
            const std::string applied =
                "position update(s) applied=" + std::to_string(placementsApplied) +
                " not applied=" + std::to_string(placementsDropped);
            const std::string stance =
                "stance sent=" + std::to_string(stanceDatagrams) + " last=" +
                std::to_string(static_cast<unsigned>(stanceSent.currentStance)) + " (" +
                stance_name(stanceSent.currentStance) + "); stance applied=" +
                std::to_string(stanceApplied) + " changed=" + std::to_string(stanceChanged) +
                " failed=" + std::to_string(stanceFailed) + " last=" +
                std::to_string(static_cast<unsigned>(stanceLastApplied.currentStance)) + " (" +
                stance_name(stanceLastApplied.currentStance) + ")";
            DSH_INFO("%s peers=%zu connected=%zu | SENT: %s | RECEIVED: %s | APPLIED: %s | %s",
                     rolePrefix, session.peers().size(),
                     static_cast<std::size_t>(session.connected_count()), sent.c_str(),
                     received.c_str(), applied.c_str(), stance.c_str());

            {
                std::vector<dsh::net::SessionEvent> peerEvents;
                session.take_events(peerEvents);
                for (const dsh::net::SessionEvent& ev : peerEvents) {
                    char who[32];

                    if (ev.peer.value == 0) {
                        std::snprintf(who, sizeof(who), "host");
                    } else {
                        std::snprintf(who, sizeof(who), "client%u",
                                      static_cast<unsigned>(ev.peer.value));
                    }
                    switch (ev.kind) {
                        case dsh::net::SessionEventKind::peerJoined:
                            DSH_INFO("%s peer %s JOINED -- it calls itself '%s'", rolePrefix, who,
                                     ev.text.c_str());
                            break;
                        case dsh::net::SessionEventKind::peerLeft:
                            DSH_WARN("%s peer %s LEFT", rolePrefix, who);
                            break;
                        case dsh::net::SessionEventKind::peerTimedOut:
                            DSH_WARN("%s peer %s TIMED OUT -- silent past the session's timeout, so nothing "
                                     "it owns is being updated any more",
                                     rolePrefix, who);
                            break;
                        case dsh::net::SessionEventKind::handshakeFailed:
                            DSH_ERROR("%s handshake FAILED: %s", rolePrefix, ev.text.c_str());
                            break;
                        case dsh::net::SessionEventKind::budgetExceeded:
                            DSH_WARN("%s peer %s exceeded its send budget (%u byte(s) in the window) -- "
                                     "datagrams are being dropped by THIS side, not by the wire",
                                     rolePrefix, who, ev.value);
                            break;
                        default:
                            break;
                    }
                }
            }

            for (const auto& peerForGone : session.peers()) {
                const std::uint8_t goneSlot = peerForGone.id.value;
                if (goneSlot >= dsh::game::PuppetDolls::kSlots) continue;
                if (peerForGone.state == dsh::net::PeerState::connected) {
                    goneSinceMs[goneSlot] = 0;
                    continue;
                }
                if (goneSinceMs[goneSlot] == 0) {
                    goneSinceMs[goneSlot] = GetTickCount64();
                    continue;
                }
                if (GetTickCount64() - goneSinceMs[goneSlot] < 20000) continue;
                dsh::game::PuppetDoll* gone = dolls.find(peerForGone.id);
                if (gone == nullptr) {
                    goneSinceMs[goneSlot] = 0;
                    continue;
                }

                const std::vector<std::uintptr_t> victims = gone->actors;
                dolls.slots()[goneSlot].reset();
                puppetActors.clear();
                for (const auto& remain : dolls.slots()) {
                    if (remain && !remain->actors.empty()) {
                        puppetActors = remain->actors;
                        break;
                    }
                }
                std::vector<std::uintptr_t> ownersNow;
                for (const auto& ownerSlot : dolls.slots()) {
                    if (ownerSlot && !ownerSlot->actors.empty()) {
                        ownersNow.push_back(ownerSlot->actors.front());
                    }
                }
                dsh::game::set_puppet_owners(ownersNow.data(), ownersNow.size());
                goneSinceMs[goneSlot] = 0;
                DSH_INFO("%s peer %u has been gone for 20 s -- its doll was dropped from the table and is "
                         "being destroyed", rolePrefix, static_cast<unsigned>(goneSlot));
                if (!victims.empty()) {
                    dsh::reflect::run_on_game_thread(
                        [victims]() { dsh::reflect::puppet_destroy_actors(victims); }, 2000);
                }
            }

            DSH_INFO("%s puppets -- %zu doll(s) in the table, %zu built on this side; %llu datagram(s) "
                     "refused for a sender id that names no peer",
                     rolePrefix, dolls.size(), dolls.built_count(),
                     static_cast<unsigned long long>(dollRefused));

            if (asHost) {
                DSH_INFO("%s relay -- %llu datagram(s) from clients that every player needs; forwarded=%llu "
                         "(%llu byte(s); %llu of them a stored manifest re-sent to another player); refused=%llu. "
                         "Two players mean forwarded=0 by construction",
                         rolePrefix, static_cast<unsigned long long>(relayCandidates),
                         static_cast<unsigned long long>(session.stats().relayedDatagrams),
                         static_cast<unsigned long long>(session.stats().relayedBytes),
                         static_cast<unsigned long long>(manifestReplayed),
                         static_cast<unsigned long long>(session.stats().relayRefused));
            }

            const std::string ownDerivedText =
                hostDerivedGetters.sampled
                    ? (" getters=" + hostDerivedGetters.note)
                    : (" getters=NOT sampled (" +
                       (hostDerivedGetters.note.empty()
                            ? std::string("the game thread did not take the sample in time")
                            : hostDerivedGetters.note) +
                       ")");
            const std::string inputSentText =
                "movement input SENT -- what this machine put on the wire: " +
                dsh::game::movement_input_line(hostInputSent) +
                "; datagram(s) carrying anything but idle=" +
                std::to_string(inputDatagramsNonZero) + " (this window " +
                std::to_string(inputDatagramsNonZeroThisWindow) +
                "), datagram(s) carrying a BOOST byte=" +
                std::to_string(boostDatagramsNonZeroThisWindow) + " this window, " +
                std::to_string(boostDatagramsNonZero) + " in the session; this machine's own four " +
                "derived getters sampled on this tick" + ownDerivedText;
            std::string inputAppliedText =
                "movement input APPLIED to the puppet -- what this machine did with the peer's: flags "
                "applied=" + std::to_string(inputFlagsApplied) + " changed=" +
                std::to_string(inputFlagsChanged) + " failed=" + std::to_string(inputFlagsFailed) +
                " (of the changed, " + std::to_string(inputFlagsChangedBoost) +
                " changed a BOOST byte); vector applied=" +
                std::to_string(inputVectorApplied) + " changed=" +
                std::to_string(inputVectorChanged) + " failed=" +
                std::to_string(inputVectorFailed) + "; the peer last sent " +
                dsh::game::movement_input_line(inputLastReceived) + "; the puppet last holds " +
                dsh::game::movement_input_line(inputLastWritten) +

                "; puppet's own byte moved without us=" + std::to_string(puppetFlagMoves);

            {
                const std::string ownFlagEdges =
                    flagEdges.havePrevious
                        ? ("sprint 0->1 " + std::to_string(flagEdgesThisWindow.sprintUp) +
                           " this window / " + std::to_string(flagEdges.sprintUp) +
                           " session, 1->0 " + std::to_string(flagEdgesThisWindow.sprintDown) +
                           " this window / " + std::to_string(flagEdges.sprintDown) +
                           " session; hover 0->1 " + std::to_string(flagEdgesThisWindow.hoverUp) +
                           " this window / " + std::to_string(flagEdges.hoverUp) +
                           " session, 1->0 " + std::to_string(flagEdgesThisWindow.hoverDown) +
                           " this window / " + std::to_string(flagEdges.hoverDown) +
                           " session; boostJump 0->1 " + std::to_string(flagEdgesThisWindow.boostJumpUp) +
                           " this window / " + std::to_string(flagEdges.boostJumpUp) +
                           " session; boostDash 0->1 " + std::to_string(flagEdgesThisWindow.boostDashUp) +
                           " this window / " + std::to_string(flagEdges.boostDashUp) + " session")
                        : std::string("no previous read to compare against yet");
                const std::string edgeVerdict =
                    flagEdges.sprintUp > 0
                        ? (" -- VERDICT: this machine's own sprint byte was OBSERVED at 0->1 at least "
                           "once, so the operator did press sprint; the level sample can miss it, this "
                           "count does not")
                        : std::string(" -- VERDICT: no 0->1 edge was OBSERVED on this machine's own sprint "
                                      "byte, so either the operator did not press it or every press began "
                                      "and ended between two reads");
                DSH_INFO("%s flag transition(s) among the reads this loop already makes "
                         "(a LOWER BOUND on the true number -- a transition shorter than the gap between "
                         "two reads is invisible to any sampler, so 0 means NOT SEEN and not DID NOT "
                         "HAPPEN): %s%s",
                         rolePrefix, ownFlagEdges.c_str(), edgeVerdict.c_str());
            }
            DSH_INFO("%s%s", rolePrefix, inputSentText.c_str());
            DSH_INFO("%s%s", rolePrefix, inputAppliedText.c_str());

            {
                const std::string energySentText =
                    "energy timers SENT -- what this machine put on the wire: " +
                    dsh::game::energy_timer_line(hostEnergySent, haveEnergy,
                                                 energySentHavePrevious,
                                                 energySentPreviousIntermission,
                                                 std::string("this machine's own two timers were never "
                                                             "read, so zeros were sent")) +
                    "; datagram(s) carrying a non-zero pair=" +
                    std::to_string(energyDatagramsNonZero) + " (this window " +
                    std::to_string(energyDatagramsNonZeroThisWindow) + ")";
                const std::string energyAppliedText =
                    "energy timers APPLIED to the puppet -- what this machine did with the peer's: "
                    "applied=" + std::to_string(energyApplied) + " changed=" +
                    std::to_string(energyChanged) + " failed=" + std::to_string(energyFailed) +
                    "; the peer last sent " +
                    dsh::game::energy_timer_line(energyLastReceived,
                                                 energyLastReceivedReadable,
                                                 energyLastStepKnown,
                                                 energyLastStep,
                                                 energyPairEverNonZero
                                                     ? "this payload carries no read flag for these two "
                                                       "fields, so a pair of zeros is not evidence that the "
                                                       "peer read them"
                                                     : "the peer has never sent a non-zero pair, and this "
                                                       "payload carries no read flag for these two fields, "
                                                       "so no readable pair has been seen on this side") +
                    "; the puppet last holds " +
                    dsh::game::energy_timer_line(energyLastApplied.written,
                                                 energyLastApplied.writtenRead,
                                                 clientAppliedHavePrevious,
                                                 clientAppliedPreviousIntermission,
                                                 energyLastNote);
                DSH_INFO("%s%s", rolePrefix, energySentText.c_str());
                DSH_INFO("%s%s", rolePrefix, energyAppliedText.c_str());
            }

            {

                const std::string weaponSentText =
                    "front weapon SENT -- this machine's own choice: " +
                    dsh::game::front_weapon_line(hostFrontWeaponIndex, haveWeapon,
                                                 "this machine's own three weapon pointers have not been "
                                                 "read") +
                    "; datagram(s) carrying one of the two slots=" +
                    std::to_string(weaponDatagramsDrawn) +
                    ", datagram(s) carrying the not-read value=" +
                    std::to_string(weaponDatagramsNotRead);
                DSH_INFO("%s%s", rolePrefix, weaponSentText.c_str());

                for (const auto& slot : dolls.slots()) {
                    if (!slot) continue;
                    const DollDiag& diag = doll_diag(slot->peer);
                    std::string held;
                    if (!diag.weaponLastApplied.beforeRead) {

                        held = diag.weaponLastNote.empty()
                                   ? std::string("no dispatch has read this doll's own three weapon pointers "
                                                 "yet")
                                   : std::string("every read of this doll's own three weapon pointers has "
                                                 "refused -- the last refusal beside this names which");
                    } else {
                        const std::uintptr_t inHand = diag.weaponLastApplied.afterRead
                                                          ? diag.weaponLastApplied.after.current
                                                          : diag.weaponLastApplied.before.current;
                        if (inHand == 0) {
                            held = "this doll's own CurrentFrontWeapon is NULL -- nothing is in its "
                                   "hand";
                        } else {
                            std::string className = dsh::reflect::object_class_name(inHand);
                            if (className.empty()) className = "<its class could not be named>";
                            held = "this doll's own CurrentFrontWeapon is " + hex_pointer(inHand) +
                                   " ('" + className + "')";
                        }
                    }
                    const std::string refusal =
                        diag.weaponLastNote.empty() ? std::string()
                                                    : ("; last refusal: " + diag.weaponLastNote);
                    DSH_INFO("%s front weapon APPLIED to peer %u's doll -- the peer's statement and what this "
                             "machine did with it: %s; stated=%llu calls=%llu applied=%llu holding=%llu "
                             "failed=%llu wire-said-not-read=%llu; %s%s",
                             rolePrefix, static_cast<unsigned>(slot->peer.value),
                             dsh::game::front_weapon_line(
                                 diag.weaponLastReceivedIndex, diag.weaponLastReceivedValid,
                                 "no state payload from this peer has been decoded on this side")
                                 .c_str(),
                             static_cast<unsigned long long>(diag.weaponStated),
                             static_cast<unsigned long long>(diag.weaponCalls),
                             static_cast<unsigned long long>(diag.weaponApplied),
                             static_cast<unsigned long long>(diag.weaponHolding),
                             static_cast<unsigned long long>(diag.weaponFailed),
                             static_cast<unsigned long long>(diag.weaponNotRead), held.c_str(),
                             refusal.c_str());
                }
            }

            {
                DSH_INFO("%s movement writes SESSION totals (NOT this window) -- velocity: applied=%llu "
                         "changed=%llu failed=%llu, last (%.1f, %.1f, %.1f); acceleration: applied=%llu "
                         "changed=%llu failed=%llu, last (%.1f, %.1f, %.1f); a `changed` on almost every "
                         "`applied` means the component was cleared between dispatches, a rare one means "
                         "the value survived",
                         rolePrefix, static_cast<unsigned long long>(velocityApplied),
                         static_cast<unsigned long long>(velocityChanged),
                         static_cast<unsigned long long>(velocityFailed),
                         static_cast<double>(velocityLastApplied[0]),
                         static_cast<double>(velocityLastApplied[1]),
                         static_cast<double>(velocityLastApplied[2]),
                         static_cast<unsigned long long>(accelerationApplied),
                         static_cast<unsigned long long>(accelerationChanged),
                         static_cast<unsigned long long>(accelerationFailed),
                         static_cast<double>(accelerationLastApplied[0]),
                         static_cast<double>(accelerationLastApplied[1]),
                         static_cast<double>(accelerationLastApplied[2]));
            }

            {
                std::string ownRefusal;
                const std::uintptr_t ownMachine = local_pawn_machine(localPawnMachine, ownRefusal);
                const dsh::game::ExclusiveState mine = dsh::game::exclusive_state_read(
                    ownMachine, movementStateClass, dsh::reflect::read_process_bytes);

                std::string mineText = dsh::game::exclusive_state_line(mine);
                if (mine.read && mine.owner >= 0x10000) {
                    mineText += " ('" + dsh::reflect::object_own_name(mine.owner) + "' [" +
                                dsh::reflect::object_class_name(mine.owner) + "])";
                }
                DSH_INFO("%s the exclusive mode this tick -- this side's doll: %s", rolePrefix,
                         mineText.c_str());

                for (const auto& slot : dolls.slots()) {
                    if (!slot || slot->actors.empty()) continue;
                    const DollDiag& diag = doll_diag(slot->peer);
                    const dsh::game::ExclusiveState theirs = dsh::game::exclusive_state_read(
                        slot->actors.front(), movementStateClass, dsh::reflect::read_process_bytes);
                    std::string theirsText = dsh::game::exclusive_state_line(theirs);
                    if (theirs.read && theirs.owner >= 0x10000) {
                        theirsText += " ('" + dsh::reflect::object_own_name(theirs.owner) + "' [" +
                                      dsh::reflect::object_class_name(theirs.owner) + "])";
                    }
                    DSH_INFO("%s the exclusive-mode stream for peer %u's doll -- start=%llu end=%llu "
                             "refused=%llu failed=%llu; the last pair this side saw from that peer was "
                             "types=%u (%s) kind=%u (%s), and that doll currently holds %s",
                             rolePrefix, static_cast<unsigned>(slot->peer.value),
                             static_cast<unsigned long long>(diag.exclusiveStarted),
                             static_cast<unsigned long long>(diag.exclusiveEnded),
                             static_cast<unsigned long long>(diag.exclusiveRefused),
                             static_cast<unsigned long long>(diag.exclusiveFailed),
                             static_cast<unsigned>(slot->lastExclusiveTypes),
                             dsh::game::exclusive_types_text(slot->lastExclusiveTypes).c_str(),
                             static_cast<unsigned>(slot->lastExclusiveOwnerKind),
                             dsh::game::exclusive_owner_kind_text(static_cast<dsh::game::ExclusiveOwnerKind>(
                                 slot->lastExclusiveOwnerKind)),
                             theirsText.c_str());
                }
            }
            {

                const GetterCapture& derived = puppetDerivedGetters;
                const std::string derivedText =
                    derived.sampled
                        ? ("derived on the puppet, asked on the dispatch that last WROTE the movement "
                           "input -- the byte it was asked about was " +
                           dsh::game::movement_input_line(derived.bytes) + " and the answers were " +
                           derived.note)
                        : ("derived on the puppet: NOT sampled (" +
                           (derived.note.empty()
                                ? std::string("no dispatch has written a movement input yet")
                                : derived.note) +
                           ")");

                std::string abilityText;
                if (puppetActors.empty()) {
                    abilityText = "the machine is not built yet, so there is no puppet to read an "
                                  "ability system off";
                } else {
                    dsh::game::AbilitySystemState ability;
                    std::string abilityNote;
                    if (!dsh::game::read_character_ability_system(
                            puppetActors.front(), movementStateClass, ability, abilityNote,
                            dsh::game::stance_read_process_bytes)) {
                        abilityText = "NOT read -- " + abilityNote;
                    } else if (ability.abilitySystem == 0) {

                        abilityText =
                            "the pointer at is NULL -- the puppet has no ability system, so "
                            "every route that goes through activating an ability is closed";
                    } else {
                        std::string className = dsh::reflect::object_class_name(ability.abilitySystem);
                        if (className.empty()) {
                            className = "<its class could not be named>";
                        }
                        abilityText = "the pointer at is (0x" +
                                      std::to_string(static_cast<unsigned long long>(
                                          ability.abilitySystem)) +
                                      ") and holds a '" + className + "'";
                    }
                }

                DSH_INFO("%s the two BOOST bytes are not the whole question -- %s; ability "
                         "system: %s; %s",
                         rolePrefix, derivedText.c_str(), abilityText.c_str(),
                         sprint_call_line(sprintLast, sprintCalls).c_str());

                {
                    std::string sprintOwnRefusal;
                    const std::uintptr_t ownForSprint =
                        local_pawn_machine(localPawnMachine, sprintOwnRefusal);

                    std::string meshSprint = " | the mesh owner's own bSprint: mine humanoid=" +
                                             mesh_owner_sprint_word(ownForSprint);
                    for (const auto& meshSlot : dolls.slots()) {
                        if (!meshSlot) continue;
                        meshSprint += ", peer " +
                                      std::to_string(static_cast<unsigned>(meshSlot->peer.value)) +
                                      " humanoid=" +
                                      mesh_owner_sprint_word(meshSlot->actors.empty()
                                                                 ? static_cast<std::uintptr_t>(0)
                                                                 : meshSlot->actors.front());
                    }
                    DSH_INFO("%s %s%s%s", rolePrefix,
                             sprint_call_counts_line(sprintCalls, sprintRefused, sprintByteMatched,
                                                     sprintByteReverted, sprintRawAgreed,
                                                     sprintDerivedAgreed)
                                 .c_str(),
                             meshSprint.c_str(),
                             sprint_effect_text(sprintEffectApplies, sprintEffectRefusals,
                                                sprintEffectRemovals, sprintEffectRemovalFailures,
                                                sprintEffectHandle)
                                 .c_str());
                }

                {
                    std::string ownRefusal2;
                    const std::uintptr_t own = local_pawn_machine(localPawnMachine, ownRefusal2);
                    const std::string mineInit =
                        hostDerivedGetters.sampled
                            ? (hostDerivedGetters.flags.initialized ? "1" : "0")
                            : ("NOT sampled (" +
                               (hostDerivedGetters.note.empty() ? std::string("no dispatch has taken a "
                                                                           "sample yet")
                                                                : hostDerivedGetters.note) +
                               ")");

                    std::string theirsInit;
                    {
                        std::uint32_t initDolls = 0;
                        for (const auto& initSlot : dolls.slots()) {
                            if (!initSlot) continue;
                            const DollDiag& diag = doll_diag(initSlot->peer);
                            ++initDolls;
                            theirsInit += std::string(initDolls == 1 ? "" : "; ") + "peer " +
                                          std::to_string(static_cast<unsigned>(initSlot->peer.value)) + "=";
                            if (diag.initSampled) {
                                theirsInit += diag.initValue ? "1" : "0";
                            } else {
                                theirsInit += "NOT sampled (" +
                                              (diag.initNote.empty()
                                                   ? std::string("no dispatch has written that doll's "
                                                                 "movement input yet")
                                                   : diag.initNote) +
                                              ")";
                            }
                        }
                        if (initDolls == 0) theirsInit = "no doll built";
                    }
                    auto energyText = [](std::uintptr_t actor) -> std::string {
                        if (actor < 0x10000) return "no actor";
                        const dsh::reflect::CharacterEnergy energy =
                            dsh::reflect::character_energy(actor);
                        if (!energy.read) return "NOT read (" + energy.note + ")";
                        char buf[64];
                        std::snprintf(buf, sizeof(buf), "%.1f of %.1f", energy.energy,
                                      energy.maxEnergy);
                        return std::string(buf);
                    };

                    std::string theirsEnergy;
                    {
                        std::uint32_t energyDolls = 0;
                        for (const auto& energySlot : dolls.slots()) {
                            if (!energySlot) continue;
                            ++energyDolls;
                            theirsEnergy +=
                                std::string(energyDolls == 1 ? "" : "; ") + "peer " +
                                std::to_string(static_cast<unsigned>(energySlot->peer.value)) + " " +
                                energyText(energySlot->actors.empty() ? static_cast<std::uintptr_t>(0)
                                                                      : energySlot->actors.front());
                        }
                        if (energyDolls == 0) theirsEnergy = "no doll built";
                    }
                    DSH_INFO("%s the character's own state -- IsInitialized: mine=%s, %s; energy: "
                             "mine %s, %s",
                             rolePrefix, mineInit.c_str(), theirsInit.c_str(),
                             energyText(own).c_str(), theirsEnergy.c_str());
                }
            }
            inputDatagramsNonZeroThisWindow = 0;
            boostDatagramsNonZeroThisWindow = 0;

            energyDatagramsNonZeroThisWindow = 0;

            hostDerivedGetters = GetterCapture{};
            if (pawn.valid) {
                DSH_TIME_WALK("loop:tick2s:getters");
                dsh::reflect::CharacterDerivedFlags flags;
                std::string refusal;
                bool callSucceeded = false;
                if (dsh::reflect::run_on_game_thread(
                        [&pawn, &flags, &refusal, &callSucceeded]() {

                            callSucceeded =
                                dsh::reflect::character_call_derived_flags(pawn.pawn, flags, refusal);
                        },
                        kPlacementTimeoutMs)) {
                    hostDerivedGetters.sampled = callSucceeded;
                    hostDerivedGetters.flags = flags;
                    hostDerivedGetters.note = refusal;
                }
            } else {
                hostDerivedGetters.note = "there is no local pawn handle right now";
            }

            {
                std::string movementSent;
                if (!pawn.valid) {
                    movementSent = "movement SENT: not sampled -- there is no local pawn handle right now";
                } else {
                    char trailing[256];
                    std::snprintf(trailing, sizeof(trailing),
                                  "velocity sent=(%.1f, %.1f, %.1f) this machine's MovementMode=%s(%u) "
                                  "non-zero velocity datagram(s) this window=%llu",
                                  static_cast<double>(hostVelocitySent[0]),
                                  static_cast<double>(hostVelocitySent[1]),
                                  static_cast<double>(hostVelocitySent[2]),
                                  dsh::game::movement_mode_name(hostModeSent),
                                  static_cast<unsigned>(hostModeSent),
                                  static_cast<unsigned long long>(velocityDatagramsNonZeroThisWindow));
                    movementSent = "movement SENT (this machine's own pawn): " +
                                   movement_state_line("the local pawn", hostMovement,
                                                       hostWindowMaxSpeed, trailing);
                }
                DSH_INFO("%s %s", rolePrefix, movementSent.c_str());
            }
            {

                std::uint32_t printed = 0;
                for (const auto& slot : dolls.slots()) {
                    if (!slot || slot->actors.empty()) continue;
                    const DollDiag& diag = doll_diag(slot->peer);
                    ++printed;

                    std::string stateText =
                        movement_state_line("this doll", diag.probe, diag.windowMaxSpeed, nullptr);
                    std::string stateVerdict;
                    if (const auto bar = stateText.find(" | "); bar != std::string::npos) {
                        stateVerdict = stateText.substr(bar + 3);
                        stateText.resize(bar);
                    }
                    DSH_INFO("%s movement APPLIED to peer %u's doll '%s': %s", rolePrefix,
                             static_cast<unsigned>(slot->peer.value),
                             dsh::reflect::object_class_name(slot->actors.front()).c_str(),
                             stateText.c_str());
                    DSH_INFO("%s movement deltas for peer %u's doll -- velocity this side wrote "
                             "last=(%.1f, %.1f, %.1f) cm/s, written %llu time(s) this window; "
                             "LastUpdateVelocity held it in %llu of %llu sample(s) this window; MODE writes "
                             "asked=%llu kept=%llu REVERTED=%llu refused=%llu (session %llu/%llu/%llu); "
                             "LastUpdateLocation delta between placements: >%.1fcm on %llu/%llu this window "
                             "and %llu/%llu session, max %.1f/%.1f cm win/session, last %.1fcm%s%s",
                             rolePrefix, static_cast<unsigned>(slot->peer.value),
                             static_cast<double>(diag.velocityApplied[0]),
                             static_cast<double>(diag.velocityApplied[1]),
                             static_cast<double>(diag.velocityApplied[2]),
                             static_cast<unsigned long long>(diag.velocityAppliedWindow),
                             static_cast<unsigned long long>(diag.survivorHitsWindow),
                             static_cast<unsigned long long>(diag.survivorHitsWindow +
                                                             diag.survivorMissesWindow),
                             static_cast<unsigned long long>(diag.modeWrittenWindow),
                             static_cast<unsigned long long>(diag.modeAppliedWindow),
                             static_cast<unsigned long long>(diag.modeRevertedWindow),
                             static_cast<unsigned long long>(diag.modeRefusedWindow),
                             static_cast<unsigned long long>(diag.modeWritten),
                             static_cast<unsigned long long>(diag.modeApplied),
                             static_cast<unsigned long long>(diag.modeReverted),
                             static_cast<double>(kIntegrationMoveThresholdCm),
                             static_cast<unsigned long long>(diag.integMovesWindow),
                             static_cast<unsigned long long>(diag.integComparisonsWindow),
                             static_cast<unsigned long long>(diag.integMoves),
                             static_cast<unsigned long long>(diag.integComparisons),
                             static_cast<double>(diag.integMaxWindowCm),
                             static_cast<double>(diag.integMaxCm),
                             static_cast<double>(diag.integLastCm),
                             stateVerdict.empty() ? "" : " | ",
                             stateVerdict.c_str());
                }
                if (printed == 0) {
                    DSH_INFO("%s movement APPLIED: not sampled -- no doll has been built yet, so there is no "
                             "actor to read a movement component off", rolePrefix);
                }
            }

            {

                DSH_TIME_WALK("loop:tick2s:walks");
                const std::uintptr_t ownMachine = local_pawn_machine(localPawnMachine, localPawnRefusal);
                 for (const auto& walk :
                     thruster_module_walk(
                         ownMachine, kWalkLabelMine, thrusterModuleClass, niagaraComponentClass,
                         moduleWalkClassRefusalLogged,
                         ownMachine >= 0x10000
                             ? std::string()
                             : ("this side's own pawn is not a machine actor right now: " +
                                localPawnRefusal))) {
                    DSH_INFO("%s", walk.c_str());
                }

                for (const auto& thrusterSlot : dolls.slots()) {
                    if (!thrusterSlot) continue;
                    const std::uintptr_t puppetMachine =
                        thrusterSlot->actors.empty() ? 0 : thrusterSlot->actors.front();
                    const std::string label =
                        std::string(kWalkLabelTheirs) + "-" +
                        std::to_string(static_cast<unsigned>(thrusterSlot->peer.value));
                    for (const auto& walk :
                         thruster_module_walk(puppetMachine, label.c_str(), thrusterModuleClass,
                                              niagaraComponentClass, moduleWalkClassRefusalLogged)) {
                        DSH_INFO("%s", walk.c_str());
                    }
                }

                {
                    const std::uintptr_t primitiveClass =
                        dsh::reflect::resolve_uclass_address("PrimitiveComponent");
                    dsh::reflect::ActorPlacement ownPlace;
                    std::string ownPlaceNote;
                    if (ownMachine >= 0x10000 &&
                        dsh::reflect::read_actor_placement(ownMachine, ownPlace, ownPlaceNote)) {

                        doll_track_push(ownMachine, ownPlace.locationCm);
                        (void)check_body_meshes(kWalkLabelMine, ownMachine, ownPlace.locationCm,
                                                primitiveClass, modulePartsLines, false);
                    }
                    for (const auto& bodySlot : dolls.slots()) {
                        if (!bodySlot || bodySlot->actors.empty()) continue;
                        const std::uintptr_t bodyMachine = bodySlot->actors.front();
                        if (bodyMachine < 0x10000) continue;
                        dsh::reflect::ActorPlacement bodyPlace;
                        std::string bodyPlaceNote;
                        if (!dsh::reflect::read_actor_placement(bodyMachine, bodyPlace, bodyPlaceNote)) continue;
                        doll_track_push(bodyMachine, bodyPlace.locationCm);
                        const std::string bodyLabel =
                            std::string(kWalkLabelTheirs) + "-" +
                            std::to_string(static_cast<unsigned>(bodySlot->peer.value));
                        (void)check_body_meshes(bodyLabel, bodyMachine, bodyPlace.locationCm, primitiveClass,
                                                modulePartsLines, false);
                    }
                }

                {

                    const bool anyDoll = std::any_of(dolls.slots().begin(), dolls.slots().end(),
                                                     [](const auto& s) { return s && !s->actors.empty(); });
                    if (anyDoll && !animProbeSawPuppet) {
                        animProbeSawPuppet = true;
                        animProbeDumps = 0;
                    }

                    if (sprintEdgeForAnimation.exchange(false) && !animProbeRearmed) {
                        animProbeRearmed = true;
                        animProbeDumps = 0;
                        DSH_INFO("%s the animation tables are RE-ARMED for this sprint edge: the next ten "
                                 "two-second dumps are the ones that contain the effect being applied",
                                 rolePrefix);
                    }
                    if (animProbeDumps < 10 && ownMachine >= 0x10000) {
                        ++animProbeDumps;
                        dump_anim_table(ownMachine, "mine", rolePrefix);

                        for (const auto& animSlot : dolls.slots()) {
                            if (!animSlot || animSlot->actors.empty()) continue;
                            const std::string label =
                                "theirs-" + std::to_string(static_cast<unsigned>(animSlot->peer.value));
                            dump_anim_table(animSlot->actors.front(), label.c_str(), rolePrefix);
                        }
                    }

                    if (ownMachine >= 0x10000) {
                        log_aim_geometry(ownMachine, "mine", rolePrefix);
                        std::uint64_t ownHumanoid = 0;
                        (void)dsh::reflect::read_process_bytes(ownMachine + kMachineHumanoidOffset, &ownHumanoid,
                                                               sizeof(ownHumanoid));
                        watch_anim_graph(static_cast<std::uintptr_t>(ownHumanoid), "mine", "humanoid",
                                         rolePrefix, kHumanoidWatchVars,
                                         sizeof(kHumanoidWatchVars) / sizeof(kHumanoidWatchVars[0]),
                                         animWatch, animWatchLinesLeft);
                        watch_anim_graph(ownMachine, "mine", "pawn", rolePrefix, kLegWatchVars,
                                         sizeof(kLegWatchVars) / sizeof(kLegWatchVars[0]), animWatch,
                                         animWatchLinesLeft);
                        for (const auto& animSlot : dolls.slots()) {
                            if (!animSlot || animSlot->actors.empty()) continue;
                            const std::string label =
                                "theirs-" + std::to_string(static_cast<unsigned>(animSlot->peer.value));
                            const std::uintptr_t doll = animSlot->actors.front();
                            log_aim_geometry(doll, label.c_str(), rolePrefix);
                            std::uint64_t dollHumanoid = 0;
                            (void)dsh::reflect::read_process_bytes(doll + kMachineHumanoidOffset, &dollHumanoid,
                                                                   sizeof(dollHumanoid));
                            watch_anim_graph(static_cast<std::uintptr_t>(dollHumanoid), label.c_str(),
                                             "humanoid", rolePrefix, kHumanoidWatchVars,
                                             sizeof(kHumanoidWatchVars) / sizeof(kHumanoidWatchVars[0]),
                                             animWatch, animWatchLinesLeft);
                            watch_anim_graph(doll, label.c_str(), "pawn", rolePrefix, kLegWatchVars,
                                             sizeof(kLegWatchVars) / sizeof(kLegWatchVars[0]), animWatch,
                                             animWatchLinesLeft);
                        }
                    }
                }
            }

            {
                std::string watchRefusal;
                if (!watchStarted) {
                    if (localPawnMachine < 0x10000) {
                        watchRefusal = "the local pawn has not been located yet, so there is no machine "
                                       "whose fields could be swept (" + localPawnRefusal + ")";
                    } else if (cached_module_walk(kWalkLabelMine) == nullptr) {
                        watchRefusal = "the component list the watch names its module rows after comes from "
                                       "the two-second tick's cached walk, and that walk has not run yet on "
                                       "this side -- NOTHING has been swept, which is not the same as a "
                                       "sweep that found no move";
                    } else {
                        watchRefusal = "the cached thruster walk names no module on this side's machine "
                                       "(its own line above says why), so the module rows could not be "
                                       "opened -- and that is a refusal, not a table of zeros";
                    }
                }
                DSH_INFO("%s%s", rolePrefix,
                         dsh::game::watch_summary_line(watch, watchRefusal).c_str());
            }

            hostWindowMaxSpeed = 0.0f;

            for (DollDiag& diag : dollDiag) {
                diag.windowMaxSpeed = 0.0f;
                diag.velocityAppliedWindow = 0;
                diag.velocityApplied[0] = 0.0f;
                diag.velocityApplied[1] = 0.0f;
                diag.velocityApplied[2] = 0.0f;
                diag.survivorHitsWindow = 0;
                diag.survivorMissesWindow = 0;
                diag.modeWrittenWindow = 0;
                diag.modeAppliedWindow = 0;
                diag.modeRevertedWindow = 0;
                diag.modeRefusedWindow = 0;
                diag.integMovesWindow = 0;
                diag.integComparisonsWindow = 0;
                diag.integMaxWindowCm = 0.0f;
            }
            velocityDatagramsNonZeroThisWindow = 0;

            sprintCallsThisWindow = 0;
            sprintRefusedThisWindow = 0;
            sprintByteMatchedThisWindow = 0;
            sprintByteRevertedThisWindow = 0;
            sprintRawAgreedThisWindow = 0;
            sprintDerivedAgreedThisWindow = 0;

            flagEdgesThisWindow = MovementFlagEdges{};

        }
        }

        if (!enteredLoop) {
            enteredLoop = true;
            DSH_INFO("puppetsession[%s]: entering the session loop -- the capture and the encode below "
                     "this point are done, and every line after it is the loop's",
                     asHost ? "host" : "client");

            try {
                const dsh::reflect::GateSurvey gates = dsh::reflect::survey_gates();
                for (const std::string& reportLine : gates.report_lines()) {
                    DSH_INFO("puppetsession: %s", reportLine.c_str());
                }
            } catch (...) {
                DSH_WARN("puppetsession: the door/wall/gate survey threw and was ABANDONED -- the session "
                         "carries on, because this survey is a diagnostic and nothing else depends on it");
            }
        }

        dsh::game::set_action_capture_enabled(true);

        {
            std::string moduleRefusal;
            const std::uintptr_t mine = local_pawn_machine(localPawnMachine, moduleRefusal);
            std::uint64_t frontRaw = 0, leftRaw = 0, rightRaw = 0;
            if (mine >= 0x10000) {
                dsh::reflect::read_process_bytes(mine + 0x1630, &frontRaw, sizeof(frontRaw));
                dsh::reflect::read_process_bytes(mine + 0x1610, &leftRaw, sizeof(leftRaw));
                dsh::reflect::read_process_bytes(mine + 0x1618, &rightRaw, sizeof(rightRaw));
            }
            dsh::game::set_local_weapon_modules(static_cast<std::uintptr_t>(frontRaw),
                                                static_cast<std::uintptr_t>(leftRaw),
                                                static_cast<std::uintptr_t>(rightRaw));

            if (!ammoFunctionResolved) {
                ammoFunctionResolved = true;
                const std::uintptr_t ammoInterface = dsh::reflect::resolve_uclass_address("DAAmmoInterface");
                if (ammoInterface < 0x10000) {
                    DSH_ERROR("ammo: the DAAmmoInterface UClass is not in this process's object table, so no "
                              "projectile can be captured this session");
                } else {
                    const dsh::reflect::FunctionRef ref =
                        dsh::reflect::reflect_resolve_function(ammoInterface, "SetAmmoParams");
                    dsh::game::set_ammo_params_function(ref.usable ? ref.function : 0);
                    if (ref.usable) {
                        DSH_INFO("ammo: capturing the game's own 'SetAmmoParams' on DAAmmoInterface (0x%llX) -- "
                                 "this is where the projectile is handed its FDAAmmoParams",
                                 static_cast<unsigned long long>(ref.function));
                    } else {
                        DSH_ERROR("ammo: DAAmmoInterface resolved to 0x%llX but declares no "
                                  "'SetAmmoParams' this build can find -- no projectile will be captured",
                                  static_cast<unsigned long long>(ammoInterface));
                    }
                }
            }
            dsh::game::set_ammo_capture_enabled(true);

            if (!modulePropsDumped && mine >= 0x10000) {
                modulePropsDumped = true;
                for (const auto& slot : {std::make_pair(0u, frontRaw), std::make_pair(1u, leftRaw),
                                         std::make_pair(2u, rightRaw)}) {
                    const auto module = static_cast<std::uintptr_t>(slot.second);
                    if (module < 0x10000) {
                        DSH_INFO("weaponprops: the %s weapon has no module actor, so its class cannot be "
                                 "asked",
                                 slot.first == 0 ? "front" : (slot.first == 1 ? "left" : "right"));
                        continue;
                    }
                    std::uint64_t classRaw = 0;
                    dsh::reflect::read_process_bytes(module + 0x0010, &classRaw, sizeof(classRaw));
                    std::string propNote;
                    const auto propLines = dsh::reflect::class_property_values(
                        static_cast<std::uintptr_t>(classRaw), module, 40, propNote);
                    DSH_INFO("weaponprops: %s weapon '%s' [%s] declares %zu own member(s) --"
                             " ClassProperty values are the classes it can spawn",
                             slot.first == 0 ? "front" : (slot.first == 1 ? "left" : "right"),
                             dsh::reflect::object_own_name(module).c_str(),
                             dsh::reflect::object_class_name(module).c_str(), propLines.size());
                    for (const std::string& propLine : propLines) {
                        DSH_INFO("weaponprops: %s", propLine.c_str());
                    }
                    if (!propNote.empty()) DSH_INFO("weaponprops: %s", propNote.c_str());
                }
            }
        }
        session.update(now);
        Sleep(2);
        }
    }
    transport->close();

    g_sessionLive.store(false, std::memory_order_release);
    {
        const bool dispatched = dsh::reflect::run_on_game_thread(
            []() { dsh::reflect::puppet_destroy_all_hosts(); }, 2000);
        if (!dispatched) {
            DSH_WARN("%s teardown: the game thread did not take the puppet destroy within 2000 ms -- the dolls "
                     "are left to the world (a reload in progress destroys them anyway)", rolePrefix);
        }
    }
    dsh::reflect::forget_target_model();

    r.ok = haveLoadout && r.chunksSent > 0 &&
           (manifestDatagrams == 0 || (buildAttempted && modulesMerged > 0));

    r.actorsSpawned = puppetBuilt ? 1 : 0;
    r.machineModulesMerged = modulesMerged;
    if (r.detail.empty()) {

        const std::string ownLoadout =
            "loadout '" + (haveLoadout ? loadout.className : std::string("<none>")) + "' (" +
            std::to_string(loadout.slot_count()) + " of 9 slot(s) filled, FDACustomizeData " +
            (loadout.customizePresent ? "read" : "NOT read");
        const std::string ownVelocity =
            hostMovement.readable
                ? ("last velocity sent (" + std::to_string(hostVelocitySent[0]) + ", " +
                   std::to_string(hostVelocitySent[1]) + ", " + std::to_string(hostVelocitySent[2]) +
                   ") cm/s with MovementMode " + std::to_string(static_cast<unsigned>(hostModeSent)) +
                   " (" + dsh::game::movement_mode_name(hostModeSent) + "), " +
                   std::to_string(velocityDatagramsNonZero) +
                   " datagram(s) carried a non-zero velocity")
                : std::string("no velocity was ever sampled on the local pawn, so 0.0 was sent on every "
                              "datagram -- see the movement SENT lines above for why");
        const std::string ownEnergy =
            haveEnergy
                ? ("last two energy timers sent " +
                   dsh::game::energy_timer_line(hostEnergySent, true, energySentHavePrevious,
                                                energySentPreviousIntermission, std::string()) +
                   ", " + std::to_string(energyDatagramsNonZero) +
                   " datagram(s) carried a pair that was not both zeros -- a zero there means the machine "
                   "never entered the state these two fields describe, not that the pair was unreadable")
                : std::string("the two energy timers were never read on this side, so zeros were sent on "
                              "every datagram -- see the energy timers SENT lines above for the refusal in "
                              "its own words");

        std::string surge;
        {
            std::uint32_t dollsWithComparisons = 0;
            for (const auto& slot : dolls.slots()) {
                if (!slot) continue;
                const DollDiag& diag = doll_diag(slot->peer);
                if (diag.integComparisons == 0) continue;
                ++dollsWithComparisons;
                surge += std::string(dollsWithComparisons == 1 ? "" : "; ") + "peer " +
                         std::to_string(static_cast<unsigned>(slot->peer.value)) + "'s doll moved by more "
                         "than " + std::to_string(kIntegrationMoveThresholdCm) + " cm between two "
                         "placements on " + std::to_string(diag.integMoves) + " of " +
                         std::to_string(diag.integComparisons) + " comparison(s), largest " +
                         std::to_string(diag.integMaxCm) + " cm, last " + std::to_string(diag.integLastCm) +
                         " cm";
            }
            if (dollsWithComparisons == 0) {
                surge = "no doll's own LastUpdateLocation was readable on two consecutive placements, so no "
                        "comparison of a doll's position between two teleports was made";
            }
        }

        std::uint64_t weaponCallsAll = 0;
        std::uint64_t weaponAppliedAll = 0;
        std::uint64_t weaponHoldingAll = 0;
        std::uint64_t weaponFailedAll = 0;
        std::uint64_t weaponNotReadAll = 0;
        for (const auto& weaponSlot : dolls.slots()) {
            if (!weaponSlot) continue;
            const DollDiag& diag = doll_diag(weaponSlot->peer);
            weaponCallsAll += diag.weaponCalls;
            weaponAppliedAll += diag.weaponApplied;
            weaponHoldingAll += diag.weaponHolding;
            weaponFailedAll += diag.weaponFailed;
            weaponNotReadAll += diag.weaponNotRead;
        }

        const std::uint64_t ranSeconds = (GetTickCount64() - runStart) / 1000;

        std::string ranText;
        if (seconds <= 0) {
            ranText = std::to_string(ranSeconds) + " s (no requested duration)";
        } else {
            ranText = std::to_string(ranSeconds) + " s" +
                      (ranSeconds == static_cast<std::uint64_t>(seconds)
                           ? std::string()
                           : (" of the " + std::to_string(seconds) + " s it was asked for"));
        }
        r.detail = std::string(asHost ? "host" : "client") + " ran " + ranText +
                   "; the two directions, this side's own machine first: THIS SIDE sent " +
                   ", " + std::to_string(r.chunksSent) + " appearance chunk(s) carrying " + ownLoadout +
                   " in " + std::to_string(manifestRepeats) + " full pass(es) (" +
                   (manifestAckedPeers.empty() ? std::string("NOT acknowledged by the peer: the repeat "
                                                             "was still running when the session ended")
                                               : std::string("acknowledged"))
                   + "), " + std::to_string(stateDatagrams) + " position datagram(s), " +
                   std::to_string(stanceDatagrams) + " of them carrying stance " +
                   std::to_string(static_cast<unsigned>(stanceSent.currentStance)) + " (" +
                   stance_name(stanceSent.currentStance) + ", DefaultStance " +
                   std::to_string(static_cast<unsigned>(stanceSent.defaultStance)) + "); " + ownVelocity +
                   "; " + ownEnergy + ". THE PEER'S machine, as this side built it: " +
                   std::to_string(manifestDatagrams) + " manifest datagram(s) received carrying loadout '" +
                   (havePuppetLoadout ? puppetLoadout.className : std::string("<none>")) + "', " +
                   (buildAttempted ? ("the game filled " + std::to_string(modulesMerged) +
                                      " of 10 module pointer(s)")
                                   : std::string("no machine was built (no complete manifest arrived)")) +
                   ", " + std::to_string(placementsApplied) + " position update(s) applied and " +
                   std::to_string(placementsDropped) + " not applied (the game thread did not take them in "
                   "time), stance applied " + std::to_string(stanceApplied) + " time(s) (" +
                   std::to_string(stanceChanged) + " a NEW value) and failed " +
                   std::to_string(stanceFailed) + " time(s); " + std::to_string(weaponCallsAll) +
                   " front-weapon call(s) made across every doll here, " + std::to_string(weaponAppliedAll) +
                   " applied, " + std::to_string(weaponHoldingAll) + " already held, " +
                   std::to_string(weaponFailedAll) + " failed, " + std::to_string(weaponNotReadAll) +
                   " datagram(s) whose sender could not read its own pointers; " +
                   std::to_string(velocityApplied) + " velocity write(s) succeeded and " +
                   std::to_string(velocityFailed) + " failed, the component carried the last written "
                   "value in " + std::to_string(velocitySurvivorHits) + " of " +
                   std::to_string(velocitySurvivorHits + velocitySurvivorMisses) +
                   " movement sample(s); " + std::to_string(energyApplied) +
                   std::to_string(energyFailed) + " failed; " +
                   surge + ".";
    }
    DSH_INFO("puppetsession: %s -- %s", r.ok ? "ok" : "no", r.detail.c_str());
    return r;
}

}
