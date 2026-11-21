add_library(dsh INTERFACE)
add_library(dsh::dsh ALIAS dsh)

target_include_directories(dsh INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
