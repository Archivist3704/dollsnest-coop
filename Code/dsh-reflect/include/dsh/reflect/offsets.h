#pragma once

#include <cstdint>

namespace dsh::reflect::offsets {

inline constexpr const char* kEngineBuildString = "4.27.2-0+++UE4+4.27-Nitro-DollsNest";

inline constexpr std::int32_t kGObjects        = 0x04A73450;
inline constexpr std::int32_t kGNames          = 0x04A37100;
inline constexpr std::int32_t kProcessEvent    = 0x01281150;
inline constexpr std::int32_t kAppendString    = 0x010922E0;
inline constexpr std::uint32_t kProcessEventIdx = 0x44;

inline constexpr std::uint32_t kElementsPerChunk  = 0x10000;
inline constexpr std::uint32_t kObjectsOffset     = 0x00;
inline constexpr std::uint32_t kNumElementsOffset = 0x14;
inline constexpr std::uint32_t kNumChunksOffset   = 0x1C;
inline constexpr std::uint32_t kObjectItemStride  = 0x18;

inline constexpr std::uint32_t kUObjectVtableOffset    = 0x00;
inline constexpr std::uint32_t kUObjectClassOffset     = 0x10;
inline constexpr std::uint32_t kUObjectNameOffset      = 0x18;
inline constexpr std::uint32_t kUObjectOuterOffset     = 0x20;

inline constexpr std::uint32_t kUObjectIndexOffset      = 0x0C;

inline constexpr std::uint32_t kUStructSuperOffset      = 0x40;

inline constexpr std::uint32_t kFNameEntryHeaderSize = 2;

}
