#include "../stdafx.h"
#include "file_move_batch.h"
#include "../../common/ansi_path_conversion.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <CommCtrl.h>
#include <algorithm>
#include <iterator>
#include <limits.h>
#include <set>

extern HMODULE gInjectedModule;
extern "C" void __cdecl LeeyesInternalInvokeFullResync(void* listView);
extern "C" volatile LONG gLeeyesInternalLowItemNullFallbackCount;
extern "C" volatile LONG gLeeyesInternalLowItemGuardActive;

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
	char currentDirectory[kScopePathCapacity];
	ScopeDirectoryResult scopeDirectories[kMaximumScopeDirectories];
	DWORD scopeDirectoryCount;
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
	const int page = static_cast<int>(::SendMessageA(snapshot.window, LVM_GETCOUNTPERPAGE, 0, 0));
	snapshot.focusedWasVisible = focused >= top && top >= 0 && page > 0 && focused < top + page;
	return snapshot;
}

bool RestoreListSelectionByFind(const ListSelectionSnapshot& snapshot, int itemCount,
	DWORD& restored, int& focusedIndex)
{
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
	bool usedFast = false;
	usedFast = RestoreListSelectionByFind(snapshot, itemCount, restored, focusedIndex);
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
	{
		const bool listKnown = FindDisplayedListWindow(gDrainState.currentListWindow);
		WriteDiagnostic("scope_owner list_known=%d hwnd=%p",
			listKnown ? 1 : 0, gDrainState.currentListWindow);
		WriteDiagnostic("drain_begin queued=%lu threshold=%lu",
			static_cast<unsigned long>(gDrainState.queuedChanges),
			static_cast<unsigned long>(gBatchThreshold));
	}
}

BOOL LeeyesInternalShouldDeferFileChangeImpl(
	void* dispatchOwner, DWORD rawAction, DWORD path0, DWORD path1,
	void* returnAddress)
{
	if( !gDrainState.suppress ) return FALSE;
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
	if( !RememberDirtyList(dispatchOwner) ) return FALSE;
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
	return deferred;
}

extern "C" void __cdecl LeeyesInternalEndChangeQueueDrain()
{
	if( gDrainState.depth == 0 ) return;
	if( --gDrainState.depth != 0 ) return;

	void* dirtyLists[kMaximumDirtyLists] = {};
	const DWORD dirtyListCount = gDrainState.dirtyListCount;
	const DWORD queuedChanges = gDrainState.queuedChanges;
	const DWORD suppressedChanges = gDrainState.suppressedChanges;
	const DWORD outOfScopeChanges = gDrainState.outOfScopeChanges;
	const DWORD unknownScopeChanges = gDrainState.unknownScopeChanges;
	const LONG lowItemNullFallbacks = gLeeyesInternalLowItemNullFallbackCount;
	const LONG lowItemGuardActive = gLeeyesInternalLowItemGuardActive;
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
	int finalListCount = -1;
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
}

}
