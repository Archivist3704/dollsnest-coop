#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "dsh/game/doll_stance.h"
#include "dsh/game/module_component.h"
#include "dsh/game/movement_state.h"

namespace dsh::game {

using WatchByteReader = StanceByteReader;
using WatchClassChainCheck = bool (*)(std::uintptr_t actor, std::uintptr_t wantedClass,
                                      StanceByteReader read);

using WatchComponentAddress = std::uintptr_t (*)(std::uintptr_t actor, std::uintptr_t& offset,
                                                 std::uintptr_t& other, StanceByteReader read) noexcept;

inline constexpr const char* kWatchApiClass = kMovementApiClass;

inline constexpr const char* kDerivedMovementApiClass = "DACharacterMovementComponent";

inline constexpr std::uintptr_t kWatchControllerOffset = 0x0258;

inline constexpr std::uintptr_t kDerivedOperationalRotationOffset     = 0x0AF0;
inline constexpr std::uintptr_t kDerivedOperationalRotationRateOffset = 0x0B14;
inline constexpr std::uintptr_t kDerivedFlipRotationOffset            = 0x0B20;
inline constexpr std::uintptr_t kDerivedUseOperationalWhenStopped     = 0x0B21;
inline constexpr std::uintptr_t kDerivedMaxAccelerationFallingOffset  = 0x0B24;
inline constexpr std::uintptr_t kDerivedMinHoveringZVelocityOffset    = 0x0B28;
inline constexpr std::uintptr_t kDerivedHoveringZAccelerationOffset   = 0x0B2C;
inline constexpr std::uintptr_t kDerivedMaxAccelerationBoostJumpOffset = 0x0B30;
inline constexpr std::uintptr_t kDerivedMaxAccelerationBoostDashOffset = 0x0B34;
inline constexpr std::uintptr_t kDerivedMaxBoostDashSpeedOffset       = 0x0B38;
inline constexpr std::uintptr_t kDerivedBoostDashImpulseOffset        = 0x0B3C;

inline constexpr std::uintptr_t kDerivedMovementSpanBegin = 0x0AF0;
inline constexpr std::uintptr_t kDerivedMovementSpanEnd   = 0x0B40;

inline constexpr std::size_t kWatchStatusBytes = 0x88;

inline constexpr std::uintptr_t kWatchModulePurgedOffset = kThrusterModulePurgedOffset;
inline constexpr std::uintptr_t kWatchModuleLevelOffset  = kThrusterModuleLevelOffset;
inline constexpr std::uintptr_t kWatchStatusOffset       = kThrusterModuleStatusOffset;

inline constexpr int kWatchRowsPerComponent  = 4;
inline constexpr int kWatchMaxComponents     = 24;

inline constexpr std::size_t kWatchSummaryGoal = 500;

inline constexpr std::size_t kWatchTableLineGoal = 700;

enum class WatchKind : std::uint8_t {
    u8    = 0,
    i32   = 1,
    f32   = 2,
    ptr   = 3,
};

[[nodiscard]] std::size_t watch_kind_bytes(WatchKind kind) noexcept;

enum class WatchGate : std::uint8_t {
    actor          = 0,
    component      = 1,
    compbit2       = 2,
    compbit5       = 3,
    derived        = 4,
    module         = 5,
    moduleComponent = 6,
};

struct WatchRow {
    const char*    name = nullptr;
    std::uintptr_t offset = 0;
    WatchKind      kind = WatchKind::u8;
    WatchGate      gate = WatchGate::actor;
    std::uint8_t   bitIndex = 0xFF;
    int            componentIndex = -1;
};

inline constexpr int kWatchModuleScalarRows = 2;
inline constexpr int kWatchStatusRowCount   = static_cast<int>(kWatchStatusBytes);

#define DSH_WATCH_SCALAR_ROWS(X)                                                                       \
                                            \
    X("bIgnoreFriction", 0x06E0, u8,  actor)                                                  \
    X("bHover",          0x06E1, u8,  actor)                                                  \
    X("bSprint",         0x06E2, u8,  actor)                                                  \
    X("bBoostJump",      0x06F0, u8,  actor)                                                  \
    X("bBoostDash",      0x06F1, u8,  actor)                                                  \
    X("RecentInput[0]",  0x06F4, f32, actor)                                                  \
    X("RecentInput[1]",  0x06F8, f32, actor)                                                  \
    X("RecentInput[2]",  0x06FC, f32, actor)                                                  \
    X("IntermissionTime", 0x0700, f32, actor)                                                 \
    X("FastChargeStart", 0x0704, f32, actor)                                                  \
    X("CurrentStance",   0x10F6, u8,  actor)                                                  \
    X("DefaultStance",   0x10F5, u8,  actor)                                                  \
     \
    X("MovementMode",    0x0168, u8,  component)                                              \
    X("CustomMovementMode", 0x0169, u8, component)                                            \
    X("GravityScale",    0x0150, f32, component)                                              \
    X("MaxWalkSpeed",    0x018C, f32, component)                                              \
    X("MaxAcceleration", 0x01A0, f32, component)                                              \
    X("Velocity[0]",     0x00C4, f32, component)                                              \
    X("Velocity[1]",     0x00C8, f32, component)                                              \
    X("Velocity[2]",     0x00CC, f32, component)                                              \
    X("Acceleration[0]", 0x022C, f32, component)                                              \
    X("Acceleration[1]", 0x0230, f32, component)                                              \
    X("Acceleration[2]", 0x0234, f32, component)                                              \
                                 \
    X("OperationalRotation[0]", 0x0AF0, f32, derived)                                         \
    X("OperationalRotation[1]", 0x0AF4, f32, derived)                                         \
    X("OperationalRotation[2]", 0x0AF8, f32, derived)                                         \
    X("OperationalRotationRate[0]", 0x0B14, f32, derived)                                     \
    X("OperationalRotationRate[1]", 0x0B18, f32, derived)                                     \
    X("OperationalRotationRate[2]", 0x0B1C, f32, derived)                                     \
    X("bFlipRotationToMovement", 0x0B20, u8, derived)                                         \
    X("bUseOperationalWhenStopped", 0x0B21, u8, derived)                                      \
    X("MaxAccelerationFalling", 0x0B24, f32, derived)                                         \
    X("MinHoveringZVelocity", 0x0B28, f32, derived)                                           \
    X("HoveringZAcceleration", 0x0B2C, f32, derived)                                          \
    X("MaxAccelerationBoostJump", 0x0B30, f32, derived)                                       \
    X("MaxAccelerationBoostDash", 0x0B34, f32, derived)                                       \
    X("MaxBoostDashSpeed", 0x0B38, f32, derived)                                              \
    X("BoostDashImpulse", 0x0B3C, f32, derived)

#define DSH_WATCH_SCALAR_COUNT_ONE(NAME, OFFSET, KIND, OWNER) +1
inline constexpr int kWatchScalarRowCount = 0 DSH_WATCH_SCALAR_ROWS(DSH_WATCH_SCALAR_COUNT_ONE);
#undef DSH_WATCH_SCALAR_COUNT_ONE

struct WatchComponentField {
    const char*    label = nullptr;
    std::uintptr_t offset = 0;
    WatchKind      kind = WatchKind::u8;
    std::uint8_t   bitIndex = 0xFF;
};
inline constexpr WatchComponentField kWatchComponentFields[kWatchRowsPerComponent] = {
    {"bAutoActivate(:7)", 0x0089, WatchKind::u8,  7   },
    {"bIsActive(:0)",     0x008A, WatchKind::u8,  0   },
    {"Asset",           0x0450, WatchKind::ptr, 0xFF},
    {" flags",            0x0558, WatchKind::u8,  0xFF},
};

struct WatchSlot {
    std::string    name;
    std::uintptr_t address = 0;
    WatchKind      kind = WatchKind::u8;
    std::uint8_t   bitIndex = 0xFF;
    WatchGate      gate = WatchGate::actor;

    std::uintptr_t offsetAbsolute = 0;

    std::uintptr_t offsetInComponent = 0;
    int            componentIndex = -1;
    int            componentField = -1;
    bool           available = false;
    std::uint64_t  previous = 0;
    std::uint64_t  changes = 0;
    std::uint64_t  firstDatagram = 0;
    std::uint64_t  firstMillis = 0;
};

struct WatchModuleKey {
    std::uintptr_t module = 0;
    std::uintptr_t firstComponent = 0;
    int            componentCount = 0;
    bool           usable = false;

    std::vector<std::string> componentNames;

    [[nodiscard]] bool operator==(const WatchModuleKey& other) const noexcept {
        return module == other.module && firstComponent == other.firstComponent &&
               componentCount == other.componentCount && usable == other.usable &&
               componentNames == other.componentNames;
    }
    [[nodiscard]] bool operator!=(const WatchModuleKey& other) const noexcept { return !(*this == other); }
};

struct ObservedWatch {
    std::vector<WatchSlot> rows;

    std::uintptr_t machine = 0;
    std::uintptr_t component = 0;
    std::uintptr_t derived = 0;
    bool           characterOk = false;
    bool           componentOk = false;
    bool           derivedOk = false;
    bool           moduleOk = false;

    WatchByteReader reader = nullptr;

    WatchModuleKey moduleKey;
    std::uint64_t  steps = 0;
    std::uint64_t  changes = 0;
    std::uint64_t  statusChanges = 0;
    std::uint64_t  statusBytesMoved = 0;

    std::string    summaryRows;
    bool           summaryRowsTruncated = false;

    std::vector<std::string> tableLines;

    std::uint64_t  traced = 0;

    std::uint64_t  bytesRead = 0;
    std::uint64_t  sweeps = 0;

    int            moduleRows = 0;
    int            derivedRows = 0;
    int            componentRows = 0;
};

[[nodiscard]] int watch_row_count(int componentCount) noexcept;
[[nodiscard]] int watch_component_count(int componentCount) noexcept;

[[nodiscard]] const WatchRow* watch_scalar_rows() noexcept;

[[nodiscard]] int watch_scalar_row_index_of(std::string_view name) noexcept;

struct WatchModuleSample {
    std::uintptr_t module = 0;
    bool           usable = false;
    bool           purgedRead = false;
    std::uint8_t   purged = 0;
    bool           levelRead = false;
    std::int32_t   level = 0;
    bool           statusRead = false;
    std::uint8_t   status[kWatchStatusBytes] = {};

    struct Component {
        std::uintptr_t address = 0;
        std::string    name;
        bool           activationRead = false;
        std::uint8_t   autoActivate = 0;
        std::uint8_t   isActive = 0;
        bool           niagara = false;
        bool           assetRead = false;
        std::uintptr_t asset = 0;
        bool           niagaraFlagsRead = false;
        std::uint8_t   niagaraFlags = 0;
    };
    std::vector<Component> components;
};

struct WatchChange {
    std::string_view name;
    WatchKind        kind = WatchKind::u8;
    std::uint64_t    oldValue = 0;
    std::uint64_t    newValue = 0;
    std::uint64_t    datagram = 0;
    std::uint64_t    millis = 0;
};

using WatchByteReader = StanceByteReader;
using WatchClassChainCheck = bool (*)(std::uintptr_t actor, std::uintptr_t wantedClass,
                                      StanceByteReader read);
using WatchComponentAddress = std::uintptr_t (*)(std::uintptr_t actor, std::uintptr_t& offset,
                                                 std::uintptr_t& other, StanceByteReader read) noexcept;

int watch_begin(ObservedWatch& watch, std::uintptr_t machine, std::uintptr_t characterClass,
                              std::uintptr_t derivedClass, const WatchModuleKey& moduleKey,
                              WatchByteReader read, WatchClassChainCheck chainReaches,
                              WatchComponentAddress resolver);

[[nodiscard]] std::size_t watch_step(ObservedWatch& watch, const WatchModuleSample& module,
                                     std::uint64_t datagram, std::uint64_t millis,
                                     std::vector<WatchChange>& trace);

[[nodiscard]] std::string watch_summary_line(const ObservedWatch& watch, const std::string& refusal);

[[nodiscard]] std::vector<std::string> watch_table_lines(const ObservedWatch& watch);

[[nodiscard]] const char* watch_kind_name(WatchKind kind) noexcept;
[[nodiscard]] std::string watch_value_text(WatchKind kind, std::uint64_t bits);

[[nodiscard]] std::string watch_change_line(const WatchChange& change);

inline constexpr std::uint64_t kWatchMaxTracedChanges = 400;

}
