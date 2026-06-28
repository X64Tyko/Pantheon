import { useEffect, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { libraryStore } from '../stores/LibraryStore'
import { useDebounce } from '../hooks/useDebounce'
import { SourceSwitcher } from '../components/media/SourceSwitcher'
import { LibraryFilters } from '../components/media/LibraryFilters'
import { MediaGrid } from '../components/media/MediaGrid'
import { MediaDetail } from '../components/media/MediaDetail'
import { filterInputStyle } from '../channel/styles'
import type { LibraryDensity, ScraperSearchResult } from '../api/types'

const DENSITY_ICONS: Record<LibraryDensity, string> = { minimal: '⊞', standard: '⊟', rich: '≡' }

export default observer(function LibraryPage() {
  const store = libraryStore
  const [selectedDiscover, setSelectedDiscover] = useState<ScraperSearchResult | null>(null)
  const [rawQ, setRawQ] = useState(store.query)
  const debouncedQ = useDebounce(rawQ, 300)

  useEffect(() => {
    store.loadLibraries().then(() => store.fetch())
  }, [])

  useEffect(() => { store.setQuery(debouncedQ) }, [debouncedQ])

  const handleToggleDiscover = () => {
    store.toggleDiscoverMode()
    setSelectedDiscover(null)
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', overflow: 'hidden', background: 'var(--hds-bg)' }}>
      {/* Top bar */}
      <div style={{
        padding: '14px 24px 10px', borderBottom: '1px solid var(--hds-line)',
        display: 'flex', flexDirection: 'column', gap: 10, flexShrink: 0,
      }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10 }}>
          <input
            style={{ ...filterInputStyle, flex: 1 }}
            placeholder={store.discoverMode ? 'Search scrapers…' : 'Search library…'}
            value={rawQ}
            onChange={e => setRawQ(e.target.value)}
          />

          <button
            onClick={handleToggleDiscover}
            title={store.discoverMode ? 'Switch to Library mode' : 'Switch to Discover mode — search scrapers'}
            style={{
              height: 30, padding: '0 12px', borderRadius: 6, cursor: 'pointer',
              border: `1px solid ${store.discoverMode ? 'oklch(0.65 0.18 220)' : 'var(--hds-line)'}`,
              background: store.discoverMode ? 'oklch(0.65 0.18 220 / 0.12)' : 'transparent',
              color: store.discoverMode ? 'oklch(0.65 0.18 220)' : 'var(--hds-txt-3)',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              letterSpacing: '0.08em', whiteSpace: 'nowrap',
              transition: 'border-color .12s, background .12s, color .12s',
            }}
          >◎ Discover</button>

          {!store.discoverMode && (
            <>
              <div style={{ display: 'flex', gap: 2 }}>
                {(['minimal', 'standard', 'rich'] as LibraryDensity[]).map(d => (
                  <button key={d} onClick={() => store.setDensity(d)} title={d} style={{
                    width: 30, height: 30,
                    border: `1px solid ${store.density === d ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
                    background: store.density === d ? 'oklch(0.55 0.14 292 / 0.2)' : 'transparent',
                    color: store.density === d ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
                    borderRadius: 6, cursor: 'pointer', fontSize: 14,
                  }}>{DENSITY_ICONS[d]}</button>
                ))}
              </div>
              <button onClick={() => store.toggleSidebar()} title="Toggle filters" style={{
                width: 30, height: 30,
                border: `1px solid ${store.sidebarOpen ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
                background: store.sidebarOpen ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
                color: store.sidebarOpen ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
                borderRadius: 6, cursor: 'pointer', fontSize: 12,
              }}>⊧</button>
            </>
          )}
        </div>

        {!store.discoverMode && <SourceSwitcher libraries={store.libraries} />}

        {store.discoverMode && (
          <div style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
            color: 'oklch(0.65 0.18 220)', letterSpacing: '0.06em',
          }}>
            Discover mode — results come from TMDB &amp; TVDB, not your library
          </div>
        )}
      </div>

      {/* Content area */}
      <div style={{ flex: 1, display: 'flex', overflow: 'hidden' }}>
        {!store.discoverMode && store.sidebarOpen && <LibraryFilters />}

        <div style={{ flex: 1, overflowY: 'auto' }}>
          {store.discoverMode ? (
            <DiscoverGrid
              results={store.discoverResults}
              loading={store.discoverLoading}
              query={rawQ}
              selectedKey={selectedDiscover ? `${selectedDiscover.source}-${selectedDiscover.external_id}` : null}
              onSelect={r => setSelectedDiscover(
                prev => prev?.external_id === r.external_id && prev?.source === r.source ? null : r
              )}
            />
          ) : store.loading ? (
            <div style={{
              padding: '16px 24px', display: 'grid',
              gridTemplateColumns: 'repeat(auto-fill, minmax(170px, 1fr))', gap: 14,
            }}>
              {Array.from({ length: 24 }, (_, i) => (
                <div key={i} className="hds-skeleton" style={{ aspectRatio: '2/3', borderRadius: 10 }} />
              ))}
            </div>
          ) : (
            <MediaGrid
              shows={store.shows}
              movies={store.movies}
              density={store.density}
              selectedId={store.selectedId}
              onItemClick={(id, type) => store.selectItem(id, type)}
            />
          )}
        </div>

        {store.discoverMode && selectedDiscover && (
          <MediaDetail
            discoverResult={selectedDiscover}
            onClose={() => setSelectedDiscover(null)}
          />
        )}

        {!store.discoverMode && store.selectedId && store.selectedType && (
          <MediaDetail
            id={store.selectedId}
            content_type={store.selectedType}
            onClose={() => store.clearSelection()}
          />
        )}
      </div>
    </div>
  )
})

// ── Discover grid ─────────────────────────────────────────────────────────────

function DiscoverGrid({ results, loading, query, selectedKey, onSelect }: {
  results:     ScraperSearchResult[]
  loading:     boolean
  query:       string
  selectedKey: string | null
  onSelect:    (r: ScraperSearchResult) => void
}) {
  if (loading) {
    return (
      <div style={{
        padding: '16px 24px', display: 'grid',
        gridTemplateColumns: 'repeat(auto-fill, minmax(150px, 1fr))', gap: 14,
      }}>
        {Array.from({ length: 12 }, (_, i) => (
          <div key={i} className="hds-skeleton" style={{ aspectRatio: '2/3', borderRadius: 10 }} />
        ))}
      </div>
    )
  }

  if (!query.trim()) {
    return (
      <div style={{
        padding: '48px 24px', textAlign: 'center',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-3)',
        lineHeight: 1.6,
      }}>
        Type to search TMDB and TVDB
      </div>
    )
  }

  if (results.length === 0) {
    return (
      <div style={{
        padding: '48px 24px', textAlign: 'center',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-3)',
        lineHeight: 1.7,
      }}>
        No results for "{query}"<br />
        <span style={{ fontSize: 10, color: 'oklch(0.45 0.02 285)' }}>
          Make sure TMDB or TVDB API keys are configured and enabled in{' '}
          <a href="/scrapers" style={{ color: 'var(--hds-violet)', textDecoration: 'none' }}>Scrapers</a>.
        </span>
      </div>
    )
  }

  return (
    <div style={{
      padding: '16px 24px', display: 'grid',
      gridTemplateColumns: 'repeat(auto-fill, minmax(150px, 1fr))', gap: 14,
    }}>
      {results.map(r => (
        <DiscoverCard
          key={`${r.source}-${r.external_id}`}
          result={r}
          selected={selectedKey === `${r.source}-${r.external_id}`}
          onClick={() => onSelect(r)}
        />
      ))}
    </div>
  )
}

function DiscoverCard({ result, selected, onClick }: {
  result:   ScraperSearchResult
  selected: boolean
  onClick:  () => void
}) {
  const [hovered, setHovered] = useState(false)
  const srcColor = result.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  return (
    <div
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      style={{
        borderRadius: 10, overflow: 'hidden', cursor: 'pointer',
        border: `1px solid ${selected ? 'var(--hds-violet)' : hovered ? 'var(--hds-line-s)' : 'var(--hds-line)'}`,
        background: selected ? 'oklch(0.55 0.14 292 / 0.08)' : 'var(--hds-bg-2)',
        display: 'flex', flexDirection: 'column', position: 'relative',
        boxShadow: selected ? '0 0 0 1px var(--hds-violet)' : 'none',
        transition: 'border-color .12s, background .12s',
      }}
    >
      {result.in_library && (
        <div style={{
          position: 'absolute', top: 8, right: 8, zIndex: 2,
          background: 'oklch(0.7 0.16 150)', borderRadius: 6,
          fontFamily: "'JetBrains Mono', monospace", fontSize: 8,
          color: '#fff', padding: '2px 6px', letterSpacing: '0.06em',
          boxShadow: '0 2px 8px rgba(0,0,0,0.4)',
        }}>IN LIBRARY</div>
      )}

      {result.poster_url ? (
        <img
          src={result.poster_url}
          alt={result.title}
          style={{ width: '100%', aspectRatio: '2/3', objectFit: 'cover' }}
          onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
        />
      ) : (
        <div style={{
          width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)',
          display: 'flex', alignItems: 'center', justifyContent: 'center',
          fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
        }}>no poster</div>
      )}

      <div style={{ padding: '10px 10px 12px', flex: 1, display: 'flex', flexDirection: 'column', gap: 4 }}>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontSize: 12, fontWeight: 600,
          color: 'var(--hds-txt)', lineHeight: 1.3,
          display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical', overflow: 'hidden',
        }}>{result.title}</div>

        <div style={{ display: 'flex', gap: 5, alignItems: 'center' }}>
          {result.year && (
            <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)' }}>
              {result.year}
            </span>
          )}
          <span style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 8,
            color: srcColor, borderRadius: 4, border: `1px solid ${srcColor}`,
            padding: '1px 5px', letterSpacing: '0.05em',
          }}>{result.source.toUpperCase()}</span>
        </div>
      </div>
    </div>
  )
}
