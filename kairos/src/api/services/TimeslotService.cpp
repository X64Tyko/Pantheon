#include "TimeslotService.h"
#include "../AuthContext.h"
#include "../ChannelAuth.h"
#include "../RouteHelpers.h"
#include "../ServiceContext.h"
#include "../../db/Database.h"
#include "../../db/TimeslotRepository.h"
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

using json = nlohmann::json;
using Req  = httplib::Request;
using Res  = httplib::Response;

TimeslotService::TimeslotService(const ServiceContext& ctx)
	: db_(ctx.db)
	, guest_mutation_limiter_(ctx.guest_mutation_limiter)
{
}

void TimeslotService::registerRoutes(httplib::Server& svr)
{
	// Every route below is nested under /api/blocks/:bid/... — same
	// ownership boundary as BlockService (channel_auth::canEditChannel,
	// resolved from the block via canEditChannelForBlock). Previously had NO
	// role/ownership check at all: any authenticated viewer, not just a
	// future guest/real-viewer builder, could already mutate arbitrary
	// channels' timeslot data. Closed here as part of adding the ownership
	// model, not just guarding the new self-service path.

	// ── Slots ─────────────────────────────────────────────────────────────────

	// POST /api/blocks/:bid/slots  — create slot
	svr.Post("/api/blocks/:bid/slots", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto d = json::parse(req.body);
			TimeslotRepository repo(db_);
			std::string slot_id = repo.createSlot(bid, d);
			res.status          = 201;
			route::ok(res, json{{"slot_id", slot_id}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/blocks/:bid/slots", e);
			route::err(res, 400, e.what());
		}
	});

	// PATCH /api/blocks/:bid/slots/:sid  — update slot fields
	svr.Patch("/api/blocks/:bid/slots/:sid", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto d = json::parse(req.body);
			TimeslotRepository repo(db_);
			if (!repo.slotBelongsToBlock(sid, bid))
			{
				route::err(res, 404, "slot not found");
				return;
			}
			if (d.contains("slot_offset_mins")) repo.updateSlotField(sid, "slot_offset_mins", d["slot_offset_mins"].get<int>());
			if (d.contains("slot_duration_mins")) repo.updateSlotField(sid, "slot_duration_mins", d["slot_duration_mins"].get<int>());
			if (d.contains("overflow")) repo.updateSlotField(sid, "overflow", d["overflow"].get<std::string>());
			if (d.contains("late_start_mins")) repo.updateSlotField(sid, "late_start_mins", d["late_start_mins"].get<int>());
			if (d.contains("early_start_secs")) repo.updateSlotField(sid, "early_start_secs", d["early_start_secs"].get<int>());
			if (d.contains("align_to_mins")) repo.updateSlotField(sid, "align_to_mins", d["align_to_mins"].get<int>());
			if (d.contains("start_scope")) repo.updateSlotField(sid, "start_scope", d["start_scope"].get<std::string>());
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/blocks/:bid/slots/:sid", e);
			route::err(res, 400, e.what());
		}
	});

	// DELETE /api/blocks/:bid/slots/:sid
	svr.Delete("/api/blocks/:bid/slots/:sid", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			TimeslotRepository repo(db_);
			if (!repo.slotBelongsToBlock(sid, bid))
			{
				route::err(res, 404, "slot not found");
				return;
			}
			repo.deleteSlot(sid);
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/blocks/:bid/slots/:sid", e);
			route::err(res, 500, e.what());
		}
	});

	// POST /api/blocks/:bid/slots/reorder  — [{slot_id: ...}, ...]
	svr.Post("/api/blocks/:bid/slots/reorder", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto arr = json::parse(req.body);
			std::vector<std::string> ids;
			for (const auto& item : arr) ids.push_back(item.at("slot_id").get<std::string>());
			TimeslotRepository repo(db_);
			repo.reorderSlots(bid, ids);
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/blocks/:bid/slots/reorder", e);
			route::err(res, 400, e.what());
		}
	});

	// POST /api/blocks/:bid/slots/:sid/cursor/reset
	svr.Post("/api/blocks/:bid/slots/:sid/cursor/reset", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			TimeslotRepository repo(db_);
			if (!repo.slotBelongsToBlock(sid, bid))
			{
				route::err(res, 404, "slot not found");
				return;
			}
			repo.resetSlotCursor(sid);
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/blocks/:bid/slots/:sid/cursor/reset", e);
			route::err(res, 500, e.what());
		}
	});

	// ── Queue entries ─────────────────────────────────────────────────────────

	// POST /api/blocks/:bid/slots/:sid/queue
	svr.Post("/api/blocks/:bid/slots/:sid/queue", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto d = json::parse(req.body);
			TimeslotRepository repo(db_);
			if (!repo.slotBelongsToBlock(sid, bid))
			{
				route::err(res, 404, "slot not found");
				return;
			}
			std::string entry_id = repo.addQueueEntry(sid, d);
			res.status           = 201;
			route::ok(res, json{{"entry_id", entry_id}}.dump());
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/blocks/:bid/slots/:sid/queue", e);
			route::err(res, 400, e.what());
		}
	});

	// PATCH /api/blocks/:bid/slots/:sid/queue/:eid
	svr.Patch("/api/blocks/:bid/slots/:sid/queue/:eid", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		auto eid = req.path_params.at("eid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto d = json::parse(req.body);
			TimeslotRepository repo(db_);
			if (!repo.entryBelongsToSlot(eid, sid))
			{
				route::err(res, 404, "queue entry not found");
				return;
			}
			if (d.contains("premiere_date")) repo.updateQueueEntry(eid, "premiere_date", d["premiere_date"].get<std::string>());
			if (d.contains("pre_premiere_behavior")) repo.updateQueueEntry(eid, "pre_premiere_behavior", d["pre_premiere_behavior"].get<std::string>());
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("PATCH /api/blocks/:bid/slots/:sid/queue/:eid", e);
			route::err(res, 400, e.what());
		}
	});

	// DELETE /api/blocks/:bid/slots/:sid/queue/:eid
	svr.Delete("/api/blocks/:bid/slots/:sid/queue/:eid", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		auto eid = req.path_params.at("eid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			TimeslotRepository repo(db_);
			if (!repo.entryBelongsToSlot(eid, sid))
			{
				route::err(res, 404, "queue entry not found");
				return;
			}
			repo.deleteQueueEntry(eid);
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("DELETE /api/blocks/:bid/slots/:sid/queue/:eid", e);
			route::err(res, 500, e.what());
		}
	});

	// POST /api/blocks/:bid/slots/:sid/queue/reorder  — [{entry_id: ...}, ...]
	svr.Post("/api/blocks/:bid/slots/:sid/queue/reorder", [this](const Req& req, Res& res)
	{
		auto bid = req.path_params.at("bid");
		auto sid = req.path_params.at("sid");
		if (!currentUser() || !channel_auth::canMutateChannelForBlock(db_, guest_mutation_limiter_, *currentUser(), bid))
		{
			route::err(res, 403, "Forbidden");
			return;
		}
		try
		{
			auto arr = json::parse(req.body);
			std::vector<std::string> ids;
			for (const auto& item : arr) ids.push_back(item.at("entry_id").get<std::string>());
			TimeslotRepository repo(db_);
			repo.reorderQueueEntries(sid, ids);
			repo.resetSlotCursor(sid);
			route::ok(res, "{}");
		}
		catch (const std::exception& e)
		{
			route::logErr("POST /api/blocks/:bid/slots/:sid/queue/reorder", e);
			route::err(res, 400, e.what());
		}
	});
}