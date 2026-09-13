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
#include <atomic>
#include <map>
#include <winternl.h>
//#include <concurrent_unordered_set.h>
//#include <concurrent_unordered_map.h>

// File-level tracing is useful while investigating a new API path, but opening
// and closing a log file for every image access makes normal thumbnail loading
// disproportionately slow. Enable explicitly for diagnostics when needed.
#ifndef UNICODEHACK_PATH_DEBUG
#define UNICODEHACK_PATH_DEBUG 0
#endif

// Temporary, narrow diagnostics for image-plugin path failures. This is kept
// separate from the expensive per-file tracing above and is disabled again
// after the WebP path is confirmed.
#ifndef UNICODEHACK_PLUGIN_DEBUG
#define UNICODEHACK_PLUGIN_DEBUG 0
#endif

// NOTE: every injected dll has to export at least one symbol - otherwise
// the OS loader will fail with STATUS_INVALID_IMAGE_FORMAT (0x0C000007B)
__declspec(dllexport) void dummyExport() {}

// NOTE: this needs to be in global scope - otherwise the trampolines and hooks
// are deleted when the destructor of nCodeHook is called!
NCodeHookIA32 nCodeHook;
HMODULE gInjectedModule = nullptr;

namespace Utility
{
	std::map<std::string, std::wstring> gWidePathCache;
	std::mutex gWidePathCacheMutex;
	std::map<std::string, std::wstring> gSyntheticPathAliases;
	std::mutex gSyntheticPathAliasesMutex;
	std::map<std::string, std::wstring> gSyntheticLeafAliases;
	std::map<std::wstring, std::string> gFullyAsciiAliasCache;
	std::mutex gFullyAsciiAliasCacheMutex;
	std::map<std::wstring, std::string> gPrivateDirectoryAliasCache;
	std::mutex gPrivateDirectoryAliasCacheMutex;
	// ANSI Leeyes controls cannot carry every Unicode filename.  Keep a
	// process-local reverse map for names that lost characters during CP932
	// conversion so the drawing hook can send the original UTF-16 name to the
	// Unicode window implementation.  The newest conversion wins because the
	// same lossy basename can legitimately occur in different directories.
	std::map<std::string, std::wstring> gLossyDisplayNames;
	std::mutex gLossyDisplayNamesMutex;

	void RememberLossyDisplayName( const std::string& ansi, const std::wstring& wide )
	{
		if( ansi.empty() || wide.empty() || ansi.find('?') == std::string::npos ) return;
		std::lock_guard<std::mutex> lock(gLossyDisplayNamesMutex);
		auto remember = [&]( const std::string& key, const std::wstring& value )
		{
			if( key.empty() ) return;
			// Conversion and drawing are normally adjacent operations. Keep the
			// newest value so repeated lossy spellings from different directories
			// do not permanently make the display name unusable.
			gLossyDisplayNames[key] = value;
		};
		remember(ansi, wide);
		auto slash = ansi.find_last_of("\\/");
		auto wideSlash = wide.find_last_of(L"\\/");
		if( slash != std::string::npos && wideSlash != std::wstring::npos )
			remember(ansi.substr(slash + 1), wide.substr(wideSlash + 1));
	}

	bool LookupLossyDisplayName( LPCSTR text, int length, std::wstring& wide )
	{
		if( !text ) return false;
		std::string key(text, length < 0 ? strlen(text) : (size_t)length);
		std::lock_guard<std::mutex> lock(gLossyDisplayNamesMutex);
		auto found = gLossyDisplayNames.find(key);
		if( found != gLossyDisplayNames.end() )
		{
			wide = found->second;
			return true;
		}

		// Status bars and path captions often prepend a label to the lossy
		// basename instead of passing the basename as a standalone string.
		// Replace the longest remembered component and decode the untouched
		// CP932 pieces normally. This keeps the fix useful outside DrawTextA.
		size_t bestPos = std::string::npos;
		size_t bestLength = 0;
		std::wstring bestValue;
		for( const auto& candidate : gLossyDisplayNames )
		{
			if( candidate.first.size() <= bestLength ) continue;
			size_t pos = key.find(candidate.first);
			if( pos == std::string::npos ) continue;
			bestPos = pos;
			bestLength = candidate.first.size();
			bestValue = candidate.second;
		}
		if( bestPos == std::string::npos ) return false;

		auto decode = []( const char* source, size_t count, std::wstring& result ) -> bool
		{
			if( count == 0 ) return true;
			int required = ::MultiByteToWideChar(932, 0, source, (int)count, nullptr, 0);
			if( required <= 0 ) return false;
			std::wstring converted((size_t)required, L'\0');
			if( ::MultiByteToWideChar(932, 0, source, (int)count,
				&converted[0], required) != required ) return false;
			result += converted;
			return true;
		};
		std::wstring result;
		if( !decode(key.data(), bestPos, result) ) return false;
		result += bestValue;
		if( !decode(key.data() + bestPos + bestLength,
			key.size() - bestPos - bestLength, result) ) return false;
		wide.swap(result);
		return true;
	}

	bool HasSyntheticAlias( const std::string& path )
	{
		return path.find("\\~u") != std::string::npos;
	}

	void RememberSyntheticPathAlias( const std::string& aliasPath, const std::wstring& realPath )
	{
		if( aliasPath.empty() || realPath.empty() ) return;
	std::lock_guard<std::mutex> lock(gSyntheticPathAliasesMutex);
	gSyntheticPathAliases[aliasPath] = realPath;
	auto slash = aliasPath.find_last_of('\\');
	if( slash != std::string::npos && slash + 1 < aliasPath.size() )
		gSyntheticLeafAliases[aliasPath.substr(slash + 1)] = realPath;
	}

	bool LookupSyntheticPathAlias( const std::string& aliasPath, std::wstring& realPath )
	{
		if( !HasSyntheticAlias(aliasPath) ) return false;
	std::lock_guard<std::mutex> lock(gSyntheticPathAliasesMutex);
	auto it = gSyntheticPathAliases.find( aliasPath );
	if( it != gSyntheticPathAliases.end() )
	{
		realPath = it->second;
		return true;
	}
	auto slash = aliasPath.find_last_of('\\');
	if( slash == std::string::npos || slash + 1 >= aliasPath.size() ) return false;
	auto leaf = gSyntheticLeafAliases.find(aliasPath.substr(slash + 1));
	if( leaf == gSyntheticLeafAliases.end() ) return false;
	realPath = leaf->second;
	return true;
}

	const char* GetPathDebugLogPath()
	{
		static std::string path;
		if( path.empty() )
		{
			char modulePath[MAX_PATH] = {};
			if( gInjectedModule && ::GetModuleFileNameA(gInjectedModule, modulePath, MAX_PATH) )
			{
				char* slash = strrchr( modulePath, '\\' );
				if( slash )
				{
					*(slash + 1) = '\0';
					path = modulePath;
					path += "_path_debug.txt";
				}
			}
			if( path.empty() ) path = "_path_debug.txt";
		}
		return path.c_str();
	}

	// Leeyes and the old Susie APIs remain MAX_PATH/ANSI based, but the real
	// filesystem operations can still use Windows extended-length paths. Keep
	// the \"\\?\\\" prefix internal so it never leaks into the UI or plugins.
	std::wstring ToExtendedPath( const std::wstring& path )
	{
		if( path.empty() || path.compare(0, 4, L"\\\\?\\") == 0 ) return path;
		if( path.size() < 248 ) return path;
		if( path.compare(0, 2, L"\\\\") == 0 )
			return L"\\\\?\\UNC\\" + path.substr(2);
		if( path.size() >= 3 && path[1] == L':'
			&& (path[2] == L'\\' || path[2] == L'/') )
			return L"\\\\?\\" + path;
		return path;
	}

	DWORD GetFileAttributesWLong( const std::wstring& path )
	{
		auto extended = ToExtendedPath(path);
		return ::GetFileAttributesW( extended.c_str() );
	}

	BOOL GetFileAttributesExWLong( const std::wstring& path, GET_FILEEX_INFO_LEVELS level, LPVOID info )
	{
		auto extended = ToExtendedPath(path);
		return ::GetFileAttributesExW( extended.c_str(), level, info );
	}

	HANDLE FindFirstFileWLong( const std::wstring& pattern, LPWIN32_FIND_DATAW findData )
	{
		auto extended = ToExtendedPath(pattern);
		return ::FindFirstFileW( extended.c_str(), findData );
	}

	HANDLE CreateFileWLong( const std::wstring& path, DWORD desiredAccess, DWORD shareMode,
		LPSECURITY_ATTRIBUTES security, DWORD creationDisposition, DWORD flags, HANDLE templateFile )
	{
		auto extended = ToExtendedPath(path);
		return ::CreateFileW( extended.c_str(), desiredAccess, shareMode, security,
			creationDisposition, flags, templateFile );
	}

	BOOL CreateHardLinkWLong( const std::wstring& linkPath, const std::wstring& existingPath )
	{
		auto extendedLink = ToExtendedPath(linkPath);
		auto extendedExisting = ToExtendedPath(existingPath);
		return ::CreateHardLinkW( extendedLink.c_str(), extendedExisting.c_str(), NULL );
	}

std::wstring GetShortPathWLong( const std::wstring& path )
	{
		auto extended = ToExtendedPath(path);
		std::vector<wchar_t> buffer(MAX_PATH, L'\0');
		DWORD length = ::GetShortPathNameW( extended.c_str(), buffer.data(), (DWORD)buffer.size() );
		if( length == 0 ) return std::wstring();
		if( length >= buffer.size() )
		{
			buffer.assign(length + 1, L'\0');
			length = ::GetShortPathNameW( extended.c_str(), buffer.data(), (DWORD)buffer.size() );
			if( length == 0 || length >= buffer.size() ) return std::wstring();
		}
		std::wstring result(buffer.data(), length);
		if( result.compare(0, 4, L"\\\\?\\") == 0 ) result.erase(0, 4);
		return result;
	}
	
std::wstring GetWidePath( std::string Path )
{
	// Some callers (including Susie/WIC paths) pass an extended-length prefix
	// through an ANSI API. It is an internal filesystem notation, not part of
	// the ANSI path that this resolver should decode.
	if( Path.compare(0, 8, "\\\\?\\UNC\\") == 0 )
		Path = "\\\\" + Path.substr(8);
	else if( Path.compare(0, 4, "\\\\?\\") == 0 )
		Path.erase(0, 4);

	std::wstring syntheticPath;
	if( LookupSyntheticPathAlias(Path, syntheticPath) ) return syntheticPath;

	if( Path.find('?') != std::string::npos )
	{
		std::lock_guard<std::mutex> lock(gWidePathCacheMutex);
		auto it = gWidePathCache.find( Path );
		if( it != gWidePathCache.end() ) return it->second;
	}

	std::wstring settled_path ;
	std::string tmp;
	wchar_t wide_file_name[MAX_PATH]={};
	bool unknown_code_found = false;

	tmp .reserve( Path.size() );
	settled_path .reserve( MAX_PATH );
	for( size_t pos = 0; pos < Path.size(); ++pos )
	{
		const char current = Path[pos];
		if(current == '?' ){ unknown_code_found = true; }
		if( isleadbyte( static_cast<BYTE>(current) ) )
		{
			tmp.push_back( current );
			// A malformed ANSI path can end with a lead byte. Do not advance
			// past end() and dereference it; preserve the unresolved byte as a
			// wildcard so the normal Unicode lookup can still try to recover it.
			if( pos + 1 >= Path.size() )
			{
				tmp.push_back( '?' );
				unknown_code_found = true;
			}
			else
			{
				tmp.push_back( Path[++pos] );
			}
			continue;
		}
		else
		{
			if( static_cast<signed char>(current) < 0 )
			{
				tmp.push_back( '?');
			}
			else
			{
				tmp.push_back( current );
			}
		}
		if( current == '\\' )
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
					HANDLE hFind = FindFirstFileWLong(indefinite_path, &FindData);
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
			HANDLE hFind = FindFirstFileWLong(indefinite_path, &FindData);
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
	if( Path.find('?') != std::string::npos && settled_path.find(L'?') == std::wstring::npos )
	{
		std::lock_guard<std::mutex> lock(gWidePathCacheMutex);
		gWidePathCache[Path] = settled_path;
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
	DWORD attrs = GetFileAttributesWLong( path );
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
	HANDLE hDir = CreateFileWLong( linkPath, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
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

// Returns a stable synthetic ANSI path for Path and remembers the mapping.
// Path must already be a fully resolved, existing wide path. No filesystem
// link is created here; the A-file hooks resolve this synthetic path back to
// the real Unicode path.
std::string GetOrCreateAsciiAlias( const std::wstring& Path, const std::string& parentAnsi = std::string() )
{
	DWORD attrs = GetFileAttributesWLong( Path );
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

	// Convert the short synthetic leaf by itself. Converting the complete real
	// path through a MAX_PATH-sized buffer fails precisely for the long names
	// this fallback is meant to repair.
	char aliasLeaf[MAX_PATH] = {};
	if( !WideCharToMultiByte( 932, 0, aliasName, -1, aliasLeaf, MAX_PATH, 0, 0 ) )
		return std::string();
	std::string leafAnsi = aliasLeaf;
	std::string aliasAnsi;
	if( !parentAnsi.empty() )
	{
		aliasAnsi = parentAnsi + "\\" + leafAnsi;
	}
	else
	{
		char mbcs_name[MAX_PATH] = {};
		WideCharToMultiByte( 932, 0, aliasPath.c_str(), -1, mbcs_name, MAX_PATH, 0, 0 );
		aliasAnsi = mbcs_name;
		if( aliasAnsi.empty() ) return std::string();
	}
	RememberSyntheticPathAlias( aliasAnsi, Path );
	return aliasAnsi;
}

bool WideToCP932( const std::wstring& path, std::string& result )
{
	char buffer[MAX_PATH * 4] = {};
	BOOL usedDefault = FALSE;
	int length = ::WideCharToMultiByte( 932, WC_NO_BEST_FIT_CHARS, path.c_str(), -1,
		buffer, sizeof(buffer), "?", &usedDefault );
	if( length <= 0 || usedDefault ) return false;
	result.assign( buffer, length - 1 );
	return true;
}

// CP932 conversion is lossy: different Unicode characters can produce the same
// byte spelling, and MultiByteToWideChar() then chooses only one of them.  Use
// the same best-fit conversion in the opposite direction only while comparing
// directory entries, so the actual Unicode entry can be recovered when it is
// the unique match (U+00B7/U+30FB is one example, not a special case).
bool WideToCP932BestFit( const std::wstring& value, std::string& result )
{
	int length = ::WideCharToMultiByte( 932, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr );
	if( length <= 0 ) return false;
	std::vector<char> buffer((size_t)length);
	if( ::WideCharToMultiByte( 932, 0, value.c_str(), -1, buffer.data(), length, nullptr, nullptr ) <= 0 )
		return false;
	result.assign( buffer.data(), (size_t)length - 1 );
	return true;
}

// Recover a final path component that was collapsed by an ANSI/CP932
// conversion.  This is deliberately not part of GetWidePath(): the normal
// file-list path must remain a single inexpensive conversion.  Archive
// plug-ins call this only after their original path lookup has failed.
bool ResolveAnsiPathCollision( const std::string& ansiPath, std::wstring& resolved )
{
	// Resolve every component, including parents of image files and search masks.
	// FindFirstFileW is deliberately used for existence checks: the W attribute
	// hooks can report success for a repaired spelling without returning that name.
	int length = ::MultiByteToWideChar(932, 0, ansiPath.c_str(), -1, nullptr, 0);
	if( length <= 0 ) return false;
	std::vector<wchar_t> buffer(length);
	::MultiByteToWideChar(932, 0, ansiPath.c_str(), -1, buffer.data(), length);
	std::wstring decoded(buffer.data());
	std::replace(decoded.begin(), decoded.end(), L'/', L'\\');
	if( decoded.size() < 3 || decoded[1] != L':' || decoded[2] != L'\\' ) return false;
	std::wstring current = decoded.substr(0, 3);
	size_t offset = 3;
	while( offset < decoded.size() )
	{
		size_t end = decoded.find(L'\\', offset);
		if( end == std::wstring::npos ) end = decoded.size();
		std::wstring leaf = decoded.substr(offset, end - offset);
		if( leaf.empty() ) { offset = end + 1; continue; }
		if( current.back() != L'\\' ) current += L'\\';
		if( leaf.find_first_of(L"*?") != std::wstring::npos )
		{
			if( end != decoded.size() ) return false;
			resolved = current + leaf;
			return true;
		}
		WIN32_FIND_DATAW data = {};
		HANDLE h = FindFirstFileWLong(current + leaf, &data);
		if( h != INVALID_HANDLE_VALUE )
		{
			::FindClose(h);
			current += data.cFileName;
		}
		else
		{
			std::string wanted;
			if( !WideToCP932BestFit(leaf, wanted) ) return false;
			h = FindFirstFileWLong(current + L"*", &data);
			if( h == INVALID_HANDLE_VALUE ) return false;
			int matches = 0;
			std::wstring match;
			do
			{
				std::string encoded;
				if( WideToCP932BestFit(data.cFileName, encoded) && encoded == wanted )
				{ match = data.cFileName; if( ++matches > 1 ) break; }
			} while( ::FindNextFileW(h, &data) );
			::FindClose(h);
			if( matches != 1 ) return false;
			current += match;
		}
		offset = end + 1;
	}
	resolved = current;
	return true;
}

// A caller can also pass the already-decoded Unicode spelling to a W API.
// For a lossy CP932 spelling, re-encode it and recover the unique real
// directory entry before giving up on the W call.
bool ResolveWidePathCollision( const std::wstring& widePath, std::wstring& resolved )
{
	std::string ansiPath;
	if( widePath.empty() || !WideToCP932BestFit(widePath, ansiPath) ) return false;
	if( !ResolveAnsiPathCollision(ansiPath, resolved) ) return false;
	return resolved != widePath;
}

bool GetAliasRoot( std::wstring& rootWide, std::string& rootAnsi )
{
	static std::mutex rootMutex;
	static std::wstring cachedRootWide;
	static std::string cachedRootAnsi;
	std::lock_guard<std::mutex> lock(rootMutex);
	if( !cachedRootWide.empty() )
	{
		rootWide = cachedRootWide;
		rootAnsi = cachedRootAnsi;
		return true;
	}

	wchar_t tempPath[MAX_PATH] = {};
	DWORD length = ::GetTempPathW( MAX_PATH, tempPath );
	if( length == 0 || length >= MAX_PATH ) return false;

	rootWide.assign( tempPath, length );
	if( !rootWide.empty() && rootWide.back() != L'\\' ) rootWide += L'\\';
	rootWide += L"LeeyesUnicodeHack";
	if( !WideToCP932( rootWide, rootAnsi ) ) return false;

	DWORD attrs = GetFileAttributesWLong( rootWide );
	if( attrs == INVALID_FILE_ATTRIBUTES )
	{
		if( !::CreateDirectoryW( rootWide.c_str(), NULL ) && ::GetLastError() != ERROR_ALREADY_EXISTS )
			return false;
	}
	cachedRootWide = rootWide;
	cachedRootAnsi = rootAnsi;
	return true;
}

std::string GetPrivateDirectoryAlias( const std::wstring& directory )
{
	{
		std::lock_guard<std::mutex> lock(gPrivateDirectoryAliasCacheMutex);
		auto it = gPrivateDirectoryAliasCache.find( directory );
		if( it != gPrivateDirectoryAliasCache.end() ) return it->second;
	}

	std::wstring rootWide;
	std::string rootAnsi;
	if( !GetAliasRoot( rootWide, rootAnsi ) ) return std::string();

	wchar_t aliasName[64] = {};
	swprintf_s( aliasName, L"~u%016llx", FnvHash( directory ) );
	std::wstring aliasPath = rootWide + L"\\" + aliasName;
	DWORD attrs = GetFileAttributesWLong( aliasPath );
	if( attrs == INVALID_FILE_ATTRIBUTES )
	{
		if( !CreateDirectoryJunction( aliasPath, directory ) ) return std::string();
	}
	else if( (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 || !DirectoryJunctionExists( aliasPath ) )
	{
		return std::string();
	}

	std::string result = rootAnsi + "\\" + std::string("~u") +
		[] (uint64_t value) {
			char hashText[32] = {};
			sprintf_s(hashText, "%016llx", value);
			return std::string(hashText);
		}(FnvHash(directory));
	{
		std::lock_guard<std::mutex> lock(gPrivateDirectoryAliasCacheMutex);
		gPrivateDirectoryAliasCache[directory] = result;
	}
	return result;
}

// Return a path that is completely representable in CP932. If 8.3 names are
// unavailable, use a temporary ASCII-only directory for the fallback aliases.
// Keeping these aliases outside the user's folders avoids modifying their
// directory trees just because an old ANSI plugin is being called.
std::string GetFullyAsciiAlias( const std::wstring& path )
{
	{
		std::lock_guard<std::mutex> lock(gFullyAsciiAliasCacheMutex);
		auto it = gFullyAsciiAliasCache.find( path );
		if( it != gFullyAsciiAliasCache.end() ) return it->second;
	}

	auto cacheResult = [&path]( const std::string& result ) {
		if( !result.empty() )
		{
			std::lock_guard<std::mutex> lock(gFullyAsciiAliasCacheMutex);
			gFullyAsciiAliasCache[path] = result;
		}
		return result;
	};

	DWORD attrs = GetFileAttributesWLong( path );
	if( attrs == INVALID_FILE_ATTRIBUTES ) return std::string();
	bool isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

	std::string direct;
	if( WideToCP932(path, direct) ) return cacheResult(direct);

	auto slashPos = path.find_last_of( L"\\/" );
	if( slashPos == std::wstring::npos ) return std::string();
	std::wstring parent = path.substr( 0, slashPos );
	std::wstring leaf = path.substr( slashPos + 1 );

	std::string leafAnsi;
	if( !isDirectory && WideToCP932(leaf, leafAnsi) )
	{
		auto parentAlias = GetPrivateDirectoryAlias( parent );
		if( !parentAlias.empty() ) return cacheResult(parentAlias + "\\" + leafAnsi);
	}

	// The common WebP/JPG case has an ASCII filename inside a Unicode folder.
	// Resolve the folder alias first; asking for an 8.3 name for every individual
	// image is much slower and provides no additional benefit in that case.
	auto shortPath = GetShortPathWLong( path );
	if( !shortPath.empty() && WideToCP932(shortPath, direct) )
		return cacheResult(direct);

	std::wstring rootWide;
	std::string rootAnsi;
	if( !GetAliasRoot( rootWide, rootAnsi ) ) return std::string();

	wchar_t aliasName[64] = {};
	std::wstring extension;
	auto dotPos = leaf.find_last_of( L'.' );
	if( !isDirectory && dotPos != std::wstring::npos ) extension = leaf.substr(dotPos);
	std::string extensionAnsi;
	WideToCP932( extension, extensionAnsi );
	std::wstring aliasExtension = extensionAnsi.empty() ? L"" : extension;
	swprintf_s( aliasName, L"~u%016llx%s", FnvHash(path), aliasExtension.c_str() );
	std::wstring aliasPath = rootWide + L"\\" + aliasName;
	std::string aliasAnsi = rootAnsi + "\\" + std::string("~u") +
		[] (uint64_t value) {
			char hashText[32] = {};
			sprintf_s(hashText, "%016llx", value);
			return std::string(hashText);
		}(FnvHash(path)) + extensionAnsi;
	if( GetFileAttributesWLong(aliasPath) == INVALID_FILE_ATTRIBUTES )
	{
		bool created = isDirectory
			? CreateDirectoryJunction( aliasPath, path )
			: (CreateHardLinkWLong( aliasPath, path ) != 0);
		if( !created )
		{
			// A hard link cannot cross volumes (the common case here is an image
			// or archive on E: and the process temp directory on C:). Keep the
			// alias virtual and let the A-file hooks resolve it back to Path.
			RememberSyntheticPathAlias( aliasAnsi, path );
			return cacheResult(aliasAnsi);
		}
	}

	return cacheResult(aliasAnsi);
}

std::string GetShortPath( std::wstring Path )
{
	return GetFullyAsciiAlias( Path );
}
std::string GetShortPath( std::string Path )
{
		return GetShortPath( GetWidePath( Path ) );
}

// Archive plug-ins are frequently older than the image plug-ins and some of
// them still mishandle otherwise valid multibyte names. Give those calls a
// process-private ASCII spelling and resolve it back to the real Unicode path
// in the file API hooks. This creates no link or directory beside the user's
// archive.
std::string GetArchivePathAlias( const std::wstring& path )
{
	if( path.empty() || GetFileAttributesWLong(path) == INVALID_FILE_ATTRIBUTES ) return std::string();
	std::wstring rootWide;
	std::string rootAnsi;
	if( !GetAliasRoot(rootWide, rootAnsi) ) return std::string();

	std::wstring leaf = path.substr(path.find_last_of(L"\\/") + 1);
	std::wstring extension;
	auto dotPos = leaf.find_last_of(L'.');
	if( dotPos != std::wstring::npos ) extension = leaf.substr(dotPos);
	std::string extensionAnsi;
	if( !extension.empty() && !WideToCP932(extension, extensionAnsi) ) extensionAnsi = ".bin";

	char hashText[32] = {};
	sprintf_s(hashText, "%016llx", FnvHash(path));
	std::string alias = rootAnsi + "\\~u" + hashText + extensionAnsi;
	RememberSyntheticPathAlias(alias, path);
	return alias;
}

}


namespace Kernel32
{
#if 1

	typedef int (WINAPI *WideCharToMultiByteFPtr)(UINT, DWORD, LPCWCH, int, LPSTR, int, LPCCH, LPBOOL);
	WideCharToMultiByteFPtr originalWideCharToMultiByte = nullptr;

	int WINAPI WideCharToMultiByteHook(UINT codePage, DWORD flags, LPCWCH input, int inputLength,
		LPSTR output, int outputLength, LPCCH defaultChar, LPBOOL usedDefault)
	{
		static thread_local bool inside = false;
		if( !originalWideCharToMultiByte ) return 0;
		if( inside )
			return originalWideCharToMultiByte(codePage, flags, input, inputLength, output, outputLength, defaultChar, usedDefault);
		inside = true;
		int ret = originalWideCharToMultiByte(codePage, flags, input, inputLength, output, outputLength, defaultChar, usedDefault);
		if( (codePage == 932 || (codePage == CP_ACP && ::GetACP() == 932))
			&& input && output && ret > 1 )
		{
			bool hasNonAscii = false;
			int wideLength = inputLength < 0 ? (int)wcslen(input) : inputLength;
			for( int i = 0; i < wideLength; ++i )
				if( input[i] > 0x7F ) { hasNonAscii = true; break; }
			size_t ansiLength = (size_t)ret - (inputLength < 0 ? 1u : 0u);
			if( ansiLength > 0 && ansiLength <= (size_t)outputLength
				&& hasNonAscii && memchr(output, '?', ansiLength) )
			{
				std::string ansi(output, ansiLength);
				std::wstring wide(input, (size_t)wideLength);
				Utility::RememberLossyDisplayName(ansi, wide);
			}
		}
		inside = false;
		return ret;
	}

	void hookPathConversions()
	{
		originalWideCharToMultiByte = nCodeHook.createHookByName("kernel32.dll", "WideCharToMultiByte", WideCharToMultiByteHook);
		if( !originalWideCharToMultiByte )
			originalWideCharToMultiByte = nCodeHook.createHookByName("kernelbase.dll", "WideCharToMultiByte", WideCharToMultiByteHook);
	}

struct SearchContext
{
	std::string parentDirAnsi;
	std::wstring parentDirWide;
	bool wideEnumeration = false;
};

struct SearchContextEntry
{
	HANDLE handle;
	SearchContext context;
	SearchContextEntry( HANDLE h, const SearchContext& value ) : handle(h), context(value) {}
};

// Normal FindNextFile calls are extremely frequent. Keep the exceptional
// Unicode-search handles in an atomic side table so the common path does not
// take a mutex or perform a tree lookup for every JPG/WebP item.
std::atomic<SearchContextEntry*> gSearchContextEntries[128] = {};
std::atomic<HANDLE> gLastSearchHandle = INVALID_HANDLE_VALUE;
std::atomic<SearchContextEntry*> gLastSearchEntry = nullptr;

void RememberSearchContext( HANDLE handle, const SearchContext& context )
{
	if( handle == INVALID_HANDLE_VALUE ) return;
	auto* entry = new SearchContextEntry( handle, context );
	for( auto& slot : gSearchContextEntries )
	{
		auto* existing = slot.load();
		if( existing && existing->handle == handle )
		{
			slot.store(entry);
			gLastSearchEntry.store(entry);
			gLastSearchHandle.store(handle);
			return;
		}
		SearchContextEntry* empty = nullptr;
		if( slot.compare_exchange_strong(empty, entry) )
		{
			gLastSearchEntry.store(entry);
			gLastSearchHandle.store(handle);
			return;
		}
	}
	delete entry;
}

SearchContextEntry* FindSearchContext( HANDLE handle )
{
	if( gLastSearchHandle.load() != handle ) return nullptr;
	auto* last = gLastSearchEntry.load();
	if( last && last->handle == handle ) return last;
	for( auto& slot : gSearchContextEntries )
	{
		auto* entry = slot.load();
		if( entry && entry->handle == handle ) return entry;
	}
	return nullptr;
}

void ForgetSearchContext( HANDLE handle )
{
	for( auto& slot : gSearchContextEntries )
	{
		auto* entry = slot.load();
		if( entry && entry->handle == handle ) slot.compare_exchange_strong(entry, nullptr);
	}
	if( gLastSearchHandle.load() == handle )
	{
		gLastSearchEntry.store(nullptr);
		gLastSearchHandle.store(INVALID_HANDLE_VALUE);
	}
}

bool HasUnknownChar( LPCSTR text )
{
	if( !text ) return false;
	return strchr( text, '?' ) != nullptr;
}

bool HasPathLengthIssue( const std::string& parentDirAnsi, LPCSTR leafName )
{
	if( parentDirAnsi.empty() || !leafName ) return false;
	return parentDirAnsi.size() + 1 + strlen(leafName) >= MAX_PATH - 1;
}

#if UNICODEHACK_PATH_DEBUG
void LogFindResult( const char* api, LPCSTR pattern, LPCSTR name )
{
	if( !name || !strstr(name, "neekosan") ) return;
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") != 0 || !lf ) return;
	fprintf(lf, "%s pattern=[%s] name=[%s] bytes=", api,
		pattern ? pattern : "<continued>", name);
	for( const unsigned char* p = reinterpret_cast<const unsigned char*>(name); *p; ++p )
		fprintf(lf, "%02X", *p);
	fprintf(lf, "\n");
	fclose(lf);
}
#endif

void ResolveSearchParentDir( SearchContext& context )
{
	if( context.parentDirWide.empty() && !context.parentDirAnsi.empty() )
	{
		context.parentDirWide = Utility::GetWidePath( context.parentDirAnsi );
	}
}

// Resolve only the directory part through the Unicode path resolver and convert
// the final ANSI search mask separately. GetWidePath() treats '?' as a missing
// character, while '?' in a FindFirstFile mask is a real wildcard.
bool MakeWideSearchPattern( LPCSTR searchPattern, std::wstring& widePattern,
	std::wstring& parentDirWide )
{
	if( !searchPattern || (!HasUnknownChar(searchPattern)
		&& !Utility::HasSyntheticAlias(searchPattern)) ) return false;
	std::string ansiPattern( searchPattern );
	std::wstring syntheticRealPath;
	if( Utility::LookupSyntheticPathAlias(ansiPattern, syntheticRealPath) )
	{
		widePattern = syntheticRealPath;
		auto slashPos = syntheticRealPath.find_last_of(L"\\/");
		parentDirWide = slashPos == std::wstring::npos ? L"" : syntheticRealPath.substr(0, slashPos);
		return !widePattern.empty();
	}
	auto slashPos = ansiPattern.find_last_of( "\\/" );
	if( slashPos == std::string::npos ) return false;

	std::string parentAnsi = ansiPattern.substr( 0, slashPos );
	std::string maskAnsi = ansiPattern.substr( slashPos + 1 );
	parentDirWide = Utility::GetWidePath( parentAnsi );
	if( parentDirWide.empty() || parentDirWide.find(L'?') != std::wstring::npos ) return false;

	wchar_t wideMask[MAX_PATH] = {};
	if( !::MultiByteToWideChar( 932, 0, maskAnsi.c_str(), -1, wideMask, MAX_PATH ) ) return false;
	widePattern = parentDirWide;
	if( widePattern.empty() || widePattern.back() != L'\\' ) widePattern += L'\\';
	widePattern += wideMask;
	return true;
}

bool CopyFindDataWToA( const WIN32_FIND_DATAW& source, const std::wstring& parentDirWide,
	const std::string& parentDirAnsi,
	LPWIN32_FIND_DATA destination )
{
	if( !destination ) return false;
	destination->dwFileAttributes = source.dwFileAttributes;
	destination->ftCreationTime = source.ftCreationTime;
	destination->ftLastAccessTime = source.ftLastAccessTime;
	destination->ftLastWriteTime = source.ftLastWriteTime;
	destination->nFileSizeHigh = source.nFileSizeHigh;
	destination->nFileSizeLow = source.nFileSizeLow;
	destination->dwReserved0 = source.dwReserved0;
	destination->dwReserved1 = source.dwReserved1;
	destination->cFileName[0] = '\0';
	destination->cAlternateFileName[0] = '\0';

	char fileName[MAX_PATH] = {};
	char alternateName[14] = {};
	::WideCharToMultiByte( 932, 0, source.cAlternateFileName, -1,
		alternateName, sizeof(alternateName), 0, 0 );
	::WideCharToMultiByte( 932, 0, source.cFileName, -1,
		fileName, sizeof(fileName), 0, 0 );

	// '?' is not legal in a Windows filename, so one in the CP932 result means
	// that the name was not representable. Also replace a representable leaf
	// when the complete ANSI path would exceed Leeyes' MAX_PATH-sized buffers.
	// The latter is what makes long ASCII filenames work inside a Unicode folder.
	bool needsAlias = strchr(fileName, '?') != nullptr
		|| HasPathLengthIssue(parentDirAnsi, fileName);
	if( needsAlias && !parentDirWide.empty() )
	{
		if( alternateName[0] && !strchr(fileName, '?') )
		{
			strncpy_s( fileName, sizeof(fileName), alternateName, _TRUNCATE );
		}
		else
		{
			std::wstring fullWide = parentDirWide + L"\\" + source.cFileName;
			auto alias = Utility::GetOrCreateAsciiAlias( fullWide, parentDirAnsi );
			if( !alias.empty() )
			{
				auto slashPos = alias.find_last_of( '\\' );
				auto leafOnly = slashPos == std::string::npos ? alias : alias.substr( slashPos + 1 );
				strncpy_s( fileName, sizeof(fileName), leafOnly.c_str(), _TRUNCATE );
			}
		}
	}

	strncpy_s( destination->cFileName, sizeof(destination->cFileName), fileName, _TRUNCATE );
	strncpy_s( destination->cAlternateFileName, sizeof(destination->cAlternateFileName), alternateName, _TRUNCATE );
	return destination->cFileName[0] != '\0';
}

// cFileName may come back from the OS with '?' standing in for characters that don't fit
// CP932. Prefer the 8.3 alternate name when one exists; when it doesn't (8.3 name generation
// disabled on this volume, or none applicable), fall back to a junction/hard-link alias -
// otherwise callers like Leeyes' own directory-tree code end up processing a name that is
// mostly '?' placeholders, which is what actually crashes it on some real-world folder names.
void FixupFindData( const std::wstring& parentDirWide, const std::string& parentDirAnsi,
	LPWIN32_FIND_DATA lpFindFileData )
{
	bool hasUnknown = false;
	for( int pos = 0; lpFindFileData->cFileName[pos]; ++pos )
	{
		if( lpFindFileData->cFileName[pos] == '?' ) { hasUnknown = true; break; }
	}
	bool pathTooLong = HasPathLengthIssue(parentDirAnsi, lpFindFileData->cFileName);
	if( !hasUnknown && !pathTooLong ) return;

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
	HANDLE hFind = Utility::FindFirstFileWLong( fullPattern, &wideFindData );
	if( hFind == INVALID_HANDLE_VALUE ) return;
	::FindClose( hFind );

	std::wstring fullWide = parentDirWide + L"\\" + wideFindData.cFileName;
	auto alias = Utility::GetOrCreateAsciiAlias( fullWide, parentDirAnsi );
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
	// A best-fit CP932 spelling can contain no '?' yet name a different Unicode
	// entry (U+00B7 becomes U+30FB). Delphi also uses FindFirstFile for file
	// existence/metadata checks before invoking any image/archive plugin.
	if( ret == INVALID_HANDLE_VALUE && lpFileName && lpFindFileData
		&& !Utility::HasSyntheticAlias(lpFileName) )
	{
		std::wstring realPath;
		if( Utility::ResolveAnsiPathCollision(lpFileName, realPath) )
		{
			WIN32_FIND_DATAW data = {};
			ret = Utility::FindFirstFileWLong(realPath, &data);
			if( ret != INVALID_HANDLE_VALUE )
			{
				std::string ansi(lpFileName);
				auto slash = ansi.find_last_of("\\/");
				CopyFindDataWToA(data, realPath.substr(0, realPath.find_last_of(L"\\/")),
					ansi.substr(0, slash), lpFindFileData);
				SearchContext context;
				context.parentDirAnsi = ansi.substr(0, slash);
				context.parentDirWide = realPath.substr(0, realPath.find_last_of(L"\\/"));
				context.wideEnumeration = true;
				RememberSearchContext(ret, context);
				return ret;
			}
		}
	}
	if( ret != INVALID_HANDLE_VALUE ) ForgetSearchContext( ret );
	bool pathHasUnknown = HasUnknownChar( lpFileName );
	bool pathHasSynthetic = lpFileName && Utility::HasSyntheticAlias( lpFileName );
	bool pathNeedsUnicode = pathHasUnknown || pathHasSynthetic;
	std::string searchPattern;
	if( pathNeedsUnicode ) searchPattern = lpFileName;

	// FindFirstFileA fails before returning a handle when the directory itself
	// contains a CP932-unrepresentable character. Retry the same search through
	// the Unicode API, then let FindNextFileHook continue that W enumeration.
	if( ret == INVALID_HANDLE_VALUE && pathNeedsUnicode )
	{
		std::wstring widePattern;
		std::wstring parentDirWide;
		if( MakeWideSearchPattern( searchPattern.c_str(), widePattern, parentDirWide ) )
		{
			WIN32_FIND_DATAW wideFindData = {};
			ret = Utility::FindFirstFileWLong( widePattern, &wideFindData );
			if( ret != INVALID_HANDLE_VALUE )
			{
				std::string parentDirAnsi = searchPattern.substr( 0, searchPattern.find_last_of("\\/") );
				CopyFindDataWToA( wideFindData, parentDirWide, parentDirAnsi, lpFindFileData );
				SearchContext context;
				context.parentDirAnsi = parentDirAnsi;
				context.parentDirWide = parentDirWide;
				context.wideEnumeration = true;
				RememberSearchContext( ret, context );
			}
		}
	}

	if( ret != INVALID_HANDLE_VALUE && lpFindFileData )
	{
		bool resultHasUnknown = HasUnknownChar( lpFindFileData->cFileName );
		std::string resultParentAnsi;
		auto resultSlashPos = searchPattern.find_last_of( '\\' );
		if( resultSlashPos != std::string::npos )
			resultParentAnsi = searchPattern.substr( 0, resultSlashPos );
		bool resultPathTooLong = HasPathLengthIssue(resultParentAnsi, lpFindFileData->cFileName);
		// The common ASCII/JPG case stays on the original fast path: no map entry,
		// mutex, or path conversion is created for a normal directory scan.
		if( pathNeedsUnicode || resultHasUnknown || resultPathTooLong )
		{
			if( searchPattern.empty() ) searchPattern = lpFileName ? lpFileName : "";
			SearchContext context;
			auto slashPos = searchPattern.find_last_of( '\\' );
			if( slashPos != std::string::npos )
				context.parentDirAnsi = searchPattern.substr( 0, slashPos );
			if( context.parentDirWide.empty() )
				ResolveSearchParentDir( context );
			auto existing = FindSearchContext( ret );
			if( existing ) context = existing->context;
			else RememberSearchContext( ret, context );
			if( !context.wideEnumeration && !context.parentDirWide.empty() )
				FixupFindData( context.parentDirWide, context.parentDirAnsi, lpFindFileData );
		}
		#if UNICODEHACK_PATH_DEBUG
		LogFindResult("FindFirstFileA", lpFileName, lpFindFileData->cFileName);
		#endif
	}
	return ret;
}
void hookFindFirstFile()
{
	originalFindFirstFile = nCodeHook.createHookByName("kernelbase.dll", "FindFirstFileA", FindFirstFileHook);
	if( !originalFindFirstFile )
	{
		originalFindFirstFile = nCodeHook.createHookByName("kernel32.dll", "FindFirstFileA", FindFirstFileHook);
	}
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookFindFirstFile install %s\n", originalFindFirstFile ? "OK" : "FAILED");
		fclose(lf);
	}
}


typedef BOOL  (WINAPI *FindNextFileFPtr)( HANDLE hFindFile,    LPWIN32_FIND_DATA lpFindFileData   );
FindNextFileFPtr originalFindNextFile = nullptr;


BOOL  WINAPI FindNextFileHook(HANDLE hFindFile,       LPWIN32_FIND_DATA lpFindFileData   )
{
	SearchContextEntry* contextEntry = FindSearchContext( hFindFile );
	bool hasContext = contextEntry != nullptr;
	SearchContext context;
	if( contextEntry ) context = contextEntry->context;

	WIN32_FIND_DATAW wideFindData = {};
	BOOL ret = (hasContext && context.wideEnumeration)
		? ::FindNextFileW( hFindFile, &wideFindData )
		: originalFindNextFile(hFindFile,lpFindFileData);
	if( ret && hasContext && context.wideEnumeration )
	{
		CopyFindDataWToA( wideFindData, context.parentDirWide, context.parentDirAnsi, lpFindFileData );
	}
	else if( ret && hasContext && (HasUnknownChar( lpFindFileData->cFileName )
		|| HasPathLengthIssue(context.parentDirAnsi, lpFindFileData->cFileName)) )
	{
		FixupFindData( context.parentDirWide, context.parentDirAnsi, lpFindFileData );
	}
	#if UNICODEHACK_PATH_DEBUG
	if( ret ) LogFindResult("FindNextFileA", nullptr, lpFindFileData->cFileName);
	#endif
	return ret;
}
void hookFindNextFile()
{
	originalFindNextFile = nCodeHook.createHookByName("kernelbase.dll", "FindNextFileA", FindNextFileHook);
	if( !originalFindNextFile )
	{
		originalFindNextFile = nCodeHook.createHookByName("kernel32.dll", "FindNextFileA", FindNextFileHook);
	}
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookFindNextFile install %s\n", originalFindNextFile ? "OK" : "FAILED");
		fclose(lf);
	}
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

bool IsExtendedAnsiPath( LPCSTR path )
{
	return path && strncmp(path, "\\\\?\\", 4) == 0;
}

LPCSTR StripExtendedAnsiPrefix( LPCSTR path )
{
	return IsExtendedAnsiPath(path) ? path + 4 : path;
}

HANDLE  WINAPI CreateFileAHook(    __in     LPCSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
)
{
	LPCSTR pathForWideResolution = StripExtendedAnsiPrefix(lpFileName);
	bool hasExtendedPrefix = IsExtendedAnsiPath(lpFileName);
	bool hasUnknown = pathForWideResolution && strchr( pathForWideResolution, '?' ) != nullptr;
	bool hasSyntheticAlias = pathForWideResolution && Utility::HasSyntheticAlias(pathForWideResolution);
	std::wstring resolvedWide;
	bool wideAttempted = false;
	HANDLE ret = INVALID_HANDLE_VALUE;
	if( hasUnknown || hasExtendedPrefix || hasSyntheticAlias )
	{
		resolvedWide = Utility::GetWidePath( pathForWideResolution );
		if( !resolvedWide.empty() && resolvedWide.find(L'?') == std::wstring::npos )
		{
			ret = Utility::CreateFileWLong( resolvedWide, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
				dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile );
			wideAttempted = true;
		}
	}
	if( !wideAttempted )
	{
		ret = originalCreateFileA(lpFileName,dwDesiredAccess, dwShareMode,lpSecurityAttributes,dwCreationDisposition,dwFlagsAndAttributes  ,hTemplateFile   );
	}
#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			fprintf(lf, "CreateFileA in=[%s] ret=%s\n", lpFileName ? lpFileName : "<null>", (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
#endif
	if( ret == INVALID_HANDLE_VALUE && !wideAttempted )
	{
		if( resolvedWide.empty() ) resolvedWide = Utility::GetWidePath( pathForWideResolution );
		if( resolvedWide.empty() || Utility::GetFileAttributesWLong(resolvedWide) == INVALID_FILE_ATTRIBUTES )
		{
			std::wstring collisionPath;
			if( Utility::ResolveAnsiPathCollision(pathForWideResolution, collisionPath) )
				resolvedWide = collisionPath;
		}
	#if UNICODEHACK_PATH_DEBUG
		{
			FILE* lf = nullptr;
			if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
			{
				fprintf(lf, "ansi_in=[%s]\n", lpFileName);
				char wideAsAnsiForLog[MAX_PATH*2] = {};
				WideCharToMultiByte(CP_UTF8, 0, resolvedWide.c_str(), -1, wideAsAnsiForLog, sizeof(wideAsAnsiForLog), 0, 0);
				fprintf(lf, "resolved_wide(utf8)=[%s]\n", wideAsAnsiForLog);
				fclose(lf);
			}
		}
	#endif
		ret = Utility::CreateFileWLong(resolvedWide, dwDesiredAccess, dwShareMode, lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
		DWORD lastErr2 = ::GetLastError();
	#if UNICODEHACK_PATH_DEBUG
		{
			FILE* lf = nullptr;
			if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
			{
				fprintf(lf, "CreateFileW result: %s (lastError=%lu)\n", (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL", lastErr2);
				fclose(lf);
			}
		}
	#endif
		::SetLastError(lastErr2);
	}
	return ret;
}
void hookCreateFileA()
{
	originalCreateFileA = nCodeHook.createHookByName("kernelbase.dll", "CreateFileA", CreateFileAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
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

bool TryResolveSyntheticWidePath(LPCWSTR path, std::wstring& resolved)
{
	if( !path || !wcsstr(path, L"\\~u") ) return false;
	std::string ansiPath;
	if( !Utility::WideToCP932(path, ansiPath) ) return false;
	if( ansiPath.compare(0, 8, "\\\\?\\UNC\\") == 0 )
		ansiPath = "\\\\" + ansiPath.substr(8);
	else if( ansiPath.compare(0, 4, "\\\\?\\") == 0 )
		ansiPath.erase(0, 4);
	return Utility::LookupSyntheticPathAlias(ansiPath, resolved);
}

HANDLE  WINAPI CreateFileWHook(    __in     LPCWSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
)
{
	std::wstring resolved;
	HANDLE ret = TryResolveSyntheticWidePath(lpFileName, resolved)
		? Utility::CreateFileWLong(resolved, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
			dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
		: originalCreateFileW(lpFileName,dwDesiredAccess, dwShareMode,lpSecurityAttributes,dwCreationDisposition,dwFlagsAndAttributes  ,hTemplateFile   );
	if( ret == INVALID_HANDLE_VALUE && lpFileName )
	{
		std::wstring collisionPath;
		if( Utility::ResolveWidePathCollision(lpFileName, collisionPath) )
			ret = Utility::CreateFileWLong(collisionPath, dwDesiredAccess, dwShareMode,
				lpSecurityAttributes, dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
	}
#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			char utf8[MAX_PATH*2] = {};
			WideCharToMultiByte(CP_UTF8, 0, lpFileName ? lpFileName : L"<null>", -1, utf8, sizeof(utf8), 0, 0);
			fprintf(lf, "CreateFileW in(utf8)=[%s] ret=%s\n", utf8, (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
#endif
	return ret;
}
void hookCreateFileW()
{
	originalCreateFileW = nCodeHook.createHookByName("kernelbase.dll", "CreateFileW", CreateFileWHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookCreateFileW install %s\n", originalCreateFileW ? "OK" : "FAILED");
		fclose(lf);
	}
}

typedef DWORD (WINAPI *GetFileAttributesWFPtr)( LPCWSTR lpFileName );
GetFileAttributesWFPtr originalGetFileAttributesW = nullptr;
DWORD WINAPI GetFileAttributesWHook( LPCWSTR lpFileName )
{
	std::wstring resolved;
	DWORD ret = TryResolveSyntheticWidePath(lpFileName, resolved)
		? Utility::GetFileAttributesWLong(resolved)
		: originalGetFileAttributesW(lpFileName);
	if( ret == INVALID_FILE_ATTRIBUTES && lpFileName
		&& Utility::ResolveWidePathCollision(lpFileName, resolved) )
		ret = Utility::GetFileAttributesWLong(resolved);
	return ret;
}
void hookGetFileAttributesW()
{
	originalGetFileAttributesW = nCodeHook.createHookByName("kernelbase.dll", "GetFileAttributesW", GetFileAttributesWHook);
}

typedef BOOL (WINAPI *GetFileAttributesExWFPtr)( LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation );
GetFileAttributesExWFPtr originalGetFileAttributesExW = nullptr;
BOOL WINAPI GetFileAttributesExWHook( LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation )
{
	std::wstring resolved;
	BOOL ret = TryResolveSyntheticWidePath(lpFileName, resolved)
		? Utility::GetFileAttributesExWLong(resolved, fInfoLevelId, lpFileInformation)
		: originalGetFileAttributesExW(lpFileName, fInfoLevelId, lpFileInformation);
	if( !ret && lpFileName
		&& Utility::ResolveWidePathCollision(lpFileName, resolved) )
		ret = Utility::GetFileAttributesExWLong(resolved, fInfoLevelId, lpFileInformation);
	return ret;
}
void hookGetFileAttributesExW()
{
	originalGetFileAttributesExW = nCodeHook.createHookByName("kernelbase.dll", "GetFileAttributesExW", GetFileAttributesExWHook);
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
	std::wstring resolved;
	HANDLE ret = TryResolveSyntheticWidePath(lpFileName, resolved)
		? Utility::CreateFileWLong(resolved, dwDesiredAccess, dwShareMode, nullptr,
			dwCreationDisposition, 0, nullptr)
		: originalCreateFile2(lpFileName, dwDesiredAccess, dwShareMode, dwCreationDisposition, pCreateExParams);
	if( ret == INVALID_HANDLE_VALUE && lpFileName
		&& Utility::ResolveWidePathCollision(lpFileName, resolved) )
		ret = Utility::CreateFileWLong(resolved, dwDesiredAccess, dwShareMode, nullptr,
			dwCreationDisposition, 0, nullptr);
#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			char utf8[MAX_PATH*2] = {};
			WideCharToMultiByte(CP_UTF8, 0, lpFileName ? lpFileName : L"<null>", -1, utf8, sizeof(utf8), 0, 0);
			fprintf(lf, "CreateFile2 in(utf8)=[%s] ret=%s\n", utf8, (ret != INVALID_HANDLE_VALUE) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
#endif
	return ret;
}
void hookCreateFile2()
{
	originalCreateFile2 = nCodeHook.createHookByName("kernelbase.dll", "CreateFile2", CreateFile2Hook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
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
	LPCSTR pathForWideResolution = StripExtendedAnsiPrefix(lpFileName);
	bool hasExtendedPrefix = IsExtendedAnsiPath(lpFileName);
	bool hasUnknown = pathForWideResolution && strchr( pathForWideResolution, '?' ) != nullptr;
	bool hasSyntheticAlias = pathForWideResolution && Utility::HasSyntheticAlias(pathForWideResolution);
	std::wstring resolvedWide;
	bool wideAttempted = false;
	DWORD ret = INVALID_FILE_ATTRIBUTES;
	if( hasUnknown || hasExtendedPrefix || hasSyntheticAlias )
	{
		resolvedWide = Utility::GetWidePath( pathForWideResolution );
		if( !resolvedWide.empty() && resolvedWide.find(L'?') == std::wstring::npos )
		{
			ret = Utility::GetFileAttributesWLong( resolvedWide );
			wideAttempted = true;
		}
	}
	if( !wideAttempted ) ret = originalGetFileAttributesA( lpFileName );
#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			fprintf(lf, "GetFileAttributesA in=[%s] ret=%s\n", lpFileName ? lpFileName : "<null>", (ret != INVALID_FILE_ATTRIBUTES) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
#endif
	if( ret == INVALID_FILE_ATTRIBUTES && !wideAttempted )
	{
		if( resolvedWide.empty() ) resolvedWide = Utility::GetWidePath( pathForWideResolution );
		if( resolvedWide.empty() || Utility::GetFileAttributesWLong(resolvedWide) == INVALID_FILE_ATTRIBUTES )
		{
			std::wstring collisionPath;
			if( Utility::ResolveAnsiPathCollision(pathForWideResolution, collisionPath) )
				resolvedWide = collisionPath;
		}
		ret = Utility::GetFileAttributesWLong( resolvedWide );
#if UNICODEHACK_PATH_DEBUG
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			char wideAsAnsiForLog[MAX_PATH*2] = {};
			WideCharToMultiByte(CP_UTF8, 0, resolvedWide.c_str(), -1, wideAsAnsiForLog, sizeof(wideAsAnsiForLog), 0, 0);
			fprintf(lf, "GetFileAttributesA ansi_in=[%s] resolved_wide(utf8)=[%s] result=%s\n",
				lpFileName, wideAsAnsiForLog, (ret != INVALID_FILE_ATTRIBUTES) ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
#endif
	}
	return ret;
}
void hookGetFileAttributesA()
{
	originalGetFileAttributesA = nCodeHook.createHookByName("kernelbase.dll", "GetFileAttributesA", GetFileAttributesAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookGetFileAttributesA install %s\n", originalGetFileAttributesA ? "OK" : "FAILED");
		fclose(lf);
	}
}

typedef BOOL (WINAPI *GetFileAttributesExAFPtr)( LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation );
GetFileAttributesExAFPtr originalGetFileAttributesExA = nullptr;
BOOL WINAPI GetFileAttributesExAHook( LPCSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation )
{
	LPCSTR pathForWideResolution = StripExtendedAnsiPrefix(lpFileName);
	bool hasExtendedPrefix = IsExtendedAnsiPath(lpFileName);
	bool hasUnknown = pathForWideResolution && strchr( pathForWideResolution, '?' ) != nullptr;
	bool hasSyntheticAlias = pathForWideResolution && Utility::HasSyntheticAlias(pathForWideResolution);
	std::wstring resolvedWide;
	bool wideAttempted = false;
	BOOL ret = FALSE;
	if( hasUnknown || hasExtendedPrefix || hasSyntheticAlias )
	{
		resolvedWide = Utility::GetWidePath( pathForWideResolution );
		if( !resolvedWide.empty() && resolvedWide.find(L'?') == std::wstring::npos )
		{
			ret = Utility::GetFileAttributesExWLong( resolvedWide, fInfoLevelId, lpFileInformation );
			wideAttempted = true;
		}
	}
	if( !wideAttempted ) ret = originalGetFileAttributesExA( lpFileName, fInfoLevelId, lpFileInformation );
#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			fprintf(lf, "GetFileAttributesExA in=[%s] ret=%s\n", lpFileName ? lpFileName : "<null>", ret ? "SUCCESS" : "FAIL");
			fclose(lf);
		}
	}
#endif
	if( !ret && !wideAttempted )
	{
		if( resolvedWide.empty() ) resolvedWide = Utility::GetWidePath( pathForWideResolution );
		if( resolvedWide.empty() || Utility::GetFileAttributesWLong(resolvedWide) == INVALID_FILE_ATTRIBUTES )
		{
			std::wstring collisionPath;
			if( Utility::ResolveAnsiPathCollision(pathForWideResolution, collisionPath) )
				resolvedWide = collisionPath;
		}
		ret = Utility::GetFileAttributesExWLong( resolvedWide, fInfoLevelId, lpFileInformation );
	}
	return ret;
}
void hookGetFileAttributesExA()
{
	originalGetFileAttributesExA = nCodeHook.createHookByName("kernelbase.dll", "GetFileAttributesExA", GetFileAttributesExAHook);
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookGetFileAttributesExA install %s\n", originalGetFileAttributesExA ? "OK" : "FAILED");
		fclose(lf);
	}
}

	// Leeyes keeps file-list entries in ANSI form.  Entries that cannot be
	// represented in CP932 are given a process-local ASCII alias by the
	// enumeration hooks, so file operations must resolve that alias too.
	bool ResolveAnsiOperationPath( LPCSTR input, bool mustExist, std::wstring& wide )
	{
		if( !input || !*input ) return false;
		std::string ansi(input);
		if( Utility::LookupSyntheticPathAlias(ansi, wide) ) return true;
		bool hasLossyName = Utility::LookupLossyDisplayName(input, -1, wide);
		if( hasLossyName
			&& wide.find(L'?') == std::wstring::npos
			&& (!mustExist || Utility::GetFileAttributesWLong(wide) != INVALID_FILE_ATTRIBUTES) )
			return true;
		if( !hasLossyName && !Utility::HasSyntheticAlias(ansi) && ansi.find('?') == std::string::npos )
		{
			wide = Utility::GetWidePath(ansi);
			return false;
		}

		wide = Utility::GetWidePath(ansi);
		if( wide.empty() || wide.find(L'?') != std::wstring::npos )
		{
			std::wstring collision;
			if( Utility::ResolveAnsiPathCollision(ansi, collision) ) wide.swap(collision);
		}
		return !wide.empty() && wide.find(L'?') == std::wstring::npos
			&& (!mustExist || Utility::GetFileAttributesWLong(wide) != INVALID_FILE_ATTRIBUTES);
	}

	typedef BOOL (WINAPI *MoveFileAFPtr)(LPCSTR, LPCSTR);
	MoveFileAFPtr originalMoveFileA = nullptr;
	typedef BOOL (WINAPI *MoveFileExAFPtr)(LPCSTR, LPCSTR, DWORD);
	MoveFileExAFPtr originalMoveFileExA = nullptr;
	typedef BOOL (WINAPI *DeleteFileAFPtr)(LPCSTR);
	DeleteFileAFPtr originalDeleteFileA = nullptr;
	typedef BOOL (WINAPI *RemoveDirectoryAFPtr)(LPCSTR);
	RemoveDirectoryAFPtr originalRemoveDirectoryA = nullptr;

	BOOL WINAPI MoveFileAHook(LPCSTR existingName, LPCSTR newName)
	{
		std::wstring existingWide, newWide;
		bool sourceResolved = ResolveAnsiOperationPath(existingName, true, existingWide);
		bool destinationResolved = ResolveAnsiOperationPath(newName, false, newWide);
		if( sourceResolved || destinationResolved )
		{
			if( existingWide.empty() ) existingWide = Utility::GetWidePath(existingName ? existingName : "");
			if( newWide.empty() ) newWide = Utility::GetWidePath(newName ? newName : "");
			if( existingWide.empty() || newWide.empty()
				|| existingWide.find(L'?') != std::wstring::npos
				|| newWide.find(L'?') != std::wstring::npos ) return FALSE;
			return ::MoveFileW(existingWide.c_str(), newWide.c_str());
		}
		return originalMoveFileA ? originalMoveFileA(existingName, newName) : FALSE;
	}

	BOOL WINAPI MoveFileExAHook(LPCSTR existingName, LPCSTR newName, DWORD flags)
	{
		std::wstring existingWide, newWide;
		bool sourceResolved = ResolveAnsiOperationPath(existingName, true, existingWide);
		bool destinationResolved = ResolveAnsiOperationPath(newName, false, newWide);
		if( sourceResolved || destinationResolved )
		{
			if( existingWide.empty() ) existingWide = Utility::GetWidePath(existingName ? existingName : "");
			if( newWide.empty() ) newWide = Utility::GetWidePath(newName ? newName : "");
			if( existingWide.empty() || newWide.empty()
				|| existingWide.find(L'?') != std::wstring::npos
				|| newWide.find(L'?') != std::wstring::npos ) return FALSE;
			return ::MoveFileExW(existingWide.c_str(), newWide.c_str(), flags);
		}
		return originalMoveFileExA ? originalMoveFileExA(existingName, newName, flags) : FALSE;
	}

	BOOL WINAPI DeleteFileAHook(LPCSTR fileName)
	{
		std::wstring wide;
		if( ResolveAnsiOperationPath(fileName, true, wide) )
			return ::DeleteFileW(wide.c_str());
		return originalDeleteFileA ? originalDeleteFileA(fileName) : FALSE;
	}

	BOOL WINAPI RemoveDirectoryAHook(LPCSTR path)
	{
		std::wstring wide;
		if( ResolveAnsiOperationPath(path, true, wide) )
			return ::RemoveDirectoryW(wide.c_str());
		return originalRemoveDirectoryA ? originalRemoveDirectoryA(path) : FALSE;
	}

	void hookFileOperations()
	{
		originalMoveFileA = nCodeHook.createHookByName("kernelbase.dll", "MoveFileA", MoveFileAHook);
		if( !originalMoveFileA )
			originalMoveFileA = nCodeHook.createHookByName("kernel32.dll", "MoveFileA", MoveFileAHook);
		originalMoveFileExA = nCodeHook.createHookByName("kernelbase.dll", "MoveFileExA", MoveFileExAHook);
		if( !originalMoveFileExA )
			originalMoveFileExA = nCodeHook.createHookByName("kernel32.dll", "MoveFileExA", MoveFileExAHook);
		originalDeleteFileA = nCodeHook.createHookByName("kernelbase.dll", "DeleteFileA", DeleteFileAHook);
		if( !originalDeleteFileA )
			originalDeleteFileA = nCodeHook.createHookByName("kernel32.dll", "DeleteFileA", DeleteFileAHook);
		originalRemoveDirectoryA = nCodeHook.createHookByName("kernelbase.dll", "RemoveDirectoryA", RemoveDirectoryAHook);
		if( !originalRemoveDirectoryA )
			originalRemoveDirectoryA = nCodeHook.createHookByName("kernel32.dll", "RemoveDirectoryA", RemoveDirectoryAHook);
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
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
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
	typedef BOOL (WINAPI *SetWindowTextWFPtr)( HWND hWnd, LPCWSTR lpString );
	SetWindowTextWFPtr originalSetWindowTextW = nullptr;
	typedef LRESULT (WINAPI *SendMessageAFPtr)( HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam );
	SendMessageAFPtr originalSendMessageA = nullptr;
	typedef LRESULT (WINAPI *SendMessageWFPtr)( HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam );
	SendMessageWFPtr originalSendMessageW = nullptr;
	bool IsTextMessage( UINT message );

	int (WINAPI *originalDrawTextA)(HDC, LPCSTR, int, LPRECT, UINT) = nullptr;
	BOOL (WINAPI *originalExtTextOutA)(HDC, int, int, UINT, const RECT*, LPCSTR, UINT, const INT*) = nullptr;
	BOOL (WINAPI *originalTextOutA)(HDC, int, int, LPCSTR, int) = nullptr;

	int WINAPI DrawTextAHook(HDC dc, LPCSTR text, int length, LPRECT rect, UINT format)
	{
		std::wstring wide;
		bool mapped = text && Utility::LookupLossyDisplayName(text, length, wide);
		if( mapped )
		{
			return ::DrawTextW(dc, wide.c_str(), length < 0 ? -1 : (int)wide.size(), rect, format);
		}
		return originalDrawTextA ? originalDrawTextA(dc, text, length, rect, format) : 0;
	}

	BOOL WINAPI ExtTextOutAHook(HDC dc, int x, int y, UINT options, const RECT* rect,
		LPCSTR text, UINT length, const INT* spacing)
	{
		std::wstring wide;
		bool mapped = text && Utility::LookupLossyDisplayName(text, (int)length, wide);
		if( mapped )
		{
			const INT* wideSpacing = (spacing && wide.size() == length) ? spacing : nullptr;
			return ::ExtTextOutW(dc, x, y, options, rect, wide.c_str(), (UINT)wide.size(), wideSpacing);
		}
		return originalExtTextOutA ? originalExtTextOutA(dc, x, y, options, rect, text, length, spacing) : FALSE;
	}

	BOOL WINAPI TextOutAHook(HDC dc, int x, int y, LPCSTR text, int length)
	{
		std::wstring wide;
		bool mapped = text && Utility::LookupLossyDisplayName(text, length, wide);
		if( mapped )
		{
			return ::TextOutW(dc, x, y, wide.c_str(), (int)wide.size());
		}
		return originalTextOutA ? originalTextOutA(dc, x, y, text, length) : FALSE;
	}

	void hookTextDrawing()
	{
		originalDrawTextA = (decltype(originalDrawTextA))nCodeHook.createHookByName("user32.dll", "DrawTextA", DrawTextAHook);
		originalExtTextOutA = (decltype(originalExtTextOutA))nCodeHook.createHookByName("gdi32.dll", "ExtTextOutA", ExtTextOutAHook);
		originalTextOutA = (decltype(originalTextOutA))nCodeHook.createHookByName("gdi32.dll", "TextOutA", TextOutAHook);
	}

	bool LooksLikePath( LPCSTR text )
	{
		if( !text || !*text ) return false;
		return (text[0] && text[1] == ':' && (text[2] == '\\' || text[2] == '/'))
			|| strchr( text, '\\' ) != nullptr
			|| strchr( text, '/' ) != nullptr;
	}

	bool TryResolvePathText( HWND hWnd, LPCSTR text, std::wstring& resolved )
	{
		if( !text || (strchr( text, '?' ) == nullptr && !Utility::HasSyntheticAlias(text)) || !LooksLikePath( text )
			|| !::IsWindowUnicode( hWnd ) ) return false;

		resolved = Utility::GetWidePath( text );
		return !resolved.empty()
			&& resolved.find( L'?' ) == std::wstring::npos
			&& Utility::GetFileAttributesWLong( resolved ) != INVALID_FILE_ATTRIBUTES;
	}

	// A list/edit control may receive only a filename rather than a full path.
	// The ANSI spelling is still recoverable when it came from a lossy
	// WideCharToMultiByte(CP932) conversion observed by the conversion hook.
	bool TryResolveLossyText( HWND hWnd, LPCSTR text, int length, std::wstring& resolved )
	{
		return text && ::IsWindowUnicode(hWnd)
			&& Utility::LookupLossyDisplayName(text, length, resolved);
	}

	bool LooksLikePath( LPCWSTR text )
	{
		if( !text || !*text ) return false;
		return (text[0] && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/'))
			|| wcschr( text, L'\\' ) != nullptr
			|| wcschr( text, L'/' ) != nullptr;
	}

	// A W call can still contain '?' if Leeyes converted the path to ANSI before
	// handing it to a Unicode control. Resolve wildcard components one directory
	// at a time so this also works when the damaged component is not the leaf.
	bool TryResolveWidePathText( HWND hWnd, LPCWSTR text, std::wstring& resolved )
	{
		if( !text || !*text || !LooksLikePath(text)
			|| !::IsWindowUnicode( hWnd ) || !wcschr(text, L'?') ) return false;

		std::wstring input( text );
		if( input.size() < 3 || input[1] != L':' || input[2] != L'\\' ) return false;
		resolved = input.substr( 0, 3 );
		size_t pos = 3;
		while( pos <= input.size() )
		{
			size_t next = input.find_first_of( L"\\/", pos );
			std::wstring component = input.substr( pos,
				next == std::wstring::npos ? std::wstring::npos : next - pos );
			if( !component.empty() )
			{
				if( component.find_first_of( L"?*" ) != std::wstring::npos )
				{
					WIN32_FIND_DATAW findData = {};
					std::wstring pattern = resolved + component;
					HANDLE hFind = ::FindFirstFileW( pattern.c_str(), &findData );
					if( hFind == INVALID_HANDLE_VALUE ) return false;
					::FindClose( hFind );
					resolved += findData.cFileName;
				}
				else resolved += component;
			}
			if( next == std::wstring::npos ) break;
			resolved += L'\\';
			pos = next + 1;
		}
		return resolved.find(L'?') == std::wstring::npos
			&& Utility::GetFileAttributesWLong( resolved ) != INVALID_FILE_ATTRIBUTES;
	}

	BOOL WINAPI SetWindowTextAHook( HWND hWnd, LPCSTR lpString )
	{
		static thread_local bool resolving = false;
		std::wstring resolved;
		if( !resolving && (TryResolvePathText( hWnd, lpString, resolved )
			|| TryResolveLossyText( hWnd, lpString, -1, resolved )) )
		{
			resolving = true;
			BOOL ret = ::SetWindowTextW( hWnd, resolved.c_str() );
			resolving = false;
			return ret;
		}
		return originalSetWindowTextA( hWnd, lpString );
	}

	BOOL WINAPI SetWindowTextWHook( HWND hWnd, LPCWSTR lpString )
	{
		static thread_local bool resolving = false;
		std::wstring resolved;
		if( !resolving && TryResolveWidePathText( hWnd, lpString, resolved ) )
		{
			resolving = true;
			BOOL ret = originalSetWindowTextW( hWnd, resolved.c_str() );
			resolving = false;
			return ret;
		}
		return originalSetWindowTextW( hWnd, lpString );
	}

	bool IsTextMessage( UINT message )
	{
		switch( message )
		{
		case WM_SETTEXT:
		case CB_ADDSTRING:
		case CB_INSERTSTRING:
		case LB_ADDSTRING:
		case LB_INSERTSTRING:
			return true;
		default:
			return false;
		}
	}

	LRESULT WINAPI SendMessageAHook( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		static thread_local bool resolving = false;
		std::wstring resolved;
		if( !resolving && IsTextMessage( message )
			&& (TryResolvePathText( hWnd, reinterpret_cast<LPCSTR>(lParam), resolved )
				|| TryResolveLossyText( hWnd, reinterpret_cast<LPCSTR>(lParam), -1, resolved )) )
		{
			resolving = true;
			LRESULT ret = ::SendMessageW( hWnd, message, wParam, reinterpret_cast<LPARAM>(resolved.c_str()) );
			resolving = false;
			return ret;
		}
		return originalSendMessageA( hWnd, message, wParam, lParam );
	}

	LRESULT WINAPI SendMessageWHook( HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam )
	{
		static thread_local bool resolving = false;
		std::wstring resolved;
		if( !resolving && IsTextMessage( message )
			&& TryResolveWidePathText( hWnd, reinterpret_cast<LPCWSTR>(lParam), resolved ) )
		{
			resolving = true;
			LRESULT ret = originalSendMessageW( hWnd, message, wParam,
				reinterpret_cast<LPARAM>(resolved.c_str()) );
			resolving = false;
			return ret;
		}
		return originalSendMessageW( hWnd, message, wParam, lParam );
	}

	void hookSetWindowTextA()
	{
		originalSetWindowTextA = nCodeHook.createHookByName("user32.dll", "SetWindowTextA", SetWindowTextAHook);
		originalSetWindowTextW = nCodeHook.createHookByName("user32.dll", "SetWindowTextW", SetWindowTextWHook);
		originalSendMessageA = nCodeHook.createHookByName("user32.dll", "SendMessageA", SendMessageAHook);
		originalSendMessageW = nCodeHook.createHookByName("user32.dll", "SendMessageW", SendMessageWHook);
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			fprintf(lf, "hookWindowText install A=%s W=%s SendA=%s SendW=%s\n",
				originalSetWindowTextA ? "OK" : "FAILED", originalSetWindowTextW ? "OK" : "FAILED",
				originalSendMessageA ? "OK" : "FAILED", originalSendMessageW ? "OK" : "FAILED");
			fclose(lf);
		}
	}
}

namespace Shell32
{
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

typedef UINT  (WINAPI *DragQueryFileWFPtr)(__in HDROP hDrop, __in UINT iFile, __out_ecount_opt(cch) LPWSTR lpszFile, __in UINT cch);
DragQueryFileWFPtr originalDragQueryFileW = nullptr;

UINT WINAPI DragQueryFileWHook( __in HDROP hDrop, __in UINT iFile, __out_ecount_opt(cch) LPWSTR lpszFile, __in UINT cch )
{
	if( iFile == 0xffffffff )
		return originalDragQueryFileW(hDrop, iFile, lpszFile, cch);

	UINT originalSize = originalDragQueryFileW(hDrop, iFile, lpszFile, cch);
	#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			char pathUtf8[MAX_PATH * 2] = {};
			if( lpszFile ) WideCharToMultiByte(CP_UTF8, 0, lpszFile, -1, pathUtf8, sizeof(pathUtf8), 0, 0);
			fprintf(lf, "DragQueryFileW call i=%u cch=%u size=%u path=[%s]\n", iFile, cch, originalSize, lpszFile ? pathUtf8 : "<null>");
			fclose(lf);
		}
	}
	#endif
	UINT wideSize = originalDragQueryFileW(hDrop, iFile, nullptr, 0);
	if( !wideSize ) return originalSize;

	std::wstring originalPath(wideSize + 1, L'\0');
	wideSize = originalDragQueryFileW(hDrop, iFile, const_cast<wchar_t*>(originalPath.data()), (UINT)originalPath.size());
	if( !wideSize ) return originalSize;
	originalPath.resize(wideSize);

	std::string ansiPath;
	std::wstring collisionPath;
	if( !Utility::WideToCP932BestFit(originalPath, ansiPath)
		|| !Utility::ResolveAnsiPathCollision(ansiPath, collisionPath)
		|| collisionPath != originalPath )
		return originalSize;

	// Return the Unicode spelling produced by CP932 decoding (U+30FB for the
	// U+00B7 collision). The file hooks map that legacy spelling back to the
	// unique real entry, while Leeyes no longer retains the unrepresentable
	// Unicode spelling in its ANSI-oriented path state.
	std::wstring legacyPath = Utility::GetWidePath(ansiPath);
	if( legacyPath.empty() || legacyPath == originalPath ) return originalSize;

	#if UNICODEHACK_PATH_DEBUG
	if( originalPath.find(L"neekosan") != std::wstring::npos )
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			char originalUtf8[MAX_PATH * 2] = {};
			char legacyUtf8[MAX_PATH * 2] = {};
			WideCharToMultiByte(CP_UTF8, 0, originalPath.c_str(), -1, originalUtf8, sizeof(originalUtf8), 0, 0);
			WideCharToMultiByte(CP_UTF8, 0, legacyPath.c_str(), -1, legacyUtf8, sizeof(legacyUtf8), 0, 0);
			fprintf(lf, "DragQueryFileW original=[%s] returned=[%s]\n", originalUtf8, legacyUtf8);
			fclose(lf);
		}
	}
	#endif

	if( !lpszFile ) return (UINT)legacyPath.size();
	if( cch == 0 ) return 0;
	UINT copyLength = (UINT)std::min<size_t>(legacyPath.size(), (size_t)cch - 1);
	wmemcpy(lpszFile, legacyPath.c_str(), copyLength);
	lpszFile[copyLength] = L'\0';
	return copyLength;
}

void hookDragQueryFileW()
{
	originalDragQueryFileW = nCodeHook.createHookByName("shell32.dll", "DragQueryFileW", DragQueryFileWHook);
	#if UNICODEHACK_PATH_DEBUG
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookDragQueryFileW install %s\n", originalDragQueryFileW ? "OK" : "FAILED");
		fclose(lf);
	}
	#endif
}

typedef UINT  (WINAPI *DragQueryFileAFPtr)(__in HDROP hDrop, __in UINT iFile, __out_ecount_opt(cch) LPSTR lpszFile, __in UINT cch);
DragQueryFileAFPtr originalDragQueryFileA = nullptr;

UINT   WINAPI DragQueryFileAHook( __in HDROP hDrop, __in UINT iFile, __out_ecount_opt(cch) LPSTR lpszFile, __in UINT cch)
{
	auto size = originalDragQueryFileA( hDrop, iFile, lpszFile, cch );
	#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			fprintf(lf, "DragQueryFileA call i=%u cch=%u size=%u hasBuffer=%s\n",
				iFile, cch, size, lpszFile ? "yes" : "no");
			if( lpszFile && strstr(lpszFile, "neekosan") )
			{
				fprintf(lf, "DragQueryFileA path=[%s] bytes=", lpszFile);
				for( const unsigned char* p = reinterpret_cast<const unsigned char*>(lpszFile); *p; ++p )
					fprintf(lf, "%02X", *p);
				fprintf(lf, "\n");
			}
			fclose(lf);
		}
	}
	#endif
	if( iFile == 0xffffffff )
	{
		return size;
	}
	else if( lpszFile )
	{
		// DragQueryFileA can silently collapse distinct Unicode characters to the
		// same CP932 bytes (for example U+00B7 and U+30FB).  If that ANSI spelling
		// does not exist, return a collision-resolvable CP932 spelling backed by
		// the exact W path so Leeyes stays on the real drive.
		bool ansiPathMissing = size && size < cch && Kernel32::originalGetFileAttributesA
			&& Kernel32::originalGetFileAttributesA(lpszFile) == INVALID_FILE_ATTRIBUTES;
		if(size && memchr(lpszFile, '?' , size) == nullptr && !ansiPathMissing)
		{
			return size;
		}
		auto wsize = DragQueryFileW( hDrop, iFile,nullptr,0 );
		std::wstring wstr(  wsize+1, L'\0');
		size = DragQueryFileW( hDrop, iFile,const_cast<wchar_t*>( wstr.data() ), wstr.size() );
		if( ansiPathMissing )
		{
			std::wstring directCollisionPath;
			if( Utility::ResolveAnsiPathCollision(lpszFile, directCollisionPath) )
			{
				#if UNICODEHACK_PATH_DEBUG
				FILE* lf = nullptr;
				if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
				{
					char wideUtf8[MAX_PATH * 2] = {};
					WideCharToMultiByte(CP_UTF8, 0, directCollisionPath.c_str(), -1, wideUtf8, sizeof(wideUtf8), 0, 0);
					fprintf(lf, "DragQueryFileA direct collision=[%s] returned=[%s]\n", wideUtf8, lpszFile);
					fclose(lf);
				}
				#endif
				return size;
			}

			auto fullWidePath = MakePath(lpszFile, wstr);
			std::string safePath;
			std::wstring collisionPath;
			if( Utility::WideToCP932BestFit(fullWidePath, safePath)
				&& Utility::ResolveAnsiPathCollision(safePath, collisionPath)
				&& collisionPath == fullWidePath )
			{
				// Keep the real CP932 spelling. The A-file hooks resolve this
				// collision back to fullWidePath without exposing a TEMP alias.
			}
			else safePath = Utility::GetShortPath(fullWidePath);
			if( !safePath.empty() && safePath.size() + 1 <= cch )
			{
				::strcpy_s(lpszFile, cch, safePath.c_str());
				#if UNICODEHACK_PATH_DEBUG
				FILE* lf = nullptr;
				if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
				{
					char wideUtf8[MAX_PATH * 2] = {};
					WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, wideUtf8, sizeof(wideUtf8), 0, 0);
					fprintf(lf, "DragQueryFileA resolved wide=[%s] returned=[%s]\n", wideUtf8, lpszFile);
					fclose(lf);
				}
				#endif
				return (UINT)safePath.size();
			}
		}
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
	#if UNICODEHACK_PATH_DEBUG
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "hookDragQueryFileA install %s\n", originalDragQueryFileA ? "OK" : "FAILED");
		fclose(lf);
	}
	#endif
}
}

namespace SusieAM00
{
typedef int   (__stdcall *IsSupportedAM00FPtr)(LPSTR filename, DWORD dw);
typedef int  ( __stdcall *GetArchiveInfoAM00FPtr)(LPSTR buf, long len, unsigned int flag, HLOCAL *lphInf);
typedef int (__stdcall *GetFileInfoAM00FPtr)(LPSTR buf, long len, LPSTR filename, unsigned int flag, LPVOID lpInfo);
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
GetArchiveInfoAM00FPtr originalGetArchiveInfo = nullptr;
GetFileAM00FPtr originalGetFile = nullptr;
std::map<std::string, std::wstring> gArchiveWidePathCache;
std::mutex gArchiveWidePathCacheMutex;

bool NeedsArchivePathAlias( LPCSTR path )
{
	if( !path ) return false;
	if( strchr(path, '?') ) return true;
	for( const unsigned char* p = reinterpret_cast<const unsigned char*>(path); *p; ++p )
		if( *p >= 0x80 ) return true;
	return false;
}

std::wstring GetArchiveWidePath( LPCSTR path )
{
	if( !path ) return std::wstring();
	std::string ansiPath(path);
	{
		std::lock_guard<std::mutex> lock(gArchiveWidePathCacheMutex);
		auto it = gArchiveWidePathCache.find(ansiPath);
		if( it != gArchiveWidePathCache.end() ) return it->second;
	}

	std::wstring widePath = Utility::GetWidePath(ansiPath);
	if( widePath.empty() || Utility::GetFileAttributesWLong(widePath) == INVALID_FILE_ATTRIBUTES )
	{
		std::wstring collisionPath;
		if( Utility::ResolveAnsiPathCollision(ansiPath, collisionPath) )
			widePath = collisionPath;
	}
	if( !widePath.empty() && Utility::GetFileAttributesWLong(widePath) != INVALID_FILE_ATTRIBUTES )
	{
		std::lock_guard<std::mutex> lock(gArchiveWidePathCacheMutex);
		gArchiveWidePathCache[ansiPath] = widePath;
	}
	return widePath;
}

std::string GetArchiveRetryPath( LPCSTR path )
{
	std::wstring widePath = GetArchiveWidePath(path);
	if( widePath.empty() ) return std::string();

	// If CP932 gives us a unique real directory entry, keep the plugin on the
	// user's actual path. The A/W file hooks repair the lossy CP932 spelling at
	// the filesystem boundary, so no TEMP alias is needed when it is unique.
	std::string directPath;
	std::wstring collisionPath;
	if( Utility::WideToCP932BestFit(widePath, directPath)
		&& Utility::ResolveAnsiPathCollision(directPath, collisionPath)
		&& collisionPath == widePath )
		return directPath;

	return Utility::GetArchivePathAlias( widePath );
}

struct ZipEntryInfo
{
	uint16_t method = 0;
	uint32_t compressedSize = 0;
	uint32_t uncompressedSize = 0;
	uint32_t localHeaderOffset = 0;
};

bool ReadArchiveRange( HANDLE file, uint64_t offset, void* buffer, size_t size )
{
	LARGE_INTEGER position;
	position.QuadPart = (LONGLONG)offset;
	if( !::SetFilePointerEx(file, position, nullptr, FILE_BEGIN) ) return false;
	BYTE* out = reinterpret_cast<BYTE*>(buffer);
	while( size )
	{
		DWORD chunk = (DWORD)std::min<size_t>(size, 1u << 20);
		DWORD read = 0;
		if( !::ReadFile(file, out, chunk, &read, nullptr) || read != chunk ) return false;
		out += read;
		size -= read;
	}
	return true;
}

uint16_t ZipU16( const BYTE* p )
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t ZipU32( const BYTE* p )
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool BuildZipIndex( const std::wstring& path, std::vector<ZipEntryInfo>& entries )
{
	HANDLE file = Utility::CreateFileWLong(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if( file == INVALID_HANDLE_VALUE ) return false;
	LARGE_INTEGER fileSize;
	bool ok = ::GetFileSizeEx(file, &fileSize) != FALSE && fileSize.QuadPart >= 22;
	if( !ok ) { ::CloseHandle(file); return false; }

	const uint64_t tailLength = std::min<uint64_t>((uint64_t)fileSize.QuadPart, 0x10000ull + 22ull);
	std::vector<BYTE> tail((size_t)tailLength);
	ok = ReadArchiveRange(file, (uint64_t)fileSize.QuadPart - tailLength, tail.data(), tail.size());
	if( !ok ) { ::CloseHandle(file); return false; }

	size_t eocd = SIZE_MAX;
	for( size_t i = tail.size() - 22; ; --i )
	{
		if( ZipU32(tail.data() + i) == 0x06054B50u ) { eocd = i; break; }
		if( i == 0 ) break;
	}
	if( eocd == SIZE_MAX ) { ::CloseHandle(file); return false; }

	uint16_t count = ZipU16(tail.data() + eocd + 10);
	uint32_t centralSize = ZipU32(tail.data() + eocd + 12);
	uint32_t centralOffset = ZipU32(tail.data() + eocd + 16);
	if( !count || !centralSize || (uint64_t)centralOffset + centralSize > (uint64_t)fileSize.QuadPart )
	{
		::CloseHandle(file);
		return false;
	}

	std::vector<BYTE> central(centralSize);
	ok = ReadArchiveRange(file, centralOffset, central.data(), central.size());
	::CloseHandle(file);
	if( !ok ) return false;

	size_t cursor = 0;
	for( uint16_t i = 0; i < count && cursor + 46 <= central.size(); ++i )
	{
		const BYTE* header = central.data() + cursor;
		if( ZipU32(header) != 0x02014B50u ) return false;
		uint16_t nameLength = ZipU16(header + 28);
		uint16_t extraLength = ZipU16(header + 30);
		uint16_t commentLength = ZipU16(header + 32);
		size_t recordSize = 46ull + nameLength + extraLength + commentLength;
		if( cursor + recordSize > central.size() ) return false;
		ZipEntryInfo entry;
		entry.method = ZipU16(header + 10);
		entry.compressedSize = ZipU32(header + 20);
		entry.uncompressedSize = ZipU32(header + 24);
		entry.localHeaderOffset = ZipU32(header + 42);
		entries.push_back(entry);
		cursor += recordSize;
	}
	return entries.size() == count;
}

class DeflateBitReader
{
	const std::vector<BYTE>& m_Data;
	size_t m_Position = 0;
	uint32_t m_Bits = 0;
	int m_BitCount = 0;
public:
	explicit DeflateBitReader( const std::vector<BYTE>& data ) : m_Data(data) {}
	bool Ensure( int count )
	{
		while( m_BitCount < count && m_Position < m_Data.size() )
		{
			m_Bits |= (uint32_t)m_Data[m_Position++] << m_BitCount;
			m_BitCount += 8;
		}
		return m_BitCount >= count;
	}
	uint32_t Peek( int count )
	{
		return m_Bits & ((1u << count) - 1u);
	}
	void Drop( int count )
	{
		m_Bits >>= count;
		m_BitCount -= count;
	}
	bool Read( int count, uint32_t& value )
	{
		if( !Ensure(count) ) return false;
		value = Peek(count);
		Drop(count);
		return true;
	}
	void AlignByte()
	{
		int drop = m_BitCount & 7;
		if( drop ) Drop(drop);
	}
};

struct DeflateHuffman
{
	struct Code { uint16_t code; uint8_t length; uint16_t symbol; };
	std::vector<Code> codes;
	std::vector<uint32_t> lookup;

	bool Build( const std::vector<int>& lengths )
	{
		int count[16] = {};
		for( int length : lengths )
		{
			if( length < 0 || length > 15 ) return false;
			if( length ) ++count[length];
		}
		int next[16] = {};
		int code = 0;
		for( int bits = 1; bits <= 15; ++bits )
		{
			code = (code + count[bits - 1]) << 1;
			next[bits] = code;
		}
		codes.clear();
		for( size_t symbol = 0; symbol < lengths.size(); ++symbol )
		{
			int length = lengths[symbol];
			if( !length ) continue;
			int canonical = next[length]++;
			uint16_t reversed = 0;
			for( int bit = 0; bit < length; ++bit ) reversed = (uint16_t)((reversed << 1) | ((canonical >> bit) & 1));
			codes.push_back({reversed, (uint8_t)length, (uint16_t)symbol});
		}
		lookup.assign(1u << 15, 0);
		for( const auto& item : codes )
		{
			uint32_t packed = ((uint32_t)item.length << 16) | item.symbol;
			uint32_t repeats = 1u << (15 - item.length);
			for( uint32_t i = 0; i < repeats; ++i ) lookup[item.code | (i << item.length)] = packed;
		}
		return !codes.empty();
	}

	bool Decode( DeflateBitReader& reader, int& symbol ) const
	{
		if( reader.Ensure(15) )
		{
			uint32_t packed = lookup[reader.Peek(15)];
			if( !packed ) return false;
			int length = (int)(packed >> 16);
			symbol = (int)(packed & 0xFFFF);
			reader.Drop(length);
			return true;
		}
		uint32_t bits = 0;
		for( int length = 1; length <= 15; ++length )
		{
			uint32_t bit = 0;
			if( !reader.Read(1, bit) ) return false;
			bits |= bit << (length - 1);
			for( const auto& item : codes )
				if( item.length == length && item.code == bits ) { symbol = item.symbol; return true; }
		}
		return false;
	}
};

bool InflateDeflate( const std::vector<BYTE>& compressed, size_t expectedSize, std::vector<BYTE>& output )
{
	static const int lengthBase[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
	static const int lengthExtra[] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
	static const int distanceBase[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
	static const int distanceExtra[] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
	DeflateBitReader reader(compressed);
	output.clear();
	output.reserve(expectedSize);
	bool finalBlock = false;
	while( !finalBlock )
	{
		uint32_t finalBit = 0, blockType = 0;
		if( !reader.Read(1, finalBit) || !reader.Read(2, blockType) || blockType == 3 ) return false;
		finalBlock = finalBit != 0;
		if( blockType == 0 )
		{
			reader.AlignByte();
			uint32_t length = 0, inverse = 0;
			if( !reader.Read(16, length) || !reader.Read(16, inverse) || ((length ^ inverse) & 0xFFFFu) != 0xFFFFu ) return false;
			for( uint32_t i = 0; i < length; ++i ) { uint32_t byte = 0; if( !reader.Read(8, byte) ) return false; output.push_back((BYTE)byte); }
			continue;
		}

		std::vector<int> literalLengths(288), distanceLengths(32);
		if( blockType == 1 )
		{
			for( int i = 0; i <= 143; ++i ) literalLengths[i] = 8;
			for( int i = 144; i <= 255; ++i ) literalLengths[i] = 9;
			for( int i = 256; i <= 279; ++i ) literalLengths[i] = 7;
			for( int i = 280; i < 288; ++i ) literalLengths[i] = 8;
			std::fill(distanceLengths.begin(), distanceLengths.end(), 5);
		}
		else
		{
			uint32_t hlit = 0, hdist = 0, hclen = 0;
			if( !reader.Read(5, hlit) || !reader.Read(5, hdist) || !reader.Read(4, hclen) ) return false;
			hlit += 257; hdist += 1; hclen += 4;
			static const int codeLengthOrder[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
			std::vector<int> codeLengthLengths(19);
			for( uint32_t i = 0; i < hclen; ++i ) { uint32_t value = 0; if( !reader.Read(3, value) ) return false; codeLengthLengths[codeLengthOrder[i]] = (int)value; }
			DeflateHuffman codeLengthTree;
			if( !codeLengthTree.Build(codeLengthLengths) ) return false;
			std::vector<int> allLengths;
			allLengths.reserve(hlit + hdist);
			while( allLengths.size() < hlit + hdist )
			{
				int symbol = 0;
				if( !codeLengthTree.Decode(reader, symbol) ) return false;
				if( symbol <= 15 ) allLengths.push_back(symbol);
				else if( symbol == 16 )
				{
					if( allLengths.empty() ) return false;
					uint32_t extra = 0; if( !reader.Read(2, extra) ) return false;
					int repeat = 3 + (int)extra, value = allLengths.back();
					if( allLengths.size() + repeat > hlit + hdist ) return false;
					while( repeat-- ) allLengths.push_back(value);
				}
				else if( symbol == 17 || symbol == 18 )
				{
					int repeatBase = symbol == 17 ? 3 : 11, repeatBits = symbol == 17 ? 3 : 7;
					uint32_t extra = 0; if( !reader.Read(repeatBits, extra) ) return false;
					int repeat = repeatBase + (int)extra;
					if( allLengths.size() + repeat > hlit + hdist ) return false;
					while( repeat-- ) allLengths.push_back(0);
				}
				else return false;
			}
			literalLengths.assign(allLengths.begin(), allLengths.begin() + hlit);
			distanceLengths.assign(allLengths.begin() + hlit, allLengths.end());
		}

		DeflateHuffman literalTree, distanceTree;
		if( !literalTree.Build(literalLengths) || !distanceTree.Build(distanceLengths) ) return false;
		while( true )
		{
			int symbol = 0;
			if( !literalTree.Decode(reader, symbol) ) return false;
			if( symbol < 256 ) output.push_back((BYTE)symbol);
			else if( symbol == 256 ) break;
			else if( symbol >= 257 && symbol <= 285 )
			{
				int lengthIndex = symbol - 257;
				uint32_t extra = 0;
				if( !reader.Read(lengthExtra[lengthIndex], extra) ) return false;
				int length = lengthBase[lengthIndex] + (int)extra;
				int distanceSymbol = 0;
				if( !distanceTree.Decode(reader, distanceSymbol) || distanceSymbol < 0 || distanceSymbol >= 30 ) return false;
				if( !reader.Read(distanceExtra[distanceSymbol], extra) ) return false;
				int distance = distanceBase[distanceSymbol] + (int)extra;
				if( distance <= 0 || (size_t)distance > output.size() ) return false;
				for( int i = 0; i < length; ++i ) output.push_back(output[output.size() - distance]);
			}
			else return false;
			if( output.size() > expectedSize ) return false;
		}
	}
	return output.size() == expectedSize;
}

bool ExtractZipEntry( const std::wstring& path, long index, HLOCAL* outputHandle )
{
	if( !outputHandle || index < 0 ) return false;
	static std::mutex cacheMutex;
	static std::map<std::wstring, std::vector<ZipEntryInfo>> cache;
	std::vector<ZipEntryInfo> entries;
	{
		std::lock_guard<std::mutex> lock(cacheMutex);
		auto found = cache.find(path);
		if( found != cache.end() ) entries = found->second;
	}
	if( entries.empty() )
	{
		if( !BuildZipIndex(path, entries) ) return false;
		std::lock_guard<std::mutex> lock(cacheMutex);
		cache[path] = entries;
	}
	if( (size_t)index >= entries.size() ) return false;
	const ZipEntryInfo& entry = entries[(size_t)index];
	if( entry.uncompressedSize > (256u * 1024u * 1024u) ) return false;

	HANDLE file = Utility::CreateFileWLong(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if( file == INVALID_HANDLE_VALUE ) return false;
	BYTE localHeader[30] = {};
	bool ok = ReadArchiveRange(file, entry.localHeaderOffset, localHeader, sizeof(localHeader));
	if( !ok || ZipU32(localHeader) != 0x04034B50u ) { ::CloseHandle(file); return false; }
	uint16_t nameLength = ZipU16(localHeader + 26);
	uint16_t extraLength = ZipU16(localHeader + 28);
	uint64_t dataOffset = (uint64_t)entry.localHeaderOffset + 30ull + nameLength + extraLength;
	std::vector<BYTE> compressed(entry.compressedSize);
	ok = ReadArchiveRange(file, dataOffset, compressed.data(), compressed.size());
	::CloseHandle(file);
	if( !ok ) return false;

	std::vector<BYTE> output;
	if( entry.method == 0 ) output = compressed;
	else if( entry.method == 8 )
	{
		if( !InflateDeflate(compressed, entry.uncompressedSize, output) ) return false;
	}
	else return false;
	if( output.size() != entry.uncompressedSize ) return false;
	HLOCAL memory = (HLOCAL)::LocalAlloc(LMEM_FIXED, output.empty() ? 1 : output.size());
	if( !memory ) return false;
	if( !output.empty() ) memcpy(memory, output.data(), output.size());
	*outputHandle = memory;
	return true;
}

#if UNICODEHACK_PLUGIN_DEBUG
void LogPluginPath( const char* api, LPCSTR input, const char* alias,
	int firstResult, int secondResult, const char* module = nullptr )
{
	static std::atomic<int> count = 0;
	if( count.fetch_add(1) >= 100000 ) return;
	FILE* lf = nullptr;
	if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
	{
		fprintf(lf, "plugin %s module=[%s] input=[%s] alias=[%s] first=%d second=%d\n",
			api, module ? module : "", input ? input : "<null>", alias ? alias : "",
			firstResult, secondResult);
		fclose(lf);
	}
}
#endif

int __stdcall GetArchiveInfoHook(LPSTR Buf, long Len, unsigned int Flag, HLOCAL *Inf)
{
	auto ret = originalGetArchiveInfo( Buf , Len, Flag, Inf );
	auto originalResult = ret;
	std::string retryPath;
	if( !(Flag & 0x07) && Buf && NeedsArchivePathAlias(Buf) && ret != SPI_ALL_RIGHT )
	{
		retryPath = GetArchiveRetryPath( Buf );
		if( !retryPath.empty() )
			ret = originalGetArchiveInfo( const_cast<LPSTR>(retryPath.c_str()), Len, Flag, Inf );
	}
#if UNICODEHACK_PLUGIN_DEBUG
	LogPluginPath( "GetArchiveInfo", Buf, retryPath.c_str(), originalResult, ret, "archive" );
#endif
	return ret;
}

int __stdcall GetFileHook(LPSTR Src, long Len, LPSTR Dst, unsigned int Flag, SPI_PROGRESS PrgressCallback, long Data)
{
	auto ret =originalGetFile( Src, Len, Dst, Flag, PrgressCallback, Data );
	auto originalResult = ret;
	std::string retryPath;
	if( !(Flag & 0x07) && Src && NeedsArchivePathAlias(Src) && ret != SPI_ALL_RIGHT )
	{
		retryPath = GetArchiveRetryPath( Src );
		if( !retryPath.empty() )
			ret = originalGetFile(const_cast<LPSTR>(retryPath.c_str()), Len, Dst, Flag, PrgressCallback, Data );
	}
#if UNICODEHACK_PLUGIN_DEBUG
	LogPluginPath( "GetFile", Src, retryPath.c_str(), originalResult, ret, "archive" );
#endif
	return ret;
}

// Archive plugins are not limited to the configured ax7z.spi. In particular,
// ZIP files are handled by arc.spi in the stock Leeyes installation. Wrap the
// Susie entry points returned by GetProcAddress per module, just like image
// plugins below, so each plugin keeps its own original function pointer.
enum ArchivePluginFunction
{
	ArchivePluginIsSupported,
	ArchivePluginGetArchiveInfo,
	ArchivePluginGetFileInfo,
	ArchivePluginGetFile
};

struct ArchivePluginCallContext
{
	ArchivePluginFunction function;
	FARPROC original;
	char moduleName[MAX_PATH];
};

int __stdcall ArchiveIsSupportedDispatchImpl(ArchivePluginCallContext* context, LPSTR Filename, DWORD Dw)
{
	auto original = reinterpret_cast<IsSupportedAM00FPtr>(context->original);
	int ret = original(Filename, Dw);
#if UNICODEHACK_PLUGIN_DEBUG
	const char* moduleLeaf = strrchr(context->moduleName, '\\');
	moduleLeaf = moduleLeaf ? moduleLeaf + 1 : context->moduleName;
	if( _stricmp(moduleLeaf, "arc.spi") == 0 )
	{
		char detail[96] = {};
		sprintf_s(detail, "filename_ptr=%p dw=%lu", Filename, (unsigned long)Dw);
		LogPluginPath("IsSupported", (Dw < 0x10000) ? Filename : nullptr,
			detail, ret, ret, context->moduleName);
	}
#endif
	return ret;
}

int __stdcall ArchiveFileInfoDispatchImpl(ArchivePluginCallContext* context, LPSTR Buf,
	long Len, LPSTR Filename, unsigned int Flag, LPVOID Inf)
{
	auto original = reinterpret_cast<GetFileInfoAM00FPtr>(context->original);
	auto ret = original( Buf, Len, Filename, Flag, Inf );
	auto originalResult = ret;
	std::string retryPath;
	if( !(Flag & 0x07) && Buf && NeedsArchivePathAlias(Buf) && ret != SPI_ALL_RIGHT )
	{
		retryPath = GetArchiveRetryPath( Buf );
		if( !retryPath.empty() )
			ret = original( const_cast<LPSTR>(retryPath.c_str()), Len, Filename, Flag, Inf );
	}
#if UNICODEHACK_PLUGIN_DEBUG
	char nameInfo[MAX_PATH * 2] = {};
	sprintf_s(nameInfo, "filename=%s", Filename ? Filename : "<null>");
	LogPluginPath( "GetFileInfo", Buf, retryPath.empty() ? nameInfo : retryPath.c_str(), originalResult, ret, context->moduleName );
#endif
	return ret;
}

int __stdcall ArchiveInfoDispatchImpl(ArchivePluginCallContext* context, LPSTR Buf,
	long Len, unsigned int Flag, HLOCAL *Inf)
{
	auto original = reinterpret_cast<GetArchiveInfoAM00FPtr>(context->original);
	int originalResult = original( Buf, Len, Flag, Inf );
	int ret = originalResult;
	std::string retryPath;
	bool needsAlias = !(Flag & 0x07) && Buf && NeedsArchivePathAlias(Buf);
	if( ret != SPI_ALL_RIGHT && needsAlias )
	{
		retryPath = GetArchiveRetryPath( Buf );
		if( !retryPath.empty() )
			ret = original( const_cast<LPSTR>(retryPath.c_str()), Len, Flag, Inf );
	}
#if UNICODEHACK_PLUGIN_DEBUG
	LogPluginPath( "GetArchiveInfo", Buf, retryPath.c_str(), originalResult, ret, context->moduleName );
#endif
	return ret;
}

int __stdcall ArchiveGetFileDispatchImpl(ArchivePluginCallContext* context, LPSTR Src,
	long Len, LPSTR Dst, unsigned int Flag, SPI_PROGRESS PrgressCallback, long Data)
{
	auto original = reinterpret_cast<GetFileAM00FPtr>(context->original);
#if UNICODEHACK_PLUGIN_DEBUG
	char callInfo[160] = {};
	sprintf_s(callInfo, "dst=%p len=%ld flag=%u data=%ld", Dst, Len, Flag, Data);
	LogPluginPath( "GetFileArgs", Src, callInfo, 0, 0, context->moduleName );
#endif
	int originalResult = SPI_OTHER_ERROR;
	int ret = SPI_OTHER_ERROR;
	std::string retryPath;
	bool needsAlias = !(Flag & 0x07) && Src && NeedsArchivePathAlias(Src);
	const char* moduleFileName = strrchr(context->moduleName, '\\');
	if( moduleFileName ) ++moduleFileName;
	else moduleFileName = context->moduleName;
	if( needsAlias && _stricmp(moduleFileName, "arc.spi") == 0 && Dst && (Flag & 0x100) )
	{
		std::wstring wideArchive = GetArchiveWidePath(Src);
		HLOCAL extracted = nullptr;
		bool extractedOk = ExtractZipEntry(wideArchive, Len, &extracted);
		if( !extractedOk && Len > 0 ) extractedOk = ExtractZipEntry(wideArchive, Len - 1, &extracted);
		if( extractedOk )
		{
			*reinterpret_cast<HLOCAL*>(Dst) = extracted;
#if UNICODEHACK_PLUGIN_DEBUG
			char extractInfo[160] = {};
			BYTE* extractData = reinterpret_cast<BYTE*>(::LocalLock(extracted));
			SIZE_T extractSize = ::LocalSize(extracted);
			if( extractData )
				sprintf_s(extractInfo, "native-deflate size=%llu head=%02X%02X%02X%02X%02X%02X%02X%02X",
					(unsigned long long)extractSize, extractData[0], extractData[1], extractData[2], extractData[3],
					extractData[4], extractData[5], extractData[6], extractData[7]);
			else strcpy_s(extractInfo, "native-deflate lock-failed");
			if( extractData ) ::LocalUnlock(extracted);
			LogPluginPath( "GetFileZipFallback", Src, extractInfo, SPI_ALL_RIGHT, SPI_ALL_RIGHT, context->moduleName );
#endif
			return SPI_ALL_RIGHT;
		}
	}
	if( needsAlias )
	{
		retryPath = GetArchiveRetryPath( Src );
		if( !retryPath.empty() )
			ret = original( const_cast<LPSTR>(retryPath.c_str()), Len, Dst, Flag, PrgressCallback, Data );
	}
	if( !needsAlias || ret != SPI_ALL_RIGHT )
	{
		originalResult = original( Src, Len, Dst, Flag, PrgressCallback, Data );
		ret = originalResult;
	}
#if UNICODEHACK_PLUGIN_DEBUG
	LogPluginPath( "GetFile", Src, Dst ? Dst : retryPath.c_str(), originalResult, ret, context->moduleName );
	if( Dst && (Flag & 0x100) && ret == SPI_ALL_RIGHT )
	{
		HLOCAL dataHandle = *reinterpret_cast<HLOCAL*>(Dst);
		SIZE_T dataSize = dataHandle ? ::LocalSize(dataHandle) : 0;
		SIZE_T globalSize = dataHandle ? ::GlobalSize(dataHandle) : 0;
		BYTE* data = dataHandle ? reinterpret_cast<BYTE*>(::LocalLock(dataHandle)) : nullptr;
		if( !data && dataHandle ) data = dataHandle == nullptr ? nullptr : reinterpret_cast<BYTE*>(::GlobalLock(dataHandle));
		char dataInfo[160] = {};
		if( data )
			sprintf_s(dataInfo, "handle=%p lsize=%llu gsize=%llu head=%02X%02X%02X%02X", dataHandle,
				(unsigned long long)dataSize, (unsigned long long)globalSize, data[0], data[1], data[2], data[3]);
		else
			sprintf_s(dataInfo, "handle=%p lsize=%llu gsize=%llu data=null", dataHandle,
				(unsigned long long)dataSize, (unsigned long long)globalSize);
		LogPluginPath( "GetFileData", Src, dataInfo, originalResult, ret, context->moduleName );
		if( dataHandle ) ::LocalUnlock(dataHandle);
		if( dataHandle ) ::GlobalUnlock(dataHandle);
	}
#endif
	return ret;
}

__declspec(naked) int __stdcall ArchiveIsSupportedDispatch()
{
	__asm
	{
		mov edx, esp
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call ArchiveIsSupportedDispatchImpl
		ret 8
	}
}

__declspec(naked) int __stdcall ArchiveInfoDispatch()
{
	__asm
	{
		mov edx, esp
		push dword ptr [edx+16]
		push dword ptr [edx+12]
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call ArchiveInfoDispatchImpl
		ret 16
	}
}

__declspec(naked) int __stdcall ArchiveFileInfoDispatch()
{
	__asm
	{
		mov edx, esp
		push dword ptr [edx+20]
		push dword ptr [edx+16]
		push dword ptr [edx+12]
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call ArchiveFileInfoDispatchImpl
		ret 20
	}
}

__declspec(naked) int __stdcall ArchiveGetFileDispatch()
{
	__asm
	{
		mov edx, esp
		push dword ptr [edx+24]
		push dword ptr [edx+20]
		push dword ptr [edx+16]
		push dword ptr [edx+12]
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call ArchiveGetFileDispatchImpl
		ret 24
	}
}

void* CreateArchivePluginThunk(ArchivePluginCallContext* context, void* dispatch)
{
	BYTE* code = reinterpret_cast<BYTE*>(::VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if( !code ) return nullptr;
	code[0] = 0xB8;
	*reinterpret_cast<DWORD*>(code + 1) = static_cast<DWORD>(reinterpret_cast<uintptr_t>(context));
	code[5] = 0xE9;
	intptr_t relative = reinterpret_cast<BYTE*>(dispatch) - (code + 10);
	if( relative < INT32_MIN || relative > INT32_MAX )
	{
		::VirtualFree(code, 0, MEM_RELEASE);
		return nullptr;
	}
	*reinterpret_cast<LONG*>(code + 6) = static_cast<LONG>(relative);
	::FlushInstructionCache(::GetCurrentProcess(), code, 10);
	return code;
}

FARPROC WrapArchivePluginFunction(HMODULE hModule, const char* procName, FARPROC original, const char* moduleName)
{
	void* dispatch = nullptr;
	ArchivePluginFunction function;
	if( strcmp(procName, "IsSupported") == 0 )
	{
		dispatch = reinterpret_cast<void*>(&ArchiveIsSupportedDispatch);
		function = ArchivePluginIsSupported;
	}
	else if( strcmp(procName, "GetArchiveInfo") == 0 )
	{
		dispatch = reinterpret_cast<void*>(&ArchiveInfoDispatch);
		function = ArchivePluginGetArchiveInfo;
	}
	else if( strcmp(procName, "GetFileInfo") == 0 )
	{
		dispatch = reinterpret_cast<void*>(&ArchiveFileInfoDispatch);
		function = ArchivePluginGetFileInfo;
	}
	else if( strcmp(procName, "GetFile") == 0 )
	{
		dispatch = reinterpret_cast<void*>(&ArchiveGetFileDispatch);
		function = ArchivePluginGetFile;
	}
	else return original;

	auto* context = new ArchivePluginCallContext();
	context->function = function;
	context->original = original;
	strncpy_s(context->moduleName, sizeof(context->moduleName), moduleName ? moduleName : "", _TRUNCATE);
	(void)hModule;
	void* thunk = CreateArchivePluginThunk(context, dispatch);
	return thunk ? reinterpret_cast<FARPROC>(thunk) : original;
}

// Non-BMP images (jpg, png, ...) are decoded by whichever Susie *image* plugin (.spi) is
// registered for that format, via a separate plugin interface (GetPictureInfo/GetPicture)
// from the archive one above (GetArchiveInfo/GetFile). Without hooking these too, a Unicode
// path that fails ANSI access falls straight through to the plugin and the image just
// fails to load, even though the folder/archive access above already works correctly.
typedef int (__stdcall *GetPictureInfoAM00FPtr)(LPSTR buf, long len, unsigned int flag, LPVOID lpInfo);
typedef int (__stdcall *GetPictureAM00FPtr)(LPSTR buf, long len, unsigned int flag, HANDLE *pHBInfo, HANDLE *pHBm, SPI_PROGRESS lpPrgressCallback, long lData);

// GetPictureInfo/GetPicture have the same export names in every Susie image
// plugin. Keep the original address with each returned wrapper instead of
// sharing one global trampoline; otherwise loading a second image plugin would
// make the first plugin call the wrong implementation.
enum ImagePluginFunction
{
	ImagePluginIsSupported,
	ImagePluginGetPictureInfo,
	ImagePluginGetPicture
};

struct ImagePluginCallContext
{
	ImagePluginFunction function;
	FARPROC original;
	char moduleName[MAX_PATH];
};

int __stdcall IsSupportedDispatchImpl(ImagePluginCallContext* context, LPSTR Filename, DWORD Dw)
{
	auto original = reinterpret_cast<IsSupportedAM00FPtr>(context->original);
	auto ret = original( Filename, Dw );
#if UNICODEHACK_PATH_DEBUG
	{
		FILE* lf = nullptr;
		if( fopen_s(&lf, Utility::GetPathDebugLogPath(), "a") == 0 && lf )
		{
			fprintf(lf, "IsSupported module=[%s] Filename=[%s] Dw=%lu ret=%d\n",
				context->moduleName, (Dw < 0x10000 && Filename) ? Filename : "<buffer-or-null>",
				(unsigned long)Dw, ret);
			fclose(lf);
		}
	}
#endif
	// Per the Susie SPI convention, Dw is either a small flag/handle value or -
	// when large enough to plausibly be one - a pointer to a header buffer the
	// caller already read itself; only the filename+flag case needs a path retry.
	if( !ret && Dw < 0x10000 && Filename && strchr(Filename, '?') )
	{
		auto retryPath = Utility::GetShortPath( Filename );
		if( !retryPath.empty() )
			ret = original( const_cast<LPSTR>(retryPath.c_str()), Dw );
	}
	return ret;
}

int __stdcall GetPictureInfoDispatchImpl(ImagePluginCallContext* context, LPSTR Buf, long Len, unsigned int Flag, LPVOID Inf)
{
	auto original = reinterpret_cast<GetPictureInfoAM00FPtr>(context->original);
	bool pathCall = !(Flag & 0x07) && Buf;
	bool needsAlias = pathCall && strchr(Buf, '?');
	// The normal case is already an ANSI-safe path. Avoid constructing any
	// temporary strings or doing fallback bookkeeping for every JPG/PNG item.
	if( !needsAlias )
	{
		return original( Buf, Len, Flag, Inf );
	}

	std::string retryPath;
	int originalResult = SPI_OTHER_ERROR;
	int ret = SPI_OTHER_ERROR;
	if( needsAlias )
	{
		retryPath = Utility::GetShortPath( Buf );
		if( !retryPath.empty() )
		{
			ret = original( const_cast<LPSTR>(retryPath.c_str()), Len, Flag, Inf );
		}
	}
	if( ret != SPI_ALL_RIGHT )
	{
		originalResult = original( Buf, Len, Flag, Inf );
		if( originalResult == SPI_ALL_RIGHT ) ret = originalResult;
	}
	return ret;
}

int __stdcall GetPictureDispatchImpl(ImagePluginCallContext* context, LPSTR Buf, long Len, unsigned int Flag,
	HANDLE *pHBInfo, HANDLE *pHBm, SPI_PROGRESS PrgressCallback, long Data)
{
	auto original = reinterpret_cast<GetPictureAM00FPtr>(context->original);
	bool pathCall = !(Flag & 0x07) && Buf;
	bool needsAlias = pathCall && strchr(Buf, '?');
	// Keep the hot path equivalent to the original direct plugin call. The
	// Unicode fallback is only needed when the ANSI path is actually damaged.
	if( !needsAlias )
	{
		return original( Buf, Len, Flag, pHBInfo, pHBm, PrgressCallback, Data );
	}

	std::string retryPath;
	int originalResult = SPI_OTHER_ERROR;
	int ret = SPI_OTHER_ERROR;
	if( needsAlias )
	{
		retryPath = Utility::GetShortPath( Buf );
		if( !retryPath.empty() )
		{
			ret = original( const_cast<LPSTR>(retryPath.c_str()), Len, Flag,
				pHBInfo, pHBm, PrgressCallback, Data );
		}
	}
	if( ret != SPI_ALL_RIGHT )
	{
		originalResult = original( Buf, Len, Flag, pHBInfo, pHBm, PrgressCallback, Data );
		if( originalResult == SPI_ALL_RIGHT ) ret = originalResult;
	}
	return ret;
}

// The project is intentionally built as Win32. These dispatchers preserve the
// original Susie stack layout, pass the wrapper context in EAX, and call the
// typed C++ implementation above. A separate executable thunk loads the
// context into EAX for every plugin/API pair.
__declspec(naked) int __stdcall IsSupportedDispatch()
{
	__asm
	{
		cmp dword ptr [esp+8], 10000h
		jae image_plugin_direct
		mov edx, [esp+4]
		test edx, edx
		jz image_plugin_direct
	image_plugin_scan:
		mov cl, [edx]
		test cl, cl
		jz image_plugin_direct
		cmp cl, '?'
		je image_plugin_alias
		inc edx
		jmp image_plugin_scan
	image_plugin_direct:
		mov edx, [eax+4]
		jmp edx
	image_plugin_alias:
		mov edx, esp
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call IsSupportedDispatchImpl
		ret 8
	}
}

__declspec(naked) int __stdcall GetPictureInfoDispatch()
{
	__asm
	{
		test dword ptr [esp+12], 7
		jnz image_info_direct
		mov edx, [esp+4]
		test edx, edx
		jz image_info_direct
	image_info_scan:
		mov cl, [edx]
		test cl, cl
		jz image_info_direct
		cmp cl, '?'
		je image_info_alias
		inc edx
		jmp image_info_scan
	image_info_direct:
		mov edx, [eax+4]
		jmp edx
	image_info_alias:
		mov edx, esp
		push dword ptr [edx+16]
		push dword ptr [edx+12]
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call GetPictureInfoDispatchImpl
		ret 16
	}
}

__declspec(naked) int __stdcall GetPictureDispatch()
{
	__asm
	{
		test dword ptr [esp+12], 7
		jnz image_picture_direct
		mov edx, [esp+4]
		test edx, edx
		jz image_picture_direct
	image_picture_scan:
		mov cl, [edx]
		test cl, cl
		jz image_picture_direct
		cmp cl, '?'
		je image_picture_alias
		inc edx
		jmp image_picture_scan
	image_picture_direct:
		mov edx, [eax+4]
		jmp edx
	image_picture_alias:
		mov edx, esp
		push dword ptr [edx+28]
		push dword ptr [edx+24]
		push dword ptr [edx+20]
		push dword ptr [edx+16]
		push dword ptr [edx+12]
		push dword ptr [edx+8]
		push dword ptr [edx+4]
		push eax
		call GetPictureDispatchImpl
		ret 28
	}
}

void* CreateImagePluginThunk(ImagePluginCallContext* context, void* dispatch)
{
	BYTE* code = reinterpret_cast<BYTE*>(::VirtualAlloc(nullptr, 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
	if( !code ) return nullptr;
	code[0] = 0xB8; // mov eax, immediate context
	*reinterpret_cast<DWORD*>(code + 1) = static_cast<DWORD>(reinterpret_cast<uintptr_t>(context));
	code[5] = 0xE9; // jmp dispatch
	intptr_t relative = reinterpret_cast<BYTE*>(dispatch) - (code + 10);
	if( relative < INT32_MIN || relative > INT32_MAX )
	{
		::VirtualFree(code, 0, MEM_RELEASE);
		return nullptr;
	}
	*reinterpret_cast<LONG*>(code + 6) = static_cast<LONG>(relative);
	::FlushInstructionCache(::GetCurrentProcess(), code, 10);
	return code;
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

bool IsTargetImagePlugin( HMODULE hModule, char* moduleName, size_t moduleNameSize )
{
	if( !hModule || !moduleName || moduleNameSize == 0 ) return false;
	moduleName[0] = '\0';
	if( !GetModuleFileNameA( hModule, moduleName, (DWORD)moduleNameSize ) ) return false;
	const char* fileName = strrchr( moduleName, '\\' );
	const char* slash = strrchr( moduleName, '/' );
	if( slash && (!fileName || slash > fileName) ) fileName = slash;
	if( fileName ) ++fileName;
	else fileName = moduleName;

	static const char* const targetNames[] =
	{
		"iftwebp.spi",
		"ifjpegx.spi",
		"ifpng.spi",
		"ifavif.spi",
		"wic_loader.spi",
		"axffmpeg.spi"
	};
	for( const auto targetName : targetNames )
		if( _stricmp(fileName, targetName) == 0 ) return true;
	return false;
}

bool IsWebpImagePlugin( const char* moduleName )
{
	if( !moduleName ) return false;
	const char* fileName = strrchr( moduleName, '\\' );
	const char* slash = strrchr( moduleName, '/' );
	if( slash && (!fileName || slash > fileName) ) fileName = slash;
	if( fileName ) ++fileName;
	else fileName = moduleName;
	return _stricmp(fileName, "iftwebp.spi") == 0;
}

void* GetImageDispatch( const char* procName )
{
	if( strcmp(procName, "IsSupported") == 0 ) return reinterpret_cast<void*>(&IsSupportedDispatch);
	if( strcmp(procName, "GetPictureInfo") == 0 ) return reinterpret_cast<void*>(&GetPictureInfoDispatch);
	if( strcmp(procName, "GetPicture") == 0 ) return reinterpret_cast<void*>(&GetPictureDispatch);
	return nullptr;
}

FARPROC WrapImagePluginFunction( HMODULE hModule, const char* procName, FARPROC original, const char* moduleName )
{
	void* dispatch = GetImageDispatch(procName);
	if( !dispatch || !original ) return original;

	auto* context = new ImagePluginCallContext();
	context->function = strcmp(procName, "IsSupported") == 0
		? ImagePluginIsSupported
		: (strcmp(procName, "GetPictureInfo") == 0 ? ImagePluginGetPictureInfo : ImagePluginGetPicture);
	context->original = original;
	strncpy_s(context->moduleName, sizeof(context->moduleName), moduleName ? moduleName : "", _TRUNCATE);
	(void)hModule;

	void* thunk = CreateImagePluginThunk(context, dispatch);
	return thunk ? reinterpret_cast<FARPROC>(thunk) : original;
}

FARPROC WINAPI GetProcAddressHook(    _In_ HMODULE hModule,    _In_ LPCSTR lpProcName 	)
{
		auto ret = originalGetProcAddress( hModule, lpProcName );
		char moduleName[MAX_PATH] = {};
		if( hModule ) GetModuleFileNameA( hModule, moduleName, sizeof(moduleName) );
		const bool isImageProc = lpProcName && (strcmp(lpProcName, "IsSupported") == 0
			|| strcmp(lpProcName, "GetPictureInfo") == 0
			|| strcmp(lpProcName, "GetPicture") == 0);
		const bool isTargetImagePlugin = isImageProc
			&& IsTargetImagePlugin( hModule, moduleName, sizeof(moduleName) );
		const bool isArchiveEntryProc = lpProcName && (strcmp(lpProcName, "GetArchiveInfo") == 0
			|| strcmp(lpProcName, "GetFileInfo") == 0
			|| strcmp(lpProcName, "GetFile") == 0);
		const char* moduleFileName = strrchr(moduleName, '\\');
		if( moduleFileName ) ++moduleFileName;
		else moduleFileName = moduleName;
		const char* moduleExtension = strrchr(moduleFileName, '.');
		const bool isKnownArchiveModule = _stricmp(moduleFileName, "arc.spi") == 0
			|| _stricmp(moduleFileName, "ax7z.spi") == 0
			|| _stricmp(moduleFileName, "axpdf.spi") == 0
			|| _stricmp(moduleFileName, "axffmpeg.spi") == 0;
		const bool isArchiveProbe = lpProcName && strcmp(lpProcName, "IsSupported") == 0
			&& isKnownArchiveModule;
		const bool isArchiveProc = isArchiveEntryProc || isArchiveProbe;
		const bool isArchivePlugin = isArchiveProc && moduleExtension
			&& _stricmp(moduleExtension, ".spi") == 0;
#if UNICODEHACK_PLUGIN_DEBUG
		if( isArchiveProc )
			LogPluginPath( "ArchiveProc", lpProcName, moduleName, ret ? 1 : 0, isArchivePlugin ? 1 : 0, moduleName );
#endif
#if UNICODEHACK_PLUGIN_DEBUG
		if( isTargetImagePlugin )
		{
			LogPluginPath( "GetProcAddress", lpProcName, "", ret ? 1 : 0, 0, moduleName );
		}
#endif
		if( !lpProcName ) return ret;
		if( ret && isArchivePlugin )
			return WrapArchivePluginFunction( hModule, lpProcName, ret, moduleName );
		// Return a per-plugin wrapper rather than patching all plugins to one shared
		// hook. Leeyes calls the pointer returned by GetProcAddress, so this keeps
		// each plugin's original function and supports all configured image plugins
		// concurrently.
		// IsSupported is called while Leeyes probes every file in a list. Keep that
		// hot path untouched for the newly added plugins; their GetPictureInfo and
		// GetPicture calls still receive the Unicode fallback when an actual image
		// path is damaged. WebP keeps its previously verified IsSupported fallback.
		const bool shouldWrapFunction = isTargetImagePlugin
			&& (strcmp(lpProcName, "IsSupported") != 0 || IsWebpImagePlugin(moduleName));
		if( ret && shouldWrapFunction )
			return WrapImagePluginFunction( hModule, lpProcName, ret, moduleName );
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
		gInjectedModule = hModule;
		// install first, before anything else has a chance to trigger p9np's load
		Kernel32::hookLoadLibrary();
		Kernel32::hookLdrLoadDll();
		Kernel32::hookRegQueryValueExW();
		Kernel32::hookPathConversions();
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
			Kernel32::hookGetFileAttributesW();
			Kernel32::hookGetFileAttributesExW();
			Kernel32::hookFileOperations();
		}
		if( Profile->Get("Option","HookSetWindowText",UINT() ) )
		{
			User32::hookSetWindowTextA();
		}
		User32::hookTextDrawing();
		//�p�X�Ɖ摜�t�@�C������Unicode�Ȃ炱�ꂾ���ł����邪���Ƀt�@�C�����ʖ�
		Shell32::hookSHBindToParent();
		//���Ƀv���O�C�����t�b�N���邱�ƂŃv���O�C���{�̘M�炸�ɑΉ�
		SusieAM00::hookGetProcAddress();
		if( Profile->Get("Option","HookCreateMutex",UINT() ) )
		{//���d�N��������
			Kernel32::hookCreateMutexA();
		}
		Shell32::hookDragQueryFileW();
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

