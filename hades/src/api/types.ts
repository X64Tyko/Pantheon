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
  // True if a PIN is configured for the profile-switch picker (see
  // ProfileSelectPage) — never the PIN itself, just whether to prompt for one.
  has_pin: boolean
  // Library-wide fallback audio/subtitle language — used by
  // GET /api/playback/:content_type/:id whenever no item-specific
  // preference exists (show/movie track-preference). '' means unset.
  default_audio_lang:    string
  default_subtitle_lang: string
    // Which page this account lands on after login/profile-switch — ''
    // (inherit the admin-configured global default), 'home', or 'guide'.
    default_landing_page: string
    // A self-created, passwordless demo-server account (see
    // api.createGuest) — always role 'viewer'. Drives the "Guest" badge on
    // UsersPage and the guest-only self-service setup/delete UI.
    is_guest: boolean
    // Admin grant of channel-builder access — see AuthStore::
    // updateChannelBuilderEnabled. Meaningless for a guest account, which
    // uses the separate guest_channel_builder_enabled server-wide setting
    // (api.getPublicSettings) instead.
    channel_builder_enabled: boolean
    // Epoch seconds — most recent activity across every session this account
    // has ever held (or created_at if it's never had one). Only meaningfully
    // populated on the admin Users list (api.getProfiles); 0 elsewhere.
    last_seen: number
}

// GET/PUT .../track-preference (shows, movies) — same shape both ways.
export interface TrackPreference {
  audio_lang:    string
  subtitle_lang: string
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

// One row of kairos's playback_history table (see
// kairos/src/db/PlaybackHistoryRepository.h) — an append-only "session" of
// watching a title, separate from watch_progress (current resume state).
// Same shape backs both GET /api/activity/history (past sessions) and
// GET /api/activity/active (sessions still being pinged right now, the
// cross-platform presence signal DeviceConnectionsPanel merges with Roku's
// own ECP heartbeat).
export interface PlaybackHistoryEntry {
  event_id:            string
  user_id:             string
    content_type: 'movie' | 'episode' | 'channel'
  content_id:           string
  title:                string
  device_type:          '' | 'web' | 'android-mobile' | 'android-tv' | 'roku' | 'cast'
    direct_stream: boolean
  started_at_ms:        number
  ended_at_ms:          number
  started_position_ms: number
  last_position_ms:    number
  duration_ms:          number
  completed:            boolean
}

// GET /api/activity/crash (Hermes's own aggregating route, NOT proxied to
// Kairos like most /api/* calls — see shared/crash/CrashHandler.h). Each
// field is the raw marker text for that service, empty if it hasn't crashed
// since its marker was last overwritten.
export interface CrashStatus {
  hermes:     string
  kairos:     string
  hephaestus: string
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
  sync_priority:   number
  // Whether a confirmed/refreshed scraper match automatically pushes
  // writeback to this source, instead of requiring the "Push to Sources"
  // button or a "Writeback All" run. Defaults false — writeback pushing to
  // a real external library shouldn't newly start happening on its own.
  auto_writeback: boolean
  // Per-field writeback opt-outs, all default true (preserving existing
  // manual/bulk-writeback behavior) — flip one off for a source you don't
  // want that specific field touched on.
  writeback_update_art:           boolean
  writeback_update_external_ids:  boolean
  writeback_update_collections:   boolean
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
  preferred_scraper:   '' | ScraperSource
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

export interface Channel {
  channel_id:               string
  name:                     string
  number:                   number
  timezone:                 string
  seed?:                    number
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
    // Disables Hephaestus's native/direct-stream bucket for this channel
    // entirely — every viewer gets the transcode bucket (smooth speed-based
    // drift correction, always had loudnorm) at the cost of direct-stream's
    // CPU/GPU savings. Default false/undefined: today's dual-bucket behavior.
    force_transcode?: boolean
  content_tag?:             string  // admin-assigned rating tag, TV scale; empty = unrated (fails closed for restricted accounts)
    // Server-derived on create (see POST /api/channels), never client-set —
    // undefined/absent means an admin-owned channel, same as today.
    // is_demo=true only for a guest's throwaway channel (excluded from the
    // real lineup); a real viewer's owned channel has is_demo=false.
    owner_user_id?: string
    is_demo?: boolean
  // Number of weeks to project backward on launch to stagger rerun cursors.
  // 0 (default) = disabled; applied automatically at channel creation and
  // manually via trigger_pre_seed in PATCH for an existing channel.
  pre_seed_weeks?: number
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
    direct_stream?: boolean // vod only
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
  // Distinct episodes with a completed watch_progress entry for the caller
  // — see kairos's ShowRow::watched_episode_count. 0 for a logged-out
  // caller, same as everywhere else user-scoped fields default when unset.
  watched_episode_count: number
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
  watched?:        boolean
  view_count?:     number
    is_multi_part?: boolean
}

// A truly interleaved show+movie result (see kairos's MixedSort.h) — used
// by the Library page's "All" content type and mixed-shelf browsing, not a
// separate "every movie before every show" section of the same grid.
export interface MixedIndexEntry {
    content_type: 'show' | 'movie' | 'episode'
    id: string
    title: string
}

export interface MixedMediaItem {
    content_type: 'show' | 'movie' | 'episode'
    id: string
    title: string
    thumb?: string
    art?: string
    library_id?: string
    year?: number
    audience_rating?: number
    watched: boolean
    view_count: number
    episode_count?: number // shows only
    duration_ms?: number // episodes only
    season?: number // episodes only
    episode?: number // episodes only
    show_id?: string // episodes only
    show_title?: string // episodes only
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
  original_title:          string
    // Display-language override for this show — '' inherits the library's
    // preferred_language (itself falling back to the scraper default).
    preferred_language: string
  content_rating:          string
  overview:                string
  year?:                   number
  studio:                  string
  status:                  string
  genres:                  string[]
  tags:                    string[]
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
  watched_episode_count:   number
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
  original_title:   string
    // See ShowDetail's identical field.
    preferred_language: string
  content_rating:   string
  duration_ms:      number
  year?:            number
  release_date?:    string
  overview:         string
  tagline:          string
  studio:           string
  director:         string
  writer:           string
  genres:           string[]
  tags:             string[]
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
  watched?:         boolean
  view_count?:      number
    is_multi_part?: boolean
    parts?: MoviePart[]
}

// ── Multi-part movies (GitHub #3) ─────────────────────────────────────────────

export interface MoviePart {
    part_num: number
    file_path: string
    duration_ms: number
}

export interface MovieGroupingCandidatePart {
    movie_id: string
    title: string
    file_path: string
    year?: number
    part_num: number
}

export interface MovieGroupingCandidate {
    base_title: string
    confidence: number // 0-100
    parts: MovieGroupingCandidatePart[]
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
  watched?:    boolean
  view_count?: number
}

export interface ExternalId {
  source:      string
  external_id: string
  priority:    number
}

// language is an ISO 639-1 code when known, '' for untagged entries (e.g.
// AniList synonyms, manually-entered titles).
export interface AlternateTitle {
    language: string
    title: string
    overview: string
}

export interface ItemMetadata {
  external_ids:     ExternalId[]
    alternate_titles: AlternateTitle[]
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

// See Kairos's WatchTogetherService.cpp (describeSession) — Kairos owns
// identity/discovery only (this shape), never live playback state
// (position/paused), which lives on Hermes instead — see watchTogetherApi.ts.
export interface WatchTogetherSession {
  session_id:     string
  host_user_id:   string
  host_username:  string
  content_type:   'movie' | 'episode'
  content_id:     string
  title:          string
  // episode only — same shape as WatchProgress above, so a session card can
  // build the same `/api/shows/{show_id}/thumb` art URL / "SxEy Show" label.
  season?:        number
  episode?:       number
  show_id?:       string
  show_title?:    string
  member_count:   number
  created_at:     number
  closed_at:      number | null
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
  source:           ScraperSource
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

export type PlaylistMode       = 'sequential' | 'show_collection' | 'shuffle'
export type PlaylistMembership = 'static' | 'smart'
export type SmartPlaylistType = 'show' | 'movie' | 'mixed'

export interface Playlist {
  playlist_id: string
  title:       string
  mode:        PlaylistMode
  item_count:  number
  total_ms:    number
  plex_link?:  PlexLink
  // Smart-membership fields (see kairos/src/db/Database.cpp migration 87) —
  // membership==='smart' means item_count/total_ms reflect the last
  // refresh-smart run, not a live count; filter_expr is the canon
  // filter-syntax string (components/media/filterSyntax.ts) driving it.
  membership:  PlaylistMembership
  filter_expr: string
  smart_type:  SmartPlaylistType
  smart_sort:  string
    // '' = the sort mode's own natural direction; 'asc'/'desc' overrides it
    // (ignored by 'random'). Smart playlists had no direction control at all
    // before kairos's Database.cpp migration 102.
    smart_sort_dir: '' | 'asc' | 'desc'
    // Show-typed only: flatten to a per-episode list under smart_sort (any
    // mode) instead of "sort the shows, list each one's episodes in season
    // order". See PlaylistRepository::refreshSmart.
    smart_expand_episodes: boolean
  smart_limit: number
  last_smart_refresh_at: number | null
  // Home-shelf fields (Database.cpp migration 88) — a Home shelf is just a
  // smart playlist with show_on_home=true; there's no separate shelf entity.
  // home_tile_limit is a *display* cap (how many tiles before "Continue in
  // Library"), independent of smart_limit which caps actual membership.
  // home_active_start/end are 'MM-DD' or '' (both empty = always shown).
  show_on_home:      boolean
  home_order:        number
  home_tile_limit:   number
  home_active_start: string
  home_active_end:   string
  // Library "Playlists" section tile poster — a pasted URL (same override
  // convention as shows/movies' custom poster). Empty = the frontend falls
  // back to a collage of the first few items' own posters (see
  // PlaylistBrowseEntry.preview_items).
  poster_source: string
}

// GET /api/playlists/browse — any-authenticated-user tile-rendering summary
// for the Library's Playlists section (see PlaylistRepository::listBrowse).
// Deliberately excludes membership/filter_expr/sync internals GET
// /api/playlists exposes for editing — this is browse-only.
export interface PlaylistBrowseEntry {
  playlist_id:   string
  title:         string
  mode:          PlaylistMode
  poster_source: string
  item_count:    number
  total_ms:      number
  // First 4 items by position — only meaningful when poster_source is empty
  // (client-side collage fallback).
  preview_items: { item_type: 'episode' | 'movie'; item_id: string }[]
}

// A remote playlist/collection item that couldn't be resolved against this
// library during an import/resync (see PlexSyncHelper.cpp's
// syncSourceListItems) — reported by title so a hand-curated cross-source
// watch order (e.g. a "Gundam Unicorn order" mixing movies/OVAs/specials)
// shows exactly what's still missing, not just an unexplained item-count
// shortfall.
export interface UnresolvedSyncItem {
  title:     string
  item_type: 'movie' | 'episode' | 'show'
}

// GET /api/home-playlists — the subset of fields the Home page actually
// needs to render a shelf (see PlaylistRepository::HomeShelfRow/listHomeShelves).
export interface HomePlaylistShelf {
  playlist_id:     string
  title:           string
  smart_type:      SmartPlaylistType
  filter_expr:     string
  smart_sort:      string
    smart_sort_dir: '' | 'asc' | 'desc'
    // Show-typed only — when true, this shelf renders as a per-episode feed
    // (see api.getMixedMediaIndex's expandEpisodes option) instead of one tile
    // per matching show.
    smart_expand_episodes: boolean
  home_tile_limit: number
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
  // A collection can contain whole shows, not just movies/episodes — see
  // kairos/src/api/services/PlexSyncHelper.cpp's resolveAndExpand, which
  // expands a resolved show into all its episodes when actually importing
  // (playlist_item has no 'show' item_type of its own).
  item_type:   'episode' | 'movie' | 'show'
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
    id: string
    url: string
    dest_path: string
    status: 'queued' | 'running' | 'done' | 'error'
    progress: number   // 0–100, current item
    playlist_index: number   // current item number within a playlist (0 = not a playlist / unknown)
    playlist_count: number   // total items in the playlist (0 = not a playlist / unknown)
    log: string[]
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

export type ScraperSource = 'tmdb' | 'tvdb' | 'anidb' | 'tvmaze' | 'trakt' | 'anilist' | 'wikidata'

export interface ScraperConfig {
  source:          ScraperSource
    // Always "" from the server now — GET /api/scrapers/config never returns
    // the real key/pin (see has_api_key/has_pin below). Only meaningful as an
    // outgoing value: leave blank to keep whatever's already configured,
    // non-blank to set a new one. Never actually round-trips the stored value.
  api_key:         string
  enabled:         boolean
  language:        string
  language_weight: number
    pin?: string  // TVDB subscriber pin — same write-only convention as api_key
    has_api_key?: boolean // true if a key is already configured server-side
    has_pin?: boolean // TVDB only
}

export interface ScraperSettings {
  configs:               ScraperConfig[]
  match_threshold:       number   // 0–1; default 1.0
  anidb_download_posters: boolean // off by default — see SettingsPage's AniDB section for the rate-limit tradeoff
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

// GET /api/subtitles/broken — a sidecar file kairos's sync-time content
// check (see kairos/src/source/SubtitleValidation.h) flagged as broken,
// e.g. found matching the "<video>.<lang>.srt" naming convention but
// containing ~0 real cues. Excluded from playback while valid is false;
// still recorded (not silently dropped) so this list has something to show.
export interface BrokenSubtitleItem {
  subtitle_id:    string
  media_type:     'episode' | 'movie'
  media_id:       string
  title:          string
  file_path:      string
  language:       string
  forced:         boolean
  sdh:            boolean
  source:         string
  valid:          boolean
  invalid_reason: string
  // Bytes, stat()'d live off disk — omitted if the file's gone missing.
  // Unusually small is a good tell for a broken sub (near-empty content
  // that still matched the naming convention).
  file_size?:     number
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

// GET /api/shows/:id/resolve-play-target — server-side resolvePlayTarget.ts
// (see that file), collapsing what used to be 2-3 client round-trips
// (watch-state, then conditionally next-episode or the full episode list)
// into one.
export interface ResolvedPlayTarget {
  kind:       'movie' | 'episode'
  id:         string
  position_ms: number
    // Multi-part movies (GitHub #3) — present only when the movie is
    // multi-part; position_ms above is already the offset WITHIN part_num,
    // not the summed position across parts.
    part_num?: number
    total_parts?: number
}

// GET /api/tv/manifest — backs Home's row composition and Library/Detail/
// Guide's zone layout, shared identically by /tv and (eventually) native
// clients. See kairos/src/db/Database.cpp's v81 migration + TvManifestService
// for the server side; hades/src/tv/useHomeManifest.ts for how /tv consumes
// it. Deliberately loose typing on zone config (Record<string, unknown>) —
// the manifest is data describing composition, not a typed contract for
// every possible future field.
export type TvItemAction =
  | 'open-detail' | 'play-direct-with-position' | 'play-latest-episode'
  | 'play-resolved' | 'navigate-library' | 'watch-live'

// TvDataSource/TvZone.dataSource removed in kairos v105 — confirmed dead on
// every client (nothing anywhere ever read `.dataSource`, on this client or
// Android's), a stale leftover from before Home rows moved to the opaque
// `filter` shape below. Was a half-implemented per-zone endpoint/queryParam
// config that no client ever actually fetched through automatically.

export interface TvHomeRow {
  id:    string
  order: number
    type: 'hero' | 'shelf' | 'guide' | 'watch-together'
  title?: string
    // Opaque — forwarded verbatim as query params to GET /api/tv/shelf-items
    // (via api.getTvShelfItems). Never inspected/branched on client-side; the
    // server decides what each key means and how to resolve it into tiles, so
    // a new shelf "shape" (e.g. today's mixed-media shelves, which the old
    // dataSource.endpoint design couldn't express at all) never needs a
    // client-side change to support. Absent for 'guide'/'watch-together' rows
    // — those carry no filter at all, since neither resolves through GET
    // /api/tv/shelf-items (a session's host/member_count shape doesn't fit
    // the uniform tile shape every other row's filter resolves to); the row's
    // mere presence in `home.rows` is the whole signal, same as 'guide'.
    filter?: Record<string, unknown>
  itemAction?:  TvItemAction
  endTile?:     TvItemAction
  emptyBehavior?: 'hide'
  requiresArt?: boolean
  actions?:     TvItemAction[]
}

export interface TvZone {
  id:    string
  order: number
  filterFields?: string[]
  itemAction?:   TvItemAction
  showOnly?:     boolean
    // Which fields this zone renders — same "server owns which fields exist,
    // client owns how each one is rendered" split filterFields already uses.
    // Currently only detail/meta-block declares this (kairos v97); undefined/
    // empty means a manifest that predates it, so callers fall back to their
    // own fixed field set rather than rendering nothing.
    fields?: string[]
    // Which action buttons this zone offers — currently only detail's
    // play-button zone declares this (kairos v105: "play"/"play-from-beginning"/
    // "watch-together"). A loose string[] rather than a closed union, same
    // "server owns the vocabulary" reasoning as filterFields/fields — a zone
    // this generic can't assume every future consumer wants the same set of
    // action ids. undefined means a manifest predating this field (or a zone
    // that's never declared it), so callers should default to showing
    // whatever they showed before this field existed rather than nothing.
    actions?: string[]
}

// GET /api/tv/shelf-items' response item shape — the same render-ready tile
// MixedMediaItem already is, plus latest_episode (shows only, populated only
// when the row's filter.sort is "recently_aired" — needed by the "Recently
// Aired" shelf's play-latest-episode itemAction).
export type TvShelfTile = MixedMediaItem & {
    latest_episode?: {
        episode_id: string
        season: number
        episode: number
        air_date: string
    }
}

export interface TvManifest {
  version: number
  home:    { rows: TvHomeRow[] }
  library: { zones: TvZone[] }
  detail:  { zones: TvZone[] }
  guide:   { zones: TvZone[] }
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

// One completed run of a named "hot zone" operation (full sync, a sync
// phase, an EPG regenerate, a scraper match, chapter sync) — see
// shared/metrics/OperationMetrics.h. Distinct from ComponentMetrics
// (MetricsStore.ts), which is a continuous whole-process gauge; this is a
// bounded history of discrete completed jobs.
export interface OperationRun {
  started_at_ms:  number
  duration_ms:    number
  avg_cpu_pct:    number
  max_cpu_pct:    number
  avg_ram_bytes:  number
  max_ram_bytes:  number
  peak_threads:   number
  samples:        number
}

// Keyed by operation name (e.g. "sync.full", "sync.phase.chapters",
// "epg.generate"), most-recent-first, absent entirely for a name with no
// runs yet.
export type OperationMetricsResponse = Record<string, OperationRun[]>

// One of the five JobScheduler-driven background jobs (see
// kairos/src/jobs/JobScheduler.h) — sync/metadata_refresh/chapter_detection/
// writeback_sweep/backup. mode picks which of interval_hours vs.
// daily_hour+daily_minute is active; the other pair is still present
// (whatever it was last set to) so switching modes in the UI doesn't lose
// the previous value.
export interface ScheduledJob {
    name: 'sync' | 'metadata_refresh' | 'chapter_detection' | 'writeback_sweep' | 'media_normalize' | 'backup'
    enabled: boolean
    mode: 'interval' | 'daily'
    interval_hours: number
    daily_hour: number
    daily_minute: number
    next_run_ms: number | null
    last_run_ms: number
    last_run_ok: boolean
    last_error: string
}

export type ScheduledJobPatch = Partial<
    Pick<ScheduledJob, 'enabled' | 'mode' | 'interval_hours' | 'daily_hour' | 'daily_minute'>
>

export interface BackupInfo {
    id: string
    created_ms: number
    size_bytes: number
}

