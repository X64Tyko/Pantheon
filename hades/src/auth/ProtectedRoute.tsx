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

  // Invite-created account that hasn't replaced its temp/placeholder
  // password yet — block the rest of the app behind /set-password until it
  // does (setPassword() clears the flag and this gate releases).
  if (user.must_change_password && location.pathname !== '/set-password') {
    return <Navigate to="/set-password" replace />
  }

  return <Outlet />
}
