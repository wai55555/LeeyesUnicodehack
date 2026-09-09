#include "stdafx.h"
#include <../NCodeHook/NCodeHookInstantiation.h>
#ifdef NDEBUG
#pragma comment(lib,"../NCodeHook/distorm.lib")
#else
#pragma comment(lib,"../NCodeHook/distorm.lib")
//#pragma comment(lib,"../NCodeHook/distorm_dbg.lib")
#endif

#include < ppl.h>
#include <mutex>
#include <csignal>
#include <exception>
#include <cstdint>
#include <map>
#include <winternl.h>
//#include <concurrent_unordered_set.h>
//#include <concurrent_unordered_map.h>

// NOTE: every injected dll has to export at least one symbol - otherwise
// the OS loader will fail with STATUS_INVALID_IMAGE_FORMAT (0x0C000007B)
__declspec(dllexport) void dummyExport() {}

// NOTE: this needs to be in global scope - otherwise the trampolines and hooks
// are deleted when the destructor of nCodeHook is called!
NCodeHookIA32 nCodeHook;

namespace Utility
{
	
std::wstring GetWidePath( std::string Path )
{
	std::wstring settled_path ;
	std::string tmp;
	wchar_t wide_file_name[MAX_PATH]={};
	bool unknown_code_found = false;

	tmp .reserve( Path.size() );
	settled_path .reserve( MAX_PATH );
	for( auto itr = Path.begin(); itr != Path.end(); ++itr )
	{
		if(*itr == '?' ){ unknown_code_found = true; }
		if( isleadbyte( static_cast<BYTE>(*itr) ) )
		{
			tmp.push_back( *itr );
			++itr;
			tmp.push_back( *itr );
			continue;
		}
		else
		{
			if( *itr < 0 )
			{
					tmp.push_back( '?');
			}
			else
			{
					tmp.push_back( *itr );
			}
		}
		if( *itr == '\\' )
		{
			if( unknown_code_found )
			{//�f�B���N�g���������j�R�[�h
				auto len = ::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ) ;
				if( len )
				{
					std::wstring indefinite_path( settled_path  );
					indefinite_path.append(wide_file_name );
					indefinite_path.pop_back();
					indefinite_path.push_back(L'.');

					_WIN32_FIND_DATAW FindData={};
					HANDLE hFind = ::FindFirstFileW(indefinite_path.c_str() ,&FindData);
					if( hFind != INVALID_HANDLE_VALUE )
					{
						do
						{
							if( FindData.cFileName[0] != L'.' )//?��������[.],[..]�Ƃ������|����
							{
								settled_path .append( FindData.cFileName );
								settled_path .push_back(L'\\' );
								break;
							}
						}	while( ::FindNextFileW( hFind, &FindData ) );
						::FindClose( hFind );
					}
				}
				unknown_code_found = false;
			}
			else
			{
				auto len = ::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH );
				if(len )
				{
					settled_path .append( wide_file_name  );
				}
			}
			tmp.clear();
		}
	}

	if( unknown_code_found )
	{//�t�@�C���������j�R�[�h
		if(::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ))
		{
			std::wstring indefinite_path( settled_path  );
			indefinite_path.append(wide_file_name );
			_WIN32_FIND_DATAW FindData={};
			HANDLE hFind = FindFirstFileW(indefinite_path.c_str() ,&FindData);
			if( hFind != INVALID_HANDLE_VALUE )
			{			
				settled_path .append( FindData.cFileName );
				FindClose( hFind );
			}
			else
			{//�T���Q�[�g�y�A�Ƃ����Ƃ���������?
				//short name
			}
		}
		unknown_code_found = false;
	}
	else
	{
		if(::MultiByteToWideChar(932, 0, tmp.data(), tmp.size()+1 , wide_file_name, MAX_PATH ))
		{
			settled_path .append( wide_file_name );
		}
	}	
	return settled_path ;
}

// Some volumes have 8.3 short name generation disabled (fsutil 8dot3name), in which case
// GetShortPathNameW can never help. As a fallback that works on any NTFS volume regardless
// of that setting, create a stable ASCII-safe alias next to the real item - a directory
// junction for folders, a hard link for files - and use that instead. The alias is reused
// on subsequent calls (named deterministically from a hash of the real path) rather than
// recreated every time.
uint64_t FnvHash( const std::wstring& s )
{
	uint64_t h = 1469598103934665603ULL;
	for( auto ch : s )
	{
		h ^= (uint16_t)ch;
		h *= 1099511628211ULL;
	}
	return h;
}

bool DirectoryJunctionExists( const std::wstring& path )
{
	DWORD attrs = ::GetFileAttributesW( path.c_str() );
	return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}

#pragma pack(push, 1)
struct ReparseMountPointBuffer
{
	DWORD ReparseTag;
	WORD ReparseDataLength;
	WORD Reserved;
	WORD SubstituteNameOffset;
	WORD SubstituteNameLength;
	WORD PrintNameOffset;
	WORD PrintNameLength;
	WCHAR PathBuffer[1024];
};
#pragma pack(pop)

bool CreateDirectoryJunction( const std::wstring& linkPath, const std::wstring& targetPath )
{
	if( !::CreateDirectoryW( linkPath.c_str(), NULL ) )
	{
		if( ::GetLastError() != ERROR_ALREADY_EXISTS ) return false;
	}
	HANDLE hDir = ::CreateFileW( linkPath.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL );
	if( hDir == INVALID_HANDLE_VALUE ) return false;

	std::wstring substituteName = L"\\??\\" + targetPath;
	if( !substituteName.empty() && substituteName.back() != L'\\' ) substituteName += L'\\';
	std::wstring printName = targetPath;
	if( !printName.empty() && printName.back() != L'\\' ) printName += L'\\';

	ReparseMountPointBuffer buf = {};
	buf.ReparseTag = 0xA0000003; // IO_REPARSE_TAG_MOUNT_POINT
	buf.SubstituteNameOffset = 0;
	buf.SubstituteNameLength = (WORD)(substituteName.size() * sizeof(WCHAR));
	buf.PrintNameOffset = buf.SubstituteNameLength + sizeof(WCHAR);
	buf.PrintNameLength = (WORD)(printName.size() * sizeof(WCHAR));
	buf.ReparseDataLength = (WORD)(8 + buf.SubstituteNameLength + sizeof(WCHAR) + buf.PrintNameLength + sizeof(WCHAR));

	memcpy( buf.PathBuffer, substituteName.c_str(), buf.SubstituteNameLength + sizeof(WCHAR) );
	memcpy( (BYTE*)buf.PathBuffer + buf.PrintNameOffset, printName.c_str(), buf.PrintNameLength + sizeof(WCHAR) );

	DWORD bytesReturned = 0;
	BOOL ok = ::DeviceIoControl( hDir, 0x000900A4 /*FSCTL_SET_REPARSE_POINT*/, &buf,
		buf.ReparseDataLength + 8, NULL, 0, &bytesReturned, NULL );
	::CloseHandle( hDir );
	return ok != 0;
}

// Returns the ANSI-safe alias path (CP932) for Path, creating the alias if needed.
// Path must already be a fully resolved, existing wide path.
std::string GetOrCreateAsciiAlias( const std::wstring& Path )
{
	DWORD attrs = ::GetFileAttributesW( Path.c_str() );
	if( attrs == INVALID_FILE_ATTRIBUTES ) return std::string();
	bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

	auto slashPos = Path.find_last_of( L'\\' );
	std::wstring parentDir = (slashPos == std::wstring::npos) ? L"" : Path.substr( 0, slashPos );
	std::wstring nameOnly = (slashPos == std::wstring::npos) ? Path : Path.substr( slashPos + 1 );

	std::wstring ext;
	auto dotPos = nameOnly.find_last_of( L'.' );
	if( !isDirectory && dotPos != std::wstring::npos ) ext = nameOnly.substr( dotPos );

	wchar_t aliasName[64] = {};
	swprintf_s( aliasName, L"~u%016llx%s", FnvHash( Path ), ext.c_str() );
	std::wstring aliasPath = parentDir + L"\\" + aliasName;

	DWORD aliasAttrs = ::GetFileAttributesW( aliasPath.c_str() );
	if( aliasAttrs == INVALID_FILE_ATTRIBUTES )
	{
		bool created = isDirectory
			? CreateDirectoryJunction( aliasPath, Path )
			: (::CreateHardLinkW( aliasPath.c_str(), Path.c_str(), NULL ) != 0);
		if( !created ) return std::string();
	}

	char mbcs_name[MAX_PATH] = {};
	WideCharToMultiByte( 932, 0, aliasPath.c_str(), -1, mbcs_name, MAX_PATH, 0, 0 );
	return mbcs_name;
}

std::string GetShortPath( std::wstring Path )
{
		char mbcs_name[MAX_PATH];
		wchar_t wide_name[MAX_PATH];
		auto len = ::GetShortPathNameW( Path.c_str(), wide_name, MAX_PATH );
		if( len == 0 || len >= MAX_PATH )
		{
			// 8.3 short names unavailable on this volume (or path too long) - fall back
			// to a junction/hard-link alias, which works regardless of that volume setting.
			auto alias = GetOrCreateAsciiAlias( Path );
			if( !alias.empty() ) return alias;
			WideCharToMultiByte( 932, 0, Path.c_str(), -1, mbcs_name, MAX_PATH, 0,0 );
			return mbcs_name;
		}
		WideCharToMultiByte( 932, 0, wide_name, -1, mbcs_name, MAX_PATH, 0,0 );
		return mbcs_name;
}
std::string GetShortPath( std::string Path )
{
		return GetShortPath( GetWidePath( Path ) );
}

}


namespace Kernel32
{
#if 1

std::map<HANDLE, std::wstring> gSearchParentDir;
std::mutex gSearchParentDirMutex;

// cFileName may come back from the OS with '?' standing in for characters that don't fit
// CP932. Prefer the 8.3 alternate name when one exists; when it doesn't (8.3 name generation
// disabled on this volume, or none applicable), fall back to a junction/hard-link alias -
// otherwise callers like Leeyes' own directory-tree code end up processing a name that is
// mostly '?' placeholders, which is what actually crashes it on some real-world folder names.
void FixupFindData( const std::wstring& parentDirWide, LPWIN32_FIND_DATA lpFindFileData )
{
	bool hasUnknown = false;
	for( int pos = 0; lpFindFileData->cFileName[pos]; ++pos )
	{
		if( lpFindFileData->cFileName[pos] == '?' ) { hasUnknown = true; break; }
	}
	if( !hasUnknown ) return;

	if( lpFindFileData->cAlternateFileName[0] )
	{
		::strncpy_s(lpFindFileData->cFileName,lpFindFileData->cAlternateFileName ,14);
		return;
	}

	if( parentDirWide.empty() ) return;

	// cFileName's '?' characters are already valid single-character wildcards for
	// FindFirstFileW; everything else just needs converting back to its real wide form.
	wchar_t widePattern[MAX_PATH] = {};
	if( !::MultiByteToWideChar( 932, 0, lpFindFileData->cFileName, -1, widePattern, MAX_PATH ) ) return;

	std::wstring fullPattern = parentDirWide + L"\\" + widePattern;
	WIN32_FIND_DATAW wideFindData = {};
	HANDLE hFind = ::FindFirstFileW( fullPattern.c_str(), &wideFindData );
	if( hFind == INVALID_HANDLE_VALUE ) return;
	::FindClose( hFind );

	std::wstring fullWide = parentDirWide + L"\\" + wideFindData.cFileName;
	auto alias = Utility::GetOrCreateAsciiAlias( fullWide );
	if( !alias.empty() )
	{
		auto slashPos = alias.find_last_of( '\\' );
		auto leafOnly = slashPos == std::string::npos ? alias : alias.substr( slashPos + 1 );
		::strncpy_s(lpFindFileData->cFileName, leafOnly.c_str(), leafOnly.size());
	}
}

typedef HANDLE  (WINAPI *FindFirstFileFPtr)(LPCTSTR lpFileName,   LPWIN32_FIND_DATA lpFindFileData  );
FindFirstFileFPtr originalFindFirstFile = nullptr;

HANDLE  WINAPI FindFirstFileHook(LPCTSTR lpFileName,  LPWIN32_FIND_DATA lpFindFileData )
{
	HANDLE ret = originalFindFirstFile(lpFileName,lpFindFileData);
	if( ret != INVALID_HANDLE_VALUE )
	{
		std::wstring parentDirWide;
		std::string searchPattern( lpFileName );
		auto slashPos = searchPattern.find_last_of( '\\' );
		if( slashPos != std::string::npos )
		{
			parentDirWide = Utility::GetWidePath( searchPattern.substr( 0, slashPos ) );
		}
		{
			std::lock_guard<std::mutex> lock(gSearchParentDirMutex);
			gSearchParentDir[ret] = parentDirWide;
		}
		FixupFindData( parentDirWide, lpFindFileData );
	}
	return ret;
}
void hookFindFirstFile()
{
	originalFindFirstFile = nCodeHook.createHookByName("kernel32.dll", "FindFirstFileA", FindFirstFileHook);
}


typedef HANDLE  (WINAPI *FindNextFileFPtr)( HANDLE hFindFile,    LPWIN32_FIND_DATA lpFindFileData   );
FindNextFileFPtr originalFindNextFile = nullptr;

HANDLE  WINAPI FindNextFileHook(HANDLE hFindFile,       LPWIN32_FIND_DATA lpFindFileData   )
{
	HANDLE ret = originalFindNextFile(hFindFile,lpFindFileData);
	if( ret != INVALID_HANDLE_VALUE )
	{
		std::wstring parentDirWide;
		{
			std::lock_guard<std::mutex> lock(gSearchParentDirMutex);
			auto it = gSearchParentDir.find( hFindFile );
			if( it != gSearchParentDir.end() ) parentDirWide = it->second;
		}
		FixupFindData( parentDirWide, lpFindFileData );
	}
	return ret;
}
void hookFindNextFile()
{
	originalFindNextFile = nCodeHook.createHookByName("kernel32.dll", "FindNextFileA", FindNextFileHook);
}
#endif




typedef HANDLE  (WINAPI *CreateFileAFPtr)(     __in     LPCSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
);
CreateFileAFPtr originalCreateFileA = nullptr;

HANDLE  WINAPI CreateFileAHook(    __in     LPCSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
)
{
	HANDLE ret = originalCreateFileA(lpFileName,dwDesiredAccess, dwShareMode,lpSecurityAttributes,dwCreationDisposition,dwFlagsAndAttributes  ,hTemplateFile   );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "CreateFileA in=[%s] ret=%s\n", lpFileName ? lpFileName : "<null>", (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
	if( ret == INVALID_HANDLE_VALUE )
	{
		auto resolvedWide = Utility::GetWidePath( lpFileName );
		{
			FILE* lf = nullptr;
			if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
			{
				fprintf(lf, "ansi_in=[%s]\n", lpFileName);
				char wideAsAnsiForLog[MAX_PATH*2] = {};
				WideCharToMultiByte(CP_UTF8, 0, resolvedWide.c_str(), -1, wideAsAnsiForLog, sizeof(wideAsAnsiForLog), 0, 0);
				fprintf(lf, "resolved_wide(utf8)=[%s]\n", wideAsAnsiForLog);
				fclose(lf);
			}
		}
		ret = CreateFileW(resolvedWide.c_str(), dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes  ,hTemplateFile   );
		DWORD lastErr2 = ::GetLastError();
		{
			FILE* lf = nullptr;
			if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
			{
				fprintf(lf, "CreateFileW result: %s (lastError=%lu)\n", (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL", lastErr2);
				fclose(lf);
			}
		}
		::SetLastError(lastErr2);
	}
	return ret;
}
void hookCreateFileA()
{
	originalCreateFileA = nCodeHook.createHookByName("kernelbase.dll", "CreateFileA", CreateFileAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
	{
		fprintf(lf, "hookCreateFileA install %s\n", originalCreateFileA ? "OK" : "FAILED");
		fclose(lf);
	}
}

// Diagnostic-only: CreateFileA is the only variant this patch has ever needed to fix up,
// on the assumption Leeyes' own file I/O is ANSI/MBCS throughout. This hook exists only to
// confirm or rule that out - if Leeyes (or a Tnt-Unicode-aware code path inside it) calls
// CreateFileW directly with an already-corrupted ('?'-containing) wide string, no ANSI hook
// could ever see or fix that call.
typedef HANDLE  (WINAPI *CreateFileWFPtr)(     __in     LPCWSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
);
CreateFileWFPtr originalCreateFileW = nullptr;
HANDLE  WINAPI CreateFileWHook(    __in     LPCWSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
)
{
	HANDLE ret = originalCreateFileW(lpFileName,dwDesiredAccess, dwShareMode,lpSecurityAttributes,dwCreationDisposition,dwFlagsAndAttributes  ,hTemplateFile   );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			char utf8[MAX_PATH*2] = {};
			WideCharToMultiByte(CP_UTF8, 0, lpFileName ? lpFileName : L"<null>", -1, utf8, sizeof(utf8), 0, 0);
			fprintf(lf, "CreateFileW in(utf8)=[%s] ret=%s\n", utf8, (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
	return ret;
}
void hookCreateFileW()
{
	originalCreateFileW = nCodeHook.createHookByName("kernelbase.dll", "CreateFileW", CreateFileWHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
	{
		fprintf(lf, "hookCreateFileW install %s\n", originalCreateFileW ? "OK" : "FAILED");
		fclose(lf);
	}
}

// Diagnostic-only: CreateFile2 is the Unicode-only file-open API introduced in Windows 8,
// commonly used by WIC (Windows Imaging Component) - which this codebase's own WIC_Loader.spi
// plugin depends on - to open files for decoding. If Leeyes' image pipeline goes through WIC
// somewhere, CreateFile2 could be the actual file-open call, entirely invisible to the
// CreateFileA/W hooks above.
typedef HANDLE (WINAPI *CreateFile2FPtr)(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, LPVOID pCreateExParams);
CreateFile2FPtr originalCreateFile2 = nullptr;
HANDLE WINAPI CreateFile2Hook(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode, DWORD dwCreationDisposition, LPVOID pCreateExParams)
{
	HANDLE ret = originalCreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			char utf8[MAX_PATH*2] = {};
			WideCharToMultiByte(CP_UTF8, 0, lpFileName ? lpFileName : L"<null>", -1, utf8, sizeof(utf8), 0, 0);
			fprintf(lf, "CreateFile2 in(utf8)=[%s] ret=%s\n", utf8, (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
	return ret;
}
void hookCreateFile2()
{
	originalCreateFile2 = nCodeHook.createHookByName("kernelbase.dll", "CreateFile2", CreateFile2Hook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
	{
		fprintf(lf, "hookCreateFile2 install %s\n", originalCreateFile2 ? "OK" : "FAILED");
		fclose(lf);
	}
}


// Many apps (Leeyes included, apparently) check a file's existence/attributes via
// GetFileAttributesA before attempting to actually open it. Without this hook, that
// pre-check fails on a '?'-corrupted ANSI name even though CreateFileAHook's fallback
// would have successfully opened the real file, so the app never gets that far.
typedef DWORD (WINAPI *GetFileAttributesAFPtr)( LPCSTR lpFileName );
GetFileAttributesAFPtr originalGetFileAttributesA = nullptr;
DWORD WINAPI GetFileAttributesAHook( LPCSTR lpFileName )
{
	DWORD ret = originalGetFileAttributesA( lpFileName );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "GetFileAttributesA in=[%s] ret=%s\n", lpFileName ? lpFileName : "<null>", (ret != INVALID_FILE_ATTRIBUTES) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
	if( ret == INVALID_FILE_ATTRIBUTES )
	{
		auto resolvedWide = Utility::GetWidePath( lpFileName );
		ret = ::GetFileAttributesW( resolvedWide.c_str() );
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			char wideAsAnsiForLog[MAX_PATH*2] = {};
			WideCharToMultiByte(CP_UTF8, 0, resolvedWide.c_str(), -1, wideAsAnsiForLog, sizeof(wideAsAnsiForLog), 0, 0);
			fprintf(lf, "GetFileAttributesA ansi_in=[%s] resolved_wide(utf8)=[%s] result=%s\n",
				lpFileName, wideAsAnsiForLog, (ret != INVALID_FILE_ATTRIBUTES) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
	return ret;
}
void hookGetFileAttributesA()
{
	originalGetFileAttributesA = nCodeHook.createHookByName("kernelbase.dll", "GetFileAttributesA", GetFileAttributesAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
	{
		fprintf(lf, "hookGetFileAttributesA install %s\n", originalGetFileAttributesA ? "OK" : "FAILED");
		fclose(lf);
	}
}

typedef BOOL (WINAPI *GetFileAttributesExAFPtr)( LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation );
GetFileAttributesExAFPtr originalGetFileAttributesExA = nullptr;
BOOL WINAPI GetFileAttributesExAHook( LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation )
{
	BOOL ret = originalGetFileAttributesExA( lpFileName, fInfoLevelId, lpFileInformation );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "GetFileAttributesExA in=[%s] ret=%s\n", lpFileName ? lpFileName : "<null>", ret ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
	if( !ret )
	{
		ret = ::GetFileAttributesExW( Utility::GetWidePath( lpFileName ).c_str(), fInfoLevelId, lpFileInformation );
	}
	return ret;
}
void hookGetFileAttributesExA()
{
	originalGetFileAttributesExA = nCodeHook.createHookByName("kernelbase.dll", "GetFileAttributesExA", GetFileAttributesExAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
	{
		fprintf(lf, "hookGetFileAttributesExA install %s\n", originalGetFileAttributesExA ? "OK" : "FAILED");
		fclose(lf);
	}
}


// p9np.dll (WSL's \\wsl$ network provider) has an internal bug that throws an uncaught
// C++ exception - crashing the whole process - when something in this process (typically
// shell/COM code enumerating network locations) causes it to load and run. Leeyes has no
// legitimate use for \\wsl$ paths, so simply refusing to ever load that one DLL into this
// process removes the crash at its source without touching any system-wide setting.
bool IsBlockedLibrary( LPCSTR name )
{
	if( !name ) return false;
	auto slash = strrchr( name, '\\' );
	auto leaf = slash ? slash + 1 : name;
	return _stricmp( leaf, "p9np.dll" ) == 0;
}
bool IsBlockedLibraryW( LPCWSTR name )
{
	if( !name ) return false;
	auto slash = wcsrchr( name, L'\\' );
	auto leaf = slash ? slash + 1 : name;
	return _wcsicmp( leaf, L"p9np.dll" ) == 0;
}

typedef HMODULE (WINAPI *LoadLibraryAFPtr)( LPCSTR lpLibFileName );
LoadLibraryAFPtr originalLoadLibraryA = nullptr;
HMODULE WINAPI LoadLibraryAHook( LPCSTR lpLibFileName )
{
	if( IsBlockedLibrary( lpLibFileName ) ) { ::SetLastError( ERROR_MOD_NOT_FOUND ); return NULL; }
	return originalLoadLibraryA( lpLibFileName );
}

typedef HMODULE (WINAPI *LoadLibraryWFPtr)( LPCWSTR lpLibFileName );
LoadLibraryWFPtr originalLoadLibraryW = nullptr;
HMODULE WINAPI LoadLibraryWHook( LPCWSTR lpLibFileName )
{
	if( IsBlockedLibraryW( lpLibFileName ) ) { ::SetLastError( ERROR_MOD_NOT_FOUND ); return NULL; }
	return originalLoadLibraryW( lpLibFileName );
}

typedef HMODULE (WINAPI *LoadLibraryExAFPtr)( LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags );
LoadLibraryExAFPtr originalLoadLibraryExA = nullptr;
HMODULE WINAPI LoadLibraryExAHook( LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags )
{
	if( IsBlockedLibrary( lpLibFileName ) ) { ::SetLastError( ERROR_MOD_NOT_FOUND ); return NULL; }
	return originalLoadLibraryExA( lpLibFileName, hFile, dwFlags );
}

typedef HMODULE (WINAPI *LoadLibraryExWFPtr)( LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags );
LoadLibraryExWFPtr originalLoadLibraryExW = nullptr;
HMODULE WINAPI LoadLibraryExWHook( LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags )
{
	if( IsBlockedLibraryW( lpLibFileName ) ) { ::SetLastError( ERROR_MOD_NOT_FOUND ); return NULL; }
	return originalLoadLibraryExW( lpLibFileName, hFile, dwFlags );
}

void hookLoadLibrary()
{
	// if p9np.dll is already loaded in this process (e.g. loaded before our hooks were
	// installed) blocking further loads wouldn't help, but that only happens if something
	// touched it before our DllMain ran, which is not the case for the crashes observed.
	originalLoadLibraryA = nCodeHook.createHookByName("kernel32.dll", "LoadLibraryA", LoadLibraryAHook);
	originalLoadLibraryW = nCodeHook.createHookByName("kernel32.dll", "LoadLibraryW", LoadLibraryWHook);
	originalLoadLibraryExA = nCodeHook.createHookByName("kernel32.dll", "LoadLibraryExA", LoadLibraryExAHook);
	originalLoadLibraryExW = nCodeHook.createHookByName("kernel32.dll", "LoadLibraryExW", LoadLibraryExWHook);
}

// kernel32's LoadLibrary* functions are thin wrappers over ntdll's LdrLoadDll; internal
// OS components (like the network-provider loading code in mpr.dll) often call LdrLoadDll
// directly, bypassing the kernel32 hooks above entirely. Hook the lower-level entry point
// too so p9np.dll is blocked regardless of how something tries to load it.
typedef NTSTATUS (NTAPI *LdrLoadDllFPtr)( PWCHAR PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName, PVOID *ModuleHandle );
LdrLoadDllFPtr originalLdrLoadDll = nullptr;
NTSTATUS NTAPI LdrLoadDllHook( PWCHAR PathToFile, PULONG Flags, PUNICODE_STRING ModuleFileName, PVOID *ModuleHandle )
{
	if( ModuleFileName && ModuleFileName->Buffer && ModuleFileName->Length > 0 )
	{
		std::wstring name( ModuleFileName->Buffer, ModuleFileName->Length / sizeof(WCHAR) );
		if( IsBlockedLibraryW( name.c_str() ) )
		{
			return (NTSTATUS)0xC0000135; // STATUS_DLL_NOT_FOUND
		}
	}
	return originalLdrLoadDll( PathToFile, Flags, ModuleFileName, ModuleHandle );
}
void hookLdrLoadDll()
{
	originalLdrLoadDll = nCodeHook.createHookByName("ntdll.dll", "LdrLoadDll", LdrLoadDllHook);
}

// Blocking p9np.dll's own load (above) stops its internal bug from running, but components
// that enumerate ALL registered network providers (mpr.dll, the shell's network-location
// code) still see P9NP listed in HKLM\...\NetworkProvider\Order and try to involve it, which
// on this system still ends up throwing an uncaught exception in some code paths even though
// p9np.dll itself never actually loads. Filtering P9NP out of what THIS PROCESS sees when it
// reads that one registry value reproduces the effect of removing it from the machine-wide
// list (which is what actually stops the crash - proven repeatedly) without writing to the
// registry at all, so no administrator rights are needed and no other process is affected.
std::wstring RemoveP9NPFromProviderOrder( const std::wstring& value )
{
	std::wstring result;
	size_t start = 0;
	while( start <= value.size() )
	{
		auto comma = value.find( L',', start );
		auto end = (comma == std::wstring::npos) ? value.size() : comma;
		auto segment = value.substr( start, end - start );
		if( _wcsicmp( segment.c_str(), L"P9NP" ) != 0 )
		{
			if( !result.empty() ) result += L',';
			result += segment;
		}
		if( comma == std::wstring::npos ) break;
		start = comma + 1;
	}
	return result;
}

typedef LONG (WINAPI *RegQueryValueExWFPtr)( HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData );
RegQueryValueExWFPtr originalRegQueryValueExW = nullptr;
LONG WINAPI RegQueryValueExWHook( HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData )
{
	LONG ret = originalRegQueryValueExW( hKey, lpValueName, lpReserved, lpType, lpData, lpcbData );
	if( ret == ERROR_SUCCESS && lpData && lpcbData && lpValueName && _wcsicmp( lpValueName, L"ProviderOrder" ) == 0
		&& (!lpType || *lpType == REG_SZ) )
	{
		std::wstring value( reinterpret_cast<LPCWSTR>(lpData) );
		if( value.find(L"P9NP") != std::wstring::npos )
		{
			auto filtered = RemoveP9NPFromProviderOrder( value );
			auto byteLen = (filtered.size() + 1) * sizeof(WCHAR);
			memcpy( lpData, filtered.c_str(), byteLen ); // always <= original, safe in-place
			*lpcbData = (DWORD)byteLen;
		}
	}
	return ret;
}
void hookRegQueryValueExW()
{
	originalRegQueryValueExW = nCodeHook.createHookByName("advapi32.dll", "RegQueryValueExW", RegQueryValueExWHook);
}





typedef __out_opt HANDLE  (WINAPI *CreateMutexAFPtr)(     __in_opt LPSECURITY_ATTRIBUTES lpMutexAttributes,
    __in     BOOL bInitialOwner,
    __in_opt LPCSTR lpName
);
CreateMutexAFPtr originalCreateMutexA = nullptr;
__out_opt HANDLE WINAPI CreateMutexAHook(    __in_opt LPSECURITY_ATTRIBUTES lpMutexAttributes,
    __in     BOOL bInitialOwner,
    __in_opt LPCSTR lpName
    )
{
	auto ret = originalCreateMutexA( lpMutexAttributes, bInitialOwner, lpName );
	if( lpName && strcmp(lpName, "Leeyes@Kenji" )  == 0 )
	{
		if(::GetLastError() == ERROR_ALREADY_EXISTS)
		{
			SetLastError( NO_ERROR );
		}
	}
	return ret;
}
void hookCreateMutexA()
{
	originalCreateMutexA = nCodeHook.createHookByName("kernelbase.dll", "CreateMutexA", CreateMutexAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
	{
		fprintf(lf, "hookCreateMutexA install %s\n", originalCreateMutexA ? "OK" : "FAILED");
		fclose(lf);
	}
}

}

namespace User32
{
	typedef BOOL (WINAPI *SetWindowTextAFPtr)( HWND hWnd, LPCSTR lpString );
	SetWindowTextAFPtr originalSetWindowTextA = nullptr;

	bool LooksLikePath( LPCSTR text )
	{
		if( !text || !*text ) return false;
		return (text[0] && text[1] == ':' && (text[2] == '\\' || text[2] == '/'))
			|| strchr( text, '\\' ) != nullptr
			|| strchr( text, '/' ) != nullptr;
	}

	BOOL WINAPI SetWindowTextAHook( HWND hWnd, LPCSTR lpString )
	{
		static thread_local bool resolving = false;
		if( !resolving && lpString && strchr( lpString, '?' ) != nullptr
			&& LooksLikePath( lpString ) && ::IsWindowUnicode( hWnd ) )
		{
			resolving = true;
			std::wstring resolved = Utility::GetWidePath( lpString );
			bool resolvedSuccessfully = !resolved.empty()
				&& resolved.find( L'?' ) == std::wstring::npos
				&& ::GetFileAttributesW( resolved.c_str() ) != INVALID_FILE_ATTRIBUTES;
			if( resolvedSuccessfully )
			{
				BOOL ret = ::SetWindowTextW( hWnd, resolved.c_str() );
				resolving = false;
				return ret;
			}
			resolving = false;
		}
		return originalSetWindowTextA( hWnd, lpString );
	}

	void hookSetWindowTextA()
	{
		originalSetWindowTextA = nCodeHook.createHookByName("user32.dll", "SetWindowTextA", SetWindowTextAHook);
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "hookSetWindowTextA install %s\n", originalSetWindowTextA ? "OK" : "FAILED");
			fclose(lf);
		}
	}
}

namespace Shell32
{
#if 1

void WriteToBufer(LPCITEMIDLIST pIDlist,const LPSTRRET pStrret, LPSTR Buf, long Length)
{
	switch( pStrret->uType )
	{
	case STRRET_WSTR:
		if(pStrret->pOleStr[0])
		{
			int length =::WideCharToMultiByte(932, 0,  pStrret->pOleStr, -1 , NULL, 0, 0, NULL);
			if( length <= Length )
			{
				::WideCharToMultiByte(932, 0, pStrret->pOleStr, -1, Buf, Length, 0, NULL);
			}
		}
		break;
	case STRRET_OFFSET:
		strcpy_s(Buf, Length, ( ( reinterpret_cast<LPCSTR>(pIDlist) ) + pStrret->uOffset ) );
		break;
	case STRRET_CSTR:
		strcpy_s(Buf, Length, pStrret->cStr );
		break;
	}
}
	

//�K�v�ȕ��������Z���p�X�ɁE�c���[�r���[��FindFirstFile�ƌ��ʂ��Ⴄ�Ə��Ƀt�@�C�������d���A�A�h���X�o�[�Ō��ɂ���
std::wstring MakePath(std::string MBCSPath, std::wstring WidePath )
{
	std::list< bool >  flag_list;
	std::string tmp;
	bool hit = false;
	for(  auto itr=MBCSPath.begin() ; itr != MBCSPath.end(); ++itr )
	{
		if( isleadbyte( static_cast<BYTE>(*itr) ) )
		{	
			++itr;
			continue;
		}

		if( *itr == '?' )		
		{
			hit = true;
		}	
		else if( *itr == '\\' )
		{
			flag_list.push_back( hit );
			hit = false;
		}
	}
	flag_list.push_back( hit );

	wchar_t short_path_name[ MAX_PATH ];
	auto len = ::GetShortPathNameW( WidePath.c_str(), short_path_name, MAX_PATH );
	if( len ==0)
	{
		return WidePath;
	}
	std::list< std::wstring >  long_name_list;
	std::list< std::wstring >  short_name_list;

	std::wstring tmp2;
	for( auto itr : WidePath )
	{
		tmp2.push_back( itr );
		if( itr == L'\\' )
		{
			long_name_list.push_back( tmp2 );
			tmp2.clear();
		}	
	}
	long_name_list.push_back( tmp2 );

	tmp2.clear();
	std::wstring short_name( short_path_name );
	for( auto itr : short_name )
	{
		tmp2.push_back( itr );
		if( itr == L'\\' )
		{
			short_name_list.push_back( tmp2 );
			tmp2.clear();
		}	
	}
	short_name_list.push_back( tmp2 );

	if( short_name_list.size() != long_name_list.size() || flag_list.size() != long_name_list.size() )
	{
		return short_name;
	}	

	std::wstring result;
	auto f = flag_list.begin();
	auto sn = short_name_list.begin();
	auto ln = long_name_list.begin(); 
	for(; f != flag_list.end(); ++f, ++sn, ++ln )
	{
		if( *f )
		{
			result += *sn;
		}
		else
		{
			result += *ln;
		}
	}
	return result;
}

typedef HRESULT (STDMETHODCALLTYPE *GetDisplayNameOfFPtr )( IShellFolder* This,	PCUITEMID_CHILD pidl,    SHGDNF uFlags,   STRRET *pName);

UINT_PTR GetQueryInterfacePtr( IShellFolder* This)
{
	auto pVtbl = *reinterpret_cast< UINT_PTR** >(This);
	return pVtbl[0];
}


typedef std::map<UINT_PTR,GetDisplayNameOfFPtr> GetDisplayNameOfFPtrMap;
//typedef concurrency::concurrent_unordered_map<UINT_PTR,GetDisplayNameOfFPtr> GetDisplayNameOfFPtrMap;
static GetDisplayNameOfFPtrMap  gGetDisplayNameOfFPtrMap;
std::mutex gMutex;

 // some shell namespace items (network locations in particular) can raise a
 // structured exception from deep inside their own GetDisplayNameOf implementation
 // as part of their own internal error handling; that unwinding relies on SEH frames
 // set up in the function's original prologue, which our inline hook overwrites, so
 // the exception can end up escaping uncaught and killing the whole process instead
 // of being handled where the shell namespace code expects. Catch it here instead.
 // (kept as a separate function with no C++ objects needing unwinding, since __try/
 // __except cannot appear directly in a function that also does object unwinding.)
 bool CallOrigDisplayNameOf( GetDisplayNameOfFPtr orig_func, IShellFolder* This, PCUITEMID_CHILD pidl, SHGDNF uFlags, STRRET *pName, HRESULT* outRet )
 {
	__try
	{
		*outRet = orig_func(This, pidl, uFlags, pName);
		return true;
	}
	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		return false;
	}
 }

  HRESULT STDMETHODCALLTYPE GetDisplayNameOfHook( IShellFolder * This,	 PCUITEMID_CHILD pidl,    SHGDNF uFlags,   STRRET *pName)
 {
	 GetDisplayNameOfFPtr orig_func = nullptr;
	 {
		std::lock_guard<std::mutex> lock(gMutex);
		auto func = gGetDisplayNameOfFPtrMap.find( GetQueryInterfacePtr( This) );
		if( func != gGetDisplayNameOfFPtrMap.end() )
		{
			orig_func = func->second;
		}
	 }
	 if(orig_func)
	 {
		HRESULT ret = E_FAIL;
		if( !CallOrigDisplayNameOf( orig_func, This, pidl, uFlags, pName, &ret ) )
		{
			return E_FAIL;
		}
		if( SUCCEEDED(ret) && uFlags & SHGDN_FORPARSING )
		{
			char buf[MAX_PATH]={};
			WriteToBufer(pidl, pName,  buf, MAX_PATH );
			if(  strchr( buf, '?' ) )
			{
				if(pName->uType == STRRET_WSTR && pName->pOleStr[0] )
				{
					auto full_path = MakePath( buf, pName->pOleStr );
					if( full_path.size() < wcslen(pName->pOleStr ) )
					{
						wcscpy_s( pName->pOleStr, wcslen(pName->pOleStr )+1 , full_path.c_str() );
					}
					else
					{//���j�R�[�h�����̏ꍇ�����Ȃ鎖������
						auto new_buf = static_cast<LPWSTR>( ::CoTaskMemAlloc(  sizeof( wchar_t)*(1+full_path.size() ) ) );
						if(new_buf)
						{
							wcscpy_s( new_buf, full_path.size() +1,  full_path.c_str());
							::CoTaskMemFree(pName->pOleStr);
							pName->pOleStr =new_buf;
						}
					}
				}
			}
		}
		return ret;
	 }
	return E_FAIL;
 }


typedef HRESULT   (WINAPI *SHBindToParentFPtr)(  _In_   PCIDLIST_ABSOLUTE pidl,  _In_   REFIID riid,  _Out_  VOID **ppv,  _Out_  PCUITEMID_CHILD *ppidlLast);
SHBindToParentFPtr originalSHBindToParent = nullptr;

HRESULT   WINAPI SHBindToParentHook(  _In_   PCIDLIST_ABSOLUTE pidl,  _In_   REFIID riid,  _Out_  VOID **ppv,  _Out_  PCUITEMID_CHILD *ppidlLast)
{
	HRESULT  ret = originalSHBindToParent(pidl ,riid, ppv ,ppidlLast);
	auto psfParent = static_cast<IShellFolder*>(*ppv);
	if( SUCCEEDED( ret) && psfParent)
	{
		auto key_func = GetQueryInterfacePtr( psfParent);
		auto pVtbl = *reinterpret_cast<UINT_PTR**>(psfParent);
		std::lock_guard<std::mutex> lock(gMutex);
		auto orig_func = gGetDisplayNameOfFPtrMap.find(key_func );
		if( orig_func == gGetDisplayNameOfFPtrMap.end() )
		{
			gGetDisplayNameOfFPtrMap[ key_func ] = nCodeHook.createHook((GetDisplayNameOfFPtr)pVtbl[ 11 ], GetDisplayNameOfHook );
		}
	}
	return ret;
}

void hookSHBindToParent()
{
	originalSHBindToParent = nCodeHook.createHookByName("shell32.dll", "SHBindToParent", SHBindToParentHook);
}

typedef UINT  (WINAPI *DragQueryFileAFPtr)(__in HDROP hDrop, __in UINT iFile, __out_ecount_opt(cch) LPSTR lpszFile, __in UINT cch);
DragQueryFileAFPtr originalDragQueryFileA = nullptr;

UINT   WINAPI DragQueryFileAHook( __in HDROP hDrop, __in UINT iFile, __out_ecount_opt(cch) LPSTR lpszFile, __in UINT cch)
{
	auto size = originalDragQueryFileA( hDrop, iFile, lpszFile, cch );
	if( iFile == 0xffffffff )
	{
		return size;
	}
	else if( lpszFile )
	{
		if(size && memchr(lpszFile, '?' , size) == nullptr )
		{
			return size;
		}
		auto wsize = DragQueryFileW( hDrop, iFile,nullptr,0 );
		std::wstring wstr(  wsize+1, L'\0');
		size = DragQueryFileW( hDrop, iFile,const_cast<wchar_t*>( wstr.data() ), wstr.size() );
		return ::WideCharToMultiByte( 932,0, MakePath(lpszFile, wstr).c_str(), -1, lpszFile, cch, 0, 0 );
	}

	std::string str(  size, '\0');
	size = originalDragQueryFileA( hDrop, iFile,const_cast<char*>( str.data() ), str.size() );
	if( str.find( '?' ) == std::string::npos )
	{
		return size;
	}
	auto wsize = DragQueryFileW( hDrop, iFile,nullptr,0 );
	std::wstring wstr(  wsize, L'\0');
	size = DragQueryFileW( hDrop, iFile,const_cast<wchar_t*>( wstr.data() ), wstr.size() );
	return ::WideCharToMultiByte( 932,0, MakePath(lpszFile, wstr).c_str(), -1,nullptr, 0, 0, 0 );
}

void hookDragQueryFileA()
{
	originalDragQueryFileA = nCodeHook.createHookByName("shell32.dll", "DragQueryFileA", DragQueryFileAHook);
}


#endif
}

namespace SusieAM00
{
typedef int   (__stdcall *IsSupportedAM00FPtr)(LPSTR filename, DWORD dw);
typedef int  ( __stdcall *GetArchiveInfoAM00FPtr)(LPSTR buf, long len, unsigned int flag, HLOCAL *lphInf);
typedef int (CALLBACK *SPI_PROGRESS)(int, int, long);
typedef int (__stdcall *GetFileAM00FPtr)(LPSTR src, long len, LPSTR dest, unsigned int flag, SPI_PROGRESS lpPrgressCallback, long lData);
enum  SusieErrorCode :int
{
SPI_NO_FUNCTION	=	-1	,	/* ���̋@�\�̓C���v�������g����Ă��Ȃ� */
SPI_ALL_RIGHT,					/* ����I�� */
SPI_ABORT	,						/* �R�[���o�b�N�֐�����0��Ԃ����̂œW�J�𒆎~���� */
SPI_NOT_SUPPORT	,			/* ���m�̃t�H�[�}�b�g */
SPI_OUT_OF_ORDER,			/* �f�[�^�����Ă��� */
SPI_NO_MEMORY,				/* �������[���m�ۏo���Ȃ� */
SPI_MEMORY_ERROR,		/* �������[�G���[ */
SPI_FILE_READ_ERROR,		/* �t�@�C�����[�h�G���[ */
SPI_WINDOW_ERROR	,		/* �����J���Ȃ� (����J�̃G���[�R�[�h) */
SPI_OTHER_ERROR	,			/* �����G���[ */
SPI_FILE_WRITE_ERROR,	/* �������݃G���[ (����J�̃G���[�R�[�h) */
SPI_END_OF_FILE,				/* �t�@�C���I�[ (����J�̃G���[�R�[�h) */
};
IsSupportedAM00FPtr originalIsSupported = nullptr;
GetArchiveInfoAM00FPtr originalGetArchiveInfo = nullptr;
GetFileAM00FPtr originalGetFile = nullptr;
int __stdcall IsSupportedHook(LPSTR Filename, DWORD Dw)
{
	auto ret =originalIsSupported( Filename, Dw );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "IsSupported Filename=[%s] Dw=%lu ret=%d\n", (Dw < 0x10000 && Filename) ? Filename : "<buffer-or-null>", (unsigned long)Dw, ret);
			fclose(lf);
		}
	}
	// Per the Susie SPI convention, Dw is either a small flag/handle value or - when
	// large enough to plausibly be one - a pointer to a header buffer the caller already
	// read itself; only in the filename+flag case does the plugin need to open the file
	// by path itself, which is the only case a '?'-corrupted ANSI path could break.
	if( !ret && Dw < 0x10000 && Filename && strchr(Filename, '?') )
	{
		auto retryPath = Utility::GetShortPath( Filename );
		ret = originalIsSupported( const_cast<LPSTR>(retryPath.c_str()), Dw );
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "IsSupported retry=[%s] ret=%d\n", retryPath.c_str(), ret);
			fclose(lf);
		}
	}
	return ret;
}
int __stdcall GetArchiveInfoHook(LPSTR Buf, long Len, unsigned int Flag, HLOCAL *Inf)
{
	auto ret = originalGetArchiveInfo( Buf , Len, Flag, Inf );
	if( !(Flag & 0x07) && ret == SPI_FILE_READ_ERROR )
	{
		ret =originalGetArchiveInfo( const_cast<LPSTR>(Utility::GetShortPath( Buf ).c_str() ) , Len, Flag, Inf );
	}
	return ret;
}

int __stdcall GetFileHook(LPSTR Src, long Len, LPSTR Dst, unsigned int Flag, SPI_PROGRESS PrgressCallback, long Data)
{
	auto ret =originalGetFile( Src, Len, Dst, Flag, PrgressCallback, Data );
	if( !(Flag & 0x07)  && ret == SPI_FILE_READ_ERROR)
	{
		ret = originalGetFile(const_cast<LPSTR>(Utility::GetShortPath( Src ).c_str() ), Len, Dst, Flag, PrgressCallback, Data );
	}
	return ret;
}

// Non-BMP images (jpg, png, ...) are decoded by whichever Susie *image* plugin (.spi) is
// registered for that format, via a separate plugin interface (GetPictureInfo/GetPicture)
// from the archive one above (GetArchiveInfo/GetFile). Without hooking these too, a Unicode
// path that fails ANSI access falls straight through to the plugin and the image just
// fails to load, even though the folder/archive access above already works correctly.
typedef int (__stdcall *GetPictureInfoAM00FPtr)(LPSTR buf, long len, unsigned int flag, LPVOID lpInfo);
typedef int (__stdcall *GetPictureAM00FPtr)(LPSTR buf, long len, unsigned int flag, HANDLE *pHBInfo, HANDLE *pHBm, SPI_PROGRESS lpPrgressCallback, long lData);
GetPictureInfoAM00FPtr originalGetPictureInfo = nullptr;
GetPictureAM00FPtr originalGetPicture = nullptr;

int __stdcall GetPictureInfoHook(LPSTR Buf, long Len, unsigned int Flag, LPVOID Inf)
{
	auto ret = originalGetPictureInfo( Buf, Len, Flag, Inf );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "GetPictureInfo Buf=[%s] Len=%ld Flag=0x%x ret=%d\n", (Flag & 0x07) ? "<buffer,not-path>" : Buf, Len, Flag, ret);
			fclose(lf);
		}
	}
	if( !(Flag & 0x07) && ret == SPI_FILE_READ_ERROR )
	{
		auto retryPath = Utility::GetShortPath( Buf );
		ret = originalGetPictureInfo( const_cast<LPSTR>(retryPath.c_str() ), Len, Flag, Inf );
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "GetPictureInfo retry=[%s] ret=%d\n", retryPath.c_str(), ret);
			fclose(lf);
		}
	}
	return ret;
}

int __stdcall GetPictureHook(LPSTR Buf, long Len, unsigned int Flag, HANDLE *pHBInfo, HANDLE *pHBm, SPI_PROGRESS PrgressCallback, long Data)
{
	auto ret = originalGetPicture( Buf, Len, Flag, pHBInfo, pHBm, PrgressCallback, Data );
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "GetPicture Buf=[%s] Len=%ld Flag=0x%x ret=%d\n", (Flag & 0x07) ? "<buffer,not-path>" : Buf, Len, Flag, ret);
			fclose(lf);
		}
	}
	if( !(Flag & 0x07) && ret == SPI_FILE_READ_ERROR )
	{
		auto retryPath = Utility::GetShortPath( Buf );
		ret = originalGetPicture( const_cast<LPSTR>(retryPath.c_str() ), Len, Flag, pHBInfo, pHBm, PrgressCallback, Data );
		FILE* lf = nullptr;
		if( fopen_s(&lf, "C:\\tool\\leeyes\\leeyes261\\_path_debug.txt", "a") == 0 && lf )
		{
			fprintf(lf, "GetPicture ansi_in=[%s] retry=[%s] result=%d\n", Buf, retryPath.c_str(), ret);
			fclose(lf);
		}
	}
	return ret;
}

}

namespace
{

static std::string ArchivePluginName;

class ProfileAccessor
{
	std::string m_ProfileName;
	ProfileAccessor();
public:
	ProfileAccessor( HMODULE hModule )
	{
			char buf[MAX_PATH]={};
			GetModuleFileNameA( hModule, buf, MAX_PATH );
			auto ext_pos = strstr(buf, ".dll");
			ext_pos[1]='i';
			ext_pos[2]='n';
			ext_pos[3]='i';
			m_ProfileName = buf;
	}
	std::string Get( LPCSTR App, LPCSTR Key, LPCSTR Default )
	{
		std::vector<char> buffer(4096);
		auto len = GetPrivateProfileStringA(App,Key, Default, buffer.data() , buffer.size() , m_ProfileName.c_str() );
		return std::string( buffer.begin(), buffer.begin() +len);
	}
	UINT Get( LPCSTR Section, LPCSTR Key, UINT Default )
	{
		return GetPrivateProfileIntA(Section,Key, Default,  m_ProfileName.c_str() );
	}

	bool Set( LPCSTR Section, LPCSTR Key, LPCSTR Value )
	{
		return FALSE != WritePrivateProfileStringA( Section, Key, Value, m_ProfileName.c_str() );
	}
	bool Set( LPCSTR Section, LPCSTR Key, UINT Value )
	{
		return FALSE != Set( Section, Key, std::to_string( Value ).c_str() );
	}
};

}
namespace SusieAM00
{
typedef FARPROC   (WINAPI *GetProcAddressFPtr)(      _In_ HMODULE hModule,    _In_ LPCSTR lpProcName );
GetProcAddressFPtr originalGetProcAddress = nullptr;

 bool Compare( LPCSTR str1, LPCSTR str2 )
 {
	while( *str1 && *str2)
	{
		if( *str1 != *str2 ) return false;
		++str1;
		++str2;
	}
	return true;
 }

FARPROC WINAPI GetProcAddressHook(    _In_ HMODULE hModule,    _In_ LPCSTR lpProcName 	)
{
		// GetModuleHandleA only inspects already-loaded modules - unlike LoadLibraryA it
		// never loads/unloads the plugin itself, so it can't reenter the plugin's own
		// DllMain from inside this hook (which fires on every single GetProcAddress call
		// process-wide, including ones made by the CRT/loader while a module is loading).
		auto am = GetModuleHandleA( ArchivePluginName.c_str() );
		auto ret = originalGetProcAddress( hModule, lpProcName );
		if( am && hModule == am)
		{
			if( strcmp( lpProcName, "GetArchiveInfo" )== 0 )
			{
				nCodeHook.removeHook( GetArchiveInfoHook);
				originalGetArchiveInfo = nCodeHook.createHook((GetArchiveInfoAM00FPtr)ret,  GetArchiveInfoHook);
			}
			else if( strcmp( lpProcName, "GetFile" )== 0 )
			{
				nCodeHook.removeHook( GetFileHook);
				originalGetFile = nCodeHook.createHook((GetFileAM00FPtr)ret, GetFileHook);
			}
		}
		// image plugins are not configured by name like the one archive plugin above -
		// Leeyes can use any of several installed .spi files depending on the file format -
		// so hook these on whichever module they are resolved from, not just one fixed name.
		if( ret )
		{
			if( strcmp( lpProcName, "GetPictureInfo" )== 0 )
			{
				nCodeHook.removeHook( GetPictureInfoHook );
				originalGetPictureInfo = nCodeHook.createHook((GetPictureInfoAM00FPtr)ret, GetPictureInfoHook);
			}
			else if( strcmp( lpProcName, "GetPicture" )== 0 )
			{
				nCodeHook.removeHook( GetPictureHook );
				originalGetPicture = nCodeHook.createHook((GetPictureAM00FPtr)ret, GetPictureHook);
			}
			else if( strcmp( lpProcName, "IsSupported" )== 0 )
			{
				nCodeHook.removeHook( IsSupportedHook );
				originalIsSupported = nCodeHook.createHook((IsSupportedAM00FPtr)ret, IsSupportedHook);
			}
		}
		return ret;

}




void hookGetProcAddress()
{
	originalGetProcAddress = nCodeHook.createHookByName("kernel32.dll", "GetProcAddress", GetProcAddressHook);
}

}


namespace Diagnostics
{
void LogAddr(FILE* f, LPCVOID addr)
{
	fprintf(f, " %p", addr);
	HMODULE hMod = nullptr;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCSTR)addr, &hMod) && hMod)
	{
		char modName[MAX_PATH] = {};
		GetModuleFileNameA(hMod, modName, MAX_PATH);
		fprintf(f, "[%s+0x%p]", modName, (void*)((BYTE*)addr - (BYTE*)hMod));
	}
}

void LogAbort(const char* reason)
{
	FILE* f = nullptr;
	if (fopen_s(&f, "C:\\tool\\leeyes\\leeyes261\\_abort_log.txt", "a") == 0 && f)
	{
		fprintf(f, "%s stack:", reason);
		void* stack[24] = {};
		USHORT n = CaptureStackBackTrace(0, 24, stack, nullptr);
		for (USHORT i = 0; i < n; ++i) LogAddr(f, stack[i]);
		fprintf(f, "\n");
		fclose(f);
	}
}

void __cdecl OnTerminate()
{
	LogAbort("std::terminate");
	abort();
}

void __cdecl OnAbort(int)
{
	LogAbort("SIGABRT");
}

LONG WINAPI VectoredHandler(PEXCEPTION_POINTERS info)
{
	FILE* f = nullptr;
	if (fopen_s(&f, "C:\\tool\\leeyes\\leeyes261\\_abort_log.txt", "a") == 0 && f)
	{
		fprintf(f, "exception code=0x%08X eip=", info->ExceptionRecord->ExceptionCode);
		LogAddr(f, info->ExceptionRecord->ExceptionAddress);
		fprintf(f, " stack:");
		void* stack[24] = {};
		USHORT n = CaptureStackBackTrace(0, 24, stack, nullptr);
		for (USHORT i = 0; i < n; ++i) LogAddr(f, stack[i]);
		fprintf(f, "\n");
		fclose(f);
	}
	return EXCEPTION_CONTINUE_SEARCH;
}
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
					 )
{
	static std::unique_ptr< ProfileAccessor > Profile;
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
		// install first, before anything else has a chance to trigger p9np's load
		Kernel32::hookLoadLibrary();
		Kernel32::hookLdrLoadDll();
		Kernel32::hookRegQueryValueExW();
		AddVectoredExceptionHandler(1, Diagnostics::VectoredHandler);
		std::set_terminate(Diagnostics::OnTerminate);
		signal(SIGABRT, Diagnostics::OnAbort);
		setlocale(LC_ALL, "Japanese_Japan.932");
		::CoInitialize(0);

		//MessageBoxW(0,L"for Debug",0,0 );
		{
			Profile.reset( new ProfileAccessor( hModule ) );
			ArchivePluginName = Profile->Get("Archive","FileName", "ax7z.spi");
		}

		if(Profile->Get( "Option", "HookFindFirstFile", UINT() ) )
		{//��������L���ɂ���ƒZ���p�X�ŕ\������邽�ߏ��Ԃ��ς�����茩�h���Ďg���ɂ��������Ƀv���O�C����I�΂Ȃ�
			Kernel32::hookFindFirstFile();
			Kernel32::hookFindNextFile();
		}
		{//���Ƀt�@�C���Ή��p
			Kernel32::hookCreateFileA();
			Kernel32::hookCreateFileW();
			Kernel32::hookCreateFile2();
			Kernel32::hookGetFileAttributesA();
			Kernel32::hookGetFileAttributesExA();
		}
		if( Profile->Get("Option","HookSetWindowText",UINT() ) )
		{
			User32::hookSetWindowTextA();
		}
		//�p�X�Ɖ摜�t�@�C������Unicode�Ȃ炱�ꂾ���ł����邪���Ƀt�@�C�����ʖ�
		Shell32::hookSHBindToParent();
		//���Ƀv���O�C�����t�b�N���邱�ƂŃv���O�C���{�̘M�炸�ɑΉ�
		SusieAM00::hookGetProcAddress();
		if( Profile->Get("Option","HookCreateMutex",UINT() ) )
		{//���d�N��������
			Kernel32::hookCreateMutexA();
		}
		Shell32::hookDragQueryFileA();
		break;
	case DLL_THREAD_ATTACH:
		setlocale(LC_ALL, "Japanese_Japan.932");
		::CoInitialize(0);
		break;
	case DLL_THREAD_DETACH:
		::CoUninitialize();
		break;
	case DLL_PROCESS_DETACH:
		Profile->Set("Archive","FileName",ArchivePluginName.c_str() );
		::CoUninitialize();
		break;
	}
	return TRUE;
}

