import { observer } from 'mobx-react-lite'
import { useEffect, useRef, useState } from 'react'
import { NavLink, Outlet, useLocation, useNavigate } from 'react-router-dom'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useFocusable } from '../nav/useFocusable'
import { useAuth } from '../auth/AuthContext'
import { api } from '../api/client'
import type { UnmappedSourceUser } from '../api/types'
import { systemStore, statusStore } from '../stores'
import { tourStore } from '../stores/TourStore'
import { TourPill } from './tour/TourPill'
import { SIDEBAR_FOCUS_KEY } from '../nav/back'

const navItems: { to: string; label: string; icon: React.ReactNode; adminOnly?: boolean }[] = [
  { to: '/',          label: 'Home',      icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M2 7.5L8 2l6 5.5V14H10v-3.5H6V14H2V7.5z" strokeLinejoin="round"/></svg> },
  { to: '/library',   label: 'Library',   icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="2" y="2" width="5" height="5" rx="1"/><rect x="9" y="2" width="5" height="5" rx="1"/><rect x="2" y="9" width="5" height="5" rx="1"/><rect x="9" y="9" width="5" height="5" rx="1"/></svg> },
  { to: '/sources',   label: 'Sources',   icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/></svg>, adminOnly: true },
  { to: '/channels',  label: 'Channels',  icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><rect x="4" y="4" width="8" height="8" rx="1.5" transform="rotate(45 8 8)"/></svg>, adminOnly: true },
  { to: '/playlists', label: 'Playlists', icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><path d="M6 5.5l4 2.5-4 2.5V5.5z" fill="currentColor" stroke="none"/></svg>, adminOnly: true },
  // /filler is intentionally omitted from nav — accessed via channel context (ChannelFillerOverlay)
  { to: '/downloads', label: 'Downloads', icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><path d="M8 2v8M5 7l3 3 3-3" strokeLinecap="round" strokeLinejoin="round"/><path d="M3 12h10" strokeLinecap="round"/></svg>, adminOnly: true },
  { to: '/activity',  label: 'Activity',  icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><circle cx="8" cy="8" r="2"/></svg>, adminOnly: true },
  { to: '/review',    label: 'Review',    icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="5.5"/><path d="M5.5 8l2 2 3-3" strokeLinecap="round" strokeLinejoin="round"/></svg>, adminOnly: true },
  { to: '/settings',  label: 'Settings',  icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="8" cy="8" r="2.2"/><path d="M8 1.5v1.3M8 13.2v1.3M1.5 8h1.3M13.2 8h1.3M3.4 3.4l.9.9M11.7 11.7l.9.9M3.4 12.6l.9-.9M11.7 4.3l.9-.9" strokeLinecap="round"/></svg>, adminOnly: true },
  { to: '/users',     label: 'Users',     icon: <svg width="15" height="15" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.4"><circle cx="6" cy="5.5" r="2.2"/><path d="M1.5 13c0-2.5 2-4 4.5-4s4.5 1.5 4.5 4" strokeLinecap="round"/><circle cx="12" cy="5.5" r="1.6"/><path d="M14.5 12c0-1.8-1.1-3-2.5-3" strokeLinecap="round"/></svg>, adminOnly: true },
]

export default observer(function Layout() {
  const location     = useLocation()
  const navigate     = useNavigate()
  const { user, logout } = useAuth()
  const isChannelDetail = /^\/channels\/.+/.test(location.pathname)
  const isFullBleed     = isChannelDetail || location.pathname === '/' || location.pathname.startsWith('/library') || location.pathname.startsWith('/review') || location.pathname === '/activity'
  const onActivity      = location.pathname === '/activity'

  const [navCollapsed,     setNavCollapsed]     = useState(() => localStorage.getItem('hds-nav-collapsed') === '1')
  const [expandBtnHover,   setExpandBtnHover]   = useState(false)
  const [collapseBtnHover, setCollapseBtnHover] = useState(false)
  const [pendingRequests,  setPendingRequests]  = useState(0)
  // Mobile-only slide-in drawer (see hds-mobile-topbar/-drawer in index.css)
  // — the desktop sidebar itself just gets CSS-hidden below the breakpoint,
  // this is a separate, simpler nav surface rather than trying to force one
  // DOM structure to reflow between a vertical rail and a top bar + drawer.
  const [mobileMenuOpen,   setMobileMenuOpen]   = useState(false)

  // Closing the drawer on route change (rather than only on item click) also
  // covers back/forward navigation and any other route change.
  useEffect(() => { setMobileMenuOpen(false) }, [location.pathname])

  const toggleNav = () => setNavCollapsed(c => {
    const next = !c
    localStorage.setItem('hds-nav-collapsed', next ? '1' : '0')
    return next
  })

  // D-pad/keyboard nav auto-manages the sidebar: minimized while focus is in
  // page content, expanded when it returns to a sidebar item — standard
  // 10-foot-UI rail behavior. hasFocusedChild starts false and only becomes
  // true once a real focus event lands on a sidebar item, so this never
  // fires for mouse-only users and never overrides their manually-persisted
  // (localStorage) preference on a fresh page load. Deliberately doesn't
  // touch localStorage itself: the stored preference should keep reflecting
  // the last *manual* toggle, not this transient, focus-driven state.
  // isFocusBoundary contains up/down within the sidebar's own vertical list
  // (never escapes to page content that way) while leaving left/right open
  // so content can still hand off into/out of the sidebar at its edges.
  const { ref: sidebarRef, focusKey: sidebarFocusKey, hasFocusedChild } = useFocusable({
    focusKey: SIDEBAR_FOCUS_KEY,
    trackChildren: true,
    isFocusBoundary: true,
    focusBoundaryDirections: ['up', 'down'],
    saveLastFocusedChild: true,
  })
  // Only reacts to real transitions once the sidebar has genuinely had focus
  // at least once — hasFocusedChild starts false on mount regardless of
  // input method, so collapsing on that initial false would fight a mouse-
  // only user's manually-persisted preference before they've touched a key.
  const sidebarEverFocusedRef = useRef(false)
  useEffect(() => {
    if (hasFocusedChild) {
      sidebarEverFocusedRef.current = true
      setNavCollapsed(false)
    } else if (sidebarEverFocusedRef.current) {
      setNavCollapsed(true)
    }
  }, [hasFocusedChild])

  // Fetch pending request count for admin badge.
  useEffect(() => {
    if (user?.role !== 'admin') return
    api.getRequests().then(reqs => setPendingRequests(reqs.filter(r => r.status === 'pending').length)).catch(() => {})
  }, [user?.role, location.pathname])

  // Refresh setup-tour progress as the admin navigates — cheap GETs, only
  // fires on route change (not a polling loop), and refreshCompletion()
  // itself no-ops once the tour's been dismissed.
  useEffect(() => {
    if (user?.role !== 'admin') return
    tourStore.refreshCompletion()
  }, [user?.role, location.pathname])

  // Connect the global SSE log stream once and keep it alive.
  useEffect(() => { systemStore.connectLogs() }, [])

  // Status polling lifecycle — pause when tab is hidden.
  useEffect(() => {
    const onVisibility = () => {
      if (document.visibilityState === 'hidden') {
        statusStore.stopPolling()
      } else {
        statusStore.startPolling()
      }
    }
    document.addEventListener('visibilitychange', onVisibility)
    statusStore.startPolling()
    return () => {
      document.removeEventListener('visibilitychange', onVisibility)
      statusStore.stopPolling()
    }
  }, [])

  // Clear unread-error badge while the user is on the Activity page.
  useEffect(() => {
    if (onActivity) systemStore.clearUnreadErrors()
  }, [onActivity])

  // Poll for source-reported accounts with no local Pantheon account yet —
  // admin-only, same gating as the pending-requests fetch above. Runs
  // independent of which page the admin is on, so the corner notice can
  // surface even without visiting Sources.
  useEffect(() => {
    if (user?.role !== 'admin') { systemStore.stopUnmappedUsersPolling(); return }
    systemStore.startUnmappedUsersPolling()
    return () => systemStore.stopUnmappedUsersPolling()
  }, [user?.role])

  const hasErrors = systemStore.unreadErrors > 0
  const col = navCollapsed

  return (
    <div className="hds-app-shell" style={{ display: 'flex', height: '100vh', overflow: 'hidden', background: 'var(--hds-bg)' }}>
      {/* ── Sidebar (desktop) ───────────────────────────────────────────────── */}
      <nav ref={sidebarRef} className="hds-sidebar-desktop" style={{
        width: col ? 52 : 236, flexShrink: 0, display: 'flex', flexDirection: 'column',
        borderRight: '1px solid var(--hds-line-s)',
        background: 'linear-gradient(180deg, oklch(0.17 0.018 286), var(--hds-bg))',
        padding: col ? '22px 8px' : '22px 18px',
        transition: 'width .18s cubic-bezier(0.4,0,0.2,1), padding .18s cubic-bezier(0.4,0,0.2,1)',
        overflow: 'hidden',
      }}>
        {/* Brand + collapse toggle */}
        <div style={{ marginBottom: 30 }}>
          {col ? (
            <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 8 }}>
              <div style={{
                fontFamily: "'Chakra Petch', sans-serif", fontWeight: 800,
                fontSize: 15, color: 'var(--hds-gold)',
                textShadow: '0 0 16px oklch(0.83 0.13 84 / 0.4)',
              }}>H</div>
              <button
                onClick={toggleNav}
                title="Expand nav"
                onMouseEnter={() => setExpandBtnHover(true)}
                onMouseLeave={() => setExpandBtnHover(false)}
                style={{
                  display: 'flex', alignItems: 'center', justifyContent: 'center',
                  width: 28, height: 28, borderRadius: 7,
                  border: `1px solid ${expandBtnHover ? 'var(--hds-line)' : 'var(--hds-line-s)'}`,
                  background: expandBtnHover ? 'var(--hds-bg-3)' : 'transparent',
                  cursor: 'pointer',
                  color: expandBtnHover ? 'var(--hds-txt)' : 'var(--hds-txt-3)',
                  transition: 'border-color .12s, color .12s, background .12s',
                }}
              >
                <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round">
                  <path d="M4 2l3 3.5-3 3.5" />
                </svg>
              </button>
            </div>
          ) : (
            <div style={{ display: 'flex', alignItems: 'flex-start', justifyContent: 'space-between' }}>
              <div>
                <div style={{
                  fontFamily: "'Chakra Petch', sans-serif", fontWeight: 800,
                  fontSize: 24, letterSpacing: '0.32em', color: 'var(--hds-gold)',
                  textShadow: '0 0 22px oklch(0.83 0.13 84 / 0.35)',
                }}>HADES</div>
                <div style={{ fontSize: 9.5, letterSpacing: '0.42em', color: 'var(--hds-violet)', marginTop: 5, opacity: 0.85 }}>
                  KAIROS ENGINE
                </div>
              </div>
              <button
                onClick={toggleNav}
                title="Collapse nav"
                onMouseEnter={() => setCollapseBtnHover(true)}
                onMouseLeave={() => setCollapseBtnHover(false)}
                style={{
                  display: 'flex', alignItems: 'center', justifyContent: 'center',
                  width: 26, height: 26, marginTop: 2, borderRadius: 7,
                  border: `1px solid ${collapseBtnHover ? 'var(--hds-line-s)' : 'transparent'}`,
                  background: collapseBtnHover ? 'var(--hds-bg-3)' : 'transparent',
                  cursor: 'pointer',
                  color: collapseBtnHover ? 'var(--hds-txt)' : 'var(--hds-txt-3)',
                  flexShrink: 0,
                  transition: 'border-color .12s, color .12s, background .12s',
                }}
              >
                <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round">
                  <path d="M7 2L4 5.5 7 9" />
                </svg>
              </button>
            </div>
          )}
        </div>

        {/* Nav */}
        <FocusContext.Provider value={sidebarFocusKey}>
          <div style={{ display: 'flex', flexDirection: 'column', gap: 3, flex: 1 }}>
            {navItems.filter(item => !item.adminOnly || user?.role === 'admin').map(({ to, label, icon }) => (
              <SidebarNavItem
                key={to} to={to} label={label} icon={icon} col={col}
                pendingRequests={to === '/review' ? pendingRequests : 0}
                hasErrors={to === '/activity' && hasErrors}
                unreadErrors={systemStore.unreadErrors}
                anyRunning={to === '/activity' && statusStore.anyRunning}
              />
            ))}
          </div>
        </FocusContext.Provider>

        {/* Status footer */}
        <div style={{ paddingTop: 14, borderTop: '1px solid var(--hds-line-s)', display: 'flex', flexDirection: 'column', gap: 8 }}>
          {col ? (
            <div style={{ display: 'flex', flexDirection: 'column', alignItems: 'center', gap: 7 }}>
              {statusStore.syncing && (
                <ProcessSpinner color="var(--hds-violet)" title="Syncing" />
              )}
              {statusStore.matching && (
                <ProcessSpinner color="var(--hds-gold)" title="Matching" />
              )}
              <span style={{
                width: 7, height: 7, borderRadius: '50%',
                background: 'oklch(0.7 0.16 150)',
                boxShadow: '0 0 8px oklch(0.7 0.16 150)',
                animation: 'hds-pulse 2.6s ease-in-out infinite',
              }} />
            </div>
          ) : (
            <>
              {statusStore.syncing && (
                <ProcessRow color="var(--hds-violet)" label="Syncing" />
              )}
              {statusStore.matching && (
                <ProcessRow color="var(--hds-gold)" label="Matching" />
              )}
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
            </>
          )}
        </div>
      </nav>

      {/* ── Top bar (mobile only — hidden on desktop via CSS) ──────────────── */}
      <div className="hds-mobile-topbar">
        <button
          onClick={() => setMobileMenuOpen(o => !o)}
          aria-label={mobileMenuOpen ? 'Close menu' : 'Open menu'}
          style={{
            display: 'flex', alignItems: 'center', justifyContent: 'center',
            width: 36, height: 36, borderRadius: 8, flexShrink: 0,
            border: '1px solid var(--hds-line-s)', background: 'var(--hds-bg-3)',
            color: 'var(--hds-txt)', cursor: 'pointer',
          }}
        >
          {mobileMenuOpen ? (
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"><path d="M3 3l10 10M13 3L3 13" /></svg>
          ) : (
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"><path d="M2 4h12M2 8h12M2 12h12" /></svg>
          )}
        </button>
        <div style={{
          fontFamily: "'Chakra Petch', sans-serif", fontWeight: 800,
          fontSize: 17, letterSpacing: '0.24em', color: 'var(--hds-gold)',
          textShadow: '0 0 16px oklch(0.83 0.13 84 / 0.35)',
        }}>HADES</div>
      </div>

      {/* ── Slide-in drawer (mobile only) — same navItems list as the desktop
          sidebar, but a separate, D-pad-nav-framework-free component: reusing
          SidebarNavItem here (it registers a useFocusable with a focusKey per
          route) would collide with the desktop sidebar's own copies of those
          same focusKeys, since CSS-hiding the sidebar on mobile doesn't unmount
          it. Touch UI has no use for D-pad focus registration anyway. ── */}
      {mobileMenuOpen && (
        <div
          className="hds-mobile-drawer-backdrop"
          onClick={() => setMobileMenuOpen(false)}
          style={{ position: 'fixed', inset: 0, zIndex: 200, background: 'oklch(0.08 0.015 286 / 0.75)' }}
        >
          <div
            onClick={e => e.stopPropagation()}
            style={{
              position: 'absolute', top: 0, left: 0, bottom: 0, width: 'min(78vw, 300px)',
              background: 'linear-gradient(180deg, oklch(0.17 0.018 286), var(--hds-bg))',
              borderRight: '1px solid var(--hds-line-s)',
              padding: '22px 14px', display: 'flex', flexDirection: 'column', gap: 3,
              overflowY: 'auto',
            }}
          >
            {navItems.filter(item => !item.adminOnly || user?.role === 'admin').map(({ to, label, icon }) => (
              <MobileNavLink key={to} to={to} label={label} icon={icon} />
            ))}
            {user && (
              <div style={{
                marginTop: 'auto', paddingTop: 14, borderTop: '1px solid var(--hds-line-s)',
                display: 'flex', alignItems: 'center', justifyContent: 'space-between', gap: 8,
              }}>
                <span style={{ fontSize: 12, color: 'var(--hds-txt-3)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                  {user.username}
                  {user.role === 'admin' && <span style={{ marginLeft: 5, color: 'var(--hds-gold)', opacity: 0.7, fontSize: 10 }}>ADMIN</span>}
                </span>
                <button
                  onClick={() => { logout().then(() => navigate('/login')) }}
                  style={{
                    background: 'none', border: '1px solid var(--hds-line)', borderRadius: 6,
                    cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 11, padding: '4px 10px',
                    fontFamily: "'JetBrains Mono', monospace",
                  }}
                >Sign out</button>
              </div>
            )}
          </div>
        </div>
      )}

      {/* ── Main content ────────────────────────────────────────────────────── */}
      <main className="hds-main-content" style={{
        flex: 1, minWidth: 0, display: 'flex', flexDirection: 'column',
        overflow: 'hidden',
        fontFamily: "'JetBrains Mono', monospace",
        fontSize: 13,
        color: 'var(--hds-txt)',
      }}>
        {/* Debug banner — shown when any debug logging flag is active */}
        {statusStore.anyDebugEnabled && (
          <DebugBanner
            syncDebug={statusStore.syncDebug}
            epgDebug={statusStore.epgDebug}
            onSettings={() => navigate('/settings')}
          />
        )}

        <div style={{
          flex: 1, minWidth: 0,
          overflow: isFullBleed ? 'hidden' : 'auto',
          padding: isFullBleed ? 0 : 24,
        }}
          className={isFullBleed ? '' : 'scrollbar-dark'}
        >
          <Outlet />
        </div>
      </main>

      {/* ── First-time setup tour pill ──────────────────────────────────────── */}
      {user?.role === 'admin' && <TourPill />}

      {/* ── Error toast ─────────────────────────────────────────────────────── */}
      {systemStore.toast && (
        <ErrorToast
          toast={systemStore.toast}
          onViewLogs={() => { systemStore.dismissToast(); navigate('/activity') }}
          onDismiss={() => systemStore.dismissToast()}
        />
      )}

      {/* ── Unregistered source users notice ────────────────────────────────── */}
      {user?.role === 'admin' && !systemStore.unmappedUsersDismissed && systemStore.unmappedSourceUsers.length > 0 && (
        <UnmappedUsersNotice
          users={systemStore.unmappedSourceUsers}
          onReview={() => { systemStore.dismissUnmappedUsersNotice(); navigate('/sources') }}
          onDismiss={() => systemStore.dismissUnmappedUsersNotice()}
        />
      )}
    </div>
  )
})

// Plain NavLink, no useFocusable — the mobile drawer is touch-only, and
// registering these with the D-pad nav framework would collide with the
// desktop sidebar's own focusKeys for the same routes (see the drawer's own
// comment above for why that matters: CSS-hiding the sidebar on mobile
// doesn't unmount it, so both would be registered simultaneously).
function MobileNavLink({ to, label, icon }: { to: string; label: string; icon: React.ReactNode }) {
  return (
    <NavLink to={to} style={{ textDecoration: 'none' }}>
      {({ isActive }) => (
        <div style={{
          display: 'flex', alignItems: 'center', gap: 13,
          padding: '13px 12px', borderRadius: 9,
          background: isActive ? 'var(--hds-bg-3)' : 'transparent',
          color: isActive ? 'var(--hds-gold)' : 'var(--hds-txt-2)',
          fontWeight: isActive ? 600 : 400,
          fontFamily: "'JetBrains Mono', monospace", fontSize: 14,
        }}>
          <span style={{ color: isActive ? 'var(--hds-gold)' : 'var(--hds-txt-3)', flexShrink: 0 }}>{icon}</span>
          {label}
        </div>
      )}
    </NavLink>
  )
}

// Sidebar nav is persistent chrome present on every non-full-screen page —
// wiring it into the nav framework (not just Guide/Player) is what gives
// D-pad users an escape route out of any page-local focus island (e.g.
// pressing left at the Guide's leftmost column) rather than a dead end.
function SidebarNavItem({ to, label, icon, col, pendingRequests, hasErrors, unreadErrors, anyRunning }: {
  to: string; label: string; icon: React.ReactNode; col: boolean
  pendingRequests: number; hasErrors: boolean; unreadErrors: number; anyRunning: boolean
}) {
  const navigate = useNavigate()
  // Enter/select needs an explicit navigate() call — the synthetic activate()
  // path doesn't trigger NavLink's own native click-based navigation.
  //
  // forceFocus on Home specifically: the library never auto-focuses anything
  // on mount — smartNavigate() silently no-ops on every arrow press until
  // something is focused at least once. forceFocus is its own fallback
  // mechanism for exactly this ("preferred fallback when focus is lost"),
  // re-evaluated live on every navigation attempt rather than needing one
  // well-timed imperative setFocus() call at just the right point in the
  // route tree (which routes like /player/* that mount outside <Layout>
  // would miss entirely).
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `sidebar-nav-${to}`,
    onEnterPress: () => navigate(to),
    forceFocus: to === '/',
  })

  return (
    <NavLink to={to} title={col ? label : undefined} style={{ textDecoration: 'none' }}>
      {({ isActive }) => (
        <div
          ref={ref} data-tv-focused={focused}
          style={{
            position: 'relative', display: 'flex', alignItems: 'center',
            gap: col ? 0 : 11,
            justifyContent: col ? 'center' : 'flex-start',
            padding: col ? '10px 0' : '10px 12px', borderRadius: 9,
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
          {!col && <span>{label}</span>}

          {/* Pending requests badge on Review (admin) */}
          {pendingRequests > 0 && (
            <span style={{
              marginLeft: col ? undefined : 'auto',
              position: col ? 'absolute' : undefined,
              top: col ? 4 : undefined, right: col ? 4 : undefined,
              minWidth: 16, height: 16, borderRadius: 8, padding: '0 4px',
              background: 'oklch(0.75 0.12 80)',
              color: 'oklch(0.15 0.02 80)',
              fontSize: 9, fontWeight: 700,
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              letterSpacing: 0,
            }}>
              {pendingRequests > 99 ? '99+' : pendingRequests}
            </span>
          )}

          {/* Activity indicator: red error badge > purple sync dot */}
          {hasErrors ? (
            <span style={{
              marginLeft: col ? undefined : 'auto',
              position: col ? 'absolute' : undefined,
              top: col ? 4 : undefined, right: col ? 4 : undefined,
              minWidth: 16, height: 16, borderRadius: 8,
              padding: '0 4px',
              background: 'oklch(0.55 0.22 22)',
              color: 'oklch(1 0 0)',
              fontSize: 9, fontWeight: 700,
              display: 'flex', alignItems: 'center', justifyContent: 'center',
              boxShadow: '0 0 10px oklch(0.55 0.22 22 / 0.7)',
              animation: 'hds-pulse 2s ease-in-out infinite',
              letterSpacing: 0,
            }}>
              {unreadErrors > 99 ? '99+' : unreadErrors}
            </span>
          ) : anyRunning ? (
            <span style={{
              marginLeft: col ? undefined : 'auto',
              position: col ? 'absolute' : undefined,
              top: col ? 6 : undefined, right: col ? 6 : undefined,
              width: 6, height: 6, borderRadius: '50%',
              background: 'var(--hds-violet)',
              animation: 'hds-pulse 2.6s ease-in-out infinite',
            }} />
          ) : null}
        </div>
      )}
    </NavLink>
  )
}

// ── Process status components ─────────────────────────────────────────────────

function ProcessSpinner({ color, title }: { color: string; title: string }) {
  return (
    <svg width="13" height="13" viewBox="0 0 13 13" fill="none"
         style={{ color, flexShrink: 0, animation: 'spin 0.9s linear infinite' }}>
      <title>{title}</title>
      <circle cx="6.5" cy="6.5" r="4.5" stroke="currentColor" strokeWidth="1.6"
              strokeDasharray="18 10" strokeLinecap="round" />
    </svg>
  )
}

function ProcessRow({ color, label }: { color: string; label: string }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, color }}>
      <ProcessSpinner color={color} title={label} />
      <span style={{
        fontSize: 10, fontFamily: "'JetBrains Mono', monospace",
        letterSpacing: '0.08em', whiteSpace: 'nowrap',
      }}>
        {label}
      </span>
    </div>
  )
}

// ── Debug banner ──────────────────────────────────────────────────────────────

function DebugBanner({ syncDebug, epgDebug, onSettings }: {
  syncDebug:  boolean
  epgDebug:   boolean
  onSettings: () => void
}) {
  const modes = [
    syncDebug && 'sync',
    epgDebug  && 'epg',
  ].filter(Boolean).join(' + ')

  return (
    <div style={{
      display: 'flex', alignItems: 'center', gap: 10,
      padding: '0 16px',
      height: 32, flexShrink: 0,
      background: 'oklch(0.22 0.08 60 / 0.55)',
      borderBottom: '1px solid oklch(0.55 0.18 60 / 0.5)',
      fontSize: 10, letterSpacing: '0.06em',
      color: 'oklch(0.88 0.16 70)',
      fontFamily: "'JetBrains Mono', monospace",
    }}>
      <span style={{
        width: 6, height: 6, borderRadius: '50%', flexShrink: 0,
        background: 'oklch(0.75 0.2 70)',
        boxShadow: '0 0 8px oklch(0.75 0.2 70 / 0.8)',
        animation: 'hds-pulse 2s ease-in-out infinite',
      }} />
      <span style={{ flex: 1 }}>
        DEBUG LOGGING ACTIVE
        <span style={{ color: 'oklch(0.65 0.12 70)', marginLeft: 6 }}>({modes})</span>
        <span style={{ color: 'oklch(0.55 0.08 70)', marginLeft: 8 }}>
          — verbose output is being written to logs
        </span>
      </span>
      <button
        onClick={onSettings}
        style={{
          background: 'none', border: '1px solid oklch(0.45 0.1 60 / 0.6)',
          borderRadius: 5, padding: '2px 10px',
          color: 'oklch(0.75 0.14 70)', fontSize: 10,
          cursor: 'pointer', fontFamily: "'JetBrains Mono', monospace",
          letterSpacing: '0.04em',
          transition: 'border-color .12s, color .12s',
        }}
      >
        Settings
      </button>
    </div>
  )
}

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

// Same fixed-card shape as ErrorToast, but bottom-left (so it never collides
// with an error toast that shows up at the same time), neutral gold styling
// instead of the error-red treatment, and no auto-dismiss timer — this
// persists until the admin dismisses it or the list resolves down to zero
// (imported users drop off automatically; see SourceRepository::listUnmappedSourceUsers).
function UnmappedUsersNotice({
  users, onReview, onDismiss,
}: {
  users:     UnmappedSourceUser[]
  onReview:  () => void
  onDismiss: () => void
}) {
  return (
    <div style={{
      position: 'fixed', bottom: 24, left: 24, zIndex: 9999,
      width: 360, maxWidth: 'calc(100vw - 48px)',
      background: 'oklch(0.16 0.022 286)',
      border: '1px solid oklch(0.5 0.1 84 / 0.5)',
      borderLeft: '4px solid var(--hds-gold)',
      borderRadius: 10,
      boxShadow: '0 8px 32px -4px rgba(0,0,0,0.7), 0 0 0 1px oklch(0.83 0.13 84 / 0.12)',
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
          background: 'var(--hds-gold)',
        }} />
        <span style={{ fontSize: 10, letterSpacing: '0.18em', color: 'var(--hds-gold-2)', fontWeight: 700, flex: 1 }}>
          UNREGISTERED USERS
        </span>
        <button
          onClick={onDismiss}
          style={{ background: 'none', border: 'none', cursor: 'pointer', color: 'var(--hds-txt-3)', fontSize: 14, lineHeight: 1, padding: '0 2px' }}
          title="Dismiss"
        >✕</button>
      </div>

      {/* Message */}
      <div style={{ padding: '10px 14px 4px', fontSize: 12, color: 'var(--hds-txt-2)', lineHeight: 1.55 }}>
        Your sources have {users.length} user{users.length === 1 ? '' : 's'} not registered here:
      </div>
      <div style={{ padding: '4px 14px 8px', display: 'flex', flexDirection: 'column', gap: 2, maxHeight: 120, overflowY: 'auto' }}>
        {users.slice(0, 8).map(u => (
          <div key={`${u.source_id}:${u.external_user_id}`} style={{ fontSize: 11.5, color: 'var(--hds-txt)', display: 'flex', justifyContent: 'space-between', gap: 8 }}>
            <span style={{ overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{u.display_name || u.external_user_id}</span>
            <span style={{ color: 'var(--hds-txt-3)', flexShrink: 0 }}>{u.source_display_name}</span>
          </div>
        ))}
        {users.length > 8 && (
          <div style={{ fontSize: 11, color: 'var(--hds-txt-3)' }}>+{users.length - 8} more</div>
        )}
      </div>

      {/* Footer */}
      <div style={{ padding: '4px 12px 12px', display: 'flex', gap: 8 }}>
        <button
          onClick={onReview}
          style={{
            flex: 1, padding: '7px 0',
            background: 'oklch(0.83 0.13 84 / 0.16)',
            border: '1px solid oklch(0.83 0.13 84 / 0.45)',
            borderRadius: 7,
            color: 'var(--hds-gold-2)',
            fontSize: 11, fontWeight: 600, cursor: 'pointer',
            fontFamily: "'JetBrains Mono', monospace",
            letterSpacing: '0.06em',
            transition: 'background .12s',
          }}
        >
          Review in Sources →
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
