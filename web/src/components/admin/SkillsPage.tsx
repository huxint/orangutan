import { useEffect, useState } from 'react'
import { Zap, RotateCcw } from 'lucide-react'
import { motion } from 'framer-motion'
import { apiFetch } from '../../api/client'

interface Skill { name: string; description: string; tools: string[]; source_path: string }

export function SkillsPage() {
  const [skills, setSkills] = useState<Skill[]>([])
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)

  const load = () => {
    setError(''); setLoading(true)
    apiFetch<Skill[]>('/api/skills').then(setSkills).catch(e => setError(e.message)).finally(() => setLoading(false))
  }

  useEffect(load, [])

  return (
    <div className="p-6 h-full overflow-y-auto pb-20">
      <div className="flex items-center justify-between mb-4">
        <h1 className="text-xl font-bold">Skills</h1>
        {!loading && !error && <span className="text-xs text-text-muted">{skills.length} loaded</span>}
      </div>

      {error ? (
        <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
          {error} <button onClick={load} className="ml-2 inline-flex items-center gap-1 text-xs underline"><RotateCcw size={12} />Retry</button>
        </div>
      ) : loading ? (
        <div className="flex items-center gap-2 text-sm text-text-muted">
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" /> Loading...
        </div>
      ) : skills.length === 0 ? (
        <p className="text-sm text-text-muted">No skills configured.</p>
      ) : (
        <div className="space-y-2">
          {skills.map((s, i) => (
            <motion.div
              key={s.name}
              initial={{ opacity: 0, y: 6 }}
              animate={{ opacity: 1, y: 0 }}
              transition={{ delay: i * 0.03 }}
              className="rounded-xl border border-border bg-bg-surface p-3 hover:border-accent/20 transition-colors"
            >
              <div className="flex items-start gap-2.5">
                <div className="shrink-0 p-1.5 rounded-lg bg-accent-dim text-accent"><Zap size={14} /></div>
                <div className="min-w-0">
                  <div className="flex items-center gap-2">
                    <span className="font-semibold text-sm text-text">{s.name}</span>
                    <span className="text-[10px] font-semibold bg-accent-dim text-accent px-1.5 py-0.5 rounded">{s.tools.length} tools</span>
                  </div>
                  <p className="mt-0.5 text-xs text-text-secondary leading-relaxed">{s.description}</p>
                  <p className="mt-1 text-[10px] text-text-muted font-mono truncate">{s.source_path}</p>
                </div>
              </div>
            </motion.div>
          ))}
        </div>
      )}
    </div>
  )
}
