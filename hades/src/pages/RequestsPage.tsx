import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { ArrLookupResult, ArrServiceOptions, ContentRequest, RequestStatus } from '../api/types'
import styles from './RequestsPage.module.css'

const STATUS_TEXT_CLASS: Record<RequestStatus, string> = {
  pending:  styles.statusPending,
  approved: styles.statusApproved,
  rejected: styles.statusRejected,
}
const STATUS_PILL_CLASS: Record<RequestStatus, string> = {
  pending:  styles.statusPillPending,
  approved: styles.statusPillApproved,
  rejected: styles.statusPillRejected,
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
    <div className={styles.pageRoot}>
      {/* Left panel */}
      <div className={styles.leftPanel}>
        <div className={styles.leftPanelHeader}>
          <div className={styles.leftPanelTitle}>
            Requests
            {pending > 0 && (
              <span className={styles.pendingBadge}>{pending}</span>
            )}
          </div>
          <div className={styles.filterRow}>
            {(['pending', 'all', 'approved', 'rejected'] as const).map(f => (
              <button key={f} onClick={() => setFilter(f)}
                className={`${styles.filterBtn} ${filter === f ? styles.filterBtnActive : styles.filterBtnInactive}`}>
                {f === 'all' ? 'All' : f}
                {f !== 'all' && (
                  <span className={styles.filterCount}>
                    {requests.filter(r => r.status === f).length}
                  </span>
                )}
              </button>
            ))}
          </div>
        </div>

        <div className={styles.listScroll}>
          {loading ? (
            <div className={styles.skeletonList}>
              {[...Array(5)].map((_, i) => (
                <div key={i} className={`hds-skeleton ${styles.skeletonRow}`} />
              ))}
            </div>
          ) : visible.length === 0 ? (
            <div className={styles.emptyState}>
              {filter === 'pending' ? 'No pending requests' : `No ${filter} requests`}
            </div>
          ) : (
            <div className={styles.listPad}>
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
        <div className={styles.selectPrompt}>
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
  const sourceClass = r.source === 'tmdb' ? styles.rowSourceTmdb : styles.rowSourceTvdb

  return (
    <div
      onClick={onClick}
      className={`${styles.row} ${selected ? styles.rowSelected : styles.rowUnselected}`}
    >
      {/* Poster */}
      <div className={styles.rowPoster}>
        {r.poster_url && (
          <img src={r.poster_url} alt="" className={styles.rowPosterImg}
            onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
        )}
      </div>

      <div className={styles.rowInfo}>
        <div className={styles.rowTitle}>{r.title}</div>
        <div className={styles.rowMetaRow}>
          {r.year && <span className={styles.rowYear}>{r.year}</span>}
          <span className={`${styles.rowSourceBadge} ${sourceClass}`}>{r.source.toUpperCase()}</span>
          <span className={styles.rowContentType}>{r.content_type}</span>
        </div>
      </div>

      {/* Status dot */}
      <div className={styles.rowStatusCol}>
        <span className={`${styles.rowStatusText} ${STATUS_TEXT_CLASS[r.status]}`}>{r.status.toUpperCase()}</span>
        <span className={styles.rowDate}>
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
  const sourceChipClass = r.source === 'tmdb' ? styles.metaChipTmdb : styles.metaChipTvdb

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
    } catch (e: any) {
      setArrError(e.message ?? `Could not reach ${serviceLabel}.`)
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
    } catch (e: any) {
      setArrError(e.message ?? `Failed to add to ${serviceLabel}.`)
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
    <div className={styles.detailRoot}>
      {/* Backdrop */}
      <div className={styles.backdrop}>
        <div className={styles.backdropImg} style={{
          background: r.poster_url
            ? `url(${r.poster_url}) center 20%/cover no-repeat`
            : 'linear-gradient(135deg, oklch(0.12 0.04 292), oklch(0.16 0.03 280))',
        }} />
        <div className={styles.backdropScrim} />
        <button onClick={onClose} className={styles.backdropCloseBtn}>×</button>
        {r.poster_url && (
          <img src={r.poster_url} alt="" className={styles.backdropPoster} />
        )}
      </div>

      <div className={`${styles.detailBody} ${r.poster_url ? styles.detailBodyWithPoster : styles.detailBodyNoPoster}`}>
        <div className={styles.titleRow}>
          <h2 className={styles.titleText}>
            {r.title}
          </h2>
          <span className={`${styles.statusPill} ${STATUS_PILL_CLASS[r.status]}`}>{r.status.toUpperCase()}</span>
        </div>

        <div className={styles.metaChipsRow}>
          {r.year && <span className={styles.metaChip}>{r.year}</span>}
          <span className={styles.metaChip}>{r.content_type}</span>
          <span className={`${styles.metaChip} ${sourceChipClass}`}>{r.source.toUpperCase()}</span>
        </div>

        <div className={styles.requestedMeta}>
          Requested {new Date(r.created_at * 1000).toLocaleString()} · user {r.user_id.slice(0, 8)}
        </div>

        {/* Actions */}
        {r.status === 'pending' && (
          <div className={styles.actionsSection}>
            <div className={styles.actionsLabel}>
              {serviceLabel.toUpperCase()}
            </div>

            {arrStep === 'idle' && (
              <div className={styles.idleActionsRow}>
                <button onClick={handleApproveClick} className={`${styles.approveAccentColors} ${styles.approveBtn}`}>Approve → {serviceLabel}</button>
                <button onClick={handleReject} disabled={rejecting}
                  className={`${styles.rejectBtn} ${rejecting ? styles.rejectBtnDisabled : styles.rejectBtnEnabled}`}>Reject</button>
              </div>
            )}

            {(arrStep === 'loading' || arrStep === 'adding') && (
              <div className={styles.loadingText}>
                {arrStep === 'loading' ? `Looking up in ${serviceLabel}…` : `Adding to ${serviceLabel}…`}
              </div>
            )}

            {arrStep === 'form' && options && (
              <div className={styles.formCol}>
                <label className={styles.formLabel}>
                  Quality Profile
                  <select value={qualityProfileId ?? ''} onChange={e => setQualityProfileId(Number(e.target.value))} className={styles.selectInput}>
                    {options.quality_profiles.map(p => <option key={p.id} value={p.id}>{p.name}</option>)}
                  </select>
                </label>
                <label className={styles.formLabel}>
                  Root Folder
                  <select value={rootFolder} onChange={e => setRootFolder(e.target.value)} className={styles.selectInput}>
                    {options.root_folders.map(f => <option key={f} value={f}>{f}</option>)}
                  </select>
                </label>
                <label className={styles.checkboxLabel}>
                  <input type="checkbox" checked={searchOnAdd} onChange={e => setSearchOnAdd(e.target.checked)} className={styles.checkboxInput} />
                  Search immediately
                </label>
                <div className={styles.formActionsRow}>
                  <button onClick={() => setArrStep('idle')} className={styles.cancelBtn}>Cancel</button>
                  <button onClick={handleConfirmApprove} className={`${styles.approveAccentColors} ${styles.confirmBtn}`}>
                    Confirm → {serviceLabel}
                  </button>
                </div>
              </div>
            )}

            {arrStep === 'error' && (
              <div className={styles.errorBox}>
                <div className={styles.errorMsg}>{arrError}</div>
                <button onClick={() => setArrStep('idle')} className={styles.cancelBtn}>Try Again</button>
              </div>
            )}
          </div>
        )}

        {arrStep === 'done' && (
          <div className={`${styles.statusBanner} ${styles.statusBannerApproved}`}>
            Approved — added to {serviceLabel}{searchOnAdd ? ', search queued' : ''}
          </div>
        )}

        {r.status === 'approved' && arrStep !== 'done' && (
          <div className={`${styles.statusBanner} ${styles.statusBannerApproved}`}>
            Approved
          </div>
        )}

        {r.status === 'rejected' && (
          <div className={`${styles.statusBanner} ${styles.statusBannerRejected}`}>
            Rejected
          </div>
        )}
      </div>
    </div>
  )
}
