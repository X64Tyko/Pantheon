import { useEffect, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import { useMediaDetail } from '../components/media/useMediaDetail'
import { useScrollCollapse } from '../components/media/useScrollCollapse'
import { EpisodeShelf } from '../components/media/EpisodeShelf'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import { resolvePlayPath } from '../player/resolvePlayTarget'
import { peekDetailReturnTo, clearPendingDetailReturn } from './tvDetailNav'

// Sized larger than desktop's collapsed header — this is a TV, not a phone,
// so even the locked-slim state keeps real weight for 10-foot viewing.
const HERO_HEIGHT  = 620
const HERO_OVERLAP = 60

export function TvLibraryDetail() {
  const navigate = useNavigate()
  const { type, id } = useParams<{ type: 'show' | 'movie'; id: string }>()
  // Wherever the card that opened this was clicked from (Home or Library) —
  // set right before navigate() by that page, read once here and held for
  // this whole visit. Falls back to Library for any other way of landing on
  // this route (e.g. a future deep link) — see tvDetailNav.ts.
  const [returnTo] = useState(() => peekDetailReturnTo())
  useNavBack(() => navigate(returnTo))

  // Safety net for leaving via something other than Back (e.g. Play) — the
  // legitimate Back path already consumes the pending entry on the
  // destination's own mount, before this cleanup runs; see tvDetailNav.ts.
  useEffect(() => () => clearPendingDetailReturn(), [])

  const {
    loading, detail, contentType, posterUrl, backdropUrl, title, year, overview, genres, rating,
    seasonsWithEpisodes, setFocusedEpisode,
  } = useMediaDetail({ id, content_type: type })

  const { scrollRef, collapsed } = useScrollCollapse(HERO_HEIGHT - HERO_OVERLAP)

  const goPlay = async () => {
    if (!id || !contentType) return
    const path = await resolvePlayPath(contentType, id)
    if (path) navigate(path)
  }
  const goBack = () => navigate(returnTo)

  const play = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-play', forceFocus: true, onEnterPress: goPlay })
  const back = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-detail-back', onEnterPress: goBack })

  if (loading) {
    return <div style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
      <div className="hds-skeleton" style={{ width: 300, height: 450, borderRadius: 12 }} />
    </div>
  }

  return (
    <div ref={scrollRef} style={{ flex: 1, minHeight: 0, overflowY: 'auto', position: 'relative' }} className="scrollbar-dark">
      {/* Backdrop — scrolls with the rest of the content; by the time the
          sticky header below locks at the top it's already out of view, so
          it needs no collapse handling of its own. */}
      <div style={{
        position: 'absolute', top: 0, left: 0, right: 0, height: HERO_HEIGHT, zIndex: 0,
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
      </div>

      {/* Spacer — pushes the sticky header down to its expanded starting
          position (overlapping the backdrop's lower edge). Past this point
          the header has nowhere left to go and locks at the top. */}
      <div style={{ height: HERO_HEIGHT - HERO_OVERLAP }} />

      {/* Sticky, collapse-aware header — locks at the top once scrolled past
          its expanded starting position and shrinks into a slim bar. The
          title/overview stay visible in both states, so retargeting them by
          hovering/focusing an episode further down the shelf list (see
          useMediaDetail's focusedEpisode) is still visible either way — the
          reason the hero used to be fully fixed no longer requires that,
          since the locked header serves the same purpose. */}
      <div style={{
        position: 'sticky', top: 0, zIndex: 2,
        padding: collapsed ? '18px 48px' : '40px 48px',
        background: collapsed ? 'var(--hds-bg)' : 'transparent',
        borderBottom: `1px solid ${collapsed ? 'var(--hds-line-s)' : 'transparent'}`,
        boxShadow: collapsed ? '0 6px 28px oklch(0 0 0 / 0.4)' : 'none',
        transition: 'padding .25s ease, background .25s ease, border-color .25s ease, box-shadow .25s ease',
      }}>
        <div style={{
          display: 'flex', gap: collapsed ? 24 : 40,
          alignItems: collapsed ? 'center' : 'flex-end',
          transition: 'gap .25s ease',
        }}>
          <div style={{
            width: collapsed ? 90 : 220, height: collapsed ? 135 : 330, borderRadius: collapsed ? 8 : 12,
            overflow: 'hidden', flexShrink: 0,
            background: 'var(--hds-bg-3)', boxShadow: '0 12px 40px oklch(0 0 0 / 0.55)',
            transition: 'width .25s ease, height .25s ease, border-radius .25s ease',
          }}>
            {posterUrl && <img src={posterUrl} alt={title} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />}
          </div>

          <div style={{ flex: 1, minWidth: 0, paddingBottom: collapsed ? 0 : 8 }}>
            <h1 style={{
              fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700,
              fontSize: collapsed ? 24 : 40,
              color: '#fff', margin: collapsed ? 0 : '0 0 12px', lineHeight: 1.1,
              transition: 'font-size .25s ease, margin .25s ease',
              whiteSpace: collapsed ? 'nowrap' : undefined,
              overflow: collapsed ? 'hidden' : undefined,
              textOverflow: collapsed ? 'ellipsis' : undefined,
            }}>{title}</h1>

            <div style={{
              maxHeight: collapsed ? 0 : 220, opacity: collapsed ? 0 : 1, overflow: 'hidden',
              transition: 'max-height .25s ease, opacity .18s ease',
            }}>
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
            </div>

            <div style={{
              display: 'flex', gap: collapsed ? 10 : 14, marginTop: collapsed ? 10 : 0,
              transition: 'gap .25s ease, margin-top .25s ease',
            }}>
              <button
                ref={play.ref} data-tv-focused={play.focused}
                onClick={goPlay}
                style={{
                  display: 'flex', alignItems: 'center', gap: 10, cursor: 'pointer',
                  padding: collapsed ? '9px 20px' : '14px 28px', borderRadius: 10, border: 'none',
                  background: 'var(--hds-gold)', color: 'oklch(0.15 0.02 90)',
                  fontFamily: "'Chakra Petch', sans-serif", fontSize: collapsed ? 13 : 16, fontWeight: 700,
                  transition: 'padding .25s ease, font-size .25s ease',
                }}
              >
                <svg width={collapsed ? 13 : 16} height={collapsed ? 13 : 16} viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
                Play
              </button>
              <button
                ref={back.ref} data-tv-focused={back.focused}
                onClick={goBack}
                style={{
                  padding: collapsed ? '9px 20px' : '14px 28px', borderRadius: 10, cursor: 'pointer',
                  border: '1px solid var(--hds-glass-border)', background: 'var(--hds-glass)', color: '#fff',
                  fontFamily: "'Chakra Petch', sans-serif", fontSize: collapsed ? 13 : 16, fontWeight: 600,
                  transition: 'padding .25s ease, font-size .25s ease',
                }}
              >Back</button>
            </div>
          </div>
        </div>
      </div>

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
  )
}
