#pragma once

#include <cstdint>
#include <string>

namespace dsh::reflect {

using PrimitiveReadFn = bool (*)(std::uintptr_t address, void* dst, std::size_t bytes);

struct RenderPrimitive {
    bool           located = false;
    bool           read = false;
    bool           matrixMatchesComponent = false;
    std::uintptr_t proxy = 0;
    std::uintptr_t proxySlot = 0;
    std::uintptr_t matrixOffset = 0;
    std::uintptr_t sceneInfo = 0;
    std::uintptr_t sceneInfoSlot = 0;
    std::uintptr_t sceneInfoInner = 0;

    float          translationCm[3] = {0.0f, 0.0f, 0.0f};
    float          axisX[3] = {0.0f, 0.0f, 0.0f};

    bool           factsRead = false;
    int            proxyMobility = -1;
    bool           proxyStaticLighting = false;
    bool           proxyVfNeedsPrimitiveUniformBuffer = false;
    std::uintptr_t proxyScene = 0;
    std::uintptr_t proxySceneInfo = 0;
    bool           proxySceneInfoPointsBack = false;
    std::uintptr_t proxyComponent = 0;
    int            candidates = 0;
    std::string    note;
};

[[nodiscard]] bool scan_render_primitive(std::uintptr_t component, const float expectedLocationCm[3],
                                         PrimitiveReadFn read, RenderPrimitive& out);

[[nodiscard]] bool read_render_primitive_at(std::uintptr_t component, std::uintptr_t proxySlot,
                                            std::uintptr_t matrixOffset, std::uintptr_t sceneInfoSlot,
                                            PrimitiveReadFn read, RenderPrimitive& out);

}
