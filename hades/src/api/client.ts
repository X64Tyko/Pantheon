import type {
  ArrConfig, ArrLookupResult, ArrServiceOptions,
  AuthResponse,
  Block, BlockContent, BumperContentType, BumperMode, ChannelBumper, ChannelExport,
  Channel, ContentRequest, ContentType, CredentialStatus, DownloadJob, EpisodeOrder,
  Episode, EpisodeGroup, EpisodeSearchResult, EpgPreviewResponse, EpgProgram, ExportDepth, GroupingCandidatesResult, ImportPreviewResult, ImportResult, MediaLanguages, ShowGroupingResult, StartScope,
  FillerEntry, FillerEntryAdvancement, FillerList, FillerListDetail, FillerSelectionMode,
  Library, LibraryInfo, LibraryWithSource,
  Movie, MovieDetail, PagedResult, PathMap, PlexBrowseItem, PlexBrowseList,
  Playlist, PlaylistDetail, ReviewQueueItem, ScraperSearchResult, ScraperSettings, ScraperStats,
  Show, ShowDetail, Source, SourceType, User,
} from './types'

export const TOKEN_KEY = 'kairos_token'

/** Append ?token= to a media proxy path so <img> tags can authenticate. */
export function mediaUrl(path: string): string {
  const token = localStorage.getItem(TOKEN_KEY)
  return token ? `${path}?token=${encodeURIComponent(token)}` : path
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const headers: Record<string, string> = {}
  if (body != null) headers['Content-Type'] = 'application/json'
  const token = localStorage.getItem(TOKEN_KEY)
  if (token) headers['Authorization'] = `Bearer ${token}`

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
    throw new Error((payload as any).error ?? res.statusText)
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

export const api = {
  // Auth
  checkSetup:  ()                                            => request<{ setup_required: boolean }>('GET',    '/auth/setup'),
  setup:       (username: string, password: string)          => request<AuthResponse>('POST', '/auth/setup', { username, password }),
  login:       (username: string, password: string)          => request<AuthResponse>('POST', '/auth/login', { username, password }),
  logout:      ()                                            => request<void>('POST', '/auth/logout'),
  getMe:       ()                                            => request<User>('GET',  '/auth/me'),
  // User management (admin only)
  getUsers:    ()                                            => request<User[]>('GET',    '/users'),
  createUser:  (username: string, password: string, role: string) => request<void>('POST', '/users', { username, password, role }),
  updateUser:  (id: string, patch: { password?: string; role?: string }) => request<void>('PATCH', `/users/${id}`, patch),
  deleteUser:  (id: string)                                 => request<void>('DELETE', `/users/${id}`),

  // Sources
  getSources:       ()                                  => request<Source[]>    ('GET',    '/sources'),
  getSourceTypes:   ()                                  => request<SourceType[]>('GET',    '/sources/types'),
  createSource:     (b: Omit<Source, 'enabled'>)        => request<{source_id: string}>('POST', '/sources', b),
  deleteSource:     (id: string)                        => request<void>        ('DELETE', `/sources/${id}`),

  // Libraries
  getAvailableLibs: (sourceId: string)                  => request<LibraryInfo[]>('GET',    `/sources/${sourceId}/libraries/available`),
  browseLocalDir:   (sourceId: string, path: string)    => request<LibraryInfo[]>('GET',    `/sources/${sourceId}/fs${path ? `?path=${encodeURIComponent(path)}` : ''}`),
  getLibraries:     (sourceId: string)                  => request<Library[]>    ('GET',    `/sources/${sourceId}/libraries`),
  addLibrary:       (sourceId: string, b: Pick<Library, 'external_lib_id'|'display_name'|'library_type'|'preferred_scraper'|'preferred_language'>) =>
                                                        request<{library_id: string}>('POST', `/sources/${sourceId}/libraries`, b),
  patchLibrary:     (sourceId: string, lid: string, b: Partial<Pick<Library, 'display_name'|'preferred_scraper'|'preferred_language'>>) =>
                                                        request<void>('PATCH', `/sources/${sourceId}/libraries/${lid}`, b),
  removeLibrary:    (sourceId: string, lid: string)     => request<void>         ('DELETE', `/sources/${sourceId}/libraries/${lid}`),
  triggerSync:      (sourceId: string)                  => request<{status: string}>('POST', `/sources/${sourceId}/sync`),
  syncAll:          ()                                  => request<{status: string}>('POST', '/sync/all'),

  // Media language catalog (probed from library sample via ffprobe, cached 1 h)
  getMediaLanguages: () => request<MediaLanguages>('GET', '/media/languages'),

  // Channels
  getChannels:      ()                                                            => request<Channel[]>('GET',    '/channels'),
  createChannel:    (b: Omit<Channel, 'channel_id' | 'default_filler_entries' | 'default_filler_selection'>) => request<{channel_id: string}>('POST', '/channels', b),
  updateChannel:    (id: string, b: Partial<Pick<Channel, 'name' | 'number' | 'timezone' | 'seed' | 'default_filler_selection' | 'advance_mode' | 'offline_video_path' | 'offline_image_path' | 'offline_audio_id' | 'offline_audio_type' | 'offline_audio_title' | 'logo_path' | 'anchor_hashes' | 'audio_lang' | 'subtitle_lang'>>) => request<void>('PATCH', `/channels/${id}`, b),
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
  createRequest:  (b: { content_type: 'show'|'movie'; source: 'tmdb'|'tvdb'|'anidb'; external_id: string; title: string; year?: number; poster_url?: string }) =>
                    request<{ request_id: string; status: string; duplicate?: boolean }>('POST', '/requests', b),
  updateRequest:  (id: string, status: 'approved'|'rejected')                                => request<{ok: boolean}>('PATCH',  `/requests/${id}`, { status }),
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
  getSyncStatus:    ()                                  => request<{running: boolean}>('GET', '/sync/status'),

  // Content — list
  getAllLibraries: ()                                    => request<LibraryWithSource[]>('GET', '/libraries'),
  getFilterValues: (field: string, params: { type?: 'movie' | 'show'; library_id?: string } = {}) =>
    request<{ values: string[] }>('GET', `/metadata/values?${qs({ field, ...params })}`).then(r => r.values),
  getShows:       (p: { limit?: number; offset?: number; library_id?: string; q?: string; genre?: string; year?: number; content_rating?: string; label?: string; network?: string; actor?: string; sort?: string } = {}) =>
                    request<PagedResult<Show>>('GET', `/shows?${qs(p)}`),
  getEpisodes:    (showId: string, season?: number)     => request<Episode[]>('GET', `/shows/${showId}/episodes${season != null ? '?season=' + season : ''}`),
  getMovies:      (p: { limit?: number; offset?: number; library_id?: string; q?: string; genre?: string; year?: number; content_rating?: string; label?: string; actor?: string; sort?: string } = {}) =>
                    request<PagedResult<Movie>>('GET', `/movies?${qs(p)}`),

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

  // Block filler entries
  addBlockFiller:    (channelId: string, blockId: string, b: { content_type: string; content_id: string; advancement: FillerEntryAdvancement; weight?: number; season_filter?: number }) =>
                       request<FillerEntry>('POST',   `/channels/${channelId}/blocks/${blockId}/filler`, b),
  updateBlockFiller: (channelId: string, blockId: string, entryId: number, b: { advancement?: FillerEntryAdvancement; weight?: number }) =>
                       request<void>       ('PATCH',  `/channels/${channelId}/blocks/${blockId}/filler/${entryId}`, b),
  removeBlockFiller: (channelId: string, blockId: string, entryId: number) =>
                       request<void>       ('DELETE', `/channels/${channelId}/blocks/${blockId}/filler/${entryId}`),

  // Channel EPG — cache-backed (used by XMLTV/m3u generation)
  getChannelEpg: (channelId: string, hours?: number) =>
    request<EpgProgram[]>('GET', `/channels/${channelId}/epg${hours != null ? `?hours=${hours}` : ''}`),
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
  searchEpisodes:    (p: { q?: string; show_id?: string; season?: number; limit?: number; offset?: number } = {}) =>
                       request<{items: EpisodeSearchResult[]}>('GET', `/episodes?${qs(p)}`),

  // Playlists
  getPlaylists:      ()                                       => request<Playlist[]>    ('GET',    '/playlists'),
  createPlaylist:    (b: { title: string })                   => request<{playlist_id: string}>('POST', '/playlists', b),
  getPlaylist:       (id: string)                             => request<PlaylistDetail>('GET',    `/playlists/${id}`),
  updatePlaylist:    (id: string, b: { title?: string; mode?: string }) => request<void>('PATCH',  `/playlists/${id}`, b),
  deletePlaylist:    (id: string)                             => request<void>          ('DELETE', `/playlists/${id}`),
  addPlaylistItem:   (id: string, b: { item_type: 'episode'|'movie'; item_id: string }) =>
                       request<{id: number, position: number}>('POST',   `/playlists/${id}/items`, b),
  removePlaylistItem:(id: string, iid: number)                => request<void>          ('DELETE', `/playlists/${id}/items/${iid}`),
  movePlaylistItem:  (id: string, iid: number, position: number) => request<void>       ('PATCH',  `/playlists/${id}/items/${iid}`, { position }),

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

  // Plex-linked list sync
  plexSyncPlaylist:        (id: string, b: { source_id: string; external_id: string; plex_type: 'playlist'|'collection' }) =>
                             request<{synced: number; total: number}>('POST', `/playlists/${id}/plex-sync`, b),
  unlinkPlaylist:          (id: string) => request<void>('DELETE', `/playlists/${id}/plex-link`),
  plexSyncAllPlaylists:    () => request<{status: string}>('POST', '/playlists/plex-sync-all'),
  plexSyncFillerList:      (id: string, b: { source_id: string; external_id: string; plex_type: 'playlist'|'collection' }) =>
                             request<{synced: number; total: number}>('POST', `/filler-lists/${id}/plex-sync`, b),
  unlinkFillerList:        (id: string) => request<void>('DELETE', `/filler-lists/${id}/plex-link`),
  plexSyncAllFillerLists:  () => request<{status: string}>('POST', '/filler-lists/plex-sync-all'),

  // Plex browse — lists playlists / collections live from the Plex server
  browsePlexPlaylists:         (sourceId: string)                   => request<PlexBrowseList[]>('GET', `/sources/${sourceId}/browse/playlists`),
  browsePlexPlaylistItems:     (sourceId: string, plid: string)     => request<PlexBrowseItem[]>('GET', `/sources/${sourceId}/browse/playlists/${plid}/items`),
  browsePlexCollections:       (sourceId: string, libraryId: string)=> request<PlexBrowseList[]>('GET', `/sources/${sourceId}/browse/collections?library_id=${encodeURIComponent(libraryId)}`),
  browsePlexCollectionItems:   (sourceId: string, cid: string)      => request<PlexBrowseItem[]>('GET', `/sources/${sourceId}/browse/collections/${cid}/items`),

  // Content — detail + update
  getShow:        (id: string)                          => request<ShowDetail>('GET',   `/shows/${id}`),
  updateShow:     (id: string, b: Partial<ShowDetail>)  => request<void>      ('PATCH', `/shows/${id}`, b),
  getMovie:       (id: string)                          => request<MovieDetail>('GET',  `/movies/${id}`),
  updateMovie:    (id: string, b: Partial<MovieDetail>) => request<void>       ('PATCH',`/movies/${id}`, b),

  // Channel bumpers
  getBumpers:    (channelId: string)                                                           => request<ChannelBumper[]>('GET',    `/channels/${channelId}/bumpers`),
  createBumper:  (channelId: string, b: { content_type: BumperContentType; content_id: string; mode: BumperMode; every_n: number; season_filter?: number }) =>
                   request<ChannelBumper>                            ('POST',   `/channels/${channelId}/bumpers`, b),
  updateBumper:  (channelId: string, bumperId: number, b: Partial<Pick<ChannelBumper, 'content_type'|'content_id'|'mode'|'every_n'|'position'>>) =>
                   request<void>                                     ('PATCH',  `/channels/${channelId}/bumpers/${bumperId}`, b),
  deleteBumper:  (channelId: string, bumperId: number)               => request<void>          ('DELETE', `/channels/${channelId}/bumpers/${bumperId}`),

  // Runtime settings
  getSettings:    ()                                                     => request<{ epg_debug: boolean; sync_debug: boolean; sync_threads: number; image_cache_ttl_hours: number }>('GET',   '/config/settings'),
  updateSettings: (b: Partial<{ epg_debug: boolean; sync_debug: boolean; sync_threads: number; image_cache_ttl_hours: number }>) => request<{ epg_debug: boolean; sync_debug: boolean; sync_threads: number; image_cache_ttl_hours: number }>('PATCH', '/config/settings', b),
  clearAllEpg:    ()                                                     => request<{ cleared: number }>('POST', '/config/epg/clear-all'),
  resetLibrary:   ()                                                     => request<{ ok: boolean }>('POST', '/config/library/reset'),

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
  getScraperStats:     ()                                         => request<ScraperStats>('GET',    '/scrapers/stats'),
  getReviewQueue:      (p: { status?: string; limit?: number; offset?: number } = {}) =>
                         request<{items: ReviewQueueItem[]; total: number}>('GET', `/scrapers/queue?${qs(p)}`),
  acceptCandidate:     (id: string)                               => request<{ok: boolean}>('POST', `/scrapers/queue/${id}/accept`, {}),
  rejectCandidate:     (id: string)                               => request<{ok: boolean}>('POST', `/scrapers/queue/${id}/reject`, {}),
  manualMatch:         (kairos_id: string, b: { item_type: 'show'|'movie'; source: string; external_id: string; title: string; year?: number; poster_url?: string; overview?: string }) =>
                         request<{ok: boolean}>('POST', `/scrapers/queue/${kairos_id}/manual-match`, b),
  scraperSearch:       (q: string, type?: 'show' | 'movie')       =>
                         request<{items: ScraperSearchResult[]}>('GET', `/scrapers/search?${qs({ q, type })}`),
}
