#pragma once

#include "directory_snapshot.h"
#include <cstring>
#include <limits>

// Internal, versioned protocol between the launcher and a background reader.
// Input is untrusted: do not publish a partially decoded state on failure.
namespace SnapshotWire
{
const DWORD kMagic = 0x31534c4c; // LLS1, little endian
const size_t kMaxBytes = 64u * 1024u * 1024u;
const DWORD kMaxEntries = 200000;
const DWORD kMaxNameUnits = 255;

inline bool Put(std::vector<BYTE>& data, const void* source, size_t length)
{
	if( length > kMaxBytes - data.size() ) return false;
	const BYTE* bytes = static_cast<const BYTE*>(source);
	data.insert(data.end(), bytes, bytes + length);
	return true;
}
template<class T> inline bool Put(std::vector<BYTE>& data, const T& value)
{ return Put(data, &value, sizeof(value)); }

inline bool Encode(const DirectorySnapshot::State& state, std::vector<BYTE>& output)
{
	static_assert(sizeof(wchar_t) == 2, "wire protocol requires UTF-16 wchar_t");
	if( !state.directoryId || state.entries.size() > kMaxEntries ) return false;
	std::vector<BYTE> data;
	const DWORD count = static_cast<DWORD>(state.entries.size());
	if( !Put(data, kMagic) || !Put(data, state.volume) || !Put(data, state.directoryId)
		|| !Put(data, count) ) return false;
	for( size_t i = 0; i < state.entries.size(); ++i )
	{
		const DirectorySnapshot::Entry& e = state.entries[i];
		if( e.name.empty() || e.name.size() > kMaxNameUnits || e.name.find(L'\0') != std::wstring::npos
			|| e.name.find_first_of(L"\\/") != std::wstring::npos || e.name == L"." || e.name == L".."
			|| (i && !(state.entries[i-1].name < e.name)) ) return false;
		const DWORD length = static_cast<DWORD>(e.name.size());
		if( !Put(data, length) || !Put(data, e.attributes) || !Put(data, e.size)
			|| !Put(data, e.created) || !Put(data, e.written)
			|| !Put(data, e.name.data(), length * sizeof(wchar_t)) ) return false;
	}
	output.swap(data);
	return true;
}

template<class T> inline bool Get(const std::vector<BYTE>& data, size_t& offset, T& out)
{
	if( offset > data.size() || sizeof(T) > data.size() - offset ) return false;
	std::memcpy(&out, data.data() + offset, sizeof(T));
	offset += sizeof(T); return true;
}
inline bool Decode(const std::vector<BYTE>& data, DirectorySnapshot::State& output)
{
	static_assert(sizeof(wchar_t) == 2, "wire protocol requires UTF-16 wchar_t");
	if( data.size() > kMaxBytes ) return false;
	size_t offset = 0;
	DWORD magic = 0, count = 0;
	DirectorySnapshot::State state;
	if( !Get(data, offset, magic) || magic != kMagic || !Get(data, offset, state.volume)
		|| !Get(data, offset, state.directoryId) || !state.directoryId
		|| !Get(data, offset, count) || count > kMaxEntries ) return false;
	// At least 34 bytes are needed per entry, including one UTF-16 code unit.
	if( count > (data.size() - offset) / 34 ) return false;
	state.entries.reserve(count);
	for( DWORD i = 0; i < count; ++i )
	{
		DWORD length = 0;
		DirectorySnapshot::Entry e = {};
		if( !Get(data, offset, length) || !length || length > kMaxNameUnits
			|| !Get(data, offset, e.attributes) || !Get(data, offset, e.size)
			|| !Get(data, offset, e.created) || !Get(data, offset, e.written)
			|| length * sizeof(wchar_t) > data.size() - offset ) return false;
		e.name.resize(length);
		std::memcpy(&e.name[0], data.data() + offset, length * sizeof(wchar_t));
		offset += length * sizeof(wchar_t);
		if( e.name.find(L'\0') != std::wstring::npos || e.name.find_first_of(L"\\/") != std::wstring::npos
			|| e.name == L"." || e.name == L".."
			|| (!state.entries.empty() && !(state.entries.back().name < e.name)) ) return false;
		if( e.attributes & FILE_ATTRIBUTE_REPARSE_POINT ) state.containsReparsePoint = true;
		state.entries.push_back(std::move(e));
	}
	if( offset != data.size() ) return false;
	output = std::move(state);
	return true;
}
}
