#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "dsh/reflect/render_scan.h"

namespace dsh::reflect {

[[nodiscard]] std::uintptr_t load_asset_by_path(const std::string& path);
[[nodiscard]] std::string object_path_name(std::uintptr_t obj);

std::string obj_diff_mark();
std::string obj_diff_report();

bool           puppet_apply_actor_pose(std::uintptr_t actor, const float posM[3], float yawRad,
                                       float pitchRad);

struct DollCustomize {
    float coloring[13][4] = {};
    float figure[5] = {};
};

inline constexpr int kModuleColorCount = 9;
inline constexpr int kModuleColorChannels = 4;
inline constexpr int kModuleColorComponents = 4;

inline constexpr std::size_t kModuleColorBytes =
    static_cast<std::size_t>(kModuleColorCount) * kModuleColorChannels * kModuleColorComponents *
    sizeof(float);
static_assert(kModuleColorBytes == 576, "FDAMachineColoringData is 0x0240 = 576 bytes");

inline constexpr int kMobilityColorIndex = 2;

[[nodiscard]] const char* module_color_name(int index);

struct MachineLoadout {
    std::string  className;
    std::string  classPath;
    std::uint8_t trigger[3] = {0, 0, 0};
    struct Slot {
        std::string className;
        std::string classPath;
        int         level = 0;
        bool        present = false;
    };

    Slot slots[9];

    bool         customizePresent = false;

    std::string  partsItemName[4];
    std::string  partsItemPath[4];
    DollCustomize customize;
    std::uint8_t inverseMesh[3] = {0, 0, 0};

    bool         moduleColoringPresent = false;
    float        moduleColoring[kModuleColorCount][kModuleColorChannels][kModuleColorComponents] = {};

    [[nodiscard]] int slot_count() const noexcept {
        int n = 0;
        for (const Slot& s : slots) {
            if (s.present) ++n;
        }
        return n;
    }
};

[[nodiscard]] bool capture_machine_loadout(std::uintptr_t machineActor, MachineLoadout& out,
                                           std::string& note);

[[nodiscard]] const char* machine_slot_name(int index);

struct MachineModuleState {
    struct Entry {
        std::uintptr_t offset = 0;
        std::string    name;
        std::uintptr_t pointer = 0;
        std::string    className;
    };
    Entry entries[10];
    int   nonNull = 0;
};

[[nodiscard]] MachineModuleState read_machine_modules(std::uintptr_t machineActor);

[[nodiscard]] bool spawn_machine_with_loadout(const MachineLoadout& loadout, std::uintptr_t& actorOut,
                                              std::string& report, std::string& note);

[[nodiscard]] bool actor_is_doll(std::uintptr_t actor);

[[nodiscard]] std::vector<std::string> class_property_values(std::uintptr_t classAddress,
                                                              std::uintptr_t instanceAddress,
                                                              std::uint32_t maxLines,
                                                              std::string& note);

[[nodiscard]] std::string ammo_params_text(std::uintptr_t actor);

struct AmmoSpawnRequest {
    std::string   classPath;
    std::string   speedCurvePath;
    std::string   pointDamageCurvePath;
    std::string   damageTypePath;

    std::uint8_t  targetKind = 0;
    std::string   targetName;

    float         locationCm[3] = {0.0f, 0.0f, 0.0f};
    float         rotation[3] = {0.0f, 0.0f, 0.0f};
    unsigned char params[0x120] = {};
};

struct AmmoHomingTarget {
    std::uintptr_t component = 0;
    std::uintptr_t actor = 0;
    std::string    componentName;
    std::string    componentClass;
    std::string    actorName;
    std::string    actorClass;
    std::uint64_t  socketRaw = 0;
    std::string    socketName;
};

[[nodiscard]] bool read_ammo_homing_target(std::uintptr_t ammoActor, AmmoHomingTarget& out, std::string& note);

struct LockonTarget {
    std::uintptr_t component = 0;
    std::string    componentName;
    std::uint64_t  socketRaw = 0;
    std::string    socketName;
    bool           asked = false;
    std::string    note;
};
[[nodiscard]] bool read_lockon_target(std::uintptr_t targetActor, LockonTarget& out, std::string& note);

struct AmmoSpawnPrepared {
    std::uintptr_t actorClass = 0;
    std::uintptr_t processEvent = 0;
    std::uintptr_t gameplayStaticsCdo = 0;
    std::uintptr_t spawnAmmoFunction = 0;
    std::string    classNameForLog;
    unsigned char  params[0x120] = {};

    std::uintptr_t targetComponent = 0;
    std::uint64_t  targetSocketRaw = 0;
    std::string    targetSocketName;
    std::uintptr_t setTargetFunction = 0;
    std::uintptr_t setSocketFunction = 0;
};

struct ActorPlacement {
    float locationCm[3] = {0.0f, 0.0f, 0.0f};
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    bool  valid = false;
};
[[nodiscard]] bool read_actor_placement(std::uintptr_t actor, ActorPlacement& out, std::string& note);

struct ComponentPlacement {
    float locationCm[3] = {0.0f, 0.0f, 0.0f};
    bool  valid = false;
};

[[nodiscard]] bool object_from_weak_index(std::uint32_t index, std::uint32_t serial, std::uintptr_t& out,
                                          std::string& note);

[[nodiscard]] bool read_component_location(std::uintptr_t component, ComponentPlacement& out,
                                            std::string& note);

[[nodiscard]] bool read_viewport_size(std::uintptr_t playerController, std::int32_t& width,
                                      std::int32_t& height, std::string& note);
[[nodiscard]] bool project_world_to_screen(std::uintptr_t playerController, const float worldCm[3],
                                           float& screenX, float& screenY, std::string& note);
[[nodiscard]] bool set_hidden_in_game(std::uintptr_t component, bool hidden, std::string& note);

[[nodiscard]] bool locate_render_primitive(std::uintptr_t component, const float expectedLocationCm[3],
                                           RenderPrimitive& out);
[[nodiscard]] bool read_render_primitive(std::uintptr_t component, std::uintptr_t proxySlot,
                                         std::uintptr_t matrixOffset, std::uintptr_t sceneInfoSlot,
                                         RenderPrimitive& out);

struct ActorBox {
    float originCm[3] = {0.0f, 0.0f, 0.0f};
    float extentCm[3] = {0.0f, 0.0f, 0.0f};
    bool  valid = false;
};
[[nodiscard]] bool read_actor_bounds(std::uintptr_t actor, ActorBox& out, std::string& note);

struct WorldBounds {
    bool  read = false;
    float killZ = 0.0f;
    bool  boundsChecks = false;
    std::string note;
};
[[nodiscard]] WorldBounds read_world_bounds();

[[nodiscard]] bool prepare_ammo_spawn(const AmmoSpawnRequest& request, const LockonTarget& homing,
                                      AmmoSpawnPrepared& out, std::string& note);

[[nodiscard]] bool spawn_ammo_from_params(const AmmoSpawnPrepared& prepared, const ActorPlacement& placement,
                                           std::uintptr_t owner, std::uintptr_t instigator,
                                           std::uintptr_t& actorOut, std::string& note);

[[nodiscard]] bool read_process_bytes(std::uintptr_t address, void* dst, std::size_t bytes);
[[nodiscard]] std::string object_class_name(std::uintptr_t obj);

[[nodiscard]] std::string object_own_name(std::uintptr_t obj);

[[nodiscard]] bool make_ftext(const std::wstring& text, unsigned char* out18, std::string& note);

struct MenuHandle {
    bool           shown = false;
    std::string    note;
    std::uintptr_t task = 0;
    std::uintptr_t selectedAddress = 0;
    std::int32_t   selected = -1;
    std::uint32_t  optionCount = 0;
};

[[nodiscard]] MenuHandle menu_show_select(std::uintptr_t causer, const std::vector<std::wstring>& options,
                                          const std::wstring& mainCaption,
                                          const std::wstring& subCaption);

[[nodiscard]] std::int32_t menu_read_choice(std::uintptr_t selectedAddress);

struct MenuTaskState {
    bool          readable = false;
    std::uint32_t onCancelled = 0;
    std::uint32_t onFinished = 0;
};

[[nodiscard]] MenuTaskState menu_read_task_state(std::uintptr_t task);

[[nodiscard]] std::uintptr_t resolve_uclass_address(const char* className);

struct FunctionRef {
    std::uintptr_t function = 0;
    std::uintptr_t processEvent = 0;
    bool           usable = false;
    std::string    note;
};

[[nodiscard]] bool object_is_still_registered(std::uintptr_t object, std::string& note);

[[nodiscard]] std::uintptr_t find_object_of_class(const char* className);

[[nodiscard]] FunctionRef reflect_resolve_function(std::uintptr_t declaringClass,
                                                   const char* functionName);

[[nodiscard]] bool reflect_invoke_function(std::uintptr_t self, const FunctionRef& fn,
                                           void* params, std::string& note);

struct StaticFunctionRef {
    std::uintptr_t cdo = 0;
    FunctionRef    fn;
    bool           usable = false;
    std::string    note;
};

[[nodiscard]] StaticFunctionRef reflect_resolve_static(const char* className, const char* functionName);

[[nodiscard]] std::uintptr_t main_module_base();

[[nodiscard]] bool write_process_bytes(std::uintptr_t address, const void* src, std::size_t bytes);

[[nodiscard]] bool stance_call_rep_notify(std::uintptr_t actor, std::uint8_t oldStance,
                                          std::string& note);

struct CharacterDerivedFlags {
    bool sprint = false;
    bool sprinting = false;
    bool hover = false;
    bool hovering = false;

    bool initialized = false;
};

[[nodiscard]] bool character_call_derived_flags(std::uintptr_t actor, CharacterDerivedFlags& out,
                                                std::string& note);

[[nodiscard]] bool character_call_set_sprint(std::uintptr_t actor, bool sprint, std::string& note);

[[nodiscard]] bool character_apply_sprint_effect(std::uintptr_t actor, std::uint64_t& handleOut,
                                                 std::string& note);
[[nodiscard]] bool character_remove_sprint_effect(std::uintptr_t actor, std::uint64_t handle,
                                                  std::string& note);

[[nodiscard]] bool character_call_sprint_ability(std::uintptr_t actor, std::string& note);

[[nodiscard]] bool character_sprint_delegate_text(std::uintptr_t actor, std::string& out);

[[nodiscard]] bool character_exclusive_delegate_text(std::uintptr_t actor, std::string& out);

[[nodiscard]] bool character_start_exclusive_mode(std::uintptr_t actor, std::uintptr_t owner,
                                                  std::uint8_t types, std::string& note);

[[nodiscard]] bool character_end_exclusive_mode(std::uintptr_t actor, std::string& note);

struct ModuleArrivalResult {

    bool        callMade[9] = {false, false, false, false, false, false, false, false, false};

    std::string unresolved;

    std::string note;
};

[[nodiscard]] const char* const* module_names_rep_notify_slots();

bool module_call_arrival_notify(std::uintptr_t machine, ModuleArrivalResult& out);

[[nodiscard]] bool machine_call_equip_front_weapon(std::uintptr_t machine, std::uintptr_t newFrontWeapon,
                                                   std::string& note);

[[nodiscard]] bool capture_doll_customize(std::uintptr_t machineOrDollActor, DollCustomize& out,
                                          std::string& note);

int            puppet_destroy_all_hosts();

int            puppet_destroy_actors(const std::vector<std::uintptr_t>& actors);

struct NetLayerState {
    std::uintptr_t world = 0;
    std::uintptr_t netDriver = 0;
    std::uintptr_t authorityGameMode = 0;
    std::uint32_t  worldCount = 0;
    std::string    driverName;
    std::string    driverClass;
    std::string    survey;
    std::string    note;
    [[nodiscard]] std::string line() const;
};

[[nodiscard]] NetLayerState net_layer_state();

[[nodiscard]] bool net_open_level_listen(const std::string& map, const std::string& options,
                                         std::string& note);

[[nodiscard]] bool net_client_connect(const std::string& url, std::string& note);

}
