import { observer } from 'mobx-react-lite'
import { useEffect } from 'react'
import { NavLink, Outlet, useLocation, useNavigate } from 'react-router-dom'
import { useAuth } from '../auth/AuthContext'
import { systemStore } from '../stores'

const navItems: { to: string; label: string; icon: React.ReactNode; adminOnly?: boolean }[] = [
  { to: '/sources',   label: 'Sources',      icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/></svg> },
  { to: '/channels',  label: 'Channels',     icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="4" y="4" width="8" height="8" rx="1.5" transform="rotate(45 8 8)"/></svg> },
  { to: '/content',   label: 'Content',      icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="3" y="3.5" width="10" height="2" rx="1"/><rect x="3" y="7" width="10" height="2" rx="1"/><rect x="3" y="10.5" width="10" height="2" rx="1"/></svg> },
  { to: '/groups',    label: 'Groups',       icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M3 4h4v4H3z" rx="0.8"/><path d="M9 4h4v4H9z" rx="0.8"/><path d="M6 8v2M10 8v2M4 12h8" strokeLinecap="round"/></svg> },
  { to: '/playlists', label: 'Playlists',    icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><path d="M6 5.5l4 2.5-4 2.5V5.5z" fill="currentColor" stroke="none"/></svg> },

  { to: '/downloads', label: 'Downloads',    icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M8 2v8M5 7l3 3 3-3" strokeLinecap="round" strokeLinejoin="round"/><path d="M3 12h10" strokeLinecap="round"/></svg> },
  { to: '/activity',  label: 'Activity',     icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><circle cx="8" cy="8" r="2"/></svg> },
  { to: '/settings',  label: 'Settings',     icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="2.2"/><path d="M8 1.5v1.3M8 13.2v1.3M1.5 8h1.3M13.2 8h1.3M3.4 3.4l.9.9M11.7 11.7l.9.9M3.4 12.6l.9-.9M11.7 4.3l.9-.9" strokeLinecap="round"/></svg> },
  { to: '/users',     label: 'Users',        icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="6" cy="5.5" r="2.2"/><path d="M1.5 13c0-2.5 2-4 4.5-4s4.5 1.5 4.5 4" strokeLinecap="round"/><circle cx="12" cy="5.5" r="1.6"/><path d="M14.5 12c0-1.8-1.1-3-2.5-3" strokeLinecap="round"/></svg>, adminOnly: true },
]

export default observer(function Layout() {
  const location     = useLocation()
  const navigate     = useNavigate()
  const { user, logout } = useAuth()
  const isChannelDetail = /^\/channels\/.+/.test(location.pathname)
  const onActivity   = location.pathname === '/activity'

  // Connect the global SSE log stream once and keep it alive.
  useEffect(() => { systemStore.connectLogs() }, [])

  // Sync polling lifecycle — pause when tab is hidden.
  useEffect(() => {
    const onVisibility = () => {
      if (document.visibilityState === 'hidden') {
        systemStore.stopPolling()
      } else {
        systemStore.startPolling()
      }
    }
    document.addEventListener('visibilitychange', onVisibility)
    systemStore.startPolling()
    return () => {
      document.removeEventListener('visibilitychange', onVisibility)
      systemStore.stopPolling()
    }
  }, [])

  // Clear unread-error badge while the user is on the Activity page.
  useEffect(() => {
    if (onActivity) systemStore.clearUnreadErrors()
  }, [onActivity])

  const hasErrors = systemStore.unreadErrors > 0

  return (
    <div style={{ display: 'flex', height: '100vh', overflow: 'hidden', background: 'var(--hds-bg)' }}>
      {/* ── Sidebar ─────────────────────────────────────────────────────────── */}
      <nav style={{
        width: 236, flexShrink: 0, display: 'flex', flexDirection: 'column',
        borderRight: '1px solid var(--hds-line-s)',
        background: 'linear-gradient(180deg, oklch(0.17 0.018 286), var(--hds-bg))',
        padding: '22px 18px',
      }}>
        {/* Brand */}
        <div style={{ marginBottom: 30 }}>
          <div style={{
            fontFamily: "'Chakra Petch', sans-serif", fontWeight: 800,
            fontSize: 24, letterSpacing: '0.32em', color: 'var(--hds-gold)',
            textShadow: '0 0 22px oklch(0.83 0.13 84 / 0.35)',
          }}>HADES</div>
          <div style={{ fontSize: 9.5, letterSpacing: '0.42em', color: 'var(--hds-violet)', marginTop: 5, opacity: 0.85 }}>
            KAIROS ENGINE
          </div>
        </div>

        {/* Nav */}
        <div style={{ display: 'flex', flexDirection: 'column', gap: 3, flex: 1 }}>
          {navItems.filter(item => !item.adminOnly || user?.role === 'admin').map(({ to, label, icon }) => (
            <NavLink key={to} to={to} style={{ textDecoration: 'none' }}>
              {({ isActive }) => (
                <div style={{
                  position: 'relative', display: 'flex', alignItems: 'center', gap: 11,
                  padding: '10px 12px', borderRadius: 9,
                  background: isActive ? 'var(--hds-bg-3)' : 'transparent',
                  color: isActive ? 'var(--hds-gold)' : 'var(--hds-txt-2)',
                  boxShadow: isActive ? 'inset 0 0 0 1px oklch(0.83 0.13 84 / 0.18)' : 'none',
                  fontWeight: isActive ? 600 : 400,
                  cursor: 'pointer',
                  fontFamily: "'JetBrains Mono', monospace",
                  fontSize: 13,
                  transition: 'background .14s, color .14s',
                }}>
                  {isActive && (
                    <span style={{
                      position: 'absolute', left: 0, top: 9, bottom: 9, width: 3,
                      borderRadius: 3, background: 'var(--hds-gold)',
                      boxShadow: '0 0 12px var(--hds-gold)',
                    }} />
                  )}
                  <span style={{ color: isActive ? 'var(--hds-gold)' : 'var(--hds-txt-3)', flexShrink: 0 }}>
                    {icon}
                  </span>
                  <span>{label}</span>

                  {/* Activity indicator: red error badge > purple sync dot */}
                  {to === '/activity' && (
                    hasErrors ? (
                      <span style={{
                        marginLeft: 'auto',
                        minWidth: 18, height: 18, borderRadius: 9,
                        padding: '0 5px',
                        background: 'oklch(0.55 0.22 22)',
                        color: '#fff',
                        fontSize: 9.5, fontWeight: 700,
                        display: 'flex', alignItems: 'center', justifyContent: 'center',
                        boxShadow: '0 0 10px oklch(0.55 0.22 22 / 0.7)',
                        animation: 'hds-pulse 2s ease-in-out infinite',
                        letterSpacing: 0,
                      }}>
                        {systemStore.unreadErrors > 99 ? '99+' : systemStore.unreadErrors}
                      </span>
                    ) : systemStore.syncing ? (
                      <span style={{ marginLeft: 'auto', width: 6, height: 6, borderRadius: '50%', background: 'var(--hds-violet)', animation: 'hds-pulse 2.6s ease-in-out infinite' }} />
                    ) : null
                  )}
                </div>
              )}
            </NavLink>
          ))}
        </div>

        {/* Status footer */}
        <div style={{ paddingTop: 14, borderTop: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', gap: 8 }}>
          <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
            <span style={{
              width: 7, height: 7, borderRadius: '50%', flexShrink: 0,
              background: 'oklch(0.7 0.16 150)',
              boxShadow: '0 0 8px oklch(0.7 0.16 150)',
              animation: 'hds-pulse 2.6s ease-in-out infinite',
            }} />
            <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
              v0.1.0 · running
            </span>
          </div>
          {user && (
            <div style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8 }}>
              <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                {user.username}
                {user.role === 'admin' && <span style={{ marginLeft: 5, color: 'var(--hds-gold)', opacity: 0.7, fontSize: 9 }}>ADMIN</span>}
              </span>
              <button
                onClick={() => { logout().then(() => navigate('/login')) }}
                title="Sign out"
                style={{
                  background: 'none', border: 'none', cursor: 'pointer',
                  color: 'var(--hds-txt-3)', fontSize: 10, padding: '2px 4px',
                  borderRadius: 4, flexShrink: 0,
                  fontFamily: "'JetBrains Mono', monospace",
                  letterSpacing: '0.05em',
                }}
              >
                exit
              </button>
            </div>
          )}
        </div>
      </nav>

      {/* ── Main content ────────────────────────────────────────────────────── */}
      <main style={{
        flex: 1, minWidth: 0,
        overflow: isChannelDetail ? 'hidden' : 'auto',
        padding: isChannelDetail ? 0 : 24,
        fontFamily: "'JetBrains Mono', monospace",
        fontSize: 13,
        color: 'var(--hds-txt)',
      }}
        className={isChannelDetail ? '' : 'scrollbar-dark'}
      >
        <Outlet />
      </main>

      {/* ── Error toast ─────────────────────────────────────────────────────── */}
      {systemStore.toast && (
        <ErrorToast
          toast={systemStore.toast}
          onViewLogs={() => { systemStore.dismissToast(); navigate('/activity') }}
          onDismiss={() => systemStore.dismissToast()}
        />
      )}
    </div>
  )
})

// ── Error toast popup ─────────────────────────────────────────────────────────

function ErrorToast({
  toast, onViewLogs, onDismiss,
}: {
  toast:      { id: number; msg: string; ts: string }
  onViewLogs: () => void
  onDismiss:  () => void
}) {
  return (
    <div style={{
      position: 'fixed', bottom: 24, right: 24, zIndex: 9999,
      width: 360, maxWidth: 'calc(100vw - 48px)',
      background: 'oklch(0.16 0.022 286)',
      border: '1px solid oklch(0.35 0.12 22 / 0.8)',
      borderLeft: '4px solid oklch(0.55 0.22 22)',
      borderRadius: 10,
      boxShadow: '0 8px 32px -4px rgba(0,0,0,0.7), 0 0 0 1px oklch(0.55 0.22 22 / 0.15)',
      fontFamily: "'JetBrains Mono', monospace",
      overflow: 'hidden',
      animation: 'hds-toast-in 0.22s cubic-bezier(0.22,1,0.36,1)',
    }}>
      {/* Header */}
      <div style={{
        display: 'flex', alignItems: 'center', gap: 8,
        padding: '10px 12px 8px',
        borderBottom: '1px solid oklch(0.25 0.02 286)',
      }}>
        <span style={{
          width: 7, height: 7, borderRadius: '50%', flexShrink: 0,
          background: 'oklch(0.62 0.22 22)',
          boxShadow: '0 0 8px oklch(0.62 0.22 22)',
          animation: 'hds-pulse 2s ease-in-out infinite',
        }} />
        <span style={{ fontSize: 10, letterSpacing: '0.18em', color: 'oklch(0.7 0.16 22)', fontWeight: 700, flex: 1 }}>
          ENGINE ERROR
        </span>
        <span style={{ fontSize: 10, color: 'var(--hds-txt-3)' }}>{toast.ts}</span>
        <button
          onClick={onDismiss}
          style={{ background: 'none', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 14, lineHeight: 1, padding: '0 2px' }}
          title="Dismiss"
        >✕</button>
      </div>

      {/* Message */}
      <div style={{ padding: '10px 14px 12px', fontSize: 12, color: 'var(--hds-txt-2)', lineHeight: 1.55, wordBreak: 'break-word' }}>
        {toast.msg || 'An unexpected error occurred in the engine.'}
      </div>

      {/* Footer */}
      <div style={{ padding: '0 12px 12px', display: 'flex', gap: 8 }}>
        <button
          onClick={onViewLogs}
          style={{
            flex: 1, padding: '7px 0',
            background: 'oklch(0.55 0.22 22 / 0.18)',
            border: '1px solid oklch(0.55 0.22 22 / 0.5)',
            borderRadius: 7,
            color: 'oklch(0.78 0.16 22)',
            fontSize: 11, fontWeight: 600, cursor: 'pointer',
            fontFamily: "'JetBrains Mono', monospace",
            letterSpacing: '0.06em',
            transition: 'background .12s',
          }}
        >
          View Logs →
        </button>
        <button
          onClick={onDismiss}
          style={{
            padding: '7px 14px',
            background: 'transparent',
            border: '1px solid var(--hds-line)',
            borderRadius: 7,
            color: 'var(--hds-txt-3)',
            fontSize: 11, cursor: 'pointer',
            fontFamily: "'JetBrains Mono', monospace",
          }}
        >
          Dismiss
        </button>
      </div>
    </div>
  )
}
