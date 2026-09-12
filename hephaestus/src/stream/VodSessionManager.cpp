#include "VodSessionManager.h"
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>

static std::string generateSessionId()
{
	thread_local std::mt19937_64 rng(std::random_device{}());
	std::ostringstream ss;
	ss << std::hex << std::setfill('0') << std::setw(16) << rng();
	return ss.str();
}

// Matches SessionManager's active/idle dual cadence — see that file's comment.
static constexpr auto kActiveRefreshInterval = std::chrono::seconds(15);
static constexpr auto kIdleRefreshInterval   = std::chrono::minutes(5);

VodSessionManager::VodSessionManager(std::string ffmpeg_path, VodStreamOptions opts, KairosClient& kairos,
									 int max_sessions)
	: ffmpeg_path(std::move(ffmpeg_path))
	, opts(std::move(opts))
	, kairos(kairos)
	, max_sessions(max_sessions)
{
	reaper_thread = std::thread([this] { reapLoop(); });
	refreshSettings(); // blocking, but only once, at startup
	settings_refresh_thread = std::thread([this]
	{
		while (!stop_settings_refresh.load())
		{
			bool active;
			{
				std::lock_guard<std::mutex> lock(mtx);
				active = !sessions.empty();
			}
			auto wait = active ? kActiveRefreshInterval : kIdleRefreshInterval;
			for (auto elapsed = std::chrono::seconds(0); elapsed < wait && !stop_settings_refresh.load();
				 elapsed      += std::chrono::seconds(1))
				std::this_thread::sleep_for(std::chrono::seconds(1));
			if (stop_settings_refresh.load()) break;
			refreshSettings();
		}
	});
}

VodSessionManager::~VodSessionManager()
{
	stop_reaper.store(true);
	if (reaper_thread.joinable()) reaper_thread.join();
	stop_settings_refresh.store(true);
	if (settings_refresh_thread.joinable()) settings_refresh_thread.join();
	std::lock_guard<std::mutex> lock(mtx);
	for (auto& [id, s] : sessions) s->stop();
	sessions.clear();
}

void VodSessionManager::refreshSettings()
{
	auto verbose    = kairos.getVerboseTranscodeLogs();
	auto ffmpeg_dbg = kairos.getFfmpegDebugLogs();
	auto bs         = kairos.getBufferSize();
	std::lock_guard<std::mutex> lock(settings_mtx);
	if (verbose) cached_verbose_transcode_logs = verbose;
	if (ffmpeg_dbg) cached_ffmpeg_debug_logs = ffmpeg_dbg;
	if (bs) cached_buffer_size = *bs * 1024; // KB -> bytes
}

void VodSessionManager::pushKeyframeCache(const std::string& content_type, const std::string& content_id,
										  const std::vector<int64_t>& keyframes_ms, int64_t size, int64_t mtime)
{
	kairos.pushKeyframeCache(content_type, content_id, keyframes_ms, size, mtime);
}

std::shared_ptr<VodSession> VodSessionManager::create(const std::string& content_type, const std::string& content_id,
													  const std::string& file_path, int64_t position_ms,
													  int audio_track, int subtitle_track, bool hdr_capable,
													  const std::optional<ClientCapabilities>& client_caps,
													  const std::vector<ExternalSubtitle>& external_subtitles,
													  int64_t fallback_duration_ms,
													  const std::string& preferred_audio_lang,
													  const std::string& preferred_subtitle_lang,
													  const std::vector<int64_t>& kairos_keyframes_ms,
													  int64_t kairos_keyframes_size,
													  int64_t kairos_keyframes_mtime)
{
	// Hard cap on concurrent transcode sessions — each one spawns an
	// ffmpeg-class process, so without this an authenticated caller could
	// loop-request sessions and exhaust host CPU/GPU. A pre-check against the
	// current map size rather than a reserved-slot counter: the actual
	// session::start() work below is expensive and deliberately runs outside
	// this lock, so a handful of concurrent requests right at the boundary
	// could transiently land a few over the cap — acceptable, since the goal
	// is bounding sustained growth, not exact enforcement.
	if (max_sessions > 0)
	{
		std::lock_guard<std::mutex> lock(mtx);
		if (sessions.size() >= static_cast<size_t>(max_sessions)) return nullptr;
	}

	VodStreamOptions session_opts = opts;
	{
		std::lock_guard<std::mutex> lock(settings_mtx);
		if (cached_verbose_transcode_logs) session_opts.verbose_transcode_logs = *cached_verbose_transcode_logs;
		if (cached_ffmpeg_debug_logs) session_opts.ffmpeg_debug_logs = *cached_ffmpeg_debug_logs;
		if (cached_buffer_size > 0) session_opts.buffer_size = cached_buffer_size;
	}

	auto session = std::make_shared<VodSession>(generateSessionId(), content_type, content_id, ffmpeg_path, session_opts, *this);
	if (!session->start(file_path, position_ms, audio_track, subtitle_track, hdr_capable, client_caps,
						external_subtitles, fallback_duration_ms,
						preferred_audio_lang, preferred_subtitle_lang,
						kairos_keyframes_ms, kairos_keyframes_size, kairos_keyframes_mtime))
		return nullptr;

	std::lock_guard<std::mutex> lock(mtx);
	sessions[session->sessionId()] = session;
	return session;
}

std::shared_ptr<VodEncodeStream> VodSessionManager::getOrCreateVideoStream(
	const VideoStreamKey& key, const std::function<std::shared_ptr<VodEncodeStream>()>& factory)
{
	std::lock_guard<std::mutex> lock(stream_mtx);
	auto it = video_streams.find(key);
	if (it != video_streams.end())
	{
		if (auto existing = it->second.lock()) return existing;
	}
	auto created       = factory();
	video_streams[key] = created;
	return created;
}

std::shared_ptr<VodEncodeStream> VodSessionManager::getOrCreateAudioStream(
	const AudioStreamKey& key, const std::function<std::shared_ptr<VodEncodeStream>()>& factory)
{
	std::lock_guard<std::mutex> lock(stream_mtx);
	auto it = audio_streams.find(key);
	if (it != audio_streams.end())
	{
		if (auto existing = it->second.lock()) return existing;
	}
	auto created       = factory();
	audio_streams[key] = created;
	return created;
}

std::shared_ptr<VodSession> VodSessionManager::get(const std::string& sessionId)
{
	std::lock_guard<std::mutex> lock(mtx);
	auto it = sessions.find(sessionId);
	return (it != sessions.end() && it->second->isActive()) ? it->second : nullptr;
}

void VodSessionManager::stop(const std::string& sessionId)
{
	std::shared_ptr<VodSession> session;
	{
		std::lock_guard<std::mutex> lock(mtx);
		auto it = sessions.find(sessionId);
		if (it == sessions.end()) return;
		session = it->second;
		sessions.erase(it);
	}
	session->stop();
}

std::vector<std::shared_ptr<VodSession>> VodSessionManager::listActive()
{
	std::lock_guard<std::mutex> lock(mtx);
	std::vector<std::shared_ptr<VodSession>> out;
	for (auto& [id, session] : sessions) if (session->isActive()) out.push_back(session);
	return out;
}

void VodSessionManager::reapLoop()
{
	while (!stop_reaper.load())
	{
		for (int i = 0; i < 5 && !stop_reaper.load(); ++i) std::this_thread::sleep_for(std::chrono::seconds(1));
		if (stop_reaper.load()) break;

		{
			std::lock_guard<std::mutex> lock(mtx);
			for (auto it = sessions.begin(); it != sessions.end();)
			{
				if (!it->second->isActive() || it->second->isIdle())
				{
					it->second->stop();
					it = sessions.erase(it);
				}
				else ++it;
			}
		}
		// Expired weak_ptr entries — the streams themselves already tore
		// down (their last owning VodSession went away above, or on a track
		// switch); this just stops video_streams/audio_streams from growing
		// unbounded with dead keys over a long-running instance's lifetime.
		{
			std::lock_guard<std::mutex> lock(stream_mtx);
			for (auto it = video_streams.begin(); it != video_streams.end();) it = it->second.expired() ? video_streams.erase(it) : std::next(it);
			for (auto it = audio_streams.begin(); it != audio_streams.end();) it = it->second.expired() ? audio_streams.erase(it) : std::next(it);
		}
	}
}