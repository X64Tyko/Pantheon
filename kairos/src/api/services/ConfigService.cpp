#include "ConfigService.h"
#include "../AuthContext.h"
#include "../RouteHelpers.h"
#include "../../conf/ConfStore.h"
#include "../../conf/GuestSettings.h"
#include "../../conf/SecuritySettings.h"
#include "../../conf/ViewerSettings.h"
#include "../../db/ConfigRepository.h"
#include "../../db/ScheduleRepository.h"
#include "../../db/SourceRepository.h"
#include "../../email/EmailService.h"
#include "../../source/SyncManager.h"
#include "../../scheduler/RuntimeFlags.h"
#include <SQLiteCpp/SQLiteCpp.h>
#include <nlohmann/json.hpp>
#include <ctime>
#include <filesystem>
#include <fstream>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

// Public pantheon-relay App ID (see /ChimeraShare/pantheon-relay) — the
// out-of-the-box default so casting works without any setup; overridable
// per-deployment via PATCH /api/config/settings for anyone self-hosting
// their own relay under their own App ID.
static constexpr const char* kDefaultCastAppId = "FA339D2D";

ConfigService::ConfigService(const ServiceContext& ctx)
	: db_(ctx.db)
	, conf_(ctx.conf)
	, sync_(ctx.sync)
	, email_(ctx.email)
{
}

void ConfigService::registerRoutes(httplib::Server& svr)
{
	auto persistFlag = [this](const char* key, bool value)
	{
		SQLite::Statement s(db_.get(),
							"INSERT INTO app_config(key,value) VALUES(?,?)"
							" ON CONFLICT(key) DO UPDATE SET value=excluded.value");
		s.bind(1, std::string(key));
		s.bind(2, value ? "1" : "0");
		s.exec();
	};

	// Empty (never configured) falls back to the shared public relay's App ID
	// so casting works out of the box — see kDefaultCastAppId.
	auto castAppId = [this]()
	{
		std::string v = ConfigRepository(db_).getValue("cast_app_id");
		return v.empty() ? std::string(kDefaultCastAppId) : v;
	};

	// Empty (never configured) falls back to Home — same "empty means
	// unconfigured, apply a sane default" shape as castAppId above. A
	// per-user default_landing_page (AuthStore) takes priority over this
	// when set; this is only the server-wide fallback.
	auto defaultLandingPage = [this]()
	{
		std::string v = ConfigRepository(db_).getValue("default_landing_page");
		return v.empty() ? std::string("home") : v;
	};

	auto settingsJson = [this, castAppId, defaultLandingPage]()
	{
		return json{
			{"epg_debug", g_epg_debug.load()},
			{"sync_debug", g_debug_logging.load()},
			{"sync_threads", sync_.getThreadCount()},
			{"stream_buffer_size", g_buffer_size.load()},
			{"image_cache_ttl_hours", conf_.getImageCacheTtlHours()},
			{"verbose_transcode_logs", g_verbose_transcode_logs.load()},
			{"verbose_gateway_logs", g_verbose_gateway_logs.load()},
			{"hades_debug", g_hades_debug.load()},
			{"cast_app_id", castAppId()},
			{"default_landing_page", defaultLandingPage()},
			{"internal_token", conf_.getInternalToken()},
			{"guest_profiles_enabled", guest_settings::enabled(db_)},
			{"guest_idle_timeout_days", guest_settings::idleTimeoutDays(db_)},
			{"guest_max_concurrent", guest_settings::maxConcurrent(db_)},
			{"guest_channel_builder_enabled", guest_settings::channelBuilderEnabled(db_)},
			{"guest_max_demo_channels", guest_settings::maxDemoChannels(db_)},
			{"viewer_max_channels", viewer_settings::maxChannels(db_)},
			// Raw stored value, not the OR-with-guest-profiles effective one —
			// this is what the Settings page's own toggle reflects/edits; it
			// shows (and forces) on whenever guest_profiles_enabled is also on,
			// but that's a UI rule the frontend applies itself from these two
			// fields together, not something baked into the value returned here.
			{"require_admin_password_switch", security_settings::requireAdminPasswordSwitchRaw(db_)},
		};
	};

	svr.Get("/api/config/settings", [this, settingsJson](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		route::ok(res, settingsJson().dump());
	});

	// A public read-only subset for internal services (Hephaestus, Hermes) and
	// the Hades frontend. This is marked public in Router.cpp, so it's
	// accessible without a token (for Hephaestus/Hermes) or with any valid
	// token (for viewer users).
	svr.Get("/api/config/public-settings", [this, castAppId, defaultLandingPage](const Req&, Res& res)
	{
		route::ok(res, json{
					  {"stream_buffer_size", g_buffer_size.load()},
					  {"verbose_transcode_logs", g_verbose_transcode_logs.load()},
					  {"verbose_gateway_logs", g_verbose_gateway_logs.load()},
					  {"cast_app_id", castAppId()},
					  {"default_landing_page", defaultLandingPage()},
					  // Only the on/off flag, not the idle-timeout/cap admin
					  // tunables — this is read pre-login (Hades' login page
					  // deciding whether to show "Continue as Guest" at all),
					  // same reasoning as every other field already here.
					  {"guest_profiles_enabled", guest_settings::enabled(db_)},
					  // Same "read pre-login/pre-auth" reasoning as
					  // guest_profiles_enabled above — ChannelsPage needs these to
					  // decide whether to show the guest demo builder UI at all, and
					  // viewer_max_channels for the real-viewer builder's own quota
					  // display.
					  {"guest_channel_builder_enabled", guest_settings::channelBuilderEnabled(db_)},
					  {"guest_max_demo_channels", guest_settings::maxDemoChannels(db_)},
					  {"viewer_max_channels", viewer_settings::maxChannels(db_)},
					  // The EFFECTIVE (already OR'd with guest_profiles_enabled)
					  // value, unlike settingsJson's raw one above — this is what
					  // ProfileSelectPage.tsx reads to decide whether an admin
					  // tile should prompt for a PIN or fall back to a real
					  // password, and it needs the same answer AuthStore::
					  // switchProfile itself enforces or the picker's own PIN
					  // prompt would just fail server-side instead of routing to
					  // the right form to begin with.
					  {"require_admin_password_switch", security_settings::requireAdminPasswordSwitchEffective(db_)},
				  }.dump());
	});

	svr.Patch("/api/config/settings", [this, persistFlag, settingsJson](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto b = json::parse(req.body);
			if (b.contains("epg_debug") && b["epg_debug"].is_boolean())
			{
				bool v = b["epg_debug"].get<bool>();
				g_epg_debug.store(v);
				persistFlag("epg_debug", v);
			}
			if (b.contains("sync_debug") && b["sync_debug"].is_boolean())
			{
				bool v = b["sync_debug"].get<bool>();
				g_debug_logging.store(v);
				persistFlag("sync_debug", v);
				std::cout << "[config] sync debug logging " << (v ? "enabled" : "disabled") << '\n';
			}
			if (b.contains("sync_threads") && b["sync_threads"].is_number_integer())
			{
				int n = b["sync_threads"].get<int>();
				// setThreadCount alone only ever lived in SyncManager's
				// in-memory atomic — conf_.setSyncThreadsOverride persists it,
				// so a restart doesn't silently revert to KAIROS_SYNC_THREADS.
				if (n >= 1 && n <= 32)
				{
					sync_.setThreadCount(n);
					conf_.setSyncThreadsOverride(n);
				}
			}
			if (b.contains("stream_buffer_size") && b["stream_buffer_size"].is_number_integer())
			{
				int n = b["stream_buffer_size"].get<int>();
				if (n >= 1024)
				{
					g_buffer_size.store(n);
					conf_.setBufferSize(n);
				}
			}
			if (b.contains("image_cache_ttl_hours") && b["image_cache_ttl_hours"].is_number_integer())
			{
				int h = b["image_cache_ttl_hours"].get<int>();
				if (h >= 1 && h <= 720) conf_.setImageCacheTtlHours(h);
			}
			if (b.contains("verbose_transcode_logs") && b["verbose_transcode_logs"].is_boolean())
			{
				bool v = b["verbose_transcode_logs"].get<bool>();
				g_verbose_transcode_logs.store(v);
				persistFlag("verbose_transcode_logs", v);
			}
			if (b.contains("verbose_gateway_logs") && b["verbose_gateway_logs"].is_boolean())
			{
				bool v = b["verbose_gateway_logs"].get<bool>();
				g_verbose_gateway_logs.store(v);
				persistFlag("verbose_gateway_logs", v);
			}
			if (b.contains("hades_debug") && b["hades_debug"].is_boolean())
			{
				bool v = b["hades_debug"].get<bool>();
				g_hades_debug.store(v);
				persistFlag("hades_debug", v);
			}
			if (b.contains("cast_app_id") && b["cast_app_id"].is_string())
			{
				ConfigRepository(db_).setValue("cast_app_id", b["cast_app_id"].get<std::string>());
			}
			if (b.contains("default_landing_page") && b["default_landing_page"].is_string())
			{
				auto v = b["default_landing_page"].get<std::string>();
				if (v == "home" || v == "guide") ConfigRepository(db_).setValue("default_landing_page", v);
			}
			if (b.contains("guest_profiles_enabled") && b["guest_profiles_enabled"].is_boolean())
			{
				bool v = b["guest_profiles_enabled"].get<bool>();
				ConfigRepository(db_).setValue("guest_profiles_enabled", v ? "1" : "0");
				// Auto-enable require_admin_password_switch as a safe default the
				// moment guest profiles turn on — deliberately one-directional
				// (turning guest profiles back off does NOT auto-revert this) so
				// it stays a real, independently-held setting afterward rather
				// than one that silently re-weakens itself. An admin who wants it
				// off again while guests remain on can't: requireAdminPassword
				// SwitchEffective is enforced as raw-OR-guest_profiles_enabled
				// regardless of what gets written here.
				if (v) ConfigRepository(db_).setValue("require_admin_password_switch", "1");
			}
			if (b.contains("require_admin_password_switch") && b["require_admin_password_switch"].is_boolean())
			{
				ConfigRepository(db_).setValue("require_admin_password_switch", b["require_admin_password_switch"].get<bool>() ? "1" : "0");
			}
			if (b.contains("guest_idle_timeout_days") && b["guest_idle_timeout_days"].is_number_integer())
			{
				int d = b["guest_idle_timeout_days"].get<int>();
				if (d >= 1 && d <= 365) ConfigRepository(db_).setValue("guest_idle_timeout_days", std::to_string(d));
			}
			if (b.contains("guest_max_concurrent") && b["guest_max_concurrent"].is_number_integer())
			{
				int n = b["guest_max_concurrent"].get<int>();
				if (n >= 1 && n <= 1000) ConfigRepository(db_).setValue("guest_max_concurrent", std::to_string(n));
			}
			if (b.contains("guest_channel_builder_enabled") && b["guest_channel_builder_enabled"].is_boolean())
			{
				bool v = b["guest_channel_builder_enabled"].get<bool>();
				// Meaningless (and a confusing state to persist) if guests can't
				// even sign in — refuse rather than silently no-op, so the admin
				// gets a clear error instead of a toggle that looks saved but isn't.
				if (v && !guest_settings::enabled(db_))
				{
					route::err(res, 400, "guest_profiles_enabled must be on first");
					return;
				}
				ConfigRepository(db_).setValue("guest_channel_builder_enabled", v ? "1" : "0");
			}
			if (b.contains("guest_max_demo_channels") && b["guest_max_demo_channels"].is_number_integer())
			{
				int n = b["guest_max_demo_channels"].get<int>();
				if (n >= 1 && n <= 10) ConfigRepository(db_).setValue("guest_max_demo_channels", std::to_string(n));
			}
			if (b.contains("viewer_max_channels") && b["viewer_max_channels"].is_number_integer())
			{
				int n = b["viewer_max_channels"].get<int>();
				if (n >= 1 && n <= 50) ConfigRepository(db_).setValue("viewer_max_channels", std::to_string(n));
			}
			// Empty means "unconfigured" to Router.cpp, which fails closed —
			// reject it here instead of silently breaking channel advancement.
			if (b.contains("internal_token") && b["internal_token"].is_string())
			{
				auto v = b["internal_token"].get<std::string>();
				if (!v.empty()) conf_.setInternalToken(v);
				else
				{
					route::err(res, 400, "internal_token cannot be empty");
					return;
				}
			}
			route::ok(res, settingsJson().dump());
		}
		catch (const std::exception& e)
		{
			route::err(res, 400, e.what());
		}
	});

	svr.Post("/api/config/epg/clear-all", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			int affected = ScheduleRepository(db_).clearAllScheduled();
			route::ok(res, json{{"cleared", affected}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/config/epg/clear-all", e);
			route::err(res, 500, e.what());
		}
	});

	// POST /api/logs/client — receive logs from the Hades frontend. user_id
	// (best-effort, see auth/AuthContext.tsx's currentUserRef) is folded into
	// the printed line rather than given its own column/table — this stays a
	// LogBuffer line like every other log source, just with attribution
	// instead of none, not a new structured store.
	svr.Post("/api/logs/client", [this](const Req& req, Res& res)
	{
		try
		{
			auto b              = json::parse(req.body);
			std::string level   = b.value("level", "error");
			std::string msg     = b.value("message", "");
			std::string user_id = b.value("user_id", "");
			if (!msg.empty())
			{
				std::cerr << "[hades] [" << level << "]"
					<< (user_id.empty() ? "" : " [user:" + user_id + "]")
					<< " " << msg << std::endl;
			}
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (...)
		{
			route::err(res, 400, "invalid body");
		}
	});

	// Reset the entire media library index — wipes all show/episode/movie data and
	// source mappings so the next sync starts completely fresh. Keeps source/library
	// configuration, channels, users, and settings intact.
	svr.Post("/api/config/library/reset", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			SQLite::Transaction txn(db_.get());

			// Null out FK columns in media_cursor before deleting the rows they point at.
			db_.get().exec("UPDATE media_cursor SET episode_id = NULL WHERE episode_id IS NOT NULL");
			db_.get().exec("UPDATE media_cursor SET movie_id   = NULL WHERE movie_id   IS NOT NULL");

			// Scraper match/override data tied to kairos IDs.
			db_.get().exec("DELETE FROM item_match_candidate");
			db_.get().exec("DELETE FROM special_link_candidate");
			db_.get().exec("DELETE FROM metadata_override");
			db_.get().exec("DELETE FROM scraper_job");

			// Chapters are keyed by episode file path; stale after ID reassignment.
			db_.get().exec("DELETE FROM chapter");

			// Playlists are source-synced; they'll be recreated on next sync.
			db_.get().exec("DELETE FROM playlist_item");
			db_.get().exec("DELETE FROM playlist");

			// Per-show/movie sticky track preference (migrations v92/v94) —
			// both reference show(show_id)/movie(movie_id) with no ON DELETE
			// CASCADE (see AuthStore::deleteUserCascade's own comment on the
			// exact same gap for user deletion — these two tables were added
			// after this reset route and that deletion path both predate
			// them, and neither was ever updated to know about them). Without
			// this, resetting a library that has any per-user track
			// preference on a show/movie throws a foreign-key constraint
			// violation on the DELETE FROM show/movie below instead of
			// actually resetting anything.
			db_.get().exec("DELETE FROM show_track_preference");
			db_.get().exec("DELETE FROM movie_track_preference");

			// Core media tables — delete in FK order (episode → show).
			db_.get().exec("DELETE FROM episode");
			db_.get().exec("DELETE FROM show_season");
			db_.get().exec("DELETE FROM show");
			db_.get().exec("DELETE FROM movie");
			db_.get().exec("DELETE FROM source_mapping");

			txn.commit();

			// Reload sources so SyncManager's in-memory list is consistent.
			sync_.loadSources();

			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/config/library/reset", e);
			route::err(res, 500, e.what());
		}
	});

	// Sanitized snapshot of just the library/matching tables (no user/session
	// data) for offline analysis of dedupe/scraper issues. Reads via ATTACH
	// against the live db file rather than the shared connection, so it's
	// WAL-safe alongside an in-progress sync — never blocks or is blocked by
	// one. The temp copy lives under the OS temp dir and is deleted again
	// before the response returns.
	svr.Get("/api/config/debug-dump", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		namespace fs = std::filesystem;
		fs::path out_path;
		try
		{
			const std::time_t now = std::time(nullptr);
			std::tm tm{};
			gmtime_r(&now, &tm);
			char stamp[32];
			std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
			const std::string filename = std::string("kairos-analysis-") + stamp + ".db";
			out_path                   = fs::temp_directory_path() / filename;

			static const std::vector<std::string> kTables = {
				"media_source", "media_library", "source_mapping",
				"show", "movie", "episode",
				"scraper_job", "scanned_file", "match_candidate",
				"item_match_candidate", "metadata_override", "special_link_candidate",
			};

			{
				// Scoped so `out` (and its file handle) closes before we read
				// the file back below.
				SQLite::Database out(out_path.string(), SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE);
				SQLite::Statement attach(out, "ATTACH DATABASE ? AS src");
				attach.bind(1, db_.get().getFilename());
				attach.exec();

				SQLite::Transaction txn(out);
				for (const auto& t : kTables)
				{
					SQLite::Statement sq(out, "SELECT sql FROM src.sqlite_master WHERE type='table' AND name=?");
					sq.bind(1, t);
					if (!sq.executeStep()) continue; // older schema without this table — skip
					out.exec(sq.getColumn(0).getString());
					out.exec("INSERT INTO main." + t + " SELECT * FROM src." + t);
				}
				txn.commit();
				out.exec("DETACH DATABASE src");
			}

			std::ifstream f(out_path, std::ios::binary);
			std::string body((std::istreambuf_iterator<char>(f)), {});
			f.close();

			std::error_code ec;
			fs::remove(out_path, ec);

			res.set_header("Content-Disposition", "attachment; filename=\"" + filename + "\"");
			res.set_content(body, "application/octet-stream");
		}
		catch (const std::exception& e)
		{
			std::error_code ec;
			if (!out_path.empty()) fs::remove(out_path, ec);
			route::logErr("GET /api/config/debug-dump", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/config/credentials", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		json result = json::array();
		for (const auto& r : SourceRepository(db_).listSourcesBasic())
		{
			result.push_back({
				{"source_id", r.source_id},
				{"source_type", r.source_type},
				{"display_name", r.display_name},
				{"has_token", conf_.hasToken(r.source_id)},
				{"has_user_id", conf_.hasUserId(r.source_id)},
				{"sync_priority", r.syncPriority},
			});
		}
		route::ok(res, result.dump());
	});

	svr.Get("/api/config/credentials/:source_id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto sid = req.path_params.at("source_id");
		route::ok(res, json{
					  {"has_token", conf_.hasToken(sid)},
					  {"has_user_id", conf_.hasUserId(sid)}
				  }.dump());
	});

	svr.Put("/api/config/credentials/:source_id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto sid     = req.path_params.at("source_id");
			auto b       = json::parse(req.body);
			auto token   = b.value("token", "");
			auto user_id = b.value("user_id", "");
			conf_.setCredentials(sid, token, user_id);
			sync_.loadSources();
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const json::exception& e)
		{
			route::err(res, 400, e.what());
		}
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/config/credentials/:source_id", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Delete("/api/config/credentials/:source_id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto sid = req.path_params.at("source_id");
		try
		{
			conf_.removeSource(sid);
			sync_.loadSources();
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/config/credentials/" + sid, e);
			route::err(res, 500, e.what());
		}
	});

	svr.Get("/api/config/path-maps/:source_id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		auto sid    = req.path_params.at("source_id");
		auto maps   = conf_.getPathMaps(sid);
		json result = json::array();
		for (const auto& [from, to] : maps) result.push_back({{"from", from}, {"to", to}});
		route::ok(res, result.dump());
	});

	svr.Put("/api/config/path-maps/:source_id", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto sid = req.path_params.at("source_id");
			auto b   = json::parse(req.body);
			std::vector<std::pair<std::string, std::string>> maps;
			if (b.contains("maps") && b["maps"].is_array())
			{
				for (const auto& m : b["maps"])
				{
					auto from = m.value("from", "");
					auto to   = m.value("to", "");
					if (!from.empty()) maps.push_back({from, to});
				}
			}
			conf_.setPathMaps(sid, maps);
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const json::exception& e) { route::err(res, 400, e.what()); }
		catch (const std::exception& e)
		{
			route::logErr("PUT /api/config/path-maps/:source_id", e);
			route::err(res, 500, e.what());
		}
	});

	// SMTP settings — backs invite emails (AuthService) and this section's own
	// "Send Test Email" button. Password is never read back (same
	// don't-leak-secrets shape as /api/config/credentials/:source_id above) —
	// GET reports only whether one is set; a blank password field on save
	// preserves whatever's already stored.
	svr.Get("/api/config/smtp", [this](const Req&, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		ConfigRepository repo(db_);
		route::ok(res, json{
					  {"host", repo.getValue("smtp_host")},
					  {"port", repo.getValue("smtp_port")},
					  {"username", repo.getValue("smtp_username")},
					  {"has_password", !repo.getValue("smtp_password").empty()},
					  {"from_address", repo.getValue("smtp_from_address")},
					  {"public_base_url", repo.getValue("smtp_public_base_url")},
				  }.dump());
	});

	svr.Post("/api/config/smtp", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto b = json::parse(req.body);
			ConfigRepository repo(db_);
			if (b.contains("host")) repo.setValue("smtp_host", b["host"].get<std::string>());
			if (b.contains("port")) repo.setValue("smtp_port", b["port"].get<std::string>());
			if (b.contains("username")) repo.setValue("smtp_username", b["username"].get<std::string>());
			if (b.contains("password") && !b["password"].get<std::string>().empty()) repo.setValue("smtp_password", b["password"].get<std::string>());
			if (b.contains("from_address")) repo.setValue("smtp_from_address", b["from_address"].get<std::string>());
			if (b.contains("public_base_url")) repo.setValue("smtp_public_base_url", b["public_base_url"].get<std::string>());
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const json::exception& e)
		{
			route::err(res, 400, e.what());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/config/smtp", e);
			route::err(res, 500, e.what());
		}
	});

	svr.Post("/api/config/smtp/test", [this](const Req& req, Res& res)
	{
		if (!currentUser() || currentUser()->role != "admin")
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto b               = json::parse(req.body);
			const std::string to = b.value("to", "");
			if (to.empty())
			{
				route::err(res, 400, "'to' address required");
				return;
			}
			std::string error;
			const bool ok = email_.testConnection(to, &error);
			if (!ok)
			{
				route::err(res, 502, error.empty() ? "send failed" : error);
				return;
			}
			route::ok(res, json{{"ok", true}}.dump());
		}
		catch (const json::exception& e)
		{
			route::err(res, 400, e.what());
		}
	});
}