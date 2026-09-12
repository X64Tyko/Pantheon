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
