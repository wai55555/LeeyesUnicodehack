#pragma once

#include <cstdint>
#include <limits>

// Standalone diagnostic prototype. The caller supplies a monotonic clock and
// owns all thread synchronization, filesystem snapshots, and UI operations.
class StateSyncScheduler
{
public:
	struct Ticket { std::uint64_t operation, target, changes; };
	explicit StateSyncScheduler(std::uint64_t intervalMs = 300,
		std::uint64_t retryMs = 1000)
		: interval_(intervalMs), retry_(retryMs) {}

	void ChangeTarget(std::uint64_t now)
	{
		++target_;
		++changes_;
		dirty_ = true;
		due_ = now;
	}

	void Changed(std::uint64_t now)
	{
		++changes_;
		if( !dirty_ ) { dirty_ = true; due_ = Add(now, interval_); }
	}

	bool Begin(std::uint64_t now, Ticket& ticket)
	{
		if( !dirty_ || inFlight_ || now < due_ ) return false;
		inFlight_ = true;
		active_ = { ++operation_, target_, changes_ };
		ticket = active_;
		return true;
	}

	// An enumeration is not atomic. Never apply a snapshot if a notification
	// arrived while it was collected: a mixed snapshot can otherwise remove a
	// selected item or apply a stale rename. The caller must serialize this
	// check and the start of UI application with Changed/ChangeTarget.
	bool CanApply(const Ticket& ticket) const
	{
		return Matches(ticket) && ticket.target == target_ && ticket.changes == changes_;
	}

	// Call after both the scan and any UI application have succeeded or failed.
	// A changed generation remains dirty and is scanned again.
	bool Finish(const Ticket& ticket, bool success, std::uint64_t now)
	{
		if( !Matches(ticket) ) return false;
		inFlight_ = false;
		if( ticket.target != target_ ) { due_ = now; return true; }
		if( !success ) { dirty_ = true; due_ = Add(now, retry_); return true; }
		if( ticket.changes == changes_ ) dirty_ = false;
		else { dirty_ = true; due_ = Add(now, interval_); }
		return true;
	}

	bool IsDirty() const { return dirty_; }
	bool InFlight() const { return inFlight_; }
	bool Owns(const Ticket& ticket) const { return Matches(ticket); }

private:
	static std::uint64_t Add(std::uint64_t a, std::uint64_t b)
	{
		const auto max = (std::numeric_limits<std::uint64_t>::max)();
		return b > max - a ? max : a + b;
	}
	bool Matches(const Ticket& t) const
	{
		return inFlight_ && t.operation == active_.operation
			&& t.target == active_.target && t.changes == active_.changes;
	}
	std::uint64_t interval_, retry_, target_ = 0, changes_ = 0, operation_ = 0, due_ = 0;
	Ticket active_ = {};
	bool dirty_ = false, inFlight_ = false;
};
