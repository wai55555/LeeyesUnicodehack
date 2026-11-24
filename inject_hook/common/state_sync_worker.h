#pragma once

#include "state_sync_coordinator.h"
#include <atomic>
#include <new>
#include <thread>

// Explicit-lifetime background reader. Never construct or stop it from
// DllMain or the UI thread: shutdown may wait for a child timeout. The owner
// must stop and join it before unloading the module that contains this code.
class StateSyncWorker
{
public:
	StateSyncWorker(const std::wstring& launcher, DWORD timeoutMs,
		std::uint64_t intervalMs = 300, std::uint64_t retryMs = 1000)
		: launcher_(launcher), timeoutMs_(timeoutMs), coordinator_(intervalMs, retryMs),
		  wake_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
	~StateSyncWorker() { Stop(); }
	StateSyncWorker(const StateSyncWorker&) = delete;
	StateSyncWorker& operator=(const StateSyncWorker&) = delete;

	bool Start()
	{
		if( !wake_.value || thread_.joinable() || stopped_.load() ) return false;
		try { thread_ = std::thread(&StateSyncWorker::Run, this); }
		catch( ... ) { return false; }
		return true;
	}
	void Stop()
	{
		stopped_.store(true);
		if( wake_.value ) ::SetEvent(wake_.value);
		if( thread_.joinable() ) thread_.join();
	}
	void SetTarget(const std::wstring& path)
	{
		coordinator_.SetTarget(path, ::GetTickCount64());
		if( wake_.value ) ::SetEvent(wake_.value);
	}
	void Changed()
	{
		coordinator_.Changed(::GetTickCount64());
		if( wake_.value ) ::SetEvent(wake_.value);
	}
	bool TakeCandidate(StateSyncCoordinator::Candidate& candidate)
	{ return coordinator_.TakeCandidate(candidate); }
	bool Failed() const { return failed_.load(); }
	DWORD LastError() { return failed_.load() ? lastError_.load() : coordinator_.CurrentError(); }
	bool CanApply(const StateSyncCoordinator::Candidate& candidate)
	{ return coordinator_.CanApply(candidate); }
	bool FinishApply(const StateSyncCoordinator::Candidate& candidate, bool success)
	{
		const bool finished = coordinator_.FinishApply(candidate, success, ::GetTickCount64());
		if( finished && wake_.value ) ::SetEvent(wake_.value);
		return finished;
	}

private:
	void Run()
	{
		try { RunLoop(); }
		catch( const std::bad_alloc& )
		{
			lastError_.store(ERROR_NOT_ENOUGH_MEMORY);
			failed_.store(true);
		}
		catch( ... )
		{
			lastError_.store(ERROR_GEN_FAILURE);
			failed_.store(true);
		}
	}
	void RunLoop()
	{
		while( !stopped_.load() )
		{
			StateSyncCoordinator::Work work = {};
			if( coordinator_.TryBegin(::GetTickCount64(), work) )
			{
				DirectorySnapshot::State state;
				SnapshotWire::ProcessResult process;
				try { process = SnapshotWire::ReadOnWorkerThread(launcher_, work.path, timeoutMs_, state); }
				catch( const std::bad_alloc& )
				{
					// An exception can occur after process creation. We cannot prove
					// cleanup without a returned handle; disable the worker so its
					// caller can fall back instead of silently waiting forever.
					lastError_.store(ERROR_NOT_ENOUGH_MEMORY);
					failed_.store(true);
					return;
				}
				catch( ... )
				{
					lastError_.store(ERROR_GEN_FAILURE);
					failed_.store(true);
					return;
				}
				coordinator_.FinishFetch(work, process, std::move(state), ::GetTickCount64());
				continue;
			}
			::WaitForSingleObject(wake_.value, 100);
		}
	}
	std::wstring launcher_;
	DWORD timeoutMs_;
	StateSyncCoordinator coordinator_;
	DirectorySnapshot::Handle wake_;
	std::atomic<bool> stopped_{false};
	std::atomic<bool> failed_{false};
	std::atomic<DWORD> lastError_{ERROR_SUCCESS};
	std::thread thread_;
};
