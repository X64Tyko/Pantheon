import { useState, useEffect, useRef } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import type { LibraryWithSource, PlaylistBrowseEntry } from '../api/types'
import type { FilterTreeStore, FilterRuleItem } from './media/filterTree'
import styles from './PickerFilters.module.css'

// Field/operator registry lives in a dependency-free leaf module (media/
// filterFields.ts) so filterSyntax.ts/filterTree.ts can import it without a
// circular dependency on this file — re-exported here so existing call
// sites importing from 'PickerFilters' don't all need new import paths.
export {
  FIELD_DEFS, RESOLUTIONS, DECADES,
  type FilterField, type FilterOp, type ValueType, type FieldDef,
} from './media/filterFields'
import { FIELD_DEFS, RESOLUTIONS, DECADES, type FilterField } from './media/filterFields'

// Same disambiguation as SourceSwitcher.tsx's identical helper — two
// libraries from different sources can share a display name (e.g. both a
// Plex and a Jellyfin "Movies" library), so the source name is appended
// whenever that's the case.
function libLabel(lib: LibraryWithSource, all: LibraryWithSource[]): string {
  const dups = all.filter(l => l.display_name === lib.display_name)
  return dups.length > 1 ? `${lib.display_name} (${lib.source_name})` : lib.display_name
}

function distinctSources(libs: LibraryWithSource[]): { source_id: string; source_name: string }[] {
  const seen = new Map<string, string>()
  for (const l of libs) if (!seen.has(l.source_id)) seen.set(l.source_id, l.source_name)
  return Array.from(seen, ([source_id, source_name]) => ({ source_id, source_name }))
}

// ─── FilterTagInput ───────────────────────────────────────────────────────────

// Module-level cache: field → resolved options (shared across all instances/renders)
const filterValCache = new Map<string, Promise<string[]>>()

function fetchFilterValues(field: string): Promise<string[]> {
  if (!filterValCache.has(field)) {
    const p = api.getFilterValues(field)
    filterValCache.set(field, p)
    p.catch(() => filterValCache.delete(field))
  }
  return filterValCache.get(field)!.catch(() => [])
}

// Multi-value combobox: values separated by ';', suggestions fetched from API.
// Active token = text after last ';'; confirmed tokens are the ones before it.
function FilterTagInput({ field, value, onChange }: {
  field:    FilterField
  value:    string
  onChange: (v: string) => void
}) {
  const [options, setOptions] = useState<string[]>([])
  const [open,    setOpen]    = useState(false)
  const [hiIdx,   setHiIdx]   = useState(0)
  const inputRef = useRef<HTMLInputElement>(null)
  const wrapRef  = useRef<HTMLDivElement>(null)

  useEffect(() => {
    setOptions([])
    fetchFilterValues(field).then(setOptions)
  }, [field])

  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (wrapRef.current && !wrapRef.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  const tokens        = value.split(';')
  const activeRaw     = tokens[tokens.length - 1]
  const confirmed     = tokens.slice(0, -1)
  const activeToken   = activeRaw.trimStart()

  const confirmedSet  = new Set(confirmed.map(t => t.trim().toLowerCase()))
  const filtered      = activeToken === ''
    ? options.filter(o => !confirmedSet.has(o.toLowerCase()))
    : options.filter(o => !confirmedSet.has(o.toLowerCase()) && o.toLowerCase().includes(activeToken.toLowerCase()))

  const pick = (option: string) => {
    const next = [...confirmed, option].join(';')
    onChange(next)
    setOpen(false)
    setHiIdx(0)
    inputRef.current?.focus()
  }

  const handleKeyDown = (e: React.KeyboardEvent<HTMLInputElement>) => {
    if (!open || filtered.length === 0) return
    if (e.key === 'ArrowDown') { e.preventDefault(); setHiIdx(i => Math.min(i + 1, filtered.length - 1)) }
    else if (e.key === 'ArrowUp') { e.preventDefault(); setHiIdx(i => Math.max(i - 1, 0)) }
    else if (e.key === 'Enter') { e.preventDefault(); pick(filtered[hiIdx] ?? filtered[0]) }
    else if (e.key === 'Escape') setOpen(false)
  }

  return (
    <div ref={wrapRef} className={styles.tagInputWrap}>
      <input
        ref={inputRef}
        value={value}
        onChange={e => { onChange(e.target.value); setOpen(true); setHiIdx(0) }}
        onFocus={() => setOpen(true)}
        onKeyDown={handleKeyDown}
        className={`${styles.input} ${styles.tagInput}`}
        placeholder={options.length > 0 ? 'type or pick… ; for multiple' : 'value…'}
        autoComplete="off"
        spellCheck={false}
      />
      {open && filtered.length > 0 && (
        <ul className={styles.dropdown}>
          {filtered.map((opt, i) => (
            <li
              key={opt}
              onMouseDown={e => { e.preventDefault(); pick(opt) }}
              onMouseEnter={() => setHiIdx(i)}
              className={`${styles.option} ${i === hiIdx ? styles.optionHighlighted : ''}`}
            >
              {opt}
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}

// ─── FilterRuleRow ────────────────────────────────────────────────────────────

const FilterRuleRow = observer(function FilterRuleRow({ rule, filteredLibs, playlists = [], onUpdate, onRemove }: {
  rule:         FilterRuleItem
  filteredLibs: LibraryWithSource[]
  playlists?:   PlaylistBrowseEntry[]
  onUpdate:     (id: string, patch: Partial<Pick<FilterRuleItem, 'field' | 'op' | 'value'>>) => void
  onRemove:     (id: string) => void
}) {
  const def = FIELD_DEFS[rule.field]
  const set = (patch: Partial<Pick<FilterRuleItem, 'field' | 'op' | 'value'>>) => onUpdate(rule.id, patch)
  return (
    <div className={styles.row}>
      <select value={rule.field} onChange={e => set({ field: e.target.value as FilterField })}
        className={`${styles.input} ${styles.selectFixed}`}>
        {(Object.keys(FIELD_DEFS) as FilterField[]).map(f => (
          <option key={f} value={f}>{FIELD_DEFS[f].label}</option>
        ))}
      </select>
      <select value={rule.op} onChange={e => set({ op: e.target.value as typeof rule.op })}
        className={`${styles.input} ${styles.selectFixed}`}>
        {def.ops.map(o => <option key={o.id} value={o.id}>{o.label}</option>)}
      </select>

      {def.valueType === 'library' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className={`${styles.input} ${styles.valueFlex}`}>
          <option value="">Any</option>
          {filteredLibs.map(l => <option key={l.library_id} value={l.library_id}>{libLabel(l, filteredLibs)}</option>)}
        </select>
      )}
      {def.valueType === 'playlist' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className={`${styles.input} ${styles.valueFlex}`}>
          <option value="">Any</option>
          {playlists.map(p => <option key={p.playlist_id} value={p.playlist_id}>{p.title}</option>)}
        </select>
      )}
      {def.valueType === 'source' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className={`${styles.input} ${styles.valueFlex}`}>
          <option value="">Any</option>
          {distinctSources(filteredLibs).map(s => <option key={s.source_id} value={s.source_id}>{s.source_name}</option>)}
        </select>
      )}
      {def.valueType === 'resolution' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className={`${styles.input} ${styles.valueFlex}`}>
          <option value="">Any</option>
          {RESOLUTIONS.map(r => <option key={r} value={r}>{r}</option>)}
        </select>
      )}
      {def.valueType === 'decade' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className={`${styles.input} ${styles.valueFlex}`}>
          <option value="">Any</option>
          {DECADES.map(d => <option key={d} value={d}>{d}</option>)}
        </select>
      )}
      {def.valueType === 'number' && (
        <input value={rule.value} onChange={e => set({ value: e.target.value })}
          className={`${styles.input} ${styles.valueFlex}`} type="number"
          placeholder={rule.field === 'year' ? 'e.g. 2010' : rule.field === 'duration' ? 'mins' : '0–10'} />
      )}
      {def.valueType === 'days' && (
        <div className={styles.daysRow}>
          <input value={rule.value} onChange={e => set({ value: e.target.value })}
            className={`${styles.input} ${styles.valueFlex}`} type="number" placeholder="30" />
          {rule.op === 'in_last' && <span className={styles.daysLabel}>days</span>}
        </div>
      )}
      {def.valueType === 'text' && (
        <FilterTagInput field={rule.field} value={rule.value} onChange={v => set({ value: v })} />
      )}

      <button onClick={() => onRemove(rule.id)} className={styles.removeBtn}>✕</button>
    </div>
  )
})

// ─── Match All/Any selector ─────────────────────────────────────────────────

function MatchSelect({ value, onChange }: { value: 'all' | 'any'; onChange: (m: 'all' | 'any') => void }) {
  return (
    <select
      value={value}
      onChange={e => onChange(e.target.value as 'all' | 'any')}
      className={`${styles.input} ${styles.matchSelect}`}
    >
      <option value="all">All</option>
      <option value="any">Any</option>
    </select>
  )
}

// ─── FilterSection ────────────────────────────────────────────────────────────

// Rule-builder panel — one level of grouping ("(genre is Horror AND year is
// after 2015) OR studio is A24"), matching Plex's smart-filter UX. Deeper
// nesting is supported by the underlying canon syntax (filterSyntax.ts) for
// power users typing directly into the search bar; the visual builder is
// deliberately bounded to one level to stay usable.
//
// layout='vertical' (default) stacks each rule/group full-width, one per
// line — right for a narrow host (Playlists/Filler Lists/channel picker's
// side panels). layout='horizontal' wraps rules/groups left-to-right in a
// bounded-width flow instead — for the Library page, where the rule builder
// sits in a full-width bar above the library pills rather than a narrow
// sidebar (a sidebar the rule list would otherwise grow to fill and force
// into its own awkward internal scroll as rules/groups are added).
export const FilterSection = observer(function FilterSection({ tree, filteredLibs, playlists = [], layout = 'vertical' }: {
  tree:         FilterTreeStore
  filteredLibs: LibraryWithSource[]
  playlists?:   PlaylistBrowseEntry[]
  layout?:      'vertical' | 'horizontal'
}) {
  const ruleCount = tree.items.reduce((n, it) => n + (it.kind === 'rule' ? 1 : it.rules.length), 0)
  const horizontal = layout === 'horizontal'

  return (
    <div className={`${styles.section} ${horizontal ? styles.sectionHorizontal : ''}`}>
      <button
        onClick={() => tree.toggleOpen()}
        className={`${styles.toggleBtn} ${horizontal ? styles.toggleBtnHorizontal : ''}`}
      >
        <span className={styles.toggleIcon}>{tree.open ? '▼' : '▶'}</span>
        <span>Filters</span>
        {ruleCount > 0 && (
          <span className={styles.ruleCountText}>({ruleCount} rule{ruleCount !== 1 ? 's' : ''})</span>
        )}
      </button>
      {tree.open && (
        <div className={`${styles.panel} ${horizontal ? styles.panelHorizontal : ''}`}>
          <div className={`${styles.matchRow} ${horizontal ? styles.matchRowHorizontal : ''}`}>
            <span className={styles.matchText}>Match</span>
            <MatchSelect value={tree.match} onChange={m => tree.setMatch(m)} />
            <span className={styles.matchText}>of the following:</span>
          </div>

          <div className={`${styles.itemsRow} ${horizontal ? styles.itemsRowHorizontal : ''}`}>
            {tree.items.map(item => item.kind === 'rule' ? (
              <div key={item.id} className={horizontal ? styles.itemBoxFixed : undefined}>
                <FilterRuleRow
                  rule={item} filteredLibs={filteredLibs} playlists={playlists}
                  onUpdate={(id, patch) => tree.updateRule(id, patch)}
                  onRemove={id => tree.removeItem(id)}
                />
              </div>
            ) : (
              <div key={item.id} className={`${styles.groupBox} ${horizontal ? styles.itemBoxFixed : ''}`}>
                <div className={styles.groupHeader}>
                  <span className={styles.matchText}>Match</span>
                  <MatchSelect value={item.match} onChange={m => tree.setGroupMatch(item.id, m)} />
                  <span className={styles.matchText}>within this group</span>
                  <button onClick={() => tree.removeItem(item.id)} className={`${styles.removeBtn} ${styles.removeGroupBtn}`}>
                    ✕ remove group
                  </button>
                </div>
                {item.rules.map(rule => (
                  <FilterRuleRow
                    key={rule.id} rule={rule} filteredLibs={filteredLibs} playlists={playlists}
                    onUpdate={(id, patch) => tree.updateRule(id, patch)}
                    onRemove={id => tree.removeRuleFromGroup(item.id, id)}
                  />
                ))}
                <button onClick={() => tree.addRuleToGroup(item.id)} className={styles.addRuleToGroupBtn}>
                  + Add rule to group
                </button>
              </div>
            ))}
          </div>

          <div className={styles.actionsRow}>
            <button onClick={() => tree.addRule()} className={styles.actionBtn}>
              + Add Rule
            </button>
            <button onClick={() => tree.addGroup()} className={styles.actionBtn}>
              + Add Group
            </button>
          </div>
        </div>
      )}
    </div>
  )
})
