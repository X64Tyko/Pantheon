import type { BlockType, FillerEntryAdvancement, FillerSelectionMode, NoHistoryBehavior } from '../api/types'
import type { BlockDraft } from './types'

export const PPH_DEFAULT = 46
export const PPH_MIN = 22
export const PPH_MAX = 96
export const GUTTER_W = 58
export const DAY_MIN_W = 94

// day_mask bits: Sun=1 Mon=2 Tue=4 Wed=8 Thu=16 Fri=32 Sat=64  →  index 0=Mon…6=Sun
export const DAYS = [['Mo', 'MON'], ['Tu', 'TUE'], ['We', 'WED'], ['Th', 'THU'], ['Fr', 'FRI'], ['Sa', 'SAT'], ['Su', 'SUN']] as const
export const DAY_BITS = [2, 4, 8, 16, 32, 64, 1]

export const BLOCK_META: Record<BlockType, { name: string; bg: string; solid: string; edge: string; border: string }> = {
  episode: { name: 'Episode', bg: 'linear-gradient(160deg, oklch(0.37 0.095 287), oklch(0.31 0.08 287))', solid: 'oklch(0.36 0.09 287)', edge: 'oklch(0.74 0.13 287)', border: 'oklch(0.48 0.1 287)' },
  movie:   { name: 'Movie',   bg: 'linear-gradient(160deg, oklch(0.4 0.09 58), oklch(0.33 0.075 56))',     solid: 'oklch(0.39 0.085 58)',  edge: 'oklch(0.78 0.12 68)',  border: 'oklch(0.5 0.1 60)' },
  premier: { name: 'Premier', bg: 'linear-gradient(160deg, oklch(0.42 0.13 19), oklch(0.34 0.11 18))',     solid: 'oklch(0.41 0.12 18)',   edge: 'oklch(0.76 0.17 24)',  border: 'oklch(0.52 0.14 20)' },
  filler:  { name: 'Filler',  bg: 'linear-gradient(160deg, oklch(0.32 0.018 262), oklch(0.27 0.015 262))', solid: 'oklch(0.31 0.018 262)', edge: 'oklch(0.62 0.03 262)', border: 'oklch(0.42 0.02 262)' },
}

export const RATINGS = ['', 'TV-Y', 'TV-Y7', 'TV-G', 'TV-PG', 'TV-14', 'TV-MA']
export const DELAY_OPTS: [number, string][]     = [[0,'None'],[5,'5 min'],[10,'10 min'],[15,'15 min'],[30,'30 min'],[60,'1 hr']]
export const EARLY_OPTS: [number, string][]     = [[0,'None'],[15,'15 sec'],[30,'30 sec'],[60,'1 min'],[120,'2 min']]
export const ALIGN_OPTS: [number, string][]     = [[0,'None'],[15,':00/:15/:30/:45'],[30,':00/:30'],[60,'Top of hour']]
export const FILLER_ADV_OPTS: [FillerEntryAdvancement, string][] = [['sequential','Sequential'],['shuffle','Shuffle'],['sized','Sized']]
export const FILLER_SEL_OPTS: [FillerSelectionMode, string][]    = [['round_robin','Round-robin'],['random','Random'],['weighted','Weighted']]

export const NO_HISTORY_OPTS: [NoHistoryBehavior, string, string][] = [
  ['normal',       'Normal',       'Shows without premiers play as a regular episode show'],
  ['fallback_all', 'Fallback All', 'Use the full episode catalog as the rerun pool'],
  ['exclude',      'Exclude',      'Skip shows with no play history during selection'],
  ['skip',         'Skip',         'Leave the slot empty'],
]

export const BLANK_DRAFT: BlockDraft = {
  block_type: 'episode', day_mask: 62,
  start_time: '20:00', end_time: '21:00',
  program_count: 0, late_start_mins: 5, early_start_secs: 15,
  advancement: 'sequential', cursor_scope: 'block',
  priority: 1, max_content_rating: '',
  filler_selection: 'round_robin',
  align_to_mins: 0, inter_filler: false,
  smart_pct: 30, start_scope: 'block',
  no_history_behavior:        'normal',
  max_consecutive_episodes:   0,
}
