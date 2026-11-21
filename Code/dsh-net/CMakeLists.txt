add_library(dsh-net STATIC
    src/transport.cpp
    src/session.cpp
    src/packet.cpp
    src/discovery.cpp
)
add_library(dsh::net ALIAS dsh-net)

target_include_directories(dsh-net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dsh-net PUBLIC dsh::platform)

target_link_libraries(dsh-net PRIVATE ws2_32)

target_link_libraries(dsh-net PUBLIC iphlpapi)
