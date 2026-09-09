// simple_example.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"
#include <iostream>
#include <../NInjectLib/Process.h>
#include <ShlObj.h>



#pragma comment(linker, "/subsystem:\"windows\" /entry:\"wmainCRTStartup\"")

// TargetID: �v���Z�XID
// �߂�l: ���� �]�݂�HWND / ���s NULL
HWND GetWindowHandle(	const DWORD TargetID)	
{
	HWND hWnd = GetTopWindow(NULL);
	do {
		if(GetWindowLong( hWnd, GWL_HWNDPARENT) != 0 || !IsWindowVisible( hWnd))
			continue;
		DWORD ProcessID;
		GetWindowThreadProcessId( hWnd, &ProcessID);
		if(TargetID == ProcessID)
			return hWnd;
	} while((hWnd = GetNextWindow( hWnd, GW_HWNDNEXT)) != NULL);

	return NULL;
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

			// lay out the stack exactly as if LoadLibraryW(dllFullPath) had just been called:
			// [esp] = return address (the real entry point), [esp+4] = the stdcall argument
			DWORD stackArgs[2] = { originalEntryPoint, (DWORD)remoteDllPath };
			DWORD newEsp = ctx.Esp - sizeof(stackArgs);
			process.writeMemory((LPVOID)newEsp, stackArgs, sizeof(stackArgs));

			ctx.Esp = newEsp;
			ctx.Eip = (DWORD)pLoadLibrary;
			if (!SetThreadContext(pInfo.hThread, &ctx)) throw std::exception("SetThreadContext failed");

			ResumeThread(pInfo.hThread);

			if( option )
			{
				if( WAIT_TIMEOUT == WaitForInputIdle( pInfo.hProcess, 5000  ))
				{
					throw std::exception("WaitForInputIdle return WAIT_TIMEOUT");
				}
				
				HWND hwnd = NULL;
				DWORD count = 0;
				if( !( hwnd = GetWindowHandle( pInfo.dwProcessId)) )
				{
					Sleep(20);
					if( count> 1000 )
					{
						throw std::exception("GetWindowHandle  Time Out");
					}
					++count;
				}
				LPDROPFILES dropfiles=nullptr;
				auto length = _tcslen( option );
				DWORD size =  sizeof(DROPFILES) + (length+2)*sizeof(TCHAR) ;
				auto h =::GlobalAlloc( GMEM_SHARE ,size );
				dropfiles = (LPDROPFILES)::GlobalLock( h );

				if( dropfiles )
				{
					dropfiles->fWide  = sizeof(TCHAR) ==sizeof(char) ? FALSE:TRUE;
					dropfiles->fNC = FALSE;
					dropfiles->pFiles = sizeof(DROPFILES) ;
					dropfiles->pt = POINT();
					LPTSTR filenamelist =reinterpret_cast<LPTSTR>( reinterpret_cast<LPBYTE>( dropfiles) +sizeof(DROPFILES));
			
					if(! _tcscpy_s( filenamelist,length+2, option ))
					{
						memset(filenamelist + length+1, 0,sizeof(TCHAR) );
						::GlobalUnlock( h );
						PostMessage(hwnd ,WM_DROPFILES,WPARAM(h),0);
					}
					else
					{
						throw std::exception("Cant Write File Name");

						::GlobalUnlock( h );
					}
				}
				else
				{
					::GlobalFree( h );
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