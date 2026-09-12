import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { ImportUserResult, Library, LibraryInfo, PathMap, SourceUser } from '../api/types'
import { sourceStore, systemStore, userStore } from '../stores'
import { tourStore } from '../stores/TourStore'
import { TourSpotlight } from '../components/tour/TourSpotlight'
import {LANGUAGE_OPTIONS} from '../constants/languages'

type TestState = 'idle' | 'testing' | 'ok' | 'failed'

const NEEDS_TOKEN   = ['plex', 'jellyfin', 'emby']
const NEEDS_USER_ID = ['jellyfin', 'emby']

const SOURCE_HELP: Record<string, { url: string; token: string; userId?: string }> = {
  plex: {
    url:   'The address of your Plex server, e.g. http://192.168.1.10:32400 or http://plex:32400. Find it in Plex Web → Settings → Remote Access.',
    token: 'Open Plex Web, play any item, then open its XML (⋮ → Get Info → View XML). The X-Plex-Token= value in the URL is your token. Alternatively: Plex Web → Account → Privacy → see your token in network requests.',
  },
  jellyfin: {
    url:    'The address of your Jellyfin server, e.g. http://192.168.1.10:8096.',
    token:  'Jellyfin Dashboard → Administration → API Keys → click + to create a new key. Paste the generated key here.',
    userId: 'Jellyfin Dashboard → Users → click a user → copy the ID from the URL (the long hex string after /users/).',
  },
  emby: {
    url:    'The address of your Emby server, e.g. http://192.168.1.10:8096.',
    token:  'Emby Dashboard → Advanced → API Keys → New API Key. Paste the generated key here.',
    userId: 'Emby Dashboard → Users → click a user → copy the ID from the URL.',
  },
}

function SourceHelpGuide({ sourceType }: { sourceType: string }) {
  const [open, setOpen] = useState(false)
  const help = SOURCE_HELP[sourceType]
  if (!help) return null
  return (
    <div className="col-span-2 text-xs">
      <button
        type="button"
        onClick={() => setOpen(v => !v)}
        className="flex items-center gap-1.5 text-violet-400/70 hover:text-violet-300 transition-colors"
      >
        <span className="inline-flex items-center justify-center w-3.5 h-3.5 rounded-full border border-violet-500/40 text-[9px] leading-none">?</span>
        {open ? 'Hide setup guide' : 'How do I find these values?'}
      </button>
      {open && (
        <div className="mt-2 space-y-2 bg-zinc-900/60 border border-violet-900/30 rounded-lg p-3">
          <div>
            <span className="text-zinc-400 font-medium">Base URL — </span>
            <span className="text-zinc-500">{help.url}</span>
          </div>
          <div>
            <span className="text-zinc-400 font-medium">Auth token — </span>
            <span className="text-zinc-500">{help.token}</span>
          </div>
          {help.userId && (
            <div>
              <span className="text-zinc-400 font-medium">User ID — </span>
              <span className="text-zinc-500">{help.userId}</span>
            </div>
          )}
        </div>
      )}
    </div>
  )
}

// Ordered scraper preference for one library + item type. Replaces the old
// single "preferred scraper" dropdown — near-best candidates are broken by
// rank (see ScraperManager.cpp's isAmbiguousTie/pickBest), so this is a real
// fallback chain, not just a single tie-breaker.
function ScraperPriorityEditor({ sourceId, libraryId, itemType, availableScrapers }: {
  sourceId: string
  libraryId: string
  itemType: 'show' | 'movie'
  availableScrapers: string[]
}) {
  const [order,  setOrder]  = useState<string[] | null>(null)
  const [saving, setSaving] = useState(false)

  useEffect(() => {
    api.getScraperPriority(sourceId, libraryId, itemType).then(r => setOrder(r.order)).catch(() => setOrder([]))
  }, [sourceId, libraryId, itemType])

  const persist = async (next: string[]) => {
    setOrder(next)
    setSaving(true)
    try { await api.setScraperPriority(sourceId, libraryId, itemType, next) } catch {}
    setSaving(false)
  }

  const move = (i: number, delta: number) => {
    if (!order) return
    const j = i + delta
    if (j < 0 || j >= order.length) return
    const next = [...order]
    ;[next[i], next[j]] = [next[j], next[i]]
    persist(next)
  }

  const remove = (i: number) => { if (order) persist(order.filter((_, idx) => idx !== i)) }
  const add    = (source: string) => { if (order && source) persist([...order, source]) }

  if (order === null) return <div className="text-xs text-zinc-600">Loading…</div>

  const addable = availableScrapers.filter(s => !order.includes(s))

  return (
    <div className="space-y-1">
      <div className="text-[10px] uppercase tracking-wide text-zinc-600">{itemType === 'show' ? 'Shows' : 'Movies'}</div>
      {order.length === 0 && (
        <div className="text-xs text-zinc-600">No preference — matching goes purely by score.</div>
      )}
      {order.map((source, i) => (
        <div key={source} className="flex items-center gap-2 px-2 py-1 rounded bg-zinc-950/60 border border-zinc-800/60 text-xs">
          <span className="text-amber-500 w-4 text-center font-medium">{i + 1}</span>
          <span className="flex-1 uppercase text-zinc-300">{source}</span>
          <button disabled={saving || i === 0} onClick={() => move(i, -1)} className="text-zinc-500 hover:text-zinc-300 disabled:opacity-30">↑</button>
          <button disabled={saving || i === order.length - 1} onClick={() => move(i, 1)} className="text-zinc-500 hover:text-zinc-300 disabled:opacity-30">↓</button>
          <button disabled={saving} onClick={() => remove(i)} className="text-red-400 hover:text-red-300">×</button>
        </div>
      ))}
      {addable.length > 0 && (
        <select
          value=""
          disabled={saving}
          onChange={e => add(e.target.value)}
          className="input w-full text-xs"
        >
          <option value="">+ add scraper to priority…</option>
          {addable.map(s => <option key={s} value={s}>{s.toUpperCase()}</option>)}
        </select>
      )}
    </div>
  )
}

// Per-source-card pill listing accounts the source reports that have no
// local Pantheon account imported yet (see SystemStore.unmappedSourceUsers,
// populated globally by Layout's poll — this just filters the shared list
// rather than fetching its own copy). Expands inline to Import/Import All;
// no separate mapping/merge UI, per explicit scope.
const UnmappedUsersPill = observer(function UnmappedUsersPill({ sourceId }: { sourceId: string }) {
  const users = systemStore.unmappedSourceUsers.filter(u => u.source_id === sourceId)
  const [expanded, setExpanded]   = useState(false)
  const [importing, setImporting] = useState<string | null>(null) // external_user_id in flight
  const [bulkImporting, setBulkImporting] = useState(false)
  const [results, setResults]     = useState<Record<string, ImportUserResult>>({})

  if (users.length === 0) return null

  const doImport = async (externalUserId: string) => {
    setImporting(externalUserId)
    try {
      const result = await api.importSourceUser(sourceId, externalUserId)
      setResults(prev => ({ ...prev, [externalUserId]: result }))
      await systemStore.refreshUnmappedUsers()
    } catch (e: any) {
      setResults(prev => ({ ...prev, [externalUserId]: { ok: false, user_id: '', username: '', invite_method: 'temp_password', invite_error: e.message } as ImportUserResult }))
    } finally {
      setImporting(null)
    }
  }

  const importAll = async () => {
    setBulkImporting(true)
    try {
      for (const u of users) await doImport(u.external_user_id)
    } finally {
      setBulkImporting(false)
    }
  }

  return (
    <div onClick={e => e.stopPropagation()} className="pt-1">
      <button
        onClick={() => setExpanded(v => !v)}
        className="text-xs px-2 py-0.5 bg-amber-900/25 hover:bg-amber-800/35
                   text-amber-300 rounded border border-amber-700/30 transition-colors"
      >
        {users.length} unregistered {expanded ? '▲' : '▼'}
      </button>
      {expanded && (
        <div className="mt-2 space-y-1.5 bg-zinc-950/60 border border-amber-900/25 rounded-lg p-2.5">
          {users.map(u => {
            const result = results[u.external_user_id]
            return (
              <div key={u.external_user_id} className="text-xs space-y-1">
                <div className="flex items-center justify-between gap-2">
                  <span className="text-zinc-300 truncate">{u.display_name || u.external_user_id}</span>
                  {!result && (
                    <button
                      onClick={() => doImport(u.external_user_id)}
                      disabled={importing === u.external_user_id || bulkImporting}
                      className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                                 text-violet-300 rounded border border-violet-800/30
                                 disabled:opacity-40 transition-colors shrink-0"
                    >
                      {importing === u.external_user_id ? 'Importing…' : 'Import'}
                    </button>
                  )}
                </div>
                {result && (
                  result.ok ? (
                    <div className="bg-emerald-950/30 border border-emerald-800/30 rounded px-2 py-1.5 text-emerald-300">
                      {result.invite_method === 'temp_password' ? (
                        <>
                          Account created. Temp password (share once, then discard):{' '}
                          <code className="text-emerald-200 select-all">{result.temp_password}</code>
                        </>
                      ) : (
                        <>Account created. Invite email {result.invite_sent ? 'sent' : `could not be sent (${result.invite_error ?? 'unknown error'})`} — link: <code className="text-emerald-200 select-all break-all">{result.invite_link}</code></>
                      )}
                    </div>
                  ) : (
                    <div className="bg-red-950/30 border border-red-800/30 rounded px-2 py-1.5 text-red-300">
                      Import failed{result.invite_error ? `: ${result.invite_error}` : ''}
                    </div>
                  )
                )}
              </div>
            )
          })}
          {users.length > 1 && (
            <button
              onClick={importAll}
              disabled={bulkImporting || importing !== null}
              className="text-xs px-2 py-0.5 bg-amber-900/25 hover:bg-amber-800/35
                         text-amber-300 rounded border border-amber-700/30
                         disabled:opacity-40 transition-colors"
            >
              {bulkImporting ? 'Importing all…' : 'Import All'}
            </button>
          )}
        </div>
      )}
    </div>
  )
})

// Lists every account this source has ever reported (mapped or not) and lets
// an admin link one to an *existing* Pantheon user for per-account watch-data
// sync — separate from UnmappedUsersPill above, which only ever creates
// brand-new accounts. Renders nothing while loading or if the source hasn't
// discovered any other accounts yet (nothing to manage).
const PerAccountWatchSyncCard = observer(function PerAccountWatchSyncCard({ sourceId, sourceType }: { sourceId: string; sourceType: string }) {
  const [users, setUsers]     = useState<SourceUser[]>([])
  const [loading, setLoading] = useState(true)
  const [pending, setPending] = useState<string | null>(null) // external_user_id in flight

  const load = () => {
    api.getSourceUsers(sourceId)
      .then(u => { setUsers(u); setLoading(false) })
      .catch(() => setLoading(false))
  }
  useEffect(() => { setLoading(true); load() }, [sourceId])

  const isPlex = sourceType === 'plex'

  const handleChange = async (externalUserId: string, userId: string) => {
    setPending(externalUserId)
    try {
      if (userId) await api.linkSourceUser(sourceId, externalUserId, userId)
      else        await api.unlinkSourceUser(sourceId, externalUserId)
      load()
    } finally {
      setPending(null)
    }
  }

  if (loading || users.length === 0) return null

  return (
    <div className="card p-3 space-y-2">
      <span className="section-label">Per-Account Watch Sync</span>
      <p className="text-xs text-zinc-600">
        Link any of this source's other accounts to an existing Pantheon user so their
        own watch history stays in sync too — independent of the single account above.
      </p>
      {isPlex && (
        <p className="text-xs text-amber-500/80">
          ⚠ Works for Plex Home members. "Friends" (separate, shared-in Plex accounts) can't
          be linked — Kairos never has their password/token, only Home profiles can be
          switched into using the admin account.
        </p>
      )}
      <div className="space-y-1.5">
        {users.map(u => (
          <div key={u.external_user_id} className="flex items-center justify-between gap-2">
            <span className="text-xs text-zinc-300 truncate">{u.display_name || u.external_user_id}</span>
            <select
              className="input text-xs min-w-[150px]"
              disabled={pending === u.external_user_id}
              value={u.imported_user_id}
              onChange={e => handleChange(u.external_user_id, e.target.value)}
            >
              <option value="">— unlinked —</option>
              {userStore.users.map(usr => (
                <option key={usr.user_id} value={usr.user_id}>{usr.username}</option>
              ))}
            </select>
          </div>
        ))}
      </div>
    </div>
  )
})

export default observer(function SourcesPage() {
  const store = sourceStore

  // ── Add-source form ────────────────────────────────────────────────────────
  const [showAdd, setShowAdd]   = useState(false)
  const [form, setForm]         = useState({ source_id: '', source_type: 'plex', display_name: '', base_url: '', token: '', user_id: '', sync_priority: 0 })
  const [testState, setTest]    = useState<TestState>('idle')
  const [testError, setTestErr] = useState('')

  const updateForm = (patch: Partial<typeof form>) => {
    setForm(prev => ({ ...prev, ...patch }))
    if ('source_type' in patch || 'base_url' in patch || 'token' in patch || 'user_id' in patch) {
      setTest('idle'); setTestErr('')
    }
  }

  const runTest = async () => {
    setTest('testing'); setTestErr('')
    try {
      const r = await api.testSource({ source_type: form.source_type, base_url: form.base_url, token: form.token, user_id: form.user_id })
      r.ok ? setTest('ok') : (setTest('failed'), setTestErr(r.error ?? 'Connection failed'))
    } catch (e: any) {
      setTest('failed'); setTestErr(e.message)
    }
  }

  const addSource = async () => {
    await store.addSource({ source_id: form.source_id, source_type: form.source_type as any, display_name: form.display_name, base_url: form.base_url, sync_priority: form.sync_priority })
    if (form.token) await store.setCredentials(form.source_id, form.token, form.user_id)
    setShowAdd(false)
    setForm({ source_id: '', source_type: 'plex', display_name: '', base_url: '', token: '', user_id: '', sync_priority: 0 })
    setTest('idle'); setTestErr('')
  }

  const cancelAdd = () => { setShowAdd(false); setTest('idle'); setTestErr('') }

  const needsToken   = NEEDS_TOKEN.includes(form.source_type)
  const needsUserId  = NEEDS_USER_ID.includes(form.source_type)
  const canTest      = needsToken
    ? (!!form.base_url && !!form.token && (!needsUserId || !!form.user_id))
    : !!form.base_url
  const saveReady    = !!form.source_id && !!form.display_name && (!needsToken || testState === 'ok')

  // ── Add-library form ───────────────────────────────────────────────────────
  const [showAddLib, setShowAddLib] = useState(false)
  const [libForm, setLibForm]       = useState({ external_lib_id: '', display_name: '', library_type: 'show' as 'show' | 'movie' | 'mixed' | 'music' | 'photo', preferred_scraper: '' as '' | 'tmdb' | 'tvdb' | 'anidb' | 'tvmaze' | 'trakt' | 'anilist' | 'wikidata', preferred_language: '', include_anidb: false })

  // ── Library edit state ─────────────────────────────────────────────────────
  const [editingLib, setEditingLib] = useState<string | null>(null)
  const [editForm, setEditForm]     = useState({ display_name: '', library_type: 'show' as Library['library_type'], preferred_scraper: '' as '' | 'tmdb' | 'tvdb' | 'anidb' | 'tvmaze' | 'trakt' | 'anilist' | 'wikidata', preferred_language: '', include_anidb: false, show_on_home: true, skip_scraping: false })

  // ── Local folder browser ───────────────────────────────────────────────────
  const [localBrowsePath,    setLocalBrowsePath]   = useState('')
  const [localEntries,       setLocalEntries]       = useState<LibraryInfo[]>([])
  const [localBrowseLoading, setLocalBrowseLoading] = useState(false)

  const browseTo = async (path: string) => {
    if (!store.selectedId) return
    setLocalBrowseLoading(true)
    try {
      const entries = await api.browseLocalDir(store.selectedId, path)
      setLocalBrowsePath(path)
      setLocalEntries(entries)
    } catch {}
    finally { setLocalBrowseLoading(false) }
  }

  const addLib = async () => {
    if (!store.selectedId) return
    await store.addLibrary(store.selectedId, libForm.external_lib_id, libForm.display_name, libForm.library_type, libForm.preferred_scraper, libForm.preferred_language, libForm.include_anidb)
    setShowAddLib(false)
    setLibForm({ external_lib_id: '', display_name: '', library_type: 'show', preferred_scraper: '', preferred_language: '', include_anidb: false })
    setLocalBrowsePath(''); setLocalEntries([])
  }

  const openEditLib = (lib: { library_id: string; display_name: string; library_type: Library['library_type']; preferred_scraper: '' | 'tmdb' | 'tvdb' | 'anidb' | 'tvmaze' | 'trakt' | 'anilist' | 'wikidata'; preferred_language: string; include_anidb: boolean; show_on_home: boolean; skip_scraping: boolean }) => {
    setEditingLib(lib.library_id)
    setEditForm({ display_name: lib.display_name, library_type: lib.library_type, preferred_scraper: lib.preferred_scraper, preferred_language: lib.preferred_language ?? '', include_anidb: lib.include_anidb, show_on_home: lib.show_on_home, skip_scraping: lib.skip_scraping })
    setConfirmLib(null)
  }

  const saveEditLib = async () => {
    if (!store.selectedId || !editingLib) return
    await store.updateLibrary(store.selectedId, editingLib, editForm)
    setEditingLib(null)
  }

  // ── Credential editor ──────────────────────────────────────────────────────
  const [editingCreds, setEditingCreds] = useState(false)
  const [credToken, setCredToken]       = useState('')
  const [credUserId, setCredUserId]     = useState('')

  // ── Confirm-remove state ───────────────────────────────────────────────────
  const [confirmSrc, setConfirmSrc]   = useState<string | null>(null)  // source_id pending removal
  const [confirmLib, setConfirmLib]   = useState<string | null>(null)  // library_id pending removal
  const [confirmPm,  setConfirmPm]    = useState<number  | null>(null) // path-map index pending removal
  const [confirmSync, setConfirmSync] = useState<string | null>(null) // source_id pending sync confirmation
  const [confirmHardSync, setConfirmHardSync] = useState<string | null>(null) // source_id pending hard-sync confirmation

  // ── Path maps ──────────────────────────────────────────────────────────────
  const [pathMaps, setPathMaps]   = useState<PathMap[]>([])
  const [samplePath, setSample]   = useState<string | null>(null)
  const [pmFrom, setPmFrom]       = useState('')
  const [pmTo, setPmTo]           = useState('')
  const [showAddPm, setShowAddPm] = useState(false)

  // ── Sync priority ─────────────────────────────────────────────────────────
  // Local staging so typing doesn't fire a PATCH per keystroke — persisted on blur.
  const [priorityDraft, setPriorityDraft] = useState<number | null>(null)

  useEffect(() => { setEditingCreds(false); setCredToken(''); setCredUserId(''); setEditingLib(null); setPriorityDraft(null) }, [store.selectedId])
  useEffect(() => {
    if (!store.selectedId) { setPathMaps([]); setSample(null); return }
    api.getPathMaps(store.selectedId).then(setPathMaps).catch(() => setPathMaps([]))
    api.getSamplePath(store.selectedId).then(r => setSample(r.path)).catch(() => setSample(null))
  }, [store.selectedId])
  useEffect(() => { store.fetchAll(); userStore.fetchAll() }, [])

  // Enabled scrapers, for the per-library priority editor's "add" list.
  const [enabledScrapers, setEnabledScrapers] = useState<string[]>([])
  useEffect(() => {
    api.getScraperSettings()
      .then(s => setEnabledScrapers(s.configs.filter(c => c.enabled).map(c => c.source)))
      .catch(() => setEnabledScrapers([]))
  }, [])

  const savePathMaps = async (maps: PathMap[]) => {
    if (!store.selectedId) return
    await api.setPathMaps(store.selectedId, maps)
    setPathMaps(maps)
  }

  const addPathMap = async () => {
    if (!pmFrom) return
    await savePathMaps([...pathMaps, { from: pmFrom, to: pmTo }])
    setPmFrom(''); setPmTo(''); setShowAddPm(false)
  }

  const removePathMap = async (idx: number) => {
    await savePathMaps(pathMaps.filter((_, i) => i !== idx))
  }

  // ── Render ─────────────────────────────────────────────────────────────────
  return (
    <div className="space-y-5">
      {/* Not while showAdd is open — the spotlight's dimmed backdrop would
          otherwise darken the very form fields the admin needs to fill in;
          it's only meant to point at the button before they've clicked it. */}
      {tourStore.currentStep?.route === '/sources' && !showAdd && <TourSpotlight step={tourStore.currentStep} />}
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Media Sources</h1>
        <button onClick={() => setShowAdd(v => !v)} className="btn-primary" data-tour="add-source-btn">
          + Add Source
        </button>
      </div>

      {store.error && (
        <div className="text-red-400 text-sm bg-red-950/30 border border-red-900/40 rounded-lg p-3">
          {store.error}
        </div>
      )}

      {/* ── Add source form ─────────────────────────────────────────────── */}
      {showAdd && (
        <div className="card p-4 space-y-4">
          <h2 className="section-label">New Source</h2>
          <div className="grid grid-cols-2 gap-3">
            <input
              placeholder="source_id  (e.g. plex_home)"
              value={form.source_id}
              onChange={e => updateForm({ source_id: e.target.value })}
              className="input"
            />
            <select
              value={form.source_type}
              onChange={e => updateForm({ source_type: e.target.value })}
              className="input"
            >
              {store.sourceTypes.map(t => (
                <option key={t.type} value={t.type} disabled={!t.supported}>
                  {t.display_name}{!t.supported ? ' (coming soon)' : ''}
                </option>
              ))}
            </select>
            <input
              placeholder="Display name"
              value={form.display_name}
              onChange={e => updateForm({ display_name: e.target.value })}
              className="input"
            />
            <input
              placeholder={
                form.source_type === 'local'
                  ? 'Path  (e.g. /media/library)'
                  : 'Base URL  (e.g. http://plex:32400)'
              }
              value={form.base_url}
              onChange={e => updateForm({ base_url: e.target.value })}
              className="input"
            />
            {needsToken && (
              <input
                placeholder="Auth token"
                type="password"
                value={form.token}
                onChange={e => updateForm({ token: e.target.value })}
                className={needsUserId ? 'input' : 'input col-span-2'}
              />
            )}
            {needsUserId && (
              <input
                placeholder="User ID"
                value={form.user_id}
                onChange={e => updateForm({ user_id: e.target.value })}
                className="input"
              />
            )}
            <input
              type="number"
              placeholder="Sync priority (lower syncs/wins first, 0 default)"
              value={form.sync_priority}
              onChange={e => updateForm({ sync_priority: Number(e.target.value) })}
              className="input"
            />
            <SourceHelpGuide sourceType={form.source_type} />
          </div>

          {needsToken && (
            <div className="flex items-center gap-3">
              <button
                onClick={runTest}
                disabled={!canTest || testState === 'testing'}
                className="btn-secondary disabled:opacity-40"
              >
                {testState === 'testing' ? 'Testing…' : 'Test Connection'}
              </button>
              {testState === 'ok' && (
                <span className="text-xs text-emerald-400">✓ Connected</span>
              )}
              {testState === 'failed' && (
                <span className="text-xs text-red-400 truncate max-w-xs" title={testError}>
                  ✗ {testError}
                </span>
              )}
            </div>
          )}

          <div className="flex gap-2 pt-1">
            <button
              onClick={addSource}
              disabled={!saveReady}
              title={needsToken && testState !== 'ok' ? 'Run a successful connection test first' : undefined}
              className="btn-primary disabled:opacity-40 disabled:cursor-not-allowed"
            >
              Save
            </button>
            <button onClick={cancelAdd} className="btn-ghost">Cancel</button>
          </div>
        </div>
      )}

      <div className="grid grid-cols-2 gap-5">
        {/* Source list */}
        <div className="space-y-2">
          {store.sources.length === 0 && !store.loading && (
            <p className="text-zinc-600 text-sm">No sources configured yet — click + Add Source above to connect your media library.</p>
          )}
          {store.sources.map(src => (
            <div
              key={src.source_id}
              onClick={() => store.select(src.source_id)}
              className={`cursor-pointer rounded-lg border p-3 space-y-2 transition-all duration-150 ${
                store.selectedId === src.source_id
                  ? 'border-amber-500/40 bg-amber-500/5 ring-1 ring-amber-500/10'
                  : 'border-violet-900/30 bg-zinc-900 hover:border-violet-700/50'
              }`}
            >
              <div className="flex items-center justify-between">
                <span className="font-medium text-sm text-zinc-100">{src.display_name}</span>
                <span className="text-[10px] text-violet-500/70 uppercase tracking-widest">
                  {src.source_type}
                </span>
              </div>
              <div className="text-xs text-zinc-600 truncate">{src.base_url}</div>
              {src.source_type !== 'local' && src.user_sync_error && (
                <div className="text-xs text-amber-500/80" title="listing this server's other accounts failed — content sync itself is unaffected">
                  ⚠ Couldn't list server users: {src.user_sync_error}
                </div>
              )}
              <div className="flex gap-2 pt-0.5">
                {confirmSync === src.source_id ? (
                  <span className="flex items-center gap-1.5 text-xs">
                    <span className="text-amber-400">Path maps recommended. Sync anyway?</span>
                    <button
                      onClick={e => { e.stopPropagation(); store.triggerSync(src.source_id); setConfirmSync(null) }}
                      className="px-2 py-0.5 rounded bg-amber-900/60 border border-amber-700/50 text-amber-200 hover:bg-amber-800/60 transition-colors"
                    >Yes</button>
                    <button
                      onClick={e => { e.stopPropagation(); setConfirmSync(null) }}
                      className="px-2 py-0.5 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
                    >No</button>
                  </span>
                ) : (
                  <button
                    onClick={async e => {
                      e.stopPropagation()
                      let hasMaps = pathMaps.length > 0
                      // If we're clicking sync on a source that isn't currently selected,
                      // we need to check its path maps specifically.
                      if (store.selectedId !== src.source_id) {
                        try {
                          const maps = await api.getPathMaps(src.source_id)
                          hasMaps = maps.length > 0
                        } catch { hasMaps = true } // fallback to allowing if API fails
                      }

                      if (!hasMaps && src.source_type !== 'local') {
                        setConfirmSync(src.source_id)
                      } else {
                        store.triggerSync(src.source_id)
                      }
                    }}
                    disabled={store.syncing}
                    data-tour="sync-source-btn"
                    className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                               text-violet-300 rounded border border-violet-800/30
                               disabled:opacity-40 transition-colors"
                  >
                    {store.syncing ? 'Syncing…' : 'Sync'}
                  </button>
                )}
                {confirmHardSync === src.source_id ? (
                  <span className="flex items-center gap-1.5 text-xs">
                    <span className="text-orange-400">
                      Re-resolves every item from scratch, like the first sync ever. Also drops
                      any manual cross-source links for this source. Continue?
                    </span>
                    <button
                      onClick={e => { e.stopPropagation(); store.triggerHardSync(src.source_id); setConfirmHardSync(null) }}
                      className="px-2 py-0.5 rounded bg-orange-900/60 border border-orange-700/50 text-orange-200 hover:bg-orange-800/60 transition-colors"
                    >Yes</button>
                    <button
                      onClick={e => { e.stopPropagation(); setConfirmHardSync(null) }}
                      className="px-2 py-0.5 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
                    >No</button>
                  </span>
                ) : (
                  <button
                    onClick={e => { e.stopPropagation(); setConfirmHardSync(src.source_id) }}
                    disabled={store.syncing}
                    title="Forces a full recheck of this source, as if it were being synced for the first time"
                    className="text-xs px-2 py-0.5 bg-orange-900/30 hover:bg-orange-800/40
                               text-orange-300 rounded border border-orange-800/30
                               disabled:opacity-40 transition-colors"
                  >
                    Hard Sync
                  </button>
                )}
                {confirmSrc === src.source_id ? (
                  <span className="flex items-center gap-1.5 text-xs">
                    <span className="text-red-400">Remove source + all libraries?</span>
                    <button
                      onClick={e => { e.stopPropagation(); store.removeSource(src.source_id); setConfirmSrc(null) }}
                      className="px-2 py-0.5 rounded bg-red-900/60 border border-red-700/50 text-red-300 hover:bg-red-800/60 transition-colors"
                    >Yes</button>
                    <button
                      onClick={e => { e.stopPropagation(); setConfirmSrc(null) }}
                      className="px-2 py-0.5 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
                    >No</button>
                  </span>
                ) : (
                  <button
                    onClick={e => { e.stopPropagation(); setConfirmSrc(src.source_id) }}
                    className="btn-danger"
                  >
                    Remove
                  </button>
                )}
              </div>
              <UnmappedUsersPill sourceId={src.source_id} />
            </div>
          ))}
        </div>

        {/* Detail panel */}
        {store.selectedId && (
          <div className="space-y-4">
            {/* Credentials */}
            {NEEDS_TOKEN.includes(store.selected?.source_type ?? '') && (
              <div className="card p-3 space-y-2">
                <div className="flex items-center justify-between">
                  <span className="section-label">Credentials</span>
                  {!editingCreds && (
                    <button
                      onClick={() => { setEditingCreds(true); setCredToken(''); setCredUserId('') }}
                      className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                                 text-violet-300 rounded border border-violet-800/30 transition-colors"
                    >
                      {store.credentials[store.selectedId]?.has_token ? 'Update' : 'Set Token'}
                    </button>
                  )}
                </div>

                {!editingCreds ? (
                  <div className="flex items-center gap-2">
                    {store.credentials[store.selectedId]?.has_token ? (
                      <>
                        <span className="text-xs text-emerald-400">● Stored in kairos.conf</span>
                        <button
                          onClick={() => store.deleteCredentials(store.selectedId!)}
                          className="text-xs text-red-500 hover:text-red-400 ml-auto transition-colors"
                        >
                          Remove
                        </button>
                      </>
                    ) : (
                      <span className="text-xs text-zinc-600">○ Not configured</span>
                    )}
                  </div>
                ) : (
                  <div className="space-y-2">
                    <input
                      placeholder="Paste token"
                      type="password"
                      value={credToken}
                      onChange={e => setCredToken(e.target.value)}
                      className="input w-full"
                    />
                    {NEEDS_USER_ID.includes(store.selected?.source_type ?? '') && (
                      <input
                        placeholder="User ID"
                        value={credUserId}
                        onChange={e => setCredUserId(e.target.value)}
                        className="input w-full"
                      />
                    )}
                    <div className="flex gap-2">
                      <button
                        onClick={async () => {
                          if (credToken) await store.setCredentials(store.selectedId!, credToken, credUserId || undefined)
                          setEditingCreds(false)
                        }}
                        disabled={!credToken}
                        className="btn-primary disabled:opacity-40"
                      >
                        Save
                      </button>
                      <button onClick={() => setEditingCreds(false)} className="btn-ghost">
                        Cancel
                      </button>
                    </div>
                  </div>
                )}
              </div>
            )}

            {/* Sync priority */}
            <div className="card p-3 space-y-2">
              <span className="section-label">Sync Priority</span>
              <p className="text-xs text-zinc-600">
                Lower syncs first. When the same show or movie is matched across
                multiple sources, the higher-priority (lower-numbered) source's
                data wins conflicts — a lower-priority source only fills in
                fields it left empty, it's never locked out entirely.
              </p>
              <input
                type="number"
                value={priorityDraft ?? store.selected?.sync_priority ?? 0}
                onChange={e => setPriorityDraft(Number(e.target.value))}
                onBlur={() => {
                  if (priorityDraft !== null && priorityDraft !== store.selected?.sync_priority)
                    store.setSyncPriority(store.selectedId!, priorityDraft)
                }}
                className="input w-full"
              />
            </div>

            {/* Writeback settings */}
            {store.selected?.source_type !== 'local' && (
              <div className="card p-3 space-y-2">
                <span className="section-label">Writeback</span>
                <p className="text-xs text-zinc-600">
                  Pushes confirmed metadata back to this source. Manual (the item's own "Push
                  to Sources" button) and bulk ("Writeback All" on the Activity page) always
                  work regardless of the setting below — Auto only controls whether a scraper
                  match confirmation or refresh also pushes here on its own.
                </p>
                <label className="flex items-center gap-2 text-sm text-zinc-400">
                  <input
                    type="checkbox"
                    checked={store.selected?.auto_writeback ?? false}
                    onChange={e => store.setAutoWriteback(store.selectedId!, e.target.checked)}
                  />
                  Auto writeback on match confirm/refresh
                </label>
                <div className="pl-1 pt-1 space-y-2 border-l border-zinc-800 ml-1">
                  <p className="text-[11px] text-zinc-600 pl-2">
                    Applies whenever writeback runs against this source, auto or manual:
                  </p>
                  <label className="flex items-center gap-2 text-sm text-zinc-400 pl-2">
                    <input
                      type="checkbox"
                      checked={store.selected?.writeback_update_art ?? true}
                      onChange={e => store.setWritebackUpdateArt(store.selectedId!, e.target.checked)}
                    />
                    Update poster/backdrop art
                  </label>
                  <label className="flex items-center gap-2 text-sm text-zinc-400 pl-2">
                    <input
                      type="checkbox"
                      checked={store.selected?.writeback_update_external_ids ?? true}
                      onChange={e => store.setWritebackUpdateExternalIds(store.selectedId!, e.target.checked)}
                    />
                    Update external IDs (IMDb/TVDB/TMDB)
                  </label>
                  <label className="flex items-center gap-2 text-sm text-zinc-400 pl-2">
                    <input
                      type="checkbox"
                      checked={store.selected?.writeback_update_collections ?? true}
                      onChange={e => store.setWritebackUpdateCollections(store.selectedId!, e.target.checked)}
                    />
                    Update collections
                  </label>
                </div>
              </div>
            )}

            {/* Watch history sync */}
            {store.selected?.source_type !== 'local' && (
              <div className="card p-3 space-y-2">
                <span className="section-label">Watch History Sync</span>
                <p className="text-xs text-zinc-600">
                  Seed a Pantheon user's Continue Watching from this source's watch/resume state
                  during sync. Only ever fills in progress that isn't already there — never
                  overwrites what you've watched through Hades itself.
                </p>
                <select
                  value={store.selected?.synced_user_id ?? ''}
                  onChange={e => store.setSyncedUser(store.selectedId!, e.target.value)}
                  className="input w-full"
                >
                  <option value="">— off —</option>
                  {userStore.users.map(u => (
                    <option key={u.user_id} value={u.user_id}>{u.username}</option>
                  ))}
                </select>
              </div>
            )}

            {/* Per-account watch sync */}
            {store.selected?.source_type !== 'local' && (
              <PerAccountWatchSyncCard sourceId={store.selectedId!} sourceType={store.selected!.source_type} />
            )}

            {/* Path Maps */}
            <div className="card p-3 space-y-2">
              <div className="flex items-center justify-between">
                <span className="section-label">Path Maps</span>
                <button
                  onClick={() => setShowAddPm(v => !v)}
                  className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                             text-violet-300 rounded border border-violet-800/30 transition-colors"
                >
                  + Add
                </button>
              </div>

              {samplePath !== null && (
                <div className="text-[10px] text-zinc-600 font-mono break-all leading-relaxed
                                bg-zinc-900/60 border border-zinc-800/40 rounded px-2 py-1.5">
                  <span className="text-zinc-500 not-italic font-sans">Example path: </span>
                  {samplePath}
                </div>
              )}

              {pathMaps.length === 0 && !showAddPm && (
                <span className="text-xs text-zinc-600">No path maps configured.</span>
              )}

              {pathMaps.map((pm, idx) => (
                <div key={idx} className="flex items-center gap-2 text-xs font-mono">
                  <span className="text-zinc-300 truncate flex-1">{pm.from}</span>
                  <span className="text-zinc-600 shrink-0">→</span>
                  <span className="text-zinc-300 truncate flex-1">{pm.to}</span>
                  {confirmPm === idx ? (
                    <span className="flex items-center gap-1 text-xs shrink-0">
                      <button
                        onClick={() => { removePathMap(idx); setConfirmPm(null) }}
                        className="px-1.5 py-0.5 rounded bg-red-900/60 border border-red-700/50 text-red-300 hover:bg-red-800/60 transition-colors"
                      >✓</button>
                      <button
                        onClick={() => setConfirmPm(null)}
                        className="px-1.5 py-0.5 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
                      >✕</button>
                    </span>
                  ) : (
                    <button
                      onClick={() => setConfirmPm(idx)}
                      className="btn-danger shrink-0"
                    >
                      ✕
                    </button>
                  )}
                </div>
              ))}

              {showAddPm && (
                <div className="space-y-2 pt-1 border-t border-zinc-800/40">
                  <div className="flex gap-2">
                    <input
                      placeholder="From (e.g. /data)"
                      value={pmFrom}
                      onChange={e => setPmFrom(e.target.value)}
                      className="input flex-1 font-mono text-xs"
                    />
                    <input
                      placeholder="To (e.g. /media)"
                      value={pmTo}
                      onChange={e => setPmTo(e.target.value)}
                      className="input flex-1 font-mono text-xs"
                    />
                  </div>
                  <div className="flex gap-2">
                    <button onClick={addPathMap} disabled={!pmFrom} className="btn-primary disabled:opacity-40">
                      Save
                    </button>
                    <button
                      onClick={() => { setShowAddPm(false); setPmFrom(''); setPmTo('') }}
                      className="btn-ghost"
                    >
                      Cancel
                    </button>
                  </div>
                </div>
              )}
            </div>

            {/* Libraries */}
            <div className="space-y-3">
              <div className="flex items-center justify-between">
                <h2 className="text-sm font-semibold text-zinc-300">
                  Libraries — {store.selected?.display_name}
                </h2>
                <button
                  onClick={() => {
                    const opening = !showAddLib
                    setShowAddLib(v => !v)
                    if (opening) {
                      if (store.selected?.source_type === 'local') {
                        browseTo(store.selected.base_url)
                      } else {
                        store.fetchAvailable(store.selectedId!)
                      }
                    }
                  }}
                  className="text-xs px-2 py-1 bg-violet-900/30 hover:bg-violet-800/40
                             text-violet-300 rounded border border-violet-800/30 transition-colors"
                >
                  + Add
                </button>
              </div>

              {showAddLib && (() => {
                const isLocal = store.selected?.source_type === 'local'
                return (
                <div className="card p-3 space-y-2">
                  {isLocal ? (() => {
                    const basePath = store.selected?.base_url ?? ''
                    const relParts = localBrowsePath.startsWith(basePath)
                      ? localBrowsePath.slice(basePath.length).split('/').filter(Boolean)
                      : []
                    return (
                      <div className="space-y-2">
                        {/* Breadcrumb */}
                        <div className="flex items-center gap-1 text-[11px] font-mono text-zinc-500 flex-wrap">
                          <button
                            type="button"
                            onClick={() => browseTo(basePath)}
                            className="hover:text-violet-300 transition-colors"
                          >
                            {basePath.split('/').filter(Boolean).pop() ?? '/'}
                          </button>
                          {relParts.map((seg, i) => {
                            const p = basePath + '/' + relParts.slice(0, i + 1).join('/')
                            return (
                              <span key={i} className="flex items-center gap-1">
                                <span className="text-zinc-700">/</span>
                                <button type="button" onClick={() => browseTo(p)} className="hover:text-violet-300 transition-colors">{seg}</button>
                              </span>
                            )
                          })}
                        </div>

                        {/* Folder list */}
                        {localBrowseLoading ? (
                          <div className="text-xs text-zinc-600 py-1">Loading…</div>
                        ) : localEntries.length === 0 ? (
                          <div className="text-xs text-zinc-600 py-1">No subdirectories.</div>
                        ) : (
                          <div className="space-y-1 max-h-52 overflow-y-auto pr-0.5">
                            {localEntries.map(e => (
                              <div
                                key={e.external_lib_id}
                                className={`flex items-center gap-1 px-2.5 py-1.5 rounded-md border transition-colors ${
                                  libForm.external_lib_id === e.external_lib_id
                                    ? 'border-amber-500/40 bg-amber-500/5 ring-1 ring-amber-500/10'
                                    : 'border-zinc-800/40 hover:border-violet-700/40'
                                }`}
                              >
                                <button
                                  type="button"
                                  className="flex-1 text-left min-w-0"
                                  onClick={() => setLibForm({ external_lib_id: e.external_lib_id, display_name: e.name, library_type: e.type as any, preferred_scraper: '', preferred_language: '', include_anidb: libForm.include_anidb })}
                                >
                                  <span className="text-xs text-zinc-200 truncate block">{e.name}</span>
                                </button>
                                <span className="text-[10px] text-violet-500/60 uppercase tracking-widest shrink-0">{e.type}</span>
                                <button
                                  type="button"
                                  title="Browse into folder"
                                  onClick={() => browseTo(e.external_lib_id)}
                                  className="text-zinc-600 hover:text-violet-300 transition-colors px-1 shrink-0 text-sm leading-none"
                                >›</button>
                              </div>
                            ))}
                          </div>
                        )}
                      </div>
                    )
                  })() : (
                    store.available.length > 0 ? (
                      <select
                        value={libForm.external_lib_id}
                        onChange={e => {
                          const lib = store.available.find(l => l.external_lib_id === e.target.value)
                          setLibForm({ external_lib_id: e.target.value, display_name: lib?.name ?? libForm.display_name, library_type: (lib?.type ?? 'show') as any, preferred_scraper: '', preferred_language: '', include_anidb: libForm.include_anidb })
                        }}
                        className="input w-full"
                      >
                        <option value="">— select from server —</option>
                        {store.available.map(l => (
                          <option key={l.external_lib_id} value={l.external_lib_id}>
                            {l.name} ({l.type})
                          </option>
                        ))}
                      </select>
                    ) : (
                      <div className="space-y-1">
                        <input
                          placeholder="Library ID (e.g. 1, 2, 3 — from your media server)"
                          value={libForm.external_lib_id}
                          onChange={e => setLibForm({ ...libForm, external_lib_id: e.target.value })}
                          className="input w-full font-mono"
                        />
                        <p className="text-[10px] text-zinc-600">
                          Server unavailable — enter the library ID manually. For Plex this is a section number like 1 or 2.
                        </p>
                      </div>
                    )
                  )}
                  <input
                    placeholder="Display name"
                    value={libForm.display_name}
                    onChange={e => setLibForm({ ...libForm, display_name: e.target.value })}
                    className="input w-full"
                  />
                  {!isLocal && (
                    <select
                      value={libForm.library_type}
                      onChange={e => setLibForm({ ...libForm, library_type: e.target.value as any })}
                      className="input w-full"
                    >
                      <option value="show">TV Shows</option>
                      <option value="movie">Movies</option>
                      <option value="mixed">Mixed</option>
                      <option value="music">Music</option>
                      <option value="photo">Photos</option>
                    </select>
                  )}
                  <label className="flex items-center gap-2 text-sm text-zinc-400">
                    <input
                      type="checkbox"
                      checked={libForm.include_anidb}
                      onChange={e => setLibForm({ ...libForm, include_anidb: e.target.checked })}
                    />
                    Include AniDB (anime only — leave off for general movie/TV libraries)
                  </label>
                  <select
                    value={libForm.preferred_language}
                    onChange={e => setLibForm({ ...libForm, preferred_language: e.target.value })}
                    className="input w-full"
                  >
                    <option value="">Language — scraper default</option>
                      {LANGUAGE_OPTIONS.map(l => <option key={l.value} value={l.value}>{l.label}</option>)}
                  </select>
                  <div className="flex gap-2">
                    <button
                      onClick={addLib}
                      disabled={!libForm.external_lib_id || !libForm.display_name}
                      className="btn-primary disabled:opacity-40"
                    >Save</button>
                    <button onClick={() => setShowAddLib(false)} className="btn-ghost">Cancel</button>
                  </div>
                </div>
                )
              })()}

              {store.libraries.length === 0 && (
                <p className="text-zinc-600 text-sm">No libraries added yet.</p>
              )}
              {store.libraries.map(lib => (
                <div key={lib.library_id} className="card px-3 py-2.5 space-y-2">
                  {/* Header row — always visible */}
                  <div className="flex items-center justify-between">
                    <div className="flex-1 min-w-0 mr-3">
                      <div className="text-sm text-zinc-200">{lib.display_name}</div>
                      <div className="text-xs text-zinc-600 mt-0.5 flex items-center gap-1.5">
                        <span>{lib.library_type}</span>
                        <span>·</span>
                        <span>id: {lib.external_lib_id}</span>
                        {lib.preferred_scraper && (
                          <><span>·</span><span className="text-zinc-500">prefers {lib.preferred_scraper}</span></>
                        )}
                        {lib.preferred_language && (
                          <><span>·</span><span className="text-zinc-500">{lib.preferred_language}</span></>
                        )}
                        {!lib.show_on_home && (
                          <><span>·</span><span className="text-amber-500">hidden from Home</span></>
                        )}
                        {lib.skip_scraping && (
                          <><span>·</span><span className="text-zinc-500">scraping off</span></>
                        )}
                      </div>
                    </div>
                    <div className="flex items-center gap-1.5 shrink-0">
                      {editingLib !== lib.library_id && confirmLib !== lib.library_id && (
                        <button
                          onClick={() => store.triggerLibrarySync(store.selectedId!, lib.library_id)}
                          disabled={store.syncing}
                          title="New/changed content in just this library — quicker than a full source sync, but removals need a full Sync to be noticed"
                          className="px-2 py-0.5 rounded text-xs bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:text-zinc-200 hover:bg-zinc-700 disabled:opacity-40 transition-colors"
                        >{store.syncing ? 'Syncing…' : 'Sync'}</button>
                      )}
                      {editingLib !== lib.library_id && confirmLib !== lib.library_id && (
                        <button
                          onClick={() => openEditLib(lib)}
                          className="px-2 py-0.5 rounded text-xs bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:text-zinc-200 hover:bg-zinc-700 transition-colors"
                        >Edit</button>
                      )}
                      {confirmLib === lib.library_id ? (
                        <span className="flex items-center gap-1.5 text-xs">
                          <span className="text-red-400 shrink-0">Sure?</span>
                          <button
                            onClick={() => { store.removeLibrary(store.selectedId!, lib.library_id); setConfirmLib(null) }}
                            className="px-2 py-0.5 rounded bg-red-900/60 border border-red-700/50 text-red-300 hover:bg-red-800/60 transition-colors"
                          >Yes</button>
                          <button
                            onClick={() => setConfirmLib(null)}
                            className="px-2 py-0.5 rounded bg-zinc-800 border border-zinc-700/50 text-zinc-400 hover:bg-zinc-700 transition-colors"
                          >No</button>
                        </span>
                      ) : editingLib !== lib.library_id ? (
                        <button
                          onClick={() => { setConfirmLib(lib.library_id); setEditingLib(null) }}
                          className="btn-danger"
                        >Remove</button>
                      ) : null}
                    </div>
                  </div>

                  {/* Inline edit form — expands when this library is being edited */}
                  {editingLib === lib.library_id && (
                    <div className="space-y-1.5 pt-1 border-t border-zinc-700/40">
                      <input
                        value={editForm.display_name}
                        onChange={e => setEditForm({ ...editForm, display_name: e.target.value })}
                        placeholder="Display name"
                        className="input w-full text-sm"
                      />
                      <select
                        value={editForm.library_type}
                        onChange={e => setEditForm({ ...editForm, library_type: e.target.value as Library['library_type'] })}
                        className="input w-full text-sm"
                      >
                        <option value="show">Show</option>
                        <option value="movie">Movie</option>
                        <option value="mixed">Mixed (shows and movies together)</option>
                      </select>
                      {editForm.library_type !== lib.library_type && (
                        <p className="text-xs text-amber-500">
                          Changing this reclassifies the library's items on the next sync.
                        </p>
                      )}
                      {(lib.library_type === 'show' || lib.library_type === 'mixed') && (
                        <ScraperPriorityEditor
                          sourceId={store.selectedId!} libraryId={lib.library_id}
                          itemType="show" availableScrapers={enabledScrapers}
                        />
                      )}
                      {(lib.library_type === 'movie' || lib.library_type === 'mixed') && (
                        <ScraperPriorityEditor
                          sourceId={store.selectedId!} libraryId={lib.library_id}
                          itemType="movie" availableScrapers={enabledScrapers}
                        />
                      )}
                      <label className="flex items-center gap-2 text-sm text-zinc-400">
                        <input
                          type="checkbox"
                          checked={editForm.include_anidb}
                          onChange={e => setEditForm({ ...editForm, include_anidb: e.target.checked })}
                        />
                        Include AniDB (anime only)
                      </label>
                      <label className="flex items-center gap-2 text-sm text-zinc-400">
                        <input
                          type="checkbox"
                          checked={editForm.show_on_home}
                          onChange={e => setEditForm({ ...editForm, show_on_home: e.target.checked })}
                        />
                        Show in Home shelves
                      </label>
                      <label className="flex items-center gap-2 text-sm text-zinc-400">
                        <input
                          type="checkbox"
                          checked={editForm.skip_scraping}
                          onChange={e => setEditForm({ ...editForm, skip_scraping: e.target.checked })}
                        />
                        Skip scraping (filler, bumpers, home videos)
                      </label>
                      <select
                        value={editForm.preferred_language}
                        onChange={e => setEditForm({ ...editForm, preferred_language: e.target.value })}
                        className="input w-full text-sm"
                      >
                        <option value="">Language — scraper default</option>
                          {LANGUAGE_OPTIONS.map(l => <option key={l.value} value={l.value}>{l.label}</option>)}
                      </select>
                      <div className="flex gap-2 pt-0.5">
                        <button
                          onClick={saveEditLib}
                          disabled={!editForm.display_name}
                          className="btn-primary text-xs disabled:opacity-40"
                        >Save</button>
                        <button
                          onClick={() => setEditingLib(null)}
                          className="btn-ghost text-xs"
                        >Cancel</button>
                        <button
                          onClick={() => { setEditingLib(null); setConfirmLib(lib.library_id) }}
                          className="btn-danger text-xs ml-auto"
                        >Remove</button>
                      </div>
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>
        )}
      </div>
    </div>
  )
})
