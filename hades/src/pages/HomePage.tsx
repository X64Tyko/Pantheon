import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api, mediaUrl } from '../api/client'
import type { Channel, Show, Movie, ShowDetail, MovieDetail, ScraperStats } from '../api/types'
import { MediaDetailHero } from '../components/media/MediaDetailHero'
import { getScrollPos, saveScrollPos } from '../hooks/scrollMemory'
import { ghostBtnStyle, goldBtnStyle } from '../channel/styles'

const SCROLL_KEY = 'home'

// ── Helpers ──────────────────────────────────────────────────────────────────

function isShow(item: Show | Movie): item is Show { return 'show_id' in item }

function proxyThumb(item: Show | Movie) {
  if (!item.thumb) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/thumb` : `/api/movies/${item.movie_id}/thumb`)
}
function proxyArt(item: Show | Movie) {
  if (!item.art) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/art` : `/api/movies/${item.movie_id}/art`)
}

// ── Component ────────────────────────────────────────────────────────────────

export default function HomePage() {
  const navigate = useNavigate()
  const scrollContainerRef = useRef<HTMLDivElement>(null)
  const savedScrollY       = useRef(0)
  const restoredScrollRef  = useRef(false)
  const allItemsRef        = useRef<Map<string, Show | Movie>>(new Map())

  // Data
  const [recentShows,  setRecentShows]  = useState<Show[]>([])
  const [recentMovies, setRecentMovies] = useState<Movie[]>([])
  const [channels,     setChannels]     = useState<Channel[]>([])
  const [stats,        setStats]        = useState<ScraperStats | null>(null)
  const [loading,      setLoading]      = useState(true)

  // Hero
  const heroCandidates    = useRef<(Show | Movie)[]>([])
  const heroIdx           = useRef(0)
  const heroBeforeDetail  = useRef<{ item: Show | Movie; detail: ShowDetail | MovieDetail | null; idx: number } | null>(null)
  const [heroItem,   setHeroItem]   = useState<Show | Movie | null>(null)
  const [heroDetail, setHeroDetail] = useState<ShowDetail | MovieDetail | null>(null)
  const [heroFading, setHeroFading] = useState(false)
  const heroIntervalRef = useRef<ReturnType<typeof setInterval> | null>(null)

  // Detail view
  const [detailOpen, setDetailOpen] = useState(false)
  const [detailId,   setDetailId]   = useState<string | null>(null)
  const [detailType, setDetailType] = useState<'show' | 'movie' | null>(null)

  // Crossfade
  const [transitioning, setTransitioning] = useState(false)

  // ── Hero helpers ────────────────────────────────────────────────────────────

  const loadHeroDetail = (item: Show | Movie) => {
    const p = isShow(item) ? api.getShow(item.show_id) : api.getMovie(item.movie_id)
    p.then(d => setHeroDetail(d)).catch(() => {})
  }

  const transitionHeroTo = (item: Show | Movie, detail: ShowDetail | MovieDetail | null = null) => {
    setHeroFading(true)
    setTimeout(() => {
      setHeroItem(item)
      setHeroDetail(detail)
      setHeroFading(false)
      if (!detail) loadHeroDetail(item)
    }, 260)
  }

  const goToHero = (idx: number) => {
    const candidates = heroCandidates.current
    if (!candidates.length) return
    transitionHeroTo(candidates[idx % candidates.length])
    heroIdx.current = idx % candidates.length
  }

  const startRotation = () => {
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    heroIntervalRef.current = setInterval(() => goToHero(heroIdx.current + 1), 9000)
  }

  // ── Data load ───────────────────────────────────────────────────────────────

  useEffect(() => {
    setLoading(true)
    Promise.all([
      api.getShows({ limit: 24, sort: 'recently_added' }),
      api.getMovies({ limit: 16, sort: 'recently_added' }),
      api.getChannels(),
      api.getScraperStats().catch(() => null),
    ]).then(([sr, mr, chs, st]) => {
      setRecentShows(sr.items)
      setRecentMovies(mr.items)
      setChannels(chs)
      setStats(st)

      sr.items.forEach(s => allItemsRef.current.set(s.show_id, s))
      mr.items.forEach(m => allItemsRef.current.set(m.movie_id, m))

      const withArt = [...sr.items.filter(s => s.art), ...mr.items.filter(m => m.art)]
      heroCandidates.current = withArt
      const first = withArt[0] ?? sr.items[0]
      if (first) { heroIdx.current = 0; setHeroItem(first); loadHeroDetail(first) }
    }).finally(() => {
      setLoading(false)
      if (!restoredScrollRef.current) {
        restoredScrollRef.current = true
        setTimeout(() => scrollContainerRef.current?.scrollTo({ top: getScrollPos(SCROLL_KEY) }), 32)
      }
    })
  }, [])

  useEffect(() => {
    if (heroItem && !detailOpen) startRotation()
    return () => { if (heroIntervalRef.current) clearInterval(heroIntervalRef.current) }
  }, [heroItem?.art, detailOpen]) // eslint-disable-line react-hooks/exhaustive-deps

  // ── Detail open / close ─────────────────────────────────────────────────────

  const openDetail = (id: string, type: 'show' | 'movie') => {
    if (heroIntervalRef.current) clearInterval(heroIntervalRef.current)
    savedScrollY.current = scrollContainerRef.current?.scrollTop ?? 0

    // Transition hero to selected item's backdrop (if it has one)
    const listItem = allItemsRef.current.get(id)
    if (listItem?.art && heroItem) {
      heroBeforeDetail.current = { item: heroItem, detail: heroDetail, idx: heroIdx.current }
      transitionHeroTo(listItem, null)
    }

    // Crossfade shelves → detail
    setTransitioning(true)
    setTimeout(() => {
      setDetailId(id)
      setDetailType(type)
      setDetailOpen(true)
      setTransitioning(false)
      scrollContainerRef.current?.scrollTo({ top: 0 })
    }, 200)
  }

  const closeDetail = () => {
    // Restore hero
    if (heroBeforeDetail.current) {
      const { item, detail, idx } = heroBeforeDetail.current
      heroIdx.current = idx
      transitionHeroTo(item, detail)
      heroBeforeDetail.current = null
    }

    // Crossfade detail → shelves
    setTransitioning(true)
    setTimeout(() => {
      setDetailOpen(false)
      setDetailId(null)
      setDetailType(null)
      setTransitioning(false)
      // Restore scroll after content mounts
      setTimeout(() => {
        scrollContainerRef.current?.scrollTo({ top: savedScrollY.current })
      }, 32)
    }, 200)
  }

  const needsReview = stats ? stats.uncertain + stats.unmatched : 0

  return (
    <div style={{ height: '100%', overflow: 'hidden' }}>
      <div
        ref={scrollContainerRef}
        onScroll={e => saveScrollPos(SCROLL_KEY, e.currentTarget.scrollTop)}
        style={{ height: '100%', overflowY: 'auto', background: 'var(--hds-bg)' }}
        className="scrollbar-dark"
      >
        {/* Hero */}
        {loading ? (
          <div className="hds-skeleton" style={{ height: '62vh', minHeight: 360 }} />
        ) : heroItem ? (
          <HeroPanel
            item={heroItem}
            detail={heroDetail}
            fading={heroFading}
            detailMode={detailOpen}
            totalCandidates={heroCandidates.current.length}
            currentIdx={heroIdx.current}
            reviewCount={needsReview}
            onViewDetail={() => openDetail(
              isShow(heroItem) ? heroItem.show_id : heroItem.movie_id,
              isShow(heroItem) ? 'show' : 'movie',
            )}
            onDotClick={i => { startRotation(); goToHero(i) }}
            onReviewClick={() => navigate('/review')}
          />
        ) : (
          <EmptyHero onGoToSources={() => navigate('/sources')} />
        )}

        {/* Crossfading content area */}
        <div style={{
          opacity:    transitioning ? 0 : 1,
          transition: 'opacity .2s ease',
          position: 'relative', zIndex: 2,
          minHeight: '40vh',
        }}>
          {detailOpen && detailId && detailType ? (
            <MediaDetailHero
              id={detailId}
              content_type={detailType}
              onBack={closeDetail}
              showBackdrop={false}
            />
          ) : (
            <Shelves
              loading={loading}
              recentShows={recentShows}
              recentMovies={recentMovies}
              channels={channels}
              onItemClick={openDetail}
              onNavigate={navigate}
            />
          )}
        </div>
      </div>
    </div>
  )
}

// ── Hero panel ────────────────────────────────────────────────────────────────

function HeroPanel({
  item, detail, fading, detailMode, totalCandidates, currentIdx,
  reviewCount, onViewDetail, onDotClick, onReviewClick,
}: {
  item:            Show | Movie
  detail:          ShowDetail | MovieDetail | null
  fading:          boolean
  detailMode:      boolean
  totalCandidates: number
  currentIdx:      number
  reviewCount:     number
  onViewDetail:    () => void
  onDotClick:      (i: number) => void
  onReviewClick:   () => void
}) {
  const backdrop = proxyArt(item)
  const bg = backdrop
    ? `url(${backdrop}) center/cover no-repeat`
    : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)'

  const genres: string[] = detail && 'genres' in detail && Array.isArray(detail.genres)
    ? (detail.genres as string[]).slice(0, 4) : []
  const overview = detail?.overview ?? ''
  const rating   = detail?.audience_rating ?? item.audience_rating

  return (
    <div style={{
      position: 'relative', height: '62vh', minHeight: 360,
      background: bg, flexShrink: 0,
      opacity: fading ? 0 : 1, transition: 'opacity .26s ease',
      marginBottom: -60, paddingBottom: 60, overflow: 'visible',
    }}>
      {/* Side dim — lightens in detail mode */}
      <div style={{
        position: 'absolute', inset: 0,
        background: 'linear-gradient(to right, oklch(0 0 0 / 0.88) 0%, oklch(0 0 0 / 0.42) 52%, transparent 100%)',
        opacity: detailMode ? 0.28 : 1,
        transition: 'opacity .5s ease',
      }} />
      {/* Bottom bleed — always solid */}
      <div style={{
        position: 'absolute', inset: 0,
        background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 44%)',
        paddingBottom: 60, marginBottom: -60,
      }} />
      {/* Extended bottom bleed below the hero */}
      <div style={{
        position: 'absolute', bottom: -60, left: 0, right: 0, height: 90, zIndex: 1,
        background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 100%)',
        pointerEvents: 'none',
      }} />

      {/* Text content — fades out in detail mode */}
      <div style={{
        position: 'absolute', bottom: 0, left: 0, padding: '0 64px 56px', maxWidth: 640,
        opacity: detailMode ? 0 : 1, transition: 'opacity .35s ease',
        pointerEvents: detailMode ? 'none' : 'auto',
      }}>
        {genres.length > 0 && (
          <div style={{ display: 'flex', gap: 8, marginBottom: 14, flexWrap: 'wrap' }}>
            {genres.map(g => (
              <span key={g} style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                padding: '3px 10px', borderRadius: 12,
                background: 'var(--hds-glass)', border: '1px solid var(--hds-glass-border)',
                color: 'var(--hds-txt-2)', letterSpacing: '0.06em',
              }}>{g}</span>
            ))}
          </div>
        )}

        <h1 style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 34, fontWeight: 700,
          color: 'oklch(1 0 0)', margin: 0, lineHeight: 1.1, letterSpacing: '-0.02em',
        }}>{item.title}</h1>

        <div style={{
          display: 'flex', alignItems: 'center', gap: 16, marginTop: 10, marginBottom: 14,
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-2)',
        }}>
          {item.year && <span>{item.year}</span>}
          {rating != null && <span style={{ color: 'var(--hds-gold)' }}>★ {rating.toFixed(1)}</span>}
          <span style={{ opacity: 0.5 }}>{'show_id' in item ? 'series' : 'film'}</span>
        </div>

        {overview && (
          <p style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 12, lineHeight: 1.6,
            color: 'oklch(0.75 0.01 285)', margin: '0 0 22px',
            display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical', overflow: 'hidden',
          }}>{overview}</p>
        )}

        <button style={goldBtnStyle} onClick={onViewDetail}>View Details</button>
      </div>

      {/* Review notification pill */}
      {reviewCount > 0 && (
        <button
          onClick={onReviewClick}
          style={{
            position: 'absolute', top: 18, right: 24, zIndex: 3,
            display: 'flex', alignItems: 'center', gap: 7,
            padding: '5px 12px', borderRadius: 20,
            border: '1px solid var(--hds-match-amber)',
            background: 'oklch(0.75 0.12 80 / 0.12)',
            backdropFilter: 'blur(8px)',
            color: 'var(--hds-match-amber)',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            cursor: 'pointer', letterSpacing: '0.06em',
          }}
        >
          <svg width="10" height="10" viewBox="0 0 10 10" fill="currentColor">
            <path d="M5 1L9.33 8.5H.67L5 1z" />
            <rect x="4.5" y="4" width="1" height="2.5" fill="var(--hds-bg)" />
            <rect x="4.5" y="7" width="1" height="1" fill="var(--hds-bg)" />
          </svg>
          {reviewCount} need{reviewCount === 1 ? 's' : ''} review
        </button>
      )}

      {/* Rotation dots — hidden in detail mode */}
      {totalCandidates > 1 && !detailMode && (
        <div style={{ position: 'absolute', bottom: 20, right: 24, display: 'flex', gap: 6, zIndex: 2 }}>
          {Array.from({ length: Math.min(totalCandidates, 8) }, (_, i) => (
            <button key={i} onClick={() => onDotClick(i)} style={{
              width: i === currentIdx ? 18 : 6, height: 6, borderRadius: 3,
              border: 'none', cursor: 'pointer', padding: 0,
              background: i === currentIdx ? 'var(--hds-gold)' : 'oklch(1 0 0 / 0.3)',
              transition: 'width .2s, background .2s',
            }} />
          ))}
        </div>
      )}
    </div>
  )
}

function EmptyHero({ onGoToSources }: { onGoToSources: () => void }) {
  return (
    <div style={{
      height: '62vh', minHeight: 360, flexShrink: 0,
      display: 'flex', flexDirection: 'column', alignItems: 'center', justifyContent: 'center', gap: 16,
      background: 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
    }}>
      <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 15, color: 'var(--hds-txt-3)' }}>
        No content yet
      </div>
      <button style={ghostBtnStyle} onClick={onGoToSources}>Add a Source →</button>
    </div>
  )
}

// ── Shelves ───────────────────────────────────────────────────────────────────

function Shelves({
  loading, recentShows, recentMovies, channels, onItemClick, onNavigate,
}: {
  loading:      boolean
  recentShows:  Show[]
  recentMovies: Movie[]
  channels:     Channel[]
  onItemClick:  (id: string, type: 'show' | 'movie') => void
  onNavigate:   (path: string) => void
}) {
  return (
    <div style={{ padding: '20px 0 48px' }}>

      {loading ? (
        <><ShelfSkeleton /><ShelfSkeleton /></>
      ) : (
        <>
          {recentShows.length > 0 && (
            <Shelf
              title="Recently Added Shows"
              items={recentShows.map(s => ({
                id: s.show_id, title: s.title, year: s.year,
                thumb_url: proxyThumb(s), rating: s.audience_rating,
                content_type: 'show' as const,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={() => onNavigate('/library')}
            />
          )}
          {recentMovies.length > 0 && (
            <Shelf
              title="Recently Added Movies"
              items={recentMovies.map(m => ({
                id: m.movie_id, title: m.title, year: m.year,
                thumb_url: proxyThumb(m), rating: m.audience_rating,
                content_type: 'movie' as const,
              }))}
              onItemClick={(id, type) => onItemClick(id, type)}
              onViewAll={() => onNavigate('/library')}
            />
          )}
          {channels.length > 0 && (
            <ChannelsSection channels={channels} onChannelClick={id => onNavigate(`/channels/${id}`)} />
          )}
        </>
      )}
    </div>
  )
}

// ── Shelf ────────────────────────────────────────────────────────────────────

interface ShelfEntry {
  id: string; title: string; year?: number
  thumb_url?: string; rating?: number; content_type: 'show' | 'movie'
}

function Shelf({ title, items, onItemClick, onViewAll }: {
  title:       string
  items:       ShelfEntry[]
  onItemClick: (id: string, type: 'show' | 'movie') => void
  onViewAll?:  () => void
}) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const [showArrows, setShowArrows] = useState(false)
  const scroll = (d: 'left' | 'right') =>
    scrollRef.current?.scrollBy({ left: d === 'right' ? 340 : -340, behavior: 'smooth' })

  return (
    <div
      style={{ padding: '0 0 8px', marginBottom: 24, position: 'relative' }}
      onMouseEnter={() => setShowArrows(true)}
      onMouseLeave={() => setShowArrows(false)}
    >
      <div style={{
        display: 'flex', alignItems: 'baseline', justifyContent: 'space-between',
        padding: '0 24px', marginBottom: 14,
      }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
          color: 'var(--hds-txt)', letterSpacing: '0.03em',
        }}>{title}</span>
        {onViewAll && (
          <button onClick={onViewAll} style={{
            background: 'none', border: 'none', cursor: 'pointer',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            color: 'var(--hds-txt-3)', letterSpacing: '0.06em',
          }}>View All →</button>
        )}
      </div>

      {showArrows && <ShelfArrow side="left"  onClick={() => scroll('left')} />}
      <div ref={scrollRef} style={{
        display: 'flex', gap: 12, overflowX: 'auto', overflowY: 'hidden',
        padding: '8px 24px', scrollbarWidth: 'none',
      }}>
        {items.map(item => (
          <ShelfCard key={item.id} item={item} onClick={() => onItemClick(item.id, item.content_type)} />
        ))}
      </div>
      {showArrows && <ShelfArrow side="right" onClick={() => scroll('right')} />}
    </div>
  )
}

function ShelfCard({ item, onClick }: { item: ShelfEntry; onClick: () => void }) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = item.thumb_url && !imgErr

  return (
    <div
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        flexShrink: 0, width: 150, borderRadius: 10, overflow: 'hidden', cursor: 'pointer',
        boxShadow: hovered ? '0 0 0 2px var(--hds-violet)' : 'none',
        transform: hovered ? 'scale(1.04)' : 'scale(1)',
        transition: 'transform .15s cubic-bezier(0.2,0,0.2,1), box-shadow .15s',
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {showImg ? (
          <img
            src={item.thumb_url} alt={item.title} onError={() => setImgErr(true)}
            style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          />
        ) : (
          <span style={{
            fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
            fontSize: 26, color: 'var(--hds-violet)', opacity: 0.4,
          }}>
            {item.title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {hovered && (
          <div style={{
            position: 'absolute', bottom: 0, left: 0, right: 0,
            background: 'linear-gradient(to top, oklch(0 0 0 / 0.85), transparent)',
            padding: '24px 10px 10px',
          }}>
            <div style={{
              fontFamily: "'Chakra Petch', sans-serif", fontSize: 10, fontWeight: 600,
              color: '#fff', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
            }}>{item.title}</div>
            {item.rating != null && (
              <div style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                color: 'var(--hds-gold)', marginTop: 2,
              }}>★ {item.rating.toFixed(1)}</div>
            )}
          </div>
        )}
      </div>
      <div style={{ padding: '8px 4px 4px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{item.title}</div>
        <div style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 2,
        }}>
          {item.year}{item.year && ' · '}{item.content_type}
        </div>
      </div>
    </div>
  )
}

function ShelfArrow({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button onClick={onClick} style={{
      position: 'absolute', top: '50%', transform: 'translateY(-50%)',
      left: side === 'left' ? 4 : undefined, right: side === 'right' ? 4 : undefined,
      zIndex: 2, width: 36, height: 36, borderRadius: '50%', cursor: 'pointer',
      background: 'var(--hds-glass)', backdropFilter: 'blur(8px)',
      border: '1px solid var(--hds-glass-border)', color: 'var(--hds-txt)', fontSize: 22,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}

// ── Channels section ──────────────────────────────────────────────────────────

function ChannelsSection({ channels, onChannelClick }: {
  channels:       Channel[]
  onChannelClick: (id: string) => void
}) {
  return (
    <div style={{ padding: '0 0 8px' }}>
      <div style={{ padding: '0 24px', marginBottom: 14 }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
          color: 'var(--hds-txt)', letterSpacing: '0.03em',
        }}>Channels</span>
      </div>
      <div style={{
        display: 'flex', gap: 12, overflowX: 'auto', overflowY: 'hidden',
        padding: '8px 24px', scrollbarWidth: 'none',
      }}>
        {channels.map(ch => (
          <ChannelCard key={ch.channel_id} channel={ch} onClick={() => onChannelClick(ch.channel_id)} />
        ))}
      </div>
    </div>
  )
}

function ChannelCard({ channel, onClick }: { channel: Channel; onClick: () => void }) {
  const [hovered, setHovered] = useState(false)
  return (
    <button
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        flexShrink: 0, width: 160, height: 90, borderRadius: 10,
        border: `1px solid ${hovered ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: hovered ? 'var(--hds-bg-3)' : 'var(--hds-bg-2)',
        cursor: 'pointer', display: 'flex', flexDirection: 'column',
        alignItems: 'center', justifyContent: 'center', gap: 6,
        transition: 'border-color .14s, background .14s, box-shadow .14s',
        boxShadow: hovered ? '0 4px 20px oklch(0.55 0.14 292 / 0.2)' : 'none',
      }}
    >
      <div style={{
        fontFamily: "'Chakra Petch', sans-serif", fontSize: 22, fontWeight: 800,
        color: hovered ? 'var(--hds-gold)' : 'var(--hds-txt-2)', lineHeight: 1, transition: 'color .14s',
      }}>{channel.number}</div>
      <div style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)',
        maxWidth: 130, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
      }}>{channel.name}</div>
    </button>
  )
}

// ── Skeletons ─────────────────────────────────────────────────────────────────

function ShelfSkeleton() {
  return (
    <div style={{ marginBottom: 32 }}>
      <div className="hds-skeleton" style={{ height: 14, width: 180, borderRadius: 4, margin: '0 24px 14px' }} />
      <div style={{ display: 'flex', gap: 12, padding: '8px 24px' }}>
        {Array.from({ length: 7 }, (_, i) => (
          <div key={i} className="hds-skeleton" style={{ width: 150, aspectRatio: '2/3', borderRadius: 10, flexShrink: 0 }} />
        ))}
      </div>
    </div>
  )
}
