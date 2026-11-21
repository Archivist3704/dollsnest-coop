#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dsh::reflect {

struct ModuleInfo {
    std::string   name;
    std::string   fullPath;
    std::uintptr_t base = 0;
    std::uint64_t  size = 0;

    [[nodiscard]] bool contains(const void* p) const noexcept {
        const auto v = reinterpret_cast<std::uintptr_t>(p);
        return v >= base && v < base + size;
    }
    [[nodiscard]] bool contains(std::uintptr_t v) const noexcept {
        return v >= base && v < base + size;
    }
};

struct SectionInfo {
    std::string name;
    std::uintptr_t start = 0;
    std::uint64_t  size = 0;
    std::uint32_t  characteristics = 0;

    [[nodiscard]] bool writable() const noexcept {
        return (characteristics & 0x80000000u) != 0;
    }
    [[nodiscard]] bool executable() const noexcept {
        return (characteristics & 0x20000000u) != 0;
    }
};

[[nodiscard]] ModuleInfo   main_module();
[[nodiscard]] std::vector<SectionInfo> sections_of(const ModuleInfo& module);

[[nodiscard]] bool looks_like_image_pointer(std::uintptr_t v);

[[nodiscard]] bool try_read_u64(std::uintptr_t address, std::uint64_t& out);
[[nodiscard]] bool try_read_u32(std::uintptr_t address, std::uint32_t& out);
[[nodiscard]] bool try_read_bytes(std::uintptr_t address, void* dst, std::size_t n);

[[nodiscard]] inline std::uint64_t read_u64_fast(std::uintptr_t address) noexcept {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(v));
    return v;
}

[[nodiscard]] std::uint64_t pointer_check_count();
[[nodiscard]] std::uint64_t range_refresh_count();
void reset_scanner_counters();
[[nodiscard]] std::size_t   module_count();

[[nodiscard]] std::string describe_region(std::uintptr_t address);

}
