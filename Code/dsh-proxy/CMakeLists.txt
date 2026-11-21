add_library(dsh-proxy-version SHARED
    src/proxy.cpp
    src/proxy.rc
    exports.def
)

set_target_properties(dsh-proxy-version PROPERTIES
    PREFIX ""
    OUTPUT_NAME "version"
)

target_compile_options(dsh-proxy-version PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(dsh-proxy-version PRIVATE kernel32 user32)

message(STATUS "dsh-proxy: version.dll proxy target enabled (deploy path: copy version.dll + dollsnest-coop.dll into the game's Win64 directory)")
