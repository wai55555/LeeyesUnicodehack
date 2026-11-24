#pragma once

#include "state_snapshot_wire.h"
#include <memory>

namespace SnapshotWire
{
struct AttributeListGuard
{
	LPPROC_THREAD_ATTRIBUTE_LIST value;
	explicit AttributeListGuard(LPPROC_THREAD_ATTRIBUTE_LIST list) : value(list) {}
	~AttributeListGuard() { if( value ) ::DeleteProcThreadAttributeList(value); }
	AttributeListGuard(const AttributeListGuard&) = delete;
	AttributeListGuard& operator=(const AttributeListGuard&) = delete;
};
struct ProcessResult
{
	DWORD error = ERROR_SUCCESS;
	// If false, the caller must not start another child until it knows this
	// one has exited; a terminated process can remain pending on kernel I/O.
	bool cleanupConfirmed = true;
	std::shared_ptr<void> pendingProcess;
	bool CleanupComplete() const
	{
		return cleanupConfirmed || (pendingProcess
			&& ::WaitForSingleObject(pendingProcess.get(), 0) == WAIT_OBJECT_0);
	}
};
inline void KeepPendingProcess(ProcessResult& result, DirectorySnapshot::Handle& process)
{
	if( result.cleanupConfirmed ) return;
	result.pendingProcess = std::shared_ptr<void>(process.value, [](void* handle) { ::CloseHandle(handle); });
	process.value = nullptr;
}
inline std::wstring QuoteArgument(const std::wstring& value)
{
	std::wstring out(1, L'"'); size_t slashes = 0;
	for( wchar_t c : value )
	{
		if( c == L'\\' ) { ++slashes; continue; }
		if( c == L'"' ) { out.append(slashes * 2 + 1, L'\\'); out += c; }
		else { out.append(slashes, L'\\'); out += c; }
		slashes = 0;
	}
	out.append(slashes * 2, L'\\'); out += L'"'; return out;
}

// Blocking coordinator: run only on a background worker, never on the UI
// thread. A fresh state is committed only after the child has exited and the
// complete, bounded packet has passed validation.
inline ProcessResult ReadOnWorkerThread(const std::wstring& launcher, const std::wstring& path,
	DWORD timeoutMs, DirectorySnapshot::State& output)
{
	ProcessResult result;
	SECURITY_ATTRIBUTES security = { sizeof(security), nullptr, TRUE };
	HANDLE rawRead = nullptr, rawWrite = nullptr;
	if( !::CreatePipe(&rawRead, &rawWrite, &security, 0) ) { result.error = ::GetLastError(); return result; }
	DirectorySnapshot::Handle readPipe(rawRead), writePipe(rawWrite);
	if( !::SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0) ) { result.error = ::GetLastError(); return result; }
	DirectorySnapshot::Handle job(::CreateJobObjectW(nullptr, nullptr));
	if( !job.value ) { result.error = ::GetLastError(); return result; }
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if( !::SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) )
	{ result.error = ::GetLastError(); return result; }
	SIZE_T attrSize = 0;
	::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
	if( !attrSize ) { result.error = ::GetLastError(); return result; }
	std::vector<BYTE> attributes(attrSize);
	LPPROC_THREAD_ATTRIBUTE_LIST attr = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	if( !::InitializeProcThreadAttributeList(attr, 1, 0, &attrSize) ) { result.error = ::GetLastError(); return result; }
	AttributeListGuard attributeGuard(attr);
	if( !::UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &rawWrite,
		sizeof(rawWrite), nullptr, nullptr) )
	{
		result.error = ::GetLastError(); return result;
	}
	wchar_t number[32] = {};
	swprintf_s(number, _countof(number), L"%llu", static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(rawWrite)));
	std::wstring command = QuoteArgument(launcher) + L" --internal-state-snapshot " + number + L" " + QuoteArgument(path);
	std::vector<wchar_t> mutableCommand(command.begin(), command.end()); mutableCommand.push_back(L'\0');
	STARTUPINFOEXW startup = {}; startup.StartupInfo.cb = sizeof(startup); startup.lpAttributeList = attr;
	PROCESS_INFORMATION info = {};
	const BOOL created = ::CreateProcessW(launcher.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
		CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
		nullptr, nullptr, &startup.StartupInfo, &info);
	if( !created ) result.error = ::GetLastError();
	if( !created ) return result;
	DirectorySnapshot::Handle process(info.hProcess), thread(info.hThread);
	if( !::AssignProcessToJobObject(job.value, process.value) )
	{
		result.error = ::GetLastError(); ::TerminateProcess(process.value, 1);
		result.cleanupConfirmed = ::WaitForSingleObject(process.value, 3000) == WAIT_OBJECT_0;
		KeepPendingProcess(result, process);
		return result;
	}
	::CloseHandle(writePipe.value); writePipe.value = nullptr;
	if( ::ResumeThread(thread.value) == static_cast<DWORD>(-1) ) result.error = ::GetLastError();
	const ULONGLONG start = ::GetTickCount64();
	std::vector<BYTE> packet;
	bool exited = false, broken = false;
	while( result.error == ERROR_SUCCESS )
	{
		DWORD available = 0;
		if( !::PeekNamedPipe(readPipe.value, nullptr, 0, nullptr, &available, nullptr) )
		{
			const DWORD error = ::GetLastError();
			if( error == ERROR_BROKEN_PIPE ) broken = true;
			else { result.error = error; break; }
		}
		if( available )
		{
			if( available > kMaxBytes + 8 - packet.size() ) { result.error = ERROR_BUFFER_OVERFLOW; break; }
			const size_t oldSize = packet.size(); packet.resize(oldSize + available);
			DWORD read = 0;
			if( !::ReadFile(readPipe.value, packet.data() + oldSize, available, &read, nullptr) || !read )
			{ result.error = ::GetLastError(); if( !result.error ) result.error = ERROR_READ_FAULT; break; }
			packet.resize(oldSize + read);
		}
		const DWORD wait = ::WaitForSingleObject(process.value, 0);
		if( wait == WAIT_OBJECT_0 ) exited = true;
		else if( wait != WAIT_TIMEOUT ) { result.error = ::GetLastError(); break; }
		if( exited && broken ) break;
		if( ::GetTickCount64() - start >= timeoutMs ) { result.error = ERROR_TIMEOUT; break; }
		if( !available ) ::Sleep(5);
	}
	if( !exited )
	{
		::TerminateJobObject(job.value, 1);
		result.cleanupConfirmed = ::WaitForSingleObject(process.value, 3000) == WAIT_OBJECT_0;
		KeepPendingProcess(result, process);
	}
	if( result.error != ERROR_SUCCESS ) return result;
	DWORD exitCode = 0, status = 0, length = 0;
	size_t offset = 0;
	if( !::GetExitCodeProcess(process.value, &exitCode) || exitCode != 0
		|| !Get(packet, offset, status) || !Get(packet, offset, length)
		|| length != packet.size() - offset || (status && length) )
	{ result.error = ERROR_INVALID_DATA; return result; }
	if( status ) { result.error = status; return result; }
	std::vector<BYTE> body(packet.begin() + offset, packet.end());
	if( !Decode(body, output) ) result.error = ERROR_INVALID_DATA;
	return result;
}
}
