import { useCallback, useEffect, useRef, useState } from 'react'
import { api } from '../api/client'
import type { RokuDevice } from '../api/types'
import type { CastMediaArgs } from '../cast/castMedia'
import { buildRokuLoadCommand } from './rokuMedia'

// Deliberate v1 simplification (see pantheon-roku's design doc): simple
// polling instead of a push channel for state observation — command
// delivery latency (the long-poll on the Roku side) is what actually
// matters for responsiveness, not how quickly the scrubber updates here.
const STATE_POLL_MS = 1500

interface RokuState {
  playing?:    boolean
  positionMs?: number
  durationMs?: number
  volume?:     number
  muted?:      boolean
}

// Same shape as useCastSession's return value (see PlayerControls.tsx's
// shared `CastBridge` prop) — a Roku "session" here isn't a persistent SDK
// object like CAF's RemotePlayer, just "which of this user's paired devices
// is currently the active cast target," reconstructed from plain REST calls.
export function useRokuSession() {
  const [devices,  setDevices]  = useState<RokuDevice[]>([])
  const [activeId, setActiveId] = useState<string | null>(null)
  const [state,    setState]    = useState<RokuState>({})
  const pollRef = useRef<ReturnType<typeof setInterval> | null>(null)

  useEffect(() => {
    api.getRokuDevices().then(ds => setDevices(ds.filter(d => d.paired))).catch(() => {})
  }, [])

  useEffect(() => {
    if (!activeId) { setState({}); return }
    const poll = () => api.getRokuDeviceState(activeId).then(r => setState(r.state)).catch(() => {})
    poll()
    pollRef.current = setInterval(poll, STATE_POLL_MS)
    return () => { if (pollRef.current) clearInterval(pollRef.current) }
  }, [activeId])

  const send = useCallback((command: Record<string, unknown>) => {
    if (!activeId) return
    api.sendRokuCommand(activeId, command).catch(() => {})
  }, [activeId])

  const load = useCallback(async (args: CastMediaArgs, deviceId: string) => {
    setActiveId(deviceId)
    await api.sendRokuCommand(deviceId, buildRokuLoadCommand(args))
  }, [])

  const deviceName = devices.find(d => d.id === activeId)?.name ?? null

  return {
    devices,
    available:   devices.length > 0,
    connected:   activeId !== null,
    deviceName,
    currentMs:   state.positionMs ?? 0,
    durationMs:  state.durationMs ?? 0,
    paused:      state.playing === false,
    volumeLevel: state.volume ?? 1,
    muted:       state.muted ?? false,
    load,
    togglePlay:  () => send({ type: state.playing ? 'pause' : 'play' }),
    seek:        (ms: number) => send({ type: 'seek', positionMs: ms }),
    // Roku volume is a hardware/OS-level control, not a per-channel field the
    // way Chromecast's RemotePlayer.volumeLevel is — see pantheon-roku's
    // PlayerScreen.brs, which treats this command as a deliberate no-op.
    // Kept here (rather than hidden from the UI) so the slider/mute button
    // still render normally instead of needing Roku-specific layout branches.
    setVolumeLevel: (level: number) => send({ type: 'setVolume', level }),
    toggleMuted:    () => send({ type: 'setVolume', level: state.muted ? (state.volume ?? 1) : 0 }),
    endSession: () => { send({ type: 'stop' }); setActiveId(null) },
  }
}
