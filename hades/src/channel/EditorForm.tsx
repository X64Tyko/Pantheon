import { observer } from 'mobx-react-lite'
import { useState, useEffect } from 'react'
import type { ReactNode } from 'react'
import type { Advancement, BlockType, CursorScope, EpisodeOrder, FillerSelectionMode, NoHistoryBehavior, Show, Playlist, EpisodeSearchResult } from '../api/types'
import {
  ALIGN_OPTS, BLOCK_META, DAYS, DAY_BITS, DELAY_OPTS, EARLY_OPTS,
  FILLER_ADV_OPTS, FILLER_SEL_OPTS, NO_HISTORY_OPTS, RATINGS,
} from './constants'
import { getLimitMode } from './utils'
import { inputStyle } from './styles'
import { FillerEntryRow, FillerAddPanel } from './FillerPanel'
import { ContentPicker } from './ContentPicker'
import { HelpTip, HelpSection, GifSlot } from './HelpTip'
import { api } from '../api/client'
import type { ChannelDetailStore } from './store'
import type { LimitMode } from './types'

// ─── Accordion section ────────────────────────────────────────────────────────

export function AccordionSection({ title, badge, open, onToggle, children, forceOpen }: {
  title:      string
  badge?:     ReactNode
  open:       boolean
  onToggle:   () => void
  children:   ReactNode
  forceOpen?: boolean
}) {
  if (forceOpen) {
    return (
      <div style={{ borderRadius: 9, border: '1px solid var(--hds-line)', marginBottom: 8, overflow: 'hidden', background: 'oklch(0.21 0.022 288 / 0.5)' }}>
        <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '11px 13px' }}>
          <span style={{ width: 7, height: 7, borderRadius: 2, background: 'var(--hds-gold)', flexShrink: 0 }} />
          <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace" }}>{title}</span>
          {badge}
        </div>
        <div style={{ padding: '4px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
          {children}
        </div>
      </div>
    )
  }
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

// ─── Card section (always-open, for compact/modal layout) ─────────────────────

export function CardSection({ title, summary, children }: {
  title:    string
  summary?: ReactNode
  children: ReactNode
}) {
  return (
    <div style={{ borderRadius: 9, border: '1px solid var(--hds-line)', marginBottom: 8, overflow: 'hidden', background: 'oklch(0.21 0.022 288 / 0.5)' }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, padding: '11px 13px' }}>
        <span style={{ width: 7, height: 7, borderRadius: 2, background: 'var(--hds-gold)', flexShrink: 0 }} />
        <span style={{ flex: 1, fontSize: 10, letterSpacing: '0.2em', color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace" }}>{title}</span>
        {summary && <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{summary}</span>}
      </div>
      <div style={{ padding: '4px 14px 16px', borderTop: '1px solid var(--hds-line-s)' }}>
        {children}
      </div>
    </div>
  )
}

// ─── Launcher row (compact mode — opens filler/bumper overlay) ─────────────────

function LauncherRow({ icon, title, summary, onClick }: {
  icon:    ReactNode
  title:   string
  summary: string
  onClick: () => void
}) {
  return (
    <div
      onClick={onClick}
      style={{ display: 'flex', alignItems: 'center', gap: 12, padding: '12px 13px', marginBottom: 9, borderRadius: 11, cursor: 'pointer', border: '1px solid var(--hds-line-s)', background: 'oklch(0.19 0.018 288 / 0.45)', transition: 'border-color .12s, background .12s' }}
      onMouseEnter={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line)'; (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.24 0.025 290 / 0.5)' }}
      onMouseLeave={e => { (e.currentTarget as HTMLDivElement).style.borderColor = 'var(--hds-line-s)'; (e.currentTarget as HTMLDivElement).style.background = 'oklch(0.19 0.018 288 / 0.45)' }}
    >
      <span style={{ width: 30, height: 30, flexShrink: 0, display: 'flex', alignItems: 'center', justifyContent: 'center', borderRadius: 8, border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 16 }}>{icon}</span>
      <div style={{ flex: 1, minWidth: 0 }}>
        <div style={{ fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, letterSpacing: '0.18em', color: 'var(--hds-txt)' }}>{title}</div>
        <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", marginTop: 2, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }}>{summary}</div>
      </div>
      <span style={{ fontSize: 13, color: 'var(--hds-violet)', flexShrink: 0 }}>›</span>
    </div>
  )
}

// ─── Editor form ──────────────────────────────────────────────────────────────

export const EditorForm = observer(function EditorForm({ channelId, store, limitMode, hidePicker, compact }: {
  channelId:   string
  store:       ChannelDetailStore
  limitMode:   LimitMode
  hidePicker?: boolean
  compact?:    boolean
}) {
  const d   = store.draft
  const m   = BLOCK_META[d.block_type]
  const sec = store.openSections
  const tog = (s: string) => store.toggleSection(s)
  const sh  = store.showHints

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

  // Compact-mode summaries for launcher rows
  const fillerSummary = fillerCount > 0
    ? `${fillerCount} list${fillerCount !== 1 ? 's' : ''} · ${d.filler_selection}`
    : 'Channel default will be used'
  const bumperSlotsOn = [d.intro_content_id, d.outro_content_id, d.interstitial_content_id].filter(Boolean).length
  const bumperSummary = bumperSlotsOn > 0
    ? `${bumperSlotsOn} slot${bumperSlotsOn !== 1 ? 's' : ''} configured`
    : 'None configured'

  // Days summary for compact card header
  const DAY_NAMES = ['Mon','Tue','Wed','Thu','Fri','Sat','Sun']
  const DBITS     = [0x02,0x04,0x08,0x10,0x20,0x40,0x01]
  const daysOn    = DAY_NAMES.filter((_,i) => (d.day_mask & DBITS[i]) !== 0)
  const daysStr   = daysOn.length === 7 ? 'Every day'
    : daysOn.length === 5 && (d.day_mask & 0x3e) === 0x3e ? 'Weekdays'
    : daysOn.length === 2 && (d.day_mask & 0x41) === 0x41 ? 'Weekends'
    : daysOn.join('·') || 'No days'
  const stopStr   = limitMode === 'programs' ? `${d.program_count}p` : limitMode === 'end' ? (d.end_time ?? 'open') : 'fill day'
  const timingStr = `${d.start_time || '—'} · ${stopStr}`

  return (
    <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '12px 12px 20px' }} className="scrollbar-dark">

      {/* ── Block name (sidebar mode only — modal puts it in header) ── */}
      {!compact && (
        <input
          type="text"
          placeholder="Block name (optional)"
          value={d.name ?? ''}
          onChange={e => store.setDraft('name', e.target.value)}
          style={{ ...inputStyle, width: '100%', marginBottom: 10 }}
        />
      )}

      {/* ── SCHEDULE ── */}
      <AccordionSection title="SCHEDULE" open={sec.schedule} onToggle={() => tog('schedule')} forceOpen={compact} badge={compact ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{daysStr}</span> : undefined}>
        <div style={{ display: 'flex', alignItems: 'center', fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)', marginBottom: 7 }}>
          BLOCK TYPE
          <HelpTip title="Block Types" tip="What each block type does">
            <HelpSection title="Episode">
              Plays TV show episodes from your content list. The <b style={{ color: 'var(--hds-txt)' }}>ORDER</b> setting controls how the engine cycles through shows — sequential rotation, weighted random, or rerun modes that draw from play history.
            </HelpSection>
            <HelpSection title="Movie">
              Plays individual movies, one per selection. The block advances through your movie list according to the ORDER setting.
            </HelpSection>
            <HelpSection title="Premier">
              <p style={{ margin: '0 0 10px' }}>First-run only — only plays episodes that have never aired on this channel. Before scheduling, the engine checks play history; only unseen episodes are eligible.</p>
              <p style={{ margin: '0 0 10px' }}>Pair a Premier block with a Rerun block (<b style={{ color: 'var(--hds-txt)' }}>rerun_shuffle</b> or <b style={{ color: 'var(--hds-txt)' }}>rerun_smart</b>) covering the same shows. Once an episode premieres it automatically enters the rerun pool — no manual maintenance needed.</p>
              <GifSlot label="Premier block paired with Rerun block — new episodes premiere once, then feed into the rerun rotation" />
            </HelpSection>
            <HelpSection title="Filler">
              Fills dead air with short clips (commercials, bumpers) from a filler pool. Filler blocks draw from filler lists attached to any block's FILLER section, or the channel default if none are set. No main content list is needed.
            </HelpSection>
          </HelpTip>
        </div>
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
      <AccordionSection title="TIMING" open={sec.timing} onToggle={() => tog('timing')} forceOpen={compact} badge={compact ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{timingStr}</span> : undefined}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: 9, marginBottom: 16 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>START TIME</div>
            <input type="time" value={d.start_time} onChange={e => store.setDraft('start_time', e.target.value)} style={inputStyle} />
          </div>
          <div>
            <div style={{ display: 'flex', alignItems: 'center', fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>
              LATE START
              <HelpTip title="Late Start" tip="Allow this block to fire late if its slot is taken">
                <p style={{ margin: '0 0 12px' }}>A higher-priority block overrunning into this block's scheduled start will normally cause this block to be skipped for that day.</p>
                <p style={{ margin: '0 0 12px' }}>With Late Start set to N minutes, the block will still fire — up to N minutes after its scheduled start — instead of being dropped. It still ends at its normal time or program count, so a late start means fewer programs play that run.</p>
                <p style={{ margin: '0 0 4px' }}><b style={{ color: 'var(--hds-txt)' }}>Example:</b> a movie block (priority 5) runs until 22:15, pushing into a 22:00 news block (priority 3). Without Late Start the news block is skipped. With Late Start = 20 min, news fires at 22:15.</p>
                <GifSlot label="Higher-priority block overruns; lower-priority block fires late within tolerance" />
              </HelpTip>
            </div>
            <select value={String(d.late_start_mins)} onChange={e => store.setDraft('late_start_mins', +e.target.value)} style={inputStyle}>
              {DELAY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {sh && d.late_start_mins > 0 && (
              <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
                Block may start up to {d.late_start_mins} min late if preempted by a higher-priority block.
              </div>
            )}
          </div>
          <div>
            <div style={{ display: 'flex', alignItems: 'center', fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>
              EARLY START
              <HelpTip title="Early Start" tip="Steal dead air before this block's scheduled start">
                <p style={{ margin: '0 0 12px' }}>If the block before this one ends early and leaves unscheduled time, Early Start lets this block claim that gap.</p>
                <p style={{ margin: '0 0 12px' }}>With Early Start set to N seconds, the block may begin up to N seconds before its scheduled start time. The first program plays normally — it just begins sooner.</p>
                <p style={{ margin: '0 0 4px' }}><b style={{ color: 'var(--hds-txt)' }}>Example:</b> a filler block ends at 21:58:30, leaving 90 seconds before a 22:00 episode block. With Early Start = 120s, the episode block begins at 21:58:30 instead of waiting for 22:00.</p>
                <GifSlot label="Previous block ends early; this block claims the dead air gap" />
              </HelpTip>
            </div>
            <select value={String(d.early_start_secs)} onChange={e => store.setDraft('early_start_secs', +e.target.value)} style={inputStyle}>
              {EARLY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {sh && d.early_start_secs > 0 && (
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
        {sh && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', marginBottom: 14, lineHeight: 1.55 }}>{limitHelp}</div>}

        <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', marginBottom: 5 }}>
          <div style={{ display: 'flex', alignItems: 'center', fontSize: 9.5, letterSpacing: '0.18em', color: 'var(--hds-txt-3)' }}>
            ALIGN START
            <HelpTip title="Align Start" tip="Snap this block's start to a clock boundary">
              <HelpSection title="Block Scope">
                Snaps the first program of the block to the next upcoming boundary — :00, :15, :30, or :45. If a conflict delays the block to 20:07, it waits until 20:15 rather than starting mid-interval. Only fires once at block start.
              </HelpSection>
              <HelpSection title="Episode Scope">
                <p style={{ margin: '0 0 10px' }}>Snaps each individual episode to the next boundary, creating a grid-locked schedule where every program starts on a clean time mark.</p>
                <p style={{ margin: 0 }}>Early Start and Late Start define the tolerance window around each snap point. If the tolerance is not enough to reach the next boundary, the episode may be skipped.</p>
              </HelpSection>
            </HelpTip>
          </div>
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
        {sh && <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', lineHeight: 1.55 }}>
          {(d.start_scope ?? 'block') === 'episode'
            ? 'Snaps each episode to the next time boundary. Early/late start define the tolerance window.'
            : 'Snaps the first program of the block to the next time boundary.'}
        </div>}
      </AccordionSection>

      {/* ── PLAYBACK ── */}
      <AccordionSection title="PLAYBACK" open={sec.playback} onToggle={() => tog('playback')}>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9, marginBottom: 14 }}>
          <div>
            <div style={{ display: 'flex', alignItems: 'center', fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>
              PRIORITY
              <HelpTip title="Block Priority" tip="How overlapping blocks are resolved">
                <p style={{ margin: '0 0 12px' }}>When two blocks overlap on the same time slot, the higher-priority block wins the contested minutes. The lower-priority block is cut short at the conflict point.</p>
                <p style={{ margin: '0 0 12px' }}>A lower-priority block that loses its entire start window is skipped for that day unless it has a <b style={{ color: 'var(--hds-txt)' }}>Late Start</b> tolerance set.</p>
                <p style={{ margin: '0 0 12px' }}>Priority only matters where blocks overlap. Non-overlapping blocks play independently of their priority values.</p>
                <p style={{ margin: '0 0 4px' }}><b style={{ color: 'var(--hds-txt)' }}>Tip:</b> use a low-priority 24/7 filler block as a catch-all that anything else can override, or set a high priority on a special event block to punch through a normal scheduled lineup.</p>
                <GifSlot label="Two overlapping blocks — higher priority wins the contested window; lower priority is cut short" />
              </HelpTip>
            </div>
            <input type="number" min={1} value={d.priority} onChange={e => store.setDraft('priority', Math.max(1, +e.target.value || 1))} style={inputStyle} />
            {sh && <div style={{ fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 4 }}>higher wins conflicts</div>}
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
            {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
              {orderHint[d.advancement]}
            </div>}
          </div>
          <div>
            <div style={{ display: 'flex', alignItems: 'center', fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>
              CURSOR
              <HelpTip title="Cursor Scope" tip="How episode positions are shared between blocks">
                <HelpSection title="Per Block (default)">
                  Each block has its own episode position per show. The same show in two blocks plays completely independently — Morning could be on S01E03 while Evening is on S02E01. Blocks never interfere with each other. Safe default for most setups.
                </HelpSection>
                <HelpSection title="Per Channel">
                  All blocks on this channel share episode positions for the same show. If Morning plays Kim Possible S01E03 tonight, Evening picks up at S01E04 tomorrow. Use this to build a continuous channel where every block contributes to a single run through the library.
                </HelpSection>
                <HelpSection title="Global">
                  Positions are shared across all channels system-wide. A show played on Channel 1 advances the same cursor used by Channel 2. Use this for a cross-channel rerun pool where episode state follows the content, not the channel.
                </HelpSection>
              </HelpTip>
            </div>
            <select value={d.cursor_scope} onChange={e => store.setDraft('cursor_scope', e.target.value as CursorScope)} style={inputStyle}>
              <option value="block">Per block</option>
              <option value="channel">Per channel</option>
              <option value="global">Global</option>
            </select>
            {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
              {cursorHint[d.cursor_scope]}
            </div>}
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
            {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 3 }}>
              Episodes won't repeat until {d.smart_pct ?? 30}% of the pool has played since last air
            </div>}
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
            {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 3 }}>
              {NO_HISTORY_OPTS.find(([v]) => v === (d.no_history_behavior ?? 'normal'))?.[2]}
            </div>}
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
            {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
              When the limit is hit the engine re-rolls show selection, forcing a switch even if the same show wins.
            </div>}
          </div>
        )}
        {(d.advancement === 'rerun_shuffle' || d.advancement === 'rerun_smart') && (
          <div style={{ marginTop: 9 }}>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>GROUP SNAP</div>
            <label style={{ display: 'flex', alignItems: 'center', gap: 8, cursor: 'pointer' }}>
              <input type="checkbox"
                checked={d.snap_to_group_start ?? true}
                onChange={e => store.setDraft('snap_to_group_start', e.target.checked)} />
              <span style={{ fontSize: 9.5, color: 'var(--hds-txt-3)' }}>Snap to Part 1 when a multi-part episode is selected</span>
            </label>
            {sh && (d.snap_to_group_start ?? true) && (
              <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 4, lineHeight: 1.5 }}>
                When a mid-group episode (Part 2+) is randomly selected, the run starts from Part 1. For shows without a premier block, this creates a lead-in rerun before an upcoming premiere.
              </div>
            )}
          </div>
        )}
      </AccordionSection>

      {/* ── Compact launchers (modal mode only) ── */}
      {compact && (
        <>
          <LauncherRow
            icon={<span style={{ width: 7, height: 7, borderRadius: 2, background: 'var(--hds-violet)', display: 'inline-block' }} />}
            title="FILLER & FALLBACK"
            summary={fillerSummary}
            onClick={() => { store.fillerOverlayOpen = true; store.bumperOverlayOpen = false }}
          />
          <LauncherRow
            icon={<span style={{ width: 7, height: 7, borderRadius: 2, background: 'oklch(0.65 0.12 320)', display: 'inline-block' }} />}
            title="BUMPERS"
            summary={bumperSummary}
            onClick={() => { store.bumperOverlayOpen = true; store.fillerOverlayOpen = false }}
          />
        </>
      )}

      {!compact && (<>

      {/* ── CONTENT (sidebar accordion: list + controls inline, add via modal) ── */}
      <AccordionSection
        title="CONTENT"
        open={sec.content}
        onToggle={() => tog('content')}
        badge={contentCount > 0 ? <span style={{ fontSize: 10, color: 'var(--hds-txt-3)', letterSpacing: '0.04em' }}>{contentCount}</span> : undefined}
      >
        {(store.editing || store.isNewMode) && (
          <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'flex-end', marginBottom: 9 }}>
            <button
              onClick={() => store.modalOpen = true}
              style={{ color: 'var(--hds-violet)', background: 'transparent', border: '1px solid var(--hds-line)', borderRadius: 5, fontFamily: "'JetBrains Mono', monospace", fontSize: 10.5, cursor: 'pointer', padding: '3px 8px', display: 'flex', alignItems: 'center', gap: 4 }}
            >
              <svg width="10" height="10" viewBox="0 0 11 11" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round"><path d="M1 4V1h3M10 1H7m3 0v3M1 7v3h3M10 10H7m3 0V7" /></svg>
              Browse Library
            </button>
          </div>
        )}

        <div
          onDragOver={e => e.preventDefault()}
          onDrop={e => { e.preventDefault(); if (store.dragContent) { store.addContent(channelId, store.dragContent); store.dragContent = null } }}
          style={{ display: 'flex', flexDirection: 'column', gap: 6, minHeight: 40, padding: store.dragContent ? 8 : 0, border: store.dragContent ? '1px dashed var(--hds-violet)' : '1px solid transparent', borderRadius: 9, transition: '.12s' }}
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
                          .then(() => store.loadEpg(channelId))
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
                    {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', padding: '0 10px 7px', lineHeight: 1.5 }}>
                      {isSequential && 'Episodes before switching shows. 1 = strict rotation.'}
                      {isShuffle && 'Selection probability relative to other shows in the block.'}
                      {isRerun && 'WEIGHT: pick probability. RUN: consecutive episodes per selection before re-rolling.'}
                    </div>}
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
                    {sh && <div style={{ fontSize: 9.5, color: 'var(--hds-txt-3)', marginTop: 5, lineHeight: 1.5 }}>
                      {!isShowColl
                        ? 'In-Order: playlist items play sequentially as a single flat list, regardless of the block\'s advancement setting.'
                        : 'Show Collection: the block\'s advancement setting (rerun, shuffle, etc.) applies across the distinct shows inside this playlist. Each show\'s episodes are tracked separately.'}
                    </div>}
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
          {(store.editing || store.isNewMode) && store.draftContent.length === 0 && (
            <div style={{ textAlign: 'center', padding: 6, color: 'var(--hds-txt-3)', fontSize: 11 }}>
              {hidePicker ? 'Drag items from the library panel or click to add' : 'No content — use Browse Library to add'}
            </div>
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

      {/* ── FILLER launcher ── */}
      <LauncherRow
        icon={<span style={{ width: 7, height: 7, borderRadius: 2, background: 'var(--hds-violet)', display: 'inline-block' }} />}
        title="FILLER & FALLBACK"
        summary={fillerSummary}
        onClick={() => { store.modalOpen = true; store.fillerOverlayOpen = true; store.bumperOverlayOpen = false }}
      />

      {/* ── BUMPERS launcher ── */}
      <LauncherRow
        icon={<span style={{ width: 7, height: 7, borderRadius: 2, background: 'oklch(0.65 0.12 320)', display: 'inline-block' }} />}
        title="BUMPERS"
        summary={bumperSummary}
        onClick={() => { store.modalOpen = true; store.bumperOverlayOpen = true; store.fillerOverlayOpen = false }}
      />

      </>)}

    </div>
  )
})

// ─── Bumper slot picker ───────────────────────────────────────────────────────

type BumperTab = 'show' | 'playlist' | 'episode'

function BumperSlot({ label, hint, contentType, contentId, onSet, onClear, children }: {
  label:       string
  hint:        string
  contentType: string
  contentId:   string
  onSet:       (ct: BumperTab, cid: string) => void
  onClear:     () => void
  children?:   ReactNode
}) {
  const [open,      setOpen]      = useState(false)
  const [tab,       setTab]       = useState<BumperTab>('show')
  const [q,         setQ]         = useState('')
  const [seasonFlt, setSeasonFlt] = useState('')
  const [shows,     setShows]     = useState<Show[]>([])
  const [lists,     setLists]     = useState<Playlist[]>([])
  const [eps,       setEps]       = useState<EpisodeSearchResult[]>([])
  const [loading,   setLoading]   = useState(false)

  const hasContent = contentType !== '' && contentId !== ''

  useEffect(() => {
    if (!open) return
    setLoading(true)
    if (tab === 'show')     api.getShows({ limit: 80, q }).then(r => { setShows(r.items); setLoading(false) }).catch(() => setLoading(false))
    if (tab === 'playlist') api.getPlaylists().then(r => { setLists(r); setLoading(false) }).catch(() => setLoading(false))
    if (tab === 'episode') {
      const season = seasonFlt.trim() !== '' ? parseInt(seasonFlt, 10) : undefined
      api.searchEpisodes({ q: q || undefined, season: Number.isFinite(season) ? season : undefined, limit: 40 }).then(r => { setEps(r.items); setLoading(false) }).catch(() => setLoading(false))
    }
  }, [open, tab, q, seasonFlt])

  const pick = (ct: BumperTab, cid: string) => { onSet(ct, cid); setOpen(false); setQ(''); setSeasonFlt('') }

  const tabBtn = (t: BumperTab, label: string) => (
    <button onClick={() => setTab(t)} style={{ padding: '3px 9px', border: 'none', borderRadius: 4, cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace", fontSize: 10, background: tab === t ? 'var(--hds-violet)' : 'transparent', color: tab === t ? 'oklch(0.15 0.02 286)' : 'var(--hds-txt-2)' }}>
      {label}
    </button>
  )

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 4 }}>
        <span style={{ fontSize: 9.5, letterSpacing: '0.14em', color: 'var(--hds-txt-3)', width: 80, flexShrink: 0 }}>{label.toUpperCase()}</span>
        {hasContent ? (
          <div style={{ display: 'flex', alignItems: 'center', gap: 6, flex: 1, minWidth: 0 }}>
            <span style={{ fontSize: 9.5, padding: '2px 6px', borderRadius: 3, background: 'var(--hds-bg)', color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace", letterSpacing: '0.06em' }}>{contentType}</span>
            <span style={{ fontSize: 11, color: 'var(--hds-txt-2)', flex: 1, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{contentId}</span>
            <button onClick={onClear} style={{ background: 'transparent', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 12, padding: '0 2px' }}>✕</button>
          </div>
        ) : (
          <button onClick={() => setOpen(!open)} style={{ background: 'transparent', border: '1px dashed var(--hds-line)', borderRadius: 5, cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 10.5, padding: '3px 10px', fontFamily: "'JetBrains Mono', monospace" }}>
            {open ? '✕ Cancel' : '+ Set'}
          </button>
        )}
        {hasContent && (
          <button onClick={() => setOpen(!open)} style={{ background: 'transparent', border: 'none', cursor: 'pointer', color: 'var(--hds-violet)', fontSize: 10.5, padding: '2px 4px', fontFamily: "'JetBrains Mono', monospace" }}>
            {open ? '✕' : 'Change'}
          </button>
        )}
      </div>
      <div style={{ fontSize: 10, color: 'var(--hds-txt-3)', lineHeight: 1.5, marginLeft: 88, marginBottom: children ? 0 : 0 }}>{hint}</div>
      {children && <div style={{ marginLeft: 88 }}>{children}</div>}
      {open && (
        <div style={{ marginTop: 8, border: '1px solid var(--hds-line)', borderRadius: 8, background: 'oklch(0.16 0.016 286)', overflow: 'hidden' }}>
          <div style={{ display: 'flex', gap: 2, padding: '6px 8px', background: 'var(--hds-bg-3)', borderBottom: '1px solid var(--hds-line-s)' }}>
            {tabBtn('show', 'Show')} {tabBtn('playlist', 'Playlist')} {tabBtn('episode', 'Episode')}
          </div>
          <div style={{ padding: '6px 8px', display: 'flex', gap: 6 }}>
            <input value={q} onChange={e => setQ(e.target.value)} placeholder="Search…" style={{ ...inputStyle, flex: 1, fontSize: 11, padding: '5px 8px' }} autoFocus />
            {tab === 'episode' && (
              <input
                type="number" min={0} value={seasonFlt} onChange={e => setSeasonFlt(e.target.value)}
                placeholder="S#" title="Filter by season (0 = specials)"
                style={{ ...inputStyle, width: 48, fontSize: 11, padding: '5px 6px' }}
              />
            )}
          </div>
          <div style={{ maxHeight: 160, overflow: 'auto' }} className="scrollbar-dark">
            {loading ? (
              <div style={{ padding: 10, color: 'var(--hds-txt-3)', fontSize: 11 }}>Loading…</div>
            ) : tab === 'show' ? (
              shows.filter(s => !q || s.title.toLowerCase().includes(q.toLowerCase())).map(s => (
                <div key={s.show_id} onClick={() => pick('show', s.show_id)} style={{ padding: '6px 12px', cursor: 'pointer', fontSize: 12, borderBottom: '1px solid var(--hds-line-s)' }}
                  onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
                  onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}>
                  {s.title}{s.year ? <span style={{ color: 'var(--hds-txt-3)', fontSize: 10, marginLeft: 5 }}>({s.year})</span> : null}
                </div>
              ))
            ) : tab === 'playlist' ? (
              lists.filter(p => !q || p.title.toLowerCase().includes(q.toLowerCase())).map(p => (
                <div key={p.playlist_id} onClick={() => pick('playlist', p.playlist_id)} style={{ padding: '6px 12px', cursor: 'pointer', fontSize: 12, borderBottom: '1px solid var(--hds-line-s)' }}
                  onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
                  onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}>
                  {p.title}
                </div>
              ))
            ) : (
              eps.map(ep => (
                <div key={ep.episode_id} onClick={() => pick('episode', ep.episode_id)} style={{ padding: '6px 12px', cursor: 'pointer', fontSize: 12, borderBottom: '1px solid var(--hds-line-s)' }}
                  onMouseEnter={e => (e.currentTarget as HTMLDivElement).style.background = 'var(--hds-bg-3)'}
                  onMouseLeave={e => (e.currentTarget as HTMLDivElement).style.background = ''}>
                  <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>{ep.show_title} · </span>
                  S{String(ep.season).padStart(2,'0')}E{String(ep.episode).padStart(2,'0')} — {ep.title}
                </div>
              ))
            )}
          </div>
        </div>
      )}
    </div>
  )
}

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
