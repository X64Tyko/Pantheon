import { observer } from 'mobx-react-lite'
import { useEffect } from 'react'
import { NavLink, Outlet, useLocation } from 'react-router-dom'
import { systemStore } from '../stores'

const navItems = [
  { to: '/sources',   label: 'Sources',      icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/></svg> },
  { to: '/channels',  label: 'Channels',     icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="4" y="4" width="8" height="8" rx="1.5" transform="rotate(45 8 8)"/></svg> },
  { to: '/content',   label: 'Content',      icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="3" y="3.5" width="10" height="2" rx="1"/><rect x="3" y="7" width="10" height="2" rx="1"/><rect x="3" y="10.5" width="10" height="2" rx="1"/></svg> },
  { to: '/playlists', label: 'Playlists',    icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><path d="M6 5.5l4 2.5-4 2.5V5.5z" fill="currentColor" stroke="none"/></svg> },
  { to: '/filler',    label: 'Filler Lists', icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M3 5h10M3 8h7M3 11h5" strokeLinecap="round"/></svg> },
  { to: '/activity',  label: 'Activity',     icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><circle cx="8" cy="8" r="2"/></svg> },
]

export default observer(function Layout() {
  const location = useLocation()
  const isChannelDetail = /^\/channels\/.+/.test(location.pathname)

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
          {navItems.map(({ to, label, icon }) => (
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
                  {to === '/activity' && systemStore.syncing && (
                    <span style={{ marginLeft: 'auto', width: 6, height: 6, borderRadius: '50%', background: 'var(--hds-violet)', animation: 'hds-pulse 2.6s ease-in-out infinite' }} />
                  )}
                </div>
              )}
            </NavLink>
          ))}
        </div>

        {/* Status footer */}
        <div style={{ paddingTop: 18, borderTop: '1px solid var(--hds-line-s)', display: 'flex', alignItems: 'center', gap: 8 }}>
          <span style={{
            width: 7, height: 7, borderRadius: '50%',
            background: 'oklch(0.7 0.16 150)',
            boxShadow: '0 0 8px oklch(0.7 0.16 150)',
            animation: 'hds-pulse 2.6s ease-in-out infinite',
            flexShrink: 0,
          }} />
          <span style={{ fontSize: 11, color: 'var(--hds-txt-3)', fontFamily: "'JetBrains Mono', monospace" }}>
            v0.1.0 · running
          </span>
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
    </div>
  )
})
