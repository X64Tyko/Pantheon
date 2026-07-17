import { observer } from 'mobx-react-lite'
import type { LibraryWithSource } from '../../api/types'
import { libraryStore } from '../../stores/LibraryStore'
import styles from './SourceSwitcher.module.css'

function libLabel(lib: LibraryWithSource, all: LibraryWithSource[]): string {
  const dups = all.filter(l => l.display_name === lib.display_name)
  return dups.length > 1 ? `${lib.display_name} (${lib.source_name})` : lib.display_name
}

// Multi-select: each library pill toggles independently (all selected by
// default — see LibraryStore.loadLibraries), so e.g. a bumpers-only library
// can be excluded from "browse everything" without switching to a
// single-library view to do it. The "All" pill is a quick select-all/
// select-none toggle, not a third selection state of its own.
export const SourceSwitcher = observer(function SourceSwitcher({ libraries }: { libraries: LibraryWithSource[] }) {
  const selected = libraryStore.selectedLibIds
  const allSelected = libraries.length > 0 && selected.size >= libraries.length

  return (
    <div className={styles.row}>
      <button
        className={`${styles.pill} ${allSelected ? styles.pillActive : ''}`}
        onClick={() => allSelected ? libraryStore.selectNoLibraries() : libraryStore.selectAllLibraries()}
        title={allSelected ? 'Deselect all libraries' : 'Select all libraries'}
      >
        {allSelected ? 'All Libraries' : selected.size === 0 ? 'None Selected' : `${selected.size} Selected`}
      </button>
      {libraries.map(lib => (
        <button
          key={lib.library_id}
          className={`${styles.pill} ${selected.has(lib.library_id) ? styles.pillActive : ''}`}
          onClick={() => libraryStore.toggleLibrary(lib.library_id)}
        >{libLabel(lib, libraries)}</button>
      ))}
    </div>
  )
})
