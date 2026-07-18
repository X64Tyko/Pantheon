import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { TvHomeRow } from '../api/types'

export interface HomeManifestState {
  rows:    TvHomeRow[]
  loading: boolean
}

// Fetches GET /api/tv/manifest once on mount and exposes just Home's row
// list, already sorted by the server (row_order) — TvHome.tsx renders in
// this order rather than a hardcoded JSX sequence. Library/Detail/Guide's
// zones live in the same response but aren't this hook's concern.
export function useHomeManifest(): HomeManifestState {
  const [rows, setRows] = useState<TvHomeRow[]>([])
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    api.getTvManifest()
      .then(m => setRows(m.home.rows))
      .catch(() => setRows([]))
      .finally(() => setLoading(false))
  }, [])

  return { rows, loading }
}

// A manifest dataSource's params travel over the wire in the same snake_case
// shape Kairos' query params use (e.g. hide_empty) — api.getShows/getMovies
// take camelCase (hideEmpty). This is the one seam between the two; every
// other field name already matches (limit/sort/home/filter/q/...). Exported
// so callers can pass a row's own dataSource.params straight into the
// already-typed api.getShows/getMovies rather than going through a
// polymorphic (and therefore untyped) fetch-by-endpoint-string function.
export function toClientParams(params: Record<string, unknown> = {}): Record<string, unknown> {
  const { hide_empty, ...rest } = params
  return hide_empty === undefined ? rest : { ...rest, hideEmpty: hide_empty }
}
