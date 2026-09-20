#pragma once

#include <Windows.h>

namespace LeeyesInternal
{

void ConfigureFileMoveBatch(DWORD threshold, bool diagnosticLog);
void ResetFileMoveBatchState();

extern "C" BOOL __cdecl LeeyesInternalShouldDeferFileChange(
	void* listView, DWORD rawAction, void* returnAddress);
extern "C" void __cdecl LeeyesInternalBeginChangeQueueDrain(void* notifier);
extern "C" void __cdecl LeeyesInternalEndChangeQueueDrain();

}
