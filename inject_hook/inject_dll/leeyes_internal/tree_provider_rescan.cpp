#include "../stdafx.h"
#include "tree_provider_rescan.h"
#include "tree_layout_guard.h"

#include <atomic>
#include <cstring>
#include <limits>

#pragma comment(lib, "comctl32.lib")

namespace LeeyesInternal
{
namespace
{

std::atomic<bool> gHooksReady(false);
__declspec(thread) bool gSilentRescan = false;
__declspec(thread) HRESULT gShellEnumFailure = S_OK;
extern "C" DWORD gShellApplicationAddress = 0;
extern "C" DWORD gShellOwnerContinuation = 0;
extern "C" DWORD gShellEnumContinuation = 0;

extern "C" BOOL __cdecl TreeProviderUseSilentOwner()
{
	return gSilentRescan ? TRUE : FALSE;
}

extern "C" void __cdecl TreeProviderObserveShellResult(HRESULT result)
{
	if( gSilentRescan && FAILED(result) && SUCCEEDED(gShellEnumFailure) )
		gShellEnumFailure = result;
}

extern "C" __declspec(naked) void TreeProviderShellOwnerHook()
{
	__asm
	{
		pushfd
		pushad
		call TreeProviderUseSilentOwner
		test eax, eax
		jnz silent_owner
		popad
		popfd
		mov eax, dword ptr [gShellApplicationAddress]
		mov eax, dword ptr [eax]
		mov eax, dword ptr [eax]
		mov eax, dword ptr [eax + 30h]
		jmp dword ptr [gShellOwnerContinuation]
	silent_owner:
		popad
		popfd
		mov eax, 0
		jmp dword ptr [gShellOwnerContinuation]
	}
}

extern "C" __declspec(naked) void TreeProviderShellEnumHook()
{
	__asm
	{
		mov eax, dword ptr [eax]
		call dword ptr [eax + 10h]
		pushfd
		pushad
		push eax
		call TreeProviderObserveShellResult
		add esp, 4
		popad
		popfd
		jmp dword ptr [gShellEnumContinuation]
	}
}

bool MakeJump(BYTE* site, size_t length, void* destination, BYTE* patch)
{
	if( !site || !destination || !patch || length < 5 ) return false;
	const LONGLONG relative = static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(destination))
		- static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(site + 5));
	if( relative < (std::numeric_limits<LONG>::min)()
		|| relative > (std::numeric_limits<LONG>::max)() ) return false;
	memset(patch, 0x90, length);
	patch[0] = 0xE9;
	const LONG displacement = static_cast<LONG>(relative);
	memcpy(patch + 1, &displacement, sizeof(displacement));
	return true;
}

bool WritePatch(BYTE* site, const BYTE* patch, size_t length)
{
	DWORD oldProtection = 0;
	if( !site || !patch || !length
		|| !::VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &oldProtection) ) return false;
	memcpy(site, patch, length);
	::FlushInstructionCache(::GetCurrentProcess(), site, length);
	DWORD ignored = 0;
	return ::VirtualProtect(site, length, oldProtection, &ignored) != FALSE;
}

}

bool InstallTreeProviderRescanHooks()
{
	if( gHooksReady.load(std::memory_order_acquire) ) return true;
	BYTE* ownerSite = nullptr;
	BYTE* enumSite = nullptr;
	if( !GetLeeyesTreeProviderHookSites(ownerSite, enumSite) ) return false;
	HMODULE pinned = nullptr;
	if( !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCWSTR>(&TreeProviderShellOwnerHook), &pinned) ) return false;

	BYTE ownerOriginal[10] = {};
	BYTE enumOriginal[5] = {};
	BYTE ownerPatch[10] = {};
	BYTE enumPatch[5] = {};
	memcpy(ownerOriginal, ownerSite, sizeof(ownerOriginal));
	memcpy(enumOriginal, enumSite, sizeof(enumOriginal));
	if( ownerOriginal[0] != 0xA1 || enumOriginal[0] != 0x8B
		|| !MakeJump(ownerSite, sizeof(ownerPatch), reinterpret_cast<void*>(&TreeProviderShellOwnerHook), ownerPatch)
		|| !MakeJump(enumSite, sizeof(enumPatch), reinterpret_cast<void*>(&TreeProviderShellEnumHook), enumPatch) ) return false;
	memcpy(&gShellApplicationAddress, ownerOriginal + 1, sizeof(gShellApplicationAddress));
	gShellOwnerContinuation = reinterpret_cast<DWORD>(ownerSite + sizeof(ownerPatch));
	gShellEnumContinuation = reinterpret_cast<DWORD>(enumSite + sizeof(enumPatch));
	if( !WritePatch(ownerSite, ownerPatch, sizeof(ownerPatch)) ) return false;
	if( !WritePatch(enumSite, enumPatch, sizeof(enumPatch)) )
	{
		// The owner hook is pass-through while gSilentRescan is false. Restore it
		// when possible; if protection restoration fails the pinned hook remains safe.
		WritePatch(ownerSite, ownerOriginal, sizeof(ownerOriginal));
		return false;
	}
	gHooksReady.store(true, std::memory_order_release);
	return true;
}

bool InvokeTreeProviderRescan(DWORD owner, DWORD node, HRESULT& failure)
{
	failure = E_FAIL;
	if( !gHooksReady.load(std::memory_order_acquire) || !owner || !node
		|| !VerifyLeeyesTreeLayout() ) return false;
	const DWORD rescan = 0x004F7BE0;
	bool completed = false;
	gShellEnumFailure = S_OK;
	__try
	{
		__try
		{
			gSilentRescan = true;
			__asm
			{
				mov eax, owner
				mov edx, node
				call dword ptr [rescan]
			}
			completed = true;
		}
		__except( EXCEPTION_EXECUTE_HANDLER )
		{
			failure = static_cast<HRESULT>(::GetExceptionCode());
		}
	}
	__finally
	{
		gSilentRescan = false;
	}
	if( !completed ) return false;
	failure = gShellEnumFailure;
	return SUCCEEDED(failure);
}

}
