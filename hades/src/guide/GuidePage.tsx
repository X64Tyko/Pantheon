import { useNavigate } from 'react-router-dom'
import { useGuideSession } from './useGuideSession'
import { GuideGrid } from './GuideGrid'
import { GuidePreview } from './GuidePreview'
import styles from './GuidePage.module.css'

export function GuidePage() {
  const navigate = useNavigate()
  const {
    channels, epgByChannel, windowStartMs, nowMs,
      focusedId, focusedChannel, focusedProgram, nowProgram,
      selectChannel, selectProgram, manifestUrl,
  } = useGuideSession()

  if (channels.length === 0) return null

  const watchChannel = (channelId: string) => navigate(`/player/channel/${channelId}`)

  return (
      // Hero is a fixed, non-scrolling region — same "browse, don't scroll
      // past it" model as Home's own hero. Layout.tsx's full-bleed content
      // area (this route is in its isFullBleed list) is itself
      // overflow:hidden, so the grid below — not this outer div — is what
      // actually needs to fill the remaining space and do its own scrolling;
      // GuideGrid already handles that internally (see its own class comment).
      <div className={styles.root}>
          <GuidePreview
              channel={focusedChannel}
              previewProgram={focusedProgram ?? nowProgram}
              nowMs={nowMs}
              manifestUrl={manifestUrl}
              onWatch={() => focusedId && watchChannel(focusedId)}
          />
          <div className={styles.gridWrap}>
              <GuideGrid
                  channels={channels}
                  epgByChannel={epgByChannel}
                  windowStartMs={windowStartMs}
                  nowMs={nowMs}
                  focusedChannelId={focusedId}
                  onFocusChannel={selectChannel}
                  onFocusProgram={selectProgram}
                  onWatch={watchChannel}
              />
          </div>
    </div>
  )
}
