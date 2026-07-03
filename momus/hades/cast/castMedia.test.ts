import { describe, it, expect } from 'vitest'
import { toAbsoluteStreamUrl, buildLoadRequest } from '@/cast/castMedia'

// The real chrome.cast.media.* classes only exist once the gstatic Cast
// Sender SDK script has loaded in a real browser (index.html) — never true
// in a vitest run. Minimal fakes here mirror just the fields castMedia.ts
// actually reads/writes, so buildLoadRequest() can run and be asserted on
// without needing the real SDK.
class FakeMediaInfo {
  contentId: string
  contentType: string
  streamType: unknown
  metadata: any
  constructor(contentId: string, contentType: string) {
    this.contentId = contentId
    this.contentType = contentType
  }
}
class FakeLoadRequest {
  media: FakeMediaInfo
  autoplay = false
  currentTime = 0
  constructor(media: FakeMediaInfo) { this.media = media }
}
class FakeImage {
  url: string
  constructor(url: string) { this.url = url }
}
class FakeMovieMediaMetadata {
  title = ''
  images: FakeImage[] = []
}
class FakeTvShowMediaMetadata {
  seriesTitle = ''
  title = ''
  season = 0
  episode = 0
  images: FakeImage[] = []
}

// castMedia.ts only touches chrome.cast.* inside buildLoadRequest()'s body,
// never at module-load time, so this just needs to be set before the first
// buildLoadRequest() call below, not before the import above.
;(globalThis as any).chrome = {
  cast: {
    Image: FakeImage,
    media: {
      MediaInfo:           FakeMediaInfo,
      LoadRequest:          FakeLoadRequest,
      MovieMediaMetadata:    FakeMovieMediaMetadata,
      TvShowMediaMetadata:   FakeTvShowMediaMetadata,
      StreamType: { LIVE: 'LIVE', BUFFERED: 'BUFFERED' },
    },
  },
}

// window.location.origin is stubbed in momus/hades/setup.ts to
// 'http://pantheon.local'.

describe('toAbsoluteStreamUrl', () => {
  it('resolves a relative path against window.location.origin', () => {
    expect(toAbsoluteStreamUrl('/stream/vod/abc/playlist.m3u8'))
      .toBe('http://pantheon.local/stream/vod/abc/playlist.m3u8')
  })
})

describe('buildLoadRequest', () => {
  it('builds an HLS MediaInfo with an absolute contentId', () => {
    const req = buildLoadRequest({
      manifestUrl: '/stream/vod/m1/playlist.m3u8',
      isLive: false, currentMs: 0,
      metadata: { title: 'Some Movie' },
    }) as unknown as FakeLoadRequest

    expect(req.media.contentId).toBe('http://pantheon.local/stream/vod/m1/playlist.m3u8')
    expect(req.media.contentType).toBe('application/x-mpegURL')
  })

  it('always sets autoplay', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: true, currentMs: 0, metadata: { title: 'X' },
    }) as unknown as FakeLoadRequest
    expect(req.autoplay).toBe(true)
  })

  it('uses BUFFERED stream type and sets currentTime (seconds) for VOD', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: false, currentMs: 45_000, metadata: { title: 'X' },
    }) as unknown as FakeLoadRequest
    expect(req.media.streamType).toBe('BUFFERED')
    expect(req.currentTime).toBe(45)
  })

  it('uses LIVE stream type and leaves currentTime untouched for channels', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: true, currentMs: 999_000, metadata: { title: 'X' },
    }) as unknown as FakeLoadRequest
    expect(req.media.streamType).toBe('LIVE')
    expect(req.currentTime).toBe(0) // never assigned for live — VodSession.cpp EVENT-tag rationale in castMedia.ts
  })

  it('builds MovieMediaMetadata when no seriesTitle is given', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: false, currentMs: 0,
      metadata: { title: 'Some Movie' },
    }) as unknown as FakeLoadRequest
    expect(req.media.metadata).toBeInstanceOf(FakeMovieMediaMetadata)
    expect(req.media.metadata.title).toBe('Some Movie')
  })

  it('builds TvShowMediaMetadata with season/episode when seriesTitle is given', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: false, currentMs: 0,
      metadata: { title: 'Pilot', seriesTitle: 'Some Show', season: 1, episode: 4 },
    }) as unknown as FakeLoadRequest
    expect(req.media.metadata).toBeInstanceOf(FakeTvShowMediaMetadata)
    expect(req.media.metadata.seriesTitle).toBe('Some Show')
    expect(req.media.metadata.title).toBe('Pilot')
    expect(req.media.metadata.season).toBe(1)
    expect(req.media.metadata.episode).toBe(4)
  })

  it('attaches an absolute poster image when imageUrl is given', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: false, currentMs: 0,
      metadata: { title: 'X', imageUrl: '/api/movies/m1/thumb' },
    }) as unknown as FakeLoadRequest
    expect(req.media.metadata.images).toHaveLength(1)
    expect(req.media.metadata.images[0].url).toBe('http://pantheon.local/api/movies/m1/thumb')
  })

  it('omits images entirely when no imageUrl is given', () => {
    const req = buildLoadRequest({
      manifestUrl: '/x', isLive: false, currentMs: 0,
      metadata: { title: 'X' },
    }) as unknown as FakeLoadRequest
    expect(req.media.metadata.images).toHaveLength(0)
  })
})
