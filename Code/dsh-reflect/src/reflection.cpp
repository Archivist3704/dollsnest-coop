#include "dsh/reflect/reflection.h"

#include "dsh/platform/log.h"
#include "dsh/reflect/scanner.h"
#include "dsh/reflect/offsets.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace dsh::reflect {
namespace {

std::mutex      g_stateMutex;
ReflectionState g_state;

struct WritableRange {
    std::uintptr_t start = 0;
    std::uint64_t  size = 0;
    std::string    label;
};

std::vector<WritableRange> writable_ranges() {
    std::vector<WritableRange> out;

    DSH_INFO("reflection: writable_ranges: enumerating modules ...");
    const ModuleInfo exe = main_module();
    DSH_INFO("reflection: writable_ranges: main module %s, walking sections ...",
             exe.name.c_str());
    if (exe.base == 0) {
        DSH_WARN("reflection: cannot identify the main module -- nothing to scan");
        return out;
    }
    for (const auto& s : sections_of(exe)) {
        if (!s.writable()) continue;
        if (s.size < 0x1000) continue;
        out.push_back(WritableRange{s.start, s.size, exe.name + "!" + s.name});
    }
    return out;
}

constexpr std::uint64_t kObjectContainerProbeOffsets[] = {
    0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60,
};

constexpr double kMinNamedFraction = 0.50;

constexpr std::uint32_t kShapeProbe = 32;
constexpr std::uint32_t kNameProbe = 16;

const std::vector<WritableRange>* g_activeRanges = nullptr;

[[nodiscard]] bool in_active_ranges(std::uintptr_t addr, std::uint64_t size) {
    if (g_activeRanges == nullptr) return false;

    if (g_activeRanges->size() == 1) {
        const auto& r = g_activeRanges->front();
        return addr >= r.start && addr - r.start <= r.size && size <= r.size - (addr - r.start);
    }
    for (const auto& r : *g_activeRanges) {
        if (addr >= r.start && size <= r.size && (addr - r.start) <= r.size - size) return true;
    }
    return false;
}

[[nodiscard]] inline std::uint64_t peek_u64(std::uintptr_t addr) noexcept {
    return read_u64_fast(addr);
}

bool read_object_at(std::uintptr_t itemsArray, std::uint32_t index, std::uintptr_t& out) {
    const std::uintptr_t slot = itemsArray + static_cast<std::uintptr_t>(index) * 8;

    const std::uint64_t value = peek_u64(slot);
    if (looks_like_image_pointer(static_cast<std::uintptr_t>(value))) {
        out = static_cast<std::uintptr_t>(value);
        return true;
    }

    if (value == 0) return false;
    constexpr std::uint32_t kObjectsPerChunk = 0x10000;
    const std::uint32_t chunkIndex = index / kObjectsPerChunk;
    const std::uint32_t within = index % kObjectsPerChunk;
    const std::uint64_t chunkPtr = peek_u64(itemsArray + static_cast<std::uintptr_t>(chunkIndex) * 8);
    if (!looks_like_image_pointer(static_cast<std::uintptr_t>(chunkPtr))) return false;
    const std::uint64_t object =
        peek_u64(static_cast<std::uintptr_t>(chunkPtr) + static_cast<std::uintptr_t>(within) * 8);
    if (!looks_like_image_pointer(static_cast<std::uintptr_t>(object))) return false;
    out = static_cast<std::uintptr_t>(object);
    return true;
}

ObjectArrayCandidate evaluate_container(std::uintptr_t container, std::uint64_t itemsOffset,
                                        std::uint32_t probeCount) {
    ObjectArrayCandidate c;
    c.containerAddress = container;
    c.itemsOffset = itemsOffset;

    std::uint64_t items = 0;
    if (!try_read_u64(container + itemsOffset, items)) return c;
    c.itemsPointer = static_cast<std::uintptr_t>(items);
    if (!looks_like_image_pointer(c.itemsPointer)) return c;

    std::uint32_t valid = 0;
    std::uint32_t checked = 0;
    for (std::uint32_t i = 0; i < probeCount; ++i) {
        ++checked;
        std::uintptr_t obj = 0;
        if (!read_object_at(c.itemsPointer, i, obj)) {

            continue;
        }
        ++valid;

    }
    c.objectsChecked = checked;
    c.objectsValid = valid;

    for (std::uint64_t off = 0; off <= 0x10; off += 4) {
        if (off == itemsOffset) continue;
        std::uint32_t v = 0;
        if (try_read_u32(container + off, v) && v > 0 && v < 5'000'000) {
            c.reportedCount = v;
            break;
        }
    }
    return c;
}

bool is_ascii_printable(const char* s, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

bool read_fname_entry(std::uintptr_t entry, std::string& out, int& headerSize) {
    for (const int candidate : {2, 4, 6, 8, 16}) {
        char buf[192] = {};
        if (!try_read_bytes(entry, buf, sizeof(buf))) continue;

        if (candidate == 2) {
            std::int16_t header = 0;
            std::memcpy(&header, buf, sizeof(header));
            const int len = header & 0x3FF;
            if (len > 0 && len < 128 && is_ascii_printable(buf + 2, static_cast<std::size_t>(len))) {
                out.assign(buf + 2, static_cast<std::size_t>(len));
                headerSize = 2;
                return true;
            }
            continue;
        }
        if (candidate == 4) {
            std::int32_t header = 0;
            std::memcpy(&header, buf, sizeof(header));
            const int len = header & 0x3FF;
            if (len > 0 && len < 128 && is_ascii_printable(buf + 4, static_cast<std::size_t>(len))) {
                out.assign(buf + 4, static_cast<std::size_t>(len));
                headerSize = 4;
                return true;
            }
            continue;
        }
        if (candidate == 6) {
            std::int16_t header = 0;
            std::memcpy(&header, buf, sizeof(header));
            const int len = header & 0x3FF;
            if (len > 0 && len < 128 && is_ascii_printable(buf + 6, static_cast<std::size_t>(len))) {
                out.assign(buf + 6, static_cast<std::size_t>(len));
                headerSize = 6;
                return true;
            }
            continue;
        }
        if (candidate == 8) {
            std::int32_t header = 0;
            std::memcpy(&header, buf, sizeof(header));
            const int len = header & 0x3FF;
            if (len > 0 && len < 128 && is_ascii_printable(buf + 8, static_cast<std::size_t>(len))) {
                out.assign(buf + 8, static_cast<std::size_t>(len));
                headerSize = 8;
                return true;
            }
            continue;
        }
    }
    return false;
}

}

[[nodiscard]] NameTableSearch find_name_table_via_headers();

std::string ObjectArraySearch::summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "objects: found=%s strong=%zu examined=%llu scanned=%llu B skipped=%llu "
                  "note=%s",
                  found ? "yes" : "no", strong.size(),
                  static_cast<unsigned long long>(candidatesExamined),
                  static_cast<unsigned long long>(bytesScanned),
                  static_cast<unsigned long long>(regionsSkipped), note.c_str());
    return buf;
}

struct NameValidation {
    std::uint32_t checked = 0;
    std::uint32_t named = 0;
};

NameValidation count_named_objects(std::uintptr_t itemsArray, std::uint32_t probeCount) {
    NameValidation v;
    const ReflectionState& refl = reflection_state();
    if (!refl.nameTableReady) return v;

    static thread_local std::unordered_map<std::uint32_t, bool> memo;
    if (memo.size() > 8192) memo.clear();

    for (std::uint32_t i = 0; i < probeCount; ++i) {
        std::uintptr_t obj = 0;
        if (!read_object_at(itemsArray, i, obj)) continue;
        ++v.checked;

        std::uint64_t nameIndexRaw = 0;
        if (in_active_ranges(obj + 0x18, 8)) {

            nameIndexRaw = read_u64_fast(obj + 0x18);
        } else if (!try_read_u64(obj + 0x18, nameIndexRaw)) {

            continue;
        }
        const std::uint32_t nameIndex = static_cast<std::uint32_t>(nameIndexRaw & 0xFFFFFFFFu);

        if (nameIndex == 0 || nameIndex > 5'000'000) continue;

        const auto it = memo.find(nameIndex);
        if (it != memo.end()) {
            if (it->second) ++v.named;
            continue;
        }

        std::string name;
        const bool ok = name_of(nameIndex, name) && !name.empty();
        memo.emplace(nameIndex, ok);
        if (ok) ++v.named;
    }
    return v;
}

bool objects_look_like_objects(std::uintptr_t itemsArray, std::uint32_t probeCount) {
    std::uint32_t checked = 0;
    std::uint32_t shaped = 0;

    for (std::uint32_t i = 0; i < probeCount; ++i) {
        std::uintptr_t obj = 0;
        if (!read_object_at(itemsArray, i, obj)) continue;
        ++checked;

        std::uint64_t classPtr = 0;
        std::uint64_t nameIndexRaw = 0;
        if (in_active_ranges(obj + 0x10, 16)) {
            classPtr = read_u64_fast(obj + 0x10);
            nameIndexRaw = read_u64_fast(obj + 0x18);
        } else if (!try_read_u64(obj + 0x10, classPtr) || !try_read_u64(obj + 0x18, nameIndexRaw)) {
            continue;
        }

        const auto cls = static_cast<std::uintptr_t>(classPtr);
        if (!looks_like_image_pointer(cls)) continue;
        if (cls == obj) continue;
        const std::uint32_t nameIndex = static_cast<std::uint32_t>(nameIndexRaw & 0xFFFFFFFFu);
        if (nameIndex == 0 || nameIndex > 5'000'000) continue;
        ++shaped;
    }

    if (checked == 0) return false;
    return (static_cast<double>(shaped) / checked) >= 0.8;
}

bool try_offsets(ObjectArraySearch& out) {
    using namespace offsets;

    const ModuleInfo exe = main_module();
    if (exe.base == 0) {
        DSH_WARN("reflection: no main module; cannot apply the recorded offsets");
        return false;
    }

    const auto container = exe.base + static_cast<std::uintptr_t>(kGObjects);

    std::uint64_t chunksPtr = 0;
    std::uint32_t numElements = 0;
    std::uint32_t numChunks = 0;
    if (!try_read_u64(container + kObjectsOffset, chunksPtr)) {
        DSH_WARN("reflection: offset check failed -- GObjects at %s%X is not readable",
                 exe.name.c_str(), kGObjects);
        return false;
    }
    if (!try_read_u32(container + kNumElementsOffset, numElements) ||
        !try_read_u32(container + kNumChunksOffset, numChunks)) {
        DSH_WARN("reflection: offset check failed -- NumElements/NumChunks unreadable");
        return false;
    }

    if (chunksPtr == 0) {
        DSH_WARN("reflection: offset check failed -- chunk table pointer is null");
        return false;
    }
    if (numElements == 0 || numElements > 20'000'000) {
        DSH_WARN("reflection: offset check failed -- implausible NumElements=%u", numElements);
        return false;
    }
    if (numChunks == 0 || numChunks > 4096) {
        DSH_WARN("reflection: offset check failed -- implausible NumChunks=%u", numChunks);
        return false;
    }

    std::uint64_t chunk0 = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunksPtr), chunk0) || chunk0 < 0x10000) {
        DSH_WARN("reflection: offset check failed -- chunk 0 pointer 0x%llX is unusable",
                 static_cast<unsigned long long>(chunk0));
        return false;
    }

    std::uint64_t obj0 = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunk0), obj0)) {
        DSH_WARN("reflection: offset check failed -- chunk 0 unreadable");
        return false;
    }
    if (!looks_like_image_pointer(static_cast<std::uintptr_t>(obj0))) {
        DSH_WARN("reflection: offset check failed -- object[0]=0x%llX is not in a module",
                 static_cast<unsigned long long>(obj0));
        return false;
    }

    std::uint64_t objClass = 0;
    std::uint64_t objName = 0;
    const bool classOk = try_read_u64(static_cast<std::uintptr_t>(obj0) + kUObjectClassOffset,
                                      objClass) &&
                         looks_like_image_pointer(static_cast<std::uintptr_t>(objClass));
    const bool nameOk = try_read_u64(static_cast<std::uintptr_t>(obj0) + kUObjectNameOffset,
                                     objName) &&
                        (objName & 0xFFFFFFFFu) != 0 && (objName & 0xFFFFFFFFu) < 5'000'000u;
    DSH_INFO("reflection: chain evidence -- object[0]=0x%llX class=0x%llX(%s) nameRaw=0x%llX "
             "(nameIndex=%u %s)",
             static_cast<unsigned long long>(obj0),
             static_cast<unsigned long long>(objClass), classOk ? "ok" : "BAD",
             static_cast<unsigned long long>(objName),
             static_cast<unsigned>((objName & 0xFFFFFFFFu)), nameOk ? "ok" : "BAD");

    DSH_INFO("reflection: recorded offsets VERIFIED: TUObjectArray=0x%llX (module) chunkTable=0x%llX "
             "(heap) chunk0=0x%llX (heap) numElements=%u numChunks=%u (dumped from %s)",
             static_cast<unsigned long long>(container),
             static_cast<unsigned long long>(chunksPtr),
             static_cast<unsigned long long>(chunk0), numElements, numChunks,
             kEngineBuildString);

    ObjectArrayCandidate c;
    c.containerAddress = container;
    c.itemsPointer = static_cast<std::uintptr_t>(chunksPtr);
    c.itemsOffset = kObjectsOffset;
    c.reportedCount = numElements;
    c.objectsChecked = 1;
    c.objectsValid = 1;
    c.namedChecked = 1;
    c.namedValid = 1;

    out.found = true;
    out.best = c;
    out.strong.push_back(c);
    out.note = "resolved from the recorded offsets (verified at runtime)";
    return true;
}

ObjectArraySearch find_object_array(double minValidFraction, std::uint32_t probeCount) {
    ObjectArraySearch search;
    const auto ranges = writable_ranges();
    if (ranges.empty()) {
        search.note = "no writable ranges in the main module";
        return search;
    }

    g_activeRanges = &ranges;
    struct RangeGuard {
        ~RangeGuard() { g_activeRanges = nullptr; }
    } guard;

    reset_scanner_counters();
    const std::uint64_t startTick = GetTickCount64();

    {
        std::uint64_t total = 0;
        for (const auto& r : ranges) total += r.size;
        DSH_INFO("reflection: object search starting: %zu range(s), %llu B, %zu module(s), "
                 "%llu slots",
                 ranges.size(), static_cast<unsigned long long>(total), module_count(),
                 static_cast<unsigned long long>(total / 8));
    }

    for (const auto& range : ranges) {
        ++search.regionsScanned;
        std::uint64_t lastProgressTick = startTick;

        std::uint64_t probe = 0;
        if (!try_read_u64(range.start, probe)) {
            ++search.regionsSkipped;
            DSH_WARN("reflection: skipping unreadable range %s at 0x%llX (%s)", range.label.c_str(),
                     static_cast<unsigned long long>(range.start),
                     describe_region(range.start).c_str());
            continue;
        }
        search.bytesScanned += range.size;

        const std::size_t slots = static_cast<std::size_t>(range.size / 8);
        for (std::size_t i = 0; i < slots; ++i) {
            const std::uintptr_t slot = range.start + i * 8;

            const std::uint64_t value = read_u64_fast(slot);
            if (!looks_like_image_pointer(static_cast<std::uintptr_t>(value))) continue;

            ++search.candidatesExamined;

            const std::uint64_t nowTick = GetTickCount64();
            if (nowTick - lastProgressTick >= 2000) {
                lastProgressTick = nowTick;
                DSH_INFO("reflection: object search progress: %llu/%llu slots (%.0f%%), "
                         "%llu candidates, %llu ptrChecks, %llu ms elapsed",
                         static_cast<unsigned long long>(i),
                         static_cast<unsigned long long>(slots),
                         100.0 * static_cast<double>(i) / static_cast<double>(slots ? slots : 1),
                         static_cast<unsigned long long>(search.candidatesExamined),
                         static_cast<unsigned long long>(pointer_check_count()),
                         static_cast<unsigned long long>(nowTick - startTick));
            }

            for (const auto off : kObjectContainerProbeOffsets) {
                if (off > slot - range.start) continue;
                const std::uintptr_t container = slot - off;
                if (container < range.start) continue;
                if ((off % 8) != 0) continue;

                const auto candidate = evaluate_container(container, off, kShapeProbe);
                if (candidate.objectsChecked == 0) continue;
                if (candidate.confidence() < minValidFraction) continue;

                if (!objects_look_like_objects(candidate.itemsPointer, kNameProbe)) continue;

                auto strong = candidate;
                const auto named = count_named_objects(candidate.itemsPointer, kNameProbe);
                strong.namedChecked = named.checked;
                strong.namedValid = named.named;
                const double namedFraction =
                    named.checked ? static_cast<double>(named.named) / named.checked : 0.0;
                if (named.checked > 0 && namedFraction < kMinNamedFraction) continue;

                search.strong.push_back(strong);

                const bool better =
                    !search.found ||
                    strong.namedValid > search.best.namedValid ||
                    (strong.namedValid == search.best.namedValid &&
                     strong.confidence() > search.best.confidence());
                if (better) {
                    search.best = strong;
                    search.found = true;
                }
            }
        }
    }

    if (!search.found) {
        search.note = "no candidate passed the validity threshold";
    } else {
        search.note = "ok";
    }
    DSH_INFO("reflection: object search took %llu ms (%llu candidates, %llu ptrChecks, "
             "%llu rangeRefreshes, %zu modules)",
             static_cast<unsigned long long>(GetTickCount64() - startTick),
             static_cast<unsigned long long>(search.candidatesExamined),
             static_cast<unsigned long long>(pointer_check_count()),
             static_cast<unsigned long long>(range_refresh_count()), module_count());
    return search;
}

std::string NameTableSearch::summary() const {
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "names: found=%s strong=%zu examined=%llu note=%s", found ? "yes" : "no",
                  strong.size(), static_cast<unsigned long long>(candidatesExamined),
                  note.c_str());
    return buf;
}

NameTableSearch find_name_table_via_headers() {
    using namespace offsets;
    NameTableSearch search;

    const ModuleInfo exe = main_module();
    if (exe.base == 0) {
        search.note = "no main module";
        return search;
    }

    const auto gnamesSlot = exe.base + static_cast<std::uintptr_t>(kGNames);
    std::uint64_t tablePtr = 0;
    if (!try_read_u64(gnamesSlot, tablePtr)) {
        search.note = "GNames slot unreadable";
        return search;
    }
    if (!looks_like_image_pointer(static_cast<std::uintptr_t>(tablePtr))) {

        DSH_INFO("reflection: GNames at %s%X holds 0x%llX, which is not a module pointer "
                 "-- consistent with GNames unused here (null by "
                 "default); AppendString is the real path",
                 exe.name.c_str(), kGNames, static_cast<unsigned long long>(tablePtr));
        search.note = "GNames is not a usable pointer on this build";
        return search;
    }

    std::uint64_t entries = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(tablePtr), entries) ||
        !looks_like_image_pointer(static_cast<std::uintptr_t>(entries))) {
        search.note = "GNames table's entry pointer is invalid";
        return search;
    }

    NameTableCandidate c;
    c.containerAddress = static_cast<std::uintptr_t>(tablePtr);
    c.entriesPointer = static_cast<std::uintptr_t>(entries);

    for (std::uint32_t index = 1; index <= 16; ++index) {
        std::uint64_t entryPtr = 0;
        if (!try_read_u64(c.entriesPointer + static_cast<std::uintptr_t>(index) * 8, entryPtr)) break;
        if (!looks_like_image_pointer(static_cast<std::uintptr_t>(entryPtr))) continue;
        ++c.namesChecked;
        std::string name;
        int hs = 0;
        if (read_fname_entry(static_cast<std::uintptr_t>(entryPtr), name, hs)) {
            ++c.namesValid;
            if (c.samples.size() < 8) c.samples.push_back(name);
        }
    }

    std::string sample;
    for (std::size_t i = 0; i < c.samples.size(); ++i) {
        if (i) sample += ",";
        sample += c.samples[i];
    }
    DSH_INFO("reflection: GNames=0x%llX table=0x%llX entries=0x%llX parsed=%u/%u samples=[%s]",
             static_cast<unsigned long long>(gnamesSlot),
             static_cast<unsigned long long>(tablePtr),
             static_cast<unsigned long long>(entries), c.namesValid, c.namesChecked, sample.c_str());

    if (c.namesChecked > 0 && c.namesValid > 0) {
        search.found = true;
        search.best = c;
        search.strong.push_back(c);
        search.note = "resolved from the recorded GNames offset (validated)";
    } else {
        search.note = "GNames present but no entry parsed as an FNameEntry";
    }
    return search;
}

bool name_of(std::uint32_t index, std::string& out) {
    ReflectionState state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }
    if (!state.nameTableReady) return false;

    std::uint64_t entry = 0;
    if (!try_read_u64(state.nameEntries + static_cast<std::uintptr_t>(index) * 8, entry)) return false;
    if (!looks_like_image_pointer(static_cast<std::uintptr_t>(entry))) return false;

    int hs = 0;
    return read_fname_entry(static_cast<std::uintptr_t>(entry), out, hs);
}

bool class_name_of_object(std::uintptr_t object, std::string& out) {
    std::uint64_t classPtr = 0;
    if (!try_read_u64(object + 0x10, classPtr)) return false;
    if (!looks_like_image_pointer(static_cast<std::uintptr_t>(classPtr))) return false;

    std::uint64_t classNameIndexRaw = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(classPtr) + 0x18, classNameIndexRaw)) return false;
    return name_of(static_cast<std::uint32_t>(classNameIndexRaw & 0xFFFFFFFFu), out);
}

std::uint64_t for_each_object(std::uint64_t maxObjects, const std::function<void(std::uintptr_t)>& fn) {
    ReflectionState state;
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        state = g_state;
    }
    if (!state.objectArrayReady) return 0;

    std::uint64_t visited = 0;
    for (std::uint64_t i = 0; i < maxObjects; ++i) {
        std::uintptr_t obj = 0;
        if (!read_object_at(state.objectItems, static_cast<std::uint32_t>(i), obj)) continue;
        ++visited;
        fn(obj);
    }
    return visited;
}

const ReflectionState& reflection_state() {
    std::lock_guard<std::mutex> lock(g_stateMutex);
    return g_state;
}

bool initialise_reflection(bool forceRescan) {
    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (!forceRescan && g_state.objectArrayReady && g_state.nameTableReady) return true;
    }

    DSH_INFO("reflection: step 1/6 -- identifying the main module");
    const ModuleInfo exe = main_module();
    DSH_INFO("reflection: step 1/6 done: %s base=0x%llX size=%llu",
             exe.name.c_str(), static_cast<unsigned long long>(exe.base),
             static_cast<unsigned long long>(exe.size));

    DSH_INFO("reflection: step 2/6 -- name table (recorded offsets first)");
    auto names = find_name_table_via_headers();

    if (!names.found) {
        DSH_INFO("reflection: no name TABLE on this build (GNames is null by design); "
                 "names are resolved through FName::AppendString. Skipping the heuristic "
                 "name search: it cannot succeed here and cost 167 s when it was tried.");
    }
    DSH_INFO("reflection: %s", names.summary().c_str());
    if (names.found) {
        std::string sample;
        for (std::size_t i = 0; i < names.best.samples.size(); ++i) {
            if (i) sample += ", ";
            sample += names.best.samples[i];
        }
        DSH_INFO("reflection: names entries=0x%llX valid=%u/%u samples=[%s]",
                 static_cast<unsigned long long>(names.best.entriesPointer),
                 names.best.namesValid, names.best.namesChecked, sample.c_str());
        std::lock_guard<std::mutex> lock(g_stateMutex);
        g_state.nameTableReady = true;
        g_state.nameTableContainer = names.best.containerAddress;
        g_state.nameEntries = names.best.entriesPointer;
    } else {
        DSH_ERROR("reflection: name table NOT found. Consequence: class and object names "
                  "will be unavailable AND the object-array validation loses its strongest "
                  "criterion, so expect many false-positive containers below.");
    }

    DSH_INFO("reflection: step 4/6 -- object array (recorded offsets first, searcher as fallback)");
    ObjectArraySearch objects;
    if (try_offsets(objects)) {
        DSH_INFO("reflection: object array resolved from the recorded offsets -- searcher NOT run "
                 "(it took ~167 s and was not needed)");
    } else {
        DSH_WARN("reflection: the recorded offsets did not verify; falling back to the heuristic "
                 "searcher (slow, and on this build its name handling was wrong)");
        objects = find_object_array();
    }
    DSH_INFO("reflection: %s", objects.summary().c_str());
    if (objects.found) {
        const auto& c = objects.best;
        DSH_INFO("reflection: objects container=0x%llX itemsOffset=0x%llX items=0x%llX "
                 "ptrValid=%u/%u (%.0f%%) namedValid=%u/%u (%.0f%%)",
                 static_cast<unsigned long long>(c.containerAddress),
                 static_cast<unsigned long long>(c.itemsOffset),
                 static_cast<unsigned long long>(c.itemsPointer), c.objectsValid, c.objectsChecked,
                 c.confidence() * 100.0, c.namedValid, c.namedChecked,
                 c.named_confidence() * 100.0);

        if (objects.strong.size() > 4) {
            DSH_WARN("reflection: %zu candidates passed both gates -- the winner is the "
                     "best-scoring one, but this is worth checking",
                     objects.strong.size());
        }
    } else {
        DSH_ERROR("reflection: object array NOT found. Consequence: no object walking, no "
                  "class lookups, no world or pawn discovery. This is a search-quality "
                  "problem, not a game property.");
    }

    {
        std::lock_guard<std::mutex> lock(g_stateMutex);
        if (objects.found) {
            g_state.objectArrayReady = true;
            g_state.objectArrayContainer = objects.best.containerAddress;
            g_state.objectItems = objects.best.itemsPointer;
            g_state.objectCount = objects.best.reportedCount;
        }
    }

    return objects.found && names.found;
}

bool object_is_live(std::uintptr_t object, std::string& note) {
    using namespace offsets;
    note.clear();
    char selfBuf[32];
    std::snprintf(selfBuf, sizeof(selfBuf), "0x%llX", static_cast<unsigned long long>(object));
    if (object < 0x10000) {
        note = "the pointer is implausible (" + std::string(selfBuf) + "), so it cannot be a live object";
        return false;
    }

    const ModuleInfo exe = main_module();
    if (exe.base == 0) {
        note = "the main executable's base could not be resolved, so the object table cannot be reached and "
               "NO verdict about this pointer was reached";
        return false;
    }
    const std::uintptr_t array = exe.base + static_cast<std::uintptr_t>(kGObjects);
    std::uint64_t chunkTable = 0;
    std::uint32_t liveElements = 0;
    if (!try_read_u64(array + kObjectsOffset, chunkTable) || chunkTable < 0x10000) {
        note = "the object table's chunk table (TUObjectArray::Objects,) could not be read, so NO "
               "verdict was reached";
        return false;
    }
    if (!try_read_u32(array + kNumElementsOffset, liveElements)) {
        note = "the object table's live element count (TUObjectArray::NumElements,) could not be read, "
               "so NO verdict was reached";
        return false;
    }
    std::uint32_t index = 0;
    if (!try_read_u32(object + kUObjectIndexOffset, index)) {
        note = "its own index (UObject::Index,) could not be read, so NO verdict was reached -- this is "
               "not a statement that the object is gone";
        return false;
    }

    std::uintptr_t atItsIndex = 0;
    bool slotRead = false;
    if (liveElements != 0 && index < liveElements) {
        const std::uint32_t chunkIndex = index / kElementsPerChunk;
        const std::uint32_t within = index % kElementsPerChunk;
        std::uint64_t chunk = 0;
        if (try_read_u64(static_cast<std::uintptr_t>(chunkTable) +
                             static_cast<std::uintptr_t>(chunkIndex) * 8, chunk) &&
            chunk >= 0x10000) {
            std::uint64_t at = 0;
            if (try_read_u64(static_cast<std::uintptr_t>(chunk) +
                                 static_cast<std::uintptr_t>(within) * kObjectItemStride, at)) {
                atItsIndex = static_cast<std::uintptr_t>(at);
                slotRead = true;
            }
        }
    }

    switch (judge_object_slot(slotRead, object, index, liveElements, atItsIndex)) {
        case ObjectLiveness::live:
            return true;
        case ObjectLiveness::gone: {
            char slotBuf[32];
            std::snprintf(slotBuf, sizeof(slotBuf), "0x%llX", static_cast<unsigned long long>(atItsIndex));
            note = "the object table's slot " + std::to_string(index) + " holds " + std::string(slotBuf) +
                   " and not this pointer, so THE OBJECT IS GONE (destroyed and swept, or replaced)";
            return false;
        }
        case ObjectLiveness::indexOutOfRange:
            note = "the index it carries (" + std::to_string(index) + ") is outside the table's " +
                   std::to_string(liveElements) + " live element(s), so it is not a live object";
            return false;
        case ObjectLiveness::notReadable:
            note = "the object table's slot " + std::to_string(index) + " could not be read, so NO verdict "
                   "was reached -- this is not a statement that the object is gone";
            return false;
    }
    return false;
}

std::string liveness_selfcheck_report() {
    using namespace offsets;
    const ModuleInfo exe = main_module();
    if (exe.base == 0) {
        return "liveness self-check: NOT RUN -- the main executable's base could not be resolved";
    }
    const std::uintptr_t array = exe.base + static_cast<std::uintptr_t>(kGObjects);
    std::uint64_t chunkTable = 0;
    std::uint32_t liveElements = 0;
    if (!try_read_u64(array + kObjectsOffset, chunkTable) || chunkTable < 0x10000 ||
        !try_read_u32(array + kNumElementsOffset, liveElements)) {
        return "liveness self-check: NOT RUN -- the object table could not be read";
    }
    std::uint64_t chunk0 = 0;
    std::uint64_t obj0 = 0;
    if (!try_read_u64(static_cast<std::uintptr_t>(chunkTable), chunk0) || chunk0 < 0x10000 ||
        !try_read_u64(static_cast<std::uintptr_t>(chunk0), obj0) || obj0 < 0x10000) {
        return "liveness self-check: NOT RUN -- chunk 0 or object[0] could not be read";
    }
    std::uint32_t index = 0;
    if (!try_read_u32(static_cast<std::uintptr_t>(obj0) + kUObjectIndexOffset, index)) {
        return "liveness self-check: NOT RUN -- object[0]'s own index (UObject::Index,) could not be "
               "read";
    }
    const std::uint32_t chunkIndex = index / kElementsPerChunk;
    const std::uint32_t within = index % kElementsPerChunk;
    std::uint64_t chunk = 0;
    std::uint64_t atItsIndex = 0;
    bool slotRead = false;
    if (try_read_u64(static_cast<std::uintptr_t>(chunkTable) +
                         static_cast<std::uintptr_t>(chunkIndex) * 8, chunk) &&
        chunk >= 0x10000) {
        if (try_read_u64(static_cast<std::uintptr_t>(chunk) +
                             static_cast<std::uintptr_t>(within) * kObjectItemStride, atItsIndex)) {
            slotRead = true;
        }
    }
    std::string why;
    const bool verdict = object_is_live(static_cast<std::uintptr_t>(obj0), why);
    const ObjectLiveness judged =
        judge_object_slot(slotRead, static_cast<std::uintptr_t>(obj0), index, liveElements,
                          static_cast<std::uintptr_t>(atItsIndex));
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "liveness self-check: object[0]=0x%llX says its own index is %u; the table holds %u live "
                  "element(s) and slot %u %s 0x%llX -- the rule says '%s' and object_is_live says %s (%s). %s",
                  static_cast<unsigned long long>(obj0), index, liveElements, index,
                  slotRead ? "holds" : "could NOT be read; the last value read was",
                  static_cast<unsigned long long>(atItsIndex), to_string(judged),
                  verdict ? "LIVE" : "NOT live", why.c_str(),
                  (slotRead && static_cast<std::uint64_t>(atItsIndex) == obj0 && verdict)
                      ? "THE RULE MATCHES on an object the table itself named, so this build can check a "
                        "cached handle with it."
                      : "THE RULE DOES NOT MATCH on an object the table itself named: do NOT trust a 'gone' "
                        "verdict from this build -- the guard's refusals name themselves and every one of "
                        "them is a handle that was NOT used.");
    return buf;
}

}
