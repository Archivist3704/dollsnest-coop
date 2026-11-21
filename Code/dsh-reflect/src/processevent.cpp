#include "dsh/reflect/processevent.h"

#include "dsh/platform/log.h"
#include "dsh/reflect/reflection.h"
#include "dsh/reflect/scanner.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace dsh::reflect {
namespace {

constexpr std::uint64_t kNamePrivateOffset = 0x18;
constexpr std::uint64_t kVtablePointerOffset = 0x00;

std::uintptr_t find_uobject_class_object() {

    std::unordered_map<std::uintptr_t, std::string> classNames;

    std::uintptr_t objectClass = 0;
    for_each_object(300000, [&](std::uintptr_t obj) {
        if (objectClass) return;
        std::uintptr_t cls = 0;
        if (!try_read_u64(obj + 0x10, cls)) return;
        cls = static_cast<std::uintptr_t>(cls);
        if (!looks_like_image_pointer(cls)) return;

        auto it = classNames.find(cls);
        if (it == classNames.end()) {
            std::string clsName;
            if (!class_name_of_object(cls, clsName)) return;
            it = classNames.emplace(cls, std::move(clsName)).first;
        }
        if (it->second != "Class") return;

        std::uint64_t nameIndexRaw = 0;
        if (!try_read_u64(obj + kNamePrivateOffset, nameIndexRaw)) return;
        std::string name;
        if (!name_of(static_cast<std::uint32_t>(nameIndexRaw & 0xFFFFFFFFu), name)) return;
        if (name == "Object") objectClass = obj;
    });
    return objectClass;
}

std::uintptr_t find_instance_of(std::uintptr_t classObject) {
    std::uintptr_t instance = 0;
    for_each_object(300000, [&](std::uintptr_t obj) {
        if (instance) return;
        if (obj == classObject) return;
        std::uintptr_t cls = 0;
        if (!try_read_u64(obj + 0x10, cls)) return;
        if (static_cast<std::uintptr_t>(cls) == classObject) instance = obj;
    });
    return instance;
}

struct Fingerprint {
    int         score = 0;
    std::string evidence;
};

Fingerprint fingerprint_function(std::uintptr_t fn) {
    Fingerprint fp;
    unsigned char code[64] = {};
    if (!try_read_bytes(fn, code, sizeof(code))) {
        return fp;
    }

    char detail[256] = {};

    bool touchesNameOffset = false;
    for (int i = 0; i + 4 < 48; ++i) {
        const unsigned char b0 = code[i];

        if ((b0 == 0x48 || b0 == 0x4C) && (code[i + 1] == 0x8B || code[i + 1] == 0x8D)) {
            const unsigned char modrm = code[i + 2];

            if ((modrm & 0xC7) == 0x40 || (modrm & 0xC7) == 0x41) {
                if (code[i + 3] == static_cast<unsigned char>(kNamePrivateOffset)) {
                    touchesNameOffset = true;
                    break;
                }
            }
        }
    }
    if (touchesNameOffset) {
        fp.score += 2;
        std::strcat(detail, "reads[];");
    }

    bool hasShift16 = false;
    for (int i = 0; i + 3 < 56; ++i) {

        if (code[i] == 0xC1 && (code[i + 1] & 0x38) == 0x28 && code[i + 2] == 0x10) {
            hasShift16 = true;
            break;
        }
        if ((code[i] == 0x48 || code[i] == 0x4C) && code[i + 1] == 0xC1 &&
            (code[i + 2] & 0x38) == 0x28 && code[i + 3] == 0x10) {
            hasShift16 = true;
            break;
        }
    }
    if (hasShift16) {
        fp.score += 1;
        std::strcat(detail, "shr16;");
    }

    bool hasIndirectCall = false;
    for (int i = 0; i + 6 < 64; ++i) {
        if (code[i] != 0xFF) continue;
        const unsigned char m = code[i + 1];
        if ((m & 0xF8) == 0xD0 || (m & 0xF8) == 0x50 || m == 0x15) {
            hasIndirectCall = true;
            break;
        }
    }
    if (hasIndirectCall) {
        fp.score += 1;
        std::strcat(detail, "indirect-call;");
    }

    if (code[0] == 0xCC || code[0] == 0x00) {
        fp.score -= 2;
        std::strcat(detail, "padding-start(NEGATIVE);");
    }

    if (code[0] == 0x48 || code[0] == 0x4C || code[0] == 0x40 || code[0] == 0x53 ||
        code[0] == 0x55 || code[0] == 0x56 || code[0] == 0x57) {
        fp.score += 1;
        std::strcat(detail, "prologue;");
    }

    std::snprintf(detail + std::strlen(detail), sizeof(detail) - std::strlen(detail),
                  " bytes=%02X %02X %02X %02X %02X %02X %02X %02X", code[0], code[1], code[2],
                  code[3], code[4], code[5], code[6], code[7]);
    fp.evidence = detail;
    return fp;
}

}

std::string ProcessEventSearch::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "processevent: found=%s bestSlot=%u score=%d slotsScanned=%u readable=%u note=%s",
                  found ? "yes" : "no", best.vtableIndex, best.score, vtableSlotsScanned,
                  readableSlots, note.c_str());
    return buf;
}

ProcessEventSearch find_process_event() {
    ProcessEventSearch search;

    const ReflectionState& refl = reflection_state();
    if (!refl.objectArrayReady || !refl.nameTableReady) {
        search.note = "reflection tables not available (run 'scan' first)";
        return search;
    }

    const std::uintptr_t uobjectClass = find_uobject_class_object();
    if (!uobjectClass) {
        search.note = "could not find the UObject class object by name";
        return search;
    }
    std::uintptr_t instance = find_instance_of(uobjectClass);
    if (!instance) {

        instance = uobjectClass;
        DSH_WARN("processevent: no plain UObject instance found; falling back to the class "
                 "object's vtable (UClass), which may sit at a different slot");
    }

    std::uint64_t vtable = 0;
    if (!try_read_u64(instance + kVtablePointerOffset, vtable)) {
        search.note = "could not read the instance vtable pointer";
        return search;
    }
    search.uobjectVtable = static_cast<std::uintptr_t>(vtable);
    if (!looks_like_image_pointer(search.uobjectVtable)) {
        search.note = "vtable pointer is not inside a loaded module";
        return search;
    }

    DSH_INFO("processevent: instance=0x%llX vtable=0x%llX (scanning slots)",
             static_cast<unsigned long long>(instance),
             static_cast<unsigned long long>(search.uobjectVtable));

    constexpr std::uint32_t kMaxSlots = 512;
    for (std::uint32_t slot = 0; slot < kMaxSlots; ++slot) {
        std::uint64_t fn = 0;
        if (!try_read_u64(search.uobjectVtable + slot * 8ull, fn)) {

            break;
        }
        ++search.vtableSlotsScanned;
        if (!looks_like_image_pointer(static_cast<std::uintptr_t>(fn))) continue;
        ++search.readableSlots;

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(fn), &mbi, sizeof(mbi)) == 0) continue;
        const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                           PAGE_EXECUTE_WRITECOPY;
        if ((mbi.Protect & exec) == 0) continue;

        const auto fp = fingerprint_function(static_cast<std::uintptr_t>(fn));
        if (fp.score <= 0) continue;

        ProcessEventCandidate c;
        c.vtableIndex = slot;
        c.address = static_cast<std::uintptr_t>(fn);
        c.score = fp.score;
        c.evidence = fp.evidence;
        search.all.push_back(c);
    }

    if (search.all.empty()) {
        search.note = "no vtable slot matched the structural fingerprint";
        return search;
    }

    std::size_t bestIdx = 0;
    for (std::size_t i = 1; i < search.all.size(); ++i) {
        if (search.all[i].score > search.all[bestIdx].score) bestIdx = i;
    }
    search.best = search.all[bestIdx];
    search.found = search.best.plausible();
    search.note = search.found ? "ok (structural match)" : "best candidate is weak";

    for (const auto& c : search.all) {
        DSH_INFO("processevent: candidate slot=%u addr=0x%llX score=%d %s", c.vtableIndex,
                 static_cast<unsigned long long>(c.address), c.score, c.evidence.c_str());
    }
    return search;
}

}
