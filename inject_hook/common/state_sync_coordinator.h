#pragma once

#include "state_sync_scheduler.h"
#include "state_snapshot_process.h"
#include "directory_entry_identity.h"
#include <mutex>

// State machine shared by the UI-side controller and one background reader.
// It deliberately does not own a thread, a window, or Leeyes objects.
class StateSyncCoordinator
{
public:
	struct Work { StateSyncScheduler::Ticket ticket; std::wstring path; };
	struct Candidate {
		StateSyncScheduler::Ticket ticket; DirectorySnapshot::State state;
		bool hasEntryIdentities = false; DirectoryEntryIdentity::State entryIdentities;
	};
	explicit StateSyncCoordinator(std::uint64_t intervalMs = 300, std::uint64_t retryMs = 1000)
		: scheduler_(intervalMs, retryMs) {}

	void SetTarget(const std::wstring& path, std::uint64_t now)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		path_ = path;
		scheduler_.ChangeTarget(now);
		currentError_ = ERROR_SUCCESS;
		DiscardReady(now);
	}
	void Changed(std::uint64_t now)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		scheduler_.Changed(now);
		currentError_ = ERROR_SUCCESS;
		DiscardReady(now);
	}
	bool TryBegin(std::uint64_t now, Work& work)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( path_.empty() || (cleanupPending_ && !pendingCleanup_.CleanupComplete()) ) return false;
		pendingCleanup_ = SnapshotWire::ProcessResult(); cleanupPending_ = false;
		std::wstring path = path_;
		StateSyncScheduler::Ticket ticket = {};
		if( !scheduler_.Begin(now, ticket) ) return false;
		work.ticket = ticket; work.path.swap(path);
		return true;
	}
	// Called once for the ticket, by the worker after the child has returned.
	// A stale or failed fetch never publishes or clears the dirty state.
	bool FinishFetch(const Work& work, const SnapshotWire::ProcessResult& process,
		DirectorySnapshot::State&& state, std::uint64_t now)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !scheduler_.Owns(work.ticket) || ready_ || taken_ ) return false;
		if( !process.cleanupConfirmed ) { pendingCleanup_ = process; cleanupPending_ = true; }
		if( scheduler_.CanApply(work.ticket) )
			currentError_ = process.error ? process.error : !process.cleanupConfirmed ? ERROR_BUSY
				: !state.directoryId ? ERROR_INVALID_DATA : ERROR_SUCCESS;
		if( process.error || !process.cleanupConfirmed || !state.directoryId )
			return scheduler_.Finish(work.ticket, false, now);
		if( !scheduler_.CanApply(work.ticket) )
			return scheduler_.Finish(work.ticket, true, now);
		candidate_.ticket = work.ticket;
		candidate_.state = std::move(state);
		candidate_.hasEntryIdentities = false;
		candidate_.entryIdentities = DirectoryEntryIdentity::State();
		ready_ = true;
		return true;
	}
	bool TakeCandidate(Candidate& candidate)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !ready_ ) return false;
		candidate = std::move(candidate_);
		ready_ = false; taken_ = true;
		return true;
	}
	// The caller must serialize this check and the UI application with its
	// notification and target-switch handlers, and prevent nested dispatch.
	bool CanApply(const Candidate& candidate)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return taken_ && scheduler_.CanApply(candidate.ticket);
	}
	bool FinishApply(const Candidate& candidate, bool success, std::uint64_t now)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !taken_ ) return false;
		if( !scheduler_.Finish(candidate.ticket, success, now) ) return false;
		taken_ = false;
		return true;
	}
	DWORD CurrentError()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return currentError_;
	}

private:
	void DiscardReady(std::uint64_t now)
	{
		if( !ready_ ) return;
		scheduler_.Finish(candidate_.ticket, true, now);
		candidate_ = Candidate();
		ready_ = false;
	}
	std::mutex mutex_;
	StateSyncScheduler scheduler_;
	std::wstring path_;
	SnapshotWire::ProcessResult pendingCleanup_;
	bool cleanupPending_ = false;
	DWORD currentError_ = ERROR_SUCCESS;
	Candidate candidate_ = {};
	bool ready_ = false, taken_ = false;
};
