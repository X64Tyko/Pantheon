export interface User {
  user_id:  string
  username: string
  role:     'admin' | 'viewer'
}

export interface AuthResponse {
  token: string
  user:  User
}

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
  offline_video_path?:      string
  offline_image_path?:      string
  offline_audio_id?:        string
  offline_audio_type?:      'episode' | 'movie' | ''
  offline_audio_title?:     string
  logo_path?:               string
  anchor_hashes?:           Record<string, number>
}

export interface EpgPreviewResponse {
  programs: EpgProgram[]
  anchors:  Record<string, number>
}

// ── List-view types (minimal) ────────────────────────────────────────────────

export type MatchStatus = 'matched' | 'uncertain' | 'unmatched' | 'unscraped'

export interface Show {
  show_id:         string
  title:           string
  content_rating:  string
  episode_count:   number
  year?:           number
  thumb?:          string
  art?:            string
  source_base_url?: string
  audience_rating?: number
  match_status?:   MatchStatus
  match_score?:    number | null
}

export interface Movie {
  movie_id:        string
  title:           string
  content_rating:  string
  duration_ms:     number
  year?:           number
  thumb?:          string
  art?:            string
  source_base_url?: string
  audience_rating?: number
  match_status?:   MatchStatus
  match_score?:    number | null
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
  seasons:                 { number: number; name: string }[]
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

export type BlockType              = 'episode' | 'filler' | 'movie' | 'timeslot'
export type SlotOverflow          = 'cutoff' | 'finish'
export type PrePremiereBehavior   = 'replay_previous' | 'filler' | 'skip'
export type PlayStyle              = 'standard' | 'rerun'
export type Advancement            = 'sequential' | 'shuffle' | 'smart'
export type NoHistoryBehavior      = 'normal' | 'fallback_all' | 'exclude' | 'skip'
export type StartScope             = 'block' | 'episode'
export type FillerAdvancement      = 'sequential' | 'shuffle'
export type FillerEntryAdvancement = 'sequential' | 'shuffle' | 'sized'
export type FillerSelectionMode    = 'round_robin' | 'random' | 'weighted'
export type CursorScope            = 'global' | 'channel' | 'block'
export type ContentType            = 'show' | 'movie' | 'episode' | 'playlist' | 'filler_list'

export interface TimeslotQueueEntry {
  entry_id:              string
  queue_index:           number
  content_type:          'show' | 'movie'
  content_id:            string
  title:                 string    // resolved server-side
  premiere_date:         string    // "YYYY-MM-DD" or ""
  pre_premiere_behavior: PrePremiereBehavior
}

export interface TimeslotSlot {
  slot_id:            string
  slot_index:         number
  slot_offset_mins:   number
  slot_duration_mins: number
  overflow:           SlotOverflow
  late_start_mins:    number
  early_start_secs:   number
  align_to_mins:      number
  start_scope:        StartScope
  queue_pos:          number
  episode_pos:        number
  queue:              TimeslotQueueEntry[]
}

export interface FillerEntry {
  id:            number
  content_type:  'show' | 'movie' | 'episode' | 'playlist' | 'filler_list'
  content_id:    string
  title:         string                // display name, populated server-side
  advancement:   FillerEntryAdvancement
  weight:        number                // for 'weighted' selection; default 1
  position:      number                // round-robin order
  season_filter?: number               // show only: null = all seasons, N = season N
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
  play_style:               PlayStyle
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

export type BumperMode = 'between' | 'filler'
export type BumperContentType = 'show' | 'episode' | 'playlist'

export interface ChannelBumper {
  id:            number
  channel_id:    string
  content_type:  BumperContentType
  content_id:    string
  mode:          BumperMode
  every_n:       number
  position:      number
  title?:        string
  season_filter?: number
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
  play_style:          PlayStyle
  advancement:         Advancement
  cursor_scope:        CursorScope
  priority:            number
  filler_entries:      FillerEntry[]    // empty = inherit channel default
  filler_selection:    FillerSelectionMode
  align_to_mins:       number           // 0=none; 15/30/60 = snap first program to next N-min boundary
  inter_filler:        boolean          // insert filler between programs
  smart_pct:           number           // cooldown threshold % for smart mode
  start_scope:         StartScope       // 'block' = align/early/late on block entry; 'episode' = per-item
  no_history_behavior:        NoHistoryBehavior
  max_consecutive_episodes:   number           // 0 = unlimited; rerun modes only
  snap_to_group_start:        boolean          // snap mid-group random pick to Part 1 (rerun modes)
  content:                    BlockContent[]
  slots?:                     TimeslotSlot[]   // non-empty only when block_type === 'timeslot'
  // Block intro/outro/interstitials
  intro_content_type:         string
  intro_content_id:           string
  outro_content_type:         string
  outro_content_id:           string
  interstitial_content_type:  string
  interstitial_content_id:    string
  interstitial_every_n:       number
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

export interface GroupingCandidatePart {
  episode_id: string
  title:      string
  season:     number
  episode:    number
  part_num:   number
  confirmed:  boolean
}

export interface GroupingCandidate {
  base_title:     string
  confidence:     number   // 0-100
  adjacent:       boolean
  already_grouped: boolean
  parts:          GroupingCandidatePart[]
}

export interface GroupingCandidatesResult {
  show_id:    string
  candidates: GroupingCandidate[]
}

export interface ShowGroupingResult {
  show_id:    string
  show_title: string
  candidates: GroupingCandidate[]
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

// ── Import preview ────────────────────────────────────────────────────────────

export interface ImportPreviewItem {
  content_type:  string
  title:         string
  resolved:      boolean
  tvdb_id?:      string
  imdb_id?:      string
  tmdb_id?:      string
  year?:         number
  season_filter?: number
}

export interface ImportPreviewBlock {
  name:         string
  block_type:   string
  advancement:  string
  day_mask:     number
  start_time:   string
  end_time?:    string
  program_count?: number
  content:      ImportPreviewItem[]
}

export interface ImportPreviewResult {
  blocks:           ImportPreviewBlock[]
  unresolved_count: number
}

// ── Arr integrations ──────────────────────────────────────────────────────────

// ── Content requests ──────────────────────────────────────────────────────────

export type RequestStatus = 'pending' | 'approved' | 'rejected'

export interface ContentRequest {
  request_id:   string
  user_id:      string
  content_type: 'show' | 'movie'
  source:       ScraperSource
  external_id:  string
  title:        string
  year?:        number | null
  poster_url:   string
  status:       RequestStatus
  created_at:   number
}

export interface ArrConfig {
  sonarr_url:     string
  sonarr_api_key: string
  radarr_url:     string
  radarr_api_key: string
}

export interface ArrLookupResult {
  title:        string
  year:         number
  external_id:  string
  poster_url:   string
  already_added: boolean
  add_data:     unknown  // opaque — sent back verbatim on /arr/add
}

export interface ArrQualityProfile {
  id:   number
  name: string
}

export interface ArrServiceOptions {
  quality_profiles: ArrQualityProfile[]
  root_folders:     string[]
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

// ── Library browser ──────────────────────────────────────────────────────────

export type LibraryDensity = 'minimal' | 'standard' | 'rich'

export interface MediaHeroItem {
  id:           string
  title:        string
  year?:        number
  overview?:    string
  backdrop_url?: string
  poster_url?:  string
  content_type: 'show' | 'movie'
  genres?:      string[]
  rating?:      number
}

// ── Scraper infrastructure ────────────────────────────────────────────────────

export type ScraperSource = 'tmdb' | 'tvdb' | 'anidb'

export interface ScraperConfig {
  source:    ScraperSource
  api_key:   string
  enabled:   boolean
  language:  string
  pin?:      string  // TVDB subscriber pin
}

export interface ScraperSettings {
  configs:         ScraperConfig[]
  match_threshold: number   // 0–1; default 1.0
}

export interface ItemMatchCandidate {
  candidate_id: string
  source:       ScraperSource
  external_id:  string
  title:        string
  year?:        number
  score:        number        // 0–1 confidence
  accepted:     boolean | null
  poster_url:   string
  overview:     string
}

export interface ReviewQueueItem {
  kairos_id:       string
  item_type:       'show' | 'movie'
  title:           string
  year?:           number
  thumb:           string
  source_id:       string
  source_base_url: string
  match_status:    'uncertain' | 'unmatched'
  match_score:     number
  candidates:      ItemMatchCandidate[]
}

export interface ScraperStats {
  total:     number
  matched:   number
  uncertain: number
  unmatched: number
  unscraped: number
}

export interface ScraperSearchResult {
  source:       ScraperSource
  external_id:  string
  title:        string
  year?:        number
  overview:     string
  poster_url:   string
  content_type: 'show' | 'movie'
  in_library:   boolean
}
