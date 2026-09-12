// ISO 639-2 code → display name for language dropdowns — shared by
// ChannelDefaultsPanel (per-channel audio/subtitle default), the
// show/movie detail page (per-item preference), and Settings (library-wide
// user default). Previously three separate near-duplicate copies of this
// table existed; consolidated here since a third caller was about to add a
// fourth.
const LANG_NAMES: Record<string, string> = {
  eng: 'English',   jpn: 'Japanese',  fre: 'French',   fra: 'French',
  ger: 'German',    deu: 'German',    spa: 'Spanish',   por: 'Portuguese',
  ita: 'Italian',   chi: 'Chinese',   zho: 'Chinese',   kor: 'Korean',
  rus: 'Russian',   ara: 'Arabic',    nld: 'Dutch',     dut: 'Dutch',
  pol: 'Polish',    swe: 'Swedish',   nor: 'Norwegian', dan: 'Danish',
  fin: 'Finnish',   hun: 'Hungarian', ces: 'Czech',     cze: 'Czech',
  slk: 'Slovak',    hrv: 'Croatian',  srp: 'Serbian',   bul: 'Bulgarian',
  ron: 'Romanian',  rum: 'Romanian',  tur: 'Turkish',   heb: 'Hebrew',
  hin: 'Hindi',     tha: 'Thai',      vie: 'Vietnamese', ind: 'Indonesian',
  msa: 'Malay',     ukr: 'Ukrainian', cat: 'Catalan',   lat: 'Latin',
}

export function langLabel(code: string): string {
  return LANG_NAMES[code] ? `${LANG_NAMES[code]} (${code})` : code
}

// Renders a <select> populated with library-discovered languages plus the
// full LANG_NAMES list as a fallback, deduped and sorted (discovered first).
export function LangSelect({ value, onChange, langs, emptyLabel, className }: {
  value:      string
  onChange:   (v: string) => void
  langs:      string[]   // discovered from library
  emptyLabel: string
  className?: string
}) {
  // Merge discovered languages with the static list; discovered ones come first.
  const discovered = [...new Set(langs)]
  const staticCodes = Object.keys(LANG_NAMES).filter(k => !discovered.includes(k))
  // Remove bibliographic/terminological duplicates (keep only one per name)
  const seen = new Set<string>()
  const deduped: string[] = []
  for (const code of discovered) {
    const name = LANG_NAMES[code] ?? code
    if (!seen.has(name)) { seen.add(name); deduped.push(code) }
  }
  const staticDeduped: string[] = []
  for (const code of staticCodes) {
    const name = LANG_NAMES[code] ?? code
    if (!seen.has(name)) { seen.add(name); staticDeduped.push(code) }
  }

  // If the current value isn't in any list, add it so the select shows it.
  const hasValue = value && [...deduped, ...staticDeduped].includes(value)

  return (
    <select
      value={value}
      onChange={e => onChange(e.target.value)}
      className={className}
    >
      <option value="">{emptyLabel}</option>
      {!hasValue && value && (
        <option value={value}>{langLabel(value)}</option>
      )}
      {deduped.length > 0 && (
        <optgroup label="In your library">
          {deduped.map(code => (
            <option key={code} value={code}>{langLabel(code)}</option>
          ))}
        </optgroup>
      )}
      {staticDeduped.length > 0 && (
        <optgroup label="Other languages">
          {staticDeduped.map(code => (
            <option key={code} value={code}>{langLabel(code)}</option>
          ))}
        </optgroup>
      )}
    </select>
  )
}
