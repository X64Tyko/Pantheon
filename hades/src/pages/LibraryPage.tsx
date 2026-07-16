import { useEffect, useRef, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { libraryStore } from '../stores/LibraryStore'
import { useDebounce } from '../hooks/useDebounce'
import { getScrollPos, saveScrollPos } from '../hooks/scrollMemory'
import { SourceSwitcher } from '../components/media/SourceSwitcher'
import { LibraryFilters } from '../components/media/LibraryFilters'
import { FilterSection } from '../components/PickerFilters'
import { MediaGrid } from '../components/media/MediaGrid'
import { MediaDetail } from '../components/media/MediaDetail'
import { LoadMoreSentinel } from '../channel/BrowserTiles'
import { filterInputStyle } from '../channel/styles'
import type { LibraryDensity, ScraperSearchResult } from '../api/types'
import { useFocusable } from '../nav/useFocusable'
import { parseFilterSyntax, countClauses } from '../components/media/filterSyntax'

const DENSITY_ICONS: Record<LibraryDensity, string> = { minimal: '⊞', standard: '⊟', rich: '≡' }
const SCROLL_KEY = 'library-grid'

export default observer(function LibraryPage() {
  const store = libraryStore
  const [selectedDiscover, setSelectedDiscover] = useState<ScraperSearchResult | null>(null)
  const [rawQ, setRawQ] = useState(store.query)
  const debouncedQ = useDebounce(rawQ, 300)

  const gridScrollRef  = useRef<HTMLDivElement>(null)
  const savedGridScroll = useRef(0)
  const restoredRef     = useRef(false)
  const [transitioning, setTransitioning] = useState(false)

  const detailOpen = !!(store.selectedId || selectedDiscover)

  useEffect(() => {
    store.loadLibraries().then(() => store.fetch()).then(() => {
      if (restoredRef.current) return
      restoredRef.current = true
      setTimeout(() => gridScrollRef.current?.scrollTo({ top: getScrollPos(SCROLL_KEY) }), 32)
    })
  }, [])

  useEffect(() => { store.setQuery(debouncedQ) }, [debouncedQ])

  const handleToggleDiscover = () => {
    store.toggleDiscoverMode()
    setSelectedDiscover(null)
  }

  const openDetail = (fn: () => void) => {
    savedGridScroll.current = gridScrollRef.current?.scrollTop ?? 0
    setTransitioning(true)
    setTimeout(() => { fn(); setTransitioning(false) }, 200)
  }

  const closeDetail = () => {
    setTransitioning(true)
    setTimeout(() => {
      store.clearSelection()
      setSelectedDiscover(null)
      setTransitioning(false)
      setTimeout(() => gridScrollRef.current?.scrollTo({ top: savedGridScroll.current }), 32)
    }, 200)
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%', overflow: 'hidden', background: 'var(--hds-bg)' }}>
      {/* Top bar — hidden while viewing an item's detail hero */}
      {!detailOpen && (
        <div style={{
          padding: '14px 24px 10px', borderBottom: '1px solid var(--hds-line)',
          display: 'flex', flexDirection: 'column', gap: 10, flexShrink: 0,
        }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 10, flexWrap: 'wrap' }}>
            <div style={{ flex: 1, position: 'relative' }}>
              <input
                style={{ ...filterInputStyle, width: '100%', boxSizing: 'border-box' }}
                placeholder={store.discoverMode ? 'Search scrapers…' : 'Search, or try genre:horror year:>2015 …'}
                value={rawQ}
                onChange={e => setRawQ(e.target.value)}
              />
              {!store.discoverMode && <SearchSyntaxHint text={rawQ} />}
            </div>

            <DiscoverToggleButton discoverMode={store.discoverMode} onClick={handleToggleDiscover} />

            {!store.discoverMode && (
              <>
                <div style={{ display: 'flex', gap: 2 }}>
                  {(['minimal', 'standard', 'rich'] as LibraryDensity[]).map(d => (
                    <DensityButton key={d} density={d} active={store.density === d} onClick={() => store.setDensity(d)} />
                  ))}
                </div>
                <ResultCount loading={store.loading} total={store.total} />
                <FilterToggleButton open={store.sidebarOpen} onClick={() => store.toggleSidebar()} />
                <HideEmptyToggleButton hideEmpty={store.hideEmpty} onClick={() => store.setHideEmpty(!store.hideEmpty)} />
              </>
            )}
          </div>

          {/* Rule builder sits here, above the library pills, as a horizontal
              bar that wraps rules/groups left-to-right instead of stacking
              them in the narrow sidebar (LibraryFilters.tsx) — that sidebar
              used to hold this too, but it grows unbounded as rules/groups
              are added and forced an awkward internal scroll. */}
          {!store.discoverMode && <FilterSection tree={store.filterTree} filteredLibs={store.libraries} layout="horizontal" />}

          {!store.discoverMode && <SourceSwitcher libraries={store.libraries} />}

          {store.error && (
            <div style={{
              padding: '8px 12px', borderRadius: 8,
              border: '1px solid oklch(0.5 0.16 25 / 0.5)', background: 'oklch(0.22 0.06 25 / 0.35)',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
              color: 'oklch(0.8 0.14 25)',
            }}>
              {store.error}
            </div>
          )}

          {store.discoverMode && (
            <div style={{
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              color: 'oklch(0.65 0.18 220)', letterSpacing: '0.06em',
            }}>
              Discover mode — results come from TMDB &amp; TVDB, not your library
            </div>
          )}
        </div>
      )}

      {/* Content area */}
      <div style={{ flex: 1, display: 'flex', overflow: 'hidden' }}>
        {!store.discoverMode && !detailOpen && store.sidebarOpen && <LibraryFilters />}

        <div
          ref={gridScrollRef}
          onScroll={e => saveScrollPos(SCROLL_KEY, e.currentTarget.scrollTop)}
          style={{
            flex: 1, minHeight: 0,
            // MediaDetailHero owns its own internal fixed-hero/scrolling-body
            // split (so its hero stays pinned behind it) — this container
            // must stop being a second scroller while it's open, or the two
            // fight each other.
            overflowY: detailOpen ? 'hidden' : 'auto',
            opacity: transitioning ? 0 : 1, transition: 'opacity .2s ease',
          }}
        >
          {detailOpen ? (
            selectedDiscover ? (
              <MediaDetail
                discoverResult={selectedDiscover}
                onClose={closeDetail}
                onViewInLibrary={(id, type) => openDetail(() => { setSelectedDiscover(null); store.selectItem(id, type) })}
              />
            ) : (
              <MediaDetail id={store.selectedId!} content_type={store.selectedType!} onClose={closeDetail} />
            )
          ) : store.discoverMode ? (
            <DiscoverGrid
              results={store.discoverResults}
              loading={store.discoverLoading}
              query={rawQ}
              selectedKey={null}
              onSelect={r => openDetail(() => setSelectedDiscover(r))}
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
            <>
              <MediaGrid
                shows={store.shows}
                movies={store.movies}
                density={store.density}
                selectedId={null}
                onItemClick={(id, type) => openDetail(() => store.selectItem(id, type))}
              />
              {store.shows.length + store.movies.length < store.total && (
                <LoadMoreSentinel loading={store.loadingMore} onVisible={() => store.loadMore()} />
              )}
            </>
          )}
        </div>
      </div>
    </div>
  )
})

// ── Top-bar buttons ──────────────────────────────────────────────────────────

// Small inline hint showing when typed search-bar text parsed into real
// field:value clauses (the canon filter syntax — see components/media/
// filterSyntax.ts) rather than just fuzzy free text. Purely informational —
// the query itself already works the same way whether or not this renders,
// since the raw text is sent straight through as `q`/`filter` either way.
function SearchSyntaxHint({ text }: { text: string }) {
  if (!text.trim()) return null
  const n = countClauses(parseFilterSyntax(text))
  if (n === 0) return null
  return (
    <div style={{
      position: 'absolute', top: '100%', left: 0, marginTop: 4,
      fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
      color: 'var(--hds-violet)', letterSpacing: '0.02em',
    }}>
      {n} filter{n !== 1 ? 's' : ''} parsed from your search
    </div>
  )
}

function DiscoverToggleButton({ discoverMode, onClick }: { discoverMode: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'library-discover-toggle', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      title={discoverMode ? 'Switch to Library mode' : 'Switch to Discover mode — search scrapers'}
      style={{
        height: 30, padding: '0 12px', borderRadius: 6, cursor: 'pointer',
        border: `1px solid ${discoverMode ? 'oklch(0.65 0.18 220)' : 'var(--hds-line)'}`,
        background: discoverMode ? 'oklch(0.65 0.18 220 / 0.12)' : 'transparent',
        color: discoverMode ? 'oklch(0.65 0.18 220)' : 'var(--hds-txt-3)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        letterSpacing: '0.08em', whiteSpace: 'nowrap',
        transition: 'border-color .12s, background .12s, color .12s',
      }}
    >◎ Discover</button>
  )
}

// How many items match the current filter — not just what's loaded so far
// via infinite scroll (store.shows.length + store.movies.length), the real
// server-computed total (PagedResult.total, already summed across both
// content types in LibraryStore.fetch/loadMore).
function ResultCount({ loading, total }: { loading: boolean; total: number }) {
  return (
    <span style={{
      fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
      color: 'var(--hds-txt-3)', whiteSpace: 'nowrap', letterSpacing: '0.03em',
    }}>
      {loading ? '…' : `${total.toLocaleString()} item${total === 1 ? '' : 's'}`}
    </span>
  )
}

function DensityButton({ density, active, onClick }: { density: LibraryDensity; active: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: `library-density-${density}`, onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick} title={density}
      style={{
        width: 30, height: 30,
        border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: active ? 'oklch(0.55 0.14 292 / 0.2)' : 'transparent',
        color: active ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
        borderRadius: 6, cursor: 'pointer', fontSize: 14,
      }}
    >{DENSITY_ICONS[density]}</button>
  )
}

function FilterToggleButton({ open, onClick }: { open: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'library-filter-toggle', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick} title="Toggle filters"
      style={{
        width: 30, height: 30,
        border: `1px solid ${open ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: open ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
        color: open ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
        borderRadius: 6, cursor: 'pointer', fontSize: 12,
      }}
    >⊧</button>
  )
}

function HideEmptyToggleButton({ hideEmpty, onClick }: { hideEmpty: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'library-hide-empty-toggle', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      title={hideEmpty ? 'Hiding items with no media — click to show everything' : 'Showing items with no media — click to hide empty items'}
      style={{
        height: 30, padding: '0 10px', borderRadius: 6, cursor: 'pointer',
        border: `1px solid ${hideEmpty ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: hideEmpty ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
        color: hideEmpty ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        letterSpacing: '0.04em', whiteSpace: 'nowrap',
      }}
    >{hideEmpty ? '◎ Has Media' : '◎ All Items'}</button>
  )
}

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
          <a href="/settings?tab=scrapers" style={{ color: 'var(--hds-violet)', textDecoration: 'none' }}>Scrapers</a>.
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
  const [hoveredState, setHovered] = useState(false)
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `discover-card-${result.source}-${result.external_id}`,
    onEnterPress: onClick,
  })
  const hovered = hoveredState || focused
  const srcColor = result.source === 'tmdb' ? 'oklch(0.65 0.18 220)' : 'oklch(0.65 0.12 280)'

  return (
    <div
      ref={ref} data-tv-focused={focused}
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
      {/* Already requested by someone — shown even when not yet in the
          library, so a requester doesn't submit a duplicate for content
          that's pending/approved but not fulfilled yet. */}
      {!result.in_library && result.request_status && (
        <div style={{
          position: 'absolute', top: 8, right: 8, zIndex: 2,
          background: result.request_status === 'approved' ? 'oklch(0.7 0.16 150)' : 'oklch(0.78 0.15 84)',
          borderRadius: 6,
          fontFamily: "'JetBrains Mono', monospace", fontSize: 8,
          color: result.request_status === 'approved' ? '#fff' : 'oklch(0.2 0.04 70)',
          padding: '2px 6px', letterSpacing: '0.06em',
          boxShadow: '0 2px 8px rgba(0,0,0,0.4)',
        }}>{result.request_status === 'approved' ? 'APPROVED' : result.request_status === 'rejected' ? 'REJECTED' : 'REQUESTED'}</div>
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
