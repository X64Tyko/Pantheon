import type { ReactNode } from 'react'
import './setup' // calls the library's init() once, on module load
import { useGamepadNav } from './useGamepadNav'
import './focus.css'

// Mounted once around the whole <Routes> tree in App.tsx. The library
// (norigin-spatial-navigation) doesn't need a wrapping Provider for basic
// operation — useFocusable() talks directly to its own singleton service —
// so this component's only real job is mounting gamepad polling once,
// app-wide. Kept as a wrapper (rather than inlining into App.tsx) so the
// import of './setup' has an obvious, single, unmissable home.
export function FocusRoot({ children }: { children: ReactNode }) {
  useGamepadNav()
  return <>{children}</>
}
