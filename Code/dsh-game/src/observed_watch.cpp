#include "dsh/game/observed_watch.h"

#include <cstdio>
#include <cstring>

namespace dsh::game {
namespace {

bool read_bits(WatchByteReader read, std::uintptr_t address, std::size_t bytes, std::uint64_t& out) {
    out = 0;
    if (read == nullptr || address < 0x10000) return false;
    if (bytes == 0 || bytes > 8) return false;
    std::uint8_t buffer[8] = {};
    if (!read(address, buffer, bytes)) return false;
    std::uint64_t value = 0;
    std::memcpy(&value, buffer, bytes);
    out = value;
    return true;
}

std::uint64_t mask_bit(std::uint64_t value, std::uint8_t bitIndex) {
    if (bitIndex == 0xFF) return value;
    return (value >> bitIndex) & 0x1u;
}

}

namespace {
constexpr WatchRow make_scalar_row(const char* name, std::uintptr_t offset, WatchKind kind,
                                   WatchGate gate) noexcept {
    WatchRow row;
    row.name = name;
    row.offset = offset;
    row.kind = kind;
    row.gate = gate;
    row.bitIndex = 0xFF;
    row.componentIndex = -1;
    return row;
}
}

#define DSH_WATCH_SCALAR_MAKE(NAME, OFFSET, KIND, GATE)                                                \
    make_scalar_row(NAME, OFFSET, WatchKind::KIND, WatchGate::GATE),
namespace {
constexpr WatchRow kWatchScalarRows[kWatchScalarRowCount] = {
    DSH_WATCH_SCALAR_ROWS(DSH_WATCH_SCALAR_MAKE)
};
}
#undef DSH_WATCH_SCALAR_MAKE

const WatchRow* watch_scalar_rows() noexcept { return kWatchScalarRows; }

int watch_scalar_row_index_of(std::string_view name) noexcept {
    for (int i = 0; i < kWatchScalarRowCount; ++i) {
        if (name == kWatchScalarRows[i].name) return i;
    }
    return -1;
}

std::size_t watch_kind_bytes(WatchKind kind) noexcept {
    switch (kind) {
        case WatchKind::u8:  return 1;
        case WatchKind::i32: return 4;
        case WatchKind::f32: return 4;
        case WatchKind::ptr: return 8;
    }
    return 1;
}

const char* watch_kind_name(WatchKind kind) noexcept {
    switch (kind) {
        case WatchKind::u8:  return "u8";
        case WatchKind::i32: return "i32";
        case WatchKind::f32: return "f32";
        case WatchKind::ptr: return "ptr";
    }
    return "?";
}

std::string watch_value_text(WatchKind kind, std::uint64_t bits) {
    char buf[48];
    switch (kind) {
        case WatchKind::u8:
            std::snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(bits & 0xFFu));
            return buf;
        case WatchKind::i32:
            std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(static_cast<std::int32_t>(bits)));
            return buf;
        case WatchKind::f32: {
            float value = 0.0f;
            const std::uint32_t raw = static_cast<std::uint32_t>(bits & 0xFFFFFFFFu);
            std::memcpy(&value, &raw, sizeof(value));
            std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(value));
            return buf;
        }
        case WatchKind::ptr:
            std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(bits));
            return buf;
    }
    return "?";
}

int watch_component_count(int componentCount) noexcept {
    if (componentCount <= 0) return 0;
    return componentCount < kWatchMaxComponents ? componentCount : kWatchMaxComponents;
}

int watch_row_count(int componentCount) noexcept {
    return kWatchScalarRowCount + kWatchModuleScalarRows + kWatchStatusRowCount +
           watch_component_count(componentCount) * kWatchRowsPerComponent;
}

namespace {
std::string module_row_name(const char* label, std::uintptr_t offset) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%s(%04llX)", label, static_cast<unsigned long long>(offset));
    return buf;
}

std::string status_row_name(std::size_t byteIndex) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "ModuleStatus[%zu]", byteIndex);
    return buf;
}

std::string component_row_name(const std::string& component, const char* field) {
    return component + "." + field;
}

std::string compose_dense(const std::vector<WatchSlot>& rows, std::size_t goal, bool& truncated,
                          const char* statusEntry) {
    std::string out;
    truncated = false;
    std::size_t statusRun = 0;
    for (const WatchSlot& row : rows) {
        if (row.gate == WatchGate::module && row.offsetAbsolute >= kWatchStatusOffset) {

            if (statusRun++ != 0) continue;
            if (!out.empty()) out += ' ';
            out += statusEntry;
            continue;
        }
        const std::size_t extra = out.empty() ? row.name.size() : row.name.size() + 1;
        if (out.size() + extra > goal) {
            truncated = true;
            break;
        }
        if (!out.empty()) out += ' ';
        out += row.name;
    }
    return out;
}

std::vector<std::string> compose_table(const std::vector<WatchSlot>& rows, std::size_t goal) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t statusRun = 0;
    int lastComponent = -2;
    for (const WatchSlot& row : rows) {
        std::string piece;
        if (row.gate == WatchGate::module && row.offsetAbsolute >= kWatchStatusOffset) {
            if (statusRun++ != 0) continue;
            piece = "ModuleStatus(.., one row per byte)";
        } else if (row.gate == WatchGate::moduleComponent) {
            if (row.componentIndex == lastComponent) continue;
            lastComponent = row.componentIndex;

            const std::size_t dot = row.name.find('.');
            piece = (dot == std::string::npos ? row.name : row.name.substr(0, dot)) + ".(4 rows)";
        } else {
            piece = row.name;
        }
        const std::size_t extra = current.empty() ? piece.size() : piece.size() + 1;
        if (!current.empty() && current.size() + extra > goal) {
            lines.push_back(current);
            current.clear();
        }
        if (!current.empty()) current += ' ';
        current += piece;
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}
}

int watch_begin(ObservedWatch& watch, std::uintptr_t machine, std::uintptr_t characterClass,
                std::uintptr_t derivedClass, const WatchModuleKey& moduleKey, WatchByteReader read,
                WatchClassChainCheck chainReaches, WatchComponentAddress resolver) {
    watch.rows.clear();
    watch.machine = machine;
    watch.component = 0;
    watch.derived = 0;
    watch.characterOk = false;
    watch.componentOk = false;
    watch.derivedOk = false;
    watch.moduleOk = false;
    watch.reader = read;
    watch.moduleKey = moduleKey;
    watch.moduleRows = 0;
    watch.derivedRows = 0;
    watch.componentRows = 0;

    const int components = watch_component_count(moduleKey.componentCount);
    watch.rows.reserve(static_cast<std::size_t>(watch_row_count(moduleKey.componentCount)));

    if (machine >= 0x10000 && read != nullptr && chainReaches != nullptr && characterClass != 0) {
        watch.characterOk = chainReaches(machine, characterClass, read);
    }
    if (watch.characterOk && resolver != nullptr) {
        std::uintptr_t chosenOffset = 0;
        std::uintptr_t other = 0;
        watch.component = resolver(machine, chosenOffset, other, read);
        watch.componentOk = watch.component >= 0x10000;
    }
    if (watch.componentOk && derivedClass != 0 && chainReaches != nullptr) {

        watch.derivedOk = chainReaches(watch.component, derivedClass, read);
        if (watch.derivedOk) watch.derived = watch.component;
    }

    for (int i = 0; i < kWatchScalarRowCount; ++i) {
        const WatchRow& row = kWatchScalarRows[i];
        WatchSlot slot;
        slot.name = row.name;
        slot.kind = row.kind;
        slot.bitIndex = row.bitIndex;
        slot.gate = row.gate;
        slot.offsetAbsolute = row.offset;
        switch (row.gate) {
            case WatchGate::actor:
                slot.address = machine + row.offset;
                break;
            case WatchGate::component:
            case WatchGate::compbit2:
            case WatchGate::compbit5:
                slot.address = watch.componentOk ? watch.component + row.offset : 0;
                break;
            case WatchGate::derived:
                slot.address = watch.derivedOk ? watch.derived + row.offset : 0;
                ++watch.derivedRows;
                break;
            default:
                slot.address = 0;
                break;
        }
        watch.rows.push_back(std::move(slot));
    }

    {
        const std::uintptr_t module = moduleKey.module;
        const bool haveModule = module >= 0x10000;
        watch.moduleOk = haveModule;

        WatchSlot purged;
        purged.name = module_row_name("bPurged", kWatchModulePurgedOffset);
        purged.kind = WatchKind::u8;
        purged.gate = WatchGate::module;
        purged.offsetAbsolute = kWatchModulePurgedOffset;
        purged.address = haveModule ? module + kWatchModulePurgedOffset : 0;
        watch.rows.push_back(std::move(purged));

        WatchSlot level;
        level.name = module_row_name("ModuleLevel", kWatchModuleLevelOffset);
        level.kind = WatchKind::i32;
        level.gate = WatchGate::module;
        level.offsetAbsolute = kWatchModuleLevelOffset;
        level.address = haveModule ? module + kWatchModuleLevelOffset : 0;
        watch.rows.push_back(std::move(level));

        for (std::size_t b = 0; b < kWatchStatusBytes; ++b) {
            WatchSlot status;
            status.name = status_row_name(b);
            status.kind = WatchKind::u8;
            status.gate = WatchGate::module;
            status.offsetAbsolute = kWatchStatusOffset + b;
            status.address = haveModule ? module + kWatchStatusOffset + b : 0;
            watch.rows.push_back(std::move(status));
        }
        watch.moduleRows = kWatchModuleScalarRows + kWatchStatusRowCount;
    }

    for (int c = 0; c < components; ++c) {
        const std::string component =
            (c < static_cast<int>(moduleKey.componentNames.size()) &&
             !moduleKey.componentNames[static_cast<std::size_t>(c)].empty())
                ? moduleKey.componentNames[static_cast<std::size_t>(c)]
                : ("component#" + std::to_string(c));
        for (int f = 0; f < kWatchRowsPerComponent; ++f) {
            WatchSlot slot;
            slot.name = component_row_name(component, kWatchComponentFields[f].label);
            slot.kind = kWatchComponentFields[f].kind;
            slot.bitIndex = kWatchComponentFields[f].bitIndex;
            slot.gate = WatchGate::moduleComponent;
            slot.offsetInComponent = kWatchComponentFields[f].offset;
            slot.componentIndex = c;
            slot.componentField = f;
            watch.rows.push_back(std::move(slot));
        }
    }
    watch.componentRows = components * kWatchRowsPerComponent;

    watch.summaryRows = compose_dense(watch.rows, kWatchSummaryGoal, watch.summaryRowsTruncated,
                                      "ModuleStatus(..)");
    watch.tableLines = compose_table(watch.rows, kWatchTableLineGoal);
    return static_cast<int>(watch.rows.size());
}

std::size_t watch_step(ObservedWatch& watch, const WatchModuleSample& module, std::uint64_t datagram,
                       std::uint64_t millis, std::vector<WatchChange>& trace) {

    const bool moduleUsable = module.usable && module.module >= 0x10000;
    watch.moduleOk = watch.moduleKey.module >= 0x10000 || moduleUsable;
    const std::size_t componentCount =
        moduleUsable ? (module.components.size() < static_cast<std::size_t>(kWatchMaxComponents)
                            ? module.components.size()
                            : static_cast<std::size_t>(kWatchMaxComponents))
                     : 0;

    if (moduleUsable) {
        for (WatchSlot& row : watch.rows) {
            if (row.gate == WatchGate::module && row.address == 0) {
                row.address = module.module + row.offsetAbsolute;
            }
            if (row.gate == WatchGate::moduleComponent && row.componentIndex >= 0 &&
                static_cast<std::size_t>(row.componentIndex) < componentCount) {
                const WatchModuleSample::Component& component =
                    module.components[static_cast<std::size_t>(row.componentIndex)];
                if (component.address >= 0x10000) {
                    row.address = component.address + row.offsetInComponent;
                }
            }
        }
    }

    std::size_t moved = 0;
    for (WatchSlot& row : watch.rows) {

        bool watched = true;
        switch (row.gate) {
            case WatchGate::actor:
                watched = watch.characterOk;
                break;
            case WatchGate::component:
            case WatchGate::compbit2:
            case WatchGate::compbit5:
                watched = watch.componentOk;
                break;
            case WatchGate::derived:
                watched = watch.derivedOk;
                break;
            case WatchGate::module:

                watched = moduleUsable &&
                          (row.offsetAbsolute < kWatchModuleLevelOffset
                               ? module.purgedRead
                               : (row.offsetAbsolute < kWatchStatusOffset ? module.levelRead
                                                                          : module.statusRead));
                break;
            case WatchGate::moduleComponent: {
                if (!moduleUsable || row.componentIndex < 0 ||
                    static_cast<std::size_t>(row.componentIndex) >= componentCount) {
                    watched = false;
                    break;
                }
                const WatchModuleSample::Component& component =
                    module.components[static_cast<std::size_t>(row.componentIndex)];
                switch (row.componentField) {
                    case 0: watched = component.activationRead; break;
                    case 1: watched = component.activationRead; break;

                    case 2: watched = component.niagara && component.assetRead; break;
                    case 3: watched = component.niagaraFlagsRead; break;
                    default: watched = false; break;
                }
                break;
            }
        }
        if (!watched || row.address < 0x10000) {
            row.available = false;
            continue;
        }

        std::uint64_t value = 0;
        if (!read_bits(watch.reader, row.address, watch_kind_bytes(row.kind), value)) {
            row.available = false;
            continue;
        }

        watch.bytesRead += watch_kind_bytes(row.kind);
        value = mask_bit(value, row.bitIndex);

        if (!row.available) {

            row.available = true;
            row.previous = value;
            continue;
        }
        if (value == row.previous) continue;

        ++moved;
        ++watch.changes;
        ++row.changes;
        if (row.changes == 1) {
            row.firstDatagram = datagram;
            row.firstMillis = millis;
        }
        if (row.gate == WatchGate::module && row.offsetAbsolute >= kWatchStatusOffset) {
            ++watch.statusChanges;
            ++watch.statusBytesMoved;
        }
        if (watch.traced < kWatchMaxTracedChanges) {
            WatchChange change;
            change.name = row.name;
            change.kind = row.kind;
            change.oldValue = row.previous;
            change.newValue = value;
            change.datagram = datagram;
            change.millis = millis;
            trace.push_back(change);
            ++watch.traced;
        }
        row.previous = value;
    }

    ++watch.steps;
    ++watch.sweeps;
    return moved;
}

std::string watch_summary_line(const ObservedWatch& watch, const std::string& refusal) {
    std::string out;
    const int components = watch_component_count(watch.moduleKey.componentCount);

    if (!refusal.empty()) {

        out = "watch: NOT started -- ";
        out += refusal;
        return out;
    }

    char head[320];
    std::snprintf(head, sizeof(head),
                  "watch: %llu datagram(s) swept, %llu row move(s) seen (%llu of them a ModuleStatus byte); "
                  "%d row(s) watched (%d fixed, %d module, %d component over %d component(s)); %llu guarded "
                  "byte(s) read over %llu sweep(s) = %.1f byte(s) per datagram",
                  static_cast<unsigned long long>(watch.steps),
                  static_cast<unsigned long long>(watch.changes),
                  static_cast<unsigned long long>(watch.statusChanges),
                  static_cast<int>(watch.rows.size()), kWatchScalarRowCount, watch.moduleRows,
                  watch.componentRows, components,
                  static_cast<unsigned long long>(watch.bytesRead),
                  static_cast<unsigned long long>(watch.sweeps),
                  watch.sweeps == 0 ? 0.0 : static_cast<double>(watch.bytesRead) /
                                                static_cast<double>(watch.sweeps));
    out = head;

    if (!watch.characterOk) {
        out += "; the character chain check REFUSED (or no DACharacter UClass was resolved), so NOTHING "
               "was read from the actor or from its movement component -- and that is a refusal, not a "
               "table of zeros";
    } else if (!watch.componentOk) {
        out += "; the actor's class chain reached DACharacter but NEITHER movement component pointer "
               " is set, so the component's fields were never read";
    }
    if (watch.componentOk && !watch.derivedOk) {
        out += "; the movement component's chain does NOT reach DACharacterMovementComponent, so the "
               " block was NEVER read as that class's fields";
    }
    if (watch.moduleKey.module < 0x10000) {
        out += "; the thruster module rows exist but were NOT opened: no walk has named a module on this "
               "side yet";
    }

    out += "; every row below is swept once per datagram on the send path. THE WATCHED TABLE IS: ";
    out += watch.summaryRows.empty() ? std::string("<empty>") : watch.summaryRows;
    if (watch.summaryRowsTruncated) {
        out += " ...[LIST TRUNCATED HERE -- the rows that do not fit are named IN FULL on the "
               "once-per-session table line, and each is named again by the trace when it moves]";
    }
    if (watch.traced >= kWatchMaxTracedChanges) {
        out += "; the per-move lines stopped after ";
        out += std::to_string(static_cast<unsigned long long>(kWatchMaxTracedChanges));
        out += " of them, so the counters above are the only complete record of this session's moves";
    }
    return out;
}

std::vector<std::string> watch_table_lines(const ObservedWatch& watch) {
    static const char* const kFirstPrefix = "watch table: ";
    static const char* const kNextPrefix = "watch table (continued): ";
    std::vector<std::string> out;
    if (watch.tableLines.empty()) {
        out.push_back("watch table: <no rows were built>");
        return out;
    }
    for (std::size_t i = 0; i < watch.tableLines.size(); ++i) {
        if (i == 0) {
            out.push_back(std::string(kFirstPrefix) + watch.tableLines[i] +
                          " (a component's `.(4 rows)` is its activation pair at:7/:0 and, "
                          "if the walk classified it as a Niagara component, Asset and the "
                          " flags; every row here is swept once per datagram on the send path)");
        } else {
            out.push_back(std::string(kNextPrefix) + watch.tableLines[i]);
        }
    }
    return out;
}

std::string watch_change_line(const WatchChange& change) {
    char head[96];
    std::snprintf(head, sizeof(head), " dg=%llu (+%llums) ",
                  static_cast<unsigned long long>(change.datagram),
                  static_cast<unsigned long long>(change.millis));
    std::string out = "WATCH MOVE:";
    out += head;
    out += change.name.empty() ? std::string("<unnamed row>") : std::string(change.name);
    out += " changed from ";
    out += watch_value_text(change.kind, change.oldValue);
    out += " to ";
    out += watch_value_text(change.kind, change.newValue);
    return out;
}

}
