#pragma once

class AuthStore;
class ConfStore;
class Database;
class DownloadManager;
class EPGMaterializer;
class LogBuffer;
class RuleEngine;
class ScheduleCache;
class SyncManager;

// Bundles all shared Kairos dependencies so each IKairosService
// can declare exactly what it needs in its constructor.
struct ServiceContext {
	Database&        db;
	ConfStore&       conf;
	SyncManager&     sync;
	ScheduleCache&   schedule_cache;
	EPGMaterializer& materializer;
	RuleEngine&      engine;
	AuthStore&       auth;
	LogBuffer&       logs;
	DownloadManager& dl;
};
