import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { ArrLookupResult, ArrQualityProfile } from '../api/types'

interface Props {
  type:     'show' | 'movie'
  title:    string
  tvdb_id?: string
  tmdb_id?: string
  imdb_id?: string
  onClose:  () => void
  onAdded:  () => void
}

const overlay: React.CSSProperties = {
  position: 'fixed', inset: 0, zIndex: 1000,
  background: 'rgba(0,0,0,0.65)', backdropFilter: 'blur(2px)',
  display: 'flex', alignItems: 'center', justifyContent: 'center',
}

const card: React.CSSProperties = {
  width: 520, maxHeight: '80vh', overflow: 'auto',
  background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line)',
  borderRadius: 14, boxShadow: '0 24px 80px -12px rgba(0,0,0,0.8)',
  fontFamily: "'JetBrains Mono', monospace",
  animation: 'hds-in 0.18s ease both',
}

const selectStyle: React.CSSProperties = {
  padding: '8px 10px', background: 'var(--hds-bg-3)',
  border: '1px solid var(--hds-line)', borderRadius: 7,
  color: 'var(--hds-txt)', fontSize: 12,
  fontFamily: "'JetBrains Mono', monospace", width: '100%', outline: 'none',
}

const btnPrimary: React.CSSProperties = {
  padding: '9px 18px', background: 'oklch(0.83 0.13 84 / 0.15)',
  border: '1px solid oklch(0.83 0.13 84 / 0.4)', borderRadius: 8,
  color: 'var(--hds-gold)', fontSize: 12, fontWeight: 600, cursor: 'pointer',
  fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.07em',
}

const btnGhost: React.CSSProperties = {
  padding: '9px 14px', background: 'transparent',
  border: '1px solid var(--hds-line)', borderRadius: 8,
  color: 'var(--hds-txt-2)', fontSize: 12, cursor: 'pointer',
  fontFamily: "'JetBrains Mono', monospace",
}

type Step = 'lookup' | 'confirm' | 'done'

export default function ArrAddModal({ type, title, tvdb_id, tmdb_id, imdb_id, onClose, onAdded }: Props) {
  const [step,       setStep]       = useState<Step>('lookup')
  const [results,    setResults]    = useState<ArrLookupResult[]>([])
  const [selected,   setSelected]   = useState<ArrLookupResult | null>(null)
  const [profiles,   setProfiles]   = useState<ArrQualityProfile[]>([])
  const [rootFolders,setRootFolders]= useState<string[]>([])
  const [qualityId,  setQualityId]  = useState<number>(0)
  const [rootFolder, setRootFolder] = useState('')
  const [loading,    setLoading]    = useState(true)
  const [adding,     setAdding]     = useState(false)
  const [error,      setError]      = useState('')

  useEffect(() => {
    const run = async () => {
      setLoading(true)
      setError('')
      try {
        const [lookupRes, optsRes] = await Promise.all([
          api.arrLookup({ type, title, tvdb_id, tmdb_id, imdb_id }),
          api.arrOptions(type),
        ])
        setResults(lookupRes)
        setProfiles(optsRes.quality_profiles)
        setRootFolders(optsRes.root_folders)
        if (optsRes.quality_profiles.length > 0) setQualityId(optsRes.quality_profiles[0].id)
        if (optsRes.root_folders.length > 0)     setRootFolder(optsRes.root_folders[0])
      } catch (e: any) {
        setError(e.message ?? 'Lookup failed')
      } finally {
        setLoading(false)
      }
    }
    run()
  }, [])

  const confirmAdd = async () => {
    if (!selected) return
    setAdding(true)
    setError('')
    try {
      await api.arrAdd({ type, add_data: selected.add_data, quality_profile_id: qualityId, root_folder: rootFolder })
      setStep('done')
      onAdded()
    } catch (e: any) {
      setError(e.message ?? 'Add failed')
    } finally {
      setAdding(false)
    }
  }

  const arrName = type === 'show' ? 'Sonarr' : 'Radarr'

  return (
    <div style={overlay} onClick={e => { if (e.target === e.currentTarget) onClose() }}>
      <div style={card}>
        {/* Header */}
        <div style={{ padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
          <div>
            <div style={{ fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', letterSpacing: '0.06em' }}>
              Add to {arrName}
            </div>
            <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 3 }}>{title}</div>
          </div>
          <button onClick={onClose} style={{ background: 'none', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 16, lineHeight: 1 }}>✕</button>
        </div>

        <div style={{ padding: 20, display: 'flex', flexDirection: 'column', gap: 16 }}>

          {/* Loading */}
          {loading && (
            <div style={{ fontSize: 12, color: 'var(--hds-txt-3)', textAlign: 'center', padding: '24px 0' }}>
              Searching {arrName}…
            </div>
          )}

          {/* Error */}
          {error && (
            <div style={{ fontSize: 11, color: 'oklch(0.72 0.18 22)', padding: '8px 10px', background: 'oklch(0.55 0.22 22 / 0.1)', borderRadius: 7, border: '1px solid oklch(0.55 0.22 22 / 0.3)' }}>
              {error}
            </div>
          )}

          {/* Done */}
          {step === 'done' && (
            <div style={{ textAlign: 'center', padding: '16px 0' }}>
              <div style={{ fontSize: 13, color: 'oklch(0.72 0.18 140)', fontWeight: 600, marginBottom: 6 }}>Added successfully</div>
              <div style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>{selected?.title} sent to {arrName}.</div>
            </div>
          )}

          {/* Step: lookup results */}
          {!loading && step === 'lookup' && results.length === 0 && !error && (
            <div style={{ fontSize: 12, color: 'var(--hds-txt-3)', textAlign: 'center', padding: '16px 0' }}>
              No results found in {arrName}.
            </div>
          )}

          {!loading && step === 'lookup' && results.length > 0 && (
            <>
              <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.08em' }}>SELECT A MATCH</div>
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8, maxHeight: 320, overflowY: 'auto' }}>
                {results.map((r, i) => (
                  <button
                    key={i}
                    onClick={() => { setSelected(r); setStep('confirm') }}
                    style={{
                      display: 'flex', alignItems: 'center', gap: 12, padding: '10px 12px',
                      background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line)',
                      borderRadius: 9, cursor: 'pointer', textAlign: 'left',
                    }}
                  >
                    {r.poster_url && (
                      <img src={r.poster_url} alt="" style={{ width: 36, height: 54, objectFit: 'cover', borderRadius: 4, flexShrink: 0 }} />
                    )}
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <div style={{ fontSize: 12, fontWeight: 600, color: 'var(--hds-txt)', whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>
                        {r.title}
                        {r.year > 0 && <span style={{ marginLeft: 6, fontWeight: 400, color: 'var(--hds-txt-3)' }}>({r.year})</span>}
                      </div>
                      {r.already_added && (
                        <div style={{ fontSize: 10, color: 'oklch(0.72 0.18 140)', marginTop: 3 }}>Already in {arrName}</div>
                      )}
                    </div>
                    <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', flexShrink: 0 }}>Select →</span>
                  </button>
                ))}
              </div>
            </>
          )}

          {/* Step: confirm */}
          {step === 'confirm' && selected && (
            <>
              <div style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '10px 12px', background: 'var(--hds-bg-3)', borderRadius: 9, border: '1px solid var(--hds-line)' }}>
                {selected.poster_url && (
                  <img src={selected.poster_url} alt="" style={{ width: 36, height: 54, objectFit: 'cover', borderRadius: 4, flexShrink: 0 }} />
                )}
                <div>
                  <div style={{ fontSize: 13, fontWeight: 600, color: 'var(--hds-txt)' }}>{selected.title}</div>
                  {selected.year > 0 && <div style={{ fontSize: 11, color: 'var(--hds-txt-3)', marginTop: 2 }}>{selected.year}</div>}
                  {selected.already_added && <div style={{ fontSize: 10, color: 'oklch(0.72 0.18 140)', marginTop: 4 }}>Already in {arrName}</div>}
                </div>
              </div>

              {!selected.already_added && (
                <>
                  {profiles.length > 0 && (
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                      <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>QUALITY PROFILE</label>
                      <select value={qualityId} onChange={e => setQualityId(Number(e.target.value))} style={selectStyle}>
                        {profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                      </select>
                    </div>
                  )}
                  {rootFolders.length > 0 && (
                    <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                      <label style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.06em' }}>ROOT FOLDER</label>
                      <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} style={selectStyle}>
                        {rootFolders.map(f => <option key={f} value={f}>{f}</option>)}
                      </select>
                    </div>
                  )}
                </>
              )}
            </>
          )}
        </div>

        {/* Footer */}
        {step !== 'done' && (
          <div style={{ padding: '0 20px 20px', display: 'flex', gap: 8, justifyContent: 'flex-end' }}>
            {step === 'confirm' && (
              <button style={btnGhost} onClick={() => setStep('lookup')}>← Back</button>
            )}
            <button style={btnGhost} onClick={onClose}>Cancel</button>
            {step === 'confirm' && selected && !selected.already_added && (
              <button style={btnPrimary} disabled={adding} onClick={confirmAdd}>
                {adding ? 'Adding…' : `Add to ${arrName}`}
              </button>
            )}
            {step === 'confirm' && selected?.already_added && (
              <button style={btnGhost} onClick={onClose}>Close</button>
            )}
          </div>
        )}
        {step === 'done' && (
          <div style={{ padding: '0 20px 20px', display: 'flex', justifyContent: 'flex-end' }}>
            <button style={btnPrimary} onClick={onClose}>Done</button>
          </div>
        )}
      </div>
    </div>
  )
}
