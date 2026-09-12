import { observer } from 'mobx-react-lite'
import type {
    Advancement,
    BlockType,
    CursorScope,
    FillerSelectionMode,
    NoHistoryBehavior,
    PlayStyle,
    StartScope
} from '../api/types'
import {
    ALIGN_OPTS, BLOCK_META, DAYS, DAY_BITS, DELAY_OPTS, EARLY_OPTS, FILLER_SEL_OPTS, NO_HISTORY_OPTS,
} from './constants'
import { getLimitMode } from './utils'
import { HelpTip, HelpSection, GifSlot } from './HelpTip'
import { AccordionSection } from './sections'
import type { ChannelDetailStore } from './store'
import type { LimitMode } from './types'
import styles from './EditorForm.module.css'
import shared from './sharedStyles.module.css'

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
    // Episode-scope Align Start always pads gaps with real filler content to
    // reach each boundary (RuleEngine.cpp), independently of inter_filler —
    // that's a deliberate backend design (alignment needs *some* padding
    // mechanism, filler is the only one implemented), but it left the
    // checkbox below looking dead: unchecking it has no effect while this is
    // true, since the scheduler never consults it in this mode.
    const alignmentForcesFiller = (d.start_scope ?? 'block') === 'episode' && d.align_to_mins > 0

  const daysOn    = DAYS.filter((_, i) => (d.day_mask & DAY_BITS[i]) !== 0).map(([s]) => s)
  const daysStr   = daysOn.length === 7 ? 'Every day'
    : daysOn.length === 5 && (d.day_mask & 0x3e) === 0x3e ? 'Weekdays'
    : daysOn.length === 2 && (d.day_mask & 0x41) === 0x41 ? 'Weekends'
    : daysOn.join('·') || 'No days'
  const stopStr   = limitMode === 'programs' ? `${d.program_count}p` : limitMode === 'end' ? (d.end_time ?? 'open') : 'fill day'
  const timingStr = isTimeslot ? (d.start_time || '—') : `${d.start_time || '—'} · ${stopStr}`

  return (
    <div className={`${styles.root} scrollbar-dark`}>

      {/* ── SCHEDULE ── */}
      <AccordionSection title="SCHEDULE" open={sec.schedule} onToggle={() => tog('schedule')} badge={<span className={styles.badgeText}>{daysStr}</span>}>
        <div className={styles.sectionLabelRow}>
          BLOCK TYPE
          <HelpTip title="Block Types" tip="What each block type does">
            <HelpSection title="Episode">
              Plays TV show episodes from your content list. The <b className={shared.boldTxt}>ORDER</b> setting controls how the engine cycles through shows — sequential rotation, weighted random, or rerun modes that draw from play history.
            </HelpSection>
            <HelpSection title="Movie">
              Plays individual movies, one per selection. The block advances through your movie list according to the ORDER setting.
            </HelpSection>
            <HelpSection title="Timeslot">
              <p className={shared.p10}>Fixed-time scheduling for classic programming blocks (e.g., <b>Toonami</b>, <b>One Saturday Morning</b>).</p>
              <p className={shared.p10}>Timeslot blocks contain multiple slots at exact times. Each slot can rotate through a list of shows based on premiere dates or simple exhaustion, ensuring your schedule stays perfectly aligned with the wall clock.</p>
            </HelpSection>
            <HelpSection title="Filler">
              Fills dead air with short clips (commercials, bumpers) from a filler pool. Filler blocks draw from filler lists attached to any block's FILLER section, or the channel default if none are set. No main content list is needed.
            </HelpSection>
          </HelpTip>
        </div>
        <div className={styles.segmentGrid4}>
          {(['episode', 'movie', 'timeslot', 'filler'] as BlockType[]).map(t => {
            const tm = BLOCK_META[t]
            const on = d.block_type === t
            return (
              // Block-type colors come from BLOCK_META, a small closed set of
              // per-type accent colors (not design tokens) — same "data-
              // driven, not tokenizable" exception as DayColumn.tsx's block
              // rendering.
              <button key={t} onClick={() => store.setDraft('block_type', t)} className={styles.blockTypeButton}
                style={{ border: `1px solid ${on ? tm.edge : 'var(--hds-line)'}`, background: on ? tm.solid : 'var(--hds-bg-3)', color: on ? 'var(--hds-txt)' : 'var(--hds-txt-2)' }}>
                {tm.name}
              </button>
            )
          })}
        </div>

        <div className={styles.rowBetween}>
          <div className={styles.sectionLabel}>DAYS</div>
          <span className={`${styles.hintTextTight} ${styles.hintTextTightFlush}`}>drag to paint</span>
        </div>
        <div className={styles.dayRow}>
          {DAYS.map(([short], i) => {
            const bit = DAY_BITS[i]
            const on  = (d.day_mask & bit) !== 0
            return (
              <div key={short} onMouseDown={() => store.dayDown(i)} onMouseEnter={() => store.dayEnter(i)}
                className={`${styles.dayCell} ${on ? styles.dayCellActive : ''}`}>
                {short}
              </div>
            )
          })}
        </div>
        <div className={styles.dayPresetRow}>
          {[['Weekdays', 62], ['Weekend', 65], ['Every day', 127], ['Clear', 0]].map(([label, mask]) => (
            <button key={label} onClick={() => store.setDraft('day_mask', mask as number)} className={styles.dayPresetButton}>
              {label}
            </button>
          ))}
        </div>
      </AccordionSection>

      {/* ── TIMING ── */}
      <AccordionSection title="TIMING" open={sec.timing} onToggle={() => tog('timing')} badge={<span className={styles.badgeText}>{timingStr}</span>}>
        <div className={styles.timingGrid3}>
          <div>
            <div className={styles.fieldLabel}>START TIME</div>
            <input type="time" value={d.start_time} onChange={e => store.setDraft('start_time', e.target.value)} className={shared.input} />
          </div>
          <div>
            <div className={styles.fieldLabelRow}>
              LATE START
              <HelpTip title="Late Start" tip="Allow this block to fire late if its slot is taken">
                <p className={shared.p12}>A higher-priority block overrunning into this block's scheduled start will normally cause this block to be skipped for that day.</p>
                <p className={shared.p12}>With Late Start set to N minutes, the block will still fire — up to N minutes after its scheduled start — instead of being dropped. It still ends at its normal time or program count, so a late start means fewer programs play that run.</p>
                <p className={shared.p4}><b className={shared.boldTxt}>Example:</b> a movie block (priority 5) runs until 22:15, pushing into a 22:00 news block (priority 3). Without Late Start the news block is skipped. With Late Start = 20 min, news fires at 22:15.</p>
                <GifSlot label="Higher-priority block overruns; lower-priority block fires late within tolerance" />
              </HelpTip>
            </div>
            <select value={String(d.late_start_mins)} onChange={e => store.setDraft('late_start_mins', +e.target.value)} className={shared.input}>
              {DELAY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {sh && d.late_start_mins > 0 && (
              <div className={styles.hintText}>
                Block may start up to {d.late_start_mins} min late if preempted by a higher-priority block.
              </div>
            )}
          </div>
          <div>
            <div className={styles.fieldLabelRow}>
              EARLY START
              <HelpTip title="Early Start" tip="Steal dead air before this block's scheduled start">
                <p className={shared.p12}>If the block before this one ends early and leaves unscheduled time, Early Start lets this block claim that gap.</p>
                <p className={shared.p12}>With Early Start set to N seconds, the block may begin up to N seconds before its scheduled start time. The first program plays normally — it just begins sooner.</p>
                <p className={shared.p4}><b className={shared.boldTxt}>Example:</b> a filler block ends at 21:58:30, leaving 90 seconds before a 22:00 episode block. With Early Start = 120s, the episode block begins at 21:58:30 instead of waiting for 22:00.</p>
                <GifSlot label="Previous block ends early; this block claims the dead air gap" />
              </HelpTip>
            </div>
            <select value={String(d.early_start_secs)} onChange={e => store.setDraft('early_start_secs', +e.target.value)} className={shared.input}>
              {EARLY_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
            </select>
            {sh && d.early_start_secs > 0 && (
              <div className={styles.hintText}>
                Block may steal up to {d.early_start_secs}s of trailing dead air from the previous block.
              </div>
            )}
          </div>
        </div>

        {isTimeslot && (
          <div className={styles.timingGrid2}>
            <div>
              <div className={styles.fieldLabel}>PRIORITY</div>
              <input type="number" min={1} value={d.priority} onChange={e => store.setDraft('priority', Math.max(1, +e.target.value || 1))} className={shared.input} />
              {sh && <div className={`${styles.hintText} ${styles.hintTextMicroSize}`}>higher wins conflicts with movie blocks</div>}
            </div>
          </div>
        )}

        {!isTimeslot && (<>
        <div className={`${styles.sectionLabel} ${styles.sectionLabelMb}`}>STOP CONDITION</div>
        <div className={styles.segmentGrid3}>
          {([['programs', '# Programs'], ['end', 'End time'], ['fill', 'Fill day']] as [LimitMode, string][]).map(([k, label]) => {
            const on = limitMode === k
            return (
              <button key={k} onClick={() => store.setLimitMode(k)} className={`${styles.conditionButton} ${on ? styles.conditionButtonActive : ''}`}>
                {label}
              </button>
            )
          })}
        </div>
        {limitMode === 'programs' && (
          <input type="number" min={1} value={d.program_count} onChange={e => store.setDraft('program_count', Math.max(1, +e.target.value || 1))} placeholder="number of programs" className={`${shared.input} ${styles.fullWidthInputMb}`} />
        )}
        {limitMode === 'end' && (
          <input type="time" value={d.end_time ?? ''} onChange={e => store.setDraft('end_time', e.target.value)} className={`${shared.input} ${styles.fullWidthInputMb}`} />
        )}
        {sh && <div className={styles.hintTextLoose}>{limitHelp}</div>}

        <div className={styles.rowBetween}>
          <div className={`${styles.sectionLabelRow} ${styles.mbFlush}`}>
            ALIGN START
            <HelpTip title="Align Start" tip="Snap this block's start to a clock boundary">
              <HelpSection title="Block Scope">
                Snaps the first program of the block to the next upcoming boundary — :00, :15, :30, or :45. If a conflict delays the block to 20:07, it waits until 20:15 rather than starting mid-interval. Only fires once at block start.
              </HelpSection>
              <HelpSection title="Episode Scope">
                <p className={shared.p10}>Snaps each individual episode to the next boundary, creating a grid-locked schedule where every program starts on a clean time mark.</p>
                <p className={shared.p0}>Early Start and Late Start define the tolerance window around each snap point. If the tolerance is not enough to reach the next boundary, the episode may be skipped.</p>
              </HelpSection>
            </HelpTip>
          </div>
          {d.align_to_mins > 0 && (
            <div className={styles.alignScopeToggle}>
              {(['block', 'episode'] as const).map(scope => {
                const on = (d.start_scope ?? 'block') === scope
                return (
                  <button key={scope} onClick={() => store.setDraft('start_scope', scope)}
                    className={`${styles.alignScopeButton} ${on ? styles.alignScopeButtonActive : ''}`}>
                    {scope}
                  </button>
                )
              })}
            </div>
          )}
        </div>
        <select value={String(d.align_to_mins)} onChange={e => store.setDraft('align_to_mins', +e.target.value)} className={`${shared.input} ${styles.fullWidthInputMb}`}>
          {ALIGN_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
        </select>
        {sh && <div className={styles.hintTextLooseNoMargin}>
          {(d.start_scope ?? 'block') === 'episode'
            ? 'Snaps each episode to the next time boundary. Early/late start define the tolerance window.'
            : 'Snaps the first program of the block to the next time boundary.'}
        </div>}
        </>)}
      </AccordionSection>

      {/* ── PLAYBACK ── (hidden for timeslot) */}
      {!isTimeslot && <AccordionSection title="PLAYBACK" open={sec.playback} onToggle={() => tog('playback')}>
        <div className={styles.timingGrid2}>
          <div>
            <div className={styles.fieldLabelRow}>
              PRIORITY
              <HelpTip title="Block Priority" tip="How overlapping blocks are resolved">
                <p className={shared.p12}>When two blocks overlap on the same time slot, the higher-priority block wins the contested minutes. The lower-priority block is cut short at the conflict point.</p>
                <p className={shared.p12}>A lower-priority block that loses its entire start window is skipped for that day unless it has a <b className={shared.boldTxt}>Late Start</b> tolerance set.</p>
                <p className={shared.p12}>Priority only matters where blocks overlap. Non-overlapping blocks play independently of their priority values.</p>
                <p className={shared.p4}><b className={shared.boldTxt}>Tip:</b> use a low-priority 24/7 filler block as a catch-all that anything else can override, or set a high priority on a special event block to punch through a normal scheduled lineup.</p>
                <GifSlot label="Two overlapping blocks — higher priority wins the contested window; lower priority is cut short" />
              </HelpTip>
            </div>
            <input type="number" min={1} value={d.priority} onChange={e => store.setDraft('priority', Math.max(1, +e.target.value || 1))} className={shared.input} />
            {sh && <div className={`${styles.hintText} ${styles.hintTextMicroSize}`}>higher wins conflicts</div>}
          </div>
          <div>
            <div className={styles.fieldLabel}>PLAY STYLE</div>
            <select value={d.play_style ?? 'standard'} onChange={e => store.setDraft('play_style', e.target.value as PlayStyle)} className={shared.input}>
              <option value="standard">Standard</option>
              <option value="rerun">Rerun</option>
            </select>
          </div>
        </div>
        <div className={`${styles.timingGrid2} ${styles.mbFlush}`}>
          <div>
            <div className={styles.fieldLabel}>ORDER</div>
            <select value={d.advancement} onChange={e => store.setDraft('advancement', e.target.value as Advancement)} className={shared.input}>
              <option value="sequential">Sequential</option>
              <option value="shuffle">Shuffle</option>
              <option value="smart">Smart</option>
            </select>
            {sh && <div className={styles.hintText}>
              {orderHint[d.advancement]}
            </div>}
          </div>
          <div>
            <div className={styles.fieldLabelRow}>
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
            <select value={d.cursor_scope} onChange={e => store.setDraft('cursor_scope', e.target.value as CursorScope)} className={shared.input}>
              <option value="block">Per block</option>
              <option value="channel">Per channel</option>
              <option value="global">Global</option>
            </select>
            {sh && <div className={styles.hintText}>
              {cursorHint[d.cursor_scope]}
            </div>}
          </div>
        </div>
        <div className={styles.mt9}>
            <label className={`${styles.checkboxLabel} ${alignmentForcesFiller ? styles.checkboxLabelDisabled : ''}`}>
            <input type="checkbox"
                   checked={alignmentForcesFiller || (d.inter_filler ?? false)}
                   disabled={alignmentForcesFiller}
              onChange={e => store.setDraft('inter_filler', e.target.checked)} />
            <span className={styles.checkboxLabelText}>Insert filler clips between programs</span>
          </label>
            {alignmentForcesFiller && sh && <div className={styles.hintTextTight}>
                Automatic — Episode-scope Align Start already pads gaps with filler to reach each boundary,
                regardless of this setting.
            </div>}
        </div>
          {(alignmentForcesFiller || d.inter_filler) && (
              <div className={styles.mt9}>
                  <div className={styles.fieldLabelRow}>
                      FILLER SELECTION
                      <HelpTip title="Filler Selection"
                               tip="How this block picks which filler list to draw from when multiple are attached">
                          <HelpSection title="Round-robin (default)">
                              Cycles through the block's filler lists in a fixed order. With only one list attached,
                              this always draws from the same (only) list.
                          </HelpSection>
                          <HelpSection title="Random">
                              Picks a filler list at random each time. With only one list attached, this has no effect —
                              attach more than one to see variety here.
                          </HelpSection>
                          <HelpSection title="Weighted">
                              Picks a filler list at random, weighted by each list's own weight value.
                          </HelpSection>
                      </HelpTip>
                  </div>
                  <select value={d.filler_selection ?? 'round_robin'}
                          onChange={e => store.setDraft('filler_selection', e.target.value as FillerSelectionMode)}
                          className={shared.input}>
                      {FILLER_SEL_OPTS.map(([v, l]) => <option key={v} value={v}>{l}</option>)}
                  </select>
                  {sh && <div className={styles.hintTextTight}>
                      This picks which filler list to use — the clips within a list are ordered by that list's own
                      advancement setting (Sequential/Shuffle/Sized) on the Filler tab.
                  </div>}
              </div>
          )}
        {d.advancement === 'smart' && (
          <div className={styles.mt9}>
            <div className={styles.fieldLabel}>COOLDOWN THRESHOLD</div>
            <div className={styles.rangeRow}>
              <input type="range" min={5} max={80} step={5}
                value={d.smart_pct ?? 30}
                onChange={e => store.setDraft('smart_pct', Number(e.target.value))}
                className={styles.rangeInput} />
              <span className={styles.rangeValue}>{d.smart_pct ?? 30}%</span>
            </div>
            {sh && <div className={styles.hintTextTight}>
              Episodes won't repeat until {d.smart_pct ?? 30}% of the pool has played since last air
            </div>}
          </div>
        )}
        {isRerun && (
          <div className={styles.mt9}>
            <div className={styles.fieldLabel}>NO HISTORY BEHAVIOR</div>
            <select
              value={d.no_history_behavior ?? 'normal'}
              onChange={e => store.setDraft('no_history_behavior', e.target.value as NoHistoryBehavior)}
              className={shared.input}
            >
              {NO_HISTORY_OPTS.map(([v, label]) => <option key={v} value={v}>{label}</option>)}
            </select>
            {sh && <div className={styles.hintTextTight}>
              {NO_HISTORY_OPTS.find(([v]) => v === (d.no_history_behavior ?? 'normal'))?.[2]}
            </div>}
          </div>
        )}
        {isRerun && (
          <div className={styles.mt9}>
            <div className={styles.fieldLabel}>EPISODE LIMIT</div>
            <div className={styles.rangeRow}>
              <input type="number" min={0} step={1}
                value={d.max_consecutive_episodes ?? 0}
                onChange={e => store.setDraft('max_consecutive_episodes', Math.max(0, parseInt(e.target.value) || 0))}
                className={`${shared.input} ${styles.narrowInput}`} />
              <span className={styles.checkboxLabelText}>
                {(d.max_consecutive_episodes ?? 0) === 0 ? 'no limit' : 'max consecutive from the same show'}
              </span>
            </div>
            {sh && <div className={styles.hintText}>
              When the limit is hit the engine re-rolls show selection, forcing a switch even if the same show wins.
            </div>}
          </div>
        )}
        {isRerun && (
          <div className={styles.mt9}>
            <div className={styles.fieldLabel}>GROUP SNAP</div>
            <label className={styles.checkboxLabel}>
              <input type="checkbox"
                checked={d.snap_to_group_start ?? true}
                onChange={e => store.setDraft('snap_to_group_start', e.target.checked)} />
              <span className={styles.checkboxLabelText}>Snap to Part 1 when a multi-part episode is selected</span>
            </label>
            {sh && (d.snap_to_group_start ?? true) && (
              <div className={styles.hintText}>
                When a mid-group episode (Part 2+) is randomly selected, the run starts from Part 1. For shows without a premier block, this creates a lead-in rerun before an upcoming premiere.
              </div>
            )}
          </div>
        )}
      </AccordionSection>}

    </div>
  )
})
