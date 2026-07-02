import { lazy, Suspense } from 'react'
import { Route, Routes } from 'react-router-dom'
import { AuthProvider }       from './auth/AuthContext'
import { ErrorBoundary }      from './components/ErrorBoundary'
import LoginPage              from './auth/LoginPage'
import ProtectedRoute         from './auth/ProtectedRoute'
import SetupPage              from './auth/SetupPage'
import Layout                 from './components/Layout'
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

// Lazy: hls.js is a ~500KB dependency that only the player route needs — every
// other page load (the vast majority of app usage) shouldn't pay for it.
const PlayerPage = lazy(() => import('./player/PlayerPage').then(m => ({ default: m.PlayerPage })))

// Matches PlayerPage's own background so the moment before the lazy chunk
// resolves doesn't flash unstyled content.
const playerFallback = <div style={{ position: 'fixed', inset: 0, background: '#000', zIndex: 100 }} />

export default function App() {
  return (
    <AuthProvider>
      <ErrorBoundary>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/setup" element={<SetupPage />} />

        <Route element={<ProtectedRoute />}>
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

          <Route element={<Layout />}>
            <Route index element={<HomePage />} />
            <Route path="library"      element={<LibraryPage />} />
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
      </Routes>
      </ErrorBoundary>
    </AuthProvider>
  )
}
