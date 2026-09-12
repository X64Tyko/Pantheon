import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { mediaUrl } from '../../api/client'
import type { PlaylistBrowseEntry } from '../../api/types'
import { useFocusable } from '../../nav/useFocusable'
import { runPlaylistPlayAction, type PlaylistPlayAction } from '../../player/resolvePlayTarget'
import styles from './PlaylistTile.module.css'

function previewThumbUrl(item: { item_type: 'episode' | 'movie'; item_id: string }): string {
  return mediaUrl(item.item_type === 'movie' ? `/api/movies/${item.item_id}/thumb` : `/api/episodes/${item.item_id}/thumb`)
}

// Library "Playlists" section tile — poster_source (a pasted URL, same
// override convention as shows/movies) if set, else a collage of the first
// few items' own posters (preview_items, from PlaylistRepository::
// listBrowse) as a cheap zero-effort default. Clicking the tile itself turns
// the playlist into a normal library filter (see LibraryStore.enterPlaylist);
// the hover-revealed Play/Restart/Shuffle buttons instead jump straight into
// playback (see resolvePlayTarget.ts's runPlaylistPlayAction).
export function PlaylistTile({ playlist, onClick }: { playlist: PlaylistBrowseEntry; onClick: () => void }) {
  const [hovered, setHovered] = useState(false)
  const [busy, setBusy] = useState(false)
  const navigate = useNavigate()
  const { ref, focused } = useFocusable<object, HTMLDivElement>({
    focusKey: `playlist-tile-${playlist.playlist_id}`,
    onEnterPress: onClick,
  })
  const active = hovered || focused

  const runAction = async (e: React.MouseEvent, action: PlaylistPlayAction) => {
    e.stopPropagation()
    if (busy) return
    setBusy(true)
    try {
      const path = await runPlaylistPlayAction(playlist.playlist_id, playlist.mode, action)
      if (path) navigate(path)
    } finally {
      setBusy(false)
    }
  }

  return (
    <div
      ref={ref} data-tv-focused={focused}
      className={`${styles.tile} ${active ? styles.tileActive : ''}`}
      onClick={onClick}
      onMouseEnter={() => setHovered(true)}
      onMouseLeave={() => setHovered(false)}
    >
      <div className={styles.posterWrap}>
        {playlist.poster_source ? (
          <img src={mediaUrl(playlist.poster_source)} alt={playlist.title} className={styles.posterImg} />
        ) : playlist.preview_items.length > 0 ? (
          <div className={styles.collage}>
            {playlist.preview_items.slice(0, 4).map((it, i) => (
              <img key={i} src={previewThumbUrl(it)} alt="" className={styles.collageImg} />
            ))}
          </div>
        ) : (
          <span className={styles.placeholder}>▶</span>
        )}
        {active && playlist.item_count > 0 && (
          <div className={styles.actionOverlay}>
            <button onClick={e => runAction(e, 'play')} disabled={busy} title="Play (resumes where you left off)" className={styles.actionBtn}>▶</button>
            <button onClick={e => runAction(e, 'restart')} disabled={busy} title="Restart from the beginning" className={styles.actionBtn}>↻</button>
            <button onClick={e => runAction(e, 'shuffle')} disabled={busy} title="Shuffle play" className={styles.actionBtn}>🔀</button>
          </div>
        )}
      </div>
      <div className={styles.title}>{playlist.title}</div>
      <div className={styles.meta}>{playlist.item_count} item{playlist.item_count === 1 ? '' : 's'}</div>
    </div>
  )
}
