import { createContext, useContext, useEffect, useState, type ReactNode } from 'react'
import { api, TOKEN_KEY } from '../api/client'
import type { User } from '../api/types'

interface AuthContextValue {
  user:          User | null
  isLoading:     boolean
  setupRequired: boolean
  login:         (username: string, password: string) => Promise<void>
  logout:        () => Promise<void>
  completeSetup: (username: string, password: string) => Promise<void>
  // Self-service password change — clears must_change_password, letting
  // ProtectedRoute's gate release. Used by SetPasswordPage.
  setPassword:   (password: string) => Promise<void>
  // Establishes a session from a token Kairos already minted (invite claim,
  // which auto-logs in on success) rather than going through /auth/login.
  // Callers must await this before navigating — like login/completeSetup, it
  // loads profiles as part of establishing the session, and navigating
  // before that resolves lets ProtectedRoute momentarily see an empty
  // profiles list and skip the picker it shouldn't.
  applySession:  (token: string, user: User) => Promise<void>
  // Every profile on this server, for the "Who's watching?" picker —
  // populated once a real login session exists. Empty until then.
  profiles:      User[]
  // Deliberately in-memory only (never persisted) — a fresh page load always
  // starts this false, which is what makes ProfileSelectPage reappear on
  // every app launch even though the device itself stays logged in. Treated
  // as already-satisfied once profiles.length<=1 (nothing to pick).
  profileChosen: boolean
  // Switches the active session to a different profile without its
  // password — see AuthStore::switchProfile for the actual gate (PIN, if
  // the target profile has one; always required for admin profiles).
  switchProfile: (userId: string, pin?: string) => Promise<void>
  // Re-arms the picker on demand (Layout's "Switch Profile" link) without a
  // full logout — the caller still has to navigate to /profiles itself,
  // this just stops ProtectedRoute treating the current profile as chosen.
  // Also refetches profiles, since the cached list (last loaded at login)
  // would otherwise still show a stale has_pin for anyone whose PIN an
  // admin set/cleared via the Users page in the meantime — ProfileSelectPage
  // would then skip the PIN prompt it should show (or show one it
  // shouldn't), relying on the server's 403 as an ugly fallback instead of
  // just asking correctly the first time. Callers should await this before
  // navigating, same reasoning as applySession above.
  reopenProfilePicker: () => Promise<void>
  // Picking the tile for whichever profile is already the active session
  // (e.g. the admin who just typed their password) needs no PIN and no
  // switchProfile call — it's already this profile. Without this, an
  // admin profile with no PIN set yet would be unable to get past its own
  // picker tile (switchProfile always requires one for role=admin),
  // a chicken-and-egg lockout since the PIN can only be set from inside
  // the app. ProfileSelectPage uses this instead of switchProfile when
  // user.user_id matches the tile clicked.
  confirmCurrentProfile: () => void
}

const AuthContext = createContext<AuthContextValue | null>(null)

// Plain, non-React mutable holder for the logged-in user's id — mirrors the
// statusStore pattern (stores/StatusStore.ts) since remoteLog.ts (called
// once at app startup from main.tsx, outside the component tree) needs to
// read this without a hook. Kept to just the id, not the full User, since
// that's all any consumer so far needs; updated in lockstep with every
// setUser call below via the small wrapper.
export const currentUserRef: { id: string | null } = { id: null }

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user,          setUserState]     = useState<User | null>(null)
  const setUser = (u: User | null) => { currentUserRef.id = u?.user_id ?? null; setUserState(u) }
  const [isLoading,     setIsLoading]     = useState(true)
  const [setupRequired, setSetupRequired] = useState(false)
  const [profiles,        setProfiles]        = useState<User[]>([])
  const [profileChosenRaw, setProfileChosenRaw] = useState(false)

  // Nothing to pick when there's only one profile on the server — treated as
  // already-chosen so a single-admin household never sees an empty-feeling
  // one-tile picker.
  const profileChosen = profileChosenRaw || profiles.length <= 1

  const loadProfiles = async () => {
    try { setProfiles(await api.getProfiles()) } catch { /* picker just stays empty; ProtectedRoute still gates on user */ }
  }

  useEffect(() => {
    let cancelled = false

    const init = async () => {
      try {
        const { setup_required } = await api.checkSetup()
        if (cancelled) return
        if (setup_required) {
          setSetupRequired(true)
          setIsLoading(false)
          return
        }
        const token = localStorage.getItem(TOKEN_KEY)
        if (token) {
          try {
            const me = await api.getMe()
            if (cancelled) return
            setUser(me)
            await loadProfiles()
          } catch {
            localStorage.removeItem(TOKEN_KEY)
          }
        }
      } catch {
        // Network error — leave loading=false so the app can still attempt routes.
      } finally {
        if (!cancelled) setIsLoading(false)
      }
    }

    init()

    const onUnauthorized = () => {
      localStorage.removeItem(TOKEN_KEY)
      setUser(null)
    }
    window.addEventListener('kairos:unauthorized', onUnauthorized)

    return () => {
      cancelled = true
      window.removeEventListener('kairos:unauthorized', onUnauthorized)
    }
  }, [])

  const login = async (username: string, password: string) => {
    const { token, user } = await api.login(username, password)
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
    await loadProfiles()
  }

  const completeSetup = async (username: string, password: string) => {
    const { token, user } = await api.setup(username, password)
    localStorage.setItem(TOKEN_KEY, token)
    setSetupRequired(false)
    setUser(user)
    await loadProfiles()
  }

  const logout = async () => {
    try { await api.logout() } catch { /* ignore network errors on logout */ }
    localStorage.removeItem(TOKEN_KEY)
    setUser(null)
    setProfiles([])
    setProfileChosenRaw(false)
  }

  const setPassword = async (password: string) => {
    if (!user) throw new Error('not logged in')
    await api.updateUser(user.user_id, { password })
    setUser({ ...user, must_change_password: false })
  }

  const applySession = async (token: string, user: User) => {
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
    await loadProfiles()
  }

  const switchProfile = async (userId: string, pin?: string) => {
    const { token, user } = await api.switchProfile(userId, pin)
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
    setProfileChosenRaw(true)
  }

  const reopenProfilePicker = async () => {
    setProfileChosenRaw(false)
    await loadProfiles()
  }

  const confirmCurrentProfile = () => setProfileChosenRaw(true)

  return (
    <AuthContext.Provider value={{
      user, isLoading, setupRequired, login, logout, completeSetup, setPassword, applySession,
      profiles, profileChosen, switchProfile, reopenProfilePicker, confirmCurrentProfile,
    }}>
      {children}
    </AuthContext.Provider>
  )
}

export function useAuth() {
  const ctx = useContext(AuthContext)
  if (!ctx) throw new Error('useAuth must be used inside AuthProvider')
  return ctx
}
