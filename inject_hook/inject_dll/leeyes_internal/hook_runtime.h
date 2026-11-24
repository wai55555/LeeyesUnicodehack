#pragma once

#include <Windows.h>

namespace LeeyesInternal
{

void RequestTraceStop();
bool StopTraceWorkerAndJoin();

}

extern "C" __declspec(dllexport) void WINAPI InitializeLeeyesInternalHooks();
