import { useEffect, useState } from 'react'
import { apiFetch } from '../../api/client'

interface Skill {
  name: string
  description: string
  tools: string[]
  source_path: string
}

export function SkillsPage() {
  const [skills, setSkills] = useState<Skill[]>([])
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)

  const load = () => {
    setError('')
    setLoading(true)
    apiFetch<Skill[]>('/api/skills')
      .then(setSkills)
      .catch(e => setError(e.message))
      .finally(() => setLoading(false))
  }

  useEffect(load, [])

  return (
    <div className="p-6">
      <h1 className="text-2xl font-semibold mb-4">Skills</h1>
      {error ? (
        <div>
          <p className="text-red-400 mb-2">{error}</p>
          <button onClick={load} className="px-3 py-1 bg-accent text-white rounded hover:bg-accent-hover">Retry</button>
        </div>
      ) : loading ? (
        <p className="text-text-muted">Loading...</p>
      ) : skills.length === 0 ? (
        <p className="text-text-muted">No skills configured.</p>
      ) : (
        <div className="divide-y divide-border">
          {skills.map(s => (
            <div key={s.name} className="py-3">
              <div className="flex items-center gap-2 mb-1">
                <span className="font-medium text-text">{s.name}</span>
                <span className="text-xs bg-accent-bg text-accent px-2 py-0.5 rounded-full">{s.tools.length} tools</span>
              </div>
              <p className="text-sm text-text">{s.description}</p>
              <p className="text-xs text-text-muted mt-1">{s.source_path}</p>
            </div>
          ))}
        </div>
      )}
    </div>
  )
}
