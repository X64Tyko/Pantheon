import { useEffect, useState } from 'react'
import { api } from '../../api/client'
import { useAuth } from '../../auth/AuthContext'
import { AccordionSection } from '../../channel/sections'
import { goldBtnStyle, inputStyle } from '../../channel/styles'
import type {
    GroupingCandidate,
    EpisodeGroup,
    MovieDetail,
    MovieGroupingCandidate,
    ShowDetail,
    ExternalId,
    ItemMetadata,
    SpecialCandidate,
    LinkedSpecial
} from '../../api/types'
import styles from './LibraryAdminPanel.module.css'

type Detail = ShowDetail | MovieDetail
type Section = 'details' | 'grouping' | 'movie-parts' | 'specials' | 'edit' | null

const SOURCE_LINK_CLASS: Record<string, string> = {
  tmdb: styles.externalLinkTmdb,
  tvdb: styles.externalLinkTvdb,
  imdb: styles.externalLinkImdb,
  anidb: styles.externalLinkAnidb,
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
    <div className={styles.panel}>
      <AccordionSection title="DETAILS" open={open === 'details'} onToggle={() => toggle('details')}>
        <DetailsSection detail={detail} isShow={isShow} isAdmin={isAdmin} onDetailChanged={setDetail} />
      </AccordionSection>

      {isShow && (
        <AccordionSection title="EPISODE GROUPING" open={open === 'grouping'} onToggle={() => toggle('grouping')}>
          <GroupingSection showId={(detail as ShowDetail).show_id} isAdmin={isAdmin} active={open === 'grouping'} />
        </AccordionSection>
      )}

        {!isShow && (
            <AccordionSection title="MOVIE PARTS" open={open === 'movie-parts'} onToggle={() => toggle('movie-parts')}>
                <MoviePartsSection movie={detail as MovieDetail} isAdmin={isAdmin} active={open === 'movie-parts'}
                                   onMovieChanged={setDetail}/>
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
    if (eid.source === 'tmdb') return { label: 'TMDb', href: `https://www.themoviedb.org/${isShow ? 'tv' : 'movie'}/${eid.external_id}`, source: 'tmdb' }
    if (eid.source === 'tvdb') return { label: 'TVDb', href: `https://thetvdb.com/?id=${eid.external_id}&tab=series`, source: 'tvdb' }
    if (eid.source === 'imdb') return { label: 'IMDb', href: `https://www.imdb.com/title/${eid.external_id}`, source: 'imdb' }
    if (eid.source === 'anidb') return { label: 'AniDB', href: `https://anidb.net/a/${eid.external_id}`, source: 'anidb' }
    return null
  }).filter(Boolean) as { label: string; href: string; source: string }[]

  if (plexUrl) links.unshift({ label: 'Plex', href: plexUrl, source: 'plex' })

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
    <div className={styles.sectionBody}>
      <div>
        <span className={styles.fieldLabel}>External Links</span>
        {links.length === 0 ? (
          <p className={styles.metaText}>
            No external IDs stored.
          </p>
        ) : (
          <div className={styles.wrapRow8}>
            {links.map(l => (
              <a key={l.label} href={l.href} target="_blank" rel="noreferrer"
                 className={`${styles.externalLink} ${l.source === 'plex' ? styles.externalLinkPlex : SOURCE_LINK_CLASS[l.source]}`}>
                {l.label} ↗
              </a>
            ))}
          </div>
        )}
      </div>

      <div>
        <span className={styles.fieldLabel}>Scraper Priority & Identifiers</span>
        {loading ? (
           <div className={styles.metaText}>Loading…</div>
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

      <div className={styles.stack4}>
        <IdRow label="Kairos ID" value={id} />
        {detail.external_id && <IdRow label="Source ID" value={detail.external_id} />}
      </div>

      {(detail.source_base_url || detail.locked || isAdmin) && (
        <div>
          <span className={styles.fieldLabel}>Technical</span>
          {detail.source_base_url && <IdRow label="Source URL" value={detail.source_base_url} />}
          {detail.locked && (
            <div className={styles.lockedNotice}>Locked — manual edits won't be overwritten by future syncs.</div>
          )}
          {isAdmin && (
            <label className={`${styles.checkboxLabel} ${togglingScrape ? styles.checkboxLabelBusy : ''}`}>
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
        <div className={styles.actionBlock}>
          <button onClick={handleRefresh} disabled={refreshing}
                  className={`${styles.goldButtonSmall} ${refreshing ? styles.buttonBusy : ''}`} style={goldBtnStyle}>
            {refreshing ? 'Refreshing…' : 'Refresh Metadata from Scrapers'}
          </button>
          <p className={styles.hint}>
            Re-fetches overview, art, and ratings based on the priority above.
          </p>
        </div>
      )}
    </div>
  )
}

function IdRow({ label, value }: { label: string; value: string }) {
  return (
    <div className={styles.idRow}>
      <span className={styles.idRowLabel}>{label}</span>
      <span className={styles.idRowValue}>{value}</span>
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
    <div className={`${styles.stack4} ${styles.actionBlock}`}>
      {metadata.external_ids.length === 0 && (
        <div className={styles.metaText}>
          No scraper IDs linked.
        </div>
      )}
      {metadata.external_ids.sort((a,b) => a.priority - b.priority).map((eid, i) => (
        <div key={`${eid.source}:${eid.external_id}`} className={styles.eidRow}>
          <span className={styles.eidPriority}>{eid.priority}</span>

          <div className={styles.eidMeta}>
            <span className={styles.eidSource}>{eid.source}</span>
            <span className={styles.eidValue}>{eid.external_id}</span>
          </div>

          {isAdmin && (
            <div className={styles.eidActions}>
              <button disabled={saving || i === 0} onClick={() => move(i, -1)} className={styles.iconButton}>↑</button>
              <button disabled={saving || i === metadata.external_ids.length - 1} onClick={() => move(i, 1)} className={styles.iconButton}>↓</button>
              <button disabled={saving} onClick={() => remove(i)} className={`${styles.iconButton} ${styles.iconButtonDanger}`}>×</button>
            </div>
          )}
        </div>
      ))}
      {isAdmin && (
        <>
          <div className={styles.addRow}>
            <input
              value={addValue}
              onChange={e => { setAddValue(e.target.value); setAddError(null) }}
              onKeyDown={e => e.key === 'Enter' && addId()}
              placeholder="source:id — e.g. tmdb:603"
              disabled={saving}
              className={styles.addInput}
            />
            <button disabled={saving || !addValue.trim()} onClick={addId}
                     className={`${styles.addButton} ${(saving || !addValue.trim()) ? styles.addButtonDisabled : ''}`}>Add</button>
          </div>
          {addError && (
            <div className={styles.addError}>{addError}</div>
          )}
          <p className={styles.metaTextXs}>
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

  if (loading) return <div className={styles.metaTextSm}>Loading…</div>

  const unconfirmed = candidates.filter(c => !c.already_grouped)

  return (
    <div className={styles.sectionBodyWide}>
      <div>
        <span className={styles.fieldLabel}>Confirmed Groups</span>
        {groups.length === 0 ? (
          <p className={styles.metaText}>
            No confirmed groups yet.
          </p>
        ) : (
          <div className={styles.stack8}>
            {groups.map(g => (
              <div key={g.group_id} className={styles.confirmedCard}>
                <div className={styles.confirmedCardHeader}>
                  <span className={styles.confirmedCardTitle}>{g.name}</span>
                  <span className={styles.confirmedCardCount}>{g.members.length} parts</span>
                  {isAdmin && (
                    <button disabled={saving} onClick={() => remove(g.group_id)} className={styles.textButton}>Delete</button>
                  )}
                </div>
                <div className={styles.confirmedCardMembers}>
                  {g.members.map(m => (
                    <div key={m.id} className={styles.confirmedCardMember}>
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
        <span className={styles.fieldLabel}>Detected Candidates</span>
        {unconfirmed.length === 0 ? (
          <p className={styles.metaText}>
            {candidates.length === 0 ? 'No multi-part patterns detected.' : 'All detected candidates are already confirmed.'}
          </p>
        ) : (
          <div className={styles.stack8}>
            {unconfirmed.sort((a, b) => b.confidence - a.confidence).map((c, i) => (
              <div key={i} className={`${styles.candidateCard} ${c.confidence >= 80 ? styles.candidateCardConfident : ''}`}>
                <div className={styles.confirmedCardHeader}>
                  <span className={styles.confirmedCardTitle}>{c.base_title}</span>
                  <span className={`${styles.candidatePill} ${c.confidence >= 80 ? styles.candidatePillConfident : ''}`}>{c.confidence}%</span>
                  {!c.adjacent && <span className={`${styles.metaTextXs} ${styles.metaTextXsGold}`}>non-adjacent</span>}
                  {isAdmin && (
                    <button disabled={saving} onClick={() => confirm(c)} className={styles.candidateOutlineButton}>Confirm</button>
                  )}
                </div>
                <div className={styles.confirmedCardMembers}>
                  {c.parts.map(p => (
                    <div key={p.episode_id} className={styles.confirmedCardMember}>
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

// ── Movie parts (GitHub #3) ─────────────────────────────────────────────────
// Auto-detection groups multi-part movies at sync time; this is the manual
// safety valve — grouping-candidates is a global (not per-movie) heuristic
// scan, filtered here to candidates touching the movie this panel is open
// on, mirroring GroupingSection's confirm/reject shape.

function MoviePartsSection({movie, isAdmin, active, onMovieChanged}: {
    movie: MovieDetail; isAdmin: boolean; active: boolean; onMovieChanged: (d: Detail) => void
}) {
    const [candidates, setCandidates] = useState<MovieGroupingCandidate[]>([])
    const [loading, setLoading] = useState(false)
    const [saving, setSaving] = useState(false)

    const load = () => {
        setLoading(true)
        api.getMovieGroupingCandidates()
            .then(r => setCandidates(r.candidates.filter(c => c.parts.some(p => p.movie_id === movie.movie_id))))
            .finally(() => setLoading(false))
    }

    useEffect(() => {
        if (active) load()
    }, [active, movie.movie_id]) // eslint-disable-line react-hooks/exhaustive-deps

    const refreshMovie = () => {
        api.getMovie(movie.movie_id).then(onMovieChanged).catch(() => {
        })
    }

    const confirm = async (c: MovieGroupingCandidate) => {
        setSaving(true)
        try {
            const sorted = [...c.parts].sort((a, b) => a.part_num - b.part_num)
            const [target, ...rest] = sorted
            await api.linkMovieParts(target.movie_id, rest.map(p => p.movie_id))
        } catch { /* surfaced via unchanged list on reload */
        }
        setSaving(false)
        load()
        refreshMovie()
    }

    const unlink = async (partNum: number) => {
        setSaving(true)
        try {
            await api.unlinkMoviePart(movie.movie_id, partNum)
        } catch {
        }
        setSaving(false)
        load()
        refreshMovie()
    }

    if (loading) return <div className={styles.metaTextSm}>Loading…</div>

    const parts = movie.is_multi_part ? (movie.parts ?? []) : []

    return (
        <div className={styles.sectionBodyWide}>
            <div>
                <span className={styles.fieldLabel}>Parts</span>
                {parts.length === 0 ? (
                    <p className={styles.metaText}>Not a multi-part movie.</p>
                ) : (
                    <div className={styles.stack8}>
                        {parts.map(p => (
                            <div key={p.part_num} className={styles.confirmedCard}>
                                <div className={styles.confirmedCardHeader}>
                                    <span className={styles.confirmedCardTitle}>Part {p.part_num}</span>
                                    {isAdmin && (
                                        <button disabled={saving} onClick={() => unlink(p.part_num)}
                                                className={styles.textButton}>Unlink</button>
                                    )}
                                </div>
                                <div className={styles.confirmedCardMembers}>
                                    <div className={styles.confirmedCardMember}>{p.file_path}</div>
                                </div>
                            </div>
                        ))}
                    </div>
                )}
            </div>

            <div>
                <span className={styles.fieldLabel}>Detected Candidates</span>
                {candidates.length === 0 ? (
                    <p className={styles.metaText}>No multi-part patterns detected involving this movie.</p>
                ) : (
                    <div className={styles.stack8}>
                        {candidates.sort((a, b) => b.confidence - a.confidence).map((c, i) => (
                            <div key={i}
                                 className={`${styles.candidateCard} ${c.confidence >= 80 ? styles.candidateCardConfident : ''}`}>
                                <div className={styles.confirmedCardHeader}>
                                    <span className={styles.confirmedCardTitle}>{c.base_title}</span>
                                    <span
                                        className={`${styles.candidatePill} ${c.confidence >= 80 ? styles.candidatePillConfident : ''}`}>{c.confidence}%</span>
                                    {isAdmin && (
                                        <button disabled={saving} onClick={() => confirm(c)}
                                                className={styles.candidateOutlineButton}>Link as multi-part</button>
                                    )}
                                </div>
                                <div className={styles.confirmedCardMembers}>
                                    {c.parts.map(p => (
                                        <div key={p.movie_id} className={styles.confirmedCardMember}>
                                            Part {p.part_num} · {p.title}{p.year ? ` (${p.year})` : ''}
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
    <div className={styles.sectionBodyWide}>
      <div className={styles.stack10}>
        <label className={styles.checkboxLabelLg}>
          <input
            type="checkbox"
            disabled={!isAdmin}
            checked={show.find_specials}
            onChange={e => toggleFindSpecials(e.target.checked)}
          />
          Automatically look for specials during sync
        </label>

        <div className={styles.row8}>
          <span className={styles.metaTextSm}>Episode order</span>
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
          <button disabled={scanning} onClick={scan}
                  className={`${styles.goldButtonSmall} ${styles.goldButtonSmallStart} ${scanning ? styles.buttonBusy : ''}`} style={goldBtnStyle}>
            {scanning ? 'Scanning…' : 'Scan for Specials'}
          </button>
        )}
      </div>

      {loading ? (
        <div className={styles.metaTextSm}>Loading…</div>
      ) : (
        <>
          <div>
            <span className={styles.fieldLabel}>Confirmed Specials</span>
            {linked.length === 0 ? (
              <p className={styles.metaText}>
                No linked specials yet.
              </p>
            ) : (
              <div className={styles.stack6}>
                {linked.map(s => (
                  <div key={s.episode_id} className={`${styles.metaText} ${styles.confirmedCard}`}>
                    S00E{String(s.episode_number).padStart(2, '0')} · {s.title} → linked to "{s.linked_movie_title}"
                  </div>
                ))}
              </div>
            )}
          </div>

          <div>
            <span className={styles.fieldLabel}>Detected Candidates</span>
            {pending.length === 0 ? (
              <p className={styles.metaText}>
                {candidates.length === 0 ? 'No candidates found yet — try scanning.' : 'All detected candidates are already decided.'}
              </p>
            ) : (
              <div className={styles.stack8}>
                {[...pending].sort((a, b) => a.episode_number - b.episode_number || b.score - a.score).map(c => (
                  <div key={c.candidate_id} className={`${styles.candidateCard} ${c.score >= 0.6 ? styles.candidateCardConfident : ''}`}>
                    <div className={styles.confirmedCardHeader}>
                      <span className={styles.confirmedCardTitle}>
                        S00E{String(c.episode_number).padStart(2, '0')} · {c.special_title}
                      </span>
                      <span className={`${styles.candidatePill} ${c.score >= 0.6 ? styles.candidatePillConfident : ''}`}>{Math.round(c.score * 100)}%</span>
                      {isAdmin && (
                        <>
                          <button disabled={saving} onClick={() => accept(c)} className={styles.candidateOutlineButton}>Accept</button>
                          <button disabled={saving} onClick={() => reject(c)} className={styles.textButton}>Reject</button>
                        </>
                      )}
                    </div>
                    <div className={`${styles.metaText} ${styles.metaTextSpaced}`}>
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
    <div className={styles.sectionBody}>
      <div className={styles.formGrid}>
        <Field label="Title" span={2}>
          <input style={inputStyle} value={draft.title ?? ''} onChange={e => patch('title', e.target.value)} />
        </Field>
        <Field label="Original Title" span={2}>
          <input style={inputStyle} value={draft.original_title ?? ''} onChange={e => patch('original_title', e.target.value)} />
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
            <Field label="Writer">
              <input style={inputStyle} value={movie.writer ?? ''} onChange={e => patch('writer', e.target.value)} />
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
        <Field label="Tags (comma-separated)" span={2}>
          <input
            style={inputStyle}
            placeholder="scraper-derived keywords — e.g. christmas, time travel"
            value={Array.isArray(draft.tags) ? draft.tags.join(', ') : ''}
            onChange={e => patch('tags', e.target.value.split(',').map(s => s.trim()).filter(Boolean))}
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
          <textarea style={inputStyle} className={styles.formTextarea} rows={4} value={draft.overview ?? ''} onChange={e => patch('overview', e.target.value)} />
        </Field>
        <Field label="Custom Poster URL" span={2}>
          <input style={inputStyle} value={draft.thumb ?? ''} onChange={e => patch('thumb', e.target.value)} />
        </Field>
        <Field label="Custom Backdrop URL" span={2}>
          <input style={inputStyle} value={draft.art ?? ''} onChange={e => patch('art', e.target.value)} />
        </Field>
      </div>

      {error && (
        <div className={styles.formError}>{error}</div>
      )}

      <div className={styles.saveRow}>
        <button onClick={save} disabled={saving} style={goldBtnStyle} className={saving ? styles.buttonBusy : ''}>
          {saving ? 'Saving…' : 'Save Changes'}
        </button>
        <span className={styles.metaText}>
          Saving locks this record against future syncs.
        </span>
      </div>
    </div>
  )
}

function Field({ label, children, span = 1 }: { label: string; children: React.ReactNode; span?: 1 | 2 }) {
  return (
    <div className={span === 2 ? styles.formFieldSpan2 : undefined}>
      <label className={styles.fieldLabel}>{label}</label>
      {children}
    </div>
  )
}
