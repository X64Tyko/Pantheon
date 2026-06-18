import type { Block } from '../api/types'

export type LimitMode = 'programs' | 'end' | 'fill'
export type PickerTab = 'shows' | 'movies' | 'episodes' | 'playlists' | 'filler_lists'
export type BlockDraft = Omit<Block, 'block_id' | 'channel_id' | 'content' | 'filler_entries'>
