add_executable(dsh-rva-check
    src/main.cpp
)
target_link_libraries(dsh-rva-check PRIVATE kernel32)
