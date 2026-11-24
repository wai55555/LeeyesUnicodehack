#pragma once

#include "state_snapshot_process.h"
#include "watched_snapshot_stream.h"

// Single-threaded transport for a supervised watcher child. All methods,
// including Stop, belong on a background thread. If Stop does not confirm
// termination, the object retains its process handle and Start refuses to
// create another child until the previous process is known to have exited.
// If Stop returns false, the owner must keep this object alive and must not
// replace it with another instance while that child may still be running.
class WatchedSnapshotProcess
{
public:
	WatchedSnapshotProcess() : read_(nullptr), control_(nullptr), process_(nullptr), job_(nullptr) {}
	~WatchedSnapshotProcess() { Stop(0); }
	WatchedSnapshotProcess(const WatchedSnapshotProcess&) = delete;
	WatchedSnapshotProcess& operator=(const WatchedSnapshotProcess&) = delete;

	bool Start(const std::wstring& launcher, const std::wstring& path, DWORD& error, bool collectEntryIdentities = false)
	{
		error = ERROR_SUCCESS;
		if( process_.value )
		{
			if( ::WaitForSingleObject(process_.value, 0) != WAIT_OBJECT_0 ) { error = ERROR_BUSY; return false; }
			::CloseHandle(process_.value); process_.value = nullptr;
		}
		if( read_.value || control_.value || job_.value || launcher.empty() || path.empty() ) { error = ERROR_INVALID_PARAMETER; return false; }
		SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
		HANDLE rawRead = nullptr, rawWrite = nullptr;
		if( !::CreatePipe(&rawRead, &rawWrite, &security, 0) ) { error = ::GetLastError(); return false; }
		read_.value = rawRead; DirectorySnapshot::Handle write(rawWrite);
		if( !::SetHandleInformation(read_.value, HANDLE_FLAG_INHERIT, 0) ) { error = ::GetLastError(); ResetTransport(); return false; }
		HANDLE rawControlRead = nullptr, rawControlWrite = nullptr;
		if( !::CreatePipe(&rawControlRead, &rawControlWrite, &security, 0) ) { error = ::GetLastError(); ResetTransport(); return false; }
		DirectorySnapshot::Handle controlRead(rawControlRead); control_.value = rawControlWrite;
		if( !::SetHandleInformation(control_.value, HANDLE_FLAG_INHERIT, 0) ) { error = ::GetLastError(); ResetTransport(); return false; }
		job_.value = ::CreateJobObjectW(nullptr, nullptr);
		if( !job_.value ) { error = ::GetLastError(); ResetTransport(); return false; }
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if( !::SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) )
		{ error = ::GetLastError(); ResetTransport(); return false; }
		SIZE_T size = 0; ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		if( !size ) { error = ::GetLastError(); ResetTransport(); return false; }
		try
		{
			std::vector<BYTE> storage(size);
			auto attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
			if( !::InitializeProcThreadAttributeList(attributes, 1, 0, &size) ) { error = ::GetLastError(); ResetTransport(); return false; }
			SnapshotWire::AttributeListGuard guard(attributes);
			HANDLE inherited[] = { rawWrite, rawControlRead };
			if( !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) )
			{ error = ::GetLastError(); ResetTransport(); return false; }
			wchar_t number[32] = {};
			swprintf_s(number, L"%llu", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(rawWrite)));
			wchar_t controlNumber[32] = {};
			swprintf_s(controlNumber, L"%llu", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(rawControlRead)));
			const std::wstring command = SnapshotWire::QuoteArgument(launcher) + L" --internal-state-watch " + number + L" " + SnapshotWire::QuoteArgument(path) + L" " + controlNumber
				+ (collectEntryIdentities ? L" --entry-identities" : L"");
			std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(L'\0');
			STARTUPINFOEXW startup = {}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attributes;
			PROCESS_INFORMATION info = {};
			if( !::CreateProcessW(launcher.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
				CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &startup.StartupInfo, &info) )
			{ error = ::GetLastError(); ResetTransport(); return false; }
			process_.value = info.hProcess; DirectorySnapshot::Handle thread(info.hThread);
			if( !::AssignProcessToJobObject(job_.value, process_.value) )
			{
				error = ::GetLastError(); ::TerminateProcess(process_.value, 1); ResetTransport(); return false;
			}
			::CloseHandle(write.value); write.value = nullptr;
			if( ::ResumeThread(thread.value) == static_cast<DWORD>(-1) )
			{ error = ::GetLastError(); Stop(0); return false; }
			return true;
		}
		catch( const std::bad_alloc& ) { error = ERROR_NOT_ENOUGH_MEMORY; Stop(0); return false; }
		catch( ... ) { error = ERROR_GEN_FAILURE; Stop(0); return false; }
	}
	bool RequestRefresh(std::uint64_t request, DWORD& error)
	{
		error = ERROR_SUCCESS;
		if( !control_.value || !request ) { error = ERROR_INVALID_PARAMETER; return false; }
		DWORD written = 0;
		if( !::WriteFile(control_.value, &request, sizeof(request), &written, nullptr) || written != sizeof(request) )
		{ error = ::GetLastError(); if( !error ) error = ERROR_WRITE_FAULT; return false; }
		return true;
	}

	// Returns false on timeout, EOF, invalid framing, or transport error. The
	// receiver must invalidate candidates on every error other than timeout;
	// the owner separately enforces a bounded heartbeat/overall deadline.
	bool Next(DWORD timeoutMs, WatchedSnapshotStream::Header& header, std::vector<BYTE>& body, DWORD& error)
	{
		error = ERROR_SUCCESS;
		if( !read_.value || !process_.value ) { error = ERROR_INVALID_HANDLE; return false; }
		const ULONGLONG started = ::GetTickCount64();
		try
		{
			for( ;; )
			{
				if( pending_.size() >= sizeof(header) )
				{
					memcpy(&header, pending_.data(), sizeof(header));
					if( header.magic != WatchedSnapshotStream::kMagic || header.length > SnapshotWire::kMaxBytes )
					{ error = ERROR_INVALID_DATA; return false; }
					const size_t total = sizeof(header) + header.length;
					if( pending_.size() >= total )
					{
						body.assign(pending_.begin() + sizeof(header), pending_.begin() + total);
						pending_.erase(pending_.begin(), pending_.begin() + total); return true;
					}
				}
				DWORD available = 0;
				if( !::PeekNamedPipe(read_.value, nullptr, 0, nullptr, &available, nullptr) ) { error = ::GetLastError(); return false; }
				if( available )
				{
					const size_t limit = SnapshotWire::kMaxBytes + sizeof(header);
					if( pending_.size() > limit || available > limit - pending_.size() ) { error = ERROR_BUFFER_OVERFLOW; return false; }
					const size_t offset = pending_.size(); pending_.resize(offset + available); DWORD count = 0;
					if( !::ReadFile(read_.value, pending_.data() + offset, available, &count, nullptr) || !count )
					{ error = ::GetLastError(); if( !error ) error = ERROR_READ_FAULT; return false; }
					pending_.resize(offset + count); continue;
				}
				const DWORD wait = ::WaitForSingleObject(process_.value, 0);
				if( wait == WAIT_OBJECT_0 ) { error = ERROR_BROKEN_PIPE; return false; }
				if( wait != WAIT_TIMEOUT ) { error = ::GetLastError(); return false; }
				if( ::GetTickCount64() - started >= timeoutMs ) { error = ERROR_TIMEOUT; return false; }
				::Sleep(5);
			}
		}
		catch( const std::bad_alloc& ) { error = ERROR_NOT_ENOUGH_MEMORY; return false; }
		catch( ... ) { error = ERROR_GEN_FAILURE; return false; }
	}

	bool Stop(DWORD timeoutMs)
	{
		if( job_.value ) ::TerminateJobObject(job_.value, 1);
		ResetTransport();
		if( !process_.value ) return true;
		if( ::WaitForSingleObject(process_.value, timeoutMs) != WAIT_OBJECT_0 ) return false;
		::CloseHandle(process_.value); process_.value = nullptr; return true;
	}
	bool Exited() const
	{ return process_.value && ::WaitForSingleObject(process_.value, 0) == WAIT_OBJECT_0; }
private:
	void ResetTransport()
	{
		if( read_.value ) { ::CloseHandle(read_.value); read_.value = nullptr; }
		if( control_.value ) { ::CloseHandle(control_.value); control_.value = nullptr; }
		if( job_.value ) { ::CloseHandle(job_.value); job_.value = nullptr; }
		pending_.clear();
	}
	DirectorySnapshot::Handle read_, control_, process_, job_;
	std::vector<BYTE> pending_;
};
