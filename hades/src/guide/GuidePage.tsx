import { useEffect, useRef, useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { api } from '../api/client'
import type { Channel, EpgProgram } from '../api/types'
import { startPreview, switchPreview, stopPreview } from './previewApi'
import { GuideGrid } from './GuideGrid'
import { GuidePreview } from './GuidePreview'
import { WINDOW_LOOKBACK_MIN, WINDOW_FORWARD_HOURS } from './constants'

const FOCUS_DEBOUNCE_MS = 300

export function GuidePage() {
  const navigate = useNavigate()
  const [channels,     setChannels]     = useState<Channel[]>([])
  const [epgByChannel, setEpgByChannel] = useState<Record<string, EpgProgram[]>>({})
  const [focusedId,    setFocusedId]    = useState<string | null>(null)
  const [manifestUrl,  setManifestUrl]  = useState<string | null>(null)
  const [nowMs,        setNowMs]        = useState(() => Date.now())

  const sessionIdRef  = useRef<string | null>(null)
  const debounceRef   = useRef<ReturnType<typeof setTimeout> | null>(null)
  const windowStartMs = useRef(Date.now() - WINDOW_LOOKBACK_MIN * 60_000).current

  useEffect(() => {
    const fromSec  = Math.floor(windowStartMs / 1000)
    const hours    = WINDOW_LOOKBACK_MIN / 60 + WINDOW_FORWARD_HOURS

    api.getChannels().then(chs => {
      setChannels(chs)
      // Preview only starts once the user actually hovers/focuses a column
      // (see the focusedId effect below) — leave it unset here rather than
      // defaulting to channels[0], which used to spin up a live encode
      // session on every homepage load with no user interaction at all.
      Promise.all(chs.map(ch => api.getChannelEpg(ch.channel_id, hours, fromSec).catch(() => [])))
        .then(results => {
          const byChannel: Record<string, EpgProgram[]> = {}
          chs.forEach((ch, i) => { byChannel[ch.channel_id] = results[i] })
          setEpgByChannel(byChannel)
        })
    })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [])

  useEffect(() => {
    const tick = setInterval(() => setNowMs(Date.now()), 30_000)
    return () => clearInterval(tick)
  }, [])

  const beginPreview = (channelId: string) => {
    if (!sessionIdRef.current) {
      startPreview(channelId).then(res => {
        sessionIdRef.current = res.session_id
        setManifestUrl(res.manifest_url)
      }).catch(() => {})
    } else {
      switchPreview(sessionIdRef.current, channelId).catch(() => {})
    }
  }

  useEffect(() => {
    if (!focusedId) return
    if (debounceRef.current) clearTimeout(debounceRef.current)

    debounceRef.current = setTimeout(() => {
      // Tab is backgrounded — the visibility effect below owns starting/
      // stopping the session so a hidden Guide tab doesn't idle-hold a GPU
      // encoder slot indefinitely.
      if (document.hidden) return
      beginPreview(focusedId)
    }, FOCUS_DEBOUNCE_MS)

    return () => { if (debounceRef.current) clearTimeout(debounceRef.current) }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focusedId])

  // A backgrounded tab still runs its JS (hls.js keeps polling), so the
  // session never looks idle to Hephaestus's own reaper — it'll happily hold
  // a scarce hardware encoder slot forever if we don't explicitly stop it.
  useEffect(() => {
    const onVisibility = () => {
      if (document.hidden) {
        if (debounceRef.current) clearTimeout(debounceRef.current)
        if (sessionIdRef.current) {
          stopPreview(sessionIdRef.current)
          sessionIdRef.current = null
          setManifestUrl(null)
        }
      } else if (focusedId) {
        beginPreview(focusedId)
      }
    }
    document.addEventListener('visibilitychange', onVisibility)
    return () => document.removeEventListener('visibilitychange', onVisibility)
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [focusedId])

  useEffect(() => () => { if (sessionIdRef.current) stopPreview(sessionIdRef.current) }, [])

  const focusedChannel = channels.find(c => c.channel_id === focusedId) ?? null
  const nowProgram = focusedId
    ? (epgByChannel[focusedId] ?? []).find(p => p.wall_clock_start_ms <= nowMs && nowMs < p.wall_clock_end_ms) ?? null
    : null

  if (channels.length === 0) return null

  const watchChannel = (channelId: string) => navigate(`/player/channel/${channelId}`)

  return (
    <div>
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
