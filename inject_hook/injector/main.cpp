// simple_example.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <../NInjectLib/Process.h>
#include <ShlObj.h>
#include <commctrl.h>
#include <dbghelp.h>
#include <psapi.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

#ifndef UNICODEHACK_SWEEP_DIAGNOSTICS
#define UNICODEHACK_SWEEP_DIAGNOSTICS 0
#endif



#pragma comment(linker, "/subsystem:\"windows\" /entry:\"wmainCRTStartup\"")

// TargetID: �v���Z�XID
// �߂�l: ���� �]�݂�HWND / ���s NULL
static bool gBackgroundLaunch = false;
static HDESK gSweepDesktop = NULL;
static std::wstring gSweepDesktopName;
static std::wstring gSweepDesktopPath;

class SweepLogger
{
public:
	 explicit SweepLogger(const std::wstring& path)
		: mPath(path), mFile(INVALID_HANDLE_VALUE), mProblem(false)
	{
		mFile = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	}

	~SweepLogger()
	{
		std::lock_guard<std::mutex> guard(mMutex);
		FlushLocked(true);
		if( mFile != INVALID_HANDLE_VALUE ) ::CloseHandle(mFile);
	}

	void Write(const std::wstring& message, bool durable = false)
	{
		if( message.find(L"ui_stall") != std::wstring::npos
			|| message.find(L"foreground_violation") != std::wstring::npos
			|| message.find(L"background_enforce_failed") != std::wstring::npos )
			mProblem.store(true, std::memory_order_release);
		const std::wstring line = L"tick_ms=" + std::to_wstring(GetTickCount64())
			+ L" " + message + L"\r\n";
		const int byteCount = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
			static_cast<int>(line.size()), NULL, 0, NULL, NULL);
		if( byteCount <= 0 ) return;
		std::string utf8(static_cast<size_t>(byteCount), '\0');
		::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
			&utf8[0], byteCount, NULL, NULL);
		std::lock_guard<std::mutex> guard(mMutex);
		if( mFile == INVALID_HANDLE_VALUE )
		{
			mProblem.store(true, std::memory_order_release);
			return;
		}
		mBuffer.append(utf8);
		// Detailed per-item events are intentionally buffered so the diagnostic
		// logger does not turn every successful thumbnail observation into a disk
		// write. Critical boundaries still force the complete preceding buffer to
		// disk before the caller can continue.
		if( durable || mBuffer.size() >= 64 * 1024 ) FlushLocked(durable);
	}

	const std::wstring& Path() const { return mPath; }
	bool HasProblem() const { return mProblem.load(std::memory_order_acquire); }

private:
	void FlushLocked(bool durable)
	{
		if( mBuffer.empty() )
		{
			if( durable && mFile != INVALID_HANDLE_VALUE && !::FlushFileBuffers(mFile) )
				mProblem.store(true, std::memory_order_release);
			return;
		}
		if( mFile == INVALID_HANDLE_VALUE )
		{
			mBuffer.clear();
			mProblem.store(true, std::memory_order_release);
			return;
		}
		size_t offset = 0;
		while( offset < mBuffer.size() )
		{
			const DWORD requested = static_cast<DWORD>(std::min<size_t>(
				mBuffer.size() - offset, static_cast<size_t>(0xFFFFFFFFUL)));
			DWORD written = 0;
			if( requested == 0 || !::WriteFile(mFile, mBuffer.data() + offset,
				requested, &written, NULL) || written == 0 )
			{
				mBuffer.clear();
				mProblem.store(true, std::memory_order_release);
				return;
			}
			offset += written;
		}
		mBuffer.clear();
		if( durable && !::FlushFileBuffers(mFile) )
			mProblem.store(true, std::memory_order_release);
	}

	std::wstring mPath;
	HANDLE mFile;
	std::atomic<bool> mProblem;
	std::mutex mMutex;
	std::string mBuffer;
};

enum class SweepFolderTerminalState
{
	Unknown,
	Done,
	Failed
};

static bool DecodeSweepEventPath(const std::string& line,
	const char* marker, const char* endMarker, std::wstring& path)
{
	const size_t markerPosition = line.find(marker);
	if( markerPosition == std::string::npos ) return false;
	const size_t begin = markerPosition + strlen(marker);
	const size_t end = endMarker
		? line.rfind(endMarker)
		: line.rfind(']');
	if( end == std::string::npos || end <= begin ) return false;
	const std::string utf8 = line.substr(begin, end - begin);
	const int wideLength = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
		static_cast<int>(utf8.size()), NULL, 0);
	if( wideLength <= 0 ) return false;
	path.assign(static_cast<size_t>(wideLength), L'\0');
	return ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
		static_cast<int>(utf8.size()), &path[0], wideLength) == wideLength;
}

static std::map<std::wstring, SweepFolderTerminalState> LoadSweepFolderStates(
	const std::wstring& logPath)
{
	std::map<std::wstring, SweepFolderTerminalState> states;
	int pathLength = ::WideCharToMultiByte(CP_ACP, 0, logPath.c_str(), -1,
		NULL, 0, NULL, NULL);
	if( pathLength <= 1 ) return states;
	std::string narrowPath(static_cast<size_t>(pathLength), '\0');
	::WideCharToMultiByte(CP_ACP, 0, logPath.c_str(), -1, &narrowPath[0],
		pathLength, NULL, NULL);
	narrowPath.resize(static_cast<size_t>(pathLength - 1));
	std::ifstream input(narrowPath.c_str(), std::ios::binary);
	if( !input ) return states;

	struct EventMarker
	{
		const char* marker;
		const char* endMarker;
		SweepFolderTerminalState state;
	};
	static const EventMarker markers[] = {
		{ " folder_done path=[", "] items=", SweepFolderTerminalState::Done },
		{ " folder_timeout path=[", NULL, SweepFolderTerminalState::Failed },
		{ " folder_stalled path=[", NULL, SweepFolderTerminalState::Failed },
		{ " folder_drop_failed path=[", NULL, SweepFolderTerminalState::Failed },
		{ " folder_process_exit path=[", NULL, SweepFolderTerminalState::Failed },
		{ " folder_unprocessed path=[", NULL, SweepFolderTerminalState::Failed },
		{ " folder_skip_failed path=[", NULL, SweepFolderTerminalState::Failed },
		{ " folder_skip_completed path=[", NULL, SweepFolderTerminalState::Done }
	};
	std::string line;
	while( std::getline(input, line) )
	{
		for( const EventMarker& marker : markers )
		{
			std::wstring path;
			if( !DecodeSweepEventPath(line, marker.marker, marker.endMarker, path) )
				continue;
			SweepFolderTerminalState state = marker.state;
			if( state == SweepFolderTerminalState::Done
				&& line.find(" thumbnail_failed=0 thumbnail_unprocessed=0") == std::string::npos )
				state = SweepFolderTerminalState::Failed;
			// The append-only log is ordered by the durable event boundary. The
			// latest terminal result is authoritative: a later timeout must undo
			// an older folder_done, and a later successful retry must clear the
			// older failure. Do not let historical events suppress revalidation.
			states[path] = state;
			break;
		}
	}
	return states;
}

static std::set<std::wstring> LoadCompletedSweepFolders(const std::wstring& logPath)
{
	std::set<std::wstring> completed;
	const auto states = LoadSweepFolderStates(logPath);
	for( const auto& entry : states )
		if( entry.second == SweepFolderTerminalState::Done )
			completed.insert(entry.first);
	return completed;
}

static std::set<std::wstring> LoadFailedSweepFolders(const std::wstring& logPath)
{
	std::set<std::wstring> failed;
	const auto states = LoadSweepFolderStates(logPath);
	for( const auto& entry : states )
		if( entry.second == SweepFolderTerminalState::Failed )
			failed.insert(entry.first);
	return failed;
}

bool IsBackgroundLaunch()
{
	#if !UNICODEHACK_SWEEP_DIAGNOSTICS
	return false;
	#else
	_TCHAR value[8] = {};
	DWORD length = GetEnvironmentVariable(TEXT("LEYEESW_BACKGROUND"), value, _countof(value));
	return gBackgroundLaunch || (length == 1 && value[0] == TEXT('1'));
	#endif
}

struct WindowSearchContext
{
	DWORD processId;
	bool includeHidden;
	HWND result;
};

BOOL CALLBACK FindMainFormWindow(HWND hWnd, LPARAM parameter)
{
	WindowSearchContext* context = reinterpret_cast<WindowSearchContext*>(parameter);
	DWORD processId = 0;
	GetWindowThreadProcessId(hWnd, &processId);
	if( processId != context->processId
		|| (!context->includeHidden && !IsWindowVisible(hWnd)) ) return TRUE;
	TCHAR className[256] = {};
	GetClassName(hWnd, className, _countof(className));
	if( _tcscmp(className, TEXT("TMainForm")) == 0 )
	{
		context->result = hWnd;
		return FALSE;
	}
	return TRUE;
}

HWND GetWindowHandle(	const DWORD TargetID)	
{
	WindowSearchContext context = { TargetID, IsBackgroundLaunch(), NULL };
	if( context.includeHidden && gSweepDesktop )
		::EnumDesktopWindows(gSweepDesktop, FindMainFormWindow,
			reinterpret_cast<LPARAM>(&context));
	else
		EnumWindows(FindMainFormWindow, reinterpret_cast<LPARAM>(&context));
	return context.result;
}

static HWND FindListViewWindow(HWND mainWindow)
{
	struct ListSearchContext
	{
		HWND result;
	};
	ListSearchContext context = { NULL };
	EnumChildWindows(mainWindow, [](HWND hWnd, LPARAM parameter) -> BOOL
	{
		ListSearchContext* context = reinterpret_cast<ListSearchContext*>(parameter);
		TCHAR className[256] = {};
		GetClassName(hWnd, className, _countof(className));
		if( _tcscmp(className, TEXT("TAcvListView")) == 0 )
		{
			context->result = hWnd;
			return FALSE;
		}
		return TRUE;
	}, reinterpret_cast<LPARAM>(&context));
	return context.result;
}

static void EnforceBackgroundWindow(HWND mainWindow, SweepLogger* logger)
{
	if( !mainWindow || !::IsWindow(mainWindow) ) return;
	LONG_PTR extendedStyle = ::GetWindowLongPtr(mainWindow, GWL_EXSTYLE);
	if( !(extendedStyle & WS_EX_NOACTIVATE) )
	{
		::SetWindowLongPtr(mainWindow, GWL_EXSTYLE, extendedStyle | WS_EX_NOACTIVATE);
		::SetWindowPos(mainWindow, HWND_BOTTOM, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
		if( logger ) logger->Write(L"background_enforce hwnd="
			+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow))
			+ L" action=add_noactivate");
	}
	if( gSweepDesktop )
	{
		// Keep the diagnostic window visible on its private desktop. This preserves
		// Leeyes' drag/drop path without exposing the window on the user's desktop.
		if( !::IsWindowVisible(mainWindow) || ::IsIconic(mainWindow) )
		{
			::ShowWindow(mainWindow, SW_SHOWNOACTIVATE);
			::SetWindowPos(mainWindow, HWND_BOTTOM, 0, 0, 0, 0,
				SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER
				| SWP_SHOWWINDOW);
			if( logger ) logger->Write(L"background_enforce hwnd="
				+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow))
				+ L" action=isolated_desktop_noactivate");
		}
	}
	else
	{
		// Leeyes may ignore or defer WM_DROPFILES while iconic. Keep the window
		// alive on the interactive desktop, but place it outside the work area and
		// make it non-activating so it cannot cover or focus the user's windows.
		::ShowWindow(mainWindow, SW_SHOWNOACTIVATE);
		::SetWindowPos(mainWindow, HWND_BOTTOM, -32000, -32000, 0, 0,
			SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
		if( logger ) logger->Write(L"background_enforce hwnd="
			+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow))
			+ L" action=offscreen_noactivate");
	}
	if( !gSweepDesktop && ::GetForegroundWindow() == mainWindow )
	{
		if( logger ) logger->Write(L"foreground_violation hwnd="
			+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow)), true);
		::ShowWindow(mainWindow, SW_SHOWMINNOACTIVE);
		::SetWindowPos(mainWindow, HWND_BOTTOM, 0, 0, 0, 0,
			SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
		if( ::GetForegroundWindow() == mainWindow )
		{
			// A programmatic activation may survive minimization.  Hiding only the
			// diagnostic child prevents it from stealing the user's desktop focus.
			::ShowWindow(mainWindow, SW_HIDE);
			if( logger ) logger->Write(L"background_enforce hwnd="
				+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow))
				+ L" action=hide_after_violation", true);
		}
	}
}

static bool SendWindowMessageWithTimeout(HWND hwnd, UINT message, WPARAM wParam,
	LPARAM lParam, DWORD timeoutMilliseconds, LRESULT& result);

static SIZE_T GetLargestFreeRegion(HANDLE process)
{
	if( !process ) return 0;
	SIZE_T largest = 0;
	ULONG_PTR address = 0;
	for(;;)
	{
		MEMORY_BASIC_INFORMATION information = {};
		SIZE_T queried = ::VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address),
			&information, sizeof(information));
		if( queried == 0 ) break;
		if( information.State == MEM_FREE && information.RegionSize > largest )
			largest = information.RegionSize;
		ULONG_PTR next = reinterpret_cast<ULONG_PTR>(information.BaseAddress)
			+ information.RegionSize;
		if( next <= address ) break;
		address = next;
	}
	return largest;
}

static SIZE_T GetAddressSpaceUsage(HANDLE process)
{
	if( !process ) return 0;
	SIZE_T used = 0;
	ULONG_PTR address = 0;
	for(;;)
	{
		MEMORY_BASIC_INFORMATION information = {};
		SIZE_T queried = ::VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address),
			&information, sizeof(information));
		if( queried == 0 ) break;
		if( information.State != MEM_FREE ) used += information.RegionSize;
		ULONG_PTR next = reinterpret_cast<ULONG_PTR>(information.BaseAddress)
			+ information.RegionSize;
		if( next <= address ) break;
		address = next;
	}
	return used;
}

static ULONGLONG FileTimeToMilliseconds(const FILETIME& value)
{
	ULARGE_INTEGER time = {};
	time.LowPart = value.dwLowDateTime;
	time.HighPart = value.dwHighDateTime;
	return time.QuadPart / 10000ULL;
}

static DWORD ReadSweepNumber(const TCHAR* name, DWORD fallback, DWORD minimum);

class SweepWatchdog
{
public:
	SweepWatchdog(DWORD processId, SweepLogger* logger)
		: mProcessId(processId), mLogger(logger), mStop(false), mHangDumpWritten(false),
		  mUnresponsiveSince(0), mHangTerminationMilliseconds(ReadSweepNumber(
			TEXT("LEYEESW_HANG_TERMINATE_MS"), 15000, 1000))
	{
		mThread = std::thread(&SweepWatchdog::Run, this);
	}

	~SweepWatchdog()
	{
		Stop();
	}

	void Stop()
	{
		if( !mThread.joinable() ) return;
		mStop.store(true, std::memory_order_release);
		mThread.join();
	}

	private:
	void WriteHangDump(HANDLE process)
	{
		if( mHangDumpWritten || !process ) return;
		mHangDumpWritten = true;
		TCHAR directory[MAX_PATH] = {};
		if( ::GetCurrentDirectory(_countof(directory), directory) == 0 ) return;
		std::wstring path(directory);
		if( !path.empty() && path.back() != L'\\' ) path += L'\\';
		path += L"background_sweep_hang.dmp";
		HANDLE dump = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
			NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if( dump == INVALID_HANDLE_VALUE )
		{
			if( mLogger ) mLogger->Write(L"hang_dump path=[" + path
				+ L"] result=open_failed error=" + std::to_wstring(::GetLastError()), true);
			return;
		}
		const BOOL written = ::MiniDumpWriteDump(process, mProcessId, dump,
			MiniDumpNormal, NULL, NULL, NULL);
		::CloseHandle(dump);
		if( mLogger ) mLogger->Write(L"hang_dump path=[" + path + L"] result="
			+ std::to_wstring(written ? 1 : 0) + L" error="
			+ std::to_wstring(written ? ERROR_SUCCESS : ::GetLastError()), true);
	}

	void Run()
	{
		HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ
			| PROCESS_TERMINATE | SYNCHRONIZE,
			FALSE, mProcessId);
		ULONGLONG nextHeartbeat = 0;
		while( !mStop.load(std::memory_order_acquire) )
		{
			if( process && ::WaitForSingleObject(process, 0) == WAIT_OBJECT_0 )
			{
				if( mLogger ) mLogger->Write(L"process_exit pid=" + std::to_wstring(mProcessId), true);
				break;
			}
			HWND mainWindow = GetWindowHandle(mProcessId);
			HWND foreground = ::GetForegroundWindow();
			DWORD foregroundPid = 0;
			if( foreground ) ::GetWindowThreadProcessId(foreground, &foregroundPid);
			const bool targetForeground = foregroundPid == mProcessId;
			if( mainWindow )
			{
				EnforceBackgroundWindow(mainWindow, mLogger);
				if( targetForeground && mLogger )
					mLogger->Write(L"foreground_violation pid=" + std::to_wstring(mProcessId), true);
			}
			if( mLogger && GetTickCount64() >= nextHeartbeat )
			{
				const ULONGLONG heartbeatTick = GetTickCount64();
				LRESULT nullResult = 0;
				const bool uiResponsive = mainWindow
					&& SendWindowMessageWithTimeout(mainWindow, WM_NULL, 0, 0, 500, nullResult);
				const bool uiHung = mainWindow && ::IsHungAppWindow(mainWindow);
				PROCESS_MEMORY_COUNTERS_EX memory = {};
				memory.cb = sizeof(memory);
				SIZE_T privateBytes = 0;
				SIZE_T workingSet = 0;
				if( process && ::GetProcessMemoryInfo(process,
					reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) )
				{
					privateBytes = memory.PrivateUsage;
					workingSet = memory.WorkingSetSize;
				}
				DWORD handleCount = 0;
				if( process ) ::GetProcessHandleCount(process, &handleCount);
				DWORD gdiCount = process ? ::GetGuiResources(process, GR_GDIOBJECTS) : 0;
				DWORD userCount = process ? ::GetGuiResources(process, GR_USEROBJECTS) : 0;
				FILETIME creation = {}, exit = {}, kernel = {}, user = {};
				ULONGLONG kernelCpu = 0;
				ULONGLONG userCpu = 0;
				if( process && ::GetProcessTimes(process, &creation, &exit, &kernel, &user) )
				{
					kernelCpu = FileTimeToMilliseconds(kernel);
					userCpu = FileTimeToMilliseconds(user);
				}
				mLogger->Write(L"heartbeat pid=" + std::to_wstring(mProcessId)
					+ L" main_hwnd=" + std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow))
					+ L" foreground_pid=" + std::to_wstring(foregroundPid)
					+ L" main_visible=" + std::to_wstring(mainWindow ? ::IsWindowVisible(mainWindow) : 0)
					+ L" main_iconic=" + std::to_wstring(mainWindow ? ::IsIconic(mainWindow) : 0)
					+ L" ui_responsive=" + std::to_wstring(uiResponsive ? 1 : 0)
					+ L" ui_hung=" + std::to_wstring(uiHung ? 1 : 0)
					+ L" private_bytes=" + std::to_wstring(static_cast<unsigned long long>(privateBytes))
					+ L" working_set=" + std::to_wstring(static_cast<unsigned long long>(workingSet))
					+ L" address_space_used=" + std::to_wstring(static_cast<unsigned long long>(GetAddressSpaceUsage(process)))
					+ L" largest_free=" + std::to_wstring(static_cast<unsigned long long>(GetLargestFreeRegion(process)))
					+ L" gdi=" + std::to_wstring(gdiCount)
					+ L" user=" + std::to_wstring(userCount)
					+ L" handles=" + std::to_wstring(handleCount)
					+ L" kernel_cpu_ms=" + std::to_wstring(kernelCpu)
					+ L" user_cpu_ms=" + std::to_wstring(userCpu));
				if( mainWindow && (!uiResponsive || uiHung) )
				{
					if( mUnresponsiveSince == 0 ) mUnresponsiveSince = heartbeatTick;
					mLogger->Write(L"ui_stall pid=" + std::to_wstring(mProcessId)
						+ L" responsive=" + std::to_wstring(uiResponsive ? 1 : 0)
						+ L" hung=" + std::to_wstring(uiHung ? 1 : 0), true);
					WriteHangDump(process);
					if( heartbeatTick - mUnresponsiveSince >= mHangTerminationMilliseconds )
					{
						const BOOL terminated = ::TerminateProcess(process, 1);
						mLogger->Write(L"diagnostic_child_terminate pid="
							+ std::to_wstring(mProcessId) + L" reason=ui_stall_continuous"
							+ L" elapsed_ms="
							+ std::to_wstring(heartbeatTick - mUnresponsiveSince)
							+ L" result=" + std::to_wstring(terminated ? 1 : 0)
							+ L" error=" + std::to_wstring(terminated ? ERROR_SUCCESS : ::GetLastError()),
							true);
						if( terminated ) break;
					}
				}
				else mUnresponsiveSince = 0;
				nextHeartbeat = GetTickCount64() + 1000;
			}
			::Sleep(100);
		}
		if( process ) ::CloseHandle(process);
	}

	DWORD mProcessId;
	SweepLogger* mLogger;
	std::atomic<bool> mStop;
	bool mHangDumpWritten;
	ULONGLONG mUnresponsiveSince;
	DWORD mHangTerminationMilliseconds;
	std::thread mThread;
};

static bool SendWindowMessageWithTimeout(HWND hwnd, UINT message, WPARAM wParam,
	LPARAM lParam, DWORD timeoutMilliseconds, LRESULT& result)
{
	DWORD_PTR rawResult = 0;
	if( !::SendMessageTimeout(hwnd, message, wParam, lParam,
		SMTO_ABORTIFHUNG | SMTO_BLOCK, timeoutMilliseconds, &rawResult) ) return false;
	result = static_cast<LRESULT>(rawResult);
	return true;
}

static bool PostDropPath(HWND hwnd, LPCTSTR path, bool waitForHandler = false)
{
	if( !hwnd || !path ) return false;
	// Leeyes may clear the accept-files style while rebuilding a folder. Restore
	// it immediately before each diagnostic drop instead of relying on startup
	// state that may belong to the previous folder.
	::DragAcceptFiles(hwnd, TRUE);
	const size_t length = _tcslen(path);
	const DWORD payloadBytes = static_cast<DWORD>((length + 2) * sizeof(TCHAR));
	const DWORD size = sizeof(DROPFILES) + payloadBytes;
	HGLOBAL h = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, size);
	if( !h ) return false;
	LPDROPFILES dropfiles = static_cast<LPDROPFILES>(::GlobalLock(h));
	if( !dropfiles )
	{
		::GlobalFree(h);
		return false;
	}

	dropfiles->fWide = TRUE;
	dropfiles->fNC = FALSE;
	dropfiles->pFiles = sizeof(DROPFILES);
	dropfiles->pt = POINT();
	LPTSTR fileName = reinterpret_cast<LPTSTR>(
		reinterpret_cast<LPBYTE>(dropfiles) + sizeof(DROPFILES));
	const errno_t copyResult = _tcscpy_s(fileName, length + 2, path);
	if( copyResult == 0 ) fileName[length + 1] = TEXT('\0');
	::GlobalUnlock(h);
	if( copyResult != 0 )
	{
		::GlobalFree(h);
		return false;
	}
	if( waitForHandler )
	{
		LRESULT messageResult = 0;
		// The sweep thread is attached to the same desktop as Leeyes. A
		// synchronous send therefore gives an actual WM_DROPFILES handling
		// boundary. On timeout the target may still own HDROP, so leave it to the
		// diagnostic process teardown rather than freeing it here.
		if( !SendWindowMessageWithTimeout(hwnd, WM_DROPFILES, WPARAM(h), 0,
			120000, messageResult) ) return false;
		return true;
	}

	if( !::PostMessage(hwnd, WM_DROPFILES, WPARAM(h), 0) )
	{
		::GlobalFree(h);
		return false;
	}
	return true;
}

static std::wstring ToExtendedSweepPath(const std::wstring& path)
{
	if( path.empty() || path.compare(0, 4, L"\\\\?\\") == 0 ) return path;
	if( path.size() < 248 ) return path;
	if( path.compare(0, 2, L"\\\\") == 0 ) return L"\\\\?\\UNC\\" + path.substr(2);
	if( path.size() >= 3 && path[1] == L':'
		&& (path[2] == L'\\' || path[2] == L'/') )
		return L"\\\\?\\" + path;
	return path;
}

static bool GetSweepDirectoryIdentity(const std::wstring& directory,
	std::wstring& identity, DWORD& error)
{
	identity.clear();
	error = ERROR_SUCCESS;
	const std::wstring openPath = ToExtendedSweepPath(directory);
	HANDLE handle = ::CreateFileW(openPath.c_str(), FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if( handle == INVALID_HANDLE_VALUE )
	{
		error = ::GetLastError();
		return false;
	}
	BY_HANDLE_FILE_INFORMATION information = {};
	const bool success = ::GetFileInformationByHandle(handle, &information) != FALSE;
	if( success )
	{
		ULARGE_INTEGER fileIndex = {};
		fileIndex.LowPart = information.nFileIndexLow;
		fileIndex.HighPart = information.nFileIndexHigh;
		identity = std::to_wstring(information.dwVolumeSerialNumber) + L":"
			+ std::to_wstring(fileIndex.QuadPart);
	}
	else error = ::GetLastError();
	::CloseHandle(handle);
	return success;
}

static bool IsSweepCandidateFile(const wchar_t* name)
{
	if( !name ) return false;
	const wchar_t* dot = wcsrchr(name, L'.');
	if( !dot || dot == name || !dot[1] ) return false;
	std::wstring extension(dot + 1);
	for( wchar_t& character : extension )
		if( character >= L'A' && character <= L'Z' ) character += L'a' - L'A';
	// This is intentionally a broad inventory classification. The ListView is
	// authoritative for the actual displayed count; this list only explains
	// common file/filter deltas without opening or decoding the files.
	static const wchar_t* const extensions[] = {
		L"jpg", L"jpeg", L"jpe", L"jfif", L"png", L"gif", L"bmp", L"webp",
		L"tif", L"tiff", L"tga", L"ico", L"psd", L"jp2", L"j2k", L"jpf",
		L"jpm", L"jpx", L"avif", L"heic", L"heif", L"bpg", L"mng", L"tlg",
		L"svg", L"pdf", L"zip", L"7z", L"rar", L"lzh", L"cab", L"cbz",
		L"avi", L"mp4", L"mpg", L"mpeg", L"wmv", L"mkv", L"m4v", L"mov",
		L"webm", L"flv", L"swf", L"gal", L"lcm", L"lmt", L"mag", L"eri",
		L"pi", L"url", L"ugoira"
	};
	for( const wchar_t* candidate : extensions )
		if( extension == candidate ) return true;
	return false;
}

struct SweepDirectoryInventory
{
	unsigned long long files = 0;
	unsigned long long directories = 0;
	unsigned long long candidateFiles = 0;
	std::set<std::wstring> allFileNames;
	std::set<std::wstring> candidateFileNames;
	std::set<std::wstring> directoryNames;
	unsigned long long skippedDirectories = 0;
	unsigned long long unprocessedDirectories = 0;
	DWORD error = ERROR_SUCCESS;
	bool enumerationComplete = true;
};

static SweepDirectoryInventory PushSweepChildren(const std::wstring& current,
	std::vector<std::wstring>& pending, std::set<std::wstring>& visited,
	SweepLogger* logger)
{
	SweepDirectoryInventory inventory;
	std::wstring searchPath = ToExtendedSweepPath(current);
	if( !searchPath.empty() && searchPath.back() != L'\\' && searchPath.back() != L'/' )
		searchPath += L'\\';
	searchPath += L'*';

	WIN32_FIND_DATAW data = {};
	HANDLE find = ::FindFirstFileW(searchPath.c_str(), &data);
	if( find == INVALID_HANDLE_VALUE )
	{
		inventory.error = ::GetLastError();
		inventory.enumerationComplete = false;
		if( logger ) logger->Write(L"directory_enumeration_failed path=[" + current
			+ L"] error=" + std::to_wstring(inventory.error), true);
		return inventory;
	}
	do
	{
		if( wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0 )
			continue;
		std::wstring child = current;
		if( !child.empty() && child.back() != L'\\' && child.back() != L'/' )
			child += L'\\';
		child += data.cFileName;
		if( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		{
			++inventory.directories;
			std::wstring identity;
			DWORD identityError = ERROR_SUCCESS;
			if( !GetSweepDirectoryIdentity(child, identity, identityError) )
			{
				++inventory.skippedDirectories;
				++inventory.unprocessedDirectories;
				if( logger ) logger->Write(L"directory_unprocessed path=[" + child
					+ L"] reason=identity_unavailable error="
					+ std::to_wstring(identityError), true);
				continue;
			}
			if( !visited.insert(identity).second )
			{
				++inventory.skippedDirectories;
				if( logger ) logger->Write(L"directory_skipped path=[" + child
					+ L"] reason=cycle_or_duplicate identity=[" + identity + L"]", true);
				continue;
			}
			if( data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT )
			{
				if( logger ) logger->Write(L"reparse_follow path=[" + child
					+ L"] identity=[" + identity + L"]", true);
			}
			if( logger ) logger->Write(L"directory_discovered path=[" + child
				+ L"] identity=[" + identity + L"]");
			pending.push_back(child);
		}
		else
		{
			++inventory.files;
			if( IsSweepCandidateFile(data.cFileName) ) ++inventory.candidateFiles;
		}
	}
	while( ::FindNextFileW(find, &data) );
	const DWORD lastError = ::GetLastError();
	if( lastError != ERROR_NO_MORE_FILES )
	{
		inventory.error = lastError;
		inventory.enumerationComplete = false;
		if( logger ) logger->Write(L"directory_enumeration_failed path=[" + current
			+ L"] error=" + std::to_wstring(lastError), true);
	}
	::FindClose(find);
	return inventory;
}

static std::wstring SweepManifestPath(const SweepLogger* logger)
{
	if( !logger ) return L"background_sweep_manifest.txt";
	const std::wstring& logPath = logger->Path();
	const size_t slash = logPath.find_last_of(L"\\/");
	if( slash == std::wstring::npos ) return L"background_sweep_manifest.txt";
	return logPath.substr(0, slash + 1) + L"background_sweep_manifest.txt";
}

static bool SweepWideToUtf8(const std::wstring& value, std::string& output)
{
	output.clear();
	if( value.empty() ) return true;
	const int length = ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
		static_cast<int>(value.size()), NULL, 0, NULL, NULL);
	if( length <= 0 ) return false;
	output.resize(static_cast<size_t>(length));
	return ::WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
		static_cast<int>(value.size()), &output[0], length, NULL, NULL) == length;
}

static bool SweepUtf8ToWide(const std::string& value, std::wstring& output)
{
	output.clear();
	if( value.empty() ) return true;
	const int length = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), NULL, 0);
	if( length <= 0 ) return false;
	output.resize(static_cast<size_t>(length));
	return ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
		static_cast<int>(value.size()), &output[0], length) == length;
}

static bool WriteSweepManifest(const std::wstring& path,
	const std::wstring& root, const std::vector<std::wstring>& folders,
	bool coverageComplete, SweepLogger* logger)
{
	std::string rootUtf8;
	if( !SweepWideToUtf8(root, rootUtf8) ) return false;
	std::string content = "LEYEESW_SWEEP_MANIFEST_V1 coverage_complete=";
	content += coverageComplete ? "1\n" : "0\n";
	content += rootUtf8;
	content += "\n";
	for( const std::wstring& folder : folders )
	{
		std::string utf8;
		if( !SweepWideToUtf8(folder, utf8) ) return false;
		content.append(utf8);
		content.push_back('\n');
	}

	const std::wstring temporaryPath = path + L".tmp";
	HANDLE file = ::CreateFileW(temporaryPath.c_str(), GENERIC_WRITE,
		FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if( file == INVALID_HANDLE_VALUE )
	{
		if( logger ) logger->Write(L"sweep_manifest_write_failed path=[" + path
			+ L"] error=" + std::to_wstring(::GetLastError()), true);
		return false;
	}
	bool success = true;
	size_t offset = 0;
	while( offset < content.size() )
	{
		const DWORD requested = static_cast<DWORD>(std::min<size_t>(
			content.size() - offset, 1024 * 1024));
		DWORD written = 0;
		if( !::WriteFile(file, content.data() + offset, requested, &written, NULL)
			|| written != requested )
		{
			success = false;
			break;
		}
		offset += written;
	}
	if( success && !::FlushFileBuffers(file) ) success = false;
	const DWORD writeError = success ? ERROR_SUCCESS : ::GetLastError();
	::CloseHandle(file);
	if( !success || !::MoveFileExW(temporaryPath.c_str(), path.c_str(),
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) )
	{
		const DWORD error = success ? ::GetLastError() : writeError;
		::DeleteFileW(temporaryPath.c_str());
		if( logger ) logger->Write(L"sweep_manifest_write_failed path=[" + path
			+ L"] error=" + std::to_wstring(error), true);
		return false;
	}
	return true;
}

static bool ReadSweepFile(const std::wstring& path, std::string& content)
{
	content.clear();
	HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if( file == INVALID_HANDLE_VALUE ) return false;
	LARGE_INTEGER size = {};
	if( !::GetFileSizeEx(file, &size) || size.QuadPart < 0
		|| static_cast<unsigned long long>(size.QuadPart) > 512ULL * 1024ULL * 1024ULL )
	{
		::CloseHandle(file);
		return false;
	}
	content.resize(static_cast<size_t>(size.QuadPart));
	size_t offset = 0;
	while( offset < content.size() )
	{
		const DWORD requested = static_cast<DWORD>(std::min<size_t>(
			content.size() - offset, 1024 * 1024));
		DWORD read = 0;
		if( !::ReadFile(file, &content[0] + offset, requested, &read, NULL)
			|| read != requested )
		{
			::CloseHandle(file);
			content.clear();
			return false;
		}
		offset += read;
	}
	::CloseHandle(file);
	return true;
}

static bool LoadSweepManifest(const std::wstring& path, const std::wstring& root,
	std::vector<std::wstring>& folders, bool& coverageComplete, SweepLogger* logger)
{
	folders.clear();
	coverageComplete = false;
	std::string content;
	if( !ReadSweepFile(path, content) ) return false;
	size_t cursor = 0;
	auto nextLine = [&content, &cursor](std::string& line) -> bool
	{
		if( cursor > content.size() ) return false;
		const size_t end = content.find('\n', cursor);
		if( end == std::string::npos )
		{
			line = content.substr(cursor);
			cursor = content.size() + 1;
		}
		else
		{
			line = content.substr(cursor, end - cursor);
			cursor = end + 1;
		}
		if( !line.empty() && line.back() == '\r' ) line.pop_back();
		return true;
	};
	std::string line;
	if( !nextLine(line) || line.find("LEYEESW_SWEEP_MANIFEST_V1 coverage_complete=") != 0 )
		return false;
	coverageComplete = line.find("coverage_complete=1") != std::string::npos;
	std::string rootLine;
	if( !nextLine(rootLine) ) return false;
	std::wstring manifestRoot;
	if( !SweepUtf8ToWide(rootLine, manifestRoot) || manifestRoot != root ) return false;
	while( nextLine(line) )
	{
		if( line.empty() ) continue;
		std::wstring folder;
		if( !SweepUtf8ToWide(line, folder) ) return false;
		folders.push_back(folder);
	}
	if( folders.empty() || folders.front() != root ) return false;
	if( logger ) logger->Write(L"sweep_manifest_loaded path=[" + path
		+ L"] folders=" + std::to_wstring(folders.size())
		+ L" coverage_complete=" + std::to_wstring(coverageComplete ? 1 : 0));
	return true;
}

static bool BuildSweepManifest(const std::wstring& root, const std::wstring& path,
	std::vector<std::wstring>& folders, bool& coverageComplete, SweepLogger* logger)
{
	folders.clear();
	coverageComplete = true;
	std::vector<std::wstring> pending;
	std::set<std::wstring> visited;
	std::wstring rootIdentity;
	DWORD rootIdentityError = ERROR_SUCCESS;
	if( GetSweepDirectoryIdentity(root, rootIdentity, rootIdentityError) )
		visited.insert(rootIdentity);
	else
	{
		coverageComplete = false;
		if( logger ) logger->Write(L"directory_unprocessed path=[" + root
			+ L"] reason=root_identity_unavailable error="
			+ std::to_wstring(rootIdentityError), true);
	}
	pending.push_back(root);
	while( !pending.empty() )
	{
		const std::wstring current = pending.back();
		pending.pop_back();
		folders.push_back(current);
		const SweepDirectoryInventory inventory = PushSweepChildren(current, pending,
			visited, logger);
		if( !inventory.enumerationComplete || inventory.unprocessedDirectories != 0 )
			coverageComplete = false;
	}
	if( logger ) logger->Write(L"sweep_manifest_built path=[" + path
		+ L"] folders=" + std::to_wstring(folders.size())
		+ L" coverage_complete=" + std::to_wstring(coverageComplete ? 1 : 0), true);
	return WriteSweepManifest(path, root, folders, coverageComplete, logger);
}

static SweepDirectoryInventory InspectSweepDirectory(const std::wstring& current,
	SweepLogger* logger)
{
	SweepDirectoryInventory inventory;
	std::wstring searchPath = ToExtendedSweepPath(current);
	if( !searchPath.empty() && searchPath.back() != L'\\' && searchPath.back() != L'/' )
		searchPath += L'\\';
	searchPath += L'*';
	WIN32_FIND_DATAW data = {};
	HANDLE find = ::FindFirstFileW(searchPath.c_str(), &data);
	if( find == INVALID_HANDLE_VALUE )
	{
		inventory.error = ::GetLastError();
		inventory.enumerationComplete = false;
		if( logger ) logger->Write(L"directory_enumeration_failed path=[" + current
			+ L"] error=" + std::to_wstring(inventory.error), true);
		return inventory;
	}
	do
	{
		if( wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0 )
			continue;
		if( data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
		{
			++inventory.directories;
			inventory.directoryNames.insert(data.cFileName);
			std::wstring child = current;
			if( !child.empty() && child.back() != L'\\' && child.back() != L'/' )
				child += L'\\';
			child += data.cFileName;
			std::wstring identity;
			DWORD identityError = ERROR_SUCCESS;
			if( !GetSweepDirectoryIdentity(child, identity, identityError) )
			{
				++inventory.skippedDirectories;
				++inventory.unprocessedDirectories;
				if( logger ) logger->Write(L"directory_unprocessed path=[" + child
					+ L"] reason=identity_unavailable error="
					+ std::to_wstring(identityError), true);
			}
		}
		else
		{
			++inventory.files;
			inventory.allFileNames.insert(data.cFileName);
			if( IsSweepCandidateFile(data.cFileName) )
			{
				++inventory.candidateFiles;
				inventory.candidateFileNames.insert(data.cFileName);
			}
		}
	}
	while( ::FindNextFileW(find, &data) );
	const DWORD lastError = ::GetLastError();
	if( lastError != ERROR_NO_MORE_FILES )
	{
		inventory.error = lastError;
		inventory.enumerationComplete = false;
		if( logger ) logger->Write(L"directory_enumeration_failed path=[" + current
			+ L"] error=" + std::to_wstring(lastError), true);
	}
	::FindClose(find);
	return inventory;
}

static DWORD ReadSweepNumber(const TCHAR* name, DWORD fallback, DWORD minimum)
{
	TCHAR value[32] = {};
	DWORD length = GetEnvironmentVariable(name, value, _countof(value));
	if( length == 0 || length >= _countof(value) ) return fallback;
	TCHAR* end = NULL;
	unsigned long parsed = _tcstoul(value, &end, 10);
	if( end == value || *end != TEXT('\0') || parsed < minimum || parsed > 1000000UL )
		return fallback;
	return static_cast<DWORD>(parsed);
}

struct ListViewSnapshot
{
	HWND list;
	DWORD count;
	std::wstring title;
	std::string first;
	std::string middle;
	std::string last;
	bool valid;
};

static bool ReadListItemText(HANDLE process, HWND list, DWORD index, std::string& text)
{
	if( !process || !list ) return false;
	const DWORD textCapacity = 512;
	const SIZE_T itemBytes = sizeof(LVITEMA);
	const SIZE_T allocationBytes = itemBytes + textCapacity;
	LPBYTE remote = static_cast<LPBYTE>(::VirtualAllocEx(process, NULL,
		allocationBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if( !remote ) return false;
	LVITEMA item = {};
	item.mask = LVIF_TEXT;
	item.iItem = static_cast<int>(index);
	item.iSubItem = 0;
	item.pszText = reinterpret_cast<LPSTR>(remote + itemBytes);
	item.cchTextMax = static_cast<int>(textCapacity);
	SIZE_T written = 0;
	bool success = ::WriteProcessMemory(process, remote, &item, sizeof(item), &written)
		&& written == sizeof(item);
	if( success )
	{
		LRESULT result = 0;
		success = SendWindowMessageWithTimeout(list, LVM_GETITEMTEXTA, index,
			reinterpret_cast<LPARAM>(remote), 1000, result);
		if( success )
		{
			std::vector<char> buffer(textCapacity, '\0');
			SIZE_T read = 0;
			success = ::ReadProcessMemory(process, remote + itemBytes,
				buffer.data(), buffer.size(), &read) && read == buffer.size();
			if( success ) text.assign(buffer.data());
		}
	}
	::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
	return success;
}

static bool ReadListItemTextWide(HANDLE process, HWND list, DWORD index,
	std::wstring& text)
{
	if( !process || !list ) return false;
	const DWORD textCapacity = 512;
	const SIZE_T itemBytes = sizeof(LVITEMW);
	const SIZE_T textBytes = static_cast<SIZE_T>(textCapacity) * sizeof(wchar_t);
	const SIZE_T allocationBytes = itemBytes + textBytes;
	LPBYTE remote = static_cast<LPBYTE>(::VirtualAllocEx(process, NULL,
		allocationBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	if( !remote ) return false;
	LVITEMW item = {};
	item.mask = LVIF_TEXT;
	item.iItem = static_cast<int>(index);
	item.iSubItem = 0;
	item.pszText = reinterpret_cast<LPWSTR>(remote + itemBytes);
	item.cchTextMax = static_cast<int>(textCapacity);
	SIZE_T written = 0;
	bool success = ::WriteProcessMemory(process, remote, &item, sizeof(item), &written)
		&& written == sizeof(item);
	if( success )
	{
		LRESULT result = 0;
		success = SendWindowMessageWithTimeout(list, LVM_GETITEMTEXTW, index,
			reinterpret_cast<LPARAM>(remote), 1000, result);
		if( success )
		{
			std::vector<wchar_t> buffer(textCapacity, L'\0');
			SIZE_T read = 0;
			success = ::ReadProcessMemory(process, remote + itemBytes,
				buffer.data(), textBytes, &read) && read == textBytes;
			if( success ) text.assign(buffer.data());
		}
	}
	::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
	return success;
}

static bool DecodeAnsiListText(const std::string& text, std::wstring& wide)
{
	wide.clear();
	if( text.empty() ) return true;
	int length = ::MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS,
		text.c_str(), static_cast<int>(text.size()), NULL, 0);
	if( length <= 0 )
	{
		length = ::MultiByteToWideChar(CP_ACP, 0, text.c_str(),
			static_cast<int>(text.size()), NULL, 0);
	}
	if( length <= 0 ) return false;
	wide.resize(static_cast<size_t>(length));
	return ::MultiByteToWideChar(CP_ACP, 0, text.c_str(),
		static_cast<int>(text.size()), &wide[0], length) == length;
}

static ListViewSnapshot CaptureListViewSnapshot(DWORD processId, HWND mainWindow)
{
	ListViewSnapshot snapshot = { NULL, 0, std::wstring(), std::string(), std::string(),
		std::string(), false };
	if( !mainWindow ) return snapshot;
	wchar_t title[512] = {};
	::GetWindowTextW(mainWindow, title, _countof(title));
	snapshot.title.assign(title);
	snapshot.list = FindListViewWindow(mainWindow);
	if( !snapshot.list ) return snapshot;
	LRESULT result = 0;
	if( !SendWindowMessageWithTimeout(snapshot.list, LVM_GETITEMCOUNT, 0, 0, 2000, result)
		|| result < 0 ) return snapshot;
	snapshot.count = static_cast<DWORD>(result);
	snapshot.valid = true;
	if( snapshot.count == 0 ) return snapshot;
	HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
		| PROCESS_VM_READ | PROCESS_VM_WRITE, FALSE, processId);
	if( !process )
	{
		snapshot.valid = false;
		return snapshot;
	}
	const DWORD middle = snapshot.count / 2;
	const bool first = ReadListItemText(process, snapshot.list, 0, snapshot.first);
	const bool middleRead = ReadListItemText(process, snapshot.list, middle, snapshot.middle);
	const bool last = ReadListItemText(process, snapshot.list, snapshot.count - 1, snapshot.last);
	::CloseHandle(process);
	if( !first || !middleRead || !last ) snapshot.valid = false;
	return snapshot;
}

static bool ListViewSnapshotChanged(const ListViewSnapshot& before,
	const ListViewSnapshot& current)
{
	if( !before.valid ) return true;
	if( !current.valid ) return false;
	return before.list != current.list || before.count != current.count
		|| before.title != current.title || before.first != current.first
		|| before.middle != current.middle
		|| before.last != current.last;
}

static HWND WaitForStableListView(DWORD processId, const ListViewSnapshot& before,
	unsigned long long expectedFiles, unsigned long long expectedCandidateFiles,
	DWORD minimumWaitMilliseconds,
	SweepLogger* logger)
{
	const ULONGLONG started = GetTickCount64();
	const DWORD listWaitTimeoutMilliseconds = ReadSweepNumber(
		TEXT("LEYEESW_LIST_WAIT_TIMEOUT_MS"), 30000, 1000);
	const ULONGLONG timeout = started + listWaitTimeoutMilliseconds;
	const DWORD uiStallGraceMilliseconds = ReadSweepNumber(
		TEXT("LEYEESW_UI_STALL_GRACE_MS"), 5000, 1000);
	ULONGLONG uiUnresponsiveSince = 0;
	DWORD previousCount = 0xFFFFFFFFUL;
	DWORD stableMilliseconds = 0;
	HWND lastList = NULL;
	// An invalid pre-drop snapshot is not evidence that a transition occurred.
	// Accepting the first visible ListView in that state can mistake Leeyes'
	// startup/default folder for the folder just submitted by WM_DROPFILES.
	bool transitionObserved = false;
	bool stateLogged = false;
	if( logger ) logger->Write(L"list_wait_before hwnd="
		+ std::to_wstring(reinterpret_cast<uintptr_t>(before.list))
		+ L" count=" + std::to_wstring(before.count)
		+ L" valid=" + std::to_wstring(before.valid ? 1 : 0)
		+ L" title_length=" + std::to_wstring(before.title.size()));

	while( GetTickCount64() < timeout )
	{
		HWND mainWindow = GetWindowHandle(processId);
		if( mainWindow )
		{
			LRESULT nullResult = 0;
			const bool uiResponsive = SendWindowMessageWithTimeout(
				mainWindow, WM_NULL, 0, 0, 500, nullResult);
			const bool uiHung = ::IsHungAppWindow(mainWindow) != FALSE;
			if( !uiResponsive || uiHung )
			{
				if( uiUnresponsiveSince == 0 )
				{
					uiUnresponsiveSince = GetTickCount64();
					if( logger ) logger->Write(L"list_ui_unresponsive hwnd="
						+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow)));
				}
				else if( GetTickCount64() - uiUnresponsiveSince
					>= uiStallGraceMilliseconds )
				{
					if( logger ) logger->Write(L"list_ui_stall hwnd="
						+ std::to_wstring(reinterpret_cast<uintptr_t>(mainWindow))
						+ L" grace_ms="
						+ std::to_wstring(uiStallGraceMilliseconds), true);
					return NULL;
				}
			}
			else uiUnresponsiveSince = 0;
		}
		else uiUnresponsiveSince = 0;
		HWND list = mainWindow ? FindListViewWindow(mainWindow) : NULL;
		DWORD count = 0;
		if( list )
		{
			LRESULT result = 0;
			if( !SendWindowMessageWithTimeout(list, 0x1004, 0, 0, 2000, result) )
			{
				// A busy ListView can miss one probe while the Leeyes UI thread is
				// still making progress.  Do not turn that single transport timeout
				// into a false folder failure.  WM_NULL/IsHungAppWindow above still
				// terminates the wait when the UI remains unresponsive for the grace
				// period, and the overall list-wait deadline remains a hard failure.
				if( logger ) logger->Write(L"list_count_probe_timeout hwnd="
					+ std::to_wstring(reinterpret_cast<uintptr_t>(list)));
				::Sleep(100);
				continue;
			}
			count = static_cast<DWORD>(result);
		}
		ListViewSnapshot current = CaptureListViewSnapshot(processId, mainWindow);
		if( logger && !stateLogged )
		{
			logger->Write(L"list_wait_state hwnd="
				+ std::to_wstring(reinterpret_cast<uintptr_t>(list))
				+ L" count=" + std::to_wstring(count)
				+ L" snapshot_valid=" + std::to_wstring(current.valid ? 1 : 0)
				+ L" title_length=" + std::to_wstring(current.title.size()));
			stateLogged = true;
		}
		if( list != before.list || count != before.count )
			transitionObserved = true;
		else if( !before.valid && current.valid )
		{
			// During startup there may be no ListView to snapshot before the first
			// drop. This is only a readiness observation, not proof that WM_DROPFILES
			// was accepted; the per-item name audit below remains authoritative.
			transitionObserved = true;
		}
		else if( before.valid && current.valid && current.count == count
			&& ListViewSnapshotChanged(before, current) )
			transitionObserved = true;

		const bool emptyNoOp = expectedFiles == 0 && before.count == 0 && count == 0;
		if( (transitionObserved || emptyNoOp) && list && list == lastList
			&& count == previousCount && !(expectedCandidateFiles != 0 && count == 0) )
			stableMilliseconds += 100;
		else
			stableMilliseconds = 0;
		lastList = list;
		previousCount = count;

		if( (transitionObserved || emptyNoOp) && list
			&& GetTickCount64() - started >= minimumWaitMilliseconds
			&& stableMilliseconds >= 800 ) return list;
		Sleep(100);
	}
	if( logger && !transitionObserved )
		logger->Write(L"list_transition_unobserved", true);
	return NULL;
}

struct SweepListResult
{
	DWORD count;
	DWORD attempted;
	DWORD completed;
	DWORD timedOutIndex;
	DWORD thumbnailRequested;
	DWORD thumbnailAssigned;
	DWORD thumbnailFailed;
	DWORD thumbnailUnprocessed;
	DWORD listTextRead;
	DWORD listNameMatched;
	DWORD listNameExactMatched;
	DWORD listNameAliasMatched;
	DWORD listNameUnmatched;
	DWORD listNameMissing;
	bool listNameAuditComplete;
	bool timeout;
};

static DWORD ReadSweepNumber(const TCHAR* name, DWORD fallback, DWORD minimum);

static bool BuildAnsiRoundTripAlias(const std::wstring& name, std::wstring& alias)
{
	alias.clear();
	if( name.empty() ) return false;
	const int ansiLength = ::WideCharToMultiByte(932, 0, name.c_str(), -1,
		NULL, 0, NULL, NULL);
	if( ansiLength <= 1 ) return false;
	std::vector<char> ansi(static_cast<size_t>(ansiLength), '\0');
	if( ::WideCharToMultiByte(932, 0, name.c_str(), -1, ansi.data(), ansiLength,
		NULL, NULL) <= 0 ) return false;
	const int wideLength = ::MultiByteToWideChar(932, 0, ansi.data(), -1,
		NULL, 0);
	if( wideLength <= 1 ) return false;
	std::vector<wchar_t> roundTrip(static_cast<size_t>(wideLength), L'\0');
	if( ::MultiByteToWideChar(932, 0, ansi.data(), -1, roundTrip.data(), wideLength)
		<= 0 ) return false;
	alias.assign(roundTrip.data());
	return alias != name;
}

static void BuildDisplayNameAliases(const std::set<std::wstring>& expectedNames,
	const std::set<std::wstring>& fileNames,
	std::map<std::wstring, std::set<std::wstring>>& aliases)
{
	aliases.clear();
	for( const std::wstring& name : expectedNames )
	{
		aliases[name].insert(name);
		std::wstring lossyAlias;
		// Leeyes keeps the list model in CP932. A unique round-trip spelling is
		// therefore an expected internal alias; collisions remain ambiguous and
		// are rejected by EraseDisplayedName below.
		if( BuildAnsiRoundTripAlias(name, lossyAlias) )
			aliases[lossyAlias].insert(name);
		if( fileNames.find(name) == fileNames.end() ) continue;
		const size_t extension = name.find_last_of(L'.');
		if( extension != std::wstring::npos && extension > 0 )
			aliases[name.substr(0, extension)].insert(name);
	}
}

enum DisplayNameMatchKind
{
	DisplayNameNoMatch,
	DisplayNameExact,
	DisplayNameAlias
};

static DisplayNameMatchKind EraseDisplayedName(const std::wstring& text,
	std::set<std::wstring>& expectedNames,
	const std::map<std::wstring, std::set<std::wstring>>& aliases)
{
	if( expectedNames.erase(text) != 0 ) return DisplayNameExact;
	const std::map<std::wstring, std::set<std::wstring>>::const_iterator found =
		aliases.find(text);
	if( found == aliases.end() || found->second.size() != 1 ) return DisplayNameNoMatch;
	if( expectedNames.erase(*found->second.begin()) == 0 ) return DisplayNameNoMatch;
	return DisplayNameAlias;
}

static void AuditListItemName(HANDLE process, HWND list, DWORD index,
	const std::wstring& directory, std::set<std::wstring>& expectedNames,
	const std::map<std::wstring, std::set<std::wstring>>& displayAliases,
	SweepListResult& result, SweepLogger* logger)
{
	std::wstring wideText;
	std::string ansiText;
	const bool wideRead = ReadListItemTextWide(process, list, index, wideText);
	const bool ansiRead = ReadListItemText(process, list, index, ansiText);
	if( wideRead || ansiRead ) ++result.listTextRead;
	else result.listNameAuditComplete = false;

	std::wstring ansiWideText;
	const bool ansiDecoded = ansiRead && DecodeAnsiListText(ansiText, ansiWideText);
	DisplayNameMatchKind match = wideRead
		? EraseDisplayedName(wideText, expectedNames, displayAliases)
		: DisplayNameNoMatch;
	if( match == DisplayNameNoMatch && ansiDecoded )
		match = EraseDisplayedName(ansiWideText, expectedNames, displayAliases);
	if( match != DisplayNameNoMatch )
	{
		++result.listNameMatched;
		if( match == DisplayNameAlias ) ++result.listNameAliasMatched;
		else ++result.listNameExactMatched;
		return;
	}

	++result.listNameUnmatched;
	if( logger ) logger->Write(L"list_item_unmatched path=[" + directory
		+ L"] index=" + std::to_wstring(index)
		+ L" read_w=" + std::to_wstring(wideRead ? 1 : 0)
		+ L" read_a=" + std::to_wstring(ansiRead ? 1 : 0)
		+ L" ui_w=[" + wideText + L"] ui_a=[" + ansiWideText + L"]", true);
}

static bool ObserveThumbnailAssignment(HANDLE process, HWND list, DWORD index,
	const std::wstring& directory, SweepLogger* logger, DWORD& failure,
	DWORD& unprocessed)
{
	if( !process )
	{
		++unprocessed;
		if( logger ) logger->Write(L"thumbnail_unprocessed path=[" + directory
			+ L"] index=" + std::to_wstring(index) + L" reason=process_handle_unavailable", true);
		return false;
	}
	LVITEMA item = {};
	item.mask = LVIF_IMAGE;
	item.iItem = static_cast<int>(index);
	item.iSubItem = 0;
	item.iImage = -1;
	LPVOID remoteItem = ::VirtualAllocEx(process, NULL, sizeof(item), MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE);
	if( !remoteItem )
	{
		++failure;
		if( logger ) logger->Write(L"thumbnail_failure path=[" + directory
			+ L"] index=" + std::to_wstring(index) + L" reason=remote_alloc", true);
		return false;
	}
	SIZE_T written = 0;
	if( !::WriteProcessMemory(process, remoteItem, &item, sizeof(item), &written)
		|| written != sizeof(item) )
	{
		++failure;
		::VirtualFreeEx(process, remoteItem, 0, MEM_RELEASE);
		if( logger ) logger->Write(L"thumbnail_failure path=[" + directory
			+ L"] index=" + std::to_wstring(index) + L" reason=remote_write", true);
		return false;
	}

	// Assignment is an observation boundary, not a reason to add seconds of
	// controller delay for every item with no image index. A short bounded poll
	// still catches asynchronous assignment while recording the item as
	// unprocessed when Leeyes does not expose one promptly.
	const DWORD assignmentTimeout = ReadSweepNumber(
		TEXT("LEYEESW_THUMBNAIL_ASSIGN_TIMEOUT_MS"), 500, 50);
	const ULONGLONG deadline = GetTickCount64() + assignmentTimeout;
	bool transportFailure = false;
	for(;;)
	{
		LRESULT messageResult = 0;
		if( !SendWindowMessageWithTimeout(list, 0x1005, index,
			reinterpret_cast<LPARAM>(remoteItem), 1000, messageResult) )
		{
			transportFailure = true;
			break;
		}
		SIZE_T read = 0;
		LVITEMA observed = {};
		if( !::ReadProcessMemory(process, remoteItem, &observed, sizeof(observed), &read)
			|| read != sizeof(observed) )
		{
			transportFailure = true;
			break;
		}
		if( messageResult && observed.iImage >= 0 )
		{
			if( logger ) logger->Write(L"thumbnail_assignment_observed path=[" + directory
				+ L"] index=" + std::to_wstring(index) + L" image_index="
				+ std::to_wstring(observed.iImage));
			::VirtualFreeEx(process, remoteItem, 0, MEM_RELEASE);
			return true;
		}
		if( GetTickCount64() >= deadline ) break;
		::Sleep(assignmentTimeout < 50 ? assignmentTimeout : 25);
	}
	::VirtualFreeEx(process, remoteItem, 0, MEM_RELEASE);
	if( transportFailure )
	{
		++failure;
		if( logger ) logger->Write(L"thumbnail_failure path=[" + directory
			+ L"] index=" + std::to_wstring(index)
			+ L" reason=assignment_query", true);
	}
	else
	{
		++unprocessed;
		if( logger ) logger->Write(L"thumbnail_unprocessed path=[" + directory
			+ L"] index=" + std::to_wstring(index)
			+ L" reason=assignment_not_observed", true);
	}
	return false;
}

static SweepListResult SweepListView(HWND list, DWORD stride, DWORD delayMilliseconds,
	HANDLE process, const std::wstring& directory,
	const SweepDirectoryInventory& inventory, SweepLogger* logger)
{
	SweepListResult result = {};
	result.listNameAuditComplete = true;
	if( !list ) return result;
	LRESULT itemCount = 0;
	if( !SendWindowMessageWithTimeout(list, 0x1004, 0, 0, 2000, itemCount) )
	{
		result.timeout = true;
		if( logger ) logger->Write(L"item_count_timeout path=[" + directory + L"]", true);
		return result;
	}
	result.count = static_cast<DWORD>(itemCount);
	const ULONGLONG started = GetTickCount64();
	if( logger ) logger->Write(L"list_ready path=[" + directory + L"] items="
		+ std::to_wstring(result.count));
	std::set<std::wstring> expectedStorage;
	const std::set<std::wstring>* expectedNames = &inventory.allFileNames;
	const wchar_t* expectation = L"all_files";
	if( result.count == inventory.candidateFiles )
	{
		expectedNames = &inventory.candidateFileNames;
		expectation = L"candidate_files";
	}
	else if( result.count == inventory.candidateFiles + inventory.directories )
	{
		// Leeyes may expose the immediate folders together with the filtered
		// image/archive files. This is a separate expected shape from files-only.
		expectedStorage = inventory.candidateFileNames;
		expectedStorage.insert(inventory.directoryNames.begin(), inventory.directoryNames.end());
		expectedNames = &expectedStorage;
		expectation = L"candidate_files_and_directories";
	}
	else if( result.count == inventory.files + inventory.directories )
	{
		expectedStorage = inventory.allFileNames;
		expectedStorage.insert(inventory.directoryNames.begin(), inventory.directoryNames.end());
		expectedNames = &expectedStorage;
		expectation = L"all_files_and_directories";
	}
	else if( result.count > inventory.files + inventory.directories )
	{
		// Keep the complete immediate-entry set for mismatch reporting. The
		// excess entries are still reported individually below.
		expectedStorage = inventory.allFileNames;
		expectedStorage.insert(inventory.directoryNames.begin(), inventory.directoryNames.end());
		expectedNames = &expectedStorage;
		expectation = L"all_files_with_unexpected_entries";
	}
	std::set<std::wstring> remainingNames = *expectedNames;
	std::map<std::wstring, std::set<std::wstring>> displayAliases;
	BuildDisplayNameAliases(remainingNames, inventory.allFileNames, displayAliases);
	if( logger ) logger->Write(L"list_name_expectation path=[" + directory
		+ L"] kind=" + expectation + L" expected="
		+ std::to_wstring(remainingNames.size()));
	for( DWORD index = 0; index < result.count; index += stride )
	{
		++result.attempted;
		++result.thumbnailRequested;
		const ULONGLONG itemStarted = GetTickCount64();
		if( logger ) logger->Write(L"item_start path=[" + directory + L"] index="
			+ std::to_wstring(index), true);
		AuditListItemName(process, list, index, directory, remainingNames,
			displayAliases, result, logger);
		if( logger ) logger->Write(L"thumbnail_request path=[" + directory
			+ L"] index=" + std::to_wstring(index));
		LRESULT messageResult = 0;
		if( !SendWindowMessageWithTimeout(list, 0x1013, static_cast<WPARAM>(index), TRUE,
			5000, messageResult) )
		{
			result.timeout = true;
			result.timedOutIndex = index;
			++result.thumbnailFailed;
			++result.thumbnailUnprocessed;
			if( logger ) logger->Write(L"item_timeout path=[" + directory + L"] index="
				+ std::to_wstring(index) + L" elapsed_ms="
				+ std::to_wstring(GetTickCount64() - itemStarted), true);
			return result;
		}
		++result.completed;
		if( logger ) logger->Write(L"thumbnail_request_done path=[" + directory
			+ L"] index=" + std::to_wstring(index));
		if( ObserveThumbnailAssignment(process, list, index, directory, logger,
			result.thumbnailFailed, result.thumbnailUnprocessed) ) ++result.thumbnailAssigned;
		if( logger ) logger->Write(L"item_done path=[" + directory + L"] index="
			+ std::to_wstring(index) + L" elapsed_ms="
			+ std::to_wstring(GetTickCount64() - itemStarted));
		if( delayMilliseconds > 0 ) Sleep(delayMilliseconds);
	}
	if( result.count > 0 && ((result.count - 1) % stride) != 0 )
	{
		const DWORD index = result.count - 1;
		++result.attempted;
		++result.thumbnailRequested;
		const ULONGLONG itemStarted = GetTickCount64();
		if( logger ) logger->Write(L"item_start path=[" + directory + L"] index="
			+ std::to_wstring(index), true);
		AuditListItemName(process, list, index, directory, remainingNames,
			displayAliases, result, logger);
		if( logger ) logger->Write(L"thumbnail_request path=[" + directory
			+ L"] index=" + std::to_wstring(index));
		LRESULT messageResult = 0;
		if( !SendWindowMessageWithTimeout(list, 0x1013, static_cast<WPARAM>(index), TRUE,
			5000, messageResult) )
		{
			result.timeout = true;
			result.timedOutIndex = index;
			++result.thumbnailFailed;
			++result.thumbnailUnprocessed;
			if( logger ) logger->Write(L"item_timeout path=[" + directory + L"] index="
				+ std::to_wstring(index) + L" elapsed_ms="
				+ std::to_wstring(GetTickCount64() - itemStarted), true);
			return result;
		}
		++result.completed;
		if( logger ) logger->Write(L"thumbnail_request_done path=[" + directory
			+ L"] index=" + std::to_wstring(index));
		if( ObserveThumbnailAssignment(process, list, index, directory, logger,
			result.thumbnailFailed, result.thumbnailUnprocessed) ) ++result.thumbnailAssigned;
		if( logger ) logger->Write(L"item_done path=[" + directory + L"] index="
			+ std::to_wstring(index) + L" elapsed_ms="
			+ std::to_wstring(GetTickCount64() - itemStarted));
	}
	result.listNameMissing = static_cast<DWORD>(remainingNames.size());
	if( stride != 1 ) result.listNameAuditComplete = false;
	if( logger ) logger->Write(L"list_name_audit path=[" + directory
		+ L"] expected=" + std::to_wstring(expectedNames->size())
		+ L" text_read=" + std::to_wstring(result.listTextRead)
		+ L" matched=" + std::to_wstring(result.listNameMatched)
		+ L" exact_matched=" + std::to_wstring(result.listNameExactMatched)
		+ L" alias_matched=" + std::to_wstring(result.listNameAliasMatched)
		+ L" unmatched=" + std::to_wstring(result.listNameUnmatched)
		+ L" missing=" + std::to_wstring(result.listNameMissing)
		+ L" complete=" + std::to_wstring(result.listNameAuditComplete ? 1 : 0), true);
	if( logger ) logger->Write(L"list_sweep_done path=[" + directory + L" ] elapsed_ms="
		+ std::to_wstring(GetTickCount64() - started) + L" attempted="
		+ std::to_wstring(result.attempted) + L" completed="
		+ std::to_wstring(result.completed) + L" thumbnail_requested="
		+ std::to_wstring(result.thumbnailRequested) + L" thumbnail_assigned="
		+ std::to_wstring(result.thumbnailAssigned) + L" thumbnail_failed="
		+ std::to_wstring(result.thumbnailFailed) + L" thumbnail_unprocessed="
		+ std::to_wstring(result.thumbnailUnprocessed)
		+ L" list_name_exact_matched="
		+ std::to_wstring(result.listNameExactMatched)
		+ L" list_name_alias_matched="
		+ std::to_wstring(result.listNameAliasMatched)
		+ L" thumbnail_generation=external_plugin_summary"
		+ L" assignment_scope=listview_image_index");
	return result;
}

static int RunBackgroundSweep(DWORD processId, const std::wstring& root,
	SweepLogger* logger, bool skipCompleted)
{
	const DWORD rootAttributes = ::GetFileAttributesW(root.c_str());
	if( rootAttributes == INVALID_FILE_ATTRIBUTES
		|| !(rootAttributes & FILE_ATTRIBUTE_DIRECTORY) )
	{
		std::wcerr << L"BACKGROUND_SWEEP root_not_found path=[" << root << L"]" << std::endl;
		return 1;
	}

	// The diagnostic sweep is exhaustive by default.  A larger stride is only an
	// explicit comparison mode and must never be used as the full-test result.
	const DWORD stride = ReadSweepNumber(TEXT("LEYEESW_SWEEP_STRIDE"), 1, 1);
	const DWORD delayMilliseconds = ReadSweepNumber(TEXT("LEYEESW_SWEEP_DELAY_MS"), 10, 0);
	const bool synchronousDrop = ReadSweepNumber(
		TEXT("LEYEESW_SWEEP_SYNC_DROP"), 0, 0) != 0;
	const bool skipFailedSweepFolders = ReadSweepNumber(
		TEXT("LEYEESW_SWEEP_SKIP_FAILED"), 0, 0) != 0;
	// Zero means unlimited.  Build a durable Unicode manifest before exercising
	// Leeyes so a UI hang cannot discard the traversal queue between child
	// restarts.  The manifest is diagnostic state under DebugRoot, not user data.
	const DWORD maxFolders = ReadSweepNumber(TEXT("LEYEESW_SWEEP_MAX_FOLDERS"), 0, 0);
	const std::wstring manifestPath = SweepManifestPath(logger);
	std::vector<std::wstring> manifestFolders;
	bool manifestCoverageComplete = false;
	if( !LoadSweepManifest(manifestPath, root, manifestFolders,
		manifestCoverageComplete, logger) )
	{
		if( !BuildSweepManifest(root, manifestPath, manifestFolders,
			manifestCoverageComplete, logger) )
		{
			if( logger ) logger->Write(L"sweep_manifest_failed path=[" + manifestPath + L"]", true);
			return 1;
		}
	}
	const std::set<std::wstring> completedFolders = skipCompleted && logger
		? LoadCompletedSweepFolders(logger->Path()) : std::set<std::wstring>();
	const std::set<std::wstring> failedFolders = skipFailedSweepFolders && logger
		? LoadFailedSweepFolders(logger->Path()) : std::set<std::wstring>();
	bool coverageComplete = manifestCoverageComplete;
	DWORD processedFolders = 0;
	DWORD skippedFailedFolders = 0;
	unsigned long long skippedDirectories = 0;
	unsigned long long unprocessedDirectories = 0;
	std::map<std::wstring, DWORD> folderAttempts;
	const DWORD maxFolderAttempts = 3;

	std::wcout << L"BACKGROUND_SWEEP root=[" << root << L"]"
		<< L" max_folders=" << (maxFolders == 0 ? L"unlimited" : std::to_wstring(maxFolders))
		<< L" stride=" << stride << L" delay_ms=" << delayMilliseconds
		<< L" manifest_folders=" << manifestFolders.size() << std::endl;
	size_t manifestIndex = 0;
	while( manifestIndex < manifestFolders.size()
		&& (maxFolders == 0 || processedFolders < maxFolders) )
	{
		const std::wstring& directory = manifestFolders[manifestIndex++];
		const DWORD attempt = ++folderAttempts[directory];
		const bool alreadyCompleted = completedFolders.find(directory) != completedFolders.end();
		const bool alreadyFailed = failedFolders.find(directory) != failedFolders.end();
		if( logger ) logger->Write(L"folder_start path=[" + directory + L"]", true);
		const SweepDirectoryInventory inventory = InspectSweepDirectory(directory, logger);
		skippedDirectories += inventory.skippedDirectories;
		unprocessedDirectories += inventory.unprocessedDirectories;
		if( !inventory.enumerationComplete || inventory.unprocessedDirectories != 0 )
			coverageComplete = false;
		if( logger ) logger->Write(L"folder_inventory path=[" + directory
			+ L"] files=" + std::to_wstring(inventory.files)
			+ L" candidate_files=" + std::to_wstring(inventory.candidateFiles)
			+ L" directories=" + std::to_wstring(inventory.directories)
			+ L" skipped_directories=" + std::to_wstring(inventory.skippedDirectories)
			+ L" unprocessed_directories=" + std::to_wstring(inventory.unprocessedDirectories)
			+ L" enumeration_complete=" + std::to_wstring(inventory.enumerationComplete ? 1 : 0)
			+ L" error=" + std::to_wstring(inventory.error), true);
		if( alreadyCompleted )
		{
			if( logger ) logger->Write(L"folder_skip_completed path=[" + directory
				+ L"] reason=resume_log");
			continue;
		}
		if( alreadyFailed )
		{
			++skippedFailedFolders;
			coverageComplete = false;
			if( logger ) logger->Write(L"folder_skip_failed path=[" + directory
				+ L"] reason=resume_log", true);
			continue;
		}

		HWND mainWindow = GetWindowHandle(processId);
		const ListViewSnapshot before = CaptureListViewSnapshot(processId, mainWindow);
		// WM_DROPFILES is owned by TMainForm in the real application.  The child
		// TAcvListView is useful for observing the result, but posting the drop to
		// that child does not enter Leeyes' folder-change path reliably.
		HWND dropTarget = mainWindow;
		if( !mainWindow || !dropTarget || !PostDropPath(dropTarget, directory.c_str(),
			synchronousDrop) )
		{
			if( logger ) logger->Write(L"folder_drop_failed path=[" + directory + L"]", true);
			std::wcerr << L"BACKGROUND_SWEEP drop_failed path=[" << directory << L"]" << std::endl;
			return 1;
		}
		if( logger ) logger->Write(L"folder_drop_posted path=[" + directory
			+ L"] target="
			+ std::to_wstring(reinterpret_cast<uintptr_t>(dropTarget))
			+ L" delivery=" + (synchronousDrop ? L"send_timeout" : L"post"), true);
		HWND list = WaitForStableListView(processId, before, inventory.files,
			inventory.candidateFiles, 500, logger);
		if( !list )
		{
			if( attempt < maxFolderAttempts )
			{
				if( logger ) logger->Write(L"folder_retry path=[" + directory
					+ L"] attempt=" + std::to_wstring(attempt)
					+ L" reason=list_timeout", true);
				--manifestIndex;
				continue;
			}
			coverageComplete = false;
			++processedFolders;
			if( logger ) logger->Write(L"folder_timeout path=[" + directory
				+ L"] attempts=" + std::to_wstring(attempt), true);
			std::wcerr << L"BACKGROUND_SWEEP list_timeout path=[" << directory << L"]" << std::endl;
			continue;
		}
		HANDLE targetProcess = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION
			| PROCESS_VM_READ | PROCESS_VM_WRITE | SYNCHRONIZE, FALSE, processId);
		const SweepListResult sweep = SweepListView(list, stride, delayMilliseconds,
			targetProcess, directory, inventory, logger);
		if( targetProcess ) ::CloseHandle(targetProcess);
		if( sweep.timeout )
		{
			if( logger ) logger->Write(L"folder_stalled path=[" + directory + L"] index="
				+ std::to_wstring(sweep.timedOutIndex), true);
			std::wcerr << L"BACKGROUND_SWEEP item_timeout path=[" << directory
				<< L"] index=" << sweep.timedOutIndex << std::endl;
			return 2;
		}
		const bool thumbnailComplete = sweep.thumbnailFailed == 0
			&& sweep.thumbnailUnprocessed == 0;
		const bool listNamesComplete = sweep.listNameAuditComplete
			&& sweep.listNameUnmatched == 0 && sweep.listNameMissing == 0;
		if( !thumbnailComplete || !listNamesComplete ) coverageComplete = false;
		const wchar_t* expectedClassification = L"unexplained_difference";
		if( sweep.count == inventory.candidateFiles ) expectedClassification = L"candidate_files_exact";
		else if( sweep.count == inventory.candidateFiles + inventory.directories )
			expectedClassification = L"candidate_files_and_directories_exact";
		else if( sweep.count == inventory.files ) expectedClassification = L"all_files_exact";
		else if( sweep.count == inventory.files + inventory.directories )
			expectedClassification = L"all_entries_exact";
		else if( sweep.count < inventory.candidateFiles ) expectedClassification = L"less_than_candidate_files";
		else if( sweep.count > inventory.files + inventory.directories )
			expectedClassification = L"more_than_enumerated_entries";
		if( wcscmp(expectedClassification, L"candidate_files_exact") != 0
			&& wcscmp(expectedClassification, L"candidate_files_and_directories_exact") != 0
			&& wcscmp(expectedClassification, L"all_files_exact") != 0
			&& wcscmp(expectedClassification, L"all_entries_exact") != 0 )
			coverageComplete = false;
		const bool folderResultComplete = thumbnailComplete && listNamesComplete
			&& wcscmp(expectedClassification, L"unexplained_difference") != 0
			&& wcscmp(expectedClassification, L"less_than_candidate_files") != 0
			&& wcscmp(expectedClassification, L"more_than_enumerated_entries") != 0
			&& inventory.enumerationComplete && inventory.unprocessedDirectories == 0;
		// A first drop can race Leeyes' startup and expose the previous/default
		// list. Retry that shape mismatch (or a list with no matching names), but
		// do not repeatedly resubmit a list whose shape is correct and whose
		// remaining names reveal a real Unicode/display mismatch.
		const bool retryableStartupList =
			wcscmp(expectedClassification, L"unexplained_difference") == 0
			|| (sweep.listNameMatched == 0 && sweep.listNameUnmatched != 0);
		if( !folderResultComplete && retryableStartupList
			&& attempt < maxFolderAttempts )
		{
			if( logger ) logger->Write(L"folder_retry path=[" + directory
				+ L"] attempt=" + std::to_wstring(attempt)
				+ L" reason=list_or_thumbnail_audit", true);
			--manifestIndex;
			continue;
		}
		++processedFolders;
		if( logger ) logger->Write(L"list_expected_delta path=[" + directory
			+ L"] list_items=" + std::to_wstring(sweep.count)
			+ L" files=" + std::to_wstring(inventory.files)
			+ L" candidate_files=" + std::to_wstring(inventory.candidateFiles)
			+ L" directories=" + std::to_wstring(inventory.directories)
			+ L" name_matched=" + std::to_wstring(sweep.listNameMatched)
			+ L" name_unmatched=" + std::to_wstring(sweep.listNameUnmatched)
			+ L" name_missing=" + std::to_wstring(sweep.listNameMissing)
			+ L" classification=" + expectedClassification
			+ L" unexplained=" + std::to_wstring(
				wcscmp(expectedClassification, L"unexplained_difference") == 0 ? 1 : 0), true);
		std::wcout << L"BACKGROUND_SWEEP folder=" << processedFolders
			<< L" items=" << sweep.count << L" ensure_sent=" << sweep.completed
			<< L" thumbnail_assigned=" << sweep.thumbnailAssigned
			<< L" path=[" << directory << L"]" << std::endl;
		if( folderResultComplete )
		{
			if( logger ) logger->Write(L"folder_done path=[" + directory + L"] items="
				+ std::to_wstring(sweep.count) + L" completed="
				+ std::to_wstring(sweep.completed) + L" thumbnail_requested="
				+ std::to_wstring(sweep.thumbnailRequested) + L" thumbnail_assigned="
				+ std::to_wstring(sweep.thumbnailAssigned) + L" thumbnail_failed="
				+ std::to_wstring(sweep.thumbnailFailed) + L" thumbnail_unprocessed="
				+ std::to_wstring(sweep.thumbnailUnprocessed) + L" list_name_matched="
				+ std::to_wstring(sweep.listNameMatched) + L" list_name_unmatched="
				+ std::to_wstring(sweep.listNameUnmatched) + L" list_name_missing="
				+ std::to_wstring(sweep.listNameMissing) + L" list_name_exact_matched="
				+ std::to_wstring(sweep.listNameExactMatched)
				+ L" list_name_alias_matched="
				+ std::to_wstring(sweep.listNameAliasMatched), true);
		}
		else if( logger )
		{
			const wchar_t* reason = !thumbnailComplete
				? L"thumbnail_result_incomplete"
				: !listNamesComplete
					? L"list_name_mismatch"
				: (!inventory.enumerationComplete || inventory.unprocessedDirectories != 0)
					? L"directory_enumeration_incomplete"
					: L"unexplained_list_delta";
			logger->Write(L"folder_unprocessed path=[" + directory
				+ L"] reason=" + reason, true);
		}
	}
	const bool loggerProblem = logger && logger->HasProblem();
	if( logger ) logger->Write(L"sweep_done folders=" + std::to_wstring(processedFolders)
		+ L" pending=" + std::to_wstring(manifestFolders.size() - manifestIndex)
		+ L" full_run=" + std::to_wstring(maxFolders == 0 ? 1 : 0)
		+ L" skipped_failed_folders=" + std::to_wstring(skippedFailedFolders)
		+ L" discovered_directories=" + std::to_wstring(manifestFolders.size())
		+ L" skipped_directories=" + std::to_wstring(skippedDirectories)
		+ L" unprocessed_directories=" + std::to_wstring(unprocessedDirectories)
		+ L" complete=" + std::to_wstring(
			manifestIndex == manifestFolders.size() && maxFolders == 0 && coverageComplete
			&& !loggerProblem ? 1 : 0), true);
	return 0;
}

static void AppendDword(std::vector<BYTE>& code, DWORD value)
{
	for( unsigned int shift = 0; shift < 32; shift += 8 )
		code.push_back(static_cast<BYTE>((value >> shift) & 0xFF));
}

static void AppendPushImmediate(std::vector<BYTE>& code, DWORD value)
{
	code.push_back(0x68);
	AppendDword(code, value);
}

static LPVOID CreatePreEntryBootstrap(Process& process, LPVOID remoteDllPath,
	DWORD originalEntryPoint, LPVOID loadLibrary, LPVOID getProcAddress)
{
	static const char initName[] = "InitializeLeeyesInternalHooks";
	LPVOID remoteInitName = process.allocMem(sizeof(initName));
	process.writeMemory(remoteInitName, initName, sizeof(initName));

	std::vector<BYTE> code;
	AppendPushImmediate(code, static_cast<DWORD>(reinterpret_cast<uintptr_t>(remoteDllPath)));
	code.push_back(0xB8); // mov eax, LoadLibraryW
	AppendDword(code, static_cast<DWORD>(reinterpret_cast<uintptr_t>(loadLibrary)));
	code.push_back(0xFF); code.push_back(0xD0); // call eax
	code.push_back(0x85); code.push_back(0xC0); // test eax, eax
	code.push_back(0x74);
	size_t firstJumpDisplacement = code.size();
	code.push_back(0);
	AppendPushImmediate(code, static_cast<DWORD>(reinterpret_cast<uintptr_t>(remoteInitName)));
	code.push_back(0x50); // push the HMODULE returned by LoadLibraryW
	code.push_back(0xB8); // mov eax, GetProcAddress
	AppendDword(code, static_cast<DWORD>(reinterpret_cast<uintptr_t>(getProcAddress)));
	code.push_back(0xFF); code.push_back(0xD0); // call eax
	code.push_back(0x85); code.push_back(0xC0); // test init export
	code.push_back(0x74);
	size_t secondJumpDisplacement = code.size();
	code.push_back(0);
	code.push_back(0xFF); code.push_back(0xD0); // call InitializeLeeyesInternalHooks
	size_t entryJump = code.size();
	code.push_back(0xBA); // mov edx, original entry point
	AppendDword(code, originalEntryPoint);
	code.push_back(0xFF); code.push_back(0xE2); // jmp edx

	for( size_t displacement : { firstJumpDisplacement, secondJumpDisplacement } )
	{
		int value = static_cast<int>(entryJump) - static_cast<int>(displacement + 1);
		if( value < -128 || value > 127 ) throw std::exception("bootstrap jump out of range");
		code[displacement] = static_cast<BYTE>(static_cast<signed char>(value));
	}

	LPVOID remoteCode = process.allocMem(static_cast<DWORD>(code.size()));
	process.writeMemory(remoteCode, code.data(), static_cast<DWORD>(code.size()));
	return remoteCode;
}


int _tmain(int argc, _TCHAR* argv[])
{
	/*if (argc != 3)
	{
		std::cout << "Usage is: simple_example executable dll" << std::endl;
		return 1;
	}*/

	//const char* exe = argv[1];
	//const char* dll = argv[2];

	LPCTSTR exe = TEXT("Leeyes.exe");
	LPCTSTR dll = TEXT("inject_dll.dll");
	const bool sweepOption = argc >= 2
		&& _tcsicmp(argv[1], TEXT("--background-sweep")) == 0;
	#if UNICODEHACK_SWEEP_DIAGNOSTICS
	if( sweepOption && argc < 3 )
	{
		std::wcerr << L"usage: LeeyesW.exe --background-sweep <root>" << std::endl;
		return 2;
	}
	const bool sweepMode = sweepOption;
	#else
	if( sweepOption )
	{
		std::wcerr << L"background sweep is available only in the diagnostic Debug build"
			<< std::endl;
		return 3;
	}
	const bool sweepMode = false;
	#endif
	const LPCTSTR option = !sweepMode && argc >= 2 ? argv[1] : NULL;
	const std::wstring sweepRoot = sweepMode ? std::wstring(argv[2]) : std::wstring();
	gBackgroundLaunch = sweepMode;
	const bool backgroundLaunch = IsBackgroundLaunch();
	#if UNICODEHACK_SWEEP_DIAGNOSTICS
	const bool skipCompletedSweepFolders =
		ReadSweepNumber(TEXT("LEYEESW_SWEEP_SKIP_COMPLETED"), 0, 0) != 0;
	#else
	const bool skipCompletedSweepFolders = false;
	#endif

	TCHAR path[MAX_PATH]={};
	TCHAR current_dir[MAX_PATH]={};
	TCHAR dllFullPath[MAX_PATH]={};

	_tcscpy_s( path, argv[0] );
	auto find = _tcsrchr( path, TEXT('\\'));
	if( find && find < (path+MAX_PATH-2) )
	{
		find = _tcsinc(find);
		*find =  TEXT('\0');
		_tcscpy_s( current_dir, path );
		if(_tcscat_s( path,MAX_PATH,  exe))
		{
			abort();
		}
		_tcscpy_s( dllFullPath, current_dir );
		if(_tcscat_s( dllFullPath, MAX_PATH, dll))
		{
			abort();
		}
	}
	else
	{
		abort();
	}
	std::unique_ptr<SweepLogger> sweepLogger;
	#if UNICODEHACK_SWEEP_DIAGNOSTICS
	if( sweepMode )
	{
		sweepLogger.reset(new SweepLogger(std::wstring(current_dir)
			+ L"background_sweep_events.log"));
		sweepLogger->Write(L"sweep_launcher_start root=[" + sweepRoot + L"]", true);
	}
	#endif
	if( backgroundLaunch )
	{
		gSweepDesktopName = L"LeeyesUnicodeHackDiag_"
			+ std::to_wstring(::GetCurrentProcessId()) + L"_"
			+ std::to_wstring(::GetTickCount());
		gSweepDesktopPath = L"WinSta0\\" + gSweepDesktopName;
		gSweepDesktop = ::CreateDesktopW(gSweepDesktopName.c_str(), NULL, NULL, 0,
			GENERIC_ALL, NULL);
		if( !gSweepDesktop )
		{
			if( sweepLogger ) sweepLogger->Write(L"diagnostic_desktop_failed error="
				+ std::to_wstring(::GetLastError()), true);
			return 2;
		}
		if( sweepLogger ) sweepLogger->Write(L"diagnostic_desktop_created name=["
			+ gSweepDesktopPath + L"]", true);
	}

	STARTUPINFO sInfo = {0};
	sInfo.cb = sizeof(STARTUPINFO);
	if( backgroundLaunch )
	{
		// The child is visible on a private desktop, so Leeyes retains its normal
		// drag/drop behavior while no diagnostic window can cover the user's desktop.
		sInfo.dwFlags |= STARTF_USESHOWWINDOW;
		sInfo.wShowWindow = SW_SHOWNOACTIVATE;
		sInfo.lpDesktop = const_cast<LPTSTR>(gSweepDesktopPath.c_str());
	}
	PROCESS_INFORMATION pInfo= {0};
	std::unique_ptr<SweepWatchdog> watchdog;
	bool childResumed = false;
	int launcherExitCode = 0;

	if (CreateProcess(path, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, current_dir, &sInfo, &pInfo))
	{
		try
		{
			Process process(pInfo.dwProcessId);

			DWORD dllPathSize = (_tcslen(dllFullPath) + 1) * sizeof(TCHAR);
			LPVOID remoteDllPath = process.allocMem(dllPathSize);
			process.writeMemory(remoteDllPath, dllFullPath, dllPathSize);

			HMODULE hKernel32 = GetModuleHandle(TEXT("kernel32.dll"));
			LPVOID pLoadLibrary = (LPVOID)GetProcAddress(hKernel32, "LoadLibraryW");
			if (!pLoadLibrary) throw std::exception("Cant Resolve LoadLibraryW");

			// Redirect the still-suspended primary thread through LoadLibraryW before it
			// reaches Leeyes' real entry point, so every hook - including the Susie image
			// plugin ones - is installed before Leeyes does its own early plugin-scanning
			// startup work (which resolves GetProcAddress for image plugins too early for a
			// post-startup injection to catch). This technique was previously abandoned
			// because it matched a "suspended process injection" pattern that triggered a
			// crash via a p9np.dll bug on this system; that root cause is now fixed
			// independently (inject_dll.dll blocks p9np.dll's own load and hides P9NP from
			// this process's view of the network provider list), so this ordering is safe
			// again and lets hooks cover Leeyes' full startup sequence.
			uintptr_t imageBase = process.getImageBase(pInfo.hThread);
			IMAGE_DOS_HEADER dosHeader;
			process.readMemory((LPVOID)imageBase, &dosHeader, sizeof(dosHeader));
			IMAGE_NT_HEADERS ntHeaders;
			process.readMemory((LPVOID)(imageBase + dosHeader.e_lfanew), &ntHeaders, sizeof(ntHeaders));
			DWORD originalEntryPoint = (DWORD)(imageBase + ntHeaders.OptionalHeader.AddressOfEntryPoint);

			CONTEXT ctx = {};
			ctx.ContextFlags = CONTEXT_CONTROL;
			if (!GetThreadContext(pInfo.hThread, &ctx)) throw std::exception("GetThreadContext failed");

			LPVOID pGetProcAddress = reinterpret_cast<LPVOID>(GetProcAddress(
				hKernel32, "GetProcAddress"));
			if( !pGetProcAddress ) throw std::exception("Cant Resolve GetProcAddress");
			// Keep the primary thread suspended while LoadLibraryW returns and the
			// exported internal-hook initializer runs outside loader lock. The stub
			// then jumps to the original entry point without changing its startup stack.
			ctx.Eip = reinterpret_cast<DWORD>(CreatePreEntryBootstrap(
				process, remoteDllPath, originalEntryPoint, pLoadLibrary, pGetProcAddress));
			if (!SetThreadContext(pInfo.hThread, &ctx)) throw std::exception("SetThreadContext failed");

			if( ResumeThread(pInfo.hThread) == static_cast<DWORD>(-1) )
				throw std::exception("ResumeThread failed");
			childResumed = true;
			if( backgroundLaunch )
			{
				watchdog.reset(new SweepWatchdog(pInfo.dwProcessId, sweepLogger.get()));
				if( sweepLogger ) sweepLogger->Write(L"sweep_process_resumed pid="
					+ std::to_wstring(pInfo.dwProcessId), true);
			}

			if( option || sweepMode )
			{
				DWORD idleResult = WaitForInputIdle( pInfo.hProcess, 5000 );
				if( idleResult != 0 && idleResult != WAIT_TIMEOUT )
				{
					throw std::exception("WaitForInputIdle failed");
				}
				
				HWND hwnd = NULL;
				TCHAR windowTitle[256] = {};
				TCHAR windowClass[256] = {};
				// A large folder can keep Leeyes in its startup/list-building phase for
				// longer than the old 20-second polling limit.  Do not turn that normal
				// startup delay into a false injection failure.  The process handle is
				// checked on every pass so a real early exit still fails promptly.
				const ULONGLONG windowWaitStarted = GetTickCount64();
				const ULONGLONG windowWaitLimit = 120000;
				for(;;)
				{
					hwnd = GetWindowHandle( pInfo.dwProcessId);
					if( hwnd )
					{
						GetWindowText(hwnd, windowTitle, _countof(windowTitle));
						// A TMainForm HWND can exist while Leeyes is still restoring its
						// startup state. The private diagnostic desktop does not reliably
						// expose the caption through GetWindowText, however; the stable HWND
						// check below is the readiness boundary for that mode. Keep the title
						// requirement for the normal launcher, where it is observable.
						if( backgroundLaunch || windowTitle[0] != TEXT('\0') ) break;
					}
					if( WaitForSingleObject(pInfo.hProcess, 0) == WAIT_OBJECT_0 )
					{
						throw std::exception("Leeyes exited before TMainForm was created");
					}
					if( GetTickCount64() - windowWaitStarted >= windowWaitLimit )
					{
						throw std::exception("TMainForm wait timed out after 120 seconds");
					}
					Sleep(20);
				}
				// TMainForm recreates its HWND while restoring startup state.  A handle
				// discovered at the first visible/title-bearing moment can therefore be
				// invalid before WM_DROPFILES reaches the queue.  Require the same live
				// main-form handle to survive for half a second.
				HWND stableHwnd = hwnd;
				DWORD stableCount = 0;
				DWORD stabilityWait = 0;
				while( stableCount < 25 )
				{
					if( stabilityWait++ > 500 )
						throw std::exception("TMainForm did not stabilize");
					Sleep(20);
					HWND candidate = GetWindowHandle( pInfo.dwProcessId );
					if( candidate && candidate == stableHwnd && IsWindow(candidate) )
						++stableCount;
					else
					{
						stableHwnd = candidate;
						stableCount = 0;
					}
				}
				hwnd = stableHwnd;
				if( backgroundLaunch && gSweepDesktop )
				{
					if( !::SetThreadDesktop(gSweepDesktop) )
						throw std::exception("SetThreadDesktop failed");
					if( sweepLogger ) sweepLogger->Write(
						L"diagnostic_thread_desktop_attached", true);
				}
				if( backgroundLaunch ) EnforceBackgroundWindow(hwnd, sweepLogger.get());
				GetClassName(hwnd, windowClass, _countof(windowClass));
				#ifdef UNICODE
				std::wcout << L"DROP_TARGET hwnd=" << hwnd << L" title=[" << windowTitle << L"] class=[" << windowClass << L"]" << std::endl;
				#else
				std::cout << "DROP_TARGET hwnd=" << hwnd << " title=[" << windowTitle << "] class=[" << windowClass << "]" << std::endl;
				#endif
				if( option )
				{
					// Leeyes imports DragQueryFileA, but its drop handler needs the
					// original Unicode HDROP so the A/W hook pair can recover a CP932
					// collision such as U+00B7/U+30FB without losing the source path.
					if( !PostDropPath(hwnd, option) )
						throw std::exception("WM_DROPFILES PostMessage failed");
					std::cout << "WM_DROPFILES_POST_RESULT=1" << std::endl;
				}
				#if UNICODEHACK_SWEEP_DIAGNOSTICS
				if( sweepMode )
				{
					if( RunBackgroundSweep(pInfo.dwProcessId, sweepRoot, sweepLogger.get(),
						skipCompletedSweepFolders) != 0 )
						throw std::exception("background sweep failed");
					// The sweep owns this diagnostic Leeyes process. Close only that
					// process after the requested folders have been exercised.
					HWND finalWindow = GetWindowHandle(pInfo.dwProcessId);
					if( finalWindow ) ::PostMessage(finalWindow, WM_CLOSE, 0, 0);
					if( sweepLogger ) sweepLogger->Write(L"sweep_close_requested", true);
					if( watchdog ) watchdog->Stop();
					if( ::WaitForSingleObject(pInfo.hProcess, 5000) != WAIT_OBJECT_0 )
					{
						if( sweepLogger ) sweepLogger->Write(L"sweep_child_exit_timeout", true);
						::TerminateProcess(pInfo.hProcess, 3);
						::WaitForSingleObject(pInfo.hProcess, 2000);
					}
				}
				#endif
			}
			std::cout << "DLL successfully injected" << std::endl;
		}
		catch (std::exception& e)
		{
			#if UNICODEHACK_SWEEP_DIAGNOSTICS
			if( sweepMode ) launcherExitCode = 2;
			if( sweepLogger ) sweepLogger->Write(L"sweep_error exception=std_exception", true);
			#endif
			std::cout << "Error while trying to inject dll into process: " << e.what() << std::endl;
			if( watchdog ) watchdog->Stop();
			if( !childResumed ) ResumeThread(pInfo.hThread);
			#if UNICODEHACK_SWEEP_DIAGNOSTICS
			if( sweepMode )
			{
				HWND failedWindow = GetWindowHandle(pInfo.dwProcessId);
				if( failedWindow ) ::PostMessage(failedWindow, WM_CLOSE, 0, 0);
				if( ::WaitForSingleObject(pInfo.hProcess, 2000) != WAIT_OBJECT_0 )
					::TerminateProcess(pInfo.hProcess, 2);
			}
			#endif
			CloseHandle(pInfo.hThread);
			CloseHandle(pInfo.hProcess);
		}
	}
	else
	{
		DWORD lastErr = GetLastError();
		launcherExitCode = 1;
		std::cout << "Failed to start process, system error: " << lastErr << std::endl;
	}
	if( gSweepDesktop )
	{
		::CloseDesktop(gSweepDesktop);
		gSweepDesktop = NULL;
	}
	return launcherExitCode;
}
