#pragma once

#include <Windows.h>

namespace LeeyesInternal
{

void RequestTraceStop();

}

extern "C" __declspec(dllexport) void WINAPI InitializeLeeyesInternalHooks();
