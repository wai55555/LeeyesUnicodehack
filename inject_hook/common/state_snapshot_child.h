#pragma once

#include "state_snapshot_wire.h"
#include <new>

namespace SnapshotWire
{
inline bool WriteAll(HANDLE pipe, const void* data, size_t length)
{
	const BYTE* cursor = static_cast<const BYTE*>(data);
	while( length )
	{
		const DWORD chunk = static_cast<DWORD>(length > 65536 ? 65536 : length);
		DWORD written = 0;
		if( !::WriteFile(pipe, cursor, chunk, &written, nullptr) || !written ) return false;
		cursor += written; length -= written;
	}
	return true;
}

// Only the explicitly passed pipe is used. Parent owns timeout and validation.
inline int RunChild(HANDLE pipe, const wchar_t* path)
{
	if( !pipe || pipe == INVALID_HANDLE_VALUE || ::GetFileType(pipe) != FILE_TYPE_PIPE || !path ) return 2;
	DWORD status = ERROR_SUCCESS, length = 0;
	std::vector<BYTE> data;
	try
	{
		DirectorySnapshot::State state;
		if( !DirectorySnapshot::Read(path, state, status, kMaxEntries) )
		{
			if( status == ERROR_SUCCESS ) status = ERROR_GEN_FAILURE;
		}
		else
		{
			#if defined(UNICODEHACK_SWEEP_DIAGNOSTICS) && UNICODEHACK_SWEEP_DIAGNOSTICS
			wchar_t signalPath[MAX_PATH * 4] = {}, delayText[16] = {};
			const DWORD signalLength = ::GetEnvironmentVariableW(L"LEYEES_STATE_TEST_READY_FILE", signalPath, _countof(signalPath));
			const DWORD delayLength = ::GetEnvironmentVariableW(L"LEYEES_STATE_TEST_AFTER_READ_MS", delayText, _countof(delayText));
			if( signalLength && signalLength < _countof(signalPath) )
			{
				DirectorySnapshot::Handle signal(::CreateFileW(signalPath, FILE_APPEND_DATA,
					FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
				if( signal.value != INVALID_HANDLE_VALUE ) { const BYTE marker = 1; DWORD written = 0; ::WriteFile(signal.value, &marker, 1, &written, nullptr); }
			}
			if( delayLength && delayLength < _countof(delayText) )
			{
				const unsigned long delay = wcstoul(delayText, nullptr, 10);
				if( delay > 0 && delay <= 5000 ) ::Sleep(delay);
			}
			#endif
			if( !Encode(state, data) ) status = ERROR_BUFFER_OVERFLOW;
			else length = static_cast<DWORD>(data.size());
		}
	}
	catch( const std::bad_alloc& ) { status = ERROR_NOT_ENOUGH_MEMORY; data.clear(); }
	catch( ... ) { status = ERROR_GEN_FAILURE; data.clear(); }
	if( status != ERROR_SUCCESS ) length = 0;
	if( !WriteAll(pipe, &status, sizeof(status)) || !WriteAll(pipe, &length, sizeof(length))
		|| (length && !WriteAll(pipe, data.data(), length)) ) return 3;
	return 0;
}
}
