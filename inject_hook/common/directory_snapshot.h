#pragma once

// Filesystem snapshot; no Leeyes hooks or UI calls are made here.
// The caller must run it off the UI thread and separately track watcher
// generations/overflow: filesystem enumeration is not an atomic snapshot.
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

namespace DirectorySnapshot
{
struct Handle
{
	HANDLE value;
	bool search;
	Handle(HANDLE handle, bool isSearch = false) : value(handle), search(isSearch) {}
	~Handle() { if( value && value != INVALID_HANDLE_VALUE ) { if( search ) ::FindClose(value); else ::CloseHandle(value); } }
	Handle(const Handle&) = delete;
	Handle& operator=(const Handle&) = delete;
};
struct Entry
{
	std::wstring name;
	DWORD attributes;
	ULONGLONG size, created, written;
};
struct State
{
	DWORD volume = 0;
	ULONGLONG directoryId = 0;
	std::vector<Entry> entries;
};
enum class ChangeKind { Added, Removed, MetadataChanged };
struct Change
{
	ChangeKind kind;
	// Indices refer to the immutable input states. A missing side is SIZE_MAX.
	size_t before, after;
};
enum class DiffResult { Complete, DifferentDirectory, InvalidState, LimitExceeded };
inline ULONGLONG Combine(DWORD high, DWORD low)
{
	return (static_cast<ULONGLONG>(high) << 32) | low;
}
inline bool Same(const State& a, const State& b)
{
	// Zero is also the default/unavailable identity. Two failed or never-read
	// states must not authorize an unchanged-state decision.
	if( !a.directoryId || !b.directoryId || a.volume != b.volume
		|| a.directoryId != b.directoryId || a.entries.size() != b.entries.size() ) return false;
	for( size_t i = 0; i < a.entries.size(); ++i )
	{
		const Entry& x = a.entries[i]; const Entry& y = b.entries[i];
		if( x.name != y.name || x.attributes != y.attributes || x.size != y.size
			|| x.created != y.created || x.written != y.written ) return false;
	}
	return true;
}
// A strict subset with byte-for-byte equal retained metadata is the one
// change shape for which removing existing rows cannot disturb their previous
// relative sort order. This does not establish unchanged file contents.
inline bool OnlyRemovals(const State& before, const State& after)
{
	if( !before.directoryId || !after.directoryId || before.volume != after.volume
		|| before.directoryId != after.directoryId || before.entries.size() <= after.entries.size() ) return false;
	for( size_t i = 1; i < before.entries.size(); ++i )
		if( !(before.entries[i - 1].name < before.entries[i].name) ) return false;
	for( size_t i = 1; i < after.entries.size(); ++i )
		if( !(after.entries[i - 1].name < after.entries[i].name) ) return false;
	size_t i = 0;
	for( const Entry& y : after.entries )
	{
		while( i < before.entries.size() && before.entries[i].name < y.name ) ++i;
		if( i == before.entries.size() ) return false;
		const Entry& x = before.entries[i++];
		if( x.name != y.name || x.attributes != y.attributes || x.size != y.size
			|| x.created != y.created || x.written != y.written ) return false;
	}
	return true;
}
// Compare by exact wide name; rename is deliberately a removal and addition.
// Unchanged metadata cannot establish unchanged contents. The caller must
// retain the original notification path for modifications that cannot safely
// be reconstructed, and must reject stale snapshots using watcher generations.
// On anything except Complete, no partial changes are returned.
inline DiffResult Diff(const State& before, const State& after,
	size_t maximumChanges, std::vector<Change>& changes)
{
	changes.clear();
	if( !before.directoryId || !after.directoryId ) return DiffResult::InvalidState;
	if( before.volume != after.volume || before.directoryId != after.directoryId )
		return DiffResult::DifferentDirectory;
	for( size_t i = 1; i < before.entries.size(); ++i )
		if( !(before.entries[i-1].name < before.entries[i].name) ) return DiffResult::InvalidState;
	for( size_t i = 1; i < after.entries.size(); ++i )
		if( !(after.entries[i-1].name < after.entries[i].name) ) return DiffResult::InvalidState;
	std::vector<Change> result;
	size_t i = 0, j = 0;
	while( i < before.entries.size() || j < after.entries.size() )
	{
		Change change = {};
		bool changed = true;
		if( j == after.entries.size() || (i < before.entries.size()
			&& before.entries[i].name < after.entries[j].name) )
			change = { ChangeKind::Removed, i++, SIZE_MAX };
		else if( i == before.entries.size() || after.entries[j].name < before.entries[i].name )
			change = { ChangeKind::Added, SIZE_MAX, j++ };
		else
		{
			const Entry& x = before.entries[i]; const Entry& y = after.entries[j];
			changed = x.attributes != y.attributes || x.size != y.size
				|| x.created != y.created || x.written != y.written;
			change = { ChangeKind::MetadataChanged, i++, j++ };
		}
		if( changed )
		{
			if( result.size() == maximumChanges ) return DiffResult::LimitExceeded;
			result.push_back(change);
		}
	}
	changes.swap(result);
	return DiffResult::Complete;
}
inline HANDLE OpenDirectory(const std::wstring& path)
{
	return ::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}
inline bool Identity(HANDLE h, DWORD& volume, ULONGLONG& id, ULONGLONG& written, DWORD& error)
{
	BY_HANDLE_FILE_INFORMATION info = {};
	if( !::GetFileInformationByHandle(h, &info) ) { error = ::GetLastError(); return false; }
	if( !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ) { error = ERROR_DIRECTORY; return false; }
	volume = info.dwVolumeSerialNumber;
	id = Combine(info.nFileIndexHigh, info.nFileIndexLow);
	written = Combine(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime);
	if( !id ) { error = ERROR_NOT_SUPPORTED; return false; }
	return true;
}
// On any failure, leaves output untouched and reports the Win32 error.
inline bool Read(const std::wstring& path, State& output, DWORD& error,
	size_t maximumEntries = 200000)
{
	error = ERROR_SUCCESS;
	if( path.empty() ) { error = ERROR_INVALID_NAME; return false; }
	Handle before(OpenDirectory(path));
	if( before.value == INVALID_HANDLE_VALUE ) { error = ::GetLastError(); return false; }
	State next;
	ULONGLONG initialWrite = 0;
	if( !Identity(before.value, next.volume, next.directoryId, initialWrite, error) ) return false;
	#ifdef DIRECTORY_SNAPSHOT_TEST_HOOK
	DIRECTORY_SNAPSHOT_TEST_HOOK(path);
	#endif
	std::wstring pattern = path;
	if( pattern.back() != L'\\' && pattern.back() != L'/' ) pattern += L'\\';
	pattern += L'*';
	WIN32_FIND_DATAW data = {};
	Handle search(::FindFirstFileW(pattern.c_str(), &data), true);
	if( search.value == INVALID_HANDLE_VALUE )
	{
		error = ::GetLastError();
		if( error != ERROR_FILE_NOT_FOUND ) return false;
		error = ERROR_SUCCESS;
	}
	else
	{
		do
		{
			if( wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0 ) continue;
			if( next.entries.size() >= maximumEntries ) { error = ERROR_BUFFER_OVERFLOW; return false; }
			Entry item;
			item.name = data.cFileName; item.attributes = data.dwFileAttributes;
			item.size = Combine(data.nFileSizeHigh, data.nFileSizeLow);
			item.created = Combine(data.ftCreationTime.dwHighDateTime, data.ftCreationTime.dwLowDateTime);
			item.written = Combine(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
			next.entries.push_back(item);
		} while( ::FindNextFileW(search.value, &data) );
		error = ::GetLastError();
		if( error != ERROR_NO_MORE_FILES ) return false;
		error = ERROR_SUCCESS;
	}
	Handle after(OpenDirectory(path));
	if( after.value == INVALID_HANDLE_VALUE ) { error = ::GetLastError(); return false; }
	DWORD volume = 0; ULONGLONG id = 0, finalWrite = 0;
	const bool valid = Identity(after.value, volume, id, finalWrite, error);
	if( !valid ) return false;
	if( volume != next.volume || id != next.directoryId || finalWrite != initialWrite )
	{ error = ERROR_RETRY; return false; }
	std::sort(next.entries.begin(), next.entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
	output = std::move(next);
	return true;
}
}
