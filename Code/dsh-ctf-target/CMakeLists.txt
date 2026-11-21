add_executable(dsh-ctf-target
    src/main.cpp
)
target_link_libraries(dsh-ctf-target PRIVATE kernel32)
