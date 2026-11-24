#pragma once

#include <windows.h>
#include <cwchar>

// Used for comparing a Shell path to the original ANSI notification path.
// A successful conversion must preserve every UTF-16 code unit; merely
// reporting that no default character was used is not sufficient.
inline bool ExactAnsiPath(UINT codePage, const wchar_t* path, char* output, int capacity)
{
	if( !output || capacity <= 0 ) return false;
	output[0] = '\0';
	if( !path || !*path ) return false;
	BOOL usedDefault = FALSE;
	const int bytes = ::WideCharToMultiByte(codePage, WC_NO_BEST_FIT_CHARS, path, -1,
		output, capacity, nullptr, &usedDefault);
	if( !bytes || usedDefault ) { output[0] = '\0'; return false; }
	wchar_t restored[MAX_PATH] = {};
	const int units = ::MultiByteToWideChar(codePage, 0, output, bytes, restored, _countof(restored));
	if( !units || wcscmp(path, restored) != 0 ) { output[0] = '\0'; return false; }
	return true;
}
