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
unsigned long long gTreePathSearchKnownPaths = 0;
unsigned long long gTreePathSearchBestPrefix = 0;
unsigned long long gTreePathSearchPathNodes = 0;
unsigned long long gTreePathSearchItemFailures = 0;
bool gTreePathSearchVerifyFailed = false;

namespace
{

// Leeyes talks to the ANSI APIs, so CP_ACP is the process code page. Resolve it
// explicitly: with the UTF-8 system option (ACP 65001), WC_NO_BEST_FIT_CHARS is
// not valid and every conversion would fail, disabling the tree batching.
UINT TreePathCodePage(UINT codePage)
{
	return codePage == CP_ACP ? ::GetACP() : codePage;
}

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
	codePage = TreePathCodePage(codePage);
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
	codePage = TreePathCodePage(codePage);
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

namespace
{

bool TreePathPrefix(const std::wstring& prefix, const std::wstring& path)
{
	if( prefix.empty() || path.size() <= prefix.size() ) return false;
	if( ::CompareStringOrdinal(prefix.data(), static_cast<int>(prefix.size()),
		path.data(), static_cast<int>(prefix.size()), TRUE) != CSTR_EQUAL ) return false;
	return path[prefix.size()] == L'\\' || path[prefix.size()] == L'/';
}

std::wstring TreePathLeaf(const std::wstring& path)
{
	const size_t separator = path.find_last_of(L"\\/");
	return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

}

bool TreeItemTextReadback(TVITEMW& item, wchar_t* buffer, size_t capacity)
{
	if( !buffer || capacity < 2 ) return false;
	// A null return means the control wrote into the array it was given; a
	// non-null one may point at the control's own copy.
	const wchar_t* reported = item.pszText ? item.pszText : buffer;
	if( reported == buffer ) return true;
	__try
	{
		// Bound the read by the caller's capacity: the length check the callers
		// run afterwards rejects a label that fills the buffer, and it needs a
		// terminated string to stay meaningful.
		const size_t length = wcsnlen_s(reported, capacity);
		if( length >= capacity ) return false;
		memcpy(buffer, reported, (length + 1) * sizeof(wchar_t));
	}
	__except( EXCEPTION_EXECUTE_HANDLER ) { return false; }
	return true;
}

namespace
{

bool TreeItemText(HWND window, HTREEITEM item, std::wstring& text)
{
	wchar_t label[1024] = {};
	TVITEMW data = {};
	data.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
	data.stateMask = TVIS_EXPANDED;
	data.hItem = item;
	data.pszText = label;
	data.cchTextMax = static_cast<int>(_countof(label));
	if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&data)) ) return false;
	if( !TreeItemTextReadback(data, label, _countof(label)) ) return false;
	if( !label[0] || wcsnlen_s(label, _countof(label)) == _countof(label) - 1 || !data.lParam ) return false;
	try { text.assign(label); }
	catch( ... ) { return false; }
	return true;
}

}

bool FindTreeNodeByPathComponents(HWND window, UINT codePage, const std::wstring& path,
	DWORD timeBudgetMs, unsigned maximumNodes, HTREEITEM& item, DWORD& node,
	bool& expandedState)
{
	item = nullptr;
	node = 0;
	expandedState = false;
	if( !window || !::IsWindow(window) || !timeBudgetMs || !maximumNodes || path.empty() ) return false;
	try
	{
		// 1) Find the closest ancestor whose cached path is a prefix of the target.
		gTreePathSearchKnownPaths = 0;
		gTreePathSearchBestPrefix = 0;
		std::wstring bestPrefix;
		HTREEITEM bestItem = nullptr;
		DWORD bestNode = 0;
		std::vector<HTREEITEM> pending;
		HTREEITEM root = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM, TVGN_ROOT, 0));
		if( !root ) return false;
		pending.push_back(root);
		const ULONGLONG deadline = ::GetTickCount64() + timeBudgetMs;
		unsigned visited = 0;
		while( !pending.empty() )
		{
			if( ++visited > maximumNodes || ::GetTickCount64() >= deadline ) return false;
			const HTREEITEM current = pending.back();
			pending.pop_back();
			TVITEMW state = {};
			state.mask = TVIF_PARAM | TVIF_STATE;
			state.stateMask = TVIS_EXPANDED;
			state.hItem = current;
			if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&state)) ) return false;
			if( state.lParam )
			{
				TreeNodePathIdentity identity = {};
				if( ReadTreeNodePathIdentity(static_cast<DWORD>(state.lParam), identity, true)
					&& identity.pathKnown )
				{
					std::wstring decoded;
					if( ReadDelphiAnsiTreePath(CP_ACP, identity.cachedPath, decoded) )
					{
						++gTreePathSearchKnownPaths;
						if( decoded.size() == path.size()
							&& ::CompareStringOrdinal(decoded.data(), static_cast<int>(decoded.size()),
								path.data(), static_cast<int>(path.size()), TRUE) == CSTR_EQUAL )
						{
							item = current;
							node = static_cast<DWORD>(state.lParam);
							expandedState = (state.state & TVIS_EXPANDED) != 0;
							return true;
						}
						if( decoded.size() < path.size() && decoded.size() > bestPrefix.size()
							&& TreePathPrefix(decoded, path) )
						{
							bestPrefix.swap(decoded);
							bestItem = current;
							bestNode = static_cast<DWORD>(state.lParam);
							gTreePathSearchBestPrefix = static_cast<unsigned long long>(bestPrefix.size());
						}
					}
				}
			}
			HTREEITEM sibling = reinterpret_cast<HTREEITEM>(::SendMessageW(window,
				TVM_GETNEXTITEM, TVGN_NEXT, reinterpret_cast<LPARAM>(current)));
			if( sibling ) pending.push_back(sibling);
			HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window,
				TVM_GETNEXTITEM, TVGN_CHILD, reinterpret_cast<LPARAM>(current)));
			if( child ) pending.push_back(child);
		}
		if( !bestItem ) return false;

		// 2) Descend through the remaining components by exact child item names.
		std::wstring remaining = path.substr(bestPrefix.size());
		size_t offset = 0;
		while( offset < remaining.size() && (remaining[offset] == L'\\' || remaining[offset] == L'/') ) ++offset;
		HTREEITEM current = bestItem;
		DWORD currentNode = bestNode;
		while( offset < remaining.size() )
		{
			const size_t end = remaining.find_first_of(L"\\/", offset);
			const std::wstring component = remaining.substr(offset, end == std::wstring::npos ? std::wstring::npos : end - offset);
			if( component.empty() ) break;
			HTREEITEM match = nullptr;
			DWORD matchNode = 0;
			for( HTREEITEM child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
				TVGN_CHILD, reinterpret_cast<LPARAM>(current))); child; )
			{
				if( ++visited > maximumNodes || ::GetTickCount64() >= deadline ) return false;
				std::wstring text;
				if( !TreeItemText(window, child, text) ) return false;
				if( ::CompareStringOrdinal(text.data(), static_cast<int>(text.size()),
					component.data(), static_cast<int>(component.size()), TRUE) == CSTR_EQUAL )
				{
					if( match ) return false; // duplicate child name, ambiguous
					match = child;
					TVITEMW value = {};
					value.mask = TVIF_PARAM;
					value.hItem = child;
					if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&value)) ) return false;
					matchNode = static_cast<DWORD>(value.lParam);
				}
				child = reinterpret_cast<HTREEITEM>(::SendMessageW(window, TVM_GETNEXTITEM,
					TVGN_NEXT, reinterpret_cast<LPARAM>(child)));
			}
			if( !match || !matchNode ) return false;
			current = match;
			currentNode = matchNode;
			if( end == std::wstring::npos ) break;
			offset = end + 1;
		}
		TVITEMW verify = {};
		verify.mask = TVIF_PARAM | TVIF_STATE;
		verify.stateMask = TVIS_EXPANDED;
		verify.hItem = current;
		if( !::SendMessageW(window, TVM_GETITEMW, 0, reinterpret_cast<LPARAM>(&verify))
			|| static_cast<DWORD>(verify.lParam) != currentNode ) return false;
		TreeNodePathIdentity identity = {};
		if( ReadTreeNodePathIdentity(currentNode, identity, true) && identity.pathKnown )
		{
			std::wstring decoded;
			if( !ReadDelphiAnsiTreePath(CP_ACP, identity.cachedPath, decoded)
				|| ::CompareStringOrdinal(decoded.data(), static_cast<int>(decoded.size()),
					path.data(), static_cast<int>(path.size()), TRUE) != CSTR_EQUAL ) return false;
		}
		item = current;
		node = currentNode;
		expandedState = (verify.state & TVIS_EXPANDED) != 0;
		return true;
	}
	catch( ... ) { item = nullptr; node = 0; expandedState = false; return false; }
}

bool FindExpandedTreeNodeByPath(HWND window, UINT codePage, const std::wstring& path,
	DWORD timeBudgetMs, unsigned maximumNodes, HTREEITEM& item, DWORD& node,
	bool allowCollapsed, bool* expandedState)
{
	if( expandedState ) *expandedState = false;
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
			|| static_cast<DWORD>(verify.lParam) != matchNode
			|| (!allowCollapsed && !(verify.state & TVIS_EXPANDED))
			|| !ReadTreeNodePathIdentity(matchNode, verified, true)
			|| !CachedTreePathMatches(codePage, verified.cachedPath, path) )
		{
			gTreePathSearchVerifyFailed = true;
			return false;
		}
		if( expandedState ) *expandedState = (verify.state & TVIS_EXPANDED) != 0;
		item = match;
		node = matchNode;
		return true;
	}
	catch( ... ) { item = nullptr; node = 0; return false; }
}

}
