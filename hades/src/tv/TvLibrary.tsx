import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useEffect, useState } from 'react'
import { observer } from 'mobx-react-lite'
import { useNavigate } from 'react-router-dom'
import { libraryStore } from '../stores/LibraryStore'
import { api } from '../api/client'
import { useDebounce } from '../hooks/useDebounce'
import { LoadMoreSentinel } from '../channel/BrowserTiles'
import { useFocusable } from '../nav/useFocusable'
import { useNavBack } from '../nav/back'
import { TvMediaGrid } from './TvMediaGrid'

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

  useNavBack(() => {
    if (isFilterOpen) {
      setIsFilterOpen(false)
    } else {
      navigate('/tv')
    }
  })

  useEffect(() => {
    store.loadLibraries().then(() => store.fetch())
    api.getFilterValues('genre').then(setGenres).catch(() => {})
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => { store.setQuery(debouncedQ) }, [debouncedQ]) // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <div style={{ padding: '24px 48px 16px', display: 'flex', flexDirection: 'column', gap: 14, flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
          <TvBackButton onClick={() => navigate('/tv')} />
          <h1 style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 22, fontWeight: 700,
            color: 'var(--hds-txt)', margin: 0,
          }}>Library</h1>
          <TvSearchField value={rawQ} onChange={setRawQ} />
          <TvFilterToggle active={isFilterOpen} onClick={() => setIsFilterOpen(!isFilterOpen)} />
        </div>
      </div>

      <div style={{ flex: 1, minHeight: 0, position: 'relative' }}>
        <TvFilterOverlay
          open={isFilterOpen}
          onClose={() => setIsFilterOpen(false)}
          genres={genres}
        />

        <div style={{ height: '100%', overflowY: 'auto' }} className="scrollbar-dark">
        {store.loading ? (
          <div style={{
            display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))',
            gap: 24, padding: '8px 48px',
          }}>
            {Array.from({ length: 12 }, (_, i) => (
              <div key={i} className="hds-skeleton" style={{ aspectRatio: '2/3', borderRadius: 12 }} />
            ))}
          </div>
        ) : (
          <>
            <TvMediaGrid
              shows={store.shows}
              movies={store.movies}
              onItemClick={(id, type) => navigate(`/tv/library/${type}/${id}`)}
            />
            {store.shows.length + store.movies.length < store.total && (
              <LoadMoreSentinel loading={store.loadingMore} onVisible={() => store.loadMore()} />
            )}
          </>
        )}
      </div>
    </div>
  )
})

function TvBackButton({ onClick }: { onClick: () => void }) {
  // forceFocus: same fallback as TvHome's LibraryButton — mounts outside <Layout>, no default focus otherwise.
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({ focusKey: 'tv-library-back', onEnterPress: onClick, forceFocus: true })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      style={{
        width: 38, height: 38, borderRadius: '50%', cursor: 'pointer', flexShrink: 0,
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt)',
        display: 'flex', alignItems: 'center', justifyContent: 'center',
      }}
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
    <div ref={ref} data-tv-focused={focused} style={{
      flex: 1, maxWidth: 380,
      border: `1px solid ${focused ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
      borderRadius: 10, background: 'var(--hds-bg-2)',
    }}>
      <input
        value={value}
        onChange={e => onChange(e.target.value)}
        placeholder="Search library…"
        style={{
          width: '100%', height: 38, padding: '0 16px', border: 'none', background: 'transparent',
          color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 14, outline: 'none',
        }}
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
      style={{
        display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer',
        padding: '0 16px', height: 38, borderRadius: 10,
        border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: active ? 'oklch(0.55 0.14 292 / 0.2)' : 'var(--hds-bg-2)',
        color: active ? 'var(--hds-violet)' : 'var(--hds-txt)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 13,
      }}
    >
      <svg width="14" height="14" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.4">
        <path d="M1.5 3.5h11M3.5 7h7M5.5 10.5h3" strokeLinecap="round" />
      </svg>
      Filters
    </button>
  )
}

const TvFilterOverlay = observer(function TvFilterOverlay({ open, onClose, genres }: {
  open: boolean; onClose: () => void; genres: string[]
}) {
  const store = libraryStore
  const { ref, focusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: 'tv-library-filter-overlay',
    trackChildren: true,
    isFocusBoundary: true,
    autoRestoreFocus: true,
  })

  if (!open) return null

  return (
    <FocusContext.Provider value={focusKey}>
    <div
      ref={ref}
      style={{
        position: 'absolute', inset: 0, zIndex: 10,
        background: 'oklch(0 0 0 / 0.65)', backdropFilter: 'blur(12px)',
        display: 'flex', flexDirection: 'column',
      }}
    >
      <div style={{
        padding: '32px 48px', display: 'flex', flexDirection: 'column', gap: 24,
        maxHeight: '100%', overflowY: 'auto',
      }} className="scrollbar-dark">
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between' }}>
          <h2 style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 20, fontWeight: 700,
            color: 'var(--hds-txt)', margin: 0,
          }}>Filters</h2>
          <button
            onClick={onClose}
            style={{
              background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer',
              fontFamily: "'JetBrains Mono', monospace", fontSize: 13,
            }}
          >Close (Back)</button>
        </div>

        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-3)', textTransform: 'uppercase', letterSpacing: '0.1em' }}>Content Type</div>
          <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
            <GenreChip label="All Types" active={store.contentType === 'all'} onClick={() => store.setContentType('all')} />
            <GenreChip label="Shows"     active={store.contentType === 'show'} onClick={() => store.setContentType('show')} />
            <GenreChip label="Movies"    active={store.contentType === 'movie'} onClick={() => store.setContentType('movie')} />
          </div>
        </div>

        <div style={{ display: 'flex', flexDirection: 'column', gap: 12 }}>
          <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 12, color: 'var(--hds-txt-3)', textTransform: 'uppercase', letterSpacing: '0.1em' }}>Genres</div>
          <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
            <GenreChip label="All Genres" active={!store.filterGenre} onClick={() => store.setFilterGenre('')} />
            {genres.map(g => (
              <GenreChip key={g} label={g} active={store.filterGenre === g} onClick={() => store.setFilterGenre(g)} />
            ))}
          </div>
        </div>
      </div>
    </div>
    </FocusContext.Provider>
  )
})

function GenreChip({ label, active, onClick }: { label: string; active: boolean; onClick: () => void }) {
  const { ref, focused } = useFocusable<object, HTMLButtonElement>({
    focusKey: `tv-library-chip-${label}`, onEnterPress: onClick,
  })
  return (
    <button
      ref={ref} data-tv-focused={focused}
      onClick={onClick}
      style={{
        padding: '6px 14px', borderRadius: 20, cursor: 'pointer',
        border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: active ? 'oklch(0.55 0.14 292 / 0.2)' : 'var(--hds-bg-2)',
        color: active ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 12, whiteSpace: 'nowrap',
      }}
    >{label}</button>
  )
}
