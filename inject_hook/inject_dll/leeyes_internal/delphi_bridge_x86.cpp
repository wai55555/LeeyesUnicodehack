#include "../stdafx.h"
#include "delphi_bridge_x86.h"
#include "filelist_trace.h"
#include "file_move_batch.h"

#if !defined(_M_IX86)
#error "Leeyes internal Delphi bridge requires Win32 x86"
#endif

#include "ncodehook_bridge.h"
using LeeyesInternal::LeeyesInternalTraceEntry;
using LeeyesInternal::LeeyesInternalShouldDeferFileChange;
using LeeyesInternal::LeeyesInternalBeginChangeQueueDrain;
using LeeyesInternal::LeeyesInternalEndChangeQueueDrain;

extern "C" void (__cdecl *gLeeyesInternalOriginalRefresh)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalChanging)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalChange)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalLayoutRefresh)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalLayoutUpdate)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalFileChangeDispatch)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalChangeQueueDrain)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalFullResync)() = nullptr;
extern "C" void* gLeeyesInternalLowItemContinuation = nullptr;
extern "C" void* gLeeyesInternalLowItemMissingFallback = nullptr;
extern "C" volatile LONG gLeeyesInternalLowItemNullFallbackCount = 0;
extern "C" volatile LONG gLeeyesInternalLowItemGuardActive = 0;

extern "C" __declspec(naked) void LeeyesInternalRefreshHook()
{
	__asm
	{
		pushfd
		pushad
		mov eax, esp
		push eax
		push 1
		call LeeyesInternalTraceEntry
		add esp, 8
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalRefresh]
	}
}

extern "C" __declspec(naked) void LeeyesInternalChangingHook()
{
	__asm
	{
		pushfd
		pushad
		mov eax, esp
		push eax
		push 2
		call LeeyesInternalTraceEntry
		add esp, 8
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalChanging]
	}
}

extern "C" __declspec(naked) void LeeyesInternalChangeHook()
{
	__asm
	{
		pushfd
		pushad
		mov eax, esp
		push eax
		push 3
		call LeeyesInternalTraceEntry
		add esp, 8
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalChange]
	}
}

extern "C" __declspec(naked) void LeeyesInternalLayoutRefreshHook()
{
	__asm
	{
		pushfd
		pushad
		mov eax, esp
		push eax
		push 6
		call LeeyesInternalTraceEntry
		add esp, 8
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalLayoutRefresh]
	}
}

extern "C" __declspec(naked) void LeeyesInternalLayoutUpdateHook()
{
	__asm
	{
		pushfd
		pushad
		mov eax, esp
		push eax
		push 7
		call LeeyesInternalTraceEntry
		add esp, 8
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalLayoutUpdate]
	}
}

extern "C" __declspec(naked) void LeeyesInternalFileChangeDispatchHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, dword ptr [esp + 12]
		// After pushfd/pushad, [esp+12] points to the ESP saved by pushad.
		// That saved ESP points to EFLAGS, so return address and stack
		// arguments are at +4, +8, and +12 respectively.
		push dword ptr [ebx + 4]
		push dword ptr [ebx + 8]
		push ecx
		push edx
		push eax
		call LeeyesInternalShouldDeferFileChange
		add esp, 20
		test eax, eax
		jz pass_through
		popad
		popfd
		ret 4
	pass_through:
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalFileChangeDispatch]
	}
}

// The original 2.6.1 code performs the virtual lookup and immediately reads
// the returned item's action byte.  If the item disappeared between index
// lookup and retrieval, the existing not-found path at +0x58 is the only
// verified path that preserves the current notification processing.
extern "C" __declspec(naked) void LeeyesInternalLowItemLookupHook()
{
	__asm
	{
		call dword ptr [ecx + 18h]
		test eax, eax
		jnz have_item
		inc dword ptr [gLeeyesInternalLowItemNullFallbackCount]
		jmp dword ptr [gLeeyesInternalLowItemMissingFallback]
	have_item:
		mov al, byte ptr [eax]
		jmp dword ptr [gLeeyesInternalLowItemContinuation]
	}
}

extern "C" void __cdecl LeeyesInternalInstallLowItemNullGuardIfArmed();

extern "C" __declspec(naked) void LeeyesInternalChangeQueueDrainHook()
{
	__asm
	{
		pushfd
		pushad
		call LeeyesInternalInstallLowItemNullGuardIfArmed
		popad
		popfd
		pushfd
		pushad
		push eax
		call LeeyesInternalBeginChangeQueueDrain
		add esp, 4
		popad
		popfd
		call dword ptr [gLeeyesInternalOriginalChangeQueueDrain]
		pushfd
		pushad
		call LeeyesInternalEndChangeQueueDrain
		popad
		popfd
		ret
	}
}

extern "C" void __cdecl LeeyesInternalInvokeFullResync(void* listView)
{
	if( !listView || !gLeeyesInternalFullResync ) return;
	__asm
	{
		mov eax, listView
		mov dl, 1
		call dword ptr [gLeeyesInternalFullResync]
	}
}

namespace LeeyesInternal
{

namespace
{
typedef void (__cdecl *HookFunction)();

void* gLeeyesInternalLowItemLookupSite = nullptr;
void* gLeeyesInternalLowItemPendingSite = nullptr;
BYTE gLeeyesInternalLowItemOriginalBytes[5] = {};
bool gLeeyesInternalLowItemGuardInstalled = false;

HookFunction RefreshHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalRefreshHook);
}

HookFunction ChangingHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalChangingHook);
}

HookFunction ChangeHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalChangeHook);
}

HookFunction LayoutRefreshHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalLayoutRefreshHook);
}

HookFunction LayoutUpdateHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalLayoutUpdateHook);
}

HookFunction FileChangeDispatchHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalFileChangeDispatchHook);
}

HookFunction ChangeQueueDrainHook()
{
	return reinterpret_cast<HookFunction>(&LeeyesInternalChangeQueueDrainHook);
}

bool InstallLowItemNullGuard(void* lookupSite)
{
	if( !lookupSite || gLeeyesInternalLowItemGuardInstalled ) return false;
	const BYTE expected[5] = { 0xFF, 0x51, 0x18, 0x8A, 0x00 };
	if( memcmp(lookupSite, expected, sizeof(expected)) != 0 ) return false;
	BYTE* site = reinterpret_cast<BYTE*>(lookupSite);
	DWORD oldProtect = 0;
	if( !::VirtualProtect(site, sizeof(expected), PAGE_EXECUTE_READWRITE, &oldProtect) )
		return false;
	gLeeyesInternalLowItemNullFallbackCount = 0;
	memcpy(gLeeyesInternalLowItemOriginalBytes, site, sizeof(expected));
	BYTE patch[5] = { 0xE9, 0, 0, 0, 0 };
	const ULONG_PTR hookAddress = reinterpret_cast<ULONG_PTR>(
		&LeeyesInternalLowItemLookupHook);
	const ULONG_PTR siteEnd = reinterpret_cast<ULONG_PTR>(site + sizeof(patch));
	*reinterpret_cast<LONG*>(&patch[1]) = static_cast<LONG>(hookAddress - siteEnd);
	memcpy(site, patch, sizeof(patch));
	::VirtualProtect(site, sizeof(expected), oldProtect, &oldProtect);
	::FlushInstructionCache(::GetCurrentProcess(), site, sizeof(patch));
	gLeeyesInternalLowItemLookupSite = lookupSite;
	gLeeyesInternalLowItemContinuation = site + 5;
	gLeeyesInternalLowItemMissingFallback = site + 0x58;
	gLeeyesInternalLowItemGuardInstalled = true;
	gLeeyesInternalLowItemGuardActive = 1;
	return true;
}

void ArmLowItemNullGuard(void* lookupSite)
{
	if( !gLeeyesInternalLowItemGuardInstalled )
		gLeeyesInternalLowItemPendingSite = lookupSite;
}

void RemoveLowItemNullGuard()
{
	if( !gLeeyesInternalLowItemGuardInstalled || !gLeeyesInternalLowItemLookupSite ) return;
	BYTE* site = reinterpret_cast<BYTE*>(gLeeyesInternalLowItemLookupSite);
	DWORD oldProtect = 0;
	if( ::VirtualProtect(site, sizeof(gLeeyesInternalLowItemOriginalBytes),
		PAGE_EXECUTE_READWRITE, &oldProtect) )
	{
		memcpy(site, gLeeyesInternalLowItemOriginalBytes,
			sizeof(gLeeyesInternalLowItemOriginalBytes));
		::VirtualProtect(site, sizeof(gLeeyesInternalLowItemOriginalBytes),
			oldProtect, &oldProtect);
		::FlushInstructionCache(::GetCurrentProcess(), site,
			sizeof(gLeeyesInternalLowItemOriginalBytes));
	}
	gLeeyesInternalLowItemLookupSite = nullptr;
	gLeeyesInternalLowItemPendingSite = nullptr;
	gLeeyesInternalLowItemContinuation = nullptr;
	gLeeyesInternalLowItemMissingFallback = nullptr;
	gLeeyesInternalLowItemGuardInstalled = false;
	gLeeyesInternalLowItemGuardActive = 0;
}

extern "C" void __cdecl LeeyesInternalInstallLowItemNullGuardIfArmed()
{
	if( gLeeyesInternalLowItemGuardInstalled || !gLeeyesInternalLowItemPendingSite ) return;
	void* site = gLeeyesInternalLowItemPendingSite;
	if( InstallLowItemNullGuard(site) )
		gLeeyesInternalLowItemPendingSite = nullptr;
}
}

bool InstallPassThroughHooks(const ResolvedTargets& targets)
{
	if( !targets.refresh || !targets.changing || !targets.change
		|| !targets.changeCallsRefresh )
		return false;
	gLeeyesInternalOriginalRefresh = reinterpret_cast<HookFunction>(LeeyesInternalCreateHook(
		targets.refresh, reinterpret_cast<void*>(RefreshHook())));
	if( !gLeeyesInternalOriginalRefresh ) return false;
	gLeeyesInternalOriginalChanging = reinterpret_cast<HookFunction>(LeeyesInternalCreateHook(
		targets.changing, reinterpret_cast<void*>(ChangingHook())));
	if( !gLeeyesInternalOriginalChanging )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(RefreshHook()));
		gLeeyesInternalOriginalRefresh = nullptr;
		return false;
	}
	gLeeyesInternalOriginalChange = reinterpret_cast<HookFunction>(LeeyesInternalCreateHook(
		targets.change, reinterpret_cast<void*>(ChangeHook())));
	if( !gLeeyesInternalOriginalChange )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(ChangingHook()));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(RefreshHook()));
		gLeeyesInternalOriginalChanging = nullptr;
		gLeeyesInternalOriginalRefresh = nullptr;
		return false;
	}
	if( targets.layoutRefresh )
		gLeeyesInternalOriginalLayoutRefresh = reinterpret_cast<HookFunction>(LeeyesInternalCreateHook(
			targets.layoutRefresh, reinterpret_cast<void*>(LayoutRefreshHook())));
	if( targets.layoutUpdate )
		gLeeyesInternalOriginalLayoutUpdate = reinterpret_cast<HookFunction>(LeeyesInternalCreateHook(
			targets.layoutUpdate, reinterpret_cast<void*>(LayoutUpdateHook())));
	return true;
}

void RemovePassThroughHooks()
{
	if( gLeeyesInternalOriginalChange ) LeeyesInternalRemoveHook(reinterpret_cast<void*>(ChangeHook()));
	if( gLeeyesInternalOriginalChanging ) LeeyesInternalRemoveHook(reinterpret_cast<void*>(ChangingHook()));
	if( gLeeyesInternalOriginalRefresh ) LeeyesInternalRemoveHook(reinterpret_cast<void*>(RefreshHook()));
	if( gLeeyesInternalOriginalLayoutUpdate ) LeeyesInternalRemoveHook(reinterpret_cast<void*>(LayoutUpdateHook()));
	if( gLeeyesInternalOriginalLayoutRefresh ) LeeyesInternalRemoveHook(reinterpret_cast<void*>(LayoutRefreshHook()));
	gLeeyesInternalOriginalChange = nullptr;
	gLeeyesInternalOriginalChanging = nullptr;
	gLeeyesInternalOriginalRefresh = nullptr;
	gLeeyesInternalOriginalLayoutUpdate = nullptr;
	gLeeyesInternalOriginalLayoutRefresh = nullptr;
}

bool InstallFileMoveBatchHooks(const ResolvedTargets& targets)
{
	if( !targets.lowItemLookup || !targets.fileChangeDispatch
		|| !targets.changeQueueDrain || !targets.fullResync ) return false;
	gLeeyesInternalFullResync = reinterpret_cast<HookFunction>(targets.fullResync);
	ArmLowItemNullGuard(targets.lowItemLookup);
	gLeeyesInternalOriginalFileChangeDispatch = reinterpret_cast<HookFunction>(
		LeeyesInternalCreateHook(targets.fileChangeDispatch,
			reinterpret_cast<void*>(FileChangeDispatchHook())));
	if( !gLeeyesInternalOriginalFileChangeDispatch )
	{
		gLeeyesInternalFullResync = nullptr;
		gLeeyesInternalLowItemPendingSite = nullptr;
		return false;
	}
	gLeeyesInternalOriginalChangeQueueDrain = reinterpret_cast<HookFunction>(
		LeeyesInternalCreateHook(targets.changeQueueDrain,
			reinterpret_cast<void*>(ChangeQueueDrainHook())));
	if( !gLeeyesInternalOriginalChangeQueueDrain )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(FileChangeDispatchHook()));
		gLeeyesInternalOriginalFileChangeDispatch = nullptr;
		gLeeyesInternalFullResync = nullptr;
		gLeeyesInternalLowItemPendingSite = nullptr;
		return false;
	}
	return true;
}

void RemoveFileMoveBatchHooks()
{
	if( gLeeyesInternalOriginalChangeQueueDrain )
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(ChangeQueueDrainHook()));
	if( gLeeyesInternalOriginalFileChangeDispatch )
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(FileChangeDispatchHook()));
	gLeeyesInternalOriginalChangeQueueDrain = nullptr;
	gLeeyesInternalOriginalFileChangeDispatch = nullptr;
	gLeeyesInternalFullResync = nullptr;
	RemoveLowItemNullGuard();
	gLeeyesInternalLowItemPendingSite = nullptr;
}

}
