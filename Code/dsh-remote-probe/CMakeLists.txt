add_executable(dsh-remote-probe
    src/main.cpp
)
target_link_libraries(dsh-remote-probe PRIVATE kernel32)
