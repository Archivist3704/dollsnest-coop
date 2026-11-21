#pragma once

#include <cstdint>

namespace dsh::platform {

struct FileDigest {
    bool ok = false;
    std::uint64_t size = 0;

    char hex[17] = {0};

    char error[96] = {0};
};

[[nodiscard]] FileDigest file_sha256_prefix(const wchar_t* path);

}
