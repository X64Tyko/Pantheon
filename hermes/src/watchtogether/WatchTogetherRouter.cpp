#include "WatchTogetherRouter.h"
#include <nlohmann/json.hpp>
#include <chrono>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

namespace
{
	// EventSource (the /stream route below) can't set custom headers, so the
	// browser passes the token as ?token= instead — same fallback Kairos's own
	// pre-routing handler (api/Router.cpp) already accepts for exactly this
	// reason (see SystemStore.connectLogs()'s identical ?token= construction for
	// /api/logs/stream). Normalized to a real "Authorization: Bearer <token>"
	// value either way, so every downstream Kairos call (auth check, the
	// session lookup, an eventual close-on-reap) can just forward one consistent
	// header regardless of which form THIS request arrived in.
	std::string resolveAuthHeader(const Req& req)
	{
		auto header = req.get_header_value("Authorization");
		if (!header.empty()) return header;
		auto it = req.params.find("token");
		if (it != req.params.end() && !it->second.empty()) return "Bearer " + it->second;
		return "";
	}

	// Same "Hermes has no session store of its own, ask Kairos" pattern as
	// DeviceRouter.cpp's own authenticate() — duplicated rather than shared
	// across the two files, matching this codebase's existing convention (see
	// api/Router.cpp's authedHephaestusProxy/crash-DELETE handler, which each
	// have their own inline copy of the same round trip too).
	std::optional<std::string> authenticate(const Config& cfg, const std::string& auth)
	{
		if (auth.empty()) return std::nullopt;
		httplib::Client cli(cfg.kairos_url);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(5);
		auto r = cli.Get("/api/auth/me", httplib::Headers{{"Authorization", auth}});
		if (!r || r->status != 200) return std::nullopt;
		try
		{
			auto j       = json::parse(r->body);
			auto user_id = j.value("user_id", "");
			return user_id.empty() ? std::nullopt : std::optional(user_id);
		}
		catch (...) { return std::nullopt; }
	}

	void unauthorized(Res& res)
	{
		res.status = 401;
		res.set_content(json{{"error", "Unauthorized"}}.dump(), "application/json");
	}

	void forbidden(Res& res)
	{
		res.status = 403;
		res.set_content(json{{"error", "Forbidden"}}.dump(), "application/json");
	}

	void notFound(Res& res)
	{
		res.status = 404;
		res.set_content(json{{"error", "Session not found or ended"}}.dump(), "application/json");
	}
} // namespace

void registerWatchTogetherRoutes(httplib::Server& svr, WatchTogetherManager& watch_together, const Config& cfg)
{
	// GET /watch-together/:id/stream — SSE, same chunked-provider +
	// seq/waitAfter pattern as Kairos's own /api/logs/stream (see
	// ActivityService.cpp) and this same file's /api/logs/stream mirror
	// above in Router.cpp — a fresh subscriber's first message is a
	// synthesized `sync` event for the CURRENT authoritative state (not a
	// backlog of past commands, which a fresh joiner has no use for), then
	// every subsequent seek/pause/play/sync event as it happens.
	svr.Get(R"(/watch-together/([^/]+)/stream$)", [&watch_together, &cfg](const Req& req, Res& res)
	{
		auto auth    = resolveAuthHeader(req);
		auto user_id = authenticate(cfg, auth);
		if (!user_id)
		{
			unauthorized(res);
			return;
		}

		std::string session_id = req.matches[1];
		auto session           = watch_together.getOrCreate(session_id, cfg, auth);
		if (!session)
		{
			notFound(res);
			return;
		}

		res.set_header("Cache-Control", "no-cache");
		res.set_header("Connection", "keep-alive");
		res.set_header("X-Accel-Buffering", "no");
		res.set_header("Access-Control-Allow-Origin", "*");

		res.set_chunked_content_provider("text/event-stream",
										 [session, cur_seq = uint64_t{0}, sent_init = false]
									 (size_t, httplib::DataSink& sink) mutable -> bool
										 {
											 if (!sent_init)
											 {
												 sent_init            = true;
												 auto [snapshot, seq] = session->initialSnapshot();
												 cur_seq              = seq;
												 std::string ev       = "data:" + snapshot.dump() + "\n\n";
												 return sink.write(ev.data(), ev.size());
											 }

											 auto [events, new_seq] = session->waitAfter(cur_seq, std::chrono::milliseconds{15'000});

											 if (!sink.is_writable()) return false;

											 if (events.empty())
											 {
												 static const std::string ping = ": ping\n\n";
												 return sink.write(ping.data(), ping.size());
											 }

											 cur_seq = new_seq;
											 for (auto& event : events)
											 {
												 std::string ev = "data:" + event.dump() + "\n\n";
												 if (!sink.write(ev.data(), ev.size())) return false;
											 }
											 return true;
										 });
	});

	// POST /watch-together/:id/join — forwards to Kairos's own join (the
	// real membership write) and merges in the live position/paused Kairos
	// never tracks, so a joining client gets everything it needs — identity
	// AND where to actually start playback — in the one call it was already
	// making anyway, instead of a second round trip to fetch it separately.
	svr.Post(R"(/watch-together/([^/]+)/join$)", [&watch_together, &cfg](const Req& req, Res& res)
	{
		auto auth    = resolveAuthHeader(req);
		auto user_id = authenticate(cfg, auth);
		if (!user_id)
		{
			unauthorized(res);
			return;
		}

		std::string session_id = req.matches[1];

		httplib::Client kairos(cfg.kairos_url);
		kairos.set_connection_timeout(5);
		kairos.set_read_timeout(5);
		auto r = kairos.Post(("/api/watch-together/" + session_id + "/join").c_str(),
							 httplib::Headers{{"Authorization", auth}}, "", "application/json");
		if (!r || r->status != 200)
		{
			res.status = r ? r->status : 502;
			res.set_content(r ? r->body : json{{"error", "Kairos unreachable"}}.dump(), "application/json");
			return;
		}

		json body;
		try { body = json::parse(r->body); }
		catch (...)
		{
			res.status = 502;
			res.set_content(json{{"error", "Malformed Kairos response"}}.dump(), "application/json");
			return;
		}

		auto session        = watch_together.getOrCreate(session_id, cfg, auth);
		body["position_ms"] = session ? session->authoritativePositionMs() : 0;
		body["paused"]      = session ? session->isPaused() : true;
		res.set_content(body.dump(), "application/json");
	});

	// POST /watch-together/:id/command — host-only.
	// {"type": "seek"|"pause"|"play", "position_ms"?}
	svr.Post(R"(/watch-together/([^/]+)/command$)", [&watch_together, &cfg](const Req& req, Res& res)
	{
		auto auth    = resolveAuthHeader(req);
		auto user_id = authenticate(cfg, auth);
		if (!user_id)
		{
			unauthorized(res);
			return;
		}

		std::string session_id = req.matches[1];
		auto session           = watch_together.getOrCreate(session_id, cfg, auth);
		if (!session)
		{
			notFound(res);
			return;
		}
		if (*user_id != session->hostUserId())
		{
			forbidden(res);
			return;
		}

		json command;
		try { command = json::parse(req.body); }
		catch (...)
		{
			res.status = 400;
			res.set_content(json{{"error", "Invalid JSON"}}.dump(), "application/json");
			return;
		}
		auto type = command.value("type", "");
		if (type != "seek" && type != "pause" && type != "play")
		{
			res.status = 400;
			res.set_content(json{{"error", "type must be seek, pause, or play"}}.dump(), "application/json");
			return;
		}

		session->setHostAuthHeader(auth);
		session->pushCommand(command);
		res.set_content(json{{"ok", true}}.dump(), "application/json");
	});

	// POST /watch-together/:id/heartbeat — host-only, cheap/frequent.
	// {"position_ms": number, "paused": bool}
	svr.Post(R"(/watch-together/([^/]+)/heartbeat$)", [&watch_together, &cfg](const Req& req, Res& res)
	{
		auto auth    = resolveAuthHeader(req);
		auto user_id = authenticate(cfg, auth);
		if (!user_id)
		{
			unauthorized(res);
			return;
		}

		std::string session_id = req.matches[1];
		auto session           = watch_together.getOrCreate(session_id, cfg, auth);
		if (!session)
		{
			notFound(res);
			return;
		}
		if (*user_id != session->hostUserId())
		{
			forbidden(res);
			return;
		}

		json body;
		try { body = json::parse(req.body); }
		catch (...)
		{
			res.status = 400;
			res.set_content(json{{"error", "Invalid JSON"}}.dump(), "application/json");
			return;
		}

		session->setHostAuthHeader(auth);
		session->heartbeat(body.value("position_ms", int64_t(0)), body.value("paused", false));
		res.set_content(json{{"ok", true}}.dump(), "application/json");
	});
}