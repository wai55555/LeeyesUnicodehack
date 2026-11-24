#pragma once

#include <windows.h>
#include <cwchar>

namespace NotificationScope
{
// Lexical comparison is a diagnostic hint only: junctions, aliases, 8.3 paths,
// case-sensitive directories and watcher roots can refer to the same object
// through different names. Never drop notifications solely from Disjoint.
enum class Relation { Related, LexicallyDisjoint, Unknown };
inline bool ValidAbsolute(const wchar_t* path, size_t& length)
{
	if( !path ) return false;
	length = std::wcslen(path);
	if( length < 3 || length > 1024 || path[1] != L':' || path[2] != L'\\'
		|| !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ) return false;
	for( size_t i = 3; i < length; ++i )
	{
		if( path[i] == L'/' || path[i] == L':' ) return false;
		if( path[i] == L'\\' && path[i - 1] == L'\\' ) return false;
		if( (i == 3 || path[i - 1] == L'\\') && path[i] == L'.'
			&& (i + 1 == length || path[i + 1] == L'\\'
				|| (path[i + 1] == L'.' && (i + 2 == length || path[i + 2] == L'\\'))) ) return false;
	}
	while( length > 3 && path[length - 1] == L'\\' ) --length;
	return true;
}
inline Relation Compare(const wchar_t* target, const wchar_t* path)
{
	size_t targetLength = 0, pathLength = 0;
	if( !ValidAbsolute(target, targetLength) || !ValidAbsolute(path, pathLength) ) return Relation::Unknown;
	const size_t shorter = targetLength < pathLength ? targetLength : pathLength;
	const int result = ::CompareStringOrdinal(target, static_cast<int>(shorter),
		path, static_cast<int>(shorter), TRUE);
	if( !result ) return Relation::Unknown;
	if( result != CSTR_EQUAL ) return Relation::LexicallyDisjoint;
	if( targetLength == pathLength || shorter == 3
		|| (targetLength < pathLength && path[shorter] == L'\\')
		|| (pathLength < targetLength && target[shorter] == L'\\') ) return Relation::Related;
	return Relation::LexicallyDisjoint;
}
}
