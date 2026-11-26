#include "../stdafx.h"
#include "file_move_batch.h"
#include "../../common/ansi_path_conversion.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <CommCtrl.h>
#pragma comment(lib, "comctl32.lib")
#include <algorithm>
#include <iterator>
#include <limits.h>
#include <set>
#include <new>
#include "../../common/watched_sync_worker.h"
#if UNICODEHACK_PATH_DEBUG
#include "../../common/state_sync_worker.h"
#include "../../common/notification_scope.h"
#include "../../common/notification_fallback.h"
#include <atomic>
#endif

extern HMODULE gInjectedModule;
extern "C" void __cdecl LeeyesInternalInvokeFullResync(void* listView);
extern "C" BOOL __cdecl LeeyesInternalInvokeResync(void* listView, BOOL incremental);
extern "C" volatile LONG gLeeyesInternalLowItemNullFallbackCount;
extern "C" volatile LONG gLeeyesInternalLowItemGuardActive;
#if UNICODEHACK_PATH_DEBUG
extern "C" void __cdecl LeeyesEnumerationDiagnosticSnapshot(const char* stage, DWORD queued);
extern "C" void __cdecl LeeyesTreeShadowBegin();
extern "C" void __cdecl LeeyesTreeShadowEnd();
extern "C" BOOL __cdecl LeeyesDiagnosticInvokeResync(void* listView, BOOL incremental);
extern "C" BOOL __cdecl LeeyesDiagnosticReplayAnsi(void* owner, DWORD action,
	const char* first, DWORD firstLength, const char* second, DWORD secondLength);
extern "C" BOOL __cdecl LeeyesDiagnosticReplayMethod(void* method, void* owner, DWORD action,
	const char* first, DWORD firstLength, const char* second, DWORD secondLength);
extern "C" void* __cdecl LeeyesDiagnosticMakeAnsi(const char* bytes, DWORD length);
extern "C" void __cdecl LeeyesDiagnosticClearAnsi(void** value);
extern "C" BOOL __cdecl LeeyesDiagnosticReplayOwned(void* method, void* owner,
	DWORD action, void* first, void* second);
extern "C" void __cdecl LeeyesDiagnosticTreeStateChanged();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeForceFallback();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchAttach(DWORD owner);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchAttachPaths(DWORD owner, const wchar_t* first, const wchar_t* second);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchResolve(BOOL applied, HWND window, BOOL allowRepeat);
extern "C" void __cdecl LeeyesDiagnosticTreeIdentityAfterReplay(HWND window, DWORD owner);
extern "C" void __cdecl LeeyesDiagnosticTreeBatchCancelled();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeCaptureRecovery(HWND* window, std::wstring* first, std::wstring* second);
extern "C" DWORD __cdecl LeeyesDiagnosticTreeRequestedParent(DWORD owner, HWND window, HTREEITEM item,
	const wchar_t* first, const wchar_t* second);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeFixtureParentAdd(DWORD owner, HWND window, void* method, const wchar_t* path);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeParentIntentMatches(DWORD owner, HWND window, const wchar_t* first, const wchar_t* second,
	DWORD volume1, ULONGLONG id1, DWORD volume2, ULONGLONG id2, BOOL requireCandidate, DWORD* mask, ULONGLONG* generation);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeTestReplaceParentIntentGeneration();
extern "C" BOOL __cdecl LeeyesDiagnosticCaptureTree(void* method, void* owner, DWORD action, DWORD path0, DWORD path1);
extern "C" void __cdecl LeeyesStateShadowPosted(DWORD record);
#endif

namespace LeeyesInternal
{
namespace
{

const DWORD kMaximumDirtyLists = 8;
const DWORD kMaximumScopeDirectories = 4;
const size_t kScopePathCapacity = MAX_PATH * 4;

struct ScopeDirectoryResult
{
	char path[kScopePathCapacity];
	bool displayed;
};

struct DrainState
{
	DWORD depth;
	bool suppress;
	bool needsResync;
	bool currentOwnerKnown;
	DWORD queuedChanges;
	DWORD suppressedChanges;
	DWORD outOfScopeChanges;
	DWORD unknownScopeChanges;
	DWORD ownerProbeAttempts;
	DWORD sampledChanges;
	void* currentOwner;
	HWND currentListWindow;
	bool currentDirectoryKnown;
	bool currentDirectoryProbeAttempted;
	bool listStateWatchAttempted;
	char currentDirectory[kScopePathCapacity];
	ScopeDirectoryResult scopeDirectories[kMaximumScopeDirectories];
	DWORD scopeDirectoryCount;
	#if UNICODEHACK_PATH_DEBUG
	wchar_t pidlDirectory[MAX_PATH];
	bool pidlDirectoryKnown;
	DWORD pidlSamples;
	bool unchangedSkipEligible;
	bool unchangedRejectLogged;
	#endif
	void* dirtyLists[kMaximumDirtyLists];
	DWORD dirtyListCount;
};

DWORD gBatchThreshold = 32;
bool gDiagnosticLog = false;
bool gListDeleteBatchEnabled = false;
__declspec(thread) DrainState gDrainState = {};

struct ListWindowSearch
{
	DWORD processId;
	HWND bestWindow;
	int bestItemCount;
};

struct DisplayedListSearch
{
	DWORD processId;
	HWND listWindow;
	LONG_PTR windowUserData;
	DWORD listCount;
	DWORD visibleListCount;
	DWORD hiddenListCount;
};

struct ListSelectionSnapshot
{
	HWND window;
	std::map<std::string, DWORD> selectedNames;
	std::string focusedName;
	std::string topName;
	int originalTopIndex;
	int lastSelectedIndex;
	int originalFocusedIndex;
	bool focusedWasVisible;
};

void ObserveListDeleteBatch(void* owner, HWND window, const char* directory);
bool DeferListDeleteBatchResync(void* owner, HWND window, const char* directory);
void CompleteListDeleteBatchLegacyResync(void* owner, HWND window, const char* directory, bool legacyResynced);
LRESULT CALLBACK ListDeleteBatchSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR referenceData);

#if UNICODEHACK_PATH_DEBUG
bool TreeDiagnosticBufferActive();
bool BufferTreeBatchLog(const char* data, size_t length);
void FlushTreeDiagnosticLogs();
void WriteDiagnostic(const char* format, ...)
{
	if( !gDiagnosticLog || !gInjectedModule ) return;
	if( ::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_QUIET", nullptr, 0)
		&& strncmp(format, "state_shadow ", 13) != 0
		&& strncmp(format, "selection_find_shadow ", 22) != 0
		&& strncmp(format, "selection_verify ", 17) != 0 ) return;
	if( TreeDiagnosticBufferActive() )
	{
		char message[2048] = {};
		va_list arguments;
		va_start(arguments, format);
		const int length = _vsnprintf_s(message, _countof(message), _TRUNCATE, format, arguments);
		va_end(arguments);
		if( length >= 0 && BufferTreeBatchLog(message, static_cast<size_t>(length)) ) return;
	}
	else if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_BUFFER_DIAGNOSTICS", nullptr, 0) ) FlushTreeDiagnosticLogs();
	char path[MAX_PATH * 2] = {};
	if( !::GetModuleFileNameA(gInjectedModule, path, _countof(path)) ) return;
	char* dot = strrchr(path, '.');
	if( !dot ) return;
	strcpy_s(dot, _countof(path) - static_cast<size_t>(dot - path),
		"_file_move_batch.txt");
	FILE* log = nullptr;
	if( fopen_s(&log, path, "a") != 0 || !log ) return;
	va_list arguments;
	va_start(arguments, format);
	vfprintf(log, format, arguments);
	va_end(arguments);
	fputc('\n', log);
	fclose(log);
}

// Opt-in diagnostic only. The module is pinned before the worker starts so
// its code cannot be unloaded while a timed-out child is being cleaned up.
// The worker is intentionally process-lifetime; normal builds never create it.
std::atomic<StateSyncWorker*> gShadowWorker(nullptr);
// Independent watcher is observational until the list adapter can prove that
// a snapshot and the displayed Shell list have the same semantics.
std::atomic<WatchedSyncWorker*> gListWatchedShadow(nullptr);
std::atomic<unsigned long> gShadowPosts(0);
std::atomic<unsigned long long> gTreePostedGeneration(0);
std::atomic<unsigned long long> gTreeLastPostedTick(0);
std::atomic<unsigned long> gShadowScopeRelated(0), gShadowScopeDisjoint(0), gShadowScopeUnknown(0);
// Counters only: the diagnostic does not write observed names or paths.
std::atomic<unsigned long> gTreePostFixture(0), gTreePostDiagnostic(0), gTreePostOther(0), gTreePostUnreadable(0);
std::atomic<unsigned long> gTreePostParent(0), gTreePostUser(0), gTreePostRepo(0), gTreePostModels(0), gTreePostRemaining(0);
std::atomic<unsigned long> gTreePostSkipped(0);
std::atomic<unsigned long> gTreeTestLateIgnored(0);
std::atomic<DWORD> gShadowOwner(0);
std::atomic<bool> gShadowNavigation(false);
SRWLOCK gShadowScopeLock = SRWLOCK_INIT;
wchar_t gShadowScopeTarget[MAX_PATH] = {};
std::wstring gShadowTarget;
std::unique_ptr<DirectorySnapshot::State> gShadowLastState;
std::unique_ptr<DirectorySnapshot::State> gShadowAppliedState;
std::vector<std::wstring> gShadowAppliedNames;
bool gShadowAppliedIndexReady = false;
bool ShadowNameLess(const std::wstring& a, const std::wstring& b)
{
	return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
		b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}
void InvalidateShadowApplied()
{
	gShadowAppliedState.reset();
	gShadowAppliedNames.clear();
	gShadowAppliedIndexReady = false;
}
bool gShadowListComparisonDone = false;
struct ReplaySingleDiagnostic
{
	void* owner;
	void* method;
	HWND window;
	DWORD action, firstLength, secondLength;
	char first[kScopePathCapacity], second[kScopePathCapacity];
};
__declspec(thread) ReplaySingleDiagnostic gReplaySingle = {};
__declspec(thread) ReplaySingleDiagnostic gReplayTimer = {};
__declspec(thread) unsigned long gReplayBatchSerial = 0;
struct TreeRecoveryIdentity
{
	DWORD volume1 = 0, volume2 = 0;
	ULONGLONG id1 = 0, id2 = 0;
	bool known = false;
};
struct ReplayBatchDiagnostic
{
	struct OwnedPaths { void* first; void* second; };
	ReplayBatchDiagnostic(void* m, void* o, size_t limit, unsigned actionLimitValue = 5)
		: method(m), owner(o), journal(limit, 1024 * 1024, actionLimitValue), actionLimit(actionLimitValue), serial(++gReplayBatchSerial) {}
	~ReplayBatchDiagnostic()
	{
		for( OwnedPaths& paths : owned )
		{
			LeeyesDiagnosticClearAnsi(&paths.first);
			LeeyesDiagnosticClearAnsi(&paths.second);
		}
	}
	void* method;
	void* owner;
	NotificationFallback journal;
	unsigned actionLimit;
	unsigned long serial;
	std::vector<OwnedPaths> owned;
	HWND timerWindow = nullptr;
	bool timerScheduled = false;
	bool streamingStarted = false;
	bool fallbackPending = false;
	bool replaying = false;
	bool allowRepeat = true;
	bool abandoned = false;
	bool discoverParents = false;
	bool parentsAttached = false;
	bool discoveryWaiting = false;
	bool orderedPrefix = false;
	bool recoveryOnly = false;
	bool recoveryRetryAfterInput = false;
	bool recoveryRetryAfterChange = false;
	bool recoveryRetryChangePreflight = false;
	ULONGLONG recoveryIntentGeneration = 0;
	bool recoveryRetryAfterReadFailure = false;
	bool recoveryRetryAfterIdentityMismatch = false;
	bool recoveryReadRetry = false;
	bool recoveryReadoptFixture = false;
	bool recoveryRestoreFixtureChild = false;
	bool recoveryInterrupted = false;
	bool recoveryInputOnlyShift = true;
	unsigned recoveryInputKinds = 0;
	DWORD recoveryExpandedMask = 0;
	std::vector<std::wstring> discoveredParents;
	std::wstring testDeferredParent;
	std::wstring recoveryParent, recoveryParent2;
	TreeRecoveryIdentity recoveryIdentity;
	unsigned reentered = 0;
	unsigned drains = 1;
	unsigned captured = 0, replayed = 0;
	unsigned ownedBase = 0;
	unsigned appendedWhileStreaming = 0;
	unsigned pressureSlices = 0, pressureReplayed = 0;
	unsigned acceleratedSlices = 0;
	unsigned orderedReplayed = 0;
	unsigned slices = 0;
	ULONGLONG maxSliceMs = 0;
	unsigned actions[7] = {};
	unsigned long long capturedHash = 1469598103934665603ULL;
	unsigned long long replayedHash = 1469598103934665603ULL;
};
__declspec(thread) ReplayBatchDiagnostic* gReplayBatch = nullptr;
__declspec(thread) bool gReplayBatchDone = false;
__declspec(thread) bool gStateBatchResolving = false;
struct DeferredTreeRecovery
{
	HWND window = nullptr;
	void* owner = nullptr;
	void* method = nullptr;
	std::wstring first, second;
	unsigned long serial = 0;
	bool active = false;
	bool restoreFixtureChild = false;
	DWORD restoreExpandedMask = 0;
	unsigned long inputEpoch = 0;
	unsigned inputDepth = 0;
	bool automaticReadRetry = false;
	bool readoptFixture = false;
	bool validateParentIntent = false;
	ULONGLONG parentIntentGeneration = 0;
	TreeRecoveryIdentity identity;
	bool preflightNeeded = false, preflightStarted = false, preflightHave1 = false, preflightHave2 = false;
	bool preflightTestStall = false;
	ULONGLONG preflightDeadline = 0;
	WatchedSyncWorker::Candidate preflight1, preflight2;
};
thread_local DeferredTreeRecovery gDeferredTreeRecovery;
// Diagnostic process-lifetime workers: never join them on the UI thread.
thread_local WatchedSyncWorker* gDeferredIdentityWorker1 = nullptr;
thread_local WatchedSyncWorker* gDeferredIdentityWorker2 = nullptr;
__declspec(thread) unsigned long gDeferredTreeRecoverySerial = 0;
void ClearDeferredIdentityPreflight()
{
	if( gDeferredIdentityWorker1 )
	{
		if( gDeferredTreeRecovery.preflightHave1 ) gDeferredIdentityWorker1->Finish(gDeferredTreeRecovery.preflight1);
		try { gDeferredIdentityWorker1->SetTarget(L""); } catch( ... ) {}
	}
	if( gDeferredIdentityWorker2 )
	{
		if( gDeferredTreeRecovery.preflightHave2 ) gDeferredIdentityWorker2->Finish(gDeferredTreeRecovery.preflight2);
		try { gDeferredIdentityWorker2->SetTarget(L""); } catch( ... ) {}
	}
	gDeferredTreeRecovery.preflightStarted = gDeferredTreeRecovery.preflightHave1 = gDeferredTreeRecovery.preflightHave2 = false;
	gDeferredTreeRecovery.preflightTestStall = false;
	gDeferredTreeRecovery.preflightDeadline = 0;
}
bool StartDeferredIdentityPreflight(const std::wstring& first, const std::wstring& second)
{
	try
	{
		wchar_t launcher[MAX_PATH * 4] = {};
		const DWORD length = ::GetModuleFileNameW(gInjectedModule, launcher, _countof(launcher));
		if( !length || length >= _countof(launcher) ) return false;
		wchar_t* slash = wcsrchr(launcher, L'\\');
		if( !slash || wcscpy_s(slash + 1, _countof(launcher) - (slash + 1 - launcher), L"LeeyesW.exe") ) return false;
		HMODULE pinned = nullptr;
		if( !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&StartDeferredIdentityPreflight), &pinned) ) return false;
		WatchedSyncWorker** workers[] = { &gDeferredIdentityWorker1, &gDeferredIdentityWorker2 };
		for( auto slot : workers )
		{
			if( !*slot )
			{
				WatchedSyncWorker* worker = new (std::nothrow) WatchedSyncWorker(launcher);
				if( !worker || !worker->Start() ) { delete worker; return false; }
				*slot = worker;
			}
			if( (*slot)->Failed() ) return false;
		}
		if( !gDeferredIdentityWorker1->SetTarget(first) || !gDeferredIdentityWorker2->SetTarget(second) )
		{ ClearDeferredIdentityPreflight(); return false; }
		gDeferredTreeRecovery.preflightStarted = true;
		static bool injectedStall = false;
		gDeferredTreeRecovery.preflightTestStall = !injectedStall
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_PREFLIGHT_STALL_ONCE", nullptr, 0);
		if( gDeferredTreeRecovery.preflightTestStall ) injectedStall = true;
		gDeferredTreeRecovery.preflightDeadline = ::GetTickCount64() + (gDeferredTreeRecovery.preflightTestStall ? 2000 : 10000);
		WriteDiagnostic("state_shadow tree_identity_preflight_started=1");
		return true;
	}
	catch( ... ) { ClearDeferredIdentityPreflight(); return false; }
}
struct TreeRecoveryBackoff
{
	void* owner = nullptr;
	std::wstring first, second;
	ULONGLONG lastChange = 0;
	UINT delay = 300;
};
thread_local TreeRecoveryBackoff gTreeRecoveryBackoff;
thread_local TreeRecoveryBackoff gTreeReadFailureBackoff;
UINT TreeRecoveryChangeDelay(void* owner, const std::wstring& first, const std::wstring& second)
{
	if( !::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_BACKOFF", nullptr, 0) ) return 300;
	const ULONGLONG now = ::GetTickCount64();
	try
	{
		const bool repeated = gTreeRecoveryBackoff.owner == owner && gTreeRecoveryBackoff.first == first
			&& gTreeRecoveryBackoff.second == second && now - gTreeRecoveryBackoff.lastChange < 5000;
		const UINT delay = repeated ? (std::min)(2000u, gTreeRecoveryBackoff.delay * 2) : 300u;
		gTreeRecoveryBackoff.owner = owner; gTreeRecoveryBackoff.first = first; gTreeRecoveryBackoff.second = second;
		gTreeRecoveryBackoff.lastChange = now; gTreeRecoveryBackoff.delay = delay;
		return delay;
	}
	catch( ... ) { return 300; }
}
UINT TreeRecoveryReadFailureDelay(void* owner, const std::wstring& first, const std::wstring& second)
{
	const ULONGLONG now = ::GetTickCount64();
	try
	{
		const bool repeated = gTreeReadFailureBackoff.owner == owner && gTreeReadFailureBackoff.first == first
			&& gTreeReadFailureBackoff.second == second && now - gTreeReadFailureBackoff.lastChange < 60000;
		const UINT delay = repeated ? (std::min)(30000u, gTreeReadFailureBackoff.delay * 2) : 1000u;
		gTreeReadFailureBackoff.owner = owner; gTreeReadFailureBackoff.first = first; gTreeReadFailureBackoff.second = second;
		gTreeReadFailureBackoff.lastChange = now; gTreeReadFailureBackoff.delay = delay;
		return delay;
	}
	catch( ... ) { return 1000; }
}
LRESULT CALLBACK DeferredTreeRecoverySubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference);
bool DeferTreeRecovery(HWND window, void* owner, void* method, const std::wstring& first, const std::wstring& second,
	bool restoreFixtureChild = false, DWORD restoreExpandedMask = 0, bool automaticReadRetry = false,
	const TreeRecoveryIdentity& identity = TreeRecoveryIdentity(), bool readoptFixture = false)
{
	if( !window || !::IsWindow(window) || !owner || !method || first.empty() || second.empty()
		|| gDeferredTreeRecovery.active ) return false;
	try
	{
		gDeferredTreeRecovery.first = first; gDeferredTreeRecovery.second = second;
		gDeferredTreeRecovery.window = window; gDeferredTreeRecovery.owner = owner; gDeferredTreeRecovery.method = method;
		gDeferredTreeRecovery.restoreFixtureChild = restoreFixtureChild;
		gDeferredTreeRecovery.restoreExpandedMask = restoreExpandedMask;
		gDeferredTreeRecovery.automaticReadRetry = automaticReadRetry;
		gDeferredTreeRecovery.identity = identity;
		gDeferredTreeRecovery.readoptFixture = readoptFixture;
		gDeferredTreeRecovery.serial = ++gDeferredTreeRecoverySerial;
		const UINT_PTR id = reinterpret_cast<UINT_PTR>(&gDeferredTreeRecovery);
		if( !::SetWindowSubclass(window, DeferredTreeRecoverySubclass, id, 0) )
		{ gDeferredTreeRecovery = DeferredTreeRecovery(); return false; }
		gDeferredTreeRecovery.active = true;
		WriteDiagnostic("state_shadow tree_recovery_deferred_until_input=1");
		return true;
	}
	catch( ... ) { gDeferredTreeRecovery = DeferredTreeRecovery(); return false; }
}
struct BufferedTreeDiagnostics
{
	std::string batch, trace;
	std::wstring tracePath;
};
thread_local BufferedTreeDiagnostics gBufferedTreeDiagnostics;
bool TreeDiagnosticBufferActive()
{
	return ::GetEnvironmentVariableA("LEYEES_TREE_TEST_BUFFER_DIAGNOSTICS", nullptr, 0)
		&& (gReplayBatch || (gDrainState.depth && gDrainState.suppress));
}
bool BufferTreeBatchLog(const char* data, size_t length)
{
	try
	{
		if( gBufferedTreeDiagnostics.batch.size() + length + 1 > 1024 * 1024 ) return false;
		gBufferedTreeDiagnostics.batch.append(data, length);
		gBufferedTreeDiagnostics.batch.push_back('\n');
		return true;
	}
	catch( ... ) { return false; }
}
extern "C" BOOL __cdecl LeeyesDiagnosticBufferTreeTrace(const wchar_t* path, const char* data, DWORD length)
{
	if( !TreeDiagnosticBufferActive() || !path || !data ) return FALSE;
	try
	{
		if( gBufferedTreeDiagnostics.trace.size() + length > 1024 * 1024
			|| (!gBufferedTreeDiagnostics.tracePath.empty() && gBufferedTreeDiagnostics.tracePath != path) ) return FALSE;
		gBufferedTreeDiagnostics.tracePath = path;
		gBufferedTreeDiagnostics.trace.append(data, length);
		return TRUE;
	}
	catch( ... ) { return FALSE; }
}
void FlushTreeDiagnosticLogs()
{
	if( TreeDiagnosticBufferActive() ) return;
	if( !gBufferedTreeDiagnostics.batch.empty() )
	{
		char path[MAX_PATH * 2] = {};
		if( ::GetModuleFileNameA(gInjectedModule, path, _countof(path)) )
		{
			char* dot = strrchr(path, '.');
			if( dot && !strcpy_s(dot, _countof(path) - static_cast<size_t>(dot - path), "_file_move_batch.txt") )
			{
				FILE* log = nullptr;
				if( !fopen_s(&log, path, "a") && log )
				{ fwrite(gBufferedTreeDiagnostics.batch.data(), 1, gBufferedTreeDiagnostics.batch.size(), log); fclose(log); gBufferedTreeDiagnostics.batch.clear(); }
			}
		}
	}
	if( !gBufferedTreeDiagnostics.trace.empty() && !gBufferedTreeDiagnostics.tracePath.empty() )
	{
		HANDLE file = ::CreateFileW(gBufferedTreeDiagnostics.tracePath.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if( file != INVALID_HANDLE_VALUE )
		{
			DWORD written = 0;
			const DWORD length = static_cast<DWORD>(gBufferedTreeDiagnostics.trace.size());
			const BOOL ok = ::WriteFile(file, gBufferedTreeDiagnostics.trace.data(), length, &written, nullptr);
			::CloseHandle(file);
			if( ok && written == length ) { gBufferedTreeDiagnostics.trace.clear(); gBufferedTreeDiagnostics.tracePath.clear(); }
		}
	}
}
bool CompactTreeBatch(ReplayBatchDiagnostic* batch, bool force)
{
	const size_t acknowledged = batch->journal.AcknowledgedCount();
	if( !batch->streamingStarted || !acknowledged
		|| (!force && acknowledged < 128 && !batch->journal.NearCapacity(2 * kScopePathCapacity))
		|| batch->replayed < batch->ownedBase || acknowledged != batch->replayed - batch->ownedBase
		|| acknowledged > batch->owned.size() ) return false;
	const size_t retired = batch->journal.CompactAcknowledged();
	if( !retired ) return false;
	batch->owned.erase(batch->owned.begin(), batch->owned.begin() + retired);
	batch->ownedBase += static_cast<unsigned>(retired);
	if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_COMPACT_LOG", nullptr, 0) )
		WriteDiagnostic("state_shadow tree_batch_compacted=%lu retired_total=%u", static_cast<unsigned long>(retired), batch->ownedBase);
	return true;
}
LRESULT CALLBACK ReplayBatchSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR);
extern "C" void __cdecl LeeyesDiagnosticTreeBatchAbandon()
{
	ReplayBatchDiagnostic* batch = gReplayBatch;
	if( !batch ) return;
	if( batch->replaying ) { batch->abandoned = true; return; }
	if( batch->timerScheduled )
	{
		const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayBatch);
		::KillTimer(batch->timerWindow, timerId);
		::RemoveWindowSubclass(batch->timerWindow, ReplayBatchSubclass, timerId);
	}
	WriteDiagnostic("state_shadow tree_batch_abandoned=1 captured=%u replayed=%u", batch->captured, batch->replayed);
	gReplayBatch = nullptr; gReplayBatchDone = true; delete batch;
	FlushTreeDiagnosticLogs();
}
unsigned long long ReplayHash(unsigned long long hash, unsigned action,
	const std::string& first, const std::string& second)
{
	hash = (hash ^ action) * 1099511628211ULL;
	for( unsigned char c : first ) hash = (hash ^ c) * 1099511628211ULL;
	hash = (hash ^ 0xff) * 1099511628211ULL;
	for( unsigned char c : second ) hash = (hash ^ c) * 1099511628211ULL;
	return (hash ^ 0xfe) * 1099511628211ULL;
}
void ReplayBatchSlice(ReplayBatchDiagnostic* batch, unsigned limit, ULONGLONG budgetMs)
{
	const ULONGLONG started = ::GetTickCount64();
	unsigned processed = 0;
	batch->replaying = true;
	while( const NotificationFallback::Entry* entry = batch->journal.Next() )
	{
		if( processed >= limit || (processed && ::GetTickCount64() - started >= budgetMs) ) break;
		if( batch->replayed < batch->ownedBase || batch->replayed - batch->ownedBase >= batch->owned.size() ) break;
		ReplayBatchDiagnostic::OwnedPaths& paths = batch->owned[batch->replayed - batch->ownedBase];
		const bool trace = ::GetEnvironmentVariableA("LEYEES_TREE_TRACE_REPLAY", nullptr, 0) != 0;
		if( trace ) WriteDiagnostic("state_shadow tree_batch_replay_begin=%u action=%u", batch->replayed, entry->action);
		const BOOL dispatched = LeeyesDiagnosticReplayOwned(batch->method, batch->owner, entry->action,
			paths.first, paths.second);
		if( trace ) WriteDiagnostic("state_shadow tree_batch_replay_end=%u dispatched=%d", batch->replayed, dispatched);
		if( !dispatched ) break;
		if( batch->abandoned ) break;
		batch->replayedHash = ReplayHash(batch->replayedHash, entry->action, entry->first, entry->second);
		LeeyesDiagnosticClearAnsi(&paths.first);
		LeeyesDiagnosticClearAnsi(&paths.second);
		++batch->replayed;
		++processed;
		if( !batch->journal.Acknowledge() ) break;
	}
	batch->replaying = false;
	++batch->slices;
	const ULONGLONG elapsed = ::GetTickCount64() - started;
	if( elapsed > batch->maxSliceMs ) batch->maxSliceMs = elapsed;
}
bool FlushReplayBatch(const char* reason)
{
	ReplayBatchDiagnostic* batch = gReplayBatch;
	if( !batch || batch->replaying ) return false;
	HWND changedWindow = nullptr;
	std::wstring changedFirst, changedSecond;
	const bool retryChanged = (batch->recoveryOnly
		|| ::GetEnvironmentVariableA("LEYEES_TREE_TEST_NONRECOVERY_RETRY_CHANGE", nullptr, 0))
		&& !strcmp(reason, "related_or_unknown_event")
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_RETRY_CHANGE", nullptr, 0)
		&& LeeyesDiagnosticTreeCaptureRecovery(&changedWindow, &changedFirst, &changedSecond);
	if( !gStateBatchResolving && ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0) )
		LeeyesDiagnosticTreeBatchCancelled();
	if( batch->timerScheduled )
	{
		const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayBatch);
		::KillTimer(batch->timerWindow, timerId);
		::RemoveWindowSubclass(batch->timerWindow, ReplayBatchSubclass, timerId);
		batch->timerScheduled = false;
	}
	batch->journal.Seal();
	if( !batch->journal.Empty() ) ReplayBatchSlice(batch, UINT_MAX, MAXULONGLONG);
	const bool replayed = batch->journal.Replayed() && batch->capturedHash == batch->replayedHash;
	std::wstring recovery1, recovery2;
	DWORD recoveryExpandedMask = batch->recoveryExpandedMask;
	const bool recoveryRetryAfterInput = batch->recoveryOnly && batch->recoveryRetryAfterInput;
	const bool recoveryRetryAfterChange = batch->recoveryOnly && batch->recoveryRetryAfterChange;
	const bool recoveryRetryChangePreflight = recoveryRetryAfterChange && batch->recoveryRetryChangePreflight;
	const ULONGLONG recoveryIntentGeneration = batch->recoveryIntentGeneration;
	const bool recoveryRetryAfterReadFailure = batch->recoveryOnly && batch->recoveryRetryAfterReadFailure;
	const bool recoveryRetryAfterIdentityMismatch = batch->recoveryOnly && batch->recoveryRetryAfterIdentityMismatch;
	const bool restoreFixtureChild = batch->recoveryRestoreFixtureChild;
	const TreeRecoveryIdentity recoveryIdentity = batch->recoveryIdentity;
	const bool readoptFixture = batch->recoveryReadoptFixture;
	const HWND recoveryWindow = batch->timerWindow;
	void* recoveryOwner = batch->owner;
	void* recoveryMethod = batch->method;
	const bool recoveryInterrupted = batch->recoveryInterrupted && !batch->recoveryParent.empty();
	const unsigned recoveryInputKinds = batch->recoveryInputKinds;
	const bool shiftOnly = recoveryInterrupted && batch->recoveryInputOnlyShift && recoveryInputKinds == 1
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_REPLAY_SHIFT_INTENT", nullptr, 0);
	std::wstring interruptedFirst, interruptedSecond;
	if( replayed && !batch->abandoned && recoveryInterrupted && !batch->recoveryOnly
		&& !strcmp(reason, "timer_sliced") && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RETRY_REPLAY_INPUT", nullptr, 0) )
	{
		try { interruptedFirst = batch->recoveryParent; interruptedSecond = batch->recoveryParent2; }
		catch( ... ) { interruptedFirst.clear(); interruptedSecond.clear(); }
	}
	if( replayed && !batch->abandoned && (!batch->recoveryOnly || recoveryRetryAfterInput || recoveryRetryAfterChange || recoveryRetryAfterReadFailure)
		&& !recoveryInterrupted && !strcmp(reason, "timer_sliced")
		&& !batch->recoveryParent.empty() && !batch->recoveryParent2.empty() )
	{
		try { recovery1 = batch->recoveryParent; recovery2 = batch->recoveryParent2; }
		catch( ... ) { recovery1.clear(); recovery2.clear(); }
	}
	WriteDiagnostic("state_shadow tree_batch_replay reason=%s captured=%u replayed=%u equal=%d reentered=%u drains=%u slices=%u appended_during_slices=%u max_slice_ms=%llu actions=%u,%u,%u,%u,%u,%u action6=%u pressure_slices=%u pressure_replayed=%u accelerated_slices=%u ordered_prefix=%u",
		reason, batch->captured, batch->replayed,
		replayed ? 1 : 0, batch->reentered,
		batch->drains, batch->slices, batch->appendedWhileStreaming, batch->maxSliceMs,
		batch->actions[0], batch->actions[1], batch->actions[2], batch->actions[3], batch->actions[4], batch->actions[5], batch->actions[6],
		batch->pressureSlices, batch->pressureReplayed, batch->acceleratedSlices, batch->orderedReplayed);
	gReplayBatch = nullptr;
	gReplayBatchDone = !(replayed && batch->allowRepeat && ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0)
		&& ::GetEnvironmentVariableA("LEYEES_TREE_REPEAT_BATCH", nullptr, 0));
	delete batch;
	FlushTreeDiagnosticLogs();
	if( replayed && !recovery1.empty() && !recovery2.empty()
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_REPLACEMENT_CHILDREN", nullptr, 0) )
		LeeyesDiagnosticTreeIdentityAfterReplay(recoveryWindow, reinterpret_cast<DWORD>(recoveryOwner));
	if( retryChanged && replayed && !gReplayBatch
		&& DeferTreeRecovery(changedWindow, recoveryOwner, recoveryMethod, changedFirst, changedSecond, restoreFixtureChild, 0, false, recoveryIdentity, readoptFixture) )
	{
		const UINT_PTR id = reinterpret_cast<UINT_PTR>(&gDeferredTreeRecovery);
		const UINT delay = TreeRecoveryChangeDelay(recoveryOwner, changedFirst, changedSecond);
		if( ::SetTimer(changedWindow, id, delay, nullptr) )
		{
			WriteDiagnostic("state_shadow tree_recovery_retry_after_change=1 delay_ms=%u", delay);
			if( gDrainState.depth && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RETRY_TIMER_REENTER", nullptr, 0) )
				::SendMessageW(changedWindow, WM_TIMER, id, 0);
		}
		else WriteDiagnostic("state_shadow tree_recovery_retry_after_change_timer_failed=1");
	}
	if( recoveryInterrupted ) WriteDiagnostic("state_shadow tree_recovery_interrupted=1 input_kinds=%u", recoveryInputKinds);
	if( recoveryRetryChangePreflight && !recovery1.empty() && !recovery2.empty() && !gReplayBatch )
	{
		if( DeferTreeRecovery(recoveryWindow, recoveryOwner, recoveryMethod, recovery1, recovery2, false,
			recoveryExpandedMask, true, recoveryIdentity, readoptFixture) )
		{
			gDeferredTreeRecovery.preflightNeeded = true; gDeferredTreeRecovery.validateParentIntent = true;
			gDeferredTreeRecovery.parentIntentGeneration = recoveryIntentGeneration;
			const UINT delay = TreeRecoveryChangeDelay(recoveryOwner, recovery1, recovery2);
			if( ::SetTimer(recoveryWindow, reinterpret_cast<UINT_PTR>(&gDeferredTreeRecovery), delay, nullptr) )
				WriteDiagnostic("state_shadow tree_change_intent_preflight_scheduled=1 delay_ms=%u", delay);
			else WriteDiagnostic("state_shadow tree_change_intent_preflight_timer_failed=1");
		}
		recovery1.clear(); recovery2.clear();
	}
	if( recoveryRetryAfterReadFailure && !recovery1.empty() && !recovery2.empty() && !gReplayBatch )
	{
		if( DeferTreeRecovery(recoveryWindow, recoveryOwner, recoveryMethod, recovery1, recovery2, false, 0, true, recoveryIdentity, readoptFixture) )
		{
			gDeferredTreeRecovery.preflightNeeded = recoveryRetryAfterIdentityMismatch;
			const UINT delay = TreeRecoveryReadFailureDelay(recoveryOwner, recovery1, recovery2);
			if( ::SetTimer(recoveryWindow, reinterpret_cast<UINT_PTR>(&gDeferredTreeRecovery), delay, nullptr) )
				WriteDiagnostic("state_shadow tree_recovery_retry_after_read_failure=1 delay_ms=%u", delay);
			else WriteDiagnostic("state_shadow tree_recovery_retry_after_read_failure_timer_failed=1");
		}
		recovery1.clear(); recovery2.clear();
	}
	if( !interruptedFirst.empty() && !interruptedSecond.empty() && !gReplayBatch
		&& DeferTreeRecovery(recoveryWindow, recoveryOwner, recoveryMethod, interruptedFirst, interruptedSecond,
			shiftOnly && restoreFixtureChild, shiftOnly ? recoveryExpandedMask : 0, false, recoveryIdentity, readoptFixture) )
	{
		const UINT_PTR id = reinterpret_cast<UINT_PTR>(&gDeferredTreeRecovery);
		if( ::SetTimer(recoveryWindow, id, 300, nullptr) ) WriteDiagnostic("state_shadow tree_recovery_retry_after_replay_input=1");
		else WriteDiagnostic("state_shadow tree_recovery_retry_after_replay_input_timer_failed=1");
	}
	if( !recovery1.empty() && !recovery2.empty() && !gReplayBatch )
	{
		static bool injectedAttachFailure = false;
		const bool injectAttachFailure = !injectedAttachFailure
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_ATTACH_DEFER_FAIL_ONCE", nullptr, 0);
		if( injectAttachFailure ) { injectedAttachFailure = true; recoveryExpandedMask = 0; }
		gReplayBatch = new (std::nothrow) ReplayBatchDiagnostic(recoveryMethod, recoveryOwner, 4096);
		if( gReplayBatch )
		{
			gReplayBatch->recoveryOnly = true;
			gReplayBatch->recoveryExpandedMask = recoveryExpandedMask;
			gReplayBatch->recoveryRestoreFixtureChild = restoreFixtureChild;
			gReplayBatch->recoveryIdentity = recoveryIdentity;
			gReplayBatch->recoveryReadoptFixture = readoptFixture;
			const unsigned long serial = gReplayBatch->serial;
			const bool attached = !injectAttachFailure && LeeyesDiagnosticTreeBatchAttachPaths(reinterpret_cast<DWORD>(recoveryOwner),
				recovery1.c_str(), recovery2.c_str()) != FALSE;
			if( injectAttachFailure ) WriteDiagnostic("state_shadow tree_recovery_attach_failure_injected=1");
			if( !gReplayBatch || gReplayBatch->serial != serial )
			{ if( attached ) LeeyesDiagnosticTreeBatchCancelled(); }
			else if( !attached )
			{
				WriteDiagnostic("state_shadow tree_recovery_attach_failed=1 mask=%lu retry_input=%d interrupted=%d",
					static_cast<unsigned long>(recoveryExpandedMask), recoveryRetryAfterInput ? 1 : 0, recoveryInterrupted ? 1 : 0);
				if( recoveryExpandedMask == 0
					&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_DEFER_COLLAPSE", nullptr, 0) )
					if( DeferTreeRecovery(recoveryWindow, recoveryOwner, recoveryMethod, recovery1, recovery2, false, 0, false, recoveryIdentity, readoptFixture) )
						WriteDiagnostic("state_shadow tree_recovery_attach_deferred=1");
				LeeyesDiagnosticTreeBatchAbandon();
			}
			else { gReplayBatch->parentsAttached = true; WriteDiagnostic("state_shadow tree_batch_fixture_started=1 recovery=1"); }
		}
	}
	return replayed;
}
LRESULT CALLBACK DeferredTreeRecoverySubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR)
{
	if( message == WM_NCDESTROY )
	{
		::KillTimer(window, subclassId);
		::RemoveWindowSubclass(window, DeferredTreeRecoverySubclass, subclassId);
		ClearDeferredIdentityPreflight();
		gDeferredTreeRecovery = DeferredTreeRecovery();
		gTreeRecoveryBackoff = TreeRecoveryBackoff();
		gTreeReadFailureBackoff = TreeRecoveryBackoff();
		WriteDiagnostic("state_shadow tree_deferred_recovery_destroyed=1");
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if( message == WM_TIMER && wParam == subclassId && gDeferredTreeRecovery.active )
	{
		::KillTimer(window, subclassId);
		// A nested message loop can deliver this timer before the current
		// notification drain has returned to Leeyes. Do not attach while its
		// original callbacks are still on that drain's stack.
		if( gReplayBatch || gDrainState.depth || gDeferredTreeRecovery.inputDepth )
		{
			if( gDrainState.depth && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RETRY_TIMER_REENTER", nullptr, 0) )
				WriteDiagnostic("state_shadow tree_recovery_timer_blocked_by_drain=1");
			if( gDeferredTreeRecovery.inputDepth && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_INPUT_TIMER_REENTER", nullptr, 0) )
				WriteDiagnostic("state_shadow tree_recovery_timer_blocked_by_input=1");
			::SetTimer(window, subclassId, 500, nullptr); return 0;
		}
		std::wstring first, second;
		void* owner = gDeferredTreeRecovery.owner; void* method = gDeferredTreeRecovery.method;
		const unsigned long deferredSerial = gDeferredTreeRecovery.serial;
		static bool injectedPathCopyFailure = false;
		const bool injectPathCopyFailure = !injectedPathCopyFailure
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_DEFERRED_PATH_COPY_FAIL_ONCE", nullptr, 0);
		try
		{
			if( injectPathCopyFailure ) { injectedPathCopyFailure = true; throw std::bad_alloc(); }
			first = gDeferredTreeRecovery.first; second = gDeferredTreeRecovery.second;
		}
		catch( ... )
		{
			if( ::SetTimer(window, subclassId, 1000, nullptr) ) WriteDiagnostic("state_shadow tree_deferred_path_copy_retry=1 injected=%d", injectPathCopyFailure ? 1 : 0);
			else WriteDiagnostic("state_shadow tree_deferred_path_copy_timer_failed=1");
			return 0;
		}
		if( gDeferredTreeRecovery.preflightNeeded && gDeferredTreeRecovery.identity.known
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_PREFLIGHT", nullptr, 0) )
		{
			if( !gDeferredTreeRecovery.preflightStarted && !StartDeferredIdentityPreflight(first, second) )
			{
				WriteDiagnostic("state_shadow tree_identity_preflight_start_failed=1");
				::SetTimer(window, subclassId, TreeRecoveryReadFailureDelay(owner, first, second), nullptr); return 0;
			}
			static bool injectedChangeInput = false;
			if( gDeferredTreeRecovery.validateParentIntent && !injectedChangeInput
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_CHANGE_PREFLIGHT_INPUT", nullptr, 0) )
			{
				injectedChangeInput = true;
				::SendMessageW(window, WM_KEYDOWN, VK_SHIFT, 0);
				if( !gDeferredTreeRecovery.active || gDeferredTreeRecovery.serial != deferredSerial ) return 0;
				WriteDiagnostic("state_shadow tree_change_intent_preflight_input_injected=1");
			}
			static bool injectedIntentGeneration = false;
			if( gDeferredTreeRecovery.validateParentIntent && !injectedIntentGeneration
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_CHANGE_PREFLIGHT_REPLACE_INTENT", nullptr, 0) )
			{
				injectedIntentGeneration = true;
				WriteDiagnostic("state_shadow tree_change_intent_generation_injected=%d",
					LeeyesDiagnosticTreeTestReplaceParentIntentGeneration() ? 1 : 0);
			}
			if( ::GetTickCount64() >= gDeferredTreeRecovery.preflightDeadline )
			{
				WriteDiagnostic("state_shadow tree_identity_preflight_timeout=1 injected=%d", gDeferredTreeRecovery.preflightTestStall ? 1 : 0);
				ClearDeferredIdentityPreflight();
				::SetTimer(window, subclassId, TreeRecoveryReadFailureDelay(owner, first, second), nullptr); return 0;
			}
			if( gDeferredTreeRecovery.preflightTestStall ) { ::SetTimer(window, subclassId, 250, nullptr); return 0; }
			if( !gDeferredTreeRecovery.preflightHave1 ) gDeferredTreeRecovery.preflightHave1 = gDeferredIdentityWorker1->Take(gDeferredTreeRecovery.preflight1);
			if( !gDeferredTreeRecovery.preflightHave2 ) gDeferredTreeRecovery.preflightHave2 = gDeferredIdentityWorker2->Take(gDeferredTreeRecovery.preflight2);
			const bool stale1 = gDeferredTreeRecovery.preflightHave1 && !gDeferredIdentityWorker1->CanApply(gDeferredTreeRecovery.preflight1);
			const bool stale2 = gDeferredTreeRecovery.preflightHave2 && !gDeferredIdentityWorker2->CanApply(gDeferredTreeRecovery.preflight2);
			if( stale1 || stale2 || gDeferredIdentityWorker1->Failed() || gDeferredIdentityWorker2->Failed()
				|| gDeferredIdentityWorker1->Error() || gDeferredIdentityWorker2->Error() )
			{
				ClearDeferredIdentityPreflight();
				::SetTimer(window, subclassId, TreeRecoveryReadFailureDelay(owner, first, second), nullptr); return 0;
			}
			if( !gDeferredTreeRecovery.preflightHave1 || !gDeferredTreeRecovery.preflightHave2 )
			{ ::SetTimer(window, subclassId, 250, nullptr); return 0; }
			const TreeRecoveryIdentity expected = gDeferredTreeRecovery.identity;
			const auto& state1 = gDeferredTreeRecovery.preflight1.state;
			const auto& state2 = gDeferredTreeRecovery.preflight2.state;
			const bool matches = state1.directoryId && state2.directoryId && state1.volume == expected.volume1
				&& state1.directoryId == expected.id1 && state2.volume == expected.volume2 && state2.directoryId == expected.id2;
			const bool changed1 = state1.directoryId && (state1.volume != expected.volume1 || state1.directoryId != expected.id1);
			const bool changed2 = state2.directoryId && (state2.volume != expected.volume2 || state2.directoryId != expected.id2);
			TreeRecoveryIdentity observed;
			observed.volume1 = state1.volume; observed.id1 = state1.directoryId;
			observed.volume2 = state2.volume; observed.id2 = state2.directoryId;
			observed.known = observed.id1 && observed.id2;
			const bool current1 = gDeferredIdentityWorker1->Finish(gDeferredTreeRecovery.preflight1);
			const bool current2 = gDeferredIdentityWorker2->Finish(gDeferredTreeRecovery.preflight2);
			gDeferredTreeRecovery.preflightHave1 = gDeferredTreeRecovery.preflightHave2 = false;
			ClearDeferredIdentityPreflight();
			if( !current1 || !current2 ) { ::SetTimer(window, subclassId, 300, nullptr); return 0; }
			WriteDiagnostic("state_shadow tree_identity_preflight_match=%d", matches ? 1 : 0);
			if( !matches )
			{
				if( changed1 != changed2 && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_READD_REPLACED_PARENT", nullptr, 0) )
				{
					const BOOL added = LeeyesDiagnosticTreeFixtureParentAdd(reinterpret_cast<DWORD>(owner), window, method,
						(changed1 ? first : second).c_str());
					WriteDiagnostic("state_shadow tree_identity_fixture_parent_add=%d", added ? 1 : 0);
					if( !gDeferredTreeRecovery.active || gDeferredTreeRecovery.serial != deferredSerial ) return 0;
					if( added && observed.known && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_READOPT_REPLACED_PARENT", nullptr, 0) )
					{
						// Diagnostic fixture only: discard the old expansion intent and
						// reacquire both snapshots under the newly observed identity.
						gDeferredTreeRecovery.identity = observed;
						gDeferredTreeRecovery.readoptFixture = true;
						gDeferredTreeRecovery.restoreFixtureChild = false;
						gDeferredTreeRecovery.restoreExpandedMask = 0;
						gDeferredTreeRecovery.preflightNeeded = false;
						WriteDiagnostic("state_shadow tree_identity_fixture_readopt=1");
						::SetTimer(window, subclassId, 300, nullptr); return 0;
					}
				}
				::RemoveWindowSubclass(window, DeferredTreeRecoverySubclass, subclassId);
				gDeferredTreeRecovery = DeferredTreeRecovery(); return 0;
			}
			gDeferredTreeRecovery.preflightNeeded = false;
		}
		if( gDeferredTreeRecovery.validateParentIntent )
		{
			DWORD mask = 0; ULONGLONG generation = 0; const auto& id = gDeferredTreeRecovery.identity;
			const bool valid = !gDeferredTreeRecovery.preflightNeeded && id.known
				&& LeeyesDiagnosticTreeParentIntentMatches(reinterpret_cast<DWORD>(owner), window,
				first.c_str(), second.c_str(), id.volume1, id.id1, id.volume2, id.id2, FALSE, &mask, &generation)
				&& generation == gDeferredTreeRecovery.parentIntentGeneration;
			gDeferredTreeRecovery.restoreExpandedMask = valid ? (gDeferredTreeRecovery.restoreExpandedMask & mask) : 0;
			WriteDiagnostic("state_shadow tree_change_intent_preflight_valid=%d", valid ? 1 : 0);
		}
		static bool injectedDeferredAllocationFailure = false;
		const bool injectAllocationFailure = !injectedDeferredAllocationFailure
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_DEFERRED_ALLOC_FAIL_ONCE", nullptr, 0);
		if( injectAllocationFailure ) injectedDeferredAllocationFailure = true;
		gReplayBatch = injectAllocationFailure ? nullptr : new (std::nothrow) ReplayBatchDiagnostic(method, owner, 4096);
		if( !gReplayBatch )
		{
			if( ::SetTimer(window, subclassId, 1000, nullptr) ) WriteDiagnostic("state_shadow tree_deferred_allocation_retry=1 injected=%d", injectAllocationFailure ? 1 : 0);
			else WriteDiagnostic("state_shadow tree_deferred_allocation_timer_failed=1");
			return 0;
		}
		gReplayBatch->recoveryOnly = true;
		gReplayBatch->recoveryRestoreFixtureChild = gDeferredTreeRecovery.restoreFixtureChild;
		gReplayBatch->recoveryExpandedMask = gDeferredTreeRecovery.restoreExpandedMask;
		gReplayBatch->recoveryIdentity = gDeferredTreeRecovery.identity;
		gReplayBatch->recoveryReadoptFixture = gDeferredTreeRecovery.readoptFixture;
		gReplayBatch->recoveryReadRetry = gDeferredTreeRecovery.automaticReadRetry;
		const unsigned long batchSerial = gReplayBatch->serial;
		static bool injectedReadAttachFailure = false;
		const bool injectFailure = gDeferredTreeRecovery.automaticReadRetry && !injectedReadAttachFailure
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_READ_RETRY_ATTACH_FAIL_ONCE", nullptr, 0);
		if( injectFailure ) { injectedReadAttachFailure = true; WriteDiagnostic("state_shadow tree_read_retry_injected_attach_failure=1"); }
		const bool attached = !injectFailure && LeeyesDiagnosticTreeBatchAttachPaths(reinterpret_cast<DWORD>(owner), first.c_str(), second.c_str()) != FALSE;
		if( !gReplayBatch || gReplayBatch->serial != batchSerial || !gDeferredTreeRecovery.active
			|| gDeferredTreeRecovery.serial != deferredSerial )
		{ if( attached ) LeeyesDiagnosticTreeBatchCancelled(); return 0; }
		if( !attached )
		{
			WriteDiagnostic("state_shadow tree_deferred_recovery_not_ready=1");
			LeeyesDiagnosticTreeBatchAbandon();
			if( gDeferredTreeRecovery.active && gDeferredTreeRecovery.serial == deferredSerial && gDeferredTreeRecovery.automaticReadRetry )
			{
				gDeferredTreeRecovery.preflightNeeded = true;
				const UINT delay = TreeRecoveryReadFailureDelay(owner, first, second);
				if( ::SetTimer(window, subclassId, delay, nullptr) ) WriteDiagnostic("state_shadow tree_read_retry_parent_not_ready=1 delay_ms=%u", delay);
				else WriteDiagnostic("state_shadow tree_read_retry_parent_timer_failed=1");
			}
			return 0;
		}
		gReplayBatch->parentsAttached = true;
		ClearDeferredIdentityPreflight();
		::RemoveWindowSubclass(window, DeferredTreeRecoverySubclass, subclassId);
		gDeferredTreeRecovery = DeferredTreeRecovery();
		WriteDiagnostic("state_shadow tree_batch_fixture_started=1 recovery=1 deferred=1");
		return 0;
	}
	const bool inputMessage = message == TVM_EXPAND || message == TVM_SELECTITEM || message == WM_LBUTTONDOWN
		|| message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN
		|| message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_MOUSEWHEEL
		|| message == WM_HSCROLL || message == WM_VSCROLL;
	const bool harmlessShift = message == WM_KEYDOWN && wParam == VK_SHIFT
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_REPLAY_SHIFT_INTENT", nullptr, 0);
	const unsigned long deferredSerial = gDeferredTreeRecovery.serial;
	const bool inputActive = gDeferredTreeRecovery.active && gDeferredTreeRecovery.window == window && inputMessage;
	if( inputActive )
	{
		++gDeferredTreeRecovery.inputEpoch; ++gDeferredTreeRecovery.inputDepth;
		::KillTimer(window, subclassId);
		if( !harmlessShift )
		{
			gDeferredTreeRecovery.restoreFixtureChild = false;
			gDeferredTreeRecovery.restoreExpandedMask = 0;
			// Diagnostic: retain only the retry obligation, never the previous
			// expansion intent. Attach must revalidate the current tree state.
			if( !::GetEnvironmentVariableA("LEYEES_TREE_TEST_READ_RETRY_AFTER_INPUT", nullptr, 0) )
				gDeferredTreeRecovery.automaticReadRetry = false;
		}
	}
	const unsigned long inputEpoch = gDeferredTreeRecovery.inputEpoch;
	DWORD requestedParent = 0;
	if( inputActive && message == TVM_EXPAND && wParam == TVE_EXPAND && lParam
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_EXPLICIT_EXPANSION_RETRY", nullptr, 0) )
	{
		try
		{
			const std::wstring first = gDeferredTreeRecovery.first, second = gDeferredTreeRecovery.second;
			requestedParent = LeeyesDiagnosticTreeRequestedParent(reinterpret_cast<DWORD>(gDeferredTreeRecovery.owner), window,
				reinterpret_cast<HTREEITEM>(lParam), first.c_str(), second.c_str());
		}
		catch( ... ) { requestedParent = 0; }
	}
	if( inputActive && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_INPUT_TIMER_REENTER", nullptr, 0) )
		::SendMessageW(window, WM_TIMER, subclassId, 0);
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if( inputActive && gDeferredTreeRecovery.active && gDeferredTreeRecovery.serial == deferredSerial
		&& gDeferredTreeRecovery.inputDepth ) --gDeferredTreeRecovery.inputDepth;
	if( requestedParent && !result && gDeferredTreeRecovery.active && gDeferredTreeRecovery.window == window
		&& gDeferredTreeRecovery.serial == deferredSerial && gDeferredTreeRecovery.inputEpoch == inputEpoch )
	{
		gDeferredTreeRecovery.restoreExpandedMask = requestedParent;
		WriteDiagnostic("state_shadow tree_recovery_explicit_expand_intent=%lu", static_cast<unsigned long>(requestedParent));
	}
	if( gDeferredTreeRecovery.active && gDeferredTreeRecovery.window == window
		&& (inputMessage || message == WM_LBUTTONUP
			|| message == WM_RBUTTONUP || message == WM_KEYUP || message == WM_SYSKEYUP) )
	{
		UINT delay = 300;
		char text[16] = {};
		const DWORD length = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_DEFER_INPUT_DELAY_MS", text, _countof(text));
		if( length && length < _countof(text) )
		{
			const unsigned long requested = strtoul(text, nullptr, 10);
			if( requested >= 300 && requested <= 10000 ) delay = static_cast<UINT>(requested);
		}
		::SetTimer(window, subclassId, delay, nullptr);
	}
	return result;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeSetRecovery(const wchar_t* first, const wchar_t* second, DWORD expandedMask)
{
	if( !gReplayBatch || gReplayBatch->recoveryOnly || !first || !second || !*first || !*second ) return FALSE;
	try { gReplayBatch->recoveryParent = first; gReplayBatch->recoveryParent2 = second;
		gReplayBatch->recoveryExpandedMask = expandedMask;
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_EXPANSION_TRACE", nullptr, 0) )
			WriteDiagnostic("state_shadow tree_recovery_mask_set=%lu", static_cast<unsigned long>(expandedMask));
		return TRUE; }
	catch( ... ) { gReplayBatch->recoveryParent.clear(); gReplayBatch->recoveryParent2.clear(); return FALSE; }
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeSetRecoveryIdentity(DWORD volume1, ULONGLONG id1, DWORD volume2, ULONGLONG id2)
{
	if( !gReplayBatch || gReplayBatch->recoveryOnly || !id1 || !id2
		|| !::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_IDENTITY", nullptr, 0) ) return FALSE;
	gReplayBatch->recoveryIdentity.volume1 = volume1; gReplayBatch->recoveryIdentity.id1 = id1;
	gReplayBatch->recoveryIdentity.volume2 = volume2; gReplayBatch->recoveryIdentity.id2 = id2;
	gReplayBatch->recoveryIdentity.known = true;
	WriteDiagnostic("state_shadow tree_recovery_identity_captured=1");
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRecoveryIdentityMatches(DWORD volume1, ULONGLONG id1, DWORD volume2, ULONGLONG id2)
{
	if( !gReplayBatch || !gReplayBatch->recoveryOnly || !gReplayBatch->recoveryIdentity.known ) return TRUE;
	const TreeRecoveryIdentity& expected = gReplayBatch->recoveryIdentity;
	const bool injectedMismatch = gReplayBatch->recoveryReadRetry
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_IDENTITY_MISMATCH", nullptr, 0) != 0;
	const bool matches = !injectedMismatch && id1 && id2 && expected.volume1 == volume1 && expected.id1 == id1
		&& expected.volume2 == volume2 && expected.id2 == id2;
	WriteDiagnostic("state_shadow tree_recovery_identity_match=%d injected=%d", matches ? 1 : 0, injectedMismatch ? 1 : 0);
	return matches ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterInput(const wchar_t* first, const wchar_t* second)
{
	if( !gReplayBatch || !gReplayBatch->recoveryOnly || !first || !second || !*first || !*second ) return FALSE;
	try {
		gReplayBatch->recoveryParent = first; gReplayBatch->recoveryParent2 = second;
		// User input invalidates the old expansion intent; never restore it.
		gReplayBatch->recoveryExpandedMask = 0; gReplayBatch->recoveryRetryAfterInput = true;
		gReplayBatch->recoveryRestoreFixtureChild = false;
		return TRUE;
	} catch( ... ) { gReplayBatch->recoveryParent.clear(); gReplayBatch->recoveryParent2.clear(); return FALSE; }
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterChange(const wchar_t* first, const wchar_t* second, HWND window)
{
	if( !gReplayBatch || !gReplayBatch->recoveryOnly || !first || !second || !*first || !*second ) return FALSE;
	try {
		gReplayBatch->recoveryParent = first; gReplayBatch->recoveryParent2 = second;
		DWORD mask = 0; ULONGLONG generation = 0; const auto& id = gReplayBatch->recoveryIdentity;
		const bool preserve = id.known && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_CHANGE_PRESERVE_INTENT", nullptr, 0)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_PREFLIGHT", nullptr, 0)
			&& LeeyesDiagnosticTreeParentIntentMatches(reinterpret_cast<DWORD>(gReplayBatch->owner), window,
				first, second, id.volume1, id.id1, id.volume2, id.id2, TRUE, &mask, &generation);
		gReplayBatch->recoveryExpandedMask = preserve ? mask : 0;
		gReplayBatch->recoveryIntentGeneration = preserve ? generation : 0;
		gReplayBatch->recoveryRetryAfterChange = true; gReplayBatch->recoveryRetryChangePreflight = preserve && mask != 0;
		WriteDiagnostic("state_shadow tree_change_intent_candidate=%d mask=%lu", preserve ? 1 : 0, static_cast<unsigned long>(mask));
		return TRUE;
	} catch( ... ) { gReplayBatch->recoveryParent.clear(); gReplayBatch->recoveryParent2.clear(); return FALSE; }
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterReadFailure(const wchar_t* first, const wchar_t* second)
{
	if( !gReplayBatch || !gReplayBatch->recoveryOnly || !first || !second || !*first || !*second ) return FALSE;
	try {
		gReplayBatch->recoveryParent = first; gReplayBatch->recoveryParent2 = second;
		gReplayBatch->recoveryExpandedMask = 0; gReplayBatch->recoveryRestoreFixtureChild = false;
		gReplayBatch->recoveryRetryAfterReadFailure = true; return TRUE;
	} catch( ... ) { gReplayBatch->recoveryParent.clear(); gReplayBatch->recoveryParent2.clear(); return FALSE; }
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterIdentityMismatch(const wchar_t* first, const wchar_t* second)
{
	if( !LeeyesDiagnosticTreeRetryRecoveryAfterReadFailure(first, second) ) return FALSE;
	gReplayBatch->recoveryRetryAfterIdentityMismatch = true;
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeSetFixtureChildExpansion(BOOL expanded)
{
	if( !gReplayBatch || gReplayBatch->recoveryOnly ) return FALSE;
	gReplayBatch->recoveryRestoreFixtureChild = expanded != FALSE;
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRestoreFixtureChildRequested()
{ return gReplayBatch && gReplayBatch->recoveryOnly && gReplayBatch->recoveryRestoreFixtureChild ? TRUE : FALSE; }
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRecoveryOnly()
{ return gReplayBatch && gReplayBatch->recoveryOnly ? TRUE : FALSE; }
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRecoveryReadoptFixture()
{ return gReplayBatch && gReplayBatch->recoveryOnly && gReplayBatch->recoveryReadoptFixture ? TRUE : FALSE; }
extern "C" DWORD __cdecl LeeyesDiagnosticTreeRecoveryExpandedMask()
{ return gReplayBatch && gReplayBatch->recoveryOnly ? gReplayBatch->recoveryExpandedMask : 0; }
extern "C" void __cdecl LeeyesDiagnosticTreeTestReenterAttach()
{
	static bool injected = false;
	if( !injected && gReplayBatch && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_DISCOVERY_REENTER", nullptr, 0) )
	{
		injected = true;
		FlushReplayBatch("test_discovery_reentry");
	}
}
void FinishTreeDiscovery(ReplayBatchDiagnostic* batch, HWND window)
{
	if( !batch || batch != gReplayBatch ) return;
	const unsigned long serial = batch->serial;
	if( batch->discoveryWaiting )
	{
		const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayBatch);
		::KillTimer(batch->timerWindow, timerId);
		::RemoveWindowSubclass(batch->timerWindow, ReplayBatchSubclass, timerId);
		batch->timerScheduled = false; batch->discoveryWaiting = false;
	}
	const bool attached = !::GetEnvironmentVariableA("LEYEES_TREE_TEST_DISCOVERY_INCOMPLETE", nullptr, 0)
		&& !batch->fallbackPending && batch->discoveredParents.size() == 2
		&& LeeyesDiagnosticTreeBatchAttachPaths(reinterpret_cast<DWORD>(batch->owner),
			batch->discoveredParents[0].c_str(), batch->discoveredParents[1].c_str());
	if( !gReplayBatch || gReplayBatch != batch || gReplayBatch->serial != serial )
	{
		if( attached ) LeeyesDiagnosticTreeBatchCancelled();
		WriteDiagnostic("state_shadow tree_discovery_reentered=1 attached=%d", attached ? 1 : 0);
		return;
	}
	WriteDiagnostic("state_shadow tree_discovered_parents=%lu attached=%d captured=%u",
		static_cast<unsigned long>(batch->discoveredParents.size()), attached ? 1 : 0, batch->captured);
	if( attached ) batch->parentsAttached = true;
	else
	{
		batch->fallbackPending = true;
		if( !LeeyesDiagnosticTreeBatchResolve(FALSE, window, TRUE) ) FlushReplayBatch("discovery_fallback_failed");
	}
}
LRESULT CALLBACK ReplayBatchSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR)
{
	if( gReplayBatch && !gReplayBatch->recoveryOnly && gReplayBatch->fallbackPending
		&& !gReplayBatch->recoveryParent.empty()
		&& (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN
			|| message == WM_MBUTTONDOWN || message == WM_KEYDOWN || message == WM_SYSKEYDOWN
			|| message == WM_MOUSEWHEEL || message == WM_HSCROLL || message == WM_VSCROLL
			|| message == TVM_SELECTITEM || message == TVM_EXPAND) )
	{
		gReplayBatch->recoveryInterrupted = true;
		if( message != WM_KEYDOWN || wParam != VK_SHIFT ) gReplayBatch->recoveryInputOnlyShift = false;
		gReplayBatch->recoveryInputKinds |= (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) ? 1u
			: (message == TVM_SELECTITEM || message == TVM_EXPAND) ? 2u
			: (message == WM_MOUSEWHEEL || message == WM_HSCROLL || message == WM_VSCROLL) ? 4u : 8u;
	}
	// Diagnostic fault injection delivers an already observed second parent
	// after the first drain, while leaving every notification in the journal.
	if( message == WM_APP + 0x4f2 && wParam == subclassId && gReplayBatch
		&& gReplayBatch->discoveryWaiting && !gReplayBatch->testDeferredParent.empty() )
	{
		ReplayBatchDiagnostic* batch = gReplayBatch;
		try { batch->discoveredParents.push_back(batch->testDeferredParent); batch->testDeferredParent.clear(); }
		catch( ... ) { batch->fallbackPending = true; }
		WriteDiagnostic("state_shadow tree_discovery_deferred_parent=1");
		FinishTreeDiscovery(batch, window);
		return 0;
	}
	if( message == WM_NCDESTROY || (message == WM_TIMER && wParam == subclassId) )
	{
		if( message == WM_TIMER && gReplayBatch && gReplayBatch->discoveryWaiting )
		{
			if( gDrainState.depth )
			{
				if( !::SetTimer(window, subclassId, 50, nullptr) ) FlushReplayBatch("discovery_timer_failed");
				return 0;
			}
			FinishTreeDiscovery(gReplayBatch, window);
			return 0;
		}
		if( message == WM_TIMER && gReplayBatch
			&& (::GetEnvironmentVariableA("LEYEES_TREE_BATCH_SLICED", nullptr, 0)
				|| ::GetEnvironmentVariableA("LEYEES_TREE_STATE_SLICED_FALLBACK", nullptr, 0)) )
		{
			ReplayBatchDiagnostic* batch = gReplayBatch;
			if( batch->replaying ) { ++batch->reentered; return 0; }
			batch->streamingStarted = true;
			batch->journal.StartStreaming();
			static bool injectedLateUnknown = false;
			if( !injectedLateUnknown && batch->discoverParents
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_LATE_UNKNOWN", nullptr, 0) )
			{
				const NotificationFallback::Entry* first = batch->journal.Next();
				void* path = first ? LeeyesDiagnosticMakeAnsi(first->first.data(), static_cast<DWORD>(first->first.size())) : nullptr;
				if( path )
				{
					injectedLateUnknown = true;
					const BOOL captured = LeeyesDiagnosticCaptureTree(batch->method, batch->owner, 0, reinterpret_cast<DWORD>(path), 0);
					LeeyesDiagnosticClearAnsi(&path);
					WriteDiagnostic("state_shadow tree_batch_late_unknown_captured=%d", captured ? 1 : 0);
					if( !gReplayBatch ) return 0;
					batch = gReplayBatch;
				}
			}
			char budgetText[16] = {}, delayText[16] = {};
			const DWORD budgetLength = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_SLICE_BUDGET_MS", budgetText, _countof(budgetText));
			const DWORD delayLength = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_SLICE_DELAY_MS", delayText, _countof(delayText));
			unsigned budget = budgetLength && budgetLength < _countof(budgetText) ? strtoul(budgetText, nullptr, 10) : 8;
			unsigned delay = delayLength && delayLength < _countof(delayText) ? strtoul(delayText, nullptr, 10) : 15;
			if( !budget || budget > 50 ) budget = 8;
			if( !delay || delay > 100 ) delay = 15;
			const unsigned before = batch->replayed;
			ReplayBatchSlice(batch, 16, budget);
			if( batch->abandoned ) { LeeyesDiagnosticTreeBatchAbandon(); return 0; }
			if( !batch->journal.Empty() && batch->replayed == before )
			{
				FlushReplayBatch("slice_failed");
				return 0;
			}
			if( !batch->journal.Empty() && delay > 5 && batch->journal.Backlogged()
				&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ADAPTIVE_REPLAY", nullptr, 0) )
			{ delay = 5; ++batch->acceleratedSlices; }
			if( !batch->journal.Empty() && ::SetTimer(window, subclassId, delay, nullptr) ) return 0;
			FlushReplayBatch(batch->journal.Empty() ? "timer_sliced" : "slice_timer_failed");
			return 0;
		}
		::KillTimer(window, subclassId);
		::RemoveWindowSubclass(window, ReplayBatchSubclass, subclassId);
		if( gReplayBatch )
		{
			gReplayBatch->timerScheduled = false;
			if( message == WM_NCDESTROY && ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0) )
				LeeyesDiagnosticTreeBatchAbandon();
			else FlushReplayBatch(message == WM_TIMER ? "timer" : "window_destroyed");
		}
		if( message == WM_TIMER ) return 0;
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}
struct ShadowPendingUpdate
{
	void* owner = nullptr;
	HWND window = nullptr;
	ULONGLONG deadline = 0;
	bool correction = false;
	bool unchangedSkipEligible = false;
	bool retryUnavailable = false;
};
ShadowPendingUpdate gShadowPending;
ShadowPendingUpdate gListVerifyPending;
std::unique_ptr<DirectorySnapshot::State> gListVerifyState;
ShadowPendingUpdate gListWatchCommitted;
std::unique_ptr<DirectorySnapshot::State> gListWatchCommittedState;
unsigned gListWatchCommittedDirty = 0, gListWatchCommittedStarts = 0;
bool gListWatchBootstrapAttempted = false;
// Experimental only: a filesystem baseline is not by itself proof that the
// UI or cached image contents match it. Never use this in Release.
std::unique_ptr<DirectorySnapshot::State> gShadowPendingBaseline;
bool DispatchOwnerContainsListWindow(void* dispatchOwner, HWND listWindow);
bool IsReadableRange(const BYTE* address, size_t length);
void ProbeOwnerPidl(void* owner);
bool ApplyShadowPending(bool incremental, bool verifyFallback = false, bool sortAfter = false,
	const DirectorySnapshot::State* sortState = nullptr);
BOOL CALLBACK FindReplayTree(HWND child, LPARAM result)
{
	char name[64] = {};
	if( ::IsWindowVisible(child) && ::GetClassNameA(child, name, _countof(name))
		&& strcmp(name, "TAcvTreeView") == 0 )
	{
		HWND* found = reinterpret_cast<HWND*>(result);
		if( *found ) { *found = reinterpret_cast<HWND>(-1); return FALSE; }
		*found = child;
	}
	return TRUE;
}
LRESULT CALLBACK ReplaySingleSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR)
{
	if( message == WM_NCDESTROY || (message == WM_TIMER && wParam == subclassId) )
	{
		::KillTimer(window, subclassId);
		::RemoveWindowSubclass(window, ReplaySingleSubclass, subclassId);
		const ReplaySingleDiagnostic pending = gReplayTimer;
		gReplayTimer = ReplaySingleDiagnostic();
		if( message == WM_TIMER && pending.owner && pending.window == window
			&& (pending.method ? IsReadableRange(reinterpret_cast<const BYTE*>(pending.owner), sizeof(DWORD))
				: DispatchOwnerContainsListWindow(pending.owner, window)) )
		{
			const LRESULT before = ::SendMessageA(window, LVM_GETITEMCOUNT, 0, 0);
			HWND tree = nullptr;
			if( pending.method ) ::EnumChildWindows(::GetAncestor(window, GA_ROOT), FindReplayTree,
				reinterpret_cast<LPARAM>(&tree));
			const LRESULT treeBefore = tree && tree != reinterpret_cast<HWND>(-1)
				? ::SendMessageA(tree, TVM_GETCOUNT, 0, 0) : -1;
			const BOOL replayed = pending.method
				? LeeyesDiagnosticReplayMethod(pending.method, pending.owner, pending.action,
					pending.first, pending.firstLength, pending.second, pending.secondLength)
				: LeeyesDiagnosticReplayAnsi(pending.owner, pending.action,
					pending.first, pending.firstLength, pending.second, pending.secondLength);
			const LRESULT after = ::SendMessageA(window, LVM_GETITEMCOUNT, 0, 0);
			const LRESULT treeAfter = tree && tree != reinterpret_cast<HWND>(-1)
				? ::SendMessageA(tree, TVM_GETCOUNT, 0, 0) : -1;
			WriteDiagnostic("state_shadow replay_timer_completed=%d method=%p action=%lu before=%ld after=%ld tree_before=%ld tree_after=%ld",
				replayed ? 1 : 0, pending.method, static_cast<unsigned long>(pending.action & 0xff),
				static_cast<long>(before), static_cast<long>(after), static_cast<long>(treeBefore), static_cast<long>(treeAfter));
		}
		else WriteDiagnostic("state_shadow replay_timer_owner_invalid=1");
		if( message == WM_TIMER ) return 0;
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}
LRESULT CALLBACK ShadowWorkerSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR)
{
	if( message == WM_APP + 0x5a4 && ::GetEnvironmentVariableA("LEYEES_LIST_TEST_THUMBNAIL_PIXELS", nullptr, 0) )
	{
		// Diagnostic only: draw the list's own image into a private bitmap and
		// return a checksum. This does not prove that it depicts the source file.
		const int index = static_cast<int>(wParam);
		if( index < 0 || index >= static_cast<int>(::SendMessageW(window, LVM_GETITEMCOUNT, 0, 0)) ) return 0;
		LVITEMW item = {}; item.mask = LVIF_IMAGE; item.iItem = index; item.iImage = -1;
		if( !::SendMessageW(window, LVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || item.iImage < 0 ) return 0;
		HIMAGELIST images = reinterpret_cast<HIMAGELIST>(::SendMessageW(window, LVM_GETIMAGELIST, LVSIL_NORMAL, 0));
		if( !images ) images = reinterpret_cast<HIMAGELIST>(::SendMessageW(window, LVM_GETIMAGELIST, LVSIL_SMALL, 0));
		int width = 0, height = 0;
		if( !images || !::ImageList_GetIconSize(images, &width, &height) || width <= 0 || height <= 0 || width > 512 || height > 512 ) return 0;
		BITMAPINFO info = {}; info.bmiHeader.biSize = sizeof(info.bmiHeader); info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
		void* pixels = nullptr; HDC dc = ::CreateCompatibleDC(nullptr);
		if( !dc ) return 0;
		HBITMAP bitmap = ::CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
		if( !bitmap || !pixels ) { if( bitmap ) ::DeleteObject(bitmap); ::DeleteDC(dc); return 0; }
		HGDIOBJ old = ::SelectObject(dc, bitmap);
		if( !old || old == HGDI_ERROR ) { ::DeleteObject(bitmap); ::DeleteDC(dc); return 0; }
		const size_t bytes = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
		memset(pixels, 0xff, bytes);
		const bool drawn = ::ImageList_Draw(images, item.iImage, dc, 0, 0, ILD_NORMAL) != FALSE;
		::GdiFlush(); DWORD hash = 2166136261u;
		const BYTE* data = static_cast<const BYTE*>(pixels);
		for( size_t i = 0; i < bytes; i += 4 ) for( unsigned c = 0; c < 3; ++c ) hash = (hash ^ data[i + c]) * 16777619u;
		::SelectObject(dc, old); ::DeleteObject(bitmap); ::DeleteDC(dc);
		return drawn ? static_cast<LRESULT>(hash ? hash : 1) : 0;
	}
	if( message == WM_NCDESTROY )
	{
		WatchedSyncWorker* watched = gListWatchedShadow.load(std::memory_order_acquire);
		if( watched ) { try { watched->SetTarget(L""); } catch( ... ) {} }
		gListVerifyPending = ShadowPendingUpdate(); gListVerifyState.reset();
		gListWatchCommitted = ShadowPendingUpdate();
		gListWatchCommittedState.reset();
		gListWatchBootstrapAttempted = false;
		gShadowTarget.clear();
		::AcquireSRWLockExclusive(&gShadowScopeLock);
		gShadowScopeTarget[0] = L'\0';
		::ReleaseSRWLockExclusive(&gShadowScopeLock);
		StateSyncWorker* oldWorker = gShadowWorker.load(std::memory_order_acquire);
		if( oldWorker ) { try { oldWorker->SetTarget(L""); } catch( ... ) {} }
		if( gShadowPending.window == window )
		{
			gShadowPending = ShadowPendingUpdate();
			gShadowPendingBaseline.reset();
		}
		InvalidateShadowApplied();
		::KillTimer(window, subclassId);
		::RemoveWindowSubclass(window, ShadowWorkerSubclass, subclassId);
	}
	StateSyncWorker* worker = gShadowWorker.load(std::memory_order_acquire);
	if( message == WM_TIMER && wParam == subclassId && worker )
	{
		WatchedSyncWorker* watched = gListWatchedShadow.load(std::memory_order_acquire);
		if( watched )
		{
			WatchedSyncWorker::Candidate observed;
			if( watched->Take(observed) )
			{
				const unsigned observedDirty = watched->DirtyFrames();
				const unsigned observedStarts = watched->ChildStarts();
				const bool current = watched->CanApply(observed);
				WriteDiagnostic("state_shadow list_watched_candidate=%d entries=%lu dirty=%u starts=%u error=%lu",
					current ? 1 : 0, static_cast<unsigned long>(observed.state.entries.size()),
					watched->DirtyFrames(), watched->ChildStarts(), static_cast<unsigned long>(watched->Error()));
				const bool primary = ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_PRIMARY", nullptr, 0)
					&& ::GetEnvironmentVariableA("LEYEES_STATE_APPLY_EXPERIMENT", nullptr, 0);
				if( primary && current && !gDrainState.depth && !gShadowNavigation.load()
					&& !gListWatchBootstrapAttempted && !gListWatchCommitted.owner && !gShadowPending.owner
					&& !gListVerifyState && !gShadowTarget.empty()
					&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_BOOTSTRAP", nullptr, 0)
					&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_CONTINUOUS", nullptr, 0)
					&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_VERIFY", nullptr, 0) )
				{
					void* owner = reinterpret_cast<void*>(gShadowOwner.load());
					if( owner && DispatchOwnerContainsListWindow(owner, window) && watched->CanApply(observed) )
					{
						gListWatchBootstrapAttempted = true;
						gShadowPending.owner = owner; gShadowPending.window = window;
						gShadowPending.deadline = ::GetTickCount64() + 5000;
						WriteDiagnostic("state_shadow list_watched_bootstrap=1");
					}
				}
				if( primary && current && gListVerifyState && gListVerifyPending.window == window )
				{
					const bool same = DirectorySnapshot::Same(*gListVerifyState, observed.state);
					WriteDiagnostic("state_shadow list_watched_verify_same=%d", same ? 1 : 0);
					if( !same && !gShadowPending.owner && !gShadowNavigation.load()
						&& DispatchOwnerContainsListWindow(gListVerifyPending.owner, window) )
					{
						gShadowPending = gListVerifyPending;
						gShadowPending.correction = true;
						gShadowPending.deadline = ::GetTickCount64() + 5000;
					}
					if( same && !gShadowPending.owner && !gShadowNavigation.load()
						&& watched->CanApply(observed) && ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_CONTINUOUS", nullptr, 0) )
					{
						bool baselineReady = true;
						if( ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_SORT_SAFE", nullptr, 0) )
						{
							gListWatchCommittedState.reset(new (std::nothrow) DirectorySnapshot::State(std::move(observed.state)));
							baselineReady = gListWatchCommittedState != nullptr;
						}
						if( baselineReady )
						{
							gListWatchCommitted = gListVerifyPending;
							gListWatchCommittedDirty = observedDirty; gListWatchCommittedStarts = observedStarts;
							WriteDiagnostic("state_shadow list_watched_baseline=1 dirty=%u starts=%u", observedDirty, observedStarts);
						}
						else { gListWatchCommitted = ShadowPendingUpdate(); WriteDiagnostic("state_shadow list_watched_baseline_allocation_failed=1"); }
					}
					gListVerifyState.reset(); gListVerifyPending = ShadowPendingUpdate();
				}
				if( primary && current && !gListVerifyState && !gShadowPending.owner
					&& gListWatchCommitted.window == window && !gShadowNavigation.load()
					&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_CONTINUOUS", nullptr, 0)
					&& (observedDirty != gListWatchCommittedDirty || observedStarts != gListWatchCommittedStarts)
					&& DispatchOwnerContainsListWindow(gListWatchCommitted.owner, window) && watched->CanApply(observed) )
				{
					gShadowPending = gListWatchCommitted; gShadowPending.correction = true;
					gShadowPending.deadline = ::GetTickCount64() + 5000;
					WriteDiagnostic("state_shadow list_watched_continuous_reconcile=1 dirty=%u starts=%u", observedDirty, observedStarts);
				}
				bool requestVerification = false;
				if( primary && current && !gDrainState.depth && gShadowPending.window == window
					&& (!gShadowPending.retryUnavailable || ::GetTickCount64() >= gShadowPending.deadline)
					&& !gShadowNavigation.load() && watched->CanApply(observed) )
				{
					const ShadowPendingUpdate pending = gShadowPending;
					bool incremental = true;
					bool sortAfter = false;
					if( ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_SORT_SAFE", nullptr, 0) )
					{
						incremental = gListWatchCommittedState && DirectorySnapshot::OnlyRemovals(*gListWatchCommittedState, observed.state);
						if( !incremental && gListWatchCommittedState
							&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_ADD_SORT", nullptr, 0)
							&& DirectorySnapshot::OnlyRemovals(observed.state, *gListWatchCommittedState) )
						{ incremental = true; sortAfter = true; }
						WriteDiagnostic("state_shadow list_watched_sort_safe_incremental=%d", incremental ? 1 : 0);
					}
					if( pending.retryUnavailable ) { incremental = false; sortAfter = false; }
					const bool applied = ApplyShadowPending(incremental, false, sortAfter, sortAfter ? &observed.state : nullptr);
					static bool injectedStale = false;
					if( applied && !injectedStale && !gShadowNavigation.load()
						&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_TEST_STALE", nullptr, 0) )
					{
						injectedStale = true;
						try { watched->SetTarget(gShadowTarget); } catch( ... ) {}
						WriteDiagnostic("state_shadow list_watched_test_stale=1");
					}
					const bool stillCurrent = applied && watched->CanApply(observed) && !gShadowNavigation.load();
					WriteDiagnostic("state_shadow list_watched_apply=%d current_after=%d", applied ? 1 : 0, stillCurrent ? 1 : 0);
					if( stillCurrent && !gShadowPending.owner && ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_VERIFY", nullptr, 0) )
					{
						gListVerifyState.reset(new (std::nothrow) DirectorySnapshot::State(std::move(observed.state)));
						if( gListVerifyState )
						{
							gListVerifyPending = pending;
							gListVerifyPending.deadline = ::GetTickCount64() + 5000;
							requestVerification = true;
						}
						else
						{
							gShadowPending = pending; gShadowPending.correction = true;
							gShadowPending.deadline = ::GetTickCount64();
							WriteDiagnostic("state_shadow list_watched_verify_allocation_failed=1");
						}
					}
					// Reentrant notifications may already have installed a newer
					// pending update. Preserve it; otherwise request a correction.
					if( applied && !stillCurrent && !gShadowPending.owner && !gShadowNavigation.load()
						&& DispatchOwnerContainsListWindow(pending.owner, window) )
					{
						gShadowPending = pending;
						gShadowPending.correction = true;
						gShadowPending.deadline = ::GetTickCount64() + 5000;
						WriteDiagnostic("state_shadow list_watched_correction_pending=1");
					}
				}
				watched->Finish(observed);
				bool verificationRequested = false;
				if( requestVerification ) { try { verificationRequested = watched->RequestRefresh(); } catch( ... ) {} }
				if( requestVerification && !verificationRequested )
				{
					gListVerifyPending.deadline = ::GetTickCount64();
					WriteDiagnostic("state_shadow list_watched_verify_request_failed=1");
				}
			}
		}
		if( gListVerifyState && gListVerifyPending.window == window
			&& ::GetTickCount64() >= gListVerifyPending.deadline )
		{
			if( !gShadowPending.owner && !gShadowNavigation.load()
				&& DispatchOwnerContainsListWindow(gListVerifyPending.owner, window) )
			{
				gShadowPending = gListVerifyPending; gShadowPending.correction = true;
				gShadowPending.deadline = ::GetTickCount64();
			}
			gListVerifyState.reset(); gListVerifyPending = ShadowPendingUpdate();
			WriteDiagnostic("state_shadow list_watched_verify_timeout=1");
		}
		if( !gDrainState.depth && gShadowNavigation.exchange(false) )
		{
			gShadowTarget.clear();
			gShadowLastState.reset();
			InvalidateShadowApplied();
			void* owner = reinterpret_cast<void*>(gShadowOwner.load());
			if( owner && DispatchOwnerContainsListWindow(owner, window) )
			{
				gDrainState.currentListWindow = window;
				ProbeOwnerPidl(owner);
				const bool found = gDrainState.pidlDirectoryKnown;
				gDrainState = DrainState();
				if( !found ) gShadowNavigation.store(true);
			}
		}
		StateSyncCoordinator::Candidate candidate = {};
		if( worker->TakeCandidate(candidate) )
		{
			const bool valid = worker->CanApply(candidate);
			if( valid && !gShadowListComparisonDone
				&& ::GetEnvironmentVariableA("LEYEES_STATE_COMPARE_LIST", nullptr, 0) )
			{
				gShadowListComparisonDone = true;
				const int count = static_cast<int>(::SendMessageW(window, LVM_GETITEMCOUNT, 0, 0));
				if( count >= 0 && count <= 20000 )
				{
					std::map<std::wstring, int> names;
					for( const DirectorySnapshot::Entry& entry : candidate.state.entries ) ++names[entry.name];
					unsigned extra = 0, readFailures = 0;
					for( int index = 0; index < count; ++index )
					{
						wchar_t text[1024] = {};
						LVITEMW item = {}; item.pszText = text; item.cchTextMax = _countof(text);
						const LRESULT n = ::SendMessageW(window, LVM_GETITEMTEXTW,
							static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
						if( n <= 0 || n >= _countof(text) - 1 ) { ++readFailures; continue; }
						auto found = names.find(std::wstring(text, static_cast<size_t>(n)));
						if( found == names.end() || found->second == 0 ) ++extra;
						else --found->second;
					}
					unsigned missing = 0;
					for( const auto& name : names ) missing += static_cast<unsigned>(name.second);
					WriteDiagnostic("state_shadow list_compare missing=%u extra=%u failures=%u still_valid=%d",
						missing, extra, readFailures, worker->CanApply(candidate) ? 1 : 0);
				}
			}
			const bool finalValid = valid && worker->CanApply(candidate);
			if( finalValid && gShadowPending.window == window && !gShadowNavigation.load()
				&& (!gShadowPending.retryUnavailable || ::GetTickCount64() >= gShadowPending.deadline)
				&& !(gListWatchedShadow.load(std::memory_order_acquire)
					&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_PRIMARY", nullptr, 0)) )
			{
				const bool checkUnchanged = ::GetEnvironmentVariableA("LEYEES_STATE_SKIP_UNCHANGED_EXPERIMENT", nullptr, 0)
					&& gShadowPending.unchangedSkipEligible && gShadowPendingBaseline;
				const ULONGLONG compareStarted = ::GetTickCount64();
				const bool unchanged = checkUnchanged && DirectorySnapshot::Same(*gShadowPendingBaseline, candidate.state);
				if( checkUnchanged ) WriteDiagnostic("state_shadow unchanged_compare=%d elapsed_ms=%llu entries=%lu",
					unchanged ? 1 : 0, ::GetTickCount64() - compareStarted,
					static_cast<unsigned long>(candidate.state.entries.size()));
				if( unchanged && worker->CanApply(candidate) )
				{
					gShadowPending = ShadowPendingUpdate();
					gShadowPendingBaseline.reset();
					WriteDiagnostic("state_shadow unchanged_apply_skipped=1");
				}
				else
				{
					const bool applied = ApplyShadowPending(!gShadowPending.retryUnavailable);
					InvalidateShadowApplied();
					if( applied && worker->CanApply(candidate) && !gShadowNavigation.load() )
					{
						const ULONGLONG copyStarted = ::GetTickCount64();
						try { gShadowAppliedState.reset(new DirectorySnapshot::State(candidate.state)); }
						catch( ... ) { gShadowAppliedState.reset(); }
						if( gShadowAppliedState && ::GetEnvironmentVariableA("LEYEES_STATE_SKIP_UNCHANGED_EXPERIMENT", nullptr, 0) )
						{
							try
							{
								gShadowAppliedNames.reserve(gShadowAppliedState->entries.size());
								for( const auto& entry : gShadowAppliedState->entries ) gShadowAppliedNames.push_back(entry.name);
								std::sort(gShadowAppliedNames.begin(), gShadowAppliedNames.end(), ShadowNameLess);
								gShadowAppliedIndexReady = true;
							}
							catch( ... ) { gShadowAppliedNames.clear(); gShadowAppliedIndexReady = false; }
						}
						if( ::GetEnvironmentVariableA("LEYEES_STATE_SKIP_UNCHANGED_EXPERIMENT", nullptr, 0) )
							WriteDiagnostic("state_shadow applied_copy=%d elapsed_ms=%llu entries=%lu",
								gShadowAppliedState ? 1 : 0, ::GetTickCount64() - copyStarted,
								static_cast<unsigned long>(candidate.state.entries.size()));
					}
				}
			}
			const int same = finalValid && gShadowLastState
				? (DirectorySnapshot::Same(*gShadowLastState, candidate.state) ? 1 : 0) : -1;
			WriteDiagnostic("state_shadow candidate_valid=%d entries=%lu list_count=%ld posts=%lu same=%d scope=%lu,%lu,%lu",
				finalValid ? 1 : 0, static_cast<unsigned long>(candidate.state.entries.size()),
				static_cast<long>(::SendMessageA(window, LVM_GETITEMCOUNT, 0, 0)),
				gShadowPosts.load(std::memory_order_relaxed), same,
				gShadowScopeRelated.load(), gShadowScopeDisjoint.load(), gShadowScopeUnknown.load());
			worker->FinishApply(candidate, finalValid);
			if( finalValid ) gShadowLastState.reset(new (std::nothrow) DirectorySnapshot::State(std::move(candidate.state)));
		}
		if( worker->Failed() ) WriteDiagnostic("state_shadow worker_failed=1");
		if( gShadowPending.window == window && (!gShadowPending.retryUnavailable || ::GetTickCount64() >= gShadowPending.deadline) && (worker->Failed()
			|| ::GetTickCount64() >= gShadowPending.deadline) )
		{
			if( gShadowPending.correction && !gShadowPending.retryUnavailable
				&& !::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_PRIMARY", nullptr, 0) )
			{
				WriteDiagnostic("state_shadow correction_unavailable=1");
				gShadowPending = ShadowPendingUpdate();
				gShadowPendingBaseline.reset();
			}
			else
			{
				// A failed correction gets one final legacy refresh, not another
				// self-perpetuating correction when the watcher stays unavailable.
				const bool verifyFallback = !gShadowPending.correction;
				const bool retryUnavailable = gShadowPending.retryUnavailable;
				const bool applied = ApplyShadowPending(!retryUnavailable && ::GetEnvironmentVariableA(
					"LEYEES_STATE_FAST_FALLBACK_EXPERIMENT", nullptr, 0) != 0, verifyFallback);
				if( applied && retryUnavailable && watched )
				{
					gListWatchBootstrapAttempted = false;
					try { watched->RequestRefresh(); } catch( ... ) {}
				}
			}
		}
		return 0;
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

void StartShadowWorker(HWND window, const wchar_t* target)
{
	if( !::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_WORKER", nullptr, 0)
		|| !window || !target || !*target ) return;
	const UINT_PTR id = reinterpret_cast<UINT_PTR>(&ShadowWorkerSubclass);
	if( !::SetWindowSubclass(window, ShadowWorkerSubclass, id, 0) ) return;
	if( !::SetTimer(window, id, 300, nullptr) )
	{
		::RemoveWindowSubclass(window, ShadowWorkerSubclass, id);
		return;
	}
	StateSyncWorker* worker = gShadowWorker.load(std::memory_order_acquire);
	if( !worker )
	{
		wchar_t module[MAX_PATH * 4] = {};
		const DWORD length = ::GetModuleFileNameW(gInjectedModule, module, _countof(module));
		if( !length || length >= _countof(module) ) return;
		wchar_t* slash = wcsrchr(module, L'\\');
		if( !slash || wcscpy_s(slash + 1, _countof(module) - (slash + 1 - module), L"LeeyesW.exe") ) return;
		HMODULE pinned = nullptr;
		if( !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&ShadowWorkerSubclass), &pinned) ) return;
		char intervalText[16] = {};
		const DWORD intervalLength = ::GetEnvironmentVariableA("LEYEES_STATE_INTERVAL_MS", intervalText, _countof(intervalText));
		unsigned interval = intervalLength && intervalLength < _countof(intervalText) ? strtoul(intervalText, nullptr, 10) : 300;
		if( interval < 300 || interval > 5000 ) interval = 300;
		worker = new (std::nothrow) StateSyncWorker(module, 5000, interval);
		if( !worker ) return;
		if( !worker->Start() ) { delete worker; return; }
		gShadowWorker.store(worker, std::memory_order_release);
		WriteDiagnostic("state_shadow worker_started=1");
	}
	if( ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_SHADOW", nullptr, 0)
		&& !gListWatchedShadow.load(std::memory_order_acquire) )
	{
		wchar_t module[MAX_PATH * 4] = {};
		const DWORD length = ::GetModuleFileNameW(gInjectedModule, module, _countof(module));
		if( !length || length >= _countof(module) ) return;
		wchar_t* slash = wcsrchr(module, L'\\');
		if( !slash || wcscpy_s(slash + 1, _countof(module) - (slash + 1 - module), L"LeeyesW.exe") ) return;
		WatchedSyncWorker* watched = new (std::nothrow) WatchedSyncWorker(module);
		if( !watched ) return;
		if( !watched->Start() ) { delete watched; return; }
		gListWatchedShadow.store(watched, std::memory_order_release);
		// A watcher enabled after the legacy worker was started still needs an
		// initial target even when the current path has not changed.
		if( gShadowTarget == target ) { try { watched->SetTarget(target); } catch( ... ) {} }
		WriteDiagnostic("state_shadow list_watched_started=1");
	}
	if( gShadowTarget != target )
	{
		gListVerifyPending = ShadowPendingUpdate(); gListVerifyState.reset();
		gListWatchCommitted = ShadowPendingUpdate();
		gListWatchCommittedState.reset();
		gListWatchBootstrapAttempted = false;
		gShadowTarget = target;
		::AcquireSRWLockExclusive(&gShadowScopeLock);
		wcscpy_s(gShadowScopeTarget, target);
		::ReleaseSRWLockExclusive(&gShadowScopeLock);
		gShadowLastState.reset();
		InvalidateShadowApplied();
		worker->SetTarget(gShadowTarget);
		WatchedSyncWorker* watched = gListWatchedShadow.load(std::memory_order_acquire);
		if( watched ) { try { watched->SetTarget(gShadowTarget); } catch( ... ) {} }
		char targetUtf8[MAX_PATH * 4] = {};
		::WideCharToMultiByte(CP_UTF8, 0, target, -1, targetUtf8, sizeof(targetUtf8), nullptr, nullptr);
		WriteDiagnostic("state_shadow target_changed=1 path_utf8=[%s]", targetUtf8);
	}
}
#else
#define WriteDiagnostic(...) ((void)0)
#endif

int ReadQueuedChangeCount(void* notifier)
{
	if( !notifier ) return -1;
	// Reverse-verified TChangeNotifier layout in Leeyes 2.6.1:
	// notifier+0x0c is its change-item list, whose virtual method +0x14 is
	// Count.  The queue-drain target signature independently verifies the same
	// accesses before this helper is ever enabled.
	int count = -1;
	__try
	{
		void* queue = *reinterpret_cast<void**>(reinterpret_cast<BYTE*>(notifier) + 0x0C);
		if( !queue ) return -1;
		void* vmt = *reinterpret_cast<void**>(queue);
		if( !vmt ) return -1;
		__asm
		{
			mov eax, queue
			mov edx, vmt
			call dword ptr [edx + 14h]
			mov count, eax
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return -1; }
	return count;
}

BOOL CALLBACK FindListChild(HWND window, LPARAM parameter)
{
	ListWindowSearch* search = reinterpret_cast<ListWindowSearch*>(parameter);
	char className[64] = {};
	if( !::GetClassNameA(window, className, _countof(className))
		|| strcmp(className, "TAcvListView") != 0 ) return TRUE;
	const int itemCount = static_cast<int>(::SendMessageA(window, LVM_GETITEMCOUNT, 0, 0));
	if( !search->bestWindow || itemCount > search->bestItemCount )
	{
		search->bestWindow = window;
		search->bestItemCount = itemCount;
	}
	return TRUE;
}

BOOL CALLBACK FindListTopLevel(HWND window, LPARAM parameter)
{
	ListWindowSearch* search = reinterpret_cast<ListWindowSearch*>(parameter);
	DWORD processId = 0;
	::GetWindowThreadProcessId(window, &processId);
	if( processId == search->processId )
		::EnumChildWindows(window, &FindListChild, parameter);
	return TRUE;
}

HWND FindFileListWindow()
{
	ListWindowSearch search = { ::GetCurrentProcessId(), nullptr, -1 };
	::EnumWindows(&FindListTopLevel, reinterpret_cast<LPARAM>(&search));
	return search.bestWindow;
}

BOOL CALLBACK FindDisplayedListChild(HWND window, LPARAM parameter)
{
	DisplayedListSearch* search =
		reinterpret_cast<DisplayedListSearch*>(parameter);
	char className[64] = {};
	if( !::GetClassNameA(window, className, _countof(className))
		|| strcmp(className, "TAcvListView") != 0 ) return TRUE;
	++search->listCount;
	if( ::IsWindowVisible(window) ) ++search->visibleListCount;
	else ++search->hiddenListCount;
	if( search->listWindow == nullptr )
	{
		search->listWindow = window;
		search->windowUserData = ::GetWindowLongPtrA(window, GWLP_USERDATA);
	}
	return TRUE;
}

BOOL CALLBACK FindDisplayedListTopLevel(HWND window, LPARAM parameter)
{
	DisplayedListSearch* search =
		reinterpret_cast<DisplayedListSearch*>(parameter);
	DWORD processId = 0;
	::GetWindowThreadProcessId(window, &processId);
	if( processId == search->processId && ::IsWindowVisible(window) )
		::EnumChildWindows(window, &FindDisplayedListChild, parameter);
	return TRUE;
}

bool FindDisplayedListWindow(HWND& window)
{
	DisplayedListSearch search = { ::GetCurrentProcessId(), nullptr, 0, 0, 0 };
	::EnumWindows(&FindDisplayedListTopLevel, reinterpret_cast<LPARAM>(&search));
	WriteDiagnostic("scope_scan lists=%lu visible=%lu hidden=%lu hwnd=%p user=%p",
		static_cast<unsigned long>(search.listCount),
		static_cast<unsigned long>(search.visibleListCount),
		static_cast<unsigned long>(search.hiddenListCount),
		search.listWindow, reinterpret_cast<void*>(search.windowUserData));
	if( search.listCount != 1 || !search.listWindow ) return false;
	window = search.listWindow;
	return true;
}

bool IsReadableRange(const BYTE* address, size_t length)
{
	if( !address || length == 0 ) return false;
	MEMORY_BASIC_INFORMATION information = {};
	if( ::VirtualQuery(address, &information, sizeof(information)) == 0 ) return false;
	if( information.State != MEM_COMMIT
		|| (information.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY
			| PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 )
		return false;
	const BYTE* end = reinterpret_cast<const BYTE*>(information.BaseAddress)
		+ information.RegionSize;
	return address <= end && length <= static_cast<size_t>(end - address);
}

bool CopyDelphiAnsiString(DWORD pointer, char* destination, size_t capacity,
	DWORD& declaredLength)
{
	declaredLength = 0;
	if( !destination || capacity < 2 ) return false;
	destination[0] = '\0';
	if( !pointer ) return true;
	if( pointer < sizeof(DWORD) ) return false;
	const BYTE* data = reinterpret_cast<const BYTE*>(pointer);
	const BYTE* lengthAddress = data - sizeof(DWORD);
	if( !IsReadableRange(lengthAddress, sizeof(DWORD)) ) return false;
	__try
	{
		declaredLength = *reinterpret_cast<const DWORD*>(lengthAddress);
		if( declaredLength >= capacity ) return false;
		if( declaredLength != 0 && !IsReadableRange(data, declaredLength) ) return false;
		if( declaredLength != 0 ) memcpy(destination, data, declaredLength);
		destination[declaredLength] = '\0';
	}
	__except( EXCEPTION_EXECUTE_HANDLER )
	{
		destination[0] = '\0';
		declaredLength = 0;
		return false;
	}
	return true;
}

#if UNICODEHACK_PATH_DEBUG
bool CopyPostedWideString(DWORD pointer, wchar_t* output, size_t capacity)
{
	if( !pointer || pointer < sizeof(DWORD) || !output || capacity < 2 ) return false;
	const BYTE* bytes = reinterpret_cast<const BYTE*>(pointer);
	if( !IsReadableRange(bytes - sizeof(DWORD), sizeof(DWORD)) ) return false;
	__try
	{
		const DWORD byteLength = *reinterpret_cast<const DWORD*>(bytes - sizeof(DWORD));
		if( !byteLength || (byteLength & 1) || byteLength / sizeof(wchar_t) >= capacity
			|| !IsReadableRange(bytes, byteLength) ) return false;
		memcpy(output, bytes, byteLength);
		output[byteLength / sizeof(wchar_t)] = L'\0';
		return wcslen(output) == byteLength / sizeof(wchar_t);
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}

void CountTreePostClass(DWORD record)
{
	DWORD first = 0, second = 0; BYTE action = 0;
	if( !record || !IsReadableRange(reinterpret_cast<const BYTE*>(record), 12) ) { ++gTreePostUnreadable; return; }
	__try
	{
		const BYTE* item = reinterpret_cast<const BYTE*>(record);
		action = item[0]; first = *reinterpret_cast<const DWORD*>(item + 4);
		second = *reinterpret_cast<const DWORD*>(item + 8);
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { ++gTreePostUnreadable; return; }
	wchar_t path0[1025] = {}, path1[1025] = {};
	if( action > 5 || !CopyPostedWideString(first, path0, _countof(path0))
		|| ((action == 1 || action == 4) && !CopyPostedWideString(second, path1, _countof(path1))) )
	{ ++gTreePostUnreadable; return; }
	for( wchar_t* path : { path0, path1 } )
		for( wchar_t* p = path; *p; ++p ) if( *p >= L'A' && *p <= L'Z' ) *p += L'a' - L'A';
	if( wcsstr(path0, L"\\folder_source\\") || wcsstr(path1, L"\\folder_source\\")
		|| wcsstr(path0, L"\\folder_destination\\") || wcsstr(path1, L"\\folder_destination\\") )
		++gTreePostFixture;
	else if( wcsstr(path0, L"\\leeyes_diag_") || wcsstr(path1, L"\\leeyes_diag_") ) ++gTreePostDiagnostic;
	else
	{
		++gTreePostOther;
		const size_t n0 = wcslen(path0), n1 = wcslen(path1);
		const wchar_t* names[] = { L"\\folder_source", L"\\folder_destination" };
		bool parent = false;
		for( const wchar_t* name : names )
		{
			const size_t n = wcslen(name);
			parent = parent || (n0 >= n && wcscmp(path0 + n0 - n, name) == 0)
				|| (n1 >= n && wcscmp(path1 + n1 - n, name) == 0);
		}
		if( parent ) ++gTreePostParent;
		else if( wcsstr(path0, L"\\users\\") || wcsstr(path1, L"\\users\\") ) ++gTreePostUser;
		else if( wcsstr(path0, L"\\github\\") || wcsstr(path1, L"\\github\\") ) ++gTreePostRepo;
		else if( wcsstr(path0, L"\\llm\\models\\") || wcsstr(path1, L"\\llm\\models\\") ) ++gTreePostModels;
		else ++gTreePostRemaining;
	}
}

extern "C" void __cdecl LeeyesDiagnosticTreeLogPostClasses()
{
	if( !::GetEnvironmentVariableA("LEYEES_TREE_STATE_POST_CLASSES", nullptr, 0) ) return;
	WriteDiagnostic("state_shadow tree_post_classes fixture=%lu diagnostic=%lu other=%lu unreadable=%lu",
		gTreePostFixture.exchange(0), gTreePostDiagnostic.exchange(0), gTreePostOther.exchange(0), gTreePostUnreadable.exchange(0));
	WriteDiagnostic("state_shadow tree_post_other parent=%lu user=%lu repo=%lu models=%lu remaining=%lu",
		gTreePostParent.exchange(0), gTreePostUser.exchange(0), gTreePostRepo.exchange(0), gTreePostModels.exchange(0), gTreePostRemaining.exchange(0));
	WriteDiagnostic("state_shadow tree_post_scope_skipped=%lu", gTreePostSkipped.exchange(0));
	WriteDiagnostic("state_shadow tree_test_late_post_ignored=%lu", gTreeTestLateIgnored.exchange(0));
}

NotificationScope::Relation ClassifyPostedScope(DWORD record)
{
	if( !record || !IsReadableRange(reinterpret_cast<const BYTE*>(record), 12) )
		return NotificationScope::Relation::Unknown;
	DWORD first = 0, second = 0; BYTE action = 0;
	__try
	{
		const BYTE* item = reinterpret_cast<const BYTE*>(record);
		action = item[0];
		first = *reinterpret_cast<const DWORD*>(item + 4);
		second = *reinterpret_cast<const DWORD*>(item + 8);
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return NotificationScope::Relation::Unknown; }
	if( action > 5 ) return NotificationScope::Relation::Unknown;
	wchar_t path0[1025] = {}, path1[1025] = {};
	if( !CopyPostedWideString(first, path0, _countof(path0)) ) return NotificationScope::Relation::Unknown;
	const bool rename = action == 1 || action == 4;
	if( rename && !CopyPostedWideString(second, path1, _countof(path1)) ) return NotificationScope::Relation::Unknown;
	::AcquireSRWLockShared(&gShadowScopeLock);
	const NotificationScope::Relation a = NotificationScope::Compare(gShadowScopeTarget, path0);
	const NotificationScope::Relation b = rename ? NotificationScope::Compare(gShadowScopeTarget, path1)
		: NotificationScope::Relation::LexicallyDisjoint;
	::ReleaseSRWLockShared(&gShadowScopeLock);
	if( a == NotificationScope::Relation::Related || b == NotificationScope::Relation::Related )
		return NotificationScope::Relation::Related;
	if( a == NotificationScope::Relation::Unknown || b == NotificationScope::Relation::Unknown )
		return NotificationScope::Relation::Unknown;
	return NotificationScope::Relation::LexicallyDisjoint;
}

// Diagnostic fixture only. Lexical inequality is not a proof of filesystem
// inequality (junctions, aliases and network paths can overlap). The original
// notification still runs; only this fixture's state-generation probe ignores
// the post. Never enable this experiment in a product configuration.
bool FixturePostLexicallyDisjoint(DWORD record)
{
	if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0)
		|| ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DISCOVER_PARENTS", nullptr, 0)
		|| !::GetEnvironmentVariableA("LEYEES_TREE_STATE_FIXTURE_DISJOINT", nullptr, 0)
		|| !::GetEnvironmentVariableA("LEYEES_TREE_FIXTURE_BOTH", nullptr, 0)
		|| !record || !IsReadableRange(reinterpret_cast<const BYTE*>(record), 12) ) return false;
	DWORD first = 0, second = 0; BYTE action = 0;
	__try
	{
		const BYTE* item = reinterpret_cast<const BYTE*>(record);
		action = item[0]; first = *reinterpret_cast<const DWORD*>(item + 4);
		second = *reinterpret_cast<const DWORD*>(item + 8);
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	wchar_t target0[1025] = {}, target1[1025] = {}, path0[1025] = {}, path1[1025] = {};
	const DWORD len0 = ::GetEnvironmentVariableW(L"LEYEES_TREE_STATE_PARENT", target0, _countof(target0));
	const DWORD len1 = ::GetEnvironmentVariableW(L"LEYEES_TREE_STATE_OTHER_PARENT", target1, _countof(target1));
	if( !len0 || len0 >= _countof(target0) || !len1 || len1 >= _countof(target1) || action > 5
		|| !CopyPostedWideString(first, path0, _countof(path0)) ) return false;
	const bool rename = action == 1 || action == 4;
	if( rename && !CopyPostedWideString(second, path1, _countof(path1)) ) return false;
	if( NotificationScope::Compare(target0, path0) != NotificationScope::Relation::LexicallyDisjoint
		|| NotificationScope::Compare(target1, path0) != NotificationScope::Relation::LexicallyDisjoint ) return false;
	return !rename || (NotificationScope::Compare(target0, path1) == NotificationScope::Relation::LexicallyDisjoint
		&& NotificationScope::Compare(target1, path1) == NotificationScope::Relation::LexicallyDisjoint);
}

// Narrow fault injection for a dedicated Debug fixture. It suppresses only
// the legacy generation signal for one known child path; the original queue
// and callback remain untouched. Never use this for production decisions.
bool FixtureTestIgnoreLatePost(DWORD record)
{
	if( !::GetEnvironmentVariableA("LEYEES_TREE_WATCH_TEST_IGNORE_LATE", nullptr, 0)
		|| !::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_PRIMARY", nullptr, 0)
		|| !record || !IsReadableRange(reinterpret_cast<const BYTE*>(record), 12) ) return false;
	DWORD first = 0; BYTE action = 0;
	__try
	{
		const BYTE* item = reinterpret_cast<const BYTE*>(record);
		action = item[0]; first = *reinterpret_cast<const DWORD*>(item + 4);
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	if( action != 3 ) return false;
	wchar_t parent[1025] = {}, path[1025] = {};
	const DWORD length = ::GetEnvironmentVariableW(L"LEYEES_TREE_STATE_OTHER_PARENT", parent, _countof(parent));
	if( !length || length >= _countof(parent) || !CopyPostedWideString(first, path, _countof(path)) ) return false;
	const wchar_t suffix[] = L"\\__folder_000\\__late_probe";
	if( length + _countof(suffix) > _countof(parent) ) return false;
	wcscat_s(parent, suffix);
	return ::CompareStringOrdinal(parent, -1, path, -1, TRUE) == CSTR_EQUAL;
}

void ProbeOwnerPidl(void* owner)
{
	if( !::GetEnvironmentVariableA("LEYEES_LIST_PIDL_PROBE", nullptr, 0)
		&& !::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_WORKER", nullptr, 0) ) return;
	// In this specific shell-item class virtual method +8 returns a cached PIDL
	// at object+8. Inspect only an already-populated value; never manufacture or
	// release it, and do not assume that non-filesystem shell items have paths.
	BYTE* image = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	const BYTE signature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xF0,0x53,0x56,0x57 };
	if( !image || memcmp(image + 0xBC29C, signature, sizeof(signature)) != 0 ) return;
	HMODULE shell = ::GetModuleHandleW(L"shell32.dll");
	typedef BOOL (WINAPI *GetPath)(const void*, LPWSTR);
	GetPath getPath = shell ? reinterpret_cast<GetPath>(::GetProcAddress(shell, "SHGetPathFromIDListW")) : nullptr;
	if( !getPath ) return;
	wchar_t path[MAX_PATH] = {};
	DWORD objectAddress = 0, pidl = 0;
	bool success = false;
	__try
	{
		BYTE* object = *reinterpret_cast<BYTE**>(reinterpret_cast<BYTE*>(owner) + 0x538);
		objectAddress = reinterpret_cast<DWORD>(object);
		DWORD* vmt = object ? *reinterpret_cast<DWORD**>(object) : nullptr;
		if( vmt && vmt[2] == reinterpret_cast<DWORD>(image + 0xBC29C) )
		{
			pidl = *reinterpret_cast<DWORD*>(object + 8);
			if( pidl ) success = getPath(reinterpret_cast<void*>(pidl), path) != FALSE;
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { success = false; }
	char utf8[MAX_PATH * 4] = {};
	if( success && !::WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr) ) success = false;
	WriteDiagnostic("scope_pidl object=%08lX pidl=%08lX success=%d path_utf8=[%s]",
		static_cast<unsigned long>(objectAddress), static_cast<unsigned long>(pidl), success ? 1 : 0, success ? utf8 : "");
	if( success )
	{
		wcscpy_s(gDrainState.pidlDirectory, path);
		gDrainState.pidlDirectoryKnown = true;
		gShadowOwner.store(reinterpret_cast<DWORD>(owner));
		StartShadowWorker(gDrainState.currentListWindow, path);
	}
}

void LogDispatchSample(void* dispatchOwner, DWORD rawAction, DWORD path0,
	DWORD path1, void* returnAddress)
{
	if( !gDiagnosticLog || gDrainState.sampledChanges >= 16 ) return;
	++gDrainState.sampledChanges;
	char text0[2048] = {};
	char text1[2048] = {};
	DWORD length0 = 0;
	DWORD length1 = 0;
	const bool valid0 = CopyDelphiAnsiString(path0, text0, _countof(text0), length0);
	const bool valid1 = CopyDelphiAnsiString(path1, text1, _countof(text1), length1);
	WriteDiagnostic("dispatch_sample index=%lu action=%lu raw_action=0x%08lX owner=%p caller=%p path0_valid=%d path0_len=%lu path0=[%s] path1_valid=%d path1_len=%lu path1=[%s]",
		static_cast<unsigned long>(gDrainState.sampledChanges),
		static_cast<unsigned long>(rawAction & 0xFF),
		static_cast<unsigned long>(rawAction), dispatchOwner, returnAddress,
		valid0 ? 1 : 0, static_cast<unsigned long>(length0),
		valid0 ? text0 : "<invalid>", valid1 ? 1 : 0,
		static_cast<unsigned long>(length1), valid1 ? text1 : "<invalid>");
}
#endif

bool DispatchOwnerContainsListWindow(void* dispatchOwner, HWND listWindow)
{
	if( !dispatchOwner || !listWindow ) return false;
	const DWORD windowValue = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(listWindow));
	const BYTE* object = reinterpret_cast<const BYTE*>(dispatchOwner);
	if( !IsReadableRange(object, sizeof(DWORD)) ) return false;
	// Do not assume a private field offset.  The verified dispatch owner is a
	// Delphi ListView object; find the live HWND value in its readable object
	// storage and bind only on an exact runtime relationship.
	for( size_t offset = 0; offset < 0x1000; offset += sizeof(DWORD) )
	{
		if( !IsReadableRange(object + offset, sizeof(DWORD)) ) return false;
		DWORD value = 0;
		__try { value = *reinterpret_cast<const DWORD*>(object + offset); }
		__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
		if( value == windowValue ) return true;
	}
	return false;
}

// Only bind a batch to a folder when the current Leeyes shell object exposes
// its already-cached filesystem PIDL. Failure always keeps original dispatch.
// The byte conversion must be lossless: replacement characters or best-fit
// mappings could make two distinct Unicode directories appear identical.
bool ReadOwnerDirectory(void* owner, char* output, size_t capacity)
{
	if( !owner || !output || capacity == 0 ) return false;
	output[0] = '\0';
	BYTE* image = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	const BYTE signature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xF0,0x53,0x56,0x57 };
	if( !image || !IsReadableRange(image + 0xBC29C, sizeof(signature))
		|| memcmp(image + 0xBC29C, signature, sizeof(signature)) != 0 ) return false;
	HMODULE shell = ::GetModuleHandleW(L"shell32.dll");
	typedef BOOL (WINAPI *GetPath)(const void*, LPWSTR);
	GetPath getPath = shell ? reinterpret_cast<GetPath>(::GetProcAddress(shell, "SHGetPathFromIDListW")) : nullptr;
	if( !getPath ) return false;
	wchar_t path[MAX_PATH] = {};
	bool success = false;
	__try
	{
		BYTE* object = *reinterpret_cast<BYTE**>(reinterpret_cast<BYTE*>(owner) + 0x538);
		DWORD* vmt = object ? *reinterpret_cast<DWORD**>(object) : nullptr;
		if( vmt && vmt[2] == reinterpret_cast<DWORD>(image + 0xBC29C) )
		{
			DWORD pidl = *reinterpret_cast<DWORD*>(object + 8);
			if( pidl ) success = getPath(reinterpret_cast<void*>(pidl), path) != FALSE;
		}
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	if( !success || !path[0] ) return false;
	return capacity <= INT_MAX && ExactAnsiPath(CP_ACP, path, output, static_cast<int>(capacity));
}

bool GetListItemText(HWND window, int index, std::string& text)
{
	char buffer[1024] = {};
	LVITEMA item = {};
	item.iSubItem = 0;
	item.pszText = buffer;
	item.cchTextMax = _countof(buffer);
	LRESULT length = ::SendMessageA(window, LVM_GETITEMTEXTA,
		static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item));
	if( length < 0 ) return false;
	text.assign(buffer, static_cast<size_t>(length));
	return true;
}

bool IsPathSeparator(char value)
{
	return value == '\\' || value == '/';
}

bool SplitAbsolutePath(const char* path, char* directory, size_t capacity,
	const char*& leaf)
{
	leaf = nullptr;
	if( !path || !*path || !directory || capacity < 4 ) return false;
	const size_t pathLength = strlen(path);
	const bool drivePath = pathLength >= 3 && path[1] == ':'
		&& IsPathSeparator(path[2]);
	const bool uncPath = pathLength >= 2
		&& IsPathSeparator(path[0]) && IsPathSeparator(path[1]);
	if( !drivePath && !uncPath ) return false;
	const char* backslash = strrchr(path, '\\');
	const char* slash = strrchr(path, '/');
	const char* separator = backslash;
	if( !separator || (slash && slash > separator) ) separator = slash;
	if( !separator || !separator[1] ) return false;
	size_t length = static_cast<size_t>(separator - path);
	if( drivePath && length == 2 ) ++length;
	if( length == 0 || length >= capacity ) return false;
	memcpy(directory, path, length);
	directory[length] = '\0';
	leaf = separator + 1;
	return true;
}

bool SameDirectory(const char* left, const char* right)
{
	// Case-sensitive directories may contain names differing only by case.
	// An exact-byte mismatch is safe to pass through to the original handler.
	return left && right && *left && *right && strcmp(left, right) == 0;
}

#if UNICODEHACK_PATH_DEBUG
// Test only: never skip a notification for a name that was present in the
// last applied snapshot. Existing files may have changed contents without
// changing the metadata used by DirectorySnapshot::Same.
bool IsKnownTransientDiagnostic(DWORD action, DWORD path0, DWORD path1, unsigned& reason)
{
	reason = 0;
	if( !gShadowAppliedState || !gShadowAppliedIndexReady ) { reason = 1; return false; }
	if( action != 0 && action != 2 ) { reason = 2; return false; }
	char first[kScopePathCapacity] = {}, second[kScopePathCapacity] = {}, directory[kScopePathCapacity] = {};
	DWORD firstLength = 0, secondLength = 0;
	const char* leaf = nullptr;
	if( !CopyDelphiAnsiString(path0, first, _countof(first), firstLength)
		|| !CopyDelphiAnsiString(path1, second, _countof(second), secondLength)
		|| !firstLength || secondLength || memchr(first, '\0', firstLength)
		|| !SplitAbsolutePath(first, directory, _countof(directory), leaf)
		|| !SameDirectory(directory, gDrainState.currentDirectory) ) { reason = 3; return false; }
	// The Unicode hook can expose a synthetic ~u name to ANSI code. Do not
	// equate such a name with a real filesystem leaf based on bytes alone.
	if( leaf[0] == '~' && (leaf[1] == 'u' || leaf[1] == 'U') ) { reason = 4; return false; }
	try
	{
		std::wstring name;
		for( const unsigned char* p = reinterpret_cast<const unsigned char*>(leaf); *p; ++p )
		{
			if( *p >= 0x80 || *p == '?' ) { reason = 5; return false; }
			name.push_back(static_cast<wchar_t>(*p));
		}
		if( std::binary_search(gShadowAppliedNames.begin(), gShadowAppliedNames.end(), name, ShadowNameLess) )
		{ reason = 6; return false; }
		return true;
	}
	catch( ... ) { reason = 7; return false; }
}
#endif

bool IsUsableLeafName(const char* leaf)
{
	return leaf && *leaf && strcmp(leaf, ".") != 0 && strcmp(leaf, "..") != 0
		&& strchr(leaf, '\\') == nullptr && strchr(leaf, '/') == nullptr;
}

bool BuildChildPath(const char* directory, const char* leaf, char* path,
	size_t capacity)
{
	if( !directory || !*directory || !IsUsableLeafName(leaf)
		|| !path || capacity == 0 ) return false;
	const size_t directoryLength = strlen(directory);
	const size_t leafLength = strlen(leaf);
	const bool needsSeparator = !IsPathSeparator(directory[directoryLength - 1]);
	if( directoryLength + (needsSeparator ? 1 : 0) + leafLength >= capacity )
		return false;
	strcpy_s(path, capacity, directory);
	if( needsSeparator ) strcat_s(path, capacity, "\\");
	strcat_s(path, capacity, leaf);
	return true;
}

bool ListContainsLeaf(HWND window, const char* leaf)
{
	if( !window || !IsUsableLeafName(leaf) ) return false;
	LVFINDINFOA find = {};
	find.flags = LVFI_STRING;
	find.psz = leaf;
	return ::SendMessageA(window, LVM_FINDITEMA, static_cast<WPARAM>(-1),
		reinterpret_cast<LPARAM>(&find)) >= 0;
}

void RememberScopeDirectory(const char* directory, bool displayed)
{
	if( !directory || !*directory ) return;
	for( DWORD index = 0; index < gDrainState.scopeDirectoryCount; ++index )
	{
		if( SameDirectory(gDrainState.scopeDirectories[index].path, directory) )
		{
			gDrainState.scopeDirectories[index].displayed = displayed;
			return;
		}
	}
	if( gDrainState.scopeDirectoryCount >= kMaximumScopeDirectories ) return;
	ScopeDirectoryResult& result =
		gDrainState.scopeDirectories[gDrainState.scopeDirectoryCount++];
	strncpy_s(result.path, _countof(result.path), directory, _TRUNCATE);
	result.displayed = displayed;
}

bool FindRememberedScopeDirectory(const char* directory, bool& displayed)
{
	for( DWORD index = 0; index < gDrainState.scopeDirectoryCount; ++index )
	{
		const ScopeDirectoryResult& result = gDrainState.scopeDirectories[index];
		if( SameDirectory(result.path, directory) )
		{
			displayed = result.displayed;
			return true;
		}
	}
	return false;
}

void BindCurrentDirectory(const char* directory, const char* evidence)
{
	if( !directory || !*directory ) return;
	strncpy_s(gDrainState.currentDirectory, _countof(gDrainState.currentDirectory),
		directory, _TRUNCATE);
	gDrainState.currentDirectoryKnown = true;
	RememberScopeDirectory(directory, true);
	WriteDiagnostic("scope_directory_bound path=[%s] evidence=%s",
		gDrainState.currentDirectory, evidence ? evidence : "unknown");
}

bool DirectoryMatchesDisplayedList(const char* directory)
{
	if( !directory || !*directory || !gDrainState.currentListWindow ) return false;
	bool remembered = false;
	if( FindRememberedScopeDirectory(directory, remembered) ) return remembered;
	const DWORD attributes = ::GetFileAttributesA(directory);
	if( attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) )
	{
		RememberScopeDirectory(directory, false);
		return false;
	}
	const int itemCount = static_cast<int>(::SendMessageA(
		gDrainState.currentListWindow, LVM_GETITEMCOUNT, 0, 0));
	if( itemCount <= 0 )
	{
		RememberScopeDirectory(directory, false);
		return false;
	}
	const int maximumProbes = 32;
	const int probeCount = itemCount < maximumProbes ? itemCount : maximumProbes;
	int usable = 0;
	int hits = 0;
	for( int probe = 0; probe < probeCount; ++probe )
	{
		const int index = probeCount == 1 ? 0
			: static_cast<int>((static_cast<__int64>(probe) * (itemCount - 1))
				/ (probeCount - 1));
		std::string item;
		if( !GetListItemText(gDrainState.currentListWindow, index, item)
			|| !IsUsableLeafName(item.c_str()) ) continue;
		++usable;
		char candidate[kScopePathCapacity] = {};
		if( BuildChildPath(directory, item.c_str(), candidate, _countof(candidate))
			&& ::GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES )
			++hits;
		if( hits >= 3 ) break;
	}
	const bool displayed = hits >= 3 || (itemCount < 3 && usable > 0 && hits == usable);
	RememberScopeDirectory(directory, displayed);
	WriteDiagnostic("scope_directory_probe path=[%s] items=%d usable=%d hits=%d displayed=%d",
		directory, itemCount, usable, hits, displayed ? 1 : 0);
	return displayed;
}

enum class FileChangeScope
{
	Unknown,
	DisplayedDirectory,
	OtherDirectory
};

FileChangeScope ClassifyFileChangeScope(DWORD action, DWORD path0, DWORD path1)
{
	char text0[kScopePathCapacity] = {};
	char text1[kScopePathCapacity] = {};
	DWORD length0 = 0;
	DWORD length1 = 0;
	if( !CopyDelphiAnsiString(path0, text0, _countof(text0), length0)
		|| !CopyDelphiAnsiString(path1, text1, _countof(text1), length1) )
		return FileChangeScope::Unknown;
	char directory0[kScopePathCapacity] = {};
	char directory1[kScopePathCapacity] = {};
	const char* leaf0 = nullptr;
	const char* leaf1 = nullptr;
	const bool has0 = length0 != 0;
	const bool has1 = length1 != 0;
	const bool absolute0 = has0
		&& SplitAbsolutePath(text0, directory0, _countof(directory0), leaf0);
	const bool absolute1 = has1
		&& SplitAbsolutePath(text1, directory1, _countof(directory1), leaf1);
	if( (has0 && !absolute0) || (has1 && !absolute1) || (!absolute0 && !absolute1) )
		return FileChangeScope::Unknown;

	if( !gDrainState.currentDirectoryKnown )
	{
		if( absolute0 && DirectoryMatchesDisplayedList(directory0) )
			BindCurrentDirectory(directory0, "list_filesystem_probe");
		else if( absolute1 && DirectoryMatchesDisplayedList(directory1) )
			BindCurrentDirectory(directory1, "list_filesystem_probe");
		// Remove/rename callbacks still refer to the old item that is present in
		// the displayed ListView before dispatch. Use this as a fallback when a
		// large removal leaves too few files for the filesystem probe.
		else if( (action == 1 || action == 2 || action == 4 || action == 5)
			&& absolute0 && ListContainsLeaf(gDrainState.currentListWindow, leaf0) )
			BindCurrentDirectory(directory0, "existing_list_item");
	}
	if( !gDrainState.currentDirectoryKnown ) return FileChangeScope::OtherDirectory;
	if( (absolute0 && SameDirectory(directory0, gDrainState.currentDirectory))
		|| (absolute1 && SameDirectory(directory1, gDrainState.currentDirectory)) )
		return FileChangeScope::DisplayedDirectory;
	return FileChangeScope::OtherDirectory;
}

ListSelectionSnapshot CaptureListSelection()
{
	ListSelectionSnapshot snapshot = {};
	snapshot.lastSelectedIndex = -1;
	snapshot.originalFocusedIndex = -1;
	snapshot.window = FindFileListWindow();
	if( !snapshot.window ) return snapshot;
	int index = -1;
	while( (index = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETNEXTITEM,
		static_cast<WPARAM>(index), LVNI_SELECTED))) >= 0 )
	{
		snapshot.lastSelectedIndex = index;
		std::string text;
		if( GetListItemText(snapshot.window, index, text) && !text.empty() )
			++snapshot.selectedNames[text];
	}
	const int focused = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETNEXTITEM,
		static_cast<WPARAM>(-1), LVNI_FOCUSED));
	snapshot.originalFocusedIndex = focused;
	if( focused >= 0 ) GetListItemText(snapshot.window, focused, snapshot.focusedName);
	const int top = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETTOPINDEX, 0, 0));
	snapshot.originalTopIndex = top;
	#if UNICODEHACK_PATH_DEBUG
	if( top >= 0 && ::GetEnvironmentVariableA("LEYEES_STATE_PRESERVE_TOP_ITEM", nullptr, 0) )
		GetListItemText(snapshot.window, top, snapshot.topName);
	#endif
	const int page = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETCOUNTPERPAGE, 0, 0));
	snapshot.focusedWasVisible = focused >= top && top >= 0 && page > 0 && focused < top + page;
	return snapshot;
}

#if UNICODEHACK_PATH_DEBUG
int FindUniqueListText(HWND window, const std::string& name, int itemCount)
{
	if( name.empty() ) return -1;
	int cursor = -1, found = -1;
	for( int probes = 0; probes <= itemCount; ++probes )
	{
		LVFINDINFOA find = {}; find.flags = LVFI_STRING; find.psz = name.c_str();
		const int index = static_cast<int>(::SendMessageA(window, LVM_FINDITEMA,
			static_cast<WPARAM>(cursor), reinterpret_cast<LPARAM>(&find)));
		if( index < 0 || index <= cursor || index >= itemCount ) break;
		cursor = index; std::string text;
		if( !GetListItemText(window, index, text) ) return -1;
		if( text == name ) { if( found >= 0 ) return -1; found = index; }
	}
	return found;
}
#endif
bool RestoreListSelectionByFind(const ListSelectionSnapshot& snapshot, int itemCount,
	DWORD& restored, int& focusedIndex)
{
	#if UNICODEHACK_PATH_DEBUG
	if( !::GetEnvironmentVariableA("LEYEES_SELECTION_FAST_EXPERIMENT", nullptr, 0) ) return false;
	#endif
	// Limit repeated searches and keep early selections on the linear scan,
	// which can finish without visiting the rest of the list.
	if( itemCount < 256 || snapshot.selectedNames.size() > 16
		|| (snapshot.lastSelectedIndex < itemCount / 2 && snapshot.originalFocusedIndex < itemCount / 2) ) return false;
	for( const auto& selected : snapshot.selectedNames ) if( selected.second != 1 ) return false;
	try
	{
	std::map<std::string, DWORD> wanted = snapshot.selectedNames;
	if( !snapshot.focusedName.empty() && wanted.find(snapshot.focusedName) == wanted.end() )
		wanted[snapshot.focusedName] = 0;
	std::vector<int> selectedIndices;
	int foundFocus = -1;
	for( std::map<std::string, DWORD>::const_iterator it = wanted.begin(); it != wanted.end(); ++it )
	{
		DWORD selectedFound = 0;
		const bool needFocus = it->first == snapshot.focusedName;
		int cursor = -1;
		for( int probes = 0; probes <= itemCount; ++probes )
		{
			if( selectedFound == it->second && (!needFocus || foundFocus >= 0) ) break;
			LVFINDINFOA find = {}; find.flags = LVFI_STRING; find.psz = it->first.c_str();
			const int index = static_cast<int>(::SendMessageA(snapshot.window, LVM_FINDITEMA,
				static_cast<WPARAM>(cursor), reinterpret_cast<LPARAM>(&find)));
			if( index < 0 || index <= cursor || index >= itemCount ) break;
			cursor = index;
			std::string text;
			if( !GetListItemText(snapshot.window, index, text) ) return false;
			if( text != it->first ) continue;
			if( needFocus && foundFocus < 0 ) foundFocus = index;
			if( selectedFound < it->second ) { selectedIndices.push_back(index); ++selectedFound; }
		}
		// An unsuccessful find may mean that the item was removed or that the
		// control's search semantics differ. Preserve the original scan in both cases.
		if( selectedFound != it->second || (needFocus && foundFocus < 0) ) return false;
	}
	// Match the original scan's ascending row order, including notification
	// order when setting multiple selected items.
	std::sort(selectedIndices.begin(), selectedIndices.end());
	#if UNICODEHACK_PATH_DEBUG
	if( ::GetEnvironmentVariableA("LEYEES_SELECTION_FAST_VALIDATE", nullptr, 0) )
	{
		std::map<std::string, DWORD> remaining = snapshot.selectedNames;
		std::vector<int> expected;
		int expectedFocus = -1;
		for( int i = 0; i < itemCount; ++i )
		{
			std::string text;
			if( !GetListItemText(snapshot.window, i, text) ) continue;
			std::map<std::string, DWORD>::iterator selected = remaining.find(text);
			if( selected != remaining.end() && selected->second ) { expected.push_back(i); --selected->second; }
			if( expectedFocus < 0 && !snapshot.focusedName.empty() && text == snapshot.focusedName ) expectedFocus = i;
		}
		const bool matches = selectedIndices == expected && foundFocus == expectedFocus;
		WriteDiagnostic("selection_find_shadow matches=%d expected=%lu found=%lu focus_equal=%d",
			matches ? 1 : 0, static_cast<unsigned long>(expected.size()),
			static_cast<unsigned long>(selectedIndices.size()), expectedFocus == foundFocus ? 1 : 0);
		if( !matches ) return false;
	}
	#endif
	for( size_t i = 0; i < selectedIndices.size(); ++i )
	{
		LVITEMA state = {}; state.stateMask = LVIS_SELECTED; state.state = LVIS_SELECTED;
		::SendMessageA(snapshot.window, LVM_SETITEMSTATE, static_cast<WPARAM>(selectedIndices[i]),
			reinterpret_cast<LPARAM>(&state));
	}
	restored = static_cast<DWORD>(selectedIndices.size());
	focusedIndex = foundFocus;
	return true;
	}
	catch( ... ) { return false; }
}
DWORD RestoreListSelection(const ListSelectionSnapshot& snapshot, bool preserveOffscreenFocus = false, bool preserveTopItem = false)
{
	if( !snapshot.window || !::IsWindow(snapshot.window) ) return 0;
	std::map<std::string, DWORD> remaining = snapshot.selectedNames;
	unsigned long long selectionsRemaining = 0;
	for( std::map<std::string, DWORD>::const_iterator it = remaining.begin(); it != remaining.end(); ++it )
		selectionsRemaining += it->second;
	DWORD restored = 0;
	int focusedIndex = -1;
	const int itemCount = static_cast<int>(::SendMessageA(
		snapshot.window, LVM_GETITEMCOUNT, 0, 0));
	int topIndex = itemCount > 0 && snapshot.originalTopIndex >= 0
		? (snapshot.originalTopIndex < itemCount ? snapshot.originalTopIndex : itemCount - 1)
		: -1;
	#if UNICODEHACK_PATH_DEBUG
	if( preserveTopItem )
	{
		const int anchor = FindUniqueListText(snapshot.window, snapshot.topName, itemCount);
		if( anchor >= 0 ) topIndex = anchor;
		WriteDiagnostic("state_shadow list_top_anchor_found=%d", anchor >= 0 ? 1 : 0);
	}
	#endif
	bool usedFast = false;
	usedFast = RestoreListSelectionByFind(snapshot, itemCount, restored, focusedIndex);
	#if UNICODEHACK_PATH_DEBUG
	WriteDiagnostic("selection_find used=%d", usedFast ? 1 : 0);
	#endif
	for( int index = 0; !usedFast && index < itemCount; ++index )
	{
		// Later rows cannot change the result once every requested occurrence
		// and the first matching focus have been found. Missing selections
		// still require the complete scan, preserving the original behaviour.
		if( selectionsRemaining == 0 && (snapshot.focusedName.empty() || focusedIndex >= 0) ) break;
		std::string text;
		if( !GetListItemText(snapshot.window, index, text) ) continue;
		std::map<std::string, DWORD>::iterator selected = remaining.find(text);
		if( selected != remaining.end() && selected->second != 0 )
		{
			LVITEMA state = {};
			state.stateMask = LVIS_SELECTED;
			state.state = LVIS_SELECTED;
			::SendMessageA(snapshot.window, LVM_SETITEMSTATE,
				static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&state));
			--selected->second;
			--selectionsRemaining;
			++restored;
		}
		if( focusedIndex < 0 && !snapshot.focusedName.empty()
			&& text == snapshot.focusedName ) focusedIndex = index;
	}
	if( focusedIndex >= 0 && (snapshot.focusedWasVisible || preserveOffscreenFocus) )
	{
		LVITEMA state = {};
		state.stateMask = LVIS_FOCUSED;
		state.state = LVIS_FOCUSED;
		::SendMessageA(snapshot.window, LVM_SETITEMSTATE,
			static_cast<WPARAM>(focusedIndex), reinterpret_cast<LPARAM>(&state));
	}
	if( topIndex >= 0 )
	{
		::SendMessageA(snapshot.window, LVM_ENSUREVISIBLE,
			static_cast<WPARAM>(topIndex), FALSE);
		const int currentTop = static_cast<int>(::SendMessageA(
			snapshot.window, LVM_GETTOPINDEX, 0, 0));
		RECT itemBounds = { LVIR_BOUNDS, 0, 0, 0 };
		if( currentTop >= 0 && currentTop != topIndex
			&& ::SendMessageA(snapshot.window, LVM_GETITEMRECT,
				static_cast<WPARAM>(currentTop), reinterpret_cast<LPARAM>(&itemBounds)) )
		{
			const int itemHeight = itemBounds.bottom - itemBounds.top;
			if( itemHeight > 0 )
				::SendMessageA(snapshot.window, LVM_SCROLL, 0,
					static_cast<LPARAM>((topIndex - currentTop) * itemHeight));
		}
	}
	#if UNICODEHACK_PATH_DEBUG
	if( ::GetEnvironmentVariableA("LEYEES_SELECTION_FULL_SCAN_VERIFY", nullptr, 0) )
	{
		std::map<std::string, DWORD> expectedRemaining = snapshot.selectedNames;
		std::vector<int> expectedIndices, actualIndices;
		for( int i = 0; i < itemCount; ++i )
		{
			std::string text;
			if( !GetListItemText(snapshot.window, i, text) ) continue;
			std::map<std::string, DWORD>::iterator found = expectedRemaining.find(text);
			if( found != expectedRemaining.end() && found->second )
			{ expectedIndices.push_back(i); --found->second; }
		}
		int index = -1;
		while( (index = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETNEXTITEM,
			static_cast<WPARAM>(index), LVNI_SELECTED))) >= 0 ) actualIndices.push_back(index);
		WriteDiagnostic("selection_full_scan_verify match=%d expected=%lu actual=%lu",
			expectedIndices == actualIndices ? 1 : 0,
			static_cast<unsigned long>(expectedIndices.size()), static_cast<unsigned long>(actualIndices.size()));
	}
	if( usedFast || ::GetEnvironmentVariableA("LEYEES_SELECTION_VERIFY", nullptr, 0) )
	{
		const ListSelectionSnapshot check = CaptureListSelection();
		WriteDiagnostic("selection_verify selected_equal=%d focused_equal=%d",
			check.selectedNames == snapshot.selectedNames ? 1 : 0,
			check.focusedName == snapshot.focusedName ? 1 : 0);
	}
	#endif
	return restored;
}

struct ListDeleteBatchState
{
	void* owner = nullptr;
	HWND window = nullptr;
	std::wstring target;
	std::unique_ptr<DirectorySnapshot::State> committed;
	std::unique_ptr<WatchedSyncWorker::Candidate> candidate;
	bool pendingApply = false;
	bool bootstrapPending = false;
	bool requestNeeded = false;
	bool requestOutstanding = false;
	bool subclassAttached = false;
	bool timerActive = false;
	bool fallbackDisabled = false;
	ULONGLONG pendingSince = 0;
	ULONGLONG requestSince = 0;
	ULONGLONG quietSince = 0;
	unsigned observedDirty = 0;
};
ListDeleteBatchState gListDeleteBatch;
WatchedSyncWorker* gListDeleteWorker = nullptr;
HMODULE gListDeletePinnedModule = nullptr;
const UINT_PTR kListDeleteTimer = reinterpret_cast<UINT_PTR>(&gListDeleteBatch);
const UINT kListDeleteIntervalMs = 300;
const ULONGLONG kListDeleteQuietMs = 300;
const ULONGLONG kListDeleteFallbackMs = 10000;

bool OnlyRegularFilesRemoved(const DirectorySnapshot::State& before, const DirectorySnapshot::State& after)
{
	if( !DirectorySnapshot::OnlyRemovals(before, after) ) return false;
	size_t j = 0;
	for( const DirectorySnapshot::Entry& oldEntry : before.entries )
	{
		while( j < after.entries.size() && after.entries[j].name < oldEntry.name ) ++j;
		if( j < after.entries.size() && after.entries[j].name == oldEntry.name ) { ++j; continue; }
		if( oldEntry.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
	}
	return true;
}

bool ExactAnsiDirectoryToWide(const char* source, std::wstring& destination)
{
	if( !source || !*source ) return false;
	const int length = ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1, nullptr, 0);
	if( length <= 0 || length > static_cast<int>(MAX_PATH * 4) ) return false;
	try
	{
		std::wstring wide(static_cast<size_t>(length), L'\0');
		if( !::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, source, -1, &wide[0], length) ) return false;
		BOOL substituted = FALSE;
		const int bytes = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1,
			nullptr, 0, nullptr, &substituted);
		if( !bytes || substituted || bytes != static_cast<int>(strlen(source)) + 1 ) return false;
		std::string roundtrip(static_cast<size_t>(bytes), '\0');
		if( !::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.c_str(), -1,
			&roundtrip[0], bytes, nullptr, &substituted) || substituted
			|| memcmp(roundtrip.data(), source, static_cast<size_t>(bytes - 1)) != 0 ) return false;
		wide.resize(static_cast<size_t>(length - 1));
		destination.swap(wide);
		return true;
	}
	catch( ... ) { return false; }
}

bool EnsureListDeleteWorker(HWND window, void* owner, const char* directory)
{
	if( !gListDeleteBatchEnabled || !window || !owner || !directory
		|| !::IsWindow(window) || !DispatchOwnerContainsListWindow(owner, window) ) return false;
	std::wstring target;
	if( !ExactAnsiDirectoryToWide(directory, target) ) return false;
	// Keep the state-confirmed worker to ordinary local fixed volumes until
	// network and removable-drive enumeration behavior is verified.
	if( target.size() < 3 || target[1] != L':' || (target[2] != L'\\' && target[2] != L'/') ) return false;
	wchar_t driveRoot[] = { target[0], L':', L'\\', L'\0' };
	if( ::GetDriveTypeW(driveRoot) != DRIVE_FIXED ) return false;
	DWORD attributes = ::GetFileAttributesW(target.c_str());
	if( attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)
		|| (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
	const bool sessionChanged = gListDeleteBatch.window != window || gListDeleteBatch.owner != owner
		|| gListDeleteBatch.target != target;
	if( !sessionChanged && gListDeleteBatch.fallbackDisabled ) return false;
	if( gListDeleteWorker && !sessionChanged )
	{
		if( !gListDeleteBatch.subclassAttached )
		{
			if( !::SetWindowSubclass(window, ListDeleteBatchSubclass, kListDeleteTimer, 0) ) return false;
			gListDeleteBatch.subclassAttached = true;
		}
		if( !::SetTimer(window, kListDeleteTimer, kListDeleteIntervalMs, nullptr) ) return false;
		gListDeleteBatch.timerActive = true;
		return true;
	}
	if( sessionChanged && gListDeleteBatch.window )
	{
		::KillTimer(gListDeleteBatch.window, kListDeleteTimer);
		::RemoveWindowSubclass(gListDeleteBatch.window, ListDeleteBatchSubclass, kListDeleteTimer);
		gListDeleteBatch.subclassAttached = false;
		gListDeleteBatch.timerActive = false;
	}
	if( gListDeleteBatch.candidate && gListDeleteWorker )
	{
		bool current = false;
		gListDeleteWorker->Release(*gListDeleteBatch.candidate, current);
	}
	if( !gListDeleteWorker )
	{
		wchar_t module[MAX_PATH * 4] = {};
		const DWORD length = ::GetModuleFileNameW(gInjectedModule, module, _countof(module));
		if( !length || length >= _countof(module) ) return false;
		wchar_t* slash = wcsrchr(module, L'\\');
		if( !slash || wcscpy_s(slash + 1, _countof(module) - static_cast<size_t>(slash + 1 - module), L"LeeyesW.exe") ) return false;
		if( !gListDeletePinnedModule
			&& !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&ListDeleteBatchSubclass), &gListDeletePinnedModule) )
			return false;
		WatchedSyncWorker* worker = new (std::nothrow) WatchedSyncWorker(module);
		if( !worker ) return false;
		if( !worker->Start() ) { delete worker; return false; }
		gListDeleteWorker = worker;
		#if UNICODEHACK_PATH_DEBUG
		WriteDiagnostic("list_delete worker_started=1");
		#endif
	}
	if( gListDeleteBatch.target != target )
	{
		try { if( !gListDeleteWorker->SetTarget(target) ) return false; }
		catch( ... ) { return false; }
		gListDeleteBatch.target = target;
		#if UNICODEHACK_PATH_DEBUG
		WriteDiagnostic("list_delete target_changed=1");
		#endif
	}
	if( sessionChanged )
	{
		gListDeleteBatch.committed.reset();
		gListDeleteBatch.candidate.reset();
		gListDeleteBatch.pendingApply = false;
		gListDeleteBatch.bootstrapPending = false;
		gListDeleteBatch.requestNeeded = false;
		gListDeleteBatch.requestOutstanding = false;
		gListDeleteBatch.pendingSince = 0;
		gListDeleteBatch.requestSince = 0;
		gListDeleteBatch.quietSince = ::GetTickCount64();
		gListDeleteBatch.fallbackDisabled = false;
	}
	gListDeleteBatch.owner = owner;
	gListDeleteBatch.window = window;
	if( !::SetWindowSubclass(window, ListDeleteBatchSubclass, kListDeleteTimer, 0) ) return false;
	gListDeleteBatch.subclassAttached = true;
	if( !::SetTimer(window, kListDeleteTimer, kListDeleteIntervalMs, nullptr) )
	{
		::RemoveWindowSubclass(window, ListDeleteBatchSubclass, kListDeleteTimer);
		gListDeleteBatch.subclassAttached = false;
		return false;
	}
	gListDeleteBatch.timerActive = true;
	return true;
}

void ScheduleListDeleteRefresh(bool apply, bool bootstrap)
{
	if( !gListDeleteWorker ) return;
	const ULONGLONG now = ::GetTickCount64();
	gListDeleteBatch.pendingApply = gListDeleteBatch.pendingApply || apply;
	gListDeleteBatch.bootstrapPending = gListDeleteBatch.bootstrapPending || bootstrap;
	gListDeleteBatch.requestNeeded = true;
	gListDeleteBatch.pendingSince = now;
	gListDeleteBatch.quietSince = now;
	if( !gListDeleteBatch.requestOutstanding ) gListDeleteBatch.requestSince = 0;
}

LRESULT CALLBACK ListDeleteBatchSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR)
{
	if( message == WM_TIMER && wParam == subclassId )
	{
		if( gDrainState.depth || !gListDeleteWorker || gListDeleteBatch.window != window
			|| !gListDeleteBatch.owner || !DispatchOwnerContainsListWindow(gListDeleteBatch.owner, window) ) return 0;
		const ULONGLONG now = ::GetTickCount64();
		char currentDirectory[kScopePathCapacity] = {};
		std::wstring currentTarget;
		if( !ReadOwnerDirectory(gListDeleteBatch.owner, currentDirectory, _countof(currentDirectory))
			|| !ExactAnsiDirectoryToWide(currentDirectory, currentTarget)
			|| currentTarget != gListDeleteBatch.target )
		{
			::KillTimer(window, subclassId);
			if( gListDeleteBatch.pendingApply )
			{
				ListSelectionSnapshot selection = CaptureListSelection();
				LeeyesInternalInvokeFullResync(gListDeleteBatch.owner);
				RestoreListSelection(selection);
			}
			if( gListDeleteBatch.candidate )
			{
				bool current = false;
				gListDeleteWorker->Release(*gListDeleteBatch.candidate, current);
			}
			try { gListDeleteWorker->SetTarget(L""); } catch( ... ) {}
			gListDeleteBatch.target.clear();
			gListDeleteBatch.committed.reset();
			gListDeleteBatch.candidate.reset();
			gListDeleteBatch.pendingApply = false;
			gListDeleteBatch.bootstrapPending = false;
			gListDeleteBatch.subclassAttached = false;
			gListDeleteBatch.timerActive = false;
			gListDeleteBatch.requestNeeded = false;
			gListDeleteBatch.requestOutstanding = false;
			gListDeleteBatch.timerActive = false;
			gListDeleteBatch.pendingSince = 0;
			return 0;
		}
		const unsigned dirty = gListDeleteWorker->DirtyFrames();
		if( dirty != gListDeleteBatch.observedDirty )
		{
			gListDeleteBatch.observedDirty = dirty;
			gListDeleteBatch.quietSince = now;
			if( gListDeleteBatch.pendingApply ) gListDeleteBatch.pendingSince = now;
			if( gListDeleteBatch.requestOutstanding )
			{
				gListDeleteBatch.requestOutstanding = false;
				gListDeleteBatch.requestNeeded = true;
				gListDeleteBatch.requestSince = 0;
			}
			if( gListDeleteBatch.candidate )
			{
				bool releasedCurrent = false;
				gListDeleteWorker->Release(*gListDeleteBatch.candidate, releasedCurrent);
				gListDeleteBatch.candidate.reset();
			}
		}
		if( gListDeleteBatch.target.empty() || !::IsWindow(window) ) return 0;
		if( gListDeleteWorker->Failed() )
			gListDeleteBatch.pendingSince = 1;
		if( gListDeleteBatch.requestNeeded && !gListDeleteBatch.requestOutstanding
			&& now - gListDeleteBatch.quietSince >= kListDeleteQuietMs )
		{
			bool requested = false;
			try { requested = gListDeleteWorker->RequestRefresh(); } catch( ... ) {}
			if( requested )
			{
				gListDeleteBatch.requestNeeded = false;
				gListDeleteBatch.requestOutstanding = true;
				gListDeleteBatch.requestSince = now;
			}
			else if( gListDeleteWorker->Failed() || gListDeleteWorker->Error() != ERROR_SUCCESS )
				gListDeleteBatch.pendingSince = 1;
		}
		if( !gListDeleteBatch.candidate )
		{
			WatchedSyncWorker::Candidate observed;
			if( gListDeleteWorker->Take(observed) )
			{
				try { gListDeleteBatch.candidate.reset(new WatchedSyncWorker::Candidate(std::move(observed))); }
				catch( ... )
				{
					bool current = false;
					gListDeleteWorker->Release(observed, current);
					gListDeleteBatch.pendingSince = 1;
				}
				gListDeleteBatch.requestOutstanding = false;
			}
		}
		if( gListDeleteBatch.candidate )
		{
			WatchedSyncWorker::Candidate& candidate = *gListDeleteBatch.candidate;
			if( !gListDeleteWorker->CanApply(candidate) )
			{
				bool current = false;
				gListDeleteWorker->Release(candidate, current);
				gListDeleteBatch.candidate.reset();
				gListDeleteBatch.requestNeeded = true;
				gListDeleteBatch.requestOutstanding = false;
				gListDeleteBatch.pendingSince = now;
				gListDeleteBatch.quietSince = now;
			}
			else if( now - gListDeleteBatch.quietSince >= kListDeleteQuietMs )
			{
				const bool bootstrap = gListDeleteBatch.bootstrapPending;
				const bool apply = gListDeleteBatch.pendingApply;
				bool incremental = false;
				if( apply && gListDeleteBatch.committed )
					incremental = OnlyRegularFilesRemoved(*gListDeleteBatch.committed, candidate.state);
				if( bootstrap || apply )
				{
					void* owner = gListDeleteBatch.owner;
					#if UNICODEHACK_PATH_DEBUG
					ULONGLONG resyncMs = 0, restoreMs = 0;
					#endif
					bool applied = owner && ::IsWindow(window)
						&& DispatchOwnerContainsListWindow(owner, window)
						&& gListDeleteWorker->CanApply(candidate);
					if( applied && apply )
					{
						ListSelectionSnapshot selection = CaptureListSelection();
						if( selection.window != window ) applied = false;
						else
						{
							#if UNICODEHACK_PATH_DEBUG
							const ULONGLONG resyncStarted = ::GetTickCount64();
							#endif
							applied = LeeyesInternalInvokeResync(owner, incremental ? TRUE : FALSE) != FALSE;
							#if UNICODEHACK_PATH_DEBUG
							resyncMs = ::GetTickCount64() - resyncStarted;
							#endif
							if( applied )
							{
								#if UNICODEHACK_PATH_DEBUG
								const ULONGLONG restoreStarted = ::GetTickCount64();
								#endif
								RestoreListSelection(selection);
								#if UNICODEHACK_PATH_DEBUG
								restoreMs = ::GetTickCount64() - restoreStarted;
								#endif
							}
						}
					}
					const bool current = applied && gListDeleteWorker->CanApply(candidate)
						&& DispatchOwnerContainsListWindow(owner, window);
					if( current && gListDeleteWorker->Finish(candidate) )
					{
						#if UNICODEHACK_PATH_DEBUG
						const LRESULT listCount = ::SendMessageA(window, LVM_GETITEMCOUNT, 0, 0);
						WriteDiagnostic("list_delete applied=1 incremental=%d entries=%lu list_count=%ld wait_ms=%llu resync_ms=%llu restore_ms=%llu",
							incremental ? 1 : 0, static_cast<unsigned long>(candidate.state.entries.size()),
							static_cast<long>(listCount),
							gListDeleteBatch.pendingSince ? now - gListDeleteBatch.pendingSince : 0, resyncMs, restoreMs);
						#endif
						try { gListDeleteBatch.committed.reset(new DirectorySnapshot::State(std::move(candidate.state))); }
						catch( ... ) { gListDeleteBatch.committed.reset(); }
						gListDeleteBatch.pendingApply = false;
						gListDeleteBatch.bootstrapPending = !gListDeleteBatch.committed;
						gListDeleteBatch.requestNeeded = false;
						gListDeleteBatch.requestOutstanding = false;
						gListDeleteBatch.pendingSince = 0;
						gListDeleteBatch.quietSince = now;
					}
					else
					{
						#if UNICODEHACK_PATH_DEBUG
						WriteDiagnostic("list_delete stale_after_apply=1");
						#endif
						bool releasedCurrent = false;
						gListDeleteWorker->Release(candidate, releasedCurrent);
						gListDeleteBatch.committed.reset();
						gListDeleteBatch.requestNeeded = true;
						gListDeleteBatch.requestOutstanding = false;
						gListDeleteBatch.pendingApply = true;
						gListDeleteBatch.bootstrapPending = false;
						gListDeleteBatch.pendingSince = now;
						gListDeleteBatch.quietSince = now;
					}
				}
				else
				{
					if( gListDeleteWorker->Finish(candidate) )
						gListDeleteBatch.committed.reset(new (std::nothrow) DirectorySnapshot::State(std::move(candidate.state)));
					else { gListDeleteBatch.committed.reset(); gListDeleteBatch.bootstrapPending = true; }
				}
				gListDeleteBatch.candidate.reset();
			}
		}
		if( gListDeleteBatch.pendingApply && gListDeleteBatch.pendingSince
			&& now - gListDeleteBatch.pendingSince >= kListDeleteFallbackMs
			&& (!gListDeleteWorker || gListDeleteWorker->Failed() || gListDeleteWorker->Error() != ERROR_SUCCESS
				|| (!gListDeleteBatch.requestOutstanding && !gListDeleteBatch.candidate)) )
		{
			#if UNICODEHACK_PATH_DEBUG
			WriteDiagnostic("list_delete legacy_fallback=1 error=%lu",
				static_cast<unsigned long>(gListDeleteWorker ? gListDeleteWorker->Error() : ERROR_INVALID_HANDLE));
			#endif
			const void* owner = gListDeleteBatch.owner;
			if( owner && ::IsWindow(window) && DispatchOwnerContainsListWindow(const_cast<void*>(owner), window) )
			{
				ListSelectionSnapshot selection = CaptureListSelection();
				LeeyesInternalInvokeFullResync(const_cast<void*>(owner));
				RestoreListSelection(selection);
			}
			gListDeleteBatch.committed.reset();
			gListDeleteBatch.pendingApply = false;
			gListDeleteBatch.bootstrapPending = true;
			gListDeleteBatch.requestNeeded = true;
			gListDeleteBatch.requestOutstanding = false;
			gListDeleteBatch.pendingSince = now;
			gListDeleteBatch.quietSince = now;
			gListDeleteBatch.fallbackDisabled = true;
			::KillTimer(window, subclassId);
			::RemoveWindowSubclass(window, ListDeleteBatchSubclass, subclassId);
			gListDeleteBatch.subclassAttached = false;
			gListDeleteBatch.timerActive = false;
		}
		return 0;
	}
	if( message == WM_NCDESTROY )
	{
		::KillTimer(window, subclassId);
		::RemoveWindowSubclass(window, ListDeleteBatchSubclass, subclassId);
		if( gListDeleteBatch.window == window )
		{
			gListDeleteBatch.window = nullptr;
			gListDeleteBatch.owner = nullptr;
			gListDeleteBatch.committed.reset();
			gListDeleteBatch.candidate.reset();
			gListDeleteBatch.pendingApply = false;
			gListDeleteBatch.bootstrapPending = false;
			gListDeleteBatch.subclassAttached = false;
			gListDeleteBatch.timerActive = false;
			if( gListDeleteWorker ) { try { gListDeleteWorker->SetTarget(L""); } catch( ... ) {} }
			gListDeleteBatch.target.clear();
		}
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}

void ObserveListDeleteBatch(void* owner, HWND window, const char* directory)
{
	gDrainState.listStateWatchAttempted = true;
	if( !EnsureListDeleteWorker(window, owner, directory) ) return;
	gListDeleteBatch.observedDirty = gListDeleteWorker->DirtyFrames();
	if( !gListDeleteBatch.committed ) ScheduleListDeleteRefresh(false, true);
}

bool DeferListDeleteBatchResync(void* owner, HWND window, const char* directory)
{
	if( !gListDeleteBatchEnabled || !gListDeleteWorker || gListDeleteBatch.fallbackDisabled
		|| gListDeleteWorker->Failed() || gListDeleteWorker->Error() != ERROR_SUCCESS
		|| !gListDeleteBatch.subclassAttached || !gListDeleteBatch.timerActive
		|| !window || !::IsWindow(window) || !DispatchOwnerContainsListWindow(owner, window)
		|| !directory ) return false;
	std::wstring target;
	if( !ExactAnsiDirectoryToWide(directory, target) || target != gListDeleteBatch.target
		|| gListDeleteBatch.owner != owner || gListDeleteBatch.window != window ) return false;
	gListDeleteBatch.pendingApply = true;
	gListDeleteBatch.bootstrapPending = false;
	gListDeleteBatch.pendingSince = ::GetTickCount64();
	gListDeleteBatch.quietSince = gListDeleteBatch.pendingSince;
	gListDeleteBatch.requestNeeded = true;
	#if UNICODEHACK_PATH_DEBUG
	WriteDiagnostic("list_delete deferred=1 baseline_known=%d baseline_entries=%lu",
		gListDeleteBatch.committed ? 1 : 0,
		static_cast<unsigned long>(gListDeleteBatch.committed ? gListDeleteBatch.committed->entries.size() : 0));
	#endif
	return true;
}

void CompleteListDeleteBatchLegacyResync(void* owner, HWND window, const char* directory, bool legacyResynced)
{
	if( !gListDeleteBatchEnabled || !gListDeleteWorker || gListDeleteBatch.fallbackDisabled
		|| !owner || !window || !directory ) return;
	std::wstring target;
	if( !ExactAnsiDirectoryToWide(directory, target) || target != gListDeleteBatch.target
		|| gListDeleteBatch.owner != owner || gListDeleteBatch.window != window ) return;
	// Keep the last confirmed state across ordinary callbacks. Their changes stay
	// in the next state diff; an add or metadata change then selects the full path.
	// Rebase only after a legacy full refresh, or after the first observed update
	// establishes the initial baseline.
	if( legacyResynced )
	{
		gListDeleteBatch.committed.reset();
		gListDeleteBatch.pendingApply = false;
		gListDeleteBatch.bootstrapPending = true;
	}
	else if( !gListDeleteBatch.committed ) gListDeleteBatch.bootstrapPending = true;
	else if( !gListDeleteBatch.pendingApply ) return;
	gListDeleteBatch.requestNeeded = true;
	gListDeleteBatch.requestOutstanding = false;
	gListDeleteBatch.pendingSince = ::GetTickCount64();
	gListDeleteBatch.quietSince = gListDeleteBatch.pendingSince;
}

#if UNICODEHACK_PATH_DEBUG
typedef int (WINAPI *DiagnosticNaturalCompare)(PCWSTR, PCWSTR);
struct DiagnosticNaturalLess
{
	DiagnosticNaturalCompare compare;
	bool operator()(const std::wstring& a, const std::wstring& b) const { return compare(a.c_str(), b.c_str()) < 0; }
};
struct DiagnosticCaseLess
{
	bool operator()(const std::wstring& a, const std::wstring& b) const { return ::lstrcmpiW(a.c_str(), b.c_str()) < 0; }
};
bool DiagnosticSortKeysUnique(const DirectorySnapshot::State& state, int sortIndex)
{
	try
	{
		std::set<ULONGLONG> numeric;
		HMODULE shlwapi = ::GetModuleHandleW(L"shlwapi.dll");
		DiagnosticNaturalCompare compare = shlwapi ? reinterpret_cast<DiagnosticNaturalCompare>(::GetProcAddress(shlwapi, "StrCmpLogicalW")) : nullptr;
		if( sortIndex == 0 && !compare ) return false;
		std::set<std::wstring, DiagnosticNaturalLess> natural(DiagnosticNaturalLess{ compare });
		std::set<std::wstring, DiagnosticCaseLess> insensitive;
		for( const auto& entry : state.entries )
		{
			if( entry.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
			if( sortIndex == 0 )
			{
				// Leeyes uses an ANSI comparator; do not infer its equivalence
				// classes for names outside the portable ASCII fixture domain.
				for( wchar_t c : entry.name ) if( c > 0x7f ) return false;
				if( !natural.insert(entry.name).second || !insensitive.insert(entry.name).second ) return false;
			}
			else
			{
				const ULONGLONG key = sortIndex == 1 ? entry.size : entry.written;
				if( !numeric.insert(key).second ) return false;
			}
		}
		if( sortIndex == 3 && numeric.size() > 1 )
		{
			// Reject timestamps closer than two seconds, including values on
			// opposite sides of a coarse filesystem timestamp boundary.
			auto previous = numeric.begin();
			for( auto current = std::next(previous); current != numeric.end(); ++current, ++previous )
				if( *current - *previous < 20000000ULL ) return false;
		}
		return true;
	}
	catch( ... ) { return false; }
}
bool InvokeDiagnosticListSort(void* owner, HWND window, const DirectorySnapshot::State* state)
{
	if( !state || ::GetEnvironmentVariableA("LEYEES_LIST_TEST_DISABLE_SORT", nullptr, 0) ) return false;
	if( !owner || !window || !DispatchOwnerContainsListWindow(owner, window) ) return false;
	BYTE* image = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	if( reinterpret_cast<ULONG_PTR>(image) != 0x00400000 ) return false;
	const BYTE signature[] = { 0x55,0x8b,0xec,0x83,0xc4,0xf8,0x89,0x45,0xf8,
		0x8b,0x45,0xf8,0x8b,0x80,0x84,0x03,0x00,0x00,0x85,0xc0,0x7c,0x47 };
	BYTE* sort = image + 0x000d6f04;
	if( !IsReadableRange(sort, sizeof(signature)) || memcmp(sort, signature, sizeof(signature)) != 0 ) return false;
	__try
	{
		BYTE* object = static_cast<BYTE*>(owner);
		if( !IsReadableRange(object + 0x384, sizeof(DWORD)) || !IsReadableRange(object + 0x404, sizeof(DWORD)) ) return false;
		const int sortIndex = *reinterpret_cast<const int*>(object + 0x384);
		// Only these modes have a distinct-key fixture checked against the
		// filesystem. In another mode equal keys produced different ordering
		// from the original full rebuild, so leave that path to Leeyes.
		if( sortIndex != 0 && sortIndex != 1 && sortIndex != 3 ) return false;
		if( !DiagnosticSortKeysUnique(*state, sortIndex) ) return false;
		const BYTE* items = *reinterpret_cast<BYTE* const*>(object + 0x404);
		if( !IsReadableRange(items, 12) ) return false;
		const int internalCount = *reinterpret_cast<const int*>(items + 8);
		const int listCount = static_cast<int>(::SendMessageW(window, LVM_GETITEMCOUNT, 0, 0));
		if( internalCount < 0 || internalCount != listCount ) return false;
		__asm {
			mov eax, owner
			call sort
		}
		if( listCount > 0 ) ::SendMessageW(window, LVM_REDRAWITEMS, 0, listCount - 1);
		return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}
bool ApplyShadowPending(bool incremental, bool verifyFallback, bool sortAfter, const DirectorySnapshot::State* sortState)
{
	const ShadowPendingUpdate pending = gShadowPending;
	gShadowPending = ShadowPendingUpdate();
	gShadowPendingBaseline.reset();
	InvalidateShadowApplied();
	if( !pending.owner || !pending.window || !::IsWindow(pending.window)
		|| !DispatchOwnerContainsListWindow(pending.owner, pending.window) )
	{
		WriteDiagnostic("state_shadow apply_owner_invalid=1");
		return false;
	}
	const ListSelectionSnapshot selection = CaptureListSelection();
	const ULONGLONG started = ::GetTickCount64();
	if( !LeeyesDiagnosticInvokeResync(pending.owner, incremental ? TRUE : FALSE) )
	{
		if( !gShadowPending.owner && !gShadowNavigation.load() && ::IsWindow(pending.window)
			&& DispatchOwnerContainsListWindow(pending.owner, pending.window) )
		{
			gShadowPending = pending; gShadowPending.correction = true;
			gShadowPending.unchangedSkipEligible = false; gShadowPending.retryUnavailable = true;
			gShadowPending.deadline = ::GetTickCount64() + 1000;
			WriteDiagnostic("state_shadow resync_retry_pending=1");
		}
		gListWatchCommitted = ShadowPendingUpdate(); gListWatchCommittedState.reset();
		WriteDiagnostic("state_shadow apply_resync_unavailable=1 tick=%llu", ::GetTickCount64());
		return false;
	}
	if( incremental && sortAfter )
	{
		const bool sorted = InvokeDiagnosticListSort(pending.owner, pending.window, sortState);
		WriteDiagnostic("state_shadow list_watched_add_sort=%d", sorted ? 1 : 0);
		if( !sorted )
		{
			if( !LeeyesDiagnosticInvokeResync(pending.owner, FALSE) )
			{
				if( !gShadowPending.owner && !gShadowNavigation.load() && ::IsWindow(pending.window)
					&& DispatchOwnerContainsListWindow(pending.owner, pending.window) )
				{
					gShadowPending = pending; gShadowPending.correction = true;
					gShadowPending.unchangedSkipEligible = false; gShadowPending.retryUnavailable = true;
					gShadowPending.deadline = ::GetTickCount64() + 1000;
					WriteDiagnostic("state_shadow resync_retry_pending=1");
				}
				gListWatchCommitted = ShadowPendingUpdate(); gListWatchCommittedState.reset();
				WriteDiagnostic("state_shadow apply_resync_unavailable=1 tick=%llu", ::GetTickCount64());
				return false;
			}
			incremental = false;
		}
	}
	RestoreListSelection(selection, ::GetEnvironmentVariableA("LEYEES_STATE_PRESERVE_OFFSCREEN_FOCUS", nullptr, 0) != 0,
		::GetEnvironmentVariableA("LEYEES_STATE_PRESERVE_TOP_ITEM", nullptr, 0) != 0);
	WriteDiagnostic("state_shadow apply_completed=1 incremental=%d elapsed_ms=%llu",
		incremental ? 1 : 0, ::GetTickCount64() - started);
	// A fallback enumeration can overlap filesystem changes. Request one fresh
	// candidate afterwards; this is only an experiment, not a correctness proof.
	StateSyncWorker* worker = gShadowWorker.load(std::memory_order_acquire);
	if( verifyFallback && worker && !worker->Failed() && !gShadowNavigation.load() )
	{
		gShadowPending = pending;
		gShadowPending.correction = true;
		gShadowPending.deadline = ::GetTickCount64() + 5000;
		try { worker->Changed(); }
		catch( ... ) { gShadowPending = ShadowPendingUpdate(); }
	}
	return true;
}
#endif

bool RememberDirtyList(void* listView)
{
	if( !listView ) return false;
	for( DWORD index = 0; index < gDrainState.dirtyListCount; ++index )
		if( gDrainState.dirtyLists[index] == listView ) return true;
	if( gDrainState.dirtyListCount < kMaximumDirtyLists )
	{
		gDrainState.dirtyLists[gDrainState.dirtyListCount++] = listView;
		return true;
	}
	// Never suppress a callback whose owner cannot be resynchronized later.
	return false;
}

}

#if UNICODEHACK_PATH_DEBUG
bool ReadDiagnosticNotifierMethod(void* notifier, DWORD& callback, DWORD& owner)
{
	callback = owner = 0;
	__try
	{
		callback = *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(notifier) + 0x18);
		owner = *reinterpret_cast<DWORD*>(reinterpret_cast<BYTE*>(notifier) + 0x1c);
		return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { callback = owner = 0; return false; }
}

// This exact Leeyes build routes action 6 through the default arms of both
// tree callbacks. Keep invoking the original callback in journal order; the
// signature check only limits this opt-in experiment to the inspected build.
bool TreeAction6ReplaySupported()
{
	static const BYTE first[] = { 0x8b,0xc3,0x2c,0x01,0x72,0x0f,0x74,0x70,0xfe,0xc8,0x0f,0x84,0x58,0x02,0x00,0x00,0xe9,0x84,0x02,0x00,0x00 };
	static const BYTE second[] = { 0x80,0xeb,0x03,0x74,0x15,0xfe,0xcb,0x0f,0x84,0x53,0x01,0x00,0x00,0xfe,0xcb,0x0f,0x84,0xf9,0x02,0x00,0x00,0xe9,0x5e,0x04,0x00,0x00 };
	__try
	{
		return memcmp(reinterpret_cast<const void*>(0x004f67c6), first, sizeof(first)) == 0
			&& memcmp(reinterpret_cast<const void*>(0x004cf3e7), second, sizeof(second)) == 0;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}

// Discover local parent paths, ASCII by default or lossless CP932 with the
// explicit probe flag. The callback must round-trip byte-for-byte. Everything
// else stays in the original ordered callback stream.
bool DiscoverTreeParent(const char* path, std::wstring& parent)
{
	if( !path || !*path ) return false;
	wchar_t wide[kScopePathCapacity] = {};
	if( !::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, path, -1, wide, _countof(wide)) ) return false;
	char roundtrip[kScopePathCapacity] = {}; BOOL substituted = FALSE;
	if( !::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide, -1, roundtrip, _countof(roundtrip), nullptr, &substituted)
		|| substituted || strcmp(path, roundtrip) ) return false;
	wchar_t* leaf = wcsrchr(wide, L'\\');
	if( !leaf || !leaf[1] || leaf - wide < 3 || wide[1] != L':' || wide[2] != L'\\'
		|| !((wide[0] >= L'A' && wide[0] <= L'Z') || (wide[0] >= L'a' && wide[0] <= L'z'))
		|| !wcscmp(leaf + 1, L".") || !wcscmp(leaf + 1, L"..")
		|| !wcsncmp(leaf + 1, L"~u", 2) || wcschr(leaf + 1, L'/') ) return false;
	*leaf = L'\0';
	const bool cp932Parent = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_PARENT_CP932", nullptr, 0) != 0;
	wchar_t* component = wide + 3;
	for( wchar_t* p = component; ; ++p )
	{
		if( (*p > 0x7f && !cp932Parent) || *p == L'/' || *p == L':' ) return false;
		if( !*p || *p == L'\\' )
		{
			const size_t length = p - component;
			if( !length || (length == 1 && component[0] == L'.')
				|| (length == 2 && component[0] == L'.' && component[1] == L'.')
				|| (length >= 2 && component[0] == L'~' && component[1] == L'u')
				|| component[length - 1] == L'.' || component[length - 1] == L' ' ) return false;
			if( !*p ) break;
			component = p + 1;
		}
	}
	try { parent.assign(wide); return true; } catch( ... ) { return false; }
}
bool ObserveDiscoveredTreeParent(ReplayBatchDiagnostic* batch, DWORD action, const char* first, DWORD secondLength)
{
	if( !batch || !batch->discoverParents || secondLength || ((action & 0xff) != 3 && (action & 0xff) != 5) ) return false;
	std::wstring parent;
	if( !DiscoverTreeParent(first, parent) ) return false;
	for( const auto& existing : batch->discoveredParents )
	{
		if( existing == parent ) return true;
		if( ::CompareStringOrdinal(existing.c_str(), -1, parent.c_str(), -1, TRUE) == CSTR_EQUAL ) return false;
	}
	// Exercise the discovery timeout while retaining every callback in the
	// journal. This does not model successful parent identification.
	if( batch->discoveredParents.size() == 1
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_HOLD_SECOND_PARENT", nullptr, 0) )
		return true;
	if( batch->discoveredParents.size() == 1
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_DEFER_SECOND_PARENT", nullptr, 0) )
	{
		try {
			if( batch->testDeferredParent.empty() ) batch->testDeferredParent = parent;
			return batch->testDeferredParent == parent;
		} catch( ... ) { return false; }
	}
	if( batch->parentsAttached || batch->discoveredParents.size() >= 2 ) return false;
	try { batch->discoveredParents.push_back(parent); return true; } catch( ... ) { return false; }
}

bool IsFixtureTreeStateEvent(DWORD action, const char* path)
{
	// This opt-in probe uses two explicitly configured parents. Exact matching
	// deliberately rejects case variations and aliases; unknown events flush the
	// journal below. It is not a general shell path identity test.
	if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0) )
	{
		if( !path || ((action & 0xff) != 3 && (action & 0xff) != 5) ) return false;
		wchar_t wide[kScopePathCapacity] = {};
		if( !::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, path, -1, wide, _countof(wide)) ) return false;
		const wchar_t* variables[] = { L"LEYEES_TREE_STATE_PARENT", L"LEYEES_TREE_STATE_OTHER_PARENT" };
		for( const wchar_t* variable : variables )
		{
			wchar_t parent[kScopePathCapacity] = {};
			const DWORD length = ::GetEnvironmentVariableW(variable, parent, _countof(parent));
			if( !length || length >= _countof(parent) || parent[length - 1] == L'\\' ) continue;
			bool ascii = true;
			for( DWORD i = 0; i < length; ++i ) if( parent[i] > 0x7f ) { ascii = false; break; }
			if( !ascii || wcsncmp(wide, parent, length) || wide[length] != L'\\' ) continue;
			const wchar_t* leaf = wide + length + 1;
			if( *leaf && wcscmp(leaf, L".") && wcscmp(leaf, L"..") && !wcschr(leaf, L'\\') && !wcschr(leaf, L'/') ) return true;
		}
		return false;
	}
	const bool destination = ::GetEnvironmentVariableA("LEYEES_TREE_FIXTURE_DESTINATION", nullptr, 0) != 0;
	const bool both = ::GetEnvironmentVariableA("LEYEES_TREE_FIXTURE_BOTH", nullptr, 0) != 0;
	auto separator = [](const char* text) -> const char* {
		for( const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p )
		{
			if( ::IsDBCSLeadByteEx(932, *p) ) { if( !p[1] ) return reinterpret_cast<const char*>(p); ++p; continue; }
			if( *p == '\\' ) return reinterpret_cast<const char*>(p);
		}
		return nullptr;
	};
	if( both )
	{
		if( (action & 0xff) != 3 && (action & 0xff) != 5 ) return false;
		const bool parentTrail = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_FIXTURE_PARENTTRAIL", nullptr, 0) != 0;
		const char* prefix = parentTrail ? "\\folder_source\x95\x5c\\" : "\\folder_source\\";
		const char* parent = path ? strstr(path, prefix) : nullptr;
		if( !parent ) { prefix = parentTrail ? "\\folder_destination\x95\x5c\\" : "\\folder_destination\\"; parent = path ? strstr(path, prefix) : nullptr; }
		if( !parent ) return false;
		const char* leaf = parent + strlen(prefix);
		if( strncmp(leaf, "__folder_", 9) != 0 && strncmp(leaf, "renamed___folder_", 17) != 0
			&& (!parentTrail || strncmp(leaf, "__generic_", 10) != 0) ) return false;
		const char* nested = separator(leaf);
		if( !nested ) return true;
		// Limited real-file test: the adapter has not been proved for arbitrary
		// descendants, aliases, or non-directory shell notifications.
		return ::GetEnvironmentVariableA("LEYEES_TREE_STATE_NESTED_FIXTURE", nullptr, 0)
			&& strcmp(nested, "\\__late_probe") == 0;
	}
	const char* prefix = destination ? "\\folder_destination\\" : "\\folder_source\\";
	const char* parent = path ? strstr(path, prefix) : nullptr;
	if( !parent || (action & 0xff) != (destination ? 3u : 5u) ) return false;
	const char* leaf = parent + strlen(prefix);
	return (strncmp(leaf, "__folder_", 9) == 0
		|| (destination && strncmp(leaf, "renamed___folder_", 17) == 0))
		&& !separator(leaf);
}
bool IsFixtureTreeRelatedPath(const char* path)
{
	if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0) ) return path != nullptr;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_FIXTURE_PARENTTRAIL", nullptr, 0) )
		return path && (strstr(path, "\\folder_source\x95\x5c\\") || strstr(path, "\\folder_destination\x95\x5c\\"));
	return path && (strstr(path, "\\folder_source\\") || strstr(path, "\\folder_destination\\"));
}

extern "C" BOOL __cdecl LeeyesDiagnosticCaptureTree(void* method, void* owner,
	DWORD action, DWORD path0, DWORD path1)
{
	if( gDrainState.depth && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_TRACE_CLASSES", nullptr, 0) )
	{
		static unsigned transientSamples = 0, otherSamples = 0;
		if( transientSamples < 64 || otherSamples < 16 )
		{
			char sample[kScopePathCapacity] = {}; DWORD length = 0;
			if( CopyDelphiAnsiString(path0, sample, _countof(sample), length) )
			{
				const bool transient = strstr(sample, "__leeyes_transient_") != nullptr;
				if( (transient && transientSamples < 64) || (!transient && gDrainState.suppress && otherSamples < 16) )
				{
					if( transient ) ++transientSamples; else ++otherSamples;
					WriteDiagnostic("state_shadow tree_test_class action=%lu transient=%d length=%lu",
						static_cast<unsigned long>(action & 0xff), transient ? 1 : 0, static_cast<unsigned long>(length));
				}
			}
		}
	}
	if( !gReplayBatch && !gReplayBatchDone && gDrainState.depth && owner
		&& reinterpret_cast<ULONG_PTR>(method) == 0x004f6784
		&& ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_REPLAY", nullptr, 0)
		&& ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_FIXTURE_START", nullptr, 0) )
	{
		char first[kScopePathCapacity] = {}, second[kScopePathCapacity] = {};
		DWORD firstLength = 0, secondLength = 0;
		const bool discoveryRequested = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DISCOVER_PARENTS", nullptr, 0) != 0;
		const bool discover = discoveryRequested
			&& ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_FIXTURE_BOTH", nullptr, 0)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_AUTO_PARENT", nullptr, 0)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_AUTO_START", nullptr, 0)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_VERIFY_PARENT_PATH", nullptr, 0);
		std::wstring discovered;
		if( (!discoveryRequested || discover) && (!discover || gDrainState.suppress)
			&& CopyDelphiAnsiString(path0, first, _countof(first), firstLength)
			&& (!::GetEnvironmentVariableA("LEYEES_TREE_TEST_TRANSIENT_START", nullptr, 0)
				|| strstr(first, "\\__leeyes_transient_"))
			&& (!(discover || ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0))
				|| (CopyDelphiAnsiString(path1, second, _countof(second), secondLength) && !secondLength))
			&& (discover ? (((action & 0xff) == 3 || (action & 0xff) == 5) && DiscoverTreeParent(first, discovered))
				: IsFixtureTreeStateEvent(action, first)) )
		{
			unsigned limit = 4096;
			char limitText[16] = {};
			const DWORD limitLength = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_JOURNAL_LIMIT", limitText, _countof(limitText));
			if( discover && limitLength && limitLength < _countof(limitText) )
			{
				const unsigned requested = strtoul(limitText, nullptr, 10);
				if( requested >= 8 && requested <= limit ) limit = requested;
			}
			const unsigned actionLimit = discover && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ACTION6_REPLAY", nullptr, 0)
				&& TreeAction6ReplaySupported() ? 6 : 5;
			gReplayBatch = new (std::nothrow) ReplayBatchDiagnostic(method, owner, limit, actionLimit);
			if( gReplayBatch ) gReplayBatch->discoverParents = discover;
			if( gReplayBatch && ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0)
				&& !discover && !LeeyesDiagnosticTreeBatchAttach(reinterpret_cast<DWORD>(owner)) )
			{ delete gReplayBatch; gReplayBatch = nullptr; gReplayBatchDone = true; }
			WriteDiagnostic("state_shadow tree_batch_fixture_started=%d queued=%lu",
				gReplayBatch ? 1 : 0, static_cast<unsigned long>(gDrainState.queuedChanges));
		}
	}
	if( gReplayBatch )
	{
		ReplayBatchDiagnostic* batch = gReplayBatch;
		if( batch->replaying ) { ++batch->reentered; return FALSE; }
		CompactTreeBatch(batch, false);
		char first[kScopePathCapacity] = {}, second[kScopePathCapacity] = {};
		DWORD firstLength = 0, secondLength = 0;
		bool stored = false;
		bool startRelatedFallback = false;
		const bool matching = batch->method == method && batch->owner == owner;
		const bool copied = matching && CopyDelphiAnsiString(path0, first, _countof(first), firstLength)
			&& CopyDelphiAnsiString(path1, second, _countof(second), secondLength);
		// If a streaming fallback reaches its bound, replay a small oldest
		// slice before accepting the current callback. Never pass it ahead of
		// retained callbacks. This diagnostic still cannot bound an entire
		// drain under sustained overload or a slow original callback.
		if( copied && batch->streamingStarted && batch->fallbackPending && firstLength
			&& (action & 0xff) <= batch->actionLimit && !batch->journal.HasRoomFor(firstLength, secondLength)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_PRESSURE_REPLAY", nullptr, 0) )
		{
			const unsigned before = batch->replayed;
			ReplayBatchSlice(batch, 8, 8);
			++batch->pressureSlices;
			batch->pressureReplayed += batch->replayed - before;
			if( batch->abandoned ) { LeeyesDiagnosticTreeBatchAbandon(); return FALSE; }
			CompactTreeBatch(batch, true);
		}
		if( ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0) )
		{
			const bool passthrough = copied && batch->discoverParents && !secondLength
				&& ((action & 0xff) == 0 || (action & 0xff) == 2)
				&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_PREFIX", nullptr, 0)
				&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_PASSTHROUGH", nullptr, 0);
			const bool accepted = batch->fallbackPending || passthrough || (copied && (batch->discoverParents
				? ObserveDiscoveredTreeParent(batch, action, first, secondLength)
				: IsFixtureTreeStateEvent(action, first)));
			if( !batch->fallbackPending && (!accepted
				|| (secondLength && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0))) )
			{
				if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0) )
				{
					char executable[kScopePathCapacity] = {};
					const DWORD executableLength = ::GetModuleFileNameA(nullptr, executable, _countof(executable));
					char* separator = executableLength && executableLength < _countof(executable) ? strrchr(executable, '\\') : nullptr;
					const size_t directoryLength = separator ? static_cast<size_t>(separator - executable + 1) : 0;
					const bool runtimeDirectory = directoryLength && !_strnicmp(first, executable, directoryLength);
					WriteDiagnostic("state_shadow tree_explicit_unknown action=%lu matching=%d copied=%d path0_len=%lu path1_len=%lu diagnostic_log=%d runtime_dir=%d",
						static_cast<unsigned long>(action & 0xff), matching ? 1 : 0, copied ? 1 : 0,
						static_cast<unsigned long>(firstLength), static_cast<unsigned long>(secondLength),
						strstr(first, "\\inject_dll_file_move_batch.txt") ? 1 : 0,
						runtimeDirectory ? 1 : 0);
				}
				// A related event that is not in this fixture adapter must not
				// overtake callbacks already held in the journal. An unreadable
				// path is also unknown, so flush before letting it through.
				const bool related = copied && (batch->discoverParents || IsFixtureTreeRelatedPath(first) || IsFixtureTreeRelatedPath(second));
				startRelatedFallback = related && (action & 0xff) <= batch->actionLimit
					&& (::GetEnvironmentVariableA("LEYEES_TREE_STATE_SLICED_RELATED", nullptr, 0)
						|| ::GetEnvironmentVariableA("LEYEES_TREE_STATE_EXPLICIT_ROOTS", nullptr, 0) || batch->discoverParents);
				if( !startRelatedFallback )
				{
					if( !copied || related ) FlushReplayBatch("related_or_unknown_event");
					return FALSE;
				}
			}
		}
		if( copied )
		{
			char failText[16] = {};
			const DWORD failLength = ::GetEnvironmentVariableA("LEYEES_TREE_ALLOC_FAIL_AT", failText, _countof(failText));
			const bool forceFailure = failLength && failLength < _countof(failText)
				&& strtoul(failText, nullptr, 10) == batch->captured + 1;
			if( forceFailure ) WriteDiagnostic("state_shadow tree_batch_forced_alloc_failure=1");
			ReplayBatchDiagnostic::OwnedPaths paths = {};
			bool inVector = false;
			try
			{
				const std::string firstBytes(first, firstLength), secondBytes(second, secondLength);
				if( !forceFailure ) paths.first = LeeyesDiagnosticMakeAnsi(first, firstLength);
				if( secondLength && !forceFailure ) paths.second = LeeyesDiagnosticMakeAnsi(second, secondLength);
				if( paths.first && (!secondLength || paths.second) )
				{
					batch->owned.push_back(paths);
					inVector = true;
					stored = batch->journal.TryAppend(action & 0xff, firstBytes, secondBytes);
				}
				if( stored )
				{
					batch->capturedHash = ReplayHash(batch->capturedHash, action & 0xff, firstBytes, secondBytes);
					++batch->captured;
					if( batch->streamingStarted ) ++batch->appendedWhileStreaming;
					++batch->actions[action & 0xff];
				}
			}
			catch( ... ) { stored = false; }
			if( !stored )
			{
				if( inVector ) batch->owned.pop_back();
				LeeyesDiagnosticClearAnsi(&paths.first);
				LeeyesDiagnosticClearAnsi(&paths.second);
			}
		}
		if( stored )
		{
			// Fault-injection fixture: add an action 6 to an already-fallbacking
			// stream so the original callback and journal ordering are exercised.
			static bool injectedAction6 = false;
			if( !injectedAction6 && batch->fallbackPending && batch->actionLimit == 6
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_INJECT_ACTION6", nullptr, 0) )
			{
				ReplayBatchDiagnostic::OwnedPaths test = {};
				bool pushed = false, appended = false;
				try
				{
					const std::string bytes(first, firstLength);
					test.first = LeeyesDiagnosticMakeAnsi(first, firstLength);
					if( test.first ) { batch->owned.push_back(test); pushed = true; appended = batch->journal.TryAppend(6, bytes, ""); }
					if( appended )
					{
						batch->capturedHash = ReplayHash(batch->capturedHash, 6, bytes, "");
						++batch->captured; ++batch->actions[6];
						if( batch->streamingStarted ) ++batch->appendedWhileStreaming;
						injectedAction6 = true;
					}
				}
				catch( ... ) {}
				if( !appended ) { if( pushed ) batch->owned.pop_back(); LeeyesDiagnosticClearAnsi(&test.first); }
			}
			if( startRelatedFallback )
			{
				batch->fallbackPending = true;
				WriteDiagnostic("state_shadow tree_batch_related_captured=%u", batch->captured);
				if( !batch->discoverParents || batch->parentsAttached )
					if( !LeeyesDiagnosticTreeForceFallback() ) FlushReplayBatch("related_fallback_failed");
			}
			return TRUE;
		}
		WriteDiagnostic("state_shadow tree_batch_rejected action=%lu matching=%d copied=%d first_len=%lu second_len=%lu captured=%u replayed=%u",
			static_cast<unsigned long>(action & 0xff), matching ? 1 : 0, copied ? 1 : 0,
			static_cast<unsigned long>(firstLength), static_cast<unsigned long>(secondLength),
			batch->captured, batch->replayed);
		// Flush all older callbacks before allowing this one through. The
		// experiment is synchronous and cannot guarantee cross-notifier order.
		FlushReplayBatch("limit_or_unsupported");
		return FALSE;
	}
	if( ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_REPLAY", nullptr, 0) ) return FALSE;
	static bool attempted = false;
	const bool folderExperiment = ::GetEnvironmentVariableA("LEYEES_TREE_REPLAY_FOLDER_SINGLE", nullptr, 0) != 0;
	static unsigned samples = 0;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_REPLAY_TRACE", nullptr, 0)
		&& reinterpret_cast<ULONG_PTR>(method) == 0x004f6784 && samples < 12 )
	{
		++samples;
		WriteDiagnostic("state_shadow tree_replay_sample action=%lu depth=%lu path0=%d path1=%d",
			static_cast<unsigned long>(action & 0xff), static_cast<unsigned long>(gDrainState.depth),
			path0 ? 1 : 0, path1 ? 1 : 0);
	}
	if( attempted || !gDrainState.depth
		|| !owner || reinterpret_cast<ULONG_PTR>(method) != 0x004f6784
		|| (action & 0xff) != (folderExperiment ? 5 : 2) || gReplaySingle.owner || gReplayTimer.owner
		|| ::GetEnvironmentVariableA("LEYEES_TREE_REPLAY_PASS_ONLY", nullptr, 0)
		|| !::GetEnvironmentVariableA("LEYEES_TREE_REPLAY_SINGLE", nullptr, 0) ) return FALSE;
	if( !gDrainState.currentListWindow && !FindDisplayedListWindow(gDrainState.currentListWindow) ) return FALSE;
	char first[kScopePathCapacity] = {}, second[kScopePathCapacity] = {};
	DWORD firstLength = 0, secondLength = 0;
	if( !CopyDelphiAnsiString(path0, first, _countof(first), firstLength)
		|| !CopyDelphiAnsiString(path1, second, _countof(second), secondLength)
		|| !firstLength || memchr(first, '\0', firstLength) || memchr(second, '\0', secondLength) ) return FALSE;
	if( folderExperiment && !strstr(first, "\\folder_source\\__folder_") ) return FALSE;
	attempted = true;
	gReplaySingle.owner = owner;
	gReplaySingle.method = method;
	gReplaySingle.window = gDrainState.currentListWindow;
	gReplaySingle.action = action;
	gReplaySingle.firstLength = firstLength;
	gReplaySingle.secondLength = secondLength;
	memcpy(gReplaySingle.first, first, firstLength + 1);
	memcpy(gReplaySingle.second, second, secondLength + 1);
	WriteDiagnostic("state_shadow tree_replay_captured=1 fixture_folder=%d",
		strstr(first, "\\__folder_") ? 1 : 0);
	return TRUE;
}

extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchCanStart()
{
	return gReplayBatch && !gReplayBatch->replaying && gDrainState.depth == 0 ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeReplayActive()
{
	return gReplayBatch && gReplayBatch->replaying ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchAmbiguous()
{
	// Even a single observed event can hide a replacement if its counterpart
	// was coalesced or missed. Equal names alone cannot establish identity.
	return !gReplayBatch || gReplayBatch->journal.Size() != 0 ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchPaths(std::vector<std::string>* paths)
{
	return paths && gReplayBatch && !gReplayBatch->replaying
		&& gReplayBatch->journal.DirectoryPaths(*paths) ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeTransientPaths(std::vector<std::string>* paths)
{
	return paths && gReplayBatch && !gReplayBatch->replaying && !gReplayBatch->fallbackPending
		&& gReplayBatch->journal.AdjacentTransientPairs(*paths) ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchPathsCovered(const std::vector<std::string>* paths)
{
	return paths && gReplayBatch && !gReplayBatch->replaying
		&& gReplayBatch->journal.AllDirectoryPathsCovered(*paths,
			::GetEnvironmentVariableA("LEYEES_TREE_STATE_DIFF_CP932", nullptr, 0) != 0) ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeOrderChanges(
	const std::vector<std::pair<unsigned, std::string>>* changes, std::vector<size_t>* order,
	std::vector<std::pair<unsigned, std::string>>* originals)
{
	ReplayBatchDiagnostic* batch = gReplayBatch;
	if( !changes || !order || !batch || batch->replaying || batch->fallbackPending ) return FALSE;
	return batch->journal.OrderChanges(*changes, *order, batch->orderedPrefix, originals) ? TRUE : FALSE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBeginOrdered(size_t expected)
{
	ReplayBatchDiagnostic* batch = gReplayBatch;
	if( !batch || batch->replaying || batch->fallbackPending || batch->journal.PendingSize() != expected ) return FALSE;
	if( batch->orderedPrefix )
		WriteDiagnostic("state_shadow tree_batch_ordered_resumed=%lu", static_cast<unsigned long>(expected));
	else
	{
		if( !batch->journal.EntriesForState() ) return FALSE;
		batch->journal.StartStreaming(); batch->orderedPrefix = true;
		WriteDiagnostic("state_shadow tree_batch_ordered_started=%lu", static_cast<unsigned long>(expected));
	}
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeReplayOrdered(unsigned action, const char* path, DWORD length)
{
	ReplayBatchDiagnostic* batch = gReplayBatch;
	if( !batch || !batch->orderedPrefix || batch->replaying || batch->fallbackPending || !path ) return FALSE;
	const auto* entry = batch->journal.Next();
	if( !entry || entry->action != action || !entry->second.empty()
		|| entry->first.size() != length || memcmp(entry->first.data(), path, length) ) return FALSE;
	const unsigned before = batch->replayed;
	ReplayBatchSlice(batch, 1, MAXULONGLONG);
	if( batch->abandoned ) { LeeyesDiagnosticTreeBatchAbandon(); return FALSE; }
	if( batch->replayed != before + 1 ) return FALSE;
	++batch->orderedReplayed;
	static bool injectedOrderedFile = false;
	if( !injectedOrderedFile && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_ORDERED_FILE_EVENT", nullptr, 0) )
	{
		void* testPath = LeeyesDiagnosticMakeAnsi(path, length);
		if( testPath )
		{
			injectedOrderedFile = true;
			const BOOL captured = LeeyesDiagnosticCaptureTree(batch->method, batch->owner, 0, reinterpret_cast<DWORD>(testPath), 0);
			LeeyesDiagnosticClearAnsi(&testPath);
			if( captured ) LeeyesStateShadowPosted(0);
			WriteDiagnostic("state_shadow tree_batch_ordered_test_file=%d", captured ? 1 : 0);
		}
	}
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeOrderedDrained()
{
	return gReplayBatch && gReplayBatch->orderedPrefix && gReplayBatch->journal.Empty() ? TRUE : FALSE;
}
extern "C" DWORD __cdecl LeeyesDiagnosticTreeBatchSize()
{
	return gReplayBatch ? static_cast<DWORD>(gReplayBatch->journal.Size()) : MAXDWORD;
}

extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchResolve(BOOL applied, HWND window, BOOL allowRepeat)
{
	ReplayBatchDiagnostic* batch = gReplayBatch;
	if( !batch || batch->replaying ) return FALSE;
	batch->allowRepeat = allowRepeat != FALSE;
	if( !applied )
	{
		if( batch->recoveryOnly ) WriteDiagnostic("state_shadow tree_recovery_fallback=1");
		// The state probe is being torn down now. New callbacks arriving while
		// the replay timer is pending must join its journal, rather than trying
		// to force fallback on a probe that no longer exists.
		batch->fallbackPending = true;
		if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_SLICED_FALLBACK", nullptr, 0)
			&& window && ::IsWindow(window) )
		{
			const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayBatch);
			if( ::SetWindowSubclass(window, ReplayBatchSubclass, timerId, 0) )
			{
				batch->timerWindow = window;
				if( ::SetTimer(window, timerId, 15, nullptr) )
				{
					batch->timerScheduled = true;
					WriteDiagnostic("state_shadow tree_batch_state_fallback_scheduled=%u", batch->captured);
					return TRUE;
				}
				::RemoveWindowSubclass(window, ReplayBatchSubclass, timerId);
			}
		}
		gStateBatchResolving = true;
		const bool replayed = FlushReplayBatch("state_fallback");
		gStateBatchResolving = false;
		if( replayed && allowRepeat && ::GetEnvironmentVariableA("LEYEES_TREE_REPEAT_BATCH", nullptr, 0) ) gReplayBatchDone = false;
		return replayed ? TRUE : FALSE;
	}
	if( batch->orderedPrefix )
		WriteDiagnostic("state_shadow tree_batch_state_ordered_completed=%u", batch->orderedReplayed);
	else WriteDiagnostic("state_shadow tree_batch_state_discarded=%u", batch->captured);
	if( batch->recoveryOnly )
	{
		gTreeRecoveryBackoff = TreeRecoveryBackoff();
		gTreeReadFailureBackoff = TreeRecoveryBackoff();
		WriteDiagnostic("state_shadow tree_recovery_completed=1");
	}
	gReplayBatch = nullptr;
	gReplayBatchDone = !::GetEnvironmentVariableA("LEYEES_TREE_REPEAT_BATCH", nullptr, 0);
	delete batch;
	FlushTreeDiagnosticLogs();
	return TRUE;
}

extern "C" void __cdecl LeeyesStateShadowPosted(DWORD record)
{
	if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_POST_CLASSES", nullptr, 0) ) CountTreePostClass(record);
	gShadowPosts.fetch_add(1, std::memory_order_relaxed);
	if( FixtureTestIgnoreLatePost(record) ) ++gTreeTestLateIgnored;
	else if( FixturePostLexicallyDisjoint(record) ) ++gTreePostSkipped;
	else
	{
		gTreeLastPostedTick.store(::GetTickCount64(), std::memory_order_seq_cst);
		LeeyesDiagnosticTreeStateChanged();
		gTreePostedGeneration.fetch_add(1, std::memory_order_seq_cst);
	}
	StateSyncWorker* worker = gShadowWorker.load(std::memory_order_acquire);
	if( worker && ::GetEnvironmentVariableA("LEYEES_STATE_SCOPE_PROBE", nullptr, 0) )
	{
		const NotificationScope::Relation scope = ClassifyPostedScope(record);
		if( scope == NotificationScope::Relation::Related ) ++gShadowScopeRelated;
		else if( scope == NotificationScope::Relation::LexicallyDisjoint ) ++gShadowScopeDisjoint;
		else ++gShadowScopeUnknown;
	}
	if( worker ) { try { worker->Changed(); } catch( ... ) {} }
}
extern "C" unsigned long long __cdecl LeeyesDiagnosticTreePostedGeneration()
{
	return gTreePostedGeneration.load(std::memory_order_seq_cst);
}
extern "C" unsigned long long __cdecl LeeyesDiagnosticTreeLastPostedTick()
{
	return gTreeLastPostedTick.load(std::memory_order_seq_cst);
}
extern "C" void __cdecl LeeyesStateShadowNavigation(DWORD owner)
{
	StateSyncWorker* worker = gShadowWorker.load(std::memory_order_acquire);
	if( !worker && owner && !gDrainState.depth
		&& ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_EARLY", nullptr, 0) )
	{
		// Navigation is observed before the list's Shell object is replaced.
		// Bootstrap only from a uniquely identified list owned by this object;
		// the timer will probe again after the original navigation returns.
		HWND window = nullptr;
		if( FindDisplayedListWindow(window) && DispatchOwnerContainsListWindow(reinterpret_cast<void*>(owner), window) )
		{
			gDrainState.currentListWindow = window;
			ProbeOwnerPidl(reinterpret_cast<void*>(owner));
			gDrainState = DrainState();
			worker = gShadowWorker.load(std::memory_order_acquire);
			if( worker ) WriteDiagnostic("state_shadow list_watched_early_navigation=1");
		}
	}
	if( !worker || !owner || owner != gShadowOwner.load() ) return;
	gListVerifyPending = ShadowPendingUpdate(); gListVerifyState.reset();
	gListWatchCommitted = ShadowPendingUpdate();
	gListWatchCommittedState.reset();
	gListWatchBootstrapAttempted = false;
	if( gShadowPending.owner == reinterpret_cast<void*>(owner) ) ApplyShadowPending(false);
	gShadowNavigation.store(true);
	if( gShadowPending.owner == reinterpret_cast<void*>(owner) )
	{
		if( gShadowPending.retryUnavailable ) WriteDiagnostic("state_shadow navigation_discarded_resync_retry=1");
		gShadowPending = ShadowPendingUpdate(); gShadowPendingBaseline.reset();
	}
	::AcquireSRWLockExclusive(&gShadowScopeLock);
	gShadowScopeTarget[0] = L'\0';
	::ReleaseSRWLockExclusive(&gShadowScopeLock);
	WriteDiagnostic("state_shadow navigation_invalidated=1");
	try { worker->SetTarget(L""); } catch( ... ) {}
	WatchedSyncWorker* watched = gListWatchedShadow.load(std::memory_order_acquire);
	if( watched ) { try { watched->SetTarget(L""); } catch( ... ) {} }
}
#endif

void ConfigureFileMoveBatch(DWORD threshold, bool diagnosticLog)
{
	gBatchThreshold = threshold < 2 ? 2 : (threshold > 100000 ? 100000 : threshold);
	gDiagnosticLog = diagnosticLog;
	WriteDiagnostic("configured threshold=%lu", static_cast<unsigned long>(gBatchThreshold));
}

void ConfigureListDeleteBatch(bool enabled)
{
	gListDeleteBatchEnabled = enabled;
}

void ResetFileMoveBatchState()
{
	gDrainState = DrainState();
	gDiagnosticLog = false;
	gListDeleteBatchEnabled = false;
}

extern "C" void __cdecl LeeyesInternalBeginChangeQueueDrain(void* notifier)
{
	if( gDrainState.depth != 0 )
	{
		++gDrainState.depth;
		return;
	}
	gDrainState = DrainState();
	gDrainState.depth = 1;
	#if UNICODEHACK_PATH_DEBUG
	gReplaySingle = ReplaySingleDiagnostic();
	gDrainState.unchangedSkipEligible = true;
	#endif
	const int queuedChanges = ReadQueuedChangeCount(notifier);
	if( queuedChanges <= 0 ) return;
	gDrainState.queuedChanges = static_cast<DWORD>(queuedChanges);
	gDrainState.suppress = gDrainState.queuedChanges >= gBatchThreshold;
	#if UNICODEHACK_PATH_DEBUG
	if( gReplayBatch && !gReplayBatch->replaying )
	{
		DWORD callback = 0, owner = 0;
		if( ReadDiagnosticNotifierMethod(notifier, callback, owner)
			&& reinterpret_cast<void*>(callback) == gReplayBatch->method
			&& reinterpret_cast<void*>(owner) == gReplayBatch->owner ) ++gReplayBatch->drains;
	}
	if( gDrainState.suppress && !gReplayBatch && !gReplayBatchDone
		&& !::GetEnvironmentVariableA("LEYEES_TREE_BATCH_FIXTURE_START", nullptr, 0)
		&& ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_REPLAY", nullptr, 0) )
	{
		DWORD callback = 0, owner = 0;
		ReadDiagnosticNotifierMethod(notifier, callback, owner);
		if( callback == 0x004f6784 && owner )
		{
			char limitText[16] = {};
			const DWORD length = ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_LIMIT", limitText, _countof(limitText));
			const unsigned limit = length && length < _countof(limitText) ? strtoul(limitText, nullptr, 10) : 4096;
			if( limit && limit <= 4096 ) gReplayBatch = new (std::nothrow) ReplayBatchDiagnostic(
				reinterpret_cast<void*>(callback), reinterpret_cast<void*>(owner), limit);
			WriteDiagnostic("state_shadow tree_batch_started=%d queued=%lu limit=%u",
				gReplayBatch ? 1 : 0, static_cast<unsigned long>(gDrainState.queuedChanges), limit);
		}
	}
	if( gDrainState.suppress && ::GetEnvironmentVariableA("LEYEES_CALLBACK_BOUNDARY_PROBE", nullptr, 0) )
	{
		DWORD callback = 0, owner = 0;
		ReadDiagnosticNotifierMethod(notifier, callback, owner);
		WriteDiagnostic("state_shadow callback_boundary queued=%lu callback=%08lX owner=%08lX",
			static_cast<unsigned long>(gDrainState.queuedChanges), callback, owner);
	}
	WriteDiagnostic("drain_observe queued=%lu threshold=%lu suppress=%d",
		static_cast<unsigned long>(gDrainState.queuedChanges),
		static_cast<unsigned long>(gBatchThreshold), gDrainState.suppress ? 1 : 0);
	#endif
	if( gDrainState.suppress || gListDeleteBatchEnabled )
	{
		#if UNICODEHACK_PATH_DEBUG
		if( gDrainState.suppress )
		{
			if( !::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_QUIET", nullptr, 0) )
				LeeyesEnumerationDiagnosticSnapshot("drain_begin", gDrainState.queuedChanges);
			LeeyesTreeShadowBegin();
		}
		#endif
		const bool listKnown = FindDisplayedListWindow(gDrainState.currentListWindow);
		#if UNICODEHACK_PATH_DEBUG
		if( gDrainState.suppress && listKnown && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_SELECTED_FIXTURE_THUMBNAIL", nullptr, 0)
			&& ::GetEnvironmentVariableA("LEYEES_LIST_TEST_THUMBNAIL_PIXELS", nullptr, 0) )
			WriteDiagnostic("state_shadow selected_thumbnail_probe_attached=%d",
				::SetWindowSubclass(gDrainState.currentListWindow, ShadowWorkerSubclass,
					reinterpret_cast<UINT_PTR>(&ShadowWorkerSubclass), 0) ? 1 : 0);
		#endif
		if( gDrainState.suppress )
		{
			WriteDiagnostic("scope_owner list_known=%d hwnd=%p",
				listKnown ? 1 : 0, gDrainState.currentListWindow);
			WriteDiagnostic("drain_begin queued=%lu threshold=%lu",
				static_cast<unsigned long>(gDrainState.queuedChanges),
				static_cast<unsigned long>(gBatchThreshold));
		}
	}
}

BOOL LeeyesInternalShouldDeferFileChangeImpl(
	void* dispatchOwner, DWORD rawAction, DWORD path0, DWORD path1,
	void* returnAddress)
{
	#if UNICODEHACK_PATH_DEBUG
	LogDispatchSample(dispatchOwner, rawAction, path0, path1, returnAddress);
	StateSyncWorker* shadowWorker = gShadowWorker.load(std::memory_order_acquire);
	if( shadowWorker && (rawAction & 0xFF) <= 5 ) shadowWorker->Changed();
	if( (rawAction & 0xff) == 0 && ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_CONTINUOUS", nullptr, 0) )
	{
		char expected[kScopePathCapacity] = {}, actual[kScopePathCapacity] = {}; DWORD actualLength = 0;
		const DWORD length = ::GetEnvironmentVariableA("LEYEES_LIST_WATCH_TEST_DROP", expected, _countof(expected));
		if( length && length < _countof(expected) && strstr(expected, "\\fallback_fixture_")
			&& strstr(expected, "\\fallback_source\\__late_watch_")
			&& CopyDelphiAnsiString(path0, actual, _countof(actual), actualLength)
			&& _stricmp(actual, expected) == 0 )
		{
			const LRESULT count = gListWatchCommitted.window
				? ::SendMessageA(gListWatchCommitted.window, LVM_GETITEMCOUNT, 0, 0) : -1;
			WriteDiagnostic("state_shadow list_watched_test_drop=1 list_count=%ld", static_cast<long>(count));
			if( ::GetEnvironmentVariableA("LEYEES_LIST_WATCH_TEST_RESTART", nullptr, 0) )
			{
				WatchedSyncWorker* watched = gListWatchedShadow.load(std::memory_order_acquire);
				if( watched ) { try { watched->SetTarget(gShadowTarget); } catch( ... ) {} }
				WriteDiagnostic("state_shadow list_watched_test_restart=1");
			}
			return TRUE;
		}
	}
	#endif
	if( !gDrainState.suppress && !gListDeleteBatchEnabled ) return FALSE;
	const DWORD action = rawAction & 0xFF;
	// The dispatcher has a separate control-action branch (6) in the verified
	// Leeyes 2.6.1 body. It is not a filesystem item change and must continue
	// through Leeyes' own state machine. Leeyes loads the action into DL, so
	// EDX's upper 24 bits are unspecified and must not be treated as flags.
	if( action > 5 ) return FALSE;
	if( !gDrainState.currentOwnerKnown
		&& gDrainState.currentListWindow
		&& gDrainState.ownerProbeAttempts < 8 )
	{
		++gDrainState.ownerProbeAttempts;
		if( DispatchOwnerContainsListWindow(dispatchOwner, gDrainState.currentListWindow) )
		{
			gDrainState.currentOwner = dispatchOwner;
			gDrainState.currentOwnerKnown = true;
			WriteDiagnostic("scope_owner_bound owner=%p hwnd=%p probe=%lu",
				dispatchOwner, gDrainState.currentListWindow,
				static_cast<unsigned long>(gDrainState.ownerProbeAttempts));
			#if UNICODEHACK_PATH_DEBUG
			ProbeOwnerPidl(dispatchOwner);
			#endif
		}
	}
	if( !gDrainState.currentOwnerKnown )
		return FALSE;
	if( dispatchOwner != gDrainState.currentOwner )
	{
		++gDrainState.outOfScopeChanges;
		return FALSE;
	}
	if( !gDrainState.currentDirectoryKnown )
	{
		if( gDrainState.currentDirectoryProbeAttempted ) return FALSE;
		gDrainState.currentDirectoryProbeAttempted = true;
		char verifiedDirectory[kScopePathCapacity] = {};
		if( !ReadOwnerDirectory(dispatchOwner, verifiedDirectory, _countof(verifiedDirectory)) )
			return FALSE;
		BindCurrentDirectory(verifiedDirectory, "shell_pidl");
	}
	const FileChangeScope scope = ClassifyFileChangeScope(action, path0, path1);
	#if UNICODEHACK_PATH_DEBUG
	if( gDrainState.pidlDirectoryKnown && gDrainState.pidlSamples++ < 4 )
	{
		char first[kScopePathCapacity] = {}, second[kScopePathCapacity] = {};
		char dir0[kScopePathCapacity] = {}, dir1[kScopePathCapacity] = {};
		const char* leaf = nullptr;
		DWORD len0 = 0, len1 = 0;
		wchar_t wide0[kScopePathCapacity] = {}, wide1[kScopePathCapacity] = {};
		const bool valid0 = CopyDelphiAnsiString(path0, first, _countof(first), len0)
			&& SplitAbsolutePath(first, dir0, _countof(dir0), leaf)
			&& ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, dir0, -1, wide0, _countof(wide0)) != 0;
		const bool valid1 = CopyDelphiAnsiString(path1, second, _countof(second), len1)
			&& SplitAbsolutePath(second, dir1, _countof(dir1), leaf)
			&& ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, dir1, -1, wide1, _countof(wide1)) != 0;
		const bool matches = (valid0 && ::CompareStringOrdinal(wide0, -1, gDrainState.pidlDirectory, -1, FALSE) == CSTR_EQUAL)
			|| (valid1 && ::CompareStringOrdinal(wide1, -1, gDrainState.pidlDirectory, -1, FALSE) == CSTR_EQUAL);
		WriteDiagnostic("scope_pidl_shadow scope=%d valid0=%d valid1=%d exact_match=%d",
			static_cast<int>(scope), valid0 ? 1 : 0, valid1 ? 1 : 0, matches ? 1 : 0);
	}
	#endif
	if( scope != FileChangeScope::DisplayedDirectory )
	{
		if( scope == FileChangeScope::OtherDirectory )
			++gDrainState.outOfScopeChanges;
		else
			++gDrainState.unknownScopeChanges;
		if( gDrainState.outOfScopeChanges + gDrainState.unknownScopeChanges <= 4 )
			WriteDiagnostic("pass_scope action=%lu scope=%s owner=%p",
				static_cast<unsigned long>(action),
				scope == FileChangeScope::OtherDirectory ? "other" : "unknown",
				dispatchOwner);
		return FALSE;
	}
	if( gListDeleteBatchEnabled && !gDrainState.listStateWatchAttempted )
		ObserveListDeleteBatch(dispatchOwner, gDrainState.currentListWindow, gDrainState.currentDirectory);
	if( !gDrainState.suppress ) return FALSE;
	#if UNICODEHACK_PATH_DEBUG
	// Replay exactly one deletion in place to test Delphi's ABI and ownership.
	// It is deliberately not a queue, a fallback, or a Release path.
	static bool replayAttempted = false;
	const bool renameReplay = ::GetEnvironmentVariableA("LEYEES_REPLAY_RENAME_SINGLE", nullptr, 0) != 0;
	if( (renameReplay ? (action == 1 || action == 4) : action == 2) && !replayAttempted
		&& ::GetEnvironmentVariableA("LEYEES_REPLAY_ABI_EXPERIMENT", nullptr, 0) )
	{
		replayAttempted = true;
		char first[kScopePathCapacity] = {}, second[kScopePathCapacity] = {};
		DWORD firstLength = 0, secondLength = 0;
		if( CopyDelphiAnsiString(path0, first, _countof(first), firstLength)
			&& CopyDelphiAnsiString(path1, second, _countof(second), secondLength)
			&& firstLength && (!renameReplay || secondLength) && !memchr(first, '\0', firstLength)
			&& !memchr(second, '\0', secondLength) )
		{
			if( ::GetEnvironmentVariableA("LEYEES_REPLAY_DELAY_SINGLE", nullptr, 0) )
			{
				gReplaySingle.owner = dispatchOwner;
				gReplaySingle.window = gDrainState.currentListWindow;
				gReplaySingle.action = rawAction;
				gReplaySingle.firstLength = firstLength;
				gReplaySingle.secondLength = secondLength;
				memcpy(gReplaySingle.first, first, firstLength + 1);
				memcpy(gReplaySingle.second, second, secondLength + 1);
				WriteDiagnostic("state_shadow replay_single_deferred=1");
				return TRUE;
			}
			if( LeeyesDiagnosticReplayAnsi(dispatchOwner, rawAction,
				first, firstLength, second, secondLength) )
			{
				WriteDiagnostic("state_shadow replay_abi_completed=1 action=%lu", static_cast<unsigned long>(action));
				return TRUE;
			}
		}
		WriteDiagnostic("state_shadow replay_abi_unavailable=1");
	}
	if( ::GetEnvironmentVariableA("LEYEES_REPLAY_ABI_ONLY", nullptr, 0) ) return FALSE;
	#endif
	if( !RememberDirtyList(dispatchOwner) ) return FALSE;
	#if UNICODEHACK_PATH_DEBUG
	if( ::GetEnvironmentVariableA("LEYEES_STATE_SKIP_UNCHANGED_EXPERIMENT", nullptr, 0) )
	{
		unsigned reason = 0;
		if( !IsKnownTransientDiagnostic(action, path0, path1, reason) )
		{
			gDrainState.unchangedSkipEligible = false;
			if( !gDrainState.unchangedRejectLogged )
			{
				gDrainState.unchangedRejectLogged = true;
				WriteDiagnostic("state_shadow unchanged_guard_reject=%u action=%lu", reason, static_cast<unsigned long>(action));
			}
		}
	}
	#endif
	gDrainState.needsResync = true;
	++gDrainState.suppressedChanges;
	if( gDrainState.suppressedChanges <= 4 )
		WriteDiagnostic("defer index=%lu action=%lu caller=%p list=%p",
			static_cast<unsigned long>(gDrainState.suppressedChanges),
			static_cast<unsigned long>(action), returnAddress, dispatchOwner);
	return TRUE;
}

extern "C" BOOL __cdecl LeeyesInternalShouldDeferFileChange(
	void* dispatchOwner, DWORD rawAction, DWORD path0, DWORD path1, void* returnAddress)
{
	const BOOL deferred = LeeyesInternalShouldDeferFileChangeImpl(
		dispatchOwner, rawAction, path0, path1, returnAddress);
	#if UNICODEHACK_PATH_DEBUG
	// A callback handled by Leeyes can change the list independently of the
	// snapshot last applied by the experiment. Do not use that old baseline.
	if( !deferred && dispatchOwner == reinterpret_cast<void*>(gShadowOwner.load()) )
	{
		if( (gShadowAppliedState || gShadowPendingBaseline)
			&& ::GetEnvironmentVariableA("LEYEES_STATE_SKIP_UNCHANGED_EXPERIMENT", nullptr, 0) )
			WriteDiagnostic("state_shadow applied_baseline_invalidated action=%lu pending=%d",
				static_cast<unsigned long>(rawAction & 0xff), gShadowPending.owner ? 1 : 0);
		InvalidateShadowApplied();
		gShadowPendingBaseline.reset();
	}
	#endif
	return deferred;
}

extern "C" void __cdecl LeeyesInternalEndChangeQueueDrain()
{
	if( gDrainState.depth == 0 ) return;
	if( --gDrainState.depth != 0 ) return;
	#if UNICODEHACK_PATH_DEBUG
	if( gReplayBatch && gReplayBatch->discoverParents && !gReplayBatch->parentsAttached
		&& !gReplayBatch->replaying )
	{
		ReplayBatchDiagnostic* batch = gReplayBatch;
		if( !batch->timerScheduled && !batch->fallbackPending && batch->discoveredParents.size() == 1
			&& gDrainState.currentListWindow && ::IsWindow(gDrainState.currentListWindow)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DISCOVERY_GRACE", nullptr, 0) )
		{
			char delayText[16] = {};
			const DWORD length = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DISCOVERY_GRACE_MS", delayText, _countof(delayText));
			unsigned delay = length && length < _countof(delayText) ? strtoul(delayText, nullptr, 10) : 300;
			if( delay < 50 || delay > 2000 ) delay = 300;
			const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayBatch);
			HWND window = gDrainState.currentListWindow;
			if( ::SetWindowSubclass(window, ReplayBatchSubclass, timerId, 0) )
			{
				batch->timerWindow = window;
				if( ::SetTimer(window, timerId, delay, nullptr) )
				{
					batch->timerScheduled = true; batch->discoveryWaiting = true;
					WriteDiagnostic("state_shadow tree_discovery_wait_ms=%u", delay);
					if( !batch->testDeferredParent.empty()
						&& !::PostMessageW(window, WM_APP + 0x4f2, timerId, 0) )
						batch->fallbackPending = true;
				}
				else ::RemoveWindowSubclass(window, ReplayBatchSubclass, timerId);
			}
		}
		if( batch->discoveryWaiting && (batch->fallbackPending || batch->discoveredParents.size() >= 2) )
			FinishTreeDiscovery(batch, batch->timerWindow);
		else if( !batch->timerScheduled ) FinishTreeDiscovery(batch, gDrainState.currentListWindow);
	}
	if( gReplayBatch && !gReplayBatch->replaying
		&& !::GetEnvironmentVariableA("LEYEES_TREE_BATCH_STATE_GATE", nullptr, 0) )
	{
		ReplayBatchDiagnostic* batch = gReplayBatch;
		if( ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_DELAY", nullptr, 0) && !batch->timerScheduled
			&& gDrainState.currentListWindow && ::IsWindow(gDrainState.currentListWindow) )
		{
			const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayBatch);
			HWND window = gDrainState.currentListWindow;
			char delayText[16] = {};
			const DWORD delayLength = ::GetEnvironmentVariableA("LEYEES_TREE_BATCH_DELAY_MS", delayText, _countof(delayText));
			unsigned delay = delayLength && delayLength < _countof(delayText) ? strtoul(delayText, nullptr, 10) : 300;
			if( delay < 100 || delay > 5000 ) delay = 300;
			if( ::SetWindowSubclass(window, ReplayBatchSubclass, timerId, 0) )
			{
				batch->timerWindow = window;
				if( ::SetTimer(window, timerId, delay, nullptr) )
				{
					batch->timerScheduled = true;
					WriteDiagnostic("state_shadow tree_batch_timer_scheduled=1 captured=%u delay_ms=%u", batch->captured, delay);
				}
				else ::RemoveWindowSubclass(window, ReplayBatchSubclass, timerId);
			}
		}
		if( !batch->timerScheduled ) FlushReplayBatch("drain_end");
	}
	#endif

	void* dirtyLists[kMaximumDirtyLists] = {};
	const DWORD dirtyListCount = gDrainState.dirtyListCount;
	const DWORD queuedChanges = gDrainState.queuedChanges;
	const DWORD suppressedChanges = gDrainState.suppressedChanges;
	const DWORD outOfScopeChanges = gDrainState.outOfScopeChanges;
	const DWORD unknownScopeChanges = gDrainState.unknownScopeChanges;
	const HWND listWindow = gDrainState.currentListWindow;
	const bool listStateWatchAttempted = gDrainState.listStateWatchAttempted;
	const bool currentDirectoryKnown = gDrainState.currentDirectoryKnown;
	void* listStateOwner = gDrainState.currentOwnerKnown ? gDrainState.currentOwner : nullptr;
	char listStateDirectory[kScopePathCapacity] = {};
	if( currentDirectoryKnown ) strncpy_s(listStateDirectory, _countof(listStateDirectory), gDrainState.currentDirectory, _TRUNCATE);
	#if UNICODEHACK_PATH_DEBUG
	const bool unchangedSkipEligible = gDrainState.unchangedSkipEligible;
	#endif
	const LONG lowItemNullFallbacks = gLeeyesInternalLowItemNullFallbackCount;
	const LONG lowItemGuardActive = gLeeyesInternalLowItemGuardActive;
	const bool needsResync = gDrainState.suppress && gDrainState.needsResync;
	if( dirtyListCount != 0 )
		memcpy(dirtyLists, gDrainState.dirtyLists,
			dirtyListCount * sizeof(gDrainState.dirtyLists[0]));
	bool deferListResync = false;
	if( needsResync && gListDeleteBatchEnabled && listStateWatchAttempted && currentDirectoryKnown
		&& dirtyListCount == 1 && listStateOwner && dirtyLists[0] == listStateOwner )
		deferListResync = DeferListDeleteBatchResync(listStateOwner, listWindow, listStateDirectory);
	#if UNICODEHACK_PATH_DEBUG
	if( gDrainState.suppress ) LeeyesTreeShadowEnd();
	#endif
	// Disable suppression before entering Leeyes' normal resync path so any
	// synchronous work it causes cannot be mistaken for queue-drain callbacks.
	gDrainState = DrainState();
	#if UNICODEHACK_PATH_DEBUG
	if( gReplaySingle.owner )
	{
		const ReplaySingleDiagnostic pending = gReplaySingle;
		gReplaySingle = ReplaySingleDiagnostic();
		if( ::IsWindow(pending.window) && (pending.method
			? IsReadableRange(reinterpret_cast<const BYTE*>(pending.owner), sizeof(DWORD))
			: DispatchOwnerContainsListWindow(pending.owner, pending.window)) )
		{
			const UINT_PTR timerId = reinterpret_cast<UINT_PTR>(&gReplayTimer);
			bool scheduled = false;
			HMODULE pinned = nullptr;
			if( ::GetEnvironmentVariableA("LEYEES_REPLAY_TIMER_SINGLE", nullptr, 0)
				&& !needsResync && !gReplayTimer.owner
				&& ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
					reinterpret_cast<LPCWSTR>(&ReplaySingleSubclass), &pinned)
				&& ::SetWindowSubclass(pending.window, ReplaySingleSubclass, timerId, 0) )
			{
				gReplayTimer = pending;
				if( ::SetTimer(pending.window, timerId, 300, nullptr) )
				{
					WriteDiagnostic("state_shadow replay_timer_scheduled=1");
					scheduled = true;
				}
				else
				{
					gReplayTimer = ReplaySingleDiagnostic();
					::RemoveWindowSubclass(pending.window, ReplaySingleSubclass, timerId);
				}
			}
			if( !scheduled )
			{
				const LRESULT before = ::SendMessageA(pending.window, LVM_GETITEMCOUNT, 0, 0);
				const BOOL replayed = pending.method
					? LeeyesDiagnosticReplayMethod(pending.method, pending.owner, pending.action,
						pending.first, pending.firstLength, pending.second, pending.secondLength)
					: LeeyesDiagnosticReplayAnsi(pending.owner, pending.action,
						pending.first, pending.firstLength, pending.second, pending.secondLength);
				const LRESULT after = ::SendMessageA(pending.window, LVM_GETITEMCOUNT, 0, 0);
				WriteDiagnostic("state_shadow replay_single_completed=%d before=%ld after=%ld",
					replayed ? 1 : 0, static_cast<long>(before), static_cast<long>(after));
			}
		}
		else WriteDiagnostic("state_shadow replay_single_owner_invalid=1");
	}
	#endif

	DWORD refreshed = 0;
	DWORD restoredSelections = 0;
	#if UNICODEHACK_PATH_DEBUG
	if( needsResync && !deferListResync && dirtyListCount == 1 && listWindow
		&& ::GetEnvironmentVariableA("LEYEES_STATE_APPLY_EXPERIMENT", nullptr, 0) )
	{
		StateSyncWorker* worker = gShadowWorker.load(std::memory_order_acquire);
		if( worker && !worker->Failed() && !gShadowNavigation.load()
			&& !gShadowTarget.empty() && dirtyLists[0] == reinterpret_cast<void*>(gShadowOwner.load())
			&& (!gShadowPending.owner || (gShadowPending.owner == dirtyLists[0]
				&& gShadowPending.window == listWindow)) )
		{
			const bool startWatchedAttempt = !gShadowPending.owner;
			if( !gShadowPending.owner )
			{
				gShadowPending.deadline = ::GetTickCount64() + 5000;
				gShadowPending.unchangedSkipEligible = unchangedSkipEligible;
				gShadowPendingBaseline.reset();
				if( unchangedSkipEligible && gShadowAppliedState && gShadowAppliedIndexReady
					&& ::GetEnvironmentVariableA("LEYEES_STATE_SKIP_UNCHANGED_EXPERIMENT", nullptr, 0) )
				{
					const ULONGLONG copyStarted = ::GetTickCount64();
					try { gShadowPendingBaseline.reset(new DirectorySnapshot::State(*gShadowAppliedState)); }
					catch( ... ) { gShadowPendingBaseline.reset(); }
					WriteDiagnostic("state_shadow baseline_copy=%d elapsed_ms=%llu entries=%lu",
						gShadowPendingBaseline ? 1 : 0, ::GetTickCount64() - copyStarted,
						static_cast<unsigned long>(gShadowAppliedState->entries.size()));
				}
			}
			else if( !unchangedSkipEligible )
			{
				gShadowPending.unchangedSkipEligible = false;
				gShadowPendingBaseline.reset();
			}
			gShadowPending.owner = dirtyLists[0]; gShadowPending.window = listWindow;
			WatchedSyncWorker* watched = gListWatchedShadow.load(std::memory_order_acquire);
			if( watched && startWatchedAttempt && ::GetEnvironmentVariableA("LEYEES_LIST_WATCHED_PRIMARY", nullptr, 0) )
			{
				// Invalidate old candidates immediately and request a fresh
				// snapshot from the existing watcher without changing targets.
				try { if( !watched->RequestRefresh() ) { ApplyShadowPending(false); return; } }
				catch( ... ) { ApplyShadowPending(false); return; }
			}
			WriteDiagnostic("state_shadow apply_deferred=1");
			return;
		}
	}
	#endif
	if( needsResync && !deferListResync )
	{
		#if UNICODEHACK_PATH_DEBUG
		const ULONGLONG beginTick = ::GetTickCount64();
		#endif
		const ListSelectionSnapshot selection = CaptureListSelection();
		#if UNICODEHACK_PATH_DEBUG
		const ULONGLONG captureTick = ::GetTickCount64();
		#endif
		for( DWORD index = 0; index < dirtyListCount; ++index )
		{
			if( !dirtyLists[index] ) continue;
			LeeyesInternalInvokeFullResync(dirtyLists[index]);
			++refreshed;
		}
		#if UNICODEHACK_PATH_DEBUG
		const ULONGLONG resyncTick = ::GetTickCount64();
		#endif
		restoredSelections = RestoreListSelection(selection);
		#if UNICODEHACK_PATH_DEBUG
		WriteDiagnostic("resync_timing capture_ms=%llu full_resync_ms=%llu restore_ms=%llu",
			captureTick - beginTick, resyncTick - captureTick, ::GetTickCount64() - resyncTick);
		#endif
	}
	if( gListDeleteBatchEnabled && listStateWatchAttempted && listStateOwner
		&& currentDirectoryKnown && !deferListResync )
		CompleteListDeleteBatchLegacyResync(listStateOwner, listWindow, listStateDirectory,
			needsResync && !deferListResync);
	int finalListCount = -1;
#if UNICODEHACK_PATH_DEBUG
	if( gDiagnosticLog && listWindow && ::IsWindow(listWindow) )
		finalListCount = static_cast<int>(::SendMessageA(
			listWindow, LVM_GETITEMCOUNT, 0, 0));
#endif
	if( queuedChanges >= gBatchThreshold )
		WriteDiagnostic("drain_end queued=%lu deferred=%lu out_of_scope=%lu unknown_scope=%lu low_item_guard=%ld low_item_null_fallback=%ld refreshed=%lu selection=%lu list_count=%d",
			static_cast<unsigned long>(queuedChanges),
			static_cast<unsigned long>(suppressedChanges),
			static_cast<unsigned long>(outOfScopeChanges),
			static_cast<unsigned long>(unknownScopeChanges),
			static_cast<long>(lowItemGuardActive),
			static_cast<long>(lowItemNullFallbacks),
			static_cast<unsigned long>(refreshed),
			static_cast<unsigned long>(restoredSelections), finalListCount);
	#if UNICODEHACK_PATH_DEBUG
	if( queuedChanges >= gBatchThreshold
		&& !::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_QUIET", nullptr, 0) )
		LeeyesEnumerationDiagnosticSnapshot("drain_end", queuedChanges);
	#endif
}

}
