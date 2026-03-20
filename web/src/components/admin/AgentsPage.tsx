import { useEffect, useState } from 'react'
import { Bot, RotateCcw } from 'lucide-react'
import { motion } from 'framer-motion'
import { apiFetch } from '../../api/client'

interface Agent { key: string; provider: string; model: string; base_url: string; system_prompt: string; workspace: string; edit_mode: string }

export function AgentsPage() {
  const [agents, setAgents] = useState<Agent[]>([])
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)

  const load = () => {
    setError(''); setLoading(true)
    apiFetch<Agent[]>('/api/agents').then(setAgents).catch(e => setError(e.message)).finally(() => setLoading(false))
  }

  useEffect(load, [])

  return (
    <div className="p-6 h-full overflow-y-auto pb-20">
      <div className="flex items-center justify-between mb-4">
        <h1 className="text-xl font-bold">Agents</h1>
        {!loading && !error && <span className="text-xs text-text-muted">{agents.length} configured</span>}
      </div>

      {error ? (
        <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
          {error} <button onClick={load} className="ml-2 inline-flex items-center gap-1 text-xs underline"><RotateCcw size={12} />Retry</button>
        </div>
      ) : loading ? (
        <div className="flex items-center gap-2 text-sm text-text-muted">
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" /> Loading...
        </div>
      ) : agents.length === 0 ? (
        <p className="text-sm text-text-muted">No agents configured.</p>
      ) : (
        <div className="grid gap-3 md:grid-cols-2">
          {agents.map((a, i) => (
            <motion.div
              key={a.key}
              initial={{ opacity: 0, y: 6 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ delay: i * 0.05 }}
              className="rounded-xl border border-border bg-bg-surface p-4 hover:border-accent/20 transition-colors"
            >
              <div className="flex items-center gap-2.5 mb-3">
                <div className="p-2 rounded-lg bg-accent-dim text-accent"><Bot size={16} /></div>
                <h2 className="text-base font-bold text-text">{a.key}</h2>
              </div>
              <div className="space-y-1.5 text-xs">
                {[
                  ['Provider', a.provider],
                  ['Model', a.model],
                  ['Base URL', a.base_url],
                  ...(a.edit_mode ? [['Edit Mode', a.edit_mode]] : []),
                  ...(a.workspace ? [['Workspace', a.workspace]] : []),
                ].map(([label, value]) => (
                  <div key={label} className="flex gap-2">
                    <span className="shrink-0 w-16 text-text-muted uppercase tracking-wider font-medium">{label}</span>
                    <span className="text-text-secondary truncate">{value}</span>
                  </div>
                ))}
              </div>
            </motion.div>
          ))}
        </div>
      )}
    </div>
  )
}
