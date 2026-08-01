#include "ChannelViewerRegistry.h"
#include "ChannelSession.h"
#include "SessionManager.h"
#include "thread/TaskRegistry.h"
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

bool isChannelDirectStreamable(const MediaInfo& info, int audio_track, const ClientCapabilities& caps)
{
	if (info.video.empty()) return false;
	const auto& v = info.video[0];
	if (caps.max_height && v.height > *caps.max_height) return false;
	if (!caps.video_codecs.count(v.codec)) return false;
	// A stream copy has no filter graph to run -fps_mode cfr through (see
	// ChannelSession.cpp's own comment on that flag) — a VFR source (common
	// on older syndicated TV masters with baked-in telecine/pulldown judder)
	// needs the transcode bucket to get its frame timing normalized, or its
	// irregular timestamps ride straight through into stutter/rebuffering.
	if (isLikelyVfr(v)) return false;

	// No client audio-codec check here (unlike video's caps.video_codecs
	// check above) — ChannelSession::buildArgs now always re-encodes audio
	// for this bucket rather than copying it (see its own comment on why:
	// loudnorm needs a real filter graph), so the source's audio codec no
	// longer has any bearing on eligibility. Still confirms the requested
	// track actually exists — nothing to stream otherwise.
	auto ai = std::find_if(info.audio.begin(), info.audio.end(),
						   [&](const AudioTrack& t) { return t.relative_index == audio_track; });
	if (ai == info.audio.end()) return false;

	return true;
}

namespace
{
	std::string generateViewerId()
	{
		thread_local std::mt19937_64 rng(std::random_device{}());
		std::ostringstream ss;
		ss << std::hex << std::setfill('0') << std::setw(16) << rng();
		return ss.str();
	}

	int64_t nowMs()
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch()).count();
	}

	constexpr int64_t kMaxIdleMs = 90'000;
}

ChannelViewerRegistry::ChannelViewerRegistry(SessionManager& sessions)
	: sessions_(sessions)
{
	reaper_thread_ = std::thread([this] { reapLoop(); });
}

ChannelViewerRegistry::~ChannelViewerRegistry()
{
	stop_reaper_.store(true);
	if (reaper_thread_.joinable()) reaper_thread_.join();
}

void ChannelViewerRegistry::reapLoop()
{
	while (!stop_reaper_.load())
	{
		for (int i = 0; i < 5 && !stop_reaper_.load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
		if (stop_reaper_.load()) break;

		int64_t cutoff = nowMs() - kMaxIdleMs;
		std::lock_guard<std::mutex> lock(mtx_);
		for (auto it = viewers_.begin(); it != viewers_.end();) it = (it->second.last_seen_ms < cutoff) ? viewers_.erase(it) : std::next(it);
	}
}

ChannelViewerRegistry::StartResult ChannelViewerRegistry::start(
	const std::string& channel_id, const ClientCapabilities& caps,
	const MediaInfo* info, int audio_track)
{
	std::string bucket = (info && isChannelDirectStreamable(*info, audio_track, caps))
							 ? ChannelSession::kNativeBucket
							 : ChannelSession::kDefaultBucket;

	std::string id = generateViewerId();
	{
		std::lock_guard<std::mutex> lock(mtx_);
		viewers_[id] = ViewerEntry{channel_id, caps, bucket, nowMs()};
	}

	if (bucket == ChannelSession::kNativeBucket) sessions_.getOrCreate(channel_id, bucket); // spins up if this is the first native viewer

	return {id, bucket};
}

std::string ChannelViewerRegistry::touch(const std::string& viewer_session_id, std::string& channel_id_out)
{
	std::lock_guard<std::mutex> lock(mtx_);
	auto it = viewers_.find(viewer_session_id);
	if (it == viewers_.end()) return "";
	it->second.last_seen_ms = nowMs();
	channel_id_out          = it->second.channel_id;
	return it->second.bucket;
}

void ChannelViewerRegistry::stop(const std::string& viewer_session_id)
{
	std::lock_guard<std::mutex> lock(mtx_);
	viewers_.erase(viewer_session_id);
}

std::map<std::string, std::map<std::string, int>> ChannelViewerRegistry::viewerCounts() const
{
	std::map<std::string, std::map<std::string, int>> out;
	std::lock_guard<std::mutex> lock(mtx_);
	for (auto& [id, entry] : viewers_) out[entry.channel_id][entry.bucket]++;
	return out;
}

void ChannelViewerRegistry::reassignForChannel(const std::string& channel_id,
											   const std::optional<MediaInfo>& info, int audio_track)
{
	bool need_native = false;
	{
		std::lock_guard<std::mutex> lock(mtx_);
		for (auto& [id, entry] : viewers_)
		{
			if (entry.channel_id != channel_id) continue;
			std::string ideal = (info && isChannelDirectStreamable(*info, audio_track, entry.caps))
									? ChannelSession::kNativeBucket
									: ChannelSession::kDefaultBucket;
			entry.bucket = ideal;
			if (ideal == ChannelSession::kNativeBucket) need_native = true;
		}
	}
	// Ensure the native bucket session exists if anyone now needs it — a
	// no-op (existing active session returned) if it's already running.
	//
	// Dispatched asynchronously, NOT called inline: this function can itself
	// run synchronously inside SessionManager::getOrCreate()'s own call to
	// ChannelSession::start() (the fast-path item-resolution case, which is
	// the common one) — that call holds SessionManager's mtx for its whole
	// duration. Calling back into getOrCreate() from here on that same call
	// stack would try to re-lock a non-recursive mutex already held by this
	// thread (deadlock), and if that lock were dropped instead, would
	// recurse without bound (this same callback fires again from the nested
	// session's own start()). A background task breaks the cycle entirely:
	// by the time it runs, the in-flight getOrCreate() call that triggered
	// this has already returned and inserted its session, so this just
	// finds it already active — the ordinary case — instead of racing its
	// own caller.
	if (need_native)
	{
		SessionManager* sessions = &sessions_;
		std::string cid          = channel_id;
		TaskRegistry::global().spawn([sessions, cid] { sessions->getOrCreate(cid, ChannelSession::kNativeBucket); });
	}
}