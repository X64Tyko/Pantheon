#pragma once
#include "../IKairosService.h"
#include "../../model/WritebackFields.h"
#include <atomic>
#include <httplib.h>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>

class ConfStore;
class Database;
class SyncManager;
class ScraperManager;
struct ServiceContext;
struct ShowDetail;
struct MovieDetail;

class ContentService : public IKairosService
{
public:
	ContentService(const ServiceContext& ctx, ScraperManager& scraper);
	void registerRoutes(httplib::Server& svr) override;

	// Entry point for ScraperManager's MatchConfirmedCallback (wired up in
	// Router.cpp, not a direct member reference — ScraperManager can't hold
	// a ContentService& itself since ContentService already holds a
	// ScraperManager&, and the two constructing each other back-and-forth
	// would be circular). Pushes to every target whose source has
	// auto_writeback enabled; no-ops entirely if the item isn't
	// match_confirmed (mirrors the single-item routes' own gate) or if none
	// of its targets have auto_writeback on. Safe to call for an item with
	// zero targets or that doesn't exist — both just no-op.
	void autoWritebackIfEnabled(const std::string& item_type, const std::string& kairos_id);

	// Guarded async trigger — same shape as SyncManager::triggerSync/
	// ScraperManager::triggerMatch. Returns false (no-op) if a writeback
	// sweep is already running. Shared by POST /api/writeback/all and the
	// writeback_sweep scheduled job (see JobService).
	bool triggerWritebackAll(const std::string& library_id = "", const std::string& source_id = "");

private:
	Database& db_;
	ConfStore& conf_;
	SyncManager& sync_;
	ScraperManager& scraper_;

	// Guards "Writeback All" against overlapping runs — same
	// compare_exchange-then-detached-thread shape as SyncManager::
	// triggerSync's sync_running_, scoped to this service since writeback
	// isn't a sync and shouldn't share or block on SyncManager's own flag.
	std::atomic<bool> writeback_running_{false};

	void proxyImage(const httplib::Request& req, const std::string& imgPath,
					const std::string& sourceId, httplib::Response& res);

	// Resolves imgPath (CDN URL / source-relative path / "local:" sentinel —
	// same three forms proxyImage() serves) into raw bytes for writeback
	// upload. Unlike proxyImage this is a synchronous, uncached fetch — it's
	// only called from an admin-triggered writeback action, not hot-path
	// image traffic, so the on-disk image-cache machinery would be
	// pointless overhead here.
	std::optional<WritebackImage> fetchImageBytes(const std::string& imgPath,
												  const std::string& sourceId);

	// Builds WritebackFields from an already-match-confirmed show/movie and
	// pushes to every linked source, or only source_id_filter's target when
	// non-empty (used by "Writeback All"'s per-source scoping — the
	// single-item routes always pass "" for unfiltered/all-targets
	// behavior). auto_only=true additionally skips any target whose source
	// doesn't have auto_writeback enabled (used by autoWritebackIfEnabled();
	// the single-item/bulk routes always pass false — an explicit trigger
	// bypasses the auto gate entirely, it doesn't mean "auto"). Per-target
	// art/external_ids/collections toggles (MediaSourceConfig::
	// writeback_update_*) are applied here too, shaping a copy of `fields`
	// per target rather than a single shared object, since different
	// targets can have different settings. Returns a JSON array of
	// {source_id, source_type, ok}, same shape the single-item routes have
	// always returned. Caller must have already checked match_confirmed —
	// these don't re-check it, so "Writeback All" can rely on
	// getWritebackEligibleShowIds/getWritebackEligibleMovieIds having
	// already filtered for it.
	nlohmann::json writebackShow(const ShowDetail& d, const std::string& source_id_filter, bool auto_only = false);
	nlohmann::json writebackMovie(const MovieDetail& d, const std::string& source_id_filter, bool auto_only = false);

	// Background worker for POST /api/writeback/all — iterates every
	// match-confirmed show then movie (scoped per getWritebackEligible*Ids),
	// pushing each to its targets. Logs progress via std::cout, same
	// convention SyncManager::syncAll() uses (visible on the Activity page's
	// log stream) since this can run long against a large library. Clears
	// writeback_running_ on exit via the caller (see registerRoutes), not
	// internally, so a thrown exception mid-run still releases the guard.
	void runWritebackAll(const std::string& library_id, const std::string& source_id);
};