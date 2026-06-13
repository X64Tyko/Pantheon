import type {
  Channel, CredentialStatus, Episode, Library, LibraryInfo, LibraryWithSource,
  Movie, MovieDetail, PagedResult, Show, ShowDetail, Source, SourceType,
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
  getChannels:      ()                                  => request<Channel[]>('GET',    '/channels'),
  createChannel:    (b: Omit<Channel, 'channel_id'>)    => request<{channel_id: string}>('POST', '/channels', b),
  deleteChannel:    (id: string)                        => request<void>     ('DELETE', `/channels/${id}`),

  // Connection test (no persistence)
  testSource:       (b: {source_type: string, base_url: string, token: string}) =>
                                                        request<{ok: boolean, error?: string}>('POST', '/sources/test', b),

  // Credentials (kairos.conf via API)
  getCredentials:    (id: string)                       => request<CredentialStatus>('GET',    `/config/credentials/${id}`),
  setCredentials:    (id: string, b: {token: string, user_id?: string}) =>
                                                        request<{ok: boolean}>('PUT',    `/config/credentials/${id}`, b),
  deleteCredentials: (id: string)                       => request<{ok: boolean}>('DELETE', `/config/credentials/${id}`),

  // Sync status
  getSyncStatus:    ()                                  => request<{running: boolean}>('GET', '/sync/status'),

  // Content — list
  getAllLibraries: ()                                    => request<LibraryWithSource[]>('GET', '/libraries'),
  getShows:       (p: { limit?: number; offset?: number; library_id?: string } = {}) =>
                    request<PagedResult<Show>>('GET', `/shows?${qs(p)}`),
  getEpisodes:    (showId: string)                      => request<Episode[]>('GET', `/shows/${showId}/episodes`),
  getMovies:      (p: { limit?: number; offset?: number; library_id?: string } = {}) =>
                    request<PagedResult<Movie>>('GET', `/movies?${qs(p)}`),

  // Content — detail + update
  getShow:        (id: string)                          => request<ShowDetail>('GET',   `/shows/${id}`),
  updateShow:     (id: string, b: Partial<ShowDetail>)  => request<void>      ('PATCH', `/shows/${id}`, b),
  getMovie:       (id: string)                          => request<MovieDetail>('GET',  `/movies/${id}`),
  updateMovie:    (id: string, b: Partial<MovieDetail>) => request<void>       ('PATCH',`/movies/${id}`, b),
}
