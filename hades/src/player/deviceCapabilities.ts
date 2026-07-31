import type {DeclaredClientCapabilities} from './playbackApi'

// Representative codec strings for MediaSource.isTypeSupported() — mirrors
// Android's DeviceCodecCapabilities.kt (real MediaCodecList probing) with the
// closest browser-side equivalent. isTypeSupported answers "can this
// browser's media pipeline decode/play this," which is exactly what
// Hephaestus's isChannelDirectStreamable/isVideoDirectStreamable need to
// know before handing back a stream-copied source instead of transcoding.
const VIDEO_CODEC_PROBES: [name: string, mime: string][] = [
    ['h264', 'video/mp4; codecs="avc1.640028"'],
    ['hevc', 'video/mp4; codecs="hvc1.1.6.L93.B0"'],
    ['av1', 'video/mp4; codecs="av01.0.08M.08"'],
]
const AUDIO_CODEC_PROBES: [name: string, mime: string][] = [
    ['aac', 'audio/mp4; codecs="mp4a.40.2"'],
    ['ac3', 'audio/mp4; codecs="ac-3"'],
    ['eac3', 'audio/mp4; codecs="ec-3"'],
]

export function detectClientCapabilities(): DeclaredClientCapabilities {
    const supported = (mime: string): boolean => {
        try {
            return typeof MediaSource !== 'undefined' && MediaSource.isTypeSupported(mime)
        } catch {
            return false
        }
    }

    return {
        video_codecs: VIDEO_CODEC_PROBES.filter(([, mime]) => supported(mime)).map(([name]) => name),
        audio_codecs: AUDIO_CODEC_PROBES.filter(([, mime]) => supported(mime)).map(([name]) => name),
        max_height: typeof window !== 'undefined' ? Math.max(window.screen.width, window.screen.height) : undefined,
    }
}
