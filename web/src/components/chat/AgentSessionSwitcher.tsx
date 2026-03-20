import { useEffect, useRef, useState } from 'react'
import { CalendarDays, ChevronDown, Plus, Radio, TerminalSquare } from 'lucide-react'
import { motion, AnimatePresence } from 'framer-motion'
import { cn } from '../../lib/utils'
import type { ChatSessionSummary } from './types'

interface AgentSessionSwitcherProps {
  sessions: ChatSessionSummary[]
  currentSessionId?: string | null
  onSelectSession: (sessionId: string) => void
  onStartNewChat: () => void
}

function formatTime(iso: string): string {
  return new Date(iso).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
}

function formatDateLabel(iso: string): string {
  const now = new Date()
  const today = new Date(now.getFullYear(), now.getMonth(), now.getDate())
  const yesterday = new Date(today.getTime() - 86400000)
  const d = new Date(iso)
  const day = new Date(d.getFullYear(), d.getMonth(), d.getDate())
  if (day.getTime() === today.getTime()) return 'Today'
  if (day.getTime() === yesterday.getTime()) return 'Yesterday'
  return d.toLocaleDateString([], { month: 'short', day: 'numeric', year: 'numeric' })
}

function groupSessions(sessions: ChatSessionSummary[]) {
  const groups = new Map<string, ChatSessionSummary[]>()
  for (const s of sessions) {
    const label = formatDateLabel(s.created_at)
    const arr = groups.get(label)
    if (arr) arr.push(s)
    else groups.set(label, [s])
  }
  return Array.from(groups.entries()).map(([label, items]) => ({ label, sessions: items }))
}

function sessionTitle(s: ChatSessionSummary): string {
  if (s.preview?.trim()) return s.preview.trim()
  if (s.origin_kind === 'channel' && s.origin_ref) return s.origin_ref
  if (s.origin_kind === 'cli') return 'CLI conversation'
  if (s.origin_kind === 'web') return 'Web conversation'
  return s.id
}

function originBadge(s: ChatSessionSummary) {
  switch (s.origin_kind) {
    case 'channel': return { icon: Radio, label: 'Channel', cls: 'bg-warning-dim text-warning' }
    case 'cli': return { icon: TerminalSquare, label: 'CLI', cls: 'bg-blue-500/10 text-blue-400' }
    default: return { icon: CalendarDays, label: 'Web', cls: 'bg-success-dim text-success' }
  }
}

export function AgentSessionSwitcher({
  sessions, currentSessionId, onSelectSession, onStartNewChat,
}: AgentSessionSwitcherProps) {
  const [open, setOpen] = useState(false)
  const ref = useRef<HTMLDivElement>(null)
  const groups = groupSessions(sessions)

  useEffect(() => {
    if (!open) return
    const handler = (e: MouseEvent) => {
      if (!ref.current?.contains(e.target as Node)) setOpen(false)
    }
    document.addEventListener('mousedown', handler)
    return () => document.removeEventListener('mousedown', handler)
  }, [open])

  return (
    <div ref={ref} className="relative">
      <div className="flex items-center gap-1.5">
        <button
          onClick={onStartNewChat}
          className="rounded-lg border border-border bg-bg-elevated px-2.5 py-1.5 text-xs font-medium text-text-secondary
            hover:text-text hover:border-accent/20 transition-colors flex items-center gap-1"
        >
          <Plus size={13} />
          New
        </button>
        <button
          onClick={() => setOpen(p => !p)}
          className="rounded-lg border border-border bg-bg-elevated px-2.5 py-1.5 text-xs text-text-secondary
            hover:text-text hover:border-accent/20 transition-colors flex items-center gap-1.5 min-w-[7rem]"
        >
          <span className="truncate">{sessions.length} sessions</span>
          <ChevronDown size={13} className={cn('shrink-0 transition-transform duration-200', open && 'rotate-180')} />
        </button>
      </div>

      <AnimatePresence>
        {open && (
          <motion.div
            initial={{ opacity: 0, y: -4, scale: 0.97 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={{ opacity: 0, y: -4, scale: 0.97 }}
            transition={{ duration: 0.15 }}
            className="absolute right-0 z-30 mt-1.5 w-[24rem] rounded-xl border border-border bg-bg-elevated shadow-2xl overflow-hidden"
          >
            <div className="border-b border-border px-3 py-2.5">
              <div className="text-xs font-semibold text-text">Sessions</div>
            </div>

            {groups.length === 0 ? (
              <div className="px-3 py-6 text-center text-xs text-text-muted">No sessions yet.</div>
            ) : (
              <div className="max-h-80 overflow-y-auto p-1.5">
                {groups.map(g => (
                  <div key={g.label} className="mb-1.5 last:mb-0">
                    <div className="px-2 py-1 text-[10px] font-semibold uppercase tracking-widest text-text-muted">
                      {g.label}
                    </div>
                    {g.sessions.map(s => {
                      const b = originBadge(s)
                      const B = b.icon
                      const active = s.id === currentSessionId
                      return (
                        <button
                          key={s.id}
                          onClick={() => { setOpen(false); onSelectSession(s.id) }}
                          className={cn(
                            'w-full rounded-lg px-2.5 py-2 text-left transition-colors',
                            active ? 'bg-accent-dim' : 'hover:bg-bg-surface',
                          )}
                        >
                          <div className="flex items-center justify-between gap-2">
                            <span className="truncate text-sm font-medium text-text">{sessionTitle(s)}</span>
                            <span className={cn('shrink-0 inline-flex items-center gap-0.5 rounded px-1.5 py-0.5 text-[10px] font-semibold', b.cls)}>
                              <B size={9} />{b.label}
                            </span>
                          </div>
                          <div className="mt-0.5 text-[11px] text-text-muted">
                            {formatTime(s.created_at)} · {s.message_count} msgs
                          </div>
                        </button>
                      )
                    })}
                  </div>
                ))}
              </div>
            )}
          </motion.div>
        )}
      </AnimatePresence>
    </div>
  )
}
