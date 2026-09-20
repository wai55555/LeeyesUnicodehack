#pragma once

#include <Windows.h>
#include <cstdint>

namespace LeeyesInternal
{

enum class TargetKind : DWORD
{
	Refresh = 1,
	Changing = 2,
	Change = 3,
	FolderChange = 4,
	NotifyTimer = 5,
	LayoutRefresh = 6,
	LayoutUpdate = 7,
	FileChangeDispatch = 8,
	FullResync = 9,
	ChangeQueueDrain = 10
};

struct ResolvedTargets
{
	HMODULE module = nullptr;
	uintptr_t imageBase = 0;
	DWORD sizeOfImage = 0;
	void* refresh = nullptr;
	void* changing = nullptr;
	void* change = nullptr;
	void* folderChange = nullptr;
	void* notifyTimer = nullptr;
	void* layoutRefresh = nullptr;
	void* layoutUpdate = nullptr;
	void* fileChangeDispatch = nullptr;
	void* fullResync = nullptr;
	void* changeQueueDrain = nullptr;
	bool changeCallsRefresh = false;
	char failure[128] = {};
};

bool ResolveTargets(ResolvedTargets& targets, bool requireTraceTargets,
	bool requireBatchTargets);
const char* TargetKindName(TargetKind kind);

}
