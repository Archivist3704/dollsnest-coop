#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    int seconds = 60;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--seconds" && i + 1 < argc) {
            seconds = std::atoi(argv[i + 1]);
        }
    }
    if (seconds <= 0) seconds = 60;

    std::printf("pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
    std::printf("test target alive for %d s -- inject me now\n", seconds);
    std::fflush(stdout);

    for (int i = 0; i < seconds * 10; ++i) {
        Sleep(100);
    }
    std::printf("test target exiting normally\n");
    return 0;
}
