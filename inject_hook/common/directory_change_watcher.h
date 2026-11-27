#pragma once

// Experimental building block for an isolated worker process, never the UI
// thread. Cancellation and filesystem calls can block indefinitely on a
// broken network provider. The caller must supervise the entire process.
#include "directory_snapshot.h"

class DirectoryChangeWatcher
{
public:
	enum class Result { Changed, Timeout, Overflow, Invalidated, Error };
	DirectoryChangeWatcher() : event_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
	~DirectoryChangeWatcher() { Close(); }
	DirectoryChangeWatcher(const DirectoryChangeWatcher&) = delete;
	DirectoryChangeWatcher& operator=(const DirectoryChangeWatcher&) = delete;

	// subtree=false is appropriate when snapshots represent only direct entries.
	bool Open(const std::wstring& path, DWORD& error, bool subtree = true)
	{
		Close(); error = ERROR_SUCCESS;
		if( !event_.value || path.empty() ) { error = event_.value ? ERROR_INVALID_NAME : ERROR_NOT_ENOUGH_MEMORY; return false; }
		directory_ = ::CreateFileW(path.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
			FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
		if( directory_ == INVALID_HANDLE_VALUE ) { error = ::GetLastError(); return false; }
		ULONGLONG write = 0;
		if( !DirectorySnapshot::Identity(directory_, volume_, id_, write, error) ) { Close(); return false; }
		try { path_ = path; subtree_ = subtree; }
		catch( ... ) { error = ERROR_NOT_ENOUGH_MEMORY; Close(); return false; }
		if( !Arm(error) ) { Close(); return false; }
		return true;
	}

	// A Changed result is a dirty hint, not a complete event history. The
	// consumer must independently verify identity and take a fresh snapshot.
	Result Wait(DWORD timeoutMs, DWORD& error)
	{
		error = ERROR_SUCCESS;
		if( directory_ == INVALID_HANDLE_VALUE || !armed_ ) { error = ERROR_INVALID_HANDLE; return Result::Error; }
		const DWORD wait = ::WaitForSingleObject(event_.value, timeoutMs);
		if( wait == WAIT_TIMEOUT ) return CheckIdentity(error);
		if( wait != WAIT_OBJECT_0 ) { error = ::GetLastError(); return Result::Error; }
		DWORD transferred = 0;
		if( !::GetOverlappedResult(directory_, &overlapped_, &transferred, FALSE) )
		{
			error = ::GetLastError(); armed_ = false;
			return error == ERROR_NOTIFY_ENUM_DIR ? Result::Overflow : Result::Error;
		}
		armed_ = false;
		if( !transferred ) { error = ERROR_NOTIFY_ENUM_DIR; return Result::Overflow; }
		if( !Arm(error) ) return Result::Error;
		if( CheckIdentity(error) != Result::Timeout ) return Result::Invalidated;
		return Result::Changed;
	}

	// Called at snapshot boundaries and on a timer, even after a quiet wait.
	// A watch on a moved directory may remain attached to its old identity.
	Result CheckIdentity(DWORD& error) const
	{
		error = ERROR_SUCCESS;
		if( directory_ == INVALID_HANDLE_VALUE || path_.empty() ) { error = ERROR_INVALID_HANDLE; return Result::Error; }
		DirectorySnapshot::Handle current(DirectorySnapshot::OpenDirectory(path_));
		if( current.value == INVALID_HANDLE_VALUE ) { error = ::GetLastError(); return Result::Invalidated; }
		DWORD volume = 0; ULONGLONG id = 0, write = 0;
		if( !DirectorySnapshot::Identity(current.value, volume, id, write, error) ) return Result::Invalidated;
		return volume == volume_ && id == id_ ? Result::Timeout : Result::Invalidated;
	}
	bool Matches(const DirectorySnapshot::State& state) const
	{
		return directory_ != INVALID_HANDLE_VALUE && id_ && state.directoryId == id_ && state.volume == volume_;
	}

	void Close()
	{
		if( directory_ != INVALID_HANDLE_VALUE )
		{
			if( armed_ )
			{
				::CancelIoEx(directory_, &overlapped_);
				DWORD ignored = 0; ::GetOverlappedResult(directory_, &overlapped_, &ignored, TRUE);
			}
			::CloseHandle(directory_); directory_ = INVALID_HANDLE_VALUE;
		}
		armed_ = false; path_.clear(); volume_ = 0; id_ = 0; subtree_ = true;
	}
private:
	bool Arm(DWORD& error)
	{
		::ResetEvent(event_.value);
		::ZeroMemory(&overlapped_, sizeof(overlapped_)); overlapped_.hEvent = event_.value;
		const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME
			| FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE
			| FILE_NOTIFY_CHANGE_CREATION | FILE_NOTIFY_CHANGE_SECURITY;
		if( !::ReadDirectoryChangesW(directory_, buffer_, sizeof(buffer_), subtree_ ? TRUE : FALSE,
			filter, nullptr, &overlapped_, nullptr) )
		{ error = ::GetLastError(); return false; }
		armed_ = true; return true;
	}
	HANDLE directory_ = INVALID_HANDLE_VALUE;
	DirectorySnapshot::Handle event_;
	OVERLAPPED overlapped_ = {};
	alignas(DWORD) BYTE buffer_[64 * 1024] = {};
	std::wstring path_;
	DWORD volume_ = 0;
	ULONGLONG id_ = 0;
	bool armed_ = false;
	bool subtree_ = true;
};
