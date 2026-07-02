import { authHeaders } from '../api/client'

export interface PreviewStartResponse {
  session_id:   string
  manifest_url: string
}

export async function startPreview(channelId: string): Promise<PreviewStartResponse> {
  const res = await fetch('/stream/preview/start', {
    method:  'POST',
    headers: { 'Content-Type': 'application/json', ...authHeaders() },
    body:    JSON.stringify({ channel_id: channelId }),
  })
  if (!res.ok) throw new Error((await res.json().catch(() => ({}))).error ?? res.statusText)
  return res.json()
}

export function switchPreview(sessionId: string, channelId: string) {
  return fetch(`/stream/preview/${sessionId}/switch`, {
    method:  'POST',
    headers: { 'Content-Type': 'application/json', ...authHeaders() },
    body:    JSON.stringify({ channel_id: channelId }),
  })
}

export function stopPreview(sessionId: string) {
  fetch(`/stream/preview/${sessionId}/stop`, { method: 'POST', headers: authHeaders() }).catch(() => {})
}
