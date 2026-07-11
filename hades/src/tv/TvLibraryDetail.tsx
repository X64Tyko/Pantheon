import { useNavigate, useParams } from 'react-router-dom'
import { useMediaDetail } from '../components/media/useMediaDetail'
import { EpisodeShelf } from '../components/media/EpisodeShelf'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import { resolvePlayPath } from '../player/resolvePlayTarget'

export function TvLibraryDetail() {
  const navigate = useNavigate()
  const { type, id } = useParams<{ type: 'show' | 'movie'; id: string }>()
  useNavBack(() => navigate('/tv/library'))

  const {
    loading, detail, contentType, posterUrl, backdropUrl, title, year, overview, genres, rating,
    seasonsWithEpisodes, setFocusedEpisode,
  } = useMediaDetail({ id, content_type: type })

  const goPlay = async () => {
    if (!id || !contentType) return
    const path = await resolvePlayPath(contentType, id)
    if (path) navigate(path)
  }
  const goBack = () => navigate('/tv/library')

  const play = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-play', forceFocus: true, onEnterPress: goPlay })
  const back = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-back', onEnterPress: goBack })

  if (loading) {
    return <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
      <div className="hds-skeleton" style={{ width: 300, height: 450, borderRadius: 12 }} />
    </div>
  }

  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>
      {/* Fixed hero — stays pinned while the season shelves below scroll, so an
          episode focused deep in a long season list still visibly retargets
          the title/overview/backdrop above rather than scrolling out of view. */}
      <div style={{
        position: 'relative', minHeight: '52vh', flexShrink: 0,
        background: backdropUrl
          ? `url(${backdropUrl}) center/cover no-repeat`
          : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)',
      }}>
        <div style={{
          position: 'absolute', inset: 0,
          background: 'linear-gradient(to right, oklch(0 0 0 / 0.8) 0%, oklch(0 0 0 / 0.3) 55%, transparent 100%)',
        }} />
        <div style={{
          position: 'absolute', inset: 0,
          background: 'linear-gradient(to top, var(--hds-bg) 0%, transparent 50%)',
        }} />

        <div style={{ position: 'relative', zIndex: 2, display: 'flex', gap: 40, padding: '40px 48px', alignItems: 'flex-end', minHeight: '52vh' }}>
          <div style={{
            width: 220, height: 330, borderRadius: 12, overflow: 'hidden', flexShrink: 0,
            background: 'var(--hds-bg-3)', boxShadow: '0 12px 40px oklch(0 0 0 / 0.55)',
          }}>
            {posterUrl && <img src={posterUrl} alt={title} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />}
          </div>

          <div style={{ flex: 1, minWidth: 0, paddingBottom: 8 }}>
            <h1 style={{
              fontFamily: "'Chakra Petch', sans-serif", fontSize: 40, fontWeight: 700,
              color: '#fff', margin: '0 0 12px', lineHeight: 1.1,
            }}>{title}</h1>

            <div style={{
              display: 'flex', alignItems: 'center', gap: 18, marginBottom: 16,
              fontFamily: "'JetBrains Mono', monospace", fontSize: 15, color: 'var(--hds-txt-2)',
            }}>
              {year && <span>{year}</span>}
              {rating != null && <span style={{ color: 'var(--hds-gold)' }}>★ {rating.toFixed(1)}</span>}
              <span style={{ opacity: 0.6 }}>{contentType === 'show' ? 'series' : 'film'}</span>
            </div>

            {genres.length > 0 && (
              <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginBottom: 16 }}>
                {genres.map(g => (
                  <span key={g} style={{
                    fontFamily: "'JetBrains Mono', monospace", fontSize: 12,
                    padding: '4px 12px', borderRadius: 14,
                    background: 'var(--hds-glass)', border: '1px solid var(--hds-glass-border)',
                    color: 'var(--hds-txt-2)',
                  }}>{g}</span>
                ))}
              </div>
            )}

            {overview && (
              <p style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 15, lineHeight: 1.7,
                color: 'oklch(0.8 0.01 285)', margin: '0 0 24px', maxWidth: 760,
                display: '-webkit-box', WebkitLineClamp: 3, WebkitBoxOrient: 'vertical', overflow: 'hidden',
              }}>{overview}</p>
            )}

            <div style={{ display: 'flex', gap: 14 }}>
              <button
                ref={play.ref} data-tv-focused={play.focused}
                onClick={goPlay}
                style={{
                  display: 'flex', alignItems: 'center', gap: 10, cursor: 'pointer',
                  padding: '14px 28px', borderRadius: 10, border: 'none',
                  background: 'var(--hds-gold)', color: 'oklch(0.15 0.02 90)',
                  fontFamily: "'Chakra Petch', sans-serif", fontSize: 16, fontWeight: 700,
                }}
              >
                <svg width="16" height="16" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
                Play
              </button>
              <button
                ref={back.ref} data-tv-focused={back.focused}
                onClick={goBack}
                style={{
                  padding: '14px 28px', borderRadius: 10, cursor: 'pointer',
                  border: '1px solid var(--hds-glass-border)', background: 'var(--hds-glass)', color: '#fff',
                  fontFamily: "'Chakra Petch', sans-serif", fontSize: 16, fontWeight: 600,
                }}
              >Back</button>
            </div>
          </div>
        </div>
      </div>

      <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }} className="scrollbar-dark">
        {seasonsWithEpisodes.length > 0 && (
          <div style={{ padding: '8px 48px 64px' }}>
            {seasonsWithEpisodes.map(s => (
              <EpisodeShelf
                key={`${s.number}-${s.episodes[0]?.episode_id ?? ''}`}
                seasonNumber={s.number} seasonName={s.name} episodes={s.episodes}
                onEpisodeHover={setFocusedEpisode} onEpisodeHoverEnd={() => setFocusedEpisode(null)}
              />
            ))}
          </div>
        )}

        {!detail && (
          <div style={{ padding: 48, textAlign: 'center', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
            Not found.
          </div>
        )}
      </div>
    </div>
  )
}
