#include "ChannelPlaylistSplicer.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <chrono>

namespace
{
	int64_t nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	std::string segFilename(const std::string& prefix, int64_t index)
	{
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%05lld.ts", static_cast<long long>(index));
		return prefix + buf;
	}
} // namespace

ChannelPlaylistSplicer::ChannelPlaylistSplicer(std::string canonical_dir, int list_size, int delete_threshold,
											   int64_t reveal_lead_ms, std::chrono::milliseconds tick_interval)
	: canonical_dir_(std::move(canonical_dir))
	, list_size_(list_size)
	, delete_threshold_(delete_threshold)
	, reveal_lead_ms_(reveal_lead_ms)
	, tick_interval_(tick_interval)
{
}

ChannelPlaylistSplicer::~ChannelPlaylistSplicer()
{
	stop();
}

void ChannelPlaylistSplicer::start()
{
	thread_ = std::thread([this] { relayLoop(); });
}

void ChannelPlaylistSplicer::stop()
{
	stop_flag_.store(true);
	if (thread_.joinable()) thread_.join();
}

void ChannelPlaylistSplicer::relayLoop()
{
	while (!stop_flag_.load())
	{
		std::this_thread::sleep_for(tick_interval_);
		if (stop_flag_.load()) return;
		std::lock_guard<std::mutex> lock(mtx_);
		relayTickLocked();
	}
}

void ChannelPlaylistSplicer::spliceTo(SpawnInfo info)
{
	std::lock_guard<std::mutex> lock(mtx_);
	if (have_active_&& info
	.
	pending_dir == active_.pending_dir
	)
	return;

	// Drain whatever's due from the outgoing source first — its ffmpeg has
	// already exited by the time a caller reaches this (see class comment),
	// so this is its complete, final content, not a snapshot mid-write.
	if (have_active_) relayTickLocked();

	active_        = std::move(info);
	have_active_   = true;
	relayed_count_ = 0;
	// No discontinuity marker needed for the very first spawn a fresh
	// splicer ever relays — there's nothing before it to be discontinuous with.
	pending_discontinuity_marker_ = !segments_.empty();

	// Relay as much of the new source's own already-accumulated (and due)
	// backlog as exists right now — this is what makes a pre-rolled
	// transition gapless.
	relayTickLocked();
}

void ChannelPlaylistSplicer::relayTickLocked()
{
	if (!have_active_) return;

	int64_t dueOffsetMs = nowMs() - active_.wall_clock_start_ms + reveal_lead_ms_;
	const auto& bounds  = active_.segment_boundaries_ms;

	while (relayed_count_ < bounds.size() && bounds[relayed_count_] <= dueOffsetMs)
	{
		int64_t index        = static_cast<int64_t>(relayed_count_);
		std::string filename = segFilename(active_.segment_prefix, index);
		std::string src      = active_.pending_dir + "/" + filename;

		if (!std::filesystem::exists(src)) break; // producer hasn't caught up yet — stop, don't skip ahead

		int64_t endMs   = (relayed_count_ + 1 < bounds.size()) ? bounds[relayed_count_ + 1] : active_.item_duration_ms;
		double duration = std::max(0.1, (endMs - bounds[relayed_count_]) / 1000.0);

		std::string canonicalName = segFilename("seg-", next_seq_);
		std::string dst           = canonical_dir_ + "/" + canonicalName;

		std::error_code ec;
		std::filesystem::create_hard_link(src, dst, ec);
		if (ec)
		{
			std::cerr << "[splicer] failed to hard-link " << src << " -> " << dst << ": " << ec.message() << "\n";
			break; // best-effort — stop rather than skip a numbered gap into canonical
		}
		std::filesystem::remove(src, ec); // pending dir is scratch space; splicer is its sole pruner

		segments_.push_back({next_seq_, duration, canonicalName, pending_discontinuity_marker_});
		pending_discontinuity_marker_ = false;
		max_duration_ever_            = std::max(max_duration_ever_, duration);
		++next_seq_;
		++relayed_count_;
	}

	// Physical retention (list_size + delete_threshold) vs. what's actually
	// listed in the playlist text (list_size) — same two-tier grace-margin
	// shape as the ffmpeg hls_flags this replaced (see project memory).
	while (static_cast<int>(segments_.size()) > list_size_ + delete_threshold_)
	{
		auto& oldest = segments_.front();
		if (oldest.discontinuity_before) ++discontinuities_rolled_off_;
		std::error_code ec;
		std::filesystem::remove(canonical_dir_ + "/" + oldest.filename, ec);
		segments_.pop_front();
	}

	// ── Write canonical playlist.m3u8 (temp_file+rename — sole writer now) ──
	size_t visibleCount = std::min(segments_.size(), static_cast<size_t>(list_size_));
	size_t firstVisible = segments_.size() - visibleCount;

	std::ostringstream out;
	out << "#EXTM3U\n#EXT-X-VERSION:3\n";
	out << "#EXT-X-TARGETDURATION:" << static_cast<int64_t>(std::ceil(max_duration_ever_)) << "\n";
	out << "#EXT-X-MEDIA-SEQUENCE:" << (visibleCount ? segments_[firstVisible].seq : next_seq_) << "\n";
	out << "#EXT-X-DISCONTINUITY-SEQUENCE:" << discontinuities_rolled_off_ << "\n";
	for (size_t i = firstVisible; i < segments_.size(); ++i)
	{
		if (segments_[i].discontinuity_before) out << "#EXT-X-DISCONTINUITY\n";
		out << "#EXTINF:" << std::fixed << std::setprecision(6) << segments_[i].duration << ",\n";
		out << segments_[i].filename << "\n";
	}

	std::string tmpPath = canonical_dir_ + "/playlist.m3u8.tmp";
	{
		std::ofstream f(tmpPath, std::ios::trunc);
		if (!f) return;
		f << out.str();
	}
	std::error_code ec;
	std::filesystem::rename(tmpPath, canonical_dir_ + "/playlist.m3u8", ec);
}

std::string ChannelPlaylistSplicer::canonicalPlaylistForTest() const
{
	std::ifstream in(canonical_dir_ + "/playlist.m3u8");
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}