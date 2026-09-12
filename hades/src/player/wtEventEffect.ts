import type {WatchTogetherEvent} from './watchTogetherApi'

// Watch Together: how far a follower's local position must drift from a
// periodic `sync` tick's authoritative one before snapping to it — small
// clock/decode jitter shouldn't cause constant micro-seeking, but a stalled
// rebuffer or a fresh join should correct promptly. Mirrored by
// PlayerPage.tsx's own copy of this constant (kept in sync manually — this
// module only owns the decision logic, not the <video>/React state it acts on).
export const WT_SYNC_DRIFT_THRESHOLD_MS = 1_500

export interface WtEventEffect {
    /** Set video.currentTime to this (ms) and mirror it into currentMs state. */
    seekToMs?: number
    pause?: boolean
    play?: boolean
}

// Pure decision logic behind PlayerPage's follower-side `applyWtEvent` —
// split out so the drift-tolerance/seek-vs-no-op rule (the same one Android's
// port mirrors) is unit-testable without a real <video> element. Position
// always snaps immediately for an explicit seek/pause/play; a plain `sync`
// tick (Hermes' periodic authoritative tick, or a fresh subscriber's first
// message) only corrects once drift exceeds WT_SYNC_DRIFT_THRESHOLD_MS so
// ordinary clock/decode jitter doesn't cause constant micro-seeking.
// paused-state always syncs (idempotent — catches a fresh join or a missed
// event even though explicit pause/play already covers most of it).
export function computeWtEventEffect(
    event: WatchTogetherEvent,
    currentMs: number,
    isPaused: boolean,
): WtEventEffect {
    const effect: WtEventEffect = {}

    if (typeof event.position_ms === 'number') {
        const driftMs = Math.abs(event.position_ms - currentMs)
        if (event.type !== 'sync' || driftMs > WT_SYNC_DRIFT_THRESHOLD_MS) {
            effect.seekToMs = event.position_ms
        }
    }

    if (event.paused === true && !isPaused) effect.pause = true
    else if (event.paused === false && isPaused) effect.play = true

    return effect
}
