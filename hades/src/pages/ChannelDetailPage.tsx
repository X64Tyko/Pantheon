import { observer } from 'mobx-react-lite'
import { useEffect, useRef } from 'react'
import { useParams, Link } from 'react-router-dom'
import { channelStore } from '../stores'
import { store } from '../channel/store'
import { DAYS, GUTTER_W, DAY_MIN_W, PPH_DEFAULT } from '../channel/constants'
import { zoomBtnStyle } from '../channel/styles'
import DayColumn from '../channel/DayColumn'
import EpgPreview, { EpgErrorBoundary } from '../channel/EpgPreview'
import ChannelDefaultsPanel from '../channel/ChannelDefaultsPanel'
import { EditorPanel } from '../channel/EditorPanel'
import { BlockEditMain } from '../channel/BlockEditMain'
import { BulkEditPanel } from '../channel/BulkEditPanel'
import ChannelFillerOverlay from '../channel/ChannelFillerOverlay'
import ChannelBumperOverlay from '../channel/ChannelBumperOverlay'
import type { Block } from '../api/types'

export default observer(function ChannelDetailPage() {
  const { id } = useParams<{ id: string }>()
  const channel   = channelStore.channels.find(c => c.channel_id === id)
  const scrollRef = useRef<HTMLDivElement>(null)

  useEffect(() => {
    if (!id) return
    if (channelStore.channels.length === 0) channelStore.fetchAll()
    store.closeEditor()
    store.load(id).then(() => {
      setTimeout(() => {
        if (scrollRef.current) scrollRef.current.scrollTop = Math.round(15.5 * store.pxPerHour)
      }, 80)
    })
  }, [id])

  useEffect(() => {
    if (channel) store.initChannelDraft(channel)
  }, [channel?.channel_id, channel?.seed, channel?.name, channel?.number, channel?.timezone, channel?.audio_lang, channel?.subtitle_lang])

  useEffect(() => {
    const up  = () => store.stopPainting()
    const esc = (e: KeyboardEvent) => { if (e.key === 'Escape') store.closeEditor() }
    window.addEventListener('mouseup', up)
    document.addEventListener('keydown', esc)
    return () => { window.removeEventListener('mouseup', up); document.removeEventListener('keydown', esc) }
  }, [])

  if (!id) return null

  const pph     = store.pxPerHour
  const gridH   = 24 * pph
  const zoomPct = Math.round(pph / PPH_DEFAULT * 100) + '%'

  const editing = store.selectedId !== null || store.isNewMode

  // Merge the active draft into blocks so the EPG preview reacts to unsaved changes.
  const epgBlocks: Block[] = (() => {
    if (store.editing) {
      return store.blocks.map(b =>
        b.block_id === store.editing!.block_id ? { ...b, ...store.draft } : b
      )
    }
    if (store.isNewMode) {
      const virtual: Block = {
        block_id: '_draft_', channel_id: id,
        content: store.draftContent, filler_entries: store.draftFillerEntries,
        ...store.draft,
      }
      return [...store.blocks, virtual]
    }
    return store.blocks
  })()

  return (
    <div style={{ display: 'flex', flexDirection: 'column', height: '100vh', overflow: 'hidden', background: 'var(--hds-bg)', fontFamily: "'JetBrains Mono', monospace", fontSize: 13, letterSpacing: '0.01em', color: 'var(--hds-txt)' }}>

      {/* ── Top bar ───────────────────────────────────────────────────────── */}
      <header style={{ display: 'flex', alignItems: 'center', gap: 18, padding: '14px 24px', borderBottom: '1px solid var(--hds-line-s)', background: 'oklch(0.17 0.018 286 / 0.6)', flexShrink: 0 }}>
        <Link to="/channels" style={{ display: 'flex', alignItems: 'center', gap: 7, color: 'var(--hds-txt-2)', textDecoration: 'none', padding: '6px 9px', borderRadius: 7 }}>
          <span style={{ fontSize: 14 }}>←</span><span>Channels</span>
        </Link>
        <div style={{ width: 1, height: 22, background: 'var(--hds-line-s)' }} />
        <div style={{ display: 'flex', alignItems: 'center', gap: 11 }}>
          <span style={{ display: 'inline-flex', alignItems: 'center', justifyContent: 'center', minWidth: 28, height: 28, padding: '0 8px', borderRadius: 7, background: 'var(--hds-bg-3)', color: 'var(--hds-gold)', fontWeight: 700, fontSize: 13, boxShadow: 'inset 0 0 0 1px var(--hds-line)' }}>
            {channel?.number ?? '?'}
          </span>
          <span style={{ fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 18, letterSpacing: '0.01em' }}>
            {channel?.name ?? 'Channel'}
          </span>
          <span style={{ fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-3)', padding: '3px 7px', border: '1px solid var(--hds-line-s)', borderRadius: 5 }}>
            {channel?.timezone ?? 'UTC'}
          </span>
        </div>

        <div style={{ flex: 1 }} />

        <div style={{ display: 'flex', alignItems: 'center', gap: 2, background: 'var(--hds-bg-3)', border: '1px solid var(--hds-line-s)', borderRadius: 9, padding: 3 }}>
          <button onClick={() => store.zoom(-1)} style={zoomBtnStyle}>−</button>
          <span style={{ minWidth: 52, textAlign: 'center', fontSize: 11, color: 'var(--hds-txt-2)', letterSpacing: '0.06em' }}>{zoomPct}</span>
          <button onClick={() => store.zoom(1)} style={zoomBtnStyle}>+</button>
        </div>

        <button
          onClick={() => store.toggleBulkMode()}
          style={{ display: 'flex', alignItems: 'center', gap: 7, padding: '9px 14px', border: `1px solid ${store.bulkMode ? 'var(--hds-violet)' : 'var(--hds-line)'}`, borderRadius: 9, background: store.bulkMode ? 'oklch(0.38 0.09 287 / 0.25)' : 'transparent', color: store.bulkMode ? 'var(--hds-txt)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: 'pointer', transition: '.12s' }}
        >
          ⊞ Multi
        </button>

        {store.isDirty && (
          <button
            onClick={() => store.discardChanges(id)}
            disabled={store.channelSaving}
            style={{ padding: '9px 14px', border: '1px solid var(--hds-line)', borderRadius: 9, background: 'transparent', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: 'pointer', transition: '.12s' }}
          >
            Discard
          </button>
        )}

        <button
          onClick={() => store.saveChannel(id)}
          disabled={store.channelSaving || !store.isDirty}
          style={{ display: 'flex', alignItems: 'center', gap: 7, padding: '9px 16px', border: 'none', borderRadius: 9, background: store.isDirty ? 'oklch(0.42 0.13 145)' : 'oklch(0.28 0.04 145)', color: store.isDirty ? 'oklch(0.95 0.03 145)' : 'oklch(0.5 0.04 145)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: store.isDirty ? 'pointer' : 'default', transition: '.15s', boxShadow: store.isDirty ? '0 4px 16px -4px oklch(0.42 0.13 145 / 0.5)' : 'none' }}
        >
          {store.channelSaving ? 'Saving…' : 'Save Channel'}
        </button>

        <button
          onClick={() => store.clearEpgCache(id)}
          disabled={store.epgClearing}
          title="Delete the committed EPG cache and regenerate from the current cursor position"
          style={{ padding: '9px 14px', border: '1px solid var(--hds-line)', borderRadius: 9, background: 'transparent', color: store.epgClearing ? 'var(--hds-txt-3)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 12, cursor: store.epgClearing ? 'default' : 'pointer', transition: '.12s' }}
        >
          {store.epgClearing ? 'Clearing…' : 'Regen EPG'}
        </button>

        <button
          onClick={() => store.openNew()}
          style={{ display: 'flex', alignItems: 'center', gap: 7, padding: '9px 16px', border: 'none', borderRadius: 9, background: 'linear-gradient(180deg, var(--hds-gold), var(--hds-gold-2))', color: 'oklch(0.2 0.04 70)', fontFamily: "'Chakra Petch', sans-serif", fontWeight: 700, fontSize: 13, cursor: 'pointer', boxShadow: '0 4px 16px -4px oklch(0.83 0.13 84 / 0.5)' }}
        >
          <span style={{ fontSize: 15, lineHeight: 1 }}>+</span> Add Block
        </button>
      </header>

      {store.error && (
        <div style={{ padding: '10px 24px', background: 'oklch(0.2 0.05 22 / 0.3)', borderBottom: '1px solid oklch(0.4 0.1 22 / 0.4)', color: 'oklch(0.72 0.16 22)', fontSize: 12, flexShrink: 0 }}>
          {store.error}
        </div>
      )}

      {store.scheduleChanged && !store.channelSaving && (
        <div style={{ padding: '9px 24px', background: 'oklch(0.22 0.07 76 / 0.4)', borderBottom: '1px solid oklch(0.45 0.1 76 / 0.5)', color: 'oklch(0.82 0.12 80)', fontSize: 11, letterSpacing: '0.03em', flexShrink: 0, display: 'flex', alignItems: 'center', gap: 10 }}>
          <span style={{ opacity: 0.7 }}>⚠</span>
          Schedule has changed since last save — weekly anchor seeds differ from confirmed. Save Channel to lock in the new schedule.
        </div>
      )}

      {store.channelSaveErr && (
        <div style={{ padding: '9px 24px', background: 'oklch(0.2 0.05 22 / 0.3)', borderBottom: '1px solid oklch(0.4 0.1 22 / 0.4)', color: 'oklch(0.72 0.16 22)', fontSize: 11, flexShrink: 0 }}>
          Save failed: {store.channelSaveErr}
        </div>
      )}

      {/* ── Body ──────────────────────────────────────────────────────────── */}
      <div style={{ flex: 1, minHeight: 0, display: 'flex', flexDirection: 'column' }}>

        {/* Content area — switches between block edit mode and week grid */}
        <div style={{ flex: 1, minHeight: 0, display: 'flex' }}>

          {editing ? (
            /* ── Block editing mode ─────────────────────────────────────── */
            <>
              <BlockEditMain channelId={id} store={store} />
              <aside style={{ flexShrink: 0, width: 360, borderLeft: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-2)', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
                <EditorPanel channelId={id} store={store} />
              </aside>
            </>
          ) : (
            /* ── Default view: week grid + channel/bulk sidebar ─────────── */
            <>
              <div style={{ flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
                <div ref={scrollRef} style={{ flex: 1, minHeight: 0, overflow: 'auto' }} className="scrollbar-dark">
                  <div style={{ minWidth: GUTTER_W + DAY_MIN_W * 7 }}>

                    {/* Sticky day header */}
                    <div style={{ display: 'flex', position: 'sticky', top: 0, zIndex: 25, borderBottom: '1px solid var(--hds-line-s)', background: 'var(--hds-bg)' }}>
                      <div style={{ width: GUTTER_W, flexShrink: 0 }} />
                      {DAYS.map(([, long]) => (
                        <div key={long} style={{ flex: `1 0 ${DAY_MIN_W}px`, textAlign: 'center', padding: '11px 0', fontSize: 10, letterSpacing: '0.24em', color: 'var(--hds-txt-2)', borderLeft: '1px solid var(--hds-line-s)' }}>
                          {long}
                        </div>
                      ))}
                    </div>

                    {/* Grid body */}
                    <div style={{ display: 'flex' }}>
                      {/* Time gutter */}
                      <div style={{ width: GUTTER_W, flexShrink: 0, position: 'relative', height: gridH }}>
                        {Array.from({ length: 25 }, (_, h) => (
                          <div key={h} style={{ position: 'absolute', top: h * pph, right: 9, transform: 'translateY(-50%)', fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>
                            {String(h).padStart(2, '0')}:00
                          </div>
                        ))}
                      </div>
                      {DAYS.map(([, long], di) => (
                        <DayColumn key={long} dayIdx={di} blocks={store.blocks} pph={pph} selectedId={store.selectedId} store={store} channelId={id} />
                      ))}
                    </div>

                  </div>
                </div>
              </div>

              <aside style={{ flexShrink: 0, width: 392, borderLeft: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-2)', display: 'flex', flexDirection: 'column', minHeight: 0 }}>
                {store.bulkMode ? (
                  <BulkEditPanel channelId={id} store={store} />
                ) : (
                  <ChannelDefaultsPanel channel={channel} channelId={id} store={store} />
                )}
              </aside>
            </>
          )}
        </div>

        {/* EPG preview — always visible at bottom */}
        <EpgErrorBoundary>
          <EpgPreview
            blocks={epgBlocks}
            epgItems={store.epgItems}
            epgLoading={store.epgLoading}
            epgDay={store.epgDay}
            timezone={channel?.timezone ?? 'UTC'}
            onDay={d => { store.epgDay = d }}
            onRefresh={() => store.loadEpg(id)}
            onSelectBlock={blockId => store.select(blockId)}
          />
        </EpgErrorBoundary>
      </div>

      {store.channelFillerOverlayOpen && channel && (
        <ChannelFillerOverlay channelId={id} channel={channel} store={store} />
      )}
      {store.channelBumperOverlayOpen && (
        <ChannelBumperOverlay channelId={id} store={store} />
      )}
    </div>
  )
})
