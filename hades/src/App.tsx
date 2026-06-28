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
import ContentPage            from './pages/ContentPage'
import DownloadPage           from './pages/DownloadPage'
import FillerPage             from './pages/FillerPage'
import GroupsPage             from './pages/GroupsPage'
import HomePage               from './pages/HomePage'
import LibraryPage            from './pages/LibraryPage'
import PlaylistPage           from './pages/PlaylistPage'
import SettingsPage           from './pages/SettingsPage'
import SourcesPage            from './pages/SourcesPage'
import UsersPage              from './pages/UsersPage'

export default function App() {
  return (
    <AuthProvider>
      <ErrorBoundary>
      <Routes>
        <Route path="/login" element={<LoginPage />} />
        <Route path="/setup" element={<SetupPage />} />

        <Route element={<ProtectedRoute />}>
          <Route element={<Layout />}>
            <Route index element={<HomePage />} />
            <Route path="library"          element={<LibraryPage />} />
            <Route path="sources"          element={<SourcesPage />} />
            <Route path="channels"         element={<ChannelsPage />} />
            <Route path="channels/:id"     element={<ChannelDetailPage />} />
            <Route path="content"          element={<ContentPage />} />
            <Route path="groups"           element={<GroupsPage />} />
            <Route path="playlists"        element={<PlaylistPage />} />
            <Route path="filler"           element={<FillerPage />} />
            <Route path="downloads"        element={<DownloadPage />} />
            <Route path="activity"         element={<ActivityPage />} />
            <Route path="settings"         element={<SettingsPage />} />
            <Route path="users"            element={<UsersPage />} />
          </Route>
        </Route>
      </Routes>
      </ErrorBoundary>
    </AuthProvider>
  )
}
