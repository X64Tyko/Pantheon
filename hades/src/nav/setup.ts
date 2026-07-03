import { init } from '@noriginmedia/norigin-spatial-navigation'

// Called once at module load (see App.tsx's top-level import) — not a React
// component, this is the library's own service singleton. shouldFocusDOMNode
// makes it call the real element.focus() on every focus change for free
// (screen readers + native :focus-visible), matching what we used to hand-roll.
// preventScroll: nav/useFocusable.ts already does its own explicit
// scrollIntoView — without this, a real <button>'s native focus() call
// (browsers auto-scroll a newly-focused element by default) would compete
// with that instead of the two agreeing on one scroll target.
init({
  shouldFocusDOMNode: true,
  domNodeFocusOptions: { preventScroll: true },
})
