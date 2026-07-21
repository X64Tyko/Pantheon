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
import styles from './ChannelDetailPage.module.css'

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
    <div className={styles.root}>

      {/* ── Top bar ───────────────────────────────────────────────────────── */}
      <header className={styles.header}>
        <Link to="/channels" className={styles.backLink}>
          <span className={styles.backArrow}>←</span><span>Channels</span>
        </Link>
        <div className={styles.divider} />
        <div className={styles.channelIdentity}>
          <span className={styles.channelNumberBadge}>
            {channel?.number ?? '?'}
          </span>
          <span className={styles.channelName}>
            {channel?.name ?? 'Channel'}
          </span>
          <span className={styles.timezoneBadge}>
            {channel?.timezone ?? 'UTC'}
          </span>
        </div>

        <div className={styles.spacer} />

        <div className={styles.zoomControl}>
          <button onClick={() => store.zoom(-1)} style={zoomBtnStyle}>−</button>
          <span className={styles.zoomLabel}>{zoomPct}</span>
          <button onClick={() => store.zoom(1)} style={zoomBtnStyle}>+</button>
        </div>

        <button
          onClick={() => store.toggleBulkMode()}
          className={`${styles.headerBtn} ${store.bulkMode ? styles.headerBtnActive : styles.headerBtnNeutral}`}
        >
          ⊞ Multi
        </button>

        {store.isDirty && (
          <button
            onClick={() => store.discardChanges(id)}
            disabled={store.channelSaving}
            className={`${styles.headerBtn} ${styles.headerBtnNeutralMuted}`}
          >
            Discard
          </button>
        )}

        <button
          onClick={() => store.saveChannel(id)}
          disabled={store.channelSaving || !store.isDirty}
          className={`${styles.saveBtn} ${store.isDirty ? styles.saveBtnDirty : styles.saveBtnClean}`}
        >
          {store.channelSaving ? 'Saving…' : 'Save Channel'}
        </button>

        <button
          onClick={() => store.clearEpgCache(id)}
          disabled={store.epgClearing}
          title="Delete the committed EPG cache and regenerate from the current cursor position"
          className={`${styles.headerBtn} ${store.epgClearing ? `${styles.headerBtnNeutralMuted} ${styles.headerBtnDisabledCursor}` : styles.headerBtnNeutral}`}
        >
          {store.epgClearing ? 'Clearing…' : 'Regen EPG'}
        </button>

        <button
          onClick={() => store.openNew()}
          className={styles.addBlockBtn}
        >
          <span className={styles.addBlockPlus}>+</span> Add Block
        </button>
      </header>

      {store.error && (
        <div className={styles.errorBanner}>
          {store.error}
        </div>
      )}

      {store.scheduleChanged && !store.channelSaving && (
        <div className={styles.scheduleChangedBanner}>
          <span className={styles.scheduleChangedIcon}>⚠</span>
          Schedule has changed since last save — weekly anchor seeds differ from confirmed. Save Channel to lock in the new schedule.
        </div>
      )}

      {store.channelSaveErr && (
        <div className={styles.errorBanner}>
          Save failed: {store.channelSaveErr}
        </div>
      )}

      {/* ── Body ──────────────────────────────────────────────────────────── */}
      <div className={styles.bodyWrap}>

        {/* Content area — switches between block edit mode and week grid */}
        <div className={styles.contentArea}>

          {editing ? (
            /* ── Block editing mode ─────────────────────────────────────── */
            <>
              <BlockEditMain channelId={id} store={store} />
              <aside className={styles.editorAside}>
                <EditorPanel channelId={id} store={store} />
              </aside>
            </>
          ) : (
            /* ── Default view: week grid + channel/bulk sidebar ─────────── */
            <>
              <div className={styles.gridViewCol}>
                <div ref={scrollRef} className={`${styles.gridScroll} scrollbar-dark`}>
                  <div style={{ minWidth: GUTTER_W + DAY_MIN_W * 7 }}>

                    {/* Sticky day header */}
                    <div className={styles.dayHeaderRow}>
                      <div style={{ width: GUTTER_W, flexShrink: 0 }} />
                      {DAYS.map(([, long]) => (
                        <div key={long} className={styles.dayHeaderLabel} style={{ flex: `1 0 ${DAY_MIN_W}px` }}>
                          {long}
                        </div>
                      ))}
                    </div>

                    {/* Grid body */}
                    <div className={styles.gridBodyRow}>
                      {/* Time gutter */}
                      <div className={styles.timeGutter} style={{ width: GUTTER_W, height: gridH }}>
                        {Array.from({ length: 25 }, (_, h) => (
                          <div key={h} className={styles.timeGutterLabel} style={{ top: h * pph }}>
                            {String(h).padStart(2, '0')}:00
                          </div>
                        ))}
                      </div>
                      {DAYS.map(([, long], di) => (
                        <DayColumn key={long} dayIdx={di} blocks={store.blocks} pph={pph} selectedId={store.selectedId} store={store} channelId={id} enableCreate />
                      ))}
                    </div>

                  </div>
                </div>
              </div>

              <aside className={styles.bulkAside}>
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
