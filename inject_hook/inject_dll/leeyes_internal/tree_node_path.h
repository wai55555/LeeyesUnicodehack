#pragma once

#include <Windows.h>
#include <cstddef>
#include <string>

namespace LeeyesInternal
{

struct TreeNodePathIdentity
{
	DWORD data = 0;
	DWORD vmt = 0;
	DWORD parent = 0;
	DWORD cachedPath = 0;
	DWORD pathLength = 0;
	DWORD pathHash = 0;
	// Leeyes fills a node's cached path only once it has been expanded or
	// refreshed. Unpopulated caches are reported as pathKnown=false instead of
	// failing the whole comparison.
	bool pathKnown = false;
};

bool EncodeTreePathExact(UINT codePage, const std::wstring& path, std::string& encoded,
	size_t maximumBytes);
bool DecodeTreePathExact(UINT codePage, const char* path, size_t length, std::wstring& decoded);
bool ReadDelphiAnsiTreePath(UINT codePage, DWORD pathPointer, std::wstring& decoded);
bool CachedTreePathMatches(UINT codePage, DWORD cachedPath, const std::wstring& expected);
bool ReadTreeNodePathIdentity(DWORD node, TreeNodePathIdentity& identity,
	bool allowMissingPath = false);
// With allowCollapsed the search also accepts a node that is not expanded and
// reports its expanded state, so a caller can decide whether the parent can be
// batched in the flat-folder mode.
bool FindExpandedTreeNodeByPath(HWND window, UINT codePage, const std::wstring& path,
	DWORD timeBudgetMs, unsigned maximumNodes, HTREEITEM& item, DWORD& node,
	bool allowCollapsed = false, bool* expandedState = nullptr);
// Finds a node whose path cache is not populated yet by locating the closest
// ancestor with a known path and descending through exact child item names.
bool FindTreeNodeByPathComponents(HWND window, UINT codePage, const std::wstring& path,
	DWORD timeBudgetMs, unsigned maximumNodes, HTREEITEM& item, DWORD& node,
	bool& expandedState);
// TVM_GETITEMW may report the label by pointing the returned pszText at the
// control's own buffer instead of copying it into the array the caller supplied.
// Read the reported label back into that array so every caller keeps one read
// path and one length rule; a label that does not fit is rejected instead of
// being truncated.
bool TreeItemTextReadback(TVITEMW& item, wchar_t* buffer, size_t capacity);
bool TreeOwnerContainsWindow(DWORD owner, HWND window);
bool FindTreeViewWindow(DWORD owner, DWORD threadId, HWND& window);

}
