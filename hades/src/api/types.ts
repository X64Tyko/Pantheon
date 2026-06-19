export interface Source {
  source_id:    string
  source_type:  'plex' | 'jellyfin' | 'emby' | 'local'
  display_name: string
  base_url:     string
  enabled:      boolean
}

export interface SourceType {
  type:         string
  display_name: string
  supported:    boolean
}

export interface LibraryInfo {
  external_lib_id: string
  name:            string
  type:            'show' | 'movie' | 'mixed'
}

export interface LibraryWithSource {
  library_id:   string
  source_id:    string
  display_name: string
  library_type: 'show' | 'movie' | 'mixed'
  source_name:  string
  source_type:  string
}

export interface PagedResult<T> {
  items: T[]
  total: number
}

export interface Library {
  library_id:      string
  source_id:       string
  external_lib_id: string
  display_name:    string
  library_type:    'show' | 'movie' | 'mixed'
  enabled:         boolean
}

export type AdvanceMode = 'scheduled' | 'on_play'

export interface Channel {
  channel_id:               string
  name:                     string
  number:                   number
  timezone:                 string
  seed?:                    number
  advance_mode?:            AdvanceMode     // default: 'scheduled'
  default_filler_entries:   FillerEntry[]
  default_filler_selection: FillerSelectionMode
}

// ── List-view types (minimal) ────────────────────────────────────────────────

export interface Show {
  show_id:        string
  title:          string
  content_rating: string
  episode_count:  number
}

export interface Movie {
  movie_id:       string
  title:          string
  content_rating: string
  duration_ms:    number
  year?:          number
}

// ── Detail types (full metadata) ─────────────────────────────────────────────

export interface ShowDetail {
  show_id:                 string
  title:                   string
  content_rating:          string
  overview:                string
  year?:                   number
  studio:                  string
  status:                  string
  genres:                  string[]
  thumb:                   string
  art:                     string
  imdb_id:                 string
  tvdb_id:                 string
  tmdb_id:                 string
  originally_available_at: string
  audience_rating?:        number
  locked:                  boolean
  episode_count:           number
  external_id:             string
  source_id:               string
  source_base_url:         string
}

export interface MovieDetail {
  movie_id:         string
  title:            string
  content_rating:   string
  duration_ms:      number
  year?:            number
  overview:         string
  tagline:          string
  studio:           string
  director:         string
  genres:           string[]
  thumb:            string
  art:              string
  imdb_id:          string
  tmdb_id:          string
  audience_rating?: number
  locked:           boolean
  external_id:      string
  source_id:        string
  source_base_url:  string
}

export interface Episode {
  episode_id:  string
  season:      number
  episode:     number
  title:       string
  duration_ms: number
  overview:    string
  air_date:    string
  thumb:       string
}

export interface CredentialStatus {
  has_token:   boolean
  has_user_id: boolean
}

export interface PathMap {
  from: string
  to:   string
}

export type BlockType              = 'episode' | 'premier' | 'filler' | 'movie'
export type Advancement            = 'sequential' | 'shuffle' | 'smart_shuffle' | 'rerun_shuffle' | 'rerun_smart'
export type NoHistoryBehavior      = 'normal' | 'fallback_all' | 'exclude' | 'skip'
export type StartScope             = 'block' | 'episode'
export type FillerAdvancement      = 'sequential' | 'shuffle'
export type FillerEntryAdvancement = 'sequential' | 'shuffle' | 'sized'
export type FillerSelectionMode    = 'round_robin' | 'random' | 'weighted'
export type CursorScope            = 'global' | 'channel' | 'block'
export type ContentType            = 'show' | 'movie' | 'episode' | 'playlist' | 'filler_list'

export interface FillerEntry {
  id:             number
  filler_list_id: string
  title:          string                // display name, populated server-side
  advancement:    FillerEntryAdvancement
  weight:         number                // for 'weighted' selection; default 1
  position:       number                // round-robin order
}

export type EpisodeOrder = 'season' | 'absolute' | 'airdate'

export interface BlockContent {
  id:               number
  block_id:         string
  content_type:     ContentType
  content_id:       string
  position:         number
  season_filter?:   number        // only for content_type='show'; absent = all seasons
  weight:           number        // weighted selection probability (rerun modes)
  run_count:        number        // sequential episodes per selection (rerun modes)
  include_specials: boolean       // include season 0 episodes
  episode_order:    EpisodeOrder  // 'season' | 'absolute' | 'airdate'
  title:            string        // display-ready label (computed server-side)
}

export type ExportDepth = 'shallow' | 'deep'

export interface ChannelExportFillerEntry {
  title:       string
  advancement: FillerEntryAdvancement
  weight:      number
}

export interface ChannelExportContent {
  content_type:  ContentType
  title:         string
  weight:        number
  run_count:     number
  season_filter?: number
  // deep only — shows
  imdb_id?:      string
  tvdb_id?:      string
  tmdb_id?:      string
  // deep only — episodes
  season?:       number
  episode?:      number
  show_imdb_id?: string
  show_tvdb_id?: string
  show_tmdb_id?: string
}

export interface ChannelExportBlock {
  name:                     string
  block_type:               BlockType
  day_mask:                 number
  start_time:               string
  end_time?:                string
  program_count:            number
  priority:                 number
  advancement:              Advancement
  cursor_scope:             CursorScope
  max_content_rating:       string
  late_start_mins:          number
  early_start_secs:         number
  align_to_mins:            number
  inter_filler:             boolean
  filler_selection:         FillerSelectionMode
  smart_pct:                number
  start_scope:              StartScope
  no_history_behavior:      NoHistoryBehavior
  max_consecutive_episodes: number
  content:                  ChannelExportContent[]
  filler_entries:           ChannelExportFillerEntry[]
}

export interface ChannelExport {
  kairos_export: number
  depth:         ExportDepth
  channel: {
    name:                     string
    number:                   number
    timezone:                 string
    advance_mode?:            AdvanceMode
    default_filler_selection: FillerSelectionMode
    seed?:                    number
    default_filler_entries:   ChannelExportFillerEntry[]
  }
  blocks: ChannelExportBlock[]
}

export interface ImportResult {
  channel_id: string
  unresolved: { block_name: string; content_type: string; title: string; reason: string }[]
}

export interface Block {
  block_id:            string
  channel_id:          string
  name:                string
  block_type:          BlockType
  day_mask:            number   // Sun=1 Mon=2 Tue=4 Wed=8 Thu=16 Fri=32 Sat=64
  start_time:          string   // "HH:MM"
  end_time?:           string   // "HH:MM" hard cutoff — absent means no time limit
  program_count:       number   // stop after N programs; 0 = no limit
  late_start_mins:     number   // block may start up to N min late if preempted; 0 = strict
  early_start_secs:    number   // block may start up to N sec early to absorb trailing flex; 0 = strict
  advancement:         Advancement
  cursor_scope:        CursorScope
  priority:            number
  max_content_rating:  string
  filler_entries:      FillerEntry[]    // empty = inherit channel default
  filler_selection:    FillerSelectionMode
  align_to_mins:       number           // 0=none; 15/30/60 = snap first program to next N-min boundary
  inter_filler:        boolean          // insert filler between programs
  smart_pct:           number           // cooldown threshold % for smart_shuffle / rerun_smart
  start_scope:         StartScope       // 'block' = align/early/late on block entry; 'episode' = per-item
  no_history_behavior:        NoHistoryBehavior
  max_consecutive_episodes:   number           // 0 = unlimited; rerun modes only
  content:                    BlockContent[]
}

export interface EpisodeGroupMember {
  id:         number
  episode_id: string
  part_num:   number
  season:     number
  episode:    number
  title:      string
}

export interface EpisodeGroup {
  group_id:   string
  name:       string
  group_type: 'multipart'
  members:    EpisodeGroupMember[]
}

// ── Plex link metadata ────────────────────────────────────────────────────────

export interface PlexLink {
  source_id:     string
  external_id:   string
  plex_type:     'playlist' | 'collection'
  last_synced_at: number | null
}

// ── Playlists ────────────────────────────────────────────────────────────────

export type PlaylistMode = 'sequential' | 'show_collection'

export interface Playlist {
  playlist_id: string
  title:       string
  mode:        PlaylistMode
  item_count:  number
  total_ms:    number
  plex_link?:  PlexLink
}

export interface PlaylistItem {
  id:          number
  position:    number
  item_type:   'episode' | 'movie'
  item_id:     string
  title:       string
  duration_ms: number
  season?:     number
  episode?:    number
}

export interface PlaylistDetail extends Omit<Playlist, 'item_count' | 'total_ms'> {
  items: PlaylistItem[]
}

// ── Filler lists ─────────────────────────────────────────────────────────────

export interface FillerList {
  filler_list_id: string
  title:          string
  advancement:    FillerAdvancement
  item_count:     number
  total_ms:       number
  plex_link?:     PlexLink
}

export interface FillerListItem {
  id:          number
  item_type:   'episode' | 'movie'
  item_id:     string
  position:    number
  title:       string
  duration_ms: number
}

export interface FillerListDetail extends Omit<FillerList, 'item_count' | 'total_ms'> {
  items: FillerListItem[]
}

// ── Episode search result ─────────────────────────────────────────────────────

export interface EpisodeSearchResult {
  episode_id:  string
  season:      number
  episode:     number
  title:       string
  duration_ms: number
  show_id:     string
  show_title:  string
}

export interface EpgProgram {
  item_type:           'episode' | 'movie' | 'filler'
  item_id:             string
  block_id:            string
  title:               string
  show_title?:         string
  show_id?:            string
  season?:             number
  episode_num?:        number
  duration_ms:         number
  wall_clock_start_ms: number
  wall_clock_end_ms:   number
  status:              string
}

// ── Plex browse (playlists / collections) ────────────────────────────────────

export interface PlexBrowseList {
  id:         string
  title:      string
  item_count: number
}

export interface PlexBrowseItem {
  item_type:   'episode' | 'movie'
  kairos_id:   string
  title:       string
  duration_ms: number
  show_title?: string
  season?:     number
  episode?:    number
  available:   boolean
}

// ── Downloads ─────────────────────────────────────────────────────────────────

export interface DownloadJob {
  id:         string
  url:        string
  dest_path:  string
  status:     'queued' | 'running' | 'done' | 'error'
  progress:   number   // 0–100
  log:        string[]
  started_at: string   // ISO-8601
}
