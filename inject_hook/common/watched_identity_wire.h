#pragma once

#include "state_snapshot_wire.h"
#include "directory_entry_identity.h"

// Optional envelope for a watched snapshot and its entry identities. Entry
// names are carried once in the validated snapshot; IDs follow in that exact
// order. The old snapshot format remains valid when identity collection is off.
namespace WatchedIdentityWire
{
const DWORD kMagic = 0x3149574c; // LWI1

inline bool Encode(const DirectorySnapshot::State& state, const DirectoryEntryIdentity::State& identities,
	std::vector<BYTE>& output)
{
	if( state.volume != identities.volume || state.directoryId != identities.directoryId
		|| state.entries.size() != identities.items.size() ) return false;
	std::vector<BYTE> snapshot;
	if( !SnapshotWire::Encode(state, snapshot) || snapshot.size() > MAXDWORD ) return false;
	std::vector<BYTE> data;
	const DWORD size = static_cast<DWORD>(snapshot.size());
	if( !SnapshotWire::Put(data, kMagic) || !SnapshotWire::Put(data, size)
		|| !SnapshotWire::Put(data, snapshot.data(), snapshot.size()) ) return false;
	for( size_t i = 0; i < identities.items.size(); ++i )
	{
		const auto& item = identities.items[i];
		if( !item.id || item.name != state.entries[i].name
			|| !SnapshotWire::Put(data, item.volume) || !SnapshotWire::Put(data, item.id) ) return false;
	}
	output.swap(data); return true;
}

inline bool Decode(const std::vector<BYTE>& data, DirectorySnapshot::State& output,
	DirectoryEntryIdentity::State& identities, bool& present)
{
	if( data.size() < sizeof(DWORD) || data.size() > SnapshotWire::kMaxBytes ) return false;
	DWORD magic = 0; std::memcpy(&magic, data.data(), sizeof(magic));
	if( magic == SnapshotWire::kMagic )
	{
		DirectorySnapshot::State state;
		if( !SnapshotWire::Decode(data, state) ) return false;
		output = std::move(state); identities = DirectoryEntryIdentity::State(); present = false; return true;
	}
	if( magic != kMagic ) return false;
	size_t offset = sizeof(magic); DWORD size = 0;
	if( !SnapshotWire::Get(data, offset, size) || size > data.size() - offset ) return false;
	std::vector<BYTE> snapshot(data.begin() + offset, data.begin() + offset + size); offset += size;
	DirectorySnapshot::State state;
	if( !SnapshotWire::Decode(snapshot, state) || state.entries.size() > (data.size() - offset) / 12
		|| state.entries.size() * 12 != data.size() - offset ) return false;
	DirectoryEntryIdentity::State parsed; parsed.volume = state.volume; parsed.directoryId = state.directoryId;
	parsed.items.reserve(state.entries.size());
	for( const auto& entry : state.entries )
	{
		DirectoryEntryIdentity::Item item; item.name = entry.name;
		if( !SnapshotWire::Get(data, offset, item.volume) || !SnapshotWire::Get(data, offset, item.id) || !item.id ) return false;
		parsed.items.push_back(std::move(item));
	}
	output = std::move(state); identities = std::move(parsed); present = true; return true;
}
}
