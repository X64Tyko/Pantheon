import { observer } from 'mobx-react-lite'
import type { ReactNode } from 'react'
import type { Advancement, BlockType, CursorScope, EpisodeOrder, FillerSelectionMode, NoHistoryBehavior } from '../api/types'
import {
  ALIGN_OPTS, BLOCK_META, DAYS, DAY_BITS, DELAY_OPTS, EARLY_OPTS,
  FILLER_ADV_OPTS, FILLER_SEL_OPTS, NO_HISTORY_OPTS, RATINGS,
} from './constants'
import { getLimitMode } from './utils'
import { inputStyle } from './styles'
import { FillerEntryRow, FillerAddPanel } from './FillerPanel'
import { ContentPicker } from './ContentPicker'
import { api } from '../api/client'
import type { ChannelDetailStore } from './store'
import type { LimitMode } from './types'

// ─── Accordion section ────────────────────────────────────────────────────────

export function AccordionSection({ title, badge, open, onToggle, children }: {
  title:    string
  badge?:   ReactNode
  open:     boolean
  onToggle: () => void
  children: ReactNode
}) {
  return (
    <div style={{ borderRadius: 9, border: '1px solid var(--hds-line-s)', marginBottom: 8, overflow: 'hidden' }}>
      <button
        onClick={onToggle}
        style={{ display: 'flex', alignItems: 'center', gap: 10, width: '100%', padding: '9px 13px', background: 'oklch(0.2 0.018 286 / 0.6)', border: 'none', cursor: 'pointer' }}
      >
        <span style={{ fontSize: 9, color: 'var(--hds-txt-3)', display: 'inline-block', transition: 'transform .15s', transform: open ? 'rotate(90deg)' : 'none' }}>▶</span>
        <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", textAlign: 'left' }}>{title}</span>
        {badge}
      </button>
      {open && (
        <div style={{ padding: '14px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
          {children}
        </div>
      )}
    </div>
  )
}

// ─── Editor form ──────────────────────────────────────────────────────────────

export const EditorForm = observer(function EditorForm({ channelId, store, limitMode }: {
  channelId: string
  store:     ChannelDetailStore
  limitMode: LimitMode
}) {
  const d   = store.draft
  const m   = BLOCK_META[d.block_type]
  const sec = store.openSections
  const tog = (s: string) => store.toggleSection(s)

  const limitHelp = limitMode === 'programs'
    ? 'Plays this many programs then yields. End time flexes with real runtime.'
    : limitMode === 'end'
    ? 'Hard cutoff at end time. Whatever is playing is cut off when the clock hits it.'
    : 'Fills until midnight, or until a higher-priority block takes over.'

  const orderHint: Record<string, string> = {
    sequential:    'Plays shows in order by position. COUNT on each show controls episodes before switching to the next.',
    shuffle:       'Picks shows randomly each slot, weighted by WEIGHT. Episodes advance sequentially within each show.',
    smart_shuffle: 'Weighted random show selection, but skips recently played episodes within each show.',
    rerun_shuffle: 'Only plays episodes already aired on this channel. Requires play history to function.',
    rerun_smart:   'Rerun pool with cooldown — avoids repeating episodes aired recently within the pool.',
  }

  const cursorHint: Record<string, string> = {
    block:   'Episode positions are tracked per block. The same show in two blocks plays independently.',
    channel: 'All blocks on this channel share episode positions for the same show.',
    global:  'Positions and rerun history are shared across all channels — a true cross-channel pool.',
  }

  const contentCount = store.draftContent.length
  const fillerCount  = store.draftFillerEntries.length

  const isRerun = d.advancement === 'rerun_shuffle' || d.advancement === 'rerun_smart'
  const premierBlocks = store.blocks.filter(b => b.block_type === 'premier')
  const newPremierCount = store.editing ? store.draftContent.filter(c =>
    c.content_type === 'show' && c.id > 0 &&
    !premierBlocks.some(pb => pb.content.some(pc => pc.content_type === 'show' && pc.content_id === c.content_id))
  ).length : 0

  return (
    <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '12px 12px 20px' }} className="scrollbar-dark">

      {/* ── Block name ── */}
      <input
        type="text"
        placeholder="Block name (optional)"
        value={d.name ?? ''}
        onChange={e => store.setDraft('name', e.target.value)}
        style={{ ...inputStyle, width: '100%', marginBottom: 10 }}
      />

      {/* ── SCHEDULE ── */}
      <AccordionSection title="SCHEDULE" open={sec.schedule} onToggle={() => tog('schedule')}>
        <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7 }}>BLOCK TYPE</div>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(4,1fr)', gap: 6, marginBottom: 16 }}>
          {(['episode', 'movie', 'premier', 'filler'] as BlockType[]).map(t => {
            const tm = BLOCK_META[t]
            const on = d.block_type === t
            return (
              <button key={t} onClick={() => store.setDraft('block_type', t)} style={{ padding: '8px 4px', border: `1px solid ${on ? tm.edge : 'var(--hds-line)'}`, borderRadius: 8, background: on ? tm.solid : 'var(--hds-bg-3)', color: on ? 'var(--hds-txt)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontWeight: 600, fontSize: 11.5, cursor: 'pointer', transition: '.12s' }}>
                {tm.name}
              </button>
            )
          })}
        </div>

        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 7 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)' }}>DAYS</div>
          <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)' }}>drag to paint</span>
        </div>
        <div style={{ display: 'flex', gap: 5, marginBottom: 7, userSelect: 'none' }}>
          {DAYS.map(([short], i) => {
            const bit = DAY_BITS[i]
            const on  = (d.day_mask & bit) !== 0
            return (
              <div key={short} onMouseDown={() => store.dayDown(i)} onMouseEnter={() => store.dayEnter(i)}
                style={{ flex: 1, textAlign: 'center', padding: '9px 0', border: `1px solid ${on ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`, borderRadius: 7, background: on ? 'oklch(0.55 0.1 84 / 0.22)' : 'var(--hds-bg-3)', color: on ? 'var(--hds-gold)' : 'var(--hds-txt-2)', fontWeight: 700, fontSize: 11, cursor: 'pointer', transition: '.1s' }}>
                {short}
              </div>
            )
          })}
        </div>
        <div style={{ display: 'flex', gap: 6, flexWrap: 'wrap' }}>
          {[['Weekdays', 62], ['Weekend', 65], ['Every day', 127], ['Clear', 0]].map(([label, mask]) => (
            <button key={label} onClick={() => store.setDraft('day_mask', mask as number)} style={{ padding: '4px 9px', border: '1px solid var(--hds-line)', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, cursor: 'pointer' }}>
              {label}
            </button>
          ))}
        </div>
      </AccordionSection>

      {/* ── TIMING ── */}
      <AccordionSection title="TIMING" open={sec.timing} onToggle={() => tog('timing')}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 9, marginBottom: 16 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>START TIME</div>
            <input type="time" value={d.start_time} onChange={e => store.setDraft('start_time', e.target.value)} style={inputStyle} />
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>LATE START</div>
            <select value={String(d.late_start_mins)} onChange={e => store.setDraft('late_start_mins', +e.target.value)} style={inputStyle}>
              {DELAY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {d.late_start_mins > 0 && (
              <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
                Block may start up to {d.late_start_mins} min late if preempted by a higher-priority block.
              </div>
            )}
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>EARLY START</div>
            <select value={String(d.early_start_secs)} onChange={e => store.setDraft('early_start_secs', +e.target.value)} style={inputStyle}>
              {EARLY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {d.early_start_secs > 0 && (
              <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
                Block may steal up to {d.early_start_secs}s of trailing dead air from the previous block.
              </div>
            )}
          </div>
        </div>

        <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7 }}>STOP CONDITION</div>
        <div style={{ display: 'grid', gridTemplateColumns: 'repeat(3,1fr)', gap: 6, marginBottom: 9 }}>
          {([['programs', '# Programs'], ['end', 'End time'], ['fill', 'Fill day']] as [LimitMode, string][]).map(([k, label]) => {
            const on = limitMode === k
            return (
              <button key={k} onClick={() => store.setLimitMode(k)} style={{ padding: '8px 4px', border: `1px solid ${on ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`, borderRadius: 8, background: on ? 'oklch(0.55 0.1 84 / 0.2)' : 'var(--hds-bg-3)', color: on ? 'var(--hds-gold)' : 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontWeight: 600, fontSize: 11, cursor: 'pointer', transition: '.12s' }}>
                {label}
              </button>
            )
          })}
        </div>
        {limitMode === 'programs' && (
          <input type="number" min={1} value={d.program_count} onChange={e => store.setDraft('program_count', Math.max(1, +e.target.value || 1))} placeholder="number of programs" style={{ ...inputStyle, width: '100%', marginBottom: 7 }} />
        )}
        {limitMode === 'end' && (
          <input type="time" value={d.end_time ?? ''} onChange={e => store.setDraft('end_time', e.target.value)} style={{ ...inputStyle, width: '100%', marginBottom: 7 }} />
        )}
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 14, lineHeight: 1.55 }}>{limitHelp}</div>

        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 5 }}>
          <div style={{ fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)' }}>ALIGN START</div>
          {d.align_to_mins > 0 && (
            <div style={{ display: 'flex', border: '1px solid var(--hds-line)', borderRadius: 6, overflow: 'hidden' }}>
              {(['block', 'episode'] as const).map(scope => {
                const on = (d.start_scope ?? 'block') === scope
                return (
                  <button key={scope} onClick={() => store.setDraft('start_scope', scope)}
                    style={{ padding: '3px 8px', border: 'none', background: on ? 'var(--hds-violet)' : 'var(--hds-bg-3)', color: on ? '#fff' : 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", fontSize: 9, cursor: 'pointer', textTransform: 'uppercase', letterSpacing: '0.08em' }}>
                    {scope}
                  </button>
                )
              })}
            </div>
          )}
        </div>
        <select value={String(d.align_to_mins)} onChange={e => store.setDraft('align_to_mins', +e.target.value)} style={{ ...inputStyle, width: '100%', marginBottom: 6 }}>
          {ALIGN_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', lineHeight: 1.55 }}>
          {(d.start_scope ?? 'block') === 'episode'
            ? 'Snaps each episode to the next time boundary. Early/late start define the tolerance window.'
            : 'Snaps the first program of the block to the next time boundary.'}
        </div>
      </AccordionSection>

      {/* ── PLAYBACK ── */}
      <AccordionSection title="PLAYBACK" open={sec.playback} onToggle={() => tog('playback')}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9, marginBottom: 14 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>PRIORITY</div>
            <input type="number" min={1} value={d.priority} onChange={e => store.setDraft('priority', Math.max(1, +e.target.value || 1))} style={inputStyle} />
            <div style={{ fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 4 }}>higher wins conflicts</div>
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>MAX RATING</div>
            <select value={d.max_content_rating} onChange={e => store.setDraft('max_content_rating', e.target.value)} style={inputStyle}>
              {RATINGS.map(r => <option key={r} value={r}>{r || 'No limit'}</option>)}
            </select>
          </div>
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>ORDER</div>
            <select value={d.advancement} onChange={e => store.setDraft('advancement', e.target.value as Advancement)} style={inputStyle}>
              <optgroup label="Standard">
                <option value="sequential">Sequential</option>
                {d.block_type !== 'premier' && <option value="shuffle">Shuffle</option>}
                {d.block_type !== 'premier' && <option value="smart_shuffle">Smart Shuffle</option>}
              </optgroup>
              <optgroup label="Reruns">
                <option value="rerun_shuffle">Rerun Shuffle</option>
                <option value="rerun_smart">Rerun Smart</option>
              </optgroup>
            </select>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
              {orderHint[d.advancement]}
            </div>
          </div>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>CURSOR</div>
            <select value={d.cursor_scope} onChange={e => store.setDraft('cursor_scope', e.target.value as CursorScope)} style={inputStyle}>
              <option value="block">Per block</option>
              <option value="channel">Per channel</option>
              <option value="global">Global</option>
            </select>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
              {cursorHint[d.cursor_scope]}
            </div>
          </div>
        </div>
        {(d.advancement === 'smart_shuffle' || d.advancement === 'rerun_smart') && (
          <div style={{ marginTop: 9 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>COOLDOWN THRESHOLD</div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <input type="range" min={5} max={80} step={5}
                value={d.smart_pct ?? 30}
                onChange={e => store.setDraft('smart_pct', Number(e.target.value))}
                style={{ flex: 1 }} />
              <span style={{ fontSize: 11, color: 'var(--hds-txt-2)', minWidth: 36 }}>{d.smart_pct ?? 30}%</span>
            </div>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 3 }}>
              Episodes won't repeat until {d.smart_pct ?? 30}% of the pool has played since last air
            </div>
          </div>
        )}
        {(d.advancement === 'rerun_shuffle' || d.advancement === 'rerun_smart') && (
          <div style={{ marginTop: 9 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>NO HISTORY BEHAVIOR</div>
            <select
              value={d.no_history_behavior ?? 'normal'}
              onChange={e => store.setDraft('no_history_behavior', e.target.value as NoHistoryBehavior)}
              style={inputStyle}
            >
              {NO_HISTORY_OPTS.map(([v, label]) => <option key={v} value={v}>{label}</option>)}
            </select>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 3 }}>
              {NO_HISTORY_OPTS.find(([v]) => v === (d.no_history_behavior ?? 'normal'))?.[2]}
            </div>
          </div>
        )}
        {(d.advancement === 'rerun_shuffle' || d.advancement === 'rerun_smart') && (
          <div style={{ marginTop: 9 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>EPISODE LIMIT</div>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
              <input type="number" min={0} step={1}
                value={d.max_consecutive_episodes ?? 0}
                onChange={e => store.setDraft('max_consecutive_episodes', Math.max(0, parseInt(e.target.value) || 0))}
                style={{ ...inputStyle, width: 64 }} />
              <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)' }}>
                {(d.max_consecutive_episodes ?? 0) === 0 ? 'no limit' : 'max consecutive from the same show'}
              </span>
            </div>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
              When the limit is hit the engine re-rolls show selection, forcing a switch even if the same show wins.
            </div>
          </div>
        )}
      </AccordionSection>

      {/* ── CONTENT ── */}
      <AccordionSection
        title="CONTENT"
        open={sec.content}
        onToggle={() => tog('content')}
        badge={contentCount > 0 ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>{contentCount}</span> : undefined}
      >
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 9 }}>
          <span />
          {(store.editing || store.isNewMode) && (
            <button onClick={() => store.openPicker()} style={{ color: 'var(--hds-violet)', background: 'transparent', border: 'none', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', padding: '2px 4px' }}>+ Add</button>
          )}
        </div>

        {store.pickerOpen && (store.editing || store.isNewMode) && (
          <ContentPicker channelId={channelId} store={store} />
        )}

        <div
          onDragOver={e => e.preventDefault()}
          onDrop={e => { e.preventDefault(); if (store.dragItem) { const sh = store.pickerShows.find(s => s.show_id === store.dragItem); store.addContent(channelId, { content_type: 'show', content_id: store.dragItem!, title: sh?.title }); store.dragItem = null } }}
          style={{ display: 'flex', flexDirection: 'column', gap: 6, minHeight: 40, padding: store.dragItem ? 8 : 0, border: store.dragItem ? '1px dashed var(--hds-violet)' : '1px solid transparent', borderRadius: 9, transition: '.12s', marginTop: store.pickerOpen ? 10 : 0 }}
        >
          {store.draftContent.map(item => {
            const dot       = BLOCK_META[item.content_type === 'movie' ? 'movie' : 'episode'].edge
            const canReset  = item.id > 0 && item.content_type === 'show' && !!store.editing
            const isShuffle    = store.draft.advancement === 'shuffle'       || store.draft.advancement === 'smart_shuffle'
            const isSequential = store.draft.advancement === 'sequential'
            const showWeightControl = (isRerun || isShuffle || isSequential) && item.content_type === 'show'
            const weightLabel  = isSequential ? 'COUNT' : 'WEIGHT'
            const miniInp: React.CSSProperties = { width: 36, padding: '2px 4px', background: 'var(--hds-bg)', border: '1px solid var(--hds-line)', borderRadius: 4, color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, textAlign: 'center' }
            const isPlaylist    = item.content_type === 'playlist'
            const playlist      = isPlaylist ? store.contentPlaylists.find(p => p.playlist_id === item.content_id) : undefined
            const playlistMode  = playlist?.mode ?? 'sequential'
            const isShowColl    = playlistMode === 'show_collection'
            const thumbUrl = item.content_type === 'show' ? `/api/shows/${item.content_id}/thumb`
                           : item.content_type === 'movie' ? `/api/movies/${item.content_id}/thumb`
                           : null
            return (
              <div key={item.id} style={{ background: 'var(--hds-bg-3)', border: `1px solid ${item.id < 0 ? 'oklch(0.55 0.12 290 / 0.6)' : 'var(--hds-line-s)'}`, borderRadius: 7, overflow: 'hidden' }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 9, padding: '8px 10px' }}>
                  <span style={{ color: 'var(--hds-txt-3)', fontSize: 13 }}>⋮⋮</span>
                  <span style={{ width: 7, height: 7, borderRadius: 2, background: dot, flexShrink: 0 }} />
                  {thumbUrl && (
                    <img src={thumbUrl} loading="lazy" style={{ width: 30, height: 44, objectFit: 'cover', borderRadius: 3, flexShrink: 0, opacity: 0, transition: 'opacity .2s' }}
                      onLoad={e => { (e.target as HTMLImageElement).style.opacity = '1' }}
                      onError={e => { (e.target as HTMLImageElement).style.display = 'none' }} />
                  )}
                  <span style={{ flex: 1, fontSize: 12, fontWeight: 500, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{item.title || item.content_id}</span>
                  {canReset && (
                    <button
                      onClick={() =>
                        api.resetBlockContentCursor(channelId, store.editing!.block_id, item.id)
                          .then(() => store.loadEpg(channelId, true))
                          .catch(e => alert(`Cursor reset failed: ${e?.message ?? e}`))
                      }
                      title="Reset cursor to beginning"
                      style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 12 }}
                    >⏮</button>
                  )}
                  <button onClick={() => store.removeContent(channelId, item.id)} style={{ width: 22, height: 22, border: 'none', borderRadius: 6, background: 'transparent', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 13 }}>×</button>
                </div>
                {showWeightControl && (
                  <div style={{ borderTop: '1px solid var(--hds-line-s)' }}>
                    <div style={{ display: 'flex', gap: 16, padding: '4px 10px 6px' }}>
                      <label style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 9.5, letterSpacing: '0.1em', color: 'var(--hds-txt-3)' }}>
                        {weightLabel}
                        <input type="number" min={1} max={99} value={item.weight ?? 1} style={miniInp}
                          onChange={e => store.updateContentField(channelId, item.id, 'weight', Number(e.target.value))} />
                      </label>
                      {isRerun && (
                        <label style={{ display: 'flex', alignItems: 'center', gap: 5, fontSize: 9.5, letterSpacing: '0.1em', color: 'var(--hds-txt-3)' }}>
                          RUN
                          <input type="number" min={1} max={99} value={item.run_count ?? 1} style={miniInp}
                            onChange={e => store.updateContentField(channelId, item.id, 'run_count', Number(e.target.value))} />
                        </label>
                      )}
                    </div>
                    <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', padding: '0 10px 7px', lineHeight: 1.5 }}>
                      {isSequential && 'Episodes before switching shows. 1 = strict rotation.'}
                      {isShuffle && 'Selection probability relative to other shows in the block.'}
                      {isRerun && 'WEIGHT: pick probability. RUN: consecutive episodes per selection before re-rolling.'}
                    </div>
                  </div>
                )}
                {isPlaylist && playlist && (
                  <div style={{ borderTop: '1px solid var(--hds-line-s)', padding: '7px 10px 8px' }}>
                    <div style={{ fontSize: 9.5, letterSpacing: '0.12em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>PLAYLIST MODE</div>
                    <div style={{ display: 'flex', gap: 6 }}>
                      <button
                        onClick={() => store.setPlaylistMode(item.content_id, 'sequential')}
                        style={{ padding: '3px 9px', border: 'none', borderRadius: 5, fontSize: 10, fontFamily: "'JetBrains Mono', monospace", fontWeight: 600, cursor: 'pointer', background: !isShowColl ? 'var(--hds-violet)' : 'var(--hds-bg)', color: !isShowColl ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-3)', transition: '.1s' }}
                      >In-Order</button>
                      <button
                        onClick={() => store.setPlaylistMode(item.content_id, 'show_collection')}
                        style={{ padding: '3px 9px', border: 'none', borderRadius: 5, fontSize: 10, fontFamily: "'JetBrains Mono', monospace", fontWeight: 600, cursor: 'pointer', background: isShowColl ? 'var(--hds-violet)' : 'var(--hds-bg)', color: isShowColl ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-3)', transition: '.1s' }}
                      >Show Collection</button>
                    </div>
                    <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 5, lineHeight: 1.5 }}>
                      {!isShowColl
                        ? 'In-Order: playlist items play sequentially as a single flat list, regardless of the block\'s advancement setting.'
                        : 'Show Collection: the block\'s advancement setting (rerun, shuffle, etc.) applies across the distinct shows inside this playlist. Each show\'s episodes are tracked separately.'}
                    </div>
                  </div>
                )}
                {item.content_type === 'show' && (
                  <ShowOrderControls
                    itemId={item.id}
                    channelId={channelId}
                    order={item.episode_order ?? 'season'}
                    includeSpecials={item.include_specials ?? false}
                    store={store}
                  />
                )}
              </div>
            )
          })}
          {(store.editing || store.isNewMode) && store.draftContent.length === 0 && !store.pickerOpen && (
            <div style={{ textAlign: 'center', padding: 6, color: 'var(--hds-txt-3)', fontSize: 11 }}>Drag shows or movies here, or use + Add</div>
          )}
        </div>

        {isRerun && newPremierCount > 0 && (
          <div style={{ marginTop: 10, padding: '10px 12px', background: 'oklch(0.41 0.12 18 / 0.08)', border: '1px solid oklch(0.52 0.14 20 / 0.35)', borderRadius: 8 }}>
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 10 }}>
              <span style={{ fontSize: 10.5, color: 'var(--hds-txt-2)' }}>
                {newPremierCount} show{newPremierCount !== 1 ? 's' : ''} without a premier block
              </span>
              <button
                onClick={() => store.createPremierBlocks(channelId)}
                disabled={store.creatingPremiers}
                style={{ padding: '4px 10px', border: '1px solid oklch(0.52 0.14 20 / 0.5)', borderRadius: 6, background: store.creatingPremiers ? 'transparent' : 'oklch(0.41 0.12 18 / 0.3)', color: store.creatingPremiers ? 'var(--hds-txt-3)' : 'oklch(0.76 0.17 24)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: store.creatingPremiers ? 'default' : 'pointer', transition: '.12s' }}
              >
                {store.creatingPremiers ? 'Creating…' : 'Create Premier Blocks'}
              </button>
            </div>
            <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 5, lineHeight: 1.5 }}>
              Creates a sequential premier block for each show, matching this block's schedule and cursor scope.
            </div>
          </div>
        )}

      </AccordionSection>

      {/* ── FILLER ── */}
      <AccordionSection
        title="FILLER"
        open={sec.filler}
        onToggle={() => tog('filler')}
        badge={fillerCount > 0 ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>{fillerCount}</span> : undefined}
      >
        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 9 }}>
          <span />
          {(store.editing || store.isNewMode) && (
            <button
              onClick={() => { store.fillerPickerOpen = !store.fillerPickerOpen }}
              style={{ color: 'var(--hds-violet)', background: 'transparent', border: 'none', fontFamily: "'JetBrains Mono', monospace", fontSize: 11, cursor: 'pointer', padding: '2px 4px' }}
            >
              {store.fillerPickerOpen ? '✕ Close' : '+ Add list'}
            </button>
          )}
        </div>

        {store.draftFillerEntries.length > 0 && (
          <>
            {store.draftFillerEntries.length > 1 && (
              <div style={{ marginBottom: 10 }}>
                <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>SELECT BY</div>
                <select value={d.filler_selection} onChange={e => store.setDraft('filler_selection', e.target.value as FillerSelectionMode)} style={inputStyle}>
                  {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                </select>
              </div>
            )}
            <div style={{ display: 'flex', flexDirection: 'column', gap: 6, marginBottom: 10 }}>
              {store.draftFillerEntries.map(entry => (
                <FillerEntryRow
                  key={entry.id}
                  entry={entry}
                  showWeight={d.filler_selection === 'weighted'}
                  onAdvancement={adv => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { advancement: adv })}
                  onWeight={w   => store.updateBlockFiller(channelId, store.editing?.block_id ?? '', entry.id, { weight: w })}
                  onRemove={()  => store.removeBlockFiller(channelId, store.editing?.block_id ?? '', entry.id)}
                />
              ))}
            </div>
          </>
        )}

        {(store.editing || store.isNewMode) && store.draftFillerEntries.length === 0 && !store.fillerPickerOpen && (
          <div style={{ textAlign: 'center', padding: '6px 0', color: 'var(--hds-txt-3)', fontSize: 11 }}>
            No filler lists — channel default will be used
          </div>
        )}

        {store.fillerPickerOpen && (store.editing || store.isNewMode) && (
          <FillerAddPanel channelId={channelId} store={store} />
        )}

        <div style={{ marginTop: 12 }}>
          <button
            onClick={() => store.setDraft('inter_filler', !d.inter_filler)}
            style={{
              display: 'flex', alignItems: 'center', justifyContent: 'space-between',
              width: '100%', padding: '8px 10px',
              border: `1px solid ${d.inter_filler ? 'oklch(0.7 0.12 84 / 0.5)' : 'var(--hds-line)'}`,
              borderRadius: 7, background: d.inter_filler ? 'oklch(0.55 0.1 84 / 0.12)' : 'var(--hds-bg-3)',
              cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
            }}
          >
            <span style={{ fontSize: 11, color: 'var(--hds-txt-2)' }}>Filler between programs</span>
            <span style={{ fontSize: 10, fontWeight: 700, letterSpacing: '0.08em', color: d.inter_filler ? 'var(--hds-gold)' : 'var(--hds-txt-3)' }}>{d.inter_filler ? 'ON' : 'OFF'}</span>
          </button>
          <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginTop: 6, lineHeight: 1.55 }}>
            When off, filler only fills leftover time at end of block. When on, also fills between programs.
          </div>
        </div>
      </AccordionSection>
    </div>
  )
})

// ─── Show ordering controls ───────────────────────────────────────────────────

const ORDER_OPTS: { value: EpisodeOrder; label: string; hint: string }[] = [
  { value: 'season',   label: 'Season',   hint: 'Standard season / episode numbering.' },
  { value: 'absolute', label: 'Absolute', hint: 'TVDB absolute episode number — ignores seasons. Best for anime.' },
  { value: 'airdate',  label: 'Air Date', hint: 'Chronological by original air date. Good for out-of-order shows.' },
]

function ShowOrderControls({ itemId, channelId, order, includeSpecials, store }: {
  itemId:          number
  channelId:       string
  order:           EpisodeOrder
  includeSpecials: boolean
  store:           ChannelDetailStore
}) {
  const btnStyle = (active: boolean): React.CSSProperties => ({
    padding: '3px 8px', border: 'none', borderRadius: 5,
    fontSize: 10, fontFamily: "'JetBrains Mono', monospace", fontWeight: 600,
    cursor: 'pointer',
    background: active ? 'var(--hds-violet)' : 'var(--hds-bg)',
    color:      active ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-3)',
    transition: '.1s',
  })
  const hint = ORDER_OPTS.find(o => o.value === order)?.hint ?? ''

  const setOrder = (v: EpisodeOrder) =>
    store.updateContentField(channelId, itemId, 'episode_order', v)
  const toggleSpecials = () =>
    store.updateContentField(channelId, itemId, 'include_specials', !includeSpecials)

  return (
    <div style={{ borderTop: '1px solid var(--hds-line-s)', padding: '7px 10px 8px' }}>
      <div style={{ fontSize: 9.5, letterSpacing: '0.12em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>EPISODE ORDER</div>
      <div style={{ display: 'flex', gap: 4, alignItems: 'center' }}>
        {ORDER_OPTS.map(o => (
          <button key={o.value} onClick={() => setOrder(o.value)} style={btnStyle(order === o.value)}>
            {o.label}
          </button>
        ))}
        <div style={{ width: 1, height: 14, background: 'var(--hds-line-s)', margin: '0 3px', flexShrink: 0 }} />
        <button onClick={toggleSpecials} style={btnStyle(includeSpecials)}>
          S00
        </button>
      </div>
      <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 5, lineHeight: 1.5 }}>
        {hint}{includeSpecials ? ' · Season 00 included.' : ' · Season 00 excluded.'}
      </div>
    </div>
  )
}
