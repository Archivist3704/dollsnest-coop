#pragma once

#include <cstdint>

extern "C" {

__declspec(dllexport) std::int32_t __cdecl DSH_Init(const char* commandLineUtf8);

__declspec(dllexport) void __cdecl DSH_Shutdown();

__declspec(dllexport) const char* __cdecl DSH_BuildStamp();

__declspec(dllexport) std::uint32_t __cdecl DSH_ApiVersion();

__declspec(dllexport) void __cdecl DSH_TestPing(const char* tag);

}
