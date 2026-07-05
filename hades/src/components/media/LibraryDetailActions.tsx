import { useEffect, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import type { ArrLookupResult, ArrServiceOptions, ScraperSearchResult } from '../../api/types'
import type { MatchStatus } from './MatchBadge'
import { MatchBadge } from './MatchBadge'
import { FixMatchPanel } from './FixMatchPanel'
import { goldBtnStyle } from '../../channel/styles'
import { resolvePlayPath } from '../../player/resolvePlayTarget'
import { useFocusable } from '../../nav/useFocusable'
import type { MediaDetailResult } from './useMediaDetail'

interface LibraryDetailActionsProps {
  id?:              string
  content_type?:    'show' | 'movie'
  discoverResult?:  ScraperSearchResult
  onViewInLibrary?: (id: string, content_type: 'show' | 'movie') => void
  // MediaDetailHero's own useMediaDetail() result, handed down instead of
  // this component fetching its own copy — a second independent fetch had
  // its own refreshKey, so Refresh Metadata's cache-bust never reached the
  // poster/backdrop <img> that MediaDetailHero actually renders.
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
  const navigate = useNavigate()
  const isAdmin  = user?.role === 'admin'
  const isLibraryItem = !discoverResult && !!id && !!content_type
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

  const { detail, title: detailTitle, refetch: refetchDetail } = media
  const [fixMatchOpen, setFixMatchOpen] = useState(false)

  const [playLoading, setPlayLoading] = useState(false)

  const handlePlay = async () => {
    if (!id) return
    setPlayLoading(true)
    try {
      const path = await resolvePlayPath(contentType, id)
      if (path) navigate(path)
    } finally {
      setPlayLoading(false)
    }
  }

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
      <div style={{ display: 'flex', flexDirection: 'column', gap: 12, margin: '4px 0 22px', maxWidth: 900 }}>
        <PlayButton onClick={handlePlay} loading={playLoading} />

        <div style={{
          padding: '12px 14px', borderRadius: 8,
          background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)',
          display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        }}>
          <MatchBadge status={matchStatus} score={detail?.match_score} size="md" />
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
            onMatched={() => { setFixMatchOpen(false); refetchDetail() }}
            onCancel={() => setFixMatchOpen(false)}
          />
        )}

        {isAdmin && (
          <>
            <button
              onClick={handlePush}
              disabled={pushing || !detail?.match_confirmed}
              title={detail?.match_confirmed ? undefined : 'Confirm this match (Fix Match) before pushing to sources'}
              style={{
                ...goldBtnStyle, boxSizing: 'border-box', alignSelf: 'flex-start',
                opacity: (pushing || !detail?.match_confirmed) ? 0.4 : 1,
                cursor: (pushing || !detail?.match_confirmed) ? 'not-allowed' : 'pointer',
              }}
            >
              {pushing ? 'Pushing…' : 'Push to Sources'}
            </button>
            {pushResult && (
              <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, color: 'var(--hds-txt-2)' }}>
                {pushResult}
              </div>
            )}
            <button
              onClick={handleRefreshMetadata}
              disabled={refreshingMetadata || !detail?.match_confirmed}
              title={detail?.match_confirmed ? "Re-fetch this item's metadata (overview, genres, images, etc.) from its matched scraper" : 'Confirm this match (Fix Match) before refreshing metadata'}
              style={{
                alignSelf: 'flex-start', padding: '8px 16px', borderRadius: 7,
                cursor: (refreshingMetadata || !detail?.match_confirmed) ? 'not-allowed' : 'pointer',
                border: '1px solid var(--hds-line)', background: 'transparent',
                color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                opacity: (refreshingMetadata || !detail?.match_confirmed) ? 0.5 : 1,
              }}
            >
              {refreshingMetadata ? 'Refreshing…' : 'Refresh Metadata'}
            </button>
            {refreshMetadataResult && (
              <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, color: 'var(--hds-txt-2)' }}>
                {refreshMetadataResult}
              </div>
            )}
          </>
        )}
      </div>
    )
  }

  if (discoverResult && discoverResult.in_library) {
    return (
      <div style={{ borderTop: '1px solid var(--hds-line-s)', borderBottom: '1px solid var(--hds-line-s)', padding: '16px 0', margin: '4px 0 22px', maxWidth: 420 }}>
        <div style={{
          padding: '10px 14px', borderRadius: 8,
          border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)', lineHeight: 1.5,
          marginBottom: (onViewInLibrary && discoverResult.library_id) ? 10 : 0,
        }}>
          Already in your library — no need to request or add it.
        </div>
        {onViewInLibrary && discoverResult.library_id && (
          <button
            onClick={() => onViewInLibrary(discoverResult.library_id!, discoverResult.content_type)}
            style={{ padding: '8px 16px', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-violet)', background: 'oklch(0.55 0.14 292 / 0.15)', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 600 }}
          >
            View in Library →
          </button>
        )}
      </div>
    )
  }

  if (discoverResult && !discoverResult.in_library) {
    return (
      <div style={{ borderTop: '1px solid var(--hds-line-s)', borderBottom: '1px solid var(--hds-line-s)', padding: '16px 0', margin: '4px 0 22px', maxWidth: 420 }}>
        {discoverResult.request_status && (
          <div style={{
            marginBottom: 14, padding: '10px 14px', borderRadius: 8,
            border: '1px solid oklch(0.78 0.15 84 / 0.4)', background: 'oklch(0.78 0.15 84 / 0.08)',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.78 0.15 84)', lineHeight: 1.5,
          }}>
            {REQUEST_STATUS_LABEL[discoverResult.request_status] ?? 'Already requested by someone else.'}
          </div>
        )}
        {isAdmin ? (
          // Admin: add directly to arr service
          <>
            <div style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 10,
            }}>{serviceLabel.toUpperCase()}</div>

            {arrStep === 'idle' && (
              <ArrLookupButton onClick={handleArrLookup} label={`Add to ${serviceLabel} →`} />
            )}

            {(arrStep === 'loading' || arrStep === 'adding') && (
              <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', padding: '8px 0' }}>
                {arrStep === 'loading' ? `Looking up in ${serviceLabel}…` : `Adding to ${serviceLabel}…`}
              </div>
            )}

            {arrStep === 'form' && options && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 11 }}>
                <label style={formLabelStyle}>
                  Quality Profile
                  <select value={qualityProfileId ?? ''} onChange={e => setQualityProfileId(Number(e.target.value))} style={selectStyle}>
                    {options.quality_profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                  </select>
                </label>
                <label style={formLabelStyle}>
                  Root Folder
                  <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} style={selectStyle}>
                    {options.root_folders.map(f => <option key={f} value={f}>{f}</option>)}
                  </select>
                </label>
                <label style={{ display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-2)' }}>
                  <input type="checkbox" checked={searchOnAdd} onChange={e => setSearchOnAdd(e.target.checked)} style={{ accentColor: 'var(--hds-violet)', width: 14, height: 14 }} />
                  Search immediately
                </label>
                <div style={{ display: 'flex', gap: 8, marginTop: 2 }}>
                  <button onClick={() => setArrStep('idle')} style={{ flex: 1, padding: '8px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>Cancel</button>
                  <button onClick={handleArrAdd} style={{ flex: 2, padding: '8px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-violet)', background: 'oklch(0.55 0.14 292 / 0.2)', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, fontWeight: 600 }}>Add to {serviceLabel}</button>
                </div>
              </div>
            )}

            {arrStep === 'done' && (
              <div style={{ padding: '10px 14px', borderRadius: 8, border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)' }}>
                {alreadyAdded ? `Already in ${serviceLabel}` : `Added${searchOnAdd ? ' — search queued' : ''}`}
              </div>
            )}

            {arrStep === 'error' && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                <div style={{ padding: '10px 14px', borderRadius: 8, border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)', lineHeight: 1.5 }}>{arrError}</div>
                <button onClick={() => setArrStep('idle')} style={{ padding: '7px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>Try Again</button>
              </div>
            )}
          </>
        ) : (
          // Viewer: submit a request for admin to approve
          <>
            <div style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 10,
            }}>REQUEST</div>

            {reqStep === 'idle' && (
              <RequestButton onClick={handleRequest} />
            )}

            {reqStep === 'loading' && (
              <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', padding: '8px 0' }}>
                Submitting request…
              </div>
            )}

            {reqStep === 'done' && (
              <div style={{ padding: '10px 14px', borderRadius: 8, border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)' }}>
                {reqDuplicate ? 'Already requested' : 'Requested — an admin will review it'}
              </div>
            )}

            {reqStep === 'error' && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                <div style={{ padding: '10px 14px', borderRadius: 8, border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)' }}>Failed to submit request.</div>
                <button onClick={() => setReqStep('idle')} style={{ padding: '7px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>Try Again</button>
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
      style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        padding: '8px 16px', borderRadius: 6, cursor: 'pointer',
        border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: active ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
        color: active ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
      }}
    >{active ? 'Cancel' : 'Fix Match'}</button>
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
      onClick={onClick} disabled={loading} style={{
        ...goldBtnStyle, boxSizing: 'border-box', display: 'flex', alignItems: 'center',
        justifyContent: 'center', gap: 8, opacity: loading ? 0.6 : 1,
        cursor: loading ? 'wait' : 'pointer', alignSelf: 'flex-start',
      }}>
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
      onClick={onClick} style={{
        width: '100%', padding: '9px 0', borderRadius: 8, cursor: 'pointer',
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
        color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
        transition: 'border-color .12s',
      }}
      onMouseEnter={e => e.currentTarget.style.borderColor = 'var(--hds-violet)'}
      onMouseLeave={e => e.currentTarget.style.borderColor = 'var(--hds-line)'}
    >{label}</button>
  )
}

function RequestButton({ onClick }: { onClick: () => void }) {
  const { ref, focused, focusSelf } = useFocusable<object, HTMLButtonElement>({ focusKey: 'detail-request', onEnterPress: onClick })
  useEffect(() => { focusSelf() }, [focusSelf])
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick} style={{
        width: '100%', padding: '9px 0', borderRadius: 8, cursor: 'pointer',
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
        color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
        transition: 'border-color .12s',
      }}
      onMouseEnter={e => e.currentTarget.style.borderColor = 'var(--hds-gold)'}
      onMouseLeave={e => e.currentTarget.style.borderColor = 'var(--hds-line)'}
    >Request →</button>
  )
}

const formLabelStyle: React.CSSProperties = {
  display: 'flex', flexDirection: 'column', gap: 6,
  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
  color: 'var(--hds-txt-3)', letterSpacing: '0.06em',
}

const selectStyle: React.CSSProperties = {
  padding: '7px 10px', borderRadius: 7,
  border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
  color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
  cursor: 'pointer', outline: 'none',
}
