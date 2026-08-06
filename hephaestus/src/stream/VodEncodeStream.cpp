#include "VodEncodeStream.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <thread>

namespace
{
	int64_t nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	// A segment just short of a head's own generated frontier is still worth
	// waiting on rather than spawning a redundant head for — same tolerance
	// prepareSegment() always had for the single-process model.
	constexpr int kVodCatchUpMarginSegments = 4;

	// Live heads (spawned, not yet fully torn down) this stream will hold onto
	// at once — a live head keeps its hardware encoder slot (NVENC/VAAPI) even
	// while paused (see FfmpegProcess::pause()'s own doc comment), so this isn't
	// just a process-count nicety, it's what keeps a pathological scrub pattern
	// from starving every other session's transcode of slots.
	constexpr int kVodMaxLiveHeads = 2;
}

VodEncodeStream::VodEncodeStream(std::string label, std::string segment_dir, std::string segment_prefix,
								 ArgsBuilder argsBuilder, int buffer_size, bool ffmpeg_debug_logs,
								 bool verbose_transcode_logs, int lookahead_secs, int hls_time_secs,
								 int head_window_segments)
	: label_(std::move(label))
	, segment_dir_(std::move(segment_dir))
	, segment_prefix_(std::move(segment_prefix))
	, args_builder_(std::move(argsBuilder))
	, buffer_size_(buffer_size)
	, ffmpeg_debug_logs_(ffmpeg_debug_logs)
	, verbose_transcode_logs_(verbose_transcode_logs)
	, lookahead_secs_(lookahead_secs)
	, hls_time_secs_(hls_time_secs)
	, head_window_segments_(head_window_segments)
{
}

VodEncodeStream::~VodEncodeStream() { stop(); }

std::string VodEncodeStream::segmentPath(int index) const
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%05d.ts", index);
	return segment_dir_ + "/" + segment_prefix_ + buf;
}

VodEncodeStream::Head* VodEncodeStream::findCoveringHead(int segment_index)
{
	Head* fallback = nullptr;
	for (auto& h : heads_)
	{
		if (segment_index < h->start_segment || segment_index >= h->window_end_segment) continue;
		if (segment_index <= h->highest_generated.load()) return h.get(); // already ready — best case
		if (!fallback) fallback = h.get();
	}
	return fallback;
}

void VodEncodeStream::evictOneIfAtCap(std::vector<std::unique_ptr<Head>>& graveyard)
{
	if (static_cast<int>(heads_.size()) < kVodMaxLiveHeads) return;
	// Least-recently-requested head goes — the one nobody's actually near
	// anymore, not necessarily the oldest one spawned.
	auto victim = std::min_element(heads_.begin(), heads_.end(), [](const auto& a, const auto& b)
	{
		return a->last_requested_at_ms.load() < b->last_requested_at_ms.load();
	});
	if (victim == heads_.end()) return;
	std::cerr << "[vod-" << label_ << "] evicting head [" << (*victim)->start_segment << ","
		<< (*victim)->window_end_segment << ") at live-head cap (" << kVodMaxLiveHeads << ")\n";
	graveyard.push_back(std::move(*victim)); // killed (blocking) once mtx_ is released — see header comment
	heads_.erase(victim);
}

VodEncodeStream::Head* VodEncodeStream::spawnHead(int segment_index, int64_t position_ms,
												  const std::vector<int64_t>& segment_start_ms, int total_segments,
												  std::vector<std::unique_ptr<Head>>& graveyard)
{
	int window_end = std::min(segment_index + head_window_segments_, total_segments);
	// Clamp against any other still-live head's own start — two live heads
	// must never claim overlapping segment ranges: segmentPath() is one flat
	// filename numbering shared by every head on this stream, so overlap
	// means two ffmpeg processes writing the same file. The head this call
	// is directly superseding for segment_index (if any) is evicted by
	// prepareSegment() right after this returns, so it doesn't need
	// accounting for here — this guards against a *different*, unrelated
	// live head whose window would otherwise still reach into this one's.
	for (auto& h : heads_)
	{
		if (h->start_segment > segment_index && h->start_segment < window_end) window_end = h->start_segment;
	}
	std::optional<double> window_duration_secs;
	if (window_end < total_segments) window_duration_secs = (segment_start_ms[static_cast<size_t>(window_end)] - position_ms) / 1000.0;

	auto trySpawn = [&](std::shared_ptr<std::atomic<bool>> exitedFlag) -> std::unique_ptr<FfmpegProcess>
	{
		auto args = args_builder_(segment_index, position_ms, window_duration_secs);
		auto proc = std::make_unique<FfmpegProcess>(
			std::move(args),
			/*on_data=*/nullptr, // output goes to disk, not stdout
			[exitedFlag](int) { exitedFlag->store(true); },
			buffer_size_, ffmpeg_debug_logs_, verbose_transcode_logs_
		);
		return proc->start() ? std::move(proc) : nullptr;
	};

	auto head                = std::make_unique<Head>();
	head->start_segment      = segment_index;
	head->window_end_segment = window_end;
	head->last_requested.store(segment_index);
	head->last_requested_at_ms.store(nowMs());

	// Spawn before evicting anyone — same overlap reasoning as VodSession's
	// own restartAt(): a doomed-to-be-evicted head's output is already
	// superseded, so there's no reason to eat its kill() teardown wait
	// before this one even starts existing.
	head->ffmpeg = trySpawn(head->exited_naturally);
	if (!head->ffmpeg)
	{
		std::cerr << "[vod-" << label_ << "] failed to spawn head at segment " << segment_index << "\n";
		return nullptr;
	}

	// Short liveness probe — same rationale/timing as VodSession's own
	// overlap check: an NVENC/VAAPI slot-acquisition failure surfaces fast.
	// Only matters here if we're actually at the live-head cap (otherwise
	// there's no eviction decision riding on it); still worth doing
	// unconditionally so a doomed head never gets counted as a real one.
	constexpr int kProbeMs = 150, kPollMs = 25;
	int waited             = 0;
	while (waited < kProbeMs && head->ffmpeg->isAlive())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
		waited += kPollMs;
	}
	if (!head->ffmpeg->isAlive())
	{
		std::cerr << "[vod-" << label_ << "] head at segment " << segment_index
			<< " died immediately (likely no free hardware encoder session) — evicting oldest live head and retrying\n";
		head->ffmpeg.reset();
		evictOneIfAtCap(graveyard);
		// If we weren't at the cap, evictOneIfAtCap() was a no-op and this
		// retry is against genuinely exhausted hardware — same terminal
		// failure shape restartAt() already has for that case.
		head->ffmpeg = trySpawn(head->exited_naturally);
		if (!head->ffmpeg)
		{
			std::cerr << "[vod-" << label_ << "] retry also failed to spawn head at segment " << segment_index << "\n";
			return nullptr;
		}
	}
	else
	{
		evictOneIfAtCap(graveyard);
	}

	heads_.push_back(std::move(head));
	return heads_.back().get();
}

VodEncodeStream::SegmentPrep VodEncodeStream::prepareSegment(int segment_index, const std::vector<int64_t>& segment_start_ms, int total_segments)
{
	if (segment_index < 0 || segment_index >= total_segments) return SegmentPrep::Failed;

	// Declared before the lock_guard below (not inside it) so it destructs —
	// and only then blocks on each evicted head's teardown — after mtx_ is
	// already released, regardless of which return statement below fires.
	// See spawnHead()'s/evictOneIfAtCap()'s own header comment for why this
	// matters: mtx_ is shared across every viewer of this content.
	std::vector<std::unique_ptr<Head>> graveyard;
	{
		std::lock_guard<std::mutex> lock(mtx_);

		// A file already on disk is always safe to serve regardless of
		// whether any live head currently "declares" it — reaped heads leave
		// their segments servable (see tick()'s own comment), and direct-
		// stream's real keyframe cadence can make a head write past its own
		// declared window_end_segment (see the overrun check in tick()).
		// Checking this first avoids spawning a redundant second writer for
		// an index another (possibly already-gone, possibly still-
		// overrunning) process already produced.
		if (std::filesystem::exists(segmentPath(segment_index))) return SegmentPrep::Ready;

		Head* head = findCoveringHead(segment_index);
		if (head)
		{
			head->last_requested.store(segment_index);
			head->last_requested_at_ms.store(nowMs());

			int generated = head->highest_generated.load();
			if (segment_index <= generated)
			{
				if (head->ffmpeg && head->ffmpeg->isPaused()) head->ffmpeg->resume();
				return SegmentPrep::Ready;
			}
			// (A segment that exists on disk but hasn't been picked up by
			// tick()'s ~2s scan yet is already handled by the exists() check
			// above, before this head was even looked up.)
			if (!head->exited_naturally->load() && segment_index <= generated + kVodCatchUpMarginSegments)
			{
				if (head->ffmpeg && head->ffmpeg->isPaused()) head->ffmpeg->resume();
				return SegmentPrep::WaitShort;
			}
			// This head's own run can't produce it soon (exhausted, or too
			// far behind within its own declared window) — fall through and
			// spawn a fresh head starting here. Whatever this head already
			// produced stays servable from disk regardless of what happens
			// to it next; it's evicted below (after the new head is
			// confirmed alive) rather than left running, since it would
			// otherwise go on writing into the same absolute segment-index
			// range — one flat filename numbering shared by every head on
			// this stream (segmentPath()) — that the fresh head is about to
			// claim.
		}

		Head* new_head = spawnHead(segment_index, segment_start_ms[static_cast<size_t>(segment_index)],
								   segment_start_ms, total_segments, graveyard);
		if (!new_head) return SegmentPrep::Failed;

		// Evict the stale/lagging head this one directly supersedes for
		// segment_index (see the fallthrough comment above) — spawn-then-
		// kill, same overlap-avoidance ordering evictOneIfAtCap() already
		// uses, so this never pays the old head's teardown wait serially.
		// spawnHead()'s own evictOneIfAtCap() only evicts by live-head-cap/
		// LRU, which has no notion of range overlap, so this is the only
		// place that actually guarantees no two live heads ever claim
		// overlapping segment ranges for the "same head, now too far behind"
		// case (spawnHead()'s own window-end clamp handles the "different,
		// unrelated head" case).
		if (head && !head->exited_naturally->load())
		{
			auto it = std::find_if(heads_.begin(), heads_.end(),
								   [&](const std::unique_ptr<Head>& h) { return h.get() == head; });
			if (it != heads_.end() && it->get() != new_head)
			{
				graveyard.push_back(std::move(*it));
				heads_.erase(it);
			}
		}

		return SegmentPrep::WaitColdStart;
	}
}

void VodEncodeStream::tick(int total_segments)
{
	// Declared before the lock_guard below for the same reason
	// prepareSegment() does — see spawnHead()'s/evictOneIfAtCap()'s own
	// header comment: mtx_ is shared across every viewer of this content, so
	// killing (blocking) while still holding it would stall all of them.
	std::vector<std::unique_ptr<Head>> graveyard;
	{
		std::lock_guard<std::mutex> lock(mtx_);

		for (auto& h : heads_)
		{
			int idx = h->highest_generated.load() + 1;
			while (idx < h->window_end_segment && idx < total_segments && std::filesystem::exists(segmentPath(idx)))
			{
				h->highest_generated.store(idx);
				++idx;
			}
		}

		// Direct-stream's real keyframe cadence doesn't always match the
		// assumed-uniform one a head's window was sized against when no real
		// keyframe data was available (see VodSession::
		// computeSegmentBoundaries()'s fallback) — ffmpeg itself has no idea
		// window_end_segment exists, so if real cuts land more often than
		// assumed, it just keeps writing past it, into index territory a
		// different head could legitimately claim as its own start
		// (spawnHead()'s window-end clamp only guards against overlap it
		// knows about at spawn time). Stopping it (moved to graveyard, not
		// killed inline — see above) the moment that's detected bounds the
		// exposure to one tick interval instead of the rest of its
		// -t-bounded run — and removing it here, not just marking it, means
		// it can't also be reaped 30s later by the idle-reap pass below;
		// this pass already owns it.
		heads_.erase(std::remove_if(heads_.begin(), heads_.end(), [&](std::unique_ptr<Head>& h)
		{
			if (!h->ffmpeg || h->exited_naturally->load() || h->window_end_segment >= total_segments) return false;
			if (!std::filesystem::exists(segmentPath(h->window_end_segment))) return false;
			std::cerr << "[vod-" << label_ << "] head [" << h->start_segment << "," << h->window_end_segment
				<< ") overran its declared window (real segment cadence denser than assumed) — stopping it early\n";
			graveyard.push_back(std::move(h));
			return true;
		}), heads_.end());

		for (auto& h : heads_)
		{
			if (!h->ffmpeg) continue;
			int generated = h->highest_generated.load();
			if (generated >= h->window_end_segment - 1) continue; // this head's whole window is done — nothing left to pace

			int floor                     = std::max(h->last_requested.load(), h->start_segment);
			int window_segments           = std::max(1, lookahead_secs_ / hls_time_secs_);
			int resume_threshold_segments = std::max(1, window_segments / 2); // hysteresis: pause at full window, resume at half
			bool should_pause             = (generated - floor) >= window_segments;
			bool should_resume            = (generated - floor) < resume_threshold_segments;

			if (should_pause && !h->ffmpeg->isPaused()) h->ffmpeg->pause();
			else if (should_resume && h->ffmpeg->isPaused()) h->ffmpeg->resume();
		}

		// Reap heads that finished their whole window (or crashed) and
		// haven't been asked for anything in a while — their segments stay
		// on disk (still servable, still referenced by the static playlist)
		// regardless of whether the head object itself sticks around.
		constexpr int64_t kHeadIdleReapMs = 30'000;
		int64_t now                       = nowMs();
		heads_.erase(std::remove_if(heads_.begin(), heads_.end(), [&](std::unique_ptr<Head>& h)
		{
			bool finished = h->highest_generated.load() >= h->window_end_segment - 1 || h->exited_naturally->load();
			if (!finished) return false;
			if (now - h->last_requested_at_ms.load() < kHeadIdleReapMs) return false;
			graveyard.push_back(std::move(h));
			return true;
		}), heads_.end());
	}
}

bool VodEncodeStream::anyHeadPaused() const
{
	std::lock_guard<std::mutex> lock(mtx_);
	for (auto& h : heads_) if (h->ffmpeg && h->ffmpeg->isPaused()) return true;
	return false;
}

int VodEncodeStream::liveHeadCount() const
{
	std::lock_guard<std::mutex> lock(mtx_);
	return static_cast<int>(heads_.size());
}

int VodEncodeStream::highestGeneratedSegment() const
{
	std::lock_guard<std::mutex> lock(mtx_);
	int best = -1;
	for (auto& h : heads_) best = std::max(best, h->highest_generated.load());
	return best;
}

std::vector<std::pair<int, int>> VodEncodeStream::headWindowsForTest() const
{
	std::lock_guard<std::mutex> lock(mtx_);
	std::vector<std::pair<int, int>> windows;
	windows.reserve(heads_.size());
	for (auto& h : heads_) windows.emplace_back(h->start_segment, h->window_end_segment);
	return windows;
}

void VodEncodeStream::stop()
{
	std::lock_guard<std::mutex> lock(mtx_);
	for (auto& h : heads_)
	{
		if (h->ffmpeg) h->ffmpeg->kill();
	}
	heads_.clear();
}