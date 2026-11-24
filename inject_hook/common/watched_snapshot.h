#pragma once

#include "directory_change_watcher.h"
#include "directory_entry_identity.h"
#include <cstdint>
#include <limits>

// Runs wholly inside a supervised process. The watcher remains armed after a
// snapshot is returned; its owner must continuously poll it and forward every
// invalidation until the UI has applied a newer candidate. A quiet poll is not
// an atomic barrier against future or delayed filesystem notifications.
class WatchedSnapshot
{
public:
	struct Candidate {
		std::uint64_t generation = 0; DirectorySnapshot::State state;
		bool hasEntryIdentities = false; DirectoryEntryIdentity::State entryIdentities;
	};
	bool Open(const std::wstring& path, DWORD& error, bool collectEntryIdentities = false)
	{
		healthy_ = false; collectEntryIdentities_ = false; path_.clear(); watcher_.Close();
		if( !Bump(error) || !watcher_.Open(path, error) ) return false;
		try { path_ = path; } catch( ... ) { watcher_.Close(); error = ERROR_NOT_ENOUGH_MEMORY; return false; }
		collectEntryIdentities_ = collectEntryIdentities; healthy_ = true; return true;
	}
	// A successful poll may invalidate an earlier candidate even though no
	// filename is retained. Overflow and identity loss stop this session.
	bool Poll(DWORD timeoutMs, bool& changed, DWORD& error)
	{
		changed = false;
		if( !healthy_ ) { error = ERROR_INVALID_HANDLE; return false; }
		const DirectoryChangeWatcher::Result result = watcher_.Wait(timeoutMs, error);
		if( result == DirectoryChangeWatcher::Result::Timeout ) return true;
		if( result == DirectoryChangeWatcher::Result::Changed )
		{
			changed = true;
			if( Bump(error) ) return true;
		}
		healthy_ = false;
		if( error == ERROR_SUCCESS ) error = ERROR_RETRY;
		return false;
	}
	bool Read(Candidate& output, DWORD& error)
	{
		lastReadStage_ = 0;
		bool changed = false;
		// Bound work when the directory is busy; callers can retry later.
		unsigned drained = 0;
		do {
			if( !Poll(0, changed, error) ) { lastReadStage_ = 1; return false; }
			if( changed && ++drained >= 64 ) { lastReadStage_ = 2; error = ERROR_RETRY; return false; }
		} while( changed );
		DirectorySnapshot::State state;
		if( !DirectorySnapshot::Read(path_, state, error) )
		{
			lastReadStage_ = 3;
			// An unsuccessful read cannot leave an older candidate authorized.
			// A transient race can be retried; other errors require reopening.
			if( error == ERROR_RETRY )
			{
				DWORD bumpError = ERROR_SUCCESS;
				if( !Bump(bumpError) ) { healthy_ = false; error = bumpError; }
			}
			else healthy_ = false;
			return false;
		}
		if( !watcher_.Matches(state) ) { lastReadStage_ = 4; healthy_ = false; error = ERROR_RETRY; return false; }
		DirectoryEntryIdentity::State identities;
		if( collectEntryIdentities_ && !DirectoryEntryIdentity::Read(path_, state, identities, error) )
		{
			lastReadStage_ = 7;
			if( error == ERROR_RETRY )
			{
				DWORD bumpError = ERROR_SUCCESS;
				if( !Bump(bumpError) ) { healthy_ = false; error = bumpError; }
			}
			else healthy_ = false;
			return false;
		}
		#ifdef WATCHED_SNAPSHOT_TEST_HOOK
		WATCHED_SNAPSHOT_TEST_HOOK(path_);
		#endif
		if( !Poll(0, changed, error) ) { lastReadStage_ = 5; return false; }
		if( changed ) { lastReadStage_ = 6; error = ERROR_RETRY; return false; }
		Candidate candidate; candidate.generation = generation_; candidate.state = std::move(state);
		candidate.hasEntryIdentities = collectEntryIdentities_; candidate.entryIdentities = std::move(identities);
		output = std::move(candidate); error = ERROR_SUCCESS; return true;
	}
	bool IsCurrent(const Candidate& candidate) const
	{ return healthy_ && candidate.generation == generation_ && watcher_.Matches(candidate.state); }
	bool RequestRefresh(DWORD& error)
	{
		if( !healthy_ ) { error = ERROR_INVALID_HANDLE; return false; }
		if( Bump(error) ) { error = ERROR_SUCCESS; return true; }
		healthy_ = false; return false;
	}
	std::uint64_t Generation() const { return generation_; }
	unsigned LastReadStage() const { return lastReadStage_; }
private:
	bool Bump(DWORD& error)
	{
		if( generation_ == (std::numeric_limits<std::uint64_t>::max)() ) { error = ERROR_ARITHMETIC_OVERFLOW; return false; }
		++generation_; return true;
	}
	DirectoryChangeWatcher watcher_;
	std::wstring path_;
	std::uint64_t generation_ = 0;
	unsigned lastReadStage_ = 0;
	bool healthy_ = false;
	bool collectEntryIdentities_ = false;
};
