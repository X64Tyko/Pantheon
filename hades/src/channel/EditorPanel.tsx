import { observer } from 'mobx-react-lite'
import { BLOCK_META } from './constants'
import { getLimitMode } from './utils'
import { EditorForm } from './EditorForm'
import { SlotEditorPanel } from './SlotEditorPanel'
import type { ChannelDetailStore } from './store'
import type { EpisodeOrder, FillerEntryAdvancement } from '../api/types'
import shared from './sharedStyles.module.css'
import styles from './EditorPanel.module.css'

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
      <div className={`${styles.sidebarSection} ${styles.sidebarSectionTight}`}>
        <div className={styles.sidebarSectionLabel}>
          SELECTED ITEM
        </div>
        <div className={styles.sidebarItemTitle}>
          {item.title || item.content_id}
        </div>

        <div className={styles.fieldRow2}>
          <div className={styles.fieldCol}>
            <div className={styles.miniFieldLabel}>WEIGHT</div>
            <input
              type="number" min={1}
              value={item.weight}
              onChange={e => upd('weight', Math.max(1, Number(e.target.value)))}
              className={`${shared.input} ${styles.miniInput}`}
            />
          </div>
          <div className={styles.fieldCol}>
            <div className={styles.miniFieldLabel}>RUN COUNT</div>
            <input
              type="number" min={1}
              value={item.run_count}
              onChange={e => upd('run_count', Math.max(1, Number(e.target.value)))}
              className={`${shared.input} ${styles.miniInput}`}
            />
          </div>
        </div>

        {isShow && (
          <div className={styles.fieldRow2}>
            <div className={styles.fieldCol}>
              <div className={styles.miniFieldLabel}>EPISODE ORDER</div>
              <select
                value={item.episode_order}
                onChange={e => upd('episode_order', e.target.value as EpisodeOrder)}
                className={`${shared.input} ${styles.miniInput}`}
              >
                <option value="season">Season</option>
                <option value="absolute">Absolute</option>
                <option value="airdate">Airdate</option>
              </select>
            </div>
            <div className={styles.specialsCol}>
              <label className={styles.specialsLabel}>
                <input
                  type="checkbox"
                  checked={item.include_specials}
                  onChange={e => upd('include_specials', e.target.checked)}
                />
                <span className={styles.specialsText}>Specials</span>
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
      <div className={styles.sidebarSection}>
        <div className={styles.sidebarSectionLabel}>
          FILLER ENTRY
        </div>
        <div className={styles.sidebarItemTitle}>
          {entry.title || entry.content_id}
        </div>

        <div className={styles.fieldRowNoGap}>
          <div className={styles.fieldCol}>
            <div className={styles.miniFieldLabel}>ADVANCEMENT</div>
            <select
              value={entry.advancement}
              onChange={e => store.updateBlockFiller(entry.id, { advancement: e.target.value as FillerEntryAdvancement })}
              className={`${shared.input} ${styles.miniInput}`}
            >
              <option value="sequential">Sequential</option>
              <option value="shuffle">Shuffle</option>
              <option value="sized">Sized</option>
            </select>
          </div>
          <div className={styles.fieldCol70}>
            <div className={styles.miniFieldLabel}>WEIGHT</div>
            <input
              type="number" min={1}
              value={entry.weight}
              onChange={e => store.updateBlockFiller(entry.id, { weight: Math.max(1, Number(e.target.value)) })}
              className={`${shared.input} ${styles.miniInput}`}
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
      <div className={styles.sidebarSection}>
        <div className={styles.sidebarSectionLabel}>
          BUMPERS
        </div>
        {slot ? (
          <div className={`${styles.bumperAssignText} ${slot === 'interstitial' ? styles.bumperAssignTextGap : ''}`}>
            Click content in the browser to assign to <strong className={styles.bumperAssignStrong}>{slot.toUpperCase()}</strong>.
          </div>
        ) : (
          <div className={styles.bumperNoSlotText}>Select a slot to assign content.</div>
        )}
        {slot === 'interstitial' && (
          <div className={styles.interstitialWrap}>
            <div className={styles.miniFieldLabel}>PLAY EVERY N EPISODES</div>
            <input
              type="number" min={1}
              value={store.draft.interstitial_every_n || 1}
              onChange={e => store.setDraft('interstitial_every_n', Math.max(1, Number(e.target.value)))}
              className={`${shared.input} ${styles.interstitialInput}`}
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
    <div className={styles.panelRoot}>

      {/* Header */}
      <div className={styles.header}>
        <span className={styles.headerTitleSpan}>
          <input type="text" value={d.name != '' ? d.name : m.name + ' Block'} onChange={e => store.setBlockName(e.target.value)} />
        </span>
        <div className={styles.headerActions}>
          <button
            onClick={() => store.toggleHints()}
            className={`${styles.hintsToggleBtn} ${store.showHints ? styles.hintsToggleBtnActive : ''}`}
          >{store.showHints ? '— hints' : '+ hints'}</button>
          <button onClick={() => store.closeEditor()} className={styles.closeBtn}>×</button>
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
      <div className={styles.footer}>
        {store.editing ? (
          <>
            <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} className={`${shared.goldBtn} hds-btn-gold`}>
              {store.saving ? 'Saving…' : 'Save Changes'}
            </button>
            <button onClick={() => store.duplicate(channelId)} className={`${shared.ghostBtn} hds-btn-ghost`}>⧉ Duplicate</button>
            <div className={styles.footerSpacer} />
            <button onClick={() => store.deleteBlock(channelId, store.editing!.block_id)} className={`${shared.dangerBtn} hds-btn-danger`}>Delete</button>
          </>
        ) : (
          <>
            <button onClick={() => store.save(channelId)} disabled={store.saving || d.day_mask === 0} className={`${shared.goldBtn} hds-btn-gold`}>
              {store.saving ? 'Saving…' : 'Create Block'}
            </button>
            <button onClick={() => store.closeEditor()} className={`${shared.ghostBtn} hds-btn-ghost`}>Cancel</button>
          </>
        )}
      </div>

      {store.saveErr && (
        <div className={styles.errorBanner}>
          {store.saveErr}
        </div>
      )}
    </div>
  )
})
