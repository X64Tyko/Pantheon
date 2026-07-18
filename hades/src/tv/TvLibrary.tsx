import { FocusContext, setFocus, doesFocusableExist } from '@noriginmedia/norigin-spatial-navigation'
import { useEffect, useLayoutEffect, useRef, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { useNavigate } from 'react-router-dom'
import { libraryStore } from '../stores/LibraryStore'
import { api } from '../api/client'
import { useDebounce } from '../hooks/useDebounce'
import { LoadMoreSentinel } from '../channel/BrowserTiles'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import { TvMediaGrid } from './TvMediaGrid'
import { rememberDetailReturn, consumeReturnFocusKey } from './tvDetailNav'
import { useZoneManifest } from './useZoneManifest'
import styles from './TvLibrary.module.css'

const TV_LIBRARY_PATH = '/tv/library'

// Filtering here is deliberately scoped down from LibraryPage's full sidebar
// (source/rating/label/network/actor filters) to search + genre chips — a
// D-pad-navigable subset appropriate for a 10-foot surface, not parity with
// the desktop admin-grade filter panel.
export const TvLibrary = observer(function TvLibrary() {
  const navigate = useNavigate()
  const store = libraryStore
  const [rawQ, setRawQ] = useState(store.query)
  const debouncedQ = useDebounce(rawQ, 300)
  const [genres, setGenres] = useState<string[]>([])
  const [isFilterOpen, setIsFilterOpen] = useState(false)
  // Which zones this screen actually renders — search-bar/library-pills/
  // filter-pills/tile-grid, per GET /api/tv/manifest's library.zones. Gates
  // presence of the secondary chrome (search, content-type toggle, genre
  // filter); the grid itself always attempts to render regardless of
  // manifest load state, same resilience the Home shelves already have.
  const { hasZone, zone } = useZoneManifest('library')
  // filter-pills declares which fields are filterable (today just
  // ["genre"]) — this screen only has genre filtering actually wired up, so
  // the check is "is genre among the declared fields" rather than a fully
  // generic loop. A real multi-field filter system (LibraryStore currently
  // only has a dedicated filterGenre, not a generic filter-value map) is
  // real follow-up scope, not built in this pass.
  const genreFilterEnabled = (zone('filter-pills')?.filterFields ?? []).includes('genre')

  // Set only when this mount is the remote's Back arriving from a grid
  // item's Detail route (TvMediaCard's onClick below) — consumed once so a
  // normal Home→Library entry still gets the usual fresh fetch + back-
  // button-focused landing. See tvDetailNav.ts.
  const [restoreFocusKey] = useState(() => consumeReturnFocusKey(TV_LIBRARY_PATH))

  useNavBack(() => {
    if (isFilterOpen) {
      setIsFilterOpen(false)
    } else {
      navigate('/tv')
    }
  })

  useEffect(() => {
    store.loadLibraries()
    // A remembered focus key means we're returning from Detail — the store
    // already holds whatever was loaded (including any "load more" pages)
    // before we navigated away; re-fetching would silently drop all of that
    // back to page 0 and could even scroll the very item we're restoring
    // focus to out of the loaded set.
    if (!restoreFocusKey) store.fetch()
    api.getFilterValues('genre').then(setGenres).catch(() => {})
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => { store.setQuery(debouncedQ) }, [debouncedQ]) // eslint-disable-line react-hooks/exhaustive-deps

  // Only ever applies once — without this guard, any later unrelated
  // fetch (e.g. picking a genre filter) would flip store.loading false→
  // true→false again and yank focus back onto this stale item. Falls back
  // to the back button (the same target its own suppressed forceFocus
  // would have landed on) if the remembered item isn't in the loaded set
  // this time — e.g. it was removed server-side while Detail was open.
  const appliedRestoreRef = useRef(false)
  useEffect(() => {
    if (!restoreFocusKey || appliedRestoreRef.current || store.loading) return
    appliedRestoreRef.current = true
    setFocus(doesFocusableExist(restoreFocusKey) ? restoreFocusKey : 'tv-library-back')
  }, [restoreFocusKey, store.loading])

  return (
    <div className={styles.screen}>
      <div className={styles.header}>
        <div className={styles.headerRow}>
          <TvBackButton onClick={() => navigate('/tv')} forceFocus={!restoreFocusKey} />
          <h1 className={styles.title}>Library</h1>
          {hasZone('search-bar') && <TvSearchField value={rawQ} onChange={setRawQ} />}
          {(hasZone('library-pills') || genreFilterEnabled) && (
            <TvFilterToggle active={isFilterOpen} onClick={() => setIsFilterOpen(!isFilterOpen)} />
          )}
        </div>
      </div>

      <div className={styles.gridArea}>
        <TvFilterOverlay
          open={isFilterOpen}
          onClose={() => setIsFilterOpen(false)}
          genres={genres}
          showContentTypeSection={hasZone('library-pills')}
          showGenreSection={genreFilterEnabled}
        />

        <div className={`${styles.scrollArea} scrollbar-dark`}>
        {store.loading ? (
          <div className={styles.skeletonGrid}>
            {Array.from({ length: 12 }, (_, i) => (
              <div key={i} className={`hds-skeleton ${styles.skeletonTile}`} />
            ))}
          </div>
        ) : (
          <>
            <TvMediaGrid
              shows={store.shows}
              movies={store.movies}
              onItemClick={(id, type) => {
                rememberDetailReturn({ returnTo: TV_LIBRARY_PATH, focusKey: `tv-media-card-${type}-${id}` })
                navigate(`/tv/library/${type}/${id}`)
              }}
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

function TvBackButton({ onClick, forceFocus = true }: { onClick: () => void; forceFocus?: boolean }) {
  // forceFocus: same fallback as TvHome's LibraryButton — mounts outside
  // <Layout>, no default focus otherwise. Suppressed when TvLibrary is
  // restoring focus to a specific grid item instead (returning from Detail).
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-library-back', onEnterPress: onClick, forceFocus })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={styles.backButton}
    >
      <svg width="16" height="16" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
        <path d="M9 2L4 7l5 5" />
      </svg>
    </button>
  )
}

function TvSearchField({ value, onChange }: { value: string; onChange: (v: string) => void }) {
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: 'tv-library-search',
    // A focused-but-not-yet-active search field just relays the physical
    // keyboard to the underlying <input> — most set-top boxes/TVs are driven
    // by a Bluetooth keyboard or phone remote at least some of the time, so
    // there's no need to build an on-screen keyboard for v1.
    onEnterPress: () => (ref.current?.querySelector('input') as HTMLInputElement | null)?.focus(),
  })
  return (
    <div ref={ref} data-tv-focused={focused} className={`${styles.searchField} ${focused ? styles.searchFieldFocused : ''}`}>
      <input
        value={value}
        onChange={e => onChange(e.target.value)}
        placeholder="Search library…"
        className={styles.searchInput}
      />
    </div>
  )
}

function TvFilterToggle({ active, onClick }: { active: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({
    focusKey: 'tv-library-filter-toggle',
    onEnterPress: onClick,
  })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={`${styles.filterToggle} ${active ? styles.filterToggleActive : ''}`}
    >
      <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
        <path d="M1.5 3.5h11M3.5 7h7M5.5 10.5h3" strokeLinecap="round" />
      </svg>
      Filters
    </button>
  )
}

const TvFilterOverlay = observer(function TvFilterOverlay({ open, onClose, genres, showContentTypeSection, showGenreSection }: {
  open: boolean; onClose: () => void; genres: string[]
  showContentTypeSection: boolean; showGenreSection: boolean
}) {
  const store = libraryStore
  const { ref, focusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'tv-library-filter-overlay',
    trackChildren: true,
    isFocusBoundary: true,
    autoRestoreFocus: true,
  })

  // isFocusBoundary only stops focus that's already inside this subtree from
  // leaving it — it does nothing if focus never actually moved in here to
  // begin with. Opening the overlay was pure React state with no setFocus()
  // call, so the D-pad kept driving whatever grid tile was focused
  // underneath (visually hidden but still logically focused) — reported as
  // "pressing down scrolls the library instead of focusing filter options."
  // Close needs its own real focusKey to have a first target at all — it was
  // a plain <button>, entirely outside spatial-nav before this.
  useLayoutEffect(() => {
    if (open) setFocus('tv-library-filter-close')
  }, [open])

  if (!open) return null

  return (
    <FocusContext.Provider value={focusKey}>
    <div ref={ref} className={styles.filterOverlay}>
      <div className={`${styles.filterOverlayInner} scrollbar-dark`}>
        <div className={styles.filterHeaderRow}>
          <h2 className={styles.filterTitle}>Filters</h2>
          <TvFilterCloseButton onClick={onClose} />
        </div>

        {showContentTypeSection && (
          <div className={styles.filterSection}>
            <div className={styles.filterSectionLabel}>Content Type</div>
            <div className={styles.chipRow}>
              <GenreChip label="All Types" active={store.contentType === 'all'} onClick={() => store.setContentType('all')} />
              <GenreChip label="Shows"     active={store.contentType === 'show'} onClick={() => store.setContentType('show')} />
              <GenreChip label="Movies"    active={store.contentType === 'movie'} onClick={() => store.setContentType('movie')} />
            </div>
          </div>
        )}

        {showGenreSection && (
          <div className={styles.filterSection}>
            <div className={styles.filterSectionLabel}>Genres</div>
            <div className={styles.chipRow}>
              <GenreChip label="All Genres" active={!store.filterGenre} onClick={() => store.setFilterGenre('')} />
              {genres.map(g => (
                <GenreChip key={g} label={g} active={store.filterGenre === g} onClick={() => store.setFilterGenre(g)} />
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
    </FocusContext.Provider>
  )
})

function TvFilterCloseButton({ onClick }: { onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({
    focusKey: 'tv-library-filter-close', onEnterPress: onClick,
  })
  return (
    <button ref={ref} data-tv-focused={focused} onClick={onClick} className={styles.closeButton}>
      Close (Back)
    </button>
  )
}

function GenreChip({ label, active, onClick }: { label: string; active: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({
    focusKey: `tv-library-chip-${label}`, onEnterPress: onClick,
  })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      className={`${styles.chip} ${active ? styles.chipActive : ''}`}
    >{label}</button>
  )
}
