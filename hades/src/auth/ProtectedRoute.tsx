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
  // session established above. Applies identically regardless of how the
  // session was obtained — including a Cast handoff (see cast/receiverMode.ts)
  // — since isCastReceiverMode() is never read here; the picker itself is
  // the point (a multi-profile household still needs "who's watching?"
  // before content starts), not something a cast arrival should skip.
  //
  // /tv gets its own D-pad-navigable picker (TvProfileSelect, rendered
  // inside TvShell) instead of the desktop ProfileSelectPage — the whole
  // reason this exists is a Cast handoff landing on /tv?castToken=... with
  // no profile chosen yet; bouncing that to an unstyled, non-D-pad desktop
  // page defeats the point of arriving on a TV surface at all. The original
  // destination is preserved via location state so picking a profile can
  // return to wherever the redirect actually interrupted (e.g. mid Cast
  // LOAD-interceptor navigation to /player/..., not just plain /tv).
  const isTvSurface = location.pathname === '/tv' || location.pathname.startsWith('/tv/')
  const profilePickerPath = isTvSurface ? '/tv/profiles' : '/profiles'
  if (!profileChosen && location.pathname !== profilePickerPath) {
    return <Navigate to={profilePickerPath} state={{ from: location }} replace />
  }

  return <Outlet />
}
