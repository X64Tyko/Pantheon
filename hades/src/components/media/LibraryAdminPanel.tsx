import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import { AccordionSection } from '../../channel/sections'
import { goldBtnStyle, inputStyle } from '../../channel/styles'
import type { GroupingCandidate, EpisodeGroup, MovieDetail, ShowDetail, ExternalId, ItemMetadata, SpecialCandidate, LinkedSpecial } from '../../api/types'

type Detail = ShowDetail | MovieDetail
type Section = 'details' | 'grouping' | 'specials' | 'edit' | null

const labelStyle: React.CSSProperties = {
  display: 'block', fontFamily: "'JetBrains Mono', monospace", fontSize: 9,
  letterSpacing: '0.08em', color: 'var(--hds-txt-3)', marginBottom: 5,
}

// ── Panel ────────────────────────────────────────────────────────────────────

export function LibraryAdminPanel({ id, content_type }: { id: string; content_type: 'show' | 'movie' }) {
  const { user } = useAuth()
  const isAdmin  = user?.role === 'admin'

  const [detail, setDetail] = useState<Detail | null>(null)
  const [open,   setOpen]   = useState<Section>(null)

  useEffect(() => {
    setDetail(null)
    setOpen(null)
    const p = content_type === 'show' ? api.getShow(id) : api.getMovie(id)
    p.then(setDetail).catch(() => {})
  }, [id, content_type])

  if (!detail) return null
  const isShow = content_type === 'show'
  const toggle = (s: Section) => setOpen(o => o === s ? null : s)

  return (
    <div style={{ marginTop: 32, maxWidth: 640 }}>
      <AccordionSection title="DETAILS" open={open === 'details'} onToggle={() => toggle('details')}>
        <DetailsSection detail={detail} isShow={isShow} isAdmin={isAdmin} onDetailChanged={setDetail} />
      </AccordionSection>

      {isShow && (
        <AccordionSection title="EPISODE GROUPING" open={open === 'grouping'} onToggle={() => toggle('grouping')}>
          <GroupingSection showId={(detail as ShowDetail).show_id} isAdmin={isAdmin} active={open === 'grouping'} />
        </AccordionSection>
      )}

      {isShow && (
        <AccordionSection title="SPECIALS" open={open === 'specials'} onToggle={() => toggle('specials')}>
          <SpecialsSection
            show={detail as ShowDetail}
            isAdmin={isAdmin}
            active={open === 'specials'}
            onShowChanged={setDetail}
          />
        </AccordionSection>
      )}

      {isAdmin && (
        <AccordionSection title="EDIT METADATA" open={open === 'edit'} onToggle={() => toggle('edit')}>
          <EditSection detail={detail} isShow={isShow} onSaved={setDetail} />
        </AccordionSection>
      )}
    </div>
  )
}

// ── Details ──────────────────────────────────────────────────────────────────

function DetailsSection({ detail, isShow, isAdmin, onDetailChanged }: { detail: Detail; isShow: boolean; isAdmin: boolean; onDetailChanged: (d: Detail) => void }) {
  const show  = isShow ? detail as ShowDetail : null
  const id    = isShow ? (detail as ShowDetail).show_id : (detail as MovieDetail).movie_id
  const type  = isShow ? 'show' : 'movie'

  const [metadata, setMetadata] = useState<ItemMetadata | null>(null)
  const [loading,  setLoading]  = useState(false)
  const [refreshing, setRefreshing] = useState(false)
  const [togglingScrape, setTogglingScrape] = useState(false)

  const handleToggleSkipScraping = async () => {
    const next = !detail.skip_scraping
    setTogglingScrape(true)
    try {
      if (isShow) await api.setShowSkipScraping(id, next)
      else        await api.setMovieSkipScraping(id, next)
      onDetailChanged({ ...detail, skip_scraping: next })
    } catch {}
    setTogglingScrape(false)
  }

  const loadMetadata = async () => {
    setLoading(true)
    try { setMetadata(await api.getItemMetadata(type, id)) } catch {}
    setLoading(false)
  }

  useEffect(() => { loadMetadata() }, [id, type])

  const plexUrl = detail.source_base_url && detail.external_id
    ? `${detail.source_base_url}/web/index.html#!/server/details?key=/library/metadata/${detail.external_id}`
    : null

  // Build links from currently loaded metadata priority
  const links = (metadata?.external_ids || []).map(eid => {
    if (eid.source === 'tmdb') return { label: 'TMDb', href: `https://www.themoviedb.org/${isShow ? 'tv' : 'movie'}/${eid.external_id}`, color: 'oklch(0.7 0.16 150)' }
    if (eid.source === 'tvdb') return { label: 'TVDb', href: `https://thetvdb.com/?id=${eid.external_id}&tab=series`, color: 'oklch(0.68 0.16 240)' }
    if (eid.source === 'imdb') return { label: 'IMDb', href: `https://www.imdb.com/title/${eid.external_id}`, color: 'oklch(0.78 0.15 84)' }
    if (eid.source === 'anidb') return { label: 'AniDB', href: `https://anidb.net/a/${eid.external_id}`, color: 'oklch(0.65 0.12 200)' }
    return null
  }).filter(Boolean) as { label: string; href: string; color: string }[]

  if (plexUrl) links.unshift({ label: 'Plex', href: plexUrl, color: 'var(--hds-gold)' })

  const handleRefresh = async () => {
    setRefreshing(true)
    try {
      await api.refreshItemMetadata(type, id)
      // Ideally we'd trigger a reload of the main detail too, but let's at least reload IDs
      await loadMetadata()
    } catch {}
    setRefreshing(false)
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 14, paddingTop: 6 }}>
      <div>
        <span style={labelStyle}>External Links</span>
        {links.length === 0 ? (
          <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
            No external IDs stored.
          </p>
        ) : (
          <div style={{ display: 'flex', gap: 8, flexWrap: 'wrap' }}>
            {links.map(l => (
              <a key={l.label} href={l.href} target="_blank" rel="noreferrer" style={{
                fontFamily: "'JetBrains Mono', monospace", fontSize: 10, padding: '4px 10px',
                borderRadius: 8, border: `1px solid ${l.color}`, color: l.color, textDecoration: 'none',
              }}>{l.label} ↗</a>
            ))}
          </div>
        )}
      </div>

      <div>
        <span style={labelStyle}>Scraper Priority & Identifiers</span>
        {loading ? (
           <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>Loading…</div>
        ) : (
          <ExternalIdEditor
            type={type}
            id={id}
            metadata={metadata}
            isAdmin={isAdmin}
            onChanged={setMetadata}
          />
        )}
      </div>

      <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
        <IdRow label="Kairos ID" value={id} />
        {detail.external_id && <IdRow label="Source ID" value={detail.external_id} />}
      </div>

      {(detail.source_base_url || detail.locked || isAdmin) && (
        <div>
          <span style={labelStyle}>Technical</span>
          {detail.source_base_url && <IdRow label="Source URL" value={detail.source_base_url} />}
          {detail.locked && (
            <div style={{
              marginTop: 6, fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              color: 'var(--hds-violet)', background: 'oklch(0.55 0.14 292 / 0.12)',
              border: '1px solid oklch(0.7 0.13 287 / 0.3)', borderRadius: 7, padding: '7px 10px',
            }}>Locked — manual edits won't be overwritten by future syncs.</div>
          )}
          {isAdmin && (
            <label style={{
              marginTop: 6, display: 'flex', alignItems: 'center', gap: 6,
              fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
              cursor: togglingScrape ? 'wait' : 'pointer',
            }}>
              <input
                type="checkbox"
                checked={detail.skip_scraping}
                disabled={togglingScrape}
                onChange={handleToggleSkipScraping}
              />
              Exclude from scraping (never sent to TMDB/TVDB)
            </label>
          )}
        </div>
      )}

      {isAdmin && (
        <div style={{ marginTop: 8 }}>
          <button onClick={handleRefresh} disabled={refreshing} style={{
            ...goldBtnStyle, fontSize: 10, padding: '6px 12px', opacity: refreshing ? 0.6 : 1, cursor: refreshing ? 'wait' : 'pointer'
          }}>
            {refreshing ? 'Refreshing…' : 'Refresh Metadata from Scrapers'}
          </button>
          <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', marginTop: 6 }}>
            Re-fetches overview, art, and ratings based on the priority above.
          </p>
        </div>
      )}
    </div>
  )
}

function IdRow({ label, value }: { label: string; value: string }) {
  return (
    <div style={{ display: 'flex', justifyContent: 'space-between', gap: 10, fontFamily: "'JetBrains Mono', monospace", fontSize: 10 }}>
      <span style={{ color: 'var(--hds-txt-3)', flexShrink: 0 }}>{label}</span>
      <span style={{ color: 'var(--hds-txt-2)', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>{value}</span>
    </div>
  )
}

export function ExternalIdEditor({ type, id, metadata, isAdmin, onChanged }: {
  type: 'show' | 'movie';
  id: string;
  metadata: ItemMetadata | null;
  isAdmin: boolean;
  onChanged: (m: ItemMetadata) => void;
}) {
  const [saving, setSaving] = useState(false)
  const [addValue, setAddValue] = useState('')
  const [addError, setAddError] = useState<string | null>(null)

  if (!metadata) return null

  // Accepts "source:id" (e.g. "tmdb:603") — the power-user shortcut for
  // dropping in a known id without going through search. If that source is
  // already linked, this updates its id in place rather than erroring or
  // creating a second row for the same source (item_external_id's key is
  // item_type+kairos_id+source, so a duplicate source can't coexist anyway).
  const addId = async () => {
    const raw = addValue.trim()
    const sep = raw.indexOf(':')
    if (sep < 1 || sep === raw.length - 1) {
      setAddError('Format: source:id — e.g. tmdb:603')
      return
    }
    const source = raw.slice(0, sep).trim().toLowerCase()
    const external_id = raw.slice(sep + 1).trim()
    if (!source || !external_id) {
      setAddError('Format: source:id — e.g. tmdb:603')
      return
    }

    setAddError(null)
    const existing = metadata.external_ids.find(eid => eid.source === source)
    const ids = existing
      ? metadata.external_ids.map(eid => eid.source === source ? { ...eid, external_id } : eid)
      : [...metadata.external_ids, { source, external_id, priority: metadata.external_ids.length + 1 }]

    setSaving(true)
    try {
      await api.setItemMetadata(type, id, { external_ids: ids })
      onChanged({ ...metadata, external_ids: ids })
      setAddValue('')
    } catch {
      setAddError('Failed to add.')
    }
    setSaving(false)
  }

  const move = async (index: number, delta: number) => {
    if (!isAdmin) return
    const ids = [...metadata.external_ids]
    const target = index + delta
    if (target < 0 || target >= ids.length) return

    const temp = ids[index]
    ids[index] = ids[target]
    ids[target] = temp

    // Re-assign priorities based on new order
    const updated = ids.map((eid, i) => ({ ...eid, priority: i + 1 }))
    
    setSaving(true)
    try {
      await api.setItemMetadata(type, id, { external_ids: updated })
      onChanged({ ...metadata, external_ids: updated })
    } catch {}
    setSaving(false)
  }

  const remove = async (index: number) => {
    if (!isAdmin) return
    const ids = metadata.external_ids.filter((_, i) => i !== index)
    const updated = ids.map((eid, i) => ({ ...eid, priority: i + 1 }))

    setSaving(true)
    try {
      await api.setItemMetadata(type, id, { external_ids: updated })
      onChanged({ ...metadata, external_ids: updated })
    } catch {}
    setSaving(false)
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 4, marginTop: 4 }}>
      {metadata.external_ids.length === 0 && (
        <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)', padding: '4px 0' }}>
          No scraper IDs linked.
        </div>
      )}
      {metadata.external_ids.sort((a,b) => a.priority - b.priority).map((eid, i) => (
        <div key={`${eid.source}:${eid.external_id}`} style={{
          display: 'flex', alignItems: 'center', gap: 8, padding: '4px 8px',
          background: 'var(--hds-bg-3)', borderRadius: 6, border: '1px solid var(--hds-line)',
        }}>
          <span style={{
            fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-gold)',
            width: 14, textAlign: 'center', fontWeight: 700
          }}>{eid.priority}</span>
          
          <div style={{ flex: 1, display: 'flex', flexDirection: 'column' }}>
            <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)', textTransform: 'uppercase' }}>{eid.source}</span>
            <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-2)' }}>{eid.external_id}</span>
          </div>

          {isAdmin && (
            <div style={{ display: 'flex', gap: 4 }}>
              <button disabled={saving || i === 0} onClick={() => move(i, -1)} style={{
                background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 10, padding: 2
              }}>↑</button>
              <button disabled={saving || i === metadata.external_ids.length - 1} onClick={() => move(i, 1)} style={{
                background: 'none', border: 'none', color: 'var(--hds-txt-3)', cursor: 'pointer', fontSize: 10, padding: 2
              }}>↓</button>
              <button disabled={saving} onClick={() => remove(i)} style={{
                background: 'none', border: 'none', color: 'var(--hds-match-red)', cursor: 'pointer', fontSize: 10, padding: 2, marginLeft: 4
              }}>×</button>
            </div>
          )}
        </div>
      ))}
      {isAdmin && (
        <>
          <div style={{ display: 'flex', gap: 6, marginTop: 4 }}>
            <input
              value={addValue}
              onChange={e => { setAddValue(e.target.value); setAddError(null) }}
              onKeyDown={e => e.key === 'Enter' && addId()}
              placeholder="source:id — e.g. tmdb:603"
              disabled={saving}
              style={{
                flex: 1, padding: '5px 8px', borderRadius: 6,
                border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
                color: 'var(--hds-txt)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
                outline: 'none',
              }}
            />
            <button disabled={saving || !addValue.trim()} onClick={addId} style={{
              padding: '5px 12px', borderRadius: 6, cursor: 'pointer',
              border: '1px solid var(--hds-line)', background: 'var(--hds-bg-2)',
              color: 'var(--hds-txt-2)', fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
              opacity: (saving || !addValue.trim()) ? 0.5 : 1,
            }}>Add</button>
          </div>
          {addError && (
            <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-match-red)' }}>{addError}</div>
          )}
          <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 8, color: 'var(--hds-txt-3)', marginTop: 2 }}>
            Priority 1 is the primary source. Use arrows to reorder — new ids are added at the bottom.
          </p>
        </>
      )}
    </div>
  )
}

// ── Grouping ─────────────────────────────────────────────────────────────────

function GroupingSection({ showId, isAdmin, active }: { showId: string; isAdmin: boolean; active: boolean }) {
  const [candidates, setCandidates]   = useState<GroupingCandidate[]>([])
  const [groups,     setGroups]       = useState<EpisodeGroup[]>([])
  const [loading,    setLoading]      = useState(false)
  const [saving,     setSaving]       = useState(false)

  const load = () => {
    setLoading(true)
    Promise.all([api.getGroupingCandidates(showId), api.getEpisodeGroups(showId)])
      .then(([c, g]) => { setCandidates(c.candidates); setGroups(g) })
      .finally(() => setLoading(false))
  }

  useEffect(() => { if (active) load() }, [active, showId]) // eslint-disable-line react-hooks/exhaustive-deps

  const confirm = async (c: GroupingCandidate) => {
    setSaving(true)
    try {
      const { group_id } = await api.createEpisodeGroup(showId, { name: c.base_title, group_type: 'multipart' })
      await Promise.all(c.parts.map(p => api.addGroupMember(showId, group_id, { episode_id: p.episode_id, part_num: p.part_num })))
    } catch { /* surfaced via unchanged list on reload */ }
    setSaving(false)
    load()
  }

  const remove = async (groupId: string) => {
    setSaving(true)
    try { await api.deleteEpisodeGroup(showId, groupId) } catch {}
    setSaving(false)
    load()
  }

  if (loading) return <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)', padding: '6px 0' }}>Loading…</div>

  const unconfirmed = candidates.filter(c => !c.already_grouped)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, paddingTop: 6 }}>
      <div>
        <span style={labelStyle}>Confirmed Groups</span>
        {groups.length === 0 ? (
          <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
            No confirmed groups yet.
          </p>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            {groups.map(g => (
              <div key={g.group_id} style={{
                border: '1px solid oklch(0.7 0.16 150 / 0.35)', background: 'oklch(0.7 0.16 150 / 0.06)',
                borderRadius: 8, padding: '9px 11px',
              }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <span style={{ flex: 1, fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600, color: 'var(--hds-txt)' }}>{g.name}</span>
                  <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'oklch(0.7 0.16 150)' }}>{g.members.length} parts</span>
                  {isAdmin && (
                    <button disabled={saving} onClick={() => remove(g.group_id)} style={{
                      fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)',
                      background: 'none', border: 'none', cursor: 'pointer',
                    }}>Delete</button>
                  )}
                </div>
                <div style={{ marginTop: 5, display: 'flex', flexDirection: 'column', gap: 2 }}>
                  {g.members.map(m => (
                    <div key={m.id} style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                      Part {m.part_num} · S{String(m.season).padStart(2, '0')}E{String(m.episode).padStart(2, '0')} · {m.title}
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </div>

      <div>
        <span style={labelStyle}>Detected Candidates</span>
        {unconfirmed.length === 0 ? (
          <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
            {candidates.length === 0 ? 'No multi-part patterns detected.' : 'All detected candidates are already confirmed.'}
          </p>
        ) : (
          <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
            {unconfirmed.sort((a, b) => b.confidence - a.confidence).map((c, i) => (
              <div key={i} style={{
                border: `1px solid ${c.confidence >= 80 ? 'oklch(0.6 0.18 260 / 0.4)' : 'var(--hds-line-s)'}`,
                borderRadius: 8, padding: '9px 11px',
              }}>
                <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                  <span style={{ flex: 1, fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600, color: 'var(--hds-txt)' }}>{c.base_title}</span>
                  <span style={{
                    fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 6px', borderRadius: 6,
                    color: c.confidence >= 80 ? 'oklch(0.75 0.18 260)' : 'var(--hds-txt-3)',
                    background: c.confidence >= 80 ? 'oklch(0.55 0.18 260 / 0.18)' : 'var(--hds-bg-3)',
                  }}>{c.confidence}%</span>
                  {!c.adjacent && <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-gold)' }}>non-adjacent</span>}
                  {isAdmin && (
                    <button disabled={saving} onClick={() => confirm(c)} style={{
                      fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '3px 9px', borderRadius: 6,
                      border: '1px solid var(--hds-violet)', background: 'transparent', color: 'var(--hds-violet)', cursor: 'pointer',
                    }}>Confirm</button>
                  )}
                </div>
                <div style={{ marginTop: 5, display: 'flex', flexDirection: 'column', gap: 2 }}>
                  {c.parts.map(p => (
                    <div key={p.episode_id} style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                      Part {p.part_num} · S{String(p.season).padStart(2, '0')}E{String(p.episode).padStart(2, '0')} · {p.title}
                    </div>
                  ))}
                </div>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  )
}

// ── Specials ─────────────────────────────────────────────────────────────────
// A show's special/OVA/TV-movie often lives as a standalone file in a Movies
// library rather than under the show's own folder — this scans every scraper
// the show has a confirmed ID with, merges what they report, and matches
// against the movie catalog. Never auto-links: scanning only ever produces
// candidates for Accept/Reject, same review-driven flow as GroupingSection.

function SpecialsSection({ show, isAdmin, active, onShowChanged }: {
  show: ShowDetail; isAdmin: boolean; active: boolean; onShowChanged: (d: Detail) => void
}) {
  const [linked,     setLinked]     = useState<LinkedSpecial[]>([])
  const [candidates, setCandidates] = useState<SpecialCandidate[]>([])
  const [loading,    setLoading]    = useState(false)
  const [scanning,   setScanning]   = useState(false)
  const [saving,     setSaving]     = useState(false)

  const load = () => {
    setLoading(true)
    Promise.all([api.getLinkedSpecials(show.show_id), api.getSpecialCandidates(show.show_id)])
      .then(([l, c]) => { setLinked(l); setCandidates(c.candidates) })
      .finally(() => setLoading(false))
  }

  useEffect(() => { if (active) load() }, [active, show.show_id]) // eslint-disable-line react-hooks/exhaustive-deps

  const scan = async () => {
    setScanning(true)
    try {
      const { candidates } = await api.scanSpecials(show.show_id)
      setCandidates(candidates)
    } catch { /* surfaced via unchanged list on reload */ }
    setScanning(false)
    load()
  }

  const accept = async (c: SpecialCandidate) => {
    setSaving(true)
    try { await api.acceptSpecialCandidate(show.show_id, c.candidate_id) } catch {}
    setSaving(false)
    load()
  }

  const reject = async (c: SpecialCandidate) => {
    setSaving(true)
    try { await api.rejectSpecialCandidate(show.show_id, c.candidate_id) } catch {}
    setSaving(false)
    load()
  }

  const toggleFindSpecials = async (checked: boolean) => {
    onShowChanged({ ...show, find_specials: checked })
    try { await api.setShowFindSpecials(show.show_id, checked) } catch {}
  }

  const changeOrder = async (order: 'season' | 'aired') => {
    onShowChanged({ ...show, episode_display_order: order })
    try { await api.setEpisodeDisplayOrder(show.show_id, order) } catch {}
  }

  const pending = candidates.filter(c => c.accepted === -1)

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 16, paddingTop: 6 }}>
      <div style={{ display: 'flex', flexDirection: 'column', gap: 10 }}>
        <label style={{ display: 'flex', alignItems: 'center', gap: 8, fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt)' }}>
          <input
            type="checkbox"
            disabled={!isAdmin}
            checked={show.find_specials}
            onChange={e => toggleFindSpecials(e.target.checked)}
          />
          Automatically look for specials during sync
        </label>

        <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
          <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)' }}>Episode order</span>
          <select
            disabled={!isAdmin}
            value={show.episode_display_order}
            onChange={e => changeOrder(e.target.value as 'season' | 'aired')}
            style={inputStyle}
          >
            <option value="season">Season (Specials grouped together)</option>
            <option value="aired">Aired (interleaved between seasons)</option>
          </select>
        </div>

        {isAdmin && (
          <button disabled={scanning} onClick={scan} style={{
            ...goldBtnStyle, fontSize: 10, padding: '6px 12px', alignSelf: 'flex-start',
            opacity: scanning ? 0.6 : 1, cursor: scanning ? 'wait' : 'pointer',
          }}>
            {scanning ? 'Scanning…' : 'Scan for Specials'}
          </button>
        )}
      </div>

      {loading ? (
        <div style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 11, color: 'var(--hds-txt-3)' }}>Loading…</div>
      ) : (
        <>
          <div>
            <span style={labelStyle}>Confirmed Specials</span>
            {linked.length === 0 ? (
              <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                No linked specials yet.
              </p>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 6 }}>
                {linked.map(s => (
                  <div key={s.episode_id} style={{
                    fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)',
                    border: '1px solid oklch(0.7 0.16 150 / 0.35)', background: 'oklch(0.7 0.16 150 / 0.06)',
                    borderRadius: 8, padding: '9px 11px',
                  }}>
                    S00E{String(s.episode_number).padStart(2, '0')} · {s.title} → linked to "{s.linked_movie_title}"
                  </div>
                ))}
              </div>
            )}
          </div>

          <div>
            <span style={labelStyle}>Detected Candidates</span>
            {pending.length === 0 ? (
              <p style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                {candidates.length === 0 ? 'No candidates found yet — try scanning.' : 'All detected candidates are already decided.'}
              </p>
            ) : (
              <div style={{ display: 'flex', flexDirection: 'column', gap: 8 }}>
                {[...pending].sort((a, b) => a.episode_number - b.episode_number || b.score - a.score).map(c => (
                  <div key={c.candidate_id} style={{
                    border: `1px solid ${c.score >= 0.6 ? 'oklch(0.6 0.18 260 / 0.4)' : 'var(--hds-line-s)'}`,
                    borderRadius: 8, padding: '9px 11px',
                  }}>
                    <div style={{ display: 'flex', alignItems: 'center', gap: 8 }}>
                      <span style={{ flex: 1, fontFamily: "'Chakra Petch', sans-serif", fontSize: 11, fontWeight: 600, color: 'var(--hds-txt)' }}>
                        S00E{String(c.episode_number).padStart(2, '0')} · {c.special_title}
                      </span>
                      <span style={{
                        fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '2px 6px', borderRadius: 6,
                        color: c.score >= 0.6 ? 'oklch(0.75 0.18 260)' : 'var(--hds-txt-3)',
                        background: c.score >= 0.6 ? 'oklch(0.55 0.18 260 / 0.18)' : 'var(--hds-bg-3)',
                      }}>{Math.round(c.score * 100)}%</span>
                      {isAdmin && (
                        <>
                          <button disabled={saving} onClick={() => accept(c)} style={{
                            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, padding: '3px 9px', borderRadius: 6,
                            border: '1px solid var(--hds-violet)', background: 'transparent', color: 'var(--hds-violet)', cursor: 'pointer',
                          }}>Accept</button>
                          <button disabled={saving} onClick={() => reject(c)} style={{
                            fontFamily: "'JetBrains Mono', monospace", fontSize: 9, color: 'var(--hds-txt-3)',
                            background: 'none', border: 'none', cursor: 'pointer',
                          }}>Reject</button>
                        </>
                      )}
                    </div>
                    <div style={{ marginTop: 5, fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
                      matched to "{c.movie_title}"{c.movie_year ? ` (${c.movie_year})` : ''} · via {c.source}
                    </div>
                  </div>
                ))}
              </div>
            )}
          </div>
        </>
      )}
    </div>
  )
}

// ── Edit ─────────────────────────────────────────────────────────────────────

function EditSection({ detail, isShow, onSaved }: { detail: Detail; isShow: boolean; onSaved: (d: Detail) => void }) {
  const [draft,   setDraft]   = useState<Partial<ShowDetail & MovieDetail>>({ ...detail })
  const [saving,  setSaving]  = useState(false)
  const [error,   setError]   = useState<string | null>(null)
  const show = draft as Partial<ShowDetail>
  const movie = draft as Partial<MovieDetail>

  const patch = (field: string, value: unknown) => setDraft(d => ({ ...d, [field]: value }))

  const save = async () => {
    setSaving(true)
    setError(null)
    try {
      const id = isShow ? (detail as ShowDetail).show_id : (detail as MovieDetail).movie_id
      if (isShow) {
        await api.updateShow(id, draft as Partial<ShowDetail>)
        onSaved(await api.getShow(id))
      } else {
        await api.updateMovie(id, draft as Partial<MovieDetail>)
        onSaved(await api.getMovie(id))
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to save.')
    }
    setSaving(false)
  }

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 10, paddingTop: 6 }}>
      <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: 10 }}>
        <Field label="Title" span={2}>
          <input style={inputStyle} value={draft.title ?? ''} onChange={e => patch('title', e.target.value)} />
        </Field>
        <Field label="Year">
          <input style={inputStyle} type="number" value={draft.year ?? ''} onChange={e => patch('year', Number(e.target.value))} />
        </Field>
        <Field label="Content Rating">
          <input style={inputStyle} value={draft.content_rating ?? ''} onChange={e => patch('content_rating', e.target.value)} />
        </Field>
        {isShow ? (
          <>
            <Field label="Network / Studio">
              <input style={inputStyle} value={show.studio ?? ''} onChange={e => patch('studio', e.target.value)} />
            </Field>
            <Field label="Status">
              <input style={inputStyle} value={show.status ?? ''} onChange={e => patch('status', e.target.value)} />
            </Field>
            <Field label="First Aired" span={2}>
              <input style={inputStyle} value={show.originally_available_at ?? ''} onChange={e => patch('originally_available_at', e.target.value)} />
            </Field>
          </>
        ) : (
          <>
            <Field label="Director">
              <input style={inputStyle} value={movie.director ?? ''} onChange={e => patch('director', e.target.value)} />
            </Field>
            <Field label="Studio">
              <input style={inputStyle} value={movie.studio ?? ''} onChange={e => patch('studio', e.target.value)} />
            </Field>
            <Field label="Tagline" span={2}>
              <input style={inputStyle} value={movie.tagline ?? ''} onChange={e => patch('tagline', e.target.value)} />
            </Field>
          </>
        )}
        <Field label="Genres (comma-separated)" span={2}>
          <input
            style={inputStyle}
            value={Array.isArray(draft.genres) ? draft.genres.join(', ') : ''}
            onChange={e => patch('genres', e.target.value.split(',').map(s => s.trim()).filter(Boolean))}
          />
        </Field>
        <Field label="IMDb ID">
          <input style={inputStyle} placeholder="tt0000000" value={draft.imdb_id ?? ''} onChange={e => patch('imdb_id', e.target.value)} />
        </Field>
        {isShow ? (
          <Field label="TVDb ID">
            <input style={inputStyle} value={show.tvdb_id ?? ''} onChange={e => patch('tvdb_id', e.target.value)} />
          </Field>
        ) : (
          <Field label="TMDb ID">
            <input style={inputStyle} value={draft.tmdb_id ?? ''} onChange={e => patch('tmdb_id', e.target.value)} />
          </Field>
        )}
        {isShow && (
          <Field label="TMDb ID">
            <input style={inputStyle} value={draft.tmdb_id ?? ''} onChange={e => patch('tmdb_id', e.target.value)} />
          </Field>
        )}
        <Field label="Overview" span={2}>
          <textarea style={{ ...inputStyle, resize: 'vertical' }} rows={4} value={draft.overview ?? ''} onChange={e => patch('overview', e.target.value)} />
        </Field>
        <Field label="Custom Poster URL" span={2}>
          <input style={inputStyle} value={draft.thumb ?? ''} onChange={e => patch('thumb', e.target.value)} />
        </Field>
        <Field label="Custom Backdrop URL" span={2}>
          <input style={inputStyle} value={draft.art ?? ''} onChange={e => patch('art', e.target.value)} />
        </Field>
      </div>

      {error && (
        <div style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-match-red)',
          border: '1px solid var(--hds-match-red)', borderRadius: 7, padding: '8px 10px',
          background: 'oklch(0.55 0.22 27 / 0.08)',
        }}>{error}</div>
      )}

      <div style={{ display: 'flex', alignItems: 'center', gap: 12 }}>
        <button onClick={save} disabled={saving} style={{ ...goldBtnStyle, opacity: saving ? 0.6 : 1, cursor: saving ? 'wait' : 'pointer' }}>
          {saving ? 'Saving…' : 'Save Changes'}
        </button>
        <span style={{ fontFamily: "'JetBrains Mono', monospace", fontSize: 10, color: 'var(--hds-txt-3)' }}>
          Saving locks this record against future syncs.
        </span>
      </div>
    </div>
  )
}

function Field({ label, children, span = 1 }: { label: string; children: React.ReactNode; span?: 1 | 2 }) {
  return (
    <div style={{ gridColumn: span === 2 ? '1 / -1' : undefined }}>
      <label style={labelStyle}>{label}</label>
      {children}
    </div>
  )
}
