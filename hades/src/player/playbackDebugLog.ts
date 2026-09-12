import {api} from '../api/client'
import {statusStore} from '../stores'
import {currentUserRef} from '../auth/AuthContext'

// Shared category tags for playbackDebugLog() calls across VideoPlayer.tsx/
// PlayerPage.tsx/usePlaybackSession.ts, so a raw [hades] log stream (server
// just prints level+message — see ConfigService.cpp's /api/logs/client) can
// be grepped by area instead of read as one undifferentiated feed. Add a new
// one here rather than inventing an ad-hoc prefix at the call site.
export type PlaybackLogCategory =
    | 'buffer'   // SourceBuffer append/flush — per-track buffered ranges, not just the HTMLMediaElement intersection
    | 'stall'    // non-fatal hls.js buffer stalls/nudges and the reload-forcing heuristic
    | 'error'    // fatal hls.js errors and the retry/give-up path
    | 'track'    // audio/subtitle track selection, both the hls.js side and the user-driven side
    | 'seek'     // user/programmatic seeks
    | 'frag'     // fragment load lifecycle
    | 'session'  // session/viewer lifecycle: item transitions, bucket reconnects, capability-viewer fallback
    | 'cast'     // Chromecast sender/receiver session events

// Best-effort, fire-and-forget, gated on hades_debug (statusStore.hadesDebug)
// — the same opt-in diagnostic posture the stall/nudge forwarding this
// generalizes already used. Never throws into the caller.
export function playbackDebugLog(category: PlaybackLogCategory, message: string) {
    if (!statusStore.hadesDebug) return
    api.sendClientLog('warn', `[${category}] ${message}`, currentUserRef.id).catch(() => {
    })
}
