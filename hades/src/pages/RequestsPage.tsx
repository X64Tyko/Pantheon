import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { ArrLookupResult, ArrServiceOptions, ContentRequest, RequestStatus } from '../api/types'

const STATUS_COLOR: Record<RequestStatus, string> = {
  pending:  'var(--hds-match-amber)',
  approved: 'oklch(0.7 0.16 150)',
  rejected: 'var(--hds-match-red)',
}

export default function RequestsPage() {
  const [requests,  setRequests]  = useState<ContentRequest[]>([])
  const [loading,   setLoading]   = useState(true)
  const [filter,    setFilter]    = useState<'all' | RequestStatus>('pending')
  const [selected,  setSelected]  = useState<ContentRequest | null>(null)

  const load = () => {
    setLoading(true)
    api.getRequests().then(setRequests).finally(() => setLoading(false))
  }

  useEffect(() => { load() }, [])

  const visible = requests.filter(r => filter === 'all' || r.status === filter)
  const pending = requests.filter(r => r.status === 'pending').length

  return (
    <div style={{ display: 'flex', height: '100%', overflow: 'hidden', background: 'var(--hds-bg)' }}>
      {/* Left panel */}
      <div style={{
        width: 420, flexShrink: 0, height: '100%', display: 'flex', flexDirection: 'column',
        borderRight: '1px solid var(--hds-line)',
      }}>
        <div style={{ padding: '18px 20px 12px', borderBottom: '1px solid var(--hds-line)', flexShrink: 0 }}>
          <div style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 16, fontWeight: 700,
            color: 'var(--hds-txt)', marginBottom: 12,
          }}>
            Requests
            {pending > 0 && (
              <span style={{
                marginLeft: 10, padding: '2px 8px', borderRadius: 10,
                background: 'oklch(0.75 0.12 80 / 0.15)', border: '1px solid var(--hds-match-amber)',
                color: 'var(--hds-match-amber)',
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              }}>{pending}</span>
            )}
          </div>
          <div style={{ display: 'flex', gap: 6 }}>
            {(['pending', 'all', 'approved', 'rejected'] as const).map(f => (
              <button key={f} onClick={() => setFilter(f)} style={{
                padding: '4px 12px', borderRadius: 6, cursor: 'pointer', fontSize: 10,
                fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em',
                border: `1px solid ${filter === f ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
                background: filter === f ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
                color: filter === f ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
                transition: 'all .12s',
              }}>
                {f === 'all' ? 'All' : f}
                {f !== 'all' && (
                  <span style={{ marginLeft: 5, opacity: 0.7 }}>
                    {requests.filter(r => r.status === f).length}
                  </span>
                )}
              </button>
            ))}
          </div>
        </div>

        <div style={{ flex: 1, overflowY: 'auto' }}>
          {loading ? (
            <div style={{ padding: '16px 20px', display: 'flex', flexDirection: 'column', gap: 10 }}>
              {[...Array(5)].map((_, i) => (
                <div key={i} className="hds-skeleton" style={{ height: 72, borderRadius: 8 }} />
              ))}
            </div>
          ) : visible.length === 0 ? (
            <div style={{
              padding: '48px 20px', textAlign: 'center',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)',
            }}>
              {filter === 'pending' ? 'No pending requests' : `No ${filter} requests`}
            </div>
          ) : (
            <div style={{ padding: '10px 10px' }}>
              {visible.map(r => (
                <RequestRow
                  key={r.request_id}
                  request={r}
                  selected={selected?.request_id === r.request_id}
                  onClick={() => setSelected(prev => prev?.request_id === r.request_id ? null : r)}
                />
              ))}
            </div>
          )}
        </div>
      </div>

      {/* Right panel */}
      {selected ? (
        <RequestDetail
          request={selected}
          onClose={() => setSelected(null)}
          onStatusChange={(id, status) => {
            setRequests(prev => prev.map(r => r.request_id === id ? { ...r, status } : r))
            setSelected(prev => prev?.request_id === id ? { ...prev, status } : prev)
          }}
        />
      ) : (
        <div style={{
          flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)',
        }}>
          Select a request to review
        </div>
      )}
    </div>
  )
}

// ── Request row ───────────────────────────────────────────────────────────────

function RequestRow({ request: r, selected, onClick }: {
  request:  ContentRequest
  selected: boolean
  onClick:  () => void
}) {
  const srcColor = r.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  return (
    <div
      onClick={onClick}
      style={{
        display: 'flex', gap: 12, alignItems: 'center', padding: '10px 10px',
        borderRadius: 8, cursor: 'pointer', marginBottom: 2,
        background: selected ? 'oklch(0.55 0.14 292 / 0.1)' : 'transparent',
        border: `1px solid ${selected ? 'var(--hds-violet)' : 'transparent'}`,
        transition: 'background .1s, border-color .1s',
      }}
      onMouseEnter={e => { if (!selected) e.currentTarget.style.background = 'var(--hds-bg-2)' }}
      onMouseLeave={e => { if (!selected) e.currentTarget.style.background = 'transparent' }}
    >
      {/* Poster */}
      <div style={{
        width: 36, height: 54, borderRadius: 5, overflow: 'hidden', flexShrink: 0,
        background: 'var(--hds-bg-3)',
      }}>
        {r.poster_url && (
          <img src={r.poster_url} alt="" style={{ width: '100%', height: '100%', objectFit: 'cover' }}
            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
        )}
      </div>

      <div style={{ flex: 1, minWidth: 0 }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 13, fontWeight: 600,
          color: 'var(--hds-txt)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }}>{r.title}</div>
        <div style={{ display: 'flex', gap: 6, marginTop: 4, alignItems: 'center' }}>
          {r.year && <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)' }}>{r.year}</span>}
          <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: srcColor, border: `1px solid ${srcColor}`, borderRadius: 4, padding: '1px 5px' }}>{r.source.toUpperCase()}</span>
          <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: 'var(--hds-txt-3)' }}>{r.content_type}</span>
        </div>
      </div>

      {/* Status dot */}
      <div style={{
        flexShrink: 0, display: 'flex', flexDirection: 'column', alignItems: 'flex-end', gap: 4,
      }}>
        <span style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 8,
          letterSpacing: '0.08em', color: STATUS_COLOR[r.status],
        }}>{r.status.toUpperCase()}</span>
        <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: 'var(--hds-txt-3)' }}>
          {new Date(r.created_at * 1000).toLocaleDateString()}
        </span>
      </div>
    </div>
  )
}

// ── Request detail ────────────────────────────────────────────────────────────

type ArrStep = 'idle' | 'loading' | 'form' | 'adding' | 'done' | 'error'

function RequestDetail({ request: r, onClose, onStatusChange }: {
  request:        ContentRequest
  onClose:        () => void
  onStatusChange: (id: string, status: 'approved' | 'rejected') => void
}) {
  const [arrStep,          setArrStep]          = useState<ArrStep>('idle')
  const [arrResult,        setArrResult]        = useState<ArrLookupResult | null>(null)
  const [options,          setOptions]          = useState<ArrServiceOptions | null>(null)
  const [qualityProfileId, setQualityProfileId] = useState<number | null>(null)
  const [rootFolder,       setRootFolder]       = useState('')
  const [searchOnAdd,      setSearchOnAdd]      = useState(true)
  const [arrError,         setArrError]         = useState('')
  const [rejecting,        setRejecting]        = useState(false)

  const serviceLabel = r.content_type === 'show' ? 'Sonarr' : 'Radarr'
  const srcColor     = r.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  const handleApproveClick = async () => {
    setArrStep('loading')
    setArrError('')
    try {
      const params: Parameters<typeof api.arrLookup>[0] = { type: r.content_type }
      if (r.source === 'tvdb')            params.tvdb_id = r.external_id
      else if (r.content_type === 'movie') params.tmdb_id = r.external_id
      else                                params.title   = r.title

      const [results, opts] = await Promise.all([api.arrLookup(params), api.arrOptions(r.content_type)])
      if (results.length === 0) { setArrError(`Not found in ${serviceLabel}.`); setArrStep('error'); return }
      setArrResult(results[0])
      setOptions(opts)
      if (opts.quality_profiles.length > 0) setQualityProfileId(opts.quality_profiles[0].id)
      if (opts.root_folders.length > 0)     setRootFolder(opts.root_folders[0])
      setArrStep('form')
    } catch {
      setArrError(`Could not reach ${serviceLabel}.`)
      setArrStep('error')
    }
  }

  const handleConfirmApprove = async () => {
    if (!arrResult || qualityProfileId === null || !rootFolder) return
    setArrStep('adding')
    try {
      await api.arrAdd({ type: r.content_type, add_data: arrResult.add_data, quality_profile_id: qualityProfileId, root_folder: rootFolder, search_on_add: searchOnAdd })
      await api.updateRequest(r.request_id, 'approved')
      onStatusChange(r.request_id, 'approved')
      setArrStep('done')
    } catch {
      setArrError(`Failed to add to ${serviceLabel}.`)
      setArrStep('error')
    }
  }

  const handleReject = async () => {
    setRejecting(true)
    try {
      await api.updateRequest(r.request_id, 'rejected')
      onStatusChange(r.request_id, 'rejected')
    } finally {
      setRejecting(false)
    }
  }

  return (
    <div style={{ flex: 1, height: '100%', overflow: 'hidden', display: 'flex', flexDirection: 'column' }}>
      {/* Backdrop */}
      <div style={{ position: 'relative', height: 200, flexShrink: 0 }}>
        <div style={{
          position: 'absolute', inset: 0,
          background: r.poster_url
            ? `url(${r.poster_url}) center 20%/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
        }} />
        <div style={{ position: 'absolute', inset: 0, background: 'linear-gradient(to top, var(--hds-bg) 0%, oklch(0 0 0 / 0.4) 100%)' }} />
        <button onClick={onClose} style={{
          position: 'absolute', top: 12, right: 12, width: 28, height: 28, borderRadius: '50%',
          border: 'none', cursor: 'pointer', background: 'oklch(0 0 0 / 0.5)',
          color: 'oklch(0.8 0.01 285)', fontSize: 18, display: 'flex', alignItems: 'center', justifyContent: 'center',
        }}>×</button>
        {r.poster_url && (
          <img src={r.poster_url} alt="" style={{
            position: 'absolute', bottom: -32, left: 24,
            width: 60, height: 90, objectFit: 'cover', borderRadius: 6,
            boxShadow: '0 4px 20px oklch(0 0 0 / 0.5)',
          }} />
        )}
      </div>

      <div style={{ flex: 1, overflowY: 'auto', padding: r.poster_url ? '44px 24px 32px' : '20px 24px 32px', maxWidth: 560 }}>
        <div style={{ display: 'flex', alignItems: 'flex-start', justifyContent: 'space-between', gap: 12, marginBottom: 10 }}>
          <h2 style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 20, fontWeight: 700, color: 'var(--hds-txt)', margin: 0, lineHeight: 1.2 }}>
            {r.title}
          </h2>
          <span style={{
            flexShrink: 0, marginTop: 4,
            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, letterSpacing: '0.08em',
            color: STATUS_COLOR[r.status],
            border: `1px solid ${STATUS_COLOR[r.status]}`,
            padding: '2px 8px', borderRadius: 10,
            background: `${STATUS_COLOR[r.status].replace(')', ' / 0.08)')}`,
          }}>{r.status.toUpperCase()}</span>
        </div>

        <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', marginBottom: 16 }}>
          {r.year && <span style={metaChip}>{r.year}</span>}
          <span style={metaChip}>{r.content_type}</span>
          <span style={{ ...metaChip, color: srcColor, borderColor: `${srcColor.replace(')', ' / 0.4)')}` }}>{r.source.toUpperCase()}</span>
        </div>

        <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 20 }}>
          Requested {new Date(r.created_at * 1000).toLocaleString()} · user {r.user_id.slice(0, 8)}
        </div>

        {/* Actions */}
        {r.status === 'pending' && (
          <div style={{ borderTop: '1px solid var(--hds-line-s)', paddingTop: 18 }}>
            <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.08em', marginBottom: 12 }}>
              {serviceLabel.toUpperCase()}
            </div>

            {arrStep === 'idle' && (
              <div style={{ display: 'flex', gap: 8 }}>
                <button onClick={handleApproveClick} style={{
                  flex: 2, padding: '9px 0', borderRadius: 8, cursor: 'pointer',
                  border: '1px solid oklch(0.7 0.16 150 / 0.6)', background: 'oklch(0.7 0.16 150 / 0.1)',
                  color: 'oklch(0.7 0.16 150)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 600,
                }}>Approve → {serviceLabel}</button>
                <button onClick={handleReject} disabled={rejecting} style={{
                  flex: 1, padding: '9px 0', borderRadius: 8, cursor: rejecting ? 'default' : 'pointer',
                  border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)',
                  color: 'var(--hds-match-red)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
                  opacity: rejecting ? 0.5 : 1,
                }}>Reject</button>
              </div>
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
                  <button onClick={handleConfirmApprove} style={{ flex: 2, padding: '8px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid oklch(0.7 0.16 150 / 0.6)', background: 'oklch(0.7 0.16 150 / 0.1)', color: 'oklch(0.7 0.16 150)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, fontWeight: 600 }}>
                    Confirm → {serviceLabel}
                  </button>
                </div>
              </div>
            )}

            {arrStep === 'error' && (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                <div style={{ padding: '10px 14px', borderRadius: 8, border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)', lineHeight: 1.5 }}>{arrError}</div>
                <button onClick={() => setArrStep('idle')} style={{ padding: '7px 0', borderRadius: 7, cursor: 'pointer', border: '1px solid var(--hds-line)', background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>Try Again</button>
              </div>
            )}
          </div>
        )}

        {arrStep === 'done' && (
          <div style={{ padding: '12px 16px', borderRadius: 8, border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)' }}>
            Approved — added to {serviceLabel}{searchOnAdd ? ', search queued' : ''}
          </div>
        )}

        {r.status === 'approved' && arrStep !== 'done' && (
          <div style={{ padding: '12px 16px', borderRadius: 8, border: '1px solid oklch(0.7 0.16 150 / 0.4)', background: 'oklch(0.7 0.16 150 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'oklch(0.7 0.16 150)' }}>
            Approved
          </div>
        )}

        {r.status === 'rejected' && (
          <div style={{ padding: '12px 16px', borderRadius: 8, border: '1px solid var(--hds-match-red)', background: 'oklch(0.55 0.22 27 / 0.08)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-match-red)' }}>
            Rejected
          </div>
        )}
      </div>
    </div>
  )
}

const metaChip: React.CSSProperties = {
  fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
  padding: '2px 8px', borderRadius: 10,
  border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-3)',
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
