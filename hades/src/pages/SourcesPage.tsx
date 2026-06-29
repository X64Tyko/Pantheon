import { observer } from 'mobx-react-lite'
import { useEffect, useState } from 'react'
import { api } from '../api/client'
import type { LibraryInfo, PathMap } from '../api/types'
import { sourceStore } from '../stores'

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

export default observer(function SourcesPage() {
  const store = sourceStore

  // ── Add-source form ────────────────────────────────────────────────────────
  const [showAdd, setShowAdd]   = useState(false)
  const [form, setForm]         = useState({ source_id: '', source_type: 'plex', display_name: '', base_url: '', token: '', user_id: '' })
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
    await store.addSource({ source_id: form.source_id, source_type: form.source_type as any, display_name: form.display_name, base_url: form.base_url })
    if (form.token) await store.setCredentials(form.source_id, form.token, form.user_id)
    setShowAdd(false)
    setForm({ source_id: '', source_type: 'plex', display_name: '', base_url: '', token: '', user_id: '' })
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
  const [libForm, setLibForm]       = useState({ external_lib_id: '', display_name: '', library_type: 'show' as 'show' | 'movie' | 'mixed' | 'music' | 'photo', preferred_scraper: '' as '' | 'tmdb' | 'tvdb' | 'anidb' })

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
    await store.addLibrary(store.selectedId, libForm.external_lib_id, libForm.display_name, libForm.library_type, libForm.preferred_scraper)
    setShowAddLib(false)
    setLibForm({ external_lib_id: '', display_name: '', library_type: 'show', preferred_scraper: '' })
    setLocalBrowsePath(''); setLocalEntries([])
  }

  // ── Credential editor ──────────────────────────────────────────────────────
  const [editingCreds, setEditingCreds] = useState(false)
  const [credToken, setCredToken]       = useState('')
  const [credUserId, setCredUserId]     = useState('')

  // ── Confirm-remove state ───────────────────────────────────────────────────
  const [confirmSrc, setConfirmSrc]   = useState<string | null>(null)  // source_id pending removal
  const [confirmLib, setConfirmLib]   = useState<string | null>(null)  // library_id pending removal
  const [confirmPm,  setConfirmPm]    = useState<number  | null>(null) // path-map index pending removal

  // ── Path maps ──────────────────────────────────────────────────────────────
  const [pathMaps, setPathMaps]   = useState<PathMap[]>([])
  const [samplePath, setSample]   = useState<string | null>(null)
  const [pmFrom, setPmFrom]       = useState('')
  const [pmTo, setPmTo]           = useState('')
  const [showAddPm, setShowAddPm] = useState(false)

  useEffect(() => { setEditingCreds(false); setCredToken(''); setCredUserId('') }, [store.selectedId])
  useEffect(() => {
    if (!store.selectedId) { setPathMaps([]); setSample(null); return }
    api.getPathMaps(store.selectedId).then(setPathMaps).catch(() => setPathMaps([]))
    api.getSamplePath(store.selectedId).then(r => setSample(r.path)).catch(() => setSample(null))
  }, [store.selectedId])
  useEffect(() => { store.fetchAll() }, [])

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
      <div className="flex items-center justify-between">
        <h1 className="text-xl font-semibold text-zinc-100">Media Sources</h1>
        <button onClick={() => setShowAdd(v => !v)} className="btn-primary">
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
            <p className="text-zinc-600 text-sm">No sources configured.</p>
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
              <div className="flex gap-2 pt-0.5">
                <button
                  onClick={e => { e.stopPropagation(); store.triggerSync(src.source_id) }}
                  disabled={store.syncing}
                  className="text-xs px-2 py-0.5 bg-violet-900/30 hover:bg-violet-800/40
                             text-violet-300 rounded border border-violet-800/30
                             disabled:opacity-40 transition-colors"
                >
                  {store.syncing ? 'Syncing…' : 'Sync'}
                </button>
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
                                  onClick={() => setLibForm({ external_lib_id: e.external_lib_id, display_name: e.name, library_type: e.type as any, preferred_scraper: '' })}
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
                          setLibForm({ external_lib_id: e.target.value, display_name: lib?.name ?? libForm.display_name, library_type: (lib?.type ?? 'show') as any, preferred_scraper: '' })
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
                  <select
                    value={libForm.preferred_scraper}
                    onChange={e => setLibForm({ ...libForm, preferred_scraper: e.target.value as any })}
                    className="input w-full"
                  >
                    <option value="">Scraper — auto (all enabled)</option>
                    <option value="tmdb">TMDB</option>
                    <option value="tvdb">TVDB</option>
                    <option value="anidb">AniDB</option>
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
                <div key={lib.library_id}
                     className="flex items-center justify-between card px-3 py-2.5">
                  <div className="flex-1 min-w-0 mr-3">
                    <div className="text-sm text-zinc-200">{lib.display_name}</div>
                    <div className="text-xs text-zinc-600 mt-0.5">
                      {lib.library_type} · id: {lib.external_lib_id}
                    </div>
                    <select
                      value={lib.preferred_scraper ?? ''}
                      onChange={e => store.updatePreferredScraper(store.selectedId!, lib.library_id, e.target.value as any)}
                      className="input text-[10px] mt-1.5 py-0.5 h-6 w-full"
                      title="Preferred scraper for this library"
                    >
                      <option value="">Auto (all enabled scrapers)</option>
                      <option value="tmdb">TMDB</option>
                      <option value="tvdb">TVDB</option>
                      <option value="anidb">AniDB</option>
                    </select>
                  </div>
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
                  ) : (
                    <button
                      onClick={() => setConfirmLib(lib.library_id)}
                      className="btn-danger"
                    >
                      Remove
                    </button>
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
