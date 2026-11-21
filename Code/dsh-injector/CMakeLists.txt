add_executable(dsh-injector
    src/main.cpp
)

target_compile_definitions(dsh-injector PRIVATE UNICODE _UNICODE)
target_link_libraries(dsh-injector PRIVATE kernel32)
