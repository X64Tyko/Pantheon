import { useState, useEffect } from 'react'
import { api } from '../api/client'
import { useDebounce } from '../hooks/useDebounce'
import type { Show, Movie, EpisodeSearchResult, Playlist } from '../api/types'

export interface BrowserSearchResult {
  shows:             Show[]
  showsTotal:        number
  showsLoadingMore:  boolean
  movies:            Movie[]
  moviesTotal:       number
  moviesLoadingMore: boolean
  eps:               EpisodeSearchResult[]
  epsHasMore:        boolean
  epsLoadingMore:    boolean
  lists:             Playlist[]
  loading:           boolean
  loadMoreShows:     () => void
  loadMoreMovies:    () => void
  loadMoreEps:       () => void
}

export function useBrowserSearch(tab: string, query: string, sFilter: string, skip = false): BrowserSearchResult {
  const dq = useDebounce(query,   300)
  const ds = useDebounce(sFilter, 300)

  const [shows,             setShows]             = useState<Show[]>([])
  const [showsTotal,        setShowsTotal]        = useState(0)
  const [showsLoadingMore,  setShowsLoadingMore]  = useState(false)
  const [movies,            setMovies]            = useState<Movie[]>([])
  const [moviesTotal,       setMoviesTotal]       = useState(0)
  const [moviesLoadingMore, setMoviesLoadingMore] = useState(false)
  const [eps,               setEps]               = useState<EpisodeSearchResult[]>([])
  const [epsHasMore,        setEpsHasMore]        = useState(false)
  const [epsLoadingMore,    setEpsLoadingMore]    = useState(false)
  const [lists,             setLists]             = useState<Playlist[]>([])
  const [loading,           setLoading]           = useState(false)

  useEffect(() => {
    if (skip) return
    const ctrl = new AbortController()
    setLoading(true)
    const season = ds.trim() !== '' ? parseInt(ds, 10) : undefined
    const parsedSeason = Number.isFinite(season) ? season : undefined
    let p: Promise<void>
    if (tab === 'shows') {
      setShowsTotal(0)
      p = api.getShows({ limit: 80, q: dq || undefined })
        .then(r => { if (!ctrl.signal.aborted) { setShows(r.items); setShowsTotal(r.total); setLoading(false) } })
    } else if (tab === 'movies') {
      setMoviesTotal(0)
      p = api.getMovies({ limit: 80, q: dq || undefined })
        .then(r => { if (!ctrl.signal.aborted) { setMovies(r.items); setMoviesTotal(r.total); setLoading(false) } })
    } else if (tab === 'episodes') {
      setEpsHasMore(false)
      p = api.searchEpisodes({ q: dq || undefined, season: parsedSeason, limit: 40 })
        .then(r => { if (!ctrl.signal.aborted) { setEps(r.items); setEpsHasMore(r.items.length >= 40); setLoading(false) } })
    } else {
      p = api.getPlaylists()
        .then(r => { if (!ctrl.signal.aborted) { setLists(r); setLoading(false) } })
    }
    p.catch(() => { if (!ctrl.signal.aborted) setLoading(false) })
    return () => ctrl.abort()
  }, [tab, dq, ds, skip])

  const loadMoreShows = () => {
    if (showsLoadingMore || shows.length >= showsTotal) return
    setShowsLoadingMore(true)
    api.getShows({ limit: 80, offset: shows.length, q: dq || undefined })
      .then(r => { setShows(s => [...s, ...r.items]); setShowsTotal(r.total) })
      .catch(() => {})
      .finally(() => setShowsLoadingMore(false))
  }

  const loadMoreMovies = () => {
    if (moviesLoadingMore || movies.length >= moviesTotal) return
    setMoviesLoadingMore(true)
    api.getMovies({ limit: 80, offset: movies.length, q: dq || undefined })
      .then(r => { setMovies(m => [...m, ...r.items]); setMoviesTotal(r.total) })
      .catch(() => {})
      .finally(() => setMoviesLoadingMore(false))
  }

  const loadMoreEps = () => {
    if (epsLoadingMore || !epsHasMore) return
    setEpsLoadingMore(true)
    const season = ds.trim() !== '' ? parseInt(ds, 10) : undefined
    api.searchEpisodes({ q: dq || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40, offset: eps.length })
      .then(r => { setEps(e => [...e, ...r.items]); setEpsHasMore(r.items.length >= 40) })
      .catch(() => {})
      .finally(() => setEpsLoadingMore(false))
  }

  return {
    shows, showsTotal, showsLoadingMore,
    movies, moviesTotal, moviesLoadingMore,
    eps, epsHasMore, epsLoadingMore,
    lists, loading,
    loadMoreShows, loadMoreMovies, loadMoreEps,
  }
}
