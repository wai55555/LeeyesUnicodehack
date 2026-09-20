#include "ncodehook_bridge.h"

#include "../../NCodeHook/NCodeHook.cpp"

extern "C" void* LeeyesInternalCreateHook(void* original, void* hook)
{
	typedef void (__cdecl *HookFunction)();
	return reinterpret_cast<void*>(nCodeHook.createHook(
		reinterpret_cast<HookFunction>(original), reinterpret_cast<HookFunction>(hook)));
}

extern "C" bool LeeyesInternalRemoveHook(void* hook)
{
	typedef void (__cdecl *HookFunction)();
	return nCodeHook.removeHook(reinterpret_cast<HookFunction>(hook));
}
