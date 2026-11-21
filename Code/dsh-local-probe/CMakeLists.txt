add_executable(dsh-local-probe
    src/main.cpp
)
target_link_libraries(dsh-local-probe PRIVATE kernel32)
