#pragma once

#include <Windows.h>

namespace LeeyesInternal
{

bool InstallTreeProviderRescanHooks();
bool InvokeTreeProviderRescan(DWORD owner, DWORD node, HRESULT& failure);

}
