import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { AccordionSection } from '../../channel/sections'
import { filterInputStyle } from '../../channel/styles'
import { libraryStore } from '../../stores/LibraryStore'

export const LibraryFilters = observer(function LibraryFilters() {
  const [typeOpen,  setTypeOpen]  = useState(true)
  const [genreOpen, setGenreOpen] = useState(true)

  return (
    <aside style={{
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
        title="GENRE"
        open={genreOpen}
        onToggle={() => setGenreOpen(o => !o)}
      >
        <div style={{ paddingTop: 6 }}>
          <input
            style={{ ...filterInputStyle, width: '100%', boxSizing: 'border-box' }}
            placeholder="Filter genre..."
            value={libraryStore.filterGenre}
            onChange={e => libraryStore.setFilterGenre(e.target.value)}
          />
        </div>
      </AccordionSection>
    </aside>
  )
})
