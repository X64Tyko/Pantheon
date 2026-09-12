import {describe, it, expect, vi, beforeEach} from 'vitest'
import {PreviewSessionController, type PreviewSessionApi} from '@/guide/previewSessionController'

// The client-side mirror of Hephaestus's PreviewSession state machine —
// see the class's own comment. Covers: reusing a live session on channel
// switch (never a second startPreview()), queuing a switch behind an
// in-flight cold start instead of racing it, a stale in-flight start
// self-stopping if superseded before it resolves, and stop()'s teardown +
// generation bump.

// Drains every pending microtask, regardless of how deep the begin()/stop()
// promise chain (startPreview().then().catch().finally(), plus any queued
// .then() on top of it) happens to be — more robust than a fixed count of
// `await Promise.resolve()` hops, which under-flushes as soon as the chain
// gains one more link.
function flushAsync(): Promise<void> {
    return new Promise(resolve => setTimeout(resolve, 0))
}

function makeApi(): PreviewSessionApi & {
    startPreview: ReturnType<typeof vi.fn>
    switchPreview: ReturnType<typeof vi.fn>
    stopPreview: ReturnType<typeof vi.fn>
} {
    return {
        startPreview: vi.fn(),
        switchPreview: vi.fn().mockResolvedValue(undefined),
        stopPreview: vi.fn(),
    }
}

describe('PreviewSessionController', () => {
    let api: ReturnType<typeof makeApi>
    let onManifestUrlChange: ReturnType<typeof vi.fn>
    let controller: PreviewSessionController

    beforeEach(() => {
        api = makeApi()
        onManifestUrlChange = vi.fn()
        controller = new PreviewSessionController(api, onManifestUrlChange)
    })

    describe('starting fresh', () => {
        it('calls startPreview, not switchPreview, when nothing is active yet', async () => {
            api.startPreview.mockResolvedValue({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            controller.begin('ch1')
            expect(api.startPreview).toHaveBeenCalledWith('ch1')
            expect(api.switchPreview).not.toHaveBeenCalled()
            await flushAsync()
            expect(onManifestUrlChange).toHaveBeenCalledWith('/m/s1.m3u8')
            expect(controller.hasSession()).toBe(true)
        })

        it('isStarting() is true while the startPreview() call is in flight', () => {
            let resolve: (v: { session_id: string; manifest_url: string }) => void
            api.startPreview.mockReturnValue(new Promise(r => {
                resolve = r
            }))
            controller.begin('ch1')
            expect(controller.isStarting()).toBe(true)
            expect(controller.hasSession()).toBe(false)
            resolve!({session_id: 's1', manifest_url: '/m/s1.m3u8'})
        })
    })

    describe('switching channels on an already-live session', () => {
        beforeEach(async () => {
            api.startPreview.mockResolvedValue({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            controller.begin('ch1')
            await flushAsync()
        })

        it('reuses the existing session via switchPreview, never a second startPreview', () => {
            controller.begin('ch2')
            expect(api.switchPreview).toHaveBeenCalledWith('s1', 'ch2')
            expect(api.startPreview).toHaveBeenCalledTimes(1)
        })

        it('does not change the manifest url on a plain switch', () => {
            onManifestUrlChange.mockClear()
            controller.begin('ch2')
            expect(onManifestUrlChange).not.toHaveBeenCalled()
        })
    })

    describe('a channel switch requested mid-cold-start', () => {
        it('queues onto the in-flight start instead of racing a second startPreview', async () => {
            let resolveStart: (v: { session_id: string; manifest_url: string }) => void
            api.startPreview.mockReturnValue(new Promise(r => {
                resolveStart = r
            }))

            controller.begin('ch1') // triggers the cold start
            controller.begin('ch2') // arrives while ch1's start is still in flight

            expect(api.startPreview).toHaveBeenCalledTimes(1) // never a second one
            expect(api.switchPreview).not.toHaveBeenCalled()  // not yet — nothing to switch onto

            resolveStart!({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            await flushAsync()

            expect(api.switchPreview).toHaveBeenCalledWith('s1', 'ch2')
        })
    })

    describe('stop() while a start is still in flight', () => {
        it('self-stops the session once the stale start resolves, instead of adopting it', async () => {
            let resolveStart: (v: { session_id: string; manifest_url: string }) => void
            api.startPreview.mockReturnValue(new Promise(r => {
                resolveStart = r
            }))

            controller.begin('ch1')
            controller.stop() // e.g. tab hidden before the cold start finished

            resolveStart!({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            await flushAsync()

            expect(api.stopPreview).toHaveBeenCalledWith('s1')
            expect(controller.hasSession()).toBe(false)
            // The now-adopted-then-discarded session's manifest url must never
            // reach the caller — stop() already cleared it to null and nothing
            // should re-set it afterward.
            expect(onManifestUrlChange).not.toHaveBeenCalledWith('/m/s1.m3u8')
        })
    })

    describe('stop()', () => {
        it('is a no-op (besides bumping generation) when nothing is active', () => {
            controller.stop()
            expect(api.stopPreview).not.toHaveBeenCalled()
            expect(onManifestUrlChange).not.toHaveBeenCalled()
        })

        it('tears down a live session and clears the manifest url', async () => {
            api.startPreview.mockResolvedValue({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            controller.begin('ch1')
            await flushAsync()

            controller.stop()
            expect(api.stopPreview).toHaveBeenCalledWith('s1')
            expect(onManifestUrlChange).toHaveBeenLastCalledWith(null)
            expect(controller.hasSession()).toBe(false)
        })

        it('a fresh begin() after stop() starts a brand new session, not a switch', async () => {
            api.startPreview.mockResolvedValue({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            controller.begin('ch1')
            await flushAsync()
            controller.stop()

            api.startPreview.mockResolvedValue({session_id: 's2', manifest_url: '/m/s2.m3u8'})
            controller.begin('ch2')
            expect(api.startPreview).toHaveBeenLastCalledWith('ch2')
            expect(api.switchPreview).not.toHaveBeenCalled()
        })
    })

    describe('a failed startPreview()', () => {
        it('leaves the controller with no session and does not throw', async () => {
            api.startPreview.mockRejectedValue(new Error('network error'))
            controller.begin('ch1')
            await flushAsync()
            expect(controller.hasSession()).toBe(false)
            expect(controller.isStarting()).toBe(false)
            expect(onManifestUrlChange).not.toHaveBeenCalled()
        })
    })
})
