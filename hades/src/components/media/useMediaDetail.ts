import { useEffect, useState } from 'react'
import { api, mediaUrl } from '../../api/client'
import type { Episode, MediaLanguages, ScraperSearchResult, ShowDetail, MovieDetail, VideoInfo } from '../../api/types'

function showThumbUrl(id: string) { return mediaUrl(`/api/shows/${id}/thumb`) }
function showArtUrl(id: string)   { return mediaUrl(`/api/shows/${id}/art`) }
function movieThumbUrl(id: string) { return mediaUrl(`/api/movies/${id}/thumb`) }
function movieArtUrl(id: string)   { return mediaUrl(`/api/movies/${id}/art`) }

export interface UseMediaDetailArgs {
  id?:             string
  content_type?:   'show' | 'movie'
  discoverResult?: ScraperSearchResult
}

// Fetch + derive everything MediaDetailHero needs that isn't JSX — shared by
// the desktop hero and TvLibraryDetail so both consume the same data shape.
export function useMediaDetail({ id, content_type, discoverResult }: UseMediaDetailArgs) {
  const [show,      setShow]      = useState<ShowDetail | null>(null)
  const [movie,     setMovie]     = useState<MovieDetail | null>(null)
  const [loading,   setLoading]   = useState(!discoverResult)
  const [episodes,  setEpisodes]  = useState<Episode[]>([])
  const [languages, setLanguages] = useState<MediaLanguages | null>(null)
  const [videoInfo, setVideoInfo] = useState<VideoInfo | null>(null)

  useEffect(() => {
    if (discoverResult) return
    if (!id || !content_type) return
    setLoading(true)
    setShow(null); setMovie(null); setEpisodes([]); setLanguages(null); setVideoInfo(null)

    if (content_type === 'show') {
      api.getShow(id).then(setShow).finally(() => setLoading(false))
      api.getEpisodes(id).then(setEpisodes).catch(() => {})
      api.getShowLanguages(id).then(setLanguages).catch(() => {})
      api.getShowVideoInfo(id).then(setVideoInfo).catch(() => {})
    } else {
      api.getMovie(id).then(setMovie).finally(() => setLoading(false))
      api.getMovieLanguages(id).then(setLanguages).catch(() => {})
      api.getMovieVideoInfo(id).then(setVideoInfo).catch(() => {})
    }
  }, [id, content_type, discoverResult])

  const detail = discoverResult ? null : (show ?? movie)
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

  const posterUrl = discoverResult?.poster_url
    ?? (id && detail?.thumb ? (contentType === 'show' ? showThumbUrl(id) : movieThumbUrl(id)) : undefined)
  const backdropUrl = discoverResult?.poster_url
    ?? (id && detail?.art ? (contentType === 'show' ? showArtUrl(id) : movieArtUrl(id)) : undefined)

  const title    = discoverResult?.title    ?? detail?.title    ?? ''
  const year     = discoverResult?.year     ?? detail?.year
  const overview = discoverResult?.overview ?? detail?.overview ?? ''
  const genres   = detail?.genres ?? []
  const rating   = detail?.audience_rating

  const seasonsWithEpisodes = show
    ? show.seasons
        .map(s => ({ ...s, episodes: episodes.filter(e => e.season === s.number).sort((a, b) => a.episode - b.episode) }))
        .filter(s => s.episodes.length > 0)
    : []

  return {
    show, movie, loading, episodes, languages, videoInfo,
    detail, contentType, posterUrl, backdropUrl,
    title, year, overview, genres, rating, seasonsWithEpisodes,
  }
}
