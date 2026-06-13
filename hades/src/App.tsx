import { Navigate, Route, Routes } from 'react-router-dom'
import Layout       from './components/Layout'
import ActivityPage from './pages/ActivityPage'
import ChannelsPage from './pages/ChannelsPage'
import ContentPage  from './pages/ContentPage'
import SourcesPage  from './pages/SourcesPage'

export default function App() {
  return (
    <Routes>
      <Route element={<Layout />}>
        <Route index element={<Navigate to="/sources" replace />} />
        <Route path="sources"  element={<SourcesPage />} />
        <Route path="channels" element={<ChannelsPage />} />
        <Route path="content"  element={<ContentPage />} />
        <Route path="activity" element={<ActivityPage />} />
      </Route>
    </Routes>
  )
}
