import { createContext, useContext, useEffect, useState, type ReactNode } from 'react'
import { api, TOKEN_KEY } from '../api/client'
import type { User } from '../api/types'

interface AuthContextValue {
  user:          User | null
  isLoading:     boolean
  setupRequired: boolean
    // Returns the freshly-authenticated user — callers that need to act on it
    // immediately (LoginPage/ProfileSelectPage resolving where to land) can't
    // rely on this context's own `user` state, which updates asynchronously.
    login: (username: string, password: string) => Promise<User>
  logout:        () => Promise<void>
  completeSetup: (username: string, password: string) => Promise<void>
  // Self-service password change — clears must_change_password, letting
  // ProtectedRoute's gate release. Used by SetPasswordPage.
  setPassword:   (password: string) => Promise<void>
  // Self-service library-wide fallback audio/subtitle language (AccountPage)
  // — see kairos migration v94. A field omitted from `b` leaves that side
  // untouched, same convention the per-show/movie preference endpoints use.
  updateTrackPreference: (b: { audio_lang?: string; subtitle_lang?: string }) => Promise<void>
    // Self-service post-login/profile-switch landing page override (AccountPage)
    // — '' means "inherit the admin-configured global default." See kairos
    // migration v96.
    updateDefaultLandingPage: (page: '' | 'home' | 'guide') => Promise<void>
  // Establishes a session from a token Kairos already minted (invite claim,
  // which auto-logs in on success) rather than going through /auth/login.
  // Callers must await this before navigating — like login/completeSetup, it
  // loads profiles as part of establishing the session, and navigating
  // before that resolves lets ProtectedRoute momentarily see an empty
  // profiles list and skip the picker it shouldn't.
  applySession:  (token: string, user: User) => Promise<void>
    // Guest-only self-service first-run setup (GuestSetupPage) — see
    // AuthService.cpp's PATCH /api/auth/me/guest, which 403s for anyone whose
    // account isn't actually a guest. Same "server rejects it regardless of
    // what the UI shows" belt-and-suspenders as every other admin-toggleable
    // capability in this codebase.
    completeGuestSetup: (b: {
        pin?: string;
        restricted?: boolean;
        max_tv_rating?: string;
        max_movie_rating?: string;
        max_channel_rating?: string
    }) => Promise<void>
    // Guest-only self-delete (AccountPage's "Delete My Guest Account") — logs
    // the device out afterward, same as logout(), since there's no account
    // left to hold a session for.
    deleteGuestAccount: () => Promise<void>
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
    // Same reasoning as login() above — returns the newly-active user directly.
    switchProfile: (userId: string, pin?: string) => Promise<User>
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
    // Resolves where a fresh login/profile-switch should land: the given
    // user's own override if set, else the admin-configured global default
    // (GET /api/config/public-settings — accessible with any valid token,
    // same route Settings' own admin view reads from), else '/'. Used by
    // LoginPage/ProfileSelectPage in place of a hardcoded landing target.
    resolveLandingPath: (user: User) => Promise<string>
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
      return user
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

  const updateTrackPreference = async (b: { audio_lang?: string; subtitle_lang?: string }) => {
    if (!user) throw new Error('not logged in')
    await api.setMyTrackPreference(b)
    setUser({
      ...user,
      default_audio_lang:    b.audio_lang    ?? user.default_audio_lang,
      default_subtitle_lang: b.subtitle_lang ?? user.default_subtitle_lang,
    })
  }

    const updateDefaultLandingPage = async (page: '' | 'home' | 'guide') => {
        if (!user) throw new Error('not logged in')
        await api.setMyLandingPage(page)
        setUser({...user, default_landing_page: page})
    }

  const applySession = async (token: string, user: User) => {
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
    await loadProfiles()
  }

    const completeGuestSetup = async (b: {
        pin?: string;
        restricted?: boolean;
        max_tv_rating?: string;
        max_movie_rating?: string;
        max_channel_rating?: string
    }) => {
        if (!user) throw new Error('not logged in')
        await api.updateGuestSetup(b)
        setUser({
            ...user,
            has_pin: b.pin !== undefined ? b.pin !== '' : user.has_pin,
            restricted: b.restricted ?? user.restricted,
            max_tv_rating: b.max_tv_rating ?? user.max_tv_rating,
            max_movie_rating: b.max_movie_rating ?? user.max_movie_rating,
            max_channel_rating: b.max_channel_rating ?? user.max_channel_rating,
        })
    }

    const deleteGuestAccount = async () => {
        await api.deleteGuest()
        localStorage.removeItem(TOKEN_KEY)
        setUser(null)
        setProfiles([])
        setProfileChosenRaw(false)
    }

  const switchProfile = async (userId: string, pin?: string) => {
    const { token, user } = await api.switchProfile(userId, pin)
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
    setProfileChosenRaw(true)
      return user
  }

  const reopenProfilePicker = async () => {
    setProfileChosenRaw(false)
    await loadProfiles()
  }

  const confirmCurrentProfile = () => setProfileChosenRaw(true)

    const resolveLandingPath = async (u: User): Promise<string> => {
        if (u.default_landing_page === 'guide') return '/guide'
        if (u.default_landing_page === 'home') return '/'
        try {
            const settings = await api.getPublicSettings()
            return settings.default_landing_page === 'guide' ? '/guide' : '/'
        } catch {
            return '/'
        }
    }

  return (
    <AuthContext.Provider value={{
        user,
        isLoading,
        setupRequired,
        login,
        logout,
        completeSetup,
        setPassword,
        updateTrackPreference,
        updateDefaultLandingPage,
        applySession,
        completeGuestSetup,
        deleteGuestAccount,
        profiles,
        profileChosen,
        switchProfile,
        reopenProfilePicker,
        confirmCurrentProfile,
        resolveLandingPath,
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
