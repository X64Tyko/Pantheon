import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { AccordionSection } from '../../channel/sections'
import { libraryStore } from '../../stores/LibraryStore'

const selectStyle: React.CSSProperties = {
  width: '100%', padding: '6px 8px', borderRadius: 6,
  border: '1px solid var(--hds-line)', background: 'var(--hds-bg-3)',
  color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
}

// "Recently Aired/Released" maps to a different backend sort mode per
// content type (shows: recently_aired, movies: recently_released) — see
// LibraryStore.showSort()/movieSort() — so it's one combined option here
// even though it's two values under the hood.
const SORT_OPTIONS: { value: string; label: string; dirless?: boolean }[] = [
  { value: 'title',                       label: 'Title' },
  { value: 'recently_added',              label: 'Date Added' },
  { value: 'year',                        label: 'Year' },
  { value: 'audience_rating',             label: 'Audience Rating' },
  { value: 'duration',                    label: 'Duration' },
  { value: 'recently_released_or_aired',  label: 'Recently Aired/Released' },
  { value: 'random',                      label: 'Random', dirless: true },
]

export const LibraryFilters = observer(function LibraryFilters() {
  const [typeOpen, setTypeOpen] = useState(true)
  const [sortOpen, setSortOpen] = useState(true)

  const dirless = SORT_OPTIONS.find(o => o.value === libraryStore.sort)?.dirless ?? false

  return (
    <>
      {/* Tap-to-close backdrop — mobile only (hds-library-filters-backdrop is
          display:none outside the <768px breakpoint, see index.css). On
          desktop this filter panel is a normal flex sibling pushing the grid
          over; on mobile that same layout leaves ~155px for the grid on a
          375px screen, so below the breakpoint it becomes a fixed overlay
          drawer instead (see .hds-library-filters in index.css). */}
      <div className="hds-library-filters-backdrop" onClick={() => libraryStore.toggleSidebar()} />
      <aside className="hds-library-filters" style={{
        width: 220, flexShrink: 0, borderRight: '1px solid var(--hds-line-s)',
        padding: '16px 14px', display: 'flex', flexDirection: 'column', gap: 8,
        overflowY: 'auto',
      }}>
      <AccordionSection
        title="CONTENT TYPE"
        open={typeOpen}
        onToggle={() => setTypeOpen(o => !o)}
      >
        <div style={{ display: 'flex', flexDirection: 'column', gap: 4, paddingTop: 6 }}>
          {(['all', 'show', 'movie'] as const).map(t => (
            <label key={t} style={{
              display: 'flex', alignItems: 'center', gap: 8,
              fontFamily: "'JetBrains Mono', monospace", fontSize: 11,
              color: libraryStore.contentType === t ? 'var(--hds-txt)' : 'var(--hds-txt-2)',
              cursor: 'pointer', padding: '3px 0',
            }}>
              <input
                type="radio" name="lib-content-type"
                checked={libraryStore.contentType === t}
                onChange={() => libraryStore.setContentType(t)}
                style={{ accentColor: 'var(--hds-violet)' }}
              />
              {t === 'all' ? 'All' : t === 'show' ? 'Shows' : 'Movies'}
            </label>
          ))}
        </div>
      </AccordionSection>

      <AccordionSection
        title="SORT"
        open={sortOpen}
        onToggle={() => setSortOpen(o => !o)}
      >
        <div style={{ display: 'flex', flexDirection: 'column', gap: 6, paddingTop: 6 }}>
          <select
            value={libraryStore.sort}
            onChange={e => libraryStore.setSort(e.target.value)}
            style={selectStyle}
          >
            {SORT_OPTIONS.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
          </select>
          {!dirless && (
            <div style={{ display: 'flex', gap: 4 }}>
              {(['asc', 'desc'] as const).map(d => (
                <button
                  key={d}
                  onClick={() => libraryStore.setSortDir(libraryStore.sortDir === d ? '' : d)}
                  style={{
                    flex: 1, padding: '5px 0', borderRadius: 6, cursor: 'pointer',
                    border: `1px solid ${libraryStore.sortDir === d ? 'var(--hds-violet)' : 'var(--hds-line)'}`,
                    background: libraryStore.sortDir === d ? 'oklch(0.55 0.14 292 / 0.18)' : 'transparent',
                    color: libraryStore.sortDir === d ? 'var(--hds-violet)' : 'var(--hds-txt-3)',
                    fontFamily: "'JetBrains Mono', monospace", fontSize: 10, letterSpacing: '0.04em',
                  }}
                >
                  {d === 'asc' ? 'ASC' : 'DESC'}
                </button>
              ))}
            </div>
          )}
          {dirless && (
            <button
              onClick={() => libraryStore.rerollRandom()}
              title="Reroll — pick a new random order"
              style={{
                display: 'flex', alignItems: 'center', justifyContent: 'center', gap: 6,
                padding: '5px 0', borderRadius: 6, cursor: 'pointer',
                border: '1px solid var(--hds-line)', background: 'transparent',
                color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace",
                fontSize: 10, letterSpacing: '0.04em',
              }}
            >
              🎲 Reroll
            </button>
          )}
        </div>
      </AccordionSection>
      </aside>
    </>
  )
})
