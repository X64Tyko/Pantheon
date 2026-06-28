import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { Show, Movie, ShowDetail, MediaHeroItem } from '../api/types'
import { HeroBanner } from '../components/media/HeroBanner'
import { MediaShelf } from '../components/media/MediaShelf'

function showToShelf(s: Show): Parameters<typeof MediaShelf>[0]['items'][number] {
  return { id: s.show_id, title: s.title, year: s.year, content_type: 'show' }
}

function movieToShelf(m: Movie): Parameters<typeof MediaShelf>[0]['items'][number] {
  return { id: m.movie_id, title: m.title, year: m.year, content_type: 'movie' }
}

function detailToHero(d: ShowDetail): MediaHeroItem {
  return {
    id: d.show_id, title: d.title, year: d.year, overview: d.overview,
    backdrop_url: d.art ? `${d.source_base_url}${d.art}` : undefined,
    content_type: 'show', genres: d.genres, rating: d.audience_rating,
  }
}

export default function HomePage() {
  const [shows,   setShows]   = useState<Show[]>([])
  const [movies,  setMovies]  = useState<Movie[]>([])
  const [hero,    setHero]    = useState<MediaHeroItem | null>(null)
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    Promise.all([
      api.getShows({ limit: 20 }),
      api.getMovies({ limit: 20 }),
    ]).then(([sr, mr]) => {
      setShows(sr.items)
      setMovies(mr.items)
      if (sr.items[0]) api.getShow(sr.items[0].show_id).then(d => setHero(detailToHero(d)))
    }).finally(() => setLoading(false))
  }, [])

  return (
    <div style={{ overflowY: 'auto', height: '100%', background: 'var(--hds-bg)' }}>
      {loading ? (
        <div className="hds-skeleton" style={{ height: '60vh', flexShrink: 0 }} />
      ) : hero ? (
        <HeroBanner item={hero} onViewDetail={() => {}} onAddToChannel={() => {}} />
      ) : (
        <div style={{
          height: '60vh', flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'center',
          background: 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
        }}>
          <span style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 14, color: 'var(--hds-txt-3)',
          }}>No content yet — add a source to get started</span>
        </div>
      )}

      <div style={{ padding: '32px 0' }}>
        {loading ? (
          [1, 2].map(i => (
            <div key={i} style={{ marginBottom: 32 }}>
              <div className="hds-skeleton" style={{ height: 14, width: 160, borderRadius: 4, margin: '0 24px 16px' }} />
              <div style={{ display: 'flex', gap: 12, padding: '4px 24px' }}>
                {[1, 2, 3, 4, 5, 6].map(j => (
                  <div key={j} className="hds-skeleton" style={{ width: 160, aspectRatio: '2/3', borderRadius: 8, flexShrink: 0 }} />
                ))}
              </div>
            </div>
          ))
        ) : (
          <>
            {shows.length > 0 && (
              <MediaShelf title="Shows" items={shows.map(showToShelf)} density="standard" onItemClick={() => {}} />
            )}
            {movies.length > 0 && (
              <MediaShelf title="Movies" items={movies.map(movieToShelf)} density="standard" onItemClick={() => {}} />
            )}
          </>
        )}
      </div>
    </div>
  )
}
