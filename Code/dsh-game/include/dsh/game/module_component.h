#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dsh::game {

inline constexpr std::uintptr_t kMachineThrusterOffset            = 0x15F8;
inline constexpr std::uintptr_t kThrusterModulePurgedOffset       = 0x0228;
inline constexpr std::uintptr_t kThrusterModuleLevelOffset        = 0x022C;
inline constexpr std::uintptr_t kThrusterModuleStatusOffset       = 0x0230;
inline constexpr std::uintptr_t kThrusterModuleMeshOffset         = 0x02D0;
inline constexpr std::uintptr_t kActorComponentAutoActivateOffset = 0x0089;
inline constexpr std::uintptr_t kActorComponentIsActiveOffset     = 0x008A;
inline constexpr std::uintptr_t kNiagaraComponentAssetOffset      = 0x0450;
inline constexpr std::uintptr_t kNiagaraComponentFlagsOffset      = 0x0558;

inline constexpr std::size_t kThrusterModuleStatusBytes = 0x0088;

inline constexpr const char* kThrusterModuleApiClass = "DAThrusterModule";

inline constexpr const char* kNiagaraApiClass        = "NiagaraComponent";

using ModuleComponentReader = bool (*)(std::uintptr_t address, void* dst, std::size_t bytes);

using ModuleNameResolver = bool (*)(std::uintptr_t nameAddress, std::string& out);

using ModuleObjectIterator = void (*)(std::uint32_t maxObjects,
                                      void (*onObject)(void* user, std::uintptr_t object,
                                                       std::uintptr_t outer),
                                      void* user, std::uint32_t& visited);

using ModuleClassChainCheck = bool (*)(std::uintptr_t object, std::uintptr_t wantedClass,
                                       ModuleComponentReader read);

struct ModuleOwnedObject {
    std::uintptr_t address = 0;
    std::uintptr_t classAddress = 0;
    std::string    className;
    std::string    objectName;

    bool           niagara = false;
    bool           assetRead = false;
    std::uintptr_t asset = 0;

    bool           activationRead = false;
    bool           autoActivate = false;
    bool           isActive = false;

    std::string    activationRefusal;

    bool           niagaraFlagsRead = false;
    bool           autoDestroy = false;
    bool           renderingEnabled = false;
};

struct ModuleComponentReport {
    std::string                          label;
    std::uintptr_t                       machine = 0;
    std::string                          machineClass;
    std::uintptr_t                       module = 0;
    std::string                          moduleClass;
    bool                                 moduleRead = false;
    bool                                 moduleUsable = false;
    std::string                          note;
    std::vector<ModuleOwnedObject>       components;

    int                                  unnameable = 0;

    int                                  unnamed = 0;

    bool                                 listFresh = true;

    std::uintptr_t                       niagaraClass = 0;

    bool                                 niagaraChecked = false;

    std::uint64_t                        walkBytesRead = 0;
    std::uint64_t                        refreshBytesRead = 0;

    bool                                 purgedRead = false;
    bool                                 purged = false;
    bool                                 levelRead = false;
    std::int32_t                         level = 0;
    bool                                 statusRead = false;
    std::uint8_t                         status[kThrusterModuleStatusBytes] = {};
    bool                                 meshRead = false;
    std::uintptr_t                       mesh = 0;
    std::string                          meshClass;

    [[nodiscard]] int niagaraCount() const noexcept {
        int n = 0;
        for (const auto& c : components) {
            if (c.niagara) ++n;
        }
        return n;
    }

    [[nodiscard]] std::vector<std::string> lines() const;
};

void read_component_state(ModuleOwnedObject& out, std::uintptr_t niagaraClass, ModuleComponentReader read,
                          ModuleClassChainCheck chainReaches, std::uint64_t* bytesRead = nullptr);

[[nodiscard]] std::vector<std::string> class_field_lines(std::uintptr_t classAddress,
                                                         ModuleComponentReader read,
                                                         ModuleNameResolver namer,
                                                         std::uint64_t* bytesRead = nullptr);
[[nodiscard]] bool module_component_dump(std::uintptr_t machine, const std::string& label,
                                         std::uintptr_t thrusterClass, std::uintptr_t niagaraClass,
                                         std::uint32_t maxObjects, ModuleObjectIterator iterate,
                                         ModuleComponentReader read, ModuleNameResolver namer,
                                         ModuleClassChainCheck chainReaches, ModuleComponentReport& out);

void refresh_module_component_state(ModuleComponentReport& out, std::uintptr_t thrusterClass,
                                    ModuleComponentReader read, ModuleClassChainCheck chainReaches);

}
