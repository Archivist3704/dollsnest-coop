add_library(dsh-game STATIC
    src/game.cpp
    src/pawn.cpp
    src/ammo_spawn.cpp
    src/pose_wire.cpp
    src/appearance_wire.cpp
    src/item_wire.cpp
    src/mission_wire.cpp
    src/health_wire.cpp
    src/health_sync.cpp
    src/doll_stance.cpp
    src/doll_stance_wire.cpp
    src/exclusive_mode_wire.cpp
    src/front_weapon.cpp
    src/front_weapon_wire.cpp
    src/movement_input.cpp
    src/movement_input_wire.cpp
    src/module_component.cpp
    src/movement_state.cpp
    src/movement_state_wire.cpp
    src/net_runtime.cpp
    src/observed_watch.cpp
    src/puppet_dolls.cpp
    src/puppet_session.cpp
)
add_library(dsh::game ALIAS dsh-game)

target_include_directories(dsh-game PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dsh-game PUBLIC dsh::dsh dsh::platform dsh::reflect dsh::net dsh::sync)
