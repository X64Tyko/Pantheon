import { useState, useEffect, useRef } from 'react'
import { observer } from 'mobx-react-lite'
import { api } from '../api/client'
import type { LibraryWithSource } from '../api/types'

// ─── Types ────────────────────────────────────────────────────────────────────

export type FilterField =
  | 'library' | 'genre' | 'year' | 'content_rating' | 'studio'
  | 'director' | 'actor' | 'writer' | 'producer' | 'country'
  | 'collection' | 'network' | 'label' | 'resolution' | 'decade'
  | 'critic_rating' | 'audience_rating' | 'duration' | 'added'

export type FilterOp =
  | 'is' | 'is_not'
  | 'contains' | 'does_not_contain' | 'begins_with' | 'ends_with'
  | 'gt' | 'gte' | 'lt' | 'lte'
  | 'before' | 'after' | 'in_last'

export type ValueType = 'text' | 'number' | 'days' | 'resolution' | 'decade' | 'library'

export interface FilterRule { id: string; field: FilterField; op: FilterOp; value: string }

// ─── Constants ────────────────────────────────────────────────────────────────

export const RESOLUTIONS = ['4K', '1080p', '720p', 'SD']
export const DECADES     = ['2020s', '2010s', '2000s', '1990s', '1980s', '1970s', '1960s', '1950s', '1940s', '1930s']

export type FieldDef = { label: string; valueType: ValueType; ops: { id: FilterOp; label: string }[] }

export const FIELD_DEFS: Record<FilterField, FieldDef> = {
  library:         { label: 'Library',         valueType: 'library',    ops: [{ id: 'is',       label: 'is' }] },
  genre:           { label: 'Genre',            valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  year:            { label: 'Year',             valueType: 'number',     ops: [{ id: 'is',       label: 'is' },           { id: 'lt',               label: 'is before' },    { id: 'gt',  label: 'is after' }] },
  content_rating:  { label: 'Content Rating',   valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  studio:          { label: 'Studio',           valueType: 'text',       ops: [{ id: 'contains', label: 'contains' },     { id: 'does_not_contain', label: 'does not contain' }, { id: 'is', label: 'is' }, { id: 'is_not', label: 'is not' }] },
  director:        { label: 'Director',         valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  actor:           { label: 'Actor',            valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  writer:          { label: 'Writer',           valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  producer:        { label: 'Producer',         valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  country:         { label: 'Country',          valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  collection:      { label: 'Collection',       valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  network:         { label: 'Network',          valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  label:           { label: 'Label',            valueType: 'text',       ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  resolution:      { label: 'Resolution',       valueType: 'resolution', ops: [{ id: 'is',       label: 'is' },           { id: 'is_not',           label: 'is not' }] },
  decade:          { label: 'Decade',           valueType: 'decade',     ops: [{ id: 'is',       label: 'is' }] },
  critic_rating:   { label: 'Critic Rating',    valueType: 'number',     ops: [{ id: 'gte',      label: 'is at least' },  { id: 'lte',              label: 'is at most' },   { id: 'gt', label: 'is greater than' }, { id: 'lt', label: 'is less than' }] },
  audience_rating: { label: 'Audience Rating',  valueType: 'number',     ops: [{ id: 'gte',      label: 'is at least' },  { id: 'lte',              label: 'is at most' },   { id: 'gt', label: 'is greater than' }, { id: 'lt', label: 'is less than' }] },
  duration:        { label: 'Duration (mins)',  valueType: 'number',     ops: [{ id: 'gte',      label: 'is at least' },  { id: 'lte',              label: 'is at most' },   { id: 'gt', label: 'is greater than' }, { id: 'lt', label: 'is less than' }] },
  added:           { label: 'Date Added',       valueType: 'days',       ops: [{ id: 'in_last',  label: 'in the last' },  { id: 'before',           label: 'before' },       { id: 'after', label: 'after' }] },
}

// ─── FilterTagInput ───────────────────────────────────────────────────────────

// Module-level cache: field → resolved options (shared across all instances/renders)
const filterValCache = new Map<string, Promise<string[]>>()

function fetchFilterValues(field: string): Promise<string[]> {
  if (!filterValCache.has(field)) {
    filterValCache.set(field, api.getFilterValues(field).catch(() => []))
  }
  return filterValCache.get(field)!
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

  // Fetch / hydrate from cache on field change
  useEffect(() => {
    setOptions([])
    fetchFilterValues(field).then(setOptions)
  }, [field])

  // Close on outside click
  useEffect(() => {
    const handler = (e: MouseEvent) => {
      if (wrapRef.current && !wrapRef.current.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [])

  // Derive active token from raw value string
  const tokens        = value.split(';')
  const activeRaw     = tokens[tokens.length - 1]
  const confirmed     = tokens.slice(0, -1)
  const activeToken   = activeRaw.trimStart()

  // Suggestions: match active token, exclude already-confirmed tokens
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
    <div ref={wrapRef} className="relative flex-1">
      <input
        ref={inputRef}
        value={value}
        onChange={e => { onChange(e.target.value); setOpen(true); setHiIdx(0) }}
        onFocus={() => setOpen(true)}
        onKeyDown={handleKeyDown}
        className="input text-xs py-0.5 w-full"
        placeholder={options.length > 0 ? 'type or pick… ; for multiple' : 'value…'}
        autoComplete="off"
        spellCheck={false}
      />
      {open && filtered.length > 0 && (
        <ul className="absolute z-50 top-full left-0 right-0 mt-0.5 max-h-48 overflow-y-auto
                        bg-zinc-900 border border-zinc-700 rounded shadow-lg text-xs">
          {filtered.map((opt, i) => (
            <li
              key={opt}
              onMouseDown={e => { e.preventDefault(); pick(opt) }}
              onMouseEnter={() => setHiIdx(i)}
              className={`px-2 py-1 cursor-pointer ${i === hiIdx ? 'bg-violet-700 text-white' : 'text-zinc-300 hover:bg-zinc-800'}`}>
              {opt}
            </li>
          ))}
        </ul>
      )}
    </div>
  )
}

// ─── FilterRuleRow ────────────────────────────────────────────────────────────

export const FilterRuleRow = observer(function FilterRuleRow({ rule, filteredLibs, onUpdate, onRemove }: {
  rule:         FilterRule
  filteredLibs: LibraryWithSource[]
  onUpdate:     (id: string, patch: Partial<Omit<FilterRule, 'id'>>) => void
  onRemove:     (id: string) => void
}) {
  const def = FIELD_DEFS[rule.field]
  const set = (patch: Partial<Omit<FilterRule, 'id'>>) => onUpdate(rule.id, patch)
  return (
    <div className="flex items-center gap-1.5">
      <select value={rule.field} onChange={e => set({ field: e.target.value as FilterField })}
        className="input text-xs py-0.5 w-32">
        {(Object.keys(FIELD_DEFS) as FilterField[]).map(f => (
          <option key={f} value={f}>{FIELD_DEFS[f].label}</option>
        ))}
      </select>
      <select value={rule.op} onChange={e => set({ op: e.target.value as FilterOp })}
        className="input text-xs py-0.5 w-32">
        {def.ops.map(o => <option key={o.id} value={o.id}>{o.label}</option>)}
      </select>

      {def.valueType === 'library' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className="input text-xs py-0.5 flex-1">
          <option value="">Any</option>
          {filteredLibs.map(l => <option key={l.library_id} value={l.library_id}>{l.display_name}</option>)}
        </select>
      )}
      {def.valueType === 'resolution' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className="input text-xs py-0.5 flex-1">
          <option value="">Any</option>
          {RESOLUTIONS.map(r => <option key={r} value={r}>{r}</option>)}
        </select>
      )}
      {def.valueType === 'decade' && (
        <select value={rule.value} onChange={e => set({ value: e.target.value })}
          className="input text-xs py-0.5 flex-1">
          <option value="">Any</option>
          {DECADES.map(d => <option key={d} value={d}>{d}</option>)}
        </select>
      )}
      {def.valueType === 'number' && (
        <input value={rule.value} onChange={e => set({ value: e.target.value })}
          className="input text-xs py-0.5 flex-1" type="number"
          placeholder={rule.field === 'year' ? 'e.g. 2010' : rule.field === 'duration' ? 'mins' : '0–10'} />
      )}
      {def.valueType === 'days' && (
        <div className="flex items-center gap-1 flex-1">
          <input value={rule.value} onChange={e => set({ value: e.target.value })}
            className="input text-xs py-0.5 flex-1" type="number" placeholder="30" />
          {rule.op === 'in_last' && <span className="text-xs text-zinc-500 shrink-0">days</span>}
        </div>
      )}
      {def.valueType === 'text' && (
        <FilterTagInput field={rule.field} value={rule.value} onChange={v => set({ value: v })} />
      )}

      <button onClick={() => onRemove(rule.id)}
        className="text-zinc-600 hover:text-red-400 transition-colors text-xs px-1 shrink-0">✕</button>
    </div>
  )
})

// ─── FilterSection ────────────────────────────────────────────────────────────

export const FilterSection = observer(function FilterSection({ rulesOpen, filterMatch, filterRules, filteredLibs, onToggleOpen, onSetMatch, onAddRule, onUpdateRule, onRemoveRule }: {
  rulesOpen:     boolean
  filterMatch:   'all' | 'any'
  filterRules:   FilterRule[]
  filteredLibs:  LibraryWithSource[]
  onToggleOpen:  () => void
  onSetMatch:    (m: 'all' | 'any') => void
  onAddRule:     () => void
  onUpdateRule:  (id: string, patch: Partial<Omit<FilterRule, 'id'>>) => void
  onRemoveRule:  (id: string) => void
}) {
  return (
    <div className="border-t border-zinc-800/40">
      <button
        className="w-full flex items-center gap-2 px-3 py-1.5 text-xs text-zinc-500 hover:text-zinc-400 transition-colors text-left"
        onClick={onToggleOpen}>
        <span className="text-[10px]">{rulesOpen ? '▼' : '▶'}</span>
        <span>Filters</span>
        {filterRules.length > 0 && (
          <span className="text-violet-400 ml-1">
            ({filterRules.length} rule{filterRules.length !== 1 ? 's' : ''})
          </span>
        )}
      </button>
      {rulesOpen && (
        <div className="px-3 pb-2.5 space-y-1.5 bg-zinc-900/40 border-t border-zinc-800/30">
          <div className="flex items-center gap-2 pt-2">
            <span className="text-xs text-zinc-500">Match</span>
            <select className="input text-xs py-0.5 w-16" value={filterMatch}
              onChange={e => onSetMatch(e.target.value as 'all' | 'any')}>
              <option value="all">All</option>
              <option value="any">Any</option>
            </select>
            <span className="text-xs text-zinc-500">of the following rules:</span>
          </div>
          {filterRules.map(rule => (
            <FilterRuleRow key={rule.id} rule={rule} filteredLibs={filteredLibs}
              onUpdate={onUpdateRule} onRemove={onRemoveRule} />
          ))}
          <button onClick={onAddRule}
            className="text-xs text-violet-400 hover:text-violet-200 transition-colors pt-0.5 block">
            + Add Rule
          </button>
        </div>
      )}
    </div>
  )
})
