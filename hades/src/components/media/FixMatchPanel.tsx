import { useEffect, useRef, useState } from 'react'
import { api, mediaUrl } from '../../api/client'
import type { ItemMetadata, ScraperSearchResult } from '../../api/types'
import { folderBaseName } from './useMediaDetail'
import { ExternalIdEditor } from './LibraryAdminPanel'
import type { MatchStatus } from './MatchBadge'
import styles from './FixMatchPanel.module.css'

interface FixMatchPanelProps {
  id:           string
  contentType:  'show' | 'movie'
  defaultQuery: string
  // acceptMatch's per-field UPDATE (ScraperManager.cpp) skips locked fields —
  // matching still updates match_status/score, but visible metadata (title,
  // poster, etc.) won't change unless the item is unlocked first. Surfaced
  // here rather than silently producing a confusing "nothing happened" result.
  locked:       boolean
  // The on-disk folder this item actually lives in (movie's own folder, or a
  // show's common episode-ancestor folder) — the fastest way to tell whether
  // a match is actually wrong, since a mismatched folder name usually gives
  // it away immediately. Undefined if not yet known (no episodes/file yet).
  folderPath?:  string
  // When the item is already 'matched' but not match_confirmed, it was
  // auto-matched and never reviewed by a human — surfaces a one-click
  // "Confirm Match" instead of forcing a re-search + re-pick of the same
  // result just to clear match_confirmed.
  matchStatus:     MatchStatus
  matchConfirmed:  boolean
  onMatched:    () => void
  onCancel:     () => void
}

// Condensed version of ReviewPage's CandidatePanel search flow — same
// scraperSearch/manualMatch API calls, scoped down to the detail view's own
// column width (maxWidth 900 in LibraryDetailActions) rather than a full
// page. Runs an initial search against the item's own title immediately on
// open, since the point of opening this is "the current match is wrong,"
// not "start from a blank search."
export function FixMatchPanel({ id, contentType, defaultQuery, locked, folderPath, matchStatus, matchConfirmed, onMatched, onCancel }: FixMatchPanelProps) {
  const [query,      setQuery]      = useState(defaultQuery)
  const [results,    setResults]    = useState<ScraperSearchResult[]>([])
  const [loading,    setLoading]    = useState(true)
  const [matchingKey, setMatchingKey] = useState<string | null>(null)
  const [error,      setError]      = useState<string | null>(null)

  const [metadata,    setMetadata]    = useState<ItemMetadata | null>(null)
  const [loadingMeta, setLoadingMeta] = useState(true)

  const loadMetadata = async () => {
    setLoadingMeta(true)
    try { setMetadata(await api.getItemMetadata(contentType, id)) } catch {}
    setLoadingMeta(false)
  }

  // Guards against an earlier, slower search response (AniDB is rate-limited
  // to ~1 req/2s server-side, so an in-flight search can easily still be
  // running when a newer one is fired) landing after a more recent search
  // and clobbering its results.
  const searchSeq = useRef(0)
  const runSearch = async (q: string) => {
    if (!q.trim()) return
    const seq = ++searchSeq.current
    setLoading(true)
    setError(null)
    try {
      const r = await api.scraperSearch(q.trim(), contentType)
      if (seq === searchSeq.current) setResults(r.items)
    } catch {
      if (seq === searchSeq.current) setError('Search failed.')
    }
    if (seq === searchSeq.current) setLoading(false)
  }

  useEffect(() => {
    runSearch(defaultQuery)
    loadMetadata()
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const [justMatchedKey, setJustMatchedKey] = useState<string | null>(null)
  // Set when this item turns out to duplicate an already-matched item — it's
  // been merged away and deleted server-side, so there's no detail left here
  // to reload. Shown instead of the results list; onMatched()/refetching the
  // (now-gone) id is deliberately skipped in this case.
  const [mergedNotice, setMergedNotice] = useState<{ title: string } | null>(null)

  const handleMatch = async (result: ScraperSearchResult) => {
    const key = `${result.source}:${result.external_id}`
    setMatchingKey(key)
    setJustMatchedKey(null)
    setError(null)
    try {
      // manualMatch() promotes this to the primary (priority 1) source
      // without deleting any other ids already linked — see linkExternalId()
      // in ScraperManager.cpp. The panel stays open (onMatched no longer
      // closes it — see LibraryDetailActions), so reload the list below to
      // reflect the new order right away rather than only on next open.
      const r = await api.manualMatch(id, {
        item_type:   contentType,
        source:      result.source,
        external_id: result.external_id,
        title:       result.title,
        year:        result.year,
        poster_url:  result.poster_url,
        overview:    result.overview,
      })
      if (r.merged_into) {
        setMatchingKey(null)
        setMergedNotice({ title: r.merged_into.title })
        return
      }
      await loadMetadata()
      setJustMatchedKey(key)
      onMatched()
    } catch {
      setError('Failed to apply match.')
    }
    setMatchingKey(null)
  }

  const [confirming,   setConfirming]   = useState(false)
  const [confirmError, setConfirmError] = useState<string | null>(null)

  const handleConfirm = async () => {
    setConfirming(true)
    setConfirmError(null)
    try {
      await api.confirmMatch(id, contentType)
      await loadMetadata()
      onMatched()
    } catch {
      setConfirmError('Failed to confirm match.')
    }
    setConfirming(false)
  }

  if (mergedNotice) {
    return (
      <div className={`${styles.panel} ${styles.panelMerged}`}>
        <div className={styles.violetNotice}>
          This was a duplicate of <strong>{mergedNotice.title}</strong> and has been merged into it —
          reopen it from the library to continue editing.
        </div>
        <button onClick={onCancel} className={styles.plainButton}>Close</button>
      </div>
    )
  }

  return (
    <div className={styles.panel}>
      {folderPath && (
        <div title={folderPath} className={styles.folderTag}>
          <span className={styles.folderTagLabel}>Folder: </span>{folderBaseName(folderPath)}
        </div>
      )}

      {locked && (
        <div className={`${styles.violetNotice} ${styles.violetNoticeCompact}`}>
          This item is locked — matching updates the link, but locked fields
          won't be overwritten. Unlock it in Library to update everything.
        </div>
      )}

      {matchStatus === 'matched' && !matchConfirmed && (
        <div className={styles.confirmBox}>
          <div className={styles.confirmBoxText}>
            This was matched automatically and hasn't been reviewed yet — confirm
            it to enable Push to Sources, and pull the latest metadata now.
          </div>
          <button
            onClick={handleConfirm}
            disabled={confirming}
            className={`${styles.confirmButton} ${confirming ? styles.confirmButtonBusy : ''}`}
          >{confirming ? 'Confirming…' : '✓ Confirm Match'}</button>
          {confirmError && (
            <div className={styles.confirmErrorText}>{confirmError}</div>
          )}
        </div>
      )}

      <div>
        <span className={styles.linkedIdsLabel}>Linked Ids</span>
        {loadingMeta ? (
          <div className={styles.loadingText}>Loading…</div>
        ) : (
          <ExternalIdEditor type={contentType} id={id} metadata={metadata} isAdmin onChanged={setMetadata} />
        )}
      </div>

      <div className={styles.searchRow}>
        <input
          autoFocus
          value={query}
          onChange={e => setQuery(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && runSearch(query)}
          placeholder="Search title…"
          className={styles.searchInput}
        />
        <button
          onClick={() => runSearch(query)}
          disabled={loading || !query.trim()}
          className={`${styles.searchButton} ${(loading || !query.trim()) ? styles.searchButtonDisabled : ''}`}
        >{loading ? '…' : 'Search'}</button>
      </div>

      {error && (
        <div className={styles.errorText}>{error}</div>
      )}

      <div className={`${styles.resultsList} scrollbar-dark`}>
        {loading ? (
          Array.from({ length: 3 }, (_, i) => (
            <div key={i} className={`hds-skeleton ${styles.resultSkeleton}`} />
          ))
        ) : results.length === 0 ? (
          <div className={styles.noResults}>No results.</div>
        ) : (
          results.map(r => {
            const key = `${r.source}:${r.external_id}`
            const isMatching = matchingKey === key
            const srcClass = r.source === 'tmdb' ? styles.resultSourceTagTmdb : r.source === 'tvdb' ? styles.resultSourceTagTvdb : styles.resultSourceTagGold
            return (
              <div key={key} className={`${styles.resultCard} ${isMatching ? styles.resultCardMatching : ''}`}>
                {r.poster_url && (
                  <img src={mediaUrl(r.poster_url)} alt=""
                    className={styles.resultPoster}
                    onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                  />
                )}
                <div className={styles.resultBody}>
                  <div className={styles.resultTitleRow}>
                    <span className={styles.resultTitle}>{r.title}</span>
                    <button
                      onClick={() => handleMatch(r)}
                      disabled={isMatching}
                      title="Set as the primary match — other linked ids stay put, just reordered below it"
                      className={`${styles.useButton} ${isMatching ? styles.useButtonDisabled : ''}`}
                    >{isMatching ? '…' : justMatchedKey === key ? '✓ Primary' : 'Use'}</button>
                  </div>
                  <div className={styles.resultMetaRow}>
                    {r.year && <span className={styles.resultYear}>{r.year}</span>}
                    <span className={`${styles.resultSourceTag} ${srcClass}`}>{r.source.toUpperCase()}</span>
                    {r.in_library && (
                      <span className={styles.resultInLibraryTag}>in library</span>
                    )}
                  </div>
                  {r.overview && (
                    <p className={styles.resultOverview}>{r.overview}</p>
                  )}
                </div>
              </div>
            )
          })
        )}
      </div>

      <button onClick={onCancel} className={styles.plainButton}>Close</button>
    </div>
  )
}
