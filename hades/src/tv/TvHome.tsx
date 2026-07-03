import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api, mediaUrl } from '../api/client'
import type { Show, Movie, WatchProgress } from '../api/types'
import { useFocusable } from '../nav/useFocusable'
import { TvGuideSection } from './TvGuideSection'

const HOME_FOCUS_KEY = 'TV_HOME'

function isShow(item: Show | Movie): item is Show { return 'show_id' in item }
function thumbUrl(item: Show | Movie) {
  if (!item.thumb) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/thumb` : `/api/movies/${item.movie_id}/thumb`)
}

export function TvHome() {
  const navigate = useNavigate()
  const [recentShows,   setRecentShows]   = useState<Show[]>([])
  const [recentMovies,  setRecentMovies]  = useState<Movie[]>([])
  const [recentlyAired, setRecentlyAired] = useState<Show[]>([])
  const [continueWatching, setContinueWatching] = useState<WatchProgress[]>([])
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    Promise.all([
      api.getShows({ limit: 16, sort: 'recently_added' }),
      api.getMovies({ limit: 16, sort: 'recently_added' }),
      api.getShows({ limit: 16, sort: 'recently_aired' }).catch(() => ({ items: [] as Show[], total: 0 })),
      api.getWatchProgress().catch(() => []),
    ]).then(([sr, mr, ra, cw]) => {
      setRecentShows(sr.items)
      setRecentMovies(mr.items)
      setRecentlyAired(ra.items)
      setContinueWatching(cw)
    }).finally(() => setLoading(false))
  }, [])

  const { ref: homeRef, focusKey: homeFocusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: HOME_FOCUS_KEY, trackChildren: true, saveLastFocusedChild: true,
    preferredChildFocusKey: 'tv-quickaction-library',
  })

  return (
    <div ref={homeRef} style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <div style={{
        display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        padding: '32px 48px 8px', flexShrink: 0,
      }}>
        <span style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 24, fontWeight: 700,
          color: 'var(--hds-txt)', letterSpacing: '0.02em',
        }}>Pantheon</span>
        <LibraryButton onClick={() => navigate('/tv/library')} />
      </div>

      <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }} className="scrollbar-dark">
        {loading ? (
          <div style={{ padding: '24px 48px', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>Loading…</div>
        ) : (
          <>
            {continueWatching.length > 0 && (
              <TvShelf
                title="Continue Watching"
                items={continueWatching.map(p => ({
                  key: `${p.content_type}:${p.content_id}`,
                  title: p.content_type === 'episode' ? (p.show_title || p.title) : p.title,
                  thumb_url: p.content_type === 'movie'
                    ? mediaUrl(`/api/movies/${p.content_id}/thumb`)
                    : p.show_id ? mediaUrl(`/api/shows/${p.show_id}/thumb`) : undefined,
                  onClick: () => navigate(`/player/${p.content_type}/${p.content_id}?t=${p.position_ms}`),
                }))}
              />
            )}
            {recentShows.length > 0 && (
              <TvShelf
                title="Recently Added Shows"
                items={recentShows.map(s => ({
                  key: s.show_id, title: s.title, year: s.year, rating: s.audience_rating,
                  thumb_url: thumbUrl(s), onClick: () => navigate(`/tv/library/show/${s.show_id}`),
                }))}
              />
            )}
            {recentMovies.length > 0 && (
              <TvShelf
                title="Recently Added Movies"
                items={recentMovies.map(m => ({
                  key: m.movie_id, title: m.title, year: m.year, rating: m.audience_rating,
                  thumb_url: thumbUrl(m), onClick: () => navigate(`/tv/library/movie/${m.movie_id}`),
                }))}
              />
            )}
            {recentlyAired.filter(s => s.latest_episode).length > 0 && (
              <TvShelf
                title="Recently Aired"
                items={recentlyAired.filter(s => s.latest_episode).map(s => ({
                  key: s.show_id, title: s.title, year: s.year, rating: s.audience_rating,
                  thumb_url: thumbUrl(s), onClick: () => navigate(`/player/episode/${s.latest_episode!.episode_id}`),
                }))}
              />
            )}

            <div style={{ padding: '8px 0 64px' }}>
              <TvGuideSection />
            </div>
          </>
        )}
      </div>
    </div>
  )
}

function LibraryButton({ onClick }: { onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-quickaction-library', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      style={{
        display: 'flex', alignItems: 'center', gap: 10, cursor: 'pointer',
        padding: '11px 22px', borderRadius: 10,
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 14,
      }}
    >
      <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
        <rect x="1.5" y="1.5" width="4.5" height="11" rx="1" /><rect x="8" y="1.5" width="4.5" height="11" rx="1" />
      </svg>
      Library
    </button>
  )
}

interface TvShelfEntry { key: string; title: string; year?: number; rating?: number; thumb_url?: string; onClick: () => void }

function TvShelf({ title, items }: { title: string; items: TvShelfEntry[] }) {
  const scrollRef = useRef<HTMLDivElement>(null)
  return (
    <div style={{ marginBottom: 32 }}>
      <div style={{
        fontFamily: "'Chakra Petch', sans-serif", fontSize: 20, fontWeight: 600,
        color: 'var(--hds-txt)', padding: '0 48px 16px',
      }}>{title}</div>
      <div ref={scrollRef} style={{
        display: 'flex', gap: 20, overflowX: 'auto', overflowY: 'hidden',
        padding: '8px 48px', scrollbarWidth: 'none',
      }}>
        {items.map(item => (
          <div key={item.key} style={{ flexShrink: 0, width: 200 }}>
            <TvShelfCard {...item} />
          </div>
        ))}
      </div>
    </div>
  )
}

function TvShelfCard({ title, year, rating, thumb_url, onClick }: TvShelfEntry) {
  const [hovered, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = thumb_url && !imgErr
  const { ref, focused } = useFocusable<object, HTMLDivElement>({ focusKey: `tv-shelf-card-${title}-${year ?? ''}`, onEnterPress: onClick })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        borderRadius: 12, overflow: 'hidden', cursor: 'pointer',
        boxShadow: active ? '0 0 0 3px var(--hds-violet), 0 8px 32px oklch(0.55 0.14 292 / 0.35)' : 'none',
        transform: active ? 'scale(1.045)' : 'scale(1)',
        transition: 'transform .15s cubic-bezier(0.2,0,0.2,1), box-shadow .15s',
      }}
    >
      <div style={{
        aspectRatio: '2/3', width: '100%', position: 'relative',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
        display: 'flex', alignItems: 'center', justifyContent: 'center', overflow: 'hidden',
      }}>
        {showImg ? (
          <img src={thumb_url} alt={title} onError={() => setImgErr(true)} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />
        ) : (
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 36, color: 'var(--hds-violet)', opacity: 0.4 }}>
            {title.split(/\s+/).slice(0, 2).map(w => w[0]).join('').toUpperCase()}
          </span>
        )}
        {rating != null && (
          <div style={{
            position: 'absolute', top: 10, right: 10,
            background: 'oklch(0 0 0 / 0.6)', borderRadius: 6, padding: '3px 8px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-gold)',
          }}>★ {rating.toFixed(1)}</div>
        )}
      </div>
      <div style={{ padding: '10px 4px 2px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{title}</div>
        {year && <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', marginTop: 2 }}>{year}</div>}
      </div>
    </div>
  )
}
