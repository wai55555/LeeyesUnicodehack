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
bool FindExpandedTreeNodeByPath(HWND window, UINT codePage, const std::wstring& path,
	DWORD timeBudgetMs, unsigned maximumNodes, HTREEITEM& item, DWORD& node);
bool TreeOwnerContainsWindow(DWORD owner, HWND window);
bool FindTreeViewWindow(DWORD owner, DWORD threadId, HWND& window);

}
