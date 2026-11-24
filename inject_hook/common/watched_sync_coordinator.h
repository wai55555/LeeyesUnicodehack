#pragma once

#include "watched_snapshot_stream.h"
#include <mutex>

// Thread-safe handoff from one supervised watcher stream to the UI. It does
// not start processes or mutate the UI. A caller must continue delivering the
// live stream and recheck a ticket before completing an application.
class WatchedSyncCoordinator
{
public:
	struct Candidate {
		std::uint64_t target = 0, generation = 0; DirectorySnapshot::State state;
		bool hasEntryIdentities = false; DirectoryEntryIdentity::State entryIdentities;
	};
	enum class Delivery { Accepted, OldTarget, Failed };
	std::uint64_t SetTarget(const std::wstring& path)
	{
		std::wstring nextPath = path;
		std::lock_guard<std::mutex> lock(mutex_);
		if( target_ == (std::numeric_limits<std::uint64_t>::max)() )
		{ error_ = ERROR_ARITHMETIC_OVERFLOW; ready_ = false; receiver_.Fail(error_); return 0; }
		++target_; path_.swap(nextPath); receiver_ = WatchedSnapshotStream::Receiver();
		pending_ = Candidate(); ready_ = false; inFlight_ = false; requested_ = 0; error_ = ERROR_SUCCESS; baselineTarget_ = 0;
		return target_;
	}
	// A failed stream receives a new epoch before restart so buffered messages
	// and candidates from the old child cannot become current again. Preserve
	// its error until a valid snapshot from the replacement arrives.
	std::uint64_t Restart(std::uint64_t target)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !target || target != target_ || path_.empty() ) return 0;
		if( target_ == (std::numeric_limits<std::uint64_t>::max)() )
		{ error_ = ERROR_ARITHMETIC_OVERFLOW; receiver_.Fail(error_); ready_ = false; return 0; }
		++target_; receiver_ = WatchedSnapshotStream::Receiver();
		pending_ = Candidate(); ready_ = false; inFlight_ = false; requested_ = 0; baselineTarget_ = 0; return target_;
	}
	bool RequestRefresh(std::uint64_t target, std::uint64_t request)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !target || target != target_ || path_.empty() || !request || request <= requested_ ) return false;
		requested_ = request; ready_ = false; pending_ = Candidate(); return true;
	}
	Delivery Accept(std::uint64_t target, const WatchedSnapshotStream::Header& header, const std::vector<BYTE>& body)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !target || target != target_ || path_.empty() ) return Delivery::OldTarget;
		if( !receiver_.Accept(header, body) )
		{ ready_ = false; pending_ = Candidate(); error_ = receiver_.Error(); return Delivery::Failed; }
		const auto kind = static_cast<WatchedSnapshotStream::Kind>(header.kind);
		if( kind == WatchedSnapshotStream::Kind::Ready || kind == WatchedSnapshotStream::Kind::Dirty
			|| kind == WatchedSnapshotStream::Kind::Refresh )
		{ ready_ = false; pending_ = Candidate(); }
		else if( kind == WatchedSnapshotStream::Kind::Snapshot )
		{
			WatchedSnapshot::Candidate received = receiver_.ReleaseCandidate();
			if( receiver_.RefreshRequest() >= requested_ )
			{
				pending_.target = target_; pending_.generation = header.generation;
				pending_.state = std::move(received.state); pending_.hasEntryIdentities = received.hasEntryIdentities;
				pending_.entryIdentities = std::move(received.entryIdentities); ready_ = true; error_ = ERROR_SUCCESS;
			}
		}
		return Delivery::Accepted;
	}
	void Disconnected(std::uint64_t target, DWORD error)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( target != target_ ) return;
		error_ = error ? error : ERROR_BROKEN_PIPE; receiver_.Fail(error_); ready_ = false; pending_ = Candidate(); baselineTarget_ = 0;
	}
	bool Take(Candidate& candidate)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if( !ready_ || inFlight_ ) return false;
		candidate = std::move(pending_); ready_ = false; inFlight_ = true;
		inFlightTarget_ = candidate.target; inFlightGeneration_ = candidate.generation;
		inFlightVolume_ = candidate.state.volume; inFlightDirectoryId_ = candidate.state.directoryId; return true;
	}
	bool CanApply(const Candidate& candidate)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return inFlight_ && candidate.target == target_ && candidate.target == inFlightTarget_
			&& candidate.generation == inFlightGeneration_ && receiver_.RefreshRequest() >= requested_
			&& receiver_.Current(candidate.generation);
	}
	bool Finish(const Candidate& candidate)
	{
		bool current = false;
		return ReleaseInternal(candidate, current, true) && current;
	}
	// Release is successful even if the ticket became stale; the caller must
	// not interpret that as authorization to commit the stale UI update.
	bool Release(const Candidate& candidate, bool& current)
	{ return ReleaseInternal(candidate, current, false); }
	// Only establishes that the previously committed ticket and this current
	// ticket belong to one uninterrupted watcher session for the same directory.
	// It does not prove that UI nodes represented the old snapshot, that every
	// filesystem change was delivered, or that unchanged IDs imply equal content.
	bool HasContinuousBaseline(const Candidate& candidate)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return baselineTarget_ && baselineTarget_ == target_ && candidate.target == target_
			&& inFlight_ && candidate.target == inFlightTarget_ && candidate.generation == inFlightGeneration_
			&& candidate.generation >= baselineGeneration_ && inFlightDirectoryId_ == baselineDirectoryId_
			&& inFlightVolume_ == baselineVolume_ && receiver_.RefreshRequest() >= requested_
			&& receiver_.Current(candidate.generation);
	}
	// A UI-side baseline capture can take time after Finish(). Reject it if a
	// newer dirty/refresh frame, disconnect, or target change was delivered.
	bool IsCommittedCurrent(const Candidate& candidate)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return baselineTarget_ && candidate.target == target_ && candidate.target == baselineTarget_
			&& candidate.generation == baselineGeneration_ && receiver_.RefreshRequest() >= requested_
			&& receiver_.Current(candidate.generation);
	}
	DWORD Error() { std::lock_guard<std::mutex> lock(mutex_); return error_; }
private:
	bool ReleaseInternal(const Candidate& candidate, bool& current, bool commit)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		current = false;
		if( !inFlight_ || candidate.target != inFlightTarget_ || candidate.generation != inFlightGeneration_ ) return false;
		current = candidate.target == target_ && receiver_.RefreshRequest() >= requested_ && receiver_.Current(candidate.generation);
		if( commit && current )
		{
			baselineTarget_ = candidate.target; baselineGeneration_ = candidate.generation;
			baselineVolume_ = inFlightVolume_; baselineDirectoryId_ = inFlightDirectoryId_;
		}
		inFlight_ = false; return true;
	}
	std::mutex mutex_;
	std::wstring path_;
	WatchedSnapshotStream::Receiver receiver_;
	Candidate pending_ = {};
	std::uint64_t target_ = 0, inFlightTarget_ = 0, inFlightGeneration_ = 0, requested_ = 0;
	std::uint64_t baselineTarget_ = 0, baselineGeneration_ = 0, baselineDirectoryId_ = 0, inFlightDirectoryId_ = 0;
	DWORD baselineVolume_ = 0, inFlightVolume_ = 0;
	DWORD error_ = ERROR_SUCCESS;
	bool ready_ = false, inFlight_ = false;
};
