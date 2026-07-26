import {describe, it, expect, vi, beforeEach} from 'vitest'
import {
    subscribeWatchTogether,
    joinWatchTogether,
    postWatchTogetherCommand,
    postWatchTogetherHeartbeat,
    type WatchTogetherEvent,
} from '@/player/watchTogetherApi'
import {TOKEN_KEY} from '@/api/client'

// These hit Hermes's /watch-together/* routes directly via raw fetch/
// EventSource (not api/client.ts's request()), so — same approach as
// api/client.test.ts — global fetch is stubbed rather than mocking the
// whole module, and the real authHeaders()/TOKEN_KEY come along for free.

const mockFetch = vi.fn()
vi.stubGlobal('fetch', mockFetch)

function respondOk(body: unknown, status = 200) {
    mockFetch.mockResolvedValueOnce({
        ok: true,
        status,
        statusText: 'OK',
        json: () => Promise.resolve(body),
    })
}

function respondError(status: number, statusText: string) {
    mockFetch.mockResolvedValueOnce({ok: false, status, statusText})
}

// vitest's `environment: 'node'` (vitest.config.ts) has no EventSource
// ambient global — same stub-per-test approach as SystemStore.test.ts.
class FakeEventSource {
    static instances: FakeEventSource[] = []
    url: string
    onmessage: ((e: { data: string }) => void) | null = null
    closed = false

    constructor(url: string) {
        this.url = url
        FakeEventSource.instances.push(this)
    }

    close() {
        this.closed = true
    }
}

function currentES(): FakeEventSource {
    return FakeEventSource.instances[FakeEventSource.instances.length - 1]
}

describe('watchTogetherApi', () => {
    beforeEach(() => {
        mockFetch.mockReset()
        FakeEventSource.instances = []
        ;(globalThis as any).EventSource = FakeEventSource
    })

    // ── subscribeWatchTogether ─────────────────────────────────────────────

    describe('subscribeWatchTogether', () => {
        it('opens an EventSource against /watch-together/:id/stream with no token', () => {
            const es = subscribeWatchTogether('s1', vi.fn())
            expect(FakeEventSource.instances).toHaveLength(1)
            expect(currentES().url).toBe('/watch-together/s1/stream')
            expect(es).toBe(currentES())
        })

        it('appends ?token= when a token is stored', () => {
            localStorage.setItem(TOKEN_KEY, 'abc 123')
            subscribeWatchTogether('s1', vi.fn())
            expect(currentES().url).toBe('/watch-together/s1/stream?token=abc%20123')
        })

        it('parses JSON messages and forwards them to onEvent', () => {
            const onEvent = vi.fn()
            subscribeWatchTogether('s1', onEvent)
            const event: WatchTogetherEvent = {type: 'sync', position_ms: 5000, paused: false}
            currentES().onmessage?.({data: JSON.stringify(event)})
            expect(onEvent).toHaveBeenCalledWith(event)
        })

        it('silently ignores malformed/ping messages instead of throwing', () => {
            const onEvent = vi.fn()
            subscribeWatchTogether('s1', onEvent)
            expect(() => currentES().onmessage?.({data: 'not json'})).not.toThrow()
            expect(onEvent).not.toHaveBeenCalled()
        })
    })

    // ── joinWatchTogether ──────────────────────────────────────────────────

    describe('joinWatchTogether', () => {
        it('POSTs /watch-together/:id/join with auth headers and returns the parsed session', async () => {
            const session = {session_id: 's1', position_ms: 42000, paused: true}
            respondOk(session)
            localStorage.setItem(TOKEN_KEY, 'tok')
            const result = await joinWatchTogether('s1')
            const [url, opts] = mockFetch.mock.calls[0]
            expect(url).toBe('/watch-together/s1/join')
            expect(opts.method).toBe('POST')
            expect(opts.headers).toMatchObject({Authorization: 'Bearer tok'})
            expect(result).toEqual(session)
        })

        it('throws with statusText on a non-ok response', async () => {
            respondError(403, 'Forbidden')
            await expect(joinWatchTogether('s1')).rejects.toThrow('Forbidden')
        })

        it('propagates a network failure (fetch rejection)', async () => {
            mockFetch.mockRejectedValueOnce(new Error('network down'))
            await expect(joinWatchTogether('s1')).rejects.toThrow('network down')
        })
    })

    // ── postWatchTogetherCommand ───────────────────────────────────────────

    describe('postWatchTogetherCommand', () => {
        it('POSTs the command as a JSON body with Content-Type', async () => {
            respondOk({})
            await postWatchTogetherCommand('s1', {type: 'seek', position_ms: 15000})
            const [url, opts] = mockFetch.mock.calls[0]
            expect(url).toBe('/watch-together/s1/command')
            expect(opts.method).toBe('POST')
            expect(opts.headers).toMatchObject({'Content-Type': 'application/json'})
            expect(JSON.parse(opts.body)).toEqual({type: 'seek', position_ms: 15000})
        })

        it('omits position_ms for pause/play commands that do not carry one', async () => {
            respondOk({})
            await postWatchTogetherCommand('s1', {type: 'pause'})
            const [, opts] = mockFetch.mock.calls[0]
            expect(JSON.parse(opts.body)).toEqual({type: 'pause'})
        })

        it('never rejects on a network failure — fire and forget', async () => {
            mockFetch.mockRejectedValueOnce(new Error('network down'))
            await expect(postWatchTogetherCommand('s1', {type: 'play'})).resolves.toBeUndefined()
        })

        it('never rejects on a non-ok response either — status is never inspected', async () => {
            respondError(500, 'Internal Server Error')
            await expect(postWatchTogetherCommand('s1', {type: 'play'})).resolves.toBeUndefined()
        })
    })

    // ── postWatchTogetherHeartbeat ─────────────────────────────────────────

    describe('postWatchTogetherHeartbeat', () => {
        it('POSTs a rounded position_ms and paused flag', async () => {
            respondOk({})
            await postWatchTogetherHeartbeat('s1', 12345.6, false)
            const [url, opts] = mockFetch.mock.calls[0]
            expect(url).toBe('/watch-together/s1/heartbeat')
            expect(opts.method).toBe('POST')
            expect(JSON.parse(opts.body)).toEqual({position_ms: 12346, paused: false})
        })

        it('never rejects on a network failure — fire and forget', async () => {
            mockFetch.mockRejectedValueOnce(new Error('network down'))
            await expect(postWatchTogetherHeartbeat('s1', 1000, true)).resolves.toBeUndefined()
        })

        it('never rejects on a non-ok response either — status is never inspected', async () => {
            respondError(500, 'Internal Server Error')
            await expect(postWatchTogetherHeartbeat('s1', 1000, true)).resolves.toBeUndefined()
        })
    })
})
