import { useEffect, useRef, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { useLocation, useNavigate } from 'react-router-dom'
import { libraryStore } from '../stores/LibraryStore'
import { useAuth } from '../auth/AuthContext'
import { useDebounce } from '../hooks/useDebounce'
import { getScrollPos, saveScrollPos } from '../hooks/scrollMemory'
import { SourceSwitcher } from '../components/media/SourceSwitcher'
import { LibraryFilters } from '../components/media/LibraryFilters'
import { FilterSection } from '../components/PickerFilters'
import { MediaGrid } from '../components/media/MediaGrid'
import { MediaDetail } from '../components/media/MediaDetail'
import { PlaylistTile } from '../components/media/PlaylistTile'
import { LoadMoreSentinel } from '../channel/BrowserTiles'
import { filterInputStyle } from '../channel/styles'
import type { LibraryDensity, ScraperSearchResult } from '../api/types'
import { runPlaylistPlayAction, type PlaylistPlayAction } from '../player/resolvePlayTarget'
import { useFocusable } from '../nav/useFocusable'
import { parseFilterSyntax, countClauses } from '../components/media/filterSyntax'
import styles from './LibraryPage.module.css'

const DENSITY_ICONS: Record<LibraryDensity, string> = { minimal: '⊞', standard: '⊟', rich: '≡' }
const SCROLL_KEY = 'library-grid'

export default observer(function LibraryPage() {
  const store = libraryStore
  const { user } = useAuth()
  const navigate = useNavigate()
  const location = useLocation()
  const [selectedDiscover, setSelectedDiscover] = useState<ScraperSearchResult | null>(null)
  const [rawQ, setRawQ] = useState(store.query)
  const debouncedQ = useDebounce(rawQ, 300)

  const gridScrollRef  = useRef<HTMLDivElement>(null)
  const savedGridScroll = useRef(0)
  const restoredRef     = useRef(false)
  const [transitioning, setTransitioning] = useState(false)

  const detailOpen = !!(store.selectedId || selectedDiscover)
  const activePlaylist = store.playlists.find(p => p.playlist_id === store.activePlaylistId)
  const [playBusy, setPlayBusy] = useState(false)

  const runChipPlayAction = async (action: PlaylistPlayAction) => {
    if (!activePlaylist || playBusy) return
    setPlayBusy(true)
    try {
      const path = await runPlaylistPlayAction(activePlaylist.playlist_id, activePlaylist.mode, action)
      if (path) navigate(path)
    } finally {
      setPlayBusy(false)
    }
  }

  useEffect(() => {
    store.loadLibraries().then(() => store.fetch()).then(() => {
      if (restoredRef.current) return
      restoredRef.current = true
      setTimeout(() => gridScrollRef.current?.scrollTo({ top: getScrollPos(SCROLL_KEY) }), 32)
    })
    store.loadPlaylists()
  }, [])

  useEffect(() => { store.setQuery(debouncedQ) }, [debouncedQ])

  // Re-clicking the Library nav item while already on this page (e.g. from
  // an open item detail) pushes a fresh history entry to the same URL —
  // location.key changes even though the pathname doesn't, so this is the
  // signal to back out of the detail view rather than silently no-op.
  useEffect(() => {
    store.clearSelection()
    setSelectedDiscover(null)
  }, [location.key])

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
    <div className={styles.pageRoot}>
      {/* Top bar — hidden while viewing an item's detail hero */}
      {!detailOpen && (
        <div className={styles.topBar}>
          <div className={styles.topBarRow}>
            <div className={styles.searchWrap}>
              <input
                className={styles.searchInputWide}
                style={filterInputStyle}
                placeholder={store.discoverMode ? 'Search scrapers…' : 'Search, or try genre:horror year:>2015 …'}
                value={rawQ}
                onChange={e => setRawQ(e.target.value)}
              />
              {!store.discoverMode && <SearchSyntaxHint text={rawQ} />}
            </div>

            <DiscoverToggleButton discoverMode={store.discoverMode} onClick={handleToggleDiscover} />

            {!store.discoverMode && (
              <>
                <div className={styles.densityRow}>
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
          {!store.discoverMode && <FilterSection tree={store.filterTree} filteredLibs={store.libraries} playlists={store.playlists} layout="horizontal" />}

          {/* Clicking a Playlists-section tile below turns it into exactly
              this — a `playlist:<id>` rule in the filter tree (see
              LibraryStore.enterPlaylist) — so dismissing it here is just
              removing that rule, not a separate "mode" to track. */}
          {!store.discoverMode && activePlaylist && (
            <div className={styles.playlistChipRow}>
              <span className={styles.playlistChip}>
                Viewing: {activePlaylist.title}
                <button onClick={() => store.exitPlaylist()} className={styles.playlistChipClose} title="Stop viewing this playlist">✕</button>
              </span>
              {activePlaylist.item_count > 0 && (
                <>
                  <button onClick={() => runChipPlayAction('play')} disabled={playBusy} className={styles.playlistActionBtn} title="Play (resumes where you left off)">▶ Play</button>
                  <button onClick={() => runChipPlayAction('restart')} disabled={playBusy} className={styles.playlistActionBtn} title="Restart from the beginning">↻ Restart</button>
                  <button onClick={() => runChipPlayAction('shuffle')} disabled={playBusy} className={styles.playlistActionBtn} title="Shuffle play">🔀 Shuffle</button>
                </>
              )}
              {user?.role === 'admin' && (
                <button
                  onClick={() => navigate(`/playlists?open=${activePlaylist.playlist_id}`)}
                  className={styles.playlistEditLink}
                  title="Edit this playlist"
                >✎ Edit</button>
              )}
            </div>
          )}

          {!store.discoverMode && <SourceSwitcher libraries={store.libraries} />}

          {store.error && (
            <div className={styles.errorBanner}>
              {store.error}
            </div>
          )}

          {store.discoverMode && (
            <div className={styles.discoverBanner}>
              Discover mode — results come from TMDB &amp; TVDB, not your library
            </div>
          )}
        </div>
      )}

      {/* Content area */}
      <div className={styles.contentArea}>
        {!store.discoverMode && !detailOpen && store.sidebarOpen && <LibraryFilters />}

        <div
          ref={gridScrollRef}
          onScroll={e => saveScrollPos(SCROLL_KEY, e.currentTarget.scrollTop)}
          className={`${styles.gridScroll} ${detailOpen ? styles.gridScrollHidden : styles.gridScrollAuto} ${transitioning ? styles.gridScrollTransitioning : styles.gridScrollVisible}`}
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
            <div className={styles.skeletonGrid}>
              {Array.from({ length: 24 }, (_, i) => (
                <div key={i} className={`hds-skeleton ${styles.skeletonTile}`} />
              ))}
            </div>
          ) : (
            <>
              {/* Library's "Playlists" section — browsable tiles, hidden once
                  you've clicked into one (the chip above takes its place).
                  Collapsible (not just always-on) so a growing playlist
                  count doesn't crowd out the actual library grid below —
                  the header stays put either way so it's still one click
                  to get back. */}
              {!activePlaylist && store.playlists.length > 0 && (
                <div className={styles.playlistSectionWrap}>
                  <button onClick={() => store.togglePlaylistsSection()} className={styles.playlistSectionToggle}>
                    <span>{store.playlistsSectionOpen ? '▼' : '▶'}</span> Playlists ({store.playlists.length})
                  </button>
                  {store.playlistsSectionOpen && (
                    <div className={styles.playlistSectionRow}>
                      {store.playlists.map(p => (
                        <PlaylistTile key={p.playlist_id} playlist={p} onClick={() => store.enterPlaylist(p.playlist_id)} />
                      ))}
                    </div>
                  )}
                </div>
              )}
              <MediaGrid
                shows={store.shows}
                movies={store.movies}
                episodes={store.episodes}
                density={store.density}
                selectedId={null}
                onItemClick={(id, type) => openDetail(() => store.selectItem(id, type))}
                onEpisodeClick={id => navigate(`/player/episode/${id}`)}
              />
              {store.shows.length + store.movies.length + store.episodes.length < store.total && (
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
    <div className={styles.searchHint}>
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
      className={`${styles.discoverToggle} ${discoverMode ? styles.discoverToggleOn : styles.discoverToggleOff}`}
    >◎ Discover</button>
  )
}

// How many items match the current filter — not just what's loaded so far
// via infinite scroll (store.shows.length + store.movies.length), the real
// server-computed total (PagedResult.total, already summed across both
// content types in LibraryStore.fetch/loadMore).
function ResultCount({ loading, total }: { loading: boolean; total: number }) {
  return (
    <span className={styles.resultCount}>
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
      className={`${styles.smallSquareBtn} ${styles.densityBtn} ${active ? styles.smallBtnActive : styles.smallBtnInactive}`}
    >{DENSITY_ICONS[density]}</button>
  )
}

function FilterToggleButton({ open, onClick }: { open: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'library-filter-toggle', onEnterPress: onClick })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick} title="Toggle filters"
      className={`${styles.smallSquareBtn} ${styles.filterToggleBtn} ${open ? styles.violetActiveWash : styles.smallBtnInactiveMuted}`}
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
      className={`${styles.hideEmptyBtn} ${hideEmpty ? styles.violetActiveWash : styles.hideEmptyBtnOff}`}
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
      <div className={styles.skeletonGridDiscover}>
        {Array.from({ length: 12 }, (_, i) => (
          <div key={i} className={`hds-skeleton ${styles.skeletonTile}`} />
        ))}
      </div>
    )
  }

  if (!query.trim()) {
    return (
      <div className={`${styles.discoverStateBox} ${styles.discoverEmptyQuery}`}>
        Type to search TMDB and TVDB
      </div>
    )
  }

  if (results.length === 0) {
    return (
      <div className={`${styles.discoverStateBox} ${styles.discoverNoResults}`}>
        No results for "{query}"<br />
        <span className={styles.noResultsSubtext}>
          Make sure TMDB or TVDB API keys are configured and enabled in{' '}
          <a href="/settings?tab=scrapers" className={styles.noResultsLink}>Scrapers</a>.
        </span>
      </div>
    )
  }

  return (
    <div className={styles.skeletonGridDiscover}>
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
  const cardStateClass = selected ? styles.discoverCardSelected : hovered ? styles.discoverCardHovered : ''

  return (
    <div
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
      className={`${styles.discoverCard} ${cardStateClass}`}
    >
      {result.in_library && (
        <div className={`${styles.statusBadge} ${styles.statusBadgeGreen}`}>IN LIBRARY</div>
      )}
      {/* Already requested by someone — shown even when not yet in the
          library, so a requester doesn't submit a duplicate for content
          that's pending/approved but not fulfilled yet. */}
      {!result.in_library && result.request_status && (
        <div className={`${styles.statusBadge} ${result.request_status === 'approved' ? styles.statusBadgeGreen : styles.statusBadgePending}`}>
          {result.request_status === 'approved' ? 'APPROVED' : result.request_status === 'rejected' ? 'REJECTED' : 'REQUESTED'}
        </div>
      )}

      {result.poster_url ? (
        <img
          src={result.poster_url}
          alt={result.title}
          className={styles.posterImg}
          onError={e => { (e.target as HTMLImageElement).style.display = 'none' }}
        />
      ) : (
        <div className={styles.posterPlaceholder}>no poster</div>
      )}

      <div className={styles.cardInfo}>
        <div className={styles.cardTitle}>{result.title}</div>

        <div className={styles.cardMetaRow}>
          {result.year && (
            <span className={styles.cardYear}>
              {result.year}
            </span>
          )}
          <span className={`${styles.sourceBadge} ${result.source === 'tmdb' ? styles.sourceBadgeTmdb : styles.sourceBadgeTvdb}`}>
            {result.source.toUpperCase()}
          </span>
        </div>
      </div>
    </div>
  )
}
