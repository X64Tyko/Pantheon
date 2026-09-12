#pragma once
#include "../Config.h"
#include "../stream/ClientCapabilities.h"
#include "../stream/SessionManager.h"
#include "../stream/VodSessionManager.h"
#include "../stream/PreviewSessionManager.h"
#include "../stream/ChannelViewerRegistry.h"
#include "../kairos/KairosClient.h"
#include "log/LogBuffer.h"
#include "cache/SegmentCache.h"
#include <httplib.h>

void registerRoutes(httplib::Server& svr, SessionManager& sessions, VodSessionManager& vodSessions,
					PreviewSessionManager& previewSessions, ChannelViewerRegistry& channelViewers,
					KairosClient& kairos, LogBuffer& logs, const Config& cfg,
					ClientCapabilityCache& capabilityCache, SegmentCache& segmentCache);