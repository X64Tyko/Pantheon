import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import type { ArrLookupResult, ArrServiceOptions, ScraperSearchResult } from '../../api/types'
import type { MatchStatus } from './MatchBadge'
import { MatchBadge } from './MatchBadge'
import { FixMatchPanel } from './FixMatchPanel'
import { goldBtnStyle } from '../../channel/styles'
import { resolvePlayPath, resolvePlayTarget } from '../../player/resolvePlayTarget'
import { useFocusable } from '../../nav/useFocusable'
import type { MediaDetailResult } from './useMediaDetail'
import styles from './LibraryDetailActions.module.css'

interface LibraryDetailActionsProps {
  id?:              string
  content_type?:    'show' | 'movie'
  discoverResult?:  ScraperSearchResult
  onViewInLibrary?: (id: string, content_type: 'show' | 'movie') => void
  // MediaDetailHero's own useMediaDetail() result, shared rather than re-fetched.
  media:            MediaDetailResult
}

type ArrStep = 'idle' | 'loading' | 'form' | 'adding' | 'done' | 'error'

const REQUEST_STATUS_LABEL: Record<string, string> = {
  pending:  'already requested — pending admin review',
  approved: 'already requested — approved, on its way',
  rejected: 'already requested — the request was declined',
}

export function LibraryDetailActions({ id, content_type, discoverResult, onViewInLibrary, media }: LibraryDetailActionsProps) {
  const { user } = useAuth()
  const isAdmin  = user?.role === 'admin'
  const isLibraryItem = !discoverResult && !!id && !!content_type
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

  const { detail, movie, title: detailTitle, refetch: refetchDetail } = media
  const [fixMatchOpen, setFixMatchOpen] = useState(false)

  // Push to Sources (admin) — writeback is gated server-side on match_confirmed
  // too; the button is just the first line of defense.
  const [pushing, setPushing] = useState(false)
  const [pushResult, setPushResult] = useState<string | null>(null)

  const handlePush = async () => {
    if (!id) return
    setPushing(true)
    setPushResult(null)
    try {
      const { results } = await api.pushToSources(id, contentType)
      if (results.length === 0) setPushResult('Not mapped to any source.')
      else if (results.every(r => r.ok)) setPushResult(`Pushed to ${results.map(r => r.source_type).join(', ')}`)
      else setPushResult(results.map(r => `${r.source_type}: ${r.ok ? 'ok' : 'failed'}`).join(' · '))
    } catch {
      setPushResult('Failed to push.')
    } finally {
      setPushing(false)
    }
  }

  // Refresh metadata (admin) — re-fetches overview/genres/images/etc. from
  // whichever scraper this item is already matched to (same apply path as
  // accepting a match, just re-run against the existing one), then clears
  // Kairos's own image cache so a same-URL-but-updated poster/backdrop shows
  // up immediately instead of waiting out image_cache_ttl_hours.
  const [refreshingMetadata, setRefreshingMetadata] = useState(false)
  const [refreshMetadataResult, setRefreshMetadataResult] = useState<string | null>(null)

  const handleRefreshMetadata = async () => {
    if (!id) return
    setRefreshingMetadata(true)
    setRefreshMetadataResult(null)
    try {
      await api.refreshMetadata(id, contentType)
      refetchDetail() // re-fetches detail fields and busts the browser-side image cache — see useMediaDetail's bust()
      setRefreshMetadataResult('Refreshed')
    } catch (e: any) {
      setRefreshMetadataResult(e.message ?? 'Failed to refresh metadata.')
    } finally {
      setRefreshingMetadata(false)
    }
  }

  // Process Chapters (admin) — re-probes chapter markers for just this item.
  const [processingChapters, setProcessingChapters] = useState(false)
  const [chapterResult,      setChapterResult]      = useState<string | null>(null)

  const handleProcessChapters = async () => {
    if (!id) return
    setProcessingChapters(true)
    setChapterResult(null)
    try {
      if (contentType === 'movie') {
        const chapters = await api.syncMovieChapters(id)
        setChapterResult(`${chapters.length} chapter${chapters.length !== 1 ? 's' : ''} found`)
      } else {
        const r = await api.syncShowChapters(id)
        setChapterResult(`${r.with_chapters}/${r.episode_count} episode${r.episode_count !== 1 ? 's' : ''} have chapters`)
      }
    } catch (e: any) {
      setChapterResult(e.message ?? 'Failed to process chapters.')
    } finally {
      setProcessingChapters(false)
    }
  }

  // Detect Structure (admin) — slow async analysis, separate from the fast
  // marker re-probe above; polls the global status endpoint until done.
  const [detecting,    setDetecting]    = useState(false)
  const [detectResult, setDetectResult] = useState<string | null>(null)
  const detectPollRef = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => () => { if (detectPollRef.current) clearInterval(detectPollRef.current) }, [])

  const pollDetectStatus = () => {
    detectPollRef.current = setInterval(async () => {
      try {
        const { running } = await api.getChapterDetectStatus()
        if (!running) {
          if (detectPollRef.current) clearInterval(detectPollRef.current)
          detectPollRef.current = null
          setDetecting(false)
          setDetectResult('Done — check the Chapters review tab')
        }
      } catch {}
    }, 3000)
  }

  const handleDetectStructure = async () => {
    if (!id) return
    setDetecting(true)
    setDetectResult(null)
    try {
      const r = contentType === 'movie' ? await api.detectMovieChapters(id) : await api.detectShowChapters(id)
      if (r.status === 'already_running') {
        setDetecting(false)
        setDetectResult('Another detection pass is already running — try again shortly.')
        return
      }
      pollDetectStatus()
    } catch (e: any) {
      setDetecting(false)
      setDetectResult(e.message ?? 'Failed to start detection.')
    }
  }

  // Request state (viewer)
  const [reqStep,      setReqStep]      = useState<'idle' | 'loading' | 'done' | 'error'>('idle')
  const [reqDuplicate, setReqDuplicate] = useState(false)

  // Arr state (admin)
  const [arrStep,          setArrStep]          = useState<ArrStep>('idle')
  const [arrResult,        setArrResult]        = useState<ArrLookupResult | null>(null)
  const [options,          setOptions]          = useState<ArrServiceOptions | null>(null)
  const [qualityProfileId, setQualityProfileId] = useState<number | null>(null)
  const [rootFolder,       setRootFolder]       = useState('')
  const [searchOnAdd,      setSearchOnAdd]      = useState(true)
  const [arrError,         setArrError]         = useState('')
  const [alreadyAdded,     setAlreadyAdded]     = useState(false)

  // Reset transient state when the selected item changes
  useEffect(() => {
    setReqStep('idle')
    setReqDuplicate(false)
    setArrStep('idle')
    setArrResult(null)
    setOptions(null)
    setQualityProfileId(null)
    setRootFolder('')
    setArrError('')
    setAlreadyAdded(false)
    setFixMatchOpen(false)
    setChapterResult(null)
    if (detectPollRef.current) { clearInterval(detectPollRef.current); detectPollRef.current = null }
    setDetecting(false)
    setDetectResult(null)
  }, [id, discoverResult?.external_id])

  const serviceLabel = contentType === 'show' ? 'Sonarr' : 'Radarr'

  const handleRequest = async () => {
    if (!discoverResult) return
    setReqStep('loading')
    try {
      const result = await api.createRequest({
        content_type: discoverResult.content_type,
        source:       discoverResult.source,
        external_id:  discoverResult.external_id,
        title:        discoverResult.title,
        year:         discoverResult.year,
        poster_url:   discoverResult.poster_url,
      })
      setReqDuplicate(!!result.duplicate)
      setReqStep('done')
    } catch {
      setReqStep('error')
    }
  }

  const handleArrLookup = async () => {
    if (!discoverResult) return
    setArrStep('loading')
    setArrError('')
    try {
      const params: Parameters<typeof api.arrLookup>[0] = { type: contentType }
      if (discoverResult.source === 'tvdb')       params.tvdb_id = discoverResult.external_id
      else if (contentType === 'movie')           params.tmdb_id = discoverResult.external_id
      else                                        params.title   = discoverResult.title

      const [results, opts] = await Promise.all([
        api.arrLookup(params),
        api.arrOptions(contentType),
      ])

      if (results.length === 0) {
        setArrError(`Not found in ${serviceLabel}. Check that ${serviceLabel} is configured.`)
        setArrStep('error')
        return
      }
      if (results[0].already_added) {
        setAlreadyAdded(true)
        setArrStep('done')
        return
      }
      setArrResult(results[0])
      setOptions(opts)
      if (opts.quality_profiles.length > 0) setQualityProfileId(opts.quality_profiles[0].id)
      if (opts.root_folders.length > 0)     setRootFolder(opts.root_folders[0])
      setArrStep('form')
    } catch (e: any) {
      setArrError(e.message ?? `Could not reach ${serviceLabel}. Check arr configuration in Sources.`)
      setArrStep('error')
    }
  }

  const handleArrAdd = async () => {
    if (!arrResult || qualityProfileId === null || !rootFolder) return
    setArrStep('adding')
    try {
      await api.arrAdd({ type: contentType, add_data: arrResult.add_data, quality_profile_id: qualityProfileId, root_folder: rootFolder, search_on_add: searchOnAdd })
      setArrStep('done')
    } catch (e: any) {
      setArrError(e.message ?? `Failed to add to ${serviceLabel}.`)
      setArrStep('error')
    }
  }

  if (isLibraryItem) {
    const matchStatus = (detail?.match_status ?? 'unscraped') as MatchStatus
    return (
      <div className={styles.libraryActionsRoot}>
        <div className={styles.matchStatusRow}>
          <div className={styles.matchStatusLeft}>
            <MatchBadge status={matchStatus} score={detail?.match_score} size="md" />
          </div>
          {isAdmin && (
            <FixMatchButton active={fixMatchOpen} onClick={() => setFixMatchOpen(o => !o)} />
          )}
        </div>

        {isAdmin && fixMatchOpen && id && (
          <FixMatchPanel
            id={id}
            contentType={contentType}
            defaultQuery={detailTitle}
            locked={!!detail?.locked}
            folderPath={detail?.folder_path}
            matchStatus={matchStatus}
            matchConfirmed={!!detail?.match_confirmed}
            // Picking a search result no longer closes the panel — it only
            // sets the new primary and refreshes the detail's match badge,
            // so the linked-ids list stays open for further add/remove/
            // reorder right after. "Close" (onCancel) is the explicit exit.
            onMatched={refetchDetail}
            onCancel={() => setFixMatchOpen(false)}
          />
        )}

        {isAdmin && (
          <>
            <button
              onClick={handlePush}
              disabled={pushing || !detail?.match_confirmed}
              title={detail?.match_confirmed ? undefined : 'Confirm this match (Fix Match) before pushing to sources'}
              style={goldBtnStyle}
              className={`${styles.pushButton} ${(pushing || !detail?.match_confirmed) ? styles.pushButtonDisabled : ''}`}
            >
              {pushing ? 'Pushing…' : 'Push to Sources'}
            </button>
            {pushResult && (
              <div className={styles.resultText}>
                {pushResult}
              </div>
            )}
            <button
              onClick={handleRefreshMetadata}
              disabled={refreshingMetadata || !detail?.match_confirmed}
              title={detail?.match_confirmed ? "Re-fetch this item's metadata (overview, genres, images, etc.) from its matched scraper" : 'Confirm this match (Fix Match) before refreshing metadata'}
              className={`${styles.secondaryActionButton} ${(refreshingMetadata || !detail?.match_confirmed) ? styles.secondaryActionButtonDisabled : ''}`}
            >
              {refreshingMetadata ? 'Refreshing…' : 'Refresh Metadata'}
            </button>
            {refreshMetadataResult && (
              <div className={styles.resultText}>
                {refreshMetadataResult}
              </div>
            )}
            <button
              onClick={handleProcessChapters}
              disabled={processingChapters}
              title={contentType === 'movie'
                ? 'Re-probe this file for chapter markers'
                : "Re-probe every episode's file for chapter markers"}
              className={`${styles.secondaryActionButton} ${processingChapters ? styles.secondaryActionButtonDisabled : ''}`}
            >
              {processingChapters ? 'Processing…' : 'Process Chapters'}
            </button>
            {chapterResult && (
              <div className={styles.resultText}>
                {chapterResult}
              </div>
            )}
            <button
              onClick={handleDetectStructure}
              disabled={detecting}
              title={contentType === 'movie'
                ? 'Analyze this file for ad-break points'
                : 'Analyze every episode for intro/credits/ad-break structure'}
              className={`${styles.secondaryActionButton} ${detecting ? styles.secondaryActionButtonDisabled : ''}`}
            >
              {detecting ? 'Detecting…' : 'Detect Structure'}
            </button>
            {detectResult && (
              <div className={styles.resultText}>
                {detectResult}
              </div>
            )}
          </>
        )}
      </div>
    )
  }

  if (discoverResult && discoverResult.in_library) {
    return (
      <div className={styles.discoverPanel}>
        <div className={`${styles.infoBox} ${styles.infoBoxGreen}`}>
          Already in your library — no need to request or add it.
        </div>
        {onViewInLibrary && discoverResult.library_id && (
          <button
            onClick={() => onViewInLibrary(discoverResult.library_id!, discoverResult.content_type)}
            className={styles.viewInLibraryButton}
          >
            View in Library →
          </button>
        )}
      </div>
    )
  }

  if (discoverResult && !discoverResult.in_library) {
    return (
      <div className={styles.discoverPanel}>
        {discoverResult.request_status && (
          <div className={`${styles.infoBox} ${styles.infoBoxAmber}`}>
            {REQUEST_STATUS_LABEL[discoverResult.request_status] ?? 'Already requested by someone else.'}
          </div>
        )}
        {isAdmin ? (
          // Admin: add directly to arr service
          <>
            <div className={styles.serviceLabel}>{serviceLabel.toUpperCase()}</div>

            {arrStep === 'idle' && (
              <ArrLookupButton onClick={handleArrLookup} label={`Add to ${serviceLabel} →`} />
            )}

            {(arrStep === 'loading' || arrStep === 'adding') && (
              <div className={styles.loadingText}>
                {arrStep === 'loading' ? `Looking up in ${serviceLabel}…` : `Adding to ${serviceLabel}…`}
              </div>
            )}

            {arrStep === 'form' && options && (
              <div className={styles.arrForm}>
                <label className={styles.arrFormLabel}>
                  Quality Profile
                  <select value={qualityProfileId ?? ''} onChange={e => setQualityProfileId(Number(e.target.value))} className={styles.arrSelect}>
                    {options.quality_profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                  </select>
                </label>
                <label className={styles.arrFormLabel}>
                  Root Folder
                  <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} className={styles.arrSelect}>
                    {options.root_folders.map(f => <option key={f} value={f}>{f}</option>)}
                  </select>
                </label>
                <label className={styles.arrCheckboxLabel}>
                  <input type="checkbox" checked={searchOnAdd} onChange={e => setSearchOnAdd(e.target.checked)} className={styles.arrCheckbox} />
                  Search immediately
                </label>
                <div className={styles.arrFormActions}>
                  <button onClick={() => setArrStep('idle')} className={styles.arrCancelButton}>Cancel</button>
                  <button onClick={handleArrAdd} className={styles.arrAddButton}>Add to {serviceLabel}</button>
                </div>
              </div>
            )}

            {arrStep === 'done' && (
              <div className={`${styles.infoBox} ${styles.infoBoxGreen}`}>
                {alreadyAdded ? `Already in ${serviceLabel}` : `Added${searchOnAdd ? ' — search queued' : ''}`}
              </div>
            )}

            {arrStep === 'error' && (
              <div className={styles.errorStack}>
                <div className={`${styles.infoBox} ${styles.infoBoxRed}`}>{arrError}</div>
                <button onClick={() => setArrStep('idle')} className={styles.tryAgainButton}>Try Again</button>
              </div>
            )}
          </>
        ) : (
          // Viewer: submit a request for admin to approve
          <>
            <div className={styles.serviceLabel}>REQUEST</div>

            {reqStep === 'idle' && (
              <RequestButton onClick={handleRequest} />
            )}

            {reqStep === 'loading' && (
              <div className={styles.loadingText}>
                Submitting request…
              </div>
            )}

            {reqStep === 'done' && (
              <div className={`${styles.infoBox} ${styles.infoBoxGreen}`}>
                {reqDuplicate ? 'Already requested' : 'Requested — an admin will review it'}
              </div>
            )}

            {reqStep === 'error' && (
              <div className={styles.errorStack}>
                <div className={`${styles.infoBox} ${styles.infoBoxRed}`}>Failed to submit request.</div>
                <button onClick={() => setReqStep('idle')} className={styles.tryAgainButton}>Try Again</button>
              </div>
            )}
          </>
        )}
      </div>
    )
  }

  return null
}

function FixMatchButton({ active, onClick }: { active: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-fix-match', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={`${styles.fixMatchButton} ${active ? styles.fixMatchButtonActive : ''}`}
    >{active ? 'Cancel' : 'Fix Match'}</button>
  )
}

// Rendered separately from LibraryDetailActions itself — MediaDetailHero
// puts this in its sticky header (locked alongside poster/title) while the
// rest of LibraryDetailActions (match status, Fix Match, Push to Sources,
// etc.) stays in the plain-scrolling content below. Self-contained (its own
// play-resolution state) rather than threaded through LibraryDetailActions'
// props, since that component's `media` prop carries a lot of unrelated
// admin/match-fixing state this button has no business touching.
export function PlayAction({ id, content_type, discoverResult }: {
  id?: string; content_type?: 'show' | 'movie'; discoverResult?: ScraperSearchResult
}) {
  const navigate = useNavigate()
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'
  const [playLoading, setPlayLoading] = useState(false)
  if (discoverResult || !id || !content_type) return null

  const handlePlay = async () => {
    setPlayLoading(true)
    try {
      const path = await resolvePlayPath(contentType, id)
      if (path) navigate(path)
    } finally {
      setPlayLoading(false)
    }
  }
  return <PlayButton onClick={handlePlay} loading={playLoading} />
}

// Host-initiated — creates a Kairos Watch Together session for whatever
// PlayAction's own resolvePlayTarget would resolve to (a show's actual next-
// episode-to-play, same as a plain Play click), then opens it. Same
// discoverResult/id/content_type gating as PlayAction — a discover-result
// (not yet in the library) has nothing to create a session against.
export function WatchTogetherAction({ id, content_type, discoverResult }: {
  id?: string; content_type?: 'show' | 'movie'; discoverResult?: ScraperSearchResult
}) {
  const navigate = useNavigate()
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'
  const [loading, setLoading] = useState(false)
  if (discoverResult || !id || !content_type) return null

  const handleClick = async () => {
    setLoading(true)
    try {
      const target = await resolvePlayTarget(contentType, id)
      if (!target) return
      const session = await api.createWatchTogether(target.kind, target.id)
      const t = target.positionMs > 0 ? `&t=${target.positionMs}` : ''
      navigate(`/player/${target.kind}/${target.id}?wt=${session.session_id}${t}`)
    } catch {
      // Best-effort, same as PlayAction — a failed create just leaves the
      // button clickable again rather than surfacing a dedicated error UI.
    } finally {
      setLoading(false)
    }
  }
  return <WatchTogetherButton onClick={handleClick} loading={loading} />
}

function WatchTogetherButton({ onClick, loading }: { onClick: () => void; loading: boolean }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-watch-together', onEnterPress: onClick, focusable: !loading })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick} disabled={loading}
      className={`${styles.secondaryActionButton} ${loading ? styles.secondaryActionButtonDisabled : ''}`}>
      {loading ? 'Starting…' : 'Watch Together'}
    </button>
  )
}

// "Play from Beginning" — ignores watch progress entirely, unlike PlayAction/
// WatchTogetherAction above. Mirrors Android's DetailViewModel.
// playFromBeginningTarget() exactly: a movie always restarts at position 0;
// a show goes to the first episode of the first shelf as currently
// displayed (media.seasonsWithEpisodes[0] — respects aired-order
// interleaving the same way the shelves the viewer actually sees do,
// whatever season/special genuinely renders first). Computed from
// MediaDetailHero's own already-fetched useMediaDetail() result (the
// `media` render-prop, same one `actions` already receives) rather than a
// second fetch or a server round-trip — there's no resume logic to
// delegate to resolve-play-target for here.
export function PlayFromBeginningAction({id, content_type, discoverResult, media}: {
    id?: string; content_type?: 'show' | 'movie'; discoverResult?: ScraperSearchResult; media: MediaDetailResult
}) {
    const navigate = useNavigate()
    const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'
    if (discoverResult || !id || !content_type) return null

    const firstEpisodeId = media.seasonsWithEpisodes[0]?.episodes[0]?.episode_id
    const disabled = contentType === 'show' && !firstEpisodeId

    const handleClick = () => {
        if (contentType === 'movie') navigate(`/player/movie/${id}`)
        else if (firstEpisodeId) navigate(`/player/episode/${firstEpisodeId}`)
    }
    return <PlayFromBeginningButton onClick={handleClick} disabled={disabled}/>
}

function PlayFromBeginningButton({onClick, disabled}: { onClick: () => void; disabled?: boolean }) {
    const {ref, focused} = useFocusable<object, HTMLButtonElement>({
        focusKey: 'detail-play-from-beginning',
        onEnterPress: onClick,
        focusable: !disabled
    })
    return (
        <button
            ref={ref} data-tv-focused={focused}
            onClick={onClick} disabled={disabled}
            className={`${styles.secondaryActionButton} ${disabled ? styles.secondaryActionButtonDisabled : ''}`}>
            ↺ Play from Beginning
        </button>
    )
}

function PlayButton({ onClick, loading }: { onClick: () => void; loading: boolean }) {
  const { ref, focused, focusSelf } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-play', onEnterPress: onClick, focusable: !loading })
  // Whenever this button exists, it's the detail view's primary action —
  // claim focus the moment it mounts (its own conditional rendering above
  // already gates "is this the right primary action right now").
  useEffect(() => { focusSelf() }, [focusSelf])
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick} disabled={loading} style={goldBtnStyle}
      className={`${styles.playButtonExtra} ${loading ? styles.playButtonLoading : ''}`}>
      <svg width="13" height="13" viewBox="0 0 14 14" fill="currentColor"><path d="M3 1.5v11l9-5.5-9-5.5z" /></svg>
      {loading ? 'Loading…' : 'Play'}
    </button>
  )
}

function ArrLookupButton({ onClick, label }: { onClick: () => void; label: string }) {
  const { ref, focused, focusSelf } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-arr-lookup', onEnterPress: onClick })
  useEffect(() => { focusSelf() }, [focusSelf])
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={`${styles.lookupButton} ${styles.lookupButtonViolet}`}
    >{label}</button>
  )
}

function RequestButton({ onClick }: { onClick: () => void }) {
  const { ref, focused, focusSelf } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-request', onEnterPress: onClick })
  useEffect(() => { focusSelf() }, [focusSelf])
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={`${styles.lookupButton} ${styles.lookupButtonGold}`}
    >Request →</button>
  )
}
