import { observer } from 'mobx-react-lite'
import { BLOCK_META } from './constants'
import { getLimitMode } from './utils'
import { goldBtnStyle, ghostBtnStyle, dangerBtnStyle } from './styles'
import { EditorForm } from './EditorForm'
import type { ChannelDetailStore } from './store'

// ─── Existing block editor ────────────────────────────────────────────────────

export const BlockEditor = observer(function BlockEditor({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const block = store.editing
  const d     = store.draft
  if (!block) return null

  const m         = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <button onClick={() => { store.modalOpen = true }} title="Expand block editor"
            style={{ display: 'flex', alignItems: 'center', gap: 5, padding: '4px 9px', border: '1px solid var(--hds-violet)', borderRadius: 7, background: 'oklch(0.55 0.14 292 / 0.1)', color: 'var(--hds-violet)', cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, letterSpacing: '0.06em', flexShrink: 0 }}>
            <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"><path d="M1 4V1h3M10 1H7m3 0v3M1 7v3h3M10 10H7m3 0V7" /></svg>
            EXPAND
          </button>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>
            {m.name} Block
          </span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <button
            onClick={() => store.toggleHints()}
            style={{ padding: '3px 8px', border: '1px solid var(--hds-line)', borderRadius: 5, background: 'transparent', color: store.showHints ? 'var(--hds-violet)' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer', letterSpacing: '0.06em' }}
          >{store.showHints ? '— hints' : '+ hints'}</button>
          <button onClick={() => store.closeEditor()} style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 15 }}>×</button>
        </div>
      </div>

      <EditorForm channelId={channelId} store={store} limitMode={limitMode} />

      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '16px 20px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
          {store.saving ? 'Saving…' : 'Save Changes'}
        </button>
        <button onClick={() => store.duplicate(channelId)} style={ghostBtnStyle}>⧉ Duplicate</button>
        <div style={{ flex: 1 }} />
        <button onClick={() => store.deleteBlock(channelId, block.block_id)} style={dangerBtnStyle}>Delete</button>
      </div>

      {store.saveErr && (
        <div style={{ padding: '8px 20px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
          {store.saveErr}
        </div>
      )}
    </div>
  )
})

// ─── New block editor ─────────────────────────────────────────────────────────

export const NewBlockEditor = observer(function NewBlockEditor({ channelId, store }: { channelId: string; store: ChannelDetailStore }) {
  const d         = store.draft
  const m         = BLOCK_META[d.block_type]
  const limitMode = getLimitMode(d)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100%' }}>
      <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '18px 20px 14px', borderBottom: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <button onClick={() => { store.modalOpen = true }} title="Expand block editor"
            style={{ display: 'flex', alignItems: 'center', gap: 5, padding: '4px 9px', border: '1px solid var(--hds-violet)', borderRadius: 7, background: 'oklch(0.55 0.14 292 / 0.1)', color: 'var(--hds-violet)', cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, letterSpacing: '0.06em', flexShrink: 0 }}>
            <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"><path d="M1 4V1h3M10 1H7m3 0v3M1 7v3h3M10 10H7m3 0V7" /></svg>
            EXPAND
          </button>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 15, letterSpacing: '0.04em' }}>
            {m.name} Block
          </span>
        </div>
        <div style={{ display: 'flex', alignItems: 'center', gap: 4 }}>
          <button
            onClick={() => store.toggleHints()}
            style={{ padding: '3px 8px', border: '1px solid var(--hds-line)', borderRadius: 5, background: 'transparent', color: store.showHints ? 'var(--hds-violet)' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9.5, cursor: 'pointer', letterSpacing: '0.06em' }}
          >{store.showHints ? '— hints' : '+ hints'}</button>
          <button onClick={() => store.closeEditor()} style={{ width: 28, height: 28, border: 'none', borderRadius: 7, background: 'transparent', color: 'var(--hds-txt-2)', cursor: 'pointer', fontSize: 15 }}>×</button>
        </div>
      </div>

      <EditorForm channelId={channelId} store={store} limitMode={limitMode} />

      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '16px 20px', borderTop: '1px solid var(--hds-line-s)', flexShrink: 0 }}>
        <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} style={goldBtnStyle}>
          {store.saving ? 'Saving…' : 'Create Block'}
        </button>
        <button onClick={() => store.closeEditor()} style={ghostBtnStyle}>Cancel</button>
      </div>

      {store.saveErr && (
        <div style={{ padding: '8px 20px', fontSize: 11, color: 'oklch(0.72 0.16 22)', background: 'oklch(0.2 0.05 22 / 0.3)', borderTop: '1px solid oklch(0.4 0.1 22 / 0.4)', flexShrink: 0 }}>
          {store.saveErr}
        </div>
      )}
    </div>
  )
})
