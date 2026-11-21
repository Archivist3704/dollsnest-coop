add_library(dsh-sync STATIC
    src/cadence.cpp
    src/entity_registry.cpp
    src/interpolator.cpp
    src/playback_clock.cpp
)
add_library(dsh::sync ALIAS dsh-sync)

target_include_directories(dsh-sync PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dsh-sync PUBLIC dsh::dsh dsh::net)
