import { useNavigate } from 'react-router-dom'
import { FocusContext } from '@noriginmedia/norigin-spatial-navigation'
import { useGuideSession } from '../guide/useGuideSession'
import { GuideGrid } from '../guide/GuideGrid'
import { GuidePreview } from '../guide/GuidePreview'
import { useFocusable } from '../nav/useFocusable'
import { useZoneManifest } from './useZoneManifest'
import styles from './TvGuideSection.module.css'

// Stable key for TvHome's Guide quick-action button to setFocus() into —
// see that button's own comment for why scrollIntoView() alone wasn't
// enough. trackChildren lets norigin-spatial-navigation resolve setFocus on
// this container key down to an actual focusable descendant (the preview's
// Watch button or the grid's first channel column/program block) instead of
// requiring the caller to know a specific, data-dependent leaf key.
export const TV_GUIDE_SECTION_FOCUS_KEY = 'tv-guide-section'

// Same session/preview state machine as the desktop GuidePage (useGuideSession)
// — GuideGrid/GuidePreview/ChannelColumn are reused as-is rather than forked,
// since Phase 1's D-pad wiring already lives in those components and their
// current sizing already reads fine on a TV-class display; true 10-foot
// font/spacing scaling is a flagged fast-follow, not a functional blocker.
export function TvGuideSection() {
  const navigate = useNavigate()
  const {
    channels, epgByChannel, windowStartMs, nowMs,
      focusedId, focusedChannel, focusedProgram, nowProgram,
      selectChannel, selectProgram, manifestUrl,
  } = useGuideSession()

  // guide.zones per GET /api/tv/manifest: channel-header/time-grid/preview-panel.
  // GuideGrid fuses the channel column and its time-scrolling programs into
  // one component per channel (see ChannelColumn) — there's no separate chunk
  // of markup a standalone "channel-header" zone could gate, so it and
  // time-grid are treated as one presence signal for the whole grid, the same
  // "zones tell you what's present, not fine internal structure" tradeoff
  // TvLibrary.tsx already makes for library-pills.
  const { hasZone } = useZoneManifest('guide')

  const { ref, focusKey } = useFocusable<object, HTMLDivElement>({
    focusKey: TV_GUIDE_SECTION_FOCUS_KEY, trackChildren: true, saveLastFocusedChild: true,
  })

  if (channels.length === 0) return null

  const watchChannel = (channelId: string) => navigate(`/player/channel/${channelId}`)

  return (
    <div ref={ref}>
      <div className={styles.heading}>Live Guide</div>
      <FocusContext.Provider value={focusKey}>
      {hasZone('preview-panel') && (
          <GuidePreview
              channel={focusedChannel} previewProgram={focusedProgram ?? nowProgram} nowMs={nowMs}
              manifestUrl={manifestUrl} onWatch={() => focusedId && watchChannel(focusedId)}
          />
      )}
      {(hasZone('time-grid') || hasZone('channel-header')) && (
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
      )}
      </FocusContext.Provider>
    </div>
  )
}
