#include "../stdafx.h"
#include "file_move_batch.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <CommCtrl.h>

extern HMODULE gInjectedModule;
extern "C" void __cdecl LeeyesInternalInvokeFullResync(void* listView);

namespace LeeyesInternal
{
namespace
{

const DWORD kMaximumDirtyLists = 8;

struct DrainState
{
	DWORD depth;
	bool suppress;
	bool needsResync;
	DWORD queuedChanges;
	DWORD suppressedChanges;
	void* dirtyLists[kMaximumDirtyLists];
	DWORD dirtyListCount;
};

DWORD gBatchThreshold = 32;
bool gDiagnosticLog = false;
__declspec(thread) DrainState gDrainState = {};

struct ListWindowSearch
{
	DWORD processId;
	HWND bestWindow;
	int bestItemCount;
};

struct ListSelectionSnapshot
{
	HWND window;
	std::map<std::string, DWORD> selectedNames;
	std::string focusedName;
	int originalTopIndex;
	bool focusedWasVisible;
};

void WriteDiagnostic(const char* format, ...)
{
#if UNICODEHACK_PATH_DEBUG
	if( !gDiagnosticLog || !gInjectedModule ) return;
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
#else
	(void)format;
#endif
}

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

ListSelectionSnapshot CaptureListSelection()
{
	ListSelectionSnapshot snapshot = {};
	snapshot.window = FindFileListWindow();
	if( !snapshot.window ) return snapshot;
	int index = -1;
	while( (index = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETNEXTITEM,
		static_cast<WPARAM>(index), LVNI_SELECTED))) >= 0 )
	{
		std::string text;
		if( GetListItemText(snapshot.window, index, text) && !text.empty() )
			++snapshot.selectedNames[text];
	}
	const int focused = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETNEXTITEM,
		static_cast<WPARAM>(-1), LVNI_FOCUSED));
	if( focused >= 0 ) GetListItemText(snapshot.window, focused, snapshot.focusedName);
	const int top = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETTOPINDEX, 0, 0));
	snapshot.originalTopIndex = top;
	const int page = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETCOUNTPERPAGE, 0, 0));
	snapshot.focusedWasVisible = focused >= top && top >= 0 && page > 0 && focused < top + page;
	return snapshot;
}

DWORD RestoreListSelection(const ListSelectionSnapshot& snapshot)
{
	if( !snapshot.window || !::IsWindow(snapshot.window) ) return 0;
	std::map<std::string, DWORD> remaining = snapshot.selectedNames;
	DWORD restored = 0;
	int focusedIndex = -1;
	const int itemCount = static_cast<int>(::SendMessageA(
		snapshot.window, LVM_GETITEMCOUNT, 0, 0));
	const int topIndex = itemCount > 0 && snapshot.originalTopIndex >= 0
		? (snapshot.originalTopIndex < itemCount ? snapshot.originalTopIndex : itemCount - 1)
		: -1;
	for( int index = 0; index < itemCount; ++index )
	{
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
			++restored;
		}
		if( focusedIndex < 0 && !snapshot.focusedName.empty()
			&& text == snapshot.focusedName ) focusedIndex = index;
	}
	if( focusedIndex >= 0 && snapshot.focusedWasVisible )
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
	return restored;
}

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

void ConfigureFileMoveBatch(DWORD threshold, bool diagnosticLog)
{
	gBatchThreshold = threshold < 2 ? 2 : (threshold > 100000 ? 100000 : threshold);
	gDiagnosticLog = diagnosticLog;
	WriteDiagnostic("configured threshold=%lu", static_cast<unsigned long>(gBatchThreshold));
}

void ResetFileMoveBatchState()
{
	gDrainState = DrainState();
	gDiagnosticLog = false;
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
	const int queuedChanges = ReadQueuedChangeCount(notifier);
	if( queuedChanges <= 0 ) return;
	gDrainState.queuedChanges = static_cast<DWORD>(queuedChanges);
	gDrainState.suppress = gDrainState.queuedChanges >= gBatchThreshold;
	if( gDrainState.suppress )
		WriteDiagnostic("drain_begin queued=%lu threshold=%lu",
			static_cast<unsigned long>(gDrainState.queuedChanges),
			static_cast<unsigned long>(gBatchThreshold));
}

extern "C" BOOL __cdecl LeeyesInternalShouldDeferFileChange(
	void* listView, DWORD rawAction, void* returnAddress)
{
	if( !gDrainState.suppress ) return FALSE;
	// The dispatcher has a separate control-action branch (6) in the verified
	// Leeyes 2.6.1 body. It is not a filesystem item change and must continue
	// through Leeyes' own state machine. Unknown future actions are also safer
	// on the original path than in a guessed batch category.
	const DWORD action = rawAction & 0xFF;
	if( action > 5 ) return FALSE;
	if( !RememberDirtyList(listView) ) return FALSE;
	gDrainState.needsResync = true;
	++gDrainState.suppressedChanges;
	if( gDrainState.suppressedChanges <= 4 )
		WriteDiagnostic("defer index=%lu action=%lu caller=%p list=%p",
			static_cast<unsigned long>(gDrainState.suppressedChanges),
			static_cast<unsigned long>(rawAction & 0xFF), returnAddress, listView);
	return TRUE;
}

extern "C" void __cdecl LeeyesInternalEndChangeQueueDrain()
{
	if( gDrainState.depth == 0 ) return;
	if( --gDrainState.depth != 0 ) return;

	void* dirtyLists[kMaximumDirtyLists] = {};
	const DWORD dirtyListCount = gDrainState.dirtyListCount;
	const DWORD queuedChanges = gDrainState.queuedChanges;
	const DWORD suppressedChanges = gDrainState.suppressedChanges;
	const bool needsResync = gDrainState.suppress && gDrainState.needsResync;
	if( dirtyListCount != 0 )
		memcpy(dirtyLists, gDrainState.dirtyLists,
			dirtyListCount * sizeof(gDrainState.dirtyLists[0]));
	// Disable suppression before entering Leeyes' normal resync path so any
	// synchronous work it causes cannot be mistaken for queue-drain callbacks.
	gDrainState = DrainState();

	DWORD refreshed = 0;
	DWORD restoredSelections = 0;
	if( needsResync )
	{
		const ListSelectionSnapshot selection = CaptureListSelection();
		for( DWORD index = 0; index < dirtyListCount; ++index )
		{
			if( !dirtyLists[index] ) continue;
			LeeyesInternalInvokeFullResync(dirtyLists[index]);
			++refreshed;
		}
		restoredSelections = RestoreListSelection(selection);
	}
	if( queuedChanges >= gBatchThreshold )
		WriteDiagnostic("drain_end queued=%lu deferred=%lu refreshed=%lu selection=%lu",
			static_cast<unsigned long>(queuedChanges),
			static_cast<unsigned long>(suppressedChanges),
			static_cast<unsigned long>(refreshed),
			static_cast<unsigned long>(restoredSelections));
}

}
