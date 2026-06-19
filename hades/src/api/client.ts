import type {
  Block, BlockContent, Channel, ContentType, CredentialStatus, Episode, EpisodeGroup,
  EpisodeSearchResult, EpgProgram, StartScope,
  FillerEntry, FillerEntryAdvancement, FillerList, FillerListDetail, FillerSelectionMode,
  Library, LibraryInfo, LibraryWithSource,
  Movie, MovieDetail, PagedResult, PathMap, PlexBrowseItem, PlexBrowseList,
  Playlist, PlaylistDetail, Show, ShowDetail, Source, SourceType,
} from './types'

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(`/api${path}`, {
    method,
    headers: body != null ? { 'Content-Type': 'application/json' } : undefined,
    body:    body != null ? JSON.stringify(body) : undefined,
  })
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
  // Sources
  getSources:       ()                                  => request<Source[]>    ('GET',    '/sources'),
  getSourceTypes:   ()                                  => request<SourceType[]>('GET',    '/sources/types'),
  createSource:     (b: Omit<Source, 'enabled'>)        => request<{source_id: string}>('POST', '/sources', b),
  deleteSource:     (id: string)                        => request<void>        ('DELETE', `/sources/${id}`),

  // Libraries
  getAvailableLibs: (sourceId: string)                  => request<LibraryInfo[]>('GET',    `/sources/${sourceId}/libraries/available`),
  getLibraries:     (sourceId: string)                  => request<Library[]>    ('GET',    `/sources/${sourceId}/libraries`),
  addLibrary:       (sourceId: string, b: Pick<Library, 'external_lib_id'|'display_name'|'library_type'>) =>
                                                        request<{library_id: string}>('POST', `/sources/${sourceId}/libraries`, b),
  removeLibrary:    (sourceId: string, lid: string)     => request<void>         ('DELETE', `/sources/${sourceId}/libraries/${lid}`),
  triggerSync:      (sourceId: string)                  => request<{status: string}>('POST', `/sources/${sourceId}/sync`),
  syncAll:          ()                                  => request<{status: string}>('POST', '/sync/all'),

  // Channels
  getChannels:      ()                                                            => request<Channel[]>('GET',    '/channels'),
  createChannel:    (b: Omit<Channel, 'channel_id' | 'default_filler_entries' | 'default_filler_selection'>) => request<{channel_id: string}>('POST', '/channels', b),
  updateChannel:    (id: string, b: Partial<Pick<Channel, 'name' | 'number' | 'timezone' | 'seed' | 'default_filler_selection' | 'advance_mode'>>) => request<void>('PATCH', `/channels/${id}`, b),
  deleteChannel:    (id: string)                                                  => request<void>('DELETE', `/channels/${id}`),

  // Channel filler entries
  addChannelFiller:    (channelId: string, b: { filler_list_id: string; advancement: FillerEntryAdvancement; weight?: number }) =>
                         request<FillerEntry>('POST',   `/channels/${channelId}/filler`, b),
  updateChannelFiller: (channelId: string, entryId: number, b: { advancement?: FillerEntryAdvancement; weight?: number }) =>
                         request<void>       ('PATCH',  `/channels/${channelId}/filler/${entryId}`, b),
  removeChannelFiller: (channelId: string, entryId: number) =>
                         request<void>       ('DELETE', `/channels/${channelId}/filler/${entryId}`),

  // Connection test (no persistence)
  testSource:       (b: {source_type: string, base_url: string, token: string}) =>
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
  getShows:       (p: { limit?: number; offset?: number; library_id?: string; q?: string; genre?: string; year?: number; content_rating?: string } = {}) =>
                    request<PagedResult<Show>>('GET', `/shows?${qs(p)}`),
  getEpisodes:    (showId: string, season?: number)     => request<Episode[]>('GET', `/shows/${showId}/episodes${season != null ? '?season=' + season : ''}`),
  getMovies:      (p: { limit?: number; offset?: number; library_id?: string; q?: string; genre?: string; year?: number; content_rating?: string } = {}) =>
                    request<PagedResult<Movie>>('GET', `/movies?${qs(p)}`),

  // Blocks
  getBlocks:         (channelId: string)                                          => request<Block[]>('GET', `/channels/${channelId}/blocks`),
  createBlock:       (channelId: string, b: Omit<Block, 'block_id'|'channel_id'|'content'|'filler_entries'>) =>
                       request<{block_id: string}>('POST', `/channels/${channelId}/blocks`, b),
  updateBlock:       (channelId: string, blockId: string, b: Partial<Omit<Block, 'block_id'|'channel_id'|'content'|'filler_entries'>>) => request<void>('PATCH', `/channels/${channelId}/blocks/${blockId}`, b),
  deleteBlock:       (channelId: string, blockId: string)                         => request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}`),
  addBlockContent:   (channelId: string, blockId: string, b: { content_type: ContentType; content_id: string; season_filter?: number | null; weight?: number; run_count?: number }) =>
                       request<{id: number, position: number}>('POST', `/channels/${channelId}/blocks/${blockId}/content`, b),
  updateBlockContent:(channelId: string, blockId: string, cid: number, b: { season_filter?: number | null; position?: number; weight?: number; run_count?: number }) =>
                       request<void>('PATCH', `/channels/${channelId}/blocks/${blockId}/content/${cid}`, b),
  removeBlockContent:       (channelId: string, blockId: string, cid: number)     => request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}/content/${cid}`),
  resetBlockContentCursor:  (channelId: string, blockId: string, cid: number)     => request<void>('DELETE', `/channels/${channelId}/blocks/${blockId}/content/${cid}/cursor`),

  // Episode groups
  getEpisodeGroups:       (showId: string)                                         => request<EpisodeGroup[]>('GET',    `/shows/${showId}/groups`),
  createEpisodeGroup:     (showId: string, b: { name: string; group_type?: string })  => request<{group_id: string}>('POST', `/shows/${showId}/groups`, b),
  deleteEpisodeGroup:     (showId: string, groupId: string)                        => request<void>('DELETE', `/shows/${showId}/groups/${groupId}`),
  addGroupMember:         (showId: string, groupId: string, b: { episode_id: string; part_num: number }) =>
                            request<{id: number, part_num: number}>('POST',   `/shows/${showId}/groups/${groupId}/members`, b),
  removeGroupMember:      (showId: string, groupId: string, memberId: number)      => request<void>('DELETE', `/shows/${showId}/groups/${groupId}/members/${memberId}`),

  // Block filler entries
  addBlockFiller:    (channelId: string, blockId: string, b: { filler_list_id: string; advancement: FillerEntryAdvancement; weight?: number }) =>
                       request<FillerEntry>('POST',   `/channels/${channelId}/blocks/${blockId}/filler`, b),
  updateBlockFiller: (channelId: string, blockId: string, entryId: number, b: { advancement?: FillerEntryAdvancement; weight?: number }) =>
                       request<void>       ('PATCH',  `/channels/${channelId}/blocks/${blockId}/filler/${entryId}`, b),
  removeBlockFiller: (channelId: string, blockId: string, entryId: number) =>
                       request<void>       ('DELETE', `/channels/${channelId}/blocks/${blockId}/filler/${entryId}`),

  // Channel EPG — cache-backed (used by XMLTV/m3u generation)
  getChannelEpg: (channelId: string, hours?: number) =>
    request<EpgProgram[]>('GET', `/channels/${channelId}/epg${hours != null ? `?hours=${hours}` : ''}`),
  // EPG preview — returns cached schedule if available, else in-memory projection (no DB writes)
  // Pass force=true to bypass cache and always simulate from current cursor state.
  previewChannelEpg: (channelId: string, hours?: number, seed?: number, force?: boolean) => {
    const params = qs({ hours: hours ?? undefined, seed: seed ?? undefined, force: force ? 1 : undefined })
    return request<EpgProgram[]>('GET', `/channels/${channelId}/epg/preview${params ? `?${params}` : ''}`)
  },

  // Episode search
  getShowSeasons:    (showId: string)                                             => request<{seasons: number[]}>('GET', `/shows/${showId}/seasons`),
  searchEpisodes:    (p: { q?: string; show_id?: string; season?: number; limit?: number } = {}) =>
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
}
