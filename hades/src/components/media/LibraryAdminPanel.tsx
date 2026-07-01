import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import { AccordionSection } from '../../channel/sections'
import { goldBtnStyle, inputStyle } from '../../channel/styles'
import type { GroupingCandidate, EpisodeGroup, MovieDetail, ShowDetail } from '../../api/types'

type Detail = ShowDetail | MovieDetail
type Section = 'details' | 'grouping' | 'edit' | null

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
        <DetailsSection detail={detail} isShow={isShow} />
      </AccordionSection>

      {isShow && (
        <AccordionSection title="EPISODE GROUPING" open={open === 'grouping'} onToggle={() => toggle('grouping')}>
          <GroupingSection showId={(detail as ShowDetail).show_id} isAdmin={isAdmin} active={open === 'grouping'} />
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

function DetailsSection({ detail, isShow }: { detail: Detail; isShow: boolean }) {
  const show  = isShow ? detail as ShowDetail : null
  const id    = isShow ? (detail as ShowDetail).show_id : (detail as MovieDetail).movie_id
  const plexUrl = detail.source_base_url && detail.external_id
    ? `${detail.source_base_url}/web/index.html#!/server/details?key=/library/metadata/${detail.external_id}`
    : null
  const links = [
    plexUrl && { label: 'Plex', href: plexUrl, color: 'var(--hds-gold)' },
    detail.imdb_id && { label: 'IMDb', href: `https://www.imdb.com/title/${detail.imdb_id}`, color: 'oklch(0.78 0.15 84)' },
    show?.tvdb_id && { label: 'TVDb', href: `https://thetvdb.com/?id=${show.tvdb_id}&tab=series`, color: 'oklch(0.68 0.16 240)' },
    detail.tmdb_id && { label: 'TMDb', href: `https://www.themoviedb.org/${isShow ? 'tv' : 'movie'}/${detail.tmdb_id}`, color: 'oklch(0.7 0.16 150)' },
  ].filter(Boolean) as { label: string; href: string; color: string }[]

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
        <span style={labelStyle}>Identifiers</span>
        <div style={{ display: 'flex', flexDirection: 'column', gap: 4 }}>
          <IdRow label="Kairos ID" value={id} />
          {detail.external_id && <IdRow label="Source ID" value={detail.external_id} />}
          {detail.imdb_id && <IdRow label="IMDb" value={detail.imdb_id} />}
          {show?.tvdb_id && <IdRow label="TVDb" value={show.tvdb_id} />}
          {detail.tmdb_id && <IdRow label="TMDb" value={detail.tmdb_id} />}
        </div>
      </div>

      {(detail.source_base_url || detail.locked) && (
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
