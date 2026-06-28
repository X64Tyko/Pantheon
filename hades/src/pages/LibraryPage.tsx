import { useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { libraryStore } from '../stores/LibraryStore'
import { SourceSwitcher } from '../components/media/SourceSwitcher'
import { LibraryFilters } from '../components/media/LibraryFilters'
import { MediaGrid } from '../components/media/MediaGrid'
import { MediaDetail } from '../components/media/MediaDetail'
import { filterInputStyle } from '../channel/styles'
import type { LibraryDensity } from '../api/types'

const DENSITY_ICONS: Record<LibraryDensity, string> = { minimal: '⊞', standard: '⊟', rich: '≡' }

export default observer(function LibraryPage() {
  const store = libraryStore

  useEffect(() => {
    store.loadLibraries().then(() => store.fetch())
  }, [])

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
            placeholder="Search library..."
            value={store.query}
            onChange={e => store.setQuery(e.target.value)}
          />
          <div style={{ display: 'flex', gap: 2 }}>
            {(['minimal', 'standard', 'rich'] as LibraryDensity[]).map(d => (
              <button
                key={d}
                onClick={() => store.setDensity(d)}
                title={d}
                style={{
                  width: 30, height: 30,
                  border: `1px solid ${store.density === d ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
                  background: store.density === d ? 'oklch(0.55 0.14 292 / 0.2)' : 'transparent',
                  color: store.density === d ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
                  borderRadius: 6, cursor: 'pointer', fontSize: 14,
                }}
              >{DENSITY_ICONS[d]}</button>
            ))}
          </div>
          <button
            onClick={() => store.toggleSidebar()}
            title="Toggle filters"
            style={{
              width: 30, height: 30,
              border: `1px solid ${store.sidebarOpen ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
              background: store.sidebarOpen ? 'oklch(0.55 0.14 292 / 0.15)' : 'transparent',
              color: store.sidebarOpen ? 'var(--hds-violet)' : 'var(--hds-txt-2)',
              borderRadius: 6, cursor: 'pointer', fontSize: 12,
            }}
          >⊧</button>
        </div>

        <SourceSwitcher libraries={store.libraries} />
      </div>

      {/* Content area */}
      <div style={{ flex: 1, display: 'flex', overflow: 'hidden' }}>
        {store.sidebarOpen && <LibraryFilters />}

        <div style={{ flex: 1, overflowY: 'auto' }}>
          {store.loading ? (
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

        {store.selectedId && store.selectedType && (
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
