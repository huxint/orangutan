import { useEffect, useRef, useState } from 'react'
import { CalendarDays, ChevronDown, Plus, Radio, TerminalSquare, MessageSquare, Clock } from 'lucide-react'
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
    case 'cli': return { icon: TerminalSquare, label: 'CLI', cls: 'bg-sky-500/10 text-sky-400' }
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
        {/* New chat button */}
        <button
          onClick={onStartNewChat}
          className={cn(
            'rounded-lg px-2.5 py-1.5 text-xs font-semibold flex items-center gap-1.5',
            'bg-accent/10 text-accent border border-accent/15',
            'hover:bg-accent/20 hover:border-accent/25 transition-all duration-150',
          )}
        >
          <Plus size={13} strokeWidth={2.5} />
          New
        </button>

        {/* Sessions dropdown trigger */}
        <button
          onClick={() => setOpen(p => !p)}
          className={cn(
            'rounded-lg border bg-bg-elevated px-2.5 py-1.5 text-xs text-text-secondary',
            'flex items-center gap-1.5 min-w-[7rem] transition-all duration-150',
            open
              ? 'border-accent/25 text-text shadow-[0_0_12px_rgba(249,115,22,0.06)]'
              : 'border-border hover:text-text hover:border-accent/15',
          )}
        >
          <MessageSquare size={12} className="shrink-0 text-text-muted" />
          <span className="truncate">{sessions.length} session{sessions.length !== 1 ? 's' : ''}</span>
          <ChevronDown size={12} className={cn('shrink-0 transition-transform duration-200', open && 'rotate-180')} />
        </button>
      </div>

      {/* Dropdown panel */}
      <AnimatePresence>
        {open && (
          <motion.div
            initial={{ opacity: 0, y: -6, scale: 0.96 }}
            animate={{ opacity: 1, y: 0, scale: 1 }}
            exit={{ opacity: 0, y: -6, scale: 0.96 }}
            transition={{ duration: 0.15, ease: [0.25, 0.1, 0.25, 1] }}
            className="absolute right-0 z-30 mt-2 w-[24rem] rounded-xl border border-border bg-bg-elevated shadow-[0_16px_48px_rgba(0,0,0,0.25)] overflow-hidden"
          >
            {/* Dropdown header */}
            <div className="border-b border-border px-3.5 py-2.5 flex items-center justify-between">
              <span className="text-xs font-bold text-text">Sessions</span>
              <span className="text-[10px] text-text-muted bg-bg-surface px-1.5 py-0.5 rounded font-semibold">
                {sessions.length}
              </span>
            </div>

            {groups.length === 0 ? (
              <div className="px-3 py-8 text-center">
                <MessageSquare size={20} className="mx-auto mb-2 text-text-muted/30" />
                <p className="text-xs text-text-muted">No sessions yet</p>
              </div>
            ) : (
              <div className="max-h-80 overflow-y-auto p-1.5">
                {groups.map(g => (
                  <div key={g.label} className="mb-2 last:mb-0">
                    {/* Date group label */}
                    <div className="flex items-center gap-2 px-2 py-1.5">
                      <span className="text-[10px] font-bold uppercase tracking-[0.12em] text-text-muted">
                        {g.label}
                      </span>
                      <div className="flex-1 h-px bg-border/60" />
                    </div>

                    {/* Session items */}
                    {g.sessions.map(s => {
                      const b = originBadge(s)
                      const B = b.icon
                      const active = s.id === currentSessionId

                      return (
                        <button
                          key={s.id}
                          onClick={() => { setOpen(false); onSelectSession(s.id) }}
                          className={cn(
                            'w-full rounded-lg px-2.5 py-2 text-left transition-all duration-150 group',
                            active
                              ? 'bg-accent-dim border border-accent/15'
                              : 'hover:bg-bg-surface border border-transparent',
                          )}
                        >
                          <div className="flex items-center justify-between gap-2">
                            <span className={cn(
                              'truncate text-[13px] font-medium',
                              active ? 'text-accent' : 'text-text group-hover:text-text',
                            )}>
                              {sessionTitle(s)}
                            </span>
                            <span className={cn(
                              'shrink-0 inline-flex items-center gap-0.5 rounded-md px-1.5 py-0.5 text-[9px] font-bold uppercase tracking-wider',
                              b.cls,
                            )}>
                              <B size={8} />{b.label}
                            </span>
                          </div>
                          <div className="mt-0.5 flex items-center gap-1.5 text-[11px] text-text-muted">
                            <Clock size={9} />
                            <span>{formatTime(s.created_at)}</span>
                            <span className="text-text-muted/40">·</span>
                            <span>{s.message_count} msg{s.message_count !== 1 ? 's' : ''}</span>
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
