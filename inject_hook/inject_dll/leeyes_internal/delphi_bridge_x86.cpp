#include "../stdafx.h"
#include "delphi_bridge_x86.h"
#include "filelist_trace.h"
#include "file_move_batch.h"

#if !defined(_M_IX86)
#error "Leeyes internal Delphi bridge requires Win32 x86"
#endif

#include "ncodehook_bridge.h"
#if UNICODEHACK_PATH_DEBUG
#include <atomic>
#include <commctrl.h>
#include <set>
#include "../../common/state_sync_worker.h"
#include "../../common/watched_sync_worker.h"
#pragma comment(lib, "comctl32.lib")
#endif
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

#if UNICODEHACK_PATH_DEBUG
extern "C" BOOL __cdecl LeeyesDiagnosticCaptureTree(void* method, void* owner,
	DWORD action, DWORD first, DWORD second);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeReplayActive();
extern "C" DWORD gLeeyesDiagnosticTreeCallbackContinuation = 0;
// The patched call site is entered by a jump, not a call. The original
// Delphi callback consumes the one stack argument with ret 4.
extern "C" __declspec(naked) void LeeyesDiagnosticTreeCallbackHook()
{
	__asm {
		// Reproduce the mov overwritten together with the callback call.
		// The original branch at 0x004C95D1 targets 0x004C960A, so that
		// instruction must remain intact even when no callback is invoked.
		mov eax, dword ptr [edi + 0x1c]
		pushfd
		pushad
		mov ebx, dword ptr [esp + 12]
		push dword ptr [ebx + 4]
		push ecx
		push edx
		push eax
		push dword ptr [edi + 0x18]
		call LeeyesDiagnosticCaptureTree
		add esp, 20
		test eax, eax
		jnz deferred
		popad
		popfd
		call dword ptr [edi + 0x18]
		jmp dword ptr [gLeeyesDiagnosticTreeCallbackContinuation]
	deferred:
		popad
		popfd
		add esp, 4
		jmp dword ptr [gLeeyesDiagnosticTreeCallbackContinuation]
	}
}
// Diagnostic only: reconstruct Delphi-owned AnsiStrings and invoke the
// original dispatcher immediately. This checks its calling convention and
// string ownership; it is not yet a deferred-replay implementation.
extern "C" BOOL __cdecl LeeyesDiagnosticReplayMethod(void* method, void* owner, DWORD action,
	const char* first, DWORD firstLength, const char* second, DWORD secondLength)
{
	static const BYTE allocSignature[] = { 0x85, 0xc0, 0x7e, 0x24, 0x50 };
	static const BYTE clearSignature[] = { 0x8b, 0x10, 0x85, 0xd2, 0x74, 0x1c };
	if( !owner || !method || !first || !firstLength
		|| firstLength > 4096 || secondLength > 4096 || (secondLength && !second)
		|| memcmp(reinterpret_cast<void*>(0x004050cc), allocSignature, sizeof(allocSignature))
		|| memcmp(reinterpret_cast<void*>(0x00405008), clearSignature, sizeof(clearSignature)) ) return FALSE;
	void* firstString = nullptr;
	void* secondString = nullptr;
	const DWORD allocate = 0x004050cc, clear = 0x00405008;
	__asm {
		mov eax, firstLength
		call allocate
		mov firstString, eax
	}
	if( !firstString ) return FALSE;
	memcpy(firstString, first, firstLength);
	if( secondLength )
	{
		__asm {
			mov eax, secondLength
			call allocate
			mov secondString, eax
		}
		if( !secondString )
		{
			__asm {
				lea eax, firstString
				call clear
			}
			return FALSE;
		}
		memcpy(secondString, second, secondLength);
	}
	__asm {
		push secondString
		mov ecx, firstString
		mov edx, action
		mov eax, owner
		call method
		lea eax, secondString
		call clear
		lea eax, firstString
		call clear
	}
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticReplayAnsi(void* owner, DWORD action,
	const char* first, DWORD firstLength, const char* second, DWORD secondLength)
{
	return LeeyesDiagnosticReplayMethod(reinterpret_cast<void*>(gLeeyesInternalOriginalFileChangeDispatch),
		owner, action, first, firstLength, second, secondLength);
}
// Allocate each string before suppressing its callback. A replay should not
// need a new Delphi string allocation after the original event is gone.
extern "C" void* __cdecl LeeyesDiagnosticMakeAnsi(const char* bytes, DWORD byteLength)
{
	static const BYTE signature[] = { 0x85, 0xc0, 0x7e, 0x24, 0x50 };
	static const BYTE clearSignature[] = { 0x8b, 0x10, 0x85, 0xd2, 0x74, 0x1c };
	if( !bytes || !byteLength || byteLength > 4096
		|| memcmp(reinterpret_cast<void*>(0x004050cc), signature, sizeof(signature))
		|| memcmp(reinterpret_cast<void*>(0x00405008), clearSignature, sizeof(clearSignature)) ) return nullptr;
	void* result = nullptr;
	const DWORD allocate = 0x004050cc;
	__asm {
		mov eax, byteLength
		call allocate
		mov result, eax
	}
	if( result ) memcpy(result, bytes, byteLength);
	return result;
}
extern "C" void __cdecl LeeyesDiagnosticClearAnsi(void** value)
{
	if( !value || !*value ) return;
	const DWORD clear = 0x00405008;
	__asm {
		mov eax, value
		call clear
	}
}
extern "C" BOOL __cdecl LeeyesDiagnosticReplayOwned(void* method, void* owner,
	DWORD action, void* first, void* second)
{
	if( !method || !owner || !first ) return FALSE;
	__asm {
		push second
		mov ecx, first
		mov edx, action
		mov eax, owner
		call method
	}
	return TRUE;
}
// Debug-only adapter allows the existing fixture reconciliation to consume a
// continuously watched candidate. The normal notifier path and generation
// guard still run; this does not enable the experiment in Release.
class DiagnosticTreeWorker
{
public:
	DiagnosticTreeWorker(const wchar_t* launcher, DWORD timeout, bool watched) : watchedMode_(watched)
	{
		if( watched ) watched_.reset(new (std::nothrow) WatchedSyncWorker(launcher, timeout,
			::GetEnvironmentVariableW(L"LEYEES_STATE_WATCH_ENTRY_IDENTITIES", nullptr, 0) != 0));
		else baseline_.reset(new (std::nothrow) StateSyncWorker(launcher, timeout));
	}
	bool Start() { return watchedMode_ ? watched_ && watched_->Start() : baseline_ && baseline_->Start(); }
	bool Failed() const { return watchedMode_ ? !watched_ || watched_->Failed() : !baseline_ || baseline_->Failed(); }
	DWORD LastError() { return watchedMode_ ? watched_ ? watched_->Error() : ERROR_INVALID_HANDLE : baseline_ ? baseline_->LastError() : ERROR_INVALID_HANDLE; }
	unsigned ChildStarts() const { return watchedMode_ && watched_ ? watched_->ChildStarts() : 0; }
	unsigned SnapshotFrames() const { return watchedMode_ && watched_ ? watched_->SnapshotFrames() : 0; }
	bool RefreshWatched() { return watchedMode_ && watched_ && watched_->RequestRefresh(); }
	bool HasContinuousBaseline(const StateSyncCoordinator::Candidate& candidate)
	{
		WatchedSyncWorker::Candidate ticket; ticket.target = candidate.ticket.target; ticket.generation = candidate.ticket.changes;
		return watchedMode_ && watched_ && watched_->HasContinuousBaseline(ticket);
	}
	bool IsCommittedCurrent(const StateSyncCoordinator::Candidate& candidate)
	{
		WatchedSyncWorker::Candidate ticket; ticket.target = candidate.ticket.target; ticket.generation = candidate.ticket.changes;
		return watchedMode_ && watched_ && watched_->IsCommittedCurrent(ticket);
	}
	void SetTarget(const std::wstring& path)
	{
		if( watchedMode_ ) { if( !watched_ || !watched_->SetTarget(path) ) throw std::bad_alloc(); }
		else baseline_->SetTarget(path);
	}
	void Changed() { if( !watchedMode_ && baseline_ ) baseline_->Changed(); }
	void RetryTarget(const std::wstring& path, bool candidateWasCurrent)
	{
		// A dirty watched stream already schedules a fresh snapshot. If only
		// an external guard changed, request one without replacing the child.
		if( !watchedMode_ ) SetTarget(path);
		else if( candidateWasCurrent && (!watched_ || !watched_->RequestRefresh()) ) throw std::bad_alloc();
	}
	bool TakeCandidate(StateSyncCoordinator::Candidate& output)
	{
		if( !watchedMode_ ) return baseline_->TakeCandidate(output);
		WatchedSyncWorker::Candidate candidate;
		if( !watched_ || !watched_->Take(candidate) ) return false;
		output.ticket = { 1, candidate.target, candidate.generation };
		output.state = std::move(candidate.state); output.hasEntryIdentities = candidate.hasEntryIdentities;
		output.entryIdentities = std::move(candidate.entryIdentities); return true;
	}
	bool CanApply(const StateSyncCoordinator::Candidate& candidate)
	{
		if( !watchedMode_ ) return baseline_->CanApply(candidate);
		WatchedSyncWorker::Candidate ticket; ticket.target = candidate.ticket.target; ticket.generation = candidate.ticket.changes;
		return watched_ && watched_->CanApply(ticket);
	}
	bool FinishApply(const StateSyncCoordinator::Candidate& candidate, bool success)
	{
		if( !watchedMode_ ) return baseline_->FinishApply(candidate, success);
		WatchedSyncWorker::Candidate ticket; ticket.target = candidate.ticket.target; ticket.generation = candidate.ticket.changes;
		if( success ) return watched_ && watched_->Finish(ticket);
		bool current = false;
		return watched_ && watched_->Release(ticket, current);
	}
private:
	bool watchedMode_;
	std::unique_ptr<StateSyncWorker> baseline_;
	std::unique_ptr<WatchedSyncWorker> watched_;
};
std::atomic<DiagnosticTreeWorker*> gDiagnosticTreeStateWorker(nullptr);
std::atomic<DiagnosticTreeWorker*> gDiagnosticTreeStateWorker2(nullptr);
// Process-lifetime Debug shadow workers. They never decide whether an
// original notification is suppressed or whether the UI is updated.
WatchedSyncWorker* gWatchedTreeShadow1 = nullptr;
WatchedSyncWorker* gWatchedTreeShadow2 = nullptr;
extern "C" void __cdecl LeeyesDiagnosticTreeStateChanged()
{
	DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
	if( worker ) { try { worker->Changed(); } catch( ... ) {} }
	worker = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
	if( worker ) { try { worker->Changed(); } catch( ... ) {} }
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchResolve(BOOL applied, HWND window, BOOL allowRepeat);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeSetRecovery(const wchar_t* first, const wchar_t* second, DWORD expandedMask);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterInput(const wchar_t* first, const wchar_t* second);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterChange(const wchar_t* first, const wchar_t* second, HWND window);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterReadFailure(const wchar_t* first, const wchar_t* second);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRetryRecoveryAfterIdentityMismatch(const wchar_t* first, const wchar_t* second);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeSetRecoveryIdentity(DWORD volume1, ULONGLONG id1, DWORD volume2, ULONGLONG id2);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRecoveryIdentityMatches(DWORD volume1, ULONGLONG id1, DWORD volume2, ULONGLONG id2);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeSetFixtureChildExpansion(BOOL expanded);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRestoreFixtureChildRequested();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRecoveryOnly();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeRecoveryReadoptFixture();
extern "C" DWORD __cdecl LeeyesDiagnosticTreeRecoveryExpandedMask();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchCanStart();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchAmbiguous();
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchPaths(std::vector<std::string>* paths);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchPathsCovered(const std::vector<std::string>* paths);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeTransientPaths(std::vector<std::string>* paths);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeOrderChanges(const std::vector<std::pair<unsigned, std::string>>* changes,
	std::vector<size_t>* order, std::vector<std::pair<unsigned, std::string>>* originals);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBeginOrdered(size_t expected);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeReplayOrdered(unsigned action, const char* path, DWORD length);
extern "C" BOOL __cdecl LeeyesDiagnosticTreeOrderedDrained();
extern "C" void __cdecl LeeyesDiagnosticTreeTestReenterAttach();
extern "C" DWORD __cdecl LeeyesDiagnosticTreeBatchSize();
extern "C" void __cdecl LeeyesDiagnosticTreeLogPostClasses();
extern "C" void __cdecl LeeyesDiagnosticTreeBatchAbandon();
extern "C" unsigned long long __cdecl LeeyesDiagnosticTreePostedGeneration();
extern "C" unsigned long long __cdecl LeeyesDiagnosticTreeLastPostedTick();
extern "C" void __cdecl LeeyesStateShadowPosted(DWORD record);

namespace
{
std::atomic<unsigned long long> gTreeTimingCalls(0);
std::atomic<unsigned long long> gTreeTimingTicks(0);
std::atomic<unsigned long> gTreeRemovalSkipped(0);
DWORD gTreeRemovalReturnSite = 0;
DWORD gTreeRemovalOwner = 0;
HWND gTreeRemovalWindow = nullptr;
// Diagnostic only: paths are coalesced by parent; this is not a recoverable
// state snapshot and must not be enabled as a product notification policy.
struct TreeRemovalParent { HTREEITEM item; DWORD node; std::wstring path; };
const size_t kTreeRemovalParentCapacity = 8;
std::vector<TreeRemovalParent> gTreeRemovalPendingParents;
bool gTreeRemovalRescanning = false;
bool gTreeRemovalProbeRejected = false;
unsigned char gTreeRemovalTimerTag = 0;
const UINT_PTR kTreeRemovalTimerId = reinterpret_cast<UINT_PTR>(&gTreeRemovalTimerTag);
std::atomic<unsigned long long> gTreeActionCounts[7] = {};
std::atomic<unsigned long long> gTreeSearchCalls(0);
std::atomic<unsigned long long> gTreeSearchHits(0);
std::atomic<unsigned long long> gShadowCandidates(0);
std::atomic<unsigned long long> gShadowPositiveCandidates(0);
std::atomic<unsigned long long> gShadowContradictions(0);
std::atomic<unsigned long long> gShadowInvalidations(0);
std::atomic<unsigned long long> gShadowStateChanges(0);
std::atomic<unsigned long long> gShadowStateUnreadable(0);
std::atomic<unsigned long long> gShadowHandleVerified(0);
std::atomic<unsigned long long> gShadowHandleFailed(0);
std::atomic<unsigned long long> gTreeNodeDestructions(0);
std::atomic<unsigned long long> gTreeDataAssignments(0);
std::atomic<unsigned long long> gTreeExperimentApplied(0);
std::atomic<unsigned long long> gTreeWindowEpoch(0);
std::atomic<unsigned long long> gTreeObjectEpoch(0);
DWORD gTreeExperimentCaller = 0;
DWORD gTreeVerifiedPathGetter = 0;
DWORD gTreeVerifiedNodeDestructor = 0;
struct TreeNodeObservation { DWORD data, vmt, part, full, parent, cachedPath, pathLength, pathHash; };
struct TreeShadowEntry { DWORD owner, root, length, result; HTREEITEM item; TreeNodeObservation state; bool stateValid; char path[1040]; };
struct TreeShadowState { bool active; DWORD count; unsigned long long generation, windowEpoch, objectEpoch; HWND monitoredTree; TreeShadowEntry entries[16]; };
__declspec(thread) TreeShadowState gTreeShadow = {};
thread_local LARGE_INTEGER gTreeTimingStart = {};
thread_local unsigned int gTreeTimingDepth = 0;
bool TreeExperimentEnabled()
{
	static const bool enabled = ::GetEnvironmentVariableA("LEYEES_TREE_CACHE_EXPERIMENT", nullptr, 0) != 0;
	return enabled;
}
LRESULT CALLBACK TreeExperimentSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference)
{
	// Treat unknown messages as mutations. A read-only whitelist is safer than
	// attempting to enumerate every message that can change a tree or its data.
	if( message != TVM_GETITEMA && message != TVM_GETITEMW
		&& message != TVM_GETNEXTITEM && message != TVM_GETCOUNT )
		gTreeWindowEpoch.fetch_add(1);
	if( message == WM_NCDESTROY ) ::RemoveWindowSubclass(window, TreeExperimentSubclass, subclassId);
	return ::DefSubclassProc(window, message, wParam, lParam);
}
}
extern "C" void (__cdecl *gLeeyesInternalOriginalTreeTiming)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalTreeAction)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalTreeSearch)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalParentSearch)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalNodeDestroy)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalNodeData)() = nullptr;
extern "C" void (__cdecl *gLeeyesInternalOriginalTreeRescanProbe)() = nullptr;
extern "C" void __cdecl LeeyesTreeRescanObserve(DWORD owner, DWORD node, DWORD caller);
extern "C" void __cdecl LeeyesDiagnosticTreeIdentityAfterReplay(HWND window, DWORD owner)
{
	DWORD result = 0;
	try
	{
		std::vector<HTREEITEM> pending;
		HTREEITEM root = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_ROOT, 0));
		if( root ) pending.push_back(root);
		unsigned visited = 0, matches = 0;
		while( !pending.empty() && visited++ < 100000 )
		{
			HTREEITEM itemHandle = pending.back(); pending.pop_back();
			wchar_t text[128] = {}; TVITEMW item = {};
			item.mask = TVIF_TEXT | TVIF_STATE; item.hItem = itemHandle; item.pszText = text;
			item.cchTextMax = _countof(text); item.stateMask = TVIS_EXPANDED;
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) { matches = 2; break; }
			if( !wcscmp(text, L"__sentinel") ) { ++matches; result = 1u | ((item.state & TVIS_EXPANDED) ? 2u : 0u); }
			HTREEITEM next = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(itemHandle)));
			HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(itemHandle)));
			if( next ) pending.push_back(next);
			if( child ) pending.push_back(child);
		}
		if( !pending.empty() || matches != 1 ) result = 0;
	}
	catch( ... ) { result = 0; }
	LeeyesTreeRescanObserve(owner, result, 0xF000005B);
}
extern "C" void __cdecl LeeyesStateShadowPosted(DWORD record);
extern "C" void __cdecl LeeyesStateShadowNavigation(DWORD owner);
extern "C" void (__cdecl *gLeeyesOriginalStateNavigation)() = nullptr;
extern "C" __declspec(naked) void LeeyesStateNavigationHook()
{
	__asm
	{
		pushfd
		pushad
		push eax
		call LeeyesStateShadowNavigation
		add esp, 4
		popad
		popfd
		jmp dword ptr [gLeeyesOriginalStateNavigation]
	}
}
extern "C" DWORD gLeeyesStateShadowPostThunk = 0;
extern "C" __declspec(naked) void LeeyesStateShadowPostHook()
{
	__asm
	{
		pushfd
		pushad
		mov ecx, dword ptr [esp + 12]
		push dword ptr [ecx + 20]
		call LeeyesStateShadowPosted
		add esp, 4
		popad
		popfd
		jmp dword ptr [gLeeyesStateShadowPostThunk]
	}
}
extern "C" void (__cdecl *gLeeyesInternalOriginalListSortProbe)() = nullptr;
__declspec(thread) LARGE_INTEGER gListSortStart = {};
extern "C" void __cdecl LeeyesListSortBegin() { ::QueryPerformanceCounter(&gListSortStart); }
extern "C" void __cdecl LeeyesListSortEnd()
{
	LARGE_INTEGER end = {}, frequency = {};
	if( ::QueryPerformanceCounter(&end) && ::QueryPerformanceFrequency(&frequency)
		&& frequency.QuadPart > 0 && end.QuadPart >= gListSortStart.QuadPart )
	{
		const unsigned long long micros = static_cast<unsigned long long>(
			(end.QuadPart - gListSortStart.QuadPart) * 1000000LL / frequency.QuadPart);
		LeeyesTreeRescanObserve(0, static_cast<DWORD>(micros > MAXDWORD ? MAXDWORD : micros), 0xF000000C);
	}
}
extern "C" __declspec(naked) void LeeyesListSortProbeHook()
{
	__asm
	{
		pushfd
		pushad
		call LeeyesListSortBegin
		popad
		popfd
		call dword ptr [gLeeyesInternalOriginalListSortProbe]
		pushfd
		pushad
		call LeeyesListSortEnd
		popad
		popfd
		ret
	}
}
extern "C" void (__cdecl *gLeeyesInternalTreeDirectoryRescan)() = nullptr;
extern "C" void __cdecl LeeyesTreeRescanObserve(DWORD owner, DWORD node, DWORD caller);
bool ObserveTreeNode(DWORD node, TreeNodeObservation& state);
bool CopyShadowPath(DWORD pointer, char* path, DWORD& length);
extern "C" DWORD gTreeShellApplicationAddress = 0;
extern "C" DWORD gTreeShellOwnerContinuation = 0;
extern "C" DWORD gTreeShellEnumContinuation = 0;
__declspec(thread) bool gTreeSilentRescan = false;
__declspec(thread) HRESULT gTreeShellEnumFailure = S_OK;
bool gTreeSilentHooksReady = false;
extern "C" BOOL __cdecl LeeyesTreeUseSilentOwner() { return gTreeSilentRescan ? TRUE : FALSE; }
extern "C" void __cdecl LeeyesTreeObserveShellResult(HRESULT result)
{
	if( gTreeSilentRescan && FAILED(result) && SUCCEEDED(gTreeShellEnumFailure) )
		gTreeShellEnumFailure = result;
}
extern "C" __declspec(naked) void LeeyesTreeShellEnumHook()
{
	__asm
	{
		mov eax, dword ptr [eax]
		call dword ptr [eax + 10h]
		pushfd
		pushad
		push eax
		call LeeyesTreeObserveShellResult
		add esp, 4
		popad
		popfd
		jmp dword ptr [gTreeShellEnumContinuation]
	}
}
extern "C" __declspec(naked) void LeeyesTreeShellOwnerHook()
{
	__asm
	{
		pushfd
		pushad
		call LeeyesTreeUseSilentOwner
		test eax, eax
		jnz silent_owner
		popad
		popfd
		mov eax, dword ptr [gTreeShellApplicationAddress]
		mov eax, dword ptr [eax]
		mov eax, dword ptr [eax]
		mov eax, dword ptr [eax + 30h]
		jmp dword ptr [gTreeShellOwnerContinuation]
	silent_owner:
		popad
		popfd
		mov eax, 0
		jmp dword ptr [gTreeShellOwnerContinuation]
	}
}
namespace
{
const UINT kTreeRescanDiagnosticMessage = WM_APP + 0x5A3;
struct TreeStateProbe
{
	struct ChildTarget { HTREEITEM item, parent; DWORD node; TreeNodeObservation observation; std::string path; };
	struct DirectChange { DWORD action; std::string path; HTREEITEM item; HTREEITEM parent; DWORD node; TreeNodeObservation observation; };
	DWORD owner = 0, action = 0;
	HWND window = nullptr;
	HTREEITEM parent = nullptr;
	HTREEITEM parent2 = nullptr;
	DWORD parentNode = 0;
	DWORD parentNode2 = 0;
	TreeNodeObservation parentObservation = {};
	TreeNodeObservation parentObservation2 = {};
	void* first = nullptr;
	void* second = nullptr;
	std::wstring parentPath, parentPath2, leaf;
	StateSyncCoordinator::Candidate candidate1 = {}, candidate2 = {};
	ULONGLONG deadline = 0, testComparisonHoldUntil = 0;
	bool running = false, rescanning = false, batchMode = false, unsupportedEntry = false;
	bool restoringFixtureExpansion = false, restoreMessagePending = false, injectedRestoreSelection = false;
	HTREEITEM restoreMessageTarget = nullptr;
	bool bothMode = false, haveCandidate1 = false, haveCandidate2 = false;
	std::vector<ChildTarget> children;
	size_t nextChild = 0;
	unsigned long long childGeneration = 0;
	DWORD childJournalSize = 0;
	bool refreshingChildren = false;
	std::vector<DirectChange> directChanges;
	bool orderedPrefix = false;
	bool orderedWaitUsed = false;
	bool identityShadowEligible = false;
	bool identityShadowCompared = false;
	bool identityShadowComparable = false, identityShadowReplacement = false;
	bool identityShadowCarry = false;
	size_t nextIdentityExpansion = 0;
	ULONGLONG orderedWaitUntil = 0;
	size_t nextDirectChange = 0;
	bool applyingDirectChanges = false;
	bool injectedDirectPost = false;
	bool signalledDirectEvent = false;
	bool signalledChildEvent = false;
	bool injectedChildPost = false;
	WatchedSyncWorker::Candidate watchedCandidate1 = {}, watchedCandidate2 = {};
	bool watched1 = false, watched2 = false, watchedCompared = false;
};
TreeStateProbe gTreeStateProbe;
// Diagnostic evidence only: neither retained IDs nor a name match establishes
// that a previously displayed tree node represented that filesystem object.
struct TreeIdentityShadow
{
	DWORD owner = 0; HWND window = nullptr;
	std::wstring path1, path2;
	HTREEITEM parent1 = nullptr, parent2 = nullptr;
	DWORD parentNode1 = 0, parentNode2 = 0;
	TreeNodeObservation parentState1 = {}, parentState2 = {};
	DirectoryEntryIdentity::State ids1, ids2;
	struct Expanded { unsigned parent; std::wstring name; DWORD volume; ULONGLONG id; };
	struct Node { unsigned parent; std::wstring name; DWORD node; TreeNodeObservation observation; };
	std::vector<Expanded> expanded;
	std::vector<Node> nodes;
	unsigned long long inputRevision = 0;
	unsigned long long structureRevision = 0;
	unsigned long long lifetimeRevision = 0;
	bool valid = false;
};
TreeIdentityShadow gTreeIdentityShadow;
struct TreeIdentityCapture
{
	TreeIdentityShadow next;
	StateSyncCoordinator::Candidate ticket1, ticket2;
	std::map<std::pair<unsigned, std::wstring>, TreeIdentityShadow::Node> previous;
	HTREEITEM cursor = nullptr;
	unsigned parent = 0, count[2] = { 0, 0 }, slices = 0;
	unsigned refreshedChildren = 0;
	unsigned long long inputRevision = 0, structureRevision = 0, lifetimeRevision = 0, postedGeneration = 0;
	ULONGLONG maxSliceMs = 0, deadline = 0;
	bool active = false, running = false, carry = false;
};
TreeIdentityCapture gTreeIdentityCapture;
struct TreeExpansionIntent
{
	DWORD owner = 0; HWND window = nullptr; std::wstring path1, path2;
	DWORD rootVolume1 = 0, rootVolume2 = 0; ULONGLONG rootId1 = 0, rootId2 = 0;
	std::vector<TreeIdentityShadow::Expanded> targets;
	unsigned long long inputRevision = 0; bool valid = false;
};
TreeExpansionIntent gTreeExpansionIntent;
void InvalidateTreeIdentityComparison(TreeStateProbe& probe)
{
	if( probe.identityShadowCompared ) LeeyesTreeRescanObserve(probe.owner, 1, 0xF0000076);
	probe.identityShadowCompared = false;
	probe.identityShadowComparable = false;
	probe.identityShadowReplacement = false;
	probe.identityShadowCarry = false;
	gTreeExpansionIntent = TreeExpansionIntent();
}
// Experimental parent expansion intent, retained across a series of replay
// batches. A name alone is not sufficient: use only with matching root IDs
// and uninterrupted observation of tree input.
struct TreeParentExpansionIntent
{
	DWORD owner = 0; HWND window = nullptr; std::wstring path1, path2;
	DWORD volume1 = 0, volume2 = 0; ULONGLONG id1 = 0, id2 = 0;
	HTREEITEM parent1 = nullptr, parent2 = nullptr;
	unsigned long long inputRevision = 0, generation = 0; DWORD expandedMask = 0; bool valid = false;
};
TreeParentExpansionIntent gTreeParentExpansionIntent;
unsigned long long gTreeParentExpansionGeneration = 0;
HTREEITEM gTreeIdentityOwnExpansion = nullptr;
const void* gTreeIdentityOwnStateMark = nullptr;
unsigned long long gTreeIdentityInputRevision = 0;
unsigned long long gTreeIdentityStructureRevision = 0;
unsigned long long gTreeIdentityLifetimeRevision = 0;
bool TreeSetItemMayChangeExpansion(LPARAM lParam)
{
	__try
	{
		const TVITEMW* item = reinterpret_cast<const TVITEMW*>(lParam);
		return !item || ((item->mask & TVIF_STATE) && (item->stateMask & TVIS_EXPANDED));
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return true; }
}
DWORD TreeSetItemTargetClass(LPARAM lParam, HTREEITEM parent1, HTREEITEM parent2)
{
	__try
	{
		const TVITEMW* item = reinterpret_cast<const TVITEMW*>(lParam);
		if( !item || !item->hItem ) return 0;
		return item->hItem == parent1 ? 1 : item->hItem == parent2 ? 2 : 3;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return 0; }
}
bool TreeSetItemExpansionValue(LPARAM lParam, HTREEITEM& itemHandle, bool& expanded)
{
	__try
	{
		const TVITEMW* item = reinterpret_cast<const TVITEMW*>(lParam);
		if( !item || !(item->mask & TVIF_STATE) || !(item->stateMask & TVIS_EXPANDED) || !item->hItem ) return false;
		itemHandle = item->hItem; expanded = (item->state & TVIS_EXPANDED) != 0; return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}
DWORD TreeSetItemStateMask(LPARAM lParam)
{
	__try { const TVITEMW* item = reinterpret_cast<const TVITEMW*>(lParam); return item ? item->stateMask : MAXDWORD; }
	__except( EXCEPTION_EXECUTE_HANDLER ) { return MAXDWORD; }
}
LRESULT CALLBACK TreeIdentityInputSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference)
{
	// Any ambiguous tree interaction invalidates an expansion intent. This
	// also treats programmatic selection/expansion as external; false positives
	// are safe and must not be silently relaxed for a diagnostic speedup.
	const bool ownExpand = message == TVM_EXPAND && wParam == TVE_EXPAND
		&& reinterpret_cast<HTREEITEM>(lParam) == gTreeIdentityOwnExpansion;
	if( ownExpand ) gTreeIdentityOwnExpansion = nullptr;
	// Unknown TreeView commands invalidate the parent cache. Read-only tree
	// queries used by the probes are explicitly exempted.
	if( message >= TV_FIRST && message < TV_FIRST + 100
		&& message != TVM_GETITEMA && message != TVM_GETITEMW && message != TVM_GETNEXTITEM
		&& message != TVM_GETCOUNT && message != TVM_GETITEMSTATE && message != TVM_GETITEMRECT
		&& message != TVM_GETVISIBLECOUNT && message != TVM_GETINDENT && message != TVM_GETIMAGELIST
		&& message != TVM_GETEDITCONTROL
		&& message != TVM_GETISEARCHSTRINGA && message != TVM_GETISEARCHSTRINGW
		&& message != TVM_GETITEMHEIGHT && message != TVM_GETBKCOLOR && message != TVM_GETTEXTCOLOR
		&& message != TVM_GETLINECOLOR && message != TVM_GETSCROLLTIME && message != TVM_GETINSERTMARKCOLOR )
	{
		const bool firstChange = gTreeIdentityShadow.valid && gTreeIdentityShadow.structureRevision == gTreeIdentityStructureRevision;
		++gTreeIdentityStructureRevision;
		// SetItem can change a label or node data but does not itself destroy a
		// TreeView item. Carry still re-reads every name and node observation.
		// Insert/delete and unknown commands invalidate lifetime evidence.
		if( message != TVM_SETITEMA && message != TVM_SETITEMW ) ++gTreeIdentityLifetimeRevision;
		if( firstChange && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_CACHE_TRACE", nullptr, 0) )
			LeeyesTreeRescanObserve(gTreeIdentityShadow.owner, message, 0xF0000066);
	}
	if( (!ownExpand && message == TVM_EXPAND) || message == TVM_SELECTITEM || message == WM_LBUTTONDOWN
		|| message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN
		|| message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_CHAR
		|| message == WM_MOUSEWHEEL || message == WM_HSCROLL || message == WM_VSCROLL )
	{
		++gTreeIdentityInputRevision;
		gTreeExpansionIntent.valid = false;
		if( gTreeParentExpansionIntent.valid ) LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner, message, 0xF000006C);
		gTreeParentExpansionIntent.valid = false;
	}
	if( message == TVM_SETITEMA || message == TVM_SETITEMW )
	{
		const bool ownMark = reinterpret_cast<const void*>(lParam) == gTreeIdentityOwnStateMark && gTreeIdentityOwnStateMark;
		if( ownMark ) gTreeIdentityOwnStateMark = nullptr;
		if( !ownMark && TreeSetItemMayChangeExpansion(lParam) )
		{
			if( gTreeParentExpansionIntent.valid )
			{
				LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner, message, 0xF000006C);
				if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_EXPANSION_TRACE", nullptr, 0) )
				{
					LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner,
						TreeSetItemTargetClass(lParam, gTreeParentExpansionIntent.parent1, gTreeParentExpansionIntent.parent2), 0xF0000070);
					LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner, TreeSetItemStateMask(lParam), 0xF0000072);
					LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner, ::InSendMessageEx(nullptr), 0xF0000073);
					HTREEITEM target = nullptr; bool expanded = false;
					if( TreeSetItemExpansionValue(lParam, target, expanded) )
					{
						TVITEMW current = {}; current.mask = TVIF_STATE; current.hItem = target; current.stateMask = TVIS_EXPANDED;
						const bool exists = ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&current)) != FALSE;
						LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner,
							(exists ? 1u : 0u) | ((current.state & TVIS_EXPANDED) ? 2u : 0u) | (expanded ? 4u : 0u), 0xF0000071);
					}
					LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner, LeeyesDiagnosticTreeReplayActive() ? 1 : 0, 0xF000006E);
					void* frames[16] = {};
					const USHORT count = ::CaptureStackBackTrace(0, _countof(frames), frames, nullptr);
					for( USHORT i = 0; i < count; ++i ) LeeyesTreeRescanObserve(gTreeParentExpansionIntent.owner, reinterpret_cast<DWORD>(frames[i]), 0xF000006F);
				}
			}
			gTreeParentExpansionIntent.valid = false;
		}
	}
	if( message == WM_NCDESTROY )
	{
		++gTreeIdentityInputRevision;
		++gTreeIdentityStructureRevision;
		++gTreeIdentityLifetimeRevision;
		gTreeExpansionIntent = TreeExpansionIntent();
		gTreeParentExpansionIntent = TreeParentExpansionIntent();
		gTreeIdentityShadow = TreeIdentityShadow();
		::RemoveWindowSubclass(window, TreeIdentityInputSubclass, subclassId);
	}
	return ::DefSubclassProc(window, message, wParam, lParam);
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeParentIntentMatches(DWORD owner, HWND window, const wchar_t* first, const wchar_t* second,
	DWORD volume1, ULONGLONG id1, DWORD volume2, ULONGLONG id2, BOOL requireCandidate, DWORD* mask, ULONGLONG* generation)
{
	if( !mask || !generation || !first || !second || !gTreeParentExpansionIntent.valid || !id1 || !id2 ) return FALSE;
	const auto& intent = gTreeParentExpansionIntent;
	if( intent.owner != owner || intent.window != window || !::IsWindow(window)
		|| intent.path1 != first || intent.path2 != second || intent.inputRevision != gTreeIdentityInputRevision
		|| intent.volume1 != volume1 || intent.id1 != id1 || intent.volume2 != volume2 || intent.id2 != id2 ) return FALSE;
	if( requireCandidate )
	{
		DiagnosticTreeWorker* w1 = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		DiagnosticTreeWorker* w2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
		if( !gTreeStateProbe.batchMode || !gTreeStateProbe.bothMode || gTreeStateProbe.owner != owner
			|| gTreeStateProbe.window != window || !gTreeStateProbe.haveCandidate1 || !gTreeStateProbe.haveCandidate2
			|| !w1 || !w2 || !w1->CanApply(gTreeStateProbe.candidate1) || !w2->CanApply(gTreeStateProbe.candidate2)
			|| gTreeStateProbe.childGeneration != LeeyesDiagnosticTreePostedGeneration()
			|| gTreeStateProbe.candidate1.state.volume != volume1 || gTreeStateProbe.candidate1.state.directoryId != id1
			|| gTreeStateProbe.candidate2.state.volume != volume2 || gTreeStateProbe.candidate2.state.directoryId != id2 ) return FALSE;
	}
	*mask = intent.expandedMask; *generation = intent.generation; return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeTestReplaceParentIntentGeneration()
{
	if( !gTreeParentExpansionIntent.valid ) return FALSE;
	gTreeParentExpansionIntent.generation = ++gTreeParentExpansionGeneration; return TRUE;
}
bool DiagnosticOwnerContainsWindow(DWORD owner, HWND window);
bool StoreTreeStateProbe(DWORD owner, HWND window, DWORD action, DWORD first, DWORD second);
LRESULT CALLBACK RescanDiagnosticSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
bool TreeStateChildVisible(HWND window, HTREEITEM parent, const std::wstring& name, bool& visible)
{
	visible = false;
	HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD,
		reinterpret_cast<LPARAM>(parent)));
	for( unsigned count = 0; child && count < 100000; ++count )
	{
		wchar_t text[1024] = {};
		TVITEMW item = {}; item.mask = TVIF_TEXT; item.hItem = child;
		item.pszText = text; item.cchTextMax = _countof(text);
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) return false;
		if( name == text ) { if( visible ) return false; visible = true; }
		child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT,
			reinterpret_cast<LPARAM>(child)));
	}
	return child == nullptr;
}
bool TreeParentAnsiPath(const std::wstring& path, std::string& result)
{
	if( path.empty() ) return false;
	try
	{
		if( !::GetEnvironmentVariableA("LEYEES_TREE_STATE_PARENT_CP932", nullptr, 0) )
		{
			result.clear();
			for( wchar_t c : path ) { if( c > 0x7f ) return false; result.push_back(static_cast<char>(c)); }
			return true;
		}
		BOOL substituted = FALSE;
		const int size = ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, path.c_str(), static_cast<int>(path.size()), nullptr, 0, nullptr, &substituted);
		if( size <= 0 || substituted ) return false;
		result.assign(size, '\0'); substituted = FALSE;
		if( ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, path.c_str(), static_cast<int>(path.size()), &result[0], size, nullptr, &substituted) != size || substituted ) return false;
		const int units = ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, result.data(), size, nullptr, 0);
		if( units <= 0 ) return false;
		std::wstring roundtrip(units, L'\0');
		return ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, result.data(), size, &roundtrip[0], units) == units && roundtrip == path;
	}
	catch( ... ) { return false; }
}
bool TreeDiagnosticResolveCachedPath(DWORD data, DWORD vmt, const char* expected, DWORD expectedLength, DWORD& detail);
bool TreeFixtureParentResolveAllowed(const std::wstring& path)
{
	const std::wstring suffix = L"\\diagnostic\\state_sync\\tree_fixture_aclretry_i0d_0k_s\\folder_destination";
	return ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RESOLVE_REPLACED_PARENT", nullptr, 0)
		&& path.size() > suffix.size() && !_wcsicmp(path.c_str() + path.size() - suffix.size(), suffix.c_str());
}
bool TreeStateCachedPathMatches(const TreeNodeObservation& state, const std::wstring& path)
{
	char observed[1040] = {};
	DWORD observedLength = 0;
	std::string expected;
	if( !TreeParentAnsiPath(path, expected) || expected.size() >= _countof(observed) ) return false;
	if( !state.cachedPath && TreeFixtureParentResolveAllowed(path) )
	{
		DWORD detail = 0;
		return TreeDiagnosticResolveCachedPath(state.data, state.vmt, expected.data(), static_cast<DWORD>(expected.size()), detail);
	}
	if( !state.cachedPath || !CopyShadowPath(state.cachedPath, observed, observedLength) ) return false;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_PARENT_CP932", nullptr, 0) ) return expected == observed;
	return _stricmp(expected.c_str(), observed) == 0;
}
bool TreeDiagnosticFixtureImage(const DirectorySnapshot::Entry& entry)
{
	if( entry.attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
	return (::GetEnvironmentVariableA("LEYEES_TREE_TEST_SELECTED_FIXTURE_IMAGES", nullptr, 0)
			&& (entry.name == L"__selected_image_0.bmp" || entry.name == L"__selected_image_1.bmp"
				|| (::GetEnvironmentVariableA("LEYEES_TREE_TEST_RENAME_IMAGE_DURING", nullptr, 0)
					&& entry.name == L"__selected_image_renamed.bmp")))
		|| (::GetEnvironmentVariableA("LEYEES_TREE_TEST_SELECTED_FIXTURE_ZIP", nullptr, 0)
			&& entry.name == L"__selected_archive.zip");
}
// A collapsed fixture item with no materialized children is not an empty
// filesystem directory. Leave it lazy only while its expander advertises
// the known nonempty directory; normal expansion must enumerate it later.
bool TreeDiagnosticLazyFixtureParent(HWND window, HTREEITEM parent, const std::wstring& path,
	const DirectorySnapshot::State& state)
{
	if( !::GetEnvironmentVariableA("LEYEES_TREE_TEST_LAZY_REPLACED_PARENT", nullptr, 0)
		|| !LeeyesDiagnosticTreeRecoveryReadoptFixture() || !TreeFixtureParentResolveAllowed(path) || !parent ) return false;
	for( const auto& entry : state.entries )
		if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) ) return false;
	TVITEMW item = {}; item.mask = TVIF_STATE | TVIF_CHILDREN; item.hItem = parent; item.stateMask = TVIS_EXPANDED;
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || (item.state & TVIS_EXPANDED)
		|| ::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent))
		|| (!state.entries.empty() && !item.cChildren) ) return false;
	LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(item.cChildren), 0xF0000051);
	return true;
}
// Other shell item types remain rejected; only explicitly named fixture files
// can be ignored for a selected-list diagnostic.
bool TreeStateFixtureMatches(HWND window, HTREEITEM parent, const DirectorySnapshot::State& state, bool& matches,
	size_t* differences = nullptr)
{
	matches = false;
	std::vector<std::wstring> expected, visible;
	try
	{
		const std::wstring& path = parent == gTreeStateProbe.parent2 ? gTreeStateProbe.parentPath2 : gTreeStateProbe.parentPath;
		if( TreeDiagnosticLazyFixtureParent(window, parent, path, state) )
		{ matches = true; if( differences ) *differences = 0; return true; }
		for( const auto& entry : state.entries )
		{
			if( TreeDiagnosticFixtureImage(entry) ) continue;
			if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) )
			{
				gTreeStateProbe.unsupportedEntry = true;
				LeeyesTreeRescanObserve(gTreeStateProbe.owner,
					(entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) ? 2 : 1, 0xF0000018);
				return false;
			}
			expected.push_back(entry.name);
		}
		HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD,
			reinterpret_cast<LPARAM>(parent)));
		for( unsigned count = 0; child && count < 100000; ++count )
		{
			wchar_t text[1024] = {};
			TVITEMW item = {}; item.mask = TVIF_TEXT; item.hItem = child;
			item.pszText = text; item.cchTextMax = _countof(text);
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) return false;
			visible.push_back(text);
			child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT,
				reinterpret_cast<LPARAM>(child)));
		}
		if( child ) return false;
		std::sort(visible.begin(), visible.end());
		std::sort(expected.begin(), expected.end());
		matches = visible == expected;
		if( differences )
		{
			size_t left = 0, right = 0, count = 0;
			while( left < visible.size() && right < expected.size() )
			{
				if( visible[left] < expected[right] ) { ++left; ++count; }
				else if( expected[right] < visible[left] ) { ++right; ++count; }
				else { ++left; ++right; }
			}
			*differences = count + (visible.size() - left) + (expected.size() - right);
		}
		return true;
	}
	catch( ... ) { return false; }
}
struct TreeOrdinalCaseLess
{
	bool operator()(const std::wstring& a, const std::wstring& b) const
	{ return ::CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN; }
};
bool TransientTreePathsAbsent(HWND window, const std::vector<std::string>& paths)
{
	try
	{
		using Names = std::set<std::wstring, TreeOrdinalCaseLess>;
		Names expected[2], visible[2], transient[2];
		for( unsigned i = 0; i < 2; ++i )
		{
			const auto& state = i ? gTreeStateProbe.candidate2.state : gTreeStateProbe.candidate1.state;
			const HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
			for( const auto& entry : state.entries )
				if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT)
					|| !expected[i].insert(entry.name).second ) return false;
			HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent)));
			for( unsigned count = 0; child; ++count )
			{
				if( count >= 100000 ) return false;
				wchar_t text[1024] = {}; TVITEMW item = {}; item.mask = TVIF_TEXT; item.hItem = child;
				item.pszText = text; item.cchTextMax = _countof(text);
				if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !*text || !visible[i].insert(text).second ) return false;
				child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
			}
		}
		for( const std::string& path : paths )
		{
			const int units = ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
			if( units <= 0 ) return false;
			std::wstring wide(units, L'\0');
			if( ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), &wide[0], units) != units ) return false;
			BOOL substituted = FALSE;
			const int bytes = ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide.data(), units, nullptr, 0, nullptr, &substituted);
			if( bytes <= 0 || substituted ) return false;
			std::string roundtrip(bytes, '\0'); substituted = FALSE;
			if( ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide.data(), units, &roundtrip[0], bytes, nullptr, &substituted) != bytes
				|| substituted || roundtrip != path ) return false;
			const size_t separator = wide.find_last_of(L'\\');
			if( separator == std::wstring::npos || separator + 1 == wide.size() ) return false;
			const std::wstring parent = wide.substr(0, separator), leaf = wide.substr(separator + 1);
			if( leaf == L"." || leaf == L".." || leaf.find(L'/') != std::wstring::npos || leaf.compare(0, 2, L"~u") == 0 ) return false;
			const bool first = parent == gTreeStateProbe.parentPath, second = parent == gTreeStateProbe.parentPath2;
			if( first == second ) return false;
			const unsigned index = second ? 1 : 0;
			if( expected[index].find(leaf) != expected[index].end() || visible[index].find(leaf) != visible[index].end()
				|| !transient[index].insert(leaf).second ) return false;
		}
		return !paths.empty();
	}
	catch( ... ) { return false; }
}
// Diagnostic adapter for direct fixture directories only. This maps a
// current name difference to the original tree callback; it does not prove
// equivalence for arbitrary shell items, renames, aliases, or callback effects.
bool CollectTreeDirectChanges(HWND window, std::vector<TreeStateProbe::DirectChange>& output)
{
	try
	{
		std::vector<TreeStateProbe::DirectChange> removed, added;
		for( unsigned i = 0; i < 2; ++i )
		{
			const std::wstring& parentPath = i ? gTreeStateProbe.parentPath2 : gTreeStateProbe.parentPath;
			const HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
			const DirectorySnapshot::State& state = i ? gTreeStateProbe.candidate2.state : gTreeStateProbe.candidate1.state;
			if( TreeDiagnosticLazyFixtureParent(window, parent, parentPath, state) ) continue;
			std::string prefix;
			if( !TreeParentAnsiPath(parentPath, prefix) ) return false;
			if( prefix.empty() ) return false;
			// A CP932 trail byte can itself be 0x5c. Test the UTF-16 path,
			// not the last encoded byte, before appending a separator.
			if( parentPath.back() != L'\\' ) prefix.push_back('\\');
			using Names = std::map<std::wstring, std::string, TreeOrdinalCaseLess>;
			Names expected, visible;
			struct ItemIdentity { HTREEITEM item; DWORD node; TreeNodeObservation observation; };
			std::map<std::wstring, ItemIdentity, TreeOrdinalCaseLess> visibleItems;
			const bool cp932 = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DIFF_CP932", nullptr, 0) != 0;
			auto insertName = [cp932](Names& names, const wchar_t* text) -> bool {
				const std::wstring wide(text);
				if( wide.empty() || wide.find_first_of(L"\\/") != std::wstring::npos ) return false;
				if( !cp932 ) for( wchar_t c : wide ) if( c > 0x7f ) return false;
				BOOL substituted = FALSE;
				const int bytes = ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &substituted);
				if( bytes <= 0 || substituted ) return false;
				std::string ansi(bytes, '\0'); substituted = FALSE;
				if( ::WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, wide.c_str(), static_cast<int>(wide.size()), &ansi[0], bytes, nullptr, &substituted) != bytes
					|| substituted || ansi.compare(0, 2, "~u") == 0 ) return false;
				const int units = ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, ansi.data(), bytes, nullptr, 0);
				if( units <= 0 ) return false;
				std::wstring roundtrip(units, L'\0');
				if( ::MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, ansi.data(), bytes, &roundtrip[0], units) != units || roundtrip != wide ) return false;
				return names.emplace(wide, ansi).second;
			};
			for( const auto& entry : state.entries )
			{
				if( TreeDiagnosticFixtureImage(entry) ) continue;
				if( !(entry.attributes & FILE_ATTRIBUTE_DIRECTORY) || (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT)
					|| !insertName(expected, entry.name.c_str()) ) return false;
			}
			HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent)));
			for( unsigned count = 0; child; ++count )
			{
				if( count >= 100000 ) return false;
				wchar_t text[1024] = {}; TVITEMW item = {}; item.mask = TVIF_TEXT | TVIF_PARAM; item.hItem = child;
				item.pszText = text; item.cchTextMax = _countof(text);
				if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !item.lParam || !insertName(visible, text) ) return false;
				ItemIdentity identity = {}; identity.item = child; identity.node = static_cast<DWORD>(item.lParam);
				if( !ObserveTreeNode(identity.node, identity.observation) || !visibleItems.emplace(text, identity).second ) return false;
				child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
			}
			for( const auto& item : visible )
			{
				auto found = expected.find(item.first);
				if( found == expected.end() )
				{
					const auto identity = visibleItems.find(item.first);
					if( identity == visibleItems.end() ) return false;
					removed.push_back({ 5, prefix + item.second, identity->second.item, parent, identity->second.node, identity->second.observation });
				}
				else if( found->first != item.first ) return false;
			}
			for( const auto& item : expected )
				if( visible.find(item.first) == visible.end() ) added.push_back({ 3, prefix + item.second, nullptr, parent, 0, {} });
		}
		removed.insert(removed.end(), added.begin(), added.end());
		if( removed.size() > 4096 ) return false;
		output.swap(removed); return true;
	}
	catch( ... ) { return false; }
}
bool TreeDirectChangeSafe(HWND window, const TreeStateProbe::DirectChange& change)
{
	if( (change.action == 0 || change.action == 2) && gTreeStateProbe.orderedPrefix
		&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_PASSTHROUGH", nullptr, 0) ) return true;
	if( change.action == 3 ) return true;
	if( change.action != 5 || !change.item || !change.node || !change.parent ) return false;
	TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = change.item;
	TreeNodeObservation currentNode = {};
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || static_cast<DWORD>(item.lParam) != change.node
		|| !ObserveTreeNode(change.node, currentNode) || memcmp(&currentNode, &change.observation, sizeof(currentNode)) != 0
		|| reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(change.item))) != change.parent ) return false;
	HTREEITEM current = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CARET, 0));
	for( unsigned depth = 0; current && depth < 1024; ++depth )
	{
		if( current == change.item )
			return ::GetEnvironmentVariableA("LEYEES_TREE_DIFF_SELECTED_PASS", nullptr, 0) != 0;
		current = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(current)));
	}
	return current == nullptr;
}
bool TreeDirectSelectionSafe(HWND window, const std::vector<TreeStateProbe::DirectChange>& changes)
{
	HTREEITEM current = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CARET, 0));
	for( unsigned depth = 0; current && depth < 1024; ++depth )
	{
		for( const auto& change : changes ) if( change.action == 5 && current == change.item )
			return ::GetEnvironmentVariableA("LEYEES_TREE_DIFF_SELECTED_PASS", nullptr, 0) != 0;
		current = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(current)));
	}
	return current == nullptr;
}
bool DirectChangesCoverJournal(const std::vector<TreeStateProbe::DirectChange>& changes)
{
	try
	{
		std::vector<std::string> paths;
		for( const auto& change : changes ) paths.push_back(change.path);
		return LeeyesDiagnosticTreeBatchPathsCovered(&paths) != FALSE;
	}
	catch( ... ) { return false; }
}
bool OrderTreeDirectChanges(std::vector<TreeStateProbe::DirectChange>& changes)
{
	try
	{
		std::vector<std::pair<unsigned, std::string>> paths; paths.reserve(changes.size());
		for( const auto& change : changes ) paths.emplace_back(change.action, change.path);
		std::vector<size_t> order;
		std::vector<std::pair<unsigned, std::string>> originals;
		const bool passthrough = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_PASSTHROUGH", nullptr, 0) != 0;
		if( !LeeyesDiagnosticTreeOrderChanges(&paths, &order, passthrough ? &originals : nullptr)
			|| (passthrough ? order.size() != originals.size() : order.size() != changes.size()) ) return false;
		std::vector<TreeStateProbe::DirectChange> sorted; sorted.reserve(order.size());
		for( size_t i = 0; i < order.size(); ++i )
		{
			const size_t index = order[i];
			if( passthrough && index == changes.size() )
			{
				TreeStateProbe::DirectChange change = {};
				change.action = originals[i].first; change.path = originals[i].second;
				sorted.push_back(change);
			}
			else { if( index >= changes.size() ) return false; sorted.push_back(changes[index]); }
		}
		changes.swap(sorted);
		return true;
	}
	catch( ... ) { return false; }
}
// Accept only paths that TreeParentAnsiPath can round-trip, and only when
// both parent name sets already agree. A one-sided event can still indicate replacement.
bool CollectTreeStateChildren(HWND window, std::vector<TreeStateProbe::ChildTarget>& result,
	const std::vector<TreeStateProbe::DirectChange>* appliedChanges = nullptr)
{
	auto fail = [](DWORD reason) {
		if( LeeyesDiagnosticTreeRecoveryOnly() ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, reason, 0xF0000046);
		return false;
	};
	try
	{
		std::vector<std::string> paths;
		if( !LeeyesDiagnosticTreeBatchPaths(&paths) ) return fail(1);
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_CHILD_PATH_CASE", nullptr, 0) )
		{
			// Mutate only this temporary comparison input. The journal retains
			// the original bytes for ordered fallback replay.
			for( std::string& path : paths )
			{
				const size_t leaf = path.rfind("\\__folder_");
				if( leaf != std::string::npos ) path[leaf + 3] = 'F';
			}
		}
		std::map<std::string, TreeStateProbe::ChildTarget> visible;
		std::map<std::string, std::string> foldedVisible;
		auto foldAscii = [](std::string path) {
			for( size_t i = 0; i < path.size(); ++i )
			{
				if( ::IsDBCSLeadByteEx(932, static_cast<BYTE>(path[i])) && i + 1 < path.size() ) { ++i; continue; }
				if( path[i] >= 'A' && path[i] <= 'Z' ) path[i] += 'a' - 'A';
			}
			return path;
		};
		for( unsigned i = 0; i < 2; ++i )
		{
			const std::wstring& parentPath = i ? gTreeStateProbe.parentPath2 : gTreeStateProbe.parentPath;
			const HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
			const DirectorySnapshot::State& state = i ? gTreeStateProbe.candidate2.state : gTreeStateProbe.candidate1.state;
			if( TreeDiagnosticLazyFixtureParent(window, parent, parentPath, state) ) continue;
			std::string prefix;
			if( !TreeParentAnsiPath(parentPath, prefix) ) return fail(5);
			if( prefix.empty() ) return fail(6);
			if( parentPath.back() != L'\\' ) prefix.push_back('\\');
			HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent)));
			for( unsigned count = 0; child; ++count )
			{
				if( count >= 100000 ) return fail(7);
				wchar_t text[1024] = {};
				TVITEMW item = {}; item.mask = TVIF_TEXT | TVIF_PARAM; item.hItem = child;
				item.pszText = text; item.cchTextMax = _countof(text);
				if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !item.lParam ) return fail(2);
				std::wstring leaf;
				for( const wchar_t* p = text; *p; ++p )
				{
					if( *p == '\\' || *p == '/' )
					{
						if( LeeyesDiagnosticTreeRecoveryOnly() )
							LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(*p), 0xF0000048);
						return fail(8);
					}
					leaf.push_back(*p);
				}
				std::string encodedLeaf;
				if( !TreeParentAnsiPath(leaf, encodedLeaf) ) return fail(8);
				std::string key = prefix + encodedLeaf;
				TreeStateProbe::ChildTarget target = {}; target.item = child; target.parent = parent;
				target.node = static_cast<DWORD>(item.lParam); target.path = key;
				if( !ObserveTreeNode(target.node, target.observation) || !visible.emplace(key, target).second
					|| !foldedVisible.emplace(foldAscii(key), key).second ) return fail(3);
				child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
			}
		}
		std::set<std::string> applied;
		std::set<std::string> added;
		std::map<std::string, std::string> foldedApplied;
		if( appliedChanges ) for( const auto& change : *appliedChanges )
		{
			const auto entry = foldedApplied.emplace(foldAscii(change.path), change.path);
			if( !entry.second && entry.first->second != change.path ) return fail(9);
			applied.insert(change.path);
			if( change.action == 3 ) added.insert(change.path);
		}
		std::vector<TreeStateProbe::ChildTarget> targets;
		if( LeeyesDiagnosticTreeRecoveryOnly() )
		{
			// Existing names may represent replacements. The opt-in diagnostic
			// separately tests skipping nodes just constructed by this candidate.
			if( !paths.empty() ) return fail(4);
			const bool skipAdded = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_SKIP_RECOVERY_ADDED", nullptr, 0) != 0;
			unsigned skipped = 0;
			for( const auto& entry : visible )
			{
				const std::string suffix = "\\__folder_000";
				const bool expansionFixture = LeeyesDiagnosticTreeRestoreFixtureChildRequested()
					&& entry.first.size() >= suffix.size()
					&& entry.first.compare(entry.first.size() - suffix.size(), suffix.size(), suffix) == 0;
				if( skipAdded && !expansionFixture && added.find(entry.first) != added.end() )
				{
					const std::string testSuffix = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_VERIFY_SKIPPED_LAST", nullptr, 0)
						? "\\__folder_999" : "\\__folder_001";
					if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_VERIFY_SKIPPED_CHILD", nullptr, 0)
						&& entry.first.size() >= testSuffix.size()
						&& entry.first.compare(entry.first.size() - testSuffix.size(), testSuffix.size(), testSuffix) == 0 )
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000004D);
					++skipped; continue;
				}
				targets.push_back(entry.second);
			}
			if( skipAdded ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, skipped, 0xF000004C);
		}
		for( std::string path : paths )
		{
			if( applied.find(path) != applied.end() ) continue;
			if( foldedApplied.find(foldAscii(path)) != foldedApplied.end() ) return false;
			auto found = visible.find(path);
			if( found == visible.end() && foldedVisible.find(foldAscii(path)) != foldedVisible.end() )
			{
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000035);
				return false;
			}
			if( found == visible.end() && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_NESTED_FIXTURE", nullptr, 0) )
			{
				const std::string suffix = "\\__late_probe";
				if( path.size() > suffix.size() && path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0 )
				{
					const std::string parent = path.substr(0, path.size() - suffix.size());
					found = visible.find(parent);
					if( found == visible.end() && foldedVisible.find(foldAscii(parent)) != foldedVisible.end() ) return false;
				}
			}
			if( found != visible.end() ) targets.push_back(found->second);
		}
		// A first diagnostic identity baseline must cover every visible child;
		// touched paths alone cannot establish the candidate-wide observation.
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_CONTINUITY", nullptr, 0) && !gTreeIdentityShadow.valid )
			for( const auto& entry : visible ) targets.push_back(entry.second);
		// A direct event and a nested event can name the same child. Refresh it
		// only once per candidate, after all retained paths have been examined.
		std::sort(targets.begin(), targets.end(), [](const TreeStateProbe::ChildTarget& a, const TreeStateProbe::ChildTarget& b) {
			return reinterpret_cast<ULONG_PTR>(a.item) < reinterpret_cast<ULONG_PTR>(b.item);
		});
		targets.erase(std::unique(targets.begin(), targets.end(), [](const TreeStateProbe::ChildTarget& a, const TreeStateProbe::ChildTarget& b) {
			return a.item == b.item;
		}), targets.end());
		result.swap(targets);
		return true;
	}
	catch( ... ) { return fail(10); }
}
// This diagnostic checks a candidate getter; it does not establish lifetime
// or authorize applying a stale snapshot.
bool TreeDiagnosticResolveCachedPath(DWORD data, DWORD vmt, const char* expected, DWORD expectedLength, DWORD& detail)
{
	detail = 0;
	void* value = nullptr;
	const DWORD method = 0x004bd470;
	const BYTE prefix[] = { 0x55,0x8b,0xec,0x33,0xc9,0x51,0x51,0x51,0x51,0x53,0x56,0x57,0x8b,0xf2,0x8b,0xd8,0x33,0xc0,0x55,0x68 };
	const BYTE suffix[] = { 0x64,0xff,0x30,0x64,0x89,0x20,0x83,0x7b,0x38,0x00 };
	bool invoked = false;
	__try
	{
		if( !data || !vmt || *reinterpret_cast<const DWORD*>(data) != vmt
			|| *reinterpret_cast<const DWORD*>(vmt) != method
			|| memcmp(reinterpret_cast<const void*>(method), prefix, sizeof(prefix))
			|| memcmp(reinterpret_cast<const void*>(method + 24), suffix, sizeof(suffix)) ) return false;
		detail |= 1;
		__asm {
			mov eax, data
			lea edx, value
			call method
		}
		invoked = true; detail |= 2;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { invoked = false; }
	char actual[1040] = {}; DWORD actualLength = 0;
	const bool valid = invoked && expected && value && CopyShadowPath(reinterpret_cast<DWORD>(value), actual, actualLength);
	if( valid ) detail |= 4;
	if( valid && actualLength == expectedLength ) detail |= 8;
	if( valid && actualLength == expectedLength && !_strnicmp(actual, expected, expectedLength) ) detail |= 16;
	const bool equal = valid && actualLength == expectedLength && !memcmp(actual, expected, expectedLength);
	if( value ) { __try { LeeyesDiagnosticClearAnsi(&value); } __except( EXCEPTION_EXECUTE_HANDLER ) {} }
	return equal;
}
bool TreeStateParentsValid(HWND window)
{
	if( !DiagnosticOwnerContainsWindow(gTreeStateProbe.owner, window) ) return false;
	for( unsigned i = 0; i < 2; ++i )
	{
		const HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
		const DWORD node = i ? gTreeStateProbe.parentNode2 : gTreeStateProbe.parentNode;
		const TreeNodeObservation& original = i ? gTreeStateProbe.parentObservation2 : gTreeStateProbe.parentObservation;
		TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = parent;
		TreeNodeObservation current = {};
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
			|| static_cast<DWORD>(item.lParam) != node || !ObserveTreeNode(node, current)
			|| memcmp(&current, &original, sizeof(current)) != 0 ) return false;
	}
	return true;
}
bool TreeDiagnosticRecheckResolvedChild(HWND window, const TreeStateProbe::ChildTarget& target)
{
	size_t slash = std::string::npos;
	for( size_t i = 0; i < target.path.size(); ++i )
	{
		if( ::IsDBCSLeadByteEx(932, static_cast<BYTE>(target.path[i])) && i + 1 < target.path.size() ) { ++i; continue; }
		if( target.path[i] == '\\' ) slash = i;
	}
	if( slash == std::string::npos || slash + 1 == target.path.size() || target.path.size() >= 1040 ) return false;
	DWORD hash = 2166136261u;
	for( unsigned char c : target.path ) hash = (hash ^ c) * 16777619u;
	if( !target.observation.cachedPath || target.observation.pathLength != target.path.size()
		|| target.observation.pathHash != hash ) return false;
	wchar_t text[1024] = {}; TVITEMW item = {}; item.mask = TVIF_TEXT | TVIF_PARAM;
	item.hItem = target.item; item.pszText = text; item.cchTextMax = _countof(text);
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
		|| static_cast<DWORD>(item.lParam) != target.node
		|| reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT,
			reinterpret_cast<LPARAM>(target.item))) != target.parent ) return false;
	std::string encodedLeaf;
	if( !TreeParentAnsiPath(text, encodedLeaf) || target.path.compare(slash + 1, std::string::npos, encodedLeaf) != 0 ) return false;
	TreeNodeObservation current = {};
	const TreeNodeObservation& old = target.observation;
	if( !ObserveTreeNode(target.node, current) || current.data != old.data || current.vmt != old.vmt
		|| current.part != old.part || current.full != old.full || current.parent != old.parent
		|| !current.cachedPath || current.pathLength != old.pathLength || current.pathHash != old.pathHash ) return false;
	char actual[1040] = {}; DWORD length = 0;
	return CopyShadowPath(current.cachedPath, actual, length) && length == target.path.size()
		&& !memcmp(actual, target.path.data(), length);
}
// Narrow diagnostic: resolve a fixture child by name afresh; never retain an
// HTREEITEM across callback replay. This is not general expansion recovery.
bool TreeDiagnosticFixtureChild(HWND window, HTREEITEM parent, HTREEITEM& found, bool& expanded)
{
	found = nullptr; expanded = false;
	HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent)));
	for( unsigned count = 0; child && count < 4096; ++count )
	{
		wchar_t text[128] = {};
		TVITEMW item = {}; item.mask = TVIF_TEXT | TVIF_STATE; item.hItem = child;
		item.pszText = text; item.cchTextMax = _countof(text); item.stateMask = TVIS_EXPANDED;
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) return false;
		if( !wcscmp(text, L"__folder_000") )
		{
			if( found ) return false;
			found = child; expanded = (item.state & TVIS_EXPANDED) != 0;
		}
		child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
	}
	return !child && found;
}
LRESULT CALLBACK TreeIdentityCaptureSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
bool TreeIdentityCaptureCurrent()
{
	TreeIdentityCapture& capture = gTreeIdentityCapture;
	DiagnosticTreeWorker* first = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
	DiagnosticTreeWorker* second = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
	return capture.active && ::GetTickCount64() < capture.deadline && capture.next.window && ::IsWindow(capture.next.window)
		&& capture.inputRevision == gTreeIdentityInputRevision && capture.structureRevision == gTreeIdentityStructureRevision
		&& capture.lifetimeRevision == gTreeIdentityLifetimeRevision && capture.postedGeneration == LeeyesDiagnosticTreePostedGeneration()
		&& first && second && first->IsCommittedCurrent(capture.ticket1) && second->IsCommittedCurrent(capture.ticket2);
}
void FinishTreeIdentityCapture(bool success)
{
	TreeIdentityCapture& capture = gTreeIdentityCapture;
	if( !capture.active ) return;
	const HWND window = capture.next.window; const DWORD owner = capture.next.owner;
	const unsigned slices = capture.slices; const ULONGLONG maxSlice = capture.maxSliceMs;
	const unsigned refreshed = capture.refreshedChildren; const bool carry = capture.carry;
	if( success ) success = TreeIdentityCaptureCurrent() && capture.previous.empty()
		&& capture.count[0] == capture.next.ids1.items.size() && capture.count[1] == capture.next.ids2.items.size();
	if( window )
	{
		::KillTimer(window, reinterpret_cast<UINT_PTR>(&gTreeIdentityCapture));
		::RemoveWindowSubclass(window, TreeIdentityCaptureSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityCapture));
	}
	if( success )
	{
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_EXPANSION_CARRY", nullptr, 0) )
		{
			try
			{
				if( gTreeParentExpansionIntent.valid && (gTreeParentExpansionIntent.owner != owner
					|| gTreeParentExpansionIntent.window != window || gTreeParentExpansionIntent.path1 != capture.next.path1
					|| gTreeParentExpansionIntent.path2 != capture.next.path2) ) gTreeParentExpansionIntent.valid = false;
				if( !gTreeParentExpansionIntent.valid )
				{
					DWORD mask = 0; bool observed = true;
					HTREEITEM parents[] = { capture.next.parent1, capture.next.parent2 };
					for( unsigned i = 0; i < 2; ++i )
					{
						TVITEMW item = {}; item.mask = TVIF_STATE; item.hItem = parents[i]; item.stateMask = TVIS_EXPANDED;
						if( !parents[i] || !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
							|| !TreeIdentityCaptureCurrent() ) { observed = false; break; }
						if( item.state & TVIS_EXPANDED ) mask |= 1u << i;
					}
					if( observed && capture.next.ids1.directoryId && capture.next.ids2.directoryId )
					{
						TreeParentExpansionIntent intent; intent.owner = owner; intent.window = window;
						intent.path1 = capture.next.path1; intent.path2 = capture.next.path2;
						intent.parent1 = capture.next.parent1; intent.parent2 = capture.next.parent2;
						intent.volume1 = capture.next.ids1.volume; intent.id1 = capture.next.ids1.directoryId;
						intent.volume2 = capture.next.ids2.volume; intent.id2 = capture.next.ids2.directoryId;
						intent.inputRevision = gTreeIdentityInputRevision; intent.generation = ++gTreeParentExpansionGeneration;
						intent.expandedMask = mask; intent.valid = true;
						gTreeParentExpansionIntent = std::move(intent);
						LeeyesTreeRescanObserve(owner, mask, 0xF000006A);
					}
				}
			}
			catch( ... ) { gTreeParentExpansionIntent = TreeParentExpansionIntent(); }
		}
		if( !TreeIdentityCaptureCurrent() ) { FinishTreeIdentityCapture(false); return; }
		capture.next.valid = true; gTreeIdentityShadow = std::move(capture.next);
		LeeyesTreeRescanObserve(owner, static_cast<DWORD>(gTreeIdentityShadow.expanded.size()), 0xF000005C);
		if( carry ) LeeyesTreeRescanObserve(owner, refreshed, 0xF0000061);
	}
	else
	{
		gTreeIdentityShadow = TreeIdentityShadow();
		if( window && !(gTreeParentExpansionIntent.valid && gTreeParentExpansionIntent.window == window) )
			::RemoveWindowSubclass(window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
		DiagnosticTreeWorker* first = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		DiagnosticTreeWorker* second = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
		try { if( first ) first->SetTarget(L""); if( second ) second->SetTarget(L""); } catch( ... ) {}
	}
	LeeyesTreeRescanObserve(owner, success ? 1 : 0, 0xF0000057);
	LeeyesTreeRescanObserve(owner, slices, 0xF0000067);
	LeeyesTreeRescanObserve(owner, static_cast<DWORD>(maxSlice > MAXDWORD ? MAXDWORD : maxSlice), 0xF0000068);
	capture = TreeIdentityCapture();
}
bool StartTreeIdentityCapture(TreeStateProbe& probe)
{
	if( gTreeIdentityCapture.active || gTreeExpansionIntent.valid ) return false;
	try
	{
		const UINT_PTR inputId = reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow);
		if( !::SetWindowSubclass(probe.window, TreeIdentityInputSubclass, inputId, 0) ) return false;
		TreeIdentityCapture next; next.next.owner = probe.owner; next.next.window = probe.window;
		next.next.path1 = probe.parentPath; next.next.path2 = probe.parentPath2;
		next.next.parent1 = probe.parent; next.next.parent2 = probe.parent2;
		next.next.parentNode1 = probe.parentNode; next.next.parentNode2 = probe.parentNode2;
		next.inputRevision = gTreeIdentityInputRevision; next.structureRevision = gTreeIdentityStructureRevision;
		next.lifetimeRevision = gTreeIdentityLifetimeRevision; next.postedGeneration = probe.childGeneration;
		next.carry = probe.identityShadowCarry; next.refreshedChildren = static_cast<unsigned>(probe.children.size());
		if( next.carry && (!gTreeIdentityShadow.valid || gTreeIdentityShadow.inputRevision != next.inputRevision
			|| gTreeIdentityShadow.lifetimeRevision != next.lifetimeRevision) ) return false;
		if( !TreeStateParentsValid(probe.window) || !ObserveTreeNode(probe.parentNode, next.next.parentState1)
			|| !ObserveTreeNode(probe.parentNode2, next.next.parentState2) ) return false;
		next.next.ids1 = probe.candidate1.entryIdentities; next.next.ids2 = probe.candidate2.entryIdentities;
		next.ticket1.ticket = probe.candidate1.ticket; next.ticket2.ticket = probe.candidate2.ticket;
		if( next.carry ) for( const auto& old : gTreeIdentityShadow.nodes )
			if( !next.previous.emplace(std::make_pair(old.parent, old.name), old).second ) return false;
		next.next.inputRevision = next.inputRevision; next.next.structureRevision = next.structureRevision;
		next.next.lifetimeRevision = next.lifetimeRevision; next.deadline = ::GetTickCount64() + 10000; next.active = true;
		gTreeIdentityCapture = std::move(next);
		const UINT_PTR id = reinterpret_cast<UINT_PTR>(&gTreeIdentityCapture);
		if( !::SetWindowSubclass(probe.window, TreeIdentityCaptureSubclass, id, 0) || !::SetTimer(probe.window, id, 15, nullptr) )
		{ FinishTreeIdentityCapture(false); return false; }
		if( !TreeIdentityCaptureCurrent() ) { FinishTreeIdentityCapture(false); return false; }
		return true;
	}
	catch( ... ) { if( gTreeIdentityCapture.active ) FinishTreeIdentityCapture(false); return false; }
}
LRESULT CALLBACK TreeIdentityCaptureSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference)
{
	if( message == WM_NCDESTROY ) { FinishTreeIdentityCapture(false); return ::DefSubclassProc(window, message, wParam, lParam); }
	if( message != WM_TIMER || wParam != subclassId ) return ::DefSubclassProc(window, message, wParam, lParam);
	TreeIdentityCapture& capture = gTreeIdentityCapture;
	if( !capture.active || capture.running ) return 0;
	capture.running = true; const ULONGLONG started = ::GetTickCount64(); unsigned processed = 0;
	bool ok = TreeIdentityCaptureCurrent();
	try
	{
		while( ok && capture.parent < 2 && processed < 32 && ::GetTickCount64() - started < 4 )
		{
			const unsigned parent = capture.parent;
			const HTREEITEM root = parent ? capture.next.parent2 : capture.next.parent1;
			if( !capture.cursor )
			{
				capture.cursor = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(root)));
				if( !TreeIdentityCaptureCurrent() ) { ok = false; break; }
				if( !capture.cursor ) { ++capture.parent; continue; }
			}
			const HTREEITEM child = capture.cursor;
			wchar_t name[1024] = {}; TVITEMW item = {}; item.mask = TVIF_TEXT | TVIF_STATE | TVIF_PARAM;
			item.hItem = child; item.pszText = name; item.cchTextMax = _countof(name); item.stateMask = TVIS_EXPANDED;
			if( ++capture.count[parent] > 4096 || !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
				|| !TreeIdentityCaptureCurrent() || !item.lParam ) { ok = false; break; }
			const HTREEITEM actualParent = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(child)));
			if( !TreeIdentityCaptureCurrent() || actualParent != root ) { ok = false; break; }
			const auto& ids = parent ? capture.next.ids2 : capture.next.ids1;
			const auto found = std::lower_bound(ids.items.begin(), ids.items.end(), std::wstring(name),
				[](const DirectoryEntryIdentity::Item& entry, const std::wstring& value) { return entry.name < value; });
			if( found == ids.items.end() || found->name != name ) { ok = false; break; }
			TreeIdentityShadow::Node node = {}; node.parent = parent; node.name = found->name; node.node = static_cast<DWORD>(item.lParam);
			if( !ObserveTreeNode(node.node, node.observation) ) { ok = false; break; }
			if( capture.carry )
			{
				const auto old = capture.previous.find(std::make_pair(parent, node.name));
				if( old == capture.previous.end() || old->second.node != node.node
					|| memcmp(&old->second.observation, &node.observation, sizeof(node.observation)) != 0 ) { ok = false; break; }
				capture.previous.erase(old);
			}
			capture.next.nodes.push_back(std::move(node));
			if( item.state & TVIS_EXPANDED ) capture.next.expanded.push_back({ parent, found->name, found->volume, found->id });
			const HTREEITEM next = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
			if( !TreeIdentityCaptureCurrent() ) { ok = false; break; }
			capture.cursor = next; ++processed;
			if( !next ) ++capture.parent;
		}
	}
	catch( ... ) { ok = false; }
	if( !capture.active ) return 0;
	++capture.slices; capture.maxSliceMs = (std::max)(capture.maxSliceMs, ::GetTickCount64() - started); capture.running = false;
	static bool invalidationInjected = false;
	if( ok && capture.parent < 2 && capture.slices == 1 && !invalidationInjected
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_CAPTURE_INVALIDATE", nullptr, 0) )
	{
		invalidationInjected = true;
		DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		bool requested = false;
		try { requested = worker && worker->RefreshWatched(); } catch( ... ) {}
		LeeyesTreeRescanObserve(capture.next.owner, requested ? 1 : 0, 0xF0000069);
		if( !requested ) ok = false;
	}
	if( !ok || capture.parent == 2 ) FinishTreeIdentityCapture(ok);
	return 0;
}
void FinishTreeStateProbe(bool replay, DWORD status)
{
	TreeStateProbe& probe = gTreeStateProbe;
	LeeyesDiagnosticTreeLogPostClasses();
	if( gWatchedTreeShadow1 )
	{
		if( probe.watched1 ) gWatchedTreeShadow1->Finish(probe.watchedCandidate1);
		try { gWatchedTreeShadow1->SetTarget(L""); } catch( ... ) {}
	}
	if( gWatchedTreeShadow2 )
	{
		if( probe.watched2 ) gWatchedTreeShadow2->Finish(probe.watchedCandidate2);
		try { gWatchedTreeShadow2->SetTarget(L""); } catch( ... ) {}
	}
	if( probe.window )
	{
		::KillTimer(probe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe));
		::KillTimer(probe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 1);
		::KillTimer(probe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 2);
		::KillTimer(probe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 3);
		::RemoveWindowSubclass(probe.window, RescanDiagnosticSubclass,
			reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe));
	}
	if( probe.batchMode )
	{
		if( replay && status != 4 && probe.bothMode && !LeeyesDiagnosticTreeRecoveryOnly()
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RESTORE_FIXTURE_CHILD", nullptr, 0) )
		{
			HTREEITEM child = nullptr; bool expanded = false;
			const bool found = TreeDiagnosticFixtureChild(probe.window, probe.parent, child, expanded);
			if( found && expanded ) LeeyesDiagnosticTreeSetFixtureChildExpansion(TRUE);
			LeeyesTreeRescanObserve(probe.owner, (found ? 1u : 0u) | (expanded ? 2u : 0u), 0xF000004A);
		}
		if( replay && status != 4 && probe.bothMode && LeeyesDiagnosticTreeRecoveryOnly()
			&& LeeyesDiagnosticTreeBatchSize() != 0 && !probe.unsupportedEntry
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_RETRY_CHANGE", nullptr, 0)
			&& LeeyesDiagnosticTreeRetryRecoveryAfterChange(probe.parentPath.c_str(), probe.parentPath2.c_str(), probe.window) )
			LeeyesTreeRescanObserve(probe.owner, 1, 0xF0000049);
		if( replay && status != 4 && probe.bothMode
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECONCILE_AFTER_REPLAY", nullptr, 0) )
		{
			const bool tryEarlyIntent = !LeeyesDiagnosticTreeRecoveryOnly() && !gTreeParentExpansionIntent.valid
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_EXPANSION_EARLY", nullptr, 0)
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_EXPANSION_CARRY", nullptr, 0)
				&& probe.window && ::SetWindowSubclass(probe.window, TreeIdentityInputSubclass,
					reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow), 0);
			const unsigned long long earlyInputRevision = gTreeIdentityInputRevision;
			DWORD expandedMask = 0; bool valid = true;
			HTREEITEM parents[] = { probe.parent, probe.parent2 };
			for( unsigned i = 0; i < 2; ++i )
			{
				TVITEMW item = {}; item.mask = TVIF_STATE; item.hItem = parents[i]; item.stateMask = TVIS_EXPANDED;
				if( !parents[i] || !::SendMessageW(probe.window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) { valid = false; break; }
				if( item.state & TVIS_EXPANDED ) expandedMask |= 1u << i;
			}
			if( valid && tryEarlyIntent && earlyInputRevision == gTreeIdentityInputRevision
				&& probe.childGeneration == LeeyesDiagnosticTreePostedGeneration()
				&& probe.haveCandidate1 && probe.haveCandidate2 )
			{
				DiagnosticTreeWorker* w1 = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
				DiagnosticTreeWorker* w2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
				if( w1 && w2 && w1->CanApply(probe.candidate1) && w2->CanApply(probe.candidate2)
					&& probe.candidate1.state.directoryId && probe.candidate2.state.directoryId )
				try
				{
					TreeParentExpansionIntent intent; intent.owner = probe.owner; intent.window = probe.window;
					intent.path1 = probe.parentPath; intent.path2 = probe.parentPath2;
					intent.parent1 = probe.parent; intent.parent2 = probe.parent2;
					intent.volume1 = probe.candidate1.state.volume; intent.id1 = probe.candidate1.state.directoryId;
					intent.volume2 = probe.candidate2.state.volume; intent.id2 = probe.candidate2.state.directoryId;
					intent.inputRevision = earlyInputRevision; intent.generation = ++gTreeParentExpansionGeneration;
					intent.expandedMask = expandedMask; intent.valid = true;
					if( earlyInputRevision == gTreeIdentityInputRevision && w1->CanApply(probe.candidate1)
						&& w2->CanApply(probe.candidate2) && probe.childGeneration == LeeyesDiagnosticTreePostedGeneration() )
					{ gTreeParentExpansionIntent = std::move(intent); LeeyesTreeRescanObserve(probe.owner, expandedMask, 0xF000006D); }
				}
				catch( ... ) { gTreeParentExpansionIntent = TreeParentExpansionIntent(); }
			}
			if( valid && gTreeParentExpansionIntent.valid
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_EXPANSION_CARRY", nullptr, 0) )
			{
				const auto& intent = gTreeParentExpansionIntent;
				DiagnosticTreeWorker* w1 = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
				DiagnosticTreeWorker* w2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
				const bool comparable = intent.owner == probe.owner && intent.window == probe.window
					&& intent.path1 == probe.parentPath && intent.path2 == probe.parentPath2
					&& intent.inputRevision == gTreeIdentityInputRevision && probe.haveCandidate1 && probe.haveCandidate2
					&& w1 && w2 && w1->CanApply(probe.candidate1) && w2->CanApply(probe.candidate2)
					&& intent.volume1 == probe.candidate1.state.volume && intent.id1 == probe.candidate1.state.directoryId
					&& intent.volume2 == probe.candidate2.state.volume && intent.id2 == probe.candidate2.state.directoryId;
				if( comparable ) expandedMask |= intent.expandedMask;
				LeeyesTreeRescanObserve(probe.owner, comparable ? expandedMask : 0, 0xF000006B);
			}
			if( valid && LeeyesDiagnosticTreeSetRecovery(probe.parentPath.c_str(), probe.parentPath2.c_str(), expandedMask) )
			{
				DiagnosticTreeWorker* firstWorker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
				DiagnosticTreeWorker* secondWorker = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
				if( firstWorker && secondWorker && probe.haveCandidate1 && probe.haveCandidate2
					&& firstWorker->CanApply(probe.candidate1) && secondWorker->CanApply(probe.candidate2) )
					LeeyesDiagnosticTreeSetRecoveryIdentity(probe.candidate1.state.volume, probe.candidate1.state.directoryId,
						probe.candidate2.state.volume, probe.candidate2.state.directoryId);
			}
		}
		if( status == 4 ) LeeyesDiagnosticTreeBatchAbandon();
		else if( !LeeyesDiagnosticTreeBatchResolve(replay ? FALSE : TRUE, probe.window,
			probe.unsupportedEntry ? FALSE : TRUE) ) status = 4;
	}
	else if( replay && status != 4 )
	{
		if( !probe.first || !gLeeyesInternalOriginalTreeAction
			|| !LeeyesDiagnosticReplayOwned(reinterpret_cast<void*>(gLeeyesInternalOriginalTreeAction),
				reinterpret_cast<void*>(probe.owner), probe.action, probe.first, probe.second) ) status = 4;
	}
	LeeyesDiagnosticClearAnsi(&probe.first);
	LeeyesDiagnosticClearAnsi(&probe.second);
	bool keepIdentityShadow = false;
	bool capturePending = false;
	if( !replay && status == 1 && probe.identityShadowEligible && probe.batchMode && probe.bothMode
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_CONTINUITY", nullptr, 0)
		&& ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_PRIMARY", nullptr, 0)
		&& probe.childGeneration == LeeyesDiagnosticTreePostedGeneration() )
	{
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_SLICED_IDENTITY_CAPTURE", nullptr, 0) )
			capturePending = StartTreeIdentityCapture(probe);
		else
		try
		{
			const UINT_PTR inputSubclass = reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow);
			if( !::SetWindowSubclass(probe.window, TreeIdentityInputSubclass, inputSubclass, 0) ) throw std::bad_alloc();
			const unsigned long long revision = gTreeIdentityInputRevision;
			const unsigned long long structureRevision = gTreeIdentityStructureRevision;
			if( probe.identityShadowCarry && (!gTreeIdentityShadow.valid || gTreeIdentityShadow.inputRevision != revision
				|| gTreeIdentityShadow.lifetimeRevision != gTreeIdentityLifetimeRevision) ) throw std::bad_alloc();
			TreeIdentityShadow next; next.owner = probe.owner; next.window = probe.window;
			next.path1 = probe.parentPath; next.path2 = probe.parentPath2;
			next.parent1 = probe.parent; next.parent2 = probe.parent2;
			next.parentNode1 = probe.parentNode; next.parentNode2 = probe.parentNode2;
			if( !TreeStateParentsValid(probe.window) || !ObserveTreeNode(next.parentNode1, next.parentState1)
				|| !ObserveTreeNode(next.parentNode2, next.parentState2) ) throw std::bad_alloc();
			next.ids1 = probe.candidate1.entryIdentities; next.ids2 = probe.candidate2.entryIdentities;
			using NodeKey = std::pair<unsigned, std::wstring>;
			std::map<NodeKey, TreeIdentityShadow::Node> previous;
			if( probe.identityShadowCarry )
				for( const auto& old : gTreeIdentityShadow.nodes )
					if( !previous.emplace(NodeKey(old.parent, old.name), old).second ) throw std::bad_alloc();
			HTREEITEM parents[] = { probe.parent, probe.parent2 };
			for( unsigned parent = 0; parent < 2; ++parent )
			{
				const auto& ids = parent ? next.ids2 : next.ids1;
				HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(probe.window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parents[parent])));
				unsigned count = 0;
				while( child )
				{
					if( ++count > 4096 ) throw std::bad_alloc();
					wchar_t name[1024] = {}; TVITEMW item = {}; item.mask = TVIF_TEXT | TVIF_STATE | TVIF_PARAM;
					item.hItem = child; item.pszText = name; item.cchTextMax = _countof(name); item.stateMask = TVIS_EXPANDED;
					if( !::SendMessageW(probe.window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !item.lParam
						|| reinterpret_cast<HTREEITEM>(::SendMessageW(probe.window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(child))) != parents[parent] ) throw std::bad_alloc();
					const auto found = std::lower_bound(ids.items.begin(), ids.items.end(), std::wstring(name),
						[](const DirectoryEntryIdentity::Item& entry, const std::wstring& value) { return entry.name < value; });
					if( found == ids.items.end() || found->name != name ) throw std::bad_alloc();
					TreeIdentityShadow::Node node = {}; node.parent = parent; node.name = found->name; node.node = static_cast<DWORD>(item.lParam);
					if( !ObserveTreeNode(node.node, node.observation) ) throw std::bad_alloc();
					if( probe.identityShadowCarry )
					{
						const auto old = previous.find(NodeKey(parent, node.name));
						if( old == previous.end() || old->second.node != node.node
							|| memcmp(&old->second.observation, &node.observation, sizeof(node.observation)) != 0 ) throw std::bad_alloc();
						previous.erase(old);
					}
					next.nodes.push_back(std::move(node));
					if( item.state & TVIS_EXPANDED ) next.expanded.push_back({ parent, found->name, found->volume, found->id });
					child = reinterpret_cast<HTREEITEM>(::SendMessageW(probe.window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
				}
				if( count != ids.items.size() ) throw std::bad_alloc();
			}
			if( probe.identityShadowCarry && !previous.empty() ) throw std::bad_alloc();
			DiagnosticTreeWorker* current1 = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
			DiagnosticTreeWorker* current2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
			if( probe.identityShadowCarry && current1
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_CARRY_REFRESH_DURING_CAPTURE", nullptr, 0) )
			{
				const bool requested = current1->RefreshWatched();
				LeeyesTreeRescanObserve(probe.owner, requested ? 1 : 0, 0xF0000064);
				if( !requested ) throw std::bad_alloc();
			}
			if( revision != gTreeIdentityInputRevision || structureRevision != gTreeIdentityStructureRevision
				|| probe.childGeneration != LeeyesDiagnosticTreePostedGeneration()
				|| !current1 || !current2 || !current1->IsCommittedCurrent(probe.candidate1)
				|| !current2->IsCommittedCurrent(probe.candidate2) ) throw std::bad_alloc();
			next.inputRevision = revision;
			next.structureRevision = structureRevision;
			next.lifetimeRevision = gTreeIdentityLifetimeRevision;
			next.valid = true; gTreeIdentityShadow = std::move(next); keepIdentityShadow = true;
			LeeyesTreeRescanObserve(probe.owner, static_cast<DWORD>(gTreeIdentityShadow.expanded.size()), 0xF000005C);
			if( probe.identityShadowCarry ) LeeyesTreeRescanObserve(probe.owner, static_cast<DWORD>(probe.children.size()), 0xF0000061);
		}
		catch( ... ) { keepIdentityShadow = false; }
	}
	if( !keepIdentityShadow && !capturePending )
	{
		const bool awaitingRecovery = replay && status == 3 && probe.batchMode && probe.bothMode
			&& gTreeExpansionIntent.valid && gTreeExpansionIntent.owner == probe.owner
			&& gTreeExpansionIntent.window == probe.window && gTreeExpansionIntent.path1 == probe.parentPath
			&& gTreeExpansionIntent.path2 == probe.parentPath2
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECONCILE_AFTER_REPLAY", nullptr, 0);
		if( !awaitingRecovery ) gTreeExpansionIntent = TreeExpansionIntent();
		if( !gTreeExpansionIntent.valid && !(gTreeParentExpansionIntent.valid && gTreeParentExpansionIntent.window == probe.window) )
		{
			if( probe.window ) ::RemoveWindowSubclass(probe.window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
			if( gTreeIdentityShadow.window && gTreeIdentityShadow.window != probe.window )
				::RemoveWindowSubclass(gTreeIdentityShadow.window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
		}
		gTreeIdentityShadow = TreeIdentityShadow();
	}
	if( !capturePending && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_CONTINUITY", nullptr, 0) )
		LeeyesTreeRescanObserve(probe.owner, keepIdentityShadow ? 1 : 0, 0xF0000057);
	DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
	if( worker && ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_PRIMARY", nullptr, 0) )
		LeeyesTreeRescanObserve(probe.owner, worker->ChildStarts(), 0xF0000031);
	if( worker && probe.haveCandidate1 ) worker->FinishApply(probe.candidate1, false);
	if( worker && !keepIdentityShadow && !capturePending ) { try { worker->SetTarget(L""); } catch( ... ) {} }
	worker = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
	if( worker && ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_PRIMARY", nullptr, 0) )
		LeeyesTreeRescanObserve(probe.owner, worker->ChildStarts(), 0xF0000032);
	if( worker && probe.haveCandidate2 ) worker->FinishApply(probe.candidate2, false);
	if( worker && !keepIdentityShadow && !capturePending ) { try { worker->SetTarget(L""); } catch( ... ) {} }
	LeeyesTreeRescanObserve(probe.owner, status, 0xF0000012);
	probe = TreeStateProbe();
}
bool DiagnosticOwnerContainsWindow(DWORD owner, HWND window)
{
	__try
	{
		const DWORD* words = reinterpret_cast<const DWORD*>(owner);
		for( unsigned i = 0; i < 0x1000 / sizeof(DWORD); ++i )
			if( words[i] == reinterpret_cast<DWORD>(window) ) return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	return false;
}
struct TreeWindowSearch { DWORD owner; HWND window; unsigned count; };
BOOL CALLBACK FindDiagnosticTreeChild(HWND window, LPARAM parameter)
{
	TreeWindowSearch* result = reinterpret_cast<TreeWindowSearch*>(parameter);
	char name[64] = {};
	if( ::IsWindowVisible(window) && ::GetClassNameA(window, name, _countof(name))
		&& strcmp(name, "TAcvTreeView") == 0 && DiagnosticOwnerContainsWindow(result->owner, window) )
	{ result->window = window; ++result->count; }
	return TRUE;
}
BOOL CALLBACK FindDiagnosticTreeTop(HWND window, LPARAM parameter)
{
	::EnumChildWindows(window, FindDiagnosticTreeChild, parameter);
	return TRUE;
}
HTREEITEM FindFixtureParent(HWND window, const wchar_t* path)
{
	std::wstring requested;
	try { if( !path || !*path ) return nullptr; requested = path; } catch( ... ) { return nullptr; }
	std::string expected;
	if( !TreeParentAnsiPath(requested, expected) || expected.size() >= 1040 ) return nullptr;
	const bool exact = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_PARENT_CP932", nullptr, 0) != 0;
	try
	{
		std::vector<HTREEITEM> pending;
		HTREEITEM root = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_ROOT, 0));
		if( !root ) return nullptr;
		pending.push_back(root);
		HTREEITEM match = nullptr;
		const ULONGLONG started = ::GetTickCount64();
		char lookupText[16] = {};
		const DWORD lookupLength = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_LOOKUP_MS", lookupText, _countof(lookupText));
		unsigned lookupMs = lookupLength && lookupLength < _countof(lookupText) ? strtoul(lookupText, nullptr, 10) : 50;
		if( lookupMs < 50 || lookupMs > 250 ) lookupMs = 50;
		const ULONGLONG deadline = started + lookupMs;
		unsigned count = 0;
		while( !pending.empty() )
		{
			if( ++count > 100000 || ::GetTickCount64() >= deadline )
			{
				if( lookupLength ) LeeyesTreeRescanObserve(0, count, 0xF0000062);
				return nullptr;
			}
			HTREEITEM item = pending.back(); pending.pop_back();
			TVITEMW data = {}; data.mask = TVIF_PARAM; data.hItem = item;
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&data)) ) return nullptr;
			TreeNodeObservation state = {}; char observed[1040] = {}; DWORD observedLength = 0;
			const bool haveState = data.lParam && ObserveTreeNode(static_cast<DWORD>(data.lParam), state);
			bool samePath = haveState && state.cachedPath && CopyShadowPath(state.cachedPath, observed, observedLength)
				&& (exact ? expected == observed : _stricmp(expected.c_str(), observed) == 0);
			if( haveState && !state.cachedPath && TreeFixtureParentResolveAllowed(requested) )
			{
				wchar_t label[128] = {}; TVITEMW text = {}; text.mask = TVIF_TEXT; text.hItem = item;
				text.pszText = label; text.cchTextMax = _countof(label);
				DWORD detail = 0;
				if( ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&text)) && !wcscmp(label, L"folder_destination") )
					samePath = TreeDiagnosticResolveCachedPath(state.data, state.vmt, expected.data(), static_cast<DWORD>(expected.size()), detail);
				if( samePath )
				{
					TVITEMW recheck = {}; recheck.mask = TVIF_PARAM; recheck.hItem = item;
					TreeNodeObservation now = {};
					samePath = ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&recheck))
						&& recheck.lParam == data.lParam && ObserveTreeNode(static_cast<DWORD>(recheck.lParam), now)
						&& now.data == state.data && now.vmt == state.vmt && now.parent == state.parent;
					LeeyesTreeRescanObserve(0, (detail << 1) | (samePath ? 1u : 0u), 0xF0000050);
				}
			}
			if( samePath )
			{ if( match ) return nullptr; match = item; }
			HTREEITEM next = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(item)));
			HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(item)));
			if( next ) pending.push_back(next);
			if( child ) pending.push_back(child);
		}
		if( lookupLength ) LeeyesTreeRescanObserve(0, static_cast<DWORD>(::GetTickCount64() - started), 0xF0000063);
		return match;
	}
	catch( ... ) { return nullptr; }
}
DWORD TreeDiagnosticRequestedParent(DWORD owner, HWND window, HTREEITEM item, const wchar_t* first, const wchar_t* second)
{
	if( !owner || !window || !item || !first || !second || !DiagnosticOwnerContainsWindow(owner, window) ) return 0;
	try
	{
		const std::wstring path1(first), path2(second);
		if( path1.empty() || path2.empty() ) return 0;
		const HTREEITEM parent1 = FindFixtureParent(window, path1.c_str());
		const HTREEITEM parent2 = FindFixtureParent(window, path2.c_str());
		if( !parent1 || !parent2 || parent1 == parent2 || (item != parent1 && item != parent2) ) return 0;
		const LRESULT state = ::SendMessageW(window, TVM_GETITEMSTATE, reinterpret_cast<WPARAM>(item), TVIS_EXPANDED);
		if( (state & TVIS_EXPANDED) || !DiagnosticOwnerContainsWindow(owner, window) ) return 0;
		return item == parent1 ? 1u : 2u;
	}
	catch( ... ) { return 0; }
}
bool TreeIdentityCachedParents(DWORD owner, HWND window, const wchar_t* path1, const wchar_t* path2,
	HTREEITEM& first, HTREEITEM& second)
{
	first = second = nullptr;
	const TreeIdentityShadow& shadow = gTreeIdentityShadow;
	if( !shadow.valid || shadow.owner != owner || shadow.window != window || !::IsWindow(window)
		|| !path1 || !path2 || shadow.path1 != path1 || shadow.path2 != path2
		|| shadow.inputRevision != gTreeIdentityInputRevision || shadow.structureRevision != gTreeIdentityStructureRevision
		|| !shadow.parent1 || !shadow.parent2 || shadow.parent1 == shadow.parent2 ) return false;
	const HTREEITEM handles[] = { shadow.parent1, shadow.parent2 };
	const DWORD nodes[] = { shadow.parentNode1, shadow.parentNode2 };
	const TreeNodeObservation* states[] = { &shadow.parentState1, &shadow.parentState2 };
	const std::wstring* paths[] = { &shadow.path1, &shadow.path2 };
	for( unsigned i = 0; i < 2; ++i )
	{
		TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = handles[i];
		TreeNodeObservation observed = {};
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
			|| static_cast<DWORD>(item.lParam) != nodes[i] || !ObserveTreeNode(nodes[i], observed)
			|| memcmp(&observed, states[i], sizeof(observed)) != 0 || !TreeStateCachedPathMatches(observed, *paths[i])
			|| shadow.inputRevision != gTreeIdentityInputRevision || shadow.structureRevision != gTreeIdentityStructureRevision ) return false;
	}
	first = handles[0]; second = handles[1]; return true;
}
__declspec(thread) bool gTreeBatchAttaching = false;
struct TreeAttachGuard
{
	TreeAttachGuard() { gTreeBatchAttaching = true; }
	~TreeAttachGuard() { gTreeBatchAttaching = false; }
};
BOOL TreeBatchAttach(DWORD owner, const wchar_t* discovered1 = nullptr, const wchar_t* discovered2 = nullptr)
{
	if( gTreeIdentityCapture.active ) { FinishTreeIdentityCapture(false); return FALSE; }
	if( gTreeBatchAttaching ) return FALSE;
	TreeAttachGuard guard;
	DiagnosticTreeWorker* existingWorker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
	DiagnosticTreeWorker* existingWorker2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
	const bool repeat = ::GetEnvironmentVariableA("LEYEES_TREE_REPEAT_BATCH", nullptr, 0) != 0;
	const bool both = ::GetEnvironmentVariableA("LEYEES_TREE_FIXTURE_BOTH", nullptr, 0) != 0;
	if( gTreeStateProbe.first || gTreeStateProbe.batchMode || (existingWorker && (!repeat || existingWorker->Failed()))
		|| (both && existingWorker2 && (!repeat || existingWorker2->Failed()))
		|| !gTreeSilentHooksReady || !gLeeyesInternalTreeDirectoryRescan ) return FALSE;
	wchar_t path[32768] = {};
	const DWORD length = discovered1 ? static_cast<DWORD>(wcslen(discovered1)) : ::GetEnvironmentVariableW(L"LEYEES_TREE_STATE_PARENT", path, _countof(path));
	if( !length || length >= _countof(path) ) return FALSE;
	if( discovered1 ) wcscpy_s(path, discovered1);
	wchar_t path2[32768] = {};
	if( both )
	{
		const DWORD length2 = discovered2 ? static_cast<DWORD>(wcslen(discovered2)) : ::GetEnvironmentVariableW(L"LEYEES_TREE_STATE_OTHER_PARENT", path2, _countof(path2));
		if( !length2 || length2 >= _countof(path2) ) return FALSE;
		if( discovered2 ) wcscpy_s(path2, discovered2);
		if( !_wcsicmp(path, path2) ) return FALSE;
	}
	TreeWindowSearch search = { owner, nullptr, 0 };
	::EnumThreadWindows(::GetCurrentThreadId(), FindDiagnosticTreeTop, reinterpret_cast<LPARAM>(&search));
	if( search.count != 1 ) return FALSE;
	if( gTreeParentExpansionIntent.valid && (gTreeParentExpansionIntent.owner != owner
		|| gTreeParentExpansionIntent.window != search.window || gTreeParentExpansionIntent.path1 != path
		|| (both && gTreeParentExpansionIntent.path2 != path2)) )
	{
		if( gTreeParentExpansionIntent.window != search.window && gTreeParentExpansionIntent.window )
			::RemoveWindowSubclass(gTreeParentExpansionIntent.window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
		gTreeParentExpansionIntent = TreeParentExpansionIntent();
	}
	if( discovered1 && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_DISCOVERY_REENTER", nullptr, 0) )
		LeeyesDiagnosticTreeTestReenterAttach();
	HTREEITEM resolved = nullptr;
	HTREEITEM resolved2 = nullptr;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_AUTO_PARENT", nullptr, 0) )
	{
		const bool cached = both && !LeeyesDiagnosticTreeRecoveryOnly()
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_CACHE", nullptr, 0)
			&& TreeIdentityCachedParents(owner, search.window, path, path2, resolved, resolved2);
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_PARENT_CACHE", nullptr, 0) )
			LeeyesTreeRescanObserve(owner, cached ? 1 : 0, 0xF0000065);
		if( !cached ) resolved = FindFixtureParent(search.window, path);
		LeeyesTreeRescanObserve(owner, resolved ? 1 : 0, 0xF0000017);
		if( !resolved ) return FALSE;
		if( both )
		{
			if( !cached ) resolved2 = FindFixtureParent(search.window, path2);
			LeeyesTreeRescanObserve(owner, resolved2 ? 1 : 0, 0xF0000019);
			if( !resolved2 || resolved2 == resolved ) return FALSE;
		}
	}
	else if( both ) return FALSE;
	if( LeeyesDiagnosticTreeRecoveryOnly() )
	{
		// A selected subtree must not be rebuilt by this diagnostic recovery.
		HTREEITEM recoveryParents[] = { resolved, resolved2 };
		for( unsigned i = 0; i < 2; ++i )
		{
			const HTREEITEM parent = recoveryParents[i];
			TVITEMW item = {}; item.mask = TVIF_STATE; item.hItem = parent; item.stateMask = TVIS_EXPANDED;
			const bool exists = parent && ::SendMessageW(search.window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item));
			const bool hasChild = exists && ::SendMessageW(search.window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent));
			LeeyesTreeRescanObserve(owner, (exists ? 1 : 0) | ((item.state & TVIS_EXPANDED) ? 2 : 0) | (hasChild ? 4 : 0), 0xF0000036);
			if( !exists ) return FALSE;
			HTREEITEM selected = reinterpret_cast<HTREEITEM>(::SendMessageW(search.window, TVM_GETNEXTITEM, TVGN_CARET, 0));
			const HTREEITEM caret = selected;
			unsigned depth = 0;
			while( selected && depth++ < 1024 )
			{
				if( selected == parent )
				{
					if( caret == parent && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_SELECTED_PARENT", nullptr, 0) )
					{ selected = nullptr; break; }
					return FALSE;
				}
				selected = reinterpret_cast<HTREEITEM>(::SendMessageW(search.window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(selected)));
			}
			if( selected ) return FALSE;
			if( exists && !(item.state & TVIS_EXPANDED) && !hasChild
				&& (LeeyesDiagnosticTreeRecoveryExpandedMask() & (1u << i))
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_MARK_RECOVERY_EXPANDED", nullptr, 0) )
			{
				TVITEMW mark = {}; mark.mask = TVIF_STATE; mark.hItem = parent;
				mark.stateMask = TVIS_EXPANDED; mark.state = TVIS_EXPANDED;
				gTreeIdentityOwnStateMark = &mark;
				const LRESULT marked = ::SendMessageW(search.window, TVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&mark));
				gTreeIdentityOwnStateMark = nullptr;
				if( !marked
					|| !LeeyesDiagnosticTreeRecoveryOnly()
					|| !::SendMessageW(search.window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) ) return FALSE;
				LeeyesTreeRescanObserve(owner, (item.state & TVIS_EXPANDED) ? 1 : 0, 0xF000003C);
			}
			if( !(item.state & TVIS_EXPANDED) )
			{
				const bool lazyFixture = !hasChild && LeeyesDiagnosticTreeRecoveryReadoptFixture()
					&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_LAZY_REPLACED_PARENT", nullptr, 0)
					&& TreeFixtureParentResolveAllowed(i ? std::wstring(path2) : std::wstring(path));
				if( lazyFixture ) LeeyesTreeRescanObserve(owner, i + 1, 0xF0000052);
				if( !lazyFixture && !::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_PARENT_STATE", nullptr, 0) ) return FALSE;
			}
		}
	}
	if( !::SetWindowSubclass(search.window, RescanDiagnosticSubclass,
		reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe), owner) ) return FALSE;
	try { gTreeStateProbe.parentPath = path; if( both ) gTreeStateProbe.parentPath2 = path2; }
	catch( ... ) { ::RemoveWindowSubclass(search.window, RescanDiagnosticSubclass,
		reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe)); return FALSE; }
	gTreeStateProbe.owner = owner; gTreeStateProbe.window = search.window; gTreeStateProbe.batchMode = true; gTreeStateProbe.parent = resolved;
	gTreeStateProbe.bothMode = both; gTreeStateProbe.parent2 = resolved2;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_AUTO_START", nullptr, 0) )
	{
		if( !resolved || !::GetEnvironmentVariableA("LEYEES_TREE_VERIFY_PARENT_PATH", nullptr, 0)
			|| !::SetTimer(search.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 1, 300, nullptr) )
		{
			::RemoveWindowSubclass(search.window, RescanDiagnosticSubclass,
				reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe));
			gTreeStateProbe = TreeStateProbe(); return FALSE;
		}
	}
	LeeyesTreeRescanObserve(owner, 0, 0xF0000015);
	return TRUE;
}
void TreeBatchCancelled()
{
	if( !gTreeStateProbe.batchMode ) return;
	gTreeExpansionIntent = TreeExpansionIntent();
	if( gTreeStateProbe.window && !(gTreeParentExpansionIntent.valid && gTreeParentExpansionIntent.window == gTreeStateProbe.window) )
		::RemoveWindowSubclass(gTreeStateProbe.window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
	if( gTreeIdentityShadow.window && gTreeIdentityShadow.window != gTreeStateProbe.window )
		::RemoveWindowSubclass(gTreeIdentityShadow.window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
	gTreeIdentityShadow = TreeIdentityShadow();
	if( gWatchedTreeShadow1 )
	{
		if( gTreeStateProbe.watched1 ) gWatchedTreeShadow1->Finish(gTreeStateProbe.watchedCandidate1);
		try { gWatchedTreeShadow1->SetTarget(L""); } catch( ... ) {}
	}
	if( gWatchedTreeShadow2 )
	{
		if( gTreeStateProbe.watched2 ) gWatchedTreeShadow2->Finish(gTreeStateProbe.watchedCandidate2);
		try { gWatchedTreeShadow2->SetTarget(L""); } catch( ... ) {}
	}
	if( gTreeStateProbe.window )
	{
		::KillTimer(gTreeStateProbe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe));
		::KillTimer(gTreeStateProbe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 1);
		::KillTimer(gTreeStateProbe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 2);
		::KillTimer(gTreeStateProbe.window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 3);
		::RemoveWindowSubclass(gTreeStateProbe.window, RescanDiagnosticSubclass,
			reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe));
	}
	DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load();
	if( worker && gTreeStateProbe.haveCandidate1 ) worker->FinishApply(gTreeStateProbe.candidate1, false);
	if( worker ) { try { worker->SetTarget(L""); } catch( ... ) {} }
	worker = gDiagnosticTreeStateWorker2.load();
	if( worker && gTreeStateProbe.haveCandidate2 ) worker->FinishApply(gTreeStateProbe.candidate2, false);
	if( worker ) { try { worker->SetTarget(L""); } catch( ... ) {} }
	gTreeStateProbe = TreeStateProbe();
}
void InvokeTreeRescanProtected(DWORD owner, DWORD node, void (__cdecl *rescan)())
{
	__try
	{
		__asm {
			mov eax, owner
			mov edx, node
			call dword ptr [rescan]
		}
	}
	__finally { gTreeSilentRescan = false; }
}
bool TreeRemovalParentCurrent(HWND window, HTREEITEM item, DWORD node,
	const std::wstring& path, bool requireExpanded, DWORD* observedFlags = nullptr)
{
	DWORD flags = 0;
	if( !window || !::IsWindow(window) || !item || !node )
	{ if( observedFlags ) *observedFlags = flags; return false; }
	TVITEMW state = {};
	state.mask = TVIF_PARAM | TVIF_STATE;
	state.stateMask = TVIS_EXPANDED;
	state.hItem = item;
	if( ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&state)) ) flags |= 1;
	if( (flags & 1) && static_cast<DWORD>(state.lParam) == node ) flags |= 2;
	if( (flags & 2) && (state.state & TVIS_EXPANDED) ) flags |= 16;
	TreeNodeObservation observed = {};
	if( (flags & 2) && ObserveTreeNode(node, observed) ) flags |= 4;
	if( (flags & 4) && TreeStateCachedPathMatches(observed, path) ) flags |= 8;
	if( observedFlags ) *observedFlags = flags;
	return (flags & 15) == 15 && (!requireExpanded || (flags & 16) != 0);
}
bool TreeRemovalAllowsCollapsedParent()
{
	return ::GetEnvironmentVariableA("LEYEES_TREE_REMOVAL_ALLOW_COLLAPSED", nullptr, 0) != 0;
}
LRESULT CALLBACK RecoveryExpansionNotifyProbe(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference)
{
	const NMHDR* notification = message == WM_NOTIFY ? reinterpret_cast<const NMHDR*>(lParam) : nullptr;
	const bool expanding = notification && notification->hwndFrom == reinterpret_cast<HWND>(reference)
		&& (notification->code == TVN_ITEMEXPANDINGA || notification->code == TVN_ITEMEXPANDINGW);
	const LRESULT result = ::DefSubclassProc(window, message, wParam, lParam);
	if( expanding ) LeeyesTreeRescanObserve(0, static_cast<DWORD>(result), 0xF000003A);
	if( message == WM_NCDESTROY ) ::RemoveWindowSubclass(window, RecoveryExpansionNotifyProbe, subclassId);
	return result;
}
LRESULT CALLBACK RescanDiagnosticSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
	UINT_PTR subclassId, DWORD_PTR reference)
{
	const bool input = message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN
		|| message == WM_MBUTTONDOWN || message == WM_KEYDOWN || message == WM_SYSKEYDOWN
		|| message == WM_MOUSEWHEEL || message == WM_HSCROLL || message == WM_VSCROLL;
	const bool ownRestore = gTreeStateProbe.restoringFixtureExpansion && gTreeStateProbe.restoreMessagePending
		&& message == TVM_EXPAND && wParam == TVE_EXPAND
		&& reinterpret_cast<HTREEITEM>(lParam) == gTreeStateProbe.restoreMessageTarget;
	if( ownRestore ) gTreeStateProbe.restoreMessagePending = false;
	if( ownRestore && !gTreeStateProbe.injectedRestoreSelection
		&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RESTORE_REENTER_SELECT", nullptr, 0) )
	{
		gTreeStateProbe.injectedRestoreSelection = true;
		const HTREEITEM parent = gTreeStateProbe.parent;
		::SendMessageW(window, TVM_SELECTITEM, TVGN_CARET, reinterpret_cast<LPARAM>(parent));
		if( !gTreeStateProbe.restoringFixtureExpansion || gTreeStateProbe.window != window ) return 0;
	}
	const bool externalTreeChange = (message == TVM_SELECTITEM || message == TVM_EXPAND)
		&& (gTreeStateProbe.restoringFixtureExpansion ? !ownRestore : !gTreeStateProbe.rescanning);
	if( (input || externalTreeChange) && gTreeStateProbe.batchMode && gTreeStateProbe.window == window
		&& LeeyesDiagnosticTreeRecoveryOnly() )
	{
		LeeyesTreeRescanObserve(gTreeStateProbe.owner, input ? 1 : 2, 0xF0000043);
		if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_RETRY_INPUT", nullptr, 0)
			&& LeeyesDiagnosticTreeRetryRecoveryAfterInput(gTreeStateProbe.parentPath.c_str(), gTreeStateProbe.parentPath2.c_str()) )
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000044);
		FinishTreeStateProbe(true, 3);
		return ::DefSubclassProc(window, message, wParam, lParam);
	}
	if( message == WM_NCDESTROY )
	{
		if( gTreeRemovalWindow == window )
		{
			::KillTimer(window, kTreeRemovalTimerId);
			gTreeRemovalWindow = nullptr;
			gTreeRemovalOwner = 0;
			gTreeRemovalPendingParents.clear();
			gTreeRemovalProbeRejected = false;
		}
		if( gTreeStateProbe.window == window && (gTreeStateProbe.first || gTreeStateProbe.batchMode) ) FinishTreeStateProbe(true, 4);
		::RemoveWindowSubclass(window, RescanDiagnosticSubclass, subclassId);
	}
	if( message == WM_TIMER && wParam == kTreeRemovalTimerId && gTreeRemovalWindow == window )
	{
		::KillTimer(window, kTreeRemovalTimerId);
		const DWORD owner = gTreeRemovalOwner;
		if( gTreeRemovalRescanning || gTreeRemovalPendingParents.empty() ) return 0;
		const TreeRemovalParent& pending = gTreeRemovalPendingParents.front();
		DWORD beforeFlags = 0;
		const bool requireExpanded = !TreeRemovalAllowsCollapsedParent();
		const bool parentCurrentBefore = TreeRemovalParentCurrent(window, pending.item,
			pending.node, pending.path, requireExpanded, &beforeFlags);
		if( !owner || !DiagnosticOwnerContainsWindow(owner, window)
			|| !parentCurrentBefore || !gLeeyesInternalTreeDirectoryRescan || !gTreeSilentHooksReady )
		{
			LeeyesTreeRescanObserve(owner, 0, 0xF0000070);
			gTreeRemovalProbeRejected = true;
			return 0;
		}
		LeeyesTreeRescanObserve(owner, gTreeRemovalSkipped.load(std::memory_order_relaxed), 0xF0000070);
		LeeyesTreeRescanObserve(owner, static_cast<DWORD>(gTreeRemovalPendingParents.size()), 0xF0000078);
		LARGE_INTEGER started = {}, ended = {}, frequency = {};
		::QueryPerformanceFrequency(&frequency);
		::QueryPerformanceCounter(&started);
		gTreeRemovalRescanning = true;
		gTreeSilentRescan = true;
		gTreeShellEnumFailure = S_OK;
		InvokeTreeRescanProtected(owner, pending.node, gLeeyesInternalTreeDirectoryRescan);
		gTreeRemovalRescanning = false;
		::QueryPerformanceCounter(&ended);
		if( frequency.QuadPart > 0 && ended.QuadPart >= started.QuadPart )
		{
			const ULONGLONG micros = static_cast<ULONGLONG>(ended.QuadPart - started.QuadPart)
				* 1000000ULL / static_cast<ULONGLONG>(frequency.QuadPart);
			LeeyesTreeRescanObserve(owner, static_cast<DWORD>(micros > MAXDWORD ? MAXDWORD : micros), 0xF0000071);
		}
		LeeyesTreeRescanObserve(owner, static_cast<DWORD>(gTreeShellEnumFailure), 0xF0000072);
		DWORD afterFlags = 0;
		const bool parentCurrentAfter = TreeRemovalParentCurrent(window, pending.item,
			pending.node, pending.path, false, &afterFlags);
		LeeyesTreeRescanObserve(owner,
			parentCurrentAfter ? 1 : 0, 0xF0000073);
		LeeyesTreeRescanObserve(owner,
			parentCurrentBefore && parentCurrentAfter && ((beforeFlags ^ afterFlags) & 16) == 0 ? 1 : 0,
			0xF0000077);
		if( FAILED(gTreeShellEnumFailure) || !parentCurrentAfter )
		{
			gTreeRemovalProbeRejected = true;
			return 0;
		}
		gTreeRemovalPendingParents.erase(gTreeRemovalPendingParents.begin());
		if( !gTreeRemovalPendingParents.empty() )
		{
			if( !::SetTimer(window, kTreeRemovalTimerId, 1, nullptr) )
			{
				gTreeRemovalProbeRejected = true;
				LeeyesTreeRescanObserve(owner, 0, 0xF0000079);
			}
		}
		else gTreeRemovalProbeRejected = false;
		return 0;
	}
	if( message == WM_TIMER && wParam == reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 3
		&& gTreeStateProbe.applyingDirectChanges && gTreeStateProbe.window == window )
	{
		if( gTreeStateProbe.rescanning ) return 0;
		DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		DiagnosticTreeWorker* worker2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
		const bool parentsValidAtStart = TreeStateParentsValid(window);
		bool success = worker && worker2 && parentsValidAtStart
			&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
			&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
		bool stale = !success && worker && worker2 && parentsValidAtStart
			&& (!worker->CanApply(gTreeStateProbe.candidate1) || !worker2->CanApply(gTreeStateProbe.candidate2)
				|| gTreeStateProbe.childGeneration != LeeyesDiagnosticTreePostedGeneration());
		const ULONGLONG start = ::GetTickCount64();
		unsigned count = 0;
		while( success && gTreeStateProbe.nextDirectChange < gTreeStateProbe.directChanges.size() )
		{
			if( gTreeStateProbe.nextDirectChange == 0 ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000002C);
			const TreeStateProbe::DirectChange& change = gTreeStateProbe.directChanges[gTreeStateProbe.nextDirectChange];
			const bool sampleParent = LeeyesDiagnosticTreeRecoveryOnly() && gTreeStateProbe.nextDirectChange == 0
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_PARENT_STATE", nullptr, 0);
			auto sampleState = [window, &change](DWORD marker) {
				TVITEMW item = {}; item.mask = TVIF_STATE | TVIF_CHILDREN; item.hItem = change.parent; item.stateMask = TVIS_EXPANDED;
				const bool valid = ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) != 0;
				const bool hasNode = valid && ::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(change.parent));
				const DWORD flags = (valid ? 1u : 0u) | ((item.state & TVIS_EXPANDED) ? 2u : 0u)
					| (hasNode ? 4u : 0u) | (item.cChildren > 0 ? 8u : 0u) | (item.cChildren < 0 ? 16u : 0u);
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, flags, marker);
			};
			if( sampleParent ) sampleState(0xF0000038);
			if( !TreeDirectChangeSafe(window, change) )
			{ LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000002E); success = false; break; }
			gTreeStateProbe.rescanning = true;
			const BOOL dispatched = gTreeStateProbe.orderedPrefix
				? LeeyesDiagnosticTreeReplayOrdered(change.action, change.path.c_str(), static_cast<DWORD>(change.path.size()))
				: LeeyesDiagnosticReplayMethod(reinterpret_cast<void*>(gLeeyesInternalOriginalTreeAction),
					reinterpret_cast<void*>(gTreeStateProbe.owner), change.action, change.path.c_str(),
					static_cast<DWORD>(change.path.size()), nullptr, 0);
			if( !gTreeStateProbe.applyingDirectChanges || gTreeStateProbe.window != window ) return 0;
			gTreeStateProbe.rescanning = false;
			if( sampleParent ) sampleState(0xF0000039);
			if( !gTreeStateProbe.injectedDirectPost && ::GetEnvironmentVariableA("LEYEES_TREE_DIFF_TEST_UNKNOWN_POST", nullptr, 0) )
			{
				char injectAfterText[16] = {};
				const DWORD injectAfterLength = ::GetEnvironmentVariableA("LEYEES_TREE_DIFF_TEST_UNKNOWN_AFTER", injectAfterText, _countof(injectAfterText));
				const unsigned injectAfter = injectAfterLength && injectAfterLength < _countof(injectAfterText)
					? strtoul(injectAfterText, nullptr, 10) : 1;
				if( injectAfter && gTreeStateProbe.nextDirectChange + 1 >= injectAfter )
				{
					gTreeStateProbe.injectedDirectPost = true;
					LeeyesStateShadowPosted(0);
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000002A);
				}
			}
			const bool parentsValid = TreeStateParentsValid(window);
			if( !gTreeStateProbe.signalledDirectEvent )
			{
				gTreeStateProbe.signalledDirectEvent = true;
				wchar_t eventName[128] = {};
				const DWORD eventLength = ::GetEnvironmentVariableW(L"LEYEES_TREE_TEST_DIRECT_EVENT", eventName, _countof(eventName));
				if( eventLength && eventLength < _countof(eventName) )
				{
					HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
					if( event ) { ::SetEvent(event); ::CloseHandle(event); }
				}
			}
			const bool candidatesCurrent = worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
				&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
			success = dispatched && parentsValid && candidatesCurrent;
			stale = dispatched && parentsValid && !candidatesCurrent;
			++gTreeStateProbe.nextDirectChange; ++count;
			if( count >= 8 || ::GetTickCount64() - start >= 8 ) break;
		}
		if( !success && stale && (!gTreeStateProbe.orderedPrefix
				|| ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_RETRY", nullptr, 0))
			&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_CHILD_RETRY", nullptr, 0) )
		{
			InvalidateTreeIdentityComparison(gTreeStateProbe);
			const DWORD staleReason = (!worker->CanApply(gTreeStateProbe.candidate1) ? 1u : 0u)
				| (!worker2->CanApply(gTreeStateProbe.candidate2) ? 2u : 0u)
				| (gTreeStateProbe.childGeneration != LeeyesDiagnosticTreePostedGeneration() ? 4u : 0u);
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, staleReason, 0xF0000030);
			const bool released1 = worker->FinishApply(gTreeStateProbe.candidate1, false);
			const bool released2 = worker2->FinishApply(gTreeStateProbe.candidate2, false);
			if( released1 ) gTreeStateProbe.haveCandidate1 = false;
			if( released2 ) gTreeStateProbe.haveCandidate2 = false;
			if( released1 && released2 )
			{
				gTreeStateProbe.applyingDirectChanges = false;
				gTreeStateProbe.directChanges.clear(); gTreeStateProbe.nextDirectChange = 0;
				gTreeStateProbe.deadline = ::GetTickCount64() + 10000;
				try { worker->RetryTarget(gTreeStateProbe.parentPath, (staleReason & 1) == 0);
					worker2->RetryTarget(gTreeStateProbe.parentPath2, (staleReason & 2) == 0); }
				catch( ... ) { FinishTreeStateProbe(true, 3); return 0; }
				::KillTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 3);
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000002B);
				if( ::SetTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe), 100, nullptr) ) return 0;
			}
		}
		if( !success ) { FinishTreeStateProbe(true, 3); return 0; }
		if( gTreeStateProbe.nextDirectChange != gTreeStateProbe.directChanges.size() ) return 0;
		if( LeeyesDiagnosticTreeRecoveryOnly()
			&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RECOVERY_EXPANSION_NOTIFY", nullptr, 0) )
		{
			const HTREEITEM parent = gTreeStateProbe.parent;
			const HWND host = ::GetParent(window);
			const UINT_PTR id = reinterpret_cast<UINT_PTR>(&RecoveryExpansionNotifyProbe);
			const bool attached = host && ::SetWindowSubclass(host, RecoveryExpansionNotifyProbe, id, reinterpret_cast<DWORD_PTR>(window));
			if( !attached ) { FinishTreeStateProbe(true, 3); return 0; }
			const LRESULT result = ::SendMessageW(window, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(parent));
			if( ::IsWindow(host) ) ::RemoveWindowSubclass(host, RecoveryExpansionNotifyProbe, id);
			LeeyesTreeRescanObserve(0, static_cast<DWORD>(result), 0xF000003B);
			if( !gTreeStateProbe.applyingDirectChanges || gTreeStateProbe.window != window ) return 0;
		}
		bool matches1 = false, matches2 = false;
		success = TreeStateParentsValid(window)
			&& TreeStateFixtureMatches(window, gTreeStateProbe.parent, gTreeStateProbe.candidate1.state, matches1)
			&& TreeStateFixtureMatches(window, gTreeStateProbe.parent2, gTreeStateProbe.candidate2.state, matches2)
			&& matches1 && matches2 && worker->CanApply(gTreeStateProbe.candidate1)
			&& worker2->CanApply(gTreeStateProbe.candidate2)
			&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
		if( !success && LeeyesDiagnosticTreeRecoveryOnly() )
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, (matches1 ? 1u : 0u) | (matches2 ? 2u : 0u), 0xF0000045);
		if( success )
		{
			// Names that existed before and after may refer to replaced folders.
			// Recheck them; newly created nodes were already built by the callback.
			const bool covered = !LeeyesDiagnosticTreeRecoveryOnly()
				&& ((gTreeStateProbe.orderedPrefix && LeeyesDiagnosticTreeOrderedDrained())
					|| DirectChangesCoverJournal(gTreeStateProbe.directChanges));
			success = (covered || CollectTreeStateChildren(window, gTreeStateProbe.children, &gTreeStateProbe.directChanges))
				&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
				&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
			if( !success && LeeyesDiagnosticTreeRecoveryOnly() )
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, (worker->CanApply(gTreeStateProbe.candidate1) ? 1u : 0u)
					| (worker2->CanApply(gTreeStateProbe.candidate2) ? 2u : 0u)
					| (gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration() ? 4u : 0u), 0xF0000047);
			if( success && !gTreeStateProbe.children.empty() )
			{
				gTreeStateProbe.applyingDirectChanges = false; gTreeStateProbe.refreshingChildren = true;
				::KillTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 3);
				if( ::SetTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 2, 15, nullptr) ) return 0;
				success = false;
			}
		}
		if( success )
		{
			const bool finished1 = worker->FinishApply(gTreeStateProbe.candidate1, true);
			const bool finished2 = worker2->FinishApply(gTreeStateProbe.candidate2, true);
			if( finished1 ) gTreeStateProbe.haveCandidate1 = false;
			if( finished2 ) gTreeStateProbe.haveCandidate2 = false;
			success = finished1 && finished2 && gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
		}
		if( success && gTreeStateProbe.orderedPrefix ) success = LeeyesDiagnosticTreeOrderedDrained() != FALSE;
		LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(gTreeStateProbe.nextDirectChange), 0xF0000029);
		FinishTreeStateProbe(!success, success ? 1 : 3); return 0;
	}
	if( message == WM_TIMER && wParam == reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 2
		&& gTreeStateProbe.refreshingChildren && gTreeStateProbe.window == window )
	{
		if( gTreeStateProbe.rescanning ) return 0;
		DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		DiagnosticTreeWorker* worker2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
		bool success = worker && worker2 && TreeStateParentsValid(window)
			&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
			&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
		bool stale = !success && worker && worker2 && DiagnosticOwnerContainsWindow(gTreeStateProbe.owner, window)
			&& (!worker->CanApply(gTreeStateProbe.candidate1) || !worker2->CanApply(gTreeStateProbe.candidate2)
				|| gTreeStateProbe.childGeneration != LeeyesDiagnosticTreePostedGeneration());
		if( !success ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000023);
		const ULONGLONG start = ::GetTickCount64();
		while( success && gTreeStateProbe.nextChild < gTreeStateProbe.children.size() )
		{
			if( !gTreeStateProbe.signalledChildEvent )
			{
				gTreeStateProbe.signalledChildEvent = true;
				wchar_t eventName[128] = {};
				const DWORD eventLength = ::GetEnvironmentVariableW(L"LEYEES_TREE_TEST_CHILD_EVENT", eventName, _countof(eventName));
				if( eventLength && eventLength < _countof(eventName) )
				{
					HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
					if( event ) { ::SetEvent(event); ::CloseHandle(event); }
				}
			}
			const TreeStateProbe::ChildTarget target = gTreeStateProbe.children[gTreeStateProbe.nextChild];
			TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = target.item;
			TreeNodeObservation current = {};
			const bool itemFound = ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) != 0;
			const bool sameNode = itemFound && static_cast<DWORD>(item.lParam) == target.node;
			const bool observed = sameNode && ObserveTreeNode(target.node, current);
			const bool sameObservation = observed && memcmp(&current, &target.observation, sizeof(current)) == 0;
			success = itemFound && sameNode && observed && sameObservation;
			if( !success && LeeyesDiagnosticTreeRecoveryOnly()
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_MARK_RECOVERY_EXPANDED", nullptr, 0) )
			{
				LeeyesTreeRescanObserve(gTreeStateProbe.owner,
					(itemFound ? 1u : 0u) | (sameNode ? 2u : 0u) | (observed ? 4u : 0u) | (sameObservation ? 8u : 0u), 0xF000003D);
				if( observed )
				{
					const TreeNodeObservation& old = target.observation;
					const DWORD differences = (current.data != old.data ? 1u : 0u) | (current.vmt != old.vmt ? 2u : 0u)
						| (current.part != old.part ? 4u : 0u) | (current.full != old.full ? 8u : 0u)
						| (current.parent != old.parent ? 16u : 0u) | (current.cachedPath != old.cachedPath ? 32u : 0u)
						| (current.pathLength != old.pathLength ? 64u : 0u) | (current.pathHash != old.pathHash ? 128u : 0u)
						| (old.cachedPath ? 256u : 0u) | (current.cachedPath ? 512u : 0u);
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, differences, 0xF000003E);
					if( old.cachedPath && !current.cachedPath && (differences & 0x1fu) == 0
						&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RESOLVE_CHILD_CACHE", nullptr, 0) )
					{
						DWORD detail = 0;
						const bool equal = TreeDiagnosticResolveCachedPath(current.data, current.vmt,
							target.path.data(), static_cast<DWORD>(target.path.size()), detail);
						if( !gTreeStateProbe.refreshingChildren || gTreeStateProbe.window != window ) return 0;
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, (detail << 1) | (equal ? 1u : 0u), 0xF0000041);
						if( equal && ::GetEnvironmentVariableA("LEYEES_TREE_TEST_ACCEPT_RESOLVED_CHILD", nullptr, 0)
							&& TreeStateParentsValid(window) && worker->CanApply(gTreeStateProbe.candidate1)
							&& worker2->CanApply(gTreeStateProbe.candidate2)
							&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration()
							&& TreeDiagnosticRecheckResolvedChild(window, target)
							&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
							&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration() )
						{
							success = true;
							LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000042);
						}
					}
				}
			}
			if( !success ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, 2, 0xF0000023);
			if( !success ) break;
			gTreeStateProbe.rescanning = true;
			const LRESULT refreshed = ::SendMessageW(window, kTreeRescanDiagnosticMessage, 0, reinterpret_cast<LPARAM>(target.item));
			if( !gTreeStateProbe.refreshingChildren || gTreeStateProbe.window != window ) return 0;
			gTreeStateProbe.rescanning = false;
			const bool testUnknownPost = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_TEST_UNKNOWN_POST", nullptr, 0) != 0;
			const bool testRelatedPost = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_TEST_RELATED_POST", nullptr, 0) != 0;
			if( !gTreeStateProbe.injectedChildPost && (testUnknownPost || testRelatedPost) )
			{
				gTreeStateProbe.injectedChildPost = true;
				if( testUnknownPost ) LeeyesStateShadowPosted(0);
				else
				{
					struct PostedString { DWORD byteLength; wchar_t text[1025]; } path = {};
					DWORD record[3] = { 3, 0, 0 };
					const std::wstring& parent = gTreeStateProbe.parentPath;
					if( parent.empty() || parent.size() >= _countof(path.text) )
					{ FinishTreeStateProbe(true, 3); return 0; }
					path.byteLength = static_cast<DWORD>(parent.size() * sizeof(wchar_t));
					memcpy(path.text, parent.c_str(), path.byteLength);
					record[1] = reinterpret_cast<DWORD>(path.text);
					LeeyesStateShadowPosted(reinterpret_cast<DWORD>(record));
				}
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, testUnknownPost ? 1 : 2, 0xF0000027);
			}
			success = refreshed == 1 && worker->CanApply(gTreeStateProbe.candidate1)
				&& worker2->CanApply(gTreeStateProbe.candidate2)
				&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
			stale = refreshed == 1 && !success;
			if( !success ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, refreshed != 1 ? 3 : 4, 0xF0000023);
			++gTreeStateProbe.nextChild;
			if( ::GetTickCount64() - start >= 8 ) break;
		}
		if( !success && stale && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_CHILD_RETRY", nullptr, 0) )
		{
			// A new notification during recovery invalidates the old expansion
			// plan, including any targets already processed on a prior timer.
			InvalidateTreeIdentityComparison(gTreeStateProbe);
			LeeyesDiagnosticTreeLogPostClasses();
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, gTreeStateProbe.childJournalSize, 0xF0000025);
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, LeeyesDiagnosticTreeBatchSize(), 0xF0000026);
			const bool current1 = worker->CanApply(gTreeStateProbe.candidate1);
			const bool current2 = worker2->CanApply(gTreeStateProbe.candidate2);
			const bool released1 = worker->FinishApply(gTreeStateProbe.candidate1, false);
			const bool released2 = worker2->FinishApply(gTreeStateProbe.candidate2, false);
			if( released1 ) gTreeStateProbe.haveCandidate1 = false;
			if( released2 ) gTreeStateProbe.haveCandidate2 = false;
			if( released1 && released2 )
			{
				gTreeStateProbe.refreshingChildren = false;
				gTreeStateProbe.children.clear(); gTreeStateProbe.nextChild = 0;
				gTreeStateProbe.deadline = ::GetTickCount64() + 10000;
				try { worker->RetryTarget(gTreeStateProbe.parentPath, current1); worker2->RetryTarget(gTreeStateProbe.parentPath2, current2); }
				catch( ... ) { FinishTreeStateProbe(true, 3); return 0; }
				::KillTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 2);
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000024);
				if( ::SetTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe), 100, nullptr) ) return 0;
			}
		}
		if( !success ) { FinishTreeStateProbe(true, 3); return 0; }
		if( gTreeStateProbe.nextChild != gTreeStateProbe.children.size() ) return 0;
		bool matches1 = false, matches2 = false;
		success = TreeStateParentsValid(window)
			&& TreeStateFixtureMatches(window, gTreeStateProbe.parent, gTreeStateProbe.candidate1.state, matches1)
			&& TreeStateFixtureMatches(window, gTreeStateProbe.parent2, gTreeStateProbe.candidate2.state, matches2)
			&& matches1 && matches2 && worker->CanApply(gTreeStateProbe.candidate1)
			&& worker2->CanApply(gTreeStateProbe.candidate2)
			&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
		if( success && LeeyesDiagnosticTreeRecoveryOnly() && gTreeExpansionIntent.valid )
		{
			const auto& intent = gTreeExpansionIntent;
			bool eligible = intent.owner == gTreeStateProbe.owner && intent.window == window
				&& intent.path1 == gTreeStateProbe.parentPath && intent.path2 == gTreeStateProbe.parentPath2
				&& intent.inputRevision == gTreeIdentityInputRevision
				&& intent.rootVolume1 == gTreeStateProbe.candidate1.state.volume && intent.rootId1 == gTreeStateProbe.candidate1.state.directoryId
				&& intent.rootVolume2 == gTreeStateProbe.candidate2.state.volume && intent.rootId2 == gTreeStateProbe.candidate2.state.directoryId
				&& gTreeStateProbe.candidate1.hasEntryIdentities && gTreeStateProbe.candidate2.hasEntryIdentities;
			if( eligible && gTreeStateProbe.nextIdentityExpansion < intent.targets.size() )
			{
				const auto target = intent.targets[gTreeStateProbe.nextIdentityExpansion];
				const auto& ids = target.parent ? gTreeStateProbe.candidate2.entryIdentities : gTreeStateProbe.candidate1.entryIdentities;
				const auto found = std::find_if(ids.items.begin(), ids.items.end(), [&](const DirectoryEntryIdentity::Item& item) { return item.name == target.name; });
				eligible = found != ids.items.end() && found->volume == target.volume && found->id == target.id;
				const HTREEITEM parent = target.parent ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
				HTREEITEM item = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(parent)));
				HTREEITEM match = nullptr; bool expanded = false;
				for( unsigned count = 0; eligible && item && count < 100000; ++count )
				{
					wchar_t name[1024] = {}; TVITEMW node = {}; node.mask = TVIF_TEXT | TVIF_STATE | TVIF_PARAM;
					node.hItem = item; node.pszText = name; node.cchTextMax = _countof(name); node.stateMask = TVIS_EXPANDED;
					if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&node)) || !node.lParam ) { eligible = false; break; }
					if( target.name == name ) { if( match ) { eligible = false; break; } match = item; expanded = (node.state & TVIS_EXPANDED) != 0; }
					item = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(item)));
				}
				eligible = eligible && !item && match;
				HTREEITEM caret = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CARET, 0));
				for( unsigned depth = 0; eligible && caret && depth < 1024; ++depth )
				{
					if( caret == match ) { eligible = false; break; }
					caret = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(caret)));
				}
				eligible = eligible && !caret && intent.inputRevision == gTreeIdentityInputRevision
					&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
					&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
				if( eligible && !expanded )
				{
					gTreeStateProbe.rescanning = true; gTreeStateProbe.restoringFixtureExpansion = true;
					gTreeStateProbe.restoreMessagePending = true; gTreeStateProbe.restoreMessageTarget = match;
					gTreeIdentityOwnExpansion = match;
					::SendMessageW(window, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(match));
					gTreeIdentityOwnExpansion = nullptr;
					if( !gTreeStateProbe.refreshingChildren || gTreeStateProbe.window != window ) return 0;
					gTreeStateProbe.restoringFixtureExpansion = false; gTreeStateProbe.restoreMessagePending = false;
					gTreeStateProbe.restoreMessageTarget = nullptr; gTreeStateProbe.rescanning = false;
					TVITEMW check = {}; check.mask = TVIF_STATE; check.hItem = match; check.stateMask = TVIS_EXPANDED;
					eligible = ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&check)) && (check.state & TVIS_EXPANDED)
						&& gTreeExpansionIntent.valid && intent.inputRevision == gTreeIdentityInputRevision
						&& TreeStateParentsValid(window) && worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
						&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
				}
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, eligible ? 1 : 0, 0xF000005F);
				if( eligible && ++gTreeStateProbe.nextIdentityExpansion < intent.targets.size() ) return 0;
			}
			gTreeExpansionIntent = TreeExpansionIntent();
		}
		if( success && LeeyesDiagnosticTreeRestoreFixtureChildRequested() )
		{
			HTREEITEM child = nullptr; bool expanded = false;
			success = TreeDiagnosticFixtureChild(window, gTreeStateProbe.parent, child, expanded);
			if( success && !expanded )
			{
				gTreeStateProbe.rescanning = true;
				gTreeStateProbe.restoringFixtureExpansion = true; gTreeStateProbe.restoreMessagePending = true;
				gTreeStateProbe.restoreMessageTarget = child;
				::SendMessageW(window, TVM_EXPAND, TVE_EXPAND, reinterpret_cast<LPARAM>(child));
				if( !gTreeStateProbe.refreshingChildren || gTreeStateProbe.window != window ) return 0;
				gTreeStateProbe.restoringFixtureExpansion = false; gTreeStateProbe.restoreMessagePending = false;
				gTreeStateProbe.restoreMessageTarget = nullptr;
				gTreeStateProbe.rescanning = false;
				HTREEITEM verified = nullptr;
				success = TreeDiagnosticFixtureChild(window, gTreeStateProbe.parent, verified, expanded)
					&& verified == child && expanded && TreeStateParentsValid(window)
					&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
					&& gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
			}
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, success ? 1 : 0, 0xF000004B);
		}
		if( success )
		{
			const bool finished1 = worker->FinishApply(gTreeStateProbe.candidate1, true);
			const bool finished2 = worker2->FinishApply(gTreeStateProbe.candidate2, true);
			if( finished1 ) gTreeStateProbe.haveCandidate1 = false;
			if( finished2 ) gTreeStateProbe.haveCandidate2 = false;
			success = finished1 && finished2 && gTreeStateProbe.childGeneration == LeeyesDiagnosticTreePostedGeneration();
		}
		gTreeStateProbe.identityShadowEligible = success
			&& !::GetEnvironmentVariableA("LEYEES_TREE_TEST_SKIP_RECOVERY_ADDED", nullptr, 0)
			&& !::GetEnvironmentVariableA("LEYEES_TREE_TEST_LAZY_REPLACED_PARENT", nullptr, 0)
			&& gTreeStateProbe.candidate1.hasEntryIdentities && gTreeStateProbe.candidate2.hasEntryIdentities
			&& gTreeStateProbe.candidate1.entryIdentities.items.size() <= 4096
			&& gTreeStateProbe.candidate2.entryIdentities.items.size() <= 4096
			&& (gTreeStateProbe.identityShadowCarry || gTreeStateProbe.children.size() == gTreeStateProbe.candidate1.entryIdentities.items.size()
				+ gTreeStateProbe.candidate2.entryIdentities.items.size())
			&& DirectoryEntryIdentity::Valid(gTreeStateProbe.candidate1.entryIdentities)
			&& DirectoryEntryIdentity::Valid(gTreeStateProbe.candidate2.entryIdentities);
		LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(gTreeStateProbe.nextChild), 0xF0000022);
		FinishTreeStateProbe(!success, success ? 1 : 3);
		return 0;
	}
	if( message == WM_TIMER && wParam == reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 1
		&& gTreeStateProbe.batchMode && !gTreeStateProbe.running && gTreeStateProbe.window == window )
	{
		::KillTimer(window, wParam);
		if( !LeeyesDiagnosticTreeBatchCanStart() )
		{
			if( !::SetTimer(window, wParam, 100, nullptr) ) FinishTreeStateProbe(true, 3);
			return 0;
		}
		::SendMessageW(window, kTreeRescanDiagnosticMessage, 0, 0);
		return 0;
	}
	if( message == WM_TIMER && wParam == reinterpret_cast<UINT_PTR>(&gTreeStateProbe)
		&& gTreeStateProbe.running && gTreeStateProbe.window == window )
	{
		if( gTreeStateProbe.testComparisonHoldUntil && ::GetTickCount64() < gTreeStateProbe.testComparisonHoldUntil ) return 0;
		DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		if( gTreeStateProbe.bothMode )
		{
			DiagnosticTreeWorker* worker2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
			if( worker && !gTreeStateProbe.haveCandidate1 )
				gTreeStateProbe.haveCandidate1 = worker->TakeCandidate(gTreeStateProbe.candidate1);
			if( worker2 && !gTreeStateProbe.haveCandidate2 )
				gTreeStateProbe.haveCandidate2 = worker2->TakeCandidate(gTreeStateProbe.candidate2);
			const DWORD error1 = worker ? worker->LastError() : ERROR_INVALID_HANDLE;
			const DWORD error2 = worker2 ? worker2->LastError() : ERROR_INVALID_HANDLE;
			const bool stale1 = worker && gTreeStateProbe.haveCandidate1 && !worker->CanApply(gTreeStateProbe.candidate1);
			const bool stale2 = worker2 && gTreeStateProbe.haveCandidate2 && !worker2->CanApply(gTreeStateProbe.candidate2);
			if( !worker || !worker2 || worker->Failed() || worker2->Failed()
				|| (error1 != ERROR_SUCCESS && error1 != ERROR_RETRY)
				|| (error2 != ERROR_SUCCESS && error2 != ERROR_RETRY)
				|| ::GetTickCount64() >= gTreeStateProbe.deadline )
			{
				if( worker && worker2 && !worker->Failed() && !worker2->Failed()
					&& LeeyesDiagnosticTreeRecoveryOnly()
					&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_RETRY_READ_FAILURE", nullptr, 0)
					&& ((error1 != ERROR_SUCCESS && error1 != ERROR_RETRY) || (error2 != ERROR_SUCCESS && error2 != ERROR_RETRY))
					&& LeeyesDiagnosticTreeRetryRecoveryAfterReadFailure(gTreeStateProbe.parentPath.c_str(), gTreeStateProbe.parentPath2.c_str()) )
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000004E);
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, error1, 0xF000001A);
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, error2, 0xF000001B);
				if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_ENTRY_IDENTITIES_TRACE", nullptr, 0) )
				{
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, worker ? worker->SnapshotFrames() : 0, 0xF0000055);
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, worker2 ? worker2->SnapshotFrames() : 0, 0xF0000056);
				}
				const DWORD reason = !worker || !worker2 ? 1 : worker->Failed() || worker2->Failed() ? 2
					: error1 != ERROR_SUCCESS && error1 != ERROR_RETRY ? 3
					: error2 != ERROR_SUCCESS && error2 != ERROR_RETRY ? 4
					: 7;
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, reason, 0xF000001C);
				FinishTreeStateProbe(true, 3); return 0;
			}
			if( stale1 || stale2 )
			{
				InvalidateTreeIdentityComparison(gTreeStateProbe);
				const bool current1 = gTreeStateProbe.haveCandidate1 && !stale1;
				const bool current2 = gTreeStateProbe.haveCandidate2 && !stale2;
				LeeyesDiagnosticTreeLogPostClasses();
				bool reset = true;
				if( gTreeStateProbe.haveCandidate1 )
				{
					reset = worker->FinishApply(gTreeStateProbe.candidate1, false);
					if( reset ) gTreeStateProbe.haveCandidate1 = false;
				}
				if( gTreeStateProbe.haveCandidate2 )
				{
					const bool finished = worker2->FinishApply(gTreeStateProbe.candidate2, false);
					if( finished ) gTreeStateProbe.haveCandidate2 = false;
					reset = reset && finished;
				}
				if( !reset ) { FinishTreeStateProbe(true, 3); return 0; }
				try { worker->RetryTarget(gTreeStateProbe.parentPath, current1); worker2->RetryTarget(gTreeStateProbe.parentPath2, current2); }
				catch( ... ) { FinishTreeStateProbe(true, 3); return 0; }
				// A stale candidate proves new work arrived. Give the replacement
				// read a fresh deadline rather than timing out solely because a
				// valid stream of changes has lasted longer than the first read.
				gTreeStateProbe.deadline = ::GetTickCount64() + 10000;
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, (stale1 ? 1 : 0) | (stale2 ? 2 : 0), 0xF000001E);
				return 0;
			}
			if( !gTreeStateProbe.haveCandidate1 || !gTreeStateProbe.haveCandidate2 ) return 0;
			if( gTreeIdentityShadow.valid && !gTreeStateProbe.identityShadowCompared )
			{
				gTreeStateProbe.identityShadowCompared = true;
				LeeyesTreeRescanObserve(gTreeStateProbe.owner,
					gTreeIdentityShadow.inputRevision == gTreeIdentityInputRevision ? 1 : 0, 0xF000005D);
				try
				{
					const StateSyncCoordinator::Candidate* candidates[] = { &gTreeStateProbe.candidate1, &gTreeStateProbe.candidate2 };
					const DirectoryEntryIdentity::State* baselines[] = { &gTreeIdentityShadow.ids1, &gTreeIdentityShadow.ids2 };
					DiagnosticTreeWorker* workers[] = { worker, worker2 };
					bool allComparable = true;
					for( unsigned i = 0; i < 2; ++i )
					{
						std::vector<DirectoryEntryIdentity::Change> changes;
						const bool comparable = candidates[i]->hasEntryIdentities && workers[i]->HasContinuousBaseline(*candidates[i])
							&& DirectoryEntryIdentity::Diff(*baselines[i], candidates[i]->entryIdentities, 4096, changes) == DirectoryEntryIdentity::DiffResult::Complete;
						allComparable = allComparable && comparable;
						DWORD replaced = 0;
						if( comparable ) for( const auto& change : changes ) if( change.kind == DirectoryEntryIdentity::ChangeKind::Replaced ) ++replaced;
						if( replaced ) gTreeStateProbe.identityShadowReplacement = true;
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, (comparable ? 0x80000000u : 0) | replaced, i ? 0xF000005A : 0xF0000059);
					}
					gTreeStateProbe.identityShadowComparable = allComparable;
					gTreeStateProbe.identityShadowCarry = allComparable
						&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_CARRY", nullptr, 0)
						&& gTreeIdentityShadow.inputRevision == gTreeIdentityInputRevision
						&& gTreeIdentityShadow.lifetimeRevision == gTreeIdentityLifetimeRevision
						&& DirectoryEntryIdentity::Same(gTreeIdentityShadow.ids1, candidates[0]->entryIdentities)
						&& DirectoryEntryIdentity::Same(gTreeIdentityShadow.ids2, candidates[1]->entryIdentities);
					if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_RESTORE", nullptr, 0) )
					{
						gTreeExpansionIntent = TreeExpansionIntent();
						if( allComparable && gTreeIdentityShadow.inputRevision == gTreeIdentityInputRevision
							&& gTreeIdentityShadow.structureRevision == gTreeIdentityStructureRevision
							&& gTreeIdentityShadow.expanded.size() <= 64 && worker->CanApply(*candidates[0]) && worker2->CanApply(*candidates[1]) )
						{
							TreeExpansionIntent intent; intent.owner = gTreeStateProbe.owner; intent.window = window;
							intent.path1 = gTreeStateProbe.parentPath; intent.path2 = gTreeStateProbe.parentPath2;
							intent.rootVolume1 = candidates[0]->state.volume; intent.rootId1 = candidates[0]->state.directoryId;
							intent.rootVolume2 = candidates[1]->state.volume; intent.rootId2 = candidates[1]->state.directoryId;
							intent.inputRevision = gTreeIdentityInputRevision;
							for( const auto& old : gTreeIdentityShadow.expanded )
							{
								const auto& items = candidates[old.parent]->entryIdentities.items;
								const auto found = std::find_if(items.begin(), items.end(), [&](const DirectoryEntryIdentity::Item& entry) { return entry.name == old.name; });
								if( found != items.end() ) intent.targets.push_back({ old.parent, found->name, found->volume, found->id });
							}
							intent.valid = !intent.targets.empty(); gTreeExpansionIntent = std::move(intent);
						}
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(gTreeExpansionIntent.targets.size()), 0xF000005E);
					}
				}
				catch( ... ) { gTreeExpansionIntent = TreeExpansionIntent(); LeeyesTreeRescanObserve(gTreeStateProbe.owner, 0, 0xF0000059); }
			}
			static bool injectedComparisonRefresh = false;
			if( gTreeStateProbe.identityShadowCompared && !injectedComparisonRefresh
				&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_COMPARISON_STALE", nullptr, 0) )
			{
				injectedComparisonRefresh = true; bool requested = false;
				try { requested = worker->RefreshWatched(); } catch( ... ) {}
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, requested ? 1 : 0, 0xF0000077);
				if( !requested ) { FinishTreeStateProbe(true, 3); return 0; }
				wchar_t eventName[128] = {};
				const DWORD length = ::GetEnvironmentVariableW(L"LEYEES_TREE_TEST_COMPARISON_EVENT", eventName, _countof(eventName));
				if( length && length < _countof(eventName) )
				{
					gTreeStateProbe.testComparisonHoldUntil = ::GetTickCount64() + 1500;
					HANDLE event = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
					if( event ) { ::SetEvent(event); ::CloseHandle(event); }
				}
				return 0;
			}
			if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_GUARD", nullptr, 0)
				&& !LeeyesDiagnosticTreeRecoveryOnly()
				&& (!gTreeIdentityShadow.valid || !gTreeStateProbe.identityShadowComparable || gTreeStateProbe.identityShadowReplacement) )
			{
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, gTreeStateProbe.identityShadowReplacement ? 1 : 2, 0xF0000060);
				FinishTreeStateProbe(true, 3); return 0;
			}
			if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_ENTRY_IDENTITIES_TRACE", nullptr, 0) )
			{
				for( unsigned i = 0; i < 2; ++i )
				{
					const auto& candidate = i ? gTreeStateProbe.candidate2 : gTreeStateProbe.candidate1;
					const bool valid = candidate.hasEntryIdentities && candidate.entryIdentities.volume == candidate.state.volume
						&& candidate.entryIdentities.directoryId == candidate.state.directoryId
						&& candidate.entryIdentities.items.size() == candidate.state.entries.size();
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, (static_cast<DWORD>(candidate.state.entries.size()) << 1) | (valid ? 1u : 0u),
						i ? 0xF0000054 : 0xF0000053);
				}
			}
			if( LeeyesDiagnosticTreeRecoveryOnly() && !LeeyesDiagnosticTreeRecoveryIdentityMatches(
				gTreeStateProbe.candidate1.state.volume, gTreeStateProbe.candidate1.state.directoryId,
				gTreeStateProbe.candidate2.state.volume, gTreeStateProbe.candidate2.state.directoryId) )
			{
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, 0, 0xF000004F);
				if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_READOPT_REPLACED_PARENT", nullptr, 0)
					&& ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_PREFLIGHT", nullptr, 0) )
					LeeyesDiagnosticTreeRetryRecoveryAfterIdentityMismatch(gTreeStateProbe.parentPath.c_str(), gTreeStateProbe.parentPath2.c_str());
				FinishTreeStateProbe(true, 3); return 0;
			}
			if( gWatchedTreeShadow1 && gWatchedTreeShadow2 && !gTreeStateProbe.watchedCompared
				&& ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_SHADOW", nullptr, 0) )
			{
				if( gTreeStateProbe.watched1 && !gWatchedTreeShadow1->CanApply(gTreeStateProbe.watchedCandidate1) )
				{ gWatchedTreeShadow1->Finish(gTreeStateProbe.watchedCandidate1); gTreeStateProbe.watched1 = false; }
				if( gTreeStateProbe.watched2 && !gWatchedTreeShadow2->CanApply(gTreeStateProbe.watchedCandidate2) )
				{ gWatchedTreeShadow2->Finish(gTreeStateProbe.watchedCandidate2); gTreeStateProbe.watched2 = false; }
				if( !gTreeStateProbe.watched1 ) gTreeStateProbe.watched1 = gWatchedTreeShadow1->Take(gTreeStateProbe.watchedCandidate1);
				if( !gTreeStateProbe.watched2 ) gTreeStateProbe.watched2 = gWatchedTreeShadow2->Take(gTreeStateProbe.watchedCandidate2);
				if( gTreeStateProbe.watched1 && gTreeStateProbe.watched2 )
				{
					const bool equal = DirectorySnapshot::Same(gTreeStateProbe.candidate1.state, gTreeStateProbe.watchedCandidate1.state)
						&& DirectorySnapshot::Same(gTreeStateProbe.candidate2.state, gTreeStateProbe.watchedCandidate2.state)
						&& gWatchedTreeShadow1->CanApply(gTreeStateProbe.watchedCandidate1)
						&& gWatchedTreeShadow2->CanApply(gTreeStateProbe.watchedCandidate2);
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, equal ? 1 : 0, 0xF0000028);
					gTreeStateProbe.watchedCompared = true;
				}
				else if( gWatchedTreeShadow1->Failed() || gWatchedTreeShadow2->Failed()
					|| gWatchedTreeShadow1->Error() || gWatchedTreeShadow2->Error() )
				{
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, 2, 0xF0000028);
					gTreeStateProbe.watchedCompared = true;
				}
				else if( ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_SHADOW_WAIT", nullptr, 0) ) return 0;
			}
			char quietText[16] = {};
			const DWORD quietLength = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_QUIET_MS", quietText, _countof(quietText));
			const unsigned quietMs = quietLength && quietLength < _countof(quietText) ? strtoul(quietText, nullptr, 10) : 0;
			const ULONGLONG lastPost = LeeyesDiagnosticTreeLastPostedTick();
			if( quietMs && quietMs <= 5000 && lastPost && ::GetTickCount64() - lastPost < quietMs ) return 0;
			const unsigned long long generation = LeeyesDiagnosticTreePostedGeneration();
			const bool ambiguous = LeeyesDiagnosticTreeBatchAmbiguous() != FALSE;
			if( ambiguous ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000021);
			bool success = DiagnosticOwnerContainsWindow(gTreeStateProbe.owner, window);
			if( success && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_CHILD_REFRESH", nullptr, 0) )
			{
				bool same[2] = { false, false };
				for( unsigned i = 0; success && i < 2; ++i )
				{
					const HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
					const DWORD node = i ? gTreeStateProbe.parentNode2 : gTreeStateProbe.parentNode;
					const TreeNodeObservation& original = i ? gTreeStateProbe.parentObservation2 : gTreeStateProbe.parentObservation;
					const DirectorySnapshot::State& state = i ? gTreeStateProbe.candidate2.state : gTreeStateProbe.candidate1.state;
					TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = parent;
					TreeNodeObservation current = {};
					success = worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
						&& ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
						&& static_cast<DWORD>(item.lParam) == node && ObserveTreeNode(node, current)
						&& memcmp(&current, &original, sizeof(current)) == 0
						&& TreeStateFixtureMatches(window, parent, state, same[i]);
				}
				if( success && same[0] && same[1] )
				{
					if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DROP_TRANSIENT_PAIRS", nullptr, 0) )
					{
						std::vector<std::string> transientPaths;
						if( LeeyesDiagnosticTreeTransientPaths(&transientPaths) && TransientTreePathsAbsent(window, transientPaths)
							&& generation == LeeyesDiagnosticTreePostedGeneration()
							&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2) )
						{
							const bool finished1 = worker->FinishApply(gTreeStateProbe.candidate1, true);
							const bool finished2 = worker2->FinishApply(gTreeStateProbe.candidate2, true);
							if( finished1 ) gTreeStateProbe.haveCandidate1 = false;
							if( finished2 ) gTreeStateProbe.haveCandidate2 = false;
							const bool accepted = finished1 && finished2 && generation == LeeyesDiagnosticTreePostedGeneration();
							LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(transientPaths.size()), 0xF0000034);
							FinishTreeStateProbe(!accepted, accepted ? 1 : 3); return 0;
						}
					}
					success = CollectTreeStateChildren(window, gTreeStateProbe.children)
						&& generation == LeeyesDiagnosticTreePostedGeneration()
						&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2);
					if( success )
					{
						gTreeStateProbe.childGeneration = generation;
						gTreeStateProbe.childJournalSize = LeeyesDiagnosticTreeBatchSize();
						gTreeStateProbe.refreshingChildren = true;
						::KillTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe));
						if( ::SetTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 2, 15, nullptr) ) return 0;
						success = false;
					}
				}
				else if( success && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_DIFF_DISPATCH", nullptr, 0) )
				{
					if( gTreeStateProbe.orderedWaitUntil && ::GetTickCount64() < gTreeStateProbe.orderedWaitUntil ) return 0;
					gTreeStateProbe.orderedWaitUntil = 0;
					const bool collected = CollectTreeDirectChanges(window, gTreeStateProbe.directChanges);
					const bool recoveryOnly = LeeyesDiagnosticTreeRecoveryOnly() != FALSE;
					if( recoveryOnly && (LeeyesDiagnosticTreeBatchSize() != 0
						|| std::any_of(gTreeStateProbe.directChanges.begin(), gTreeStateProbe.directChanges.end(),
							[](const TreeStateProbe::DirectChange& change) { return change.action != 3; })) )
					{ FinishTreeStateProbe(true, 3); return 0; }
					const bool nonempty = !gTreeStateProbe.directChanges.empty();
					const bool orderedRequested = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_PREFIX", nullptr, 0) != 0;
					const bool ordered = !orderedRequested || (collected && nonempty && OrderTreeDirectChanges(gTreeStateProbe.directChanges));
					if( orderedRequested && gTreeStateProbe.orderedPrefix && collected && nonempty && !ordered
						&& !gTreeStateProbe.orderedWaitUsed && ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_RETRY", nullptr, 0)
						&& ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDERED_WAIT", nullptr, 0) )
					{
						char waitText[16] = {};
						const DWORD waitLength = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_ORDER_WAIT_MS", waitText, _countof(waitText));
						unsigned waitMs = waitLength && waitLength < _countof(waitText) ? strtoul(waitText, nullptr, 10) : 300;
						if( waitMs < 100 || waitMs > 2000 ) waitMs = 300;
						gTreeStateProbe.orderedWaitUsed = true;
						gTreeStateProbe.orderedWaitUntil = ::GetTickCount64() + waitMs;
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, waitMs, 0xF0000033);
						return 0;
					}
					const bool covered = (orderedRequested && ordered)
						|| !::GetEnvironmentVariableA("LEYEES_TREE_STATE_DIFF_CP932", nullptr, 0)
						|| (collected && DirectChangesCoverJournal(gTreeStateProbe.directChanges));
					const bool selectionSafe = collected && TreeDirectSelectionSafe(window, gTreeStateProbe.directChanges);
					const bool current = generation == LeeyesDiagnosticTreePostedGeneration()
						&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2);
					success = collected && nonempty && ordered && covered && current && selectionSafe;
					if( success && orderedRequested ) success = LeeyesDiagnosticTreeBeginOrdered(gTreeStateProbe.directChanges.size()) != FALSE;
					if( !success ) LeeyesTreeRescanObserve(gTreeStateProbe.owner,
						(collected ? 1 : 0) | (nonempty ? 2 : 0) | (covered ? 4 : 0) | (current ? 8 : 0)
						| (selectionSafe ? 16 : 0), 0xF000002D);
					if( success )
					{
						gTreeStateProbe.orderedPrefix = orderedRequested;
						gTreeStateProbe.childGeneration = generation;
						gTreeStateProbe.applyingDirectChanges = true;
						::KillTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe));
						char intervalText[16] = {};
						const DWORD length = ::GetEnvironmentVariableA("LEYEES_TREE_DIFF_SLICE_MS", intervalText, _countof(intervalText));
						unsigned interval = length && length < _countof(intervalText) ? strtoul(intervalText, nullptr, 10) : 15;
						if( interval < 10 || interval > 250 ) interval = 15;
						if( ::SetTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe) + 3, interval, nullptr) ) return 0;
						success = false;
					}
				}
				if( !success ) { FinishTreeStateProbe(true, 3); return 0; }
			}
			char limitText[16] = {};
			const DWORD limitLength = ::GetEnvironmentVariableA("LEYEES_TREE_STATE_MAX_DIFF", limitText, _countof(limitText));
			const unsigned long maxDiff = limitLength && limitLength < _countof(limitText) ? strtoul(limitText, nullptr, 10) : 0;
			if( success && maxDiff )
			{
				for( unsigned i = 0; success && i < 2; ++i )
				{
					const HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
					const DirectorySnapshot::State& state = i ? gTreeStateProbe.candidate2.state : gTreeStateProbe.candidate1.state;
					const DWORD node = i ? gTreeStateProbe.parentNode2 : gTreeStateProbe.parentNode;
					const TreeNodeObservation& original = i ? gTreeStateProbe.parentObservation2 : gTreeStateProbe.parentObservation;
					TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = parent;
					TreeNodeObservation current = {};
					bool matches = false; size_t differences = 0;
					success = worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
						&& ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
						&& static_cast<DWORD>(item.lParam) == node && ObserveTreeNode(node, current)
						&& memcmp(&current, &original, sizeof(current)) == 0
						&& TreeStateFixtureMatches(window, parent, state, matches, &differences);
					if( success && (differences > maxDiff || (ambiguous && state.entries.size() > maxDiff)) )
					{
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, static_cast<DWORD>(differences), 0xF0000020);
						success = false;
					}
				}
			}
			bool changed = false;
			for( unsigned i = 0; success && i < 2; ++i )
			{
				HTREEITEM parent = i ? gTreeStateProbe.parent2 : gTreeStateProbe.parent;
				const DWORD node = i ? gTreeStateProbe.parentNode2 : gTreeStateProbe.parentNode;
				const TreeNodeObservation& original = i ? gTreeStateProbe.parentObservation2 : gTreeStateProbe.parentObservation;
				const DirectorySnapshot::State& state = i ? gTreeStateProbe.candidate2.state : gTreeStateProbe.candidate1.state;
				TVITEMW item = {}; item.mask = TVIF_PARAM; item.hItem = parent;
				TreeNodeObservation current = {}; bool matches = false;
				success = worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2)
					&& ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item))
					&& static_cast<DWORD>(item.lParam) == node && ObserveTreeNode(node, current)
					&& memcmp(&current, &original, sizeof(current)) == 0
					&& TreeStateFixtureMatches(window, parent, state, matches);
				if( !success ) LeeyesTreeRescanObserve(gTreeStateProbe.owner, 0x10 * i + 1, 0xF000001F);
				if( success && (!matches || ambiguous) )
				{
					gTreeStateProbe.rescanning = true;
					const LRESULT result = ::SendMessageW(window, kTreeRescanDiagnosticMessage, 0, reinterpret_cast<LPARAM>(parent));
					gTreeStateProbe.rescanning = false;
					success = result == 1 && TreeStateFixtureMatches(window, parent, state, matches) && matches
						&& worker->CanApply(gTreeStateProbe.candidate1) && worker2->CanApply(gTreeStateProbe.candidate2);
					if( !success )
					{
						const DWORD reason = result != 1 ? 2 : !matches ? 3 : 4;
						LeeyesTreeRescanObserve(gTreeStateProbe.owner, 0x10 * i + reason, 0xF000001F);
					}
					changed = true;
				}
				if( i == 0 && success && ::GetEnvironmentVariableA("LEYEES_STATE_TEST_BETWEEN_RESCANS", nullptr, 0) )
				{
					LeeyesStateShadowPosted(0);
					LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF000001D);
				}
			}
			if( success && generation != LeeyesDiagnosticTreePostedGeneration() )
			{ LeeyesTreeRescanObserve(gTreeStateProbe.owner, 0x20, 0xF000001F); success = false; }
			if( success )
			{
				const bool finished1 = worker->FinishApply(gTreeStateProbe.candidate1, true);
				const bool finished2 = worker2->FinishApply(gTreeStateProbe.candidate2, true);
				if( finished1 ) gTreeStateProbe.haveCandidate1 = false;
				if( finished2 ) gTreeStateProbe.haveCandidate2 = false;
				success = finished1 && finished2 && generation == LeeyesDiagnosticTreePostedGeneration();
			}
			FinishTreeStateProbe(!success, success ? (changed ? 1 : 2) : 3);
			return 0;
		}
		StateSyncCoordinator::Candidate candidate = {};
		if( worker && worker->TakeCandidate(candidate) )
		{
			bool success = false, visible = false;
			DWORD status = 3;
			TVITEMW parentItem = {}; parentItem.mask = TVIF_PARAM; parentItem.hItem = gTreeStateProbe.parent;
			TreeNodeObservation currentParent = {};
			if( worker->CanApply(candidate) && DiagnosticOwnerContainsWindow(gTreeStateProbe.owner, window)
				&& ::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&parentItem))
				&& static_cast<DWORD>(parentItem.lParam) == gTreeStateProbe.parentNode
				&& ObserveTreeNode(gTreeStateProbe.parentNode, currentParent)
				&& memcmp(&currentParent, &gTreeStateProbe.parentObservation, sizeof(currentParent)) == 0 )
			{
				if( gTreeStateProbe.batchMode )
				{
					bool matches = false;
					if( TreeStateFixtureMatches(window, gTreeStateProbe.parent, candidate.state, matches) && worker->CanApply(candidate) )
					{
						if( matches ) { success = true; status = 2; }
						else
						{
							gTreeStateProbe.rescanning = true;
							const LRESULT result = ::SendMessageW(window, kTreeRescanDiagnosticMessage, 0, reinterpret_cast<LPARAM>(gTreeStateProbe.parent));
							gTreeStateProbe.rescanning = false;
							bool after = false;
							success = result == 1 && TreeStateFixtureMatches(window, gTreeStateProbe.parent, candidate.state, after)
								&& after && worker->CanApply(candidate);
							if( success ) status = 1;
						}
					}
				}
				else if( TreeStateChildVisible(window, gTreeStateProbe.parent, gTreeStateProbe.leaf, visible) )
				{
				const auto found = std::lower_bound(candidate.state.entries.begin(), candidate.state.entries.end(),
					gTreeStateProbe.leaf, [](const DirectorySnapshot::Entry& e, const std::wstring& name) { return e.name < name; });
				const bool present = found != candidate.state.entries.end() && found->name == gTreeStateProbe.leaf;
				const bool expected = !present || (found->attributes & FILE_ATTRIBUTE_DIRECTORY);
				if( expected && worker->CanApply(candidate) )
				{
					if( visible == present ) { success = true; status = 2; }
					else
					{
						gTreeStateProbe.rescanning = true;
						const LRESULT result = ::SendMessageW(window, kTreeRescanDiagnosticMessage, 0,
							reinterpret_cast<LPARAM>(gTreeStateProbe.parent));
						gTreeStateProbe.rescanning = false;
						bool after = false;
						success = result == 1 && TreeStateChildVisible(window, gTreeStateProbe.parent, gTreeStateProbe.leaf, after)
							&& after == present && worker->CanApply(candidate);
						if( success ) status = 1;
					}
				}
				}
			}
			worker->FinishApply(candidate, success);
			FinishTreeStateProbe(!success, status);
			return 0;
		}
		const DWORD readError = worker ? worker->LastError() : ERROR_INVALID_HANDLE;
		if( !worker || worker->Failed() || (readError != ERROR_SUCCESS && readError != ERROR_RETRY)
			|| ::GetTickCount64() >= gTreeStateProbe.deadline )
		{
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, readError, 0xF0000013);
			FinishTreeStateProbe(true, 3);
		}
		return 0;
	}
	if( message != kTreeRescanDiagnosticMessage ) return ::DefSubclassProc(window, message, wParam, lParam);
	LeeyesTreeRescanObserve(static_cast<DWORD>(reference), static_cast<DWORD>(lParam), 0xF0000004);
	LeeyesTreeRescanObserve(static_cast<DWORD>(reference), gTreeRemovalSkipped.load(), 0xF000000B);
	if( (gTreeStateProbe.first || gTreeStateProbe.batchMode) && !gTreeStateProbe.rescanning && gTreeStateProbe.owner == static_cast<DWORD>(reference)
		&& gTreeStateProbe.window == window )
	{
		TVITEMW item = {}; item.mask = TVIF_PARAM;
		item.hItem = lParam ? reinterpret_cast<HTREEITEM>(lParam) : gTreeStateProbe.parent;
		if( !item.hItem || !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !item.lParam
			|| !DiagnosticOwnerContainsWindow(gTreeStateProbe.owner, window)
			|| (gDiagnosticTreeStateWorker.load(std::memory_order_acquire)
				&& !::GetEnvironmentVariableA("LEYEES_TREE_REPEAT_BATCH", nullptr, 0)) )
		{ FinishTreeStateProbe(true, 3); return 0; }
		gTreeStateProbe.parentNode = static_cast<DWORD>(item.lParam);
		if( !ObserveTreeNode(gTreeStateProbe.parentNode, gTreeStateProbe.parentObservation) )
		{ FinishTreeStateProbe(true, 3); return 0; }
		if( gTreeStateProbe.batchMode && ::GetEnvironmentVariableA("LEYEES_TREE_VERIFY_PARENT_PATH", nullptr, 0) )
		{
			const bool verified = TreeStateCachedPathMatches(gTreeStateProbe.parentObservation, gTreeStateProbe.parentPath);
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, verified ? 1 : 0, 0xF0000016);
			if( !verified ) { FinishTreeStateProbe(true, 3); return 0; }
		}
		if( gTreeStateProbe.bothMode )
		{
			TVITEMW other = {}; other.mask = TVIF_PARAM; other.hItem = gTreeStateProbe.parent2;
			if( !other.hItem || !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&other)) || !other.lParam )
			{ FinishTreeStateProbe(true, 3); return 0; }
			gTreeStateProbe.parentNode2 = static_cast<DWORD>(other.lParam);
			if( !ObserveTreeNode(gTreeStateProbe.parentNode2, gTreeStateProbe.parentObservation2)
				|| !TreeStateCachedPathMatches(gTreeStateProbe.parentObservation2, gTreeStateProbe.parentPath2) )
			{ FinishTreeStateProbe(true, 3); return 0; }
		}
		HMODULE module = nullptr;
		wchar_t launcher[MAX_PATH * 4] = {};
		if( !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
			reinterpret_cast<LPCWSTR>(&RescanDiagnosticSubclass), &module) )
		{ FinishTreeStateProbe(true, 3); return 0; }
		const DWORD launcherLength = ::GetModuleFileNameW(module, launcher, _countof(launcher));
		if( !launcherLength || launcherLength >= _countof(launcher) )
		{ FinishTreeStateProbe(true, 3); return 0; }
		wchar_t* slash = wcsrchr(launcher, L'\\');
		if( !slash || wcscpy_s(slash + 1, _countof(launcher) - (slash + 1 - launcher), L"LeeyesW.exe") )
		{ FinishTreeStateProbe(true, 3); return 0; }
		DiagnosticTreeWorker* worker = gDiagnosticTreeStateWorker.load(std::memory_order_acquire);
		if( !worker )
		{
			const bool watchedPrimary = ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_PRIMARY", nullptr, 0) != 0;
			worker = new (std::nothrow) DiagnosticTreeWorker(launcher, 5000, watchedPrimary);
			if( !worker || !worker->Start() ) { delete worker; FinishTreeStateProbe(true, 3); return 0; }
			gDiagnosticTreeStateWorker.store(worker, std::memory_order_release);
			LeeyesTreeRescanObserve(gTreeStateProbe.owner, watchedPrimary ? 1 : 0, 0xF000002F);
		}
		else if( worker->Failed() ) { FinishTreeStateProbe(true, 3); return 0; }
		DiagnosticTreeWorker* worker2 = nullptr;
		if( gTreeStateProbe.bothMode )
		{
			worker2 = gDiagnosticTreeStateWorker2.load(std::memory_order_acquire);
			if( !worker2 )
			{
				worker2 = new (std::nothrow) DiagnosticTreeWorker(launcher, 5000, ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_PRIMARY", nullptr, 0) != 0);
				if( !worker2 || !worker2->Start() ) { delete worker2; FinishTreeStateProbe(true, 3); return 0; }
				gDiagnosticTreeStateWorker2.store(worker2, std::memory_order_release);
			}
			else if( worker2->Failed() ) { FinishTreeStateProbe(true, 3); return 0; }
		}
		gTreeStateProbe.parent = item.hItem;
		gTreeStateProbe.running = true;
		gTreeStateProbe.deadline = ::GetTickCount64() + 10000;
		try
		{
			const bool reuse = ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_CONTINUITY", nullptr, 0)
				&& gTreeIdentityShadow.valid && worker2 && gTreeIdentityShadow.owner == gTreeStateProbe.owner
				&& gTreeIdentityShadow.window == window && ::IsWindow(window)
				&& gTreeIdentityShadow.path1 == gTreeStateProbe.parentPath && gTreeIdentityShadow.path2 == gTreeStateProbe.parentPath2;
			if( reuse )
			{
				if( ::GetEnvironmentVariableA("LEYEES_TREE_TEST_IDENTITY_FORCE_RESTART", nullptr, 0) )
				{
					worker->SetTarget(gTreeStateProbe.parentPath); worker2->SetTarget(gTreeStateProbe.parentPath2);
				}
				else if( !worker->RefreshWatched() || !worker2->RefreshWatched() ) throw std::bad_alloc();
				LeeyesTreeRescanObserve(gTreeStateProbe.owner, 1, 0xF0000058);
			}
			else
			{
				if( gTreeIdentityShadow.window && !gTreeExpansionIntent.valid
					&& !(gTreeParentExpansionIntent.valid && gTreeParentExpansionIntent.window == gTreeIdentityShadow.window) )
					::RemoveWindowSubclass(gTreeIdentityShadow.window, TreeIdentityInputSubclass, reinterpret_cast<UINT_PTR>(&gTreeIdentityShadow));
				gTreeIdentityShadow = TreeIdentityShadow();
				worker->SetTarget(gTreeStateProbe.parentPath); if( worker2 ) worker2->SetTarget(gTreeStateProbe.parentPath2);
			}
		}
		catch( ... ) { FinishTreeStateProbe(true, 3); return 0; }
		if( gTreeStateProbe.bothMode && ::GetEnvironmentVariableA("LEYEES_TREE_WATCHED_SHADOW", nullptr, 0) )
		{
			try
			{
				if( !gWatchedTreeShadow1 )
				{
					WatchedSyncWorker* shadow = new (std::nothrow) WatchedSyncWorker(launcher, 5000,
						::GetEnvironmentVariableW(L"LEYEES_STATE_WATCH_ENTRY_IDENTITIES", nullptr, 0) != 0);
					if( shadow && shadow->Start() ) gWatchedTreeShadow1 = shadow; else delete shadow;
				}
				if( !gWatchedTreeShadow2 )
				{
					WatchedSyncWorker* shadow = new (std::nothrow) WatchedSyncWorker(launcher, 5000,
						::GetEnvironmentVariableW(L"LEYEES_STATE_WATCH_ENTRY_IDENTITIES", nullptr, 0) != 0);
					if( shadow && shadow->Start() ) gWatchedTreeShadow2 = shadow; else delete shadow;
				}
				if( gWatchedTreeShadow1 && gWatchedTreeShadow2 )
				{
					gWatchedTreeShadow1->SetTarget(gTreeStateProbe.parentPath);
					gWatchedTreeShadow2->SetTarget(gTreeStateProbe.parentPath2);
				}
				else LeeyesTreeRescanObserve(gTreeStateProbe.owner, 3, 0xF0000028);
			}
			catch( ... ) { LeeyesTreeRescanObserve(gTreeStateProbe.owner, 4, 0xF0000028); }
		}
		if( !::SetTimer(window, reinterpret_cast<UINT_PTR>(&gTreeStateProbe), 100, nullptr) )
		{ FinishTreeStateProbe(true, 3); return 0; }
		LeeyesTreeRescanObserve(gTreeStateProbe.owner, 0, 0xF0000011);
		return 1;
	}
	if( !gTreeStateProbe.rescanning ) ::RemoveWindowSubclass(window, RescanDiagnosticSubclass, subclassId);
	const DWORD owner = static_cast<DWORD>(reference);
	TVITEMA item = {};
	item.mask = TVIF_PARAM;
	item.hItem = reinterpret_cast<HTREEITEM>(lParam);
	if( !gLeeyesInternalOriginalTreeRescanProbe || !DiagnosticOwnerContainsWindow(owner, window)
		|| !item.hItem || !::SendMessageA(window, TVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&item))
		|| !item.lParam ) return 0;
	const DWORD node = static_cast<DWORD>(item.lParam);
	void (__cdecl *rescan)() = gLeeyesInternalOriginalTreeRescanProbe;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_DIRECTORY_RESCAN", nullptr, 0) != 0 )
		rescan = gLeeyesInternalTreeDirectoryRescan;
	if( !rescan ) return 0;
	const bool silent = ::GetEnvironmentVariableA("LEYEES_TREE_SILENT_OWNER", nullptr, 0) != 0;
	if( silent && !gTreeSilentHooksReady ) return 0;
	LARGE_INTEGER started = {}, ended = {}, frequency = {};
	::QueryPerformanceFrequency(&frequency);
	::QueryPerformanceCounter(&started);
	gTreeSilentRescan = silent;
	gTreeShellEnumFailure = S_OK;
	InvokeTreeRescanProtected(owner, node, rescan);
	::QueryPerformanceCounter(&ended);
	if( frequency.QuadPart > 0 && ended.QuadPart >= started.QuadPart )
	{
		const ULONGLONG micros = static_cast<ULONGLONG>(ended.QuadPart - started.QuadPart)
			* 1000000ULL / static_cast<ULONGLONG>(frequency.QuadPart);
		LeeyesTreeRescanObserve(owner, static_cast<DWORD>(micros > MAXDWORD ? MAXDWORD : micros), 0xF0000006);
	}
	if( FAILED(gTreeShellEnumFailure) )
	{
		LeeyesTreeRescanObserve(owner, static_cast<DWORD>(gTreeShellEnumFailure), 0xF0000008);
		return 2;
	}
	return 1;
}
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeFixtureParentAdd(DWORD owner, HWND window, void* method, const wchar_t* path)
{
	if( !owner || !window || !method || !path || !DiagnosticOwnerContainsWindow(owner, window) ) return FALSE;
	try
	{
		const std::wstring full(path);
		const std::wstring suffix = L"\\diagnostic\\state_sync\\tree_fixture_aclretry_i0d_0k_s\\folder_destination";
		if( full.size() <= suffix.size() || _wcsicmp(full.c_str() + full.size() - suffix.size(), suffix.c_str()) ) return FALSE;
		const HTREEITEM existing = FindFixtureParent(window, path);
		if( existing )
		{
			if( !::GetEnvironmentVariableA("LEYEES_TREE_TEST_REPLACE_EXISTING_PARENT", nullptr, 0) ) return FALSE;
			HTREEITEM selected = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_CARET, 0));
			unsigned depth = 0;
			while( selected && depth++ < 1024 )
			{
				if( selected == existing ) return FALSE;
				selected = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(selected)));
			}
			if( selected ) return FALSE;
		}
		const size_t separator = full.find_last_of(L'\\');
		if( separator == std::wstring::npos || !FindFixtureParent(window, full.substr(0, separator).c_str()) ) return FALSE;
		std::string encoded;
		if( !TreeParentAnsiPath(full, encoded) || encoded.empty() ) return FALSE;
		if( existing )
		{
			if( !LeeyesDiagnosticReplayMethod(method, reinterpret_cast<void*>(owner), 5,
				encoded.c_str(), static_cast<DWORD>(encoded.size()), nullptr, 0)
				|| !::IsWindow(window) || !DiagnosticOwnerContainsWindow(owner, window) || FindFixtureParent(window, path) ) return FALSE;
		}
		return LeeyesDiagnosticReplayMethod(method, reinterpret_cast<void*>(owner), 3,
			encoded.c_str(), static_cast<DWORD>(encoded.size()), nullptr, 0);
	}
	catch( ... ) { return FALSE; }
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchAttach(DWORD owner) { return TreeBatchAttach(owner); }
extern "C" BOOL __cdecl LeeyesDiagnosticTreeBatchAttachPaths(DWORD owner, const wchar_t* first, const wchar_t* second)
{
	if( !first || !second ) return FALSE;
	return TreeBatchAttach(owner, first, second);
}
extern "C" BOOL __cdecl LeeyesDiagnosticTreeCaptureRecovery(HWND* window, std::wstring* first, std::wstring* second)
{
	if( !window || !first || !second || !gTreeStateProbe.batchMode || !gTreeStateProbe.bothMode
		|| !gTreeStateProbe.window || gTreeStateProbe.parentPath.empty() || gTreeStateProbe.parentPath2.empty() ) return FALSE;
	try { *window = gTreeStateProbe.window; *first = gTreeStateProbe.parentPath; *second = gTreeStateProbe.parentPath2; return TRUE; }
	catch( ... ) { return FALSE; }
}
extern "C" DWORD __cdecl LeeyesDiagnosticTreeRequestedParent(DWORD owner, HWND window, HTREEITEM item,
	const wchar_t* first, const wchar_t* second)
{ return TreeDiagnosticRequestedParent(owner, window, item, first, second); }
extern "C" void __cdecl LeeyesDiagnosticTreeBatchCancelled() { TreeBatchCancelled(); }
extern "C" BOOL __cdecl LeeyesDiagnosticTreeForceFallback()
{
	if( !gTreeStateProbe.batchMode ) return FALSE;
	FinishTreeStateProbe(true, 3);
	return TRUE;
}
extern "C" BOOL __cdecl LeeyesDiagnosticBufferTreeTrace(const wchar_t* path, const char* data, DWORD length);
extern "C" void __cdecl LeeyesTreeRescanObserve(DWORD owner, DWORD node, DWORD caller)
{
	wchar_t path[MAX_PATH] = {};
	const DWORD length = ::GetEnvironmentVariableW(L"LEYEES_TREE_RESCAN_PROBE", path, _countof(path));
	if( !length || length >= _countof(path) ) return;
	char line[160] = {};
	const int n = sprintf_s(line, "pid=%lu tick=%llu owner=%08lX node=%08lX caller=%08lX\r\n",
		static_cast<unsigned long>(::GetCurrentProcessId()),
		static_cast<unsigned long long>(::GetTickCount64()),
		static_cast<unsigned long>(owner), static_cast<unsigned long>(node),
		static_cast<unsigned long>(caller));
	if( n > 0 && LeeyesDiagnosticBufferTreeTrace(path, line, static_cast<DWORD>(n)) ) return;
	HANDLE file = ::CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if( file == INVALID_HANDLE_VALUE ) return;
	DWORD written = 0;
	if( n > 0 ) ::WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
	::CloseHandle(file);
}
extern "C" __declspec(naked) void LeeyesTreeRescanProbeHook()
{
	__asm
	{
		pushfd
		pushad
		push dword ptr [esp + 36]
		push edx
		push eax
		call LeeyesTreeRescanObserve
		add esp, 12
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalTreeRescanProbe]
	}
}
extern "C" unsigned long long __cdecl LeeyesTreeMutationGeneration();
extern "C" HWND __cdecl LeeyesTreeDiagnosticWindow();
bool TreeExperimentEpochsUnchanged()
{
	return LeeyesTreeMutationGeneration() == gTreeShadow.generation
		&& gTreeWindowEpoch.load() == gTreeShadow.windowEpoch
		&& gTreeObjectEpoch.load() == gTreeShadow.objectEpoch;
}
extern "C" void __cdecl LeeyesTreeShadowBegin()
{
	gTreeShadow.active = true;
	gTreeShadow.count = 0;
	gTreeShadow.generation = LeeyesTreeMutationGeneration();
	gTreeShadow.monitoredTree = nullptr;
	if( TreeExperimentEnabled() )
	{
		HWND tree = LeeyesTreeDiagnosticWindow();
		if( tree && ::GetWindowThreadProcessId(tree, nullptr) == ::GetCurrentThreadId()
			&& ::SetWindowSubclass(tree, TreeExperimentSubclass, reinterpret_cast<UINT_PTR>(&gTreeShadow), 0) )
			gTreeShadow.monitoredTree = tree;
	}
	gTreeShadow.windowEpoch = gTreeWindowEpoch.load();
	gTreeShadow.objectEpoch = gTreeObjectEpoch.load();
}
extern "C" void __cdecl LeeyesTreeShadowEnd()
{
	gTreeShadow.active = false;
	if( gTreeShadow.monitoredTree && ::IsWindow(gTreeShadow.monitoredTree) )
		::RemoveWindowSubclass(gTreeShadow.monitoredTree, TreeExperimentSubclass, reinterpret_cast<UINT_PTR>(&gTreeShadow));
	gTreeShadow.monitoredTree = nullptr;
}
extern "C" void __cdecl LeeyesTreeReadShadow(unsigned long long* candidates,
	unsigned long long* contradictions, unsigned long long* invalidations, unsigned long long* positiveCandidates,
	unsigned long long* stateChanges, unsigned long long* stateUnreadable,
	unsigned long long* handleVerified, unsigned long long* handleFailed, unsigned long long* nodeDestructions)
{
	*candidates = gShadowCandidates.load(std::memory_order_relaxed);
	*contradictions = gShadowContradictions.load(std::memory_order_relaxed);
	*invalidations = gShadowInvalidations.load(std::memory_order_relaxed);
	*positiveCandidates = gShadowPositiveCandidates.load(std::memory_order_relaxed);
	*stateChanges = gShadowStateChanges.load(std::memory_order_relaxed);
	*stateUnreadable = gShadowStateUnreadable.load(std::memory_order_relaxed);
	*handleVerified = gShadowHandleVerified.load(std::memory_order_relaxed);
	*handleFailed = gShadowHandleFailed.load(std::memory_order_relaxed);
	*nodeDestructions = gTreeNodeDestructions.load(std::memory_order_relaxed);
}
extern "C" void __cdecl LeeyesTreeNodeDestroyObserve()
{
	if( TreeExperimentEnabled() ) gTreeObjectEpoch.fetch_add(1);
	if( gTreeShadow.active ) { gTreeNodeDestructions.fetch_add(1, std::memory_order_relaxed); gTreeShadow.count = 0; }
}
extern "C" unsigned long long __cdecl LeeyesTreeDataAssignmentCount() { return gTreeDataAssignments.load(std::memory_order_relaxed); }
extern "C" unsigned long long __cdecl LeeyesTreeExperimentCount() { return gTreeExperimentApplied.load(std::memory_order_relaxed); }
extern "C" void __cdecl LeeyesTreeNodeDataObserve()
{
	if( TreeExperimentEnabled() ) gTreeObjectEpoch.fetch_add(1);
	if( gTreeShadow.active ) { gTreeDataAssignments.fetch_add(1, std::memory_order_relaxed); gTreeShadow.count = 0; }
}
extern "C" __declspec(naked) void LeeyesTreeNodeDataHook()
{
	__asm
	{
		pushfd
		pushad
		call LeeyesTreeNodeDataObserve
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalNodeData]
	}
}
extern "C" __declspec(naked) void LeeyesTreeNodeDestroyHook()
{
	__asm
	{
		pushfd
		pushad
		call LeeyesTreeNodeDestroyObserve
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalNodeDestroy]
	}
}
bool CopyShadowPath(DWORD pointer, char* path, DWORD& length)
{
	if( pointer < sizeof(DWORD) ) return false;
	__try
	{
		length = *reinterpret_cast<const DWORD*>(pointer - sizeof(DWORD));
		if( length == 0 || length >= 1040 ) return false;
		memcpy(path, reinterpret_cast<const void*>(pointer), length);
		if( memchr(path, '\0', length) ) return false;
		path[length] = '\0';
		return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}
namespace
{
bool StoreTreeStateProbe(DWORD owner, HWND window, DWORD action, DWORD first, DWORD second)
{
	if( gTreeStateProbe.first || gDiagnosticTreeStateWorker.load(std::memory_order_acquire)
		|| !owner || !window || (action != 3 && action != 5) || second ) return false;
	char path[1040] = {}; DWORD length = 0;
	wchar_t parent[MAX_PATH * 4] = {}, wide[1040] = {};
	const DWORD parentLength = ::GetEnvironmentVariableW(L"LEYEES_TREE_STATE_PARENT", parent, _countof(parent));
	if( !parentLength || parentLength >= _countof(parent) || !CopyShadowPath(first, path, length) ) return false;
	for( DWORD i = 0; i < length; ++i ) if( static_cast<unsigned char>(path[i]) >= 0x80 ) return false;
	if( !::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, path, -1, wide, _countof(wide)) ) return false;
	wchar_t* separator = wcsrchr(wide, L'\\');
	if( !separator || !separator[1] ) return false;
	*separator = L'\0';
	if( ::CompareStringOrdinal(wide, -1, parent, -1, TRUE) != CSTR_EQUAL ) return false;
	void* owned = LeeyesDiagnosticMakeAnsi(path, length);
	if( !owned ) return false;
	try
	{
		gTreeStateProbe.owner = owner; gTreeStateProbe.window = window; gTreeStateProbe.action = action;
		gTreeStateProbe.parentPath = parent; gTreeStateProbe.leaf = separator + 1;
		gTreeStateProbe.first = owned;
		return true;
	}
	catch( ... )
	{
		LeeyesDiagnosticClearAnsi(&owned);
		gTreeStateProbe = TreeStateProbe();
		return false;
	}
}
}
bool ObserveTreeNode(DWORD node, TreeNodeObservation& state)
{
	__try
	{
		memset(&state, 0, sizeof(state));
		state.data = *reinterpret_cast<const DWORD*>(node + 0x0c);
		if( !state.data ) return false;
		const DWORD* data = reinterpret_cast<const DWORD*>(state.data);
		state.vmt = data[0]; state.part = data[1]; state.full = data[2];
		state.parent = data[4]; state.cachedPath = data[14];
		if( !state.cachedPath ) return true;
		char path[1040] = {};
		if( !CopyShadowPath(state.cachedPath, path, state.pathLength) ) return false;
		state.pathHash = 2166136261u;
		for( DWORD i = 0; i < state.pathLength; ++i ) state.pathHash = (state.pathHash ^ static_cast<unsigned char>(path[i])) * 16777619u;
		return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}
bool VerifyTreeNodeHandle(DWORD node)
{
	__try
	{
		HWND tree = LeeyesTreeDiagnosticWindow();
		if( !tree ) return false;
		TVITEMA item = {};
		item.mask = TVIF_PARAM;
		item.hItem = *reinterpret_cast<HTREEITEM*>(node + 0x10);
		return item.hItem && ::SendMessageA(tree, TVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&item))
			&& static_cast<DWORD>(item.lParam) == node;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}
extern "C" void __cdecl LeeyesTreeShadowObserve(DWORD owner, DWORD pathPointer,
	DWORD root, DWORD result)
{
	if( !gTreeShadow.active || !owner || !root ) return;
	const unsigned long long generation = LeeyesTreeMutationGeneration();
	const unsigned long long windowEpoch = gTreeWindowEpoch.load();
	const unsigned long long objectEpoch = gTreeObjectEpoch.load();
	if( generation != gTreeShadow.generation || windowEpoch != gTreeShadow.windowEpoch
		|| objectEpoch != gTreeShadow.objectEpoch )
	{
		gTreeShadow.count = 0;
		gTreeShadow.generation = generation;
		gTreeShadow.windowEpoch = windowEpoch;
		gTreeShadow.objectEpoch = objectEpoch;
		gShadowInvalidations.fetch_add(1, std::memory_order_relaxed);
	}
	char path[1040] = {};
	DWORD length = 0;
	if( !CopyShadowPath(pathPointer, path, length) ) return;
	for( DWORD i = 0; i < gTreeShadow.count; ++i )
	{
		TreeShadowEntry& entry = gTreeShadow.entries[i];
		if( entry.owner == owner && entry.root == root && entry.length == length
			&& memcmp(entry.path, path, length) == 0 )
		{
			gShadowCandidates.fetch_add(1, std::memory_order_relaxed);
			if( entry.result )
			{
				gShadowPositiveCandidates.fetch_add(1, std::memory_order_relaxed);
				if( result == entry.result && VerifyTreeNodeHandle(result) )
					gShadowHandleVerified.fetch_add(1, std::memory_order_relaxed);
				else gShadowHandleFailed.fetch_add(1, std::memory_order_relaxed);
				TreeNodeObservation current = {};
				if( result != entry.result || !entry.stateValid || !ObserveTreeNode(result, current) )
					gShadowStateUnreadable.fetch_add(1, std::memory_order_relaxed);
				else if( memcmp(&entry.state, &current, sizeof(current)) != 0 )
					gShadowStateChanges.fetch_add(1, std::memory_order_relaxed);
			}
			if( result != entry.result )
			{
				gShadowContradictions.fetch_add(1, std::memory_order_relaxed);
				gTreeShadow.count = 0;
			}
			return;
		}
	}
	if( gTreeShadow.count >= _countof(gTreeShadow.entries) ) return;
	TreeShadowEntry& entry = gTreeShadow.entries[gTreeShadow.count++];
	entry.owner = owner; entry.root = root; entry.length = length; entry.result = result;
	entry.stateValid = result && ObserveTreeNode(result, entry.state);
	entry.item = nullptr;
	if( entry.stateValid ) { __try { entry.item = *reinterpret_cast<HTREEITEM*>(result + 0x10); } __except(EXCEPTION_EXECUTE_HANDLER) { entry.stateValid = false; } }
	memcpy(entry.path, path, length + 1);
}
extern "C" DWORD __cdecl LeeyesTreeExperimentLookup(DWORD owner, DWORD pathPointer, DWORD root, DWORD caller)
{
	// Debug-only experiment: TreeTiming and this environment variable must both
	// be enabled. Do not treat this as a production-safe cache.
	if( !TreeExperimentEnabled() || !gTreeShadow.active || !gTreeShadow.monitoredTree
		|| gTreeTimingDepth != 1 || caller != gTreeExperimentCaller
		|| !TreeExperimentEpochsUnchanged() ) return 0;
	char path[1040] = {}; DWORD length = 0;
	if( !CopyShadowPath(pathPointer, path, length) ) return 0;
	HWND tree = gTreeShadow.monitoredTree;
	HTREEITEM rootItem = nullptr;
	__try { rootItem = *reinterpret_cast<HTREEITEM*>(root + 0x10); }
	__except( EXCEPTION_EXECUTE_HANDLER ) { return 0; }
	TVITEMA rootInfo = {}; rootInfo.mask = TVIF_PARAM; rootInfo.hItem = rootItem;
	if( !rootItem || !::SendMessageA(tree, TVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&rootInfo))
		|| static_cast<DWORD>(rootInfo.lParam) != root || !TreeExperimentEpochsUnchanged() ) return 0;
	__try
	{
		const DWORD vmt = *reinterpret_cast<const DWORD*>(root);
		if( vmt < sizeof(DWORD) || *reinterpret_cast<const DWORD*>(vmt - sizeof(DWORD)) != gTreeVerifiedNodeDestructor ) return 0;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return 0; }
	for( DWORD i = 0; i < gTreeShadow.count; ++i )
	{
		const TreeShadowEntry entry = gTreeShadow.entries[i];
		if( !entry.result || !entry.item || !entry.stateValid || entry.owner != owner || entry.root != root
			|| entry.length != length || memcmp(entry.path, path, length) != 0 ) continue;
		TVITEMA item = {}; item.mask = TVIF_PARAM; item.hItem = entry.item;
		if( !::SendMessageA(tree, TVM_GETITEMA, 0, reinterpret_cast<LPARAM>(&item))
			|| static_cast<DWORD>(item.lParam) != entry.result || !TreeExperimentEpochsUnchanged() ) return 0;
		__try
		{
			const DWORD vmt = *reinterpret_cast<const DWORD*>(entry.result);
			if( vmt < sizeof(DWORD) || *reinterpret_cast<const DWORD*>(vmt - sizeof(DWORD)) != gTreeVerifiedNodeDestructor ) return 0;
		}
		__except( EXCEPTION_EXECUTE_HANDLER ) { return 0; }
		TreeNodeObservation current = {};
		if( !ObserveTreeNode(entry.result, current) || memcmp(&entry.state, &current, sizeof(current)) != 0 ) return 0;
		bool knownGetter = false;
		__try { knownGetter = current.vmt && *reinterpret_cast<const DWORD*>(current.vmt) == gTreeVerifiedPathGetter; }
		__except( EXCEPTION_EXECUTE_HANDLER ) { return 0; }
		char cachedPath[1040] = {}; DWORD cachedLength = 0;
		if( !knownGetter || !current.cachedPath || !CopyShadowPath(current.cachedPath, cachedPath, cachedLength)
			|| cachedLength != length || memcmp(cachedPath, path, length) != 0 ) return 0;
		HTREEITEM ancestor = entry.item;
		for( unsigned int depth = 0; ancestor && ancestor != rootItem && depth < 256; ++depth )
		{
			ancestor = reinterpret_cast<HTREEITEM>(::SendMessageA(tree, TVM_GETNEXTITEM, TVGN_PARENT, reinterpret_cast<LPARAM>(ancestor)));
			if( !TreeExperimentEpochsUnchanged() ) return 0;
		}
		if( ancestor != rootItem || !TreeExperimentEpochsUnchanged() ) return 0;
		gTreeExperimentApplied.fetch_add(1, std::memory_order_relaxed);
		return entry.result;
	}
	return 0;
}
extern "C" __declspec(naked) void LeeyesParentSearchShadowHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 36]
		push dword ptr [ebx + 24]
		push dword ptr [ebx + 20]
		push dword ptr [ebx + 28]
		call LeeyesTreeExperimentLookup
		add esp, 16
		test eax, eax
		jz experiment_original
		mov [ebx + 28], eax
		popad
		popfd
		ret
	experiment_original:
		popad
		popfd
		push edx
		push ecx
		push eax
		call dword ptr [gLeeyesInternalOriginalParentSearch]
		pushfd
		pushad
		mov ebx, esp
		push eax
		push dword ptr [ebx + 40]
		push dword ptr [ebx + 44]
		push dword ptr [ebx + 36]
		call LeeyesTreeShadowObserve
		add esp, 16
		popad
		popfd
		lea esp, [esp + 12]
		ret
	}
}
extern "C" void __cdecl LeeyesTreeRecordSearch(void* result)
{
	gTreeSearchCalls.fetch_add(1, std::memory_order_relaxed);
	if( result ) gTreeSearchHits.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void __cdecl LeeyesTreeReadSearch(unsigned long long* calls, unsigned long long* hits)
{
	*calls = gTreeSearchCalls.load(std::memory_order_relaxed);
	*hits = gTreeSearchHits.load(std::memory_order_relaxed);
}
extern "C" __declspec(naked) void LeeyesTreeSearchHook()
{
	__asm
	{
		call dword ptr [gLeeyesInternalOriginalTreeSearch]
		pushfd
		pushad
		push eax
		call LeeyesTreeRecordSearch
		add esp, 4
		popad
		popfd
		ret
	}
}
extern "C" void __cdecl LeeyesTreeRecordAction(DWORD action)
{
	action &= 0xFF;
	if( action != 0 && action != 2 && gTreeShadow.active ) gTreeShadow.count = 0;
	if( action < 7 ) gTreeActionCounts[action].fetch_add(1, std::memory_order_relaxed);
}
extern "C" BOOL __cdecl LeeyesTreeProbeAction(DWORD owner, DWORD action, DWORD first, DWORD second)
{
	char expected[1040] = {};
	DWORD n = ::GetEnvironmentVariableA("LEYEES_TREE_RESCAN_TARGET", expected, _countof(expected));
	if( !n || n >= _countof(expected) ) return FALSE;
	char value[1040] = {};
	DWORD length = 0;
	DWORD matches = 0;
	if( CopyShadowPath(first, value, length) && _stricmp(expected, value) == 0 ) matches |= 1;
	if( CopyShadowPath(second, value, length) && _stricmp(expected, value) == 0 ) matches |= 2;
	if( matches ) LeeyesTreeRescanObserve(owner, action & 0xff, 0xF0000000 | matches);
	char drop[4] = {};
	if( !matches || !gLeeyesInternalOriginalTreeRescanProbe
		|| ::GetEnvironmentVariableA("LEYEES_TREE_RESCAN_DROP", drop, _countof(drop)) != 1
		|| (drop[0] != '0' && drop[0] != '2' && drop[0] != '3' && drop[0] != '5')
		|| (action & 0xff) != static_cast<DWORD>(drop[0] - '0') ) return FALSE;
	TreeWindowSearch found = { owner, nullptr, 0 };
	::EnumThreadWindows(::GetCurrentThreadId(), FindDiagnosticTreeTop, reinterpret_cast<LPARAM>(&found));
	if( found.count != 1 || !::SetWindowSubclass(found.window, RescanDiagnosticSubclass,
		reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe), owner) ) return FALSE;
	if( ::GetEnvironmentVariableA("LEYEES_TREE_STATE_GATE", nullptr, 0) )
	{
		if( !::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_WORKER", nullptr, 0)
			|| !::GetEnvironmentVariableA("LEYEES_TREE_DIRECTORY_RESCAN", nullptr, 0)
			|| !::GetEnvironmentVariableA("LEYEES_TREE_SILENT_OWNER", nullptr, 0)
			|| !gTreeSilentHooksReady || !gLeeyesInternalTreeDirectoryRescan
			|| !StoreTreeStateProbe(owner, found.window, action & 0xff, first, second) )
		{
			::RemoveWindowSubclass(found.window, RescanDiagnosticSubclass,
				reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe));
			return FALSE;
		}
	}
	LeeyesTreeRescanObserve(owner, reinterpret_cast<DWORD>(found.window), 0xF0000003);
	return TRUE;
}
extern "C" void __cdecl LeeyesTreeReadActions(unsigned long long* actions)
{
	for( unsigned int i = 0; i < 7; ++i ) actions[i] = gTreeActionCounts[i].load(std::memory_order_relaxed);
}
extern "C" __declspec(naked) void LeeyesTreeActionHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 40]
		push ecx
		push edx
		push eax
		call LeeyesTreeProbeAction
		add esp, 16
		test eax, eax
		jnz skip_diagnostic_notification
		push dword ptr [esp + 20]
		call LeeyesTreeRecordAction
		add esp, 4
		popad
		popfd
		jmp dword ptr [gLeeyesInternalOriginalTreeAction]
	skip_diagnostic_notification:
		popad
		popfd
		ret 4
	}
}
extern "C" void __cdecl LeeyesTreeTimingBegin()
{
	if( gTreeTimingDepth++ == 0 ) ::QueryPerformanceCounter(&gTreeTimingStart);
}
extern "C" void __cdecl LeeyesTreeTimingEnd()
{
	if( gTreeTimingDepth == 0 || --gTreeTimingDepth != 0 ) return;
	LARGE_INTEGER end = {};
	if( ::QueryPerformanceCounter(&end) && end.QuadPart >= gTreeTimingStart.QuadPart )
		gTreeTimingTicks.fetch_add(static_cast<unsigned long long>(end.QuadPart - gTreeTimingStart.QuadPart), std::memory_order_relaxed);
	gTreeTimingCalls.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void __cdecl LeeyesTreeTimingRead(unsigned long long* calls,
	unsigned long long* ticks, unsigned long long* frequency)
{
	LARGE_INTEGER value = {};
	::QueryPerformanceFrequency(&value);
	*calls = gTreeTimingCalls.load(std::memory_order_relaxed);
	*ticks = gTreeTimingTicks.load(std::memory_order_relaxed);
	*frequency = static_cast<unsigned long long>(value.QuadPart);
}
bool ReadTreeRemovalOwner(DWORD frame, DWORD& owner)
{
	__try { owner = *reinterpret_cast<const DWORD*>(frame - 8); return owner != 0; }
	__except( EXCEPTION_EXECUTE_HANDLER ) { owner = 0; return false; }
}
extern "C" BOOL __cdecl LeeyesTreeProbeSkipRemoval(DWORD pathPointer, DWORD frame, DWORD caller)
{
	if( !gTreeRemovalReturnSite || caller != gTreeRemovalReturnSite || !frame
		|| gTreeRemovalRescanning || !gLeeyesInternalTreeDirectoryRescan || !gTreeSilentHooksReady
		|| !::GetEnvironmentVariableA("LEYEES_TREE_DIRECTORY_RESCAN", nullptr, 0)
		|| !::GetEnvironmentVariableA("LEYEES_TREE_SILENT_OWNER", nullptr, 0) ) return FALSE;
	char parentFilter[1040] = {}, rootFilter[1040] = {}, path[1040] = {};
	const DWORD parentFilterLength = ::GetEnvironmentVariableA("LEYEES_TREE_REMOVAL_PARENT",
		parentFilter, _countof(parentFilter));
	const DWORD rootFilterLength = ::GetEnvironmentVariableA("LEYEES_TREE_REMOVAL_ROOT",
		rootFilter, _countof(rootFilter));
	DWORD length = 0;
	if( (!parentFilterLength && !rootFilterLength)
		|| parentFilterLength >= _countof(parentFilter) || rootFilterLength >= _countof(rootFilter)
		|| !CopyShadowPath(pathPointer, path, length) ) return FALSE;
	const char* backslash = strrchr(path, '\\');
	const char* slash = strrchr(path, '/');
	const char* separator = !backslash ? slash : !slash ? backslash : (backslash > slash ? backslash : slash);
	if( !separator ) return FALSE;
	size_t parentLength = static_cast<size_t>(separator - path);
	if( parentLength == 2 && path[1] == ':' ) ++parentLength;
	if( !parentLength || parentLength >= 1040 ) return FALSE;
	if( parentFilterLength )
	{
		size_t filterLength = parentFilterLength;
		while( filterLength > 3 && (parentFilter[filterLength - 1] == '\\' || parentFilter[filterLength - 1] == '/') )
			--filterLength;
		if( filterLength != parentLength || _strnicmp(path, parentFilter, filterLength) != 0 ) return FALSE;
	}
	if( rootFilterLength )
	{
		size_t rootLength = rootFilterLength;
		while( rootLength > 3 && (rootFilter[rootLength - 1] == '\\' || rootFilter[rootLength - 1] == '/') )
			--rootLength;
		if( length <= rootLength || _strnicmp(path, rootFilter, rootLength) != 0
			|| (rootFilter[rootLength - 1] != '\\' && rootFilter[rootLength - 1] != '/'
				&& path[rootLength] != '\\' && path[rootLength] != '/') ) return FALSE;
	}
	DWORD owner = 0;
	if( !ReadTreeRemovalOwner(frame, owner) ) return FALSE;
	if( !gTreeRemovalOwner )
	{
		TreeWindowSearch found = { owner, nullptr, 0 };
		::EnumThreadWindows(::GetCurrentThreadId(), FindDiagnosticTreeTop, reinterpret_cast<LPARAM>(&found));
		if( found.count != 1 ) return FALSE;
		gTreeRemovalOwner = owner; gTreeRemovalWindow = found.window;
		LeeyesTreeRescanObserve(owner, reinterpret_cast<DWORD>(found.window), 0xF000000A);
	}
	if( owner != gTreeRemovalOwner || !::IsWindow(gTreeRemovalWindow) ) return FALSE;
	if( !::SetWindowSubclass(gTreeRemovalWindow, RescanDiagnosticSubclass,
		reinterpret_cast<UINT_PTR>(&gLeeyesInternalOriginalTreeRescanProbe), owner) ) return FALSE;
	if( gTreeRemovalProbeRejected ) return FALSE;
	std::wstring parentPath;
	try
	{
		parentPath.reserve(parentLength);
		for( size_t i = 0; i < parentLength; ++i )
		{
			const unsigned char c = static_cast<unsigned char>(path[i]);
			if( c > 0x7f ) return FALSE;
			parentPath.push_back(static_cast<wchar_t>(c));
		}
	}
	catch( ... ) { return FALSE; }
	TreeRemovalParent* pendingParent = nullptr;
	for( auto& candidate : gTreeRemovalPendingParents )
	{
		if( _wcsicmp(candidate.path.c_str(), parentPath.c_str()) == 0 )
		{ pendingParent = &candidate; break; }
	}
	const bool firstParentObservation = pendingParent == nullptr;
	if( !pendingParent )
	{
		if( gTreeRemovalPendingParents.size() >= kTreeRemovalParentCapacity )
		{
			LeeyesTreeRescanObserve(owner, static_cast<DWORD>(gTreeRemovalPendingParents.size()), 0xF000007A);
			return FALSE;
		}
		const HTREEITEM parentItem = FindFixtureParent(gTreeRemovalWindow, parentPath.c_str());
		LeeyesTreeRescanObserve(owner, parentItem ? 1 : 0, 0xF0000074);
		if( !parentItem ) return FALSE;
		TVITEMW item = {};
		item.mask = TVIF_PARAM;
		item.hItem = parentItem;
		if( !::SendMessageW(gTreeRemovalWindow, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&item)) || !item.lParam )
			return FALSE;
		try
		{
			TreeRemovalParent added = { parentItem, static_cast<DWORD>(item.lParam), parentPath };
			gTreeRemovalPendingParents.push_back(std::move(added));
			pendingParent = &gTreeRemovalPendingParents.back();
		}
		catch( ... ) { return FALSE; }
	}
	DWORD parentFlags = 0;
	const bool parentCurrent = TreeRemovalParentCurrent(gTreeRemovalWindow, pendingParent->item,
		pendingParent->node, pendingParent->path, !TreeRemovalAllowsCollapsedParent(), &parentFlags);
	if( firstParentObservation || !parentCurrent ) LeeyesTreeRescanObserve(owner, parentFlags, 0xF0000075);
	if( !parentCurrent )
	{
		if( firstParentObservation ) gTreeRemovalPendingParents.pop_back();
		return FALSE;
	}
	char quietText[16] = {};
	const DWORD quietLength = ::GetEnvironmentVariableA("LEYEES_TREE_REMOVAL_QUIET_MS", quietText, _countof(quietText));
	unsigned quietMs = quietLength && quietLength < _countof(quietText) ? strtoul(quietText, nullptr, 10) : 300;
	if( quietMs < 50 || quietMs > 5000 ) quietMs = 300;
	if( !::SetTimer(gTreeRemovalWindow, kTreeRemovalTimerId, quietMs, nullptr) )
	{
		LeeyesTreeRescanObserve(owner, 0, 0xF0000076);
		if( firstParentObservation ) gTreeRemovalPendingParents.pop_back();
		return FALSE;
	}
	if( firstParentObservation ) LeeyesTreeRescanObserve(owner, quietMs, 0xF0000076);
	gTreeRemovalSkipped.fetch_add(1, std::memory_order_relaxed);
	return TRUE;
}
extern "C" __declspec(naked) void LeeyesTreeTimingHook()
{
	__asm
	{
		pushfd
		pushad
		mov ebx, esp
		push dword ptr [ebx + 36]
		push dword ptr [ebx + 40]
		push dword ptr [ebx + 28]
		call LeeyesTreeProbeSkipRemoval
		add esp, 12
		test eax, eax
		jnz skip_tree_removal
		popad
		popfd
		pushfd
		pushad
		call LeeyesTreeTimingBegin
		popad
		popfd
		push dword ptr [esp + 4]
		call dword ptr [gLeeyesInternalOriginalTreeTiming]
		lea esp, [esp + 4]
		pushfd
		pushad
		call LeeyesTreeTimingEnd
		popad
		popfd
		ret
	skip_tree_removal:
		popad
		popfd
		ret
	}
}
#endif

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
	BOOL incremental = FALSE;
	#if UNICODEHACK_PATH_DEBUG
	// Diagnostic only: DL=0 enters Leeyes' own merge path at 0x004E1BF4.
	if( ::GetEnvironmentVariableA("LEYEES_INCREMENTAL_RESYNC_EXPERIMENT", nullptr, 0) ) incremental = TRUE;
	#endif
	LeeyesInternalInvokeResync(listView, incremental);
}

extern "C" BOOL __cdecl LeeyesInternalInvokeResync(void* listView, BOOL incremental)
{
	if( !listView || !gLeeyesInternalFullResync ) return FALSE;
	const BYTE full = incremental ? 0 : 1;
	__asm
	{
		mov eax, listView
		mov dl, full
		call dword ptr [gLeeyesInternalFullResync]
	}
	return TRUE;
}

#if UNICODEHACK_PATH_DEBUG
extern "C" BOOL __cdecl LeeyesDiagnosticInvokeResync(void* listView, BOOL incremental)
{
	if( !listView || !gLeeyesInternalFullResync
		|| ::GetEnvironmentVariableA("LEYEES_LIST_TEST_RESYNC_UNAVAILABLE", nullptr, 0) ) return FALSE;
	char unavailableText[16] = {};
	const DWORD unavailableLength = ::GetEnvironmentVariableA("LEYEES_LIST_TEST_RESYNC_UNAVAILABLE_FOR_MS", unavailableText, _countof(unavailableText));
	if( unavailableLength && unavailableLength < _countof(unavailableText) )
	{
		const unsigned duration = strtoul(unavailableText, nullptr, 10);
		static ULONGLONG unavailableStarted = 0;
		if( duration && duration <= 10000 )
		{
			if( !unavailableStarted ) unavailableStarted = ::GetTickCount64();
			if( ::GetTickCount64() - unavailableStarted < duration ) return FALSE;
		}
	}
	static bool unavailableInjected = false;
	if( !unavailableInjected && ::GetEnvironmentVariableA("LEYEES_LIST_TEST_RESYNC_UNAVAILABLE_ONCE", nullptr, 0) )
	{ unavailableInjected = true; return FALSE; }
	return LeeyesInternalInvokeResync(listView, incremental);
}
#endif

namespace LeeyesInternal
{

#if UNICODEHACK_PATH_DEBUG
bool InstallTreeTimingHook()
{
	const BYTE signature[] = { 0x55,0x8B,0xEC,0x51,0x53,0x89,0x45,0xFC,
		0x8B,0x45,0xFC,0xE8,0,0,0,0,0x33,0xC0,0x55,0x68 };
	const char mask[] = "xxxxxxxxxxxx????xxxx";
	const BYTE actionSignature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xC8,0x53,0x56,
		0x33,0xDB,0x89,0x5D,0xC8,0x89,0x5D,0xD0,0x89,0x5D,0xCC,
		0x89,0x5D,0xD4,0x89,0x5D,0xD8,0x89,0x5D,0xDC,0x89,0x4D,0xE0 };
	const BYTE searchSignature[] = { 0x55,0x8B,0xEC,0x6A,0x00,0x6A,0x00,0x6A,0x00,
		0x53,0x56,0x57,0x8B,0xF9,0x89,0x55,0xFC,0x8B,0xD8,0x8B,0x45,0xFC,0xE8 };
	const BYTE parentSignature[] = { 0x55,0x8B,0xEC,0x51,0xB9,0x05,0x00,0x00,0x00,
		0x6A,0x00,0x6A,0x00,0x49,0x75,0xF9,0x87,0x4D,0xFC,0x53,0x56,0x57,
		0x8B,0xD9,0x89,0x55,0xFC };
	const BYTE destroySignature[] = { 0x53,0x56,0x57,0x55,0x83,0xC4,0xD8,0xE8 };
	const BYTE destroyTail[] = { 0x8B,0xDA,0x8B,0xE8,0x8B,0x45,0x04,0xE8 };
	const BYTE dataSignature[] = { 0x53,0x8B,0xD8,0x80,0x7B,0x24,0x00,0x75,0x63,0x3B,0x53,0x0C,0x74,0x5E,0x89,0x53,0x0C };
	const BYTE getterPrefix[] = { 0x55,0x8B,0xEC,0x33,0xC9,0x51,0x51,0x51,0x51,0x53,0x56,0x57,0x8B,0xF2,0x8B,0xD8,0x33,0xC0,0x55,0x68 };
	const BYTE getterSuffix[] = { 0x64,0xFF,0x30,0x64,0x89,0x20,0x83,0x7B,0x38,0x00 };
	BYTE* image = reinterpret_cast<BYTE*>(::GetModuleHandleW(nullptr));
	if( !image ) return false;
	IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image);
	if( dos->e_magic != IMAGE_DOS_SIGNATURE ) return false;
	IMAGE_NT_HEADERS32* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(image + dos->e_lfanew);
	if( nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ) return false;
	BYTE* found = nullptr;
	BYTE* actionFound = nullptr;
	BYTE* searchFound = nullptr;
	BYTE* parentFound = nullptr;
	BYTE* destroyFound = nullptr;
	BYTE* dataFound = nullptr;
	BYTE* getterFound = nullptr;
	BYTE* rescanFound = nullptr;
	BYTE* directoryRescanFound = nullptr;
	BYTE* shellOwnerFound = nullptr;
	BYTE* shellEnumFound = nullptr;
	BYTE* statePostSites[2] = {};
	unsigned statePostCount = 0;
	const BYTE statePostPrefix[] = { 0x68,0xC4,0x84,0x00,0x00,0x8B,0x45,0xFC,
		0x8B,0x40,0x40,0x50,0xE8 };
	BYTE* listSortFound = nullptr;
	BYTE* stateNavigationFound = nullptr;
	const BYTE stateNavigationSignature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xF8,0x53,
		0x89,0x45,0xFC,0xC6,0x45,0xFB,0x01,0x8B,0x45,0xFC,0x80,0xB8,0x0D,0x04,0x00,0x00,0x00 };
	const BYTE listSortSignature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xF8,0x89,0x45,0xF8,
		0x8B,0x45,0xF8,0x8B,0x80,0x84,0x03,0x00,0x00,0x85,0xC0,0x7C,0x47 };
	const BYTE rescanSignature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xE8,0x53,0x56,0x33,0xC9,
		0x89,0x4D,0xE8,0x89,0x55,0xEC,0x89,0x45,0xF8,0x8D,0x75,0xEC };
	const BYTE directoryRescanSignature[] = { 0x55,0x8B,0xEC,0x83,0xC4,0xE8,0x53,0x8B,0xDA,
		0x89,0x45,0xFC,0x33,0xC0,0x89,0x45,0xF0,0x33,0xC0,0x89,0x45,0xEC };
	const BYTE shellOwnerSuffix[] = { 0x8B,0x00,0x8B,0x40,0x30,0x50,0x8D,0x55,0xC8,
		0x8B,0x45,0xF8,0x8B,0x40,0x0C,0x8B,0x08,0xFF,0x51,0x0C };
	const BYTE shellEnumSignature[] = { 0x8B,0x00,0xFF,0x50,0x10,0x85,0xC0,0x74,0x0D,
		0x33,0xC0,0x5A,0x59,0x59,0x64,0x89,0x10,0xE9 };
	IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for( WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section )
	{
		if( !(section->Characteristics & IMAGE_SCN_MEM_EXECUTE) || section->Misc.VirtualSize < sizeof(signature) ) continue;
		BYTE* begin = image + section->VirtualAddress;
		for( DWORD n = 0; n <= section->Misc.VirtualSize - sizeof(signature); ++n )
		{
			if( section->Misc.VirtualSize >= sizeof(stateNavigationSignature)
				&& n <= section->Misc.VirtualSize - sizeof(stateNavigationSignature)
				&& memcmp(begin + n, stateNavigationSignature, sizeof(stateNavigationSignature)) == 0 )
			{
				if( stateNavigationFound ) stateNavigationFound = reinterpret_cast<BYTE*>(1);
				else stateNavigationFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(statePostPrefix) + 4
				&& n <= section->Misc.VirtualSize - sizeof(statePostPrefix) - 4
				&& memcmp(begin + n, statePostPrefix, sizeof(statePostPrefix)) == 0 )
			{
				if( statePostCount < 2 ) statePostSites[statePostCount] = begin + n + 12;
				++statePostCount;
			}
			if( section->Misc.VirtualSize >= sizeof(listSortSignature)
				&& n <= section->Misc.VirtualSize - sizeof(listSortSignature)
				&& memcmp(begin + n, listSortSignature, sizeof(listSortSignature)) == 0 )
			{
				if( listSortFound ) listSortFound = reinterpret_cast<BYTE*>(1);
				else listSortFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(shellEnumSignature)
				&& n <= section->Misc.VirtualSize - sizeof(shellEnumSignature)
				&& memcmp(begin + n, shellEnumSignature, sizeof(shellEnumSignature)) == 0 )
			{
				if( shellEnumFound ) shellEnumFound = reinterpret_cast<BYTE*>(1);
				else shellEnumFound = begin + n;
			}
			if( section->Misc.VirtualSize >= 5 + sizeof(shellOwnerSuffix)
				&& n <= section->Misc.VirtualSize - 5 - sizeof(shellOwnerSuffix)
				&& begin[n] == 0xA1 && memcmp(begin + n + 5, shellOwnerSuffix, sizeof(shellOwnerSuffix)) == 0 )
			{
				if( shellOwnerFound ) shellOwnerFound = reinterpret_cast<BYTE*>(1);
				else shellOwnerFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(directoryRescanSignature)
				&& n <= section->Misc.VirtualSize - sizeof(directoryRescanSignature)
				&& memcmp(begin + n, directoryRescanSignature, sizeof(directoryRescanSignature)) == 0 )
			{
				if( directoryRescanFound ) directoryRescanFound = reinterpret_cast<BYTE*>(1);
				else directoryRescanFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(rescanSignature)
				&& n <= section->Misc.VirtualSize - sizeof(rescanSignature)
				&& memcmp(begin + n, rescanSignature, sizeof(rescanSignature)) == 0 )
			{
				if( rescanFound ) rescanFound = reinterpret_cast<BYTE*>(1);
				else rescanFound = begin + n;
			}
			if( section->Misc.VirtualSize >= 34 && n <= section->Misc.VirtualSize - 34
				&& memcmp(begin + n, getterPrefix, sizeof(getterPrefix)) == 0
				&& memcmp(begin + n + 24, getterSuffix, sizeof(getterSuffix)) == 0 )
			{
				if( getterFound ) return false;
				getterFound = begin + n;
			}
			if( memcmp(begin + n, dataSignature, sizeof(dataSignature)) == 0 )
			{
				if( dataFound ) return false;
				dataFound = begin + n;
			}
			if( section->Misc.VirtualSize >= 28 && n <= section->Misc.VirtualSize - 28
				&& memcmp(begin + n, destroySignature, sizeof(destroySignature)) == 0
				&& memcmp(begin + n + 12, destroyTail, sizeof(destroyTail)) == 0
				&& begin[n + 24] == 0xC6 && begin[n + 25] == 0x45
				&& begin[n + 26] == 0x24 && begin[n + 27] == 0x01 )
			{
				if( destroyFound ) return false;
				destroyFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(parentSignature)
				&& n <= section->Misc.VirtualSize - sizeof(parentSignature)
				&& memcmp(begin + n, parentSignature, sizeof(parentSignature)) == 0 )
			{
				if( parentFound ) return false;
				parentFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(searchSignature)
				&& n <= section->Misc.VirtualSize - sizeof(searchSignature)
				&& memcmp(begin + n, searchSignature, sizeof(searchSignature)) == 0 )
			{
				if( searchFound ) return false;
				searchFound = begin + n;
			}
			if( section->Misc.VirtualSize >= sizeof(actionSignature)
				&& n <= section->Misc.VirtualSize - sizeof(actionSignature)
				&& memcmp(begin + n, actionSignature, sizeof(actionSignature)) == 0 )
			{
				if( actionFound ) return false;
				actionFound = begin + n;
			}
			bool match = true;
			for( size_t j = 0; j < sizeof(signature); ++j )
				if( mask[j] == 'x' && begin[n + j] != signature[j] ) { match = false; break; }
			if( match ) { if( found ) return false; found = begin + n; }
		}
	}
	if( !found || !actionFound || !searchFound || !parentFound || !destroyFound || !dataFound || !getterFound ) return false;
	if( searchFound[0x3D] != 0xE8 ) return false;
	if( actionFound[0x2AE] == 0xE8 )
	{
		LONG removalDisplacement = 0;
		memcpy(&removalDisplacement, actionFound + 0x2AF, sizeof(removalDisplacement));
		if( actionFound + 0x2B3 + removalDisplacement == found )
			gTreeRemovalReturnSite = reinterpret_cast<DWORD>(actionFound + 0x2B3);
	}
	LONG displacement = 0;
	memcpy(&displacement, searchFound + 0x3E, sizeof(displacement));
	if( searchFound + 0x42 + displacement != parentFound ) return false;
	gTreeExperimentCaller = reinterpret_cast<DWORD>(searchFound + 0x42);
	gTreeVerifiedPathGetter = reinterpret_cast<DWORD>(getterFound);
	gTreeVerifiedNodeDestructor = reinterpret_cast<DWORD>(destroyFound);
	gLeeyesInternalOriginalTreeTiming = reinterpret_cast<void (__cdecl*)()>(
		LeeyesInternalCreateHook(found, reinterpret_cast<void*>(&LeeyesTreeTimingHook)));
	if( !gLeeyesInternalOriginalTreeTiming ) return false;
	gLeeyesInternalOriginalTreeAction = reinterpret_cast<void (__cdecl*)()>(
		LeeyesInternalCreateHook(actionFound, reinterpret_cast<void*>(&LeeyesTreeActionHook)));
	if( !gLeeyesInternalOriginalTreeAction )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeTimingHook));
		gLeeyesInternalOriginalTreeTiming = nullptr;
		return false;
	}
	gLeeyesInternalOriginalTreeSearch = reinterpret_cast<void (__cdecl*)()>(
		LeeyesInternalCreateHook(searchFound, reinterpret_cast<void*>(&LeeyesTreeSearchHook)));
	if( !gLeeyesInternalOriginalTreeSearch )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeActionHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeTimingHook));
		gLeeyesInternalOriginalTreeAction = nullptr;
		gLeeyesInternalOriginalTreeTiming = nullptr;
		return false;
	}
	gLeeyesInternalOriginalParentSearch = reinterpret_cast<void (__cdecl*)()>(
		LeeyesInternalCreateHook(parentFound, reinterpret_cast<void*>(&LeeyesParentSearchShadowHook)));
	if( !gLeeyesInternalOriginalParentSearch )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeSearchHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeActionHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeTimingHook));
		gLeeyesInternalOriginalTreeSearch = nullptr;
		gLeeyesInternalOriginalTreeAction = nullptr;
		gLeeyesInternalOriginalTreeTiming = nullptr;
		return false;
	}
	gLeeyesInternalOriginalNodeDestroy = reinterpret_cast<void (__cdecl*)()>(
		LeeyesInternalCreateHook(destroyFound, reinterpret_cast<void*>(&LeeyesTreeNodeDestroyHook)));
	if( !gLeeyesInternalOriginalNodeDestroy )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesParentSearchShadowHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeSearchHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeActionHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeTimingHook));
		gLeeyesInternalOriginalParentSearch = nullptr;
		gLeeyesInternalOriginalTreeSearch = nullptr;
		gLeeyesInternalOriginalTreeAction = nullptr;
		gLeeyesInternalOriginalTreeTiming = nullptr;
		return false;
	}
	gLeeyesInternalOriginalNodeData = reinterpret_cast<void (__cdecl*)()>(
		LeeyesInternalCreateHook(dataFound, reinterpret_cast<void*>(&LeeyesTreeNodeDataHook)));
	if( !gLeeyesInternalOriginalNodeData )
	{
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeNodeDestroyHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesParentSearchShadowHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeSearchHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeActionHook));
		LeeyesInternalRemoveHook(reinterpret_cast<void*>(&LeeyesTreeTimingHook));
		gLeeyesInternalOriginalNodeDestroy = nullptr;
		gLeeyesInternalOriginalParentSearch = nullptr;
		gLeeyesInternalOriginalTreeSearch = nullptr;
		gLeeyesInternalOriginalTreeAction = nullptr;
		gLeeyesInternalOriginalTreeTiming = nullptr;
		return false;
	}
	// This pass-through probe is independent of the normal diagnostic hooks.
	// A missing or ambiguous signature must never affect their installation.
	if( stateNavigationFound && stateNavigationFound != reinterpret_cast<BYTE*>(1)
		&& ::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_WORKER", nullptr, 0) )
	{
		gLeeyesOriginalStateNavigation = reinterpret_cast<void (__cdecl*)()>(
			LeeyesInternalCreateHook(stateNavigationFound, reinterpret_cast<void*>(&LeeyesStateNavigationHook)));
		LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(stateNavigationFound),
			reinterpret_cast<DWORD>(gLeeyesOriginalStateNavigation), 0xF000000F);
	}
	if( statePostCount == 2 && ::GetEnvironmentVariableA("LEYEES_STATE_SHADOW_WORKER", nullptr, 0) )
	{
		BYTE* thunk = image + 0x8568;
		const BYTE thunkPrefix[] = { 0xFF,0x25,0x04,0x27,0x5D,0x00 };
		LONG firstRelative = 0, secondRelative = 0;
		memcpy(&firstRelative, statePostSites[0] + 1, sizeof(firstRelative));
		memcpy(&secondRelative, statePostSites[1] + 1, sizeof(secondRelative));
		if( memcmp(thunk, thunkPrefix, sizeof(thunkPrefix)) == 0
			&& statePostSites[0] + 5 + firstRelative == thunk
			&& statePostSites[1] + 5 + secondRelative == thunk )
		{
			DWORD protect0 = 0, protect1 = 0;
			if( ::VirtualProtect(statePostSites[0], 5, PAGE_EXECUTE_READWRITE, &protect0) )
			{
				if( ::VirtualProtect(statePostSites[1], 5, PAGE_EXECUTE_READWRITE, &protect1) )
				{
					gLeeyesStateShadowPostThunk = reinterpret_cast<DWORD>(thunk);
					for( unsigned j = 0; j < 2; ++j )
					{
						const LONG relative = static_cast<LONG>(reinterpret_cast<DWORD>(&LeeyesStateShadowPostHook)
							- reinterpret_cast<DWORD>(statePostSites[j] + 5));
						memcpy(statePostSites[j] + 1, &relative, sizeof(relative));
						::FlushInstructionCache(::GetCurrentProcess(), statePostSites[j], 5);
					}
					::VirtualProtect(statePostSites[1], 5, protect1, &protect1);
					LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(statePostSites[0]),
						reinterpret_cast<DWORD>(statePostSites[1]), 0xF000000E);
				}
				::VirtualProtect(statePostSites[0], 5, protect0, &protect0);
			}
		}
	}
	if( listSortFound && listSortFound != reinterpret_cast<BYTE*>(1)
		&& ::GetEnvironmentVariableA("LEYEES_LIST_SORT_PROBE", nullptr, 0) != 0 )
	{
		gLeeyesInternalOriginalListSortProbe = reinterpret_cast<void (__cdecl*)()>(
			LeeyesInternalCreateHook(listSortFound, reinterpret_cast<void*>(&LeeyesListSortProbeHook)));
		LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(listSortFound),
			reinterpret_cast<DWORD>(gLeeyesInternalOriginalListSortProbe), 0xF000000D);
	}
	if( rescanFound && rescanFound != reinterpret_cast<BYTE*>(1)
		&& ::GetEnvironmentVariableW(L"LEYEES_TREE_RESCAN_PROBE", nullptr, 0) > 1 )
	{
		gLeeyesInternalOriginalTreeRescanProbe = reinterpret_cast<void (__cdecl*)()>(
			LeeyesInternalCreateHook(rescanFound, reinterpret_cast<void*>(&LeeyesTreeRescanProbeHook)));
		LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(rescanFound),
			reinterpret_cast<DWORD>(gLeeyesInternalOriginalTreeRescanProbe), 0);
		if( directoryRescanFound && directoryRescanFound != reinterpret_cast<BYTE*>(1) )
		{
			gLeeyesInternalTreeDirectoryRescan = reinterpret_cast<void (__cdecl*)()>(directoryRescanFound);
			LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(directoryRescanFound), 0, 0xF0000005);
		}
		if( shellOwnerFound && shellOwnerFound != reinterpret_cast<BYTE*>(1)
			&& shellEnumFound && shellEnumFound != reinterpret_cast<BYTE*>(1)
			&& ::GetEnvironmentVariableA("LEYEES_TREE_SILENT_OWNER", nullptr, 0) != 0 )
		{
			DWORD oldProtect = 0;
			if( ::VirtualProtect(shellOwnerFound, 10, PAGE_EXECUTE_READWRITE, &oldProtect) )
			{
				memcpy(&gTreeShellApplicationAddress, shellOwnerFound + 1, sizeof(DWORD));
				gTreeShellOwnerContinuation = reinterpret_cast<DWORD>(shellOwnerFound + 10);
				BYTE patch[10] = { 0xE9,0,0,0,0,0x90,0x90,0x90,0x90,0x90 };
				const LONG relative = static_cast<LONG>(reinterpret_cast<DWORD>(&LeeyesTreeShellOwnerHook)
					- reinterpret_cast<DWORD>(shellOwnerFound + 5));
				memcpy(patch + 1, &relative, sizeof(relative));
				memcpy(shellOwnerFound, patch, sizeof(patch));
				::VirtualProtect(shellOwnerFound, 10, oldProtect, &oldProtect);
				::FlushInstructionCache(::GetCurrentProcess(), shellOwnerFound, sizeof(patch));
				LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(shellOwnerFound), gTreeShellApplicationAddress, 0xF0000007);
				DWORD enumProtect = 0;
				if( ::VirtualProtect(shellEnumFound, 5, PAGE_EXECUTE_READWRITE, &enumProtect) )
				{
					gTreeShellEnumContinuation = reinterpret_cast<DWORD>(shellEnumFound + 5);
					BYTE enumPatch[5] = { 0xE9,0,0,0,0 };
					const LONG enumRelative = static_cast<LONG>(reinterpret_cast<DWORD>(&LeeyesTreeShellEnumHook)
						- reinterpret_cast<DWORD>(shellEnumFound + 5));
					memcpy(enumPatch + 1, &enumRelative, sizeof(enumRelative));
					memcpy(shellEnumFound, enumPatch, sizeof(enumPatch));
					::VirtualProtect(shellEnumFound, 5, enumProtect, &enumProtect);
					::FlushInstructionCache(::GetCurrentProcess(), shellEnumFound, sizeof(enumPatch));
					gTreeSilentHooksReady = true;
					LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(shellEnumFound), 0, 0xF0000009);
				}
			}
		}
	}
	if( ::GetEnvironmentVariableA("LEYEES_TREE_REPLAY_SINGLE", nullptr, 0) )
	{
		// Verified 2.6.1 call site plus its immediately adjacent instructions.
		// This opt-in diagnostic pins the DLL because the patch lives for the
		// isolated process lifetime. A different binary is left untouched.
		const BYTE context[] = { 0x8b,0x7d,0xfc,0x8b,0x47,0x1c,0xff,0x57,0x18,
			0x8b,0x45,0xfc,0x80,0x78,0x25,0x00 };
		const BYTE nullRecordBranch[] = { 0x74, 0x37 }; // je 0x004C960A
		BYTE* contextSite = image + 0x000c9601;
		HMODULE pinned = nullptr;
		if( reinterpret_cast<ULONG_PTR>(image) == 0x00400000
			&& nt->OptionalHeader.SizeOfImage >= 0x000c9601 + sizeof(context)
			&& memcmp(contextSite, context, sizeof(context)) == 0
			&& memcmp(contextSite - 0x30, nullRecordBranch, sizeof(nullRecordBranch)) == 0
			&& ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&LeeyesDiagnosticTreeCallbackHook), &pinned) )
		{
			// Replace the preceding mov and the call, leaving 0x004C960A
			// untouched because the notifier's null-record branch targets it.
			BYTE* site = contextSite + 3;
			DWORD protection = 0;
			if( ::VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &protection) )
			{
				gLeeyesDiagnosticTreeCallbackContinuation = reinterpret_cast<DWORD>(site + 6);
				BYTE patch[6] = { 0xe9, 0, 0, 0, 0, 0x90 };
				const LONG relative = static_cast<LONG>(reinterpret_cast<DWORD>(&LeeyesDiagnosticTreeCallbackHook)
					- reinterpret_cast<DWORD>(site + 5));
				memcpy(patch + 1, &relative, sizeof(relative));
				memcpy(site, patch, sizeof(patch));
				::FlushInstructionCache(::GetCurrentProcess(), site, sizeof(patch));
				::VirtualProtect(site, 6, protection, &protection);
				LeeyesTreeRescanObserve(reinterpret_cast<DWORD>(site), 0, 0xF0000010);
			}
		}
	}
	return true;
}
#endif

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
