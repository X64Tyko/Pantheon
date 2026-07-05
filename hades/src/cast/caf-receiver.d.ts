// Minimal ambient types for the CAF *receiver* SDK surface CastReceiverProvider.tsx
// actually uses. Deliberately hand-written instead of installing
// @types/chromecast-caf-receiver: that package declares the global `cast` const
// via `declare global { const cast: {...} }`, which conflicts with
// @types/chromecast-caf-sender's `declare namespace cast.framework {...}` (already
// used throughout hades/src/cast — see CastProvider.tsx) — a namespace and a const
// can't both own the same global name. Extending the sender package's own
// namespace-merging style here avoids that collision entirely, since only Hades'
// TV-shell/receiver code (this file's actual consumers) needs these declarations.
declare namespace cast.framework {
  class CastReceiverContext {
    static getInstance(): CastReceiverContext
    start(options?: CastReceiverOptions): void
    stop(): void
    getPlayerManager(): PlayerManager
  }

  class CastReceiverOptions {
    constructor()
    maxInactivity?: number
    disableIdleTimeout?: boolean
  }

  class PlayerManager {
    setMediaElement(mediaElement: HTMLMediaElement | Promise<HTMLMediaElement>): void
    setMessageInterceptor<T extends messages.MessageType>(
      type: T,
      interceptor: (data: messages.MessageTypeDataMap[T]) => messages.MessageTypeDataMap[T] | messages.ErrorData | null,
    ): void
  }

  namespace messages {
    enum MessageType {
      LOAD = 'LOAD',
    }

    enum ErrorType {
      LOAD_FAILED = 'LOAD_FAILED',
    }

    class ErrorData {
      constructor(type: ErrorType)
    }

    class MediaInformation {
      contentId: string
      contentType: string
      customData?: unknown
    }

    class LoadRequestData {
      media: MediaInformation
      currentTime?: number
      autoplay?: boolean
    }

    interface MessageTypeDataMap {
      [MessageType.LOAD]: LoadRequestData
    }
  }
}
