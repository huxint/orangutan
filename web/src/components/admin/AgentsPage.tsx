import { useEffect, useMemo, useState } from 'react'
import {
  Bot,
  RotateCcw,
  Globe,
  Cpu,
  Link2,
  FolderOpen,
  FileEdit,
  ChevronDown,
  Users,
  type LucideIcon,
} from 'lucide-react'
import { motion, AnimatePresence } from 'framer-motion'
import { apiFetch } from '../../api/client'
import { cn } from '../../lib/utils'

interface Agent {
  key: string
  provider: string
  model: string
  base_url: string
  system_prompt: string
  workspace: string
  edit_mode: string
  subagents?: string[]
}

// Cycle through accent colors by index
const AGENT_COLORS: { color: string; bg: string }[] = [
  { color: 'text-sky-400',     bg: 'bg-sky-500/10' },
  { color: 'text-violet-400',  bg: 'bg-violet-500/10' },
  { color: 'text-emerald-400', bg: 'bg-emerald-500/10' },
  { color: 'text-amber-400',   bg: 'bg-amber-500/10' },
  { color: 'text-rose-400',    bg: 'bg-rose-500/10' },
  { color: 'text-teal-400',    bg: 'bg-teal-500/10' },
  { color: 'text-pink-400',    bg: 'bg-pink-500/10' },
  { color: 'text-cyan-400',    bg: 'bg-cyan-500/10' },
]

const FIELD_ICONS: Record<string, LucideIcon> = {
  Provider: Globe,
  Model: Cpu,
  'Base URL': Link2,
  Workspace: FolderOpen,
  'Edit Mode': FileEdit,
}

function InfoRow({ icon: Icon, label, value }: { icon?: LucideIcon; label: string; value: string }) {
  return (
    <div className="flex items-center gap-2 min-w-0">
      {Icon && <Icon size={11} className="shrink-0 text-text-muted" />}
      <span className="shrink-0 text-[10px] font-semibold uppercase tracking-wider text-text-muted w-16">{label}</span>
      <span className="text-xs text-text-secondary truncate">{value}</span>
    </div>
  )
}

export function AgentsPage() {
  const [agents, setAgents] = useState<Agent[]>([])
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(true)
  const [expanded, setExpanded] = useState<string | null>(null)
  const [filter, setFilter] = useState('')

  const load = () => {
    setError(''); setLoading(true)
    apiFetch<Agent[]>('/api/agents').then(setAgents).catch(e => setError(e.message)).finally(() => setLoading(false))
  }

  useEffect(load, [])

  const filtered = useMemo(() => {
    if (!filter) return agents
    const q = filter.toLowerCase()
    return agents.filter(a =>
      a.key.toLowerCase().includes(q) ||
      a.provider.toLowerCase().includes(q) ||
      a.model.toLowerCase().includes(q),
    )
  }, [agents, filter])

  return (
    <div className="p-6 h-full overflow-y-auto pb-10">
      {/* Header */}
      <div className="flex items-center justify-between mb-5">
        <div>
          <h1 className="text-xl font-bold">Agents</h1>
          {!loading && !error && (
            <p className="text-xs text-text-muted mt-0.5">
              {agents.length} agent{agents.length !== 1 ? 's' : ''} configured
            </p>
          )}
        </div>
      </div>

      {error ? (
        <div className="rounded-xl border border-danger/20 bg-danger-dim p-4 text-danger text-sm">
          {error}
          <button onClick={load} className="ml-2 inline-flex items-center gap-1 text-xs underline">
            <RotateCcw size={12} />Retry
          </button>
        </div>
      ) : loading ? (
        <div className="flex items-center gap-2 text-sm text-text-muted">
          <div className="w-3.5 h-3.5 rounded-full border-2 border-accent border-t-transparent animate-spin" />
          Loading...
        </div>
      ) : agents.length === 0 ? (
        <div className="flex flex-col items-center justify-center py-20 text-text-muted">
          <Bot size={32} className="mb-3 opacity-30" />
          <p className="text-sm">No agents configured</p>
          <p className="text-[11px] mt-1">
            Add agents in <code className="px-1 py-0.5 rounded bg-bg-elevated text-[10px] font-mono">config.toml</code>
          </p>
        </div>
      ) : (
        <>
          {/* Search */}
          {agents.length > 4 && (
            <div className="mb-4">
              <input
                type="text"
                value={filter}
                onChange={e => setFilter(e.target.value)}
                placeholder="Filter agents..."
                className="w-full max-w-sm rounded-lg border border-border bg-bg-surface px-3 py-2 text-sm text-text placeholder:text-text-muted focus:outline-none focus:border-accent/40 transition-colors"
              />
            </div>
          )}

          {/* Grid */}
          <div className="grid gap-3 md:grid-cols-2">
            {filtered.map((a, i) => {
              const palette = AGENT_COLORS[i % AGENT_COLORS.length]
              const isExpanded = expanded === a.key
              const fields: [string, string][] = [
                ['Provider', a.provider],
                ['Model', a.model],
                ['Base URL', a.base_url],
                ...(a.edit_mode ? [['Edit Mode', a.edit_mode] as [string, string]] : []),
                ...(a.workspace ? [['Workspace', a.workspace] as [string, string]] : []),
              ]

              return (
                <motion.div
                  key={a.key}
                  initial={{ opacity: 0, y: 8 }}
                  animate={{ opacity: 1, y: 0 }}
                  transition={{ delay: i * 0.05, duration: 0.25 }}
                  className={cn(
                    'rounded-xl border bg-bg-surface overflow-hidden',
                    'transition-all duration-200',
                    isExpanded
                      ? 'border-accent/25 shadow-[0_0_24px_rgba(249,115,22,0.06)]'
                      : 'border-border hover:border-accent/15',
                  )}
                >
                  {/* Header */}
                  <button
                    type="button"
                    onClick={() => setExpanded(isExpanded ? null : a.key)}
                    className="w-full flex items-center gap-3 px-4 py-3.5 hover:bg-bg-elevated/40 transition-colors"
                  >
                    <div className={cn('p-2 rounded-lg', palette.bg)}>
                      <Bot size={16} className={palette.color} />
                    </div>
                    <div className="flex-1 text-left min-w-0">
                      <h2 className={cn('text-sm font-bold', palette.color)}>{a.key}</h2>
                      <p className="text-[11px] text-text-muted mt-0.5 truncate">
                        {a.provider} · {a.model}
                      </p>
                    </div>
                    {(a.subagents?.length ?? 0) > 0 && (
                      <span className="flex items-center gap-1 text-[10px] font-semibold bg-bg-elevated text-text-muted px-1.5 py-0.5 rounded mr-1">
                        <Users size={10} />
                        {a.subagents!.length}
                      </span>
                    )}
                    <ChevronDown
                      size={14}
                      className={cn('text-text-muted transition-transform duration-200 shrink-0', isExpanded && 'rotate-180')}
                    />
                  </button>

                  {/* Expandable details */}
                  <AnimatePresence initial={false}>
                    {isExpanded && (
                      <motion.div
                        initial={{ height: 0 }}
                        animate={{ height: 'auto' }}
                        exit={{ height: 0 }}
                        transition={{ duration: 0.2, ease: [0.25, 0.1, 0.25, 1] }}
                        className="overflow-hidden"
                      >
                        <div className="px-4 pb-4 pt-1 space-y-2 border-t border-border">
                          {fields.map(([label, value]) => (
                            <InfoRow key={label} icon={FIELD_ICONS[label]} label={label} value={value} />
                          ))}

                          {/* Subagents */}
                          {(a.subagents?.length ?? 0) > 0 && (
                            <div className="pt-1.5">
                              <span className="text-[10px] font-semibold uppercase tracking-wider text-text-muted">Subagents</span>
                              <div className="flex flex-wrap gap-1 mt-1.5">
                                {a.subagents!.map(s => (
                                  <span
                                    key={s}
                                    className="inline-block rounded-md bg-bg-elevated px-1.5 py-0.5 text-[10px] font-mono text-text-secondary"
                                  >
                                    {s}
                                  </span>
                                ))}
                              </div>
                            </div>
                          )}

                          {/* System prompt preview */}
                          {a.system_prompt && (
                            <div className="pt-1.5">
                              <span className="text-[10px] font-semibold uppercase tracking-wider text-text-muted">System Prompt</span>
                              <p className="mt-1 text-[11px] text-text-secondary leading-relaxed line-clamp-3 bg-bg/50 rounded-lg px-2.5 py-2">
                                {a.system_prompt}
                              </p>
                            </div>
                          )}
                        </div>
                      </motion.div>
                    )}
                  </AnimatePresence>
                </motion.div>
              )
            })}
          </div>

          {filtered.length === 0 && filter && (
            <div className="text-center text-sm text-text-muted py-12">
              No agents matching "<span className="text-text">{filter}</span>"
            </div>
          )}
        </>
      )}
    </div>
  )
}
