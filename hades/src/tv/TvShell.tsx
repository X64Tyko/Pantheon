import { Outlet } from 'react-router-dom'
import styles from './TvShell.module.css'

// Outermost /tv element. No admin sidebar/chrome — mirrors /player/*'s
// full-screen takeover. The X-Pantheon-Surface: tv header (which Kairos uses
// to downgrade admin->viewer, see api/client.ts) is derived from the current
// path there, not toggled by this component.
export function TvShell() {
  return (
    <div className={styles.shell}>
      <Outlet />
    </div>
  )
}
