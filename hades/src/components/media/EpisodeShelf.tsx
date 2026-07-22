import { useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../../nav/useFocusable'
import { useDebounce } from '../../hooks/useDebounce'
import { mediaUrl } from '../../api/client'
import type { Episode } from '../../api/types'
import { shuffleArray, storeQueue, type QueueItem } from '../../player/playQueue'
import styles from './EpisodeShelf.module.css'

function toQueueItems(episodes: Episode[]): QueueItem[] {
  return episodes.map(e => ({
    kind: 'episode', id: e.episode_id, title: e.title,
    season: e.season, episode: e.episode, duration_ms: e.duration_ms, thumb: e.thumb,
  }))
}

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
  const navigate = useNavigate()
  const [showArrows, setShowArrows] = useState(false)
  const [hovered, setHovered] = useState(false)
  const scroll = (d: 'left' | 'right') =>
    scrollRef.current?.scrollBy({ left: d === 'right' ? 300 : -300, behavior: 'smooth' })

  // Fresh shuffle every time — no resume semantics (unlike playlist
  // playback, see resolvePlaylistPlayPath), since "shuffle play this season"
  // means "start a new randomized run now", not "continue a previous one".
  const handleShuffle = () => {
    if (episodes.length === 0) return
    const shuffled = shuffleArray(toQueueItems(episodes))
    const token = storeQueue(shuffled)
    navigate(`/player/episode/${shuffled[0].id}?queue=${token}`)
  }

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
      className={styles.container}
      onMouseEnter={() => { setHovered(true); setShowArrows(true) }}
      onMouseLeave={() => { setHovered(false); setShowArrows(false); onEpisodeHoverEnd?.() }}
    >
      <FocusContext.Provider value={containerFocusKey}>
        <SeasonHeaderTile
          focusKey={`season-header-${seasonNumber}-${episodes[0]?.episode_id ?? ''}`}
          title={title} count={episodes.length} expanded={expanded} onShuffle={handleShuffle}
        />

        {/* Grid-rows 0fr/1fr trick animates height without ever measuring it —
            the row stays mounted (so focus/D-pad nav can always reach it,
            and there's something in the DOM to actually transition) and just
            collapses to zero visual size instead of snapping in/out. */}
        <div className={`${styles.animateRow} ${expanded ? styles.animateRowExpanded : ''}`}>
          <div className={styles.animateRowInner}>
            {showArrows && episodes.length > 3 && <EpArrow side="left" onClick={() => scroll('left')} />}
            <div ref={scrollRef} className={styles.scrollRow}>
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

function SeasonHeaderTile({ focusKey, title, count, expanded, onShuffle }: {
  focusKey: string; title: string; count: number; expanded: boolean; onShuffle: () => void
}) {
  const [hovered, setHovered] = useState(false)
  const { ref, focused } = useFocusable<object, HTMLDivElement>({ focusKey })
  const active = hovered || focused

  const [shuffleHovered, setShuffleHovered] = useState(false)
  const shuffle = useFocusable<object, HTMLButtonElement>({
    focusKey: `${focusKey}-shuffle`,
    onEnterPress: onShuffle,
  })
  const shuffleActive = shuffleHovered || shuffle.focused

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      className={`${styles.seasonHeader} ${active ? styles.seasonHeaderActive : ''}`}
    >
      <span className={styles.seasonTitle}>{title}</span>
      <span className={styles.seasonCount}>{count} episode{count === 1 ? '' : 's'}</span>
      <button
        ref={shuffle.ref} data-tv-focused={shuffle.focused}
        onMouseEnter={() => setShuffleHovered(true)}
        onMouseLeave={() => setShuffleHovered(false)}
        onClick={e => { e.stopPropagation(); onShuffle() }}
        title="Shuffle play this season"
        className={`${styles.shuffleBtn} ${shuffleActive ? styles.shuffleBtnActive : ''}`}
      >
        🔀 Shuffle
      </button>
      <span className={`${styles.chevron} ${expanded ? styles.chevronExpanded : ''}`}>›</span>
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
      className={`${styles.episodeTile} ${hovered ? styles.episodeTileHovered : ''}`}
      onClick={go}
    >
      <div className={styles.episodeThumbWrap}>
        {showImg ? (
          <img
            src={mediaUrl(`/api/episodes/${episode.episode_id}/thumb`)}
            alt={episode.title}
            onError={() => setImgErr(true)}
            className={styles.episodeImg}
          />
        ) : (
          <span className={styles.placeholderCode}>{code}</span>
        )}
        {hovered && (
          <span className={styles.playOverlay}>
            <span className={styles.playIconCircle}>
              <svg width="14" height="14" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
            </span>
          </span>
        )}
        <span className={styles.codeBadge}>{code}</span>
        {episode.watched && (
          <span className={styles.watchedBadge}>✓{episode.view_count && episode.view_count > 1 ? ` ${episode.view_count}` : ''}</span>
        )}
        {episode.duration_ms > 0 && (
          <span className={styles.durationBadge}>{Math.round(episode.duration_ms / 60000)}m</span>
        )}
      </div>
      <div className={styles.episodeInfo}>
        <div className={styles.episodeTitle}>{episode.title || code}</div>
        {episode.air_date && (
          <div className={styles.episodeAirDate}>{episode.air_date}</div>
        )}
      </div>
    </div>
  )
}

function EpArrow({ side, onClick }: { side: 'left' | 'right'; onClick: () => void }) {
  return (
    <button
      onClick={onClick}
      className={`${styles.arrowButton} ${side === 'left' ? styles.arrowLeft : styles.arrowRight}`}
    >
      {side === 'left' ? '‹' : '›'}
    </button>
  )
}
