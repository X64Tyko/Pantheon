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

export const SourceSwitcher = observer(function SourceSwitcher({ libraries }: { libraries: LibraryWithSource[] }) {
  const active = libraryStore.activeLibId

  function pillStyle(selected: boolean): React.CSSProperties {
    return {
      ...pillBase,
      background: selected ? 'var(--hds-violet)' : 'transparent',
      borderColor: selected ? 'var(--hds-violet)' : 'var(--hds-line)',
      color: selected ? 'oklch(0.1 0.02 287)' : 'var(--hds-txt-2)',
    }
  }

  return (
    <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
      <button style={pillStyle(active == null)} onClick={() => libraryStore.setLibrary(null)}>
        All Libraries
      </button>
      {libraries.map(lib => (
        <button
          key={lib.library_id}
          style={pillStyle(lib.library_id === active)}
          onClick={() => libraryStore.setLibrary(lib.library_id)}
        >{libLabel(lib, libraries)}</button>
      ))}
    </div>
  )
})
