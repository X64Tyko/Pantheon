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
  applySession:  (token: string, user: User) => void
}

const AuthContext = createContext<AuthContextValue | null>(null)

export function AuthProvider({ children }: { children: ReactNode }) {
  const [user,          setUser]          = useState<User | null>(null)
  const [isLoading,     setIsLoading]     = useState(true)
  const [setupRequired, setSetupRequired] = useState(false)

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
            if (!cancelled) setUser(me)
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
  }

  const completeSetup = async (username: string, password: string) => {
    const { token, user } = await api.setup(username, password)
    localStorage.setItem(TOKEN_KEY, token)
    setSetupRequired(false)
    setUser(user)
  }

  const logout = async () => {
    try { await api.logout() } catch { /* ignore network errors on logout */ }
    localStorage.removeItem(TOKEN_KEY)
    setUser(null)
  }

  const setPassword = async (password: string) => {
    if (!user) throw new Error('not logged in')
    await api.updateUser(user.user_id, { password })
    setUser({ ...user, must_change_password: false })
  }

  const applySession = (token: string, user: User) => {
    localStorage.setItem(TOKEN_KEY, token)
    setUser(user)
  }

  return (
    <AuthContext.Provider value={{ user, isLoading, setupRequired, login, logout, completeSetup, setPassword, applySession }}>
      {children}
    </AuthContext.Provider>
  )
}

export function useAuth() {
  const ctx = useContext(AuthContext)
  if (!ctx) throw new Error('useAuth must be used inside AuthProvider')
  return ctx
}
