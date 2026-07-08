import type { CastMediaArgs } from '../cast/castMedia'

// Unlike Chromecast's buildLoadRequest (which needs an absolute manifest
// URL — the receiver fetches media itself), a Roku "load" command only
// needs the content identity: the channel resolves its own manifest
// server-side via POST /stream/vod/start (or the live HLS path directly for
// a channel), exactly like Hades' own player does. Reuses CastMediaArgs
// as-is so PlayerPage can build one args object and hand it to either
// sender without duplicating the route/position logic.
export function buildRokuLoadCommand(args: CastMediaArgs): Record<string, unknown> {
  return {
    type:        'load',
    contentType: args.route.contentType,
    contentId:   args.route.contentId,
    positionMs:  args.isLive ? undefined : args.currentMs,
  }
}
