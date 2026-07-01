import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import type { ArrLookupResult, ArrServiceOptions, ScraperSearchResult } from '../../api/types'
import { MatchBadge } from './MatchBadge'
import { goldBtnStyle } from '../../channel/styles'

interface LibraryDetailActionsProps {
  id?:             string
  content_type?:   'show' | 'movie'
  discoverResult?: ScraperSearchResult
}

type ArrStep = 'idle' | 'loading' | 'form' | 'adding' | 'done' | 'error'

export function LibraryDetailActions({ id, content_type, discoverResult }: LibraryDetailActionsProps) {
  const { user } = useAuth()
  const isAdmin  = user?.role === 'admin'
  const isLibraryItem = !discoverResult && !!id && !!content_type
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

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
    } catch {
      setArrError(`Could not reach ${serviceLabel}. Check arr configuration in Sources.`)
      setArrStep('error')
    }
  }

  const handleArrAdd = async () => {
    if (!arrResult || qualityProfileId === null || !rootFolder) return
    setArrStep('adding')
    try {
      await api.arrAdd({ type: contentType, add_data: arrResult.add_data, quality_profile_id: qualityProfileId, root_folder: rootFolder, search_on_add: searchOnAdd })
      setArrStep('done')
    } catch {
      setArrError(`Failed to add to ${serviceLabel}.`)
      setArrStep('error')
    }
  }

  if (isLibraryItem) {
    return (
      <div style={{ display: 'flex', flexDirection: 'column', gap: 12, margin: '4px 0 22px', maxWidth: 420 }}>
        <div style={{
          padding: '12px 14px', borderRadius: 8,
          background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)',
          display: 'flex', alignItems: 'center', justifyContent: 'space-between',
        }}>
          <MatchBadge status="unscraped" size="md" />
          <button disabled style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
            padding: '3px 10px', borderRadius: 6, cursor: 'not-allowed',
            border: '1px solid var(--hds-line)', background: 'transparent',
            color: 'var(--hds-txt-3)', opacity: 0.5,
          }}>Review Match</button>
        </div>

        <button disabled style={{ ...goldBtnStyle, opacity: 0.4, cursor: 'not-allowed', boxSizing: 'border-box' }}>
          Push to Sources
        </button>
      </div>
    )
  }

  if (discoverResult && !discoverResult.in_library) {
    return (
      <div style={{ borderTop: '1px solid var(--hds-line-s)', borderBottom: '1px solid var(--hds-line-s)', padding: '16px 0', margin: '4px 0 22px', maxWidth: 420 }}>
        {isAdmin ? (
          // Admin: add directly to arr service
          <>
            <div style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 10,
            }}>{serviceLabel.toUpperCase()}</div>

            {arrStep === 'idle' && (
              <button onClick={handleArrLookup} style={{
                width: '100%', padding: '9px 0', borderRadius: 8, cursor: 'pointer',
                border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
                color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
                transition: 'border-color .12s',
              }}
              onMouseEnter={e => e.currentTarget.style.borderColor = 'var(--hds-violet)'}
              onMouseLeave={e => e.currentTarget.style.borderColor = 'var(--hds-line)'}
              >Add to {serviceLabel} →</button>
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
              <button onClick={handleRequest} style={{
                width: '100%', padding: '9px 0', borderRadius: 8, cursor: 'pointer',
                border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
                color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
                transition: 'border-color .12s',
              }}
              onMouseEnter={e => e.currentTarget.style.borderColor = 'var(--hds-gold)'}
              onMouseLeave={e => e.currentTarget.style.borderColor = 'var(--hds-line)'}
              >Request →</button>
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
