#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

// Per-client (bearer token) declared decode capability — lets
// isDirectStreamable() (VodSession.cpp) check a source file's actual codecs
// against what this *specific* client can really play, instead of the
// fixed "h264/aac, every browser" allowlist that penalizes clients (native
// Android via MediaCodec, a real TV's hardware decoder) capable of far
// more.
//
// In-memory only, keyed by the same bearer token every /stream/* request
// already carries unmodified through Hermes's proxy — no Kairos schema
// change, no new device/session concept needed (see
// ClientCapabilitiesRouter.h's own comment). Wiped on every Hephaestus
// restart, which is a real, frequent occurrence — this image gets rebuilt/
// redeployed on every push to this repo — so a client can't declare
// literally once ever: isDirectStreamable() falls back to the conservative
// allowlist whenever a token has no cached declaration, and clients
// re-declare on every login/profile-switch/app-launch, not just the first
// time ever.
struct ClientCapabilities
{
	std::set<std::string> video_codecs; // ffprobe codec_name values, e.g. {"h264","hevc","av1"}
	std::set<std::string> audio_codecs; // e.g. {"aac","ac3","eac3"}
	// Client-declared output height cap, nullopt if undeclared. Also forces
	// a transcode for an otherwise-direct-streamable source taller than this —
	// see isVideoDirectStreamable/pushVideoEncoderArgs.
	std::optional<int> max_height;
};

class ClientCapabilityCache
{
public:
	// Opportunistically sweeps entries idle longer than kMaxIdleMs as a
	// side effect — see pruneStale's own comment for why a dedicated
	// background thread isn't needed for this.
	void declare(const std::string& token, ClientCapabilities caps);

	// Explicit removal on logout — the primary cleanup path, called by the
	// client itself right before it clears its own stored token. pruneStale
	// is the safety net for everything that doesn't reach this (app crash/
	// force-kill/uninstall, or a token simply expiring after its 30-day TTL
	// with no explicit logout).
	void forget(const std::string& token);

	// get() refreshes the entry's last-seen time — an actively-streaming
	// client's declaration never goes stale purely from sitting idle
	// between VOD starts.
	std::optional<ClientCapabilities> get(const std::string& token);

	// Public/explicit rather than only-ever-internal so it's directly
	// testable — declare() also calls this itself on every write, since
	// this cache is small enough (one entry per distinct active client)
	// that a dedicated sweep thread would be pure overhead on top of that.
	void pruneStale(int64_t maxIdleMs);

private:
	struct Entry
	{
		ClientCapabilities caps;
		int64_t last_seen_ms;
	};

	std::mutex mtx;
	std::unordered_map<std::string, Entry> entries;

	// Assumes mtx is already held — shared by declare() (which sweeps as a
	// side effect of every write) and the public pruneStale() (which
	// acquires the lock itself first).
	void pruneStaleLocked(int64_t maxIdleMs);
};

// 30 days — matches Kairos's own bearer-token TTL (AuthStore.cpp), so an
// entry never outlives the token that would be needed to present it anyway.
constexpr int64_t kClientCapabilityMaxIdleMs = 30LL * 24 * 60 * 60 * 1000;