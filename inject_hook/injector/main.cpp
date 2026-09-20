// simple_example.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <vector>
#include <../NInjectLib/Process.h>
#include <ShlObj.h>



#pragma comment(linker, "/subsystem:\"windows\" /entry:\"wmainCRTStartup\"")

// TargetID: �v���Z�XID
// �߂�l: ���� �]�݂�HWND / ���s NULL
bool IsBackgroundLaunch()
{
	_TCHAR value[8] = {};
	DWORD length = GetEnvironmentVariable(TEXT("LEYEESW_BACKGROUND"), value, _countof(value));
	return length == 1 && value[0] == TEXT('1');
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
	EnumWindows(FindMainFormWindow, reinterpret_cast<LPARAM>(&context));
	return context.result;
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
	bool backgroundLaunch = IsBackgroundLaunch();

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

	STARTUPINFO sInfo = {0};
	sInfo.cb = sizeof(STARTUPINFO);
	HWND previousForeground = backgroundLaunch ? ::GetForegroundWindow() : NULL;
	if( backgroundLaunch )
	{
		// Keep the diagnostic target minimized without activation.  The original
		// foreground window is restored after Leeyes' startup settles so the
		// minimized target cannot take the user's focus.
		sInfo.dwFlags |= STARTF_USESHOWWINDOW;
		sInfo.wShowWindow = SW_SHOWMINNOACTIVE;
	}
	PROCESS_INFORMATION pInfo= {0};
	LPTSTR option = argv[1];

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

			ResumeThread(pInfo.hThread);

			if( option )
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
						// The diagnostic path includes hidden windows and only needs a
						// stable HWND for WM_DROPFILES; the normal path retains the title
						// check used by the original launcher.
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
				if( backgroundLaunch && previousForeground && ::IsWindow(previousForeground) )
					::SetForegroundWindow(previousForeground);
				GetClassName(hwnd, windowClass, _countof(windowClass));
				#ifdef UNICODE
				std::wcout << L"DROP_TARGET hwnd=" << hwnd << L" title=[" << windowTitle << L"] class=[" << windowClass << L"]" << std::endl;
				#else
				std::cout << "DROP_TARGET hwnd=" << hwnd << " title=[" << windowTitle << "] class=[" << windowClass << "]" << std::endl;
				#endif
				LPDROPFILES dropfiles = nullptr;
				auto length = _tcslen( option );
				// Leeyes imports DragQueryFileA, but its drop handler needs the
				// original Unicode HDROP so the A/W hook pair can recover a CP932
				// collision such as U+00B7/U+30FB without losing the source path.
				// The injector is a Unicode build, so an ANSI HDROP is never needed.
				DWORD payloadBytes = (DWORD)((length + 2) * sizeof(TCHAR));
				DWORD size = sizeof(DROPFILES) + payloadBytes;
				HGLOBAL h = ::GlobalAlloc( GMEM_MOVEABLE | GMEM_ZEROINIT, size );
				if( !h ) throw std::exception("GlobalAlloc for HDROP failed");
				dropfiles = (LPDROPFILES)::GlobalLock( h );
				if( !dropfiles )
				{
					::GlobalFree( h );
					throw std::exception("GlobalLock for HDROP failed");
				}

				dropfiles->fWide = TRUE;
				dropfiles->fNC = FALSE;
				dropfiles->pFiles = sizeof(DROPFILES);
				dropfiles->pt = POINT();
				LPTSTR filenamelist = reinterpret_cast<LPTSTR>(
					reinterpret_cast<LPBYTE>(dropfiles) + sizeof(DROPFILES));
				if( _tcscpy_s(filenamelist, length + 2, option) )
				{
					::GlobalUnlock( h );
					::GlobalFree( h );
					throw std::exception("Cant Write File Name");
				}
				// DROPFILES lists are terminated by two null TCHARs.
				filenamelist[length + 1] = TEXT('\0');
				::GlobalUnlock( h );
				BOOL sendResult = ::PostMessage(hwnd, WM_DROPFILES, WPARAM(h), 0);
				std::cout << "WM_DROPFILES_POST_RESULT=" << sendResult
					<< " LAST_ERROR=" << GetLastError() << std::endl;
				if( !sendResult )
				{
					// Ownership transfers to the receiver only when PostMessage
					// succeeds. On failure the injector must release the HDROP.
					::GlobalFree( h );
					throw std::exception("WM_DROPFILES PostMessage failed");
				}
			}
			std::cout << "DLL successfully injected" << std::endl;
		}
		catch (std::exception& e)
		{
			std::cout << "Error while trying to inject dll into process: " << e.what() << std::endl;
			ResumeThread(pInfo.hThread);
			CloseHandle(pInfo.hThread);
			CloseHandle(pInfo.hProcess);
		}
	}
	else
	{
		DWORD lastErr = GetLastError();
		std::cout << "Failed to start process, system error: " << lastErr << std::endl;
	}
	return 0;
}
