import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import { BLOCK_META, FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle, filterInputStyle } from './styles'
import type { FillerEntryAdvancement, FillerSelectionMode } from '../api/types'
import type { ChannelDetailStore } from './store'

type ArmedList = { filler_list_id: string; title: string }

const FillerOverlay = observer(function FillerOverlay({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const [armed, setArmed]   = useState<ArmedList | null>(null)
  const [dragId, setDragId] = useState<string | null>(null)
  const [newAdv, setNewAdv] = useState<FillerEntryAdvancement>('sized')

  const d            = store.draft
  const entries      = store.draftFillerEntries
  const fillerLists  = store.allFillerLists
  const showWeight   = d.filler_selection === 'weighted'

  const addArmed = (item: ArmedList) => {
    const already = entries.some(e => e.filler_list_id === item.filler_list_id)
    if (!already) {
      store.addBlockFiller(channelId, { filler_list_id: item.filler_list_id, advancement: newAdv, weight: 1 })
    }
    setArmed(null)
  }

  const armedHint = armed
    ? `"${armed.title}" ready — click Add below`
    : 'Click a tile to select a filler list'

  return (
    <div style={{ position: 'absolute', inset: 0, zIndex: 82, display: 'flex', flexDirection: 'column', background: 'linear-gradient(168deg, oklch(0.18 0.024 290 / 0.96), oklch(0.13 0.018 288 / 0.98))', backdropFilter: 'blur(26px)' }}>
      <div style={{ position: 'absolute', top: 0, left: 0, right: 0, height: 1, background: 'linear-gradient(90deg, transparent, oklch(0.78 0.15 292 / 0.7) 40%, oklch(0.85 0.13 84 / 0.5) 75%, transparent)', pointerEvents: 'none' }} />

      {/* Header */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 14, padding: '16px 22px 14px', borderBottom: '1px solid var(--hds-line-s)' }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>Filler &amp; Fallback</span>
        <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>select a filler list to add it to this block</span>
        <div style={{ flex: 1 }} />
        <button onClick={() => { store.fillerOverlayOpen = false }}
          style={{ width: 34, height: 34, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 9, border: '1px solid var(--hds-line-s)', background: 'transparent', color: 'var(--hds-txt-2)', fontSize: 18, cursor: 'pointer' }}>×</button>
      </div>

      {/* Body */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>
        {/* Tile grid */}
        <div style={{ flex: 1, minWidth: 0, overflow: 'auto', padding: '18px 22px' }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 14, fontFamily: "'JetBrains Mono', monospace" }}>
            FILLER LISTS
          </div>

          {fillerLists.length === 0 ? (
            <div style={{ padding: '40px 20px', textAlign: 'center', color: 'var(--hds-txt-3)', fontSize: 12 }}>
              No filler lists. Create one on the Filler page first.
            </div>
          ) : (
            <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(140px, 1fr))', gap: 14, alignContent: 'start' }}>
              {fillerLists.map(fl => {
                const isArmed   = armed?.filler_list_id === fl.filler_list_id
                const isAdded   = entries.some(e => e.filler_list_id === fl.filler_list_id)
                const isDragged = dragId === fl.filler_list_id
                return (
                  <div
                    key={fl.filler_list_id}
                    draggable
                    onDragStart={() => setDragId(fl.filler_list_id)}
                    onDragEnd={() => setDragId(null)}
                    onClick={() => setArmed(isArmed ? null : { filler_list_id: fl.filler_list_id, title: fl.title })}
                    style={{
                      display: 'flex', flexDirection: 'column', gap: 6, cursor: 'pointer',
                      borderRadius: 9, overflow: 'hidden',
                      border: `1px solid ${isArmed ? 'var(--hds-violet)' : 'var(--hds-line-s)'}`,
                      background: 'var(--hds-bg-2)',
                      boxShadow: isArmed ? '0 0 0 1px var(--hds-violet)' : 'none',
                      opacity: isDragged ? 0.5 : 1,
                      transition: 'border-color .1s',
                    }}
                    onMouseEnter={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-violet)' }}
                    onMouseLeave={e => { if (!isArmed) (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)' }}
                  >
                    <div style={{ width: '100%', aspectRatio: '2/3', background: 'var(--hds-bg-3)', display: 'flex', alignItems: 'center', justifyContent: 'center', position: 'relative' }}>
                      <span style={{ fontSize: 28, opacity: 0.25 }}>·</span>
                      {isAdded && (
                        <div style={{ position: 'absolute', top: 6, right: 6, width: 18, height: 18, borderRadius: '50%', background: 'var(--hds-violet)', display: 'flex', alignItems: 'center', justifyContent: 'center', fontSize: 9, color: '#fff', fontWeight: 700 }}>✓</div>
                      )}
                      {isArmed && (
                        <div style={{ position: 'absolute', inset: 0, background: 'oklch(0.55 0.14 292 / 0.3)', display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
                          <span style={{ fontSize: 11, color: 'oklch(0.9 0.1 292)', fontFamily: "'JetBrains Mono', monospace" }}>selected</span>
                        </div>
                      )}
                    </div>
                    <div style={{ padding: '5px 7px 7px' }}>
                      <div style={{ fontSize: 11, fontWeight: 600, lineHeight: 1.3, overflow: 'hidden', display: '-webkit-box', WebkitLineClamp: 2, WebkitBoxOrient: 'vertical' }}>
                        {fl.title}
                      </div>
                      <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 2, fontFamily: "'JetBrains Mono', monospace" }}>
                        {fl.item_count != null ? `${fl.item_count} items` : 'filler list'}
                      </div>
                    </div>
                  </div>
                )
              })}
            </div>
          )}
        </div>

        {/* Slots / entries panel */}
        <div style={{ flexShrink: 0, width: 380, borderLeft: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', background: 'oklch(0.15 0.018 288 / 0.55)', overflow: 'auto', padding: 18 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 15 }}>
            <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 11, letterSpacing: '0.14em', color: 'var(--hds-gold)' }}>BLOCK FILLER</span>
            <span style={{ fontSize: 11, color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace" }}>{entries.length}</span>
            <div style={{ flex: 1 }} />
            <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{armedHint}</span>
          </div>

          {entries.length > 1 && (
            <div style={{ marginBottom: 12 }}>
              <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
              <select value={d.filler_selection} onChange={e => store.setDraft('filler_selection', e.target.value as FillerSelectionMode)} style={inputStyle}>
                {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
            </div>
          )}

          <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 14 }}>
            {entries.map(entry => (
              <div key={entry.id} style={{ display: 'flex', alignItems: 'center', gap: 8, padding: '9px 11px', background: 'oklch(0.21 0.024 290 / 0.5)', border: '1px solid var(--hds-line-s)', borderRadius: 9 }}>
                <span style={{ width: 7, height: 7, borderRadius: 2, background: BLOCK_META.filler.edge, flexShrink: 0 }} />
                <span style={{ flex: 1, fontSize: 12, fontWeight: 500, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{entry.title || entry.filler_list_id}</span>
                <select
                  value={entry.advancement}
                  onChange={e => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { advancement: e.target.value as FillerEntryAdvancement })}
                  style={{ ...filterInputStyle, width: 88 }}
                >
                  {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                {showWeight && (
                  <input type="number" min={1} value={entry.weight}
                    onChange={e => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { weight: Math.max(1, +e.target.value || 1) })}
                    style={{ ...filterInputStyle, width: 44, textAlign: 'center' }} title="Weight" />
                )}
                <button onClick={() => store.removeBlockFiller(channelId, store.editing?.block_id ?? '', entry.id)}
                  style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13, flexShrink: 0 }}>×</button>
              </div>
            ))}

            {entries.length === 0 && (
              <div style={{ padding: '16px 12px', textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", border: '1px dashed var(--hds-line-s)', borderRadius: 8 }}>
                No filler lists — channel default will be used
              </div>
            )}
          </div>

          {/* Add selected */}
          {armed && (
            <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9, marginBottom: 10 }}>
              <div style={{ fontSize: 12, color: 'var(--hds-txt)', marginBottom: 8, fontWeight: 500 }}>{armed.title}</div>
              <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
                <select value={newAdv} onChange={e => setNewAdv(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, flex: '1 1 100px' }}>
                  {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
                <button
                  onClick={() => addArmed(armed)}
                  disabled={entries.some(e => e.filler_list_id === armed.filler_list_id)}
                  style={{ padding: '6px 14px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: 'pointer', opacity: entries.some(e => e.filler_list_id === armed.filler_list_id) ? 0.4 : 1 }}
                >
                  {entries.some(e => e.filler_list_id === armed.filler_list_id) ? 'Already added' : 'Add to block'}
                </button>
              </div>
            </div>
          )}

          {/* Inter-filler toggle */}
          <button
            onClick={() => store.setDraft('inter_filler', !d.inter_filler)}
            style={{
              display: 'flex', alignItems: 'center', justifyContent: 'space-between', width: '100%',
              padding: '8px 10px', border: `1px solid ${d.inter_filler ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`,
              borderRadius: 7, background: d.inter_filler ? 'oklch(0.55 0.1 84 / 0.12)' : 'var(--hds-bg-3)',
              cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", marginTop: 'auto',
            }}
          >
            <span style={{ fontSize: 11, color: 'var(--hds-txt-2)' }}>Filler between programs</span>
            <span style={{ fontSize: 10, fontWeight: 700, letterSpacing: '0.08em', color: d.inter_filler ? 'var(--hds-gold)' : 'var(--hds-txt-3)' }}>{d.inter_filler ? 'ON' : 'OFF'}</span>
          </button>
          <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 6, lineHeight: 1.55 }}>
            When off, filler only fills leftover time at end of block. When on, also fills between programs.
          </div>
        </div>
      </div>

      {/* Footer */}
      <div style={{ flexShrink: 0, display: 'flex', alignItems: 'center', gap: 11, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)' }}>
        <span style={{ flex: 1, fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
          {entries.length > 0 ? `${entries.length} filler list${entries.length !== 1 ? 's' : ''} · ${d.filler_selection}` : 'Channel default filler will be used'}
        </span>
        <button
          onClick={() => { store.fillerOverlayOpen = false }}
          style={{ padding: '11px 26px', borderRadius: 10, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em', border: 'none', cursor: 'pointer' }}
        >Done</button>
      </div>
    </div>
  )
})

export default FillerOverlay
