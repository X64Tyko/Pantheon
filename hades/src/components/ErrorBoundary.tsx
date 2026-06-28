import { Component, type ErrorInfo, type ReactNode } from 'react'

interface Props { children: ReactNode; fallback?: ReactNode }
interface State { error: Error | null }

export class ErrorBoundary extends Component<Props, State> {
  state: State = { error: null }

  static getDerivedStateFromError(error: Error): State { return { error } }

  componentDidCatch(error: Error, info: ErrorInfo) {
    console.error('[ErrorBoundary]', error, info)
  }

  render() {
    if (this.state.error) {
      return this.props.fallback ?? (
        <div style={{
          padding: 40,
          color: 'oklch(0.72 0.16 22)',
          fontFamily: "'JetBrains Mono', monospace",
          fontSize: 12,
        }}>
          <div style={{ marginBottom: 8, fontSize: 14 }}>Something went wrong</div>
          <div style={{ color: 'var(--hds-txt-3)' }}>{this.state.error.message}</div>
        </div>
      )
    }
    return this.props.children
  }
}
