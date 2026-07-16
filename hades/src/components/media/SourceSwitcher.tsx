import { observer } from 'mobx-react-lite'
import type { LibraryWithSource } from '../../api/types'
import { libraryStore } from '../../stores/LibraryStore'

function libLabel(lib: LibraryWithSource, all: LibraryWithSource[]): string {
  const dups = all.filter(l => l.display_name === lib.display_name)
  return dups.length > 1 ? `${lib.display_name} (${lib.source_name})` : lib.display_name
}

const pillBase: React.CSSProperties = {
  padding: '5px 14px', borderRadius: 20, cursor: 'pointer',
  fontFamily: "'JetBrains Mono', monospace", fontSize: 11, whiteSpace: 'nowrap',
  transition: 'background 0.12s, color 0.12s, border-color 0.12s',
  border: '1px solid',
}

// Multi-select: each library pill toggles independently (all selected by
// default — see LibraryStore.loadLibraries), so e.g. a bumpers-only library
// can be excluded from "browse everything" without switching to a
// single-library view to do it. The "All" pill is a quick select-all/
// select-none toggle, not a third selection state of its own.
export const SourceSwitcher = observer(function SourceSwitcher({ libraries }: { libraries: LibraryWithSource[] }) {
  const selected = libraryStore.selectedLibIds
  const allSelected = libraries.length > 0 && selected.size >= libraries.length

  function pillStyle(active: boolean): React.CSSProperties {
    return {
      ...pillBase,
      background: active ? 'var(--hds-violet)' : 'transparent',
      borderColor: active ? 'var(--hds-violet)' : 'var(--hds-line)',
      color: active ? 'oklch(0.1 0.02 287)' : 'var(--hds-txt-2)',
    }
  }

  return (
    <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap', alignItems: 'center' }}>
      <button
        style={pillStyle(allSelected)}
        onClick={() => allSelected ? libraryStore.selectNoLibraries() : libraryStore.selectAllLibraries()}
        title={allSelected ? 'Deselect all libraries' : 'Select all libraries'}
      >
        {allSelected ? 'All Libraries' : selected.size === 0 ? 'None Selected' : `${selected.size} Selected`}
      </button>
      {libraries.map(lib => (
        <button
          key={lib.library_id}
          style={pillStyle(selected.has(lib.library_id))}
          onClick={() => libraryStore.toggleLibrary(lib.library_id)}
        >{libLabel(lib, libraries)}</button>
      ))}
    </div>
  )
})
