#include "dsh/reflect/spawn.h"
#include "dsh/reflect/gamethread.h"

#include "dsh/platform/hook.h"
#include "dsh/platform/log.h"
#include "dsh/reflect/reflection.h"
#include "dsh/reflect/scanner.h"
#include "dsh/reflect/offsets.h"
#include "dsh/reflect/world.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dsh::reflect {

std::uintptr_t load_asset_by_path(const std::string& path);
std::string    object_path_name(std::uintptr_t obj);
void           prepare_path_tools();

std::uintptr_t resolve_uclass_address(const char* className);
bool           stance_call_rep_notify(std::uintptr_t actor, std::uint8_t oldStance, std::string& note);

bool module_call_arrival_notify(std::uintptr_t machine, ModuleArrivalResult& out);

std::string hex_ptr(std::uintptr_t v);
}
namespace dsh::reflect {
namespace {

thread_local std::unordered_map<std::uintptr_t, std::string> g_classNameCache;

std::string class_name_of(std::uintptr_t obj) {
    std::uint64_t cls = 0;
    if (!try_read_u64(obj + offsets::kUObjectClassOffset, cls) || cls < 0x10000) return {};
    const auto clsAddr = static_cast<std::uintptr_t>(cls);
    const auto it = g_classNameCache.find(clsAddr);
    if (it != g_classNameCache.end()) return it->second;
    std::string name;
    std::uint64_t nameField = 0;
    if (try_read_u64(clsAddr + offsets::kUObjectNameOffset, nameField)) {
        std::string note;
        resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), name, note);
    }
    g_classNameCache.emplace(clsAddr, name);
    return name;
}

std::string object_name_of(std::uintptr_t obj) {
    std::uint64_t nameField = 0;
    std::string name;
    if (try_read_u64(obj + offsets::kUObjectNameOffset, nameField)) {
        std::string note;
        resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), name, note);
    }
    return name;
}

std::uintptr_t outer_of(std::uintptr_t obj) {
    std::uint64_t outer = 0;
    if (!try_read_u64(obj + offsets::kUObjectOuterOffset, outer)) return 0;
    return static_cast<std::uintptr_t>(outer);
}

std::vector<std::uintptr_t>& puppet_host_registry() {
    static std::vector<std::uintptr_t> hosts;
    return hosts;
}

void register_puppet_host(std::uintptr_t host) {
    if (host == 0) return;
    auto& hosts = puppet_host_registry();
    for (auto h : hosts) {
        if (h == host) return;
    }
    hosts.push_back(host);
}

struct NamedObject {
    std::uintptr_t address = 0;
    std::string    objectName;
    std::string    className;
    std::string    outerName;
};

template <typename Predicate>
bool find_object_matching(Predicate pred, NamedObject& out, std::uint32_t* scannedOut) {
    using namespace offsets;
    const ModuleInfo exe = main_module();
    if (exe.base == 0) return false;
    const auto array = exe.base + static_cast<std::uintptr_t>(kGObjects);

    std::uint64_t chunkTable = 0;
    std::uint32_t numElements = 0;
    if (!try_read_u64(array + kObjectsOffset, chunkTable) || chunkTable < 0x10000) return false;
    if (!try_read_u32(array + kNumElementsOffset, numElements)) return false;

    std::uint32_t scanned = 0;
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
        ++scanned;
        const auto addr = static_cast<std::uintptr_t>(obj);

        NamedObject cand;
        cand.address = addr;
        cand.objectName = object_name_of(addr);
        if (cand.objectName.empty()) continue;
        cand.className = class_name_of(addr);
        const std::uintptr_t outer = outer_of(addr);
        cand.outerName = outer != 0 ? object_name_of(outer) : std::string();

        if (!pred(cand, outer)) continue;
        out = cand;
        if (scannedOut != nullptr) *scannedOut = scanned;
        return true;
    }
    if (scannedOut != nullptr) *scannedOut = scanned;
    return false;
}

enum class ChildrenLookup { kFound, kAbsent, kUnknown };

ChildrenLookup find_function_in_children(const char* functionName, std::uintptr_t ownerClass, NamedObject& out,
                                        int& scannedOut) {
    scannedOut = 0;
    std::uint64_t childRaw = 0;
    if (!read_process_bytes(ownerClass + 0x0048, &childRaw, sizeof(childRaw))) return ChildrenLookup::kUnknown;
    std::uintptr_t field = static_cast<std::uintptr_t>(childRaw);

    if (field < 0x10000) return ChildrenLookup::kAbsent;

    constexpr int kMaxFields = 4096;
    while (field >= 0x10000 && scannedOut < kMaxFields) {
        ++scannedOut;
        std::uint64_t fieldOuter = 0;
        std::uint64_t nextRaw = 0;
        if (!read_process_bytes(field + 0x0020, &fieldOuter, sizeof(fieldOuter))) return ChildrenLookup::kUnknown;
        if (static_cast<std::uintptr_t>(fieldOuter) == ownerClass) {
            const std::string fieldClass = object_class_name(field);
            if (fieldClass == "Function" && object_own_name(field) == functionName) {
                out.address = field;
                out.objectName = functionName;
                out.className = fieldClass;
                out.outerName = object_own_name(static_cast<std::uintptr_t>(fieldOuter));
                return ChildrenLookup::kFound;
            }
        }
        if (!read_process_bytes(field + 0x0028, &nextRaw, sizeof(nextRaw))) return ChildrenLookup::kUnknown;
        field = static_cast<std::uintptr_t>(nextRaw);
    }

    return scannedOut >= kMaxFields ? ChildrenLookup::kUnknown : ChildrenLookup::kAbsent;
}

bool find_function_owned_by(const char* functionName, std::uintptr_t ownerClass, NamedObject& out) {
    DSH_TIME_WALK("find_function_owned_by");
    if (functionName == nullptr) return false;

    static std::mutex cacheMutex;
    static std::unordered_map<std::string, NamedObject> hits;
    static std::unordered_set<std::string> misses;
    static int childrenAnswers = 0;
    std::string key(functionName);
    key += '@';
    key += std::to_string(ownerClass);
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        const auto hit = hits.find(key);
        if (hit != hits.end()) {
            out = hit->second;
            return true;
        }
        if (misses.find(key) != misses.end()) return false;
    }

    NamedObject fast;
    int fieldsScanned = 0;
    const ChildrenLookup look = find_function_in_children(functionName, ownerClass, fast, fieldsScanned);
    bool found = false;
    bool conclusive = true;
    if (look == ChildrenLookup::kFound) {
        out = fast;
        found = true;
    } else if (look == ChildrenLookup::kAbsent) {
        found = false;
    } else {
        conclusive = false;
        found = find_object_matching(
            [&](const NamedObject& c, std::uintptr_t outer) {
                return c.className == "Function" && c.objectName == functionName &&
                       outer == ownerClass;
            },
            out, nullptr);
    }
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        if (found) hits.emplace(key, out);
        else misses.insert(key);

        if (conclusive && fieldsScanned > 0 && childrenAnswers < 12) {
            ++childrenAnswers;
            if (found) {
                DSH_INFO("reflect: '%s' answered by the class's own Children chain after %d field(s) -- the "
                         "object-table walk was NOT needed", functionName, fieldsScanned);
            } else {
                DSH_INFO("reflect: '%s' is PROVABLY absent from that class's own Children chain after %d "
                         "field(s) -- the object-table walk was NOT needed either", functionName, fieldsScanned);
            }
        }
    }
    return found;
}

bool is_uclass(const std::string& objectClassName) {
    if (objectClassName == "Class") return true;
    if (objectClassName == "BlueprintGeneratedClass") return true;
    if (objectClassName == "WidgetBlueprintGeneratedClass") return true;
    if (objectClassName == "AnimBlueprintGeneratedClass") return true;

    constexpr const char* kSuffix = "GeneratedClass";
    constexpr std::size_t kSuffixLen = 14;
    return objectClassName.size() > kSuffixLen &&
           objectClassName.compare(objectClassName.size() - kSuffixLen, kSuffixLen, kSuffix) == 0;
}

bool find_uclass(const char* className, NamedObject& out) {
    return find_object_matching(
        [&](const NamedObject& c, std::uintptr_t) {
            return is_uclass(c.className) && c.objectName == className;
        },
        out, nullptr);
}

using ProcessEventFn = void (*)(void* self, void* function, void* params);

struct ProcessEventResolution {
    bool           usable = false;
    std::uintptr_t address = 0;
    std::uintptr_t vtableSlot = 0;
    std::uintptr_t headerAddress = 0;
    std::string    note;
};

ProcessEventResolution resolve_process_event(std::uintptr_t anyObject) {
    using namespace offsets;
    ProcessEventResolution r;

    static std::mutex      s_mutex;
    static std::uintptr_t  s_address = 0;
    static int             s_loggedVerdict = 0;
    {
        const std::lock_guard<std::mutex> lock(s_mutex);
        if (s_address != 0) {
            r.headerAddress = s_address;
            r.vtableSlot = s_address;
            r.address = s_address;
            r.usable = true;
            r.note = "settled earlier in this process (the the expected RVA and the live vtable slot agree)";
            return r;
        }
    }

    const ModuleInfo exe = main_module();
    if (exe.base == 0) {
        r.note = "no main module";
        return r;
    }
    r.headerAddress = exe.base + static_cast<std::uintptr_t>(kProcessEvent);

    if (anyObject >= 0x10000) {
        std::uint64_t vtable = 0;
        if (try_read_u64(anyObject, vtable) && vtable >= 0x10000) {
            std::uint64_t slot = 0;
            if (try_read_u64(static_cast<std::uintptr_t>(vtable) +
                                 static_cast<std::uintptr_t>(kProcessEventIdx) * 8, slot)) {
                r.vtableSlot = static_cast<std::uintptr_t>(slot);
            }
        }
    }

    if (r.vtableSlot == 0) {
        r.note = "could not read the live vtable slot; refusing to hook on one source alone";
    } else if (r.vtableSlot != r.headerAddress) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "the expected RVA and the live vtable DISAGREE (0x%llX vs 0x%llX) -- the recorded address is stale for "
                      "this executable; NOT hooking",
                      static_cast<unsigned long long>(r.headerAddress),
                      static_cast<unsigned long long>(r.vtableSlot));
        r.note = buf;
    } else {
        r.address = r.headerAddress;
        r.usable = true;
        r.note = "the expected RVA and the live vtable slot agree";
    }

    {
        const int verdict = r.usable ? 1 : 2;
        bool logNow = false;
        {
            const std::lock_guard<std::mutex> lock(s_mutex);
            if (s_loggedVerdict != verdict) {
                s_loggedVerdict = verdict;
                logNow = true;
            }
            if (r.usable) s_address = r.address;
        }
        if (logNow) {
            DSH_INFO("processevent: expected RVA 0x%X -> 0x%llX; live vtable[0x%X] -> 0x%llX%s", kProcessEvent,
                     static_cast<unsigned long long>(r.headerAddress), kProcessEventIdx,
                     static_cast<unsigned long long>(r.vtableSlot),
                     r.usable ? " (they agree; this is the only line this check prints until the verdict "
                                "changes)"
                              : " (no usable address; the caller's own retry is what asks again)");
        }
    }
    return r;
}

constexpr std::size_t kParamBufferBytes = 0x100;

class ParamBuffer {
public:
    ParamBuffer() { std::memset(bytes_, 0, sizeof(bytes_)); }
    [[nodiscard]] void* data() noexcept { return bytes_; }
    template <typename T>
    void set(std::size_t offset, const T& value) {
        static_assert(std::is_trivially_copyable_v<T>, "only trivially copyable fields");
        std::memcpy(bytes_ + offset, &value, sizeof(T));
    }
    template <typename T>
    [[nodiscard]] T get(std::size_t offset) const {
        T v{};
        std::memcpy(&v, bytes_ + offset, sizeof(T));
        return v;
    }
private:
    alignas(16) unsigned char bytes_[kParamBufferBytes];
};

struct TransformRaw {
    float rotation[4];
    float translation[3];
    float pad0;
    float scale[3];
    float pad1;
};

TransformRaw identity_transform() {
    TransformRaw t{};
    t.rotation[3] = 1.0f;
    t.scale[0] = t.scale[1] = t.scale[2] = 1.0f;
    return t;
}

}

bool stance_call_rep_notify(std::uintptr_t actor, std::uint8_t oldStance, std::string& note) {
    note.clear();
    if (actor < 0x10000) {
        note = "no actor to hand the stance change to";
        return false;
    }

    static std::uintptr_t s_onRep = 0;
    static ProcessEventFn s_pe = nullptr;

    if (s_onRep == 0 || s_pe == nullptr) {
        NamedObject function;

        const std::uintptr_t declaringClass = resolve_uclass_address("DACharacter");
        if (declaringClass == 0) {
            note = "the DACharacter UClass is not in the object array, so the class that DECLARES "
                   "OnRep_CurrentStance cannot be named and the function is NOT looked up";
            return false;
        }
        if (!find_function_owned_by("OnRep_CurrentStance", declaringClass, function)) {
            note = "no UFunction named OnRep_CurrentStance is owned by the DACharacter UClass, so the "
                   "engine's replicated-stance path cannot be called";
            return false;
        }
        const ProcessEventResolution pe = resolve_process_event(live_world_address());
        if (!pe.usable) {
            note = "ProcessEvent is not usable (" + pe.note + ")";
            return false;
        }
        s_onRep = function.address;
        s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
    }

    unsigned char params[0x10] = {0};
    std::memcpy(params + 0x00, &oldStance, sizeof(oldStance));
    s_pe(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(s_onRep), params);
    return true;
}

bool object_is_still_registered(std::uintptr_t object, std::string& note) {
    using namespace dsh::reflect::offsets;
    note.clear();
    if (object < 0x10000) {
        note = "the pointer is implausible, so it cannot be a live object";
        return false;
    }
    std::uint32_t index = 0;
    if (!try_read_u32(object + 0x0C, index)) {
        note = "the object's own slot index is not readable any more";
        return false;
    }
    const std::uintptr_t base = main_module_base();
    if (base == 0) {
        note = "the object array is not reachable (no module base)";
        return false;
    }
    const std::uintptr_t array = base + static_cast<std::uintptr_t>(kGObjects);
    std::uint64_t chunkTable = 0;
    if (!try_read_u64(array + kObjectsOffset, chunkTable) || chunkTable < 0x10000) {
        note = "the object array's chunk table is not readable";
        return false;
    }
    std::uint32_t numElements = 0;
    if (!try_read_u32(array + kNumElementsOffset, numElements)) {
        note = "the object array's element count is not readable";
        return false;
    }
    if (index >= numElements) {
        note = "its index " + std::to_string(index) + " is outside the array's " +
               std::to_string(numElements) + " element(s) -- it has been removed";
        return false;
    }
    const std::uint32_t chunkIndex = index / kElementsPerChunk;
    const std::uint32_t within = index % kElementsPerChunk;
    std::uint64_t chunk = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunkTable) + static_cast<std::uintptr_t>(chunkIndex) * 8,
                      chunk) || chunk < 0x10000) {
        note = "the object array's chunk " + std::to_string(chunkIndex) + " is not readable";
        return false;
    }
    std::uint64_t slot = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunk) +
                          static_cast<std::uintptr_t>(within) * kObjectItemStride,
                      slot)) {
        note = "its slot in the object array is not readable";
        return false;
    }
    if (slot != object) {
        note = "its slot now names another object (the object has been removed and the slot reused)";
        return false;
    }
    return true;
}
bool character_call_derived_flags(std::uintptr_t actor, CharacterDerivedFlags& out, std::string& note) {
    note.clear();
    out = CharacterDerivedFlags{};
    if (actor < 0x10000) {
        note = "there is no actor to ask the derived getters about (the pointer is implausible)";
        return false;
    }

    static const char* const kNames[5] = {"IsSprint", "IsSprinting", "IsHover", "IsHovering",
                                          "IsInitialized"};
    static std::uintptr_t  s_fn[5] = {0, 0, 0, 0, 0};
    static ProcessEventFn  s_pe = nullptr;
    static bool            s_resolved = false;
    static std::string     s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("DACharacter");
        if (declaringClass == 0) {
            s_refusal = "the DACharacter UClass is not in the object array, so the class that DECLARES "
                        "IsSprint/IsSprinting/IsHover/IsHovering/IsInitialized cannot be named and NONE of "
                        "the five is looked up";
        }
        for (std::size_t i = 0; s_refusal.empty() && i < 5; ++i) {
            NamedObject function;
            if (!find_function_owned_by(kNames[i], declaringClass, function)) {
                s_refusal = std::string("no UFunction named ") + kNames[i] +
                            " is owned by the DACharacter UClass, so the derived flag that getter "
                            "reports cannot be measured";
                break;
            }
            s_fn[i] = function.address;
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) {
            note = "the actor is NOT a live object (" + whyNot + "), so the getters were NOT called";
            return false;
        }
    }

    bool answer[5] = {false, false, false, false, false};
    for (std::size_t i = 0; i < 5; ++i) {

        unsigned char params[0x10] = {0};
        s_pe(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(s_fn[i]), params);

        answer[i] = params[0] != 0;
    }
    out.sprint = answer[0];
    out.sprinting = answer[1];
    out.hover = answer[2];
    out.hovering = answer[3];
    out.initialized = answer[4];

    char buf[112];
    std::snprintf(buf, sizeof(buf), "IsSprint=%d IsSprinting=%d IsHover=%d IsHovering=%d IsInitialized=%d",
                  out.sprint ? 1 : 0, out.sprinting ? 1 : 0, out.hover ? 1 : 0, out.hovering ? 1 : 0,
                  out.initialized ? 1 : 0);
    note = buf;
    return true;
}

bool character_call_set_sprint(std::uintptr_t actor, bool sprint, std::string& note) {
    note.clear();
    if (actor < 0x10000) {
        note = "there is no puppet to call SetSprint on (the pointer is implausible)";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("DACharacter");
        if (declaringClass == 0) {
            s_refusal = "the DACharacter UClass is not in the object array, so the class that DECLARES "
                        "SetSprint cannot be named and the function is NOT looked up";
        } else {
            NamedObject function;
            if (!find_function_owned_by("SetSprint", declaringClass, function)) {
                s_refusal = "no UFunction named SetSprint is owned by the DACharacter UClass, so the "
                            "game's own sprint setter cannot be called";
            } else {
                s_fn = function.address;
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    unsigned char params[0x10] = {0};
    const unsigned char wanted = sprint ? 1 : 0;
    std::memcpy(params + 0x00, &wanted, sizeof(wanted));
    s_pe(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(s_fn), params);
    return true;
}

namespace {

constexpr std::uintptr_t kCharacterAbilitySystemOffset = 0x0860;

bool is_ability_system(std::uintptr_t component) {
    std::uint64_t classRaw = 0;
    if (!read_process_bytes(component + offsets::kUObjectClassOffset, &classRaw, sizeof(classRaw)) ||
        classRaw < 0x10000) {
        return false;
    }
    for (int depth = 0; classRaw >= 0x10000 && depth < 24; ++depth) {
        const auto klass = static_cast<std::uintptr_t>(classRaw);
        if (object_own_name(klass) == "AbilitySystemComponent") return true;
        std::uint64_t super = 0;
        if (!read_process_bytes(klass + 0x0040, &super, sizeof(super))) return false;
        classRaw = super;
    }
    return false;
}

}
bool character_apply_sprint_effect(std::uintptr_t actor, std::uint64_t& handleOut, std::string& note) {
    handleOut = 0;
    note.clear();
    if (actor < 0x10000) {
        note = "there is no puppet to apply a GameplayEffect to (the pointer is implausible)";
        return false;
    }

    static std::uintptr_t s_geClass = 0;
    static std::uintptr_t s_makeContext = 0;
    static std::uintptr_t s_apply = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;

        s_geClass = resolve_uclass_address("GE_DAEnergySprinting_C");
        if (s_geClass == 0) {
            s_refusal = "the GE_DAEnergySprinting_C UClass is not in the object array, so the game's own "
                        "sprint effect cannot be named and NOTHING was applied";
        }
        if (s_refusal.empty()) {

            const std::uintptr_t declaringClass = resolve_uclass_address("AbilitySystemComponent");
            if (declaringClass == 0) {
                s_refusal = "the AbilitySystemComponent UClass is not in the object array, so the class "
                            "that DECLARES MakeEffectContext and BP_ApplyGameplayEffectToSelf cannot be "
                            "named";
            } else {
                NamedObject makeContext;
                NamedObject apply;
                if (!find_function_owned_by("MakeEffectContext", declaringClass, makeContext)) {
                    s_refusal = "no UFunction named MakeEffectContext is owned by the "
                                "AbilitySystemComponent UClass, so no effect context can be made";
                } else if (!find_function_owned_by("BP_ApplyGameplayEffectToSelf", declaringClass, apply)) {
                    s_refusal = "no UFunction named BP_ApplyGameplayEffectToSelf is owned by the "
                                "AbilitySystemComponent UClass, so nothing can be applied";
                } else {
                    s_makeContext = makeContext.address;
                    s_apply = apply.address;
                }
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) {
            note = "the actor is NOT a live object (" + whyNot + "), so no effect was applied";
            return false;
        }
    }
    std::uintptr_t abilitySystem = 0;
    if (!read_process_bytes(actor + kCharacterAbilitySystemOffset, &abilitySystem,
                            sizeof(abilitySystem)) ||
        abilitySystem < 0x10000) {
        note = "the puppet's AbilitySystem is NULL, so there is no component to apply an "
               "effect to";
        return false;
    }
    if (!is_ability_system(abilitySystem)) {
        note = "the pointer at does not lead to an AbilitySystemComponent (its class chain has "
               "no such name), so BP_ApplyGameplayEffectToSelf was NOT called on it";
        return false;
    }

    unsigned char context[0x18] = {0};
    s_pe(reinterpret_cast<void*>(abilitySystem), reinterpret_cast<void*>(s_makeContext), context);
    {
        std::uint64_t pointerAt8 = 0;
        std::uint64_t pointerAt16 = 0;
        std::memcpy(&pointerAt8, context + 0x08, sizeof(pointerAt8));
        std::memcpy(&pointerAt16, context + 0x10, sizeof(pointerAt16));
        if (pointerAt8 < 0x10000 || pointerAt16 < 0x10000) {
            char hex[0x18 * 3 + 1] = {0};
            for (std::size_t i = 0; i < sizeof(context); ++i) {
                std::snprintf(hex + i * 3, 4, "%02X ", context[i]);
            }
            char words[96];
            std::snprintf(words, sizeof(words), " = 0x%llX, = 0x%llX",
                          static_cast<unsigned long long>(pointerAt8),
                          static_cast<unsigned long long>(pointerAt16));
            note = "MakeEffectContext did NOT produce a usable FGameplayEffectContextHandle: the 0x18 bytes "
                   "are [" + std::string(hex) + "] and the two words this build reads as its TSharedPtr (" +
                   words + ") are not both plausible pointers -- so BP_ApplyGameplayEffectToSelf was NOT "
                   "called";
            return false;
        }
    }

    unsigned char frame[0x30] = {0};
    std::memcpy(frame + 0x00, &s_geClass, sizeof(s_geClass));
    const float level = 1.0f;
    std::memcpy(frame + 0x08, &level, sizeof(level));
    std::memcpy(frame + 0x10, context, sizeof(context));
    s_pe(reinterpret_cast<void*>(abilitySystem), reinterpret_cast<void*>(s_apply), frame);

    std::uint64_t handle = 0;
    std::memcpy(&handle, frame + 0x28, sizeof(handle));
    if (handle == 0) {
        note = "BP_ApplyGameplayEffectToSelf returned a ZERO FActiveGameplayEffectHandle -- the game did "
               "not create an active effect (filters, the class, or this component's ability actor info), "
               "so there is nothing to remove later and nothing was applied";
        return false;
    }
    handleOut = handle;
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "the game's own GE_DAEnergySprinting_C was applied to the puppet's ability system "
                  "(0x%llX) through MakeEffectContext + BP_ApplyGameplayEffectToSelf; the returned "
                  "FActiveGameplayEffectHandle is 0x%llX",
                  static_cast<unsigned long long>(abilitySystem),
                  static_cast<unsigned long long>(handle));
    note = buf;
    return true;
}

bool character_remove_sprint_effect(std::uintptr_t actor, std::uint64_t handle, std::string& note) {
    note.clear();
    if (actor < 0x10000) {
        note = "there is no puppet to remove a GameplayEffect from (the pointer is implausible)";
        return false;
    }
    if (handle == 0) {
        note = "the handle is zero, so there is no effect this side can name as its own";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("AbilitySystemComponent");
        if (declaringClass == 0) {
            s_refusal = "the AbilitySystemComponent UClass is not in the object array, so the class that "
                        "DECLARES RemoveActiveGameplayEffect cannot be named";
        } else {
            NamedObject function;
            if (!find_function_owned_by("RemoveActiveGameplayEffect", declaringClass, function)) {
                s_refusal = "no UFunction named RemoveActiveGameplayEffect is owned by the "
                            "AbilitySystemComponent UClass, so the effect cannot be taken off";
            } else {
                s_fn = function.address;
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) {
            note = "the actor is NOT a live object (" + whyNot + "), so no effect was removed";
            return false;
        }
    }
    std::uintptr_t abilitySystem = 0;
    if (!read_process_bytes(actor + kCharacterAbilitySystemOffset, &abilitySystem,
                            sizeof(abilitySystem)) ||
        abilitySystem < 0x10000) {
        note = "the puppet's AbilitySystem is NULL, so there is nothing to remove the effect "
               "from";
        return false;
    }
    if (!is_ability_system(abilitySystem)) {
        note = "the pointer at does not lead to an AbilitySystemComponent, so "
               "RemoveActiveGameplayEffect was NOT called on it";
        return false;
    }

    unsigned char frame[0x10] = {0};
    std::memcpy(frame + 0x00, &handle, sizeof(handle));
    const std::int32_t stacks = 0;
    std::memcpy(frame + 0x08, &stacks, sizeof(stacks));
    s_pe(reinterpret_cast<void*>(abilitySystem), reinterpret_cast<void*>(s_fn), frame);
    if (frame[0x0C] == 0) {
        note = "RemoveActiveGameplayEffect returned FALSE -- the handle is no longer active on this "
               "component (an effect that already expired, or a component that was rebuilt)";
        return false;
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "RemoveActiveGameplayEffect returned TRUE for handle 0x%llX on the ability system 0x%llX",
                  static_cast<unsigned long long>(handle),
                  static_cast<unsigned long long>(abilitySystem));
    note = buf;
    return true;
}

bool character_call_sprint_ability(std::uintptr_t actor, std::string& note) {
    note.clear();
    if (actor < 0x10000) {
        note = "there is no puppet to run a sprint ability on (the pointer is implausible)";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("AbilitySystemComponent");
        if (declaringClass == 0) {
            s_refusal = "the AbilitySystemComponent UClass is not in the object array, so the class that "
                        "DECLARES TryActivateAbilityByClass cannot be named";
        } else {
            NamedObject function;
            if (!find_function_owned_by("TryActivateAbilityByClass", declaringClass, function)) {
                s_refusal = "no UFunction named TryActivateAbilityByClass is owned by the "
                            "AbilitySystemComponent UClass";
            } else {
                s_fn = function.address;
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) {
            note = "the actor is NOT a live object (" + whyNot + "), so the sprint ability was NOT asked for";
            return false;
        }
    }

    constexpr std::uintptr_t kSprintAbilityOffset = 0x08B0;
    std::uintptr_t abilityClass = 0;
    if (!read_process_bytes(actor + kSprintAbilityOffset, &abilityClass, sizeof(abilityClass)) ||
        abilityClass < 0x10000) {
        note = "the character's SprintAbility is NULL or does not read as a pointer, so there is "
               "no class to activate -- this is a fact about the puppet's own configuration, not a refusal "
               "by the ability system";
        return false;
    }
    const std::string className = object_own_name(abilityClass);

    std::uintptr_t abilitySystem = 0;
    if (!read_process_bytes(actor + kCharacterAbilitySystemOffset, &abilitySystem,
                            sizeof(abilitySystem)) ||
        abilitySystem < 0x10000) {
        note = "the puppet's AbilitySystem is NULL, so there is no component to activate an "
               "ability on";
        return false;
    }
    if (!is_ability_system(abilitySystem)) {
        note = "the pointer at does not lead to an AbilitySystemComponent, so "
               "TryActivateAbilityByClass was NOT called on it";
        return false;
    }

    unsigned char frame[0x10] = {0};
    std::memcpy(frame + 0x00, &abilityClass, sizeof(abilityClass));
    const unsigned char allowRemote = 1;
    std::memcpy(frame + 0x08, &allowRemote, sizeof(allowRemote));
    s_pe(reinterpret_cast<void*>(abilitySystem), reinterpret_cast<void*>(s_fn), frame);

    const bool activated = frame[0x09] != 0;
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "TryActivateAbilityByClass('%s' 0x%llX) on the ability system 0x%llX returned %s "
                  "(bAllowRemoteActivation=1) -- this is the class the CHARACTER names at, not a "
                  "class resolved by name",
                  className.empty() ? "<unnamed>" : className.c_str(),
                  static_cast<unsigned long long>(abilityClass),
                  static_cast<unsigned long long>(abilitySystem),
                  activated ? "TRUE" : "FALSE");
    note = buf;
    return true;
}

bool object_from_weak_index(std::uint32_t index, std::uint32_t serial, std::uintptr_t& out,
                           std::string& note) {
    using namespace dsh::reflect::offsets;
    out = 0;
    note.clear();
    const std::uintptr_t base = main_module_base();
    if (base == 0) {
        note = "no module base, so the object array is not reachable";
        return false;
    }
    const std::uintptr_t array = base + static_cast<std::uintptr_t>(kGObjects);
    std::uint64_t chunkTable = 0;
    if (!try_read_u64(array + kObjectsOffset, chunkTable) || chunkTable < 0x10000) {
        note = "the object array's chunk table is not readable";
        return false;
    }
    std::uint32_t numElements = 0;
    if (!try_read_u32(array + kNumElementsOffset, numElements)) {
        note = "the object array's element count is not readable";
        return false;
    }
    if (index >= numElements) {
        note = "index " + std::to_string(index) + " is outside the array's " +
               std::to_string(numElements) + " element(s)";
        return false;
    }
    const std::uint32_t chunkIndex = index / kElementsPerChunk;
    const std::uint32_t within = index % kElementsPerChunk;
    std::uint64_t chunk = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunkTable) + static_cast<std::uintptr_t>(chunkIndex) * 8,
                      chunk) || chunk < 0x10000) {
        note = "chunk " + std::to_string(chunkIndex) + " is not readable";
        return false;
    }
    const auto item = static_cast<std::uintptr_t>(chunk) +
                      static_cast<std::uintptr_t>(within) * kObjectItemStride;
    std::uint64_t object = 0;
    if (!try_read_u64(item, object) || object < 0x10000) {
        note = "the slot holds no object";
        return false;
    }
    std::uint32_t slotSerial = 0;
    if (try_read_u32(item + 0x10, slotSerial) && slotSerial != serial) {
        note = "the slot's serial " + std::to_string(slotSerial) + " is not the weak pointer's " +
               std::to_string(serial) + " -- the binding is STALE (the slot was reused)";
        return false;
    }
    out = static_cast<std::uintptr_t>(object);
    return true;
}

namespace {

bool delegate_list_text(std::uintptr_t actor, std::uintptr_t offset, const char* label, std::string& out) {
    std::uint64_t data = 0;
    std::uint32_t num = 0;
    std::uint32_t max = 0;
    if (!read_process_bytes(actor + offset, &data, sizeof(data)) ||
        !read_process_bytes(actor + offset + 0x08, &num, sizeof(num)) ||
        !read_process_bytes(actor + offset + 0x0C, &max, sizeof(max))) {
        out += std::string(label) + ": NOT readable; ";
        return false;
    }
    out += std::string(label) + ": Num=" + std::to_string(num) + " Max=" + std::to_string(max);
    if (num > 8) {
        out += " (Num is implausible for a delegate -- nothing read)";
        out += "; ";
        return true;
    }
    for (std::uint32_t i = 0; i < num; ++i) {
        const auto bind = static_cast<std::uintptr_t>(data) + static_cast<std::uintptr_t>(i) * 0x10u;
        std::uint32_t objectIndex = 0;
        std::uint32_t serial = 0;
        std::uint32_t nameIndex = 0;
        std::uint32_t nameNumber = 0;
        if (!read_process_bytes(bind + 0x00, &objectIndex, sizeof(objectIndex)) ||
            !read_process_bytes(bind + 0x04, &serial, sizeof(serial)) ||
            !read_process_bytes(bind + 0x08, &nameIndex, sizeof(nameIndex)) ||
            !read_process_bytes(bind + 0x0C, &nameNumber, sizeof(nameNumber))) {
            out += " [i=" + std::to_string(i) + ": NOT readable]";
            continue;
        }
        std::string functionName;
        std::string nameNote;
        const bool named = resolve_fname(nameIndex, functionName, nameNote);
        std::string objectText;
        {
            std::uintptr_t bound = 0;
            std::string bindNote;
            if (!object_from_weak_index(objectIndex, serial, bound, bindNote) || bound == 0) {
                objectText = "obj NOT resolved (" + bindNote + ")";
            } else {
                const std::string ownName = object_own_name(bound);
                const std::string className = object_class_name(bound);
                objectText = "obj '" + (ownName.empty() ? std::string("<unnamed>") : ownName) + "' [" +
                             (className.empty() ? std::string("<no class>") : className) + "]";
            }
        }
        out += " [i=" + std::to_string(i) + " " + objectText + " fn=" +
               (named ? ("'" + functionName + "'") : ("NOT resolved (" + nameNote + ")")) + "]";
    }
    out += "; ";
    return true;
}

}
bool character_sprint_delegate_text(std::uintptr_t actor, std::string& out) {
    out.clear();
    if (actor < 0x10000) {
        out = "no actor";
        return false;
    }
    bool anyRead = false;
    anyRead = delegate_list_text(actor, 0x07A8, "OnSprintingStarted", out) || anyRead;
    anyRead = delegate_list_text(actor, 0x07B8, "OnSprintingEnded", out) || anyRead;
    return anyRead;
}

bool character_exclusive_delegate_text(std::uintptr_t actor, std::string& out) {
    out.clear();
    if (actor < 0x10000) {
        out = "no actor";
        return false;
    }
    bool anyRead = false;
    anyRead = delegate_list_text(actor, 0x0FF0, "OnExclusiveModeStarted", out) || anyRead;
    anyRead = delegate_list_text(actor, 0x1000, "OnExclusiveModeEnded", out) || anyRead;
    anyRead = delegate_list_text(actor, 0x1010, "OnExclusiveTypesChanged", out) || anyRead;
    return anyRead;
}

bool character_start_exclusive_mode(std::uintptr_t actor, std::uintptr_t owner, std::uint8_t types,
                                    std::string& note) {
    note.clear();
    if (actor < 0x10000) {
        note = "there is no puppet to start an exclusive mode on (the pointer is implausible)";
        return false;
    }
    if (owner < 0x10000) {
        note = "the owner this side would have to name is NULL, so there is nothing to start";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("DACharacter");
        if (declaringClass == 0) {
            s_refusal = "the DACharacter UClass is not in the object array, so the class that DECLARES "
                        "K2_StartExclusiveMode cannot be named";
        } else {
            NamedObject function;
            if (!find_function_owned_by("K2_StartExclusiveMode", declaringClass, function)) {
                s_refusal = "no UFunction named K2_StartExclusiveMode is owned by the DACharacter UClass";
            } else {
                s_fn = function.address;
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) {
            note = "the actor is NOT a live object (" + whyNot + "), so the mode was NOT started";
            return false;
        }
    }

    unsigned char frame[0x10] = {0};
    std::memcpy(frame + 0x00, &owner, sizeof(owner));
    const std::int32_t typesValue = static_cast<std::int32_t>(types);
    std::memcpy(frame + 0x08, &typesValue, sizeof(typesValue));
    s_pe(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(s_fn), frame);

    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "K2_StartExclusiveMode(owner 0x%llX, types %u) on the puppet returned %s -- this is the "
                  "game's own initiator, the call its own player makes when the weapon unfolds",
                  static_cast<unsigned long long>(owner), static_cast<unsigned>(types),
                  frame[0x0C] != 0 ? "TRUE" : "FALSE");
    note = buf;
    return true;
}

bool character_end_exclusive_mode(std::uintptr_t actor, std::string& note) {
    note.clear();
    if (actor < 0x10000) {
        note = "there is no puppet to end an exclusive mode on (the pointer is implausible)";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("DACharacter");
        if (declaringClass == 0) {
            s_refusal = "the DACharacter UClass is not in the object array, so the class that DECLARES "
                        "EndExclusiveMode cannot be named";
        } else {
            NamedObject function;
            if (!find_function_owned_by("EndExclusiveMode", declaringClass, function)) {
                s_refusal = "no UFunction named EndExclusiveMode is owned by the DACharacter UClass";
            } else {
                s_fn = function.address;
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) {
            note = "the actor is NOT a live object (" + whyNot + "), so the mode was NOT ended";
            return false;
        }
    }

    unsigned char frame[0x10] = {0};
    s_pe(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(s_fn), frame);

    char buf[192];
    std::snprintf(buf, sizeof(buf), "EndExclusiveMode on the puppet returned %s",
                  frame[0x00] != 0 ? "TRUE" : "FALSE");
    note = buf;
    return true;
}

bool module_call_arrival_notify(std::uintptr_t machine, ModuleArrivalResult& out) {
    out = ModuleArrivalResult{};
    if (machine < 0x10000) {
        out.note = "there is no machine actor to hand the module arrival to (the pointer is implausible)";
        return false;
    }

    const char* const* const kSlots = module_names_rep_notify_slots();

    const std::uintptr_t declaringClass = resolve_uclass_address("DAMachine");
    if (declaringClass == 0) {
        out.note = "the DAMachine UClass is not in the object array, so the class that DECLARES the "
                   "module OnRep notifications cannot be named and NONE of the nine is looked up";
        return false;
    }

    std::uintptr_t functions[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 9; ++i) {
        NamedObject function;
        const std::string functionName = std::string("OnRep_") + kSlots[i];
        if (find_function_owned_by(functionName.c_str(), declaringClass, function)) {
            functions[i] = function.address;
            continue;
        }
        if (!out.unresolved.empty()) out.unresolved += ", ";
        out.unresolved += std::string(kSlots[i]) + " (no UFunction named " + functionName +
                          " is owned by the DAMachine UClass)";
    }
    const ProcessEventResolution pe = resolve_process_event(live_world_address());
    if (!pe.usable) {
        out.note = "ProcessEvent is not usable (" + pe.note + "), so the module OnRep notifications "
                   "cannot be called" + (out.unresolved.empty() ? std::string()
                                                                 : ("; unresolved: " + out.unresolved));
        return false;
    }
    const ProcessEventFn processEventFn = reinterpret_cast<ProcessEventFn>(pe.address);

    unsigned char noFrame[0x10] = {0};

    int called = 0;
    for (std::size_t i = 0; i < 9; ++i) {
        if (functions[i] == 0) continue;
        processEventFn(reinterpret_cast<void*>(machine), reinterpret_cast<void*>(functions[i]), noFrame);
        out.callMade[i] = true;
        ++called;
    }
    return called > 0;
}

const char* const* module_names_rep_notify_slots() {
    static const char* const kSlots[9] = {
        "Mobility", "Humanoid", "Headset", "Repeater", "Thruster",
        "Utility", "Hanger", "LeftRearWeapon", "RightRearWeapon",
    };
    return kSlots;
}

bool machine_call_equip_front_weapon(std::uintptr_t machine, std::uintptr_t newFrontWeapon,
                                     std::string& note) {
    note.clear();
    if (machine < 0x10000) {
        note = "there is no machine to call EquipFrontWeapon on (the pointer is implausible)";
        return false;
    }
    if (newFrontWeapon < 0x10000) {

        note = "the front weapon module to equip is a null or implausible pointer, so EquipFrontWeapon "
               "is NOT called with it";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t declaringClass = resolve_uclass_address("DAMachine");
        if (declaringClass == 0) {
            s_refusal = "the DAMachine UClass is not in the object array, so the class that DECLARES "
                        "EquipFrontWeapon cannot be named and the function is NOT looked up";
        } else {
            NamedObject function;
            if (!find_function_owned_by("EquipFrontWeapon", declaringClass, function)) {
                s_refusal = "no UFunction named EquipFrontWeapon is owned by the DAMachine UClass, so "
                            "the game's own front-weapon verb cannot be called";
            } else {
                s_fn = function.address;
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    unsigned char params[0x10] = {0};
    std::memcpy(params + 0x00, &newFrontWeapon, sizeof(newFrontWeapon));
    s_pe(reinterpret_cast<void*>(machine), reinterpret_cast<void*>(s_fn), params);
    return true;
}

std::uintptr_t resolve_uclass_address_uncached(const char* className);

std::uintptr_t resolve_uclass_address(const char* className) {
    DSH_TIME_WALK("resolve_uclass_address");
    if (className == nullptr) return 0;
    static std::mutex cacheMutex;
    static std::unordered_map<std::string, std::uintptr_t> cache;
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        const auto it = cache.find(className);
        if (it != cache.end()) return it->second;
    }
    const std::uintptr_t found = resolve_uclass_address_uncached(className);
    {
        const std::lock_guard<std::mutex> lock(cacheMutex);
        cache.emplace(className, found);
    }
    return found;
}

std::uintptr_t resolve_uclass_address_uncached(const char* className) {
    DSH_TIME_WALK("resolve_uclass_address_uncached");
    if (className == nullptr || *className == '\0') return 0;
    NamedObject cls;
    if (!find_uclass(className, cls)) return 0;
    return cls.address;
}

std::string object_class_name(std::uintptr_t obj) {

    static thread_local bool s_threadProbe = false;
    if (!s_threadProbe) {
        s_threadProbe = true;
        DSH_INFO("thread-probe: object_class_name from thread %u",
                 static_cast<unsigned>(GetCurrentThreadId()));
    }
    return class_name_of(obj);
}

std::string object_own_name(std::uintptr_t obj) {
    return object_name_of(obj);
}

FunctionRef reflect_resolve_function(std::uintptr_t declaringClass, const char* functionName) {
    FunctionRef ref;
    const std::string name = functionName != nullptr ? std::string(functionName) : std::string();

    if (name.empty()) {
        ref.note = "no function name was given, so nothing was looked up";
        return ref;
    }
    if (declaringClass < 0x10000) {
        ref.note = "the UClass that declares '" + name + "' was not resolved (the address is "
                   "implausible), so the function is NOT looked up";
        return ref;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(declaringClass));

    NamedObject function;
    if (!find_function_owned_by(name.c_str(), declaringClass, function)) {

        ref.note = "no UFunction named '" + name + "' is owned by the UClass at " + std::string(buf) +
                   " -- either it does not exist or its DECLARING class is further up the SuperStruct "
                   "chain and must be resolved instead";
        return ref;
    }

    const ProcessEventResolution pe = resolve_process_event(live_world_address());
    if (!pe.usable) {
        ref.note = "ProcessEvent is not usable (" + pe.note + "), so '" + name +
                   "' is resolved but cannot be called";
        return ref;
    }

    ref.function = function.address;
    ref.processEvent = pe.address;
    ref.usable = true;
    ref.note = "'" + name + "' resolved on the UClass at " + std::string(buf);
    return ref;
}

bool reflect_invoke_function(std::uintptr_t self, const FunctionRef& fn, void* params,
                             std::string& note) {
    note.clear();
    if (self < 0x10000) {
        note = "there is no object to call the function on (the pointer is implausible), so the call "
               "was NOT made";
        return false;
    }
    if (!fn.usable || fn.function == 0 || fn.processEvent == 0) {
        note = fn.note.empty() ? std::string("the function was never resolved, so nothing was called")
                               : fn.note;
        return false;
    }
    reinterpret_cast<ProcessEventFn>(fn.processEvent)(reinterpret_cast<void*>(self),
                                                      reinterpret_cast<void*>(fn.function), params);
    return true;
}

std::uintptr_t main_module_base() {
    return main_module().base;
}

std::vector<std::uintptr_t> g_objDiffBefore;

std::string obj_diff_mark() {
    using namespace offsets;
    const ModuleInfo exe = main_module();
    if (exe.base == 0) return "no main module";
    const auto arr = exe.base + static_cast<std::uintptr_t>(kGObjects);
    std::uint64_t chunkTable = 0;
    std::uint32_t numElements = 0;
    if (!try_read_u64(arr + kObjectsOffset, chunkTable) || chunkTable < 0x10000) {
        return "the object table is not readable";
    }
    if (!try_read_u32(arr + kNumElementsOffset, numElements)) return "the object count is unreadable";
    g_objDiffBefore.clear();
    g_objDiffBefore.reserve(numElements);
    for (std::uint32_t i = 0; i < numElements; ++i) {
        const std::uint64_t chunk =
            read_u64_fast(static_cast<std::uintptr_t>(chunkTable) +
                          static_cast<std::uintptr_t>(i / kElementsPerChunk) * 8);
        if (chunk < 0x10000) continue;
        const std::uint64_t obj =
            read_u64_fast(static_cast<std::uintptr_t>(chunk) +
                          static_cast<std::uintptr_t>(i % kElementsPerChunk) * kObjectItemStride);
        if (obj >= 0x10000) g_objDiffBefore.push_back(static_cast<std::uintptr_t>(obj));
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "marked %zu live object(s) -- load now, then run 'objdiff report'",
                  g_objDiffBefore.size());
    return buf;
}

std::string obj_diff_report() {
    using namespace offsets;
    if (g_objDiffBefore.empty()) return "nothing was marked -- run 'objdiff mark' BEFORE loading";
    const ModuleInfo exe = main_module();
    if (exe.base == 0) return "no main module";
    const auto arr = exe.base + static_cast<std::uintptr_t>(kGObjects);
    std::uint64_t chunkTable = 0;
    std::uint32_t numElements = 0;
    if (!try_read_u64(arr + kObjectsOffset, chunkTable) || chunkTable < 0x10000) {
        return "the object table is not readable";
    }
    if (!try_read_u32(arr + kNumElementsOffset, numElements)) return "the object count is unreadable";

    std::unordered_set<std::uintptr_t> before(g_objDiffBefore.begin(), g_objDiffBefore.end());
    std::unordered_map<std::string, int> byClass;

    std::unordered_map<std::string, std::vector<std::string>> samplesByClass;

    std::vector<std::string> skeletalOwners;
    std::vector<std::pair<std::string, std::string>> animInstances;
    int newCount = 0;
    for (std::uint32_t i = 0; i < numElements; ++i) {
        const std::uint64_t chunk =
            read_u64_fast(static_cast<std::uintptr_t>(chunkTable) +
                          static_cast<std::uintptr_t>(i / kElementsPerChunk) * 8);
        if (chunk < 0x10000) continue;
        const std::uint64_t obj =
            read_u64_fast(static_cast<std::uintptr_t>(chunk) +
                          static_cast<std::uintptr_t>(i % kElementsPerChunk) * kObjectItemStride);
        if (obj < 0x10000) continue;
        const auto o = static_cast<std::uintptr_t>(obj);
        if (before.count(o) != 0) continue;
        ++newCount;
        const std::string cls = class_name_of(o);
        ++byClass[cls];
        std::vector<std::string>& bucket = samplesByClass[cls];
        if (bucket.size() < 4) bucket.push_back(object_name_of(o));
        if (cls.find("SkeletalMeshComponent") != std::string::npos) {
            const std::uint64_t outer = read_u64_fast(o + kUObjectOuterOffset);
            if (outer >= 0x10000 && skeletalOwners.size() < 80) {
                skeletalOwners.push_back(class_name_of(static_cast<std::uintptr_t>(outer)) + " -> " +
                                         object_name_of(o));
            }
        } else if (cls.find("AnimInstance") != std::string::npos ||
                   cls.find("AnimBlueprintGeneratedClass") != std::string::npos ||
                   cls.rfind("AnimBP_", 0) == 0) {
            if (animInstances.size() < 40) animInstances.emplace_back(cls, object_name_of(o));
        }
    }

    std::vector<std::pair<std::string, int>> ranked(byClass.begin(), byClass.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    std::string dumpPath;
    {
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) > 0) {
            std::string dir(exePath);
            const auto slash = dir.find_last_of("\\/");
            if (slash != std::string::npos) dir = dir.substr(0, slash + 1);
            dumpPath = dir + "dsh-objdiff.txt";
            if (FILE* f = std::fopen(dumpPath.c_str(), "w")) {
                std::fprintf(f, "new objects: %d, distinct classes: %zu\n", newCount, byClass.size());
                for (const auto& kv : ranked) std::fprintf(f, "%8d %s\n", kv.second, kv.first.c_str());
                std::fprintf(f, "\n--- every new SkeletalMeshComponent, by owning actor class ---\n");
                for (const auto& o : skeletalOwners) std::fprintf(f, "%s\n", o.c_str());
                std::fprintf(f, "\n--- new animation instances and anim classes ---\n");
                for (const auto& a : animInstances) std::fprintf(f, "%s %s\n", a.first.c_str(), a.second.c_str());
                std::fclose(f);
                DSH_INFO("objdiff: the full list is in %s", dumpPath.c_str());
            } else {
                DSH_WARN("objdiff: could not write %s", dumpPath.c_str());
            }
        }
    }

    int shown = 0;
    for (const auto& kv : ranked) {
        const std::string& c = kv.first;
        if (c.find("Skeletal") == std::string::npos && c.find("Anim") == std::string::npos &&
            c.find("Doll") == std::string::npos && c.find("Humanoid") == std::string::npos &&
            c.find("AbilitySystem") == std::string::npos &&
            c.find("AttributeSet") == std::string::npos && c.find("Pawn") == std::string::npos &&
            c.find("CharacterMovement") == std::string::npos && c.rfind("BP_", 0) != 0 &&
            c.rfind("DA", 0) != 0) {
            continue;
        }
        if (shown++ >= 70) break;
        DSH_INFO("objdiff: %-52s x%d", c.c_str(), kv.second);
    }
    for (std::size_t i = 0; i < skeletalOwners.size() && i < 24; ++i) {
        DSH_INFO("objdiff: skeletal owner: %s", skeletalOwners[i].c_str());
    }
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%d new object(s) appeared, in %zu distinct class(es)",
                  newCount, byClass.size());
    DSH_INFO("objdiff: %s", buf);
    return buf;
}

bool puppet_apply_actor_pose(std::uintptr_t actor, const float posM[3], float yawRad, float pitchRad) {
    using namespace offsets;
    if (actor < 0x10000) return false;

    {
        std::string whyNot;
        if (!object_is_still_registered(actor, whyNot)) return false;
    }

    static std::uintptr_t   s_setPose  = 0;
    static ProcessEventFn   s_pe       = nullptr;
    static std::uint64_t    s_retryAt  = 0;
    static bool             s_warned   = false;

    if (s_pe == nullptr || s_setPose == 0) {
        const std::uint64_t nowMs = GetTickCount64();
        if (nowMs < s_retryAt) return false;
        s_retryAt = nowMs + 2000;

        NamedObject actorClass, setPoseFn;
        if (!find_uclass("Actor", actorClass) ||
            !find_function_owned_by("K2_SetActorLocationAndRotation", actorClass.address, setPoseFn)) {
            if (!s_warned) {
                s_warned = true;
                DSH_WARN("actor-pose: Actor.K2_SetActorLocationAndRotation is not resolvable, so the "
                         "puppet cannot be placed or turned -- retried every 2 s, not on every update");
            }
            return false;
        }
        const ProcessEventResolution pe = resolve_process_event(live_world_address());
        if (!pe.usable) {
            if (!s_warned) {
                s_warned = true;
                DSH_WARN("actor-pose: ProcessEvent is not usable (%s), so the puppet cannot be "
                         "placed or turned -- retried every 2 s", pe.note.c_str());
            }
            return false;
        }

        s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
        s_setPose = setPoseFn.address;
    }

    constexpr float kRadToDeg = 180.0f / 3.14159265358979f;

    ParamBuffer prm;
    prm.set<float>(0x00, posM[0] * 100.0f);
    prm.set<float>(0x04, posM[1] * 100.0f);
    prm.set<float>(0x08, posM[2] * 100.0f);

    prm.set<float>(0x0C, pitchRad * kRadToDeg);
    prm.set<float>(0x10, yawRad * kRadToDeg);
    prm.set<float>(0x14, 0.0f);
    prm.set<std::uint8_t>(0x18, 0);
    prm.set<std::uint8_t>(0xA4, 1);
    s_pe(reinterpret_cast<void*>(actor), reinterpret_cast<void*>(s_setPose), prm.data());

    return prm.get<std::uint8_t>(0xA5) != 0;
}

constexpr const char* kDollApiClass = "DAHumanoid";

std::uintptr_t doll_api_class() {
    static std::uintptr_t s = 0;
    if (s != 0) return s;
    NamedObject cls;
    if (!find_uclass(kDollApiClass, cls)) return 0;
    s = cls.address;
    return s;
}

bool actor_is_doll(std::uintptr_t actor) {
    if (actor < 0x10000) return false;
    const std::uintptr_t api = doll_api_class();
    if (api == 0) return false;

    std::uint64_t classValue = 0;
    if (!try_read_u64(actor + offsets::kUObjectClassOffset, classValue)) return false;
    std::uintptr_t cls = static_cast<std::uintptr_t>(classValue);

    for (int depth = 0; cls >= 0x10000 && depth < 32; ++depth) {
        if (cls == api) return true;
        std::uint64_t super = 0;
        if (!try_read_u64(cls + offsets::kUStructSuperOffset, super)) return false;
        cls = static_cast<std::uintptr_t>(super);
    }
    return false;
}

bool read_process_bytes(std::uintptr_t address, void* dst, std::size_t bytes) {
    if (address < 0x10000 || dst == nullptr || bytes == 0) return false;
    return try_read_bytes(address, dst, bytes);
}

bool write_process_bytes(std::uintptr_t address, const void* src, std::size_t bytes) {
    if (address < 0x10000 || src == nullptr || bytes == 0) return false;

    const std::uintptr_t end = address + bytes - 1;
    if (end < address) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & writable) == 0) return false;
    if (end > reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize - 1) return false;

    std::memcpy(reinterpret_cast<void*>(address), src, bytes);
    return true;
}

namespace {
constexpr std::size_t kColoringBytes = 13 * 4 * sizeof(float);
constexpr std::size_t kFigureBytes   = 5 * sizeof(float);

constexpr std::uintptr_t kMachineHumanoidOffset = 0x15E0;

bool customize_values_plausible(const float* values, std::size_t count, std::string& note) {
    for (std::size_t i = 0; i < count; ++i) {
        const float v = values[i];
        if (!std::isfinite(v) || v < -8.0f || v > 8.0f) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "value %zu of %zu is implausible (%g) -- the struct was read at the wrong "
                          "offset or is not initialised, so nothing is sent",
                          i, count, static_cast<double>(v));
            note = buf;
            return false;
        }
    }
    return true;
}
}
bool capture_doll_customize(std::uintptr_t machineOrDollActor, DollCustomize& out,
                            std::string& note) {
    using namespace offsets;
    out = DollCustomize{};
    note.clear();

    if (machineOrDollActor < 0x10000) {
        note = "no actor to read the customisation from";
        return false;
    }

    std::uintptr_t dollActor = machineOrDollActor;
    if (!actor_is_doll(dollActor)) {
        std::uint64_t humanoidValue = 0;
        if (!try_read_u64(machineOrDollActor + kMachineHumanoidOffset, humanoidValue) ||
            humanoidValue < 0x10000) {
            note = "the actor is '" + class_name_of(machineOrDollActor) +
                   "', which owns no SetColoringData, and its ADAMachine::Humanoid is null "
                   "or unreadable -- there is no doll to read";
            return false;
        }
        const auto humanoid = static_cast<std::uintptr_t>(humanoidValue);
        if (!actor_is_doll(humanoid)) {
            note = "ADAMachine::Humanoid on '" + class_name_of(machineOrDollActor) +
                   "' points at '" + class_name_of(humanoid) +
                   "', which owns no SetColoringData -- so is not the humanoid on this build, "
                   "and nothing is sent";
            return false;
        }
        dollActor = humanoid;
    }

    if (!try_read_bytes(dollActor + 0x1338, out.coloring, kColoringBytes)) {
        note = "FDAHumanoidColoringData at could not be read";
        out = DollCustomize{};
        return false;
    }
    if (!try_read_bytes(dollActor + 0x1408, out.figure, kFigureBytes)) {
        note = "FDAHumanoidFigureData at could not be read";
        out = DollCustomize{};
        return false;
    }

    if (!customize_values_plausible(&out.coloring[0][0], 13 * 4, note) ||
        !customize_values_plausible(out.figure, 5, note)) {
        out = DollCustomize{};
        return false;
    }
    return true;
}

namespace {

constexpr std::uintptr_t kMachineTriggerDataOffset  = 0x1300;
constexpr std::uintptr_t kMachineAssembleDataOffset = 0x1308;
constexpr std::uintptr_t kMachineCustomizeDataOffset = 0x1680;
constexpr std::uintptr_t kMachineSlotStride         = 0x10;
constexpr int            kMachineSlotCount          = 9;

constexpr std::uintptr_t kHumanoidCustomizeDataOffset = 0x1318;
constexpr std::uintptr_t kHumanoidInverseOffset = kHumanoidCustomizeDataOffset + 0x104;

constexpr const char* kMachineSlotNames[kMachineSlotCount] = {
    "Hanger", "Headset", "Mobility", "Thruster", "Utility",
    "PrimaryFrontWeapon", "SecondaryFrontWeapon", "LeftRearWeapon", "RightRearWeapon",
};
constexpr const char* kPartsItemNames[4] = {"Body", "Face", "FrontHair", "BackHair"};

constexpr const char* kModuleColorNames[9] = {
    "Hanger", "Headset", "Mobility", "Thruster", "Utility",
    "PrimaryFrontWeapon", "SecondaryFrontWeapon", "LeftRearWeapon", "RightRearWeapon",
};

constexpr std::uintptr_t kMachineModuleColorOffset = kMachineAssembleDataOffset + 0x90;
constexpr std::uintptr_t kModuleColorStride       = 0x40;

struct ModuleField {
    std::uintptr_t offset;
    const char*    name;
};
constexpr ModuleField kMachineModuleFields[10] = {
    {0x15D8, "Mobility"},          {0x15E0, "Humanoid"},
    {0x15E8, "Headset"},           {0x15F0, "Repeater"},
    {0x15F8, "Thruster"},          {0x1600, "Utility"},
    {0x1608, "Hanger"},            {0x1610, "LeftRearWeapon"},
    {0x1618, "RightRearWeapon"},   {0x1630, "CurrentFrontWeapon"},
};

}

std::string hex_ptr(std::uintptr_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

const char* machine_slot_name(int index) {
    if (index < 0 || index >= kMachineSlotCount) return "?";
    return kMachineSlotNames[index];
}

const char* module_color_name(int index) {
    if (index < 0 || index >= kModuleColorCount) return "?";
    return kModuleColorNames[index];
}

MachineModuleState read_machine_modules(std::uintptr_t machineActor) {
    MachineModuleState out;
    for (int i = 0; i < 10; ++i) {
        out.entries[i].offset = kMachineModuleFields[i].offset;
        out.entries[i].name = kMachineModuleFields[i].name;
        if (machineActor < 0x10000) continue;
        std::uint64_t value = 0;
        if (!try_read_u64(machineActor + kMachineModuleFields[i].offset, value)) continue;
        const auto pointer = static_cast<std::uintptr_t>(value);
        if (pointer < 0x10000) continue;
        out.entries[i].pointer = pointer;

        out.entries[i].className = class_name_of(pointer);
        ++out.nonNull;
    }
    return out;
}

bool capture_machine_loadout(std::uintptr_t machineActor, MachineLoadout& out, std::string& note) {
    using namespace offsets;
    out = MachineLoadout{};
    note.clear();

    if (machineActor < 0x10000) {
        note = "no machine actor to read the loadout from";
        return false;
    }

    std::uint64_t classValue = 0;
    if (!try_read_u64(machineActor + kUObjectClassOffset, classValue) || classValue < 0x10000) {
        note = "the machine actor's class pointer at could not be read";
        return false;
    }
    const auto machineClass = static_cast<std::uintptr_t>(classValue);
    out.className = class_name_of(machineActor);
    out.classPath = object_path_name(machineClass);
    if (out.className.empty()) {
        note = "the machine actor at " + hex_ptr(machineActor) +
               " has no resolvable class name";
        return false;
    }
    if (out.classPath.empty()) {
        note = "the machine class '" + out.className +
               "' has no resolvable path, so the far side could not load it by name";
        return false;
    }

    if (!try_read_bytes(machineActor + kMachineTriggerDataOffset, out.trigger, sizeof(out.trigger))) {
        note = "FDATriggerData at could not be read";
        return false;
    }

    for (int i = 0; i < kMachineSlotCount; ++i) {
        const std::uintptr_t base = machineActor + kMachineAssembleDataOffset +
                                    static_cast<std::uintptr_t>(i) * kMachineSlotStride;
        std::uint64_t slotClass = 0;
        if (!try_read_u64(base, slotClass)) {
            note = std::string("FDAAssembleData slot ") + std::to_string(i) + " (" +
                   kMachineSlotNames[i] + ") could not be read at " + hex_ptr(base);
            return false;
        }
        MachineLoadout::Slot& slot = out.slots[i];
        if (slotClass < 0x10000) continue;
        std::uint32_t level = 0;
        if (!try_read_u32(base + 0x08, level)) {
            note = std::string("FDAAssembleData slot ") + std::to_string(i) + " (" +
                   kMachineSlotNames[i] + ") has a class but its ModuleLevel at " +
                   hex_ptr(base + 0x08) + " could not be read";
            return false;
        }
        const auto slotClassAddress = static_cast<std::uintptr_t>(slotClass);

        const std::string slotObjectClass = class_name_of(slotClassAddress);
        if (!is_uclass(slotObjectClass)) {
            note = std::string("FDAAssembleData slot ") + std::to_string(i) + " (" +
                   kMachineSlotNames[i] + ") holds " + hex_ptr(slotClassAddress) + ", whose class is '" +
                   (slotObjectClass.empty() ? std::string("<unnamed>") : slotObjectClass) +
                   "' and not a UClass -- so + i*0x10 is not the slot layout on this build, "
                   "and nothing is sent";
            return false;
        }
        slot.classPath = object_path_name(slotClassAddress);
        slot.className = object_name_of(slotClassAddress);
        if (slot.classPath.empty() || slot.className.empty()) {
            note = std::string("FDAAssembleData slot ") + std::to_string(i) + " (" +
                   kMachineSlotNames[i] + ") holds " + hex_ptr(slotClassAddress) +
                   ", which has no resolvable name or path -- a class we cannot name cannot be sent";
            return false;
        }
        slot.present = true;
        slot.level = static_cast<int>(level);
    }

    std::string caveat;
    std::uint64_t humanoidValue = 0;
    std::uintptr_t humanoid = 0;
    if (try_read_u64(machineActor + kMachineHumanoidOffset, humanoidValue)) {
        humanoid = static_cast<std::uintptr_t>(humanoidValue);
    }
    if (humanoid >= 0x10000 && actor_is_doll(humanoid)) {
        for (int k = 0; k < 4; ++k) {
            std::uint64_t itemValue = 0;
            if (!try_read_u64(humanoid + kHumanoidCustomizeDataOffset +
                                  static_cast<std::uintptr_t>(k) * 8,
                              itemValue)) {
                caveat += std::string("the ") + kPartsItemNames[k] +
                          " part class could not be read; ";
                continue;
            }
            if (itemValue < 0x10000) continue;
            const auto itemClass = static_cast<std::uintptr_t>(itemValue);
            out.partsItemName[k] = object_name_of(itemClass);
            out.partsItemPath[k] = object_path_name(itemClass);
            if (out.partsItemName[k].empty() || out.partsItemPath[k].empty() ||
                !is_uclass(class_name_of(itemClass))) {
                caveat += std::string("the ") + kPartsItemNames[k] + " part class " +
                          hex_ptr(itemClass) + " has no resolvable name or path; ";
                out.partsItemName[k].clear();
                out.partsItemPath[k].clear();
            }
        }
        if (!try_read_bytes(humanoid + kHumanoidInverseOffset, out.inverseMesh,
                            sizeof(out.inverseMesh))) {
            caveat += "the three bInverse*Mesh flags could not be read; ";
        }
        std::string customizeNote;
        if (capture_doll_customize(humanoid, out.customize, customizeNote)) {
            out.customizePresent = true;
        } else {

            out.customizePresent = false;
            caveat += "the doll's colours and figure values could not be read (" + customizeNote +
                      "); ";
        }
    } else {
        caveat += "the machine '" + out.className + "' reports no ADAHumanoid at " +
                  hex_ptr(machineActor + kMachineHumanoidOffset) +
                  " (that field is " + hex_ptr(humanoid) +
                  "), so FDACustomizeData could not be read and the doll will wear the class's default "
                  "body; ";
    }

    if (try_read_bytes(machineActor + kMachineModuleColorOffset, out.moduleColoring,
                       kModuleColorBytes)) {
        std::string colorNote;
        if (customize_values_plausible(&out.moduleColoring[0][0][0],
                                       kModuleColorBytes / sizeof(float), colorNote)) {
            out.moduleColoringPresent = true;
        } else {
            caveat += "the machine's FDAAssembleData::ColoringData (the ARMOUR SHELL, 576 bytes at " +
                      hex_ptr(machineActor + kMachineModuleColorOffset) + ") is implausible (" +
                      colorNote +
                      ") -- a block read at the wrong offset would paint the shell with nonsense, so "
                      "nothing is sent for it and the shell keeps the class's default colours; ";
        }
    } else {
        caveat += "the machine's FDAAssembleData::ColoringData (the ARMOUR SHELL, 576 bytes at " +
                  hex_ptr(machineActor + kMachineModuleColorOffset) +
                  ") could not be read, so the shell's colours are not sent and the shell keeps the "
                  "class's default colours; ";
    }

    note = caveat;
    return true;
}

bool spawn_machine_with_loadout(const MachineLoadout& loadout, std::uintptr_t& actorOut,
                                std::string& report, std::string& note) {
    using namespace offsets;
    actorOut = 0;
    report.clear();
    note.clear();

    if (loadout.classPath.empty()) {
        note = "no machine class path was sent, so there is no class to spawn";
        return false;
    }
    if (loadout.className.empty()) {
        note = "the machine class arrived without a name, so what the engine spawns could not be "
               "checked against what the host said";
        return false;
    }

    std::uintptr_t machineClass = load_asset_by_path(loadout.classPath);
    if (machineClass < 0x10000) {
        NamedObject found;
        if (find_uclass(loadout.className.c_str(), found)) machineClass = found.address;
    }
    if (machineClass < 0x10000) {
        note = "the machine class '" + loadout.className + "' is neither loadable from '" +
               loadout.classPath + "' nor in this machine's object table";
        return false;
    }

    const std::string resolvedName = object_name_of(machineClass);
    if (resolvedName != loadout.className) {
        note = "the path '" + loadout.classPath + "' resolved to class '" + resolvedName +
               "' (" + hex_ptr(machineClass) + "), not the '" + loadout.className +
               "' that was sent -- refusing rather than spawning a class the host did not name";
        return false;
    }

    const std::uintptr_t world = live_world_address();
    NamedObject gsClass, gsCdo, beginFn, finishFn;
    if (!find_uclass("GameplayStatics", gsClass) ||
        !find_object_matching(
            [](const NamedObject& c, std::uintptr_t) {
                return c.objectName == "Default__GameplayStatics" &&
                       c.className == "GameplayStatics";
            },
            gsCdo, nullptr) ||
        !find_function_owned_by("BeginDeferredActorSpawnFromClass", gsClass.address, beginFn) ||
        !find_function_owned_by("FinishSpawningActor", gsClass.address, finishFn)) {
        note = "the spawn functions are not resolvable on their declaring class, GameplayStatics "
               "(the CDO and both UFunctions are required together)";
        return false;
    }
    const ProcessEventResolution pe = resolve_process_event(world);
    if (!pe.usable) {
        note = "ProcessEvent not resolvable: " + pe.note;
        return false;
    }
    const ProcessEventFn processEventFn = reinterpret_cast<ProcessEventFn>(pe.address);
    TransformRaw xform = identity_transform();

    ParamBuffer begin;
    begin.set<void*>(0x00, reinterpret_cast<void*>(world));
    begin.set<void*>(0x08, reinterpret_cast<void*>(machineClass));
    begin.set<TransformRaw>(0x10, xform);
    begin.set<std::uint8_t>(0x40, 0);
    begin.set<void*>(0x48, nullptr);
    begin.set<void*>(0x50, nullptr);
    processEventFn(reinterpret_cast<void*>(gsCdo.address), reinterpret_cast<void*>(beginFn.address),
                   begin.data());
    const auto spawned = reinterpret_cast<std::uintptr_t>(begin.get<void*>(0x50));
    if (spawned < 0x10000) {
        note = "BeginDeferredActorSpawnFromClass produced no actor for '" + loadout.className + "'";
        return false;
    }

    int         slotsWritten = 0;
    std::string failedSlots;
    for (int i = 0; i < kMachineSlotCount; ++i) {
        const MachineLoadout::Slot& slot = loadout.slots[i];
        if (!slot.present) continue;
        std::uintptr_t slotClass = load_asset_by_path(slot.classPath);
        if (slotClass < 0x10000) {
            NamedObject found;
            if (find_uclass(slot.className.c_str(), found)) slotClass = found.address;
        }

        if (slotClass >= 0x10000) {
            const std::string slotName = object_name_of(slotClass);
            if (slotName != slot.className) {
                if (!failedSlots.empty()) failedSlots += ", ";
                failedSlots += "slot " + std::to_string(i) + " " + kMachineSlotNames[i] + " '" +
                               slot.className + "' (the loaded class is '" + slotName + "')";
                continue;
            }
        }

        if (slotClass < 0x10000) {
            if (!failedSlots.empty()) failedSlots += ", ";
            failedSlots += "slot " + std::to_string(i) + " " + kMachineSlotNames[i] + " '" +
                           slot.className + "'";
            continue;
        }
        const std::uintptr_t slotAddress =
            spawned + kMachineAssembleDataOffset + static_cast<std::uintptr_t>(i) * kMachineSlotStride;
        const std::int32_t level = static_cast<std::int32_t>(slot.level);
        if (!write_process_bytes(slotAddress, &slotClass, sizeof(slotClass)) ||
            !write_process_bytes(slotAddress + 0x08, &level, sizeof(level))) {
            if (!failedSlots.empty()) failedSlots += ", ";
            failedSlots += "slot " + std::to_string(i) + " " + kMachineSlotNames[i] +
                            " (the write to " + hex_ptr(slotAddress) +
                            " was refused: the page is not writable)";
            continue;
        }
        ++slotsWritten;
    }
    if (!write_process_bytes(spawned + kMachineTriggerDataOffset, loadout.trigger,
                             sizeof(loadout.trigger))) {
        note = "FDATriggerData at could not be written";
    }

    int         partsItemsWritten = 0;
    bool        customizeWritten = false;
    bool        customizeColors = false;
    bool        customizeFigure = false;
    bool        customizeInverse = false;
    std::string failedParts;
    if (loadout.customizePresent) {
        for (int k = 0; k < 4; ++k) {
            if (loadout.partsItemPath[k].empty()) continue;
            std::uintptr_t itemClass = load_asset_by_path(loadout.partsItemPath[k]);
            if (itemClass < 0x10000) {
                NamedObject found;
                if (find_uclass(loadout.partsItemName[k].c_str(), found)) itemClass = found.address;
            }

            if (itemClass >= 0x10000 && !loadout.partsItemName[k].empty()) {
                const std::string itemName = object_name_of(itemClass);
                if (itemName != loadout.partsItemName[k]) {
                    if (!failedParts.empty()) failedParts += ", ";
                    failedParts += std::string(kPartsItemNames[k]) + " '" + loadout.partsItemName[k] +
                                   "' (the loaded class is '" + itemName + "')";
                    continue;
                }
            }

            if (itemClass < 0x10000) {
                if (!failedParts.empty()) failedParts += ", ";
                failedParts += std::string(kPartsItemNames[k]) + " '" + loadout.partsItemName[k] + "'";
                continue;
            }
            if (write_process_bytes(spawned + kMachineCustomizeDataOffset +
                                        static_cast<std::uintptr_t>(k) * 8,
                                    &itemClass, sizeof(itemClass))) {
                ++partsItemsWritten;
            } else {
                if (!failedParts.empty()) failedParts += ", ";
                failedParts += std::string(kPartsItemNames[k]) +
                               " (the write of its class pointer was refused)";
            }
        }
        const bool colors = write_process_bytes(spawned + kMachineCustomizeDataOffset + 0x20,
                                                &loadout.customize.coloring[0][0],
                                                13 * 4 * sizeof(float));
        const bool figure = write_process_bytes(spawned + kMachineCustomizeDataOffset + 0xF0,
                                                loadout.customize.figure, 5 * sizeof(float));
        const bool inverse = write_process_bytes(spawned + kMachineCustomizeDataOffset + 0x104,
                                                 loadout.inverseMesh, sizeof(loadout.inverseMesh));
        customizeColors = colors;
        customizeFigure = figure;
        customizeInverse = inverse;
        customizeWritten = colors && figure && inverse;
    } else {
        if (!note.empty()) note += " | ";
        note += "the host sent no FDACustomizeData (it could not read its own doll body), so the "
                "machine keeps the body its class builds -- the merge it is asked to perform is "
                "unaffected";
    }

    bool        moduleColorsWritten = false;
    bool        moduleColorsReadBack = false;
    bool        moduleColorsIdentical = false;
    int         moduleColorDiffBytes = 0;
    std::size_t moduleColorFirstDiff = 0;

    float moduleColorsSettled[kModuleColorCount][kModuleColorChannels][kModuleColorComponents] = {};
    if (loadout.moduleColoringPresent) {
        moduleColorsWritten =
            write_process_bytes(spawned + kMachineModuleColorOffset, loadout.moduleColoring,
                                kModuleColorBytes);

        if (moduleColorsWritten &&
            try_read_bytes(spawned + kMachineModuleColorOffset, moduleColorsSettled,
                           kModuleColorBytes)) {
            moduleColorsReadBack = true;
            const auto* want = reinterpret_cast<const std::uint8_t*>(loadout.moduleColoring);
            const auto* have = reinterpret_cast<const std::uint8_t*>(moduleColorsSettled);
            for (std::size_t b = 0; b < kModuleColorBytes; ++b) {
                if (want[b] != have[b]) {
                    if (moduleColorDiffBytes == 0) moduleColorFirstDiff = b;
                    ++moduleColorDiffBytes;
                }
            }
            moduleColorsIdentical = moduleColorDiffBytes == 0;
        }
    }

    std::string colorCaveat;
    if (loadout.moduleColoringPresent && !moduleColorsWritten) {
        colorCaveat = "FDAAssembleData::ColoringData at " + hex_ptr(spawned + kMachineModuleColorOffset) +
                      " (the ARMOUR SHELL, 576 bytes) was REFUSED by the write guard, so the shell "
                      "wears the colours this machine's class builds, not the host's";
    } else if (!loadout.moduleColoringPresent) {
        colorCaveat = "the host sent no FDAAssembleData::ColoringData (it could not read its own "
                      "machine's armour shell), so the shell keeps the colours this machine's class "
                      "builds -- the nine module classes and the body are unaffected";
    }
    if (!colorCaveat.empty()) {
        if (!note.empty()) note += " | ";
        note += colorCaveat;
    }

    bool moduleColorsAllZero = loadout.moduleColoringPresent;
    for (std::size_t b = 0; moduleColorsAllZero && b < kModuleColorBytes; ++b) {
        if (reinterpret_cast<const std::uint8_t*>(loadout.moduleColoring)[b] != 0) {
            moduleColorsAllZero = false;
        }
    }

    ParamBuffer finish;
    finish.set<void*>(0x00, reinterpret_cast<void*>(spawned));
    finish.set<TransformRaw>(0x10, xform);
    finish.set<void*>(0x40, nullptr);
    processEventFn(reinterpret_cast<void*>(gsCdo.address), reinterpret_cast<void*>(finishFn.address),
                   finish.data());
    std::uintptr_t actor = reinterpret_cast<std::uintptr_t>(finish.get<void*>(0x40));
    if (actor < 0x10000) actor = spawned;
    actorOut = actor;

    register_puppet_host(actor);

    const MachineModuleState state = read_machine_modules(actor);
    const std::string actorClassName = class_name_of(actor);
    report = "machine '" + loadout.className + "' spawned at " + hex_ptr(actor) + ";";
    report += " its class reads back as '" + actorClassName + "'";
    if (actorClassName != loadout.className) {
        report += " [THE GAME PUT A DIFFERENT CLASS THERE]";
    }

    report += (actor == spawned) ? "; (its deferred and finished addresses agree)" :
                                   ("; [THE DEFERRED ADDRESS " + hex_ptr(spawned) +
                                    " IS NOT THE FINISHED ONE -- every field written above went to the "
                                    "deferred object]");
    report += "; AssembleData slots written " + std::to_string(slotsWritten) + " of the " +
              std::to_string(loadout.slot_count()) + " the host had filled (of 9);";
    report += " FDATriggerData (" + std::to_string(loadout.trigger[0]) + ", " +
              std::to_string(loadout.trigger[1]) + ", " + std::to_string(loadout.trigger[2]) + ");";
    report += " FDACustomizeData at ";
    if (!loadout.customizePresent) {
        report += "NOT SENT by the host (no readable doll body), so the class's default body stands;";
    } else if (customizeWritten) {
        report += "written (" + std::to_string(partsItemsWritten) + " of 4 part class(es), the 208 "
                  "colour bytes, the 20 figure bytes and the 3 inverse flags);";
    } else {

        report += "PARTLY WRITTEN AND THEN REFUSED by the write guard (colours " +
                  std::string(customizeColors ? "ok" : "FAILED") + ", figure values " +
                  std::string(customizeFigure ? "ok" : "FAILED") + ", inverse flags " +
                  std::string(customizeInverse ? "ok" : "FAILED") +
                  "), so the body this machine wears is not the host's;";
    }

    report += " FDAAssembleData::ColoringData (the ARMOUR SHELL, the machine's nine modules) at +" +
              hex_ptr(kMachineModuleColorOffset) + " ";
    if (!loadout.moduleColoringPresent) {
        report += "NOT SENT by the host (it could not read its own shell colours), so the shell wears "
                  "the colours its class builds;";
    } else if (!moduleColorsWritten) {
        report += "REFUSED by the write guard, so the shell wears the colours its class builds;";
    } else if (!moduleColorsReadBack) {
        report += "written but NOT READABLE BACK at " + hex_ptr(spawned + kMachineModuleColorOffset) +
                  ", so whether the game holds the host's colours is NOT established by this run;";
    } else if (moduleColorsIdentical) {
        report += "written and read back: " + std::to_string(kModuleColorBytes) +
                  " of 576 byte(s) left exactly as the host sent them";
        if (moduleColorsAllZero) {
            report += " -- AND THAT BLOCK IS ALL ZEROES, i.e. the HOST had not coloured its own shell "
                      "either, so a default-looking shell here is the HOST'S STATE and not a failed "
                      "transfer";
        }
        report += ";";
    } else {
        char diff[96];
        std::snprintf(diff, sizeof(diff), " (%zu of 576 byte(s) differ, the first at +%zu",
                      static_cast<std::size_t>(moduleColorDiffBytes), moduleColorFirstDiff);
        report += "written, and " + std::to_string(kModuleColorBytes - static_cast<std::size_t>(moduleColorDiffBytes)) +
                  " of 576 byte(s) READ BACK as the host sent them";
        report += diff;
        report += ");";
    }

    if (loadout.moduleColoringPresent) {
        const float (&shown)[kModuleColorCount][kModuleColorChannels][kModuleColorComponents] =
            moduleColorsReadBack ? moduleColorsSettled : loadout.moduleColoring;
        static const int kShownModules[2] = {0, 2};
        for (const int m : kShownModules) {
            char line[288];
            std::snprintf(line, sizeof(line),
                          " its %s Block at %04llX: Main %.4f %.4f %.4f %.4f, Glow %.4f %.4f %.4f "
                          "%.4f;",
                          module_color_name(m),
                          static_cast<unsigned long long>(m * static_cast<int>(kModuleColorStride)),
                          static_cast<double>(shown[m][0][0]), static_cast<double>(shown[m][0][1]),
                          static_cast<double>(shown[m][0][2]), static_cast<double>(shown[m][0][3]),
                          static_cast<double>(shown[m][3][0]), static_cast<double>(shown[m][3][1]),
                          static_cast<double>(shown[m][3][2]), static_cast<double>(shown[m][3][3]));
            report += line;
        }
    }
    report += " module pointers after FinishSpawningActor:";
    for (const MachineModuleState::Entry& e : state.entries) {
        char line[320];
        if (e.pointer < 0x10000) {
            std::snprintf(line, sizeof(line), "\n %04llX %s: null",
                          static_cast<unsigned long long>(e.offset), e.name.c_str());
        } else {
            std::snprintf(line, sizeof(line), "\n %04llX %s: '%s' %s",
                          static_cast<unsigned long long>(e.offset), e.name.c_str(),
                          e.className.empty() ? "<class not resolvable>" : e.className.c_str(),
                          hex_ptr(e.pointer).c_str());
        }
        report += line;
    }
    char tail[160];
    std::snprintf(tail, sizeof(tail), "\n module pointers non-null: %d of 10", state.nonNull);
    report += tail;

    std::string extra;
    if (!failedSlots.empty()) {
        extra += "AssembleData slot(s) NOT written (the class would not load here, so the slot keeps "
                 "the class's own default): " + failedSlots + "; ";
    }
    if (!failedParts.empty()) {
        extra += "FDACustomizeData part class(es) NOT resolved (the class's own default stands there): " +
                 failedParts + "; ";
    }
    if (!extra.empty()) {
        if (!note.empty()) note += " | ";
        note += extra;
    }

    return true;
}

int puppet_destroy_all_hosts() {
    NamedObject actorClass, destroyFn;
    if (!find_uclass("Actor", actorClass) ||
        !find_function_owned_by("K2_DestroyActor", actorClass.address, destroyFn)) {
        DSH_WARN("puppet-cleanup: K2_DestroyActor is not resolvable -- nothing was destroyed");
        return 0;
    }
    const ProcessEventResolution pe = resolve_process_event(live_world_address());
    if (!pe.usable) return 0;
    const ProcessEventFn processEventFn = reinterpret_cast<ProcessEventFn>(pe.address);

    std::vector<std::uintptr_t> victims = puppet_host_registry();
    puppet_host_registry().clear();
    int destroyed = 0;
    int alreadyGone = 0;
    for (const std::uintptr_t a : victims) {
        if (a < 0x10000) continue;

        std::string why;
        if (!object_is_still_registered(a, why)) {
            ++alreadyGone;
            continue;
        }
        processEventFn(reinterpret_cast<void*>(a), reinterpret_cast<void*>(destroyFn.address),
                       nullptr);
        ++destroyed;
    }
    if (alreadyGone != 0) {
        DSH_INFO("puppet-cleanup: %d of %d host actor(s) were already gone (a world change destroys them "
                 "before this teardown runs) -- the registry is cleared either way",
                 alreadyGone, static_cast<int>(victims.size()));
    }
    DSH_INFO("puppet-cleanup: destroyed %d host actor(s) we had spawned", destroyed);
    return destroyed;
}

int puppet_destroy_actors(const std::vector<std::uintptr_t>& actors) {
    if (actors.empty()) return 0;
    NamedObject actorClass, destroyFn;
    if (!find_uclass("Actor", actorClass) ||
        !find_function_owned_by("K2_DestroyActor", actorClass.address, destroyFn)) {
        DSH_WARN("puppet-cleanup: K2_DestroyActor is not resolvable -- no doll was destroyed");
        return 0;
    }
    const ProcessEventResolution pe = resolve_process_event(live_world_address());
    if (!pe.usable) return 0;
    const ProcessEventFn processEventFn = reinterpret_cast<ProcessEventFn>(pe.address);

    auto& hosts = puppet_host_registry();
    int destroyed = 0;
    int alreadyGone = 0;
    for (const std::uintptr_t a : actors) {
        if (a < 0x10000) continue;
        hosts.erase(std::remove(hosts.begin(), hosts.end(), a), hosts.end());
        std::string why;
        if (!object_is_still_registered(a, why)) {
            ++alreadyGone;
            continue;
        }
        processEventFn(reinterpret_cast<void*>(a), reinterpret_cast<void*>(destroyFn.address), nullptr);
        ++destroyed;
    }
    DSH_INFO("puppet-cleanup: a player was gone for 20 s -- destroyed %d of %zu actor(s) of that doll (%d were "
             "already gone)", destroyed, actors.size(), alreadyGone);
    return destroyed;
}

std::uintptr_t find_object_of_class(const char* className) {
    if (className == nullptr || className[0] == '\0') return 0;
    const std::string want(className);
    NamedObject found;
    if (!find_object_matching(
            [&want](const NamedObject& c, std::uintptr_t) {
                if (c.objectName.rfind("Default__", 0) == 0) return false;
                return c.className == want;
            },
            found, nullptr)) {
        return 0;
    }
    return found.address;
}

namespace {

constexpr std::uint64_t kRetryLogIntervalMs = 5000;

bool failure_due(std::uint64_t& lastReportMs) {
    const std::uint64_t now = GetTickCount64();
    if (lastReportMs != 0 && (now - lastReportMs) < kRetryLogIntervalMs) return false;
    lastReportMs = now;
    return true;
}

}

std::uintptr_t process_event_address() {

    const ProcessEventResolution resolved = resolve_process_event(live_world_address());
    if (resolved.usable && resolved.address != 0) return resolved.address;
    DSH_WARN("game-thread: resolve_process_event did not yield an address: %s", resolved.note.c_str());
    return 0;
}

std::uint32_t current_thread_id() {
    return static_cast<std::uint32_t>(GetCurrentThreadId());
}

struct PathTools {
    bool           ready = false;
    std::uintptr_t getPathFn = 0;
    std::uintptr_t makeSoftFn = 0;
    std::uintptr_t loadBlockingFn = 0;
    std::uintptr_t cdo = 0;
    ProcessEventFn processEvent = nullptr;
    int            loads = 0;
    int            failedLoads = 0;

    std::uint64_t  lastErrorMs = 0;
};

PathTools& path_tools() {
    static PathTools t;
    return t;
}

void prepare_path_tools() {
    PathTools& t = path_tools();
    if (t.ready) return;

    NamedObject libClass, getPathFn, makeSoftFn, loadBlockingFn, cdo;
    std::string missing;
    if (!find_uclass("KismetSystemLibrary", libClass)) {
        missing = "UClass KismetSystemLibrary is not in the object table";
    } else if (!find_function_owned_by("GetPathName", libClass.address, getPathFn)) {
        missing = "KismetSystemLibrary::GetPathName is not resolvable";
    } else if (!find_function_owned_by("MakeSoftObjectPath", libClass.address, makeSoftFn)) {
        missing = "KismetSystemLibrary::MakeSoftObjectPath is not resolvable";
    } else if (!find_function_owned_by("LoadAsset_Blocking", libClass.address, loadBlockingFn)) {
        missing = "KismetSystemLibrary::LoadAsset_Blocking is not resolvable";
    } else if (!find_object_matching(
                   [](const NamedObject& o, std::uintptr_t) {
                       return o.objectName == "Default__KismetSystemLibrary" &&
                              o.className == "KismetSystemLibrary";
                   },
                   cdo, nullptr)) {
        missing = "the Default__KismetSystemLibrary CDO is not in the object table";
    } else {
        const ProcessEventResolution pe = resolve_process_event(live_world_address());
        if (!pe.usable) {
            missing = "ProcessEvent is not resolvable: " + pe.note;
        } else {
            t.getPathFn = getPathFn.address;
            t.makeSoftFn = makeSoftFn.address;
            t.loadBlockingFn = loadBlockingFn.address;
            t.cdo = cdo.address;
            t.processEvent = reinterpret_cast<ProcessEventFn>(pe.address);
            t.ready = true;
            return;
        }
    }

    if (failure_due(t.lastErrorMs)) {
        DSH_ERROR("paths: %s -- asset loading and path reading stay DISABLED and will be retried on "
                  "the next call", missing.c_str());
    }
}

std::string copy_fstring(ParamBuffer& buf, std::size_t offset) {
    std::uint64_t data = 0;
    std::int32_t num = 0;
    if (!try_read_u64(reinterpret_cast<std::uintptr_t>(buf.data()) + offset, data)) return {};
    if (!try_read_bytes(reinterpret_cast<std::uintptr_t>(buf.data()) + offset + 8, &num,
                        sizeof(num))) {
        return {};
    }
    if (data < 0x10000 || num <= 0 || num > 1024) return {};
    std::vector<wchar_t> wide(static_cast<std::size_t>(num), 0);
    if (!try_read_bytes(static_cast<std::uintptr_t>(data),
                        wide.data(), static_cast<std::size_t>(num) * sizeof(wchar_t))) {
        return {};
    }
    std::string out;
    for (wchar_t c : wide) {
        if (c == 0) break;
        out.push_back(static_cast<char>(c & 0xFF));
    }
    return out;
}

std::string object_path_name(std::uintptr_t obj) {
    prepare_path_tools();
    PathTools& t = path_tools();
    if (t.processEvent == nullptr || t.getPathFn == 0 || obj == 0) return {};
    ParamBuffer prm;
    prm.set<void*>(0x00, reinterpret_cast<void*>(obj));
    t.processEvent(reinterpret_cast<void*>(t.cdo), reinterpret_cast<void*>(t.getPathFn), prm.data());
    return copy_fstring(prm, 0x08);
}

bool build_soft_object_ptr(const std::string& path, ParamBuffer& into, std::size_t offset,
                           std::vector<wchar_t>& owned) {
    PathTools& t = path_tools();
    if (t.makeSoftFn == 0) return false;

    std::vector<wchar_t> wide;
    wide.reserve(path.size() + 1);
    for (char c : path) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    wide.push_back(0);

    ParamBuffer mk;
    mk.set<void*>(0x00, wide.data());
    mk.set<std::int32_t>(0x08, static_cast<std::int32_t>(wide.size()));
    mk.set<std::int32_t>(0x0C, static_cast<std::int32_t>(wide.size()));

    DSH_INFO("paths: [b1] calling MakeSoftObjectPath for '%s'", path.c_str());
    t.processEvent(reinterpret_cast<void*>(t.cdo), reinterpret_cast<void*>(t.makeSoftFn), mk.data());
    DSH_INFO("paths: [b2] MakeSoftObjectPath returned");

    for (std::size_t k = 0; k < 0x28; ++k) into.set<std::uint8_t>(offset + k, 0);

    for (std::size_t k = 0; k < 0x08; ++k) {
        into.set<std::uint8_t>(offset + 0x10 + k, mk.get<std::uint8_t>(0x10 + k));
    }
    DSH_INFO("paths: [b3] copied the FName out of the returned path");
    std::uint64_t subData = 0;
    std::int32_t subNum = 0;
    const std::uintptr_t subOffset = 0x10 + 0x08;
    if (try_read_u64(reinterpret_cast<std::uintptr_t>(mk.data()) + subOffset, subData) &&
        try_read_bytes(reinterpret_cast<std::uintptr_t>(mk.data()) + subOffset + 8, &subNum,
                       sizeof(subNum)) &&
        subData >= 0x10000 && subNum > 0 && subNum < 4096) {
        owned.assign(static_cast<std::size_t>(subNum), 0);
        if (!try_read_bytes(static_cast<std::uintptr_t>(subData), owned.data(),
                            static_cast<std::size_t>(subNum) * sizeof(wchar_t))) {
            owned.clear();
        }
    }
    into.set<void*>(offset + 0x18, owned.empty() ? nullptr : owned.data());
    into.set<std::int32_t>(offset + 0x20, static_cast<std::int32_t>(owned.size()));
    into.set<std::int32_t>(offset + 0x24, static_cast<std::int32_t>(owned.size()));
    DSH_INFO("paths: [b4] soft pointer assembled (%zu character(s) of sub-path)", owned.size());
    return true;
}

struct LoadedAsset {
    std::uintptr_t address = 0;
    std::string    name;
};
std::unordered_map<std::string, LoadedAsset> g_loadedByPath;
bool g_loadInFlight = false;

std::uintptr_t load_asset_by_path(const std::string& path) {
    prepare_path_tools();
    if (auto it = g_loadedByPath.find(path); it != g_loadedByPath.end()) {
        const LoadedAsset hit = it->second;
        std::string why;
        const bool live = hit.address >= 0x10000 && object_is_still_registered(hit.address, why);
        if (live && !hit.name.empty() && object_name_of(hit.address) == hit.name) {
            return hit.address;
        }
        DSH_INFO("paths: the cached object for '%s' is STALE (0x%llX, '%s'%s) -- dropping it and loading "
                 "again, because a cache hit that cannot answer its own name is not a class",
                 path.c_str(), static_cast<unsigned long long>(hit.address), hit.name.c_str(),
                 live ? "" : "; it is no longer in the object table");
        g_loadedByPath.erase(it);
    }
    if (g_loadInFlight) {
        DSH_WARN("paths: a load is ALREADY IN FLIGHT; refusing to start a second one for '%s' (two "
                 "concurrent flushes are what the engine asserts on)", path.c_str());
        return 0;
    }
    PathTools& t = path_tools();
    if (t.processEvent == nullptr || t.loadBlockingFn == 0 || path.empty()) return 0;

    ParamBuffer prm;

    std::vector<wchar_t> ownedPath;
    if (!build_soft_object_ptr(path, prm, 0x00, ownedPath)) {
        DSH_WARN("paths: could not assemble a soft pointer for '%s'", path.c_str());
        return 0;
    }

    DSH_INFO("paths: [b5] dispatching LoadAsset_Blocking to the game thread");
    g_loadInFlight = true;
    bool ran = false;
    run_on_game_thread(
        [&]() {
            t.processEvent(reinterpret_cast<void*>(t.cdo),
                           reinterpret_cast<void*>(t.loadBlockingFn), prm.data());
            ran = true;
        },
        4000);
    g_loadInFlight = false;
    if (!ran) {
        DSH_WARN("paths: the game thread did not run the load for '%s'", path.c_str());
        ++t.failedLoads;
        return 0;
    }
    DSH_INFO("paths: [b6] LoadAsset_Blocking returned");
    ++t.loads;
    const auto loaded = reinterpret_cast<std::uintptr_t>(prm.get<void*>(0x28));
    if (loaded < 0x10000) {
        ++t.failedLoads;
        DSH_WARN("paths: loading '%s' produced nothing", path.c_str());
        return 0;
    }
    const std::string name = object_name_of(loaded);
    if (name.empty()) {
        ++t.failedLoads;
        DSH_WARN("paths: '%s' loaded to an object with no name", path.c_str());
        return 0;
    }
    g_loadedByPath[path] = LoadedAsset{loaded, name};
    return loaded;
}

bool make_ftext(const std::wstring& text, unsigned char* out18, std::string& note) {
    note.clear();
    if (out18 == nullptr) {
        note = "there is nowhere to put the FText";
        return false;
    }

    static std::uintptr_t s_cdo = 0;
    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t klass = resolve_uclass_address("KismetTextLibrary");
        if (klass == 0) {
            s_refusal = "the KismetTextLibrary UClass is not in the object array, so the engine's own "
                        "string-to-text converter cannot be found and no FText can be made";
        } else {
            NamedObject function;
            if (!find_function_owned_by("Conv_StringToText", klass, function)) {
                s_refusal = "no UFunction named Conv_StringToText is owned by the KismetTextLibrary "
                            "UClass, so no FText can be made";
            } else {
                s_fn = function.address;
                std::uintptr_t cdo = 0;
                if (!read_process_bytes(klass + 0x0118, &cdo, sizeof(cdo)) || cdo < 0x10000) {
                    s_refusal = "the KismetTextLibrary class default object at could not be read, "
                                "and a static function is called on the default object";
                } else {
                    s_cdo = cdo;
                }
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    struct FStringMirror {
        wchar_t*     Data;
        std::int32_t Num;
        std::int32_t Max;
    };
    FStringMirror fs{};
    fs.Data = const_cast<wchar_t*>(text.c_str());
    fs.Num = static_cast<std::int32_t>(text.size()) + 1;
    fs.Max = fs.Num;

    unsigned char frame[0x40] = {};
    std::memcpy(frame + 0x00, &fs, sizeof(fs));
    s_pe(reinterpret_cast<void*>(s_cdo), reinterpret_cast<void*>(s_fn), frame);
    std::memcpy(out18, frame + 0x10, 0x18);
    return true;
}

namespace {

struct FStringForCall {
    wchar_t*     Data;
    std::int32_t Num;
    std::int32_t Max;
};

struct ListenCall {
    std::wstring  map;
    std::wstring  options;
    unsigned char nameFrame[0x20] = {};
    unsigned char frame[0x28] = {};
    bool          fnameReturned = false;
    bool          levelCalled = false;
};

bool resolve_static_ufunction(const char* className, const char* functionName, std::uintptr_t& cdo,
                              std::uintptr_t& fn, std::string& refusal) {
    const std::uintptr_t klass = resolve_uclass_address(className);
    if (klass == 0) {
        refusal = std::string("the ") + className + " UClass is not in the object array";
        return false;
    }
    NamedObject function;
    if (!find_function_owned_by(functionName, klass, function)) {
        refusal = std::string("no UFunction named ") + functionName + " is owned by the " + className +
                  " UClass";
        return false;
    }
    std::uintptr_t object = 0;
    if (!read_process_bytes(klass + 0x0118, &object, sizeof(object)) || object < 0x10000) {
        refusal = std::string("the ") + className + " class default object at could not be read, "
                  "and a static function is called on the default object";
        return false;
    }
    cdo = object;
    fn = function.address;
    return true;
}

}

StaticFunctionRef reflect_resolve_static(const char* className, const char* functionName) {
    StaticFunctionRef out;
    const std::string cls = className != nullptr ? std::string(className) : std::string();
    if (cls.empty() || functionName == nullptr) {
        out.note = "a class name and a function name are both required";
        return out;
    }
    std::uintptr_t cdo = 0;
    std::uintptr_t fn = 0;
    if (!resolve_static_ufunction(cls.c_str(), functionName, cdo, fn, out.note)) {
        return out;
    }
    out.fn = reflect_resolve_function(resolve_uclass_address(cls.c_str()), functionName);
    if (!out.fn.usable) {
        out.note = out.fn.note;
        return out;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(cdo));
    out.cdo = cdo;
    out.usable = true;
    out.note = "'" + cls + "." + std::string(functionName) + "' resolved; invoked on its default object " +
               std::string(buf);
    return out;
}

NetLayerState net_layer_state() {
    NetLayerState s;
    const WorldSurvey survey = survey_worlds();
    s.worldCount = static_cast<std::uint32_t>(survey.worlds.size());
    s.survey = survey.line();
    if (survey.worlds.empty()) {
        s.note = "the object array holds no UWorld at all, so there is no network layer to read";
        return s;
    }

    for (const WorldSample& w : survey.worlds) {
        if (w.persistentLevel != 0) {
            s.world = w.address;
            s.authorityGameMode = w.authorityGameMode;
            break;
        }
    }
    if (s.world == 0) {
        s.world = survey.worlds.front().address;
        s.authorityGameMode = survey.worlds.front().authorityGameMode;
        s.note = "no world owns a PersistentLevel in this sample";
    }

    for (const WorldSample& w : survey.worlds) {
        if (w.netDriver == 0) continue;
        s.netDriver = w.netDriver;
        s.driverName = object_name_of(w.netDriver);
        std::uintptr_t klass = 0;
        if (read_process_bytes(w.netDriver + 0x10, &klass, sizeof(klass)) && klass >= 0x10000) {
            s.driverClass = object_name_of(klass);
        }
        break;
    }
    return s;
}

std::string NetLayerState::line() const {
    char buffer[512];
    std::snprintf(buffer, sizeof(buffer), "world=0x%llX NetDriver=0x%llX AuthorityGameMode=0x%llX",
                  static_cast<unsigned long long>(world),
                  static_cast<unsigned long long>(netDriver),
                  static_cast<unsigned long long>(authorityGameMode));
    std::string out = buffer;
    if (netDriver == 0) {
        out += " -- NO world in this sample has a NetDriver";
    } else {
        out += " ('";
        out += driverName;
        out += "' of class '";
        out += driverClass;
        out += "')";
    }
    if (!note.empty()) {
        out += " [";
        out += note;
        out += "]";
    }
    if (!survey.empty()) {
        out += " || ";
        out += survey;
    }
    return out;
}

bool net_open_level_listen(const std::string& map, const std::string& options, std::string& note) {
    note.clear();
    if (map.empty()) {
        note = "no map name was given, and an empty level name would travel nowhere";
        return false;
    }

    static std::uintptr_t s_openCdo = 0;
    static std::uintptr_t s_openFn = 0;
    static std::uintptr_t s_nameCdo = 0;
    static std::uintptr_t s_nameFn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        std::string refusal;
        if (!resolve_static_ufunction("GameplayStatics", "OpenLevel", s_openCdo, s_openFn, refusal)) {
            s_refusal = refusal;
        } else if (!resolve_static_ufunction("KismetStringLibrary", "Conv_StringToName", s_nameCdo,
                                             s_nameFn, refusal)) {
            s_refusal = refusal;
        } else {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    auto call = std::make_shared<ListenCall>();
    call->map.assign(map.begin(), map.end());
    call->options.assign(options.begin(), options.end());

    DSH_INFO("net: [b4] dispatching UGameplayStatics::OpenLevel(level='%s', options='%s', bAbsolute=false) "
             "to the game thread; the world being travelled away from is 0x%llX",
             map.c_str(), options.c_str(),
             static_cast<unsigned long long>(live_world_address()));

    bool ran = false;
    run_on_game_thread(
        [call, pe = s_pe, openCdo = s_openCdo, openFn = s_openFn, nameCdo = s_nameCdo,
         nameFn = s_nameFn, &ran]() {
            ran = true;

            FStringForCall nameIn{};
            nameIn.Data = const_cast<wchar_t*>(call->map.c_str());
            nameIn.Num = static_cast<std::int32_t>(call->map.size()) + 1;
            nameIn.Max = nameIn.Num;
            std::memcpy(call->nameFrame + 0x00, &nameIn, sizeof(nameIn));
            pe(reinterpret_cast<void*>(nameCdo), reinterpret_cast<void*>(nameFn), call->nameFrame);
            call->fnameReturned = true;

            std::uint64_t levelName = 0;
            std::memcpy(&levelName, call->nameFrame + 0x10, sizeof(levelName));
            if (levelName == 0) {
                return;
            }

            *reinterpret_cast<std::uintptr_t*>(call->frame + 0x00) = live_world_address();
            std::memcpy(call->frame + 0x08, &levelName, sizeof(levelName));
            call->frame[0x10] = 0;
            FStringForCall opts{};
            opts.Data = const_cast<wchar_t*>(call->options.c_str());
            opts.Num = static_cast<std::int32_t>(call->options.size()) + 1;
            opts.Max = opts.Num;
            std::memcpy(call->frame + 0x18, &opts, sizeof(opts));

            game_thread_pause_all(true);
            DSH_INFO("net: [b4b] the game-thread queue is CLOSED; nothing queued before or during the travel "
                     "will run against the world being torn down");

            pe(reinterpret_cast<void*>(openCdo), reinterpret_cast<void*>(openFn), call->frame);
            call->levelCalled = true;
        },
        20000);

    if (!ran) {
        note = "the game thread never ran the call (it was interrupted or the wait timed out before the "
               "work was picked up)";
        return false;
    }
    if (!call->fnameReturned) {
        note = "the game thread ran, but the string-to-name converter was never invoked";
        return false;
    }
    if (!call->levelCalled) {
        note = "the engine's string-to-name converter returned None, so there was no level name to put in "
               "the travel command; nothing was travelled to";
        return false;
    }
    DSH_INFO("net: [b5] OpenLevel returned -- the travel (if any) is over and the old world may be gone");
    return true;
}

[[nodiscard]] bool net_client_connect(const std::string& url, std::string& note);

namespace {

struct ConnectCall {
    std::wstring  url;
    unsigned char frame[0x28] = {};
    bool          called = false;
};

}
bool net_client_connect(const std::string& url, std::string& note) {
    note.clear();
    if (url.empty()) {
        note = "no address was given, and an empty URL would travel nowhere";
        return false;
    }

    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t klass = resolve_uclass_address("PlayerController");
        if (klass == 0) {
            s_refusal = "the PlayerController UClass is not in the object array";
        } else {
            NamedObject function;
            if (!find_function_owned_by("ClientTravel", klass, function)) {
                s_refusal = "no UFunction named ClientTravel is owned by the PlayerController UClass";
            } else {
                s_fn = function.address;
                const ProcessEventResolution pe = resolve_process_event(live_world_address());
                if (!pe.usable) {
                    s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
                } else {
                    s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
                }
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    const LiveObjects live = find_live_objects();
    if (live.playerController < 0x10000) {
        note = "there is no live PlayerController, and ClientTravel is called on the local player";
        return false;
    }

    const WorldSurvey before = survey_worlds();
    if (!before.pending.empty()) {
        char buffer[320];
        std::snprintf(buffer, sizeof(buffer),
                      "a PendingNetGame is ALREADY alive (0x%llX, driver 0x%llX '%s'): a second ClientTravel "
                      "while one is pending is the shape of the 2026-10-18 client crash. Nothing was dispatched; "
                      "wait for it to time out (~13 s) or restart the game",
                      static_cast<unsigned long long>(before.pending.front().address),
                      static_cast<unsigned long long>(before.pending.front().driver),
                      before.pending.front().driverClass.c_str());
        note = buffer;
        return false;
    }

    auto call = std::make_shared<ConnectCall>();
    call->url.assign(url.begin(), url.end());

    DSH_INFO("net: [c4] dispatching APlayerController::ClientTravel(url='%s', TRAVEL_Absolute) on 0x%llX to "
             "the game thread", url.c_str(),
             static_cast<unsigned long long>(live.playerController));

    bool ran = false;
    run_on_game_thread(
        [call, pe = s_pe, fn = s_fn, controller = live.playerController, &ran]() {
            ran = true;
            FStringForCall text{};
            text.Data = const_cast<wchar_t*>(call->url.c_str());
            text.Num = static_cast<std::int32_t>(call->url.size()) + 1;
            text.Max = text.Num;
            std::memcpy(call->frame + 0x00, &text, sizeof(text));
            call->frame[0x10] = 0;
            call->frame[0x11] = 0;

            game_thread_pause_all(true);
            DSH_INFO("net: [c4b] the game-thread queue is CLOSED; a client travel tears down this world too");
            pe(reinterpret_cast<void*>(controller), reinterpret_cast<void*>(fn), call->frame);
            call->called = true;
        },
        20000);

    if (!ran) {
        note = "the game thread never ran the call (it was interrupted, the trampoline is absent, or the "
               "queue was paused by another travel)";
        return false;
    }
    if (!call->called) {
        note = "the game thread ran, but ClientTravel was never invoked";
        return false;
    }
    DSH_INFO("net: [c5] ClientTravel returned -- the connection attempt is over or has been handed to the "
             "engine; the next 'net watch' samples are the evidence");
    return true;
}

namespace {

constexpr std::uint32_t kMenuFrameBytes   = 0x80;
constexpr std::uint32_t kMenuOutSelected  = 0x50;
constexpr std::uint32_t kMenuPriority     = 0x54;
constexpr std::uint32_t kMenuReturnValue  = 0x58;
constexpr int           kMenuSlots        = 4;

struct MenuSlot {
    unsigned char  frame[kMenuFrameBytes];

    unsigned char  mainCaption[0x18];
    unsigned char  subCaption[0x18];
    unsigned char  optionTexts[8][0x18];
    std::uintptr_t selectedAddress = 0;
    std::uint64_t  openedMs = 0;
    int            asked = 0;
};

MenuSlot g_menuSlots[kMenuSlots];
int      g_nextMenuSlot = 0;

MenuSlot& next_menu_slot(std::string& note) {
    MenuSlot& slot = g_menuSlots[g_nextMenuSlot];
    g_nextMenuSlot = (g_nextMenuSlot + 1) % kMenuSlots;

    if (slot.asked > 0) {
        std::int32_t last = -1;
        std::memcpy(&last, slot.frame + kMenuOutSelected, sizeof(last));
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "this frame slot has served %d menu(s) before; the previous one was built at tick "
                      "%llu and its answer slot still reads %d (%s)",
                      slot.asked, static_cast<unsigned long long>(slot.openedMs), last,
                      last < 0 ? "no answer was ever written -- that menu was abandoned or is still "
                                 "open, and it is being handed a frame that now belongs to this one"
                               : "it was answered");
        note = buf;
    } else {
        note.clear();
    }

    slot.asked += 1;
    slot.openedMs = GetTickCount64();
    return slot;
}

}
MenuHandle menu_show_select(std::uintptr_t causer, const std::vector<std::wstring>& options,
                           const std::wstring& mainCaption, const std::wstring& subCaption) {
    MenuHandle out;
    out.optionCount = static_cast<std::uint32_t>(options.size());
    if (causer < 0x10000) {
        out.note = "there is no causer to hang a menu on (the pointer is implausible)";
        return out;
    }
    if (options.empty()) {
        out.note = "a menu with no options was asked for, and an empty list has no meaning to show";
        return out;
    }

    static std::uintptr_t s_cdo = 0;
    static std::uintptr_t s_fn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;

    if (!s_resolved) {
        s_resolved = true;
        const std::uintptr_t klass = resolve_uclass_address("DADialogueTask_Select");
        if (klass == 0) {
            s_refusal = "the DADialogueTask_Select UClass is not in the object array, so the game's own "
                        "option menu cannot be found -- note that a UClass object's own FName carries no "
                        "prefix, which is why this is not 'UDADialogueTask_Select'";
        } else {
            NamedObject function;
            if (!find_function_owned_by("DialogueSelect", klass, function)) {
                s_refusal = "no UFunction named DialogueSelect is owned by the DADialogueTask_Select UClass";
            } else {
                s_fn = function.address;
                std::uintptr_t cdo = 0;
                if (!read_process_bytes(klass + 0x0118, &cdo, sizeof(cdo)) || cdo < 0x10000) {
                    s_refusal = "the DADialogueTask_Select class default object at could not be "
                                "read, and a static function is called on the default object";
                } else {
                    s_cdo = cdo;
                }
            }
        }
        if (s_refusal.empty()) {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        out.note = s_refusal;
        return out;
    }

    std::string poolNote;
    MenuSlot& slot = next_menu_slot(poolNote);
    std::memset(slot.frame, 0, sizeof(slot.frame));
    const std::int32_t none = -1;
    std::memcpy(slot.frame + kMenuOutSelected, &none, sizeof(none));
    slot.selectedAddress = reinterpret_cast<std::uintptr_t>(slot.frame + kMenuOutSelected);

    const std::size_t count = options.size() > 8 ? 8 : options.size();

    std::string textNote;
    if (!make_ftext(mainCaption, slot.mainCaption, textNote)) {
        out.note = "the menu's main caption could not be made into text: " + textNote;
        return out;
    }
    if (!make_ftext(subCaption, slot.subCaption, textNote)) {
        out.note = "the menu's sub caption could not be made into text: " + textNote;
        return out;
    }
    for (std::size_t k = 0; k < count; ++k) {
        if (!make_ftext(options[k], slot.optionTexts[k], textNote)) {
            out.note = "option " + std::to_string(k) + " could not be made into text: " + textNote;
            return out;
        }
    }

    struct FTextArrayMirror {
        unsigned char* Data;
        std::int32_t   Num;
        std::int32_t   Max;
    };
    FTextArrayMirror asks{};
    asks.Data = &slot.optionTexts[0][0];
    asks.Num = static_cast<std::int32_t>(count);
    asks.Max = static_cast<std::int32_t>(count);

    std::memcpy(slot.frame + 0x00, &causer, sizeof(causer));
    const unsigned char selectType = 0;
    std::memcpy(slot.frame + 0x08, &selectType, sizeof(selectType));
    std::memcpy(slot.frame + 0x10, slot.mainCaption, 0x18);
    std::memcpy(slot.frame + 0x28, slot.subCaption, 0x18);
    std::memcpy(slot.frame + 0x40, &asks, sizeof(asks));
    const unsigned char priority = 0;
    std::memcpy(slot.frame + kMenuPriority, &priority, sizeof(priority));

    s_pe(reinterpret_cast<void*>(s_cdo), reinterpret_cast<void*>(s_fn), slot.frame);

    static std::uintptr_t s_readyFn = 0;
    static bool           s_readyResolved = false;
    static std::string    s_readyRefusal;
    if (!s_readyResolved) {
        s_readyResolved = true;
        const std::uintptr_t taskClass = resolve_uclass_address("GameplayTask");
        if (taskClass == 0) {
            s_readyRefusal = "the GameplayTask UClass is not in the object array";
        } else {
            NamedObject ready;
            if (!find_function_owned_by("ReadyForActivation", taskClass, ready)) {
                s_readyRefusal =
                    "no UFunction named ReadyForActivation is owned by the GameplayTask UClass";
            } else {
                s_readyFn = ready.address;
            }
        }
    }

    std::uintptr_t task = 0;
    std::memcpy(&task, slot.frame + kMenuReturnValue, sizeof(task));

    std::int32_t picked = -1;
    std::memcpy(&picked, slot.frame + kMenuOutSelected, sizeof(picked));

    out.task = task;
    out.selectedAddress = slot.selectedAddress;
    out.selected = picked;

    if (task < 0x10000) {
        out.note = "DialogueSelect returned NO task object at, so there was nothing to activate "
                   "and no menu can have been shown. The usual cause is that a previous option task still "
                   "owns the dialogue resource: the engine allows one at a time";
        if (!poolNote.empty()) out.note += ". Also: " + poolNote;
        return out;
    }
    if (s_readyFn == 0) {
        out.note = "the menu's task 0x" + [&] {
            char b[32];
            std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(task));
            return std::string(b);
        }() + " was built but could NOT be activated: " + s_readyRefusal;
        return out;
    }

    unsigned char actFrame[0x10] = {};
    s_pe(reinterpret_cast<void*>(task), reinterpret_cast<void*>(s_readyFn), actFrame);

    out.shown = true;
    out.note = "DialogueSelect built task 0x" + [&] {
        char b[32];
        std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(task));
        return std::string(b);
    }() + " with " + std::to_string(count) + " option(s), ReadyForActivation was CALLED, and the "
          "OUTPUT SLOT the game will write the chosen row into is OURS and stays alive at 0x" + [&] {
        char b[32];
        std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(slot.selectedAddress));
        return std::string(b);
    }() + " (frame, not a stack local). Its value right now is " + std::to_string(picked) +
        ", which is what 'no row has been chosen yet' reads as";
    if (picked != -1) {

        out.note += ". NOTE: the slot was NOT -1 immediately after the call -- the factory wrote " +
                    std::to_string(picked) +
                    " itself, so this output is not (only) the player's answer and the watch must not "
                    "treat it as one";
    }
    if (!poolNote.empty()) out.note += ". Frame note: " + poolNote;
    return out;
}

std::int32_t menu_read_choice(std::uintptr_t selectedAddress) {
    if (selectedAddress < 0x10000) return -1;
    std::int32_t value = -1;
    if (!try_read_bytes(selectedAddress, &value, sizeof(value))) return -1;
    return value;
}

MenuTaskState menu_read_task_state(std::uintptr_t task) {
    MenuTaskState state;
    if (task < 0x10000) return state;

    std::uint32_t cancelled = 0;
    std::uint32_t finished = 0;
    if (!try_read_bytes(task + 0x70 + 8, &cancelled, sizeof(cancelled))) return state;
    if (!try_read_bytes(task + 0x80 + 8, &finished, sizeof(finished))) return state;

    if (cancelled > 64 || finished > 64) return state;

    state.readable = true;
    state.onCancelled = cancelled;
    state.onFinished = finished;
    return state;
}

namespace {

struct FieldLayout {
    std::size_t next;
    std::size_t name;
    std::size_t offset;
    std::size_t elementSize;
    std::size_t arrayDim;
};
constexpr FieldLayout kFieldLayouts[4] = {
    {0x20, 0x28, 0x4C, 0x3C, 0x38},
    {0x20, 0x28, 0x48, 0x34, 0x30},
    {0x28, 0x30, 0x4C, 0x3C, 0x38},
    {0x18, 0x20, 0x44, 0x34, 0x30},
};

struct PropertyOracle {
    const char*  name;
    std::int32_t offset;
};
constexpr PropertyOracle kPropertyOracles[3] = {{"bPurged", 0x228},
                                                {"ModuleLevel", 0x22C},
                                                {"ModuleStatusTable", 0x290}};

std::atomic<std::size_t> g_fieldLayoutIndex{0};
std::atomic<bool>        g_fieldLayoutProved{false};

std::string field_name(std::uintptr_t field, const FieldLayout& layout) {
    std::uint64_t nameField = 0;
    if (!try_read_u64(field + layout.name, nameField) || nameField == 0) return {};
    std::string text;
    std::string note;
    if (!resolve_fname(static_cast<std::uint32_t>(nameField & 0xFFFFFFFFu), text, note)) return {};
    return text;
}

bool field_value_by_name(std::uintptr_t klass, const FieldLayout& layout, std::size_t fieldOffset,
                         const char* wanted, std::int32_t& out) {
    std::uint64_t child = 0;
    if (!try_read_u64(klass + 0x0050, child) || child < 0x10000) return false;
    auto field = static_cast<std::uintptr_t>(child);
    for (int steps = 0; field >= 0x10000 && steps < 4096; ++steps) {
        if (field_name(field, layout) == wanted) return try_read_bytes(field + fieldOffset, &out, sizeof(out));
        std::uint64_t next = 0;
        if (!try_read_u64(field + layout.next, next)) return false;
        field = static_cast<std::uintptr_t>(next);
    }
    return false;
}

}
std::vector<std::string> class_property_values(std::uintptr_t classAddress, std::uintptr_t instanceAddress,
                                                std::uint32_t maxLines, std::string& note) {
    std::vector<std::string> lines;
    note.clear();
    if (classAddress < 0x10000) {
        note = "no class address was given";
        return lines;
    }

    if (!g_fieldLayoutProved.load(std::memory_order_relaxed)) {
        g_fieldLayoutProved.store(true, std::memory_order_relaxed);
        const std::uintptr_t moduleClass = resolve_uclass_address("DAFrontWeaponModule");
        if (moduleClass < 0x10000) {
            note = "the oracle class DAFrontWeaponModule is not in this process's object table, so the "
                   "FField layout cannot be proved and NO VALUE will be read";
        } else {
            for (std::size_t i = 0; i < 4; ++i) {
                bool allAgree = true;
                for (const PropertyOracle& oracle : kPropertyOracles) {
                    std::int32_t offset = 0;
                    if (!field_value_by_name(moduleClass, kFieldLayouts[i], kFieldLayouts[i].offset,
                                             oracle.name, offset) ||
                        offset != oracle.offset) {
                        allAgree = false;
                        break;
                    }
                }
                if (allAgree) {
                    g_fieldLayoutIndex.store(i + 1, std::memory_order_relaxed);
                    char buffer[256];
                    std::snprintf(buffer, sizeof(buffer),
                                  "probe: properties live in ChildProperties at Next %llX, Name "
                                  "%llX, Offset_Internal %llX -- PROVED against three "
                                  "DAFrontWeaponModule properties whose offsets the game gives (bPurged "
                                  ", ModuleLevel, ModuleStatusTable)",
                                  static_cast<unsigned long long>(kFieldLayouts[i].next),
                                  static_cast<unsigned long long>(kFieldLayouts[i].name),
                                  static_cast<unsigned long long>(kFieldLayouts[i].offset));
                    lines.push_back(buffer);
                    break;
                }
            }
            if (g_fieldLayoutIndex.load(std::memory_order_relaxed) == 0) {
                note = "none of the four candidate FField layouts reproduces the game's three "
                       "DAFrontWeaponModule offsets, so this build's property layout is NOT what this code "
                       "expects and NO VALUE will be read";
            }
        }
    }
    if (g_fieldLayoutIndex.load(std::memory_order_relaxed) == 0) return lines;
    const FieldLayout layout = kFieldLayouts[g_fieldLayoutIndex.load(std::memory_order_relaxed) - 1];

    std::int32_t classSize = 0;
    try_read_bytes(classAddress + 0x0058, &classSize, sizeof(classSize));
    if (classSize <= 0 || classSize > 0x40000) classSize = 0;

    std::uint64_t child = 0;
    if (!try_read_u64(classAddress + 0x0050, child) || child < 0x10000) {
        if (note.empty()) {
            note = "the class has no ChildProperties chain ( is null), so it declares no property of "
                   "its own -- its members belong to a parent";
        }
        return lines;
    }
    auto field = static_cast<std::uintptr_t>(child);
    std::uint32_t printed = 0;
    for (int steps = 0; field >= 0x10000 && steps < 4096 && printed < maxLines; ++steps) {
        const std::string name = field_name(field, layout);

        std::string typeName;
        {
            std::uint64_t fieldClass = 0;
            if (try_read_u64(field + 0x0008, fieldClass) && fieldClass >= 0x10000) {
                std::uint64_t typeNameField = 0;
                std::string typeNote;
                if (try_read_u64(static_cast<std::uintptr_t>(fieldClass) + 0x0000, typeNameField)) {
                    resolve_fname(static_cast<std::uint32_t>(typeNameField & 0xFFFFFFFFu), typeName, typeNote);
                }
            }
        }
        std::int32_t offset = -1, elementSize = 0, arrayDim = 0;
        try_read_bytes(field + layout.offset, &offset, sizeof(offset));
        try_read_bytes(field + layout.elementSize, &elementSize, sizeof(elementSize));
        try_read_bytes(field + layout.arrayDim, &arrayDim, sizeof(arrayDim));

        std::string line = "'" + name + "' " + (typeName.empty() ? "<unknown kind>" : typeName);
        if (offset >= 0 && arrayDim > 0 && elementSize > 0) {
            char buffer[96];
            std::snprintf(buffer, sizeof(buffer), " %llX (%d elem(s) of %d byte(s))",
                          static_cast<unsigned long long>(offset), arrayDim, elementSize);
            line += buffer;

            const bool pointerShaped = typeName.find("ClassProperty") != std::string::npos ||
                                       typeName.find("ObjectProperty") != std::string::npos ||
                                       typeName.find("InterfaceProperty") != std::string::npos;
            if (pointerShaped && instanceAddress >= 0x10000 && classSize > 0 && offset + 8 <= classSize) {
                std::uint64_t value = 0;
                if (try_read_u64(instanceAddress + static_cast<std::uintptr_t>(offset), value)) {
                    if (value >= 0x10000) {
                        const auto target = static_cast<std::uintptr_t>(value);
                        line += " = '" + object_own_name(target) + "' [" + class_name_of(target) + "]";
                    } else {
                        line += " = <null>";
                    }
                } else {
                    line += " = <not readable>";
                }
            } else if (pointerShaped && instanceAddress < 0x10000) {
                line += " -- no instance given, so no value";
            } else if (instanceAddress >= 0x10000 && classSize > 0 && arrayDim == 1 &&
                       offset + elementSize <= classSize) {

                const auto at = instanceAddress + static_cast<std::uintptr_t>(offset);
                if (typeName == "FloatProperty" && elementSize == 4) {
                    float value = 0.0f;
                    if (try_read_bytes(at, &value, sizeof(value))) {
                        char number[64];
                        std::snprintf(number, sizeof(number), " = %.3f", static_cast<double>(value));
                        line += number;
                    }
                } else if (typeName == "DoubleProperty" && elementSize == 8) {
                    double value = 0.0;
                    if (try_read_bytes(at, &value, sizeof(value))) {
                        char number[64];
                        std::snprintf(number, sizeof(number), " = %.3f", value);
                        line += number;
                    }
                } else if ((typeName == "IntProperty" || typeName == "UInt32Property") && elementSize == 4) {
                    std::int32_t value = 0;
                    if (try_read_bytes(at, &value, sizeof(value))) {
                        char number[64];
                        std::snprintf(number, sizeof(number), " = %d", value);
                        line += number;
                    }
                } else if ((typeName == "Int8Property" || typeName == "Int16Property" ||
                            typeName == "UInt16Property" || typeName == "ByteProperty") &&
                           elementSize <= 2) {

                    std::uint8_t raw[2] = {0, 0};
                    if (try_read_bytes(at, raw, static_cast<std::size_t>(elementSize))) {
                        const unsigned value = elementSize == 1
                                                   ? static_cast<unsigned>(raw[0])
                                                   : static_cast<unsigned>(raw[0]) |
                                                         (static_cast<unsigned>(raw[1]) << 8);
                        char number[64];
                        std::snprintf(number, sizeof(number), " = %u", value);
                        line += number;
                    }
                } else if (typeName == "BoolProperty" && elementSize == 1) {
                    std::uint8_t value = 0;
                    if (try_read_bytes(at, &value, sizeof(value))) {
                        char number[64];
                        std::snprintf(number, sizeof(number), " = byte 0x%02X", static_cast<unsigned>(value));
                        line += number;
                    }
                }
            }
        } else {
            line += " -- offset/size implausible, NOT interpreted";
        }
        lines.push_back(line);
        ++printed;
        std::uint64_t next = 0;
        if (!try_read_u64(field + layout.next, next)) break;
        field = static_cast<std::uintptr_t>(next);
    }
    if (note.empty() && printed == 0) note = "the class declares no property of its own";
    return lines;
}

namespace {

bool class_chain_reaches(std::uintptr_t klass, const char* wanted, int maxDepth = 64) {
    auto cls = klass;
    for (int depth = 0; cls >= 0x10000 && depth < maxDepth; ++depth) {
        if (object_name_of(cls) == wanted) return true;
        std::uint64_t super = 0;
        if (!try_read_u64(cls + 0x0040, super)) return false;
        cls = static_cast<std::uintptr_t>(super);
    }
    return false;
}

std::string ammo_params_summary(std::uintptr_t actor) {
    unsigned char block[0x120] = {};
    if (!try_read_bytes(actor + 0x0228, block, sizeof(block))) return "params NOT READ";
    float operating = 0.0f, lifespan = 0.0f, speed = 0.0f, kinetic = 0.0f, thermal = 0.0f, homing = 0.0f;
    float radius = 0.0f;
    std::uint8_t pointDamage = 0, radialDamage = 0, homingOn = 0;
    std::memcpy(&operating, block + 0x00, sizeof(operating));
    std::memcpy(&lifespan, block + 0x04, sizeof(lifespan));
    std::memcpy(&speed, block + 0x08, sizeof(speed));
    pointDamage = block[0x20];
    std::memcpy(&kinetic, block + 0x24 + 0x08, sizeof(kinetic));
    std::memcpy(&thermal, block + 0x24 + 0x0C, sizeof(thermal));
    radialDamage = block[0x50];
    std::memcpy(&radius, block + 0x6C, sizeof(radius));
    homingOn = block[0x94];
    std::memcpy(&homing, block + 0x9C, sizeof(homing));
    std::uint64_t damageType = 0;
    std::memcpy(&damageType, block + 0xA8, sizeof(damageType));

    char buffer[512];
    std::snprintf(buffer, sizeof(buffer),
                  "operating=%.1f lifespan=%.2f speed=%.1f pointDamage=%u(kinetic=%.1f thermal=%.1f) "
                  "radialDamage=%u(radius=%.1f) homing=%u(%.2fs) damageType='%s'",
                  static_cast<double>(operating), static_cast<double>(lifespan),
                  static_cast<double>(speed), static_cast<unsigned>(pointDamage),
                  static_cast<double>(kinetic), static_cast<double>(thermal),
                  static_cast<unsigned>(radialDamage), static_cast<double>(radius),
                  static_cast<unsigned>(homingOn), static_cast<double>(homing),
                  damageType >= 0x10000 ? object_name_of(static_cast<std::uintptr_t>(damageType)).c_str()
                                        : "<null>");
    return buffer;
}

std::string ammo_actor_summary(std::uintptr_t actor) {
    std::uint64_t owner = 0, instigator = 0, target = 0, socket = 0;
    std::int32_t ignoreCount = -1;
    try_read_u64(actor + 0x00E0, owner);
    try_read_u64(actor + 0x0118, instigator);
    try_read_u64(actor + 0x0348, target);
    try_read_u64(actor + 0x0350, socket);
    try_read_bytes(actor + 0x0358 + 0x08, &ignoreCount, sizeof(ignoreCount));
    std::string socketName;
    std::string socketNote;
    if ((socket & 0xFFFFFFFFull) != 0) {
        resolve_fname(static_cast<std::uint32_t>(socket & 0xFFFFFFFFull), socketName, socketNote);
    }
    std::string damageTypeIgnored;
    const auto describe = [](std::uint64_t value) -> std::string {
        if (value < 0x10000) return "<null>";
        const auto object = static_cast<std::uintptr_t>(value);
        return object_name_of(object) + " [" + class_name_of(object) + "]";
    };
    return "owner=" + describe(owner) + " instigator=" + describe(instigator) +
           " target=" + describe(target) + " socket='" + socketName + "' ignoring=" +
           std::to_string(ignoreCount) + " | " + ammo_params_summary(actor);
}

}

std::string ammo_params_text(std::uintptr_t actor) { return ammo_params_summary(actor); }

namespace {

constexpr std::size_t kAmmoFrameWorldContext = 0x000;
constexpr std::size_t kAmmoFrameClass        = 0x008;
constexpr std::size_t kAmmoFrameTransform    = 0x010;
constexpr std::size_t kAmmoFrameCollision    = 0x040;
constexpr std::size_t kAmmoFrameOwner        = 0x048;
constexpr std::size_t kAmmoFrameParams       = 0x050;
constexpr std::size_t kAmmoFrameInstigator   = 0x170;
constexpr std::size_t kAmmoFrameTargetComp   = 0x178;
constexpr std::size_t kAmmoFrameTargetSocket = 0x180;
constexpr std::size_t kAmmoFrameIgnore       = 0x188;
constexpr std::size_t kAmmoFrameReturn       = 0x198;
constexpr std::size_t kAmmoFrameBytes        = 0x200;

constexpr std::uintptr_t kAmmoTargetComponent = 0x0348;
constexpr std::uintptr_t kAmmoTargetSocket    = 0x0350;

std::unordered_set<std::string> g_ammoUnresolvedPaths;

}

bool prepare_ammo_spawn(const AmmoSpawnRequest& request, const LockonTarget& homing,
                        AmmoSpawnPrepared& out, std::string& note) {
    out = AmmoSpawnPrepared{};
    note.clear();
    if (request.classPath.empty()) {
        note = "the request names no projectile class";
        return false;
    }

    if (g_ammoUnresolvedPaths.find(request.classPath) != g_ammoUnresolvedPaths.end()) {
        note = "the projectile class '" + request.classPath + "' already failed to resolve in this process, "
               "and it is not retried on every shot";
        return false;
    }
    std::uintptr_t ammoClass = load_asset_by_path(request.classPath);
    const std::size_t dot = request.classPath.rfind('.');
    const std::string wantedName =
        (dot == std::string::npos) ? request.classPath : request.classPath.substr(dot + 1);
    if (ammoClass < 0x10000 && !wantedName.empty()) {
        NamedObject found;
        if (find_uclass(wantedName.c_str(), found)) ammoClass = found.address;
    }
    if (ammoClass < 0x10000) {
        if (g_ammoUnresolvedPaths.size() < 64) g_ammoUnresolvedPaths.insert(request.classPath);
        note = "the projectile class '" + request.classPath + "' is neither loadable nor in this machine's "
               "object table";
        return false;
    }
    const std::string resolvedName = object_name_of(ammoClass);
    if (!wantedName.empty() && resolvedName != wantedName) {
        note = "the path '" + request.classPath + "' resolved to class '" + resolvedName + "', not the '" +
               wantedName + "' the host named -- refusing rather than spawning a projectile nobody asked for";
        return false;
    }

    static std::uintptr_t s_ammoCdo = 0;
    static std::uintptr_t s_ammoFn = 0;
    static ProcessEventFn s_pe = nullptr;
    static bool           s_resolved = false;
    static std::string    s_refusal;
    if (!s_resolved) {
        s_resolved = true;
        std::string refusal;
        if (!resolve_static_ufunction("DAGameplayStatics", "SpawnAmmo", s_ammoCdo, s_ammoFn, refusal)) {
            s_refusal = refusal;
        } else {
            const ProcessEventResolution pe = resolve_process_event(live_world_address());
            if (!pe.usable) {
                s_refusal = "ProcessEvent is not usable (" + pe.note + ")";
            } else {
                s_pe = reinterpret_cast<ProcessEventFn>(pe.address);
            }
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }

    std::memcpy(out.params, request.params, sizeof(out.params));
    std::string unresolved;
    const auto fillPointer = [&](std::size_t blockOffset, const std::string& path, const char* what) {
        if (path.empty()) return;
        std::uintptr_t resolved = load_asset_by_path(path);
        if (resolved < 0x10000) resolved = 0;
        if (resolved == 0) {
            if (!unresolved.empty()) unresolved += ", ";
            unresolved += std::string(what) + " '" + path + "'";
        }
        std::memcpy(out.params + blockOffset, &resolved, sizeof(resolved));
    };
    fillPointer(0x18, request.speedCurvePath, "speed transition curve");
    fillPointer(0x48, request.pointDamageCurvePath, "point damage transition curve");
    fillPointer(0xA8, request.damageTypePath, "damage type");

    out.actorClass = ammoClass;
    out.processEvent = reinterpret_cast<std::uintptr_t>(s_pe);
    out.gameplayStaticsCdo = s_ammoCdo;
    out.spawnAmmoFunction = s_ammoFn;
    out.classNameForLog = wantedName;

    out.targetComponent = homing.component;
    out.targetSocketRaw = homing.socketRaw;
    out.targetSocketName = homing.socketName;
    if (homing.component != 0) {
        static std::uintptr_t s_setTarget = 0;
        static std::uintptr_t s_setSocket = 0;
        static bool           s_verbsResolved = false;
        if (!s_verbsResolved) {
            s_verbsResolved = true;
            const std::uintptr_t iface = resolve_uclass_address("DAAmmoInterface");
            if (iface >= 0x10000) {
                const FunctionRef setTarget = reflect_resolve_function(iface, "SetAmmoTargetComponent");
                const FunctionRef setSocket = reflect_resolve_function(iface, "SetAmmoTargetSocket");
                if (setTarget.usable) s_setTarget = setTarget.function;
                if (setSocket.usable) s_setSocket = setSocket.function;
            }
        }
        out.setTargetFunction = s_setTarget;
        out.setSocketFunction = s_setSocket;
    }

    note = unresolved.empty() ? std::string() : ("UNRESOLVED and left null: " + unresolved);
    return true;
}

bool read_actor_placement(std::uintptr_t actor, ActorPlacement& out, std::string& note) {
    out = ActorPlacement{};
    note.clear();
    if (actor < 0x10000) {
        note = "no actor was given";
        return false;
    }
    const std::uintptr_t actorClass = resolve_uclass_address("Actor");
    if (actorClass < 0x10000) {
        note = "the Actor UClass is not in this process's object table";
        return false;
    }
    const FunctionRef location = reflect_resolve_function(actorClass, "K2_GetActorLocation");
    const FunctionRef rotation = reflect_resolve_function(actorClass, "K2_GetActorRotation");
    if (!location.usable || !rotation.usable) {
        note = std::string("could not resolve ") + (location.usable ? "" : "'K2_GetActorLocation' ") +
               (rotation.usable ? "" : "'K2_GetActorRotation' ") + "on the Actor UClass";
        return false;
    }
    unsigned char locationFrame[0x10] = {};
    unsigned char rotationFrame[0x10] = {};
    std::string callNote;
    if (!reflect_invoke_function(actor, location, locationFrame, callNote)) {
        note = "K2_GetActorLocation refused: " + callNote;
        return false;
    }
    if (!reflect_invoke_function(actor, rotation, rotationFrame, callNote)) {
        note = "K2_GetActorRotation refused: " + callNote;
        return false;
    }
    std::memcpy(out.locationCm, locationFrame + 0x00, sizeof(out.locationCm));
    std::memcpy(&out.pitch, rotationFrame + 0x00, sizeof(out.pitch));
    std::memcpy(&out.yaw, rotationFrame + 0x04, sizeof(out.yaw));
    std::memcpy(&out.roll, rotationFrame + 0x08, sizeof(out.roll));
    out.valid = true;
    return true;
}

bool read_component_location(std::uintptr_t component, ComponentPlacement& out, std::string& note) {
    out = ComponentPlacement{};
    note.clear();
    if (component < 0x10000) {
        note = "no component was given";
        return false;
    }
    static std::uintptr_t sceneClass = 0;
    static FunctionRef    getLocation;
    static std::string    resolveNote;
    if (sceneClass == 0) {
        sceneClass = resolve_uclass_address("SceneComponent");
        if (sceneClass >= 0x10000) {
            getLocation = reflect_resolve_function(sceneClass, "K2_GetComponentLocation");
        }
        if (sceneClass < 0x10000) {
            resolveNote = "the SceneComponent UClass is not in this process's object table";
        } else if (!getLocation.usable) {
            resolveNote = "K2_GetComponentLocation is not usable: " + getLocation.note;
        }
    }
    if (sceneClass < 0x10000 || !getLocation.usable) {
        note = resolveNote;
        return false;
    }
    unsigned char frame[0x10] = {};
    std::string callNote;
    if (!reflect_invoke_function(component, getLocation, frame, callNote)) {
        note = "K2_GetComponentLocation refused: " + callNote;
        return false;
    }
    std::memcpy(out.locationCm, frame + 0x00, sizeof(out.locationCm));
    out.valid = true;
    return true;
}

bool read_viewport_size(std::uintptr_t playerController, std::int32_t& width, std::int32_t& height,
                        std::string& note) {
    width = 0;
    height = 0;
    note.clear();
    if (playerController < 0x10000) {
        note = "no player controller was given";
        return false;
    }
    static std::uintptr_t controllerClass = 0;
    static FunctionRef    getViewport;
    static std::string    resolveNote;
    if (controllerClass == 0) {
        controllerClass = resolve_uclass_address("PlayerController");
        if (controllerClass >= 0x10000) {
            getViewport = reflect_resolve_function(controllerClass, "GetViewportSize");
        }
        if (controllerClass < 0x10000) {
            resolveNote = "the PlayerController UClass is not in this process's object table";
        } else if (!getViewport.usable) {
            resolveNote = "GetViewportSize is not usable: " + getViewport.note;
        }
    }
    if (controllerClass < 0x10000 || !getViewport.usable) {
        note = resolveNote;
        return false;
    }
    unsigned char frame[0x08] = {};
    std::string   callNote;
    if (!reflect_invoke_function(playerController, getViewport, frame, callNote)) {
        note = "GetViewportSize refused: " + callNote;
        return false;
    }
    std::int32_t w = 0;
    std::int32_t h = 0;
    std::memcpy(&w, frame + 0x00, sizeof(w));
    std::memcpy(&h, frame + 0x04, sizeof(h));
    width = w;
    height = h;
    if (w <= 0 || h <= 0) {
        note = "the engine answered with a viewport of " + std::to_string(w) + "x" + std::to_string(h);
        return false;
    }
    return true;
}

bool project_world_to_screen(std::uintptr_t playerController, const float worldCm[3], float& screenX,
                             float& screenY, std::string& note) {
    screenX = 0.0f;
    screenY = 0.0f;
    note.clear();
    if (playerController < 0x10000) {
        note = "no player controller was given";
        return false;
    }
    static std::uintptr_t controllerClass = 0;
    static FunctionRef    project;
    static std::string    resolveNote;
    if (controllerClass == 0) {
        controllerClass = resolve_uclass_address("PlayerController");
        if (controllerClass >= 0x10000) {
            project = reflect_resolve_function(controllerClass, "ProjectWorldLocationToScreen");
        }
        if (controllerClass < 0x10000) {
            resolveNote = "the PlayerController UClass is not in this process's object table";
        } else if (!project.usable) {
            resolveNote = "ProjectWorldLocationToScreen is not usable: " + project.note;
        }
    }
    if (controllerClass < 0x10000 || !project.usable) {
        note = resolveNote;
        return false;
    }
    unsigned char frame[0x18] = {};
    float         place[3] = {worldCm[0], worldCm[1], worldCm[2]};
    std::memcpy(frame + 0x00, place, sizeof(place));
    std::string callNote;
    if (!reflect_invoke_function(playerController, project, frame, callNote)) {
        note = "ProjectWorldLocationToScreen refused: " + callNote;
        return false;
    }
    if (frame[0x15] == 0) {
        note = "the engine says that point is off the view";
        return false;
    }
    std::memcpy(&screenX, frame + 0x0C, sizeof(screenX));
    std::memcpy(&screenY, frame + 0x10, sizeof(screenY));
    return true;
}

bool set_hidden_in_game(std::uintptr_t component, bool hidden, std::string& note) {
    note.clear();
    if (component < 0x10000) {
        note = "no component was given";
        return false;
    }
    static std::uintptr_t sceneClass = 0;
    static FunctionRef    setHidden;
    static std::string    resolveNote;
    if (sceneClass == 0) {
        sceneClass = resolve_uclass_address("SceneComponent");
        if (sceneClass >= 0x10000) {
            setHidden = reflect_resolve_function(sceneClass, "SetHiddenInGame");
        }
        if (sceneClass < 0x10000) {
            resolveNote = "the SceneComponent UClass is not in this process's object table";
        } else if (!setHidden.usable) {
            resolveNote = "SetHiddenInGame is not usable: " + setHidden.note;
        }
    }
    if (sceneClass < 0x10000 || !setHidden.usable) {
        note = resolveNote;
        return false;
    }
    unsigned char frame[0x02] = {};
    std::string   callNote;
    frame[0x00] = hidden ? 1 : 0;
    frame[0x01] = 0;
    if (!reflect_invoke_function(component, setHidden, frame, callNote)) {
        note = "SetHiddenInGame refused: " + callNote;
        return false;
    }
    return true;
}

bool locate_render_primitive(std::uintptr_t component, const float expectedLocationCm[3],
                             RenderPrimitive& out) {
    return scan_render_primitive(component, expectedLocationCm, &try_read_bytes, out);
}

bool read_render_primitive(std::uintptr_t component, std::uintptr_t proxySlot, std::uintptr_t matrixOffset,
                           std::uintptr_t sceneInfoSlot, RenderPrimitive& out) {
    return read_render_primitive_at(component, proxySlot, matrixOffset, sceneInfoSlot, &try_read_bytes, out);
}

WorldBounds read_world_bounds() {
    WorldBounds out;
    const std::uintptr_t world = live_world_address();
    if (world < 0x10000) {
        out.note = "there is no live world to ask";
        return out;
    }
    static std::uintptr_t worldClass = 0;
    static FunctionRef    getSettings;
    static std::string    resolveNote;
    if (worldClass == 0) {
        worldClass = resolve_uclass_address("World");
        if (worldClass >= 0x10000) {
            getSettings = reflect_resolve_function(worldClass, "K2_GetWorldSettings");
        }
        if (worldClass < 0x10000) {
            resolveNote = "the World UClass is not in this process's object table";
        } else if (!getSettings.usable) {
            resolveNote = "K2_GetWorldSettings is not usable: " + getSettings.note;
        }
    }
    if (worldClass < 0x10000 || !getSettings.usable) {
        out.note = resolveNote;
        return out;
    }
    unsigned char frame[0x10] = {};
    std::string callNote;
    if (!reflect_invoke_function(world, getSettings, frame, callNote)) {
        out.note = "K2_GetWorldSettings refused: " + callNote;
        return out;
    }
    std::uintptr_t settings = 0;
    std::memcpy(&settings, frame + 0x00, sizeof(settings));
    if (settings < 0x10000) {
        out.note = "K2_GetWorldSettings answered with no world settings at all";
        return out;
    }
    const std::string className = object_class_name(settings);
    if (className != "WorldSettings") {
        out.note = "K2_GetWorldSettings answered with a '" + className +
                   "', which is not the world's settings -- no offset of ours is applied to it";
        return out;
    }
    if (!read_process_bytes(settings + 0x0244, &out.killZ, sizeof(out.killZ))) {
        out.note = "the world settings' KillZ could not be read";
        return out;
    }
    unsigned char flags = 0;
    if (!read_process_bytes(settings + 0x022D, &flags, sizeof(flags))) {
        out.note = "the world settings' flags byte could not be read";
        return out;
    }
    out.boundsChecks = (flags & 0x04) != 0;
    out.read = true;
    return out;
}

bool read_actor_bounds(std::uintptr_t actor, ActorBox& out, std::string& note) {
    out = ActorBox{};
    note.clear();
    if (actor < 0x10000) {
        note = "no actor was given";
        return false;
    }
    static std::uintptr_t actorClass = 0;
    static FunctionRef    getBounds;
    static std::string    resolveNote;
    if (actorClass == 0) {
        actorClass = resolve_uclass_address("Actor");
        if (actorClass >= 0x10000) getBounds = reflect_resolve_function(actorClass, "GetActorBounds");
        if (actorClass < 0x10000) {
            resolveNote = "the Actor UClass is not in this process's object table";
        } else if (!getBounds.usable) {
            resolveNote = "GetActorBounds is not usable: " + getBounds.note;
        }
    }
    if (actorClass < 0x10000 || !getBounds.usable) {
        note = resolveNote;
        return false;
    }
    unsigned char frame[0x20] = {};
    frame[0x00] = 1;
    frame[0x1C] = 0;
    std::string callNote;
    if (!reflect_invoke_function(actor, getBounds, frame, callNote)) {
        note = "GetActorBounds refused: " + callNote;
        return false;
    }
    std::memcpy(out.originCm, frame + 0x04, sizeof(out.originCm));
    std::memcpy(out.extentCm, frame + 0x10, sizeof(out.extentCm));
    out.valid = true;
    return true;
}

bool read_ammo_homing_target(std::uintptr_t ammoActor, AmmoHomingTarget& out, std::string& note) {
    out = AmmoHomingTarget{};
    note.clear();
    if (ammoActor < 0x10000) {
        note = "no projectile was given";
        return false;
    }

    std::string why;
    if (!object_is_still_registered(ammoActor, why)) {
        note = "the projectile is no longer in the object table (" + why + ")";
        return false;
    }
    std::uint64_t raw = 0;
    if (!try_read_u64(ammoActor + kAmmoTargetComponent, raw)) {
        note = "ADAAmmo::TargetComponent could not be read";
        return false;
    }
    if (raw < 0x10000) return true;
    const auto component = static_cast<std::uintptr_t>(raw);
    if (!object_is_live(component, why)) {
        note = "the projectile's TargetComponent points at a dead object (" + why + ")";
        return false;
    }
    std::uint64_t componentClass = 0;
    if (!try_read_u64(component + offsets::kUObjectClassOffset, componentClass) ||
        componentClass < 0x10000) {
        note = "the target component's own class could not be read";
        return false;
    }
    if (!class_chain_reaches(static_cast<std::uintptr_t>(componentClass), "SceneComponent")) {
        note = "the projectile's TargetComponent is a '" +
               object_class_name(component) + "', which is not a scene component -- so is not that "
               "field on this build, and nothing is taken from it";
        return false;
    }
    const std::uintptr_t actor = outer_of(component);
    if (actor < 0x10000 || !object_is_live(actor, why)) {
        note = "the target component's Outer is not a live actor, so the projectile's target cannot be named";
        return false;
    }
    out.component = component;
    out.componentName = object_own_name(component);
    out.componentClass = object_class_name(component);
    out.actor = actor;
    out.actorName = object_own_name(actor);
    out.actorClass = object_class_name(actor);

    std::uint64_t socket = 0;
    if (try_read_u64(ammoActor + kAmmoTargetSocket, socket)) {
        out.socketRaw = socket;
        std::string text;
        if (name_of(static_cast<std::uint32_t>(socket & 0xFFFFFFFFu), text)) out.socketName = text;
    }
    return true;
}

bool read_lockon_target(std::uintptr_t targetActor, LockonTarget& out, std::string& note) {
    out = LockonTarget{};
    note.clear();
    if (targetActor < 0x10000) {
        note = "no target actor on this side";
        return false;
    }

    std::string why;
    if (!object_is_still_registered(targetActor, why)) {
        note = "the target actor is no longer in the object table (" + why + ") -- the interface was not asked";
        return false;
    }
    static std::uintptr_t s_interface = 0;
    static bool           s_resolved = false;
    static std::string    s_refusal;
    if (!s_resolved) {
        s_resolved = true;
        s_interface = resolve_uclass_address("DALockonTargetInterface");
        if (s_interface < 0x10000) {
            s_refusal = "the DALockonTargetInterface UClass is not in this process's object table";
        }
    }
    if (!s_refusal.empty()) {
        note = s_refusal;
        return false;
    }
    const FunctionRef componentFn = reflect_resolve_function(s_interface, "GetLockonTargetComponent");
    if (!componentFn.usable) {
        note = "DALockonTargetInterface declares no 'GetLockonTargetComponent' this build can find";
        return false;
    }
    alignas(8) unsigned char frame[0x10] = {};
    std::string callNote;
    if (!reflect_invoke_function(targetActor, componentFn, frame, callNote)) {
        note = "GetLockonTargetComponent refused: " + callNote;
        return false;
    }
    out.asked = true;
    std::uint64_t component = 0;
    std::memcpy(&component, frame + 0x00, sizeof(component));
    if (component >= 0x10000) {
        out.component = static_cast<std::uintptr_t>(component);
        out.componentName = object_own_name(out.component);
    }
    const FunctionRef socketFn = reflect_resolve_function(s_interface, "GetLockonTargetSocket");
    if (socketFn.usable) {
        alignas(8) unsigned char sframe[0x10] = {};
        if (reflect_invoke_function(targetActor, socketFn, sframe, callNote)) {
            std::memcpy(&out.socketRaw, sframe + 0x00, sizeof(out.socketRaw));
            std::string text;
            if (name_of(static_cast<std::uint32_t>(out.socketRaw & 0xFFFFFFFFu), text)) {
                out.socketName = text;
            }
        }
    }
    return true;
}

bool spawn_ammo_from_params(const AmmoSpawnPrepared& prepared, const ActorPlacement& placement,
                             std::uintptr_t owner, std::uintptr_t instigator, std::uintptr_t& actorOut,
                             std::string& note) {
    actorOut = 0;
    note.clear();
    if (prepared.actorClass < 0x10000 || prepared.processEvent == 0 ||
        prepared.spawnAmmoFunction < 0x10000) {
        note = "the projectile was not prepared (class, ProcessEvent or SpawnAmmo is missing)";
        return false;
    }

    alignas(16) unsigned char frame[kAmmoFrameBytes] = {};
    const std::uintptr_t world = live_world_address();
    std::memcpy(frame + kAmmoFrameWorldContext, &world, sizeof(world));
    const std::uintptr_t ammoClass = prepared.actorClass;
    std::memcpy(frame + kAmmoFrameClass, &ammoClass, sizeof(ammoClass));

    constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;
    float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    {
        const float pitch = placement.pitch * kDegreesToRadians;
        const float yaw = placement.yaw * kDegreesToRadians;
        const float roll = placement.roll * kDegreesToRadians;
        const float sp = std::sin(pitch * 0.5f), cp = std::cos(pitch * 0.5f);
        const float sy = std::sin(yaw * 0.5f), cy = std::cos(yaw * 0.5f);
        const float sr = std::sin(roll * 0.5f), cr = std::cos(roll * 0.5f);

        quat[0] = cr * sp * sy - sr * cp * cy;
        quat[1] = -cr * sp * cy - sr * cp * sy;
        quat[2] = cr * cp * sy - sr * sp * cy;
        quat[3] = cr * cp * cy + sr * sp * sy;
    }
    const float scale[3] = {1.0f, 1.0f, 1.0f};
    std::memcpy(frame + kAmmoFrameTransform + 0x00, quat, sizeof(quat));
    std::memcpy(frame + kAmmoFrameTransform + 0x10, placement.locationCm, sizeof(placement.locationCm));
    std::memcpy(frame + kAmmoFrameTransform + 0x20, scale, sizeof(scale));

    frame[kAmmoFrameCollision] = 0;
    std::memcpy(frame + kAmmoFrameOwner, &owner, sizeof(owner));
    std::memcpy(frame + kAmmoFrameParams, prepared.params, sizeof(prepared.params));
    std::memcpy(frame + kAmmoFrameInstigator, &instigator, sizeof(instigator));

    {
        std::uintptr_t targetComponent = prepared.targetComponent;
        std::uint64_t  targetSocket = prepared.targetSocketRaw;
        std::memcpy(frame + kAmmoFrameTargetComp, &targetComponent, sizeof(targetComponent));
        std::memcpy(frame + kAmmoFrameTargetSocket, &targetSocket, sizeof(targetSocket));
    }

    {
        const std::int32_t count = 0;
        const std::int32_t capacity = 0;
        void* const data = nullptr;
        std::memcpy(frame + kAmmoFrameIgnore + 0x00, &data, sizeof(data));
        std::memcpy(frame + kAmmoFrameIgnore + 0x08, &count, sizeof(count));
        std::memcpy(frame + kAmmoFrameIgnore + 0x0C, &capacity, sizeof(capacity));
    }
    const ProcessEventFn processEvent = reinterpret_cast<ProcessEventFn>(prepared.processEvent);
    processEvent(reinterpret_cast<void*>(prepared.gameplayStaticsCdo),
                 reinterpret_cast<void*>(prepared.spawnAmmoFunction), frame);

    std::memcpy(&actorOut, frame + kAmmoFrameReturn, sizeof(actorOut));
    if (actorOut < 0x10000) {
        actorOut = 0;
        return false;
    }
    char where[240];
    std::snprintf(where, sizeof(where),
                  "at (%.1f, %.1f, %.1f) cm, rotation pitch %.1f yaw %.1f roll %.1f degrees (the host's own)",
                  static_cast<double>(placement.locationCm[0]), static_cast<double>(placement.locationCm[1]),
                  static_cast<double>(placement.locationCm[2]), static_cast<double>(placement.pitch),
                  static_cast<double>(placement.yaw), static_cast<double>(placement.roll));

    std::uintptr_t haveOwner = 0, haveInstigator = 0, haveTarget = 0;
    std::uint64_t  haveSocket = 0;
    (void)read_process_bytes(actorOut + 0x00E0, &haveOwner, sizeof(haveOwner));
    (void)read_process_bytes(actorOut + 0x0118, &haveInstigator, sizeof(haveInstigator));
    (void)read_process_bytes(actorOut + kAmmoTargetComponent, &haveTarget, sizeof(haveTarget));
    (void)read_process_bytes(actorOut + kAmmoTargetSocket, &haveSocket, sizeof(haveSocket));

    std::string homingNote = "no homing target";
    if (prepared.targetComponent != 0) {
        std::string verb = "the frame's target took";
        if (haveTarget != prepared.targetComponent && prepared.setTargetFunction >= 0x10000) {
            alignas(8) unsigned char tframe[0x10] = {};
            std::uintptr_t wanted = prepared.targetComponent;
            std::memcpy(tframe, &wanted, sizeof(wanted));
            FunctionRef ref;
            ref.function = prepared.setTargetFunction;
            ref.processEvent = prepared.processEvent;
            ref.usable = (ref.function >= 0x10000 && ref.processEvent != 0);
            std::string callNote;
            if (reflect_invoke_function(actorOut, ref, tframe, callNote)) {
                (void)read_process_bytes(actorOut + kAmmoTargetComponent, &haveTarget, sizeof(haveTarget));
                verb = "SetAmmoTargetComponent was called";
            } else {
                verb = "SetAmmoTargetComponent could not be called (" + callNote + ")";
            }
        }
        homingNote = verb + (haveTarget == prepared.targetComponent
                                 ? ", and the copy holds it"
                                 : ", and the copy still does NOT hold it");
        if (haveSocket != prepared.targetSocketRaw && prepared.setSocketFunction >= 0x10000) {
            alignas(8) unsigned char sframe[0x10] = {};
            std::uint64_t wanted = prepared.targetSocketRaw;
            std::memcpy(sframe, &wanted, sizeof(wanted));
            FunctionRef ref;
            ref.function = prepared.setSocketFunction;
            ref.processEvent = prepared.processEvent;
            ref.usable = (ref.function >= 0x10000 && ref.processEvent != 0);
            std::string callNote;
            if (reflect_invoke_function(actorOut, ref, sframe, callNote)) {
                (void)read_process_bytes(actorOut + kAmmoTargetSocket, &haveSocket, sizeof(haveSocket));
                homingNote += "; the socket went through SetAmmoTargetSocket";
            }
        }
    }

    char effects[200];
    std::snprintf(effects, sizeof(effects),
                  "owner %s (0x%llX), instigator 0x%llX",
                  haveOwner == owner ? "is this side's puppet" : "is NOT the puppet",
                  static_cast<unsigned long long>(haveOwner),
                  static_cast<unsigned long long>(haveInstigator));

    note = "spawned '" + object_name_of(actorOut) + "' [" + object_class_name(actorOut) + "] of class '" +
           prepared.classNameForLog + "' " + where + "; " + effects + ", " + homingNote +
           "; the params the copy carries: " + ammo_params_summary(actorOut);
    return true;
}

}
