const BASE = ''

export async function apiFetch<T>(path: string, init?: RequestInit): Promise<T> {
  const res = await fetch(`${BASE}${path}`, {
    ...init,
    headers: { 'Content-Type': 'application/json', ...init?.headers },
  })
  if (!res.ok) throw new Error(await res.text())
  return res.json()
}

export function streamChat(
  sessionId: string | null,
  message: string,
  onEvent: (type: string, data: unknown) => void,
  signal?: AbortSignal,
): Promise<void> {
  return new Promise((resolve, reject) => {
    fetch(`${BASE}/api/chat`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ session_id: sessionId, message }),
      signal,
    }).then(res => {
      if (!res.ok) return reject(new Error(`HTTP ${res.status}`))
      const reader = res.body!.getReader()
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
