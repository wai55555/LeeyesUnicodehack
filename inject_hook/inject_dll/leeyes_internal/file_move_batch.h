#pragma once

#include <Windows.h>

namespace LeeyesInternal
{

void ConfigureFileMoveBatch(DWORD threshold, bool diagnosticLog);
void ConfigureListDeleteBatch(bool enabled);
void ConfigureListAddSortBatch(bool enabled);
void ResetFileMoveBatchState();

extern "C" BOOL __cdecl LeeyesInternalShouldDeferFileChange(
	void* dispatchOwner, DWORD rawAction, DWORD path0, DWORD path1,
	void* returnAddress);
extern "C" void __cdecl LeeyesInternalBeginChangeQueueDrain(void* notifier);
extern "C" void __cdecl LeeyesInternalEndChangeQueueDrain();

}
