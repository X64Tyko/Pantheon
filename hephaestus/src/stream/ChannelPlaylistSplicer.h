#pragma once
#include "cache/SegmentCache.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstdint>

// Sole writer of a live channel's canonical playlist.m3u8 + segment files.
// Each item's segments are produced by a VodEncodeStream into its own private
// "pending" directory (never touching canonical) — this class relays
// finished, *wall-clock-due* segments from whichever pending directory is
// currently active into canonical (via hard-link, so no data is ever
// copied) under its own continuous segment numbering, and authors
// canonical's playlist.m3u8 itself, including a correct
// #EXT-X-DISCONTINUITY-SEQUENCE as old discontinuities roll off the window.
// Having exactly one writer of canonical eliminates the write-write race a
// prior on-disk patch approach had with ffmpeg's own muxer (see project
// memory) without needing any locking — there's nothing left to race.
//
// The producer is allowed to race ahead of real time (VodEncodeStream has no
// -re pacing) — pacing to true wall-clock happens entirely here, in what
// gets *revealed*, not in how fast segments get produced. Also the sole
// drift-correction mechanism now: every tick recomputes what's actually due
// from true elapsed wall-clock time, so unlike a one-time speed decision
// made at item start, it can't accumulate drift.
//
// spliceTo() is only ever called after the outgoing pending source's ffmpeg
// process has already exited (see ChannelSession::onExit()/transition()), so
// its content is final by the time this class sees it — no attempt is made
// to relay from a still-writing outgoing source.
class ChannelPlaylistSplicer
{
public:
	// canonical_dir must already exist. list_size/delete_threshold mirror the
	// same two ffmpeg hls_flags concepts (visible playlist window vs. how much
	// extra stays on disk as a grace margin for in-flight clients) they
	// replaced. reveal_lead_ms is how far ahead of true "now" a segment may be
	// revealed once it's due — without this, a segment would appear in the
	// manifest at the same instant a real-time-synced client needs to start
	// playing it, leaving no margin to fetch/buffer (this is ordinary
	// broadcast delay, not a hack). tick_interval is the background relay
	// loop's cadence. segment_cache may be null (segment caching disabled) —
	// when non-null, every segment this class prunes from canonical also
	// gets invalidated out of the cache so a viewer request occurring right
	// after eviction can't get memory-pinned indefinitely on content the
	// splicer itself already considers gone.
	ChannelPlaylistSplicer(std::string canonical_dir, int list_size, int delete_threshold,
						   int64_t reveal_lead_ms, std::chrono::milliseconds tick_interval,
						   SegmentCache* segment_cache = nullptr);
	~ChannelPlaylistSplicer();

	void start();
	void stop();

	// Everything the splicer needs to relay one item's spawn — segment
	// durations/boundaries are precomputed upfront (same technique
	// VodSession::computeSegmentBoundaries() already uses: a uniform cadence
	// for transcoded content, or predicted real keyframe cut points for
	// direct-stream — see simulateDirectStreamSegmentBoundaries(),
	// EncoderArgs.h), never discovered from ffmpeg's output after the fact.
	struct SpawnInfo
	{
		std::string pending_dir;                    // this spawn's VodEncodeStream segment_dir_
		std::string segment_prefix;                 // this spawn's VodEncodeStream segment_prefix_ (e.g. "seg-")
		std::vector<int64_t> segment_boundaries_ms; // 0-based, ascending, offset within this spawn
		int64_t item_duration_ms;                   // total span — implicit end boundary for the last segment
		int64_t wall_clock_start_ms;                // real time corresponding to offset 0 of this spawn
	};

	// Switches which spawn's segments get relayed into canonical. Drains
	// whatever's due from the outgoing source first (already final — see
	// class comment), then immediately relays as much of the new source's
	// already-produced-and-due backlog as exists — this backlog, built up
	// during a preroll lead window, is what makes a pre-rolled transition
	// gapless instead of a cold start. A no-op if already relaying this dir.
	void spliceTo(SpawnInfo info);

	// Pure per-tick relay logic exposed for testing without a real
	// background thread or real ffmpeg — see
	// test_channel_playlist_splicer.cpp.
	void relayTickForTest()
	{
		std::lock_guard<std::mutex> lock(mtx_);
		relayTickLocked();
	}

	std::string canonicalPlaylistForTest() const;

private:
	void relayLoop();
	void relayTickLocked(); // caller must hold mtx_

	std::string canonical_dir_;
	int list_size_;
	int delete_threshold_;
	int64_t reveal_lead_ms_;
	std::chrono::milliseconds tick_interval_;
	SegmentCache* segment_cache_;

	mutable std::mutex mtx_;
	SpawnInfo active_;
	size_t relayed_count_              = 0;
	bool pending_discontinuity_marker_ = false;
	bool have_active_                  = false;

	struct CanonicalSegment
	{
		int64_t seq;
		double duration;
		std::string filename;
		bool discontinuity_before;
	};

	std::deque<CanonicalSegment> segments_;
	int64_t next_seq_                   = 0;
	int64_t discontinuities_rolled_off_ = 0;
	double max_duration_ever_           = 0;

	std::thread thread_;
	std::atomic<bool> stop_flag_{false};
};