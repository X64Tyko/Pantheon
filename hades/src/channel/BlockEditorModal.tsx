import { useEffect } from 'react'
import { observer } from 'mobx-react-lite'
import { BLOCK_META } from './constants'
import { getLimitMode } from './utils'
import { goldBtnStyle, ghostBtnStyle, dangerBtnStyle } from './styles'
import { EditorForm } from './EditorForm'
import { LibraryBrowser } from './LibraryBrowser'
import type { ChannelDetailStore } from './store'

const BlockEditorModal = observer(function BlockEditorModal({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const d         = store.draft
  const m         = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  useEffect(() => {
    store.openPicker()
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') store.modalOpen = false }
    document.addEventListener('keydown', esc)
    return () => document.removeEventListener('keydown', esc)
  }, [])

  return (
    <div
      style={{ position: 'fixed', inset: 0, zIndex: 200, display: 'flex', alignItems: 'center', justifyContent: 'center', background: 'oklch(0.08 0.015 286 / 0.85)' }}
      onClick={e => { if (e.target === e.currentTarget) store.modalOpen = false }}
    >
      <div style={{ display: 'flex', flexDirection: 'column', width: 'min(98vw, 1340px)', height: '92vh', background: 'var(--hds-bg-2)', borderRadius: 14, border: '1px solid var(--hds-line)', boxShadow: '0 32px 80px -16px rgba(0,0,0,0.8)', overflow: 'hidden', fontFamily: "'JetBrains Mono', monospace", fontSize: 13, color: 'var(--hds-txt)' }}>

        {/* Header */}
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '16px 22px 13px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 16, letterSpacing: '0.04em' }}>
            {store.isNewMode ? 'New' : m.name} Block
          </span>
          <button onClick={() => { store.modalOpen = false }} style={{ width: 30, height: 30, border: 'none', borderRadius: 8, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 16 }}>×</button>
        </div>

        {/* Two-column body */}
        <div style={{ display: 'flex', flex: 1, minHeight: 0 }}>
          {/* Left: editor form */}
          <div style={{ width: 440, flexShrink: 0, borderRight: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', overflow: 'hidden' }}>
            <EditorForm channelId={channelId} store={store} limitMode={limitMode} hidePicker />
          </div>

          {/* Right: library browser */}
          <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column' }}>
            <LibraryBrowser channelId={channelId} store={store} />
          </div>
        </div>

        {/* Footer */}
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '14px 22px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
          {store.editing ? (
            <>
              <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
                {store.saving ? 'Saving…' : 'Save Changes'}
              </button>
              <button onClick={() => store.duplicate(channelId)} style={ghostBtnStyle}>⧉ Duplicate</button>
              <div style={{ flex: 1 }} />
              <button onClick={() => store.deleteBlock(channelId, store.editing!.block_id)} style={dangerBtnStyle}>Delete</button>
            </>
          ) : (
            <>
              <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
                {store.saving ? 'Saving…' : 'Create Block'}
              </button>
              <button onClick={() => { store.closeEditor(); store.modalOpen = false }} style={ghostBtnStyle}>Cancel</button>
            </>
          )}
        </div>

        {store.saveErr && (
          <div style={{ padding: '7px 22px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
            {store.saveErr}
          </div>
        )}
      </div>
    </div>
  )
})

export default BlockEditorModal
