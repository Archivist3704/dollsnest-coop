#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "dsh/platform/file_digest.h"

namespace dsh::platform {
namespace {

constexpr std::size_t kChunkBytes = 64 * 1024;

void set_error(FileDigest& digest, const char* stage, unsigned long code) {
    std::snprintf(digest.error, sizeof(digest.error), "%s error %lu", stage, code);
}

void format_prefix(const unsigned char* bytes, std::size_t count, char* out) {
    static const char* kHex = "0123456789abcdef";
    for (std::size_t i = 0; i < count; ++i) {
        out[i * 2] = kHex[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = kHex[bytes[i] & 0x0F];
    }
    out[count * 2] = '\0';
}

}

FileDigest file_sha256_prefix(const wchar_t* path) {
    FileDigest digest{};

    if (path == nullptr || path[0] == L'\0') {
        std::snprintf(digest.error, sizeof(digest.error), "no path to hash");
        return digest;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        set_error(digest, "CreateFile", GetLastError());
        return digest;
    }

    LARGE_INTEGER fileSize{};
    if (GetFileSizeEx(file, &fileSize)) {
        digest.size = static_cast<std::uint64_t>(fileSize.QuadPart);
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;

    NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(status)) {
        set_error(digest, "BCryptOpenAlgorithmProvider", static_cast<unsigned long>(status));
    } else {
        DWORD objectBytes = 0;
        DWORD written = 0;
        status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                                   &written, 0);
        if (!BCRYPT_SUCCESS(status)) {
            set_error(digest, "BCryptGetProperty", static_cast<unsigned long>(status));
        } else {
            std::vector<unsigned char> object(objectBytes);
            status = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0);
            if (!BCRYPT_SUCCESS(status)) {
                set_error(digest, "BCryptCreateHash", static_cast<unsigned long>(status));
            } else {
                std::vector<unsigned char> chunk(kChunkBytes);
                ok = true;
                for (;;) {
                    DWORD got = 0;
                    if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) {
                        set_error(digest, "ReadFile", GetLastError());
                        ok = false;
                        break;
                    }
                    if (got == 0) {
                        break;
                    }
                    status = BCryptHashData(hash, chunk.data(), got, 0);
                    if (!BCRYPT_SUCCESS(status)) {
                        set_error(digest, "BCryptHashData", static_cast<unsigned long>(status));
                        ok = false;
                        break;
                    }
                }

                if (ok) {
                    unsigned char sum[32] = {0};
                    status = BCryptFinishHash(hash, sum, sizeof(sum), 0);
                    if (!BCRYPT_SUCCESS(status)) {
                        set_error(digest, "BCryptFinishHash", static_cast<unsigned long>(status));
                        ok = false;
                    } else {
                        format_prefix(sum, 8, digest.hex);
                    }
                }
            }
        }
    }

    if (hash != nullptr) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != nullptr) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    CloseHandle(file);

    digest.ok = ok;
    return digest;
}

}
