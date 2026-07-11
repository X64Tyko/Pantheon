export interface User {
  user_id:              string
  username:             string
  role:                 'admin' | 'viewer'
  restricted:           boolean
  max_tv_rating:        string
  max_movie_rating:     string
  max_channel_rating:   string
  // Set on invite-created accounts until the owner replaces the temp/
  // placeholder password with one of their own — gates the app shell.
  must_change_password: boolean
}

export interface ContentOverride {
  entity_type: 'show' | 'movie' | 'channel'
  entity_id:   string
  mode:        'allow' | 'block'
  title?:      string // present on GET, not needed on POST
}

export interface AuthResponse {
  token: string
  user:  User
}

export interface CastTokenResponse {
  token:      string
  session_id: string
}

export interface CastSessionInfo {
  session_id: string
  created_at: number
  last_seen:  number
}

// A user's paired Roku channel (kairos/src/db/RokuDeviceRepository.h) —
// "paired" false means it's mid-pairing (waiting for the channel to
// confirm), not yet a usable cast target.
export interface RokuDevice {
  id:         string
  name:       string
  ip_address: string
  app_id:     string
  paired:     boolean
  created_at: number
}

// Live playback state as last reported by the device itself (Hermes-owned,
// in-memory — hermes/src/devices/DeviceSession.h). Empty object if the
// device hasn't reported anything yet (e.g. just connected, nothing loaded).
export interface RokuDeviceState {
  id:    string
  state: {
    playing?:    boolean
    positionMs?: number
    durationMs?: number
    volume?:     number
    muted?:      boolean
  }
}

// Admin-only "who's connected" view (GET /api/devices/all) — every live
// Roku session regardless of owner, unlike RokuDeviceState's own-devices
// scope. contentType/contentId are the bare identifiers the device itself
// reported (PlayerScreen.brs); no title threaded through — resolve one on
// demand (channel list already loaded client-side; movie/episode need a
// lookup) rather than in the device's own frequent heartbeat.
export interface DeviceConnection {
  id:           string
  user_id:      string
  last_seen_ms: number
  state: {
    playing?:     boolean
    positionMs?:  number
    durationMs?:  number
    contentType?: 'channel' | 'movie' | 'episode'
    contentId?:   string
  }
}

export interface Source {
  source_id:    string
  source_type:  'plex' | 'jellyfin' | 'emby' | 'local'
  display_name: string
  base_url:     string
  enabled:      boolean
  // Local user (if any) whose watch/resume state is seeded from this
  // source's primary account during sync. Empty = off.
  synced_user_id: string
  // Empty = the last user-discovery sync succeeded (or none has run yet);
  // otherwise a human-readable reason it came back with nothing (bad token
  // permissions, HTTP error, etc.) — see IMediaSource::lastUserDiscoveryError.
  user_sync_error: string
}

// A source-reported account with no local Pantheon account imported for it
// yet — see SourceRepository::listUnmappedSourceUsers().
export interface UnmappedSourceUser {
  source_id:           string
  source_display_name: string
  external_user_id:    string
  display_name:        string
  email:                string
}

// Every discovered account for one source, mapped or not — see
// SourceRepository::listSourceUsers(). imported_user_id is '' when unlinked.
export interface SourceUser {
  external_user_id: string
  display_name:     string
  email:            string
  imported_user_id: string
}

export interface ImportUserResult {
  ok:             boolean
  user_id:        string
  username:       string
  invite_method:  'email' | 'temp_password'
  temp_password?: string
  invite_link?:   string
  invite_sent?:   boolean
  invite_error?:  string
}

// Response shape for POST /api/users with an invite mode — same idea as
// ImportUserResult but without invite_method/username (the caller already
// knows both, since it's the one supplying them).
export interface InviteUserResult {
  ok:             boolean
  user_id:        string
  temp_password?: string
  invite_link?:   string
  invite_sent?:   boolean
  invite_error?:  string
}

export interface SmtpConfig {
  host:            string
  port:            string
  username:        string
  has_password:    boolean
  from_address:    string
  public_base_url: string
}

export interface SourceType {
  type:         string
  display_name: string
  supported:    boolean
}

export interface LibraryInfo {
  external_lib_id: string
  name:            string
  type:            'show' | 'movie' | 'mixed' | 'music' | 'photo'
}

export interface LibraryWithSource {
  library_id:   string
  source_id:    string
  display_name: string
  library_type: 'show' | 'movie' | 'mixed' | 'music' | 'photo'
  source_name:  string
  source_type:  string
  show_on_home: boolean
}

export interface PagedResult<T> {
  items: T[]
  total: number
}

export interface Library {
  library_id:          string
  source_id:           string
  external_lib_id:     string
  display_name:        string
  library_type:        'show' | 'movie' | 'mixed' | 'music' | 'photo'
  preferred_scraper:   '' | 'tmdb' | 'tvdb' | 'anidb'
  preferred_language:  string
  // AniDB is anime-only — never queried for a library unless explicitly opted
  // in here (or preferred_scraper is set to 'anidb' outright).
  include_anidb:       boolean
  enabled:             boolean
  // Off for filler/bumper/commercial libraries — excluded from Home's
  // unscoped shelves, still fully usable for channel building and Library browsing.
  show_on_home:        boolean
  // On for filler/bumper/home-video libraries — this library's items never
  // enter the scraper match queue at all.
  skip_scraping:       boolean
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
  anchor_hashes?:           Record<string, AnchorSnapshot>
  audio_lang?:              string  // e.g. "eng"; empty = use global default
  subtitle_lang?:           string  // e.g. "eng"; empty = no subtitle
  stream_resolution?:       'source' | '1080p' | '720p' | '480p'
  stream_video_bitrate?:    number  // kbps; 0 = CRF/CQ auto
  stream_audio_bitrate?:    number  // kbps; default 192
  content_tag?:             string  // admin-assigned rating tag, TV scale; empty = unrated (fails closed for restricted accounts)
}

export interface AnchorSnapshot {
  rng:          string
  cursors:      unknown[]
  block_states: unknown[]
}

export interface EpgPreviewResponse {
  programs: EpgProgram[]
  anchors:  Record<string, AnchorSnapshot>
}

export interface MediaLanguages {
  audio:    string[]
  subtitle: string[]
}

export interface VideoInfo {
  codec:     string
  width:     number
  height:    number
  bit_depth: number
}

export interface ActivitySession {
  id:              string
  kind:            'channel' | 'vod'
  title:           string
  file_path:       string
  hw_accel:        string
  decode_hw_accel: string
  started_at_ms:   number
  direct_play?:    boolean // vod only
}

// ── List-view types (minimal) ────────────────────────────────────────────────

export type MatchStatus = 'matched' | 'uncertain' | 'unmatched' | 'unscraped'

// Only present when the show list was fetched with sort=recently_aired —
// see api.getShows()/HomePage's Recently Aired shelf.
export interface LatestAiredEpisode {
  episode_id: string
  season:     number
  episode:    number
  air_date:   string
}

export interface Show {
  show_id:         string
  title:           string
  content_rating:  string
  episode_count:   number
  year?:           number
  thumb?:          string
  art?:            string
  source_base_url?: string
  library_id?:     string
  audience_rating?: number
  match_status?:   MatchStatus
  match_score?:    number | null
  latest_episode?: LatestAiredEpisode
}

export interface Movie {
  movie_id:        string
  title:           string
  content_rating:  string
  duration_ms:     number
  year?:           number
  release_date?:   string
  thumb?:          string
  art?:            string
  source_base_url?: string
  library_id?:     string
  audience_rating?: number
  match_status?:   MatchStatus
  match_score?:    number | null
}

// ── Detail types (full metadata) ─────────────────────────────────────────────

// One media_source (Plex/Jellyfin/local) a show/movie is mapped to — can be more than one.
export interface MediaSourceRef {
  source_id:    string
  source_type:  string
  display_name: string
  external_id:  string
}

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
  skip_scraping:           boolean
  find_specials:           boolean
  episode_display_order:   'season' | 'aired'
  episode_count:           number
  seasons:                 { number: number; name: string }[]
  external_id:             string
  source_id:               string
  source_base_url:         string
  match_status?:           MatchStatus
  match_score?:            number
  match_confirmed?:        boolean
  folder_path?:            string
  sources:                 MediaSourceRef[]
}

export interface MovieDetail {
  movie_id:         string
  title:            string
  content_rating:   string
  duration_ms:      number
  year?:            number
  release_date?:    string
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
  skip_scraping:    boolean
  external_id:      string
  source_id:        string
  source_base_url:  string
  file_path?:       string
  folder_path?:     string
  match_status?:    MatchStatus
  match_score?:     number
  match_confirmed?: boolean
  sources:          MediaSourceRef[]
}

export interface WritebackResult {
  results: { source_id: string; source_type: string; ok: boolean }[]
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
  file_path?:  string
}

export interface ExternalId {
  source:      string
  external_id: string
  priority:    number
}

export interface ItemMetadata {
  external_ids:     ExternalId[]
  alternate_titles: string[]
}

export interface WatchProgress {
  content_type: 'movie' | 'episode'
  content_id:   string
  position_ms:  number
  duration_ms:  number
  updated_at:   number
  title:        string
  // episode only
  season?:      number
  episode?:     number
  show_id?:     string
  show_title?:  string
  // true when this card is a synthesized "next episode" placeholder for a
  // show whose most recently watched episode was completed — position_ms is
  // always 0 here since it hasn't actually been started (see Kairos's
  // GET /api/watch-progress).
  up_next?:     boolean
}

export interface CredentialStatus {
  has_token:   boolean
  has_user_id: boolean
}

// GET /api/channels/:id/now — what's currently airing on a linear channel,
// including wall-clock timing so the player can compute elapsed position
// within the item (there's no per-item scrubber for a live channel).
export interface ChannelNow {
  item_type:           string
  item_id:             string
  file_path:           string
  duration_ms:         number
  title:               string
  block_id:            string
  wall_clock_start_ms: number
  wall_clock_end_ms:   number
  is_filler:           boolean
  show_title?:         string
  show_id?:            string
  season?:             number
  episode_num?:        number
  source_id?:          string
  external_id?:        string
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

// ── Show specials linking ─────────────────────────────────────────────────────

export interface SpecialCandidate {
  candidate_id:     string
  episode_number:   number
  special_title:    string
  special_overview: string
  special_air_date: string
  special_thumb:    string
  source:           'tmdb' | 'tvdb' | 'anidb'
  score:            number   // 0-1
  accepted:         -1 | 0 | 1
  movie_id:         string
  movie_title:      string
  movie_year:       number
}

export interface LinkedSpecial {
  episode_id:         string
  episode_number:     number
  title:              string
  linked_movie_id:    string
  linked_movie_title: string
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
  file_path?:          string
  overview?:           string
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

// ── Playlist export/import — same portable-JSON pattern as ChannelExport,
// letting a playlist be shared/moved between Pantheon instances (or people).
export interface PlaylistExportItem {
  content_type:  'movie' | 'episode'
  title:         string
  year?:         number
  season?:       number
  episode?:      number
  tvdb_id?:      string
  imdb_id?:      string
  tmdb_id?:      string
}

export interface PlaylistExport {
  kairos_export: number
  depth:         ExportDepth
  playlist: {
    title: string
    mode:  PlaylistMode
  }
  items: PlaylistExportItem[]
}

export interface PlaylistImportResult {
  playlist_id: string
  unresolved:  { content_type: string; title: string; reason: string }[]
}

export interface PlaylistImportPreviewItem {
  content_type: string
  title:        string
  resolved:     boolean
  year?:        number
  season?:      number
  episode?:     number
}

export interface PlaylistImportPreviewResult {
  items:            PlaylistImportPreviewItem[]
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
  source:          ScraperSource
  api_key:         string
  enabled:         boolean
  language:        string
  language_weight: number
  pin?:            string  // TVDB subscriber pin
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

// Present on an accept/manual-match response when the confirmed item turned
// out to duplicate an external id already owned by a different library item
// — the accepted item was merged into this one and deleted, rather than
// becoming its own separate match.
export interface MergedInto {
  kairos_id: string
  item_type: 'show' | 'movie'
  title:     string
}

// A pair SyncManager's sync-time dedup flagged as an "uncertain" possible
// duplicate (fuzzy title match, or a folder match that only succeeded via a
// case-insensitive fallback) rather than auto-merging outright.
export interface DuplicateCandidate {
  candidate_id:     string
  item_type:        'show' | 'movie'
  kairos_id_a:      string
  kairos_id_b:      string
  title_a:          string
  title_b:          string
  year_a?:          number
  year_b?:          number
  thumb_a:          string
  thumb_b:          string
  trigger:          'fuzzy_title' | 'folder_uncertain' | 'both'
  reason:           string
  title_similarity: number
  folder_a:         string
  folder_b:         string
  created_at:       string
}

export interface ReviewQueueItem {
  kairos_id:       string
  item_type:       'show' | 'movie'
  title:           string
  year?:           number
  thumb:           string
  source_id:       string
  source_base_url: string
  match_status:    'uncertain' | 'unmatched' | 'matched'
  match_score:     number
  candidates:      ItemMatchCandidate[]
  folder_path?:    string
}

export type ChapterType = 'pre_roll' | 'intro' | 'recap' | 'ad_break' | 'chapter' | 'credits' | 'post_credits' | 'outro' | 'next_time' | 'unclassified'

export interface Chapter {
  chapter_id:   string
  media_type:   'episode' | 'movie'
  media_id:     string
  chapter_type: ChapterType
  title:        string
  start_ms:     number
  end_ms:       number
  position:     number
  source:       'manual' | 'plex_intro' | 'plex_chapters' | 'file' | 'detected'
  locked:       boolean
}

// One episode/movie with its chapters embedded, for the Chapters review tab.
export interface ChapterReviewItem {
  media_type:  'episode' | 'movie'
  media_id:    string
  title:       string
  thumb:       string
  file_path:   string
  duration_ms: number
  year?:       number
  chapters:    Chapter[]
}

// GET /api/episodes/:id/next — the next playable episode after this one, or
// null if it's the last (see ContentRepository::getNextEpisode).
export interface NextEpisode {
  episode_id:  string
  season:      number
  episode:     number
  title:       string
  duration_ms: number
  overview:    string
  air_date:    string
  thumb:       string
}

// GET /api/shows/:id/watch-state — the most-recently-touched episode's watch
// progress for a show, completed or not (see resolvePlayTarget.ts).
export interface ShowWatchState {
  content_id:  string
  position_ms: number
  duration_ms: number
  completed:   boolean
  updated_at:  number
}

export interface ScraperStats {
  total:     number
  matched:   number
  uncertain: number
  unmatched: number
  unscraped: number
  skipped:   number
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
  library_id?:      string // this library's show_id/movie_id when in_library
  request_status?:  'pending' | 'approved' | 'rejected' // set if ANYONE has already requested this, regardless of in_library
}
