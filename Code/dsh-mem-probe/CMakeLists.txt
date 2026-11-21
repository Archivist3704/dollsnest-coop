add_executable(dsh-mem-probe
    src/main.cpp
)
target_link_libraries(dsh-mem-probe PRIVATE kernel32)
