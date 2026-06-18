import type { Block, BlockType } from '../api/types'
import type { BlockDraft, LimitMode, PickerTab } from './types'

export function t2m(t: string): number {
  if (!t) return 0
  const [h, m] = t.split(':').map(Number)
  return h * 60 + m
}

export function m2t(m: number): string {
  const mm = ((m % 1440) + 1440) % 1440
  return `${String(Math.floor(mm / 60)).padStart(2, '0')}:${String(mm % 60).padStart(2, '0')}`
}

export function endOf(block: Block): number {
  if (block.end_time) return t2m(block.end_time)
  if (block.program_count > 0) return Math.min(t2m(block.start_time) + 60, 1440)
  return 1440
}

export function getLimitMode(d: BlockDraft): LimitMode {
  if (d.end_time) return 'end'
  if (d.program_count > 0) return 'programs'
  return 'fill'
}

export function defaultPickerTab(t: BlockType): PickerTab {
  if (t === 'filler') return 'filler_lists'
  if (t === 'movie') return 'movies'
  return 'shows'
}

export function availablePickerTabs(t: BlockType): PickerTab[] {
  if (t === 'filler') return ['filler_lists']
  if (t === 'movie') return ['movies', 'playlists']
  return ['shows', 'episodes', 'movies', 'playlists']
}

export function normalizeBlock(b: Block): Block {
  return {
    ...b,
    filler_entries:   b.filler_entries   ?? [],
    filler_selection: b.filler_selection ?? 'round_robin',
    align_to_mins:    b.align_to_mins    ?? 0,
    inter_filler:     b.inter_filler     ?? false,
    early_start_secs: b.early_start_secs ?? 0,
  }
}

export function blockToDraft(block: Block): BlockDraft {
  return {
    block_type: block.block_type, day_mask: block.day_mask,
    start_time: block.start_time, end_time: block.end_time ?? '',
    program_count: block.program_count,
    late_start_mins: block.late_start_mins, early_start_secs: block.early_start_secs ?? 0,
    advancement: block.advancement, cursor_scope: block.cursor_scope,
    priority: block.priority, max_content_rating: block.max_content_rating,
    filler_selection: block.filler_selection ?? 'round_robin',
    align_to_mins: block.align_to_mins ?? 0, inter_filler: block.inter_filler ?? false,
    smart_pct: block.smart_pct ?? 30, start_scope: block.start_scope ?? 'block',
    no_history_behavior: block.no_history_behavior ?? 'normal',
  }
}
