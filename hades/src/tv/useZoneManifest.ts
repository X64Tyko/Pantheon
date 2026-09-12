import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { TvZone } from '../api/types'

export interface ZoneManifestState {
  zones:   TvZone[]
  loading: boolean
  hasZone: (id: string) => boolean
  zone:    (id: string) => TvZone | undefined
}

// Library/Detail/Guide counterpart of useHomeManifest.ts — same GET
// /api/tv/manifest fetch, just reading a different screen's zone list.
// Zones never carry behavior (see TvManifestService.cpp's own comment) —
// this only tells a screen which named zones are present and in what order;
// each zone's actual behavior is a fixed native/web component fed by the
// standard REST API, same as Home's rows.
export function useZoneManifest(screen: 'library' | 'detail' | 'guide'): ZoneManifestState {
  const [zones, setZones] = useState<TvZone[]>([])
  const [loading, setLoading] = useState(true)

  useEffect(() => {
    api.getTvManifest()
      .then(m => setZones(m[screen].zones.slice().sort((a, b) => a.order - b.order)))
      .catch(() => setZones([]))
      .finally(() => setLoading(false))
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [screen])

  const hasZone = (id: string) => zones.some(z => z.id === id)
  const zone = (id: string) => zones.find(z => z.id === id)

  return { zones, loading, hasZone, zone }
}
