import type {PreviewStartResponse} from './previewApi'

export interface PreviewSessionApi {
    startPreview: (channelId: string) => Promise<PreviewStartResponse>
    switchPreview: (sessionId: string, channelId: string) => Promise<unknown>
    stopPreview: (sessionId: string) => void
}

// Client-side counterpart to Hephaestus's PreviewSession/PreviewSessionManager
// state machine (see PreviewSession.h's class comment): a channel focus while
// a session is already live reuses it via switchPreview() (never a fresh
// startPreview() — that's the whole point of the manifest URL staying
// stable), a focus change that arrives mid-cold-start queues onto that
// in-flight start instead of racing a second startPreview() and orphaning
// one of the two resulting ffmpeg processes, and an in-flight start that got
// superseded by a stop() (e.g. the tab was hidden) before it resolved
// self-stops on arrival instead of resurrecting a session nothing is looking
// at anymore.
//
// Pulled out of useGuideSession's refs/effects into its own framework-free
// class specifically so this orchestration is unit-testable without a React
// renderer — this repo has no jsdom/@testing-library/react yet (see
// momus/hades/setup.ts's own comment on why environment is 'node'), the same
// reason wtEventEffect.ts's computeWtEventEffect exists as a standalone
// function for Watch Together's follower-side sync logic.
export class PreviewSessionController {
    private sessionId: string | null = null
    private starting: Promise<string | null> | null = null
    private gen = 0

    constructor(
        private readonly api: PreviewSessionApi,
        // Mirrors setManifestUrl — called with the new manifest URL once a fresh
        // session actually starts, and with null once the current session stops.
        // Never called on a plain switch (switchChannel() server-side keeps the
        // manifest URL fixed — see PreviewSession.h — so there's nothing new to
        // set).
        private readonly onManifestUrlChange: (url: string | null) => void,
    ) {
    }

    begin(channelId: string): void {
        const myGen = this.gen
        if (this.sessionId) {
            this.api.switchPreview(this.sessionId, channelId).catch(() => {
            })
            return
        }
        if (this.starting) {
            this.starting.then(sid => {
                if (sid && this.gen === myGen) this.api.switchPreview(sid, channelId).catch(() => {
                })
            })
            return
        }
        this.starting = this.api.startPreview(channelId).then(res => {
            if (this.gen !== myGen) {
                this.api.stopPreview(res.session_id);
                return null
            }
            this.sessionId = res.session_id
            this.onManifestUrlChange(res.manifest_url)
            return res.session_id
        }).catch(() => null).finally(() => {
            this.starting = null
        })
    }

    // Bumps the generation (invalidating any in-flight begin()) and tears down
    // a live session, if there is one.
    stop(): void {
        this.gen++
        if (this.sessionId) {
            this.api.stopPreview(this.sessionId)
            this.sessionId = null
            this.onManifestUrlChange(null)
        }
    }

    hasSession(): boolean {
        return this.sessionId !== null
    }

    isStarting(): boolean {
        return this.starting !== null
    }
}
