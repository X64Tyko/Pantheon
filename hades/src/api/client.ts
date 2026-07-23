import type {
  ArrConfig, ArrLookupResult, ArrServiceOptions,
  AuthResponse,
  Block, BlockContent, BumperContentType, BumperMode, ChannelBumper, ChannelExport,
  CastSessionInfo, CastTokenResponse,
  Channel, ContentOverride, ContentRequest, ContentType, CredentialStatus, DownloadJob, EpisodeOrder,
  ActivitySession, Chapter, ChapterReviewItem,
  Episode, EpisodeGroup, EpisodeSearchResult, EpgPreviewResponse, EpgProgram, ExportDepth, GroupingCandidatesResult, ImportPreviewResult, ImportResult, MediaLanguages, ShowGroupingResult, StartScope,
  FillerEntry, FillerEntryAdvancement, FillerList, FillerListDetail, FillerSelectionMode,
  Library, LibraryInfo, LibraryWithSource,
  Movie, MovieDetail, PagedResult, PathMap, PlexBrowseItem, PlexBrowseList,
  Playlist, PlaylistDetail, PlaylistExport, PlaylistImportPreviewResult, PlaylistImportResult, HomePlaylistShelf, UnresolvedSyncItem, PlaylistBrowseEntry, PlaylistItem,
  ReviewQueueItem, ScraperSearchResult, ScraperSettings, ScraperSource, ScraperStats, MergedInto, DuplicateCandidate,
  Show, ShowDetail, Source, SourceType, SpecialCandidate, LinkedSpecial, User, VideoInfo, WatchProgress, WritebackResult,
  NextEpisode, ShowWatchState, ResolvedPlayTarget, TvManifest, ChannelNow,
  ItemMetadata, ExternalId, RokuDevice, RokuDeviceState, DeviceConnection,
  UnmappedSourceUser, SourceUser, ImportUserResult, InviteUserResult, SmtpConfig,
  OperationMetricsResponse,
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

function qs(params: Record<string, string | number | undefined>): string {
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
  createChannel:    (b: Omit<Channel, 'channel_id' | 'default_filler_entries' | 'default_filler_selection'>) => request<{channel_id: string}>('POST', '/channels', b),
  updateChannel:    (id: string, b: Partial<Pick<Channel, 'name' | 'number' | 'timezone' | 'seed' | 'default_filler_selection' | 'advance_mode' | 'offline_video_path' | 'offline_image_path' | 'offline_audio_id' | 'offline_audio_type' | 'offline_audio_title' | 'logo_path' | 'anchor_hashes' | 'audio_lang' | 'subtitle_lang' | 'stream_resolution' | 'stream_video_bitrate' | 'stream_audio_bitrate' | 'content_tag'>>) => request<void>('PATCH', `/channels/${id}`, b),
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
  getEpisodes:    (showId: string, season?: number)     => request<Episode[]>('GET', `/shows/${showId}/episodes${season != null ? '?season=' + season : ''}`),
  getMovies:      (p: { limit?: number; offset?: number; library_id?: string; library_ids?: string; q?: string; filter?: string; sort?: string; sort_dir?: 'asc' | 'desc'; seed?: number; home?: boolean; hideEmpty?: boolean; playlist_id?: string } = {}) =>
                    request<PagedResult<Movie>>('GET', `/movies?${qs({ ...withCombinedFilter(p), hideEmpty: undefined, hide_empty: p.hideEmpty ? 1 : undefined })}`),

  // Watch progress
  getWatchProgress:   (limit?: number)                                     => request<WatchProgress[]>('GET', `/watch-progress${limit != null ? '?limit=' + limit : ''}`),
  // device_type/direct_play (both optional) additionally feed the local
  // play-history table (see kairos's PlaybackHistoryRepository) — nothing
  // client-side reads them back, they're just piggybacked onto pings this
  // call already sends.
  putWatchProgress:   (contentType: 'movie' | 'episode', id: string, b: { position_ms: number; duration_ms: number; completed?: boolean; device_type?: string; direct_play?: boolean }) =>
                        request<{ ok: boolean; watched: boolean }>('PUT', `/watch-progress/${contentType}/${id}`, b),
  clearWatchProgress: (contentType: 'movie' | 'episode', id: string)       => request<void>('DELETE', `/watch-progress/${contentType}/${id}`),
  getShowWatchState:  (showId: string)                                     => request<ShowWatchState | null>('GET', `/shows/${showId}/watch-state`),

  // Server-side resolvePlayTarget.ts (see hades/src/player/resolvePlayTarget.ts) —
  // one call instead of watch-state + conditionally next-episode/full-episode-list.
  getResolvedPlayTarget: (showId: string)                                  => request<ResolvedPlayTarget | null>('GET', `/shows/${showId}/resolve-play-target`),

  // Home row composition + Library/Detail/Guide zone layout — see hades/src/tv/useHomeManifest.ts.
  getTvManifest: ()                                                        => request<TvManifest>('GET', '/tv/manifest'),

  // Series continuation. Deliberately not ".../next" — see the Kairos route's
  // own comment (ContentService.cpp): that suffix is exempted from auth for
  // the live-channel schedule lookup and would leak episode metadata unauthenticated.
  getNextEpisode:     (episodeId: string)                                  => request<NextEpisode | null>('GET', `/episodes/${episodeId}/next-episode`),

  // Blocks
  getBlocks:         (channelId: string)                                          => request<Block[]>('GET', `/channels/${channelId}/blocks`),
  createBlock:       (channelId: string, b: Omit<Block, 'block_id'|'channel_id'|'content'|'filler_entries'>) =>
                       request<{block_id: string}>('POST', `/channels/${channelId}/blocks`, b),
  updateBlock:       (channelId: string, blockId: string, b: Partial<Omit<Block, 'block_id'|'channel_id'|'content'|'filler_entries'>>) => request<void>('PATCH', `/channels/${channelId}/blocks/${blockId}`, b),
  deleteBlock:       (channelId: string, blockId: string)                         => request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}`),
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
  setShowFindSpecials:    (showId: string, find_specials: boolean) => request<{ ok: boolean }>('PATCH', `/shows/${showId}/find-specials`, { find_specials }),
  setEpisodeDisplayOrder: (showId: string, episode_display_order: 'season' | 'aired') =>
                            request<{ ok: boolean }>('PATCH', `/shows/${showId}/episode-display-order`, { episode_display_order }),

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
  clearChannelEpgCache: (channelId: string) =>
    request<{ ok: boolean }>('POST', `/channels/${channelId}/epg/clear`),
  // EPG preview — POST with optional seed, hours, and draft blocks.
  // Returns { programs, anchors } where anchors maps week-anchor timestamps to mutated seeds.
  previewChannelEpg: (channelId: string, hours?: number, seed?: number, blocks?: Block[]) =>
    request<EpgPreviewResponse>('POST', `/channels/${channelId}/epg/preview`, {
      ...(hours != null ? { hours } : {}),
      ...(seed  != null ? { seed  } : {}),
      ...(blocks        ? { blocks } : {}),
    }),

  // Episode search
  getShowSeasons:    (showId: string)                                             => request<{seasons: {number: number; name: string}[]}>('GET', `/shows/${showId}/seasons`),
  searchEpisodes:    (p: {
                       q?: string; show_id?: string; season?: number; limit?: number; offset?: number
                       // 'playlist_order' only meaningful (and only offered by the UI) together with playlist_id —
                       // see LibraryFilters.tsx's conditional sort option.
                       sort?: 'title' | 'playlist_order'; sort_dir?: 'asc' | 'desc'; playlist_id?: string
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
                       smart_type?: 'show'|'movie'; smart_sort?: string; smart_limit?: number
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
  getShow:        (id: string)                          => request<ShowDetail>('GET',   `/shows/${id}`),
  updateShow:     (id: string, b: Partial<ShowDetail>)  => request<void>      ('PATCH', `/shows/${id}`, b),
  getMovie:       (id: string)                          => request<MovieDetail>('GET',  `/movies/${id}`),
  // Minimal single-episode lookup (title/overview/show_title/season/episode
  // only) — for resolving a display title (+ hero overview) from a bare
  // episode_id, e.g. the connections view and Home/TV's Continue Watching
  // hero. Not the full episode detail other pages might expect.
  getEpisodeBrief: (id: string) => request<{ episode_id: string; season: number; episode: number; title: string; overview: string; show_id: string; show_title: string }>('GET', `/episodes/${id}`),
  updateMovie:    (id: string, b: Partial<MovieDetail>) => request<void>       ('PATCH',`/movies/${id}`, b),
  pushToSources:  (id: string, contentType: 'show' | 'movie') =>
                    request<WritebackResult>('POST', `/${contentType}s/${id}/writeback`),
  refreshMetadata: (id: string, contentType: 'show' | 'movie') =>
                    request<{ ok: boolean }>('POST', `/${contentType}s/${id}/refresh-metadata`),
  // Separate from updateShow/updateMovie, which always lock the record as a
  // side effect of a metadata edit — this flag is orthogonal to that.
  setShowSkipScraping:  (id: string, skip_scraping: boolean) =>
                    request<{ ok: boolean }>('PATCH', `/shows/${id}/skip-scraping`, { skip_scraping }),
  setMovieSkipScraping: (id: string, skip_scraping: boolean) =>
                    request<{ ok: boolean }>('PATCH', `/movies/${id}/skip-scraping`, { skip_scraping }),
  getShowLanguages:  (id: string) => request<MediaLanguages>('GET', `/shows/${id}/languages`),
  getMovieLanguages: (id: string) => request<MediaLanguages>('GET', `/movies/${id}/languages`),
  getShowVideoInfo:  (id: string) => request<VideoInfo>('GET', `/shows/${id}/videoinfo`),
  getMovieVideoInfo: (id: string) => request<VideoInfo>('GET', `/movies/${id}/videoinfo`),

  // Channel bumpers
  getBumpers:    (channelId: string)                                                           => request<ChannelBumper[]>('GET',    `/channels/${channelId}/bumpers`),
  createBumper:  (channelId: string, b: { content_type: BumperContentType; content_id: string; mode: BumperMode; every_n: number; season_filter?: number }) =>
                   request<ChannelBumper>                            ('POST',   `/channels/${channelId}/bumpers`, b),
  updateBumper:  (channelId: string, bumperId: number, b: Partial<Pick<ChannelBumper, 'content_type'|'content_id'|'mode'|'every_n'|'position'>>) =>
                   request<void>                                     ('PATCH',  `/channels/${channelId}/bumpers/${bumperId}`, b),
  deleteBumper:  (channelId: string, bumperId: number)               => request<void>          ('DELETE', `/channels/${channelId}/bumpers/${bumperId}`),

  // Runtime settings
  getSettings:    ()                                                     => request<{ epg_debug: boolean; sync_debug: boolean; sync_threads: number; stream_buffer_size: number; image_cache_ttl_hours: number; verbose_transcode_logs: boolean; verbose_gateway_logs: boolean; hades_debug: boolean; cast_app_id: string }>('GET',   '/config/settings'),
  // Public read-only subset for internal services (Hephaestus, Hermes) and
  // the Hades frontend (CastProvider, which needs cast_app_id regardless of role).
  getPublicSettings: ()                                                  => request<{ stream_buffer_size: number; verbose_transcode_logs: boolean; verbose_gateway_logs: boolean; cast_app_id: string }>('GET', '/config/public-settings'),
  // user_id: best-effort attribution (see auth/AuthContext.tsx's
  // currentUserRef) — the server just logs it alongside the message,
  // nothing queries by it yet, but it means a future admin-facing view over
  // these lines isn't starting from zero attribution.
  sendClientLog: (level: 'info' | 'warn' | 'error', message: string, userId?: string | null) =>
                   request<{ ok: boolean }>('POST', '/logs/client', { level, message, user_id: userId ?? undefined }),
  updateSettings: (b: Partial<{ epg_debug: boolean; sync_debug: boolean; sync_threads: number; stream_buffer_size: number; image_cache_ttl_hours: number; verbose_transcode_logs: boolean; verbose_gateway_logs: boolean; hades_debug: boolean; cast_app_id: string }>) => request<{ epg_debug: boolean; sync_debug: boolean; sync_threads: number; stream_buffer_size: number; image_cache_ttl_hours: number; verbose_transcode_logs: boolean; verbose_gateway_logs: boolean; hades_debug: boolean; cast_app_id: string }>('PATCH', '/config/settings', b),
  clearAllEpg:    ()                                                     => request<{ cleared: number }>('POST', '/config/epg/clear-all'),
  resetLibrary:   ()                                                     => request<{ ok: boolean }>('POST', '/config/library/reset'),

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
  scraperSearch:       (q: string, type?: 'show' | 'movie')       =>
                         request<{items: ScraperSearchResult[]}>('GET', `/scrapers/search?${qs({ q, type })}`),

  getItemMetadata:     (type: 'show'|'movie', id: string)         => request<ItemMetadata>('GET', `/scrapers/metadata/${type}/${id}`),
  setItemMetadata:     (type: 'show'|'movie', id: string, b: Partial<ItemMetadata>) => request<{ ok: boolean }>('POST', `/scrapers/metadata/${type}/${id}`, b),
  refreshItemMetadata: (type: 'show'|'movie', id: string)         => request<{ ok: boolean }>('POST', `/scrapers/metadata/${type}/${id}/refresh`),

  // Chapter review (visual inspection of chapter detection, not a commit flow)
  getChapterReviewItems: (p: { media_type?: string; chapter_type?: string; q?: string; limit?: number; offset?: number } = {}) =>
                           request<{items: ChapterReviewItem[]; total: number}>('GET', `/chapters/review?${qs(p)}`),

  // Chapters for the currently-playing item (skip-intro / credits detection)
  getEpisodeChapters: (id: string) => request<Chapter[]>('GET', `/episodes/${id}/chapters`),
  getMovieChapters:   (id: string) => request<Chapter[]>('GET', `/movies/${id}/chapters`),

  // Chapter processing — one-off re-probe for testing detection, without a full source sync
  syncMovieChapters: (id: string) => request<Chapter[]>('POST', `/movies/${id}/chapters/sync`, {}),
  syncShowChapters:  (id: string) => request<{ episode_count: number; processed: number; with_chapters: number }>('POST', `/shows/${id}/chapters/sync`, {}),

  // Throws ApiError(409, {target_folder, duplicate_folder}) if folders differ and !confirm.
  mergeShow:  (id: string, duplicate_id: string, confirm = false) => request<{ok: boolean}>('POST', `/shows/${id}/merge`, { duplicate_id, confirm }),
  mergeMovie: (id: string, duplicate_id: string, confirm = false) => request<{ok: boolean}>('POST', `/movies/${id}/merge`, { duplicate_id, confirm }),

  // Sync-time "possible duplicate" review queue (see DuplicateCandidate).
  getDuplicatesQueue: (p: { item_type?: string; limit?: number; offset?: number } = {}) =>
                        request<{items: DuplicateCandidate[]; total: number}>('GET', `/duplicates/queue?${qs(p)}`),
  dismissDuplicate:   (id: string) => request<{ok: boolean}>('POST', `/duplicates/${id}/dismiss`, {}),

  // Chapter structure detection (async — ffmpeg scene-cut/fingerprint analysis, not the fast marker re-probe above)
  detectShowChapters:    (id: string) => request<{status: 'started'|'already_running'}>('POST', `/shows/${id}/chapters/detect`, {}),
  detectMovieChapters:   (id: string) => request<{status: 'started'|'already_running'}>('POST', `/movies/${id}/chapters/detect`, {}),
  getChapterDetectStatus: () => request<{running: boolean}>('GET', '/chapters/detect/status'),

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
