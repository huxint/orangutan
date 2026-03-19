import { useEffect, useState } from 'react'
import { apiFetch } from '../../api/client'

interface Tool {
  name: string
  description: string
  source: string
}

export function ToolsPage() {
  const [tools, setTools] = useState<Tool[]>([])
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)

  const load = () => {
    setError('')
    setLoading(true)
    apiFetch<Tool[]>('/api/tools')
      .then(setTools)
      .catch(e => setError(e.message))
      .finally(() => setLoading(false))
  }

  useEffect(load, [])

  return (
    <div className="p-6">
      <h1 className="text-2xl font-semibold mb-4">Tools</h1>
      {error ? (
        <div>
          <p className="text-red-400 mb-2">{error}</p>
          <button onClick={load} className="px-3 py-1 bg-accent text-white rounded hover:bg-accent-hover">Retry</button>
        </div>
      ) : loading ? (
        <p className="text-text-muted">Loading...</p>
      ) : (
        <table className="w-full text-left">
          <thead>
            <tr className="border-b border-border text-text-muted text-sm">
              <th className="py-2 pr-4">Name</th>
              <th className="py-2 pr-4">Description</th>
              <th className="py-2">Source</th>
            </tr>
          </thead>
          <tbody>
            {tools.map(t => (
              <tr key={t.name} className="border-b border-border">
                <td className="py-2 pr-4 font-medium text-accent">{t.name}</td>
                <td className="py-2 pr-4 text-text">{t.description}</td>
                <td className="py-2 text-text-muted">{t.source}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  )
}
