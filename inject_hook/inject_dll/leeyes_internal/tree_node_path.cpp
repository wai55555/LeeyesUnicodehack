#include "../stdafx.h"
#include "tree_node_path.h"
#include "tree_layout_guard.h"

#include <commctrl.h>
#include <climits>
#include <cstring>
#include <vector>

namespace LeeyesInternal
{
// Latest path-search shape, for diagnostics only.
unsigned long long gTreePathSearchVisited = 0;
unsigned long long gTreePathSearchPathNodes = 0;
unsigned long long gTreePathSearchItemFailures = 0;
bool gTreePathSearchVerifyFailed = false;

namespace
{

bool CopyAnsiString(DWORD pointer, char* output, size_t capacity, size_t& length)
{
	length = 0;
	if( !pointer || !output || capacity < 2 || pointer < sizeof(DWORD) ) return false;
	__try
	{
		const DWORD storedLength = *reinterpret_cast<const DWORD*>(pointer - sizeof(DWORD));
		if( storedLength == 0 || storedLength >= capacity ) return false;
		const char* source = reinterpret_cast<const char*>(pointer);
		if( memchr(source, '\0', storedLength) ) return false;
		memcpy(output, source, storedLength);
		output[storedLength] = '\0';
		length = storedLength;
		return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}

bool ReadNodeDataAndPath(DWORD node, DWORD* dataOut, DWORD* vmtOut,
	DWORD* parentOut, DWORD* pathOut)
{
	if( !dataOut || !vmtOut || !parentOut || !pathOut
		|| node < 0x10000 || node > MAXDWORD - 0x0F ) return false;
	__try
	{
		const DWORD data = *reinterpret_cast<const DWORD*>(node + 0x0C);
		if( !data || data > MAXDWORD - 14 * sizeof(DWORD) - sizeof(DWORD) + 1 ) return false;
		const DWORD* words = reinterpret_cast<const DWORD*>(data);
		*dataOut = data;
		*vmtOut = words[0];
		*parentOut = words[4];
		*pathOut = words[14];
		return *pathOut != 0;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
}

struct TreeWindowSearch
{
	DWORD owner;
	HWND window;
	unsigned count;
};

BOOL CALLBACK FindTreeViewChild(HWND window, LPARAM parameter)
{
	TreeWindowSearch* search = reinterpret_cast<TreeWindowSearch*>(parameter);
	char className[64] = {};
	if( ::IsWindowVisible(window) && ::GetClassNameA(window, className, _countof(className))
		&& strcmp(className, "TAcvTreeView") == 0
		&& TreeOwnerContainsWindow(search->owner, window) )
	{
		search->window = window;
		++search->count;
	}
	return TRUE;
}

BOOL CALLBACK FindTreeViewTop(HWND window, LPARAM parameter)
{
	::EnumChildWindows(window, FindTreeViewChild, parameter);
	return TRUE;
}

}

bool EncodeTreePathExact(UINT codePage, const std::wstring& path, std::string& encoded,
	size_t maximumBytes)
{
	if( path.empty() || path.size() > INT_MAX || maximumBytes == 0 ) return false;
	const bool utf8 = codePage == CP_UTF8;
	BOOL usedDefault = FALSE;
	const int bytes = ::WideCharToMultiByte(codePage, utf8 ? 0 : WC_NO_BEST_FIT_CHARS,
		path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, utf8 ? nullptr : &usedDefault);
	if( bytes <= 0 || usedDefault || static_cast<size_t>(bytes) > maximumBytes ) return false;
	try { encoded.assign(static_cast<size_t>(bytes), '\0'); }
	catch( ... ) { return false; }
	usedDefault = FALSE;
	if( ::WideCharToMultiByte(codePage, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, path.data(),
		static_cast<int>(path.size()), &encoded[0], bytes, nullptr, utf8 ? nullptr : &usedDefault) != bytes
		|| usedDefault ) return false;
	const int units = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
		encoded.data(), bytes, nullptr, 0);
	if( units <= 0 ) return false;
	try
	{
		std::wstring roundTrip(static_cast<size_t>(units), L'\0');
		return ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, encoded.data(), bytes,
			&roundTrip[0], units) == units && roundTrip == path;
	}
	catch( ... ) { return false; }
}

bool DecodeTreePathExact(UINT codePage, const char* path, size_t length, std::wstring& decoded)
{
	if( !path || length == 0 || length > INT_MAX ) return false;
	if( memchr(path, '\0', length) ) return false;
	const int units = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS,
		path, static_cast<int>(length), nullptr, 0);
	if( units <= 0 ) return false;
	try { decoded.assign(static_cast<size_t>(units), L'\0'); }
	catch( ... ) { return false; }
	if( ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, path,
		static_cast<int>(length), &decoded[0], units) != units ) return false;
	std::string encoded;
	return EncodeTreePathExact(codePage, decoded, encoded, length)
		&& encoded.size() == length && memcmp(encoded.data(), path, length) == 0;
}

bool ReadDelphiAnsiTreePath(UINT codePage, DWORD pathPointer, std::wstring& decoded)
{
	char bytes[1040] = {};
	size_t length = 0;
	return CopyAnsiString(pathPointer, bytes, _countof(bytes), length)
		&& DecodeTreePathExact(codePage, bytes, length, decoded);
}

bool CachedTreePathMatches(UINT codePage, DWORD cachedPath, const std::wstring& expected)
{
	if( !VerifyLeeyesTreeLayout() ) return false;
	char bytes[1040] = {};
	size_t length = 0;
	std::wstring decoded;
	return CopyAnsiString(cachedPath, bytes, _countof(bytes), length)
		&& DecodeTreePathExact(codePage, bytes, length, decoded)
		&& ::CompareStringOrdinal(decoded.data(), static_cast<int>(decoded.size()),
			expected.data(), static_cast<int>(expected.size()), TRUE) == CSTR_EQUAL;
}

bool ReadTreeNodePathIdentity(DWORD node, TreeNodePathIdentity& identity, bool allowMissingPath)
{
	if( !VerifyLeeyesTreeLayout() ) return false;
	TreeNodePathIdentity next = {};
	// ReadNodeDataAndPath requires a non-zero path, so read the node data here
	// when a missing cache is acceptable.
	if( allowMissingPath )
	{
		if( node < 0x10000 || node > MAXDWORD - 0x0F ) return false;
		__try
		{
			next.data = *reinterpret_cast<const DWORD*>(node + 0x0C);
			if( !next.data || next.data > MAXDWORD - 14 * sizeof(DWORD) ) return false;
			const DWORD* words = reinterpret_cast<const DWORD*>(next.data);
			next.vmt = words[0];
			next.parent = words[4];
			next.cachedPath = words[14];
		}
		__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
		if( !next.cachedPath )
		{
			next.pathKnown = false;
			next.pathLength = 0;
			next.pathHash = 0;
			identity = next;
			return true;
		}
	}
	else if( !ReadNodeDataAndPath(node, &next.data, &next.vmt, &next.parent, &next.cachedPath) ) return false;
	char bytes[1040] = {};
	size_t length = 0;
	if( !CopyAnsiString(next.cachedPath, bytes, _countof(bytes), length) ) return false;
	next.pathLength = static_cast<DWORD>(length);
	next.pathHash = 2166136261u;
	for( size_t i = 0; i < length; ++i )
		next.pathHash = (next.pathHash ^ static_cast<unsigned char>(bytes[i])) * 16777619u;
	next.pathKnown = true;
	identity = next;
	return true;
}

bool TreeOwnerContainsWindow(DWORD owner, HWND window)
{
	if( !VerifyLeeyesTreeLayout() || !owner || !window ) return false;
	__try
	{
		const DWORD* words = reinterpret_cast<const DWORD*>(owner);
		for( unsigned i = 0; i < 0x1000 / sizeof(DWORD); ++i )
			if( words[i] == reinterpret_cast<DWORD>(window) ) return true;
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	return false;
}

bool FindTreeViewWindow(DWORD owner, DWORD threadId, HWND& window)
{
	window = nullptr;
	if( !VerifyLeeyesTreeLayout() || !owner || !threadId ) return false;
	TreeWindowSearch search = { owner, nullptr, 0 };
	::EnumThreadWindows(threadId, FindTreeViewTop, reinterpret_cast<LPARAM>(&search));
	if( search.count != 1 ) return false;
	window = search.window;
	return ::IsWindow(window) != FALSE;
}

bool FindExpandedTreeNodeByPath(HWND window, UINT codePage, const std::wstring& path,
	DWORD timeBudgetMs, unsigned maximumNodes, HTREEITEM& item, DWORD& node)
{
	item = nullptr;
	node = 0;
	gTreePathSearchVisited = 0;
	gTreePathSearchPathNodes = 0;
	gTreePathSearchItemFailures = 0;
	gTreePathSearchVerifyFailed = false;
	std::string encoded;
	if( !window || !::IsWindow(window) || !timeBudgetMs || !maximumNodes
		|| !EncodeTreePathExact(codePage, path, encoded, 1039) ) return false;
	try
	{
		std::vector<HTREEITEM> pending;
		HTREEITEM root = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_ROOT, 0));
		if( !root ) return false;
		pending.push_back(root);
		HTREEITEM match = nullptr;
		DWORD matchNode = 0;
		const ULONGLONG deadline = ::GetTickCount64() + timeBudgetMs;
		unsigned visited = 0;
		while( !pending.empty() )
		{
			gTreePathSearchVisited = visited + 1;
			if( ++visited > maximumNodes || ::GetTickCount64() >= deadline ) return false;
			const HTREEITEM current = pending.back();
			pending.pop_back();
			TVITEMW state = {};
			state.mask = TVIF_PARAM | TVIF_STATE;
			state.stateMask = TVIS_EXPANDED;
			state.hItem = current;
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&state)) )
			{
				++gTreePathSearchItemFailures;
				return false;
			}
			if( state.lParam ) ++gTreePathSearchPathNodes;
			// state.lParam is the node pointer; the cached path hangs off it.
			TreeNodePathIdentity candidate = {};
			if( state.lParam && ReadTreeNodePathIdentity(static_cast<DWORD>(state.lParam), candidate)
				&& CachedTreePathMatches(codePage, candidate.cachedPath, path) )
			{
				if( match ) return false;
				match = current;
				matchNode = static_cast<DWORD>(state.lParam);
			}
			HTREEITEM sibling = reinterpret_cast<HTREEITEM>(::SendMessageW(window,
				TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(current)));
			if( sibling ) pending.push_back(sibling);
			if( state.state & TVIS_EXPANDED )
			{
				HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window,
					TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(current)));
				if( child ) pending.push_back(child);
			}
		}
		if( !match ) return false;
		TVITEMW verify = {};
		verify.mask = TVIF_PARAM | TVIF_STATE;
		verify.stateMask = TVIS_EXPANDED;
		verify.hItem = match;
		TreeNodePathIdentity verified = {};
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&verify))
			|| static_cast<DWORD>(verify.lParam) != matchNode || !(verify.state & TVIS_EXPANDED)
			|| !ReadTreeNodePathIdentity(matchNode, verified)
			|| !CachedTreePathMatches(codePage, verified.cachedPath, path) )
		{
			gTreePathSearchVerifyFailed = true;
			return false;
		}
		item = match;
		node = matchNode;
		return true;
	}
	catch( ... ) { item = nullptr; node = 0; return false; }
}

}
