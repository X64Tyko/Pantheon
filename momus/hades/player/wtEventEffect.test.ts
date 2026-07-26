import {describe, it, expect} from 'vitest'
import {computeWtEventEffect, WT_SYNC_DRIFT_THRESHOLD_MS} from '@/player/wtEventEffect'
import type {WatchTogetherEvent} from '@/player/watchTogetherApi'

// Pure decision logic behind PlayerPage.tsx's follower-side applyWtEvent —
// extracted specifically so this drift-tolerance/seek-vs-no-op rule (the one
// Android's Watch Together port mirrors) is testable without a real <video>
// element. See wtEventEffect.ts for the full rationale.

describe('computeWtEventEffect', () => {
    describe('sync events (drift tolerance)', () => {
        it('is a no-op when a sync tick is within the drift threshold', () => {
            const event: WatchTogetherEvent = {type: 'sync', position_ms: 10_000}
            const effect = computeWtEventEffect(event, /* currentMs */ 10_500, /* isPaused */ false)
            expect(effect.seekToMs).toBeUndefined()
        })

        it('is a no-op exactly at the threshold boundary (not > threshold)', () => {
            const event: WatchTogetherEvent = {type: 'sync', position_ms: 10_000}
            const currentMs = 10_000 + WT_SYNC_DRIFT_THRESHOLD_MS
            const effect = computeWtEventEffect(event, currentMs, false)
            expect(effect.seekToMs).toBeUndefined()
        })

        it('corrects position once drift exceeds the threshold', () => {
            const event: WatchTogetherEvent = {type: 'sync', position_ms: 10_000}
            const currentMs = 10_000 + WT_SYNC_DRIFT_THRESHOLD_MS + 1
            const effect = computeWtEventEffect(event, currentMs, false)
            expect(effect.seekToMs).toBe(10_000)
        })

        it('corrects when drift is in the negative direction too (behind, not just ahead)', () => {
            const event: WatchTogetherEvent = {type: 'sync', position_ms: 10_000}
            const currentMs = 10_000 - WT_SYNC_DRIFT_THRESHOLD_MS - 1
            const effect = computeWtEventEffect(event, currentMs, false)
            expect(effect.seekToMs).toBe(10_000)
        })
    })

    describe('seek/pause/play events (applied immediately)', () => {
        it('a seek event snaps position even with zero drift', () => {
            const event: WatchTogetherEvent = {type: 'seek', position_ms: 5_000}
            const effect = computeWtEventEffect(event, 5_000, false)
            expect(effect.seekToMs).toBe(5_000)
        })

        it('a pause event with a position snaps position regardless of drift', () => {
            const event: WatchTogetherEvent = {type: 'pause', position_ms: 8_000, paused: true}
            const effect = computeWtEventEffect(event, 8_050, false)
            expect(effect.seekToMs).toBe(8_000)
        })

        it('a play event with a position snaps position regardless of drift', () => {
            const event: WatchTogetherEvent = {type: 'play', position_ms: 8_000, paused: false}
            const effect = computeWtEventEffect(event, 8_050, true)
            expect(effect.seekToMs).toBe(8_000)
        })
    })

    describe('paused-state application', () => {
        it('requests pause when the event says paused and the follower is currently playing', () => {
            const event: WatchTogetherEvent = {type: 'pause', paused: true}
            const effect = computeWtEventEffect(event, 1000, /* isPaused */ false)
            expect(effect.pause).toBe(true)
            expect(effect.play).toBeUndefined()
        })

        it('requests play when the event says unpaused and the follower is currently paused', () => {
            const event: WatchTogetherEvent = {type: 'play', paused: false}
            const effect = computeWtEventEffect(event, 1000, /* isPaused */ true)
            expect(effect.play).toBe(true)
            expect(effect.pause).toBeUndefined()
        })

        it('is idempotent — paused:true while already paused requests nothing', () => {
            const event: WatchTogetherEvent = {type: 'sync', paused: true}
            const effect = computeWtEventEffect(event, 1000, /* isPaused */ true)
            expect(effect.pause).toBeUndefined()
            expect(effect.play).toBeUndefined()
        })

        it('is idempotent — paused:false while already playing requests nothing', () => {
            const event: WatchTogetherEvent = {type: 'sync', paused: false}
            const effect = computeWtEventEffect(event, 1000, /* isPaused */ false)
            expect(effect.pause).toBeUndefined()
            expect(effect.play).toBeUndefined()
        })

        it('omitted paused field requests nothing', () => {
            const event: WatchTogetherEvent = {type: 'sync', position_ms: 20_000}
            const effect = computeWtEventEffect(event, 20_000 + WT_SYNC_DRIFT_THRESHOLD_MS + 1, false)
            expect(effect.pause).toBeUndefined()
            expect(effect.play).toBeUndefined()
        })
    })

    describe('missing position_ms', () => {
        it('a pause-only event (no position_ms) never sets seekToMs', () => {
            const event: WatchTogetherEvent = {type: 'pause', paused: true}
            const effect = computeWtEventEffect(event, 5_000, false)
            expect(effect.seekToMs).toBeUndefined()
            expect(effect.pause).toBe(true)
        })
    })
})
