import { describe, it, expect, vi, beforeEach } from 'vitest'
import { api } from '@/api/client'

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
    await api.getShows({ q: 'Sci Fi & Fantasy' })
    const [url] = mockFetch.mock.calls[0]
    expect(url).toContain('q=Sci%20Fi%20%26%20Fantasy')
  })
})

// ---------------------------------------------------------------------------

describe('api client — EPG endpoints', () => {
  beforeEach(() => mockFetch.mockReset())

  it('previewChannelEpg builds correct URL with hours and seed', async () => {
    respondOk([])
    await api.previewChannelEpg('c1', 4, 42)
    const [url] = mockFetch.mock.calls[0]
    expect(url).toContain('/api/channels/c1/epg/preview')
    expect(url).toContain('hours=4')
    expect(url).toContain('seed=42')
  })

  it('previewChannelEpg emits no query string when no params given', async () => {
    respondOk([])
    await api.previewChannelEpg('c1')
    const [url] = mockFetch.mock.calls[0]
    expect(url).toBe('/api/channels/c1/epg/preview')
  })

  it('getChannelEpg appends hours when provided', async () => {
    respondOk([])
    await api.getChannelEpg('c1', 12)
    const [url] = mockFetch.mock.calls[0]
    expect(url).toBe('/api/channels/c1/epg?hours=12')
  })
})
