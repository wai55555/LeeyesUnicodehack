#pragma once

#include "watched_snapshot_process.h"
#include "watched_sync_coordinator.h"
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Experimental process-lifetime worker. Start/Stop and destruction must never
// occur on the UI thread or in DllMain. If a terminated child remains stuck in
// kernel I/O, Stop may wait indefinitely: retain the worker and its process
// handle until exit is confirmed, and never spawn a replacement meanwhile.
class WatchedSyncWorker
{
public:
	using Candidate = WatchedSyncCoordinator::Candidate;
	explicit WatchedSyncWorker(std::wstring launcher, DWORD heartbeatTimeoutMs = 5000,
		bool collectEntryIdentities = false, bool watchSubtree = true)
		: launcher_(std::move(launcher)), heartbeatTimeoutMs_(heartbeatTimeoutMs),
		collectEntryIdentities_(collectEntryIdentities), watchSubtree_(watchSubtree) {}
	~WatchedSyncWorker() { Stop(); }
	WatchedSyncWorker(const WatchedSyncWorker&) = delete;
	WatchedSyncWorker& operator=(const WatchedSyncWorker&) = delete;
	bool Start()
	{
		if( thread_.joinable() || stopped_.load() || launcher_.empty() || heartbeatTimeoutMs_ < 1000 ) return false;
		try { thread_ = std::thread(&WatchedSyncWorker::Run, this); return true; }
		catch( ... ) { return false; }
	}
	void Stop()
	{
		stopped_.store(true);
		{ std::lock_guard<std::mutex> lock(targetMutex_); coordinator_.Disconnected(targetEpoch_, ERROR_OPERATION_ABORTED); }
		if( thread_.joinable() ) thread_.join();
	}
	std::uint64_t SetTarget(const std::wstring& path)
	{
		std::wstring copy = path;
		std::lock_guard<std::mutex> lock(targetMutex_);
		if( stopped_.load() ) return 0;
		const std::uint64_t epoch = coordinator_.SetTarget(copy);
		if( !epoch ) return 0;
		targetPath_.swap(copy); targetEpoch_ = epoch; pendingRequest_ = 0; return epoch;
	}
	bool RequestRefresh()
	{
		std::lock_guard<std::mutex> lock(targetMutex_);
		if( stopped_.load() || targetPath_.empty() || requestSerial_ == (std::numeric_limits<std::uint64_t>::max)() ) return false;
		const std::uint64_t request = ++requestSerial_;
		if( !coordinator_.RequestRefresh(targetEpoch_, request) ) return false;
		pendingRequest_ = request; return true;
	}
	bool Take(Candidate& candidate) { return coordinator_.Take(candidate); }
	bool CanApply(const Candidate& candidate) { return coordinator_.CanApply(candidate); }
	bool HasContinuousBaseline(const Candidate& candidate) { return coordinator_.HasContinuousBaseline(candidate); }
	bool IsCommittedCurrent(const Candidate& candidate) { return coordinator_.IsCommittedCurrent(candidate); }
	bool Finish(const Candidate& candidate) { return coordinator_.Finish(candidate); }
	bool Release(const Candidate& candidate, bool& current) { return coordinator_.Release(candidate, current); }
	DWORD Error() { return coordinator_.Error(); }
	bool Failed() const { return failed_.load(); }
	// True only after the supervised child has been confirmed stopped. A caller
	// must not create a replacement while a failed worker still owns a live child.
	bool TerminationConfirmed() const { return terminationConfirmed_.load(); }
	unsigned SnapshotFrames() const { return snapshotFrames_.load(); }
	unsigned DirtyFrames() const { return dirtyFrames_.load(); }
	unsigned ChildStarts() const { return childStarts_.load(); }
private:
	void Run()
	{
		WatchedSnapshotProcess process;
		try { RunLoop(process); }
		catch( ... )
		{
			failed_.store(true);
			std::lock_guard<std::mutex> lock(targetMutex_);
			coordinator_.Disconnected(targetEpoch_, ERROR_GEN_FAILURE);
		}
		while( !process.Stop(100) ) ::Sleep(50);
		terminationConfirmed_.store(true);
	}
	void RunLoop(WatchedSnapshotProcess& process)
	{
		std::uint64_t activeEpoch = 0, sentRequest = 0, acknowledgedRequest = 0;
		bool active = false, failedTarget = false;
		ULONGLONG lastFrame = 0, retryAt = 0, requestStarted = 0;
		const ULONGLONG requestDeadlineMs = 30000; // Bound a refresh even if live frames keep arriving.
		while( !stopped_.load() )
		{
			std::wstring path; std::uint64_t epoch = 0, request = 0;
			{ std::lock_guard<std::mutex> lock(targetMutex_); path = targetPath_; epoch = targetEpoch_; request = pendingRequest_; }
			if( epoch != activeEpoch )
			{
				// Stop is idempotent and retains an unconfirmed process handle.
				if( !process.Stop(0) ) { ::Sleep(50); continue; }
				active = false; failedTarget = false; activeEpoch = epoch; sentRequest = acknowledgedRequest = 0;
				requestStarted = 0;
				if( !path.empty() )
				{
					DWORD error = ERROR_SUCCESS;
					if( !process.Start(launcher_, path, error, collectEntryIdentities_, watchSubtree_) )
					{ coordinator_.Disconnected(epoch, error); failedTarget = true; retryAt = ::GetTickCount64() + 1000; }
					else { ++childStarts_; active = true; lastFrame = ::GetTickCount64(); }
				}
			}
			if( failedTarget && ::GetTickCount64() >= retryAt )
			{
				std::lock_guard<std::mutex> lock(targetMutex_);
				if( !stopped_.load() && targetEpoch_ == activeEpoch )
				{
					const std::uint64_t restarted = coordinator_.Restart(activeEpoch);
					if( restarted ) { targetEpoch_ = restarted; pendingRequest_ = 0; }
					else retryAt = ::GetTickCount64() + 1000;
				}
			}
			if( !active || failedTarget ) { ::Sleep(50); continue; }
			const ULONGLONG now = ::GetTickCount64();
			if( requestStarted && now - requestStarted >= requestDeadlineMs )
			{
				coordinator_.Disconnected(activeEpoch, ERROR_TIMEOUT);
				failedTarget = true; active = false; requestStarted = 0;
				retryAt = now + 1000; process.Stop(0); continue;
			}
			if( request && request != sentRequest && sentRequest == acknowledgedRequest )
			{
				DWORD sendError = ERROR_SUCCESS;
				const ULONGLONG started = ::GetTickCount64();
				if( !process.RequestRefresh(request, sendError) )
				{
					coordinator_.Disconnected(activeEpoch, sendError); failedTarget = true;
					active = false; retryAt = ::GetTickCount64() + 1000; process.Stop(0); continue;
				}
				sentRequest = request;
				requestStarted = started;
			}
			WatchedSnapshotStream::Header header = {}; std::vector<BYTE> body; DWORD error = ERROR_SUCCESS;
			if( process.Next(100, header, body, error) )
			{
				lastFrame = ::GetTickCount64();
				if( header.kind == static_cast<DWORD>(WatchedSnapshotStream::Kind::Refresh) )
				{
					std::uint64_t ack = 0;
					if( body.size() == sizeof(ack) ) std::memcpy(&ack, body.data(), sizeof(ack));
					if( !sentRequest || ack != sentRequest || ack <= acknowledgedRequest )
					{
						coordinator_.Disconnected(activeEpoch, ERROR_INVALID_DATA); failedTarget = true;
						active = false; retryAt = ::GetTickCount64() + 1000; process.Stop(0); continue;
					}
				}
				const auto delivery = coordinator_.Accept(activeEpoch, header, body);
				if( delivery == WatchedSyncCoordinator::Delivery::Accepted )
				{
					if( header.kind == static_cast<DWORD>(WatchedSnapshotStream::Kind::Refresh) && body.size() == sizeof(acknowledgedRequest) )
						std::memcpy(&acknowledgedRequest, body.data(), sizeof(acknowledgedRequest));
					if( header.kind == static_cast<DWORD>(WatchedSnapshotStream::Kind::Snapshot) )
					{
						++snapshotFrames_;
						if( sentRequest && acknowledgedRequest == sentRequest ) requestStarted = 0;
					}
					else if( header.kind == static_cast<DWORD>(WatchedSnapshotStream::Kind::Dirty) ) ++dirtyFrames_;
				}
				if( delivery == WatchedSyncCoordinator::Delivery::Failed )
				{ failedTarget = true; active = false; retryAt = ::GetTickCount64() + 1000; process.Stop(0); }
			}
			else if( error != ERROR_TIMEOUT || ::GetTickCount64() - lastFrame >= heartbeatTimeoutMs_ )
			{
				coordinator_.Disconnected(activeEpoch, error == ERROR_TIMEOUT ? ERROR_TIMEOUT : error);
				failedTarget = true; active = false; retryAt = ::GetTickCount64() + 1000; process.Stop(0);
			}
		}
	}
	std::wstring launcher_;
	DWORD heartbeatTimeoutMs_;
	bool collectEntryIdentities_;
	bool watchSubtree_;
	WatchedSyncCoordinator coordinator_;
	std::mutex targetMutex_;
	std::wstring targetPath_;
	std::uint64_t targetEpoch_ = 0, requestSerial_ = 0, pendingRequest_ = 0;
	std::atomic<bool> stopped_{false}, failed_{false}, terminationConfirmed_{false};
	std::atomic<unsigned> snapshotFrames_{0}, dirtyFrames_{0}, childStarts_{0};
	std::thread thread_;
};
