add_executable(dsh-ctf-probe
    src/main.cpp
)
target_link_libraries(dsh-ctf-probe PRIVATE kernel32)
