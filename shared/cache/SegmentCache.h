#pragma once
#include <algorithm>
#include <cstddef>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

// Process-wide in-memory cache of full HLS segment file contents, keyed by
// a caller-chosen string — an absolute on-disk path in Hephaestus (which
// reads segments straight off disk), a request path in Hermes (which
// proxies segments over HTTP from Hephaestus). The class itself doesn't
// care which; it's pure byte-budget LRU + an optional per-group count cap.
//
// Lives in shared/ (header-only, like every other file here — see
// log/LogBuffer.h, crash/CrashHandler.h) so both services link against the
// identical class with their own independent instance/budget, without
// needing to widen either CMakeLists' `src/*.cpp` glob to also pick up a
// shared/*.cpp.
//
// budget_bytes == 0 means disabled: get()/put() short-circuit before taking
// the lock, so a low-power deployment that never opts in pays zero cost
// beyond the branch.
class SegmentCache
{
public:
	explicit SegmentCache(size_t budget_bytes)
		: budget_bytes_(budget_bytes)
	{
	}

	bool enabled() const { return budget_bytes_ > 0; }

	// nullptr on miss or when disabled. Returned as a shared_ptr so callers
	// can hand the bytes straight to httplib without an extra copy of
	// something that might be several MB (a direct-stream/native segment).
	std::shared_ptr<const std::string> get(const std::string& key)
	{
		if (!enabled()) return nullptr;
		std::lock_guard<std::mutex> lock(mtx_);
		auto it = index_.find(key);
		if (it == index_.end()) return nullptr;
		// Touch: move to MRU (front). splice() on the same list never
		// invalidates iterators/references, so index_'s stored iterator
		// stays valid.
		entries_.splice(entries_.begin(), entries_, it->second);
		return it->second->bytes;
	}

	// group_key/group_cap: when group_cap > 0, this insert also enforces
	// that at most group_cap entries sharing group_key stay resident,
	// evicting that group's own oldest member first — independent of
	// whether the global byte budget has room. This is how a live
	// channel's bucket gets capped to a handful of segments regardless of
	// how much budget is otherwise free — live playback never seeks
	// backward, so holding the full on-disk retention window in RAM would
	// just be wasted memory.
	//
	// Refuses to admit a single entry larger than half the total budget —
	// a single oversized native-bucket segment (uncapped remux bitrate)
	// otherwise thrashes the whole cache just to hold itself. Silently a
	// no-op when disabled.
	void put(const std::string& key, std::string bytes, const std::string& group_key = "",
			 size_t group_cap                                                        = 0)
	{
		if (!enabled()) return;
		size_t sz = bytes.size();
		if (sz == 0 || sz > budget_bytes_ / 2) return;

		std::lock_guard<std::mutex> lock(mtx_);

		// Re-caching an already-present key (shouldn't normally happen —
		// segment bytes are write-once — but stay correct if it does)
		// drops the stale entry first so accounting/group-membership stay
		// consistent.
		auto existing = index_.find(key);
		if (existing != index_.end()) eraseLocked(existing->second);

		evictLocked(sz);

		entries_.push_front(Entry{key, std::make_shared<std::string>(std::move(bytes)), group_key});
		index_[key]    = entries_.begin();
		current_bytes_ += sz;

		if (!group_key.empty())
		{
			group_members_[group_key].push_back(key);
			if (group_cap > 0) evictGroupLocked(group_key, group_cap);
		}
	}

	void invalidate(const std::string& key)
	{
		if (!enabled()) return;
		std::lock_guard<std::mutex> lock(mtx_);
		auto it = index_.find(key);
		if (it != index_.end()) eraseLocked(it->second);
	}

	// Removes every entry whose key starts with key_prefix — used on
	// whole-session teardown (remove_all(dir()) call sites in Hephaestus),
	// where enumerating individual segment paths isn't worth it.
	void invalidatePrefix(const std::string& key_prefix)
	{
		if (!enabled()) return;
		std::lock_guard<std::mutex> lock(mtx_);
		for (auto it = index_.begin(); it != index_.end();)
		{
			if (it->first.compare(0, key_prefix.size(), key_prefix) == 0)
			{
				auto entry_it = it->second;
				++it; // advance past this key before erasing it out from under us
				eraseLocked(entry_it);
			}
			else
			{
				++it;
			}
		}
	}

	size_t sizeBytes() const
	{
		std::lock_guard<std::mutex> lock(mtx_);
		return current_bytes_;
	}

	size_t entryCount() const
	{
		std::lock_guard<std::mutex> lock(mtx_);
		return entries_.size();
	}

private:
	struct Entry
	{
		std::string key;
		std::shared_ptr<std::string> bytes;
		std::string group_key;
	};

	// MRU at front, LRU at back.
	using EntryList = std::list<Entry>;

	size_t budget_bytes_;
	mutable std::mutex mtx_;
	EntryList entries_;
	std::unordered_map<std::string, EntryList::iterator> index_;
	std::unordered_map<std::string, std::deque<std::string>> group_members_; // group_key -> keys, oldest first
	size_t current_bytes_ = 0;

	// Assumes mtx_ is already held. Removes an entry from every index (the
	// LRU list, the key index, and — if it belongs to one — its group's
	// membership deque), keeping current_bytes_ in sync. The single point
	// all removal paths (global LRU eviction, per-group cap eviction,
	// explicit invalidation) funnel through, so those stay consistent with
	// each other by construction.
	void eraseLocked(EntryList::iterator it)
	{
		current_bytes_ -= it->bytes->size();
		if (!it->group_key.empty())
		{
			auto git = group_members_.find(it->group_key);
			if (git != group_members_.end())
			{
				auto& dq = git->second;
				auto dit = std::find(dq.begin(), dq.end(), it->key);
				if (dit != dq.end()) dq.erase(dit);
			}
		}
		index_.erase(it->key);
		entries_.erase(it);
	}

	// Assumes mtx_ is already held. Evicts globally-LRU entries (regardless
	// of group) until admitting needed_bytes more would fit within
	// budget_bytes_.
	void evictLocked(size_t needed_bytes)
	{
		while (current_bytes_ + needed_bytes > budget_bytes_ && !entries_.empty())
		{
			eraseLocked(std::prev(entries_.end()));
		}
	}

	// Assumes mtx_ is already held. Evicts group_key's own oldest members
	// until at most cap remain — independent of global byte-budget
	// headroom. This is what keeps a live channel's bucket small even when
	// the rest of the cache has plenty of room to spare.
	void evictGroupLocked(const std::string& group_key, size_t cap)
	{
		auto it = group_members_.find(group_key);
		if (it == group_members_.end()) return;
		while (it->second.size() > cap)
		{
			const std::string victim_key = it->second.front(); // copy: erased below
			auto idx_it                  = index_.find(victim_key);
			if (idx_it != index_.end())
			{
				eraseLocked(idx_it->second);
			}
			else
			{
				// index_ and group_members_ are kept in sync by eraseLocked,
				// so this shouldn't happen — guard against desync rather
				// than looping forever on a stale entry.
				it->second.pop_front();
			}
		}
	}
};