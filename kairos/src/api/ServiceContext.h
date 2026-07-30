#pragma once
#include "../util/RateLimiter.h"

class AuthStore;
class ConfStore;
class Database;
class DownloadManager;
class EmailService;
class EPGDivergenceChecker;
class EPGMaterializer;
class LogBuffer;
class RuleEngine;
class ScheduleCache;
class SyncManager;

// Bundles all shared Kairos dependencies so each IKairosService
// can declare exactly what it needs in its constructor.
struct ServiceContext
{
	Database& db;
	ConfStore& conf;
	SyncManager& sync;
	ScheduleCache& schedule_cache;
	EPGMaterializer& materializer;
	EPGDivergenceChecker& divergence_checker;
	RuleEngine& engine;
	AuthStore& auth;
	LogBuffer& logs;
	DownloadManager& dl;
	EmailService& email;
	// Shared by ChannelService/BlockService/TimeslotService — see Router.h's
	// guest_mutation_limiter_ for why this is one shared instance.
	RateLimiter& guest_mutation_limiter;
};