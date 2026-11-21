#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace dsh::net {

inline constexpr std::uint16_t kProtocolVersion = 2;

class ByteWriter {
public:
    void u8(std::uint8_t v) { raw(&v, 1); }

    void u16(std::uint16_t v) {
        const std::uint8_t b[2] = {static_cast<std::uint8_t>(v & 0xFF),
                                   static_cast<std::uint8_t>((v >> 8) & 0xFF)};
        raw(b, 2);
    }

    void u32(std::uint32_t v) {
        const std::uint8_t b[4] = {static_cast<std::uint8_t>(v & 0xFF),
                                   static_cast<std::uint8_t>((v >> 8) & 0xFF),
                                   static_cast<std::uint8_t>((v >> 16) & 0xFF),
                                   static_cast<std::uint8_t>((v >> 24) & 0xFF)};
        raw(b, 4);
    }

    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v & 0xFFFFFFFFull));
        u32(static_cast<std::uint32_t>(v >> 32));
    }

    void f32(float v) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        u32(bits);
    }

    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buffer_.insert(buffer_.end(), b, b + n);
    }

    [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }
    [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
    [[nodiscard]] std::vector<std::uint8_t> take() { return std::move(buffer_); }

private:
    std::vector<std::uint8_t> buffer_;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] bool ok() const noexcept { return failed_ == false; }
    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - cursor_; }
    [[nodiscard]] std::size_t cursor() const noexcept { return cursor_; }

    bool u8(std::uint8_t& out) {
        if (remaining() < 1) return fail();
        out = data_[cursor_++];
        return true;
    }

    bool u16(std::uint16_t& out) {
        if (remaining() < 2) return fail();
        out = static_cast<std::uint16_t>(data_[cursor_]) |
              static_cast<std::uint16_t>(data_[cursor_ + 1]) << 8;
        cursor_ += 2;
        return true;
    }

    bool u32(std::uint32_t& out) {
        if (remaining() < 4) return fail();
        out = static_cast<std::uint32_t>(data_[cursor_]) |
              (static_cast<std::uint32_t>(data_[cursor_ + 1]) << 8) |
              (static_cast<std::uint32_t>(data_[cursor_ + 2]) << 16) |
              (static_cast<std::uint32_t>(data_[cursor_ + 3]) << 24);
        cursor_ += 4;
        return true;
    }

    bool u64(std::uint64_t& out) {
        std::uint32_t lo = 0, hi = 0;
        if (!u32(lo) || !u32(hi)) return false;
        out = (static_cast<std::uint64_t>(hi) << 32) | lo;
        return true;
    }

    bool f32(float& out) {
        std::uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&out, &bits, sizeof(out));
        return true;
    }

    bool raw(void* dst, std::size_t n) {
        if (remaining() < n) return fail();
        std::memcpy(dst, data_.data() + cursor_, n);
        cursor_ += n;
        return true;
    }

    bool skip(std::size_t n) {
        if (remaining() < n) return fail();
        cursor_ += n;
        return true;
    }

private:
    bool fail() {
        failed_ = true;
        return false;
    }

    std::span<const std::uint8_t> data_;
    std::size_t cursor_ = 0;
    bool failed_ = false;
};

struct QuantisedPosition {
    std::uint16_t x = 0, y = 0, z = 0;
};
inline constexpr float kPositionRangeMeters = 3276.7f;
inline constexpr float kPositionScale = 65535.0f / (2.0f * kPositionRangeMeters);

[[nodiscard]] inline std::uint16_t quantise_position_axis(float metres) {
    if (metres < -kPositionRangeMeters) metres = -kPositionRangeMeters;
    if (metres > kPositionRangeMeters) metres = kPositionRangeMeters;
    const float scaled = (metres + kPositionRangeMeters) * kPositionScale;
    const long rounded = static_cast<long>(scaled + 0.5f);
    if (rounded < 0) return 0;
    if (rounded > 65535) return 65535;
    return static_cast<std::uint16_t>(rounded);
}

[[nodiscard]] inline float dequantise_position_axis(std::uint16_t q) {
    return (static_cast<float>(q) / kPositionScale) - kPositionRangeMeters;
}

[[nodiscard]] inline std::uint8_t quantise_angle(float radians) {
    constexpr float twoPi = 6.28318530718f;
    float r = radians;
    while (r < 0.0f) r += twoPi;
    while (r >= twoPi) r -= twoPi;
    const float scaled = r * (255.0f / twoPi);
    long q = static_cast<long>(scaled + 0.5f);
    if (q < 0) q = 0;
    if (q > 255) q = 255;
    return static_cast<std::uint8_t>(q);
}

[[nodiscard]] inline float dequantise_angle(std::uint8_t q) {
    constexpr float twoPi = 6.28318530718f;

    return static_cast<float>(q) * (twoPi / 255.0f);
}

}
