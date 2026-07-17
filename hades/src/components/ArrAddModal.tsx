import { useEffect, useState } from 'react'
import { api, mediaUrl } from '../api/client'
import type { ArrLookupResult, ArrQualityProfile } from '../api/types'
import styles from './ArrAddModal.module.css'

interface Props {
  type:     'show' | 'movie'
  title:    string
  tvdb_id?: string
  tmdb_id?: string
  imdb_id?: string
  onClose:  () => void
  onAdded:  () => void
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
    <div className={styles.overlay} onClick={e => { if (e.target === e.currentTarget) onClose() }}>
      <div className={styles.card}>
        {/* Header */}
        <div className={styles.header}>
          <div>
            <div className={styles.headerTitle}>
              Add to {arrName}
            </div>
            <div className={styles.headerSubtitle}>{title}</div>
          </div>
          <button onClick={onClose} className={styles.closeBtn}>✕</button>
        </div>

        <div className={styles.body}>

          {/* Loading */}
          {loading && (
            <div className={styles.centeredNote}>
              Searching {arrName}…
            </div>
          )}

          {/* Error */}
          {error && (
            <div className={styles.errorBanner}>
              {error}
            </div>
          )}

          {/* Done */}
          {step === 'done' && (
            <div className={styles.doneBlock}>
              <div className={styles.doneTitle}>Added successfully</div>
              <div className={styles.doneSubtitle}>{selected?.title} sent to {arrName}.</div>
            </div>
          )}

          {/* Step: lookup results */}
          {!loading && step === 'lookup' && results.length === 0 && !error && (
            <div className={styles.centeredNote}>
              No results found in {arrName}.
            </div>
          )}

          {!loading && step === 'lookup' && results.length > 0 && (
            <>
              <div className={styles.sectionLabel}>SELECT A MATCH</div>
              <div className={styles.resultsList}>
                {results.map((r, i) => (
                  <button
                    key={i}
                    onClick={() => { setSelected(r); setStep('confirm') }}
                    className={styles.resultBtn}
                  >
                    {r.poster_url && (
                      <img src={mediaUrl(r.poster_url)} alt="" className={styles.resultPoster} />
                    )}
                    <div className={styles.resultInfo}>
                      <div className={styles.resultTitle}>
                        {r.title}
                        {r.year > 0 && <span className={styles.resultYear}>({r.year})</span>}
                      </div>
                      {r.already_added && (
                        <div className={styles.resultAlready}>Already in {arrName}</div>
                      )}
                    </div>
                    <span className={styles.resultSelect}>Select →</span>
                  </button>
                ))}
              </div>
            </>
          )}

          {/* Step: confirm */}
          {step === 'confirm' && selected && (
            <>
              <div className={styles.confirmCard}>
                {selected.poster_url && (
                  <img src={mediaUrl(selected.poster_url)} alt="" className={styles.resultPoster} />
                )}
                <div>
                  <div className={styles.confirmTitle}>{selected.title}</div>
                  {selected.year > 0 && <div className={styles.confirmYear}>{selected.year}</div>}
                  {selected.already_added && <div className={styles.confirmAlready}>Already in {arrName}</div>}
                </div>
              </div>

              {!selected.already_added && (
                <>
                  {profiles.length > 0 && (
                    <div className={styles.fieldGroup}>
                      <label className={styles.fieldLabel}>QUALITY PROFILE</label>
                      <select value={qualityId} onChange={e => setQualityId(Number(e.target.value))} className={styles.select}>
                        {profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                      </select>
                    </div>
                  )}
                  {rootFolders.length > 0 && (
                    <div className={styles.fieldGroup}>
                      <label className={styles.fieldLabel}>ROOT FOLDER</label>
                      <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} className={styles.select}>
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
          <div className={styles.footer}>
            {step === 'confirm' && (
              <button className={styles.btnGhost} onClick={() => setStep('lookup')}>← Back</button>
            )}
            <button className={styles.btnGhost} onClick={onClose}>Cancel</button>
            {step === 'confirm' && selected && !selected.already_added && (
              <button className={styles.btnPrimary} disabled={adding} onClick={confirmAdd}>
                {adding ? 'Adding…' : `Add to ${arrName}`}
              </button>
            )}
            {step === 'confirm' && selected?.already_added && (
              <button className={styles.btnGhost} onClick={onClose}>Close</button>
            )}
          </div>
        )}
        {step === 'done' && (
          <div className={styles.footerEnd}>
            <button className={styles.btnPrimary} onClick={onClose}>Done</button>
          </div>
        )}
      </div>
    </div>
  )
}
