import { useCallback, useEffect, useState } from 'react'
import { api, mediaUrl } from '../../api/client'
import type { Episode, MediaLanguages, ScraperSearchResult, ShowDetail, MovieDetail, VideoInfo } from '../../api/types'

function showThumbUrl(id: string) { return mediaUrl(`/api/shows/${id}/thumb`) }
function showArtUrl(id: string)   { return mediaUrl(`/api/shows/${id}/art`) }
function movieThumbUrl(id: string) { return mediaUrl(`/api/movies/${id}/thumb`) }
function movieArtUrl(id: string)   { return mediaUrl(`/api/movies/${id}/art`) }
function episodeThumbUrl(id: string) { return mediaUrl(`/api/episodes/${id}/thumb`) }

// Admin-facing "where does this actually live on disk" display — movies have
// a single file (movie.file_path); shows are a collection of episode files,
// so there's no single path to show at the show level (hence optional).
export function splitFilePath(path?: string): { folderName?: string; fileName?: string } {
  if (!path) return {}
  const parts = path.split(/[/\\]/).filter(Boolean)
  const fileName = parts[parts.length - 1]
  const folderName = parts.length > 1 ? parts[parts.length - 2] : undefined
  return { folderName, fileName }
}

// Last segment of a *folder* path (as opposed to splitFilePath, which expects
// a file path and strips one segment off first) — e.g.
// "/media/Shows/Attack on Titan (2013)" -> "Attack on Titan (2013)". Used for
// the on-disk folder display in FixMatchPanel/ReviewPage/MediaDetailHero: a
// mismatched folder name is usually the fastest way to spot a bad scrape.
export function folderBaseName(path?: string): string | undefined {
  if (!path) return undefined
  const parts = path.split(/[/\\]/).filter(Boolean)
  return parts[parts.length - 1]
}

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
  // Bumped by refetch() — e.g. after LibraryDetailActions' Fix Match flow
  // changes match_status/score server-side, with no other prop actually
  // changing to naturally re-trigger the effect below.
  const [refreshKey, setRefreshKey] = useState(0)
  const refetch = useCallback(() => setRefreshKey(k => k + 1), [])
  // Set while an episode tile is hovered/focused inside one of this show's
  // EpisodeShelf season rows — overrides title/overview/backdropUrl below so
  // the hero retargets to the episode being previewed. null restores the
  // show/movie's own hero content.
  const [focusedEpisode, setFocusedEpisode] = useState<Episode | null>(null)

  useEffect(() => {
    if (discoverResult) return
    if (!id || !content_type) return
    setLoading(true)
    setShow(null); setMovie(null); setEpisodes([]); setLanguages(null); setVideoInfo(null)
    // A hover left over from the *previous* item would otherwise outlive it
    // whenever this hook's caller reuses the same mounted instance across an
    // id change (e.g. picking a different item from a "view in library"
    // flow without the detail view itself closing) — stale episode overriding
    // the new item's own title/overview/backdrop.
    setFocusedEpisode(null)

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
  }, [id, content_type, discoverResult, refreshKey])

  const detail = discoverResult ? null : (show ?? movie)
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

  // refreshKey busts both Kairos's own image cache (via refreshImages()) and
  // the browser's HTTP cache (Cache-Control on /thumb and /art is 1 day) —
  // without the query param, refetch() alone wouldn't be enough to show a
  // just-refreshed image immediately.
  const bust = (url: string) => refreshKey > 0 ? `${url}${url.includes('?') ? '&' : '?'}v=${refreshKey}` : url

  const posterUrl = discoverResult?.poster_url
    ? mediaUrl(discoverResult.poster_url)
    : (id && detail?.thumb ? bust(contentType === 'show' ? showThumbUrl(id) : movieThumbUrl(id)) : undefined)
  const showBackdropUrl = discoverResult?.poster_url
    ? mediaUrl(discoverResult.poster_url)
    : (id && detail?.art ? bust(contentType === 'show' ? showArtUrl(id) : movieArtUrl(id)) : undefined)
  // Episode thumb is already 16:9 (same aspect ratio EpisodeShelf's own tiles
  // use), so it doubles as a hero backdrop with no separate episode-art endpoint.
  const backdropUrl = focusedEpisode ? episodeThumbUrl(focusedEpisode.episode_id) : showBackdropUrl

  const showTitle    = discoverResult?.title    ?? detail?.title    ?? ''
  const title        = focusedEpisode ? focusedEpisode.title : showTitle
  const year     = discoverResult?.year     ?? detail?.year
  const showOverview = discoverResult?.overview ?? detail?.overview ?? ''
  const overview      = focusedEpisode ? focusedEpisode.overview : showOverview
  const genres   = detail?.genres ?? []
  const tags     = detail?.tags ?? []
  const rating   = detail?.audience_rating

  // 'season' (default): unchanged — one shelf per show.seasons entry, season 0
  // (specials) bucketed together like any other season. 'aired': season-0
  // episodes (a show's OVAs/movies — including ones linked in via the
  // specials-linking feature) are NOT bucketed; each becomes its own
  // single-episode shelf, positioned by air_date between whichever two
  // numbered seasons it chronologically falls between — e.g. a Gundam movie
  // shows up between Season 1 and Season 2 instead of in a generic Specials
  // pile at the end.
  const seasonsWithEpisodes = (() => {
    if (!show) return []
    const grouped = show.seasons
      .map(s => ({ ...s, episodes: episodes.filter(e => e.season === s.number).sort((a, b) => a.episode - b.episode) }))
      .filter(s => s.episodes.length > 0)

    if (show.episode_display_order !== 'aired') return grouped

    const numbered = grouped.filter(s => s.number !== 0).sort((a, b) => a.number - b.number)
    const specials = grouped.find(s => s.number === 0)?.episodes ?? []
    if (specials.length === 0) return grouped

    const seasonStart = (s: (typeof numbered)[number]) => s.episodes[0]?.air_date || ''
    const dated   = [...specials].filter(e => e.air_date).sort((a, b) => a.air_date.localeCompare(b.air_date))
    const undated = specials.filter(e => !e.air_date)

    const result: { number: number; name: string; episodes: typeof episodes }[] = []
    let si = 0
    for (const special of dated) {
      while (si < numbered.length && seasonStart(numbered[si]) && seasonStart(numbered[si]) <= special.air_date) {
        result.push(numbered[si]); si++
      }
      result.push({ number: 0, name: special.title, episodes: [special] })
    }
    while (si < numbered.length) { result.push(numbered[si]); si++ }
    for (const special of undated) result.push({ number: 0, name: special.title, episodes: [special] })
    return result
  })()

  // Movies only — shows are a collection of per-episode files, no single
  // path to show at the show level (see splitFilePath's own comment).
  const { folderName, fileName } = splitFilePath(movie?.file_path)

  const matchStatus = detail?.match_status
  const matchScore  = detail?.match_score

  return {
    show, movie, loading, episodes, languages, videoInfo,
    detail, contentType, posterUrl, backdropUrl,
    title, year, overview, genres, tags, rating, seasonsWithEpisodes,
    folderName, fileName, matchStatus, matchScore, refetch,
    focusedEpisode, setFocusedEpisode,
  }
}

// Lets MediaDetailHero hand its single hook result down via its `actions` render-prop.
export type MediaDetailResult = ReturnType<typeof useMediaDetail>
