#include "dsh/reflect/world.h"

#include "dsh/platform/log.h"
#include "dsh/reflect/scanner.h"
#include "dsh/reflect/offsets.h"
#include "dsh/reflect/spawn.h"

#include <windows.h>

#include <cstdio>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <string>

namespace dsh::reflect {
namespace {

std::once_flag g_appendStringOnce;
using AppendStringFn = void (*)(const void* name, void* outFString);
AppendStringFn g_appendString = nullptr;

struct FStringRaw {
    wchar_t* data = nullptr;
    std::int32_t num = 0;
    std::int32_t max = 0;
};

void init_append_string() {
    const ModuleInfo exe = main_module();
    if (exe.base == 0) return;
    g_appendString = reinterpret_cast<AppendStringFn>(
        exe.base + static_cast<std::uintptr_t>(offsets::kAppendString));
    DSH_INFO("world: FName::AppendString at 0x%llX (RVA 0x%X)",
             static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(g_appendString)),
             offsets::kAppendString);
}

std::string narrow(const wchar_t* w, int count) {
    if (!w || count <= 0) return {};
    std::string out;
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const wchar_t c = w[i];

        if (c == 0) break;
        out.push_back((c > 0 && c < 128) ? static_cast<char>(c) : '?');
    }
    return out;
}

bool call_append_string_guarded(AppendStringFn fn, const void* name, FStringRaw& out) {
    out.data = nullptr;
    out.num = 0;
    out.max = 0;
    __try {
        fn(name, &out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.data = nullptr;
        out.num = 0;
        return false;
    }
    return out.data != nullptr && out.num > 0 && out.num < 512;
}

}

bool resolve_fname(std::uint32_t index, std::string& out, std::string& note) {
    std::call_once(g_appendStringOnce, init_append_string);
    if (!g_appendString) {
        note = "AppendString unresolved";
        return false;
    }

    std::uint64_t nameValue = index;
    FStringRaw result;
    if (!call_append_string_guarded(g_appendString, &nameValue, result)) {
        note = "AppendString produced nothing usable";
        return false;
    }
    out = narrow(result.data, result.num);
    return !out.empty();
}

namespace {

std::uintptr_t pawn_from_controller(std::uintptr_t controller);

}

struct FoundObjects {
    std::uintptr_t world = 0;
    std::string    worldName;
    std::uint32_t  worldCount = 0;
    std::uintptr_t playerController = 0;
    std::string    playerControllerClass;
    std::uint32_t  controllerCount = 0;
    std::uint32_t  pawnCount = 0;
    std::uint32_t  scanned = 0;
    bool           controllerIsGameClass = false;
    int            controllerScore = 0;
    bool           controllerHasPawn = false;

    std::vector<std::string> worldSamples;

    std::vector<std::uintptr_t> worldAddresses;

    std::uintptr_t engine = 0;

    bool hasIpNetDriverClass = false;
    bool hasSteamNetDriverClass = false;
    std::uintptr_t liveWorld = 0;

    std::vector<std::uintptr_t> netDrivers;
    std::vector<std::uintptr_t> pendingNetGames;
};

[[nodiscard]] std::string class_name_of(std::uintptr_t obj,
                                        std::unordered_map<std::uintptr_t, std::string>& cache) {
    using namespace offsets;
    std::uint64_t cls = 0;
    if (!try_read_u64(obj + kUObjectClassOffset, cls) || cls < 0x10000) return {};
    const auto clsAddr = static_cast<std::uintptr_t>(cls);

    const auto cached = cache.find(clsAddr);
    if (cached != cache.end()) return cached->second;

    std::string name;
    std::uint64_t nameField = 0;
    if (try_read_u64(clsAddr + kUObjectNameOffset, nameField)) {
        std::string note;
        resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), name, note);
    }
    cache.emplace(clsAddr, name);
    return name;
}

[[nodiscard]] std::string object_name_of(std::uintptr_t obj) {
    using namespace offsets;
    std::string name;
    std::uint64_t nameField = 0;
    if (try_read_u64(obj + kUObjectNameOffset, nameField)) {
        std::string note;
        resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), name, note);
    }
    return name;
}

FoundObjects find_named_objects() {
    DSH_TIME_WALK("find_named_objects");
    using namespace offsets;
    FoundObjects found;

    const ModuleInfo exe = main_module();
    if (exe.base == 0) return found;
    const auto array = exe.base + static_cast<std::uintptr_t>(kGObjects);

    std::uint64_t chunkTable = 0;
    std::uint32_t numElements = 0;
    if (!try_read_u64(array + kObjectsOffset, chunkTable) || chunkTable < 0x10000) return found;
    if (!try_read_u32(array + kNumElementsOffset, numElements)) return found;
    if (numElements > 5'000'000) return found;

    std::unordered_map<std::uintptr_t, std::string> classNames;

    for (std::uint32_t i = 0; i < numElements; ++i) {
        const std::uint32_t chunkIndex = i / kElementsPerChunk;
        const std::uint32_t within = i % kElementsPerChunk;
        std::uint64_t chunk = 0;
        if (!try_read_u64(static_cast<std::uintptr_t>(chunkTable) +
                              static_cast<std::uintptr_t>(chunkIndex) * 8, chunk) ||
            chunk < 0x10000) {
            continue;
        }
        std::uint64_t obj = 0;
        if (!try_read_u64(static_cast<std::uintptr_t>(chunk) +
                              static_cast<std::uintptr_t>(within) * kObjectItemStride, obj) ||
            obj < 0x10000) {
            continue;
        }
        ++found.scanned;

        const auto objAddr = static_cast<std::uintptr_t>(obj);
        const std::string className = class_name_of(objAddr, classNames);
        if (className.empty()) continue;

        const std::string objName = object_name_of(objAddr);

        const bool isCdo = objName.rfind("Default__", 0) == 0;

        if (className == "World") {
            ++found.worldCount;
            found.worldAddresses.push_back(objAddr);
            std::uint64_t level = 0;
            const bool hasLevel = try_read_u64(objAddr + world_offsets::kPersistentLevel, level) &&
                                  level >= 0x10000;
            if (!isCdo && hasLevel && found.world == 0) {
                found.world = objAddr;
                found.worldName = objName;
            }
            if (found.worldSamples.size() < 6) {
                found.worldSamples.push_back(objName + (hasLevel ? " [has level]" : " [no level]") +
                                             (isCdo ? " (CDO)" : ""));
            }
        }

        if (className == "GameEngine" && !isCdo && found.engine == 0) {
            found.engine = objAddr;
        }

        if (className == "Class") {
            if (objName == "IpNetDriver") found.hasIpNetDriverClass = true;
            else if (objName == "SteamNetDriver") found.hasSteamNetDriverClass = true;
        }

        if (className.find("NetDriver") != std::string::npos && !isCdo) {
            found.netDrivers.push_back(objAddr);
        }
        if (className == "PendingNetGame" && !isCdo) {

            found.pendingNetGames.push_back(objAddr);
        }
        if (className == "World" && !isCdo && found.liveWorld == 0) {
            std::uint64_t level = 0;
            if (try_read_u64(objAddr + world_offsets::kPersistentLevel, level) && level >= 0x10000) {
                found.liveWorld = objAddr;
            }
        }
        if (className.find("PlayerController") != std::string::npos) {            ++found.controllerCount;

            const bool gameClass = className.rfind("DA", 0) == 0;
            const bool hasPawn = !isCdo && pawn_from_controller(objAddr) != 0;
            const int score = (hasPawn ? 2 : 0) + (gameClass ? 1 : 0);
            const bool better = found.playerController == 0 || score > found.controllerScore;
            if (!isCdo && better) {
                found.playerController = objAddr;
                found.playerControllerClass = className;
                found.controllerIsGameClass = gameClass;
                found.controllerScore = score;
                found.controllerHasPawn = hasPawn;
            }
        }

        if (className.find("Pawn") != std::string::npos && className.find("Component") == std::string::npos &&
            className.find("Movement") == std::string::npos) {
            ++found.pawnCount;
        }
    }

    return found;
}

WorldSurvey survey_worlds() {
    DSH_TIME_WALK("survey_worlds");
    WorldSurvey s;
    const FoundObjects found = find_named_objects();
    s.scanned = found.scanned;

    const auto class_of = [](std::uintptr_t object) -> std::string {
        std::uint64_t klass = 0;
        if (!try_read_u64(object + offsets::kUObjectClassOffset, klass) || klass < 0x10000) return {};
        return object_name_of(static_cast<std::uintptr_t>(klass));
    };
    const auto read_driver = [&class_of](std::uintptr_t address, const std::string& ownerClass) {
        NetObjectReading r;
        r.address = address;
        r.className = ownerClass;
        r.connectionClass = [&]() {
            std::uint64_t connectionClass = 0;
            if (!try_read_u64(address + 0x0170, connectionClass) || connectionClass < 0x10000) return std::string();
            std::uint64_t klass = 0;
            if (!try_read_u64(static_cast<std::uintptr_t>(connectionClass) + offsets::kUObjectNameOffset,
                              klass)) {
                return std::string();
            }
            std::string text;
            std::string note;
            if (!resolve_fname(static_cast<std::uint32_t>(klass & 0xFFFFFFFFu), text, note)) return std::string();
            return text;
        }();
        std::uint64_t clients = 0;
        std::uint32_t clientCount = 0;
        if (try_read_u64(address + 0x0090, clients) && clients >= 0x10000) {
            try_read_u32(address + 0x0090 + 8, clientCount);
        }
        r.clientConnections = clientCount;
        std::uint64_t server = 0;
        if (try_read_u64(address + 0x0088, server) && server >= 0x10000) r.serverConnection = static_cast<std::uintptr_t>(server);
        std::uint32_t timeoutWord = 0;
        if (try_read_u32(address + 0x007C, timeoutWord)) {
            float timeout = 0.0f;
            std::memcpy(&timeout, &timeoutWord, sizeof(timeout));
            r.connectionTimeout = timeout;
        }
        return r;
    };
    for (const std::uintptr_t driver : found.netDrivers) {
        NetObjectReading r = read_driver(driver, class_of(driver));
        s.drivers.push_back(std::move(r));
    }
    for (const std::uintptr_t pending : found.pendingNetGames) {
        NetObjectReading r;
        r.address = pending;
        r.className = "PendingNetGame";
        std::uint64_t driver = 0;
        if (try_read_u64(pending + 0x0030, driver) && driver >= 0x10000) {
            r.driver = static_cast<std::uintptr_t>(driver);
            r.driverClass = class_of(r.driver);
            NetObjectReading d = read_driver(r.driver, r.driverClass);
            r.connectionClass = d.connectionClass;
            r.clientConnections = d.clientConnections;
            r.serverConnection = d.serverConnection;
            r.connectionTimeout = d.connectionTimeout;
        }
        s.pending.push_back(std::move(r));
    }
    for (const std::uintptr_t address : found.worldAddresses) {
        WorldSample w;
        w.address = address;
        w.name = object_name_of(address);
        std::uint64_t level = 0;
        if (try_read_u64(address + world_offsets::kPersistentLevel, level) && level >= 0x10000) {
            w.persistentLevel = static_cast<std::uintptr_t>(level);
        }
        std::uint64_t driver = 0;
        if (try_read_u64(address + world_offsets::kNetDriver, driver)) {
            w.rawNetDriver = driver;
            if (driver >= 0x10000) w.netDriver = static_cast<std::uintptr_t>(driver);
        }
        std::uint64_t gameMode = 0;
        if (try_read_u64(address + world_offsets::kAuthorityGameMode, gameMode) && gameMode >= 0x10000) {
            w.authorityGameMode = static_cast<std::uintptr_t>(gameMode);
        }
        s.worlds.push_back(std::move(w));
    }
    return s;
}

std::string WorldSurvey::line() const {
    std::string out;
    for (const NetObjectReading& d : drivers) {
        char buf[300];
        std::snprintf(buf, sizeof(buf),
                      "%sDRIVER 0x%llX '%s' connectionClass='%s' clients=%u server=0x%llX timeout=%.1f",
                      out.empty() ? "" : " || ", static_cast<unsigned long long>(d.address),
                      d.className.c_str(), d.connectionClass.empty() ? "NOT READ" : d.connectionClass.c_str(),
                      d.clientConnections, static_cast<unsigned long long>(d.serverConnection),
                      static_cast<double>(d.connectionTimeout));
        out += buf;
    }
    for (const NetObjectReading& p : pending) {
        char buf[300];
        std::snprintf(buf, sizeof(buf),
                      "%sPENDING NET GAME 0x%llX driver=0x%llX '%s' connectionClass='%s' clients=%u server=0x%llX",
                      out.empty() ? "" : " || ", static_cast<unsigned long long>(p.address),
                      static_cast<unsigned long long>(p.driver), p.driverClass.c_str(),
                      p.connectionClass.empty() ? "NOT READ" : p.connectionClass.c_str(), p.clientConnections,
                      static_cast<unsigned long long>(p.serverConnection));
        out += buf;
    }
    if (drivers.empty() && pending.empty()) out += "NO DRIVER AND NO PENDING NET GAME";
    char header[96];
    std::snprintf(header, sizeof(header), " || worlds=%zu (of %u object(s))",
                  worlds.size(), static_cast<unsigned>(scanned));
    out += header;
    for (std::size_t i = 0; i < worlds.size() && i < 3; ++i) {
        const WorldSample& w = worlds[i];
        char buf[240];
        std::snprintf(buf, sizeof(buf),
                      " | 0x%llX '%s' level=%s NetDriver=0x%llX%s GameMode=0x%llX",
                      static_cast<unsigned long long>(w.address), w.name.c_str(),
                      w.persistentLevel == 0 ? "NULL" : "set",
                      static_cast<unsigned long long>(w.rawNetDriver),
                      w.netDriver != 0 ? " (a pointer)"
                                       : (w.rawNetDriver == 0 ? " (null)" : " (NOT a pointer)"),
                      static_cast<unsigned long long>(w.authorityGameMode));
        out += buf;
    }
    if (worlds.size() > 3) out += " | ...";
    return out;
}

std::string NetDriverDefinitionSurvey::line() const {

    char engineText[32] = "NOT FOUND";
    if (engine != 0) std::snprintf(engineText, sizeof(engineText), "0x%llX",
                                   static_cast<unsigned long long>(engine));
    std::string out = "engine=" + std::string(engineText);
    if (engine != 0) out += " '" + engineClass + "'";
    out += " NetDriverDefinitions=" + std::to_string(count);
    for (const NetDriverDefinitionInfo& d : definitions) {
        out += " | '" + d.defName + "' -> class '" + d.driverClass + "' ("
             + (d.driverClassPresent ? "PRESENT in this build" : "NOT PRESENT in this build") + ")";
        if (!d.fallbackClass.empty()) {
            out += " (fallback '" + d.fallbackClass + "' "
                 + (d.fallbackClassPresent ? "PRESENT" : "NOT PRESENT") + ")";
        }
    }

    char gameModeText[32] = "NONE";
    if (gameMode != 0) std::snprintf(gameModeText, sizeof(gameModeText), "0x%llX",
                                    static_cast<unsigned long long>(gameMode));
    char flagsText[16];
    std::snprintf(flagsText, sizeof(flagsText), "0x%X", static_cast<unsigned>(gameModeFlags));
    out += " | live GameMode=" + std::string(gameModeText);
    out += " flags=" + std::string(flagsText);
    out += " bUseSeamlessTravel=" + std::string(seamlessTravel ? "1 (TWO loads per travel)"
                                                              : "0 (one load per travel)");
    if (!note.empty()) {
        out += " [";
        out += note;
        out += "]";
    }
    return out;
}

NetDriverDefinitionSurvey survey_net_driver_definitions() {
    using namespace offsets;
    NetDriverDefinitionSurvey s;
    const FoundObjects found = find_named_objects();
    s.engine = found.engine;

    if (found.liveWorld != 0) {
        std::uint64_t gameMode = 0;
        if (try_read_u64(found.liveWorld + world_offsets::kAuthorityGameMode, gameMode) && gameMode >= 0x10000) {
            s.gameMode = static_cast<std::uintptr_t>(gameMode);
            std::uint32_t word = 0;
            if (try_read_u32(s.gameMode + 0x02A8, word)) {
                s.gameModeFlags = static_cast<std::uint8_t>(word & 0xFFu);
                s.seamlessTravel = (word & 0x01u) != 0;
            }
        }
    }
    if (s.engine == 0) {
        s.note = "no 'GameEngine' object in the object array, so UEngine::NetDriverDefinitions cannot be "
                 "reached -- and without it nothing can say which driver '?listen' would build";
        return s;
    }
    s.engineClass = object_name_of(s.engine);
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    if (!try_read_u64(s.engine + 0x0BF8, data) || !try_read_u32(s.engine + 0x0BF8 + 8, num)) {
        s.note = "UEngine::NetDriverDefinitions at could not be read";
        return s;
    }
    s.count = num;
    if (num == 0 || data < 0x10000 || num > 64) {
        s.note = num == 0 ? "the array is EMPTY, so a '?listen' travel has no definition to build from"
                          : "the array's data pointer or count is not plausible";
        return s;
    }
    for (std::uint32_t i = 0; i < num; ++i) {
        const auto base = static_cast<std::uintptr_t>(data) + static_cast<std::uintptr_t>(i) * 0x18;
        NetDriverDefinitionInfo d;
        for (int field = 0; field < 3; ++field) {
            std::uint64_t nameField = 0;
            if (!try_read_u64(base + static_cast<std::uintptr_t>(field) * 8, nameField)) continue;
            std::string text;
            std::string note;
            if (resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), text, note)) {
                if (field == 0) d.defName = text;
                else if (field == 1) d.driverClass = text;
                else d.fallbackClass = text;
            }
        }

        const auto shortName = [](const std::string& dotted) {
            const std::size_t dot = dotted.rfind('.');
            return dot == std::string::npos ? dotted : dotted.substr(dot + 1);
        };
        d.driverClassPresent = shortName(d.driverClass) == "IpNetDriver"
                                   ? found.hasIpNetDriverClass
                                   : (shortName(d.driverClass) == "SteamNetDriver"
                                          ? found.hasSteamNetDriverClass
                                          : false);
        d.fallbackClassPresent = shortName(d.fallbackClass) == "IpNetDriver"
                                     ? found.hasIpNetDriverClass
                                     : (shortName(d.fallbackClass) == "SteamNetDriver"
                                            ? found.hasSteamNetDriverClass
                                            : false);
        s.definitions.push_back(std::move(d));
    }
    return s;
}

namespace {

std::uint64_t g_savedPrimaryBytes = 0;

struct DriverElement {
    std::uintptr_t base = 0;
    std::uint64_t  primaryBytes = 0;
    std::uint64_t  fallbackBytes = 0;
    std::string    defName;
    std::string    className;
    std::string    fallbackName;
    std::string    note;
};

std::string fname_text_at(std::uintptr_t address, std::uint64_t& bytesOut) {
    bytesOut = 0;
    if (!try_read_u64(address, bytesOut)) return {};
    std::string text;
    std::string note;
    if (!resolve_fname(static_cast<std::uint32_t>(bytesOut & 0xFFFFFFFFu), text, note)) return {};
    return text;
}

DriverElement find_game_net_driver() {
    DriverElement e;
    const FoundObjects found = find_named_objects();
    if (found.engine == 0) {
        e.note = "no 'GameEngine' object, so UEngine::NetDriverDefinitions cannot be reached";
        return e;
    }
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    if (!try_read_u64(found.engine + 0x0BF8, data) || !try_read_u32(found.engine + 0x0BF8 + 8, num)) {
        e.note = "UEngine::NetDriverDefinitions at could not be read";
        return e;
    }
    if (num == 0 || data < 0x10000 || num > 64) {
        e.note = "NetDriverDefinitions is empty or implausible, so there is no definition to rewrite";
        return e;
    }
    for (std::uint32_t i = 0; i < num; ++i) {
        const auto base = static_cast<std::uintptr_t>(data) + static_cast<std::uintptr_t>(i) * 0x18;
        std::uint64_t primary = 0;
        std::uint64_t fallback = 0;
        const std::string defName = fname_text_at(base + 0x00, fallback);
        const std::string className = fname_text_at(base + 0x08, primary);
        const std::string fallbackName = fname_text_at(base + 0x10, fallback);
        if (defName != "GameNetDriver") continue;
        e.base = base;
        e.primaryBytes = primary;
        e.fallbackBytes = fallback;
        e.defName = defName;
        e.className = className;
        e.fallbackName = fallbackName;
        return e;
    }
    e.note = "no definition named 'GameNetDriver' is in NetDriverDefinitions, so '?listen' would use the "
             "engine's default and there is nothing here to rewrite";
    return e;
}

}

DriverClassSwap prefer_ip_net_driver() {
    DriverClassSwap r;
    const DriverElement e = find_game_net_driver();
    if (e.base == 0) {
        r.note = e.note;
        return r;
    }
    r.defName = e.defName;
    r.before = e.className;
    r.after = e.fallbackName;
    if (e.fallbackBytes == 0) {
        r.note = "the definition's fallback class name is empty, so there is nothing address-based to switch to";
        return r;
    }
    if (e.primaryBytes == e.fallbackBytes) {
        r.swapped = true;
        r.note = "the field already holds the fallback's name; nothing was written";
        return r;
    }
    if (!write_process_bytes(e.base + 0x08, &e.fallbackBytes, sizeof(e.fallbackBytes))) {
        r.note = "the eight-byte write to the definition's class field was REFUSED by the guarded writer";
        return r;
    }
    std::uint64_t readBack = 0;
    const std::string check = fname_text_at(e.base + 0x08, readBack);
    if (readBack != e.fallbackBytes) {
        r.note = "the write landed but did not read back, so the field is NOT trusted";
        return r;
    }
    g_savedPrimaryBytes = e.primaryBytes;
    r.after = check;
    r.swapped = true;
    return r;
}

DriverClassSwap restore_net_driver_class() {
    DriverClassSwap r;
    if (g_savedPrimaryBytes == 0) {
        r.note = "no swap has been made in this process, so there is nothing saved to restore";
        return r;
    }
    const DriverElement e = find_game_net_driver();
    if (e.base == 0) {
        r.note = e.note;
        return r;
    }
    r.defName = e.defName;
    r.before = e.className;
    if (!write_process_bytes(e.base + 0x08, &g_savedPrimaryBytes, sizeof(g_savedPrimaryBytes))) {
        r.note = "the restore write was REFUSED by the guarded writer";
        return r;
    }
    std::uint64_t readBack = 0;
    r.after = fname_text_at(e.base + 0x08, readBack);
    r.swapped = (readBack == g_savedPrimaryBytes);
    if (!r.swapped) r.note = "the restore write landed but did not read back";
    return r;
}

struct NamedObjectsCache {

    std::shared_ptr<const FoundObjects> found;
    std::uint64_t stamp = 0;
    bool          valid = false;
};

std::mutex g_namedObjectsMutex;

NamedObjectsCache& named_objects_cache() {
    static NamedObjectsCache c;
    return c;
}

namespace {

std::uintptr_t pawn_from_controller(std::uintptr_t controller) {
    if (controller < 0x10000) return 0;
    for (const std::uint32_t off : {actor_offsets::kControllerPawn, actor_offsets::kAcknowledgedPawn}) {
        std::uint64_t pawn = 0;
        if (!try_read_u64(controller + off, pawn) || pawn < 0x10000) continue;
        const auto address = static_cast<std::uintptr_t>(pawn);
        if (object_name_of(address).empty()) continue;
        return address;
    }
    return 0;
}

}

std::shared_ptr<const FoundObjects> find_named_objects_cached() {
    NamedObjectsCache& c = named_objects_cache();
    const std::uint64_t now = GetTickCount64();
    std::shared_ptr<const FoundObjects> snapshot;
    std::uint64_t stamp = 0;
    {
        const std::lock_guard<std::mutex> lock(g_namedObjectsMutex);
        if (c.valid && c.found) {
            snapshot = c.found;
            stamp = c.stamp;
        }
    }
    if (snapshot) {

        bool stillGood = false;
        if (snapshot->playerController != 0) {
            stillGood = !object_name_of(snapshot->playerController).empty();
        } else {
            stillGood = (now - stamp) < 2000;
        }
        if (stillGood) return snapshot;
    }

    std::shared_ptr<const FoundObjects> built =
        std::make_shared<const FoundObjects>(find_named_objects());
    {
        const std::lock_guard<std::mutex> lock(g_namedObjectsMutex);
        c.found = built;
        c.stamp = GetTickCount64();
        c.valid = true;
    }
    return built;
}

LiveObjects find_live_objects() {

    static thread_local bool s_threadProbe = false;
    if (!s_threadProbe) {
        s_threadProbe = true;
        DSH_INFO("thread-probe: find_live_objects from thread %u",
                 static_cast<unsigned>(GetCurrentThreadId()));
    }

    const std::shared_ptr<const FoundObjects> f = find_named_objects_cached();
    LiveObjects o;
    o.world = f->world;
    o.playerController = f->playerController;

    o.pawn = pawn_from_controller(o.playerController);
    o.worldObjectCount = f->worldCount;
    if (o.world != 0) {
        std::uint64_t level = 0;
        if (try_read_u64(o.world + world_offsets::kPersistentLevel, level) && level >= 0x10000) {
            o.level = static_cast<std::uintptr_t>(level);
        }
    }
    return o;
}

std::uintptr_t live_world_address() {

    const std::shared_ptr<const FoundObjects> f = find_named_objects_cached();
    return f ? f->world : 0;
}

LiveObjects rescan_live_objects() {
    {

        const std::lock_guard<std::mutex> lock(g_namedObjectsMutex);
        named_objects_cache().valid = false;
    }
    return find_live_objects();
}

namespace {

bool folder_is_letter_digits(const std::string& s, char lead) {
    if (s.size() < 2) return false;
    const char first = s[0];
    if (first != lead && first != static_cast<char>(lead + 32)) return false;
    for (std::size_t i = 1; i < s.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(s[i])) == 0) return false;
    }
    return true;
}

std::string region_of_package(const std::string& package) {
    if (package.empty()) return {};
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t slash = package.find('/', start);
        parts.push_back(package.substr(start, slash == std::string::npos ? std::string::npos
                                                                        : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (parts.empty()) return package;

    const std::size_t dot = parts.back().find('.');
    if (dot != std::string::npos) parts.back() = parts.back().substr(0, dot);
    if (parts.back().empty()) return package;
    parts.pop_back();
    if (parts.size() >= 2) {
        const std::string& area = parts.back();
        const std::string& region = parts[parts.size() - 2];
        if (folder_is_letter_digits(area, 'A') && folder_is_letter_digits(region, 'M')) parts.pop_back();
    }

    std::string out = (!package.empty() && package.front() == '/') ? "/" : "";
    bool first = true;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!first) out += '/';
        out += part;
        first = false;
    }
    return out;
}

}

std::string MapIdentity::line() const {
    if (key.empty()) {
        return "map: NO KEY -- " + (note.empty() ? std::string("unreadable") : note) +
               " (world " + worldName + ")";
    }
    char buf[600];
    std::snprintf(buf, sizeof(buf),
                  "map: key '%s' (%s) -- map package '%s', world '%s' 0x%llX, the PAWN's world '%s' 0x%llX, "
                  "level '%s' 0x%llX",
                  key.c_str(), keySource.c_str(), packageName.c_str(), worldName.c_str(),
                  static_cast<unsigned long long>(world), pawnWorldName.c_str(),
                  static_cast<unsigned long long>(pawnWorld), levelName.c_str(),
                  static_cast<unsigned long long>(level));
    return buf;
}

MapIdentity current_map_identity() {
    MapIdentity m;
    const LiveObjects live = find_live_objects();
    m.world = live.world;

    const std::uintptr_t pawn = pawn_from_controller(live.playerController);
    if (pawn >= 0x10000) {
        std::uint64_t pawnLevel = 0;
        std::uint64_t pawnWorld = 0;
        if (try_read_u64(pawn + offsets::kUObjectOuterOffset, pawnLevel) && pawnLevel >= 0x10000 &&
            try_read_u64(static_cast<std::uintptr_t>(pawnLevel) + offsets::kUObjectOuterOffset, pawnWorld) &&
            pawnWorld >= 0x10000) {
            m.pawnWorld = static_cast<std::uintptr_t>(pawnWorld);
            m.pawnWorldName = object_name_of(m.pawnWorld);
            if (!m.pawnWorldName.empty()) m.world = m.pawnWorld;
        }
    }

    if (m.world == 0) {
        m.note = "there is no live world (are we at the main menu?)";
        return m;
    }
    m.worldName = object_name_of(m.world);

    std::uint64_t level = 0;
    if (!try_read_u64(m.world + world_offsets::kPersistentLevel, level) || level < 0x10000) {
        if (!m.worldName.empty()) {
            m.key = m.worldName;
            m.keySource = "world (no PersistentLevel)";
        } else {
            m.note = "the live world has no PersistentLevel and no name of its own";
        }
        return m;
    }
    m.level = static_cast<std::uintptr_t>(level);
    m.levelName = object_name_of(m.level);

    std::uintptr_t top = m.level;
    for (int hop = 0; hop < 4; ++hop) {
        std::uint64_t outer = 0;
        if (!try_read_u64(top + offsets::kUObjectOuterOffset, outer) || outer < 0x10000) break;
        if (outer == top) break;
        top = static_cast<std::uintptr_t>(outer);
    }
    m.package = top;

    if (m.package != m.level) m.packageName = object_name_of(m.package);

    const std::string region = region_of_package(m.packageName);
    if (!region.empty()) {
        m.key = region;
        m.keySource = region == m.packageName ? "the map package itself" : "the REGION of the map package";
    } else if (!m.packageName.empty() && m.package != m.world) {
        m.key = m.packageName;
        m.keySource = "package";
    } else if (!m.levelName.empty()) {
        m.key = m.levelName;
        m.keySource = "level";
    } else if (!m.worldName.empty()) {
        m.key = m.worldName;
        m.keySource = "world";
    }
    if (m.key.empty()) m.note = "neither the outermost object, the level nor the world has a readable name";
    return m;
}

std::string object_name_of_actor(std::uintptr_t actor) { return object_name_of(actor); }

namespace {
struct WalkStat {
    const char*        what = nullptr;
    unsigned long long calls = 0;
    unsigned long long totalUs = 0;
    unsigned long long maxUs = 0;
};
std::mutex         g_walkMutex;
WalkStat           g_walkStats[16];
int                g_walkCount = 0;
unsigned long long g_walkLastReportMs = 0;
}

void walk_note(const char* what, long long microseconds) {
    if (what == nullptr) return;
    if (microseconds < 0) microseconds = 0;
    const auto us = static_cast<unsigned long long>(microseconds);
    std::vector<std::string> lines;
    {
        const std::lock_guard<std::mutex> lock(g_walkMutex);
        WalkStat* slot = nullptr;
        for (int i = 0; i < g_walkCount; ++i) {
            if (g_walkStats[i].what == what) {
                slot = &g_walkStats[i];
                break;
            }
        }
        if (slot == nullptr && g_walkCount < 16) {
            slot = &g_walkStats[g_walkCount++];
            slot->what = what;

            slot->calls = 0;
            slot->totalUs = 0;
            slot->maxUs = 0;
        }
        if (slot != nullptr) {
            ++slot->calls;
            slot->totalUs += us;
            if (us > slot->maxUs) slot->maxUs = us;
        }
        const unsigned long long now = GetTickCount64();
        if (g_walkLastReportMs == 0) g_walkLastReportMs = now;
        if (now - g_walkLastReportMs < 10000) return;
        g_walkLastReportMs = now;
        char buf[256];
        for (int i = 0; i < g_walkCount; ++i) {
            std::snprintf(buf, sizeof(buf), "walk: '%s' %llu call(s) in the last 10 s, %.1f ms total, %.1f ms max",
                          g_walkStats[i].what, g_walkStats[i].calls,
                          static_cast<double>(g_walkStats[i].totalUs) / 1000.0,
                          static_cast<double>(g_walkStats[i].maxUs) / 1000.0);
            lines.emplace_back(buf);
        }
        g_walkCount = 0;
    }
    for (const std::string& line : lines) DSH_INFO("%s", line.c_str());
}

}
