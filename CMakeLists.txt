cmake_minimum_required(VERSION 3.22)

project(dollsnest-coop LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "This project targets MSVC only (the game is an MSVC x64 Shipping build).")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(DSH_OUT_DIR "${CMAKE_SOURCE_DIR}/out/bin" CACHE PATH "Runtime output directory")
foreach(cfg "" "_DEBUG" "_RELWITHDEBINFO" "_RELEASE" "_MINSIZEREL")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY${cfg} "${DSH_OUT_DIR}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY${cfg} "${DSH_OUT_DIR}")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY${cfg} "${DSH_OUT_DIR}/lib")
endforeach()

add_compile_options(/W4 /permissive- /Zc:__cplusplus /utf-8 /MP)
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    UNICODE
    _UNICODE
)

set(DSH_THIRD_PARTY "${CMAKE_SOURCE_DIR}/Code/third_party")

if(EXISTS "${DSH_THIRD_PARTY}/minhook/CMakeLists.txt")

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    add_subdirectory("${DSH_THIRD_PARTY}/minhook" "${CMAKE_BINARY_DIR}/minhook" EXCLUDE_FROM_ALL)
    if(TARGET minhook)
        set(DSH_HAVE_MINHOOK ON)
    else()
        set(DSH_HAVE_MINHOOK OFF)
        message(WARNING "MinHook directory present but the target was not created")
    endif()
else()
    set(DSH_HAVE_MINHOOK OFF)
    message(WARNING
        "MinHook not found at ${DSH_THIRD_PARTY}/minhook -- hooking will be compiled out.\n"
        "Run tools/fetch-deps.ps1 to fetch it.")
endif()

set(DSH_LAYOUT_DIR "${DSH_THIRD_PARTY}/layout" CACHE PATH "optional layout headers (not shipped)")
if(EXISTS "${DSH_LAYOUT_DIR}/SDK.hpp")
    set(DSH_HAVE_LAYOUT ON)
    message(STATUS "layout headers found at ${DSH_LAYOUT_DIR}")
else()
    set(DSH_HAVE_LAYOUT OFF)
    message(STATUS "layout headers not present at ${DSH_LAYOUT_DIR} -- offsets are verified at runtime")
endif()

add_subdirectory(Code/dsh)
add_subdirectory(Code/dsh-platform)
add_subdirectory(Code/dsh-reflect)
add_subdirectory(Code/dsh-net)
add_subdirectory(Code/dsh-sync)
add_subdirectory(Code/dsh-game)
add_subdirectory(Code/dsh-bootstrap)
add_subdirectory(Code/dsh-proxy)

add_subdirectory(Code/dsh-injector)
add_subdirectory(Code/dsh-test-target)
add_subdirectory(Code/dsh-local-probe)
add_subdirectory(Code/dsh-remote-probe)
add_subdirectory(Code/dsh-ping-dll)
add_subdirectory(Code/dsh-ctf-target)
add_subdirectory(Code/dsh-ctf-probe)
add_subdirectory(Code/dsh-rva-check)
add_subdirectory(Code/dsh-mem-probe)
add_subdirectory(Code/dsh-selftest)

message(STATUS "----------------------------------------------------------------")
message(STATUS "dollsnest-coop configuration")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  output dir   : ${DSH_OUT_DIR}")
message(STATUS "  MinHook      : ${DSH_HAVE_MINHOOK}")
message(STATUS "  layout headers : ${DSH_HAVE_LAYOUT}")
message(STATUS "----------------------------------------------------------------")
