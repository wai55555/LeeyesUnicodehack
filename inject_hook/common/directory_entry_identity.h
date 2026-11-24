#pragma once

#include "directory_snapshot.h"

// Optional, isolated-worker-only identity evidence. Opening every entry can
// block on remote providers and is not a substitute for an armed watcher.
// Matching identities do not prove that file contents or descendants stayed
// unchanged while a directory was outside the watched tree.
namespace DirectoryEntryIdentity
{
struct Item { std::wstring name; DWORD volume = 0; ULONGLONG id = 0; };
struct State { DWORD volume = 0; ULONGLONG directoryId = 0; std::vector<Item> items; };
enum class ChangeKind { Added, Removed, Replaced };
struct Change { ChangeKind kind; size_t before, after; };
enum class DiffResult { Complete, DifferentDirectory, InvalidState, LimitExceeded };
enum class ReadResult { Complete, Unavailable, Retry, Failed };

inline bool IsIdentityEvidenceUnavailable(DWORD error)
{
	return error == ERROR_ACCESS_DENIED || error == ERROR_NOT_SUPPORTED
		|| error == ERROR_INVALID_FUNCTION || error == ERROR_PRIVILEGE_NOT_HELD;
}

inline bool Valid(const State& state)
{
	if( !state.directoryId || state.items.size() > 200000 ) return false;
	for( size_t i = 0; i < state.items.size(); ++i )
	{
		const auto& item = state.items[i];
		if( !item.id || item.name.empty() || item.name.size() > 255 || item.name.find(L'\0') != std::wstring::npos
			|| item.name == L"." || item.name == L".." || item.name.find_first_of(L"\\/") != std::wstring::npos
			|| (i && !(state.items[i - 1].name < item.name)) ) return false;
	}
	return true;
}

inline bool Same(const State& left, const State& right)
{
	if( !Valid(left) || !Valid(right) || left.volume != right.volume || left.directoryId != right.directoryId
		|| left.items.size() != right.items.size() ) return false;
	for( size_t i = 0; i < left.items.size(); ++i )
		if( !left.items[i].id || left.items[i].name != right.items[i].name
			|| left.items[i].volume != right.items[i].volume || left.items[i].id != right.items[i].id ) return false;
	return true;
}

// Exact UTF-16 names are compared; a rename is removal plus addition. Equal
// IDs are not evidence that file contents or a directory subtree are equal.
// No partial diff is returned on invalid input or when the limit is exceeded.
inline DiffResult Diff(const State& before, const State& after, size_t maximumChanges, std::vector<Change>& changes)
{
	changes.clear();
	if( !Valid(before) || !Valid(after) ) return DiffResult::InvalidState;
	if( before.volume != after.volume || before.directoryId != after.directoryId ) return DiffResult::DifferentDirectory;
	std::vector<Change> result; size_t i = 0, j = 0;
	while( i < before.items.size() || j < after.items.size() )
	{
		Change change = {}; bool changed = true;
		if( j == after.items.size() || (i < before.items.size() && before.items[i].name < after.items[j].name) )
			change = { ChangeKind::Removed, i++, SIZE_MAX };
		else if( i == before.items.size() || after.items[j].name < before.items[i].name )
			change = { ChangeKind::Added, SIZE_MAX, j++ };
		else
		{
			changed = before.items[i].volume != after.items[j].volume || before.items[i].id != after.items[j].id;
			change = { ChangeKind::Replaced, i++, j++ };
		}
		if( changed )
		{
			if( result.size() == maximumChanges ) return DiffResult::LimitExceeded;
			result.push_back(change);
		}
	}
	changes.swap(result); return DiffResult::Complete;
}

// On failure output is untouched. A caller must keep its watcher armed and
// discard this evidence if the watcher generation changes or overflows.
inline ReadResult ReadDetailed(const std::wstring& path, const DirectorySnapshot::State& snapshot,
	State& output, DWORD& error)
{
	error = ERROR_SUCCESS;
	if( path.empty() || !snapshot.directoryId ) { error = ERROR_INVALID_PARAMETER; return ReadResult::Failed; }
	if( snapshot.entries.size() > 200000 ) { error = ERROR_BUFFER_OVERFLOW; return ReadResult::Failed; }
	try
	{
		DirectorySnapshot::Handle root(DirectorySnapshot::OpenDirectory(path));
		if( root.value == INVALID_HANDLE_VALUE )
		{
			error = ::GetLastError();
			if( error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND )
				return ReadResult::Retry;
			return IsIdentityEvidenceUnavailable(error) ? ReadResult::Unavailable : ReadResult::Failed;
		}
		DWORD volume = 0; ULONGLONG id = 0, write = 0;
		if( !DirectorySnapshot::Identity(root.value, volume, id, write, error) )
			return IsIdentityEvidenceUnavailable(error) ? ReadResult::Unavailable
				: (error == ERROR_RETRY ? ReadResult::Retry : ReadResult::Failed);
		if( volume != snapshot.volume || id != snapshot.directoryId ) { error = ERROR_RETRY; return ReadResult::Retry; }
		State next; next.volume = volume; next.directoryId = id;
		next.items.reserve(snapshot.entries.size());
		std::wstring prefix = path;
		if( prefix.back() != L'\\' && prefix.back() != L'/' ) prefix += L'\\';
		for( size_t i = 0; i < snapshot.entries.size(); ++i )
		{
			const auto& entry = snapshot.entries[i];
			if( entry.name.empty() || entry.name.size() > 255 || entry.name.find(L'\0') != std::wstring::npos
				|| entry.name == L"." || entry.name == L".."
				|| entry.name.find_first_of(L"\\/") != std::wstring::npos
				|| (i && !(snapshot.entries[i - 1].name < entry.name)) ) { error = ERROR_INVALID_DATA; return ReadResult::Failed; }
			if( entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT ) { error = ERROR_NOT_SUPPORTED; return ReadResult::Unavailable; }
			#ifdef DIRECTORY_ENTRY_IDENTITY_TEST_HOOK
			DIRECTORY_ENTRY_IDENTITY_TEST_HOOK(path, entry.name);
			#endif
			DirectorySnapshot::Handle item(::CreateFileW((prefix + entry.name).c_str(), FILE_READ_ATTRIBUTES,
				FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
				FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
			if( item.value == INVALID_HANDLE_VALUE )
			{
				error = ::GetLastError();
				// A child may disappear between enumeration and opening it during
				// an ordinary move. The enclosing watcher invalidates old tickets
				// and checks the target identity before the next attempt.
				if( error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND )
				{ error = ERROR_RETRY; return ReadResult::Retry; }
				return IsIdentityEvidenceUnavailable(error) ? ReadResult::Unavailable
					: (error == ERROR_RETRY ? ReadResult::Retry : ReadResult::Failed);
			}
			BY_HANDLE_FILE_INFORMATION info = {};
			if( !::GetFileInformationByHandle(item.value, &info) )
			{
				error = ::GetLastError();
				return IsIdentityEvidenceUnavailable(error) ? ReadResult::Unavailable
					: (error == ERROR_RETRY ? ReadResult::Retry : ReadResult::Failed);
			}
			if( info.dwFileAttributes != entry.attributes || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
				|| DirectorySnapshot::Combine(info.nFileSizeHigh, info.nFileSizeLow) != entry.size
				|| DirectorySnapshot::Combine(info.ftCreationTime.dwHighDateTime, info.ftCreationTime.dwLowDateTime) != entry.created
				|| DirectorySnapshot::Combine(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime) != entry.written )
			{ error = ERROR_RETRY; return ReadResult::Retry; }
			Item identity; identity.name = entry.name; identity.volume = info.dwVolumeSerialNumber;
			identity.id = DirectorySnapshot::Combine(info.nFileIndexHigh, info.nFileIndexLow);
			if( !identity.id ) { error = ERROR_NOT_SUPPORTED; return ReadResult::Unavailable; }
			next.items.push_back(std::move(identity));
		}
		DirectorySnapshot::State after;
		if( !DirectorySnapshot::Read(path, after, error) )
			return error == ERROR_RETRY ? ReadResult::Retry : ReadResult::Failed;
		if( !DirectorySnapshot::Same(snapshot, after) ) { error = ERROR_RETRY; return ReadResult::Retry; }
		output = std::move(next); return ReadResult::Complete;
	}
	catch( ... ) { error = ERROR_NOT_ENOUGH_MEMORY; return ReadResult::Failed; }
}

// Preserve the original bool API for diagnostics and callers that only need
// complete identity evidence. Unavailable evidence remains a failure here.
inline bool Read(const std::wstring& path, const DirectorySnapshot::State& snapshot,
	State& output, DWORD& error)
{
	return ReadDetailed(path, snapshot, output, error) == ReadResult::Complete;
}
}
