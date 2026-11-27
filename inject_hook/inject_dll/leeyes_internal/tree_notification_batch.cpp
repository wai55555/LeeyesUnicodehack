#include "../stdafx.h"
#include "tree_notification_batch.h"
#include "ncodehook_bridge.h"
#include "tree_layout_guard.h"
#include "tree_node_path.h"
#include "tree_provider_rescan.h"
#include "file_move_batch.h"
#include "../../common/watched_sync_worker.h"

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstring>
#include <map>
#include <memory>
#include <limits>
#include <new>
#include <stdio.h>
#include <set>
#include <string>
#include <vector>
#include <utility>

#if !defined(_M_IX86)
#error "Leeyes tree notification batching requires Win32 x86"
#endif

#pragma comment(lib, "comctl32.lib")

extern "C" void* gLeeyesTreeBatchOriginalSearch = nullptr;
extern "C" void* gLeeyesTreeBatchOriginalParentSearch = nullptr;
extern "C" DWORD gLeeyesTreeBatchAddEventContinuation = 0;
extern "C" DWORD gLeeyesTreeBatchDeleteEventContinuation = 0;
extern "C" void StartupDiagnosticLog(const char* stage, const char* detail);
extern HMODULE gInjectedModule;

namespace LeeyesInternal
{
extern unsigned long long gTreePathSearchVisited;
extern unsigned long long gTreePathSearchKnownPaths;
extern unsigned long long gTreePathSearchBestPrefix;
extern unsigned long long gTreePathSearchPathNodes;
extern unsigned long long gTreePathSearchItemFailures;
extern bool gTreePathSearchVerifyFailed;
}

namespace LeeyesInternal
{
namespace
{

const size_t kParentCapacity = 2;
const size_t kShadowChildLimit = 4096;
const size_t kRemovalCompareLimit = 8192;
const ULONGLONG kTreeCompareBudgetMs = 8;
const UINT kMaximumPathBytes = 1039;
unsigned char gTreeBatchTimerTag = 0;
const UINT_PTR kTreeBatchTimerId = reinterpret_cast<UINT_PTR>(&gTreeBatchTimerTag);
unsigned char gTreeBatchSubclassTag = 0;
const UINT_PTR kTreeBatchSubclassId = reinterpret_cast<UINT_PTR>(&gTreeBatchSubclassTag);
std::atomic<bool> gHookInstalled(false);
bool gBatchLog = false;
UINT gQuietIntervalMs = 300;
unsigned gBatchMode = 2;
ULONGLONG gTimerArmedAt = 0;
UINT gTimerArmedDelayMs = 0;
ULONGLONG gLastTimerLateMs = 0;
unsigned long long gTimerDispatches = 0;
DWORD gExpectedCaller = 0x004F6A37;
DWORD gFolderAddSearchReturnSite = 0;
DWORD gFolderDeleteSearchReturnSite = 0;
HWND gTreeWindow = nullptr;
DWORD gTreeOwner = 0;
bool gTimerArmed = false;
UINT gRecoveryMessage = 0;
bool gRecoveryPosted = false;
bool gRescanning = false;
WatchedSyncWorker* gWorkerPool[kParentCapacity] = {};
bool gWorkerReplacementAttempted[kParentCapacity] = {};
DWORD gFolderAddEventCallSite = 0;
DWORD gFolderDeleteEventCallSite = 0;
bool gFolderAddEventHookInstalled = false;
bool gFolderDeleteEventHookInstalled = false;
thread_local DWORD gReplayEventAction = 0;
thread_local const std::wstring* gReplayEventPath = nullptr;
thread_local bool gReplayEventPending = false;

// Verification counters. They are diagnostic only: no user path is written,
// only a stable hash of the parent path, counts, timings and HRESULTs. Logging
// is off unless FileMoveTreeBatchLog=1 in the runtime ini or
// LEYEES_TREE_BATCH_LOG is set, so a normal run never touches the disk here.
DWORD TreeBatchPathKey(const std::wstring& path)
{
	DWORD hash = 2166136261u;
	for( size_t index = 0; index < path.size(); ++index )
	{
		hash ^= static_cast<DWORD>(path[index]);
		hash *= 16777619u;
	}
	return hash;
}

bool TreeBatchReadLogSetting()
{
	if( ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_LOG", nullptr, 0) ) return true;
	if( !gInjectedModule ) return false;
	char modulePath[MAX_PATH * 2] = {};
	if( !::GetModuleFileNameA(gInjectedModule, modulePath, _countof(modulePath)) ) return false;
	char* dot = strrchr(modulePath, '.');
	if( !dot ) return false;
	if( strcpy_s(dot, _countof(modulePath) - static_cast<size_t>(dot - modulePath), ".ini") != 0 )
		return false;
	return ::GetPrivateProfileIntA("InternalHook", "FileMoveTreeBatchLog", 0, modulePath) != 0;
}

void TreeBatchLog(const char* format, ...)
{
	if( !gBatchLog || !format || !gInjectedModule ) return;
	char path[MAX_PATH * 2] = {};
	if( !::GetModuleFileNameA(gInjectedModule, path, _countof(path)) ) return;
	char* dot = strrchr(path, '.');
	if( !dot ) return;
	if( strcpy_s(dot, _countof(path) - static_cast<size_t>(dot - path),
		"_tree_batch_log.txt") != 0 ) return;
	FILE* log = nullptr;
	if( fopen_s(&log, path, "a") != 0 || !log ) return;
	va_list arguments;
	va_start(arguments, format);
	vfprintf(log, format, arguments);
	va_end(arguments);
	fputc('\n', log);
	fclose(log);
}

struct FolderAction
{
	DWORD action = 0;
	std::wstring name;
	std::wstring fullPath;
	std::string encodedPath;
	HTREEITEM item = nullptr;
	DWORD node = 0;
	TreeNodePathIdentity identity = {};
};

struct TreeChildState
{
	std::wstring name;
	std::wstring path;
	UINT state = 0;
	TreeNodePathIdentity identity = {};
	unsigned depth = 0;
};

struct ParentSession
{
	std::wstring path;
	HTREEITEM item = nullptr;
	DWORD node = 0;
	TreeNodePathIdentity nodeIdentity = {};
	WatchedSyncWorker* worker = nullptr;
	unsigned workerSlot = 0xFFFFFFFFu;
	DWORD volume = 0;
	ULONGLONG directoryId = 0;
	ULONGLONG quietDeadline = 0;
	ULONGLONG reattachAfter = 0;
	UINT retryDelayMs = 0;
	unsigned retryCount = 0;
	bool workerReplacementAttempted = false;
	ULONGLONG retryAt = 0;
	std::vector<TreeChildState> shadowBaseline;
	std::unique_ptr<DirectorySnapshot::State> committedState;
	std::vector<TreeChildState> committedTreeBaseline;
	WatchedSyncWorker::Candidate pendingFolderCandidate;
	std::vector<FolderAction> pendingFolderActions;
	std::vector<TreeChildState> pendingTreeBaseline;
	UINT committedParentVisualState = 0;
	UINT pendingParentVisualState = 0;
	size_t nextFolderAction = 0;
	bool baselineReady = false;
	bool shadowBaselineReady = false;
	bool suppressionReady = false;
	bool dirty = false;
	bool refreshRequested = false;
	bool unavailable = false;
	bool hasPendingFolderCandidate = false;
	bool committedTreeBaselineReady = false;
	bool folderActionSuppressed = false;
	bool forceProviderRescan = false;
	// Flat-folder mode: Leeyes keeps a leaf directory node collapsed forever, so
	// an expanded-parent session can never attach. If the watcher proves the
	// folder has no subdirectories, only the action 2 inner search is suppressed
	// and no provider rescan is needed because the tree has no child nodes.
	bool flatCandidate = false;
	bool flatReady = false;
	ULONGLONG flatCommits = 0;
	// Verification counters (see TreeBatchLog).
	ULONGLONG suppressedAction2 = 0;
	ULONGLONG suppressedAdd = 0;
	ULONGLONG suppressedDelete = 0;
	ULONGLONG cycleAction2 = 0;
	ULONGLONG cycleAdd = 0;
	ULONGLONG cycleDelete = 0;
	ULONGLONG rescanCount = 0;
	ULONGLONG rescanTotalUs = 0;
	ULONGLONG rescanMaxUs = 0;
	ULONGLONG rescanLastUs = 0;
	ULONGLONG noRescanCount = 0;
	ULONGLONG sliceDispatches = 0;
	ULONGLONG sliceActions = 0;
	ULONGLONG commitCount = 0;
	unsigned baselineCaptureAttempts = 0;
	HRESULT lastShellError = S_OK;
	HRESULT lastRescanFailure = S_OK;
};

std::vector<ParentSession> gParents;

// Negative attach results are cached briefly. A bulk move can deliver thousands
// of notifications for a parent that cannot be batched (collapsed or leaf node);
// walking the tree for each of them would be slower than Leeyes' original path.
struct AttachFailure
{
	DWORD pathKey = 0;
	ULONGLONG retryAfter = 0;
};
const size_t kAttachFailureCapacity = 8;
const ULONGLONG kAttachFailureWindowMs = 1000;
std::vector<AttachFailure> gAttachFailures;

bool TreeBatchAttachRecentlyFailed(DWORD pathKey)
{
	const ULONGLONG now = ::GetTickCount64();
	for( auto it = gAttachFailures.begin(); it != gAttachFailures.end(); )
	{
		if( it->pathKey == pathKey && now < it->retryAfter ) return true;
		if( now >= it->retryAfter ) it = gAttachFailures.erase(it);
		else ++it;
	}
	return false;
}

void TreeBatchRememberAttachFailure(DWORD pathKey)
{
	const ULONGLONG now = ::GetTickCount64();
	for( auto& failure : gAttachFailures )
		if( failure.pathKey == pathKey )
		{
			failure.retryAfter = now + kAttachFailureWindowMs;
			return;
		}
	if( gAttachFailures.size() >= kAttachFailureCapacity )
	{
		// Drop the entry that expires first.
		size_t oldest = 0;
		for( size_t i = 1; i < gAttachFailures.size(); ++i )
			if( gAttachFailures[i].retryAfter < gAttachFailures[oldest].retryAfter ) oldest = i;
		gAttachFailures.erase(gAttachFailures.begin() + oldest);
	}
	try { gAttachFailures.push_back({ pathKey, now + kAttachFailureWindowMs }); }
	catch( ... ) {}
}

void TreeBatchClearAttachFailure(DWORD pathKey)
{
	for( auto it = gAttachFailures.begin(); it != gAttachFailures.end(); )
	{
		if( it->pathKey == pathKey ) it = gAttachFailures.erase(it);
		else ++it;
	}
}

LRESULT CALLBACK TreeNotificationBatchSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference);
void TreeBatchMarkUnavailable(ParentSession& parent, const char* reason);
void TreeBatchEmergencyRescanOrDisable(ParentSession& parent, DWORD owner, HWND window, const char* reason);
bool TreeBatchProviderRescan(DWORD owner, ParentSession& parent);

void TreeBatchClearPendingFolderState(ParentSession& parent)
{
	parent.pendingFolderCandidate = WatchedSyncWorker::Candidate();
	parent.pendingFolderActions.clear();
	parent.pendingTreeBaseline.clear();
	parent.pendingParentVisualState = 0;
	parent.nextFolderAction = 0;
	parent.hasPendingFolderCandidate = false;
}

void TreeBatchDiscardPendingFolderState(ParentSession& parent, bool requireProviderRescan)
{
	if( parent.hasPendingFolderCandidate && parent.worker )
	{
		bool current = false;
		parent.worker->Release(parent.pendingFolderCandidate, current);
	}
	TreeBatchClearPendingFolderState(parent);
	if( requireProviderRescan ) parent.forceProviderRescan = true;
}

bool SamePath(const std::wstring& left, const std::wstring& right)
{
	return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
		right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool TreeBatchFixedLocalPath(const std::wstring& path)
{
	if( path.size() < 3 || path.size() >= static_cast<size_t>(MAX_PATH) * 4 || path[1] != L':'
		|| (path[2] != L'\\' && path[2] != L'/') ) return false;
	wchar_t root[] = { path[0], L':', L'\\', L'\0' };
	if( ::GetDriveTypeW(root) != DRIVE_FIXED ) return false;
	const DWORD rootAttributes = ::GetFileAttributesW(root);
	if( rootAttributes == INVALID_FILE_ATTRIBUTES || !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY)
		|| (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
	size_t end = path.size();
	while( end > 3 && (path[end - 1] == L'\\' || path[end - 1] == L'/') ) --end;
	for( size_t i = 3; i <= end; ++i )
	{
		if( i != end && path[i] != L'\\' && path[i] != L'/' ) continue;
		if( i <= 3 ) continue;
		std::wstring component;
		try { component.assign(path.data(), i); }
		catch( ... ) { return false; }
		const DWORD attributes = ::GetFileAttributesW(component.c_str());
		if( attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
			|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
	}
	return true;
}

bool TreeBatchSplitParent(const std::wstring& fullPath, std::wstring& parent)
{
	const size_t separator = fullPath.find_last_of(L"\\/");
	if( separator == std::wstring::npos || separator == 0 ) return false;
	size_t length = separator;
	if( length == 2 && fullPath[1] == L':' ) ++length;
	if( length < 3 || length >= static_cast<size_t>(MAX_PATH) * 4 ) return false;
	try { parent.assign(fullPath.data(), length); }
	catch( ... ) { return false; }
	if( !TreeBatchFixedLocalPath(parent) ) return false;
	std::string encoded;
	return EncodeTreePathExact(CP_ACP, parent, encoded, kMaximumPathBytes);
}

bool TreeBatchReadAnsiPath(DWORD pathPointer, std::wstring& fullPath)
{
	if( !pathPointer ) return false;
	char encoded[kMaximumPathBytes] = {};
	size_t length = 0;
	__try
	{
		const char* source = reinterpret_cast<const char*>(pathPointer);
		for( ; length < _countof(encoded) - 1; ++length )
		{
			encoded[length] = source[length];
			if( encoded[length] == '\0' ) break;
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	if( !length || length >= _countof(encoded) - 1 ) return false;
	return DecodeTreePathExact(CP_ACP, encoded, length, fullPath);
}

bool TreeBatchReadOwner(DWORD frame, DWORD& owner)
{
	if( frame < sizeof(DWORD) * 2 ) return false;
	__try { owner = *reinterpret_cast<const DWORD*>(frame - 2 * sizeof(DWORD)); }
	__except( EXCEPTION_EXECUTE_HANDLER ) { owner = 0; return false; }
	return owner != 0;
}

bool TreeBatchNodeIdentityEqual(const TreeNodePathIdentity& left, const TreeNodePathIdentity& right)
{
	if( left.data != right.data || left.vmt != right.vmt || left.parent != right.parent ) return false;
	// A node whose path cache is not populated yet (unexpanded child) can still
	// be compared by its node data, vtable and parent pointers.
	// A cache that became populated later must not break the identity: the
	// node data, vtable and parent are unchanged, and any caller that needs the
	// path verifies it separately.
	if( !left.pathKnown || !right.pathKnown ) return true;
	return left.cachedPath == right.cachedPath && left.pathLength == right.pathLength
		&& left.pathHash == right.pathHash;
}

// True when the cached path is absent (Leeyes has not populated it yet) or it
// matches the expected filesystem path exactly.
bool TreeBatchPathAcceptable(const TreeNodePathIdentity& identity, const std::wstring& expected)
{
	if( !identity.pathKnown ) return true;
	return CachedTreePathMatches(CP_ACP, identity.cachedPath, expected);
}

bool TreeBatchExactPath(const std::wstring& left, const std::wstring& right)
{
	return left.size() == right.size()
		&& ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), FALSE) == CSTR_EQUAL;
}

void TreeBatchNoteFolderActionSuppressed(ParentSession& parent)
{
	parent.folderActionSuppressed = true;
}

extern "C" BOOL __cdecl TreeBatchReplayEventMatches(DWORD action, DWORD pathPointer)
{
	if( !gReplayEventPending || action != gReplayEventAction || !gReplayEventPath ) return FALSE;
	try
	{
		std::wstring observed;
		if( !TreeBatchReadAnsiPath(pathPointer, observed)
			|| !TreeBatchExactPath(observed, *gReplayEventPath) ) return FALSE;
		gReplayEventPending = false;
		return TRUE;
	}
	catch( ... ) { return FALSE; }
}

extern "C" __declspec(naked) void TreeBatchAddEventHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 40]
		push 3
		call TreeBatchReplayEventMatches
		add esp, 8
		test eax, eax
		jnz skip_add_event
		popad
		popfd
		push dword ptr [gLeeyesTreeBatchAddEventContinuation]
		jmp dword ptr [ebx + 3D0h]
	skip_add_event:
		popad
		popfd
		add esp, 8
		jmp dword ptr [gLeeyesTreeBatchAddEventContinuation]
	}
}

extern "C" __declspec(naked) void TreeBatchDeleteEventHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 40]
		push 5
		call TreeBatchReplayEventMatches
		add esp, 8
		test eax, eax
		jnz skip_delete_event
		popad
		popfd
		push dword ptr [gLeeyesTreeBatchDeleteEventContinuation]
		jmp dword ptr [ebx + 3D0h]
	skip_delete_event:
		popad
		popfd
		add esp, 8
		jmp dword ptr [gLeeyesTreeBatchDeleteEventContinuation]
	}
}

bool TreeBatchPatchOwnerEvent(DWORD site, void* hook, DWORD& continuation)
{
	const BYTE expected[] = { 0xFF,0x93,0xD0,0x03,0x00,0x00 };
	if( !site || !hook || memcmp(reinterpret_cast<const void*>(site), expected, sizeof(expected)) != 0 ) return false;
	const LONGLONG relative = static_cast<LONGLONG>(reinterpret_cast<ULONG_PTR>(hook))
		- static_cast<LONGLONG>(site + 5);
	if( relative < (std::numeric_limits<LONG>::min)()
		|| relative > (std::numeric_limits<LONG>::max)() ) return false;
	BYTE patch[sizeof(expected)] = { 0xE9,0,0,0,0,0x90 };
	const LONG displacement = static_cast<LONG>(relative);
	memcpy(patch + 1, &displacement, sizeof(displacement));
	DWORD oldProtection = 0;
	if( !::VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), PAGE_EXECUTE_READWRITE, &oldProtection) ) return false;
	continuation = site + sizeof(patch);
	memcpy(reinterpret_cast<void*>(site), patch, sizeof(patch));
	::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(patch));
	DWORD ignored = 0;
	::VirtualProtect(reinterpret_cast<void*>(site), sizeof(patch), oldProtection, &ignored);
	return true;
}

bool TreeBatchRestoreOwnerEvent(DWORD site, DWORD continuation)
{
	const BYTE original[] = { 0xFF,0x93,0xD0,0x03,0x00,0x00 };
	if( !site || continuation != site + sizeof(original) ) return false;
	DWORD oldProtection = 0;
	if( !::VirtualProtect(reinterpret_cast<void*>(site), sizeof(original), PAGE_EXECUTE_READWRITE, &oldProtection) ) return false;
	memcpy(reinterpret_cast<void*>(site), original, sizeof(original));
	::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(site), sizeof(original));
	DWORD ignored = 0;
	::VirtualProtect(reinterpret_cast<void*>(site), sizeof(original), oldProtection, &ignored);
	return true;
}

bool TreeBatchInstallFolderEventHooks()
{
	if( gFolderAddEventHookInstalled && gFolderDeleteEventHookInstalled ) return true;
	DWORD addSite = 0, deleteSite = 0;
	if( !GetLeeyesFolderOwnerEventCallSites(addSite, deleteSite) ) return false;
	if( !gFolderAddEventHookInstalled )
	{
		if( !TreeBatchPatchOwnerEvent(addSite, reinterpret_cast<void*>(&TreeBatchAddEventHook),
			gLeeyesTreeBatchAddEventContinuation) ) return false;
		gFolderAddEventCallSite = addSite;
		gFolderAddEventHookInstalled = true;
	}
	if( !gFolderDeleteEventHookInstalled )
	{
		if( !TreeBatchPatchOwnerEvent(deleteSite, reinterpret_cast<void*>(&TreeBatchDeleteEventHook),
			gLeeyesTreeBatchDeleteEventContinuation) ) return false;
		gFolderDeleteEventCallSite = deleteSite;
		gFolderDeleteEventHookInstalled = true;
	}
	return gFolderAddEventHookInstalled && gFolderDeleteEventHookInstalled;
}

void TreeBatchRemoveFolderEventHooks()
{
	if( gFolderDeleteEventHookInstalled
		&& TreeBatchRestoreOwnerEvent(gFolderDeleteEventCallSite, gLeeyesTreeBatchDeleteEventContinuation) )
	{
		gLeeyesTreeBatchDeleteEventContinuation = 0;
		gFolderDeleteEventCallSite = 0;
		gFolderDeleteEventHookInstalled = false;
	}
	if( gFolderAddEventHookInstalled
		&& TreeBatchRestoreOwnerEvent(gFolderAddEventCallSite, gLeeyesTreeBatchAddEventContinuation) )
	{
		gLeeyesTreeBatchAddEventContinuation = 0;
		gFolderAddEventCallSite = 0;
		gFolderAddEventHookInstalled = false;
	}
}

bool TreeBatchParentState(HWND window, DWORD owner, const ParentSession& parent, bool verifyOwner,
	bool& expanded)
{
	expanded = false;
	if( !window || !::IsWindow(window) || !owner
		|| (verifyOwner && !TreeOwnerContainsWindow(owner, window))
		|| !parent.item || !parent.node ) return false;
	TVITEMW item = {};
	item.mask = TVIF_PARAM | TVIF_STATE;
	item.stateMask = TVIS_EXPANDED;
	item.hItem = parent.item;
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
		|| static_cast<DWORD>(item.lParam) != parent.node ) return false;
	TreeNodePathIdentity observed = {};
	if( !ReadTreeNodePathIdentity(parent.node, observed, true)
		|| !TreeBatchNodeIdentityEqual(observed, parent.nodeIdentity)
		|| !TreeBatchPathAcceptable(observed, parent.path) ) return false;
	expanded = (item.state & TVIS_EXPANDED) != 0;
	return true;
}

bool TreeBatchParentCurrent(HWND window, DWORD owner, const ParentSession& parent, bool verifyOwner)
{
	bool expanded = false;
	return TreeBatchParentState(window, owner, parent, verifyOwner, expanded) && expanded;
}

bool TreeBatchParentFlatCurrent(HWND window, DWORD owner, const ParentSession& parent, bool verifyOwner)
{
	bool expanded = false;
	return TreeBatchParentState(window, owner, parent, verifyOwner, expanded) && !expanded;
}

// Identity check used by the notification hooks: a flat session is valid while
// the node stays collapsed, a normal session while it stays expanded.
bool TreeBatchParentUsable(HWND window, DWORD owner, const ParentSession& parent, bool verifyOwner)
{
	if( parent.flatCandidate ) return TreeBatchParentFlatCurrent(window, owner, parent, verifyOwner);
	return TreeBatchParentCurrent(window, owner, parent, verifyOwner);
}

bool TreeBatchParentVisualState(HWND window, const ParentSession& parent, UINT& visualState)
{
	visualState = 0;
	if( !window || !parent.item ) return false;
	TVITEMW item = {};
	item.mask = TVIF_STATE;
	item.stateMask = TVIS_EXPANDED | TVIS_SELECTED;
	item.hItem = parent.item;
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) return false;
	visualState = item.state & (TVIS_EXPANDED | TVIS_SELECTED);
	return true;
}

bool TreeBatchParentVisualStateMatches(HWND window, const ParentSession& parent, UINT expected)
{
	UINT current = 0;
	return TreeBatchParentVisualState(window, parent, current) && current == expected;
}

bool TreeBatchSnapshotSupported(const DirectorySnapshot::State& state)
{
	// DirectorySnapshot::Read and SnapshotWire::Decode validate every name and
	// derive this flag while already walking the entries. Keep this UI-side gate
	// constant-time even for large folders.
	return state.volume != 0 && state.directoryId != 0 && !state.containsReparsePoint;
}

bool TreeBatchStartCompareBudget(LARGE_INTEGER& started, LONGLONG& budgetTicks)
{
	LARGE_INTEGER frequency = {};
	if( !::QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0
		|| !::QueryPerformanceCounter(&started) ) return false;
	budgetTicks = frequency.QuadPart / 1000 * static_cast<LONGLONG>(kTreeCompareBudgetMs);
	return budgetTicks > 0;
}

bool TreeBatchCompareBudgetExceeded(const LARGE_INTEGER& started, LONGLONG budgetTicks)
{
	LARGE_INTEGER now = {};
	return !::QueryPerformanceCounter(&now) || now.QuadPart - started.QuadPart > budgetTicks;
}

bool TreeBatchCaptureVisibleSubtree(HWND window, const ParentSession& parent, std::vector<TreeChildState>& children,
	unsigned& selectedCount, unsigned& expandedCount, size_t maximumChildren)
{
	children.clear();
	selectedCount = 0;
	expandedCount = 0;
	if( !window || !::IsWindow(window) || !parent.item || parent.path.empty() ) return false;
	try
	{
		LARGE_INTEGER started = {};
		LONGLONG budgetTicks = 0;
		if( !TreeBatchStartCompareBudget(started, budgetTicks) ) return false;
		struct PendingParent { HTREEITEM item; std::wstring path; unsigned depth; };
		std::vector<PendingParent> pending;
		pending.push_back({ parent.item, parent.path, 0 });
		while( !pending.empty() )
		{
			PendingParent current = std::move(pending.back());
			pending.pop_back();
			HTREEITEM item = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
				TVGN_CHILD, reinterpret_cast<LPARAM>(current.item)));
			while( item )
			{
				if( children.size() >= maximumChildren || current.depth >= 256 ) return false;
				if( (children.size() & 31) == 0
					&& TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
				wchar_t text[1024] = {};
				TVITEMW data = {};
				data.mask = TVIF_TEXT | TVIF_STATE | TVIF_PARAM;
				data.stateMask = TVIS_SELECTED | TVIS_EXPANDED;
				data.hItem = item;
				data.pszText = text;
				data.cchTextMax = _countof(text);
				if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&data)) ) return false;
				if( text[0] == L'\0' || wcsnlen_s(text, _countof(text)) == _countof(text) - 1
					|| !data.lParam ) return false;
				TreeChildState child;
				child.name.assign(text);
				child.path = current.path;
				if( child.path.back() != L'\\' && child.path.back() != L'/' ) child.path.push_back(L'\\');
				child.path += child.name;
				child.state = data.state & (TVIS_SELECTED | TVIS_EXPANDED);
				const DWORD node = static_cast<DWORD>(data.lParam);
				child.depth = current.depth + 1;
				if( !ReadTreeNodePathIdentity(node, child.identity, true)
					|| !TreeBatchPathAcceptable(child.identity, child.path) ) return false;
				if( data.state & TVIS_SELECTED ) ++selectedCount;
				if( data.state & TVIS_EXPANDED ) ++expandedCount;
				if( data.state & TVIS_EXPANDED ) pending.push_back({ item, child.path, child.depth });
				children.emplace_back(std::move(child));
				item = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_NEXT, reinterpret_cast<LPARAM>(item)));
			}
		}
		if( TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
	}
	catch( ... ) { children.clear(); return false; }
	return true;
}

bool TreeBatchEnsureShadowBaseline(HWND window, DWORD owner, ParentSession& parent,
	const WatchedSyncWorker::Candidate& candidate)
{
	if( parent.shadowBaselineReady ) return true;
	std::vector<TreeChildState> baseline;
	unsigned selectedCount = 0, expandedCount = 0;
	if( !TreeBatchCaptureVisibleSubtree(window, parent, baseline, selectedCount, expandedCount,
		kShadowChildLimit) || !parent.worker || !parent.worker->CanApply(candidate)
		|| !TreeBatchParentCurrent(window, owner, parent, true) ) return false;
	auto less = [](const TreeChildState& left, const TreeChildState& right)
	{
		return ::CompareStringOrdinal(left.path.data(), static_cast<int>(left.path.size()),
			right.path.data(), static_cast<int>(right.path.size()), FALSE) == CSTR_LESS_THAN;
	};
	std::sort(baseline.begin(), baseline.end(), less);
	parent.shadowBaseline.swap(baseline);
	parent.shadowBaselineReady = true;
	return true;
}

bool TreeBatchCompareSnapshot(HWND window, const ParentSession& parent,
	const DirectorySnapshot::State& state, unsigned& missing, unsigned& unexpected,
	unsigned& selectedCount, unsigned& expandedCount, unsigned& selectionChanges,
	unsigned& expansionChanges, unsigned& identityChanges,
	unsigned& subtreeAdded, unsigned& subtreeRemoved)
{
	missing = unexpected = selectedCount = expandedCount = selectionChanges = expansionChanges = identityChanges = 0;
	subtreeAdded = subtreeRemoved = 0;
	if( state.entries.size() > kShadowChildLimit ) return false;
	std::vector<std::wstring> expected;
	std::vector<std::wstring> actualDirect;
	std::vector<TreeChildState> actual;
	try
	{
		expected.reserve(state.entries.size());
		actualDirect.reserve(state.entries.size());
		// Ordinary images are list entries, not TreeView children. Every current
		// directory must exist in the tree; visible files (for example archives)
		// are checked in the other direction against the complete snapshot.
		for( const auto& entry : state.entries )
			if( entry.attributes & FILE_ATTRIBUTE_DIRECTORY ) expected.push_back(entry.name);
	}
	catch( ... ) { return false; }
	if( !TreeBatchCaptureVisibleSubtree(window, parent, actual, selectedCount, expandedCount,
		kShadowChildLimit) ) return false;
	try
	{
		for( const auto& child : actual ) if( child.depth == 1 ) actualDirect.push_back(child.name);
	}
	catch( ... ) { return false; }
	auto less = [](const std::wstring& left, const std::wstring& right)
	{
		return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), FALSE) == CSTR_LESS_THAN;
	};
	std::sort(expected.begin(), expected.end(), less);
	std::sort(actualDirect.begin(), actualDirect.end(), less);
	size_t i = 0, j = 0;
	while( i < expected.size() || j < actualDirect.size() )
	{
		if( j == actualDirect.size() || (i < expected.size() && less(expected[i], actualDirect[j])) )
		{
			++missing;
			++i;
		}
		else if( i == expected.size() || less(actualDirect[j], expected[i]) )
		{
			const auto found = std::lower_bound(state.entries.begin(), state.entries.end(), actualDirect[j],
				[](const DirectorySnapshot::Entry& entry, const std::wstring& name)
				{ return entry.name < name; });
			if( found == state.entries.end() || found->name != actualDirect[j] ) ++unexpected;
			++j;
		}
		else
		{
			++i;
			++j;
		}
	}
	if( parent.shadowBaselineReady )
	{
		auto pathLess = [](const TreeChildState& left, const TreeChildState& right)
			{ return ::CompareStringOrdinal(left.path.data(), static_cast<int>(left.path.size()),
				right.path.data(), static_cast<int>(right.path.size()), FALSE) == CSTR_LESS_THAN; };
		std::sort(actual.begin(), actual.end(), pathLess);
		i = j = 0;
		while( i < parent.shadowBaseline.size() && j < actual.size() )
		{
			const TreeChildState& before = parent.shadowBaseline[i];
			const TreeChildState& after = actual[j];
			if( pathLess(before, after) ) { ++subtreeRemoved; ++i; }
			else if( pathLess(after, before) ) { ++subtreeAdded; ++j; }
			else
			{
				if( !TreeBatchNodeIdentityEqual(before.identity, after.identity) ) ++identityChanges;
				if( (before.state & TVIS_SELECTED) != (after.state & TVIS_SELECTED) ) ++selectionChanges;
				if( (before.state & TVIS_EXPANDED) != (after.state & TVIS_EXPANDED) ) ++expansionChanges;
				++i;
				++j;
			}
		}
		subtreeRemoved += static_cast<unsigned>(parent.shadowBaseline.size() - i);
		subtreeAdded += static_cast<unsigned>(actual.size() - j);
	}
	return true;
}

void TreeBatchReportShadow(const ParentSession& parent, bool comparable, unsigned missing,
	unsigned unexpected, unsigned selectedCount, unsigned expandedCount,
	unsigned selectionChanges, unsigned expansionChanges, unsigned identityChanges,
	unsigned subtreeAdded, unsigned subtreeRemoved)
{
	char detail[320] = {};
	sprintf_s(detail, _countof(detail), "parent_hash=%08lX comparable=%u missing=%u unexpected=%u subtree_added=%u subtree_removed=%u selected=%u expanded=%u identity_changes=%u selection_changes=%u expansion_changes=%u",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), comparable ? 1u : 0u,
		missing, unexpected, subtreeAdded, subtreeRemoved, selectedCount, expandedCount,
		identityChanges, selectionChanges, expansionChanges);
	StartupDiagnosticLog("tree_batch_shadow", detail);
	::OutputDebugStringA("Leeyes tree batch shadow: ");
	::OutputDebugStringA(detail);
	::OutputDebugStringA("\n");
}

bool TreeBatchLauncher(std::wstring& launcher)
{
	HMODULE module = nullptr;
	wchar_t path[MAX_PATH * 4] = {};
	if( !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
		reinterpret_cast<LPCWSTR>(&InstallTreeNotificationBatch), &module) ) return false;
	const DWORD length = ::GetModuleFileNameW(module, path, _countof(path));
	if( !length || length >= _countof(path) ) return false;
	wchar_t* slash = wcsrchr(path, L'\\');
	if( !slash || wcscpy_s(slash + 1, _countof(path) - static_cast<size_t>(slash + 1 - path), L"LeeyesW.exe") ) return false;
	try { launcher.assign(path); return true; }
	catch( ... ) { return false; }
}

bool TreeBatchArmTimer(HWND window, UINT delay)
{
	if( gTimerArmed ) return true;
	const UINT effective = delay ? delay : 1;
	gTimerArmed = window && ::SetTimer(window, kTreeBatchTimerId, effective, nullptr) != 0;
	if( gTimerArmed )
	{
		gTimerArmedAt = ::GetTickCount64();
		gTimerArmedDelayMs = effective;
	}
	return gTimerArmed;
}

bool TreeBatchProviderRescan(DWORD owner, ParentSession& parent)
{
	LARGE_INTEGER frequency = {};
	LARGE_INTEGER started = {};
	LARGE_INTEGER finished = {};
	const bool timed = ::QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0
		&& ::QueryPerformanceCounter(&started);
	HRESULT failure = S_OK;
	gRescanning = true;
	const bool rescanned = InvokeTreeProviderRescan(owner, parent.node, failure);
	gRescanning = false;
	ULONGLONG microseconds = 0;
	if( timed && ::QueryPerformanceCounter(&finished) && finished.QuadPart >= started.QuadPart )
		microseconds = static_cast<ULONGLONG>((finished.QuadPart - started.QuadPart) * 1000000
			/ frequency.QuadPart);
	++parent.rescanCount;
	parent.rescanLastUs = microseconds;
	parent.rescanTotalUs += microseconds;
	if( microseconds > parent.rescanMaxUs ) parent.rescanMaxUs = microseconds;
	parent.lastShellError = failure;
	parent.lastRescanFailure = failure;
	TreeBatchLog("tree_batch_rescan parent_hash=%08lX node=%08lX ok=%u shell_error=0x%08lX duration_us=%llu count=%llu total_us=%llu max_us=%llu",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), static_cast<unsigned long>(parent.node),
		rescanned ? 1u : 0u, static_cast<unsigned long>(failure), microseconds, parent.rescanCount,
		parent.rescanTotalUs, parent.rescanMaxUs);
	return rescanned;
}

bool TreeBatchPostRecovery(HWND window)
{
	if( gRecoveryPosted ) return true;
	if( !gRecoveryMessage || !window || !::IsWindow(window)
		|| !::PostMessageW(window, gRecoveryMessage, 0, 0) ) return false;
	gRecoveryPosted = true;
	return true;
}

void TreeBatchReleaseTarget(ParentSession& parent)
{
	if( parent.worker )
	{
		try { parent.worker->SetTarget(L""); }
		catch( ... ) {}
	}
}

void TreeBatchCloseWindow(HWND window)
{
	if( window && gTimerArmed ) ::KillTimer(window, kTreeBatchTimerId);
	gTimerArmed = false;
	gRecoveryPosted = false;
	for( auto& parent : gParents )
	{
		TreeBatchDiscardPendingFolderState(parent, true);
		TreeBatchReleaseTarget(parent);
	}
	gParents.clear();
	gTreeWindow = nullptr;
	gTreeOwner = 0;
}

bool TreeBatchAttachParent(DWORD owner, HWND window, const std::wstring& path)
{
	const DWORD pathKey = TreeBatchPathKey(path);
	if( TreeBatchAttachRecentlyFailed(pathKey) ) return false;
	const ULONGLONG attachStarted = ::GetTickCount64();
	const auto fail = [pathKey, attachStarted](const char* reason)
	{
		TreeBatchRememberAttachFailure(pathKey);
		TreeBatchLog("tree_batch_attach_failed parent_key=%08lX reason=%s elapsed_ms=%llu tick_ms=%llu",
			static_cast<unsigned long>(pathKey), reason,
			::GetTickCount64() - attachStarted, ::GetTickCount64());
		return false;
	};
	if( !owner || !window || !::IsWindow(window) || !TreeOwnerContainsWindow(owner, window) )
		return fail("owner_window");
	if( !TreeBatchFixedLocalPath(path) ) return fail("path_scope");
	std::string encoded;
	if( !EncodeTreePathExact(CP_ACP, path, encoded, kMaximumPathBytes) ) return fail("path_encode");
	if( !gParents.empty() && (gTreeWindow != window || gTreeOwner != owner) ) return fail("owner_changed");
	ParentSession* reuse = nullptr;
	for( auto& existing : gParents )
	{
		if( SamePath(existing.path, path) )
		{
			if( !existing.unavailable ) return true;
			reuse = &existing;
			break;
		}
	}
	if( !reuse && gParents.size() >= kParentCapacity )
		for( auto& existing : gParents ) if( existing.unavailable ) { reuse = &existing; break; }
	if( !reuse && gParents.size() >= kParentCapacity ) return fail("parent_capacity");
	if( reuse )
	{
		const ULONGLONG now = ::GetTickCount64();
		if( reuse->reattachAfter > now ) return fail("reattach_throttled");
		// A missing/collapsed node or a failed worker must not trigger a full
		// TreeView path search for every file notification in a large batch.
		reuse->reattachAfter = now + 1000;
	}
	const bool firstParent = gParents.empty();
	HTREEITEM item = nullptr;
	DWORD node = 0;
	// A cold attach walks the whole visible tree once. The per-notification
	// checks keep the 8ms comparison budget, but the attach search must be able
	// to reach a parent that sits below a large expanded drive tree. It is
	// bounded by time and node count, and only runs while a slot is still free.
	bool flatCandidate = false;
	bool nodeExpanded = false;
	const ULONGLONG searchStarted = ::GetTickCount64();
	bool found = FindExpandedTreeNodeByPath(window, CP_ACP, path, 50, 100000, item, node);
	if( !found && gBatchMode == 2
		&& FindExpandedTreeNodeByPath(window, CP_ACP, path, 50, 100000, item, node, true, &nodeExpanded) )
	{
		// A leaf directory node never becomes expanded in Leeyes. Accept it as a
		// flat candidate; the first snapshot decides whether the folder really has
		// no subdirectories and can therefore be batched without a rescan.
		flatCandidate = !nodeExpanded;
		found = true;
	}
	if( !found )
	{
		// Some nodes have no cached path yet even though they are present. Locate
		// them through the closest ancestor with a known path and exact child names.
		bool componentExpanded = false;
		if( FindTreeNodeByPathComponents(window, CP_ACP, path, 50, 100000, item, node,
			componentExpanded) )
		{
			found = true;
			flatCandidate = !componentExpanded;
		}
	}
	if( !found )
	{
		std::string probeEncoded;
		const bool encodeOk = EncodeTreePathExact(CP_ACP, path, probeEncoded, kMaximumPathBytes);
		HTREEITEM probeRoot = reinterpret_cast<HTREEITEM>(::SendMessageW(window,
			TVM_GETNEXTITEM, TVGN_ROOT, 0));
		bool rootItemOk = false;
		if( probeRoot )
		{
			TVITEMW probeItem = {};
			probeItem.mask = TVIF_PARAM | TVIF_STATE;
			probeItem.stateMask = TVIS_EXPANDED;
			probeItem.hItem = probeRoot;
			rootItemOk = ::SendMessageW(window, TVM_GETITEMW, 0,
				reinterpret_cast<LPARAM>(&probeItem)) != 0 && probeItem.lParam != 0;
		}
		DWORD expectedHash = 2166136261u;
		for( size_t i = 0; i < probeEncoded.size(); ++i )
			expectedHash = (expectedHash ^ static_cast<unsigned char>(probeEncoded[i])) * 16777619u;
		TreeBatchLog("tree_batch_attach_search_failed parent_key=%08lX search_ms=%llu path_length=%u encode=%u root=%u root_item=%u expected_hash=%08lX expected_bytes=%u visited=%llu path_nodes=%llu item_failures=%llu verify_failed=%u tick_ms=%llu",
			static_cast<unsigned long>(pathKey), ::GetTickCount64() - searchStarted,
			static_cast<unsigned>(path.size()), encodeOk ? 1u : 0u,
			probeRoot ? 1u : 0u, rootItemOk ? 1u : 0u,
			static_cast<unsigned long>(expectedHash), static_cast<unsigned>(probeEncoded.size()),
			gTreePathSearchVisited, gTreePathSearchPathNodes,
			gTreePathSearchItemFailures, gTreePathSearchVerifyFailed ? 1u : 0u,
			::GetTickCount64());
		TreeBatchLog("tree_batch_attach_component_failed parent_key=%08lX known_paths=%llu best_prefix=%llu",
			static_cast<unsigned long>(pathKey), gTreePathSearchKnownPaths, gTreePathSearchBestPrefix);
		return fail("node_not_found");
	}
	ParentSession parent;
	parent.path = path;
	parent.item = item;
	parent.node = node;
	parent.flatCandidate = flatCandidate;
	if( flatCandidate )
	{
		parent.dirty = true;
		parent.refreshRequested = false;
		parent.quietDeadline = 0;
	}
	// Reusing an unavailable slot means earlier notices may have been suppressed
	// against a session whose node or watcher could no longer be trusted. Keep
	// forwarding notifications and require a fresh provider rescan before this
	// replacement session is allowed to suppress any more tree searches.
	if( reuse )
	{
		parent.dirty = true;
		parent.quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
		parent.forceProviderRescan = true;
	}
	if( !ReadTreeNodePathIdentity(node, parent.nodeIdentity, true) ) return fail("node_identity");
	TreeNodePathIdentity current = {};
	if( !ReadTreeNodePathIdentity(node, current, true) || !TreeBatchNodeIdentityEqual(current, parent.nodeIdentity)
		|| !TreeBatchPathAcceptable(current, path) ) return fail("node_memory");
	unsigned slot = reuse ? reuse->workerSlot : 0xFFFFFFFFu;
	if( !reuse ) for( unsigned i = 0; i < kParentCapacity; ++i )
	{
		bool used = false;
		for( const auto& existing : gParents ) if( existing.workerSlot == i ) used = true;
		if( !used ) { slot = i; break; }
	}
	if( slot >= kParentCapacity ) return fail("worker_slot");
	WatchedSyncWorker* worker = gWorkerPool[slot];
	if( worker && worker->Failed() )
	{
		if( !worker->TerminationConfirmed() || gWorkerReplacementAttempted[slot] )
			return fail("worker_failed_replacement_used");
		gWorkerReplacementAttempted[slot] = true;
		worker = nullptr;
	}
	if( !worker )
	{
		std::wstring launcher;
		if( !TreeBatchLauncher(launcher) ) return fail("launcher_missing");
		worker = new (std::nothrow) WatchedSyncWorker(launcher, 5000, false, false);
		if( !worker || !worker->Start() ) { delete worker; return fail("worker_start"); }
		gWorkerPool[slot] = worker;
	}
	const std::uint64_t target = worker->SetTarget(path);
	if( !target ) return fail("worker_target");
	parent.worker = worker;
	parent.workerSlot = slot;
	ParentSession* attached = nullptr;
	if( reuse )
	{
		try { *reuse = std::move(parent); attached = reuse; }
		catch( ... ) { try { worker->SetTarget(L""); } catch( ... ) {} return fail("slot_assign"); }
	}
	else
	{
		try { gParents.emplace_back(std::move(parent)); attached = &gParents.back(); }
		catch( ... ) { try { worker->SetTarget(L""); } catch( ... ) {} return fail("parent_append"); }
	}
	if( firstParent )
	{
		if( !::SetWindowSubclass(window, TreeNotificationBatchSubclass, kTreeBatchSubclassId, owner) )
		{
			TreeBatchReleaseTarget(*attached);
			gParents.pop_back();
			return fail("subclass");
		}
		gTreeWindow = window;
		gTreeOwner = owner;
	}
	if( !TreeBatchArmTimer(window, 100) )
	{
		// If an unavailable slot had already suppressed events, queue recovery
		// instead of synchronously re-entering Leeyes from this hook.
		if( TreeBatchPostRecovery(window) ) return fail("timer_arm_recovery_posted");
		TreeBatchReleaseTarget(*attached);
		if( reuse )
		{
			attached->unavailable = true;
			attached->suppressionReady = false;
		}
		else gParents.pop_back();
		if( gParents.empty() && gTreeWindow == window )
		{
			::RemoveWindowSubclass(window, TreeNotificationBatchSubclass, kTreeBatchSubclassId);
			gTreeWindow = nullptr;
			gTreeOwner = 0;
		}
		return fail("timer_arm");
	}
	TreeBatchClearAttachFailure(pathKey);
	TreeBatchLog("tree_batch_parent_attached parent_key=%08lX slot=%u reuse=%u parents=%u quiet_ms=%u mode=%u",
		static_cast<unsigned long>(pathKey), slot, reuse ? 1u : 0u,
		static_cast<unsigned>(gParents.size()), gQuietIntervalMs, gBatchMode);
	return true;
}

void TreeBatchRequireCatchup(const std::wstring& path)
{
	for( auto& parent : gParents ) if( SamePath(parent.path, path) )
	{
		parent.dirty = true;
		parent.suppressionReady = false;
		parent.refreshRequested = false;
		parent.retryAt = 0;
		parent.quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
		parent.forceProviderRescan = true;
		return;
	}
}

void TreeBatchRebindWindow(DWORD owner, HWND window, const std::wstring& currentPath)
{
	if( !owner || !window || !::IsWindow(window) || !TreeOwnerContainsWindow(owner, window) ) return;
	std::wstring previousPaths[kParentCapacity];
	size_t previousCount = 0;
	try
	{
		for( const auto& parent : gParents )
			if( previousCount < kParentCapacity ) previousPaths[previousCount++] = parent.path;
	}
	catch( ... ) { return; }
	HWND previousWindow = gTreeWindow;
	if( previousWindow && ::IsWindow(previousWindow) )
		::RemoveWindowSubclass(previousWindow, TreeNotificationBatchSubclass, kTreeBatchSubclassId);
	TreeBatchCloseWindow(previousWindow);
	// Attach the parent named by this callback first. Reattach the remaining
	// tracked parents only if they still have an expanded, identity-checked node.
	if( TreeBatchAttachParent(owner, window, currentPath) ) TreeBatchRequireCatchup(currentPath);
	for( size_t i = 0; i < previousCount; ++i )
	{
		if( SamePath(previousPaths[i], currentPath) ) continue;
		if( TreeBatchAttachParent(owner, window, previousPaths[i]) )
			TreeBatchRequireCatchup(previousPaths[i]);
	}
}


void TreeBatchScheduleNext()
{
	if( !gTreeWindow || !::IsWindow(gTreeWindow) || gTimerArmed ) return;
	const ULONGLONG now = ::GetTickCount64();
	UINT delay = 0;
	for( const auto& parent : gParents )
	{
		if( parent.unavailable || !parent.worker ) continue;
		UINT current = 0;
		if( parent.hasPendingFolderCandidate ) current = 1;
		else if( !parent.baselineReady && !parent.dirty ) current = 100;
		else if( parent.dirty )
		{
			if( parent.retryAt > now ) current = static_cast<UINT>((std::min)(parent.retryAt - now, static_cast<ULONGLONG>(30000)));
			else if( parent.quietDeadline > now ) current = static_cast<UINT>((std::min)(parent.quietDeadline - now, static_cast<ULONGLONG>(gQuietIntervalMs)));
			else current = parent.refreshRequested ? 50 : 1;
		}
		if( current && (!delay || current < delay) ) delay = current;
	}
	if( delay && !TreeBatchArmTimer(gTreeWindow, delay) )
		for( auto& parent : gParents ) if( parent.dirty )
			TreeBatchEmergencyRescanOrDisable(parent, gTreeOwner, gTreeWindow, "schedule_timer_arm");
}

void TreeBatchRetry(ParentSession& parent, UINT minimumDelay, const char* reason)
{
	parent.suppressionReady = false;
	parent.dirty = true;
	parent.refreshRequested = false;
	if( parent.retryCount < 3 )
	{
		++parent.retryCount;
		if( parent.retryDelayMs == 0 ) parent.retryDelayMs = (std::max)(static_cast<UINT>(1000), minimumDelay);
		else parent.retryDelayMs = parent.retryDelayMs > 15000 ? 30000 : parent.retryDelayMs * 2;
	}
	else parent.retryDelayMs = 30000;
	parent.retryAt = ::GetTickCount64() + parent.retryDelayMs;
	TreeBatchLog("tree_batch_retry parent_hash=%08lX reason=%s delay_ms=%u retry_count=%u shell_error=0x%08lX",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), reason ? reason : "unspecified",
		parent.retryDelayMs, parent.retryCount, static_cast<unsigned long>(parent.lastShellError));
}

bool TreeBatchReplaceFailedWorker(ParentSession& parent)
{
	if( parent.workerReplacementAttempted || parent.workerSlot >= kParentCapacity
		|| gWorkerReplacementAttempted[parent.workerSlot]
		|| !parent.worker || !parent.worker->TerminationConfirmed() ) return false;
	TreeBatchDiscardPendingFolderState(parent, true);
	parent.workerReplacementAttempted = true;
	gWorkerReplacementAttempted[parent.workerSlot] = true;
	std::wstring launcher;
	if( !TreeBatchLauncher(launcher) )
	{
		TreeBatchLog("tree_batch_worker_replacement parent_hash=%08lX slot=%u result=launcher_missing",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash), parent.workerSlot);
		return false;
	}
	WatchedSyncWorker* replacement = new (std::nothrow) WatchedSyncWorker(launcher, 5000, false, false);
	if( !replacement ) return false;
	if( !replacement->Start() )
	{
		delete replacement;
		TreeBatchLog("tree_batch_worker_replacement parent_hash=%08lX slot=%u result=start_failed",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash), parent.workerSlot);
		return false;
	}
	const std::uint64_t target = replacement->SetTarget(parent.path);
	if( !target )
	{
		// Keep the started thread alive for process lifetime, but do not let this
		// parent suppress more notifications without a usable replacement target.
		gWorkerPool[parent.workerSlot] = replacement;
		parent.worker = replacement;
		TreeBatchMarkUnavailable(parent, "worker_replacement_target");
		return false;
	}
	// Retain the previous worker for process lifetime: its thread may still be
	// unwinding after a fatal pipe error, so replacing this slot must not destroy it.
	gWorkerPool[parent.workerSlot] = replacement;
	parent.worker = replacement;
	// A replacement stream has no continuity with the failed watcher. Require a
	// fresh baseline and a provider rescan before allowing any further suppression.
	parent.baselineReady = false;
	parent.suppressionReady = false;
	parent.committedState.reset();
	parent.committedTreeBaseline.clear();
	parent.committedTreeBaselineReady = false;
	parent.volume = 0;
	parent.directoryId = 0;
	parent.dirty = true;
	parent.forceProviderRescan = true;
	parent.refreshRequested = false;
	parent.quietDeadline = 0;
	parent.retryAt = ::GetTickCount64() + 100;
	TreeBatchLog("tree_batch_worker_replacement parent_hash=%08lX slot=%u result=started",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), parent.workerSlot);
	return true;
}

bool TreeBatchCaptureCommittedTree(HWND window, DWORD owner, const ParentSession& parent,
	const DirectorySnapshot::State& state, std::vector<TreeChildState>& baseline, UINT& parentVisualState);

void TreeBatchRememberState(ParentSession& parent, DirectorySnapshot::State&& state,
	bool preserveTreeBaseline = false)
{
	parent.volume = state.volume;
	parent.directoryId = state.directoryId;
	if( gBatchMode == 2 && state.entries.size() <= kRemovalCompareLimit )
	{
		try { parent.committedState.reset(new DirectorySnapshot::State(std::move(state))); }
		catch( ... ) { parent.committedState.reset(); }
	}
	else parent.committedState.reset();
	const bool keepTreeBaseline = preserveTreeBaseline && parent.committedTreeBaselineReady;
	if( !keepTreeBaseline )
	{
		parent.committedTreeBaseline.clear();
		parent.committedTreeBaselineReady = false;
		parent.committedParentVisualState = 0;
		if( gBatchMode == 2 && parent.committedState )
		{
			std::vector<TreeChildState> baseline;
			UINT parentVisualState = 0;
			if( TreeBatchCaptureCommittedTree(gTreeWindow, gTreeOwner, parent, *parent.committedState,
				baseline, parentVisualState) )
			{
				parent.committedTreeBaseline.swap(baseline);
				parent.committedTreeBaselineReady = true;
				parent.committedParentVisualState = parentVisualState;
			}
			else
			{
				TreeBatchLog("tree_batch_tree_baseline_failed parent_hash=%08lX entries=%u",
					static_cast<unsigned long>(parent.nodeIdentity.pathHash),
					static_cast<unsigned>(parent.committedState->entries.size()));
			}
		}
	}
	parent.baselineReady = true;
	parent.suppressionReady = true;
	parent.dirty = false;
	parent.refreshRequested = false;
	parent.retryDelayMs = 0;
	parent.retryCount = 0;
	parent.retryAt = 0;
	parent.quietDeadline = 0;
	parent.folderActionSuppressed = false;
	parent.forceProviderRescan = false;
	parent.baselineCaptureAttempts = 0;
	TreeBatchClearPendingFolderState(parent);
	++parent.commitCount;
	unsigned pathlessChildren = 0;
	for( const auto& child : parent.committedTreeBaseline )
		if( !child.identity.pathKnown ) ++pathlessChildren;
	TreeBatchLog("tree_batch_commit parent_hash=%08lX commit=%llu entries=%u tree_baseline=%u pathless_children=%u cycle_action2=%llu cycle_add=%llu cycle_delete=%llu suppressed_total=%llu/%llu/%llu norescan=%llu rescans=%llu last_rescan_us=%llu shell_error=0x%08lX",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), parent.commitCount,
		static_cast<unsigned>(parent.committedState ? parent.committedState->entries.size() : 0),
		static_cast<unsigned>(parent.committedTreeBaseline.size()), pathlessChildren, parent.cycleAction2,
		parent.cycleAdd, parent.cycleDelete, parent.suppressedAction2, parent.suppressedAdd,
		parent.suppressedDelete, parent.noRescanCount, parent.rescanCount, parent.rescanLastUs,
		static_cast<unsigned long>(parent.lastShellError));
	parent.cycleAction2 = 0;
	parent.cycleAdd = 0;
	parent.cycleDelete = 0;
}

void TreeBatchRememberFlatState(ParentSession& parent, DirectorySnapshot::State&& state)
{
	parent.volume = state.volume;
	parent.directoryId = state.directoryId;
	try { parent.committedState.reset(new DirectorySnapshot::State(std::move(state))); }
	catch( ... ) { parent.committedState.reset(); }
	parent.committedTreeBaseline.clear();
	parent.committedTreeBaselineReady = false;
	parent.committedParentVisualState = 0;
	parent.baselineReady = true;
	parent.suppressionReady = true;
	parent.flatReady = true;
	parent.dirty = false;
	parent.refreshRequested = false;
	parent.retryDelayMs = 0;
	parent.retryCount = 0;
	parent.retryAt = 0;
	parent.quietDeadline = 0;
	parent.folderActionSuppressed = false;
	parent.forceProviderRescan = false;
	parent.baselineCaptureAttempts = 0;
	TreeBatchClearPendingFolderState(parent);
	++parent.commitCount;
	++parent.flatCommits;
	TreeBatchLog("tree_batch_flat_commit parent_key=%08lX parent_hash=%08lX commit=%llu entries=%u cycle_action2=%llu suppressed_total=%llu norescan=%llu rescans=%llu",
		static_cast<unsigned long>(TreeBatchPathKey(parent.path)),
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), parent.commitCount,
		static_cast<unsigned>(parent.committedState ? parent.committedState->entries.size() : 0),
		parent.cycleAction2, parent.suppressedAction2, parent.noRescanCount, parent.rescanCount);
	parent.cycleAction2 = 0;
	parent.cycleAdd = 0;
	parent.cycleDelete = 0;
}

bool TreeBatchRecoverFolderActions(ParentSession& parent, HWND window, const char* reason)
{
	TreeBatchLog("tree_batch_folder_discard parent_hash=%08lX reason=%s pending_actions=%u",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), reason ? reason : "unspecified",
		static_cast<unsigned>(parent.pendingFolderActions.size()));
	TreeBatchDiscardPendingFolderState(parent, true);
	parent.dirty = true;
	parent.suppressionReady = false;
	parent.refreshRequested = false;
	parent.retryAt = 0;
	parent.quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
	if( !TreeBatchArmTimer(window, gQuietIntervalMs) )
	{
		TreeBatchEmergencyRescanOrDisable(parent, gTreeOwner, window, "recover_timer_arm");
		return false;
	}
	return true;
}

bool TreeBatchCanSkipRemovalRescan(HWND window, DWORD owner, const ParentSession& parent,
	const DirectorySnapshot::State& currentState)
{
	if( !parent.committedState || !parent.committedTreeBaselineReady
		|| parent.forceProviderRescan || parent.folderActionSuppressed
		|| currentState.entries.size() > kRemovalCompareLimit
		|| parent.committedState->entries.size() > kRemovalCompareLimit
		|| !DirectorySnapshot::OnlyRemovals(*parent.committedState, currentState)
		|| !TreeBatchParentCurrent(window, owner, parent, true)
		|| !TreeBatchParentVisualStateMatches(window, parent, parent.committedParentVisualState) ) return false;
	try
	{
		std::vector<std::wstring> visibleNames;
		std::vector<TreeChildState> visibleTree;
		visibleNames.reserve(currentState.entries.size());
		visibleTree.reserve(parent.committedTreeBaseline.size());
		LARGE_INTEGER started = {};
		LONGLONG budgetTicks = 0;
		if( !TreeBatchStartCompareBudget(started, budgetTicks) ) return false;
		HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_CHILD, reinterpret_cast<LPARAM>(parent.item)));
		while( child )
		{
			if( visibleNames.size() >= kRemovalCompareLimit ) return false;
			if( (visibleNames.size() & 31) == 0
				&& TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
			wchar_t label[1024] = {};
			TVITEMW item = {};
			item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
			item.stateMask = TVIS_EXPANDED | TVIS_SELECTED;
			item.hItem = child;
			item.pszText = label;
			item.cchTextMax = static_cast<int>(_countof(label));
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
				|| !item.lParam || !label[0] ) return false;
			const auto found = std::lower_bound(currentState.entries.begin(), currentState.entries.end(), label,
				[](const DirectorySnapshot::Entry& entry, const wchar_t* name) { return entry.name < name; });
			// A visible file can be an archive/plugin node whose descendants are
			// not represented by this direct-directory snapshot. Keep those cases
			// on the provider rescan path even when the direct names still match.
			if( found == currentState.entries.end() || found->name != label
				|| !(found->attributes & FILE_ATTRIBUTE_DIRECTORY) ) return false;
			TreeNodePathIdentity observed = {};
			const DWORD node = static_cast<DWORD>(item.lParam);
			if( !ReadTreeNodePathIdentity(node, observed, true) ) return false;
			std::wstring expectedPath = parent.path;
			if( expectedPath.empty() ) return false;
			if( expectedPath.back() != L'\\' && expectedPath.back() != L'/' ) expectedPath.push_back(L'\\');
			expectedPath += found->name;
			if( !TreeBatchPathAcceptable(observed, expectedPath) ) return false;
			TVITEMW verify = {};
			verify.mask = TVIF_PARAM;
			verify.hItem = child;
			TreeNodePathIdentity current = {};
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&verify))
				|| static_cast<DWORD>(verify.lParam) != node
				|| !ReadTreeNodePathIdentity(node, current, true)
				|| !TreeBatchNodeIdentityEqual(observed, current)
				|| reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_PARENT, reinterpret_cast<LPARAM>(child))) != parent.item ) return false;
			visibleNames.push_back(found->name);
			TreeChildState treeChild;
			treeChild.name = found->name;
			treeChild.path = expectedPath;
			treeChild.state = item.state & (TVIS_EXPANDED | TVIS_SELECTED);
			treeChild.identity = observed;
			visibleTree.emplace_back(std::move(treeChild));
			child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
				TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
		}
		std::sort(visibleNames.begin(), visibleNames.end());
		if( TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
		if( std::adjacent_find(visibleNames.begin(), visibleNames.end()) != visibleNames.end() ) return false;
		std::sort(visibleTree.begin(), visibleTree.end(), [](const TreeChildState& left, const TreeChildState& right) {
			return left.name < right.name;
		});
		if( visibleTree.size() != parent.committedTreeBaseline.size() ) return false;
		for( size_t index = 0; index < visibleTree.size(); ++index )
		{
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
			const TreeChildState& expected = parent.committedTreeBaseline[index];
			const TreeChildState& observed = visibleTree[index];
			if( !TreeBatchExactPath(expected.name, observed.name)
				|| !TreeBatchExactPath(expected.path, observed.path)
				|| expected.state != observed.state
				|| !TreeBatchNodeIdentityEqual(expected.identity, observed.identity) ) return false;
		}
		size_t checkedDirectories = 0;
		for( const auto& entry : currentState.entries )
		{
			if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) ) continue;
			++checkedDirectories;
			if( !std::binary_search(visibleNames.begin(), visibleNames.end(), entry.name)
				|| ((checkedDirectories & 31) == 0
					&& TreeBatchCompareBudgetExceeded(started, budgetTicks)) ) return false;
		}
		return TreeBatchParentCurrent(window, owner, parent, true);
	}
	catch( ... ) { return false; }
}

struct TreeBatchCaseInsensitiveLess
{
	bool operator()(const std::wstring& left, const std::wstring& right) const
	{
		return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
	}
};

struct TreeBatchVisibleFolder
{
	HTREEITEM item = nullptr;
	DWORD node = 0;
	UINT state = 0;
	int children = 0;
	TreeNodePathIdentity identity = {};
};

const DirectorySnapshot::Entry* TreeBatchFindEntry(const DirectorySnapshot::State& state,
	const std::wstring& name)
{
	const auto found = std::lower_bound(state.entries.begin(), state.entries.end(), name,
		[](const DirectorySnapshot::Entry& entry, const std::wstring& value) { return entry.name < value; });
	return found != state.entries.end() && found->name == name ? &*found : nullptr;
}

bool TreeBatchCaptureCommittedTree(HWND window, DWORD owner, const ParentSession& parent,
	const DirectorySnapshot::State& state, std::vector<TreeChildState>& baseline, UINT& parentVisualState)
{
	baseline.clear();
	parentVisualState = 0;
	const DWORD parentHash = static_cast<unsigned long>(parent.nodeIdentity.pathHash);
	const auto fail = [parentHash](const char* reason)
	{
		TreeBatchLog("tree_batch_tree_baseline_failed parent_hash=%08lX reason=%s",
			static_cast<unsigned long>(parentHash), reason);
		return false;
	};
	if( !TreeBatchSnapshotSupported(state) ) return fail("snapshot_unsupported");
	if( state.entries.size() > kRemovalCompareLimit ) return fail("entry_limit");
	if( !TreeBatchParentCurrent(window, owner, parent, true) ) return fail("parent_current");
	try
	{
		LARGE_INTEGER started = {};
		LONGLONG budgetTicks = 0;
		if( !TreeBatchStartCompareBudget(started, budgetTicks) ) return fail("budget_clock");
		if( !TreeBatchParentVisualState(window, parent, parentVisualState) )
			return fail("visual_state");
		if( !(parentVisualState & TVIS_EXPANDED) ) return fail("parent_collapsed");
		std::set<std::wstring, TreeBatchCaseInsensitiveLess> seen;
		HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_CHILD, reinterpret_cast<LPARAM>(parent.item)));
		for( unsigned count = 0; child; ++count )
		{
			if( count >= kRemovalCompareLimit ) return fail("child_limit");
			if( (count & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_children");
			wchar_t label[1024] = {};
			TVITEMW item = {};
			item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
			item.stateMask = TVIS_EXPANDED | TVIS_SELECTED;
			item.hItem = child;
			item.pszText = label;
			item.cchTextMax = static_cast<int>(_countof(label));
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
				|| !item.lParam || !label[0] || wcsnlen_s(label, _countof(label)) == _countof(label) - 1 )
				return fail("child_item");
			const std::wstring name(label);
			const DirectorySnapshot::Entry* entry = TreeBatchFindEntry(state, name);
			if( !entry ) return fail("child_not_in_snapshot");
			if( !(entry->attributes & FILE_ATTRIBUTE_DIRECTORY) ) return fail("child_not_directory");
			if( entry->name != name ) return fail("child_name");
			if( reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_PARENT, reinterpret_cast<LPARAM>(child))) != parent.item ) return fail("child_parent");
			std::wstring path;
			try
			{
				path = parent.path;
				if( path.empty() ) return false;
				if( path.back() != L'\\' && path.back() != L'/' ) path.push_back(L'\\');
				path += entry->name;
			}
			catch( ... ) { return false; }
			TreeChildState value;
			value.name = name;
			value.path = path;
			value.state = item.state & (TVIS_EXPANDED | TVIS_SELECTED);
			if( !ReadTreeNodePathIdentity(static_cast<DWORD>(item.lParam), value.identity, true) )
				return fail("child_identity");
			if( !TreeBatchPathAcceptable(value.identity, path) ) return fail("child_path");
			if( !seen.insert(name).second ) return fail("child_duplicate");
			baseline.emplace_back(std::move(value));
			child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
				TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
		}
		size_t expected = 0;
		for( size_t index = 0; index < state.entries.size(); ++index )
		{
			const auto& entry = state.entries[index];
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_entries");
			if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) ) continue;
			++expected;
			if( seen.find(entry.name) == seen.end() ) return fail("directory_missing_in_tree");
		}
		if( expected != baseline.size() ) return fail("directory_count");
		if( TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return fail("budget_final");
		if( !TreeBatchParentCurrent(window, owner, parent, true) ) return fail("parent_current_final");
		std::sort(baseline.begin(), baseline.end(), [](const TreeChildState& left, const TreeChildState& right) {
			return left.name < right.name;
		});
		return true;
	}
	catch( ... ) { baseline.clear(); return fail("exception"); }
}

bool TreeBatchFolderPath(const std::wstring& parent, const std::wstring& name,
	std::wstring& fullPath, std::string& encoded)
{
	if( parent.empty() || name.empty() || name.size() > 255
		|| name.find_first_of(L"\\/") != std::wstring::npos || name == L"." || name == L".." ) return false;
	try
	{
		fullPath = parent;
		if( fullPath.back() != L'\\' && fullPath.back() != L'/' ) fullPath.push_back(L'\\');
		fullPath += name;
	}
	catch( ... ) { return false; }
	return EncodeTreePathExact(CP_ACP, fullPath, encoded, kMaximumPathBytes)
		&& encoded.size() < kMaximumPathBytes - 1;
}

// Directory of the folder whose contents the list is showing. The list-side
// helper can be stale (it is bound by the delete batch), so fall back to the
// tree caret, which Leeyes keeps in sync with the displayed folder and whose
// path cache is populated for a selected node.
bool TreeBatchDisplayedDirectory(HWND window, std::wstring& directory, const char*& source)
{
	if( GetCurrentListDirectory(directory) )
	{
		source = "list";
		return true;
	}
	HTREEITEM caret = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
		TVGN_CARET, 0));
	if( !caret ) return false;
	TVITEMW item = {};
	item.mask = TVIF_PARAM;
	item.hItem = caret;
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !item.lParam )
		return false;
	TreeNodePathIdentity identity = {};
	if( !ReadTreeNodePathIdentity(static_cast<DWORD>(item.lParam), identity) || !identity.pathKnown )
		return false;
	std::wstring path;
	if( !ReadDelphiAnsiTreePath(CP_ACP, identity.cachedPath, path) || path.empty() ) return false;
	directory.swap(path);
	source = "caret";
	return true;
}

bool TreeBatchPathWithin(const std::wstring& ancestor, const std::wstring& path)
{
	if( ancestor.empty() || path.empty() ) return false;
	const size_t length = ancestor.size();
	if( path.size() < length
		|| ::CompareStringOrdinal(ancestor.data(), static_cast<int>(length),
			path.data(), static_cast<int>(length), TRUE) != CSTR_EQUAL ) return false;
	if( path.size() == length ) return true;
	if( ancestor.back() == L'\\' || ancestor.back() == L'/' ) return true;
	return path[length] == L'\\' || path[length] == L'/';
}

bool TreeBatchFolderLeaf(HWND window, HTREEITEM item, const TreeNodePathIdentity& identity,
	DWORD node, const std::wstring& fullPath, const std::wstring& listDirectory, HTREEITEM selected)
{
	if( !window || !item || !node ) return false;
	TVITEMW state = {};
	state.mask = TVIF_STATE | TVIF_CHILDREN | TVIF_PARAM;
	state.stateMask = TVIS_EXPANDED;
	state.hItem = item;
	// A removal is only replayed for a collapsed node that has no instantiated
	// children. The cChildren hint is not used: Leeyes reports "may have
	// children" for every directory, including empty ones.
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&state))
		|| static_cast<DWORD>(state.lParam) != node || (state.state & TVIS_EXPANDED)
		|| ::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(item)) ) return false;
	TreeNodePathIdentity current = {};
	if( !ReadTreeNodePathIdentity(node, current, true) || !TreeBatchNodeIdentityEqual(identity, current)
		|| !TreeBatchPathAcceptable(current, fullPath) ) return false;
	for( unsigned depth = 0; selected && depth < 1024; ++depth )
	{
		if( selected == item ) return false;
		selected = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_PARENT, reinterpret_cast<LPARAM>(selected)));
	}
	if( selected ) return false;
	return !listDirectory.empty() && !TreeBatchPathWithin(fullPath, listDirectory);
}

bool TreeBatchBuildFolderActions(HWND window, DWORD owner, ParentSession& parent,
	const WatchedSyncWorker::Candidate& candidate, std::vector<FolderAction>& actions)
{
	actions.clear();
	parent.pendingTreeBaseline.clear();
	parent.pendingParentVisualState = 0;
	const DWORD parentHash = static_cast<unsigned long>(parent.nodeIdentity.pathHash);
	const auto fail = [parentHash](const char* reason)
	{
		TreeBatchLog("tree_batch_folder_actions_rejected parent_hash=%08lX reason=%s",
			static_cast<unsigned long>(parentHash), reason);
		return false;
	};
	if( !parent.committedState ) return fail("no_committed_state");
	if( !parent.committedTreeBaselineReady ) return fail("no_tree_baseline");
	if( parent.forceProviderRescan ) return fail("force_rescan");
	if( !parent.worker ) return fail("no_worker");
	if( !parent.worker->HasContinuousBaseline(candidate) ) return fail("no_continuous_baseline");
	const DirectorySnapshot::State& currentState = candidate.state;
	if( currentState.volume != parent.volume || currentState.directoryId != parent.directoryId
		|| !TreeBatchParentCurrent(window, owner, parent, true)
		|| !TreeBatchParentVisualStateMatches(window, parent, parent.committedParentVisualState)
		|| !TreeBatchSnapshotSupported(currentState)
		|| !parent.committedState->directoryId
		|| parent.committedState->volume != currentState.volume
		|| parent.committedState->directoryId != currentState.directoryId
		|| parent.committedState->entries.size() > kRemovalCompareLimit
		|| currentState.entries.size() > kRemovalCompareLimit ) return fail("shape");
	try
	{
		LARGE_INTEGER started = {};
		LONGLONG budgetTicks = 0;
		if( !TreeBatchStartCompareBudget(started, budgetTicks) ) return fail("budget_clock");
		std::set<std::wstring, TreeBatchCaseInsensitiveLess> beforeNames, currentNames;
		auto validState = [](const DirectorySnapshot::State& state,
			std::set<std::wstring, TreeBatchCaseInsensitiveLess>& names,
			const LARGE_INTEGER& started, LONGLONG budgetTicks) {
			for( size_t i = 0; i < state.entries.size(); ++i )
			{
				if( (i & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
				const auto& entry = state.entries[i];
				if( entry.name.empty() || entry.name.size() > 255 || entry.name.find_first_of(L"\\/") != std::wstring::npos
					|| entry.name == L"." || entry.name == L".." || (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT)
					|| (i && !(state.entries[i - 1].name < entry.name)) || !names.insert(entry.name).second ) return false;
			}
			return true;
		};
		if( !validState(*parent.committedState, beforeNames, started, budgetTicks) )
			return fail("committed_state_invalid");
		if( !validState(currentState, currentNames, started, budgetTicks) )
			return fail("current_state_invalid");
		std::vector<FolderAction> removed, added;
		std::map<std::wstring, size_t, TreeBatchCaseInsensitiveLess> removedByName;
		const auto& before = parent.committedState->entries;
		size_t i = 0, j = 0;
		while( i < before.size() || j < currentState.entries.size() )
		{
			if( ((i + j) & 31) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_merge");
			if( j == currentState.entries.size() || (i < before.size() && before[i].name < currentState.entries[j].name) )
			{
				if( !(before[i].attributes & FILE_ATTRIBUTE_DIRECTORY) ) return fail("removed_file");
				FolderAction action; action.action = 5; action.name = before[i].name;
				if( !TreeBatchFolderPath(parent.path, action.name, action.fullPath, action.encodedPath) )
					return fail("removed_path_encode");
				if( !removedByName.emplace(action.name, removed.size()).second )
					return fail("removed_duplicate");
				removed.emplace_back(std::move(action));
				++i;
			}
			else if( i == before.size() || currentState.entries[j].name < before[i].name )
			{
				if( !(currentState.entries[j].attributes & FILE_ATTRIBUTE_DIRECTORY) ) return fail("added_file");
				FolderAction action; action.action = 3; action.name = currentState.entries[j].name;
				if( !TreeBatchFolderPath(parent.path, action.name, action.fullPath, action.encodedPath) )
					return fail("added_path_encode");
				added.emplace_back(std::move(action));
				++j;
			}
			else
			{
				const auto& oldEntry = before[i];
				const auto& newEntry = currentState.entries[j];
				if( oldEntry.attributes != newEntry.attributes || oldEntry.size != newEntry.size
					|| oldEntry.created != newEntry.created || oldEntry.written != newEntry.written )
					return fail("entry_metadata_changed");
				++i;
				++j;
			}
		}
		std::wstring listDirectory;
		const char* listSource = "unknown";
		if( !TreeBatchDisplayedDirectory(window, listDirectory, listSource) )
			return fail("list_directory");
		TreeBatchLog("tree_batch_displayed_directory parent_hash=%08lX source=%s length=%u",
			static_cast<unsigned long>(parentHash), listSource,
			static_cast<unsigned>(listDirectory.size()));
		const HTREEITEM selected = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_CARET, 0));
		UINT parentVisualState = 0;
		if( !TreeBatchParentVisualState(window, parent, parentVisualState) )
			return fail("parent_visual_state");
		if( parentVisualState != parent.committedParentVisualState )
			return fail("parent_visual_changed");

		std::map<std::wstring, TreeBatchVisibleFolder, TreeBatchCaseInsensitiveLess> visible;
		std::vector<TreeChildState> treeBaseline;
		HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_CHILD, reinterpret_cast<LPARAM>(parent.item)));
		for( unsigned count = 0; child; ++count )
		{
			if( count >= kRemovalCompareLimit ) return fail("child_limit");
			if( (count & 31) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_children");
			wchar_t label[1024] = {};
			TVITEMW item = {};
			item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE | TVIF_CHILDREN;
			item.stateMask = TVIS_EXPANDED | TVIS_SELECTED;
			item.hItem = child;
			item.pszText = label;
			item.cchTextMax = static_cast<int>(_countof(label));
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
				|| !item.lParam || !label[0] || wcsnlen_s(label, _countof(label)) == _countof(label) - 1 )
				return fail("child_item");
			const std::wstring name(label);
			const DirectorySnapshot::Entry* oldEntry = TreeBatchFindEntry(*parent.committedState, name);
			const DirectorySnapshot::Entry* newEntry = TreeBatchFindEntry(currentState, name);
			if( (!oldEntry || !(oldEntry->attributes & FILE_ATTRIBUTE_DIRECTORY))
				&& (!newEntry || !(newEntry->attributes & FILE_ATTRIBUTE_DIRECTORY)) )
				return fail("visible_not_directory");
			if( oldEntry && oldEntry->name != name ) return fail("visible_old_name");
			if( newEntry && newEntry->name != name ) return fail("visible_new_name");
			const DirectorySnapshot::Entry* pathEntry = newEntry ? newEntry : oldEntry;
			std::wstring expectedPath;
			try
			{
				expectedPath = parent.path;
				if( expectedPath.empty() ) return false;
				if( expectedPath.back() != L'\\' && expectedPath.back() != L'/' ) expectedPath.push_back(L'\\');
				expectedPath += pathEntry->name;
			}
			catch( ... ) { return false; }
			TreeBatchVisibleFolder visibleFolder;
			visibleFolder.item = child;
			visibleFolder.node = static_cast<DWORD>(item.lParam);
			visibleFolder.state = item.state;
			visibleFolder.children = item.cChildren;
			if( !ReadTreeNodePathIdentity(visibleFolder.node, visibleFolder.identity, true) )
				return fail("visible_identity");
			if( !TreeBatchPathAcceptable(visibleFolder.identity, expectedPath) )
				return fail("visible_path");
			if( reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_PARENT, reinterpret_cast<LPARAM>(child))) != parent.item )
				return fail("visible_parent");
			if( !visible.emplace(name, visibleFolder).second ) return fail("visible_duplicate");
			TreeChildState baselineChild;
			baselineChild.name = name;
			baselineChild.path = expectedPath;
			baselineChild.state = item.state & (TVIS_EXPANDED | TVIS_SELECTED);
			baselineChild.identity = visibleFolder.identity;
			treeBaseline.emplace_back(std::move(baselineChild));
			child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
				TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
		}

		// The current tree must still match the committed directory set exactly.
		// A missing baseline node or a pre-existing addition makes callback replay
		// ambiguous, so keep the provider rescan path for those cases.
		for( size_t index = 0; index < before.size(); ++index )
		{
			const auto& entry = before[index];
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_before");
			if( (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) && visible.find(entry.name) == visible.end() )
				return fail("baseline_directory_missing");
		}
		std::sort(treeBaseline.begin(), treeBaseline.end(), [](const TreeChildState& left, const TreeChildState& right) {
			return left.name < right.name;
		});
		if( treeBaseline.size() != parent.committedTreeBaseline.size() )
			return fail("tree_baseline_size");
		for( size_t index = 0; index < treeBaseline.size(); ++index )
		{
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
			const TreeChildState& beforeNode = parent.committedTreeBaseline[index];
			const TreeChildState& currentNode = treeBaseline[index];
			if( !TreeBatchExactPath(beforeNode.name, currentNode.name) )
				return fail("tree_baseline_name");
			if( !TreeBatchExactPath(beforeNode.path, currentNode.path) )
				return fail("tree_baseline_path");
			if( beforeNode.state != currentNode.state )
				return fail("tree_baseline_state");
			if( !TreeBatchNodeIdentityEqual(beforeNode.identity, currentNode.identity) )
				return fail("tree_baseline_identity");
		}
		for( size_t index = 0; index < currentState.entries.size(); ++index )
		{
			const auto& entry = currentState.entries[index];
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
			if( (entry.attributes & FILE_ATTRIBUTE_DIRECTORY)
				&& beforeNames.find(entry.name) == beforeNames.end() && visible.find(entry.name) != visible.end() )
				return fail("addition_already_visible");
		}
		for( const auto& pair : visible )
		{
			const std::wstring& name = pair.first;
			const auto* oldEntry = TreeBatchFindEntry(*parent.committedState, name);
			const auto* newEntry = TreeBatchFindEntry(currentState, name);
			if( oldEntry && (oldEntry->attributes & FILE_ATTRIBUTE_DIRECTORY)
				&& (!newEntry || !(newEntry->attributes & FILE_ATTRIBUTE_DIRECTORY)) )
			{
				auto action = removedByName.find(name);
				if( action == removedByName.end() ) return fail("removal_not_listed");
				FolderAction& removal = removed[action->second];
				if( !TreeBatchFolderLeaf(window, pair.second.item, pair.second.identity,
				pair.second.node, removal.fullPath, listDirectory, selected) )
					return fail("removal_not_leaf");
				removal.item = pair.second.item;
				removal.node = pair.second.node;
				removal.identity = pair.second.identity;
			}
			else if( !newEntry || !(newEntry->attributes & FILE_ATTRIBUTE_DIRECTORY) )
				return fail("visible_expected_directory");
		}
		for( const auto& action : removed ) if( !action.item || !action.node ) return fail("removal_binding");
		for( const auto& action : added ) if( visible.find(action.name) != visible.end() )
			return fail("addition_visible");
		if( TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return fail("budget_final");
		if( !TreeBatchParentCurrent(window, owner, parent, true) ) return fail("parent_current_final");
		actions.reserve(removed.size() + added.size());
		for( auto& action : removed ) actions.emplace_back(std::move(action));
		for( auto& action : added ) actions.emplace_back(std::move(action));
		parent.pendingTreeBaseline.swap(treeBaseline);
		parent.pendingParentVisualState = parentVisualState;
		return true;
	}
	catch( ... ) { actions.clear(); return fail("exception"); }
}

bool TreeBatchFindDirectFolder(HWND window, const ParentSession& parent, const FolderAction& action,
	const LARGE_INTEGER& started, LONGLONG budgetTicks, TreeBatchVisibleFolder& result, bool& found)
{
	found = false;
	result = TreeBatchVisibleFolder();
	if( !window || !parent.item || action.name.empty() || action.fullPath.empty() ) return false;
	HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
		TVGN_CHILD, reinterpret_cast<LPARAM>(parent.item)));
	for( unsigned count = 0; child; ++count )
	{
		if( count >= kRemovalCompareLimit || ((count & 15) == 0
			&& TreeBatchCompareBudgetExceeded(started, budgetTicks)) ) return false;
		wchar_t label[1024] = {};
		TVITEMW item = {};
		item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE | TVIF_CHILDREN;
		item.stateMask = TVIS_EXPANDED | TVIS_SELECTED;
		item.hItem = child;
		item.pszText = label;
		item.cchTextMax = static_cast<int>(_countof(label));
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
			|| !item.lParam || !label[0] || wcsnlen_s(label, _countof(label)) == _countof(label) - 1 ) return false;
		if( ::CompareStringOrdinal(label, -1, action.name.c_str(), static_cast<int>(action.name.size()), TRUE)
			== CSTR_EQUAL )
		{
			if( found || !TreeBatchExactPath(action.name, label)
				|| reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_PARENT, reinterpret_cast<LPARAM>(child))) != parent.item ) return false;
			result.item = child;
			result.node = static_cast<DWORD>(item.lParam);
			result.state = item.state;
			result.children = item.cChildren;
			if( !ReadTreeNodePathIdentity(result.node, result.identity, true)
				|| !TreeBatchPathAcceptable(result.identity, action.fullPath) ) return false;
			found = true;
		}
		child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
	}
	return !TreeBatchCompareBudgetExceeded(started, budgetTicks);
}

bool TreeBatchActionCurrent(HWND window, DWORD owner, const ParentSession& parent,
	const FolderAction& action, const std::wstring& listDirectory, HTREEITEM selected,
	const LARGE_INTEGER& started, LONGLONG budgetTicks, bool& alreadyApplied)
{
	alreadyApplied = false;
	if( (action.action != 3 && action.action != 5)
		|| !TreeBatchParentCurrent(window, owner, parent, true)
		|| !TreeBatchParentVisualStateMatches(window, parent, parent.pendingParentVisualState) ) return false;
	TreeBatchVisibleFolder visible;
	bool found = false;
	if( !TreeBatchFindDirectFolder(window, parent, action, started, budgetTicks, visible, found) ) return false;
	if( !found )
	{
		if( action.action == 3 ) return true;
		alreadyApplied = true;
		return true;
	}
	if( action.action == 3 ) { alreadyApplied = true; return true; }
	if( visible.item != action.item || visible.node != action.node
		|| !TreeBatchNodeIdentityEqual(visible.identity, action.identity)
		|| !TreeBatchFolderLeaf(window, visible.item, visible.identity, visible.node,
			action.fullPath, listDirectory, selected) ) return false;
	return true;
}

bool TreeBatchVerifyFolderTree(HWND window, DWORD owner, const ParentSession& parent,
	const DirectorySnapshot::State& currentState, const std::vector<FolderAction>& actions,
	const LARGE_INTEGER& started, LONGLONG budgetTicks,
	std::vector<TreeChildState>& verifiedTreeBaseline, UINT& parentVisualState)
{
	verifiedTreeBaseline.clear();
	parentVisualState = 0;
	const DWORD parentHash = static_cast<unsigned long>(parent.nodeIdentity.pathHash);
	const auto fail = [parentHash](const char* reason)
	{
		TreeBatchLog("tree_batch_folder_verify_failed parent_hash=%08lX reason=%s",
			static_cast<unsigned long>(parentHash), reason);
		return false;
	};
	try
	{
		std::map<std::wstring, TreeBatchVisibleFolder, TreeBatchCaseInsensitiveLess> visible;
		std::set<std::wstring, TreeBatchCaseInsensitiveLess> removedNames;
		for( const auto& action : actions )
			if( action.action == 5 && !removedNames.insert(action.name).second ) return false;
		HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
			TVGN_CHILD, reinterpret_cast<LPARAM>(parent.item)));
		for( unsigned count = 0; child; ++count )
		{
			if( count >= kRemovalCompareLimit ) return fail("child_limit");
			if( (count & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_children");
			wchar_t label[1024] = {};
			TVITEMW item = {};
			item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE | TVIF_CHILDREN;
			item.stateMask = TVIS_EXPANDED | TVIS_SELECTED;
			item.hItem = child;
			item.pszText = label;
			item.cchTextMax = static_cast<int>(_countof(label));
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
				|| !item.lParam || !label[0] || wcsnlen_s(label, _countof(label)) == _countof(label) - 1 )
				return fail("child_item");
			const std::wstring name(label);
			const DirectorySnapshot::Entry* entry = TreeBatchFindEntry(currentState, name);
			if( !entry ) return fail("child_not_in_snapshot");
			if( !(entry->attributes & FILE_ATTRIBUTE_DIRECTORY) ) return fail("child_not_directory");
			if( entry->name != name ) return fail("child_name");
			std::wstring expectedPath;
			try
			{
				expectedPath = parent.path;
				if( expectedPath.empty() ) return false;
				if( expectedPath.back() != L'\\' && expectedPath.back() != L'/' ) expectedPath.push_back(L'\\');
				expectedPath += entry->name;
			}
			catch( ... ) { return false; }
			TreeBatchVisibleFolder folder;
			folder.item = child;
			folder.node = static_cast<DWORD>(item.lParam);
			folder.state = item.state & (TVIS_EXPANDED | TVIS_SELECTED);
			folder.children = item.cChildren;
			if( !ReadTreeNodePathIdentity(folder.node, folder.identity, true) )
				return fail("child_identity");
			if( !TreeBatchPathAcceptable(folder.identity, expectedPath) )
				return fail("child_path");
			if( reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_PARENT, reinterpret_cast<LPARAM>(child))) != parent.item )
				return fail("child_parent");
			if( !visible.emplace(name, folder).second ) return fail("child_duplicate");
			TreeChildState treeChild;
			treeChild.name = name;
			treeChild.path = expectedPath;
			treeChild.state = folder.state;
			treeChild.identity = folder.identity;
			verifiedTreeBaseline.emplace_back(std::move(treeChild));
			child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
				TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
		}
		size_t expectedCount = 0;
		for( size_t index = 0; index < currentState.entries.size(); ++index )
		{
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_entries");
			const auto& entry = currentState.entries[index];
			if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) ) continue;
			++expectedCount;
			if( visible.find(entry.name) == visible.end() ) return fail("directory_missing_in_tree");
		}
		if( expectedCount != visible.size() ) return fail("visible_count");
		for( size_t index = 0; index < parent.pendingTreeBaseline.size(); ++index )
		{
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) )
				return fail("budget_baseline");
			const TreeChildState& baseline = parent.pendingTreeBaseline[index];
			if( removedNames.find(baseline.name) != removedNames.end() ) continue;
			const auto found = visible.find(baseline.name);
			if( found == visible.end() ) return fail("baseline_missing_in_tree");
			if( !TreeBatchNodeIdentityEqual(found->second.identity, baseline.identity) )
				return fail("baseline_identity");
			if( found->second.identity.pathKnown
				&& !CachedTreePathMatches(CP_ACP, found->second.identity.cachedPath, baseline.path) )
				return fail("baseline_path");
			if( found->second.state != (baseline.state & (TVIS_EXPANDED | TVIS_SELECTED)) )
				return fail("baseline_state");
		}
		for( size_t index = 0; index < actions.size(); ++index )
		{
			if( (index & 15) == 0 && TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return false;
			const FolderAction& action = actions[index];
			if( action.action != 3 ) continue;
			const auto found = visible.find(action.name);
			if( found == visible.end() ) return fail("added_missing");
			if( !TreeBatchPathAcceptable(found->second.identity, action.fullPath) )
				return fail("added_path");
		}
		if( TreeBatchCompareBudgetExceeded(started, budgetTicks) ) return fail("budget_final");
		if( !TreeBatchParentCurrent(window, owner, parent, true) ) return fail("parent_current");
		if( !TreeBatchParentVisualState(window, parent, parentVisualState) )
			return fail("parent_visual_state");
		if( parentVisualState != parent.pendingParentVisualState ) return fail("parent_visual_changed");
		std::sort(verifiedTreeBaseline.begin(), verifiedTreeBaseline.end(),
			[](const TreeChildState& left, const TreeChildState& right) { return left.name < right.name; });
		return true;
	}
	catch( ... ) { verifiedTreeBaseline.clear(); return fail("exception"); }
}

bool TreeBatchReplayFolderAction(DWORD owner, const FolderAction& action)
{
	static const BYTE allocSignature[] = { 0x85,0xC0,0x7E,0x24,0x50 };
	static const BYTE clearSignature[] = { 0x8B,0x10,0x85,0xD2,0x74,0x1C };
	if( !owner || !gFolderAddEventHookInstalled || !gFolderDeleteEventHookInstalled
		|| action.encodedPath.empty() || action.encodedPath.size() >= kMaximumPathBytes - 1
		|| (action.action != 3 && action.action != 5)
		|| memcmp(reinterpret_cast<const void*>(0x004050CC), allocSignature, sizeof(allocSignature)) != 0
		|| memcmp(reinterpret_cast<const void*>(0x00405008), clearSignature, sizeof(clearSignature)) != 0 ) return false;
	void* delphiPath = nullptr;
	DWORD allocate = 0x004050CC;
	DWORD clear = 0x00405008;
	const DWORD byteLength = static_cast<DWORD>(action.encodedPath.size());
	__asm
	{
		mov eax, byteLength
		call allocate
		mov delphiPath, eax
	}
	if( !delphiPath ) return false;
	memcpy(delphiPath, action.encodedPath.data(), byteLength);
	void* method = reinterpret_cast<void*>(0x004CF3A8);
	const DWORD actionCode = action.action;
	gReplayEventAction = action.action;
	gReplayEventPath = &action.fullPath;
	gReplayEventPending = true;
	gRescanning = true;
	__asm
	{
		push 0
		mov ecx, delphiPath
		mov edx, actionCode
		mov eax, owner
		call method
	}
	const bool ownerEventMatched = !gReplayEventPending;
	gReplayEventPending = false;
	gReplayEventPath = nullptr;
	gReplayEventAction = 0;
	gRescanning = false;
	__asm
	{
		lea eax, delphiPath
		call clear
	}
	return ownerEventMatched;
}

bool TreeBatchApplyFolderSlice(ParentSession& parent, DWORD owner, HWND window)
{
	if( !parent.hasPendingFolderCandidate ) return false;
	if( parent.unavailable || !parent.worker )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_parent_unavailable");
		return true;
	}
	if( parent.quietDeadline > ::GetTickCount64() )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_before_quiet");
		return true;
	}
	if( !parent.worker->CanApply(parent.pendingFolderCandidate) )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_candidate_stale");
		return true;
	}
	if( !TreeBatchParentCurrent(window, owner, parent, true) )
	{
		TreeBatchMarkUnavailable(parent, "slice_parent_identity");
		return true;
	}
	std::wstring listDirectory;
	const char* listSource = "unknown";
	if( !TreeBatchDisplayedDirectory(window, listDirectory, listSource) )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_list_directory");
		return true;
	}
	const HTREEITEM selected = reinterpret_cast<HTREEITEM>(::SendMessageW(window,
		TVM_GETNEXTITEM, TVGN_CARET, 0));
	LARGE_INTEGER started = {};
	LONGLONG budgetTicks = 0;
	if( !TreeBatchStartCompareBudget(started, budgetTicks) )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_budget_clock");
		return true;
	}
	unsigned applied = 0;
	bool stale = false, failed = false;
	while( parent.nextFolderAction < parent.pendingFolderActions.size() && applied < 8
		&& !TreeBatchCompareBudgetExceeded(started, budgetTicks) )
	{
		if( !parent.worker->CanApply(parent.pendingFolderCandidate) ) { stale = true; break; }
		if( !TreeBatchParentCurrent(window, owner, parent, true) ) { failed = true; break; }
		const FolderAction& action = parent.pendingFolderActions[parent.nextFolderAction];
		bool alreadyApplied = false;
		if( !TreeBatchActionCurrent(window, owner, parent, action, listDirectory, selected,
			started, budgetTicks, alreadyApplied) ) { failed = true; break; }
		if( !alreadyApplied )
		{
			if( !TreeBatchReplayFolderAction(owner, action) ) { failed = true; break; }
			bool nowApplied = false;
			if( !TreeBatchActionCurrent(window, owner, parent, action, listDirectory, selected,
				started, budgetTicks, nowApplied) || !nowApplied ) { failed = true; break; }
		}
		++parent.nextFolderAction;
		++applied;
		if( !parent.worker->CanApply(parent.pendingFolderCandidate) ) { stale = true; break; }
	}
	if( stale || failed )
	{
		TreeBatchRecoverFolderActions(parent, window, stale ? "slice_stale" : "slice_action_failed");
		return true;
	}
	if( applied )
	{
		++parent.sliceDispatches;
		parent.sliceActions += applied;
		TreeBatchLog("tree_batch_slice parent_hash=%08lX dispatch=%llu applied=%u total_actions=%u applied_total=%llu",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash), parent.sliceDispatches, applied,
			static_cast<unsigned>(parent.pendingFolderActions.size()), parent.sliceActions);
	}
	if( parent.nextFolderAction < parent.pendingFolderActions.size() )
	{
		if( !TreeBatchArmTimer(window, 1) )
			TreeBatchEmergencyRescanOrDisable(parent, owner, window, "slice_timer_arm");
		return true;
	}
	std::vector<TreeChildState> verifiedTreeBaseline;
	UINT parentVisualState = 0;
	if( !TreeBatchVerifyFolderTree(window, owner, parent, parent.pendingFolderCandidate.state,
		parent.pendingFolderActions, started, budgetTicks, verifiedTreeBaseline, parentVisualState)
		|| !parent.worker->CanApply(parent.pendingFolderCandidate)
		|| !TreeBatchParentCurrent(window, owner, parent, true) )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_verify_failed");
		return true;
	}
	if( !parent.worker->Finish(parent.pendingFolderCandidate) )
	{
		TreeBatchRecoverFolderActions(parent, window, "slice_finish_failed");
		return true;
	}
	DirectorySnapshot::State completed = std::move(parent.pendingFolderCandidate.state);
	TreeBatchClearPendingFolderState(parent);
	parent.committedTreeBaseline.swap(verifiedTreeBaseline);
	parent.committedTreeBaselineReady = true;
	parent.committedParentVisualState = parentVisualState;
	TreeBatchRememberState(parent, std::move(completed), true);
	return true;
}

void TreeBatchMarkUnavailable(ParentSession& parent, const char* reason)
{
	if( !parent.unavailable )
		TreeBatchLog("tree_batch_unavailable parent_hash=%08lX reason=%s suppressed_action2=%llu suppressed_add=%llu suppressed_delete=%llu rescans=%llu shell_error=0x%08lX",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash), reason ? reason : "unspecified",
			parent.suppressedAction2, parent.suppressedAdd, parent.suppressedDelete,
			parent.rescanCount, static_cast<unsigned long>(parent.lastShellError));
	parent.unavailable = true;
	parent.suppressionReady = false;
	parent.committedTreeBaseline.clear();
	parent.committedTreeBaselineReady = false;
	parent.committedParentVisualState = 0;
	TreeBatchDiscardPendingFolderState(parent, true);
	TreeBatchReleaseTarget(parent);
}

void TreeBatchEmergencyRescanOrDisable(ParentSession& parent, DWORD owner, HWND window,
	const char* reason)
{
	TreeBatchLog("tree_batch_emergency_rescan parent_hash=%08lX reason=%s",
		static_cast<unsigned long>(parent.nodeIdentity.pathHash), reason ? reason : "unspecified");
	TreeBatchDiscardPendingFolderState(parent, true);
	if( TreeBatchParentCurrent(window, owner, parent, true) )
	{
		const bool rescanned = TreeBatchProviderRescan(owner, parent);
		if( rescanned && TreeBatchParentCurrent(window, owner, parent, true) )
		{
			parent.dirty = false;
			parent.refreshRequested = false;
			parent.folderActionSuppressed = false;
			parent.forceProviderRescan = false;
		}
	}
	TreeBatchMarkUnavailable(parent, reason);
}

// Flat-folder session: the parent node is a collapsed leaf and the watcher has
// to prove that the folder has no subdirectories. Only the action 2 inner search
// is withheld; the tree has no child nodes to reconcile, so no provider rescan
// is needed and the first snapshot is accepted without waiting for quiet.
bool TreeBatchProcessFlatParent(ParentSession& parent, DWORD owner, HWND window)
{
	bool expanded = false;
	if( !TreeBatchParentState(window, owner, parent, true, expanded) )
	{
		TreeBatchMarkUnavailable(parent, "flat_parent_identity");
		return true;
	}
	if( expanded )
	{
		// The user expanded the node, so file/archive child nodes may exist now.
		TreeBatchMarkUnavailable(parent, "flat_parent_expanded");
		return true;
	}
	if( parent.dirty && !parent.refreshRequested )
	{
		if( parent.worker->RequestRefresh() ) parent.refreshRequested = true;
		else TreeBatchRetry(parent, 500, "flat_refresh");
		return true;
	}
	WatchedSyncWorker::Candidate candidate;
	if( !parent.worker->Take(candidate) )
	{
		if( parent.dirty && parent.worker->Error() != ERROR_SUCCESS )
			TreeBatchRetry(parent, 500, "flat_snapshot");
		return true;
	}
	if( !parent.worker->CanApply(candidate) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		parent.refreshRequested = false;
		return true;
	}
	if( parent.baselineReady && (candidate.state.volume != parent.volume
		|| candidate.state.directoryId != parent.directoryId) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		TreeBatchMarkUnavailable(parent, "flat_directory_identity");
		return true;
	}
	if( !TreeBatchSnapshotSupported(candidate.state) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		TreeBatchMarkUnavailable(parent, "flat_snapshot_unsupported");
		return true;
	}
	for( const auto& entry : candidate.state.entries )
		if( entry.attributes & FILE_ATTRIBUTE_DIRECTORY )
		{
			bool current = false;
			parent.worker->Release(candidate, current);
			TreeBatchMarkUnavailable(parent, "flat_has_directories");
			return true;
		}
	if( !parent.worker->Finish(candidate) )
	{
		TreeBatchRetry(parent, 100, "flat_finish");
		return true;
	}
	TreeBatchRememberFlatState(parent, std::move(candidate.state));
	return true;
}

bool TreeBatchProcessParent(ParentSession& parent, DWORD owner, HWND window, ULONGLONG now)
{
	if( parent.unavailable || !parent.worker ) return false;
	if( parent.worker->Failed() )
	{
		if( !TreeBatchReplaceFailedWorker(parent) )
		{
			const bool replacementExhausted = parent.workerReplacementAttempted
				|| (parent.workerSlot < kParentCapacity
					&& gWorkerReplacementAttempted[parent.workerSlot]);
			if( parent.unavailable ) return false;
			if( parent.worker->TerminationConfirmed() && replacementExhausted )
				TreeBatchMarkUnavailable(parent, "worker_replacement_exhausted");
			else TreeBatchRetry(parent, 30000, "worker_failed_no_replacement");
		}
		return false;
	}
	if( parent.flatCandidate ) return TreeBatchProcessFlatParent(parent, owner, window);
	if( !TreeBatchParentCurrent(window, owner, parent, true) )
	{ TreeBatchMarkUnavailable(parent, "parent_identity"); return false; }
	if( parent.dirty )
	{
		if( parent.retryAt > now || parent.quietDeadline > now ) return false;
		if( !parent.refreshRequested )
		{
			if( parent.worker->RequestRefresh() ) parent.refreshRequested = true;
			else TreeBatchRetry(parent, 1000, "refresh_request");
			return false;
		}
	}
	WatchedSyncWorker::Candidate candidate;
	if( !parent.worker->Take(candidate) )
	{
		if( parent.dirty && parent.worker->Error() != ERROR_SUCCESS )
			TreeBatchRetry(parent, 1000, "snapshot_take");
		return false;
	}
	if( !parent.worker->CanApply(candidate) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		parent.refreshRequested = false;
		return false;
	}
	const bool directoryChanged = parent.baselineReady && (candidate.state.volume != parent.volume
		|| candidate.state.directoryId != parent.directoryId);
	if( directoryChanged )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		TreeBatchMarkUnavailable(parent, "directory_identity");
		return false;
	}
	const bool snapshotSupported = TreeBatchSnapshotSupported(candidate.state);
	if( !snapshotSupported )
	{
		// The first snapshot cannot authorize suppression. If a later state
		// introduces a reparse point after earlier notices were coalesced, try
		// one provider refresh to reconcile those notices, then stop batching
		// this parent and let future notices use Leeyes' original path.
		parent.suppressionReady = false;
		if( !parent.baselineReady )
		{
			bool current = false;
			parent.worker->Release(candidate, current);
			TreeBatchMarkUnavailable(parent, "snapshot_unsupported_baseline");
			return false;
		}
		if( !TreeBatchParentCurrent(window, owner, parent, true) )
		{
			bool current = false;
			parent.worker->Release(candidate, current);
			if( !TreeBatchParentCurrent(window, owner, parent, true) )
				TreeBatchMarkUnavailable(parent, "reparse_parent_identity");
			else parent.refreshRequested = false;
			return false;
		}
		TreeBatchLog("tree_batch_reparse parent_hash=%08lX action=provider_rescan_once",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash));
		const bool rescanned = TreeBatchProviderRescan(owner, parent);
		if( !rescanned || !TreeBatchParentCurrent(window, owner, parent, true) )
		{
			bool current = false;
			parent.worker->Release(candidate, current);
			TreeBatchMarkUnavailable(parent, "reparse_rescan");
			return true;
		}
		if( !parent.worker->Finish(candidate) )
		{
			TreeBatchRetry(parent, 100, "reparse_finish");
			return true;
		}
		TreeBatchMarkUnavailable(parent, "reparse_stop_batching");
		return true;
	}
	if( !parent.dirty && gBatchMode == 2 && parent.committedState
		&& !parent.committedTreeBaselineReady && parent.baselineCaptureAttempts < 20 )
	{
		// Leeyes applies the provider rescan result to the tree control after the
		// call returns, so the tree can still lag the committed snapshot here.
		// Retry the baseline capture for a short bounded window instead of
		// leaving the folder-replay/removal shortcuts permanently disabled.
		++parent.baselineCaptureAttempts;
		std::vector<TreeChildState> baseline;
		UINT parentVisualState = 0;
		if( TreeBatchCaptureCommittedTree(window, owner, parent, *parent.committedState,
			baseline, parentVisualState) )
		{
			parent.committedTreeBaseline.swap(baseline);
			parent.committedTreeBaselineReady = true;
			parent.committedParentVisualState = parentVisualState;
			TreeBatchLog("tree_batch_tree_baseline parent_hash=%08lX children=%u attempts=%u",
				static_cast<unsigned long>(parent.nodeIdentity.pathHash),
				static_cast<unsigned>(parent.committedTreeBaseline.size()),
				parent.baselineCaptureAttempts);
		}
		else if( TreeBatchArmTimer(window, 100) )
		{
			bool current = false;
			parent.worker->Release(candidate, current);
			parent.refreshRequested = false;
			return false;
		}
	}
	if( !parent.dirty )
	{
		if( gBatchMode == 1 && !parent.shadowBaselineReady )
			TreeBatchEnsureShadowBaseline(window, owner, parent, candidate);
		if( parent.worker->Finish(candidate) ) TreeBatchRememberState(parent, std::move(candidate.state));
		else parent.refreshRequested = false;
		return false;
	}
	if( parent.quietDeadline > now || !TreeBatchParentCurrent(window, owner, parent, true)
		|| !parent.worker->CanApply(candidate) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		parent.refreshRequested = false;
		return false;
	}
	if( gBatchMode == 1 )
	{
		if( !parent.shadowBaselineReady ) TreeBatchEnsureShadowBaseline(window, owner, parent, candidate);
		unsigned missing = 0, unexpected = 0, selectedCount = 0, expandedCount = 0;
		unsigned selectionChanges = 0, expansionChanges = 0, identityChanges = 0;
		unsigned subtreeAdded = 0, subtreeRemoved = 0;
		const bool comparable = TreeBatchCompareSnapshot(window, parent, candidate.state,
			missing, unexpected, selectedCount, expandedCount, selectionChanges, expansionChanges,
			identityChanges, subtreeAdded, subtreeRemoved);
		if( !parent.worker->CanApply(candidate)
			|| !TreeBatchParentCurrent(window, owner, parent, true) )
		{
			bool current = false;
			parent.worker->Release(candidate, current);
			if( !TreeBatchParentCurrent(window, owner, parent, true) )
				TreeBatchMarkUnavailable(parent, "shadow_parent_identity");
			parent.refreshRequested = false;
			return false;
		}
		TreeBatchReportShadow(parent, comparable, missing, unexpected, selectedCount, expandedCount,
			selectionChanges, expansionChanges, identityChanges, subtreeAdded, subtreeRemoved);
		if( !parent.worker->Finish(candidate) )
		{
			TreeBatchRetry(parent, 100, "shadow_finish");
			return true;
		}
		TreeBatchRememberState(parent, std::move(candidate.state));
		return true;
	}
	// Keep no per-notification path journal. A single parent flag records that
	// at least one folder action was withheld; the final transition list comes
	// only from the committed and current snapshots. Any ambiguity falls through
	// to Leeyes' provider rescan below.
	if( !parent.forceProviderRescan && parent.folderActionSuppressed
		&& parent.worker->HasContinuousBaseline(candidate)
		&& gFolderAddEventHookInstalled && gFolderDeleteEventHookInstalled )
	{
		std::vector<FolderAction> actions;
		if( TreeBatchBuildFolderActions(window, owner, parent, candidate, actions)
			&& parent.worker->CanApply(candidate)
			&& TreeBatchParentCurrent(window, owner, parent, true) )
		{
			if( actions.empty() )
			{
				LARGE_INTEGER started = {};
				LONGLONG budgetTicks = 0;
				std::vector<TreeChildState> verifiedTreeBaseline;
				UINT parentVisualState = 0;
				if( TreeBatchStartCompareBudget(started, budgetTicks)
					&& TreeBatchVerifyFolderTree(window, owner, parent, candidate.state, actions,
						started, budgetTicks, verifiedTreeBaseline, parentVisualState)
					&& parent.worker->CanApply(candidate) )
				{
					if( parent.worker->Finish(candidate) )
					{
						parent.committedTreeBaseline.swap(verifiedTreeBaseline);
						parent.committedTreeBaselineReady = true;
						parent.committedParentVisualState = parentVisualState;
						TreeBatchRememberState(parent, std::move(candidate.state), true);
					}
					else TreeBatchRetry(parent, 100, "folder_noop_finish");
					return true;
				}
			}
			else
			{
				parent.pendingFolderCandidate = std::move(candidate);
				parent.pendingFolderActions.swap(actions);
				parent.nextFolderAction = 0;
				parent.hasPendingFolderCandidate = true;
				TreeBatchLog("tree_batch_folder_actions parent_hash=%08lX actions=%u added=%u removed=%u",
					static_cast<unsigned long>(parent.nodeIdentity.pathHash),
					static_cast<unsigned>(parent.pendingFolderActions.size()),
					[&]() { unsigned n = 0; for( const auto& a : parent.pendingFolderActions ) if( a.action == 3 ) ++n; return n; }(),
					[&]() { unsigned n = 0; for( const auto& a : parent.pendingFolderActions ) if( a.action == 5 ) ++n; return n; }());
				if( !TreeBatchArmTimer(window, 1) )
					TreeBatchEmergencyRescanOrDisable(parent, owner, window, "folder_slice_timer_arm");
				return true;
			}
		}
	}
	// A current snapshot from a replacement stream is not a continuous delta
	// baseline. Reconcile once through Leeyes' provider before allowing the
	// removal-only shortcut again.
	if( parent.worker->HasContinuousBaseline(candidate)
		&& TreeBatchCanSkipRemovalRescan(window, owner, parent, candidate.state) )
	{
		if( !parent.worker->Finish(candidate) )
		{
			TreeBatchRetry(parent, 100, "norescan_finish");
			return true;
		}
		++parent.noRescanCount;
		TreeBatchLog("tree_batch_no_rescan parent_hash=%08lX entries=%u norescan_total=%llu rescan_skipped_us_estimate=350000",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash),
			static_cast<unsigned>(candidate.state.entries.size()), parent.noRescanCount);
		TreeBatchRememberState(parent, std::move(candidate.state), true);
		return true;
	}
	if( parent.dirty )
		TreeBatchLog("tree_batch_fallback parent_hash=%08lX reason=%s folder_suppressed=%u force_rescan=%u continuous=%u",
			static_cast<unsigned long>(parent.nodeIdentity.pathHash),
			parent.folderActionSuppressed ? "folder_actions_unresolved" : "provider_required",
			parent.folderActionSuppressed ? 1u : 0u, parent.forceProviderRescan ? 1u : 0u,
			parent.worker->HasContinuousBaseline(candidate) ? 1u : 0u);
	// The comparison itself can outlive the watcher generation. Do not start a
	// synchronous UI rescan from a candidate that became stale while comparing.
	if( !parent.worker->CanApply(candidate) || !TreeBatchParentCurrent(window, owner, parent, true) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		parent.refreshRequested = false;
		return false;
	}
	const bool rescanned = TreeBatchProviderRescan(owner, parent);
	if( !rescanned || !TreeBatchParentCurrent(window, owner, parent, true) )
	{
		bool current = false;
		parent.worker->Release(candidate, current);
		if( !TreeBatchParentCurrent(window, owner, parent, true) )
			TreeBatchMarkUnavailable(parent, "rescan_parent_identity");
		else TreeBatchRetry(parent, parent.lastShellError == E_ACCESSDENIED ? 5000 : 1000, "rescan_failed");
		return true;
	}
	if( !parent.worker->Finish(candidate) )
	{
		TreeBatchRetry(parent, 100, "rescan_finish");
		return true;
	}
	TreeBatchRememberState(parent, std::move(candidate.state));
	return true;
}

LRESULT TreeBatchHandleTimer(HWND window)
{
	::KillTimer(window, kTreeBatchTimerId);
	gTimerArmed = false;
	if( gRescanning || window != gTreeWindow || !::IsWindow(window) ) return 0;
	const ULONGLONG now = ::GetTickCount64();
	const ULONGLONG due = gTimerArmedAt + gTimerArmedDelayMs;
	const long long late = static_cast<long long>(now) - static_cast<long long>(due);
	gLastTimerLateMs = late > 0 ? static_cast<ULONGLONG>(late) : 0;
	++gTimerDispatches;
	ULONGLONG suppressedAction2 = 0, suppressedAdd = 0, suppressedDelete = 0;
	for( const auto& parent : gParents )
	{
		suppressedAction2 += parent.suppressedAction2;
		suppressedAdd += parent.suppressedAdd;
		suppressedDelete += parent.suppressedDelete;
	}
	TreeBatchLog("tree_batch_timer dispatch=%llu armed_ms=%u late_ms=%lld suppressed_action2=%llu suppressed_add=%llu suppressed_delete=%llu parents=%u",
		gTimerDispatches, gTimerArmedDelayMs, late, suppressedAction2, suppressedAdd,
		suppressedDelete, static_cast<unsigned>(gParents.size()));
	for( auto& parent : gParents )
	{
		if( parent.unavailable || !parent.worker ) continue;
		if( parent.hasPendingFolderCandidate )
		{
			TreeBatchApplyFolderSlice(parent, gTreeOwner, window);
			break;
		}
		if( parent.dirty && (parent.retryAt > now || parent.quietDeadline > now) ) continue;
		if( TreeBatchProcessParent(parent, gTreeOwner, window, now) ) break;
		if( parent.dirty && parent.refreshRequested ) break;
	}
	TreeBatchScheduleNext();
	return 0;
}

bool TreeBatchShouldSuppress(DWORD pathPointer, DWORD frame, DWORD caller)
{
	if( !gHookInstalled.load(std::memory_order_acquire) || gRescanning || caller != gExpectedCaller ) return false;
	std::wstring fullPath, parentPath;
	if( !ReadDelphiAnsiTreePath(CP_ACP, pathPointer, fullPath)
		|| !TreeBatchSplitParent(fullPath, parentPath) ) return false;
	DWORD owner = 0;
	if( !TreeBatchReadOwner(frame, owner) ) return false;
	if( !gTreeWindow )
	{
		HWND window = nullptr;
		if( !FindTreeViewWindow(owner, ::GetCurrentThreadId(), window) ) return false;
		if( !TreeBatchAttachParent(owner, window, parentPath) ) return false;
		return false;
	}
	if( owner != gTreeOwner || !::IsWindow(gTreeWindow) )
	{
		for( auto& existing : gParents ) TreeBatchMarkUnavailable(existing, "owner_or_window_changed");
		HWND replacement = nullptr;
		if( FindTreeViewWindow(owner, ::GetCurrentThreadId(), replacement) )
			TreeBatchRebindWindow(owner, replacement, parentPath);
		return false;
	}
	HWND window = gTreeWindow;
	ParentSession* parent = nullptr;
	for( auto& candidate : gParents ) if( SamePath(candidate.path, parentPath) ) { parent = &candidate; break; }
	if( !parent )
	{
		TreeBatchAttachParent(owner, window, parentPath);
		return false;
	}
	if( parent->unavailable )
	{
		TreeBatchAttachParent(owner, window, parentPath);
		return false;
	}
	if( parent->baselineReady && !TreeBatchParentUsable(window, owner, *parent, false) )
	{
		TreeBatchMarkUnavailable(*parent, "hook_parent_identity");
		return false;
	}
	if( !parent->baselineReady || !parent->suppressionReady || !parent->worker
		|| parent->worker->Failed() || parent->worker->Error() != ERROR_SUCCESS )
	{
		static ULONGLONG lastForwardLog = 0;
		const ULONGLONG forwardNow = ::GetTickCount64();
		if( forwardNow - lastForwardLog >= 500 )
		{
			lastForwardLog = forwardNow;
			TreeBatchLog("tree_batch_hook_forward parent_key=%08lX baseline=%u suppress_ready=%u worker=%u failed=%u error=%lu flat=%u",
				static_cast<unsigned long>(TreeBatchPathKey(parentPath)),
				parent->baselineReady ? 1u : 0u, parent->suppressionReady ? 1u : 0u,
				parent->worker ? 1u : 0u,
				(parent->worker && parent->worker->Failed()) ? 1u : 0u,
				parent->worker ? static_cast<unsigned long>(parent->worker->Error()) : 0ul,
				parent->flatCandidate ? 1u : 0u);
		}
		parent->dirty = true;
		parent->suppressionReady = false;
		parent->refreshRequested = false;
		parent->quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
		if( !parent->unavailable && !TreeBatchArmTimer(window, 100)
			&& !TreeBatchPostRecovery(window) )
			TreeBatchMarkUnavailable(*parent, "hook_timer_arm");
		return false;
	}
	parent->dirty = true;
	parent->quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
	parent->refreshRequested = false;
	parent->retryAt = 0;
	if( !TreeBatchArmTimer(window, gQuietIntervalMs) )
	{
		if( !TreeBatchPostRecovery(window) )
			TreeBatchMarkUnavailable(*parent, "hook_quiet_timer_arm");
		return false;
	}
	if( gBatchMode == 2 )
	{
		++parent->cycleAction2;
		++parent->suppressedAction2;
	}
	return gBatchMode == 2;
}

DWORD TreeBatchShouldSuppressFolderSearch(DWORD pathPointer, DWORD frame, DWORD caller)
{
	if( !gHookInstalled.load(std::memory_order_acquire) || gRescanning ) return 0;
	const DWORD action = caller == gFolderAddSearchReturnSite ? 3
		: caller == gFolderDeleteSearchReturnSite ? 5 : 0;
	if( !action ) return 0;
	std::wstring fullPath, parentPath;
	if( !TreeBatchReadAnsiPath(pathPointer, fullPath) || !TreeBatchSplitParent(fullPath, parentPath) ) return 0;
	DWORD owner = 0;
	if( !TreeBatchReadOwner(frame, owner) ) return 0;
	if( !gTreeWindow )
	{
		HWND window = nullptr;
		if( FindTreeViewWindow(owner, ::GetCurrentThreadId(), window) )
			TreeBatchAttachParent(owner, window, parentPath);
		return 0;
	}
	if( owner != gTreeOwner || !::IsWindow(gTreeWindow) )
	{
		for( auto& existing : gParents ) TreeBatchMarkUnavailable(existing, "owner_or_window_changed");
		HWND replacement = nullptr;
		if( FindTreeViewWindow(owner, ::GetCurrentThreadId(), replacement) )
			TreeBatchRebindWindow(owner, replacement, parentPath);
		return 0;
	}
	HWND window = gTreeWindow;
	ParentSession* parent = nullptr;
	for( auto& candidate : gParents )
		if( SamePath(candidate.path, parentPath) ) { parent = &candidate; break; }
	if( !parent )
	{
		TreeBatchAttachParent(owner, window, parentPath);
		return 0;
	}
	if( parent->unavailable )
	{
		TreeBatchAttachParent(owner, window, parentPath);
		return 0;
	}
	if( parent->baselineReady && !TreeBatchParentUsable(window, owner, *parent, false) )
	{
		TreeBatchMarkUnavailable(*parent, "hook_folder_parent_identity");
		return 0;
	}
	if( !parent->baselineReady || !parent->suppressionReady || !parent->worker
		|| parent->worker->Failed() || parent->worker->Error() != ERROR_SUCCESS )
	{
		static ULONGLONG lastForwardLog = 0;
		const ULONGLONG forwardNow = ::GetTickCount64();
		if( forwardNow - lastForwardLog >= 500 )
		{
			lastForwardLog = forwardNow;
			TreeBatchLog("tree_batch_hook_forward parent_key=%08lX baseline=%u suppress_ready=%u worker=%u failed=%u error=%lu flat=%u",
				static_cast<unsigned long>(TreeBatchPathKey(parentPath)),
				parent->baselineReady ? 1u : 0u, parent->suppressionReady ? 1u : 0u,
				parent->worker ? 1u : 0u,
				(parent->worker && parent->worker->Failed()) ? 1u : 0u,
				parent->worker ? static_cast<unsigned long>(parent->worker->Error()) : 0ul,
				parent->flatCandidate ? 1u : 0u);
		}
		parent->dirty = true;
		parent->suppressionReady = false;
		parent->refreshRequested = false;
		parent->quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
		if( !parent->unavailable && !TreeBatchArmTimer(window, 100)
			&& !TreeBatchPostRecovery(window) )
			TreeBatchMarkUnavailable(*parent, "hook_folder_timer_arm");
		return 0;
	}
	parent->dirty = true;
	parent->quietDeadline = ::GetTickCount64() + gQuietIntervalMs;
	parent->refreshRequested = false;
	parent->retryAt = 0;
	if( !TreeBatchArmTimer(window, gQuietIntervalMs) )
	{
		if( !TreeBatchPostRecovery(window) )
			TreeBatchMarkUnavailable(*parent, "hook_quiet_folder_timer_arm");
		return 0;
	}
	if( gBatchMode != 2 ) return 0;
	// A flat session only withholds the action 2 inner search. Folder additions
	// and removals would change the directory set (and end the flat condition),
	// so they keep using Leeyes' original path.
	if( parent->flatCandidate ) return 0;
	if( action == 3 ) { ++parent->cycleAdd; ++parent->suppressedAdd; }
	else { ++parent->cycleDelete; ++parent->suppressedDelete; }
	// The state snapshot supplies names and order; retain only this bounded
	// per-parent fact, not the notification path or a replay record.
	TreeBatchNoteFolderActionSuppressed(*parent);
	// In the verified action 3/5 branches, non-null skips insertion and null
	// skips deletion. The outer owner event and common cleanup remain intact.
	return action == 3 ? 2 : 1;
}

extern "C" BOOL __cdecl TreeBatchShouldSuppressSearch(DWORD pathPointer, DWORD frame, DWORD caller)
{
	try { return TreeBatchShouldSuppress(pathPointer, frame, caller) ? TRUE : FALSE; }
	catch( ... ) { return FALSE; }
}

extern "C" __declspec(naked) void TreeBatchSearchHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 36]
		push dword ptr [ebx + 40]
		push dword ptr [ebx + 28]
		call TreeBatchShouldSuppressSearch
		add esp, 12
		test eax, eax
		jnz skip_search
		popad
		popfd
		push dword ptr [esp + 4]
		call dword ptr [gLeeyesTreeBatchOriginalSearch]
		lea esp, [esp + 4]
		ret
	skip_search:
		popad
		popfd
		ret
	}
}

extern "C" DWORD __cdecl TreeBatchShouldSuppressFolderSearchCall(DWORD pathPointer, DWORD frame, DWORD caller)
{
	try { return TreeBatchShouldSuppressFolderSearch(pathPointer, frame, caller); }
	catch( ... ) { return 0; }
}

extern "C" __declspec(naked) void TreeBatchParentSearchHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 36]
		push dword ptr [ebx + 8]
		push dword ptr [ebx + 20]
		call TreeBatchShouldSuppressFolderSearchCall
		add esp, 12
		test eax, eax
		jz call_original
		cmp eax, 1
		jne return_nonnull
		mov dword ptr [ebx + 28], 0
		jmp suppressed
	return_nonnull:
		mov dword ptr [ebx + 28], 1
	suppressed:
		popad
		popfd
		ret
	call_original:
		popad
		popfd
		push edx
		push ecx
		push eax
		call dword ptr [gLeeyesTreeBatchOriginalParentSearch]
		lea esp, [esp + 12]
		ret
	}
}

LRESULT CALLBACK TreeNotificationBatchSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference)
{
	if( message == WM_TIMER && wParam == kTreeBatchTimerId ) return TreeBatchHandleTimer(window);
	if( gRecoveryMessage && message == gRecoveryMessage )
	{
		gRecoveryPosted = false;
		return TreeBatchHandleTimer(window);
	}
	if( message == WM_NCDESTROY )
	{
		TreeBatchCloseWindow(window);
		::RemoveWindowSubclass(window, TreeNotificationBatchSubclass, subclassId);
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

}

bool InstallTreeNotificationBatch(unsigned quietIntervalMs, unsigned mode)
{
	if( gHookInstalled.load(std::memory_order_acquire) ) return true;
	if( mode < 1 || mode > 2 )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "mode_out_of_range");
		return false;
	}
	gBatchLog = TreeBatchReadLogSetting();
	if( !VerifyLeeyesTreeLayout() )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "layout_guard");
		TreeBatchLog("tree_batch_install_failed reason=layout_guard");
		return false;
	}
	if( !gRecoveryMessage ) gRecoveryMessage = ::RegisterWindowMessageW(L"LeeyesUnicodeHack.TreeBatch.Recovery");
	if( !gRecoveryMessage )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "recovery_message");
		TreeBatchLog("tree_batch_install_failed reason=recovery_message");
		return false;
	}
	if( !GetLeeyesFolderSearchReturnSites(gFolderAddSearchReturnSite, gFolderDeleteSearchReturnSite) )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "folder_search_sites");
		TreeBatchLog("tree_batch_install_failed reason=folder_search_sites");
		return false;
	}
	if( !InstallTreeProviderRescanHooks() )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "provider_rescan_hooks");
		TreeBatchLog("tree_batch_install_failed reason=provider_rescan_hooks");
		return false;
	}
	if( quietIntervalMs < 50 || quietIntervalMs > 5000 ) quietIntervalMs = 300;
	gQuietIntervalMs = quietIntervalMs;
	gBatchMode = mode;
	if( mode == 2 && !TreeBatchInstallFolderEventHooks() )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "folder_owner_event_sites");
		TreeBatchLog("tree_batch_install_failed reason=folder_owner_event_sites");
		return false;
	}
	gLeeyesTreeBatchOriginalParentSearch = LeeyesInternalCreateHook(reinterpret_cast<void*>(0x004CDD20),
		reinterpret_cast<void*>(&TreeBatchParentSearchHook));
	if( !gLeeyesTreeBatchOriginalParentSearch )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "parent_search_hook");
		TreeBatchLog("tree_batch_install_failed reason=parent_search_hook");
		TreeBatchRemoveFolderEventHooks();
		return false;
	}
	gLeeyesTreeBatchOriginalSearch = LeeyesInternalCreateHook(reinterpret_cast<void*>(0x004F65CC),
		reinterpret_cast<void*>(&TreeBatchSearchHook));
	if( !gLeeyesTreeBatchOriginalSearch )
	{
		StartupDiagnosticLog("tree_batch_failed_detail", "inner_search_hook");
		TreeBatchLog("tree_batch_install_failed reason=inner_search_hook");
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&TreeBatchParentSearchHook));
		gLeeyesTreeBatchOriginalParentSearch = nullptr;
		TreeBatchRemoveFolderEventHooks();
		return false;
	}
	gHookInstalled.store(true, std::memory_order_release);
	TreeBatchLog("tree_batch_installed mode=%u quiet_ms=%u add_event_site=%08lX delete_event_site=%08lX add_search_return=%08lX delete_search_return=%08lX log=%u",
		gBatchMode, gQuietIntervalMs, static_cast<unsigned long>(gFolderAddEventCallSite),
		static_cast<unsigned long>(gFolderDeleteEventCallSite),
		static_cast<unsigned long>(gFolderAddSearchReturnSite),
		static_cast<unsigned long>(gFolderDeleteSearchReturnSite), gBatchLog ? 1u : 0u);
	return true;
}

}
