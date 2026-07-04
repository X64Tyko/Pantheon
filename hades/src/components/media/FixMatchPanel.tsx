import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import type { ScraperSearchResult } from '../../api/types'

interface FixMatchPanelProps {
  id:           string
  contentType:  'show' | 'movie'
  defaultQuery: string
  // acceptMatch's per-field UPDATE (ScraperManager.cpp) skips locked fields —
  // matching still updates match_status/score, but visible metadata (title,
  // poster, etc.) won't change unless the item is unlocked first. Surfaced
  // here rather than silently producing a confusing "nothing happened" result.
  locked:       boolean
  onMatched:    () => void
  onCancel:     () => void
}

// Condensed version of ReviewPage's CandidatePanel search flow — same
// scraperSearch/manualMatch API calls, scoped down to the detail view's
// narrower column (maxWidth 420 in LibraryDetailActions) rather than a full
// page. Runs an initial search against the item's own title immediately on
// open, since the point of opening this is "the current match is wrong,"
// not "start from a blank search."
export function FixMatchPanel({ id, contentType, defaultQuery, locked, onMatched, onCancel }: FixMatchPanelProps) {
  const [query,      setQuery]      = useState(defaultQuery)
  const [results,    setResults]    = useState<ScraperSearchResult[]>([])
  const [loading,    setLoading]    = useState(true)
  const [matchingKey, setMatchingKey] = useState<string | null>(null)
  const [error,      setError]      = useState<string | null>(null)

  const runSearch = async (q: string) => {
    if (!q.trim()) return
    setLoading(true)
    setError(null)
    try {
      const r = await api.scraperSearch(q.trim(), contentType)
      setResults(r.items)
    } catch {
      setError('Search failed.')
    }
    setLoading(false)
  }

  useEffect(() => {
    runSearch(defaultQuery)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  const handleMatch = async (result: ScraperSearchResult) => {
    const key = `${result.source}:${result.external_id}`
    setMatchingKey(key)
    setError(null)
    try {
      await api.manualMatch(id, {
        item_type:   contentType,
        source:      result.source,
        external_id: result.external_id,
        title:       result.title,
        year:        result.year,
        poster_url:  result.poster_url,
        overview:    result.overview,
      })
      onMatched()
    } catch {
      setError('Failed to apply match.')
      setMatchingKey(null)
    }
  }

  return (
    <div style={{
      display: 'flex', flexDirection: 'column', gap: 10, padding: 12,
      borderRadius: 8, background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)',
    }}>
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

      <div style={{ display: 'flex', gap: 8 }}>
        <input
          autoFocus
          value={query}
          onChange={e => setQuery(e.target.value)}
          onKeyDown={e => e.key === 'Enter' && runSearch(query)}
          placeholder="Search title…"
          style={{
            flex: 1, padding: '6px 10px', borderRadius: 6,
            border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
            color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
            outline: 'none',
          }}
        />
        <button
          onClick={() => runSearch(query)}
          disabled={loading || !query.trim()}
          style={{
            padding: '6px 12px', borderRadius: 6, cursor: 'pointer',
            border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
            color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            opacity: (loading || !query.trim()) ? 0.5 : 1,
          }}
        >{loading ? '…' : 'Search'}</button>
      </div>

      {error && (
        <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)' }}>{error}</div>
      )}

      <div style={{ display: 'flex', flexDirection: 'column', gap: 8, maxHeight: 320, overflowY: 'auto' }} className="scrollbar-dark">
        {loading ? (
          Array.from({ length: 3 }, (_, i) => (
            <div key={i} className="hds-skeleton" style={{ height: 96, borderRadius: 7 }} />
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
                display: 'flex', gap: 0, borderRadius: 7,
                border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
                overflow: 'hidden', opacity: isMatching ? 0.6 : 1,
              }}>
                {r.poster_url && (
                  <img src={r.poster_url} alt=""
                    style={{ width: 64, height: 96, objectFit: 'cover', flexShrink: 0 }}
                    onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
                  />
                )}
                <div style={{ flex: 1, minWidth: 0, padding: '8px 10px', display: 'flex', flexDirection: 'column', gap: 4 }}>
                  <div style={{ display: 'flex', alignItems: 'flex-start', gap: 6 }}>
                    <span style={{
                      fontFamily: "'Chakra Petch', sans-serif", fontSize: 11.5, fontWeight: 600,
                      color: 'var(--hds-txt)', flex: 1, lineHeight: 1.3,
                    }}>{r.title}</span>
                    <button
                      onClick={() => handleMatch(r)}
                      disabled={isMatching}
                      style={{
                        flexShrink: 0, padding: '3px 9px', borderRadius: 5, cursor: isMatching ? 'not-allowed' : 'pointer',
                        border: '1px solid var(--hds-match-green)', background: 'oklch(0.7 0.16 150 / 0.1)',
                        color: 'var(--hds-match-green)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9, fontWeight: 600,
                      }}
                    >{isMatching ? '…' : 'Use'}</button>
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
          padding: '6px 0', borderRadius: 6, cursor: 'pointer',
          border: '1px solid var(--hds-line)', background: 'transparent',
          color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        }}
      >Close</button>
    </div>
  )
}
