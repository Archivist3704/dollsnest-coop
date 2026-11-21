#include "dsh/reflect/render_scan.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace dsh::reflect {
namespace {

constexpr std::uintptr_t kScanFirst = 0x01F0;
constexpr std::uintptr_t kScanLast = 0x0480;
constexpr std::size_t    kWindow = 0x0600;
constexpr std::size_t    kWindowFloor = 0x0100;

constexpr float          kMatrixToleranceCm = 2.0f;

bool probably_a_heap_pointer(std::uint64_t value) {

    return value >= 0x10000ull && value < 0x00007FFFFFFF0000ull && (value & 0x7ull) == 0;
}

bool read_u64(PrimitiveReadFn read, std::uintptr_t address, std::uint64_t& out) {
    if (read == nullptr) return false;
    return read(address, &out, sizeof(out));
}

float length3(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

bool affine_matrix_at(const unsigned char* window, std::size_t offset, const float* expectedCm,
                      float translationOut[3], float axisXOut[3]) {
    float m[16] = {};
    std::memcpy(m, window + offset, sizeof(m));
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(m[i])) return false;
    }
    if (std::fabs(m[3]) > 1e-3f || std::fabs(m[7]) > 1e-3f || std::fabs(m[11]) > 1e-3f) return false;
    if (std::fabs(m[15] - 1.0f) > 1e-3f) return false;
    const float x[3] = {m[0], m[1], m[2]};
    const float y[3] = {m[4], m[5], m[6]};
    const float z[3] = {m[8], m[9], m[10]};
    if (std::fabs(length3(x) - 1.0f) > 0.05f) return false;
    if (std::fabs(length3(y) - 1.0f) > 0.05f) return false;
    if (std::fabs(length3(z) - 1.0f) > 0.05f) return false;
    if (expectedCm != nullptr &&
        (std::fabs(m[12] - expectedCm[0]) > kMatrixToleranceCm ||
         std::fabs(m[13] - expectedCm[1]) > kMatrixToleranceCm ||
         std::fabs(m[14] - expectedCm[2]) > kMatrixToleranceCm)) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        translationOut[i] = m[12 + i];
        axisXOut[i] = x[i];
    }
    return true;
}

struct ProxyCandidate {
    std::uintptr_t slot = 0;
    std::uintptr_t matrixMatchOffset = 0;
    std::uintptr_t matrixAnyOffset = 0;
    std::uintptr_t sceneInfo = 0;
    std::uintptr_t sceneInfoSlot = 0;
    std::uintptr_t sceneInfoInner = 0;
    float          matchTranslation[3] = {0.0f, 0.0f, 0.0f};
    float          matchAxisX[3] = {0.0f, 0.0f, 0.0f};
    float          anyTranslation[3] = {0.0f, 0.0f, 0.0f};
    float          anyAxisX[3] = {0.0f, 0.0f, 0.0f};
    bool           readable = false;
};

ProxyCandidate examine_candidate(std::uintptr_t slot, std::uintptr_t candidate,
                                 const float expectedLocationCm[3], PrimitiveReadFn read,
                                 std::vector<unsigned char>& window) {
    ProxyCandidate out;
    out.slot = slot;
    window.resize(kWindow);
    std::size_t have = kWindow;
    while (have >= kWindowFloor && !(read != nullptr && read(candidate, window.data(), have))) {
        have /= 2;
    }
    if (have < kWindowFloor) return out;
    out.readable = true;

    constexpr std::size_t kSceneInfoWords = 8;
    for (std::size_t off = 0; off + sizeof(std::uint64_t) <= have; off += sizeof(std::uint64_t)) {
        std::uint64_t q = 0;
        std::memcpy(&q, window.data() + off, sizeof(q));
        if (!probably_a_heap_pointer(q)) continue;
        std::uint64_t inner[kSceneInfoWords] = {};
        if (!read(static_cast<std::uintptr_t>(q), inner, sizeof(inner))) continue;
        for (std::size_t word = 0; word < kSceneInfoWords; ++word) {
            if (inner[word] == static_cast<std::uint64_t>(candidate)) {
                out.sceneInfo = static_cast<std::uintptr_t>(q);
                out.sceneInfoSlot = off;
                out.sceneInfoInner = word * sizeof(std::uint64_t);
                break;
            }
        }
        if (out.sceneInfo != 0) break;
    }

    for (std::size_t off = 0; off + 0x40 <= have; off += 4) {
        if (affine_matrix_at(window.data(), off, expectedLocationCm, out.matchTranslation, out.matchAxisX)) {
            out.matrixMatchOffset = off;
            break;
        }
    }
    for (std::size_t off = 0; off + 0x40 <= have; off += 4) {
        if (affine_matrix_at(window.data(), off, nullptr, out.anyTranslation, out.anyAxisX)) {
            out.matrixAnyOffset = off;
            break;
        }
    }
    return out;
}

}

void read_proxy_facts(std::uintptr_t proxy, std::uintptr_t matrixOffset, PrimitiveReadFn read,
                      RenderPrimitive& out) {
    if (read == nullptr || proxy == 0 || matrixOffset < 0x60) return;
    bool any = false;
    unsigned char mobility = 0;
    if (read(proxy + matrixOffset - 0x60, &mobility, sizeof(mobility))) {
        out.proxyMobility = static_cast<int>(mobility);
        any = true;
    }
    unsigned char staticByte = 0;
    if (read(proxy + matrixOffset - 0x4E, &staticByte, sizeof(staticByte))) {
        out.proxyStaticLighting = (staticByte & 0x04) != 0;
        any = true;
    }
    unsigned char vfByte = 0;
    if (read(proxy + matrixOffset - 0x4A, &vfByte, sizeof(vfByte))) {
        out.proxyVfNeedsPrimitiveUniformBuffer = (vfByte & 0x08) != 0;
        any = true;
    }
    std::uint64_t scene = 0;
    if (read_u64(read, proxy + matrixOffset + 0x90, scene)) {
        out.proxyScene = static_cast<std::uintptr_t>(scene);
        any = true;
    }
    std::uint64_t info = 0;
    if (read_u64(read, proxy + matrixOffset + 0x98, info)) {
        out.proxySceneInfo = static_cast<std::uintptr_t>(info);

        if (out.proxySceneInfo >= 0x10000) {
            std::uint64_t back = 0;
            if (read_u64(read, out.proxySceneInfo + 0x08, back)) {
                out.proxySceneInfoPointsBack = back == static_cast<std::uint64_t>(proxy);
            }
        }
        any = true;
    }
    std::uint64_t owner = 0;
    if (read_u64(read, proxy + matrixOffset + 0xD0, owner)) {
        out.proxyComponent = static_cast<std::uintptr_t>(owner);
        any = true;
    }
    out.factsRead = any;
}

bool scan_render_primitive(std::uintptr_t component, const float expectedLocationCm[3], PrimitiveReadFn read,
                           RenderPrimitive& out) {
    out = RenderPrimitive{};
    if (component < 0x10000) {
        out.note = "no component was given";
        return false;
    }
    if (read == nullptr || expectedLocationCm == nullptr) {
        out.note = "no reader or no expected location was given";
        return false;
    }
    std::vector<unsigned char> window;
    std::vector<ProxyCandidate> candidates;
    for (std::uintptr_t slot = kScanFirst; slot + sizeof(std::uint64_t) <= kScanLast;
         slot += sizeof(std::uint64_t)) {
        std::uint64_t raw = 0;
        if (!read_u64(read, component + slot, raw)) continue;
        if (!probably_a_heap_pointer(raw)) continue;
        ProxyCandidate examined =
            examine_candidate(slot, static_cast<std::uintptr_t>(raw), expectedLocationCm, read, window);
        if (!examined.readable) continue;
        if (examined.sceneInfo == 0 && examined.matrixAnyOffset == 0) continue;
        candidates.push_back(examined);
    }
    out.candidates = static_cast<int>(candidates.size());
    if (candidates.empty()) {
        out.note = "no pointer in the component looked like a proxy (neither a scene info nor a transform)";
        return false;
    }

    const ProxyCandidate* chosen = nullptr;
    for (const auto& c : candidates) {
        if (c.sceneInfo != 0 && c.matrixMatchOffset != 0) {
            chosen = &c;
            break;
        }
    }
    if (chosen == nullptr) {
        for (const auto& c : candidates) {
            if (c.sceneInfo != 0 && c.matrixAnyOffset != 0) {
                chosen = &c;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        for (const auto& c : candidates) {
            if (c.matrixMatchOffset != 0) {
                chosen = &c;
                break;
            }
        }
    }
    if (chosen == nullptr) {
        out.note = "a proxy was suspected " + std::to_string(candidates.size()) +
                   " time(s) but none held a usable transform";
        return false;
    }
    out.located = true;
    out.proxySlot = chosen->slot;

    std::uint64_t proxyPointer = 0;
    if (read_u64(read, component + chosen->slot, proxyPointer)) {
        out.proxy = static_cast<std::uintptr_t>(proxyPointer);
    }
    out.matrixMatchesComponent = chosen->matrixMatchOffset != 0;
    out.matrixOffset = chosen->matrixMatchOffset != 0 ? chosen->matrixMatchOffset : chosen->matrixAnyOffset;
    out.sceneInfo = chosen->sceneInfo;
    out.sceneInfoSlot = chosen->sceneInfoSlot;
    out.sceneInfoInner = chosen->sceneInfoInner;
    const float* translation = chosen->matrixMatchOffset != 0 ? chosen->matchTranslation : chosen->anyTranslation;
    const float* axisX = chosen->matrixMatchOffset != 0 ? chosen->matchAxisX : chosen->anyAxisX;
    for (int i = 0; i < 3; ++i) {
        out.translationCm[i] = translation[i];
        out.axisX[i] = axisX[i];
    }
    read_proxy_facts(out.proxy, out.matrixOffset, read, out);
    char words[224] = {};
    std::snprintf(words, sizeof(words),
                  "proxy at component%llX, transform at proxy%llX, scene info %s at proxy%llX (back-pointer "
                  "at sceneInfo%llX), %d candidate pointer(s)",
                  static_cast<unsigned long long>(out.proxySlot),
                  static_cast<unsigned long long>(out.matrixOffset),
                  out.sceneInfo != 0 ? "FOUND" : "NULL",
                  static_cast<unsigned long long>(out.sceneInfoSlot),
                  static_cast<unsigned long long>(out.sceneInfoInner), out.candidates);
    out.note = words;
    return true;
}

bool read_render_primitive_at(std::uintptr_t component, std::uintptr_t proxySlot, std::uintptr_t matrixOffset,
                              std::uintptr_t sceneInfoSlot, PrimitiveReadFn read, RenderPrimitive& out) {
    out = RenderPrimitive{};
    out.proxySlot = proxySlot;
    out.matrixOffset = matrixOffset;
    out.sceneInfoSlot = sceneInfoSlot;
    if (component < 0x10000 || proxySlot == 0 || matrixOffset == 0 || read == nullptr) {
        out.note = "this component was never located";
        return false;
    }
    std::uint64_t proxy = 0;
    if (!read_u64(read, component + proxySlot, proxy) || !probably_a_heap_pointer(proxy)) {
        out.note = "the proxy pointer is null or unreadable now (the render state was destroyed)";
        return false;
    }
    out.proxy = static_cast<std::uintptr_t>(proxy);
    float m[16] = {};
    if (!read(out.proxy + matrixOffset, m, sizeof(m))) {
        out.note = "the proxy is gone (its memory is no longer readable)";
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        out.translationCm[i] = m[12 + i];
        out.axisX[i] = m[i];
    }
    out.read = true;
    read_proxy_facts(out.proxy, matrixOffset, read, out);
    if (sceneInfoSlot != 0) {
        std::uint64_t info = 0;
        if (read_u64(read, out.proxy + sceneInfoSlot, info)) {
            out.sceneInfo = static_cast<std::uintptr_t>(info);
        }
    }
    return true;
}

}
