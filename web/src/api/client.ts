const BASE = ''

export interface AgentSummary {
  key: string
  provider: string
  model: string
  base_url: string
  system_prompt: string
  workspace: string
  edit_mode: string
  subagents: string[]
}

async function getErrorMessage(res: Response): Promise<string> {
  const text = await res.text()
  if (!text) return `HTTP ${res.status}`

  try {
    const data = JSON.parse(text) as { error?: string }
    if (typeof data.error === 'string' && data.error) {
      return data.error
    }
  } catch {
    // Fall back to the raw response body when it is not JSON.
  }

  return text
}

export async function apiFetch<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${BASE}${path}`, {
    cache: 'no-store',
    ...init,
    headers: { 'Content-Type': 'application/json', ...init?.headers },
  })
  if (!res.ok) throw new Error(await getErrorMessage(res))
  return res.json()
}

export function getAgents(): Promise<AgentSummary[]> {
  return apiFetch<AgentSummary[]>('/api/agents')
}

export function streamChat(
  sessionId: string | null,
  message: string,
  onEvent: (type: string, data: unknown) => void,
  signal?: AbortSignal,
): Promise<void> {
  return new Promise((resolve, reject) => {
    const payload: { message: string; session_id?: string } = { message }
    if (sessionId) {
      payload.session_id = sessionId
    }

    fetch(`${BASE}/api/chat`, {
      method: 'POST',
      cache: 'no-store',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      signal,
    }).then(res => {
      if (!res.ok) {
        getErrorMessage(res).then(message => reject(new Error(message))).catch(reject)
        return
      }
      if (!res.body) {
        reject(new Error('Response body unavailable'))
        return
      }

      const reader = res.body.getReader()
      const decoder = new TextDecoder()
      let buffer = ''

      function pump(): Promise<void> {
        return reader.read().then(({ done, value }) => {
          if (done) { resolve(); return }
          buffer += decoder.decode(value, { stream: true })
          const lines = buffer.split('\n')
          buffer = lines.pop()!
          let eventType = ''
          for (const line of lines) {
            if (line.startsWith('event: ')) eventType = line.slice(7)
            else if (line.startsWith('data: ') && eventType) {
              try { onEvent(eventType, JSON.parse(line.slice(6))) } catch { /* skip */ }
              eventType = ''
            }
          }
          return pump()
        })
      }
      pump()
    }).catch(err => {
      if (signal?.aborted) { resolve(); return }
      reject(err)
    })
  })
}
