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

export interface Channel {
  channel_id: string
  name:       string
  number:     number
  timezone:   string
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
