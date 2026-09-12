#pragma once
#include <atomic>

// Host-wide cap on concurrent hardware-encode sessions (NVENC/VAAPI), shared
// across every consumer that can spawn one — ChannelSession (via
// VodEncodeStream), VodSession (via VodEncodeStream, both its video and audio
// streams), and PreviewSession. Without this, each session type's own local
// cap (SessionManager has none at all; VodSessionManager/PreviewSessionManager
// cap concurrent *viewer sessions*, not encoder slots — a shared VodEncodeStream
// already caps itself at kVodMaxLiveHeads live heads regardless of viewer
// count) has no idea what the other two are doing, so their sums can still
// oversubscribe the GPU's real concurrent-session ceiling. That oversubscription
// is what let a channel's live producer fall behind real-time in the first
// place — see VodEncodeStream::kHeadStallTimeoutMs's own comment for what
// happens next once it does.
//
// One instance, constructed once in main.cpp, handed by pointer into every
// *StreamOptions struct — a caller only needs to gate a spawn when its own
// resolved hw_accel isn't HwAccel::none; a software encode never touches this.
class EncoderAdmission
{
public:
	explicit EncoderAdmission(int max_sessions)
		: max_(max_sessions)
	{
	}

	// False (and no-op) if already at cap — 0/negative max means unlimited,
	// same convention as Config.h's other max_*_sessions fields, so this is a
	// pure opt-in: nothing changes for a deployment that never sets it.
	bool tryAcquire()
	{
		if (max_ <= 0) return true;
		int cur = active_.load();
		while (cur < max_)
		{
			if (active_.compare_exchange_weak(cur, cur + 1)) return true;
		}
		return false;
	}

	// Safe to call unconditionally even when max_ <= 0 (tryAcquire() never
	// incremented in that case, so this would just underflow) — callers only
	// ever release a slot they successfully acquired (see VodEncodeStream::Head's
	// own RAII release, and PreviewSession's mirroring of it), so this is
	// only reachable already paired with a real tryAcquire()==true.
	void release() { active_.fetch_sub(1); }

	int activeCount() const { return active_.load(); }

private:
	int max_;
	std::atomic<int> active_{0};
};