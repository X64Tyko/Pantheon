import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { api, authHeaders, TOKEN_KEY } from '@/api/client'

// ---------------------------------------------------------------------------
// Mock global fetch for all tests in this file
// ---------------------------------------------------------------------------

const mockFetch = vi.fn()
vi.stubGlobal('fetch', mockFetch)

function respondOk(body: unknown, status = 200) {
  mockFetch.mockResolvedValueOnce({
    ok: true,
    status,
    text: () => Promise.resolve(status === 204 ? '' : JSON.stringify(body)),
    json: () => Promise.resolve(body),
  })
}

function respondError(status: number, body: { error: string }) {
  mockFetch.mockResolvedValueOnce({
    ok: false,
    status,
    statusText: 'Error',
    text: () => Promise.resolve(JSON.stringify(body)),
    json: () => Promise.resolve(body),
  })
}

// ---------------------------------------------------------------------------

describe('api client — request routing', () => {
  beforeEach(() => mockFetch.mockReset())

  describe('getSources', () => {
    it('GETs /api/sources and returns parsed array', async () => {
      const sources = [{ source_id: 's1', source_type: 'plex', display_name: 'Plex', enabled: true }]
      respondOk(sources)
      const result = await api.getSources()
      expect(mockFetch).toHaveBeenCalledWith('/api/sources',
        expect.objectContaining({ method: 'GET' }))
      expect(result).toHaveLength(1)
      expect(result[0].source_id).toBe('s1')
    })
  })

  describe('getChannels', () => {
    it('GETs /api/channels', async () => {
      respondOk([])
      await api.getChannels()
      const [url, opts] = mockFetch.mock.calls[0]
      expect(url).toBe('/api/channels')
      expect(opts.method).toBe('GET')
      expect(opts.body).toBeUndefined()
    })
  })

  describe('createChannel', () => {
    it('POSTs /api/channels with JSON body and returns channel_id', async () => {
      respondOk({ channel_id: 'c1' })
      const result = await api.createChannel({ name: 'CNN', number: 1, timezone: 'UTC' })
      const [url, opts] = mockFetch.mock.calls[0]
      expect(url).toBe('/api/channels')
      expect(opts.method).toBe('POST')
      expect(JSON.parse(opts.body)).toMatchObject({ name: 'CNN', number: 1, timezone: 'UTC' })
      expect(opts.headers).toMatchObject({ 'Content-Type': 'application/json' })
      expect(result.channel_id).toBe('c1')
    })
  })

  describe('updateChannel', () => {
    it('PATCHes /api/channels/:id', async () => {
      respondOk(undefined, 204)
      await api.updateChannel('c1', { name: 'CNN International' })
      const [url, opts] = mockFetch.mock.calls[0]
      expect(url).toBe('/api/channels/c1')
      expect(opts.method).toBe('PATCH')
      expect(JSON.parse(opts.body)).toMatchObject({ name: 'CNN International' })
    })
  })

  describe('deleteChannel', () => {
    it('DELETEs /api/channels/:id with no body', async () => {
      respondOk(undefined, 204)
      await api.deleteChannel('c1')
      const [url, opts] = mockFetch.mock.calls[0]
      expect(url).toBe('/api/channels/c1')
      expect(opts.method).toBe('DELETE')
      expect(opts.body).toBeUndefined()
    })
  })

  describe('addBlockContent', () => {
    it('POSTs to the correct nested URL', async () => {
      respondOk({ id: 1, position: 0 })
      await api.addBlockContent('c1', 'b1', { content_type: 'show', content_id: 's1' })
      const [url] = mockFetch.mock.calls[0]
      expect(url).toBe('/api/channels/c1/blocks/b1/content')
    })
  })
})

// ---------------------------------------------------------------------------

describe('api client — error handling', () => {
  beforeEach(() => mockFetch.mockReset())

  it('throws the error field from a non-ok response body', async () => {
    respondError(404, { error: 'Channel not found' })
    await expect(api.getChannels()).rejects.toThrow('Channel not found')
  })

  it('falls back to statusText when body has no error field', async () => {
    mockFetch.mockResolvedValueOnce({
      ok: false,
      status: 500,
      statusText: 'Internal Server Error',
      text: () => Promise.resolve(''),
      json: () => Promise.reject(new Error('no json')),
    })
    await expect(api.getSources()).rejects.toThrow('Internal Server Error')
  })

  it('does not throw on 204 no-body responses', async () => {
    respondOk(undefined, 204)
    await expect(api.deleteChannel('c1')).resolves.toBeUndefined()
  })
})

// ---------------------------------------------------------------------------

describe('api client — query string builder (qs)', () => {
  beforeEach(() => mockFetch.mockReset())

  it('omits undefined params', async () => {
    respondOk({ items: [], total: 0 })
    await api.getShows({ genre: undefined, limit: 10 })
    const [url] = mockFetch.mock.calls[0]
    expect(url).toContain('limit=10')
    expect(url).not.toMatch(/genre/)
  })

  it('omits empty-string params', async () => {
    respondOk({ items: [], total: 0 })
    await api.getShows({ q: '' })
    const [url] = mockFetch.mock.calls[0]
    expect(url).not.toMatch(/[?&]q=/)
  })

  it('encodes special characters', async () => {
    respondOk({ items: [], total: 0 })
    // q gets folded into the canon `filter` query string (withCombinedFilter
    // in client.ts) rather than sent as its own q= param — see the
    // library-filter-syntax work this predates.
    await api.getShows({ q: 'Sci Fi & Fantasy' })
    const [url] = mockFetch.mock.calls[0]
    expect(url).toContain('filter=Sci%20Fi%20%26%20Fantasy')
  })
})

// ---------------------------------------------------------------------------

describe('api client — EPG endpoints', () => {
  beforeEach(() => mockFetch.mockReset())

  it('previewChannelEpg POSTs hours and seed in the JSON body, not the URL', async () => {
    respondOk([])
    await api.previewChannelEpg('c1', 4, 42)
    const [url, opts] = mockFetch.mock.calls[0]
    expect(url).toBe('/api/channels/c1/epg/preview')
    expect(opts.method).toBe('POST')
    expect(JSON.parse(opts.body)).toEqual({ hours: 4, seed: 42 })
  })

  it('previewChannelEpg omits hours/seed/blocks from the body when not given', async () => {
    respondOk([])
    await api.previewChannelEpg('c1')
    const [url, opts] = mockFetch.mock.calls[0]
    expect(url).toBe('/api/channels/c1/epg/preview')
    expect(JSON.parse(opts.body)).toEqual({})
  })

  it('getChannelEpg appends hours when provided', async () => {
    respondOk([])
    await api.getChannelEpg('c1', 12)
    const [url] = mockFetch.mock.calls[0]
    expect(url).toBe('/api/channels/c1/epg?hours=12')
  })
})

// ---------------------------------------------------------------------------
// authHeaders() / X-Pantheon-Surface — used directly by playbackApi.ts's
// /stream/* fetches (outside request()'s /api prefix), and folded into every
// request() call via authHeaders() itself (see client.ts). The surface is
// derived from window.location.pathname (isTvSurface() in client.ts), not a
// settable flag, so these tests drive it by changing the route.
// ---------------------------------------------------------------------------

describe('api client — auth headers and X-Pantheon-Surface', () => {
  beforeEach(() => mockFetch.mockReset())
  afterEach(() => {
    localStorage.removeItem(TOKEN_KEY)
    window.location.pathname = '/' // module-level-ish state — must not leak into other tests
  })

  it('authHeaders() is empty with no token and no active surface', () => {
    expect(authHeaders()).toEqual({})
  })

  it('authHeaders() includes a bearer token when one is stored', () => {
    localStorage.setItem(TOKEN_KEY, 'tok123')
    expect(authHeaders()).toEqual({ Authorization: 'Bearer tok123' })
  })

  it('authHeaders() includes X-Pantheon-Surface while on the /tv route', () => {
    window.location.pathname = '/tv'
    expect(authHeaders()).toMatchObject({ 'X-Pantheon-Surface': 'tv' })
  })

  it('authHeaders() includes X-Pantheon-Surface on a /tv/* subroute too', () => {
    window.location.pathname = '/tv/guide'
    expect(authHeaders()).toMatchObject({ 'X-Pantheon-Surface': 'tv' })
  })

  it('authHeaders() omits X-Pantheon-Surface once navigated away from /tv', () => {
    window.location.pathname = '/tv'
    window.location.pathname = '/'
    expect(authHeaders()).not.toHaveProperty('X-Pantheon-Surface')
  })

  it('request() sends no Authorization header when no token is set', async () => {
    respondOk([])
    await api.getChannels()
    const [, opts] = mockFetch.mock.calls[0]
    expect(opts.headers?.Authorization).toBeUndefined()
  })

  it('request() sends the bearer token once one is stored', async () => {
    localStorage.setItem(TOKEN_KEY, 'tok123')
    respondOk([])
    await api.getChannels()
    const [, opts] = mockFetch.mock.calls[0]
    expect(opts.headers.Authorization).toBe('Bearer tok123')
  })

  it('request() carries X-Pantheon-Surface while the /tv surface is active', async () => {
    window.location.pathname = '/tv'
    respondOk([])
    await api.getChannels()
    const [, opts] = mockFetch.mock.calls[0]
    expect(opts.headers['X-Pantheon-Surface']).toBe('tv')
  })

  it('dispatches kairos:unauthorized on a 401 response', async () => {
    const handler = vi.fn()
    window.addEventListener('kairos:unauthorized', handler)
    mockFetch.mockResolvedValueOnce({
      ok: false, status: 401, statusText: 'Unauthorized',
      text: () => Promise.resolve(JSON.stringify({ error: 'Unauthorized' })),
      json: () => Promise.resolve({ error: 'Unauthorized' }),
    })
    await expect(api.getChannels()).rejects.toThrow()
    expect(handler).toHaveBeenCalledTimes(1)
    window.removeEventListener('kairos:unauthorized', handler)
  })
})
