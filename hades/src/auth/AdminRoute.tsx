import { Navigate, Outlet } from 'react-router-dom'
import { useAuth } from './AuthContext'

// Nested inside ProtectedRoute (so `user` is always non-null by the time
// this runs) — redirects a viewer straight back to Home rather than letting
// them reach an admin-only page via direct URL navigation. Layout.tsx's own
// navItems filtering only hides the *link*; without this, typing the path
// directly (or a stale bookmark) still rendered the page for a viewer, and
// several backend endpoints those pages call had no server-side role check
// of their own either — this is the client-side half of closing that gap.
export default function AdminRoute() {
  const { user } = useAuth()
  if (user?.role !== 'admin') return <Navigate to="/" replace />
  return <Outlet />
}
