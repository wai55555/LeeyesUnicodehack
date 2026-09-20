#pragma once

#include "../../NCodeHook/NCodeHook.h"

typedef NCodeHook<ArchitectureIA32> NCodeHookIA32;
extern NCodeHookIA32 nCodeHook;

extern "C" void* LeeyesInternalCreateHook(void* original, void* hook);
extern "C" bool LeeyesInternalRemoveHook(void* hook);
