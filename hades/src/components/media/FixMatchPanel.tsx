import { useEffect, useRef, useState } from 'react'
import { api, mediaUrl } from '../../api/client'
import type { ItemMetadata, ScraperSearchResult } from '../../api/types'
import { folderBaseName } from './useMediaDetail'
import { ExternalIdEditor } from './LibraryAdminPanel'

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
  onMatched:    () => void
  onCancel:     () => void
}

// Condensed version of ReviewPage's CandidatePanel search flow — same
// scraperSearch/manualMatch API calls, scoped down to the detail view's own
// column width (maxWidth 900 in LibraryDetailActions) rather than a full
// page. Runs an initial search against the item's own title immediately on
// open, since the point of opening this is "the current match is wrong,"
// not "start from a blank search."
export function FixMatchPanel({ id, contentType, defaultQuery, locked, folderPath, onMatched, onCancel }: FixMatchPanelProps) {
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

  if (mergedNotice) {
    return (
      <div style={{
        display: 'flex', flexDirection: 'column', gap: 12, padding: 12,
        borderRadius: 8, background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)',
      }}>
        <div style={{
          padding: '10px 12px', borderRadius: 6, fontSize: 11, lineHeight: 1.5,
          fontFamily: "'JetBrains Mono', monospace", color: 'var(--hds-violet)',
          background: 'oklch(0.55 0.14 292 / 0.1)', border: '1px solid oklch(0.7 0.13 287 / 0.4)',
        }}>
          This was a duplicate of <strong>{mergedNotice.title}</strong> and has been merged into it —
          reopen it from the library to continue editing.
        </div>
        <button
          onClick={onCancel}
          style={{
            padding: '9px 0', borderRadius: 6, cursor: 'pointer',
            border: '1px solid var(--hds-line)', background: 'transparent',
            color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
          }}
        >Close</button>
      </div>
    )
  }

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', gap: 10, padding: 12,
      borderRadius: 8, background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)',
    }}>
      {folderPath && (
        <div
          title={folderPath}
          style={{
            padding: '6px 10px', borderRadius: 6, fontSize: 10,
            fontFamily: "'JetBrains Mono', monospace", color: 'var(--hds-txt-2)',
            background: 'var(--hds-bg-2)', border: '1px solid var(--hds-line-s)',
            overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
          }}
        >
          <span style={{ color: 'var(--hds-txt-3)' }}>Folder: </span>{folderBaseName(folderPath)}
        </div>
      )}

      {locked && (
        <div style={{
          padding: '8px 10px', borderRadius: 6, fontSize: 10, lineHeight: 1.5,
          fontFamily: "'JetBrains Mono', monospace", color: 'var(--hds-violet)',
          background: 'oklch(0.55 0.14 292 / 0.1)', border: '1px solid oklch(0.7 0.13 287 / 0.4)',
        }}>
          This item is locked — matching updates the link, but locked fields
          won't be overwritten. Unlock it in Library to update everything.
        </div>
      )}

      <div>
        <span style={{
          display: 'block', fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
          letterSpacing: '0.08em', color: 'var(--hds-txt-3)', marginBottom: 5,
        }}>Linked Ids</span>
        {loadingMeta ? (
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>Loading…</div>
        ) : (
          <ExternalIdEditor type={contentType} id={id} metadata={metadata} isAdmin onChanged={setMetadata} />
        )}
      </div>

      <div style={{ display: 'flex', gap: 8 }}>
        <input
          autoFocus
          value={query}
          onChange={e => setQuery(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && runSearch(query)}
          placeholder="Search title…"
          style={{
            flex: 1, padding: '9px 12px', borderRadius: 6,
            border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
            color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
            outline: 'none',
          }}
        />
        <button
          onClick={() => runSearch(query)}
          disabled={loading || !query.trim()}
          style={{
            padding: '9px 16px', borderRadius: 6, cursor: 'pointer',
            border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
            color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            opacity: (loading || !query.trim()) ? 0.5 : 1,
          }}
        >{loading ? '…' : 'Search'}</button>
      </div>

      {error && (
        <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)' }}>{error}</div>
      )}

      <div style={{ display: 'flex', flexDirection: 'column', gap: 8, maxHeight: 440, overflowY: 'auto', flexShrink: 0 }} className="scrollbar-dark">
        {loading ? (
          Array.from({ length: 3 }, (_, i) => (
            <div key={i} className="hds-skeleton" style={{ height: 120, borderRadius: 7, flexShrink: 0 }} />
          ))
        ) : results.length === 0 ? (
          <div style={{
            padding: '16px 0', textAlign: 'center',
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
          }}>No results.</div>
        ) : (
          results.map(r => {
            const key = `${r.source}:${r.external_id}`
            const isMatching = matchingKey === key
            const srcColor = r.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : r.source === 'tvdb' ? 'oklch(0.65 0.12 280)' : 'var(--hds-gold)'
            return (
              <div key={key} style={{
                display: 'flex', gap: 0, borderRadius: 7, flexShrink: 0,
                border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
                overflow: 'hidden', opacity: isMatching ? 0.6 : 1,
              }}>
                {r.poster_url && (
                  <img src={mediaUrl(r.poster_url)} alt=""
                    style={{ width: 80, height: 120, objectFit: 'cover', flexShrink: 0 }}
                    onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                  />
                )}
                <div style={{ flex: 1, minWidth: 0, padding: '10px 14px', display: 'flex', flexDirection: 'column', gap: 6 }}>
                  <div style={{ display: 'flex', alignItems: 'flex-start', gap: 6 }}>
                    <span style={{
                      fontFamily: "'Chakra Petch', sans-serif", fontSize: 12.5, fontWeight: 600,
                      color: 'var(--hds-txt)', flex: 1, lineHeight: 1.3,
                    }}>{r.title}</span>
                    <button
                      onClick={() => handleMatch(r)}
                      disabled={isMatching}
                      title="Set as the primary match — other linked ids stay put, just reordered below it"
                      style={{
                        flexShrink: 0, padding: '7px 16px', borderRadius: 6, cursor: isMatching ? 'not-allowed' : 'pointer',
                        border: '1px solid var(--hds-match-green)', background: 'oklch(0.7 0.16 150 / 0.1)',
                        color: 'var(--hds-match-green)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, fontWeight: 600,
                      }}
                    >{isMatching ? '…' : justMatchedKey === key ? '✓ Primary' : 'Use'}</button>
                  </div>
                  <div style={{ display: 'flex', gap: 5, alignItems: 'center' }}>
                    {r.year && <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)' }}>{r.year}</span>}
                    <span style={{
                      fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: srcColor,
                      border: `1px solid ${srcColor}`, borderRadius: 4, padding: '1px 5px', letterSpacing: '0.05em',
                    }}>{r.source.toUpperCase()}</span>
                    {r.in_library && (
                      <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: 'oklch(0.7 0.16 150)' }}>in library</span>
                    )}
                  </div>
                  {r.overview && (
                    <p style={{
                      margin: 0, fontSize: 10.5, lineHeight: 1.4, color: 'var(--hds-txt-2)',
                      display: '-webkit-box', WebkitLineClamp: 3, WebkitBoxOrient: 'vertical', overflow: 'hidden',
                    }}>{r.overview}</p>
                  )}
                </div>
              </div>
            )
          })
        )}
      </div>

      <button
        onClick={onCancel}
        style={{
          padding: '9px 0', borderRadius: 6, cursor: 'pointer',
          border: '1px solid var(--hds-line)', background: 'transparent',
          color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        }}
      >Close</button>
    </div>
  )
}
