import { lazy, Suspense } from 'react'
import { Route, Routes } from 'react-router-dom'
import { AuthProvider }       from './auth/AuthContext'
import { ErrorBoundary }      from './components/ErrorBoundary'
import LoginPage              from './auth/LoginPage'
import ProtectedRoute         from './auth/ProtectedRoute'
import AdminRoute             from './auth/AdminRoute'
import SetupPage              from './auth/SetupPage'
import SetPasswordPage        from './auth/SetPasswordPage'
import InvitePage             from './auth/InvitePage'
import ProfileSelectPage      from './auth/ProfileSelectPage'
import Layout                 from './components/Layout'
import { FocusRoot }          from './nav/FocusRoot'
import { CastProvider }       from './cast/CastProvider'
import { CastReceiverProvider } from './cast/CastReceiverProvider'
import ActivityPage           from './pages/ActivityPage'
import ChannelDetailPage      from './pages/ChannelDetailPage'
import ChannelsPage           from './pages/ChannelsPage'
import DownloadPage           from './pages/DownloadPage'
import FillerPage             from './pages/FillerPage'
import HomePage               from './pages/HomePage'
import LibraryPage            from './pages/LibraryPage'
import PlaylistPage           from './pages/PlaylistPage'
import ReviewPage             from './pages/ReviewPage'
import SettingsPage           from './pages/SettingsPage'
import SourcesPage            from './pages/SourcesPage'
import UsersPage              from './pages/UsersPage'
import styles from './App.module.css'

// Lazy: hls.js is a ~500KB dependency that only the player route needs — every
// other page load (the vast majority of app usage) shouldn't pay for it.
const PlayerPage = lazy(() => import('./player/PlayerPage').then(m => ({ default: m.PlayerPage })))

// Lazy: the whole /tv tree (10-foot screens, TV-sized grids) is dead weight
// on every desktop/mobile page load — only devices actually navigating to
// /tv should pay for this chunk.
const TvShell         = lazy(() => import('./tv/TvShell').then(m => ({ default: m.TvShell })))
const TvHome          = lazy(() => import('./tv/TvHome').then(m => ({ default: m.TvHome })))
const TvLibrary       = lazy(() => import('./tv/TvLibrary').then(m => ({ default: m.TvLibrary })))
const TvLibraryDetail = lazy(() => import('./tv/TvLibraryDetail').then(m => ({ default: m.TvLibraryDetail })))
const TvProfileSelect = lazy(() => import('./tv/TvProfileSelect').then(m => ({ default: m.TvProfileSelect })))

const playerFallback = <div className={styles.playerFallback} />
const tvFallback      = <div className={styles.tvFallback} />

export default function App() {
  return (
    <AuthProvider>
      <ErrorBoundary>
      <CastProvider />
      <CastReceiverProvider />
      <FocusRoot>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/setup" element={<SetupPage />} />
        {/* Unauthenticated invite-claim flow — only actionable with the
            unguessable token in the URL itself; see AuthStore::claimInvite. */}
        <Route path="/invite/:token" element={<InvitePage />} />

        <Route element={<ProtectedRoute />}>
          {/* Mandatory gate for invite-created (temp-password) accounts —
              reached via ProtectedRoute's must_change_password redirect, not
              a normal nav target. Deliberately outside <Layout>: the rest of
              the app is blocked until this is done. */}
          <Route path="set-password" element={<SetPasswordPage />} />

          {/* "Who's watching?" profile picker — reached via ProtectedRoute's
              !profileChosen redirect, not a normal nav target. Same
              deliberately-outside-<Layout> treatment as set-password above. */}
          <Route path="profiles" element={<ProfileSelectPage />} />

          {/* Full-screen takeover — no sidebar chrome during playback. */}
          <Route path="player/movie/:id" element={
            <Suspense fallback={playerFallback}><PlayerPage kind="movie" /></Suspense>
          } />
          <Route path="player/episode/:id" element={
            <Suspense fallback={playerFallback}><PlayerPage kind="episode" /></Suspense>
          } />
          <Route path="player/channel/:channelId" element={
            <Suspense fallback={playerFallback}><PlayerPage kind="channel" /></Suspense>
          } />

          {/* 10-foot surface — no admin sidebar, same full-screen-takeover pattern as /player/*. */}
          <Route path="tv" element={
            <Suspense fallback={tvFallback}><TvShell /></Suspense>
          }>
            <Route index                    element={<TvHome />} />
            <Route path="library"           element={<TvLibrary />} />
            <Route path="library/:type/:id" element={<TvLibraryDetail />} />
            {/* Reached via ProtectedRoute's !profileChosen redirect when it
                fires from inside /tv (e.g. a Cast handoff) — not a normal
                nav target. See ProtectedRoute.tsx and TvProfileSelect.tsx. */}
            <Route path="profiles"          element={<TvProfileSelect />} />
          </Route>

          <Route element={<Layout />}>
            <Route index element={<HomePage />} />
            <Route path="library" element={<LibraryPage />} />

            {/* Everything else under Layout is admin-only — system/source/
                channel configuration, scraper+credential settings, download
                triggering, and internal activity/logs. Viewers get browsing
                (Home/Library), requesting content (from within Library's
                detail view), and /tv. Nav-link visibility (Layout.tsx's
                navItems) hides these from viewers cosmetically, but only
                this route guard actually stops direct URL navigation —
                the two need to be kept in sync by hand, there's no single
                shared source of truth for "admin-only" between them. */}
            <Route element={<AdminRoute />}>
              <Route path="sources"      element={<SourcesPage />} />
              <Route path="channels"     element={<ChannelsPage />} />
              <Route path="channels/:id" element={<ChannelDetailPage />} />
              <Route path="playlists"    element={<PlaylistPage />} />
              <Route path="filler"       element={<FillerPage />} />
              <Route path="downloads"    element={<DownloadPage />} />
              <Route path="activity"     element={<ActivityPage />} />
              <Route path="review"       element={<ReviewPage />} />
              <Route path="settings"     element={<SettingsPage />} />
              <Route path="users"        element={<UsersPage />} />
            </Route>
          </Route>
        </Route>
      </Routes>
      </FocusRoot>
      </ErrorBoundary>
    </AuthProvider>
  )
}
