import { useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../../nav/useFocusable'
import { useDebounce } from '../../hooks/useDebounce'
import { mediaUrl } from '../../api/client'
import type { Episode } from '../../api/types'

// Debounces both directions of the expand/collapse trigger — sweeping the
// mouse across several collapsed season tiles on the way to somewhere else
// no longer fires a burst of expand/collapse for each one it passes over.
const HOVER_DEBOUNCE_MS = 150

interface EpisodeShelfProps {
  seasonNumber: number
  seasonName?:  string
  episodes:     Episode[]
  /** Fired when an episode tile is hovered/focused — lets the hero retarget to it. */
  onEpisodeHover?:    (episode: Episode) => void
  /** Fired when focus/hover leaves this season entirely (not just moving between its own tiles) — restores the hero. */
  onEpisodeHoverEnd?: () => void
}

export function EpisodeShelf({ seasonNumber, seasonName, episodes, onEpisodeHover, onEpisodeHoverEnd }: EpisodeShelfProps) {
  const scrollRef = useRef<HTMLDivElement>(null)
  const [showArrows, setShowArrows] = useState(false)
  const [hovered, setHovered] = useState(false)
  const scroll = (d: 'left' | 'right') =>
    scrollRef.current?.scrollBy({ left: d === 'right' ? 300 : -300, behavior: 'smooth' })

  const title = seasonName || (seasonNumber === 0 ? 'Specials' : `Season ${seasonNumber}`)

  // Multiple aired-order "season 0" entries can share seasonNumber 0 (each a
  // different special) — anchor on the first episode's id too so their
  // focusKeys don't collide.
  const { ref: containerRef, focusKey: containerFocusKey, hasFocusedChild } = useFocusable<object, HTMLDivElement>({
    focusKey: `season-shelf-${seasonNumber}-${episodes[0]?.episode_id ?? ''}`,
    trackChildren: true,
    saveLastFocusedChild: true,
  })
  const rawExpanded = hovered || hasFocusedChild
  const expanded = useDebounce(rawExpanded, HOVER_DEBOUNCE_MS)

  return (
    <div
      ref={containerRef}
      style={{ position: 'relative', marginBottom: 20 }}
      onMouseEnter={() => { setHovered(true); setShowArrows(true) }}
      onMouseLeave={() => { setHovered(false); setShowArrows(false); onEpisodeHoverEnd?.() }}
    >
      <FocusContext.Provider value={containerFocusKey}>
        <SeasonHeaderTile
          focusKey={`season-header-${seasonNumber}-${episodes[0]?.episode_id ?? ''}`}
          title={title} count={episodes.length} expanded={expanded}
        />

        {/* Grid-rows 0fr/1fr trick animates height without ever measuring it —
            the row stays mounted (so focus/D-pad nav can always reach it,
            and there's something in the DOM to actually transition) and just
            collapses to zero visual size instead of snapping in/out. */}
        <div style={{
          display: 'grid',
          gridTemplateRows: expanded ? '1fr' : '0fr',
          transition: 'grid-template-rows .22s cubic-bezier(0.22,1,0.36,1)',
          marginTop: expanded ? 10 : 0,
        }}>
          <div style={{ overflow: 'hidden', minHeight: 0 }}>
            {showArrows && episodes.length > 3 && <EpArrow side="left" onClick={() => scroll('left')} />}
            <div ref={scrollRef} style={{
              display: 'flex', gap: 10, overflowX: 'auto', overflowY: 'hidden',
              paddingBottom: 4, scrollbarWidth: 'none',
            }}>
              {episodes.map(ep => (
                <EpisodeTile key={ep.episode_id} episode={ep} onHover={() => onEpisodeHover?.(ep)} />
              ))}
            </div>
            {showArrows && episodes.length > 3 && <EpArrow side="right" onClick={() => scroll('right')} />}
          </div>
        </div>
      </FocusContext.Provider>
    </div>
  )
}

function SeasonHeaderTile({ focusKey, title, count, expanded }: { focusKey: string; title: string; count: number; expanded: boolean }) {
  const [hovered, setHovered] = useState(false)
  const { ref, focused } = useFocusable<object, HTMLDivElement>({ focusKey })
  const active = hovered || focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer',
        padding: '10px 14px', borderRadius: 8,
        border: `1px solid ${active ? 'var(--hds-line-s)' : 'var(--hds-line)'}`,
        background: active ? 'var(--hds-bg-2)' : 'transparent',
        transition: 'border-color .12s, background .12s',
      }}
    >
      <span style={{
        fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
        color: 'var(--hds-txt)', letterSpacing: '0.02em',
      }}>{title}</span>
      <span style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
      }}>{count} episode{count === 1 ? '' : 's'}</span>
      <span style={{
        marginLeft: 'auto', color: 'var(--hds-txt-3)', fontSize: 11,
        transform: expanded ? 'rotate(90deg)' : 'none', transition: 'transform .15s',
      }}>›</span>
    </div>
  )
}

function EpisodeTile({ episode, onHover }: { episode: Episode; onHover?: () => void }) {
  const [hoveredState, setHovered] = useState(false)
  const [imgErr,  setImgErr]  = useState(false)
  const showImg = episode.thumb && !imgErr
  const code = `S${String(episode.season).padStart(2, '0')}E${String(episode.episode).padStart(2, '0')}`
  const navigate = useNavigate()
  const go = () => navigate(`/player/episode/${episode.episode_id}`)

  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `episode-tile-${episode.episode_id}`,
    onEnterPress: go,
    onFocus: () => onHover?.(),
  })
  const hovered = hoveredState || focused

  const tooltip = [episode.overview || episode.title, episode.file_path]
    .filter(Boolean).join('\n\n')

  return (
    <div
      ref={ref} data-tv-focused={focused}
      title={tooltip}
      onMouseEnter={() => { setHovered(true); onHover?.() }}
      onMouseLeave={() => setHovered(false)}
      style={{
        flexShrink: 0, width: 220, borderRadius: 8, overflow: 'hidden', cursor: 'pointer',
        border: `1px solid ${hovered ? 'var(--hds-line-s)' : 'var(--hds-line)'}`,
        background: 'var(--hds-bg-2)', transition: 'border-color .12s, transform .12s',
        transform: hovered ? 'translateY(-2px)' : 'none',
      }}
      onClick={go}
    >
      <div style={{
        aspectRatio: '16/9', width: '100%', position: 'relative', overflow: 'hidden',
        background: 'linear-gradient(135deg, oklch(0.18 0.03 287), oklch(0.13 0.02 285))',
      }}>
        {showImg ? (
          <img
            src={mediaUrl(`/api/episodes/${episode.episode_id}/thumb`)}
            alt={episode.title}
            onError={() => setImgErr(true)}
            style={{ width: '100%', height: '100%', objectFit: 'cover' }}
          />
        ) : (
          <span style={{
            position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)',
          }}>{code}</span>
        )}
        {hovered && (
          <span style={{
            position: 'absolute', inset: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
            background: 'oklch(0 0 0 / 0.35)',
          }}>
            <span style={{
              width: 36, height: 36, borderRadius: '50%', display: 'flex', alignItems: 'center', justifyContent: 'center',
              background: 'oklch(1 0 0 / 0.9)', color: 'oklch(0.15 0.02 285)',
            }}>
              <svg width="14" height="14" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
            </span>
          </span>
        )}
        <span style={{
          position: 'absolute', top: 6, left: 6,
          background: 'oklch(0 0 0 / 0.6)', borderRadius: 4, padding: '2px 6px',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.9 0.01 285)',
          letterSpacing: '0.04em',
        }}>{code}</span>
        {episode.duration_ms > 0 && (
          <span style={{
            position: 'absolute', bottom: 6, right: 6,
            background: 'oklch(0 0 0 / 0.6)', borderRadius: 4, padding: '2px 6px',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.9 0.01 285)',
          }}>{Math.round(episode.duration_ms / 60000)}m</span>
        )}
      </div>
      <div style={{ padding: '7px 9px 9px' }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{episode.title || code}</div>
        {episode.air_date && (
          <div style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 2,
          }}>{episode.air_date}</div>
        )}
      </div>
    </div>
  )
}

function EpArrow({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button onClick={onClick} style={{
      position: 'absolute', top: 66, transform: 'translateY(-50%)',
      left: side === 'left' ? -6 : undefined, right: side === 'right' ? -6 : undefined,
      zIndex: 2, width: 28, height: 28, borderRadius: '50%', cursor: 'pointer',
      background: 'var(--hds-glass)', backdropFilter: 'blur(8px)',
      border: '1px solid var(--hds-glass-border)', color: 'var(--hds-txt)', fontSize: 16,
      display: 'flex', alignItems: 'center', justifyContent: 'center',
    }}>
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}
