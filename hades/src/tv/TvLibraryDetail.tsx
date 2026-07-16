import { useEffect, useState } from 'react'
import { useNavigate, useParams } from 'react-router-dom'
import { useMediaDetail } from '../components/media/useMediaDetail'
import { useScrollCollapse } from '../components/media/useScrollCollapse'
import { useElementHeight } from '../components/media/useElementHeight'
import { EpisodeShelf } from '../components/media/EpisodeShelf'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import { resolvePlayPath } from '../player/resolvePlayTarget'
import { peekDetailReturnTo, clearPendingDetailReturn } from './tvDetailNav'

// The backdrop is a genuinely fixed layer — never scrolls. It's
// HERO_HEIGHT_CSS tall (shown in full on first paint) until the header
// below has actually locked at the top (`collapsed`, from
// useScrollCollapse's sentinel — geometry-accurate, not a guessed scroll
// distance), at which point it shrinks to match the header's own *measured*
// rendered height (`useElementHeight`) and grows back once scrolled back
// up. The poster/title/overview/Play/Back block itself never shrinks or
// changes style — it's a `position: sticky` element inside the scroll
// container, starting at its normal document position (overlapping the
// backdrop's lower edge by HERO_OVERLAP) and translating up with the
// scroll like anything else until it locks at the top, still overlaid on
// the backdrop throughout (its zIndex is higher). Sized larger than
// desktop's own HERO_HEIGHT_CSS (see MediaDetailHero.tsx) — this is a TV,
// not a phone; matches TV's own historical hero proportions rather than
// desktop's HomePage (a different screen).
const HERO_HEIGHT_CSS = 'max(52vh, 460px)'
const HERO_OVERLAP    = 60

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

  const { scrollRef, sentinelRef, collapsed } = useScrollCollapse()
  const { ref: headerRef, height: headerHeight } = useElementHeight<HTMLDivElement>()

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
    <div style={{ flex: 1, minHeight: 0, position: 'relative', overflow: 'hidden' }}>
      {/* Fixed backdrop — never scrolls; shrinks to the locked header's
          measured height once collapsed, back to full size once not. */}
      <div style={{
        position: 'absolute', top: 0, left: 0, right: 0, zIndex: 1,
        height: collapsed && headerHeight > 0 ? headerHeight : HERO_HEIGHT_CSS,
        background: backdropUrl
          ? `url(${backdropUrl}) center/cover no-repeat`
          : 'linear-gradient(135deg, oklch(0.12 0.04 292) 0%, oklch(0.18 0.06 270) 50%, oklch(0.14 0.03 280) 100%)',
        transition: 'height .3s ease',
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

      <div ref={scrollRef} style={{ position: 'relative', height: '100%', overflowY: 'auto' }} className="scrollbar-dark">
        {/* Spacer — the sticky header's natural (unstuck) starting position,
            overlapping the fixed backdrop's lower edge by HERO_OVERLAP. */}
        <div style={{ height: `calc(${HERO_HEIGHT_CSS} - ${HERO_OVERLAP}px)` }} />

        {/* Sentinel — same document position as the header's natural top
            edge, right before it. Once this scrolls out of view the header
            has nowhere left to go but stick (see useScrollCollapse). */}
        <div ref={sentinelRef} />

        {/* Sticky header — scrolls up with the page like normal content
            until it reaches the top, then locks there. Never changes size —
            just overlaid on the still-visible backdrop behind it throughout
            (zIndex 2 > backdrop's zIndex 1). The title/overview stay
            visible always, so retargeting them by hovering/focusing an
            episode further down the shelf list (see useMediaDetail's
            focusedEpisode) is still visible either way — the reason the
            hero used to be fully fixed no longer requires that, since the
            locked header serves the same purpose. */}
        <div ref={headerRef} style={{ position: 'sticky', top: 0, zIndex: 2, padding: '40px 48px' }}>
          <div style={{ display: 'flex', gap: 40, alignItems: 'flex-end' }}>
            <div style={{
              width: 220, height: 330, borderRadius: 12, overflow: 'hidden', flexShrink: 0,
              background: 'var(--hds-bg-3)', boxShadow: '0 12px 40px oklch(0 0 0 / 0.55)',
            }}>
              {posterUrl && <img src={posterUrl} alt={title} style={{ width: '100%', height: '100%', objectFit: 'cover' }} />}
            </div>

            <div style={{ flex: 1, minWidth: 0, paddingBottom: 8 }}>
              <h1 style={{
                fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 40,
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

        {/* Plain flow — no position/z-index, so it naturally paints *below*
            the fixed backdrop (a positioned element) wherever it scrolls
            into that region, i.e. behind it. */}
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
