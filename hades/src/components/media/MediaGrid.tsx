import { observer } from 'mobx-react-lite'
import { MediaCard } from './MediaCard'
import { mediaUrl } from '../../api/client'
import type {Show, Movie, MixedMediaItem, EpisodeSearchResult, LibraryDensity} from '../../api/types'
import styles from './MediaGrid.module.css'

interface MediaGridProps {
    // Either shows+movies as two separate sections (single content-type
    // browse, or a content-type-scoped filter), or mixedItems as one already
    // globally-ordered interleaved list (contentType 'all' — see
    // LibraryStore.fetch()/MixedSort.h) — never both at once.
    shows?: Show[]
    movies?: Movie[]
    mixedItems?: MixedMediaItem[]
  // Library's "Include Episodes" toggle (default off/hidden — see
    // LibraryStore.ts). Always its own trailing section — a show can have
    // orders of magnitude more episodes than shows/movies combined, so this
    // deliberately isn't folded into the mixed/interleaved order either.
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

export const MediaGrid = observer(function MediaGrid({
                                                         shows = [],
                                                         movies = [],
                                                         mixedItems,
                                                         episodes = [],
                                                         density,
                                                         selectedId,
                                                         onItemClick,
                                                         onEpisodeClick
                                                     }: MediaGridProps) {
  return (
    <div className={`${styles.grid} ${DENSITY_CLASS[density]}`}>
        {mixedItems ? mixedItems.map(item => (
            <MediaCard
                key={item.id}
                id={item.id}
                title={item.title}
                year={item.year}
                content_type={item.content_type}
                thumb_url={item.thumb ? mediaUrl(`/api/${item.content_type}s/${item.id}/thumb`) : undefined}
                rating={item.audience_rating}
                watched={item.watched}
                watched_label={item.content_type === 'show' && item.watched && item.episode_count && item.view_count < item.episode_count
                    ? `${item.view_count}/${item.episode_count}` : undefined}
                view_count={item.view_count}
                density={density}
                selected={selectedId === item.id}
                // Library's mixed browse never sets expandEpisodes, so mixedItems
                // is always show/movie here — this narrows for TS since
                // MixedMediaItem's content_type is shared with the (episode-
                // capable) Home shelf use of the same type.
                onClick={() => {
                    if (item.content_type !== 'episode') onItemClick(item.id, item.content_type)
                }}
            />
        )) : <>
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
          watched={s.watched_episode_count > 0}
          watched_label={s.watched_episode_count > 0 && s.watched_episode_count < s.episode_count
            ? `${s.watched_episode_count}/${s.episode_count}` : undefined}
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
        </>}
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
