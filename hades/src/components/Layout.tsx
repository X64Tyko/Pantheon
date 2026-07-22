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
import styles from './Layout.module.css'

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
  const { user, reopenProfilePicker } = useAuth()
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
    <div className={`hds-app-shell ${styles.appShell}`}>
      {/* ── Sidebar (desktop) ───────────────────────────────────────────────── */}
      <nav ref={sidebarRef} className={`hds-sidebar-desktop ${styles.sidebar} ${col ? styles.sidebarCollapsed : styles.sidebarExpanded}`}>
        {/* Brand + collapse toggle */}
        <div className={styles.brandWrap}>
          {col ? (
            <div className={styles.brandCollapsedWrap}>
              <div className={styles.brandMarkCollapsed}>H</div>
              <button
                onClick={toggleNav}
                title="Expand nav"
                onMouseEnter={() => setExpandBtnHover(true)}
                onMouseLeave={() => setExpandBtnHover(false)}
                className={`${styles.navToggleBtn} ${expandBtnHover ? styles.navToggleBtnHover : ''}`}
              >
                <svg width="11" height="11" viewBox="0 0 11 11" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round">
                  <path d="M4 2l3 3.5-3 3.5" />
                </svg>
              </button>
            </div>
          ) : (
            <div className={styles.brandExpandedWrap}>
              <div>
                <div className={styles.brandTitle}>HADES</div>
                <div className={styles.brandSubtitle}>
                  KAIROS ENGINE
                </div>
              </div>
              <button
                onClick={toggleNav}
                title="Collapse nav"
                onMouseEnter={() => setCollapseBtnHover(true)}
                onMouseLeave={() => setCollapseBtnHover(false)}
                className={`${styles.navToggleBtn} ${styles.navToggleBtnExpanded} ${collapseBtnHover ? styles.navToggleBtnHover : ''}`}
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
          <div className={styles.navList}>
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
        <div className={styles.statusFooter}>
          {col ? (
            <div className={styles.statusFooterCollapsedWrap}>
              {statusStore.syncing && (
                <ProcessSpinner color="var(--hds-violet)" title="Syncing" />
              )}
              {statusStore.matching && (
                <ProcessSpinner color="var(--hds-gold)" title="Matching" />
              )}
              <span className={styles.liveDot} />
            </div>
          ) : (
            <>
              {statusStore.syncing && (
                <ProcessRow color="var(--hds-violet)" label="Syncing" />
              )}
              {statusStore.matching && (
                <ProcessRow color="var(--hds-gold)" label="Matching" />
              )}
              <div className={styles.statusRow}>
                <span className={styles.liveDot} />
                <span className={styles.versionText}>
                  v0.2.0 · running
                </span>
              </div>
              {user && (
                <div className={styles.userRow}>
                  <span className={styles.usernameText}>
                    {user.username}
                    {user.role === 'admin' && <span className={styles.adminBadgeSmall}>ADMIN</span>}
                  </span>
                  <button
                    onClick={async () => { await reopenProfilePicker(); navigate('/profiles') }}
                    title="Exit to profile picker"
                    className={styles.sidebarExitBtn}
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
          className={styles.menuToggleBtn}
        >
          {mobileMenuOpen ? (
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"><path d="M3 3l10 10M13 3L3 13" /></svg>
          ) : (
            <svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" strokeWidth="1.6" strokeLinecap="round"><path d="M2 4h12M2 8h12M2 12h12" /></svg>
          )}
        </button>
        <div className={styles.topbarBrand}>HADES</div>
      </div>

      {/* ── Slide-in drawer (mobile only) — same navItems list as the desktop
          sidebar, but a separate, D-pad-nav-framework-free component: reusing
          SidebarNavItem here (it registers a useFocusable with a focusKey per
          route) would collide with the desktop sidebar's own copies of those
          same focusKeys, since CSS-hiding the sidebar on mobile doesn't unmount
          it. Touch UI has no use for D-pad focus registration anyway. ── */}
      {mobileMenuOpen && (
        <div
          className={`hds-mobile-drawer-backdrop ${styles.drawerBackdrop}`}
          onClick={() => setMobileMenuOpen(false)}
        >
          <div
            onClick={e => e.stopPropagation()}
            className={styles.drawerPanel}
          >
            {navItems.filter(item => !item.adminOnly || user?.role === 'admin').map(({ to, label, icon }) => (
              <MobileNavLink key={to} to={to} label={label} icon={icon} />
            ))}
            {user && (
              <div className={styles.drawerUserRow}>
                <span className={styles.drawerUsernameText}>
                  {user.username}
                  {user.role === 'admin' && <span className={styles.drawerAdminBadge}>ADMIN</span>}
                </span>
                <div className={styles.drawerButtonRow}>
                  <button
                    onClick={async () => { await reopenProfilePicker(); navigate('/profiles') }}
                    className={styles.drawerExitBtn}
                  >Exit</button>
                </div>
              </div>
            )}
          </div>
        </div>
      )}

      {/* ── Main content ────────────────────────────────────────────────────── */}
      <main className={`hds-main-content ${styles.mainContent}`}>
        {/* Debug banner — shown when any debug logging flag is active */}
        {statusStore.anyDebugEnabled && (
          <DebugBanner
            syncDebug={statusStore.syncDebug}
            epgDebug={statusStore.epgDebug}
            onSettings={() => navigate('/settings?tab=diagnostics')}
          />
        )}

        <div
          className={`${styles.contentArea} ${isFullBleed ? styles.contentAreaFullBleed : `${styles.contentAreaScrollable} scrollbar-dark`}`}
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
    <NavLink to={to} className={styles.navLinkReset}>
      {({ isActive }) => (
        <div className={`${styles.mobileNavItem} ${isActive ? styles.mobileNavItemActive : ''}`}>
          <span className={`${styles.mobileNavIcon} ${isActive ? styles.mobileNavIconActive : ''}`}>{icon}</span>
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
    <NavLink to={to} title={col ? label : undefined} className={styles.navLinkReset}>
      {({ isActive }) => (
        <div
          ref={ref} data-tv-focused={focused}
          className={`${styles.sidebarItem} ${col ? styles.sidebarItemCollapsed : styles.sidebarItemExpanded} ${isActive ? styles.sidebarItemActive : ''}`}>
          {isActive && (
            <span className={styles.activeIndicatorBar} />
          )}
          <span className={`${styles.sidebarIcon} ${isActive ? styles.sidebarIconActive : ''}`}>
            {icon}
          </span>
          {!col && <span>{label}</span>}

          {/* Pending requests badge on Review (admin) */}
          {pendingRequests > 0 && (
            <span className={`${styles.badgePending} ${col ? styles.badgePendingCollapsed : styles.badgePendingExpanded}`}>
              {pendingRequests > 99 ? '99+' : pendingRequests}
            </span>
          )}

          {/* Activity indicator: red error badge > purple sync dot */}
          {hasErrors ? (
            <span className={`${styles.badgeError} ${col ? styles.badgeErrorCollapsed : styles.badgeErrorExpanded}`}>
              {unreadErrors > 99 ? '99+' : unreadErrors}
            </span>
          ) : anyRunning ? (
            <span className={`${styles.badgeRunningDot} ${col ? styles.badgeRunningDotCollapsed : styles.badgeRunningDotExpanded}`} />
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
         className={styles.processSpinner} style={{ color }}>
      <title>{title}</title>
      <circle cx="6.5" cy="6.5" r="4.5" stroke="currentColor" strokeWidth="1.6"
              strokeDasharray="18 10" strokeLinecap="round" />
    </svg>
  )
}

function ProcessRow({ color, label }: { color: string; label: string }) {
  return (
    <div className={styles.processRow} style={{ color }}>
      <ProcessSpinner color={color} title={label} />
      <span className={styles.processRowLabel}>
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
    <div className={styles.debugBanner}>
      <span className={styles.debugBannerDot} />
      <span className={styles.debugBannerText}>
        DEBUG LOGGING ACTIVE
        <span className={styles.debugBannerModes}>({modes})</span>
        <span className={styles.debugBannerNote}>
          — verbose output is being written to logs
        </span>
      </span>
      <button
        onClick={onSettings}
        className={styles.debugBannerBtn}
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
    <div className={`${styles.floatingCard} ${styles.floatingCardErrorPosition} ${styles.floatingCardErrorBorder}`}>
      {/* Header */}
      <div className={styles.cardHeader}>
        <span className={styles.cardHeaderErrorDot} />
        <span className={styles.cardHeaderErrorTitle}>
          ENGINE ERROR
        </span>
        <span className={styles.cardHeaderTimestamp}>{toast.ts}</span>
        <button
          onClick={onDismiss}
          className={styles.cardDismissBtn}
          title="Dismiss"
        >✕</button>
      </div>

      {/* Message */}
      <div className={styles.cardMessage}>
        {toast.msg || 'An unexpected error occurred in the engine.'}
      </div>

      {/* Footer */}
      <div className={styles.cardFooter}>
        <button
          onClick={onViewLogs}
          className={styles.cardActionBtnError}
        >
          View Logs →
        </button>
        <button
          onClick={onDismiss}
          className={styles.cardDismissWideBtn}
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
    <div className={`${styles.floatingCard} ${styles.floatingCardNoticePosition} ${styles.floatingCardNoticeBorder}`}>
      {/* Header */}
      <div className={styles.cardHeader}>
        <span className={styles.cardHeaderNoticeDot} />
        <span className={styles.cardHeaderNoticeTitle}>
          UNREGISTERED USERS
        </span>
        <button
          onClick={onDismiss}
          className={styles.cardDismissBtn}
          title="Dismiss"
        >✕</button>
      </div>

      {/* Message */}
      <div className={styles.cardMessageTight}>
        Your sources have {users.length} user{users.length === 1 ? '' : 's'} not registered here:
      </div>
      <div className={styles.cardUserList}>
        {users.slice(0, 8).map(u => (
          <div key={`${u.source_id}:${u.external_user_id}`} className={styles.cardUserRow}>
            <span className={styles.cardUserName}>{u.display_name || u.external_user_id}</span>
            <span className={styles.cardUserSource}>{u.source_display_name}</span>
          </div>
        ))}
        {users.length > 8 && (
          <div className={styles.cardUserMore}>+{users.length - 8} more</div>
        )}
      </div>

      {/* Footer */}
      <div className={styles.cardFooterTight}>
        <button
          onClick={onReview}
          className={styles.cardActionBtnGold}
        >
          Review in Sources →
        </button>
        <button
          onClick={onDismiss}
          className={styles.cardDismissWideBtn}
        >
          Dismiss
        </button>
      </div>
    </div>
  )
}
