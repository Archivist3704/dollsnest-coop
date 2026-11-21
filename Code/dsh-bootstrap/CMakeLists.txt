add_library(dsh-bootstrap SHARED
    src/dllmain.cpp
    src/init.cpp
)

target_include_directories(dsh-bootstrap PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dsh-bootstrap PRIVATE
    dsh::dsh
    dsh::platform
    dsh::reflect
    dsh::net
    dsh::sync
    dsh::game
)

set(DSH_BOOTSTRAP_NAME "dollsnest-coop" CACHE STRING "Output base name for the injected DLL")
set_target_properties(dsh-bootstrap PROPERTIES PREFIX "" OUTPUT_NAME "${DSH_BOOTSTRAP_NAME}")

string(TIMESTAMP DSH_BUILD_STAMP "%Y%m%d-%H%M%S" UTC)
target_compile_definitions(dsh-bootstrap PRIVATE DSH_BUILD_STAMP="${DSH_BUILD_STAMP}")

message(STATUS "dsh-bootstrap build stamp: ${DSH_BUILD_STAMP}")
