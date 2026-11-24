#pragma once

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

// Bounded replay journal for notifications that an adapter can reproduce
// losslessly. A caller must append successfully before suppressing anything.
// Preserve dispatcher bytes exactly: decoding and re-encoding an ANSI path can
// lose Unicode aliases or change collisions. The adapter owns the Delphi
// string allocation and lifetime when replaying these bytes.
// On failure the adapter must replay all earlier deferred events in order
// before passing the rejected event to the original dispatcher. If ordering
// or event lifetime cannot be preserved, it must not suppress events at all.
// This does not itself call into Leeyes.
class NotificationFallback
{
public:
	struct Entry { unsigned action; std::string first, second; };
	explicit NotificationFallback(std::size_t maxEntries, std::size_t maxCharacters, unsigned maxAction = 5)
		: maxEntries_(maxEntries), maxCharacters_(maxCharacters), maxAction_(maxAction) {}
	bool TryAppend(unsigned action, const std::string& first, const std::string& second)
	{
		if( sealed_ || action > maxAction_ || first.empty()
			|| first.find('\0') != std::string::npos || second.find('\0') != std::string::npos
			|| ((action == 1 || action == 4) && second.empty())
			|| entries_.size() >= maxEntries_
			|| characters_ > maxCharacters_ || first.size() > maxCharacters_ - characters_
			|| second.size() > maxCharacters_ - characters_ - first.size() ) return false;
		try
		{
			Entry entry = { action, first, second };
			entries_.push_back(std::move(entry));
		}
		catch( ... ) { return false; }
		characters_ += first.size() + second.size();
		return true;
	}
	void Seal() { sealed_ = true; }
	// The caller serializes access and seals before final completion. New
	// callbacks may be appended between streaming replay slices.
	void StartStreaming() { streaming_ = true; }
	bool Empty() const { return cursor_ == entries_.size(); }
	// The cursor advances only after the caller confirms an original dispatch.
	const Entry* Next() const
	{ return (sealed_ || streaming_) && cursor_ < entries_.size() ? &entries_[cursor_] : nullptr; }
	bool Acknowledge()
	{ if( !Next() ) return false; ++cursor_; return true; }
	std::size_t AcknowledgedCount() const { return cursor_; }
	bool Backlogged() const
	{ return maxEntries_ && entries_.size() - cursor_ >= maxEntries_ / 2 + maxEntries_ % 2; }
	bool NearCapacity(std::size_t reserveCharacters) const
	{
		return entries_.size() >= maxEntries_ || characters_ > maxCharacters_
			|| reserveCharacters > maxCharacters_ - characters_;
	}
	bool HasRoomFor(std::size_t firstCharacters, std::size_t secondCharacters) const
	{
		return !sealed_ && entries_.size() < maxEntries_ && characters_ <= maxCharacters_
			&& firstCharacters <= maxCharacters_ - characters_
			&& secondCharacters <= maxCharacters_ - characters_ - firstCharacters;
	}
	// Called only between replay slices, after the caller has released any
	// pointer returned by Next(). Retire acknowledged entries so a sustained
	// stream can reuse its bounded storage without dropping pending events.
	std::size_t CompactAcknowledged()
	{
		if( !streaming_ || sealed_ || !cursor_ ) return 0;
		const std::size_t retired = cursor_;
		for( std::size_t i = 0; i < retired; ++i )
			characters_ -= entries_[i].first.size() + entries_[i].second.size();
		entries_.erase(entries_.begin(), entries_.begin() + retired);
		cursor_ = 0; retired_ += retired;
		return retired;
	}
	bool Replayed() const { return sealed_ && cursor_ == entries_.size(); }
	std::size_t Size() const { return retired_ + entries_.size(); }
	std::size_t PendingSize() const { return entries_.size() - cursor_; }
	// The state adapter may inspect the complete original sequence only before
	// streaming, sealing, acknowledgement, or compaction has started.
	const std::vector<Entry>* EntriesForState() const
	{ return !sealed_ && !streaming_ && !cursor_ && !retired_ ? &entries_ : nullptr; }
	// Only complete adjacent create/delete pairs with unique exact dispatcher
	// paths are candidates. The caller must separately prove both the prior UI
	// and current filesystem state do not contain any of these names.
	bool AdjacentTransientPairs(std::vector<std::string>& paths) const
	{
		paths.clear();
		const auto* entries = EntriesForState();
		if( !entries || entries->empty() || entries->size() % 2 ) return false;
		try
		{
			std::set<std::string> seen;
			std::vector<std::string> result; result.reserve(entries->size() / 2);
			for( size_t i = 0; i < entries->size(); i += 2 )
			{
				const Entry& create = (*entries)[i]; const Entry& remove = (*entries)[i + 1];
				if( create.action != 3 || remove.action != 5 || !create.second.empty() || !remove.second.empty()
					|| create.first != remove.first || !seen.insert(create.first).second ) return false;
				result.push_back(create.first);
			}
			paths.swap(result); return true;
		}
		catch( ... ) { return false; }
	}
	// Match each proposed directory change to exactly one original callback.
	// Comparisons deliberately use exact dispatcher bytes; aliases and case
	// folding cannot prove identity or preserve callback ordering.
	bool OrderChanges(const std::vector<std::pair<unsigned, std::string>>& changes,
		std::vector<std::size_t>& order, bool remainingOnly = false,
		std::vector<std::pair<unsigned, std::string>>* originalSequence = nullptr) const
	{
		order.clear();
		if( originalSequence ) originalSequence->clear();
		if( remainingOnly ? (!streaming_ || sealed_ || retired_) : !EntriesForState() ) return false;
		const std::size_t begin = remainingOnly ? cursor_ : 0;
		if( begin == entries_.size() || (!originalSequence && entries_.size() - begin != changes.size()) ) return false;
		try
		{
			std::map<std::pair<unsigned, std::string>, std::size_t> remaining;
			for( std::size_t i = 0; i < changes.size(); ++i )
				if( !remaining.emplace(changes[i], i).second ) return false;
			std::vector<std::size_t> result; result.reserve(entries_.size() - begin);
			std::vector<std::pair<unsigned, std::string>> originals;
			if( originalSequence ) originals.reserve(entries_.size() - begin);
			for( std::size_t i = begin; i < entries_.size(); ++i )
			{
				const auto& entry = entries_[i];
				if( !entry.second.empty() ) return false;
				if( originalSequence ) originals.emplace_back(entry.action, entry.first);
				if( originalSequence && (entry.action == 0 || entry.action == 2) )
				{ result.push_back(changes.size()); continue; }
				if( entry.action != 3 && entry.action != 5 ) return false;
				const auto found = remaining.find(std::make_pair(entry.action, entry.first));
				if( found == remaining.end() ) return false;
				result.push_back(found->second); remaining.erase(found);
			}
			if( !remaining.empty() ) return false;
			order.swap(result); if( originalSequence ) originalSequence->swap(originals); return true;
		}
		catch( ... ) { return false; }
	}
	// A name visible before and after a delete/add pair may represent a
	// different object. ANSI aliases cannot safely prove inequality either.
	// Treat allocation failure or non-ASCII bytes as ambiguous.
	bool HasAmbiguousDirectoryTransitions() const
	{
		const auto* entries = EntriesForState();
		if( !entries ) return true;
		try
		{
			std::map<std::string, unsigned> seen;
			for( const Entry& entry : *entries )
			{
				if( entry.action != 3 && entry.action != 5 ) return true;
				std::string key = entry.first;
				for( char& c : key )
				{
					if( static_cast<unsigned char>(c) >= 0x80 ) return true;
					if( c >= 'A' && c <= 'Z' ) c = static_cast<char>(c - 'A' + 'a');
				}
				unsigned& flags = seen[key];
				flags |= entry.action == 3 ? 1u : 2u;
				if( flags == 3 ) return true;
			}
			return false;
		}
		catch( ... ) { return true; }
	}
	// Only ASCII paths can be mapped back to exact fixture paths here. Return
	// false if that mapping cannot be established; never return partial work.
	bool DirectoryPaths(std::vector<std::string>& paths) const
	{
		paths.clear();
		const auto* entries = EntriesForState();
		if( !entries ) return false;
		try
		{
			std::map<std::string, std::string> seen;
			for( const Entry& entry : *entries )
			{
				if( entry.action != 3 && entry.action != 5 ) return false;
				std::string key = entry.first;
				for( char& c : key )
				{
					if( static_cast<unsigned char>(c) >= 0x80 ) return false;
					if( c >= 'A' && c <= 'Z' ) c = static_cast<char>(c - 'A' + 'a');
				}
				const auto existing = seen.emplace(key, entry.first);
				// Different spellings can identify different objects in a
				// case-sensitive directory. Never silently omit either path.
				if( !existing.second && existing.first->second != entry.first ) return false;
			}
			std::vector<std::string> result;
			for( const auto& pair : seen ) result.push_back(pair.second);
			paths.swap(result);
			return true;
		}
		catch( ... ) { paths.clear(); return false; }
	}
	// Exact dispatcher bytes by default. The diagnostic-only CP932 option folds
	// ASCII bytes outside multibyte characters; it cannot prove filesystem
	// identity in case-sensitive directories or across aliases.
	bool AllDirectoryPathsCovered(const std::vector<std::string>& paths, bool foldAscii932 = false) const
	{
		const auto* entries = EntriesForState();
		if( !entries ) return false;
		try
		{
			std::set<std::string> covered;
			for( const std::string& path : paths )
			{
				std::string key;
				if( !FoldPath932(path, foldAscii932, key) ) return false;
				covered.insert(key);
			}
			for( const Entry& entry : *entries )
			{
				std::string key;
				if( (entry.action != 3 && entry.action != 5) || !FoldPath932(entry.first, foldAscii932, key)
					|| covered.find(key) == covered.end() ) return false;
			}
			return true;
		}
		catch( ... ) { return false; }
	}
private:
	static bool FoldPath932(const std::string& path, bool fold, std::string& output)
	{
		output = path;
		if( !fold ) return true;
		for( size_t i = 0; i < output.size(); ++i )
		{
			const unsigned char c = static_cast<unsigned char>(output[i]);
			if( (c >= 0x81 && c <= 0x9f) || (c >= 0xe0 && c <= 0xfc) )
			{
				if( ++i >= output.size() ) return false;
				const unsigned char trail = static_cast<unsigned char>(output[i]);
				if( trail < 0x40 || trail == 0x7f || trail > 0xfc ) return false;
			}
			else if( c >= 'A' && c <= 'Z' ) output[i] = static_cast<char>(c - 'A' + 'a');
			else if( c >= 0x80 && (c < 0xa1 || c > 0xdf) ) return false;
		}
		return true;
	}
	std::size_t maxEntries_, maxCharacters_, characters_ = 0, cursor_ = 0, retired_ = 0;
	unsigned maxAction_;
	bool sealed_ = false, streaming_ = false;
	std::vector<Entry> entries_;
};
