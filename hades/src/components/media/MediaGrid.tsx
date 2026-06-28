import { observer } from 'mobx-react-lite'
import { MediaCard } from './MediaCard'
import { mediaUrl } from '../../api/client'
import type { Show, Movie, LibraryDensity } from '../../api/types'

interface MediaGridProps {
  shows:       Show[]
  movies:      Movie[]
  density:     LibraryDensity
  selectedId:  string | null
  onItemClick: (id: string, type: 'show' | 'movie') => void
}

const MIN_WIDTH: Record<LibraryDensity, number> = { minimal: 130, standard: 170, rich: 200 }
const GAP: Record<LibraryDensity, number>       = { minimal: 8,   standard: 14,  rich: 18 }

export const MediaGrid = observer(function MediaGrid({ shows, movies, density, selectedId, onItemClick }: MediaGridProps) {
  return (
    <div style={{
      display: 'grid',
      gridTemplateColumns: `repeat(auto-fill, minmax(${MIN_WIDTH[density]}px, 1fr))`,
      gap: GAP[density],
      padding: '16px 24px',
    }}>
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
          density={density}
          selected={selectedId === m.movie_id}
          onClick={() => onItemClick(m.movie_id, 'movie')}
        />
      ))}
    </div>
  )
})
