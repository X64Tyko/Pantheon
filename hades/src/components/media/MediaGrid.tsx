import { observer } from 'mobx-react-lite'
import { MediaCard } from './MediaCard'
import { mediaUrl } from '../../api/client'
import type { Show, Movie, EpisodeSearchResult, LibraryDensity } from '../../api/types'
import styles from './MediaGrid.module.css'

interface MediaGridProps {
  shows:       Show[]
  movies:      Movie[]
  // Library's "Include Episodes" toggle (default off/hidden — see
  // LibraryStore.ts). Rendered as its own trailing section, same "shows
  // section then movies section" sequential model this grid already used
  // for those two — not truly interleaved by a single cross-type sort. A
  // playlist's Playlist Order sort orders *within* each section (shows by
  // earliest episode position, movies/episodes by their own position), not
  // across all three as one sequence.
  episodes?:      EpisodeSearchResult[]
  density:     LibraryDensity
  selectedId:  string | null
  onItemClick: (id: string, type: 'show' | 'movie') => void
  // Episodes have no detail page of their own — clicking one plays it
  // directly, unlike selecting a show/movie which opens MediaDetail.
  onEpisodeClick?: (episodeId: string) => void
}

const DENSITY_CLASS: Record<LibraryDensity, string> = {
  minimal: styles.gridMinimal,
  standard: styles.gridStandard,
  rich: styles.gridRich,
}

export const MediaGrid = observer(function MediaGrid({ shows, movies, episodes = [], density, selectedId, onItemClick, onEpisodeClick }: MediaGridProps) {
  return (
    <div className={`${styles.grid} ${DENSITY_CLASS[density]}`}>
      {shows.map(s => (
        <MediaCard
          key={s.show_id}
          id={s.show_id}
          title={s.title}
          year={s.year}
          content_type="show"
          thumb_url={s.thumb ? mediaUrl(`/api/shows/${s.show_id}/thumb`) : undefined}
          rating={s.audience_rating}
          match_status={s.match_status}
          match_score={s.match_score ?? undefined}
          density={density}
          selected={selectedId === s.show_id}
          onClick={() => onItemClick(s.show_id, 'show')}
        />
      ))}
      {movies.map(m => (
        <MediaCard
          key={m.movie_id}
          id={m.movie_id}
          title={m.title}
          year={m.year}
          content_type="movie"
          thumb_url={m.thumb ? mediaUrl(`/api/movies/${m.movie_id}/thumb`) : undefined}
          rating={m.audience_rating}
          match_status={m.match_status}
          match_score={m.match_score ?? undefined}
          watched={m.watched}
          view_count={m.view_count}
          density={density}
          selected={selectedId === m.movie_id}
          onClick={() => onItemClick(m.movie_id, 'movie')}
        />
      ))}
      {episodes.map(ep => (
        <MediaCard
          key={ep.episode_id}
          id={ep.episode_id}
          title={`${ep.show_title} — S${String(ep.season).padStart(2, '0')}E${String(ep.episode).padStart(2, '0')} — ${ep.title}`}
          content_type="episode"
          thumb_url={mediaUrl(`/api/episodes/${ep.episode_id}/thumb`)}
          density={density}
          selected={false}
          onClick={() => onEpisodeClick?.(ep.episode_id)}
        />
      ))}
    </div>
  )
})
