import { Outlet } from 'react-router-dom'

// Outermost /tv element. No admin sidebar/chrome — mirrors /player/*'s
// full-screen takeover. The X-Pantheon-Surface: tv header (which Kairos uses
// to downgrade admin->viewer, see api/client.ts) is derived from the current
// path there, not toggled by this component.
export function TvShell() {
  return (
    <div style={{
      position: 'fixed', inset: 0, background: 'var(--hds-bg)',
      overflow: 'hidden', display: 'flex', flexDirection: 'column',
    }}>
      <Outlet />
    </div>
  )
}
