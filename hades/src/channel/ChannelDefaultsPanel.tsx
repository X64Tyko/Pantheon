import { useState } from 'react'
import { observer } from 'mobx-react-lite'
import type { Channel, FillerEntryAdvancement, FillerSelectionMode } from '../api/types'
import { FILLER_ADV_OPTS, FILLER_SEL_OPTS } from './constants'
import { inputStyle, filterInputStyle } from './styles'
import { SectionLabel } from './SectionLabel'
import { FillerEntryRow } from './FillerPanel'
import type { ChannelDetailStore } from './store'

const ChannelDefaultsPanel = observer(function ChannelDefaultsPanel({ channel, channelId, store }: {
  channel:   Channel | undefined
  channelId: string
  store:     ChannelDetailStore
}) {
  const [addOpen, setAddOpen]     = useState(false)
  const [addListId, setAddListId] = useState('')
  const [addAdv, setAddAdv]       = useState<FillerEntryAdvancement>('sequential')
  const [addWeight, setAddWeight] = useState(1)

  const selectionMode = channel?.default_filler_selection ?? 'round_robin'
  const entries       = channel?.default_filler_entries   ?? []
  const showWeight    = selectionMode === 'weighted'

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>Channel Settings</span>
      </div>

      <div style={{ flex: 1, overflow: 'auto', padding: 20 }} className="scrollbar-dark">
        <div style={{ fontSize: 12.5, color: 'var(--hds-txt-2)', lineHeight: 1.6, marginBottom: 22 }}>
          Select a block to edit it, or press <span style={{ color: 'var(--hds-gold)' }}>Add Block</span> to create one.
        </div>

        <SectionLabel>CHANNEL</SectionLabel>
        <div style={{ display: 'flex', gap: 7, marginBottom: 8 }}>
          <div style={{ flex: 1 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>NAME</div>
            <input value={store.channelDraftName} onChange={e => store.setChannelDraft({ name: e.target.value })} style={inputStyle} />
          </div>
          <div style={{ width: 64 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>CH #</div>
            <input type="number" min={1} value={store.channelDraftNumber} onChange={e => store.setChannelDraft({ number: Math.max(1, +e.target.value || 1) })} style={inputStyle} />
          </div>
        </div>
        <div style={{ marginBottom: 8 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>TIMEZONE</div>
          <input value={store.channelDraftTimezone} onChange={e => store.setChannelDraft({ timezone: e.target.value })} style={inputStyle} placeholder="UTC" />
        </div>
        <div style={{ marginBottom: 14 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 4 }}>EPG SEED</div>
          <div style={{ display: 'flex', gap: 6, alignItems: 'center' }}>
            <input type="number" min={0} value={store.channelDraftSeed} onChange={e => store.setChannelDraft({ seed: Math.max(0, +e.target.value || 0) })} style={{ ...inputStyle, flex: 1 }} />
            <button
              onClick={() => store.setChannelDraft({ seed: Math.floor(Math.random() * 99999) + 1 })}
              title="Randomize seed"
              style={{ padding: '5px 9px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer' }}
            >⚄</button>
          </div>
          <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
            Controls the starting position for EPG simulation when no live schedule exists. Change to get a different ordering.
          </div>
        </div>

        {store.channelSaveErr && (
          <div style={{ padding: '7px 10px', marginBottom: 10, borderRadius: 6, background: 'oklch(0.2 0.05 22 / 0.3)', border: '1px solid oklch(0.4 0.1 22 / 0.4)', color: 'oklch(0.72 0.16 22)', fontSize: 11 }}>
            {store.channelSaveErr}
          </div>
        )}

        <button
          onClick={() => store.saveChannel(channelId)}
          disabled={store.channelSaving}
          style={{ width: '100%', padding: '9px 0', border: 'none', borderRadius: 8, background: store.channelDirty ? 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))' : 'var(--hds-bg-3)', color: store.channelDirty ? 'oklch(0.2 0.04 70)' : 'var(--hds-txt-3)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: store.channelSaving ? 'default' : 'pointer', marginBottom: 22, opacity: store.channelSaving ? 0.6 : 1, transition: 'background 0.15s, color 0.15s' }}
        >
          {store.channelSaving ? 'Saving…' : 'Save Channel'}
        </button>

        <SectionLabel>DEFAULT FILLER</SectionLabel>
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 12, lineHeight: 1.55 }}>
          Used when a block has no filler lists of its own.
        </div>

        {entries.length > 1 && (
          <div style={{ marginBottom: 12 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
            <select value={selectionMode} onChange={e => store.saveChannelFiller(channelId, { default_filler_selection: e.target.value as FillerSelectionMode })} style={inputStyle}>
              {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
          </div>
        )}

        <div style={{ display: 'flex', flexDirection: 'column', gap: 7, marginBottom: 10 }}>
          {entries.map(entry => (
            <FillerEntryRow
              key={entry.id}
              entry={entry}
              showWeight={showWeight}
              onAdvancement={adv => store.updateChannelFiller(channelId, entry.id, { advancement: adv })}
              onWeight={w   => store.updateChannelFiller(channelId, entry.id, { weight: w })}
              onRemove={()  => store.removeChannelFiller(channelId, entry.id)}
            />
          ))}
        </div>

        {entries.length === 0 && !addOpen && (
          <div style={{ textAlign: 'center', padding: '10px 6px', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            No default filler lists configured
          </div>
        )}

        <button
          onClick={() => setAddOpen(o => !o)}
          style={{ padding: '6px 12px', border: '1px solid var(--hds-line)', borderRadius: 7, background: 'transparent', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', marginBottom: addOpen ? 8 : 0 }}
        >
          {addOpen ? '✕ Cancel' : '+ Add filler list'}
        </button>

        {addOpen && (
          <div style={{ padding: '11px 12px', background: 'oklch(0.16 0.016 286)', border: '1px solid var(--hds-line)', borderRadius: 9 }}>
            <div style={{ display: 'flex', gap: 7, alignItems: 'center', flexWrap: 'wrap' }}>
              <select value={addListId} onChange={e => setAddListId(e.target.value)} style={{ ...filterInputStyle, flex: '1 1 140px' }}>
                <option value="">Select filler list…</option>
                {store.allFillerLists.map(f => <option key={f.filler_list_id} value={f.filler_list_id}>{f.title}</option>)}
              </select>
              <select value={addAdv} onChange={e => setAddAdv(e.target.value as FillerEntryAdvancement)} style={{ ...filterInputStyle, width: 96 }}>
                {FILLER_ADV_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
              </select>
              {showWeight && (
                <input type="number" min={1} value={addWeight} onChange={e => setAddWeight(Math.max(1, +e.target.value || 1))} style={{ ...filterInputStyle, width: 48 }} placeholder="Wt" />
              )}
              <button
                onClick={() => { if (addListId) { store.addChannelFiller(channelId, { filler_list_id: addListId, advancement: addAdv, weight: addWeight }); setAddListId(''); setAddOpen(false) } }}
                disabled={!addListId}
                style={{ padding: '5px 12px', border: 'none', borderRadius: 6, background: 'var(--hds-violet)', color: 'oklch(0.15 0.02 286)', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, fontWeight: 700, cursor: addListId ? 'pointer' : 'default', opacity: addListId ? 1 : 0.4 }}
              >
                Add
              </button>
            </div>
          </div>
        )}

        {store.channelFillerErr && (
          <div style={{ marginTop: 8, fontSize: 11, color: 'oklch(0.72 0.16 22)' }}>{store.channelFillerErr}</div>
        )}
      </div>
    </div>
  )
})

export default ChannelDefaultsPanel
