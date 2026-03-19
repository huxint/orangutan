import { useEffect, useState } from 'react'
import { apiFetch } from '../../api/client'

interface Agent {
  key: string
  provider: string
  model: string
  base_url: string
  system_prompt: string
  workspace: string
  edit_mode: string
}

export function AgentsPage() {
  const [agents, setAgents] = useState<Agent[]>([])
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)

  const load = () => {
    setError('')
    setLoading(true)
    apiFetch<Agent[]>('/api/agents')
      .then(setAgents)
      .catch(e => setError(e.message))
      .finally(() => setLoading(false))
  }

  useEffect(load, [])

  return (
    <div className="p-6">
      <h1 className="text-2xl font-semibold mb-4">Agents</h1>
      {error ? (
        <div>
          <p className="text-red-400 mb-2">{error}</p>
          <button onClick={load} className="px-3 py-1 bg-accent text-white rounded hover:bg-accent-hover">Retry</button>
        </div>
      ) : loading ? (
        <p className="text-text-muted">Loading...</p>
      ) : agents.length === 0 ? (
        <p className="text-text-muted">No agents configured.</p>
      ) : (
        <div className="grid gap-4">
          {agents.map(a => (
            <div key={a.key} className="bg-bg-surface border border-border rounded-lg p-4">
              <h2 className="text-lg font-medium text-accent mb-2">{a.key}</h2>
              <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-sm">
                <dt className="text-text-muted">Provider</dt><dd className="text-text">{a.provider}</dd>
                <dt className="text-text-muted">Model</dt><dd className="text-text">{a.model}</dd>
                <dt className="text-text-muted">Base URL</dt><dd className="text-text">{a.base_url}</dd>
                {a.edit_mode && <><dt className="text-text-muted">Edit Mode</dt><dd className="text-text">{a.edit_mode}</dd></>}
                {a.workspace && <><dt className="text-text-muted">Workspace</dt><dd className="text-text">{a.workspace}</dd></>}
              </dl>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
