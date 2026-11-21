add_library(dsh-reflect STATIC
    src/placeholder.cpp
    src/scanner.cpp
    src/reflection.cpp
    src/processevent.cpp
    src/world.cpp
    src/spawn.cpp
    src/render_scan.cpp
    src/gamethread.cpp
    src/mission.cpp
    src/enemies.cpp
    src/target_model.cpp
    src/gates.cpp
    src/health_core.cpp
)
add_library(dsh::reflect ALIAS dsh-reflect)

target_include_directories(dsh-reflect PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dsh-reflect PUBLIC dsh::platform)

target_link_libraries(dsh-reflect PRIVATE psapi)

if(DSH_HAVE_LAYOUT)
    target_compile_definitions(dsh-reflect PUBLIC DSH_HAVE_LAYOUT=1)
    target_include_directories(dsh-reflect PUBLIC "${DSH_LAYOUT_DIR}")
else()
    target_compile_definitions(dsh-reflect PUBLIC DSH_HAVE_LAYOUT=0)
endif()
