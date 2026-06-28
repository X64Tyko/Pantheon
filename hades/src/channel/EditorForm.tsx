import { observer } from 'mobx-react-lite'
import type { Advancement, BlockType, CursorScope, NoHistoryBehavior, PlayStyle, StartScope } from '../api/types'
import {
  ALIGN_OPTS, BLOCK_META, DAYS, DAY_BITS, DELAY_OPTS, EARLY_OPTS, NO_HISTORY_OPTS,
} from './constants'
import { getLimitMode } from './utils'
import { inputStyle } from './styles'
import { HelpTip, HelpSection, GifSlot } from './HelpTip'
import { AccordionSection } from './sections'
import type { ChannelDetailStore } from './store'
import type { LimitMode } from './types'

export { AccordionSection } from './sections'
export { CardSection }      from './sections'

export const EditorForm = observer(function EditorForm({ channelId, store, limitMode }: {
  channelId: string
  store:     ChannelDetailStore
  limitMode: LimitMode
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
    sequential: 'Plays shows in order by position. COUNT on each show controls episodes before switching.',
    shuffle:    'Picks shows randomly each slot, weighted by WEIGHT. Episodes advance sequentially within each show.',
    smart:      'Weighted random show selection with cooldown — skips recently played episodes within each show.',
  }

  const cursorHint: Record<string, string> = {
    block:   'Episode positions are tracked per block. The same show in two blocks plays independently.',
    channel: 'All blocks on this channel share episode positions for the same show.',
    global:  'Positions and rerun history are shared across all channels — a true cross-channel pool.',
  }

  const isTimeslot = d.block_type === 'timeslot'
  const isRerun    = d.play_style === 'rerun'

  const daysOn    = DAYS.filter((_, i) => (d.day_mask & DAY_BITS[i]) !== 0).map(([s]) => s)
  const daysStr   = daysOn.length === 7 ? 'Every day'
    : daysOn.length === 5 && (d.day_mask & 0x3e) === 0x3e ? 'Weekdays'
    : daysOn.length === 2 && (d.day_mask & 0x41) === 0x41 ? 'Weekends'
    : daysOn.join('·') || 'No days'
  const stopStr   = limitMode === 'programs' ? `${d.program_count}p` : limitMode === 'end' ? (d.end_time ?? 'open') : 'fill day'
  const timingStr = isTimeslot ? (d.start_time || '—') : `${d.start_time || '—'} · ${stopStr}`

  return (
    <div style={{ flex: 1, minHeight: 0, overflow: 'auto', padding: '12px 12px 20px' }} className="scrollbar-dark">

      {/* ── SCHEDULE ── */}
      <AccordionSection title="SCHEDULE" open={sec.schedule} onToggle={() => tog('schedule')} badge={<span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{daysStr}</span>}>
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
          {(['episode', 'movie', 'timeslot', 'filler'] as BlockType[]).map(t => {
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
      <AccordionSection title="TIMING" open={sec.timing} onToggle={() => tog('timing')} badge={<span style={{ fontSize: 10, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>{timingStr}</span>}>
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

        {isTimeslot && (
          <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9, marginBottom: 14 }}>
            <div>
              <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>PRIORITY</div>
              <input type="number" min={1} value={d.priority} onChange={e => store.setDraft('priority', Math.max(1, +e.target.value || 1))} style={inputStyle} />
              {sh && <div style={{ fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 4 }}>higher wins conflicts with movie blocks</div>}
            </div>
          </div>
        )}

        {!isTimeslot && (<>
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
        </>)}
      </AccordionSection>

      {/* ── PLAYBACK ── (hidden for timeslot) */}
      {!isTimeslot && <AccordionSection title="PLAYBACK" open={sec.playback} onToggle={() => tog('playback')}>
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
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>PLAY STYLE</div>
            <select value={d.play_style ?? 'standard'} onChange={e => store.setDraft('play_style', e.target.value as PlayStyle)} style={inputStyle}>
              <option value="standard">Standard</option>
              <option value="rerun">Rerun</option>
            </select>
          </div>
        </div>
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 9 }}>
          <div>
            <div style={{ fontSize: 9.5, letterSpacing: '0.16em', color: 'var(--hds-txt-3)', marginBottom: 5 }}>ORDER</div>
            <select value={d.advancement} onChange={e => store.setDraft('advancement', e.target.value as Advancement)} style={inputStyle}>
              <option value="sequential">Sequential</option>
              <option value="shuffle">Shuffle</option>
              <option value="smart">Smart</option>
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
        {d.advancement === 'smart' && (
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
        {isRerun && (
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
        {isRerun && (
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
        {isRerun && (
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
      </AccordionSection>}

    </div>
  )
})
