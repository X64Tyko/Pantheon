import { Navigate, Outlet, useLocation } from 'react-router-dom'
import { useAuth } from './AuthContext'

export default function ProtectedRoute() {
  const { user, isLoading, setupRequired } = useAuth()
  const location = useLocation()

  if (isLoading) return null

  if (!user) {
    return setupRequired
      ? <Navigate to="/setup" replace />
      : <Navigate to="/login" state={{ from: location }} replace />
  }

  return <Outlet />
}
