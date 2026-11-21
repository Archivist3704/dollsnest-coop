add_executable(dsh-selftest
    src/main.cpp
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/appearance_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/doll_stance_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/exclusive_mode_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/front_weapon_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/movement_input_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/movement_state_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/module_component.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/observed_watch.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/health_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/ammo_spawn.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/item_wire.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/src/puppet_dolls.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-reflect/src/health_core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-reflect/src/render_scan.cpp"
)

target_include_directories(dsh-selftest PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-game/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../dsh-reflect/include")
target_link_libraries(dsh-selftest PRIVATE dsh::dsh dsh::platform dsh::net dsh::sync)
