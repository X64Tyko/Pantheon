import type {
  ArrConfig, ArrLookupResult, ArrServiceOptions,
  AuthResponse,
  Block, BlockContent, BumperContentType, BumperMode, ChannelBumper, ChannelExport,
  CastSessionInfo, CastTokenResponse,
  Channel, ContentOverride, ContentRequest, ContentType, CredentialStatus, DownloadJob, EpisodeOrder,
  ActivitySession, Chapter, ChapterReviewItem, BrokenSubtitleItem,
  Episode, EpisodeGroup, EpisodeSearchResult, EpgPreviewResponse, EpgProgram, ExportDepth, GroupingCandidatesResult, ImportPreviewResult, ImportResult, MediaLanguages, ShowGroupingResult, StartScope,
  FillerEntry, FillerEntryAdvancement, FillerList, FillerListDetail, FillerSelectionMode,
  Library, LibraryInfo, LibraryWithSource,
    Movie,
    MovieDetail,
    MixedIndexEntry,
    MixedMediaItem,
    PagedResult,
    PathMap,
    PlexBrowseItem,
    PlexBrowseList,
  Playlist, PlaylistDetail, PlaylistExport, PlaylistImportPreviewResult, PlaylistImportResult, HomePlaylistShelf, UnresolvedSyncItem, PlaylistBrowseEntry, PlaylistItem,
  ReviewQueueItem, ScraperSearchResult, ScraperSettings, ScraperSource, ScraperStats, MergedInto, DuplicateCandidate,
  Show, ShowDetail, Source, SourceType, SpecialCandidate, LinkedSpecial, User, VideoInfo, WatchProgress, WatchTogetherSession, WritebackResult,
    NextEpisode,
    ShowWatchState,
    ResolvedPlayTarget,
    TvManifest,
    TvShelfTile,
    ChannelNow,
  ItemMetadata, ExternalId, RokuDevice, RokuDeviceState, DeviceConnection, PlaybackHistoryEntry, CrashStatus, TrackPreference,
  UnmappedSourceUser, SourceUser, ImportUserResult, InviteUserResult, SmtpConfig,
    OperationMetricsResponse,
    ScheduledJob,
    ScheduledJobPatch,
    BackupInfo,
} from './types'

export const TOKEN_KEY = 'kairos_token'

/** Append ?token= to a media proxy path so <img> tags can authenticate. */
export function mediaUrl(path: string): string {
  const token = localStorage.getItem(TOKEN_KEY)
  const isExternal = path.startsWith('http://') || path.startsWith('https://')
  if (isExternal) {
    return `/api/images/proxy?url=${encodeURIComponent(path)}${token ? `&token=${encodeURIComponent(token)}` : ''}`
  }
  return token ? `${path}?token=${encodeURIComponent(token)}` : path
}

export function channelLogoUrl(channelId: string): string {
  return mediaUrl(`/api/channels/${channelId}/logo`)
}

// Whether *this* execution context's display can actually show HDR — reads
// the real HDMI/EDID-negotiated capability of whatever screen Chrome is
// driving, not just "does this browser support HDR APIs." Called from
// wherever a playback session actually starts (playbackApi.ts, previewApi.ts),
// which naturally runs in the right context either way: a direct/local
// player call runs in the viewer's own tab, but a Cast session's LOAD
// interceptor navigates to /player/* *inside the receiver's own Hades
// instance* (CastReceiverProvider.tsx) — so this reads the actual
// projector/TV's capability there, not the phone/laptop that initiated the
// cast. No special-casing needed for either path.
export function isHdrCapableDisplay(): boolean {
  return typeof window !== 'undefined' && !!window.matchMedia?.('(video-dynamic-range: high)').matches
}

// Every request — API and /stream alike — carries X-Pantheon-Surface: tv
// while under the /tv route tree. Kairos downgrades an admin caller to
// viewer on that header (Router.cpp), and Hermes forwards it through both
// proxy paths. Derived straight from window.location.pathname (BrowserRouter
// keeps it in sync on every navigation) rather than a flag toggled by
// TvShell's mount/unmount effect — a stateful flag can desync from the
// actual route (e.g. a lazy chunk load racing a fast navigation away) and
// silently strip admin access on every other page for the rest of the tab.
function isTvSurface(): boolean {
  const p = window.location.pathname
  return p === '/tv' || p.startsWith('/tv/')
}

/** For fetch() calls to Hermes's /stream/* routes, which sit outside request()'s /api prefix. */
export function authHeaders(): Record<string, string> {
  const token = localStorage.getItem(TOKEN_KEY)
  const headers: Record<string, string> = token ? { Authorization: `Bearer ${token}` } : {}
  if (isTvSurface()) headers['X-Pantheon-Surface'] = 'tv'
  return headers
}

// Fetches the sanitized library/matching-tables sqlite snapshot (see Kairos's
// GET /api/config/debug-dump) and saves it via the browser's normal download
// flow. Bypasses request() because the response is a binary sqlite file, not
// JSON — reuses its Content-Disposition filename so it matches what Kairos
// actually named the snapshot.
export async function downloadDebugDump(): Promise<void> {
  const res = await fetch('/api/config/debug-dump', { headers: authHeaders() })
  if (!res.ok) {
    const payload = await res.json().catch(() => ({ error: res.statusText }))
    throw new ApiError((payload as any).error ?? res.statusText, res.status, payload)
  }
  const disposition = res.headers.get('Content-Disposition') ?? ''
  const filename = /filename="([^"]+)"/.exec(disposition)?.[1]
    ?? `kairos-analysis-${new Date().toISOString().slice(0, 19).replace(/[:T]/g, '-')}.db`

  const blob = await res.blob()
  const url  = URL.createObjectURL(blob)
  const a = document.createElement('a')
  a.href = url; a.download = filename; a.click()
  URL.revokeObjectURL(url)
}

// Carries status/body for callers that need more than the message (e.g. merge's folder-mismatch confirm).
export class ApiError extends Error {
  status: number
  body:   any
  constructor(message: string, status: number, body: any) {
    super(message)
    this.status = status
    this.body   = body
  }
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = { ...authHeaders() }
  if (body != null) headers['Content-Type'] = 'application/json'

  const res = await fetch(`/api${path}`, {
    method,
    headers: Object.keys(headers).length ? headers : undefined,
    body:    body != null ? JSON.stringify(body) : undefined,
  })

  if (res.status === 401) {
    window.dispatchEvent(new CustomEvent('kairos:unauthorized'))
  }

  if (!res.ok) {
    const payload = await res.json().catch(() => ({ error: res.statusText }))
    throw new ApiError((payload as any).error ?? res.statusText, res.status, payload)
  }
  // 204 / 202 may have no body
  const text = await res.text()
  return text ? JSON.parse(text) : (undefined as unknown as T)
}

function qs(params: Record<string, unknown>): string {
  return Object.entries(params)
    .filter(([, v]) => v !== undefined && v !== '')
    .map(([k, v]) => `${k}=${encodeURIComponent(String(v))}`)
    .join('&')
}

// getShows/getMovies accept both a plain `q` (fuzzy free text) and a `filter`
// (canon filter syntax) — the backend only understands one `filter` param,
// so fold `q` into it here (bare words in the syntax already mean "fuzzy
// free-text match", exactly what `q` always meant on its own).
function withCombinedFilter<P extends { q?: string; filter?: string; home?: boolean }>(p: P) {
  const combined = [p.filter, p.q].filter(Boolean).join(' ')
  const { q: _q, filter: _filter, home, ...rest } = p
  return { ...rest, filter: combined || undefined, home: home ? 1 : undefined }
}

export const api = {
  // Auth
  checkSetup:  ()                                            => request<{ setup_required: boolean }>('GET',    '/auth/setup'),
  setup:       (username: string, password: string)          => request<AuthResponse>('POST', '/auth/setup', { username, password }),
  login:       (username: string, password: string)          => request<AuthResponse>('POST', '/auth/login', { username, password }),
  logout:      ()                                            => request<void>('POST', '/auth/logout'),
  getMe:       ()                                            => request<User>('GET',  '/auth/me'),
  // "Who's watching?" profile picker — every profile on the server, visible
  // to any already-authenticated session (this device already passed the
  // real username/password login; see ProfileSelectPage). Switching into a
  // profile that has a PIN requires it; admin-role profiles always do.
  getProfiles:    ()                            => request<User[]>('GET', '/auth/profiles'),
  switchProfile:  (id: string, pin?: string)     => request<AuthResponse>('POST', `/auth/switch/${id}`, { pin }),
  // Invite-claim flow — unauthenticated, reached before the person has any
  // session at all; only actionable with the unguessable token in the path.
  getInvite:   (token: string)                     => request<{ username: string }>('GET', `/auth/invite/${token}`),
  claimInvite: (token: string, password: string)   => request<AuthResponse>('POST', `/auth/invite/${token}`, { password }),
    // Guest profiles (demo-server "Continue as Guest") — unauthenticated, only
    // reachable at all when getPublicSettings().guest_profiles_enabled is true
    // (the server 403s regardless, this is just what lets the login page
    // decide whether to show the entry point). display_name doubles as the
    // account's username. updateGuestSetup/deleteGuest are guest-only
    // self-service — the server 403s a non-guest caller even with a valid
    // token, see AuthService.cpp's own comment on why that's a real security
    // boundary, not incidental.
    createGuest: (display_name: string) => request<AuthResponse>('POST', '/auth/guest', {display_name}),
    updateGuestSetup: (b: Partial<{
        pin: string
        restricted: boolean
        max_tv_rating: string
        max_movie_rating: string
        max_channel_rating: string
    }>) => request<{ ok: boolean }>('PATCH', '/auth/me/guest', b),
    deleteGuest: () => request<{ ok: boolean }>('DELETE', '/auth/me/guest'),
  mintCastToken:    ()                    => request<CastTokenResponse>('POST',   '/auth/cast-token'),
  getCastSessions:  ()                    => request<CastSessionInfo[]>('GET',    '/auth/sessions?purpose=cast'),
  revokeCastSession: (sessionId: string)  => request<{ ok: boolean }>('DELETE',   `/auth/sessions/${sessionId}`),

  // Roku devices — Kairos-owned/persistent (pairing + "known devices" list)
  getRokuDevices:   ()                                  => request<RokuDevice[]>('GET', '/roku-devices'),
  addRokuDevice:    (name: string, ip_address: string)  => request<{ id: string; pairing_status: string }>('POST', '/roku-devices', { name, ip_address }),
  getRokuDevice:    (id: string)                        => request<RokuDevice>('GET', `/roku-devices/${id}`),
  deleteRokuDevice: (id: string)                        => request<{ ok: boolean }>('DELETE', `/roku-devices/${id}`),

  // Roku device sessions — Hermes-owned/live (the actual cast/command channel)
  getRokuDeviceState: (id: string)                                   => request<RokuDeviceState>('GET', `/devices/${id}`),
  sendRokuCommand:    (id: string, command: Record<string, unknown>) => request<{ ok: boolean; status: string }>('POST', `/devices/${id}/command`, command),
  // Admin-only — every connected Roku across every user, for the Activity
  // page's "how many people are connected and what are they watching" view.
  getAllDeviceConnections: () => request<DeviceConnection[]>('GET', '/devices/all'),
  // User management (admin only)
  getUsers:    ()                                            => request<User[]>('GET',    '/users'),
  createUser:  (username: string, password: string, role: string) => request<void>('POST', '/users', { username, password, role }),
  // Invite-based creation — no password supplied by the admin. 'temp_password'
  // returns {temp_password} once for the admin to relay; 'email' emails a
  // claim link if SMTP is configured (also always returns {invite_link} so
  // the admin can hand it out manually either way).
  inviteUser:  (username: string, role: string, invite: { method: 'temp_password' } | { method: 'email'; email: string }) =>
                 request<InviteUserResult>('POST', '/users', { username, role, invite }),
  resendInvite: (id: string) => request<{ ok: boolean; invite_link: string }>('POST', `/users/${id}/resend-invite`),
  updateUser:  (id: string, patch: { password?: string; role?: string }) => request<void>('PATCH', `/users/${id}`, patch),
  deleteUser:  (id: string)                                 => request<void>('DELETE', `/users/${id}`),
  updateUserRestriction: (id: string, patch: { restricted: boolean; max_tv_rating: string; max_movie_rating: string; max_channel_rating: string }) =>
                                                             request<void>('PATCH', `/users/${id}/restriction`, patch),
    // Admin grant of channel-builder access to one named account — same
    // "own PATCH, own concern" reasoning as updateUserRestriction above.
    updateUserChannelBuilder: (id: string, enabled: boolean) =>
        request<void>('PATCH', `/users/${id}/channel-builder`, {channel_builder_enabled: enabled}),
  // Profile-switch PIN (admin-managed) — empty string clears it.
  setUserPin:  (id: string, pin: string)                    => request<void>('PATCH', `/users/${id}/pin`, { pin }),
  getUserOverrides:   (id: string)                          => request<ContentOverride[]>('GET', `/users/${id}/overrides`),
  addUserOverride:    (id: string, b: ContentOverride)      => request<void>('POST', `/users/${id}/overrides`, b),
  removeUserOverride: (id: string, entityType: string, entityId: string) =>
                                                             request<void>('DELETE', `/users/${id}/overrides/${entityType}/${entityId}`),

  // Sources
  getSources:       ()                                  => request<Source[]>    ('GET',    '/sources'),
  getSourceTypes:   ()                                  => request<SourceType[]>('GET',    '/sources/types'),
  createSource:     (b: Omit<Source, 'enabled' | 'synced_user_id' | 'user_sync_error' | 'auto_writeback' | 'writeback_update_art' | 'writeback_update_external_ids' | 'writeback_update_collections'>) => request<{source_id: string}>('POST', '/sources', b),
  deleteSource:     (id: string)                        => request<void>        ('DELETE', `/sources/${id}`),
  // Which local user (if any) should have watch/resume state pulled from
  // this source's primary account during sync — empty string clears it.
  setSourceSyncedUser: (id: string, userId: string)     => request<{ok: boolean}>('PATCH', `/sources/${id}`, { synced_user_id: userId || null }),
  // Lower syncs/wins first — drives both sync order and which source's data
  // wins a field-level conflict when the same item is matched across
  // sources (see SyncManager's primary_source merge).
  setSourceSyncPriority: (id: string, priority: number) => request<{ok: boolean}>('PATCH', `/sources/${id}`, { sync_priority: priority }),
  // See Source.auto_writeback / writeback_update_* — false by default,
  // opt-in per source.
  setSourceAutoWriteback:            (id: string, enabled: boolean) => request<{ok: boolean}>('PATCH', `/sources/${id}`, { auto_writeback: enabled }),
  setSourceWritebackUpdateArt:       (id: string, enabled: boolean) => request<{ok: boolean}>('PATCH', `/sources/${id}`, { writeback_update_art: enabled }),
  setSourceWritebackUpdateExternalIds: (id: string, enabled: boolean) => request<{ok: boolean}>('PATCH', `/sources/${id}`, { writeback_update_external_ids: enabled }),
  setSourceWritebackUpdateCollections: (id: string, enabled: boolean) => request<{ok: boolean}>('PATCH', `/sources/${id}`, { writeback_update_collections: enabled }),

  // Source-reported accounts with no local Pantheon account imported yet.
  getUnmappedSourceUsers: () => request<UnmappedSourceUser[]>('GET', '/sources/unmapped-users'),
  importSourceUser: (sourceId: string, externalUserId: string, role: 'admin' | 'viewer' = 'viewer') =>
                       request<ImportUserResult>('POST', `/sources/${sourceId}/users/${externalUserId}/import`, { role }),

  // Every discovered account for one source (mapped + unmapped), and linking
  // one to an existing local user for per-account watch-data sync.
  getSourceUsers:   (sourceId: string) => request<SourceUser[]>('GET', `/sources/${sourceId}/users`),
  linkSourceUser:   (sourceId: string, externalUserId: string, userId: string) =>
                       request<{ok: boolean}>('PUT', `/sources/${sourceId}/users/${externalUserId}/link`, { user_id: userId }),
  unlinkSourceUser: (sourceId: string, externalUserId: string) =>
                       request<{ok: boolean}>('DELETE', `/sources/${sourceId}/users/${externalUserId}/link`),

  // Libraries
  getAvailableLibs: (sourceId: string)                  => request<LibraryInfo[]>('GET',    `/sources/${sourceId}/libraries/available`),
  browseLocalDir:   (sourceId: string, path: string)    => request<LibraryInfo[]>('GET',    `/sources/${sourceId}/fs${path ? `?path=${encodeURIComponent(path)}` : ''}`),
  getLibraries:     (sourceId: string)                  => request<Library[]>    ('GET',    `/sources/${sourceId}/libraries`),
  addLibrary:       (sourceId: string, b: Pick<Library, 'external_lib_id'|'display_name'|'library_type'|'preferred_scraper'|'preferred_language'|'include_anidb'>) =>
                                                        request<{library_id: string}>('POST', `/sources/${sourceId}/libraries`, b),
  patchLibrary:     (sourceId: string, lid: string, b: Partial<Pick<Library, 'display_name'|'library_type'|'preferred_scraper'|'preferred_language'|'include_anidb'|'show_on_home'|'skip_scraping'>>) =>
                                                        request<void>('PATCH', `/sources/${sourceId}/libraries/${lid}`, b),
  removeLibrary:    (sourceId: string, lid: string)     => request<void>         ('DELETE', `/sources/${sourceId}/libraries/${lid}`),
  // Content ingestion only for this one library — no orphan cleanup, no
  // source-wide chapter/specials rescans (see Kairos's SyncManager::syncLibrary).
  // Picks up new/changed content quickly; removals still need a full
  // triggerSync/triggerHardSync on the source to be noticed.
  triggerLibrarySync: (sourceId: string, lid: string)   => request<{status: string}>('POST', `/sources/${sourceId}/libraries/${lid}/sync`),
  getScraperPriority: (sourceId: string, lid: string, itemType: 'show'|'movie') =>
                                                        request<{order: string[]}>('GET', `/sources/${sourceId}/libraries/${lid}/scraper-priority?item_type=${itemType}`),
  setScraperPriority: (sourceId: string, lid: string, itemType: 'show'|'movie', order: string[]) =>
                                                        request<{ok: boolean}>('PUT', `/sources/${sourceId}/libraries/${lid}/scraper-priority`, { item_type: itemType, order }),
    // Immediate subfolder names under a local library's root (Downloads page show-folder picker)
    getShowFolders: (sourceId: string, lid: string) => request<{
        name: string
    }[]>('GET', `/sources/${sourceId}/libraries/${lid}/show-folders`),
  // Focused sibling of patchLibrary for call sites that only have a
  // library_id (e.g. a Home shelf card) — patchLibrary needs source_id too.
  setLibraryShowOnHome: (libraryId: string, showOnHome: boolean) =>
                                                        request<void>('PATCH', `/libraries/${libraryId}/home-visibility`, { show_on_home: showOnHome }),
  triggerSync:      (sourceId: string)                  => request<{status: string}>('POST', `/sources/${sourceId}/sync`),
  triggerHardSync:  (sourceId: string)                  => request<{status: string}>('POST', `/sources/${sourceId}/hard-sync`),
  syncAll:          ()                                  => request<{status: string}>('POST', '/sync/all'),
  syncAllHard:      ()                                  => request<{status: string}>('POST', '/sync/all-hard'),
  // library_id/source_id both optional — omit either (or both) for an
  // unfiltered run. 202 + {status:"started"} the same way syncAll does;
  // progress shows up on the Activity log stream, not in this response.
  writebackAll:     (opts?: {library_id?: string; source_id?: string}) =>
                                                           request<{status: string}>('POST', '/writeback/all', opts ?? {}),
  getWritebackStatus: ()                                => request<{running: boolean}>('GET', '/writeback/status'),

  // Media language catalog (probed from library sample via ffprobe, cached 1 h)
  getMediaLanguages: () => request<MediaLanguages>('GET', '/media/languages'),

  // Channels
  getChannels:      ()                                                            => request<Channel[]>('GET',    '/channels'),
  checkChannelAccess: (id: string)                                                => request<{ allowed: boolean }>('GET', `/channels/${id}/access-check`),
  getChannelNow:      (id: string)                                                => request<ChannelNow>('GET', `/channels/${id}/now`),
    createChannel: (b: Omit<Channel, 'channel_id' | 'default_filler_entries' | 'default_filler_selection' | 'owner_user_id' | 'is_demo'>) => request<{
        channel_id: string
    }>('POST', '/channels', b),
    // preserveCursor — see ChannelService.cpp's ?preserve_cursor=true: keeps
    // accumulated cursor/RNG state across a timezone/seed change that would
    // otherwise hard-reset it (Hades' save-time "keep positions" prompt).
    updateChannel: (id: string, b: Partial<Pick<Channel, 'name' | 'number' | 'timezone' | 'seed' | 'default_filler_selection' | 'advance_mode' | 'offline_video_path' | 'offline_image_path' | 'offline_audio_id' | 'offline_audio_type' | 'offline_audio_title' | 'logo_path' | 'anchor_hashes' | 'audio_lang' | 'subtitle_lang' | 'stream_resolution' | 'stream_video_bitrate' | 'stream_audio_bitrate' | 'force_transcode' | 'content_tag'>>, opts?: {
        preserveCursor?: boolean
    }) =>
        request<void>('PATCH', `/channels/${id}${opts?.preserveCursor ? '?preserve_cursor=true' : ''}`, b),
  deleteChannel:    (id: string)                                                  => request<void>('DELETE', `/channels/${id}`),
  exportChannel:    (id: string, depth: ExportDepth)                              => request<ChannelExport>('GET', `/channels/${id}/export?depth=${depth}`),
  importChannel:    (data: ChannelExport)                                         => request<ImportResult>('POST', '/channels/import', data),
  previewImport:    (data: ChannelExport)                                         => request<ImportPreviewResult>('POST', '/channels/import/preview', data),

  // Arr integrations
  getArrConfig:  ()                                                       => request<ArrConfig>('GET',   '/config/arr'),
  patchArrConfig:(b: Partial<ArrConfig>)                                  => request<{ok: boolean}>('PATCH', '/config/arr', b),
  arrLookup:     (b: { type: 'show'|'movie'; title?: string; tvdb_id?: string; tmdb_id?: string; imdb_id?: string }) =>
                   request<ArrLookupResult[]>('POST', '/arr/lookup', b),
  arrOptions:    (type: 'show'|'movie')                                   => request<ArrServiceOptions>('GET', `/arr/options/${type}`),
  arrAdd:        (b: { type: 'show'|'movie'; add_data: unknown; quality_profile_id: number; root_folder: string; search_on_add?: boolean }) =>
                   request<{ ok: boolean }>('POST', '/arr/add', b),

  // Content requests
  getRequests:    ()                                                                           => request<ContentRequest[]>('GET',    '/requests'),
  createRequest:  (b: { content_type: 'show'|'movie'; source: ScraperSource; external_id: string; title: string; year?: number; poster_url?: string }) =>
                    request<{ request_id: string; status: string; duplicate?: boolean }>('POST', '/requests', b),
  updateRequest:  (id: string, status: 'approved'|'rejected', arr_add?: { type: 'show'|'movie'; add_data: unknown; quality_profile_id: number; root_folder: string; search_on_add?: boolean }) =>
                    request<{ok: boolean}>('PATCH',  `/requests/${id}`, { status, arr_add }),
  deleteRequest:  (id: string)                                                                => request<{ok: boolean}>('DELETE', `/requests/${id}`),

  // Channel filler entries
  addChannelFiller:    (channelId: string, b: { content_type: string; content_id: string; advancement: FillerEntryAdvancement; weight?: number; season_filter?: number }) =>
                         request<FillerEntry>('POST',   `/channels/${channelId}/filler`, b),
  updateChannelFiller: (channelId: string, entryId: number, b: { advancement?: FillerEntryAdvancement; weight?: number }) =>
                         request<void>       ('PATCH',  `/channels/${channelId}/filler/${entryId}`, b),
  removeChannelFiller: (channelId: string, entryId: number) =>
                         request<void>       ('DELETE', `/channels/${channelId}/filler/${entryId}`),

  // Connection test (no persistence)
  testSource:       (b: {source_type: string, base_url: string, token: string, user_id?: string}) =>
                                                        request<{ok: boolean, error?: string}>('POST', '/sources/test', b),

  // Credentials (kairos.conf via API)
  getCredentials:    (id: string)                       => request<CredentialStatus>('GET',    `/config/credentials/${id}`),
  setCredentials:    (id: string, b: {token: string, user_id?: string}) =>
                                                        request<{ok: boolean}>('PUT',    `/config/credentials/${id}`, b),
  deleteCredentials: (id: string)                       => request<{ok: boolean}>('DELETE', `/config/credentials/${id}`),

  // Path maps (kairos.conf via API)
  getPathMaps:       (id: string)                       => request<PathMap[]>        ('GET', `/config/path-maps/${id}`),
  setPathMaps:       (id: string, maps: PathMap[])      => request<{ok: boolean}>    ('PUT', `/config/path-maps/${id}`, { maps }),

  // Sample raw file path from this source (pre-mapping) for UI hint
  getSamplePath:     (id: string)                       => request<{path: string | null}>('GET', `/sources/${id}/sample-path`),

  // Sync status
  getSyncStatus:    ()                                  => request<{running: boolean; current_source_id?: string}>('GET', '/sync/status'),

  // Content — list
  getAllLibraries: ()                                    => request<LibraryWithSource[]>('GET', '/libraries'),
  getFilterValues: (field: string, params: { type?: 'movie' | 'show'; library_id?: string } = {}) =>
    request<{ values: string[] }>('GET', `/metadata/values?${qs({ field, ...params })}`).then(r => r.values),
  // `q` (plain fuzzy free text) and `filter` (canon filter syntax — see
  // components/media/filterSyntax.ts) both fold into the same backend
  // `filter` param — simple callers can keep passing just `q`; the rule
  // builder (via components/media/filterQuery.ts) passes `filter`; either
  // or both together concatenate (bare words in the syntax mean "fuzzy
  // free-text match", same as `q` always did).
  getShows:       (p: { limit?: number; offset?: number; library_id?: string; library_ids?: string; q?: string; filter?: string; sort?: string; sort_dir?: 'asc' | 'desc'; seed?: number; home?: boolean; hideEmpty?: boolean; playlist_id?: string } = {}) =>
                    request<PagedResult<Show>>('GET', `/shows?${qs({ ...withCombinedFilter(p), hideEmpty: undefined, hide_empty: p.hideEmpty ? 1 : undefined })}`),
    getEpisodes: (showId: string, season?: number) => request<Episode[]>('GET', `/shows/${encodeURIComponent(showId)}/episodes${season != null ? '?season=' + season : ''}`),
  getMovies:      (p: { limit?: number; offset?: number; library_id?: string; library_ids?: string; q?: string; filter?: string; sort?: string; sort_dir?: 'asc' | 'desc'; seed?: number; home?: boolean; hideEmpty?: boolean; playlist_id?: string } = {}) =>
                    request<PagedResult<Movie>>('GET', `/movies?${qs({ ...withCombinedFilter(p), hideEmpty: undefined, hide_empty: p.hideEmpty ? 1 : undefined })}`),

    // Truly interleaved show+movie(+episode) browsing — see kairos's
    // MixedSort.h. Always every match, already sorted; page by hydrating
    // whatever slice a caller wants to render (getMixedMediaTiles).
    // expandEpisodes swaps shows for their individual episodes in the result
    // (a per-episode feed like "Recently Aired", still interleaved with
    // movies under the same sort) instead of one tile per matching show.
    getMixedMediaIndex: (p: {
        library_ids?: string;
        q?: string;
        filter?: string;
        sort?: string;
        sort_dir?: 'asc' | 'desc';
        home?: boolean;
        hideEmpty?: boolean;
        expandEpisodes?: boolean;
        includeMovies?: boolean
    } = {}) =>
        request<{ items: MixedIndexEntry[] }>('GET', `/mixed-media/index?${qs({
            ...withCombinedFilter(p), hideEmpty: undefined, hide_empty: p.hideEmpty ? 1 : undefined,
            expandEpisodes: undefined, expand_episodes: p.expandEpisodes ? 1 : undefined,
            includeMovies: undefined, include_movies: p.includeMovies === false ? 0 : undefined,
        })}`),
    getMixedMediaTiles: (ids: { content_type: 'show' | 'movie' | 'episode'; id: string }[]) =>
        ids.length === 0 ? Promise.resolve({items: [] as MixedMediaItem[]})
            : request<{
                items: MixedMediaItem[]
            }>('GET', `/mixed-media/hydrate?${qs({ids: ids.map(i => `${i.content_type}:${i.id}`).join(',')})}`),

  // Watch progress
  getWatchProgress:   (limit?: number)                                     => request<WatchProgress[]>('GET', `/watch-progress${limit != null ? '?limit=' + limit : ''}`),
    // device_type/direct_stream (both optional) additionally feed the local
  // play-history table (see kairos's PlaybackHistoryRepository) — nothing
  // client-side reads them back, they're just piggybacked onto pings this
  // call already sends.
    putWatchProgress: (contentType: 'movie' | 'episode', id: string, b: {
        position_ms: number;
        duration_ms: number;
        completed?: boolean;
        device_type?: string;
        direct_stream?: boolean
    }) =>
                        request<{ ok: boolean; watched: boolean }>('PUT', `/watch-progress/${contentType}/${id}`, b),
  clearWatchProgress: (contentType: 'movie' | 'episode', id: string)       => request<void>('DELETE', `/watch-progress/${contentType}/${id}`),
    // Live channels have no position/duration to report — this only ever
    // feeds Kairos's PlaybackHistoryRepository ("who's watching what" in the
    // Activity tab), not WatchProgressRepository, so it's a separate call
    // rather than widening putWatchProgress's contentType union onto a shape
    // that doesn't fit (no position_ms/duration_ms, no "watched" response).
    pingChannelActivity: (channelId: string, b: { device_type?: string; direct_stream?: boolean }) =>
        request<{ ok: boolean }>('PUT', `/watch-progress/channel/${channelId}`, b),
  getShowWatchState:  (showId: string)                                     => request<ShowWatchState | null>('GET', `/shows/${showId}/watch-state`),

  // Watch Together — identity/discovery only (Kairos owns this half; live
  // position/paused coordination is Hermes, see watchTogetherApi.ts). Same
  // /api surface as everything else in this object, unlike watchTogetherApi's
  // calls which hit Hermes's own /watch-together/* routes directly.
  createWatchTogether: (contentType: 'movie' | 'episode', contentId: string) =>
                        request<WatchTogetherSession>('POST', '/watch-together', { content_type: contentType, content_id: contentId }),
  getActiveWatchTogether: ()                                       => request<WatchTogetherSession[]>('GET', '/watch-together/active'),
  getWatchTogether:       (sessionId: string)                      => request<WatchTogetherSession>('GET', `/watch-together/${sessionId}`),
    // Joining goes through watchTogetherApi.ts's own joinWatchTogether
    // (Hermes), not here — it forwards to this same Kairos route server-side
    // but also merges in the live position/paused Kairos never tracks, saving
    // the caller a second round trip.
  leaveWatchTogether:     (sessionId: string)                      => request<{ ok: boolean }>('POST', `/watch-together/${sessionId}/leave`),
  closeWatchTogether:     (sessionId: string)                      => request<{ ok: boolean }>('POST', `/watch-together/${sessionId}/close`),

  // Plex-style sticky per-show audio/subtitle language (see kairos's
  // ShowTrackPreferenceRepository) — keyed by episode_id so the caller never
  // needs to know show_id; Kairos resolves it server-side. A field left out
  // of `b` leaves that side of the saved preference untouched.
  setEpisodeTrackPreference: (episodeId: string, b: { audio_lang?: string; subtitle_lang?: string }) =>
                        request<{ ok: boolean }>('PUT', `/episodes/${episodeId}/track-preference`, b),

  // Same show_track_preference row the episode route above writes, but
  // directly by show_id — for the detail page, so a preference can be set
  // before ever pressing play at all (the episode route is what an
  // in-player track switch calls, since that's all it has on hand).
  getShowTrackPreference: (showId: string) => request<TrackPreference>('GET', `/shows/${showId}/track-preference`),
  setShowTrackPreference: (showId: string, b: { audio_lang?: string; subtitle_lang?: string }) =>
                        request<{ ok: boolean }>('PUT', `/shows/${showId}/track-preference`, b),
  getMovieTrackPreference: (movieId: string) => request<TrackPreference>('GET', `/movies/${movieId}/track-preference`),
  setMovieTrackPreference: (movieId: string, b: { audio_lang?: string; subtitle_lang?: string }) =>
                        request<{ ok: boolean }>('PUT', `/movies/${movieId}/track-preference`, b),

  // Library-wide fallback, used whenever no show/movie-specific preference
  // is set (see kairos's migration v94). Self-service — operates on the
  // caller's own account, no id needed.
  setMyTrackPreference: (b: { audio_lang?: string; subtitle_lang?: string }) =>
                        request<{ ok: boolean }>('PATCH', '/users/me/track-preference', b),

    // '' clears the override (falls back to the admin-configured global
    // default — see SettingsPage's own "Default Landing Page").
    setMyLandingPage: (page: '' | 'home' | 'guide') =>
        request<{ ok: boolean }>('PATCH', '/users/me/landing-page', {page}),

  // Server-side resolvePlayTarget.ts (see hades/src/player/resolvePlayTarget.ts) —
  // one call instead of watch-state + conditionally next-episode/full-episode-list.
  getResolvedPlayTarget: (showId: string)                                  => request<ResolvedPlayTarget | null>('GET', `/shows/${showId}/resolve-play-target`),

  // Home row composition + Library/Detail/Guide zone layout — see hades/src/tv/useHomeManifest.ts.
  getTvManifest: ()                                                        => request<TvManifest>('GET', '/tv/manifest'),

    // Resolves one Home row's opaque `filter` (TvHomeRow.filter) into
    // render-ready tiles — the one generic call TvHome.tsx makes for every
    // shelf and the hero row, regardless of what the filter's content_type
    // says. `filter` is forwarded verbatim as query params; this function
    // never inspects its keys, matching the "server decides what a shelf
    // needs, client just renders it" split the whole /api/tv/manifest
    // contract is built on.
    getTvShelfItems: (filter: Record<string, unknown>) => request<{
        items: TvShelfTile[]
    }>('GET', `/tv/shelf-items?${qs(filter)}`),

  // Series continuation. Deliberately not ".../next" — see the Kairos route's
  // own comment (ContentService.cpp): that suffix is exempted from auth for
  // the live-channel schedule lookup and would leak episode metadata unauthenticated.
    getNextEpisode: (episodeId: string) => request<NextEpisode | null>('GET', `/episodes/${encodeURIComponent(episodeId)}/next-episode`),

  // Blocks
    // preserveCursor on createBlock/updateBlock/deleteBlock — see BlockService.cpp's
    // ?preserve_cursor=true: keeps accumulated cursor/RNG state across a
    // structural change (or any add/remove) that would otherwise hard-reset it.
  getBlocks:         (channelId: string)                                          => request<Block[]>('GET', `/channels/${channelId}/blocks`),
    createBlock: (channelId: string, b: Omit<Block, 'block_id' | 'channel_id' | 'content' | 'filler_entries'>, opts?: {
        preserveCursor?: boolean
    }) =>
        request<{
            block_id: string
        }>('POST', `/channels/${channelId}/blocks${opts?.preserveCursor ? '?preserve_cursor=true' : ''}`, b),
    updateBlock: (channelId: string, blockId: string, b: Partial<Omit<Block, 'block_id' | 'channel_id' | 'content' | 'filler_entries'>>, opts?: {
        preserveCursor?: boolean
    }) =>
        request<void>('PATCH', `/channels/${channelId}/blocks/${blockId}${opts?.preserveCursor ? '?preserve_cursor=true' : ''}`, b),
    deleteBlock: (channelId: string, blockId: string, opts?: { preserveCursor?: boolean }) =>
        request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}${opts?.preserveCursor ? '?preserve_cursor=true' : ''}`),
  addBlockContent:   (channelId: string, blockId: string, b: { content_type: ContentType; content_id: string; season_filter?: number | null; weight?: number; run_count?: number; include_specials?: boolean; episode_order?: EpisodeOrder }) =>
                       request<{id: number, position: number}>('POST', `/channels/${channelId}/blocks/${blockId}/content`, b),
  updateBlockContent:(channelId: string, blockId: string, cid: number, b: { season_filter?: number | null; position?: number; weight?: number; run_count?: number; include_specials?: boolean; episode_order?: EpisodeOrder }) =>
                       request<void>('PATCH', `/channels/${channelId}/blocks/${blockId}/content/${cid}`, b),
  removeBlockContent:       (channelId: string, blockId: string, cid: number)     => request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}/content/${cid}`),
  resetBlockContentCursor:  (channelId: string, blockId: string, cid: number)     => request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}/content/${cid}/cursor`),
  blockExportPlaylist: (channelId: string, blockId: string, b: { title: string; source_id?: string }) =>
                       request<{ playlist_id: string; item_count: number; plex_playlist_id?: string }>('POST', `/channels/${channelId}/blocks/${blockId}/playlist`, b),

  // Episode groups
  getEpisodeGroups:       (showId: string)                                         => request<EpisodeGroup[]>('GET',    `/shows/${showId}/groups`),
  createEpisodeGroup:     (showId: string, b: { name: string; group_type?: string })  => request<{group_id: string}>('POST', `/shows/${showId}/groups`, b),
  deleteEpisodeGroup:     (showId: string, groupId: string)                        => request<void>('DELETE', `/shows/${showId}/groups/${groupId}`),
  addGroupMember:         (showId: string, groupId: string, b: { episode_id: string; part_num: number }) =>
                            request<{id: number, part_num: number}>('POST',   `/shows/${showId}/groups/${groupId}/members`, b),
  removeGroupMember:      (showId: string, groupId: string, memberId: number)      => request<void>('DELETE', `/shows/${showId}/groups/${groupId}/members/${memberId}`),
  getGroupingCandidates:       (showId: string) => request<GroupingCandidatesResult>('GET', `/shows/${showId}/grouping-candidates`),
  getAllGroupingCandidates:    ()               => request<ShowGroupingResult[]>('GET', '/grouping-candidates'),

  // Show specials linking — see ScraperManager::scanSpecialsForShow
  scanSpecials:           (showId: string) => request<{ candidates: SpecialCandidate[] }>('POST', `/shows/${showId}/specials/scan`),
  getSpecialCandidates:   (showId: string) => request<{ candidates: SpecialCandidate[] }>('GET',  `/shows/${showId}/specials/candidates`),
  getLinkedSpecials:      (showId: string) => request<LinkedSpecial[]>('GET', `/shows/${showId}/specials`),
  acceptSpecialCandidate: (showId: string, candidateId: string) => request<{ ok: boolean }>('POST', `/shows/${showId}/specials/candidates/${candidateId}/accept`),
  rejectSpecialCandidate: (showId: string, candidateId: string) => request<{ ok: boolean }>('POST', `/shows/${showId}/specials/candidates/${candidateId}/reject`),
    setShowFindSpecials: (showId: string, find_specials: boolean) => request<{
        ok: boolean
    }>('PATCH', `/shows/${encodeURIComponent(showId)}/find-specials`, {find_specials}),
  setEpisodeDisplayOrder: (showId: string, episode_display_order: 'season' | 'aired') =>
      request<{
          ok: boolean
      }>('PATCH', `/shows/${encodeURIComponent(showId)}/episode-display-order`, {episode_display_order}),

  // Block filler entries
  addBlockFiller:    (channelId: string, blockId: string, b: { content_type: string; content_id: string; advancement: FillerEntryAdvancement; weight?: number; season_filter?: number }) =>
                       request<FillerEntry>('POST',   `/channels/${channelId}/blocks/${blockId}/filler`, b),
  updateBlockFiller: (channelId: string, blockId: string, entryId: number, b: { advancement?: FillerEntryAdvancement; weight?: number }) =>
                       request<void>       ('PATCH',  `/channels/${channelId}/blocks/${blockId}/filler/${entryId}`, b),
  removeBlockFiller: (channelId: string, blockId: string, entryId: number) =>
                       request<void>       ('DELETE', `/channels/${channelId}/blocks/${blockId}/filler/${entryId}`),

  // Channel EPG — cache-backed (used by XMLTV/m3u generation)
  getChannelEpg: (channelId: string, hours?: number, from?: number) => {
    const params = new URLSearchParams()
    if (hours != null) params.set('hours', String(hours))
    if (from  != null) params.set('from',  String(from))
    const qs = params.toString()
    return request<EpgProgram[]>('GET', `/channels/${channelId}/epg${qs ? `?${qs}` : ''}`)
  },
    // live=true also drops the schedule cache's usual carve-out for whatever's
    // currently on-air (Kairos's ScheduleCache::clear()) — only send it after the
    // user has explicitly agreed to interrupt live playback with this edit.
    clearChannelEpgCache: (channelId: string, opts?: { hard?: boolean; live?: boolean }) => {
        const params = new URLSearchParams()
        if (opts?.hard) params.set('hard', 'true')
        if (opts?.live) params.set('live', 'true')
        const qs = params.toString()
        return request<{ ok: boolean }>('POST', `/channels/${channelId}/epg/clear${qs ? `?${qs}` : ''}`)
    },
  // EPG preview — POST with optional seed, hours, and draft blocks.
  // Returns { programs, anchors } where anchors maps week-anchor timestamps to mutated seeds.
  previewChannelEpg: (channelId: string, hours?: number, seed?: number, blocks?: Block[]) =>
    request<EpgPreviewResponse>('POST', `/channels/${channelId}/epg/preview`, {
      ...(hours != null ? { hours } : {}),
      ...(seed  != null ? { seed  } : {}),
      ...(blocks        ? { blocks } : {}),
    }),

  // Episode search
    getShowSeasons: (showId: string) => request<{
        seasons: { number: number; name: string }[]
    }>('GET', `/shows/${encodeURIComponent(showId)}/seasons`),
  searchEpisodes:    (p: {
                       q?: string; show_id?: string; season?: number; limit?: number; offset?: number
                       // 'playlist_order' only meaningful (and only offered by the UI) together with playlist_id —
      // see LibraryFilters.tsx's conditional sort option. 'episode_number' is season/episode
      // order ignoring show grouping, unlike 'title' which groups by show first.
      sort?: 'title' | 'episode_number' | 'playlist_order'; sort_dir?: 'asc' | 'desc'; playlist_id?: string
                     } = {}) =>
                       request<PagedResult<EpisodeSearchResult>>('GET', `/episodes?${qs(p)}`),

  // Playlists
  getPlaylists:      ()                                       => request<Playlist[]>    ('GET',    '/playlists'),
  // Any-authenticated-user tile summary for the Library's Playlists section
  // (see PlaylistRepository::listBrowse) — deliberately separate from the
  // admin-only getPlaylists() above, which carries full editing internals.
  getPlaylistsBrowse: ()                                      => request<PlaylistBrowseEntry[]>('GET', '/playlists/browse'),
  createPlaylist:    (b: { title: string })                   => request<{playlist_id: string}>('POST', '/playlists', b),
  getPlaylist:       (id: string)                             => request<PlaylistDetail>('GET',    `/playlists/${id}`),
  getPlaylistPlayTarget: (id: string)                         => request<ResolvedPlayTarget | null>('GET', `/playlists/${id}/resolve-play-target`),
  // Any-authenticated-user ordered item list for Play/Restart/Shuffle (see
  // resolvePlayTarget.ts) — deliberately separate from the admin-only
  // getPlaylist() above, same split as getPlaylistsBrowse().
  getPlaylistItems: (id: string)                              => request<{items: PlaylistItem[]}>('GET', `/playlists/${id}/items`),
  updatePlaylist:    (id: string, b: {
                       title?: string; mode?: string
                       membership?: 'static'|'smart'; filter_expr?: string
      smart_type?: 'show' | 'movie' | 'mixed'; smart_sort?: string
      smart_sort_dir?: '' | 'asc' | 'desc'; smart_expand_episodes?: boolean; smart_limit?: number
                       show_on_home?: boolean; home_order?: number; home_tile_limit?: number
                       home_active_start?: string; home_active_end?: string
                       poster_source?: string
                     }) => request<void>('PATCH',  `/playlists/${id}`, b),
  deletePlaylist:    (id: string)                             => request<void>          ('DELETE', `/playlists/${id}`),
  refreshSmartPlaylist: (id: string) => request<{synced: number}>('POST', `/playlists/${id}/refresh-smart`),
  refreshAllSmartPlaylists: () => request<{status: string}>('POST', '/playlists/refresh-smart-all'),
  // Pushes this playlist's current items TO a remote Plex/Jellyfin/Emby
  // playlist or collection — opposite direction from sourceSyncPlaylist's
  // pull. Creates a new remote list the first time; reconciles (add/remove
  // diff) on later pushes to the same linked target — see
  // kairos/src/api/services/ListPushHelper.cpp.
  pushPlaylist: (id: string, b: { source_id: string; kind: 'playlist'|'collection'; title?: string; external_lib_id?: string }) =>
    request<{ created: boolean; external_id: string; pushed?: number; added?: number; removed?: number; unresolved: number }>(
      'POST', `/playlists/${id}/push`, b),
  getHomePlaylists:  () => request<HomePlaylistShelf[]>('GET', '/home-playlists'),
  addPlaylistItem:   (id: string, b: { item_type: 'episode'|'movie'; item_id: string }) =>
                       request<{id: number, position: number}>('POST',   `/playlists/${id}/items`, b),
  removePlaylistItem:(id: string, iid: number)                => request<void>          ('DELETE', `/playlists/${id}/items/${iid}`),
  movePlaylistItem:  (id: string, iid: number, position: number) => request<void>       ('PATCH',  `/playlists/${id}/items/${iid}`, { position }),

  // Playlist export/import — same portable-JSON pattern as channel export/import.
  exportPlaylist:      (id: string, deep = true) => request<PlaylistExport>('GET', `/playlists/${id}/export${deep ? '?deep=true' : ''}`),
  previewPlaylistImport: (data: PlaylistExport)   => request<PlaylistImportPreviewResult>('POST', '/playlists/import/preview', data),
  importPlaylist:      (data: PlaylistExport)     => request<PlaylistImportResult>('POST', '/playlists/import', data),

  // Filler lists
  getFillerLists:       ()                                              => request<FillerList[]>    ('GET',    '/filler-lists'),
  createFillerList:     (b: { title: string; advancement?: string })    => request<{filler_list_id: string}>('POST', '/filler-lists', b),
  getFillerList:        (id: string)                                    => request<FillerListDetail>('GET',    `/filler-lists/${id}`),
  updateFillerList:     (id: string, b: { title?: string; advancement?: string }) => request<void>('PATCH',  `/filler-lists/${id}`, b),
  deleteFillerList:     (id: string)                                    => request<void>           ('DELETE', `/filler-lists/${id}`),
  addFillerListItem:    (id: string, b: { item_type: 'episode'|'movie'; item_id: string }) =>
                          request<{id: number, position: number}>       ('POST',   `/filler-lists/${id}/items`, b),
  removeFillerListItem: (id: string, iid: number)                       => request<void>           ('DELETE', `/filler-lists/${id}/items/${iid}`),

  // Bulk add
  bulkAddPlaylistItems:    (id: string, items: { item_type: 'episode'|'movie'; item_id: string }[]) =>
                             request<{added: number}>('POST', `/playlists/${id}/items/bulk`, { items }),
  bulkAddFillerListItems:  (id: string, items: { item_type: 'episode'|'movie'; item_id: string }[]) =>
                             request<{added: number}>('POST', `/filler-lists/${id}/items/bulk`, { items }),

  // Source-linked list sync — /source-sync dispatches per the linked
  // source's actual type (Plex/Jellyfin/Emby all work); the older /plex-sync
  // route it replaces 400s on anything but a Plex source_id.
  sourceSyncPlaylist:      (id: string, b: { source_id: string; external_id: string; list_kind: 'playlist'|'collection' }) =>
                             request<{synced: number; total: number; unresolved: UnresolvedSyncItem[]}>('POST', `/playlists/${id}/source-sync`, b),
  unlinkPlaylist:          (id: string) => request<void>('DELETE', `/playlists/${id}/plex-link`),
  syncAllLinkedPlaylists:  () => request<{status: string}>('POST', '/playlists/source-sync-all'),
  sourceSyncFillerList:    (id: string, b: { source_id: string; external_id: string; list_kind: 'playlist'|'collection' }) =>
                             request<{synced: number; total: number; unresolved: UnresolvedSyncItem[]}>('POST', `/filler-lists/${id}/source-sync`, b),
  unlinkFillerList:        (id: string) => request<void>('DELETE', `/filler-lists/${id}/plex-link`),
  syncAllLinkedFillerLists: () => request<{status: string}>('POST', '/filler-lists/source-sync-all'),

  // Source browse — lists playlists / collections live from a Plex, Jellyfin,
  // or Emby source (the route dispatches polymorphically server-side; naming
  // here predates Jellyfin/Emby support but the endpoints were always generic).
  browsePlexPlaylists:         (sourceId: string)                   => request<PlexBrowseList[]>('GET', `/sources/${sourceId}/browse/playlists`),
  browsePlexPlaylistItems:     (sourceId: string, plid: string)     => request<PlexBrowseItem[]>('GET', `/sources/${sourceId}/browse/playlists/${plid}/items`),
  browsePlexCollections:       (sourceId: string, libraryId: string)=> request<PlexBrowseList[]>('GET', `/sources/${sourceId}/browse/collections?library_id=${encodeURIComponent(libraryId)}`),
  browsePlexCollectionItems:   (sourceId: string, cid: string)      => request<PlexBrowseItem[]>('GET', `/sources/${sourceId}/browse/collections/${cid}/items`),

  // Content — detail + update
    // id is a kairos_id — for local-sourced items this is a raw filesystem
    // path (contains '/'), hence encodeURIComponent on every id segment below
    // (matches the backend's (.+) route capture in ContentService.cpp).
    getShow: (id: string) => request<ShowDetail>('GET', `/shows/${encodeURIComponent(id)}`),
    updateShow: (id: string, b: Partial<ShowDetail>) => request<void>('PATCH', `/shows/${encodeURIComponent(id)}`, b),
    getMovie: (id: string) => request<MovieDetail>('GET', `/movies/${encodeURIComponent(id)}`),
  // Minimal single-episode lookup (title/overview/show_title/season/episode
  // only) — for resolving a display title (+ hero overview) from a bare
  // episode_id, e.g. the connections view and Home/TV's Continue Watching
  // hero. Not the full episode detail other pages might expect.
    getEpisodeBrief: (id: string) => request<{
        episode_id: string;
        season: number;
        episode: number;
        title: string;
        overview: string;
        show_id: string;
        show_title: string
    }>('GET', `/episodes/${encodeURIComponent(id)}`),
    updateMovie: (id: string, b: Partial<MovieDetail>) => request<void>('PATCH', `/movies/${encodeURIComponent(id)}`, b),
  pushToSources:  (id: string, contentType: 'show' | 'movie') =>
      request<WritebackResult>('POST', `/${contentType}s/${encodeURIComponent(id)}/writeback`),
  refreshMetadata: (id: string, contentType: 'show' | 'movie') =>
      request<{ ok: boolean }>('POST', `/${contentType}s/${encodeURIComponent(id)}/refresh-metadata`),
  // Separate from updateShow/updateMovie, which always lock the record as a
  // side effect of a metadata edit — this flag is orthogonal to that.
  setShowSkipScraping:  (id: string, skip_scraping: boolean) =>
      request<{ ok: boolean }>('PATCH', `/shows/${encodeURIComponent(id)}/skip-scraping`, {skip_scraping}),
  setMovieSkipScraping: (id: string, skip_scraping: boolean) =>
      request<{ ok: boolean }>('PATCH', `/movies/${encodeURIComponent(id)}/skip-scraping`, {skip_scraping}),
    getShowLanguages: (id: string) => request<MediaLanguages>('GET', `/shows/${encodeURIComponent(id)}/languages`),
    getMovieLanguages: (id: string) => request<MediaLanguages>('GET', `/movies/${encodeURIComponent(id)}/languages`),
    getShowVideoInfo: (id: string) => request<VideoInfo>('GET', `/shows/${encodeURIComponent(id)}/videoinfo`),
    getMovieVideoInfo: (id: string) => request<VideoInfo>('GET', `/movies/${encodeURIComponent(id)}/videoinfo`),

  // Channel bumpers
  getBumpers:    (channelId: string)                                                           => request<ChannelBumper[]>('GET',    `/channels/${channelId}/bumpers`),
  createBumper:  (channelId: string, b: { content_type: BumperContentType; content_id: string; mode: BumperMode; every_n: number; season_filter?: number }) =>
                   request<ChannelBumper>                            ('POST',   `/channels/${channelId}/bumpers`, b),
  updateBumper:  (channelId: string, bumperId: number, b: Partial<Pick<ChannelBumper, 'content_type'|'content_id'|'mode'|'every_n'|'position'>>) =>
                   request<void>                                     ('PATCH',  `/channels/${channelId}/bumpers/${bumperId}`, b),
  deleteBumper:  (channelId: string, bumperId: number)               => request<void>          ('DELETE', `/channels/${channelId}/bumpers/${bumperId}`),

  // Runtime settings
    getSettings: () => request<{
        epg_debug: boolean;
        sync_debug: boolean;
        sync_threads: number;
        stream_buffer_size: number;
        image_cache_ttl_hours: number;
        verbose_transcode_logs: boolean;
        ffmpeg_debug_logs: boolean;
        verbose_gateway_logs: boolean;
        hades_debug: boolean;
        cast_app_id: string;
        default_landing_page: string;
        internal_token: string;
        guest_profiles_enabled: boolean;
        guest_idle_timeout_days: number;
        guest_max_concurrent: number;
        guest_channel_builder_enabled: boolean;
        guest_max_demo_channels: number;
        viewer_max_channels: number;
        require_admin_password_switch: boolean
    }>('GET', '/config/settings'),
  // Public read-only subset for internal services (Hephaestus, Hermes) and
    // the Hades frontend (CastProvider, which needs cast_app_id regardless of
    // role; the login page, which needs guest_profiles_enabled pre-login).
    getPublicSettings: () => request<{
        stream_buffer_size: number;
        verbose_transcode_logs: boolean;
        ffmpeg_debug_logs: boolean;
        verbose_gateway_logs: boolean;
        cast_app_id: string;
        default_landing_page: string;
        guest_profiles_enabled: boolean;
        guest_channel_builder_enabled: boolean;
        guest_max_demo_channels: number;
        viewer_max_channels: number;
        require_admin_password_switch: boolean
    }>('GET', '/config/public-settings'),
  // user_id: best-effort attribution (see auth/AuthContext.tsx's
  // currentUserRef) — the server just logs it alongside the message,
  // nothing queries by it yet, but it means a future admin-facing view over
  // these lines isn't starting from zero attribution.
  sendClientLog: (level: 'info' | 'warn' | 'error', message: string, userId?: string | null) =>
                   request<{ ok: boolean }>('POST', '/logs/client', { level, message, user_id: userId ?? undefined }),
    updateSettings: (b: Partial<{
        epg_debug: boolean;
        sync_debug: boolean;
        sync_threads: number;
        stream_buffer_size: number;
        image_cache_ttl_hours: number;
        verbose_transcode_logs: boolean;
        ffmpeg_debug_logs: boolean;
        verbose_gateway_logs: boolean;
        hades_debug: boolean;
        cast_app_id: string;
        default_landing_page: string;
        internal_token: string;
        guest_profiles_enabled: boolean;
        guest_idle_timeout_days: number;
        guest_max_concurrent: number;
        guest_channel_builder_enabled: boolean;
        guest_max_demo_channels: number;
        viewer_max_channels: number;
        require_admin_password_switch: boolean
    }>) => request<{
        epg_debug: boolean;
        sync_debug: boolean;
        sync_threads: number;
        stream_buffer_size: number;
        image_cache_ttl_hours: number;
        verbose_transcode_logs: boolean;
        ffmpeg_debug_logs: boolean;
        verbose_gateway_logs: boolean;
        hades_debug: boolean;
        cast_app_id: string;
        default_landing_page: string;
        internal_token: string;
        guest_profiles_enabled: boolean;
        guest_idle_timeout_days: number;
        guest_max_concurrent: number;
        guest_channel_builder_enabled: boolean;
        guest_max_demo_channels: number;
        viewer_max_channels: number;
        require_admin_password_switch: boolean
    }>('PATCH', '/config/settings', b),
  clearAllEpg:    ()                                                     => request<{ cleared: number }>('POST', '/config/epg/clear-all'),
  resetLibrary:   ()                                                     => request<{ ok: boolean }>('POST', '/config/library/reset'),

    // Scheduled jobs (sync/metadata_refresh/chapter_detection/writeback_sweep/
    // backup) — GET/PATCH /api/jobs is a unified surface over all five even
    // though "backup" is registered server-side by a separate service (see
    // kairos/src/api/services/JobService.h); its manual trigger lives at
    // POST /api/backup/run instead of .../jobs/backup/run-now.
    getJobs: () => request<ScheduledJob[]>('GET', '/jobs'),
    updateJob: (name: string, patch: ScheduledJobPatch) => request<void>('PATCH', `/jobs/${name}`, patch),
    runJobNow: (name: string) => request<{ status: 'started' | 'already_running' }>('POST', `/jobs/${name}/run-now`),

    getBackups: () => request<{ backups: BackupInfo[]; max_count: number }>('GET', '/backup'),
    updateBackupConfig: (maxCount: number) => request<void>('PATCH', '/backup/config', {max_count: maxCount}),
    runBackupNow: () => request<{ status: 'started' }>('POST', '/backup/run'),
    deleteBackup: (id: string) => request<void>('DELETE', `/backup/${id}`),
    // Restarts Kairos on success — see BackupManager.h's own comment on why
    // restore can't be a live hot-swap. The client should expect the
    // connection to drop shortly after this resolves and poll /health.
    restoreBackup: (id: string) => request<{ status: 'restoring' }>('POST', `/backup/${id}/restore`),

  // SMTP (invite emails + Settings page "Send Test Email")
  getSmtpConfig:  ()                                                     => request<SmtpConfig>('GET', '/config/smtp'),
  setSmtpConfig:  (b: Partial<{ host: string; port: string; username: string; password: string; from_address: string; public_base_url: string }>) =>
                                                                             request<{ ok: boolean }>('POST', '/config/smtp', b),
  testSmtp:       (to: string)                                           => request<{ ok: boolean }>('POST', '/config/smtp/test', { to }),

  // Downloads
  getDownloadConfig:  ()                                              => request<{path: string}>('GET', '/config/download'),
  setDownloadConfig:  (path: string)                                  => request<{ok: boolean}>('PUT', '/config/download', { path }),
  startDownload:      (url: string, path?: string)                    => request<{job_id: string}>('POST', '/download/jobs', { url, path }),
  getDownloadJobs:    ()                                              => request<DownloadJob[]>('GET', '/download/jobs'),

  // Generic REST helpers — paths must NOT include the /api prefix
  get:    <T = unknown>(path: string)                 => request<T>('GET',    path),
  post:   <T = unknown>(path: string, body: unknown)  => request<T>('POST',   path, body),
  patch:  <T = unknown>(path: string, body: unknown)  => request<T>('PATCH',  path, body),
  del:    <T = unknown>(path: string)                 => request<T>('DELETE', path),

  // Scrapers
  getScraperSettings:  ()                                         => request<ScraperSettings>('GET',   '/scrapers/config'),
  patchScraperSettings:(b: Partial<ScraperSettings>)              => request<{ok: boolean}>('PATCH', '/scrapers/config', b),
  triggerMatch:        (b?: { target_id?: string; item_type?: string }) =>
                         request<{status: string}>('POST', '/scrapers/match', b ?? {}),
  getMatchStatus:      ()                                         => request<{running: boolean}>('GET', '/scrapers/match/status'),
  triggerRefreshAll:   ()                                         => request<{status: string}>('POST', '/scrapers/refresh-all', {}),
  getRefreshAllStatus: ()                                         => request<{running: boolean; total: number; processed: number; refreshed: number; failed: number}>('GET', '/scrapers/refresh-all/status'),
  getScraperStats:     ()                                         => request<ScraperStats>('GET',    '/scrapers/stats'),
  getReviewQueue:      (p: { status?: string; limit?: number; offset?: number } = {}) =>
                         request<{items: ReviewQueueItem[]; total: number}>('GET', `/scrapers/queue?${qs(p)}`),
  acceptCandidate:     (id: string)                               =>
                         request<{ok: boolean; merged_into?: MergedInto; folder_mismatch?: boolean}>('POST', `/scrapers/queue/${id}/accept`, {}),
  rejectCandidate:     (id: string)                               => request<{ok: boolean}>('POST', `/scrapers/queue/${id}/reject`, {}),
  manualMatch:         (kairos_id: string, b: { item_type: 'show'|'movie'; source: string; external_id: string; title: string; year?: number; poster_url?: string; overview?: string }) =>
                         request<{ok: boolean; merged_into?: MergedInto; folder_mismatch?: boolean}>('POST', `/scrapers/queue/${kairos_id}/manual-match`, b),
  // Confirms the item's already-matched state (auto-matched or previously
  // manually picked) as human-reviewed, without a re-search + re-pick round
  // trip — see ScraperManager::confirmMatch().
  confirmMatch:        (kairos_id: string, item_type: 'show'|'movie')  =>
                         request<{ok: boolean}>('POST', `/scrapers/queue/${kairos_id}/confirm`, { item_type }),
  // Bulk-confirms every currently-matched-but-unconfirmed show/movie in one
  // shot — see ScraperManager::confirmAllMatches(). Synchronous (no
  // background job/polling — it's a plain DB flip, not a network operation).
  confirmAllMatches:   ()                                          =>
                         request<{ok: boolean; confirmed: number}>('POST', '/scrapers/confirm-all', {}),
    // Reverses confirmMatch() — see ScraperManager::unconfirmMatch(). Leaves
    // match_status alone (still "matched"), only clears match_confirmed.
    unconfirmMatch: (kairos_id: string, item_type: 'show' | 'movie') =>
        request<{ ok: boolean }>('POST', `/scrapers/queue/${kairos_id}/unconfirm`, {item_type}),
    // Bulk undo for an accidental "Confirm All Matches" — see
    // ScraperManager::unconfirmAllMatches(). Same synchronous plain-DB-flip
    // shape as confirmAllMatches().
    unconfirmAllMatches: () =>
        request<{ ok: boolean; unconfirmed: number }>('POST', '/scrapers/unconfirm-all', {}),
  scraperSearch:       (q: string, type?: 'show' | 'movie')       =>
                         request<{items: ScraperSearchResult[]}>('GET', `/scrapers/search?${qs({ q, type })}`),

  getItemMetadata:     (type: 'show'|'movie', id: string)         => request<ItemMetadata>('GET', `/scrapers/metadata/${type}/${id}`),
  setItemMetadata:     (type: 'show'|'movie', id: string, b: Partial<ItemMetadata>) => request<{ ok: boolean }>('POST', `/scrapers/metadata/${type}/${id}`, b),
  refreshItemMetadata: (type: 'show'|'movie', id: string)         => request<{ ok: boolean }>('POST', `/scrapers/metadata/${type}/${id}/refresh`),

  // Chapter review (visual inspection of chapter detection, not a commit flow)
  getChapterReviewItems: (p: { media_type?: string; chapter_type?: string; q?: string; limit?: number; offset?: number } = {}) =>
                           request<{items: ChapterReviewItem[]; total: number}>('GET', `/chapters/review?${qs(p)}`),

  // Broken subtitle sidecar review — see kairos's SubtitleValidation.h.
  getBrokenSubtitles: () => request<BrokenSubtitleItem[]>('GET', '/subtitles/broken'),
  // Re-runs the content check against the file as it exists on disk right
  // now (e.g. after the admin has gone and fixed/replaced it), rather than
  // waiting for the next full library sync to notice.
  recheckSubtitle: (id: string) => request<BrokenSubtitleItem>('POST', `/subtitles/${id}/recheck`),
  // Not durable against a source="file" row — the next full library sync
  // still unconditionally regenerates every sidecar-derived row for that
  // item from scratch, so an edit only sticks until the file is rescanned.
  updateSubtitle: (id: string, b: { language?: string; forced?: boolean; sdh?: boolean }) =>
                     request<BrokenSubtitleItem>('PATCH', `/subtitles/${id}`, b),
  // Permanently deletes the actual sidecar file from the media library, not
  // just this record — the point is clearing the slot so a downloader like
  // Bazarr treats the language as missing again and fetches a fresh file.
  // No undo.
  deleteSubtitle: (id: string) => request<{ ok: boolean; file_deleted: boolean }>('DELETE', `/subtitles/${id}`),

  // Chapters for the currently-playing item (skip-intro / credits detection)
  getEpisodeChapters: (id: string) => request<Chapter[]>('GET', `/episodes/${id}/chapters`),
  getMovieChapters:   (id: string) => request<Chapter[]>('GET', `/movies/${id}/chapters`),

  // Chapter processing — one-off re-probe for testing detection, without a full source sync
  syncMovieChapters: (id: string) => request<Chapter[]>('POST', `/movies/${id}/chapters/sync`, {}),
  syncShowChapters:  (id: string) => request<{ episode_count: number; processed: number; with_chapters: number }>('POST', `/shows/${id}/chapters/sync`, {}),

  // Throws ApiError(409, {target_folder, duplicate_folder}) if folders differ and !confirm.
    mergeShow: (id: string, duplicate_id: string, confirm = false) => request<{
        ok: boolean
    }>('POST', `/shows/${encodeURIComponent(id)}/merge`, {duplicate_id, confirm}),
    mergeMovie: (id: string, duplicate_id: string, confirm = false) => request<{
        ok: boolean
    }>('POST', `/movies/${encodeURIComponent(id)}/merge`, {duplicate_id, confirm}),

  // Sync-time "possible duplicate" review queue (see DuplicateCandidate).
  getDuplicatesQueue: (p: { item_type?: string; limit?: number; offset?: number } = {}) =>
                        request<{items: DuplicateCandidate[]; total: number}>('GET', `/duplicates/queue?${qs(p)}`),
  dismissDuplicate:   (id: string) => request<{ok: boolean}>('POST', `/duplicates/${id}/dismiss`, {}),

  // Chapter structure detection (async — ffmpeg scene-cut/fingerprint analysis, not the fast marker re-probe above)
  detectShowChapters:    (id: string) => request<{status: 'started'|'already_running'}>('POST', `/shows/${id}/chapters/detect`, {}),
  detectMovieChapters:   (id: string) => request<{status: 'started'|'already_running'}>('POST', `/movies/${id}/chapters/detect`, {}),
  getChapterDetectStatus: () => request<{running: boolean}>('GET', '/chapters/detect/status'),

  // Telemetry — see kairos/src/db/PlaybackHistoryRepository.h and
  // shared/crash/CrashHandler.h. Nothing here is ever sent anywhere but this
  // response; it's purely for the admin's own Telemetry tab.
  getActivityHistory: (params?: { userId?: string; fromMs?: number; toMs?: number; limit?: number }) => {
    const q = new URLSearchParams()
    if (params?.userId)  q.set('user_id', params.userId)
    if (params?.fromMs)  q.set('from', String(params.fromMs))
    if (params?.toMs)    q.set('to', String(params.toMs))
    if (params?.limit)   q.set('limit', String(params.limit))
    const qs = q.toString()
    return request<PlaybackHistoryEntry[]>('GET', `/activity/history${qs ? '?' + qs : ''}`)
  },
  // Admin-only — sessions still being pinged within the server's own
  // staleness window (kActiveStalenessMs), i.e. "who's actively playing
  // something right now" across every platform (web/android/roku/cast),
  // merged client-side with Roku's own ECP-heartbeat connections in
  // DeviceConnectionsPanel.
  getActiveSessions: () => request<PlaybackHistoryEntry[]>('GET', '/activity/active'),
  // Hermes's own aggregating route, not the generic /api/* proxy to Kairos —
  // combines all three services' local crash markers in one response.
  getCrashStatus: () => request<CrashStatus>('GET', '/activity/crash'),
  // Explicit "I've seen this" acknowledgment — clears all three services'
  // markers at once via Hermes' own aggregated DELETE. Admin-only.
  clearCrashStatus: () => request<{ hermes: boolean; kairos: boolean; hephaestus: boolean }>('DELETE', '/activity/crash'),

  // System
  getSystemMetrics: () => request<any>('GET', '/system/metrics'),
  // Structured per-operation timing/CPU/RAM/thread history for the hot
  // zones (sync, EPG regen, scraper match, chapter sync) — see
  // shared/metrics/OperationMetrics.h. Hits Kairos directly (falls through
  // Hermes's generic /api/* proxy, unlike /system/metrics which Hermes
  // itself aggregates).
  getOperationMetrics: () => request<OperationMetricsResponse>('GET', '/metrics/operations'),
}

export default api
