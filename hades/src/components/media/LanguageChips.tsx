import type { MediaLanguages } from '../../api/types'

const LANG_NAMES: Record<string, string> = {
  eng: 'English', jpn: 'Japanese', spa: 'Spanish', fre: 'French', fra: 'French',
  ger: 'German', deu: 'German', ita: 'Italian', por: 'Portuguese', rus: 'Russian',
  kor: 'Korean', chi: 'Chinese', zho: 'Chinese', ara: 'Arabic', hin: 'Hindi',
  dut: 'Dutch', nld: 'Dutch', swe: 'Swedish', nor: 'Norwegian', dan: 'Danish',
  fin: 'Finnish', pol: 'Polish', tur: 'Turkish', gre: 'Greek', ell: 'Greek',
  heb: 'Hebrew', tha: 'Thai', vie: 'Vietnamese', ces: 'Czech', cze: 'Czech',
}

function langName(code: string): string {
  return LANG_NAMES[code.toLowerCase()] ?? code.toUpperCase()
}

export function LanguageChips({ languages }: { languages: MediaLanguages | null }) {
  if (!languages || (languages.audio.length === 0 && languages.subtitle.length === 0)) return null

  return (
    <div style={{ display: 'flex', flexDirection: 'column', gap: 6, marginBottom: 18 }}>
      {languages.audio.length > 0 && (
        <LangRow icon="🔊" label="Audio" codes={languages.audio} />
      )}
      {languages.subtitle.length > 0 && (
        <LangRow icon="💬" label="Subtitles" codes={languages.subtitle} />
      )}
    </div>
  )
}

function LangRow({ icon, label, codes }: { icon: string; label: string; codes: string[] }) {
  return (
    <div style={{ display: 'flex', alignItems: 'center', gap: 8, flexWrap: 'wrap' }}>
      <span style={{
        fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
        color: 'var(--hds-txt-3)', letterSpacing: '0.06em', display: 'flex', alignItems: 'center', gap: 4,
      }}>{icon} {label}</span>
      {codes.map(c => (
        <span key={c} style={{
          fontFamily: "'JetBrains Mono', monospace", fontSize: 10,
          padding: '2px 8px', borderRadius: 10,
          border: '1px solid var(--hds-line-s)', color: 'var(--hds-txt-2)',
        }}>{langName(c)}</span>
      ))}
    </div>
  )
}
