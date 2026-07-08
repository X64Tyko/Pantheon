import type { Show, Movie } from '../api/types'
import { mediaUrl } from '../api/client'
import { TvMediaCard } from './TvMediaCard'

function isShow(item: Show | Movie): item is Show { return 'show_id' in item }
function thumbUrl(item: Show | Movie) {
  if (!item.thumb) return undefined
  return mediaUrl(isShow(item) ? `/api/shows/${item.show_id}/thumb` : `/api/movies/${item.movie_id}/thumb`)
}

export function TvMediaGrid({ shows, movies, onItemClick }: {
  shows:       Show[]
  movies:      Movie[]
  onItemClick: (id: string, type: 'show' | 'movie') => void
}) {
  const items: (Show | Movie)[] = [...shows, ...movies]

  return (
    <div style={{
      display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))',
      gap: 24, padding: '8px 48px 64px',
    }}>
      {items.map(item => {
        const id = isShow(item) ? item.show_id : item.movie_id
        const type = isShow(item) ? 'show' as const : 'movie' as const
        return (
          <TvMediaCard
            key={`${type}-${id}`}
            id={id} title={item.title} year={item.year}
            thumb_url={thumbUrl(item)} rating={item.audience_rating}
            content_type={type}
            onClick={() => onItemClick(id, type)}
          />
        )
      })}
    </div>
  )
}
