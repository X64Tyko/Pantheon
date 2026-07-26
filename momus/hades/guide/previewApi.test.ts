import {describe, it, expect, vi, beforeEach} from 'vitest'
import {startPreview, switchPreview, stopPreview} from '@/guide/previewApi'
import {TOKEN_KEY} from '@/api/client'

// The literal client-side interface to Hephaestus's PreviewSession endpoints
// (POST /stream/preview/start|:id/switch|:id/stop — see Router.cpp). Thin
// fetch wrappers, but worth pinning down exactly: the manifest_url comes
// straight through from the server response (never derived/rewritten
// client-side — its stability across switchChannel() calls is entirely a
// server-side guarantee, see PreviewSession.h), a switch never re-sends
// hdr_capable (only start does, per PreviewSession's constructor comment),
// and stopPreview is fire-and-forget (never throws, even on a network
// failure — callers, e.g. useGuideSession's unmount cleanup, don't await it).

const mockFetch = vi.fn()
vi.stubGlobal('fetch', mockFetch)

function respondOk(body: unknown) {
    mockFetch.mockResolvedValueOnce({
        ok: true,
        status: 200,
        json: () => Promise.resolve(body),
    })
}

function respondError(status: number, body: { error: string }) {
    mockFetch.mockResolvedValueOnce({
        ok: false,
        status,
        statusText: 'Error',
        json: () => Promise.resolve(body),
    })
}

describe('previewApi', () => {
    beforeEach(() => {
        mockFetch.mockReset()
        localStorage.clear()
    })

    describe('startPreview', () => {
        it('POSTs channel_id + hdr_capable and returns the server-issued session', async () => {
            respondOk({session_id: 's1', manifest_url: '/stream/preview/s1/playlist.m3u8'})
            const res = await startPreview('ch1')
            expect(mockFetch).toHaveBeenCalledTimes(1)
            const [url, opts] = mockFetch.mock.calls[0]
            expect(url).toBe('/stream/preview/start')
            expect(opts.method).toBe('POST')
            const body = JSON.parse(opts.body)
            expect(body.channel_id).toBe('ch1')
            expect(typeof body.hdr_capable).toBe('boolean')
            expect(res).toEqual({session_id: 's1', manifest_url: '/stream/preview/s1/playlist.m3u8'})
        })

        it('carries the bearer token when logged in', async () => {
            localStorage.setItem(TOKEN_KEY, 'tok-123')
            respondOk({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            await startPreview('ch1')
            const [, opts] = mockFetch.mock.calls[0]
            expect(opts.headers.Authorization).toBe('Bearer tok-123')
        })

        it('has no Authorization header when logged out', async () => {
            respondOk({session_id: 's1', manifest_url: '/m/s1.m3u8'})
            await startPreview('ch1')
            const [, opts] = mockFetch.mock.calls[0]
            expect(opts.headers.Authorization).toBeUndefined()
        })

        it('throws with the server error message on a non-ok response', async () => {
            respondError(403, {error: 'Restricted'})
            await expect(startPreview('ch1')).rejects.toThrow('Restricted')
        })

        it('falls back to statusText when the error body is not JSON', async () => {
            mockFetch.mockResolvedValueOnce({
                ok: false, status: 500, statusText: 'Internal Server Error',
                json: () => Promise.reject(new Error('not json')),
            })
            await expect(startPreview('ch1')).rejects.toThrow('Internal Server Error')
        })
    })

    describe('switchPreview', () => {
        it('POSTs the new channel_id to the session-scoped switch route', async () => {
            respondOk({ok: true})
            await switchPreview('s1', 'ch2')
            const [url, opts] = mockFetch.mock.calls[0]
            expect(url).toBe('/stream/preview/s1/switch')
            expect(opts.method).toBe('POST')
            expect(JSON.parse(opts.body)).toEqual({channel_id: 'ch2'})
        })

        it('does not resolve hdr_capable again — switch has no such field', async () => {
            respondOk({ok: true})
            await switchPreview('s1', 'ch2')
            const [, opts] = mockFetch.mock.calls[0]
            expect(JSON.parse(opts.body)).not.toHaveProperty('hdr_capable')
        })
    })

    describe('stopPreview', () => {
        it('POSTs to the session-scoped stop route', () => {
            mockFetch.mockResolvedValueOnce({ok: true, status: 200, json: () => Promise.resolve({ok: true})})
            stopPreview('s1')
            const [url, opts] = mockFetch.mock.calls[0]
            expect(url).toBe('/stream/preview/s1/stop')
            expect(opts.method).toBe('POST')
        })

        it('never throws, even when the request rejects outright', async () => {
            mockFetch.mockRejectedValueOnce(new Error('network down'))
            expect(() => stopPreview('s1')).not.toThrow()
            // Let the swallowed rejection's microtask settle so it doesn't leak
            // into another test as an unhandled rejection.
            await new Promise(r => setTimeout(r, 0))
        })
    })
})
