import { useNavigate } from 'react-router-dom'
import { useGuideSession } from '../guide/useGuideSession'
import { GuideGrid } from '../guide/GuideGrid'
import { GuidePreview } from '../guide/GuidePreview'
import styles from './TvGuideSection.module.css'

// Same session/preview state machine as the desktop GuidePage (useGuideSession)
// — GuideGrid/GuidePreview/ChannelColumn are reused as-is rather than forked,
// since Phase 1's D-pad wiring already lives in those components and their
// current sizing already reads fine on a TV-class display; true 10-foot
// font/spacing scaling is a flagged fast-follow, not a functional blocker.
export function TvGuideSection() {
  const navigate = useNavigate()
  const {
    channels, epgByChannel, windowStartMs, nowMs,
    focusedId, setFocusedId, focusedChannel, nowProgram, manifestUrl,
  } = useGuideSession()

  if (channels.length === 0) return null

  const watchChannel = (channelId: string) => navigate(`/player/channel/${channelId}`)

  return (
    <div>
      <div className={styles.heading}>Live Guide</div>
      <GuidePreview channel={focusedChannel} nowProgram={nowProgram} manifestUrl={manifestUrl} onWatch={() => focusedId && watchChannel(focusedId)} />
      <GuideGrid
        channels={channels}
        epgByChannel={epgByChannel}
        windowStartMs={windowStartMs}
        nowMs={nowMs}
        focusedChannelId={focusedId}
        onFocus={setFocusedId}
        onWatch={watchChannel}
      />
    </div>
  )
}
