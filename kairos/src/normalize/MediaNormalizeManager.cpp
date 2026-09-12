#include "MediaNormalizeManager.h"
#include "MediaNormalizer.h"
#include "conf/ConfStore.h"
#include "db/ContentRepository.h"
#include "db/Database.h"
#include "db/SourceRepository.h"
#include "source/MediaProbe.h"
#include "source/SyncManager.h"
#include "thread/TaskRegistry.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace
{
	// Same fingerprint SyncManager writes for Hephaestus's snapToKeyframe
	// cache-hit check — duplicated locally rather than exposing SyncManager's
	// file-local helper (codebase convention, see MediaNormalizer.cpp).
	struct FileFingerprint
	{
		int64_t size = 0, mtime = 0;
	};

	FileFingerprint statFingerprint(const fs::path& p)
	{
		std::error_code ec;
		FileFingerprint fp;
		auto sz = fs::file_size(p, ec);
		if (!ec) fp.size = static_cast<int64_t>(sz);
		auto ftime = fs::last_write_time(p, ec);
		if (!ec)
		{
			auto sctp = std::chrono::clock_cast<std::chrono::system_clock>(ftime);
			fp.mtime  = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(sctp.time_since_epoch()).count());
		}
		return fp;
	}

	// tmp/backup siblings keep the original extension so ffmpeg can infer a
	// muxer from the tmp path, and so nothing about the swap ever needs a DB
	// file_path update (same directory, same final name).
	std::string siblingPath(const std::string& path, const std::string& suffix)
	{
		fs::path p(path);
		return (p.parent_path() / (p.stem().string() + suffix + p.extension().string())).string();
	}
}

MediaNormalizeManager::MediaNormalizeManager(Database& db, ConfStore& conf, SyncManager& sync)
	: db_(db)
	, conf_(conf)
	, sync_(sync)
{
}

bool MediaNormalizeManager::triggerNormalize()
{
	bool expected = false;
	if (!running_.compare_exchange_strong(expected, true)) return false;
	TaskRegistry::global().spawn([this]()
	{
		try { run(); }
		catch (const std::exception& e) { std::cerr << "[normalize] error: " << e.what() << std::endl; }
		running_.store(false);
	});
	return true;
}

void MediaNormalizeManager::run()
{
	ContentRepository content(db_);
	SourceRepository source(db_);
	const auto episodes = content.getAllEpisodesForNormalize("", "");
	const auto movies   = content.getAllMoviesForNormalize("", "");
	std::cout << "[normalize] starting: " << episodes.size() << " episode(s), "
		<< movies.size() << " movie(s) to check\n";

	int checked     = 0, normalized = 0, failed = 0;
	auto processOne = [&](const std::string& item_type, const ContentRepository::NormalizeCandidateRow& row)
	{
		++checked;
		auto resolved = source.resolveItemSource(item_type, row.kairos_id);
		if (!resolved || resolved->file_path.empty()) return;
		const std::string path = conf_.applyPathMap(resolved->file_path);

		CodecSummary summary = probeCodecSummary(path);
		if (!needsNormalize(summary)) return;

		const std::string tmp = siblingPath(path, ".normalize.tmp");
		const std::string bak = siblingPath(path, ".normalize.bak");
		std::error_code ec;

		if (!runNormalizeEncode(path, tmp, summary.r_frame_rate) ||
			!verifyNormalizedOutput(tmp, row.duration_ms))
		{
			std::cerr << "[normalize] FAILED, leaving original untouched: " << path << "\n";
			fs::remove(tmp, ec);
			++failed;
			return;
		}

		// Swap via a backup rename rather than delete-then-rename, so there's
		// never a moment where `path` points at nothing.
		fs::rename(path, bak, ec);
		if (ec)
		{
			std::cerr << "[normalize] could not move original aside, aborting swap: " << path << "\n";
			fs::remove(tmp, ec);
			++failed;
			return;
		}
		fs::rename(tmp, path, ec);
		if (ec)
		{
			std::cerr << "[normalize] could not install normalized file, restoring original: " << path << "\n";
			fs::rename(bak, path, ec);
			fs::remove(tmp, ec);
			++failed;
			return;
		}
		fs::remove(bak, ec);

		// Re-probe the replaced file so cached resolution/keyframes/language
		// fields (and Hephaestus's snapToKeyframe cache-hit check) don't go
		// stale against the new encode.
		FileProbeInfo probed    = probeFileInfo(path);
		const auto fp           = statFingerprint(path);
		const std::string table = item_type == "movie" ? "movie" : "episode";
		const std::string idCol = item_type == "movie" ? "movie_id" : "episode_id";
		SQLite::Statement upd(db_.get(),
							  "UPDATE " + table + " SET duration_ms = ?, resolution_label = ?, "
							  "audio_languages = ?, embedded_subtitle_languages = ?, "
							  "keyframes_ms = ?, keyframes_size = ?, keyframes_mtime = ? WHERE " + idCol + " = ?");
		upd.bind(1, probed.duration_ms > 0 ? probed.duration_ms : row.duration_ms);
		upd.bind(2, bucketResolutionLabel(probed.video.height));
		upd.bind(3, nlohmann::json(probed.langs.audio).dump());
		upd.bind(4, nlohmann::json(probed.langs.subtitle).dump());
		upd.bind(5, nlohmann::json(probed.keyframes_ms).dump());
		upd.bind(6, fp.size);
		upd.bind(7, fp.mtime);
		upd.bind(8, row.kairos_id);
		upd.exec();

		std::cout << "[normalize] replaced " << item_type << " " << row.kairos_id
			<< " (video=" << summary.video_codec << " audio=" << summary.audio_codec
			<< " vfr=" << summary.likely_vfr << "): " << path << "\n";
		++normalized;
	};

	for (const auto& row : episodes) processOne("episode", row);
	for (const auto& row : movies) processOne("movie", row);

	std::cout << "[normalize] done: " << checked << " checked, " << normalized
		<< " normalized, " << failed << " failed\n";
}