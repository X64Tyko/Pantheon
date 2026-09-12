import { Outlet } from 'react-router-dom'
import { useTvBackGuard } from '../nav/back'
import styles from './TvShell.module.css'

// Outermost /tv element. No admin sidebar/chrome — mirrors /player/*'s
// full-screen takeover. The X-Pantheon-Surface: tv header (which Kairos uses
// to downgrade admin->viewer, see api/client.ts) is derived from the current
// path there, not toggled by this component.
export function TvShell() {
  // Traps a Cast/TV remote's hardware Back button so it drives the same
  // in-app back logic (close overlay / Detail->Library->Home) instead of
  // exiting the whole Cast session — see back.ts's own comment for why this
  // is needed at all (a plain Escape/Backspace keydown listener isn't
  // enough for a hardware remote). Installed once for the whole /tv tree,
  // not per-screen.
  useTvBackGuard()

  return (
    <div className={styles.shell}>
      <Outlet />
    </div>
  )
}
