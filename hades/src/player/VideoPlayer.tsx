import {useCallback, useEffect, useRef, useState, type RefObject} from 'react'
import Hls from 'hls.js'
import { registerReceiverVideoElement } from '../cast/CastReceiverProvider'
import {playbackDebugLog} from './playbackDebugLog'
import styles from './VideoPlayer.module.css'

interface VideoPlayerProps {
  videoRef:        RefObject<HTMLVideoElement>
  manifestUrl:     string | null
  isLive:          boolean
  // Which subtitle track should be showing, in Hephaestus's own index scheme
  // (embedded relative_index >= 0, external sidecar <= -2, -1 = off — same
  // as VodTracks/TrackMenu's convention). VOD only; ignored for live. Now a
  // controlled selection against the master manifest's own SUBTITLES group
  // (see hephaestus's VodSession::buildMasterPlaylist) rather than a
  // separately sideloaded <track> — switching this no longer touches
  // manifestUrl/the video or audio at all.
  subtitleTrack?:  number
  // The selected track's language (BCP-47/ISO 639-2, e.g. "eng") — only
  // needed for the Safari native-HLS fallback below, which has no URL to
  // key off the way hls.js's own subtitleTracks[] does.
  subtitleLanguage?: string | null
  // Which audio track should be playing, in Hephaestus's own relative_index
  // scheme (VodTracks/TrackMenu convention) — VOD only; ignored for live.
  // Now a controlled selection against the master manifest's own AUDIO group
  // (audio is a fully independent VodEncodeStream server-side — see
  // VodSession.h's class comment) rather than something that requires a new
  // session: switching this just moves hls.js's own audioTrack pointer,
  // without touching manifestUrl/video at all.
  audioTrack?: number
  // Where to seek to once this (freshly loaded) manifest is ready — VOD only.
  // Hephaestus's VOD manifest now describes the whole file's real absolute
  // timeline from segment 0 for the session's entire life, so this is just
  // the one-time seek target for THIS load (initial mount, or a new session
  // from an audio track switch), not something forced to 0 the way it used to be.
  startPositionSec?: number
  autoPlay?:    boolean
  controls?:    boolean // native scrub bar — used by the admin Chapters review panel, not the full player (which has its own PlayerControls)
  onTimeUpdate: (currentMs: number, durationMs: number) => void
  onEnded:      () => void
  onError:      (message: string) => void
}

export function VideoPlayer({ videoRef, manifestUrl, isLive, subtitleTrack = -1, subtitleLanguage = null, audioTrack = -1, startPositionSec, autoPlay = true, controls = false, onTimeUpdate, onEnded, onError }: VideoPlayerProps) {
  const hlsRef = useRef<Hls | null>(null)
    // Forces the mount effect below to tear down and rebuild the whole hls.js
    // instance (fresh MediaSource/SourceBuffers, re-fetch the manifest from
    // scratch) — the same reset a viewer gets by leaving and re-entering a
    // channel. See recentStallsRef's own comment for why this exists.
    const [reloadKey, setReloadKey] = useState(0)

  // Maps our subtitleTrack index onto whichever of hls.js's own
  // subtitleTracks[] actually corresponds to it — hls.js assigns its own
  // opaque sequential ids that don't otherwise carry our index. Matched via
  // the X-PANTHEON-INDEX custom attribute Hephaestus embeds on every
  // #EXT-X-MEDIA entry (see buildMasterPlaylist) rather than the resolved
  // playlist URL — hls.js exposes every EXT-X-MEDIA attribute generically
  // (MediaPlaylist.attrs), so this round-trips our index exactly without
  // depending on how hls.js happens to resolve/record the URI string.
  // -1 (off) just disables the active track. Safe to call before hls.js has
  // parsed the manifest — subtitleTracks is empty until
  // SUBTITLE_TRACKS_UPDATED, so this is a silent no-op until the listener
  // below re-invokes it.
  const applySubtitleTrack = useCallback(() => {
    const hls = hlsRef.current
    if (!hls) {
      console.log('[player] applySubtitleTrack: no hls instance yet, skipping. subtitleTrack=', subtitleTrack)
      return
    }
    console.log('[player] applySubtitleTrack: subtitleTrack prop =', subtitleTrack,
      'hls.subtitleTracks =', hls.subtitleTracks.map(t => ({ id: t.id, name: t.name, panIdx: t.attrs['X-PANTHEON-INDEX'] })),
      'current hls.subtitleTrack =', hls.subtitleTrack)
    if (subtitleTrack === -1) {
      console.log('[player] applySubtitleTrack: disabling subtitles (hls.subtitleTrack = -1)')
      hls.subtitleTrack = -1
      return
    }
    const idx = hls.subtitleTracks.findIndex(t => t.attrs['X-PANTHEON-INDEX'] === String(subtitleTrack))
    if (idx !== -1) {
      console.log('[player] applySubtitleTrack: matched X-PANTHEON-INDEX', subtitleTrack, '-> hls track idx', idx, '(was', hls.subtitleTrack, ')')
      hls.subtitleTrack = idx
    } else {
      console.warn('[player] applySubtitleTrack: NO MATCH for X-PANTHEON-INDEX', subtitleTrack, 'in', hls.subtitleTracks.length, 'hls subtitleTracks — selection silently dropped')
    }
  }, [subtitleTrack])

  // hls.js's SUBTITLE_TRACKS_UPDATED listener (registered once per manifest
  // load, below) needs to always call the CURRENT applySubtitleTrack, not
  // whatever closure existed when it was registered — otherwise, if that
  // event ever refires after the user has since switched tracks, it
  // re-invokes the stale closure and silently reasserts the manifest's
  // original default, undoing the switch. This was the actual cause of a
  // subtitle switch appearing to "revert" to the previously-active track.
  const applySubtitleTrackRef = useRef(applySubtitleTrack)
  useEffect(() => { applySubtitleTrackRef.current = applySubtitleTrack }, [applySubtitleTrack])

  // Same matching scheme as applySubtitleTrack, against hls.js's own
  // audioTracks[]/audioTrack instead — no -1/"off" case here, an audio track
  // is always active (unlike subtitles). Setting hls.audioTrack triggers
  // hls.js's own internal AUDIO_TRACK_SWITCHING/SWITCHED flow (it buffers the
  // new track and swaps in sync on its own), which is what fixed the
  // "switching audio stops the video and shows a spinner" regression — that
  // was PlayerPage's handleSelectAudio calling session.reload() (a brand new
  // /stream/vod/start session, tearing down and reloading the whole player)
  // for a change that, now that audio is its own independent stream
  // server-side, only ever needed a client-side hls.js track switch.
  const applyAudioTrack = useCallback(() => {
    const hls = hlsRef.current
    if (!hls) return
    const idx = hls.audioTracks.findIndex(t => t.attrs['X-PANTHEON-INDEX'] === String(audioTrack))
    if (idx !== -1 && idx !== hls.audioTrack) {
      console.log('[player] applyAudioTrack: matched X-PANTHEON-INDEX', audioTrack, '-> hls track idx', idx, '(was', hls.audioTrack, ')')
      hls.audioTrack = idx
    } else if (idx === -1) {
      console.warn('[player] applyAudioTrack: NO MATCH for X-PANTHEON-INDEX', audioTrack, 'in', hls.audioTracks.length, 'hls audioTracks — selection silently dropped')
    }
  }, [audioTrack])

  const applyAudioTrackRef = useRef(applyAudioTrack)
  useEffect(() => { applyAudioTrackRef.current = applyAudioTrack }, [applyAudioTrack])

  // Safari (native HLS, no hls.js — see the else-if branch below) parses
  // SUBTITLES groups into the <video> element's own native textTracks
  // itself; matched by language since TextTrack has no URL to key off the
  // way hls.js's MediaPlaylist does. Imprecise if two tracks share a
  // language (picks the first) — the old single-<track> sideload had the
  // same limitation for Safari (only ever offered the one server-resolved
  // track regardless of duplicates).
  //
  // Guarded on !hlsRef.current — hls.js renders subtitles natively too (its
  // own textTracks entries on this same <video>), and setting .mode here
  // unconditionally fought its internal track-mode management: it worked
  // once on initial load (before any manual switch ever ran this with
  // tracks already populated) but broke on every switch after, since this
  // was forcibly disabling the track hls.js had just enabled and only
  // sometimes matching it back on by language.
  const applyNativeSubtitleTrack = useCallback(() => {
    if (hlsRef.current) return
    const video = videoRef.current
    if (!video) return
    for (const t of Array.from(video.textTracks)) t.mode = 'disabled'
    if (subtitleTrack === -1 || !subtitleLanguage) return
    const match = Array.from(video.textTracks).find(t => t.language === subtitleLanguage)
    if (match) match.mode = 'showing'
  }, [subtitleTrack, subtitleLanguage])

  // Re-applies whenever the caller changes which track should show — the
  // only trigger now, since a subtitle switch no longer reloads manifestUrl.
  useEffect(() => { applySubtitleTrack() }, [applySubtitleTrack])
  useEffect(() => { applyNativeSubtitleTrack() }, [applyNativeSubtitleTrack])
  useEffect(() => { applyAudioTrack() }, [applyAudioTrack])

  useEffect(() => {
    const video = videoRef.current
    if (!video || !manifestUrl) return

    let hls: Hls | null = null

    if (Hls.isSupported()) {
      // VOD sessions get a complete, #EXT-X-ENDLIST-terminated playlist from
      // Hephaestus immediately (the whole file's segment list is known and
      // declared upfront — see VodSession.cpp's sliding-window engine), so
      // hls.js correctly treats it as on-demand rather than live from the
      // very first fetch. startPosition seeds where in that (now-absolute,
        // whole-file) timeline THIS load should begin.
        //
        // Live is different but not exempt: on a genuinely fresh mount there's
        // nowhere sensible to resume, so hls.js's own default live-edge sync
        // is correct. But this effect also re-runs on a forced reload
        // (reloadKey bump below, or a channel-viewer reconnect swapping
        // manifestUrl) — and the <video> element itself persists across that,
        // only the Hls.js instance attached to it gets destroyed and rebuilt.
        // video.currentTime at that point still reflects wherever the
        // previous instance had gotten to. Without pinning it, a fresh Hls.js
        // instance picks its own live-position default with no idea this is a
        // resume — and since the splicer's sliding window deliberately keeps
        // an outgoing item's tail segments alongside the incoming one's (what
        // makes a gapless transition possible), that default can land inside
        // segments from an item that already aired. Confirmed live: a
        // stall-triggered reload rewound and replayed an already-shown
        // commercial this way.
        const resumeAt = isLive && video.currentTime > 0 ? video.currentTime : undefined
        hls = new Hls(isLive
            ? (resumeAt !== undefined ? {startPosition: resumeAt} : {})
            : {startPosition: startPositionSec ?? 0})
      hlsRef.current = hls
      // Fires once hls.js has parsed the manifest's SUBTITLES groups (and
      // possibly again later) — applySubtitleTrackRef.current() so this
      // always re-applies whatever's currently selected, not a stale
      // mount-time snapshot (see applySubtitleTrackRef's own comment).
      hls.on(Hls.Events.SUBTITLE_TRACKS_UPDATED, (_evt, data) => {
          const summary = data.subtitleTracks.map(t => `${t.id}:${t.name}(panIdx=${t.attrs['X-PANTHEON-INDEX']})`).join(', ')
        console.log('[player] hls SUBTITLE_TRACKS_UPDATED', data.subtitleTracks.map(t => ({ id: t.id, name: t.name, panIdx: t.attrs['X-PANTHEON-INDEX'], url: t.url })))
          if (isLive) playbackDebugLog('track', `SUBTITLE_TRACKS_UPDATED [${summary}]`)
        applySubtitleTrackRef.current()
      })
      // Same reasoning as SUBTITLE_TRACKS_UPDATED above — always re-applies
      // whatever's currently selected via the ref, not a stale closure.
      hls.on(Hls.Events.AUDIO_TRACKS_UPDATED, (_evt, data) => {
          const summary = data.audioTracks.map(t => `${t.id}:${t.name}(panIdx=${t.attrs['X-PANTHEON-INDEX']})`).join(', ')
        console.log('[player] hls AUDIO_TRACKS_UPDATED', data.audioTracks.map(t => ({ id: t.id, name: t.name, panIdx: t.attrs['X-PANTHEON-INDEX'], url: t.url })))
          if (isLive) playbackDebugLog('track', `AUDIO_TRACKS_UPDATED [${summary}]`)
        applyAudioTrackRef.current()
      })
      hls.on(Hls.Events.AUDIO_TRACK_SWITCHED, (_evt, data) => {
        console.log('[player] hls AUDIO_TRACK_SWITCHED -> id', data.id)
          if (isLive) playbackDebugLog('track', `AUDIO_TRACK_SWITCHED -> id=${data.id} currentTime=${video.currentTime.toFixed(2)}`)
      })
      hls.on(Hls.Events.SUBTITLE_TRACK_SWITCH, (_evt, data) => {
        console.log('[player] hls SUBTITLE_TRACK_SWITCH -> id', data.id)
          if (isLive) playbackDebugLog('track', `SUBTITLE_TRACK_SWITCH -> id=${data.id}`)
      })
      hls.on(Hls.Events.SUBTITLE_TRACK_LOADED, (_evt, data) => {
        console.log('[player] hls SUBTITLE_TRACK_LOADED', { id: data.id, url: data.details?.url, fragCount: data.details?.fragments?.length })
      })
      hls.on(Hls.Events.SUBTITLE_FRAG_PROCESSED, (_evt, data) => {
        console.log('[player] hls SUBTITLE_FRAG_PROCESSED success=', data.success, 'frag=', data.frag?.url)
      })
      hls.loadSource(manifestUrl)
      hls.attachMedia(video)
      registerReceiverVideoElement(video)

      // A "fatal" hls.js error is fatal to the current internal load state,
      // not necessarily to the stream — a channel's backing ffmpeg process
      // can keep transcoding and appending fresh segments right through a
      // transient network blip (a slow/aborted playlist fetch, a segment
      // request racing hls_flags=delete_segments off the rolling window) or
      // a decoder hiccup, and hls.js's own docs call for attempting
      // recovery before giving up: startLoad() re-polls the (very much
      // still-live) playlist for NETWORK_ERROR, recoverMediaError() for
      // MEDIA_ERROR. Previously this surfaced onError on the very first
      // fatal event with no recovery attempt at all, which read as "the
      // stream stopped playing" to anyone watching a 24/7 live channel even
      // though the encode never actually stopped. Retries are capped and
      // per-error-type so a genuinely dead stream still surfaces onError
      // rather than looping forever, but the counters reset on the next
      // successfully loaded fragment so a channel that recovers cleanly
      // isn't left with an exhausted budget for the next unrelated blip
      // hours later.
      let networkRetries = 0
      let mediaRetries   = 0
      const MAX_RETRIES  = 3
        // hls.js's own gap-controller nudges/skips over buffer holes via
        // *non-fatal* MEDIA_ERROR events (bufferStalledError/bufferNudgeOnStall)
        // and only escalates to fatal after nudgeMaxRetry consecutive failed
        // nudges *within one stall period* — but that per-stall nudge counter
        // resets the moment currentTime moves at all (gap-controller.ts), so a
        // stream that nudges past one hole, plays a few frames into the next
        // hole, nudges again, etc. never accumulates enough consecutive
        // failures to trip the fatal path — it can repeat indefinitely.
        // Server-side this reads as "stream still running" (the encode never
        // stopped); client-side it's a real repeating stall — each nudge is a
        // tiny forced seek, audible as a "chirp" — that only clears once the
        // whole hls.js instance is torn down and rebuilt (what leaving and
        // re-entering the channel does). Detect the *pattern* directly instead
        // of waiting on an escalation that may never fire: repeated stall
        // reports in a short window force that same full reload ourselves.
        //
        // Confirmed via a live regression on the Android player (which used
        // an analogous raw-event-count approach): counting bare event
        // occurrences alone is too trigger-happy — a couple of ordinary,
        // separate, self-resolving transition gaps can each report a stall
        // without ever being genuinely stuck, and forcing a reload for that
        // just adds its own re-buffering on top. Requiring currentTime to
        // have made near-zero real progress across the whole window is the
        // actual "wedged, not just jittery" signal, mirroring the
        // position-based check the Android watchdog now uses instead of a
        // raw STATE_BUFFERING count.
        const recentStalls: number[] = []
        const STALL_WINDOW_MS = 12_000
        const STALL_THRESHOLD = 3
        const STALL_MIN_PROGRESS_S = 2
        let positionAtFirstStall: number | null = null
      hls.on(Hls.Events.FRAG_LOADED, () => { networkRetries = 0; mediaRetries = 0 })
      hls.on(Hls.Events.ERROR, (_evt, data) => {
        console.warn('[player] hls ERROR fatal=', data.fatal, 'type=', data.type, 'details=', data.details,
          'url=', (data as { url?: string }).url, 'response=', data.response, 'frag=', data.frag?.url)
          if (!data.fatal) {
              if (data.details === Hls.ErrorDetails.BUFFER_STALLED_ERROR ||
                  data.details === Hls.ErrorDetails.BUFFER_NUDGE_ON_STALL) {
                  const now = Date.now()
                  // These are otherwise invisible outside this tab's own devtools —
                  // console.warn above never reaches remoteLog.ts's forwarding (that
                  // only patches console.error). Forwarding every raw occurrence
                  // (not just the 3-in-12s escalation below) is the point while
                  // hunting the segment-boundary-correlated live-channel stutter —
                  // frequency-over-time is exactly the signal that's missing
                  // server-side. Gated on hades_debug like other opt-in diagnostics
                  // (see playbackDebugLog).
                  if (isLive) playbackDebugLog('stall', `${data.details} at t=${video.currentTime.toFixed(2)}s frag=${data.frag?.url ?? 'n/a'}`)
                  if (recentStalls.length === 0) positionAtFirstStall = video.currentTime
                  recentStalls.push(now)
                  while (recentStalls.length && now - recentStalls[0] > STALL_WINDOW_MS) recentStalls.shift()
                  if (recentStalls.length === 0) positionAtFirstStall = null
                  else if (recentStalls.length >= STALL_THRESHOLD) {
                      const progressed = positionAtFirstStall !== null &&
                          video.currentTime - positionAtFirstStall >= STALL_MIN_PROGRESS_S
                      recentStalls.length = 0
                      positionAtFirstStall = null
                      if (!progressed) {
                          console.warn('[player] repeated non-fatal buffer stalls with no real progress — forcing full player reload')
                          if (isLive) playbackDebugLog('stall', `forcing full reload — repeated stalls with no progress, t=${video.currentTime.toFixed(2)}s`)
                          setReloadKey(k => k + 1)
                      } else {
                          console.warn('[player] repeated non-fatal buffer stalls but currentTime is still advancing — treating as ordinary transition jitter, not reloading')
                          if (isLive) playbackDebugLog('stall', `not reloading — currentTime still advancing (stale-buffer-fed progress would also look like this), t=${video.currentTime.toFixed(2)}s`)
                      }
                  }
              }
              return
          }
          if (isLive) {
              playbackDebugLog('error',
                  `fatal type=${data.type} details=${data.details} frag=${data.frag?.url ?? 'n/a'} ` +
                  `response=${JSON.stringify(data.response ?? null)} t=${video.currentTime.toFixed(2)}s`)
          }
        switch (data.type) {
          case Hls.ErrorTypes.NETWORK_ERROR:
            console.warn('[player] fatal NETWORK_ERROR, retry', networkRetries + 1, 'of', MAX_RETRIES)
            if (networkRetries++ < MAX_RETRIES) hls!.startLoad()
            else onError('Network error — the stream stopped responding.')
            break
          case Hls.ErrorTypes.MEDIA_ERROR:
            console.warn('[player] fatal MEDIA_ERROR, retry', mediaRetries + 1, 'of', MAX_RETRIES)
            if (mediaRetries++ < MAX_RETRIES) hls!.recoverMediaError()
            else onError('Playback error — the stream could not be decoded.')
            break
          default:
            onError('Playback failed.')
        }
      })
      // Seek diagnostics — a seek beyond the already-generated segment range
      // needs Hephaestus to actually restart its encoder there (VodSession's
      // sliding-window engine), which can take real wall-clock time; these
      // pin down whether a "stuck" seek is the browser waiting on a slow/
      // hung backend response (network log will show a long-pending seg-*
      // request) versus something purely client-side (hls.js never even
      // issuing the request, or stuck buffering after receiving it).
        video.addEventListener('seeking', () => {
            console.log('[player] video seeking -> target', video.currentTime)
            if (isLive) playbackDebugLog('seek', `seeking -> target=${video.currentTime.toFixed(2)}`)
        })
      // Re-assert the subtitle track once the video has actually landed at
      // its real position — including the implicit "seek" hls.js performs
      // internally to reach startPositionSec on a continue-watching resume.
      // applySubtitleTrack can otherwise run once, early, off
      // SUBTITLE_TRACKS_UPDATED (which fires right after manifest parse,
      // before hls.js's own startPosition seek has actually landed), which
      // was observed to leave the subtitle-stream-controller's own position
      // bookkeeping keyed off wherever currentTime happened to be at that
      // earlier moment rather than where playback actually resumed — the
      // video seeks correctly, but subtitles silently keep whatever (now
      // stale) sync state they picked up before the seek completed.
      video.addEventListener('seeked', () => {
        console.log('[player] video seeked, now at', video.currentTime)
          if (isLive) playbackDebugLog('seek', `seeked -> now=${video.currentTime.toFixed(2)}`)
        applySubtitleTrackRef.current()
      })
        video.addEventListener('waiting', () => {
            console.log('[player] video waiting (buffering) at', video.currentTime)
            if (isLive) playbackDebugLog('stall', `native 'waiting' at t=${video.currentTime.toFixed(2)}s`)
        })
        video.addEventListener('stalled', () => {
            console.warn('[player] video stalled at', video.currentTime)
            if (isLive) playbackDebugLog('stall', `native 'stalled' at t=${video.currentTime.toFixed(2)}s`)
        })
        video.addEventListener('canplay', () => {
            console.log('[player] video canplay at', video.currentTime)
            if (isLive) playbackDebugLog('stall', `native 'canplay' (recovered) at t=${video.currentTime.toFixed(2)}s`)
        })
        // pause/play — the reported incident this whole file's buffer/stall
        // logging chases was described as "video paused"; the native element
        // pausing on its own (not a user action) is itself a signal worth
        // capturing, distinct from a stall/wait (currentTime stops advancing
        // but playbackState may still say "playing").
        video.addEventListener('pause', () => {
            if (isLive) playbackDebugLog('stall', `native 'pause' at t=${video.currentTime.toFixed(2)}s ended=${video.ended}`)
        })
        video.addEventListener('play', () => {
            if (isLive) playbackDebugLog('stall', `native 'play' (resumed) at t=${video.currentTime.toFixed(2)}s`)
        })
        hls.on(Hls.Events.FRAG_LOADING, (_evt, data) => {
            console.log('[player] hls FRAG_LOADING', data.frag.type, data.frag.url)
            if (isLive) playbackDebugLog('frag', `LOADING type=${data.frag.type} sn=${data.frag.sn} cc=${data.frag.cc} url=${data.frag.url}`)
        })
        hls.on(Hls.Events.FRAG_LOADED, (_evt, data) => {
            console.log('[player] hls FRAG_LOADED', data.frag.type, data.frag.url)
            if (isLive) playbackDebugLog('frag', `LOADED type=${data.frag.type} sn=${data.frag.sn} cc=${data.frag.cc} start=${data.frag.start.toFixed(2)} duration=${data.frag.duration.toFixed(2)}`)
        })
        // Diagnostic-only (hades_debug-gated, same as the stall/nudge forwarding
        // above): chasing a live-channel report of frozen video with stale
        // audio (from an item that had already aired) continuing to play until
        // it ran out, at which point playback resumed in sync — a shape that
        // points at hls.js's audio and video SourceBuffers drifting apart
        // rather than anything server-side (Hephaestus's canonical segment
        // naming is a strictly monotonic, never-reused sequence per channel
        // session — see ChannelPlaylistSplicer::next_seq_ — so a stale
        // canonical segment being re-served under a recycled name isn't
        // possible; if this reproduces, the divergence has to be visible here,
        // client-side). BUFFER_APPENDED's timeRanges gives each track's own
        // buffered() *separately* — normal HTMLMediaElement.buffered only
        // reports the intersection, which would hide exactly this kind of
        // divergence. frag.cc (continuity counter) pinpoints which
        // discontinuity a given append belongs to, so the log can show whether
        // an audio append ever lands from an older cc than the video append
        // alongside it.
        const fmtRanges = (tr?: TimeRanges) => {
            if (!tr) return 'none'
            const parts: string[] = []
            for (let i = 0; i < tr.length; i++) parts.push(`${tr.start(i).toFixed(2)}-${tr.end(i).toFixed(2)}`)
            return parts.length ? parts.join(',') : 'empty'
        }
        hls.on(Hls.Events.BUFFER_APPENDED, (_evt, data) => {
            if (!isLive) return
            playbackDebugLog('buffer',
                `APPENDED type=${data.type} frag.type=${data.frag.type} sn=${data.frag.sn} cc=${data.frag.cc} ` +
                `frag.start=${data.frag.start.toFixed(2)} video=[${fmtRanges(data.timeRanges.video)}] audio=[${fmtRanges(data.timeRanges.audio)}] ` +
                `currentTime=${video.currentTime.toFixed(2)}`)
        })
        hls.on(Hls.Events.BUFFER_FLUSHING, (_evt, data) => {
            if (!isLive) return
            playbackDebugLog('buffer',
                `FLUSHING type=${data.type ?? 'both'} start=${data.startOffset.toFixed(2)} end=${data.endOffset.toFixed(2)} ` +
                `currentTime=${video.currentTime.toFixed(2)}`)
        })
        hls.on(Hls.Events.BUFFER_FLUSHED, (_evt, data) => {
            if (!isLive) return
            playbackDebugLog('buffer', `FLUSHED type=${data.type} currentTime=${video.currentTime.toFixed(2)}`)
        })
      if (autoPlay) video.play().catch(() => {})
    } else if (video.canPlayType('application/vnd.apple.mpegurl')) {
      // Safari: native HLS, no hls.js needed. Unlike hls.js there's no
      // startPosition option — the manifest's own absolute timeline means
      // native playback would otherwise just start at 0, so seek explicitly
      // once metadata (duration/seekable range) is actually available.
      console.log('[player] Using native HLS (Safari/iOS)')
      video.src = manifestUrl
      registerReceiverVideoElement(video)
      // Safari populates video.textTracks from the manifest's SUBTITLES
      // group only once metadata is actually loaded — same event the seek
      // below already waits on.
      video.addEventListener('loadedmetadata', applyNativeSubtitleTrack)
      if (!isLive && startPositionSec) {
        const seekOnce = () => { video.currentTime = startPositionSec; video.removeEventListener('loadedmetadata', seekOnce) }
        video.addEventListener('loadedmetadata', seekOnce)
      }
      if (autoPlay) video.play().catch(() => {})
    } else {
      onError('This browser cannot play HLS video.')
    }

    return () => {
      video.removeEventListener('loadedmetadata', applyNativeSubtitleTrack)
      hlsRef.current = null
      hls?.destroy()
    }
      // reloadKey has no meaning of its own — bumping it just re-runs this
      // effect (cleanup destroys the old hls instance, then a fresh one is
      // built below) as a forced full reload, see recentStalls' own comment.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [manifestUrl, reloadKey])

  return (
    <video
      ref={videoRef}
      onTimeUpdate={e => onTimeUpdate(e.currentTarget.currentTime * 1000, e.currentTarget.duration * 1000)}
      onEnded={onEnded}
      className={styles.video}
      playsInline
      controls={controls}
    />
  )
}
