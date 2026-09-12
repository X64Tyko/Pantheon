import type { MediaLanguages } from '../../api/types'
import styles from './LanguageChips.module.css'

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
    <div className={styles.root}>
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
    <div className={styles.row}>
      <span className={styles.rowLabel}>{icon} {label}</span>
      {codes.map(c => (
        <span key={c} className={styles.chip}>{langName(c)}</span>
      ))}
    </div>
  )
}
