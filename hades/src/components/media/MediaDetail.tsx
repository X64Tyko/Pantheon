import { useEffect, useState } from 'react'
import { api, mediaUrl } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import type { ArrLookupResult, ArrServiceOptions, ScraperSearchResult, ShowDetail, MovieDetail } from '../../api/types'
import { MatchBadge } from './MatchBadge'
import { goldBtnStyle } from '../../channel/styles'

interface MediaDetailProps {
  id?:             string
  content_type?:   'show' | 'movie'
  discoverResult?: ScraperSearchResult
  onClose:         () => void
}

const panel: React.CSSProperties = {
  width: 400, flexShrink: 0, height: '100%', overflow: 'hidden',
  display: 'flex', flexDirection: 'column',
  background: 'var(--hds-bg-2)', borderLeft: '1px solid var(--hds-line)',
}

const metaChip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
  padding: '2px 8px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
}

type ArrStep = 'idle' | 'loading' | 'form' | 'adding' | 'done' | 'error'

export function MediaDetail({ id, content_type, discoverResult, onClose }: MediaDetailProps) {
  const { user } = useAuth()
  const isAdmin  = user?.role === 'admin'

  // Library item state
  const [show,    setShow]    = useState<ShowDetail | null>(null)
  const [movie,   setMovie]   = useState<MovieDetail | null>(null)
  const [loading, setLoading] = useState(!discoverResult)

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

  // Fetch library item when no discoverResult
  useEffect(() => {
    if (discoverResult) return
    if (!id || !content_type) return
    setLoading(true)
    setShow(null)
    setMovie(null)
    if (content_type === 'show') {
      api.getShow(id).then(d => setShow(d)).finally(() => setLoading(false))
    } else {
      api.getMovie(id).then(d => setMovie(d)).finally(() => setLoading(false))
    }
  }, [id, content_type, discoverResult])

  // Reset transient state when item changes
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

  // Derive display data from either source
  const detail = discoverResult ? null : (show ?? movie)
  const contentType: 'show' | 'movie' = discoverResult?.content_type ?? content_type ?? 'show'

  const posterUrl = discoverResult?.poster_url
    ?? (detail?.thumb ? mediaUrl(contentType === 'show' ? `/api/shows/${id}/thumb` : `/api/movies/${id}/thumb`) : undefined)
  const backdropUrl = discoverResult?.poster_url   // use poster as backdrop for discover
    ?? (detail?.art   ? mediaUrl(contentType === 'show' ? `/api/shows/${id}/art`   : `/api/movies/${id}/art`)   : undefined)

  const title    = discoverResult?.title    ?? detail?.title    ?? ''
  const year     = discoverResult?.year     ?? detail?.year
  const overview = discoverResult?.overview ?? detail?.overview ?? ''
  const genres   = detail?.genres ?? []
  const rating   = detail?.audience_rating

  const serviceLabel = contentType === 'show' ? 'Sonarr' : 'Radarr'
  const srcColor = discoverResult?.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  // ── Request handler (viewer) ─────────────────────────────────────────────────

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

  // ── Arr handlers (admin) ──────────────────────────────────────────────────────

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

  return (
    <div style={panel} className="hds-in">
      {/* Backdrop */}
      <div style={{ position: 'relative', height: 200, flexShrink: 0 }}>
        <div style={{
          position: 'absolute', inset: 0,
          background: backdropUrl
            ? `url(${backdropUrl}) center/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
        }} />
        <div style={{
          position: 'absolute', inset: 0,
          background: 'linear-gradient(to top, var(--hds-bg-2) 0%, transparent 55%)',
        }} />
        <button onClick={onClose} style={{
          position: 'absolute', top: 12, right: 12,
          width: 28, height: 28, borderRadius: '50%', border: 'none', cursor: 'pointer',
          background: 'oklch(0 0 0 / 0.5)', color: 'oklch(0.8 0.01 285)', fontSize: 18,
          display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}>×</button>
        {posterUrl && (
          <img src={posterUrl} alt="" style={{
            position: 'absolute', bottom: -32, left: 20,
            width: 60, height: 90, objectFit: 'cover', borderRadius: 6,
            boxShadow: '0 4px 20px oklch(0 0 0 / 0.5)',
          }} />
        )}
      </div>

      {/* Body */}
      <div style={{ flex: 1, overflowY: 'auto', padding: posterUrl ? '44px 20px 24px' : '16px 20px 24px' }}>
        {loading && (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
            {[180, 100, 80, 260, 200].map((w, i) => (
              <div key={i} className="hds-skeleton" style={{ height: 14, borderRadius: 4, width: w }} />
            ))}
          </div>
        )}

        {(detail || discoverResult) && (
          <>
            <div style={{ display: 'flex', alignItems: 'flex-start', gap: 8, marginBottom: 8 }}>
              <h2 style={{
                fontFamily: "'Chakra Petch', sans-serif", fontSize: 18, fontWeight: 700,
                color: 'var(--hds-txt)', margin: 0, flex: 1, lineHeight: 1.2,
              }}>{title}</h2>
              {detail?.locked && (
                <span style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 7px',
                  borderRadius: 8, background: 'oklch(0.55 0.14 292 / 0.2)',
                  border: '1px solid oklch(0.7 0.13 287 / 0.4)', color: 'var(--hds-violet)',
                  flexShrink: 0, marginTop: 3,
                }}>LOCKED</span>
              )}
            </div>

            <div style={{ display: 'flex', gap: 8, marginBottom: 12, flexWrap: 'wrap' }}>
              {year && <span style={metaChip}>{year}</span>}
              {detail?.content_rating && <span style={metaChip}>{detail.content_rating}</span>}
              {rating != null && (
                <span style={{ ...metaChip, color: 'var(--hds-gold)', borderColor: 'oklch(0.83 0.13 84 / 0.4)' }}>
                  ★ {rating.toFixed(1)}
                </span>
              )}
              {discoverResult && (
                <span style={{
                  ...metaChip, color: srcColor,
                  borderColor: discoverResult.source === 'tmdb' ? 'oklch(0.65 0.18 220 / 0.4)' : 'oklch(0.65 0.12 280 / 0.4)',
                }}>{discoverResult.source.toUpperCase()}</span>
              )}
              {discoverResult?.in_library && (
                <span style={{ ...metaChip, color: 'oklch(0.7 0.16 150)', borderColor: 'oklch(0.7 0.16 150 / 0.4)' }}>
                  IN LIBRARY
                </span>
              )}
            </div>

            {genres.length > 0 && (
              <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap', marginBottom: 14 }}>
                {genres.map(g => <span key={g} style={metaChip}>{g}</span>)}
              </div>
            )}

            {overview && (
              <p style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 11, lineHeight: 1.7,
                color: 'var(--hds-txt-2)', margin: '0 0 18px',
              }}>{overview}</p>
            )}

            {/* Library item: match section */}
            {detail && (
              <div style={{
                padding: '12px 14px', borderRadius: 8,
                background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', marginBottom: 14,
              }}>
                <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
                  <MatchBadge status="unscraped" size="md" />
                  <button disabled style={{
                    fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
                    padding: '3px 10px', borderRadius: 6, cursor: 'not-allowed',
                    border: '1px solid var(--hds-line)', background: 'transparent',
                    color: 'var(--hds-txt-3)', opacity: 0.5,
                  }}>Review Match</button>
                </div>
              </div>
            )}

            {/* Library item: push to sources (disabled placeholder) */}
            {detail && (
              <button disabled style={{
                ...goldBtnStyle, width: '100%', opacity: 0.4, cursor: 'not-allowed', marginBottom: 18,
                boxSizing: 'border-box',
              }}>
                Push to Sources
              </button>
            )}

            {/* Discover item: action section — role-aware */}
            {discoverResult && !discoverResult.in_library && (
              <div style={{ borderTop: '1px solid var(--hds-line-s)', paddingTop: 16, marginBottom: 14 }}>
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
            )}

            {/* Show: seasons */}
            {show && show.seasons.length > 0 && (
              <div>
                <div style={{
                  fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                  color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 10,
                }}>SEASONS</div>
                <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
                  {show.seasons.map(s => (
                    <div key={s.number} style={{
                      padding: '8px 12px', borderRadius: 6,
                      border: '1px solid var(--hds-line-s)', background: 'var(--hds-bg)',
                      display: 'flex', justifyContent: 'space-between',
                    }}>
                      <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, color: 'var(--hds-txt-2)', fontWeight: 500 }}>
                        {s.name || `Season ${s.number}`}
                      </span>
                      <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)' }}>
                        S{String(s.number).padStart(2, '0')}
                      </span>
                    </div>
                  ))}
                </div>
              </div>
            )}

            {/* Movie: credits */}
            {movie && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                {movie.director && (
                  <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                    <span style={{ color: 'var(--hds-txt-2)' }}>Director</span> · {movie.director}
                  </div>
                )}
                {movie.studio && (
                  <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                    <span style={{ color: 'var(--hds-txt-2)' }}>Studio</span> · {movie.studio}
                  </div>
                )}
                {movie.duration_ms > 0 && (
                  <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                    <span style={{ color: 'var(--hds-txt-2)' }}>Runtime</span> · {Math.round(movie.duration_ms / 60000)} min
                  </div>
                )}
              </div>
            )}
          </>
        )}
      </div>
    </div>
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
