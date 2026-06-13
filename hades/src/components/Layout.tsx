import { observer } from 'mobx-react-lite'
import { useEffect } from 'react'
import { NavLink, Outlet } from 'react-router-dom'
import { systemStore } from '../stores'

const navItems = [
  { to: '/sources',  label: 'Sources',  icon: '⬡' },
  { to: '/channels', label: 'Channels', icon: '◈' },
  { to: '/content',  label: 'Content',  icon: '▤'  },
  { to: '/activity', label: 'Activity', icon: '◎'  },
]

export default observer(function Layout() {
  useEffect(() => {
    systemStore.startPolling()
    return () => systemStore.stopPolling()
  }, [])

  return (
    <div className="flex h-screen overflow-hidden bg-zinc-950">
      {/* ── Sidebar ─────────────────────────────────────────────────────────── */}
      <nav className="w-52 shrink-0 flex flex-col border-r border-violet-900/30 bg-zinc-950">

        {/* Brand */}
        <div className="px-5 pt-6 pb-5">
          <div className="text-xl font-black tracking-[0.2em] text-amber-400">HADES</div>
          <div className="text-[10px] text-violet-500/70 tracking-widest uppercase mt-0.5">
            Kairos Engine
          </div>
        </div>

        {/* Divider */}
        <div className="mx-4 h-px bg-gradient-to-r from-violet-900/60 via-violet-700/30 to-transparent mb-2" />

        {/* Nav links */}
        <div className="flex flex-col gap-0.5 px-2 flex-1">
          {navItems.map(({ to, label, icon }) => (
            <NavLink
              key={to}
              to={to}
              className={({ isActive }) =>
                `flex items-center gap-2.5 px-3 py-2 rounded text-sm transition-all duration-150 ${
                  isActive
                    ? 'bg-amber-500/10 text-amber-400 ring-1 ring-amber-500/20'
                    : 'text-zinc-500 hover:text-zinc-200 hover:bg-violet-950/40'
                }`
              }
            >
              {({ isActive }) => (
                <>
                  <span className={`text-base leading-none ${isActive ? 'text-amber-400' : 'text-zinc-600'}`}>
                    {icon}
                  </span>
                  <span>{label}</span>
                  {to === '/activity' && systemStore.syncing && (
                    <span className="ml-auto w-1.5 h-1.5 rounded-full bg-violet-400 animate-pulse" />
                  )}
                </>
              )}
            </NavLink>
          ))}
        </div>

        {/* Divider */}
        <div className="mx-4 h-px bg-gradient-to-r from-violet-900/60 via-violet-700/30 to-transparent mt-2 mb-3" />

        {/* Status footer */}
        <div className="px-5 pb-5 flex items-center gap-2">
          {systemStore.syncing ? (
            <>
              <span className="w-1.5 h-1.5 rounded-full bg-violet-400 animate-pulse shrink-0" />
              <span className="text-[10px] text-violet-400/80 truncate">Syncing…</span>
            </>
          ) : (
            <>
              <span className="w-1.5 h-1.5 rounded-full bg-zinc-700 shrink-0" />
              <span className="text-[10px] text-zinc-700">v0.1.0</span>
            </>
          )}
        </div>
      </nav>

      {/* ── Main content ────────────────────────────────────────────────────── */}
      <main className="flex-1 overflow-y-auto p-6 scrollbar-dark">
        <Outlet />
      </main>
    </div>
  )
})
