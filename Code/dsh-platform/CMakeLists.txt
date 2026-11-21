add_library(dsh-platform STATIC
    src/log.cpp
    src/hook.cpp
    src/file_digest.cpp
)
add_library(dsh::platform ALIAS dsh-platform)

target_include_directories(dsh-platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(dsh-platform PUBLIC dsh::dsh)

target_compile_definitions(dsh-platform PRIVATE DSH_PLATFORM_BUILD=1)

target_link_libraries(dsh-platform PRIVATE
    kernel32
    user32
    bcrypt
)

if(DSH_HAVE_MINHOOK)
    target_compile_definitions(dsh-platform PUBLIC DSH_HAVE_MINHOOK=1)
    target_link_libraries(dsh-platform PUBLIC minhook)
else()
    target_compile_definitions(dsh-platform PUBLIC DSH_HAVE_MINHOOK=0)
endif()
