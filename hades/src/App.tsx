import { Navigate, Route, Routes } from 'react-router-dom'
import Layout             from './components/Layout'
import ActivityPage       from './pages/ActivityPage'
import ChannelDetailPage  from './pages/ChannelDetailPage'
import ChannelsPage       from './pages/ChannelsPage'
import ContentPage        from './pages/ContentPage'
import DownloadPage       from './pages/DownloadPage'
import FillerPage         from './pages/FillerPage'
import PlaylistPage       from './pages/PlaylistPage'
import SettingsPage       from './pages/SettingsPage'
import SourcesPage        from './pages/SourcesPage'

export default function App() {
  return (
    <Routes>
      <Route element={<Layout />}>
        <Route index element={<Navigate to="/sources" replace />} />
        <Route path="sources"          element={<SourcesPage />} />
        <Route path="channels"         element={<ChannelsPage />} />
        <Route path="channels/:id"     element={<ChannelDetailPage />} />
        <Route path="content"          element={<ContentPage />} />
        <Route path="playlists"        element={<PlaylistPage />} />
        <Route path="filler"           element={<FillerPage />} />
        <Route path="downloads"        element={<DownloadPage />} />
        <Route path="activity"         element={<ActivityPage />} />
        <Route path="settings"         element={<SettingsPage />} />
      </Route>
    </Routes>
  )
}
