add_library(dsh-ping-dll SHARED
    src/ping.cpp
)
set_target_properties(dsh-ping-dll PROPERTIES PREFIX "" OUTPUT_NAME "dsh-ping-dll")
target_link_libraries(dsh-ping-dll PRIVATE kernel32)
