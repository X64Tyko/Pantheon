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

  useNavBack(() => navigate('/tv'))

  useEffect(() => {
    store.loadLibraries().then(() => store.fetch())
    api.getFilterValues('genre').then(setGenres).catch(() => {})
  }, []) // eslint-disable-line react-hooks/exhaustive-deps

  useEffect(() => { store.setQuery(debouncedQ) }, [debouncedQ]) // eslint-disable-line react-hooks/exhaustive-deps

  return (
    <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
      <div style={{ padding: '32px 48px 20px', display: 'flex', flexDirection: 'column', gap: 18, flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 16 }}>
          <TvBackButton onClick={() => navigate('/tv')} />
          <h1 style={{
            fontFamily: "'Chakra Petch', sans-serif", fontSize: 26, fontWeight: 700,
            color: 'var(--hds-txt)', margin: 0,
          }}>Library</h1>
          <TvSearchField value={rawQ} onChange={setRawQ} />
        </div>

        <div style={{ display: 'flex', gap: 10, flexWrap: 'wrap' }}>
          <GenreChip label="All Types" active={store.contentType === 'all'} onClick={() => store.setContentType('all')} />
          <GenreChip label="Shows"     active={store.contentType === 'show'} onClick={() => store.setContentType('show')} />
          <GenreChip label="Movies"    active={store.contentType === 'movie'} onClick={() => store.setContentType('movie')} />
          <div style={{ width: 1, alignSelf: 'stretch', background: 'var(--hds-line)', margin: '0 4px' }} />
          <GenreChip label="All Genres" active={!store.filterGenre} onClick={() => store.setFilterGenre('')} />
          {genres.map(g => (
            <GenreChip key={g} label={g} active={store.filterGenre === g} onClick={() => store.setFilterGenre(g)} />
          ))}
        </div>
      </div>

      <div style={{ flex: 1, minHeight: 0, overflowY: 'auto' }} className="scrollbar-dark">
        {store.loading ? (
          <div style={{
            display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(176px, 1fr))',
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
        width: 44, height: 44, borderRadius: '50%', cursor: 'pointer', flexShrink: 0,
        border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)', color: 'var(--hds-txt)',
        display: 'flex', alignItems: 'center', justifyContent: 'center',
      }}
    >
      <svg width="18" height="18" viewBox="0 0 14 14" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round">
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
      flex: 1, maxWidth: 420,
      border: `1px solid ${focused ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
      borderRadius: 10, background: 'var(--hds-bg-2)',
    }}>
      <input
        value={value}
        onChange={e => onChange(e.target.value)}
        placeholder="Search library…"
        style={{
          width: '100%', height: 44, padding: '0 16px', border: 'none', background: 'transparent',
          color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 15, outline: 'none',
        }}
      />
    </div>
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
      style={{
        padding: '8px 18px', borderRadius: 20, cursor: 'pointer',
        border: `1px solid ${active ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
        background: active ? 'oklch(0.55 0.14 292 / 0.2)' : 'var(--hds-bg-2)',
        color: active ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
        fontFamily: "'JetBrains Mono', monospace", fontSize: 13, whiteSpace: 'nowrap',
      }}
    >{label}</button>
  )
}
