import { useEffect } from 'react'
import { Outlet } from 'react-router-dom'
import { setSurface } from '../api/client'

// Outermost /tv element. No admin sidebar/chrome — mirrors /player/*'s
// full-screen takeover. Scopes the X-Pantheon-Surface: tv header to exactly
// the time spent under this route (Kairos downgrades admin->viewer on that
// header; leaving it set outside /tv would silently strip admin access on
// every other page in the same tab).
export function TvShell() {
  useEffect(() => {
    setSurface('tv')
    return () => setSurface(null)
  }, [])

  return (
    <div style={{
      position: 'fixed', inset: 0, background: 'var(--hds-bg)',
      overflow: 'hidden', display: 'flex', flexDirection: 'column',
    }}>
      <Outlet />
    </div>
  )
}
