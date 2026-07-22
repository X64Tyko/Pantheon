import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { AccordionSection } from '../../channel/sections'
import { libraryStore } from '../../stores/LibraryStore'
import styles from './LibraryFilters.module.css'

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
  // Only offered once a `playlist:<id>` clause is active in the filter tree
  // (see the filter below) — meaningless otherwise, there's no playlist to
  // order by.
  { value: 'playlist_order',              label: 'Playlist Order' },
]

export const LibraryFilters = observer(function LibraryFilters() {
  const [typeOpen, setTypeOpen] = useState(true)
  const [sortOpen, setSortOpen] = useState(true)

  const hasActivePlaylist = !!libraryStore.activePlaylistId
  const visibleSortOptions = SORT_OPTIONS.filter(o => o.value !== 'playlist_order' || hasActivePlaylist)
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
      <aside className={`hds-library-filters ${styles.aside}`}>
      <AccordionSection
        title="CONTENT TYPE"
        open={typeOpen}
        onToggle={() => setTypeOpen(o => !o)}
      >
        <div className={styles.typeStack}>
          {(['all', 'show', 'movie'] as const).map(t => (
            <label key={t} className={`${styles.typeLabel} ${libraryStore.contentType === t ? styles.typeLabelActive : ''}`}>
              <input
                type="radio" name="lib-content-type"
                checked={libraryStore.contentType === t}
                onChange={() => libraryStore.setContentType(t)}
                className={styles.typeRadio}
              />
              {t === 'all' ? 'All' : t === 'show' ? 'Shows' : 'Movies'}
            </label>
          ))}
          <label className={styles.typeLabel} style={{ marginTop: '0.5rem' }}>
            <input
              type="checkbox"
              checked={libraryStore.includeEpisodes}
              onChange={e => libraryStore.setIncludeEpisodes(e.target.checked)}
              className={styles.typeRadio}
            />
            Include Episodes
          </label>
        </div>
      </AccordionSection>

      <AccordionSection
        title="SORT"
        open={sortOpen}
        onToggle={() => setSortOpen(o => !o)}
      >
        <div className={styles.sortStack}>
          <select
            value={libraryStore.sort}
            onChange={e => libraryStore.setSort(e.target.value)}
            className={styles.selectInput}
          >
            {visibleSortOptions.map(o => <option key={o.value} value={o.value}>{o.label}</option>)}
          </select>
          {!dirless && (
            <div className={styles.dirRow}>
              {(['asc', 'desc'] as const).map(d => (
                <button
                  key={d}
                  onClick={() => libraryStore.setSortDir(libraryStore.sortDir === d ? '' : d)}
                  className={`${styles.dirButton} ${libraryStore.sortDir === d ? styles.dirButtonActive : ''}`}
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
              className={styles.rerollButton}
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
