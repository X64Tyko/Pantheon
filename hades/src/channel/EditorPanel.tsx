import { observer } from 'mobx-react-lite'
import { BLOCK_META } from './constants'
import { getLimitMode } from './utils'
import { goldBtnStyle, ghostBtnStyle, dangerBtnStyle, inputStyle } from './styles'
import { EditorForm } from './EditorForm'
import { SlotEditorPanel } from './SlotEditorPanel'
import type { ChannelDetailStore } from './store'
import type { EpisodeOrder, FillerEntryAdvancement } from '../api/types'

// ─── Dynamic sidebar section for the selected item in the active tab ──────────

const SelectedItemSection = observer(function SelectedItemSection({ channelId, store }: {
  channelId: string
  store:     ChannelDetailStore
}) {
  const tab = store.activeBlockTab

  // ── Content item ──
  if (tab === 'content' && store.selectedContentItemId !== null) {
    const item = store.draftContent.find(c => c.id === store.selectedContentItemId)
    if (!item) return null

    const isShow = item.content_type === 'show' || item.content_type === 'episode'
    const upd    = (field: Parameters<typeof store.updateContentField>[2], val: Parameters<typeof store.updateContentField>[3]) =>
      store.updateContentField(channelId, item.id, field, val)

    return (
      <div style={{ padding: '10px 12px 4px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", marginBottom: 8 }}>
          SELECTED ITEM
        </div>
        <div style={{ fontSize: 11, fontWeight: 500, color: 'var(--hds-txt)', marginBottom: 10, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {item.title || item.content_id}
        </div>

        <div style={{ display: 'flex', gap: 8, marginBottom: 8 }}>
          <div style={{ flex: 1 }}>
            <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3 }}>WEIGHT</div>
            <input
              type="number" min={1}
              value={item.weight}
              onChange={e => upd('weight', Math.max(1, Number(e.target.value)))}
              style={{ ...inputStyle, width: '100%', fontSize: 11, padding: '4px 7px' }}
            />
          </div>
          <div style={{ flex: 1 }}>
            <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3 }}>RUN COUNT</div>
            <input
              type="number" min={1}
              value={item.run_count}
              onChange={e => upd('run_count', Math.max(1, Number(e.target.value)))}
              style={{ ...inputStyle, width: '100%', fontSize: 11, padding: '4px 7px' }}
            />
          </div>
        </div>

        {isShow && (
          <div style={{ display: 'flex', gap: 8, marginBottom: 8 }}>
            <div style={{ flex: 1 }}>
              <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3 }}>EPISODE ORDER</div>
              <select
                value={item.episode_order}
                onChange={e => upd('episode_order', e.target.value as EpisodeOrder)}
                style={{ ...inputStyle, width: '100%', fontSize: 11, padding: '4px 7px' }}
              >
                <option value="season">Season</option>
                <option value="absolute">Absolute</option>
                <option value="airdate">Airdate</option>
              </select>
            </div>
            <div style={{ flex: 1, display: 'flex', flexDirection: 'column', justifyContent: 'flex-end' }}>
              <label style={{ display: 'flex', alignItems: 'center', gap: 6, cursor: 'pointer', paddingBottom: 6 }}>
                <input
                  type="checkbox"
                  checked={item.include_specials}
                  onChange={e => upd('include_specials', e.target.checked)}
                />
                <span style={{ fontSize: 10, color: 'var(--hds-txt-2)', letterSpacing: '0.06em' }}>Specials</span>
              </label>
            </div>
          </div>
        )}
      </div>
    )
  }

  // ── Filler entry ──
  if (tab === 'filler' && store.selectedFillerItemId !== null) {
    const entry   = store.draftFillerEntries.find(e => e.id === store.selectedFillerItemId)
    if (!entry) return null

    return (
      <div style={{ padding: '10px 12px 12px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", marginBottom: 8 }}>
          FILLER ENTRY
        </div>
        <div style={{ fontSize: 11, fontWeight: 500, color: 'var(--hds-txt)', marginBottom: 10, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
          {entry.title || entry.content_id}
        </div>

        <div style={{ display: 'flex', gap: 8 }}>
          <div style={{ flex: 1 }}>
            <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3 }}>ADVANCEMENT</div>
            <select
              value={entry.advancement}
              onChange={e => store.updateBlockFiller(entry.id, { advancement: e.target.value as FillerEntryAdvancement })}
              style={{ ...inputStyle, width: '100%', fontSize: 11, padding: '4px 7px' }}
            >
              <option value="sequential">Sequential</option>
              <option value="shuffle">Shuffle</option>
              <option value="sized">Sized</option>
            </select>
          </div>
          <div style={{ width: 70 }}>
            <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3 }}>WEIGHT</div>
            <input
              type="number" min={1}
              value={entry.weight}
              onChange={e => store.updateBlockFiller(entry.id, { weight: Math.max(1, Number(e.target.value)) })}
              style={{ ...inputStyle, width: '100%', fontSize: 11, padding: '4px 7px' }}
            />
          </div>
        </div>
      </div>
    )
  }

  // ── Bumper slot ──
  if (tab === 'bumpers') {
    const slot = store.selectedBumperSlot
    return (
      <div style={{ padding: '10px 12px 12px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ fontSize: 9, letterSpacing: '0.2em', color: 'var(--hds-violet)', fontFamily: "'JetBrains Mono', monospace", marginBottom: 8 }}>
          BUMPERS
        </div>
        {slot ? (
          <div style={{ fontSize: 10.5, color: 'var(--hds-txt-2)', marginBottom: slot === 'interstitial' ? 10 : 0 }}>
            Click content in the browser to assign to <strong style={{ color: 'var(--hds-txt)' }}>{slot.toUpperCase()}</strong>.
          </div>
        ) : (
          <div style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>Select a slot to assign content.</div>
        )}
        {slot === 'interstitial' && (
          <div style={{ marginTop: 8 }}>
            <div style={{ fontSize: 9, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 3 }}>PLAY EVERY N EPISODES</div>
            <input
              type="number" min={1}
              value={store.draft.interstitial_every_n || 1}
              onChange={e => store.setDraft('interstitial_every_n', Math.max(1, Number(e.target.value)))}
              style={{ ...inputStyle, width: 80, fontSize: 11, padding: '4px 7px' }}
            />
          </div>
        )}
      </div>
    )
  }

  return null
})

// ─── Editor panel ─────────────────────────────────────────────────────────────

export const EditorPanel = observer(function EditorPanel({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const d = store.draft
  const m = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>

      {/* Header */}
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '14px 16px 12px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 14, letterSpacing: '0.04em' }}>
          {m.name} Block
        </span>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <button
            onClick={() => store.toggleHints()}
            style={{ padding: '3px 8px', border: '1px solid var(--hds-line)', borderRadius: 5, background: 'transparent', color: store.showHints ? 'var(--hds-violet)' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer', letterSpacing: '0.06em' }}
          >{store.showHints ? '— hints' : '+ hints'}</button>
          <button onClick={() => store.closeEditor()} style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 15 }}>×</button>
        </div>
      </div>

      {/* Slot editor replaces form when a timeslot slot is selected */}
      {store.editingSlotId ? (
        <SlotEditorPanel store={store} />
      ) : (
        <>
          <SelectedItemSection channelId={channelId} store={store} />
          <EditorForm channelId={channelId} store={store} limitMode={limitMode} />
        </>
      )}

      {/* Footer */}
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '14px 16px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        {store.editing ? (
          <>
            <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle} className="hds-btn-gold">
              {store.saving ? 'Saving…' : 'Save Changes'}
            </button>
            <button onClick={() => store.duplicate(channelId)} style={ghostBtnStyle} className="hds-btn-ghost">⧉ Duplicate</button>
            <div style={{ flex: 1 }} />
            <button onClick={() => store.deleteBlock(channelId, store.editing!.block_id)} style={dangerBtnStyle} className="hds-btn-danger">Delete</button>
          </>
        ) : (
          <>
            <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle} className="hds-btn-gold">
              {store.saving ? 'Saving…' : 'Create Block'}
            </button>
            <button onClick={() => store.closeEditor()} style={ghostBtnStyle} className="hds-btn-ghost">Cancel</button>
          </>
        )}
      </div>

      {store.saveErr && (
        <div style={{ padding: '8px 16px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
          {store.saveErr}
        </div>
      )}
    </div>
  )
})
