add_executable(dsh-test-target
    src/main.cpp
)
target_link_libraries(dsh-test-target PRIVATE kernel32)
