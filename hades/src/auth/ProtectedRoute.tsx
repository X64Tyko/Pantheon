import { Navigate, Outlet, useLocation } from 'react-router-dom'
import { useAuth } from './AuthContext'

export default function ProtectedRoute() {
  const { user, isLoading, setupRequired, profileChosen } = useAuth()
  const location = useLocation()

  if (isLoading) return null

  if (!user) {
    return setupRequired
      ? <Navigate to="/setup" replace />
      : <Navigate to="/login" state={{ from: location }} replace />
  }

  // Invite-created account that hasn't replaced its temp/placeholder
  // password yet — block the rest of the app behind /set-password until it
  // does (setPassword() clears the flag and this gate releases). Takes
  // priority over the profile picker below: securing the account comes
  // first, before any "Who's watching?" convenience layer on top of it.
  if (user.must_change_password && location.pathname !== '/set-password') {
    return <Navigate to="/set-password" replace />
  }

  // "Who's watching?" picker — reappears on every fresh app load (see
  // AuthContext's profileChosen, deliberately never persisted) even though
  // the device itself stays logged in via the real username/password
  // session established above.
  if (!profileChosen && location.pathname !== '/profiles') {
    return <Navigate to="/profiles" replace />
  }

  return <Outlet />
}
